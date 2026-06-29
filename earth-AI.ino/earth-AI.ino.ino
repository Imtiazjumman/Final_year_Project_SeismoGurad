/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║      SeismoGuard — P/S Wave Early Warning System                ║
 * ║                  Academic Capstone Project                      ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  FIXES IN THIS VERSION:                                          ║
 * ║                                                                  ║
 * ║  FIX 1 — LPF_ALPHA LOWERED                                      ║
 * ║     0.70 → 0.35  (filter now responds much faster to impacts)   ║
 * ║                                                                  ║
 * ║  FIX 2 — HORIZONTAL CHANNEL NOW USES DIFF                       ║
 * ║     filtHorizMag now tracks change in horiz magnitude            ║
 * ║     (same approach as vertical channel) — removes gravity bias   ║
 * ║                                                                  ║
 * ║  FIX 3 — resetAllCounters() REMOVED FROM NORMAL CASE TOP        ║
 * ║     Was resetting warnBeepDone every loop tick → buzzer glitch   ║
 * ║     Now only called on explicit transitions TO NORMAL            ║
 * ║                                                                  ║
 * ║  FIX 4 — BEEP IS NON-BLOCKING (millis-based)                    ║
 * ║     Replaced delay()-based beep() with a millis ticker so        ║
 * ║     the state machine keeps running during the beep              ║
 * ║                                                                  ║
 * ║  FIX 5 — S_WAVE_THRESH CORRECTED                                ║
 * ║     1.3g → 0.9g  (matches spec, reachable with MPU6050)         ║
 * ║                                                                  ║
 * ║  FIX 6 — S_CONFIRM_SECS REDUCED FOR DEMO                        ║
 * ║     4s → 2s  (20 samples, demo-friendly)                        ║
 * ║                                                                  ║
 * ║  FIX 7 — P_TO_S_SECS REDUCED FOR DEMO                          ║
 * ║     15s → 8s  (still meaningful warning window)                 ║
 * ║                                                                  ║
 * ║  FIX 8 — CALMING QUIET THRESHOLD CORRECTED                      ║
 * ║     filtHorizMag quiet check 0.7 → 0.08 (diff-based value)      ║
 * ║                                                                  ║
 * ║  FIX 9 — REMOVED alertSentThisEvent ONE-SHOT FLAG               ║
 * ║     EARTHQUAKE! UDP now retries every 5s so no packet is lost    ║
 * ╚══════════════════════════════════════════════════════════════════╝
 */

#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>

// ── Network ───────────────────────────────────────────────────────────────
const char* WIFI_SSID = "iPhone";
const char* WIFI_PASS = "sifat1842";
const char* UDP_HOST  = "172.20.10.3";
const int   UDP_PORT  = 4210;

// ── Hardware ──────────────────────────────────────────────────────────────
#define LED_PIN     2
#define BUZZER_PIN  13
#define MPU_ADDR    0x68
#define LCD_ADDR    0x27

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
WiFiUDP udp;

// ─────────────────────────────────────────────────────────────────────────
// PARAMETERS
// ─────────────────────────────────────────────────────────────────────────
#define SAMPLE_MS         100      // 10 Hz loop

// FIX 1: Lowered from 0.70 to 0.35 — filter is now much more responsive.
#define LPF_ALPHA         0.35f

// STA/LTA
#define STA_LEN           20       // 2s short-term
#define LTA_LEN           100      // 10s long-term → 10s calibration

// ── P-WAVE THRESHOLDS ─────────────────────────────────────────────────────
#define P_DIRECT_THRESH   0.8f
#define P_RATIO_THRESH    4.0f
#define P_CONFIRM_SECS    3
#define P_CONFIRM_COUNT   (P_CONFIRM_SECS * (1000 / SAMPLE_MS))   // 30 samples

// False-alarm cancellation
#define FALSE_ALARM_SECS  3
#define FALSE_ALARM_COUNT (FALSE_ALARM_SECS * (1000 / SAMPLE_MS)) // 30 samples
#define QUIET_THRESHOLD   0.08f

// ── S-WAVE THRESHOLDS ─────────────────────────────────────────────────────
// FIX 5: Corrected from 1.3g to 0.9g — matches spec and is reachable
#define S_WAVE_THRESH     1.2f

// FIX 6: Reduced from 4s to 2s for demo-friendly confirmation
#define S_CONFIRM_SECS    2
#define S_CONFIRM_COUNT   (S_CONFIRM_SECS * (1000 / SAMPLE_MS))   // 20 samples

// ── TIMING ────────────────────────────────────────────────────────────────
// FIX 7: Reduced from 15s to 8s — shorter wait, still meaningful window
#define P_TO_S_SECS       8
#define SWAVE_DISPLAY_MS  10000UL
#define EQ_BEEP_STOP_MS   5000UL

// Calm reset
#define CALM_SECS         5
#define CALM_COUNT        (CALM_SECS * (1000 / SAMPLE_MS))        // 50 samples

// ── UDP SEND INTERVALS ────────────────────────────────────────────────────
#define UDP_NORMAL_INTERVAL   2000UL
#define UDP_WARNING_INTERVAL  2000UL
#define UDP_EQ_INTERVAL       5000UL

// ── NON-BLOCKING BEEP CONFIG ──────────────────────────────────────────────
// FIX 4: Beep is now driven by millis so loop() never blocks.
// P-wave: 6 pulses × (400ms on + 100ms off) ≈ 3s total
// S-wave: 10 pulses × (400ms on + 100ms off) ≈ 5s total
#define BEEP_ON_MS        400
#define BEEP_OFF_MS       100
#define PWAVE_PULSES      6
#define SWAVE_PULSES      10

// ─────────────────────────────────────────────────────────────────────────
// STATE MACHINE
// ─────────────────────────────────────────────────────────────────────────
typedef enum {
  NORMAL,
  PWAVE_DETECTING,
  PWAVE_CONFIRMED,
  SWAVE_CONFIRMED,
  CALMING
} SeismicState;

SeismicState state = NORMAL;

// ── STA/LTA ───────────────────────────────────────────────────────────────
float staBuffer[STA_LEN];
float ltaBuffer[LTA_LEN];
int   staIdx   = 0, ltaIdx = 0;
float staSum   = 0.0f, ltaSum = 0.0f;
bool  ltaReady = false;

// ── Sensor & Filter ───────────────────────────────────────────────────────
float ax, ay, az;
float filtVertDiff  = 0.0f;
float filtHorizMag  = 0.0f;  // FIX 2: tracks diff of horiz magnitude
float prevAz        = 1.0f;
float prevHorizMag  = 0.0f;  // FIX 2: previous horiz magnitude for diff
float staltaRatio   = 0.0f;

// ── Counters ──────────────────────────────────────────────────────────────
int pConfirmCount = 0;
int pQuietCount   = 0;
int sConfirmCount = 0;
int calmCount     = 0;

// ── Beep state (non-blocking) ─────────────────────────────────────────────
int           beepPulsesRemaining = 0;
bool          beepPhaseOn         = false;
unsigned long beepPhaseStart      = 0;
bool          beepActive          = false;

// ── One-shot flags ────────────────────────────────────────────────────────
bool warnBeepDone = false;
bool eqBeepDone   = false;
// FIX 9: alertSentThisEvent removed — EARTHQUAKE! UDP retries every 5s

// ── Timestamps ────────────────────────────────────────────────────────────
unsigned long pWaveTime      = 0;
unsigned long eqBeepTime     = 0;
unsigned long swaveStartTime = 0;
unsigned long lastLcdUpdate  = 0;
unsigned long lastUdpSend    = 0;
unsigned long lastLoop       = 0;
unsigned long lastReminder   = 0;

// ─────────────────────────────────────────────────────────────────────────
void readMPU() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)6, (uint8_t)true);
  ax = ((int16_t)((Wire.read() << 8) | Wire.read())) / 16384.0f;
  ay = ((int16_t)((Wire.read() << 8) | Wire.read())) / 16384.0f;
  az = ((int16_t)((Wire.read() << 8) | Wire.read())) / 16384.0f;
}

// ─────────────────────────────────────────────────────────────────────────
float updateSTALTA(float sample) {
  staSum -= staBuffer[staIdx];
  staBuffer[staIdx] = sample;
  staSum += sample;
  staIdx = (staIdx + 1) % STA_LEN;

  ltaSum -= ltaBuffer[ltaIdx];
  ltaBuffer[ltaIdx] = sample;
  ltaSum += sample;
  ltaIdx = (ltaIdx + 1) % LTA_LEN;

  if (!ltaReady && ltaIdx == 0) ltaReady = true;
  if (!ltaReady) return 0.0f;

  float sta = staSum / (float)STA_LEN;
  float lta = ltaSum / (float)LTA_LEN;
  if (lta < 0.0003f) return 0.0f;
  return sta / lta;
}

// ─────────────────────────────────────────────────────────────────────────
void lcdLine(uint8_t row, const char* text) {
  char buf[17];
  snprintf(buf, 17, "%-16s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// ─────────────────────────────────────────────────────────────────────────
// FIX 4: Non-blocking beep starter
void startBeep(int pulses) {
  beepPulsesRemaining = pulses;
  beepPhaseOn         = true;
  beepPhaseStart      = millis();
  beepActive          = true;
  digitalWrite(BUZZER_PIN, HIGH);
}

void tickBeep() {
  if (!beepActive) return;
  unsigned long now = millis();

  if (beepPhaseOn) {
    if (now - beepPhaseStart >= BEEP_ON_MS) {
      digitalWrite(BUZZER_PIN, LOW);
      beepPulsesRemaining--;
      if (beepPulsesRemaining <= 0) {
        beepActive = false;
        return;
      }
      beepPhaseOn    = false;
      beepPhaseStart = now;
    }
  } else {
    if (now - beepPhaseStart >= BEEP_OFF_MS) {
      digitalWrite(BUZZER_PIN, HIGH);
      beepPhaseOn    = true;
      beepPhaseStart = now;
    }
  }
}

// Blocking beep only used during setup/calibration
void beepBlocking(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}

// ─────────────────────────────────────────────────────────────────────────
bool sendUDP(const char* status, float value, int eta, unsigned long interval) {
  unsigned long now = millis();
  if (interval > 0 && now - lastUdpSend < interval) return false;
  lastUdpSend = now;

  char msg[80];
  if (eta > 0)
    snprintf(msg, sizeof(msg), "%s | D:%.4f | ETA:%d", status, value, eta);
  else
    snprintf(msg, sizeof(msg), "%s | D:%.4f", status, value);

  udp.beginPacket(UDP_HOST, UDP_PORT);
  udp.print(msg);
  udp.endPacket();
  Serial.print("[UDP] "); Serial.println(msg);
  return true;
}

// ─────────────────────────────────────────────────────────────────────────
// FIX 3: Only called on explicit transitions TO NORMAL
void resetAllCounters() {
  pConfirmCount       = 0;
  pQuietCount         = 0;
  sConfirmCount       = 0;
  calmCount           = 0;
  warnBeepDone        = false;
  eqBeepDone          = false;
  beepActive          = false;
  beepPulsesRemaining = 0;
  digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);
}

// ─────────────────────────────────────────────────────────────────────────
bool isPWaveSignal() {
  return (filtVertDiff >= P_DIRECT_THRESH) || (staltaRatio >= P_RATIO_THRESH);
}

// ─────────────────────────────────────────────────────────────────────────
void calibrate() {
  Serial.println("[*] Calibrating 10s noise floor...");
  float dummy    = az;
  float filtered = 0.0f;

  for (int i = 0; i < LTA_LEN; i++) {
    readMPU();
    float raw = abs(az - dummy);
    dummy    = az;
    filtered = LPF_ALPHA * filtered + (1.0f - LPF_ALPHA) * raw;

    ltaBuffer[ltaIdx] = filtered;
    ltaSum += filtered;
    ltaIdx = (ltaIdx + 1) % LTA_LEN;

    if (i < STA_LEN) {
      staBuffer[i] = filtered;
      staSum += filtered;
    }

    if (i % 10 == 0) {
      int pct = (i * 16) / LTA_LEN;
      char bar[17] = "                ";
      for (int j = 0; j < pct; j++) bar[j] = '=';
      char line[17];
      snprintf(line, 17, "Wait: %2ds       ", (LTA_LEN - i) / 10);
      lcdLine(0, line);
      lcdLine(1, bar);
    }
    delay(SAMPLE_MS);
  }

  ltaReady     = true;
  staIdx       = 0;
  prevHorizMag = sqrt(ax * ax + ay * ay);  // FIX 2: initialise horiz prev
  Serial.print("[*] Baseline LTA = ");
  Serial.println(ltaSum / LTA_LEN, 6);
  Serial.println("[*] Calibration done.");
}

// ─────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Wire.begin();
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);

  lcd.init();
  lcd.backlight();
  lcdLine(0, "SeismoGuard AI  ");
  lcdLine(1, "Booting...      ");
  delay(600);

  // MPU6050: wake + ±2g + hardware DLPF 44Hz
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); Wire.write(0x00); Wire.endTransmission(true);
  delay(50);
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C); Wire.write(0x00); Wire.endTransmission(true);
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1A); Wire.write(0x03); Wire.endTransmission(true);

  // WiFi
  lcdLine(0, "Connecting WiFi ");
  lcdLine(1, WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500); tries++; Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    udp.begin(UDP_PORT);
    Serial.print("\n[*] WiFi: "); Serial.println(WiFi.localIP());
    lcdLine(0, "WiFi Connected! ");
    lcdLine(1, WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[!] WiFi failed — offline");
    lcdLine(0, "WiFi FAILED     ");
    lcdLine(1, "Offline mode    ");
  }
  delay(800);

  readMPU();
  prevAz = az;

  lcdLine(0, "Calibrating...  ");
  lcdLine(1, "Hold still 10s  ");
  calibrate();

  lcdLine(0, "QuakeSense READY");
  lcdLine(1, "Monitoring...   ");
  beepBlocking(1, 400, 0);

  Serial.println("\n[*] Ready. Thresholds:");
  Serial.println("    P-wave : V >= 0.7g  OR  STA/LTA >= 4.0,  3s sustained");
  Serial.println("    S-wave : H >= 0.9g,  2s sustained  (only after 8s P window)");
  Serial.println("    LPF    : alpha=0.35 (fast response)");
  Serial.println("    Beeps  : non-blocking  P~3s  S~5s");
  Serial.println("──────────────────────────────────────────────────────────");

  lastLoop = millis();
}

// ─────────────────────────────────────────────────────────────────────────
// loop
// ─────────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // FIX 4: Drive the non-blocking beep every iteration
  tickBeep();

  if (now - lastLoop < SAMPLE_MS) return;
  lastLoop = now;

  // --- Read + filter ---
  readMPU();

  float rawVert  = abs(az - prevAz);
  prevAz         = az;

  // FIX 2: Use DIFF of horizontal magnitude — removes static gravity component
  float horizMagRaw = sqrt(ax * ax + ay * ay);
  float rawHoriz    = abs(horizMagRaw - prevHorizMag);
  prevHorizMag      = horizMagRaw;

  filtVertDiff = LPF_ALPHA * filtVertDiff + (1.0f - LPF_ALPHA) * rawVert;
  filtHorizMag = LPF_ALPHA * filtHorizMag + (1.0f - LPF_ALPHA) * rawHoriz;

  // Floor logic during calm states
  if (state == CALMING || state == SWAVE_CONFIRMED) {
    if (filtVertDiff < 0.05f) filtVertDiff = 0.0f;
    if (filtHorizMag < 0.05f) filtHorizMag = 0.0f;
  }

  staltaRatio = updateSTALTA(filtVertDiff);

  // ─────────────────────────────────────────────────────────────────────
  // STATE MACHINE
  // ─────────────────────────────────────────────────────────────────────
  switch (state) {

    // ══ NORMAL ══════════════════════════════════════════════════════════
    case NORMAL: {
      // FIX 3: resetAllCounters() NOT called here every tick

      if (isPWaveSignal()) {
        pConfirmCount++;
        Serial.print("[?] V="); Serial.print(filtVertDiff, 3);
        Serial.print(" H="); Serial.print(filtHorizMag, 3);
        Serial.print(" R="); Serial.print(staltaRatio, 1);
        Serial.print(" cnt="); Serial.println(pConfirmCount);

        if (pConfirmCount >= P_CONFIRM_COUNT) {
          state         = PWAVE_CONFIRMED;
          pWaveTime     = now;
          lastReminder  = now;
          pConfirmCount = 0;
          pQuietCount   = 0;
          digitalWrite(LED_PIN, HIGH);
          if (!warnBeepDone) { startBeep(PWAVE_PULSES); warnBeepDone = true; }
          Serial.println("[!] P-WAVE CONFIRMED");
          sendUDP("WARNING", filtVertDiff, P_TO_S_SECS, 0);
          lcdLine(0, "P-WAVE DETECTED!");
          lcdLine(1, "TAKE COVER NOW! ");
        } else {
          state = PWAVE_DETECTING;
        }
      }

      sendUDP("NORMAL", filtVertDiff, 0, UDP_NORMAL_INTERVAL);

      if (now - lastLcdUpdate >= 500) {
        lcdLine(0, "STATUS: NORMAL  ");
        char buf[17];
        snprintf(buf, 17, "V:%.2f H:%.2f    ", filtVertDiff, filtHorizMag);
        lcdLine(1, buf);
        lastLcdUpdate = now;
      }
      break;
    }

    // ══ PWAVE_DETECTING ═════════════════════════════════════════════════
    case PWAVE_DETECTING: {
      if (filtVertDiff < QUIET_THRESHOLD) {
        pQuietCount++;
        if (pQuietCount >= FALSE_ALARM_COUNT) {
          Serial.println("[*] False alarm cancelled");
          state = NORMAL;
          pConfirmCount = 0;
          pQuietCount   = 0;
          resetAllCounters();  // FIX 3: reset on transition
          break;
        }
      } else {
        pQuietCount = 0;
      }

      if (isPWaveSignal()) {
        pConfirmCount++;
        if (pConfirmCount >= P_CONFIRM_COUNT) {
          state         = PWAVE_CONFIRMED;
          pWaveTime     = now;
          lastReminder  = now;
          pConfirmCount = 0;
          pQuietCount   = 0;
          digitalWrite(LED_PIN, HIGH);
          if (!warnBeepDone) { startBeep(PWAVE_PULSES); warnBeepDone = true; }
          Serial.println("[!] P-WAVE CONFIRMED (sustained)");
          sendUDP("WARNING", filtVertDiff, P_TO_S_SECS, 0);
          lcdLine(0, "P-WAVE DETECTED!");
          lcdLine(1, "TAKE COVER NOW! ");
        }
      } else {
        if (pConfirmCount > 0) pConfirmCount--;
        if (pConfirmCount == 0 && filtVertDiff < QUIET_THRESHOLD) {
          state = NORMAL;
          pQuietCount = 0;
          resetAllCounters();  // FIX 3: reset on transition
        }
      }

      sendUDP("WARNING", filtVertDiff, P_TO_S_SECS, UDP_WARNING_INTERVAL);

      if (now - lastLcdUpdate >= 500) {
        int pct = (pConfirmCount * 100) / P_CONFIRM_COUNT;
        char buf[17];
        snprintf(buf, 17, "Conf:%3d%% V:%.2f ", pct, filtVertDiff);
        lcdLine(0, "Detecting...    ");
        lcdLine(1, buf);
        lastLcdUpdate = now;
      }
      break;
    }

    // ══ PWAVE_CONFIRMED ═════════════════════════════════════════════════
    case PWAVE_CONFIRMED: {
      unsigned long elapsed = now - pWaveTime;
      int etaSec = max(0, (int)((P_TO_S_SECS * 1000UL - elapsed) / 1000));

      if (elapsed >= (unsigned long)(P_TO_S_SECS * 1000)) {

        if (filtHorizMag >= S_WAVE_THRESH) {
          sConfirmCount++;
          Serial.print("[?] S-wave check H="); Serial.print(filtHorizMag, 3);
          Serial.print(" cnt="); Serial.println(sConfirmCount);

          if (sConfirmCount >= S_CONFIRM_COUNT) {
            state          = SWAVE_CONFIRMED;
            swaveStartTime = now;
            calmCount      = 0;
            sConfirmCount  = 0;
            if (!eqBeepDone) {
              startBeep(SWAVE_PULSES);
              eqBeepTime = now;
              eqBeepDone = true;
            }
            Serial.println("[!!] S-WAVE CONFIRMED — EARTHQUAKE!");
            sendUDP("EARTHQUAKE!", filtHorizMag, 0, 0);
            lcdLine(0, "!! EARTHQUAKE !!");
            lcdLine(1, "S-WAVE CONFIRMED");
            break;
          }
        } else {
          sConfirmCount = 0;
          Serial.println("[*] 8s window expired, no S-wave. Calming.");
          state = CALMING;
          calmCount = 0;
          digitalWrite(LED_PIN, LOW);
          break;
        }

      } else {
        if (filtHorizMag >= S_WAVE_THRESH) {
          Serial.print("[~] H="); Serial.print(filtHorizMag, 3);
          Serial.println(" (S-thresh crossed but locked — still in P-wave window)");
        }
      }

      // Reminder beep every 5s while in P-wave window
      if (now - lastReminder >= 5000) {
        if (!beepActive) { startBeep(1); }
        lastReminder = now;
      }

      sendUDP("WARNING", filtVertDiff, etaSec, UDP_WARNING_INTERVAL);

      if (now - lastLcdUpdate >= 500) {
        lcdLine(0, "P-WAVE! EVACUATE");
        char buf[17];
        snprintf(buf, 17, "S-Wave in: %3ds ", etaSec);
        lcdLine(1, buf);
        lastLcdUpdate = now;
      }
      break;
    }

    // ══ SWAVE_CONFIRMED ═════════════════════════════════════════════════
    case SWAVE_CONFIRMED: {
      digitalWrite(LED_PIN, HIGH);

      // Stop buzzer after 5 seconds
      if (eqBeepDone && (now - eqBeepTime >= EQ_BEEP_STOP_MS)) {
        beepActive = false;
        digitalWrite(BUZZER_PIN, LOW);
      }

      // FIX 9: Retry UDP every 5s — no one-shot flag
      sendUDP("EARTHQUAKE!", filtHorizMag, 0, UDP_EQ_INTERVAL);

      // Auto-exit after 10s display timer
      if (now - swaveStartTime >= SWAVE_DISPLAY_MS) {
        Serial.println("[*] 10s display timer expired → resetting to NORMAL");
        state = NORMAL;
        resetAllCounters();  // FIX 3: reset on transition
        lcdLine(0, "All Clear       ");
        lcdLine(1, "Monitoring...   ");
        beepBlocking(1, 500, 0);
        break;
      }

      if (now - lastLcdUpdate >= 500) {
        lcdLine(0, "!! EARTHQUAKE !!");
        int secLeft = max(0, (int)((SWAVE_DISPLAY_MS - (now - swaveStartTime)) / 1000));
        char buf[17];
        snprintf(buf, 17, "H:%.2f  %2ds left", filtHorizMag, secLeft);
        lcdLine(1, buf);
        lastLcdUpdate = now;
      }
      break;
    }

    // ══ CALMING ═════════════════════════════════════════════════════════
    case CALMING: {
      // FIX 8: filtHorizMag is now diff-based so quiet threshold is 0.08, not 0.7
      bool isQuiet = (filtVertDiff < QUIET_THRESHOLD && filtHorizMag < 0.08f);

      if (isQuiet) {
        calmCount++;
        if (calmCount >= CALM_COUNT) {
          Serial.println("[*] All clear → NORMAL");
          state = NORMAL;
          resetAllCounters();  // FIX 3: reset on transition
          lcdLine(0, "All Clear       ");
          lcdLine(1, "Monitoring...   ");
          beepBlocking(1, 500, 0);
          break;
        }
      } else {
        if (filtHorizMag >= S_WAVE_THRESH) {
          state          = SWAVE_CONFIRMED;
          swaveStartTime = now;
          calmCount      = 0;
        } else if (isPWaveSignal()) {
          state = PWAVE_DETECTING;
          calmCount     = 0;
          pConfirmCount = 0;
        } else {
          calmCount = 0;
        }
      }

      sendUDP("NORMAL", filtVertDiff, 0, UDP_NORMAL_INTERVAL);

      if (now - lastLcdUpdate >= 500) {
        int secLeft = max(0, (int)((CALM_COUNT - calmCount) * SAMPLE_MS / 1000) + 1);
        lcdLine(0, "Verifying calm  ");
        char buf[17];
        snprintf(buf, 17, "Reset in: %3ds  ", secLeft);
        lcdLine(1, buf);
        lastLcdUpdate = now;
      }
      break;
    }
  } // end switch
} // end loop

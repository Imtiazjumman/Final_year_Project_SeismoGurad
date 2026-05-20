
/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║      SeismoGuard — P/S Wave Early Warning System                ║
 * ║                  Academic Capstone Project                      ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  CHANGES IN THIS VERSION:                                        ║
 * ║                                                                  ║
 * ║  1. P-WAVE THRESHOLD LOWERED                                     ║
 * ║     P_DIRECT_THRESH : 0.5g → 0.7g  (detects at 0.7–0.9g)       ║
 * ║                                                                  ║
 * ║  2. S-WAVE THRESHOLD LOWERED                                     ║
 * ║     S_WAVE_THRESH   : 1.0g → 0.9g  (detects at 0.9–1.5g)       ║
 * ║                                                                  ║
 * ║  3. P→S WINDOW SHORTENED                                         ║
 * ║     P_TO_S_SECS     : 20s  → 15s                                ║
 * ║                                                                  ║
 * ║  4. S-WAVE LOCKED OUT DURING P-WAVE WINDOW                      ║
 * ║     Even if H ≥ S_WAVE_THRESH, system stays in P-WAVE state     ║
 * ║     for the full 15s. S-wave detection only begins after 15s.   ║
 * ║                                                                  ║
 * ║  5. BEEP DURATIONS FIXED                                         ║
 * ║     P-wave beep : ~3 seconds  (was ~0.8s)                       ║
 * ║     S-wave beep : ~5 seconds  (was ~1.2s)                       ║
 * ║                                                                  ║
 * ║  6. SWAVE DISPLAY TIMER                                          ║
 * ║     After S-wave confirmed, system shows event for 10s           ║
 * ║     then automatically returns to NORMAL regardless of sensor    ║
 * ╚══════════════════════════════════════════════════════════════════╝
 */

#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>

// ── Network ───────────────────────────────────────────────────────────────
const char* WIFI_SSID = "Imtiaz";
const char* WIFI_PASS = "01860585207i";
const char* UDP_HOST  = "192.168.0.103";
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

#define LPF_ALPHA         0.70f    // 30% of each sample passes through

// STA/LTA
#define STA_LEN           20       // 2s short-term
#define LTA_LEN           100      // 10s long-term → 10s calibration

// ── P-WAVE THRESHOLDS ─────────────────────────────────────────────────────
// CHANGED: lowered to 0.7g so it detects at 0.7–0.9g range
#define P_DIRECT_THRESH   0.7f     // filtVertDiff must reach 0.7g  ← CHANGED (was 0.5)
#define P_RATIO_THRESH    4.0f     // OR STA/LTA ratio ≥ 4.0
#define P_CONFIRM_SECS    3
#define P_CONFIRM_COUNT   (P_CONFIRM_SECS * (1000 / SAMPLE_MS))   // 30 samples

// False-alarm cancellation
#define FALSE_ALARM_SECS  3
#define FALSE_ALARM_COUNT (FALSE_ALARM_SECS * (1000 / SAMPLE_MS)) // 30 samples
#define QUIET_THRESHOLD   0.08f

// ── S-WAVE THRESHOLDS ─────────────────────────────────────────────────────
// CHANGED: lowered to 0.9g so it detects at 0.9–1.5g range
#define S_WAVE_THRESH     0.9f     // filtHorizMag must reach 0.9g  ← CHANGED (was 1.0)
#define S_CONFIRM_SECS    4
#define S_CONFIRM_COUNT   (S_CONFIRM_SECS * (1000 / SAMPLE_MS))   // 40 samples

// ── TIMING ────────────────────────────────────────────────────────────────
// CHANGED: 15 seconds P→S window (was 20)
#define P_TO_S_SECS       15                                        // ← CHANGED (was 20)

// CHANGED: S-wave event displayed for 10 seconds then auto-resets
#define SWAVE_DISPLAY_MS  10000UL                                   // ← NEW

// Buzzer auto-silence after earthquake alarm
#define EQ_BEEP_STOP_MS   5000UL

// Calm reset
#define CALM_SECS         5
#define CALM_COUNT        (CALM_SECS * (1000 / SAMPLE_MS))        // 50 samples

// ── UDP SEND INTERVALS ────────────────────────────────────────────────────
#define UDP_NORMAL_INTERVAL   2000UL
#define UDP_WARNING_INTERVAL  2000UL
#define UDP_EQ_INTERVAL       5000UL

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
float filtVertDiff = 0.0f;
float filtHorizMag = 0.0f;
float prevAz       = 1.0f;
float staltaRatio  = 0.0f;

// ── Counters ──────────────────────────────────────────────────────────────
int pConfirmCount = 0;
int pQuietCount   = 0;
int sConfirmCount = 0;
int calmCount     = 0;

// ── One-shot beep flags ───────────────────────────────────────────────────
bool warnBeepDone = false;
bool eqBeepDone   = false;
bool alertSentThisEvent = false;

// ── Timestamps ────────────────────────────────────────────────────────────
unsigned long pWaveTime     = 0;
unsigned long eqBeepTime    = 0;
unsigned long swaveStartTime = 0;  // ← NEW: tracks when S-wave was confirmed
unsigned long lastLcdUpdate = 0;
unsigned long lastUdpSend   = 0;
unsigned long lastLoop      = 0;
unsigned long lastReminder  = 0;

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
void beep(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}

// ─────────────────────────────────────────────────────────────────────────
bool sendUDP(const char* status, float value, int eta, unsigned long interval) {
  unsigned long now = millis();
  if (now - lastUdpSend < interval) return false;
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
void resetAllCounters() {
  pConfirmCount = 0;
  pQuietCount   = 0;
  sConfirmCount = 0;
  calmCount     = 0;
  warnBeepDone  = false;
  eqBeepDone    = false;
  alertSentThisEvent = false;
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

  ltaReady = true;
  staIdx   = 0;
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
  beep(1, 400, 0);

  Serial.println("\n[*] Ready. Thresholds:");
  Serial.println("    P-wave : V >= 0.7g  OR  STA/LTA >= 4.0,  3s sustained");
  Serial.println("    S-wave : H >= 0.9g,  4s sustained  (only after 15s P window)");
  Serial.println("    Beeps  : P-wave ~3s  |  S-wave ~5s");
  Serial.println("    Display: S-wave event shown for 10s then auto-reset");
  Serial.println("──────────────────────────────────────────────────────────");

  lastLoop = millis();
}

// ─────────────────────────────────────────────────────────────────────────
// loop
// ─────────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  if (now - lastLoop < SAMPLE_MS) return;
  lastLoop = now;

  // --- Read + filter ---
  readMPU();
  float rawVert  = abs(az - prevAz);
  prevAz         = az;
  float rawHoriz = sqrt(ax * ax + ay * ay);

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
      resetAllCounters();

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
          // ── CHANGED: P-wave beep ~3 seconds (6×400ms on + 100ms off = 3s) ──
          if (!warnBeepDone) { beep(6, 400, 100); warnBeepDone = true; }
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
          state = NORMAL; pConfirmCount = 0; pQuietCount = 0;
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
          // ── CHANGED: P-wave beep ~3 seconds ──────────────────────────
          if (!warnBeepDone) { beep(6, 400, 100); warnBeepDone = true; }
          Serial.println("[!] P-WAVE CONFIRMED (sustained)");
          sendUDP("WARNING", filtVertDiff, P_TO_S_SECS, 0);
          lcdLine(0, "P-WAVE DETECTED!");
          lcdLine(1, "TAKE COVER NOW! ");
        }
      } else {
        if (pConfirmCount > 0) pConfirmCount--;
        if (pConfirmCount == 0 && filtVertDiff < QUIET_THRESHOLD) {
          state = NORMAL; pQuietCount = 0;
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

      // ── KEY FIX: S-wave detection is LOCKED during the 15s P-wave window.
      // Even if H ≥ S_WAVE_THRESH, we stay in P-WAVE state and keep counting
      // down. S-wave check only begins after the full 15s has elapsed.
      if (elapsed >= (unsigned long)(P_TO_S_SECS * 1000)) {

        // 15s window is over — now check for S-wave
        if (filtHorizMag >= S_WAVE_THRESH) {
          sConfirmCount++;
          Serial.print("[?] S-wave check H="); Serial.print(filtHorizMag, 3);
          Serial.print(" cnt="); Serial.println(sConfirmCount);

          if (sConfirmCount >= S_CONFIRM_COUNT) {
            state          = SWAVE_CONFIRMED;
            swaveStartTime = now;   // ← record when we entered S-wave
            calmCount      = 0;
            sConfirmCount  = 0;
            // ── CHANGED: S-wave beep ~5 seconds (10×400ms on + 100ms off = 5s) ──
            if (!eqBeepDone) {
              beep(10, 400, 100);
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
          // No S-wave activity after 15s window — stand down
          Serial.println("[*] 15s window expired, no S-wave. Calming.");
          state = CALMING; calmCount = 0;
          digitalWrite(LED_PIN, LOW);
          break;
        }

      } else {
        // Still inside the 15s P-wave window — S-wave detection is BLOCKED.
        // Log if H is high but do NOT transition.
        if (filtHorizMag >= S_WAVE_THRESH) {
          Serial.print("[~] H="); Serial.print(filtHorizMag, 3);
          Serial.println(" (S-thresh crossed but locked — still in P-wave window)");
        }
      }

      // Reminder beep every 10s while in P-wave window
      if (now - lastReminder >= 10000) { beep(1, 150, 0); lastReminder = now; }

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
        digitalWrite(BUZZER_PIN, LOW);
      }

      if (!alertSentThisEvent) {
        if (sendUDP("EARTHQUAKE!", filtHorizMag, 0, UDP_EQ_INTERVAL)) {
          alertSentThisEvent = true;
        }
      }

      // ── KEY FIX: Auto-exit after SWAVE_DISPLAY_MS (10 seconds).
      // This gives the dashboard exactly 10 seconds to show the event,
      // then the system resets to NORMAL regardless of sensor readings.
      if (now - swaveStartTime >= SWAVE_DISPLAY_MS) {
        Serial.println("[*] 10s display timer expired → resetting to NORMAL");
        state = NORMAL;
        resetAllCounters();
        lcdLine(0, "All Clear       ");
        lcdLine(1, "Monitoring...   ");
        beep(1, 500, 0);
        break;
      }

      // Show remaining display time on LCD
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
      bool isQuiet = (filtVertDiff < QUIET_THRESHOLD && filtHorizMag < 0.7f);

      if (isQuiet) {
        calmCount++;
        if (calmCount >= CALM_COUNT) {
          Serial.println("[*] All clear → NORMAL");
          state = NORMAL;
          resetAllCounters();
          lcdLine(0, "All Clear       ");
          lcdLine(1, "Monitoring...   ");
          beep(1, 500, 0);
          break;
        }
      } else {
        if (filtHorizMag >= S_WAVE_THRESH) {
          state = SWAVE_CONFIRMED;
          swaveStartTime = now;   // ← also set timer on re-entry
          calmCount = 0;
        } else if (isPWaveSignal()) {
          state = PWAVE_DETECTING; calmCount = 0; pConfirmCount = 0;
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
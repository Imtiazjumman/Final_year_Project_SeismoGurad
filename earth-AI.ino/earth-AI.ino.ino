/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║      SeismoGuard — P/S Wave Early Warning System                ║
 * ║                  Academic Capstone Project                      ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  FIXES IN THIS VERSION:                                          ║
 * ║                                                                  ║
 * ║  1. UDP FLOODING FIXED                                           ║
 * ║     Old: EARTHQUAKE! sent every 100ms → 10 DB rows/sec          ║
 * ║     Fix: EARTHQUAKE! sent every 5s max → 12 DB rows/min         ║
 * ║     Fix: WARNING      sent every 2s max                          ║
 * ║     Fix: NORMAL       sent every 2s max                          ║
 * ║                                                                  ║
 * ║  2. THRESHOLDS RAISED TO STANDARD                                ║
 * ║     P_DIRECT_THRESH : 1.0g → 1.8g                               ║
 * ║     P_RATIO_THRESH  : 3.0  → 4.0                                ║
 * ║     P_CONFIRM_SECS  : 1s   → 2s                                  ║
 * ║     S_WAVE_THRESH   : 1.5g → 2.2g  (shake harder for EQ)        ║
 * ║     S_CONFIRM_SECS  : 1s   → 2s                                  ║
 * ║                                                                  ║
 * ║  3. CONTINUOUS ALARMING FIXED                                    ║
 * ║     Beep fires once per event (one-shot flags, unchanged)        ║
 * ║     UDP throttling means dashboard quake count increments        ║
 * ║     slowly → modal no longer re-fires every 3s refresh           ║
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
// Raised: need a meaningful vertical shake to trigger, not a tap
#define P_DIRECT_THRESH   1.5f     // filtVertDiff must reach 1.8g
#define P_RATIO_THRESH    4.0f     // OR STA/LTA ratio ≥ 4.0
#define P_CONFIRM_SECS    3        // must sustain for 2 full seconds
#define P_CONFIRM_COUNT   (P_CONFIRM_SECS * (1000 / SAMPLE_MS))   // 20 samples

// False-alarm cancellation
#define FALSE_ALARM_SECS  3
#define FALSE_ALARM_COUNT (FALSE_ALARM_SECS * (1000 / SAMPLE_MS)) // 30 samples
#define QUIET_THRESHOLD   0.08f

// ── S-WAVE THRESHOLDS ─────────────────────────────────────────────────────
// Raised significantly: must shake noticeably harder than P-wave level
#define S_WAVE_THRESH     1.8f     // filtHorizMag must reach 2.2g
#define S_CONFIRM_SECS    4       // must sustain for 2 full seconds
#define S_CONFIRM_COUNT   (S_CONFIRM_SECS * (1000 / SAMPLE_MS))   // 20 samples

// ── TIMING ────────────────────────────────────────────────────────────────
#define P_TO_S_SECS       20

// Buzzer auto-silence 5s after earthquake alarm
#define EQ_BEEP_STOP_MS   5000UL

// Calm reset
#define CALM_SECS         5
#define CALM_COUNT        (CALM_SECS * (1000 / SAMPLE_MS))        // 50 samples

// ── UDP SEND INTERVALS — THIS IS THE KEY FIX FOR DB FLOODING ─────────────
// Old code had no throttle in SWAVE_CONFIRMED:
//   sendUDP() called every 100ms → 10 packets/s → 600 DB rows/min
// Now each state has its own minimum interval between sends:
#define UDP_NORMAL_INTERVAL   2000UL   // NORMAL    → send every 2s
#define UDP_WARNING_INTERVAL  2000UL   // WARNING   → send every 2s
#define UDP_EQ_INTERVAL       5000UL   // EARTHQUAKE→ send every 5s (was 0!)

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
bool alertSentThisEvent = false; // Add this line

// ── Timestamps ────────────────────────────────────────────────────────────
unsigned long pWaveTime     = 0;
unsigned long eqBeepTime    = 0;
unsigned long lastLcdUpdate = 0;
unsigned long lastUdpSend   = 0;   // unified UDP throttle timestamp
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
// sendUDP — only sends if enough time has passed since last send
// interval = UDP_NORMAL_INTERVAL / UDP_WARNING_INTERVAL / UDP_EQ_INTERVAL
// ─────────────────────────────────────────────────────────────────────────
bool sendUDP(const char* status, float value, int eta, unsigned long interval) {
  unsigned long now = millis();
  if (now - lastUdpSend < interval) return false;   // too soon — skip
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
  alertSentThisEvent = false; // Add this line
  digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);
}

// ─────────────────────────────────────────────────────────────────────────
// isPWaveSignal — direct threshold OR STA/LTA ratio
// Vertical dominance check removed (blocks hand-shaking demos)
// ─────────────────────────────────────────────────────────────────────────
bool isPWaveSignal() {
  return (filtVertDiff >= P_DIRECT_THRESH) || (staltaRatio >= P_RATIO_THRESH);
}

// ─────────────────────────────────────────────────────────────────────────
// calibrate — 10 seconds
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
// setup
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
  Serial.println("    P-wave : V >= 1.8g  OR  STA/LTA >= 4.0,  2s sustained");
  Serial.println("    S-wave : H >= 2.2g,  2s sustained");
  Serial.println("    UDP    : NORMAL=2s  WARNING=2s  EARTHQUAKE=5s intervals");
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

  // --- ADDED FLOOR LOGIC START ---
  if (state == CALMING || state == SWAVE_CONFIRMED) {
      if (filtVertDiff < 0.05f) filtVertDiff = 0.0f; 
      if (filtHorizMag < 0.05f) filtHorizMag = 0.0f;
  } 
  // --- ADDED FLOOR LOGIC END ---
  
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
          if (!warnBeepDone) { beep(2, 300, 200); warnBeepDone = true; }
          Serial.println("[!] P-WAVE CONFIRMED");
          sendUDP("WARNING", filtVertDiff, P_TO_S_SECS, 0); // immediate on confirm
          lcdLine(0, "P-WAVE DETECTED!");
          lcdLine(1, "TAKE COVER NOW! ");
        } else {
          state = PWAVE_DETECTING;
        }
      }

      // NORMAL: send at most every 2 seconds
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
          if (!warnBeepDone) { beep(2, 300, 200); warnBeepDone = true; }
          Serial.println("[!] P-WAVE CONFIRMED (2s sustained)");
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

      // WARNING while detecting: max every 2s
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

      if (filtHorizMag >= S_WAVE_THRESH) {
        sConfirmCount++;
        Serial.print("[?] S-wave building H="); Serial.print(filtHorizMag, 3);
        Serial.print(" cnt="); Serial.println(sConfirmCount);

        if (sConfirmCount >= S_CONFIRM_COUNT) {
          state         = SWAVE_CONFIRMED;
          calmCount     = 0;
          sConfirmCount = 0;
          if (!eqBeepDone) {
            beep(5, 150, 80);
            eqBeepTime = now;
            eqBeepDone = true;
          }
          Serial.println("[!!] S-WAVE CONFIRMED — EARTHQUAKE!");
          sendUDP("EARTHQUAKE!", filtHorizMag, 0, 0); // immediate on confirm
          lcdLine(0, "!! EARTHQUAKE !!");
          lcdLine(1, "S-WAVE CONFIRMED");
          break;
        }
      } else {
        sConfirmCount = 0;
      }

      if (elapsed >= (unsigned long)(P_TO_S_SECS * 1000)) {
        Serial.println("[*] 20s window expired. Calming.");
        state = CALMING; calmCount = 0;
        digitalWrite(LED_PIN, LOW);
        break;
      }

      // Reminder every 10s
      if (now - lastReminder >= 10000) { beep(1, 150, 0); lastReminder = now; }

      // WARNING: max every 2s
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

      if (eqBeepDone && (now - eqBeepTime >= EQ_BEEP_STOP_MS)) {
        digitalWrite(BUZZER_PIN, LOW);
      }

      if (!alertSentThisEvent) { 
        if(sendUDP("EARTHQUAKE!", filtHorizMag, 0, UDP_EQ_INTERVAL)) {
            alertSentThisEvent = true; // Prevents the dashboard from flooding
        }
      }

      // Use 1.10f to ensure we exit even if sensor has small offset
      bool isQuiet = (filtVertDiff < QUIET_THRESHOLD && filtHorizMag < 1.10f);
      if (isQuiet) {
        calmCount++;
        if (calmCount >= CALM_COUNT) {
          Serial.println("[*] Shaking subsided → CALMING");
          state = CALMING; calmCount = 0;
          digitalWrite(LED_PIN,    LOW);
          digitalWrite(BUZZER_PIN, LOW);
          break;
        }
      } else {
        calmCount = 0;
      }

      if (now - lastLcdUpdate >= 500) {
        lcdLine(0, "!! EARTHQUAKE !!");
        char buf[17];
        snprintf(buf, 17, "H:%.2f  V:%.2f  ", filtHorizMag, filtVertDiff);
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
          resetAllCounters(); // This resets one-shot beep flags
          lcdLine(0, "All Clear       ");
          lcdLine(1, "Monitoring...   ");
          beep(1, 500, 0);
          break;
        }
      } else {
        if (filtHorizMag >= S_WAVE_THRESH) {
          state = SWAVE_CONFIRMED; calmCount = 0;
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
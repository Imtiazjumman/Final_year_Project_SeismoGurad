#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------- WIFI ----------------
const char* ssid = "iPhone";
const char* password = "sifat1845";

// UDP
const char* udpAddress = "172.20.10.2";
const int udpPort = 4210;
WiFiUDP udp;

// ---------------- MPU6050 ----------------
const int MPU = 0x68;

// ---------------- LCD ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- OUTPUT ----------------
const int ledPin = 2;
const int buzzerPin = 13;

// ---------------- VARIABLES ----------------
float accMagnitude = 0;
float prevMagnitude = 0;
float diff = 0;

// smoothing
float filteredDiff = 0;

// detection control
int quakeCounter = 0;

// sensitivity (adjust if needed)
float threshold = 0.8;

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin();

  pinMode(ledPin, OUTPUT);
  pinMode(buzzerPin, OUTPUT);

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("QuakeSense AI");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  // WiFi connect
  WiFi.begin(ssid, password);

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 20) {
    delay(500);
    Serial.print(".");
    timeout++;
  }

  lcd.clear();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Connected");
    lcd.print("WiFi Connected");
  } else {
    Serial.println("WiFi Failed");
    lcd.print("WiFi Failed");
  }

  delay(1500);
  lcd.clear();

  // wake MPU6050
  Wire.beginTransmission(MPU);
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission(true);
}

// ---------------- READ SENSOR ----------------
void readMPU(float& ax, float& ay, float& az) {

  Wire.beginTransmission(MPU);
  Wire.write(0x3B);
  Wire.endTransmission(false);

  int bytes = Wire.requestFrom(MPU, 14, true);
  if (bytes != 14) return;

  ax = (Wire.read() << 8 | Wire.read()) / 16384.0;
  ay = (Wire.read() << 8 | Wire.read()) / 16384.0;
  az = (Wire.read() << 8 | Wire.read()) / 16384.0;

  Wire.read();
  Wire.read();  // temp skip
  Wire.read();
  Wire.read();
  Wire.read();
  Wire.read();
}

// ---------------- LOOP ----------------
void loop() {

  float ax, ay, az;
  readMPU(ax, ay, az);

  // magnitude of acceleration
  accMagnitude = sqrt(ax * ax + ay * ay + az * az);

  // difference (motion intensity)
  diff = abs(accMagnitude - prevMagnitude);
  prevMagnitude = accMagnitude;

  // smoothing (low pass filter)
  filteredDiff = (0.7 * filteredDiff) + (0.3 * diff);

  Serial.print("Diff: ");
  Serial.print(diff);
  Serial.print(" Filtered: ");
  Serial.println(filteredDiff);

  // ---------------- DETECTION LOGIC ----------------
  if (filteredDiff > threshold) {
    quakeCounter++;
  } else {
    if (quakeCounter > 0) quakeCounter--;
  }

  String status;

  if (quakeCounter > 6) {
    status = "EARTHQUAKE!";
    triggerAlarm();
  } else if (quakeCounter > 2) {
    status = "WARNING";
  } else {
    status = "NORMAL";
  }

  // ---------------- LED ----------------
  digitalWrite(ledPin, (status == "EARTHQUAKE!") ? HIGH : LOW);

  // ---------------- UDP SEND ----------------
  String data = status + " | D:" + String(filteredDiff);

  udp.beginPacket(udpAddress, udpPort);
  udp.print(data);
  udp.endPacket();

  // ---------------- LCD ----------------
  lcd.setCursor(0, 0);
  lcd.print("Status:        ");
  lcd.setCursor(8, 0);
  lcd.print(status);

  lcd.setCursor(0, 1);
  lcd.print("D:");
  lcd.print(filteredDiff);
  lcd.print("     ");

  delay(100);
}

// ---------------- ALARM ----------------
void triggerAlarm() {
  lcd.setCursor(0, 0);
  lcd.print("!!! ALERT !!!  ");
  lcd.setCursor(0, 1);
  lcd.print("EVACUATE NOW   ");

  for (int i = 0; i < 5; i++) {
    digitalWrite(buzzerPin, HIGH);
    delay(150);
    digitalWrite(buzzerPin, LOW);
    delay(150);
  }
}
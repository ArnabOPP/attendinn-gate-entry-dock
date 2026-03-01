#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include "SPIFFS.h"
#include "time.h"

// --- CONFIG ---
#define WIFI_SSID "CHATTERJEE VILLA 4G"
#define WIFI_PASSWORD "Apurba@1234"
const String BACKEND_API_URL = "https://attendinn-backend.vercel.app/api/users/rfid-scan";

// --- NTP CONFIG (IST) ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0;

// --- PINS ---
#define SS_PIN 5
#define RST_PIN 4
#define GREEN_LED 12
#define RED_LED 14
#define BUZZER_PIN 26

LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 mfrc522(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;

const char* offlineFile = "/offline_logs.csv";

// --- Reconnect tracking ---
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 5000; // try every 5 seconds

// ─────────────────────────────────────────────
// DISPLAY HELPERS
// ─────────────────────────────────────────────
void updateLCD(String line1, String line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

void log(String msg) {
  Serial.println("[LOG] " + msg);
}

// ─────────────────────────────────────────────
// TIME
// ─────────────────────────────────────────────
unsigned long getTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    log("Time not synced, using millis fallback");
    return millis() / 1000;
  }
  time(&now);
  return now;
}

void syncNTP() {
  log("Syncing NTP time...");
  updateLCD("Syncing Time...", "NTP Server");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  delay(1500);
  unsigned long t = getTime();
  if (t > 100000) {
    log("Time synced: " + String(t));
    updateLCD("Time Synced", "OK");
  } else {
    log("Time sync FAILED");
    updateLCD("Time Sync Fail", "Using fallback");
  }
  delay(800);
}

// ─────────────────────────────────────────────
// SPIFFS OFFLINE STORAGE
// ─────────────────────────────────────────────
int countOfflineLogs() {
  if (!SPIFFS.exists(offlineFile)) return 0;
  File file = SPIFFS.open(offlineFile, FILE_READ);
  if (!file) return 0;
  int count = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) count++;
  }
  file.close();
  return count;
}

void saveOffline(String uid) {
  File file = SPIFFS.open(offlineFile, FILE_APPEND);
  if (!file) {
    log("ERROR: Cannot open offline file for writing!");
    updateLCD("Storage Error!", "Log failed");
    return;
  }
  unsigned long timestamp = getTime();
  file.println(uid + "," + String(timestamp));
  file.close();

  int total = countOfflineLogs();
  log("Saved offline: UID=" + uid + " | Timestamp=" + String(timestamp) + " | Total pending=" + String(total));
  updateLCD("Saved Locally", String(total) + " pending");
}

// ─────────────────────────────────────────────
// SYNC OFFLINE LOGS TO SERVER
// ─────────────────────────────────────────────
void syncOfflineData() {
  if (WiFi.status() != WL_CONNECTED) {
    log("Sync skipped - no WiFi");
    return;
  }
  if (!SPIFFS.exists(offlineFile)) {
    log("No offline file found, nothing to sync");
    return;
  }

  File file = SPIFFS.open(offlineFile, FILE_READ);
  if (!file || file.size() == 0) {
    file.close();
    log("Offline file empty, nothing to sync");
    return;
  }

  log("Starting sync of offline logs...");
  updateLCD("Syncing Logs...", "Please wait");

  // Read all lines into memory
  std::vector<String> lines;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) lines.push_back(line);
  }
  file.close();

  log("Found " + String(lines.size()) + " offline record(s) to sync");

  int successCount = 0;
  int failCount = 0;
  String remainingData = "";

  for (int i = 0; i < lines.size(); i++) {
    String line = lines[i];
    int commaIndex = line.indexOf(',');
    if (commaIndex < 0) continue; // malformed line

    String uid = line.substring(0, commaIndex);
    String timestamp = line.substring(commaIndex + 1);

    log("Uploading [" + String(i+1) + "/" + String(lines.size()) + "] UID=" + uid + " TS=" + timestamp);
    updateLCD("Uploading " + String(i+1) + "/" + String(lines.size()), uid.substring(0, 16));

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, BACKEND_API_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);

    String payload = "{\"uid\":\"" + uid + "\", \"timestamp\":" + timestamp + ", \"isOffline\": true}";
    log("Payload: " + payload);

    int httpCode = http.POST(payload);
    String response = http.getString();
    http.end();

    log("Server response code: " + String(httpCode));
    log("Server response body: " + response);

    if (httpCode == 200 || httpCode == 201) {
      successCount++;
      log("Upload SUCCESS for UID=" + uid);
      updateLCD("Uploaded OK", uid.substring(0, 16));
      delay(400);
    } else {
      failCount++;
      remainingData += line + "\n";
      log("Upload FAILED for UID=" + uid + " | Code=" + String(httpCode));
      updateLCD("Upload Failed", "Code:" + String(httpCode));
      delay(400);
    }
  }

  // Rewrite file with only failed entries
  File rewrite = SPIFFS.open(offlineFile, FILE_WRITE);
  rewrite.print(remainingData);
  rewrite.close();

  log("Sync complete. Success=" + String(successCount) + " | Failed=" + String(failCount));
  if (failCount == 0) {
    updateLCD("Sync Complete!", "All uploaded OK");
  } else {
    updateLCD("Sync Done", String(failCount) + " failed,retry");
  }
  delay(1500);
}

// ─────────────────────────────────────────────
// WIFI
// ─────────────────────────────────────────────
void connectToWiFi() {
  log("Attempting WiFi connection to: " + String(WIFI_SSID));
  updateLCD("Connecting WiFi", String(WIFI_SSID).substring(0, 16));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    retry++;
    log("WiFi attempt " + String(retry) + "/20...");
    // Animate dots on LCD
    String dots = "";
    for (int d = 0; d < (retry % 4); d++) dots += ".";
    updateLCD("Connecting WiFi", dots);
  }

  if (WiFi.status() == WL_CONNECTED) {
    log("WiFi connected! IP: " + WiFi.localIP().toString());
    updateLCD("WiFi Connected!", WiFi.localIP().toString());
    delay(800);
    syncNTP();
    syncOfflineData();
  } else {
    log("WiFi connection FAILED. Running in offline mode.");
    updateLCD("WiFi Failed!", "Offline Mode ON");
    delay(1200);
  }
}

void attemptReconnect() {
  unsigned long now = millis();
  if (now - lastReconnectAttempt < RECONNECT_INTERVAL) return;
  lastReconnectAttempt = now;

  log("WiFi lost. Attempting reconnect...");
  updateLCD("WiFi Lost!", "Reconnecting...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 10) {
    delay(500);
    retry++;
    log("Reconnect attempt " + String(retry) + "/10");
    updateLCD("Reconnecting..", String(retry) + "/10");
  }

  if (WiFi.status() == WL_CONNECTED) {
    log("Reconnected! IP: " + WiFi.localIP().toString());
    updateLCD("Reconnected!", WiFi.localIP().toString());
    delay(800);
    syncNTP();
    syncOfflineData(); // Push any pending offline logs immediately
  } else {
    log("Reconnect failed. Still offline.");
    updateLCD("Still Offline", "Storing locally");
    delay(800);
  }
}

// ─────────────────────────────────────────────
// FEEDBACK
// ─────────────────────────────────────────────
void triggerSuccess(String name) {
  log("Access granted for: " + name);
  updateLCD("Welcome!", name.substring(0, 16));
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(500);
  digitalWrite(BUZZER_PIN, LOW);
  delay(1000);
  digitalWrite(GREEN_LED, LOW);
}

void triggerError(String errorMsg) {
  log("Access denied: " + errorMsg);
  updateLCD("Access Denied", errorMsg.substring(0, 16));
  digitalWrite(RED_LED, HIGH);
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
  delay(1000);
  digitalWrite(RED_LED, LOW);
}

void triggerOfflineSave(int pendingCount) {
  log("Card saved offline. Total pending: " + String(pendingCount));
  updateLCD("Saved Offline", String(pendingCount) + " pending");
  digitalWrite(GREEN_LED, HIGH);
  // Two short beeps = offline save (different from online success)
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
  }
  delay(800);
  digitalWrite(GREEN_LED, LOW);
}

void showReadyScreen() {
  int pending = countOfflineLogs();
  if (pending > 0) {
    updateLCD("AttendInn", String(pending) + " offline pending");
  } else {
    updateLCD("AttendInn", "Scan ID Card");
  }
}

// ─────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  log("=== AttendInn 2.0 Boot ===");

  if (!SPIFFS.begin(true)) {
    log("SPIFFS mount FAILED!");
  } else {
    log("SPIFFS mounted OK");
  }

  lcd.init();
  lcd.backlight();
  updateLCD("AttendInn 2.0", "Starting...");
  delay(1000);

  SPI.begin();
  mfrc522.PCD_Init();
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;
  log("RFID reader initialized");

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  connectToWiFi();
  showReadyScreen();
}

// ─────────────────────────────────────────────
// MAIN LOOP
// ─────────────────────────────────────────────
void loop() {
  // If WiFi is down, keep trying to reconnect in background
  if (WiFi.status() != WL_CONNECTED) {
    attemptReconnect();
  }

  // Wait for a card
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;

  log("Card detected. Reading block 4...");
  updateLCD("Card Detected", "Reading...");

  byte block = 4;
  byte buffer[18];
  byte size = sizeof(buffer);

  if (mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &(mfrc522.uid)) != MFRC522::STATUS_OK) {
    log("Authentication FAILED for card");
    triggerError("Auth Failed");
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(1000);
    showReadyScreen();
    return;
  }

  if (mfrc522.MIFARE_Read(block, buffer, &size) == MFRC522::STATUS_OK) {
    String cardData = "";
    for (uint8_t i = 0; i < 16; i++) {
      if (isAlphaNumeric(buffer[i])) cardData += (char)buffer[i];
      else break;
    }
    cardData.trim();
    log("Card data read: '" + cardData + "'");

    if (cardData.length() == 0) {
      log("Card data is empty or unreadable");
      triggerError("Empty Card");
    } else if (WiFi.status() == WL_CONNECTED) {
      // --- ONLINE PATH ---
      log("Online mode. Sending to server: " + cardData);
      updateLCD("Scanning...", "Sending online");

      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      http.begin(client, BACKEND_API_URL);
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(8000);

      String payload = "{\"uid\":\"" + cardData + "\"}";
      log("POST payload: " + payload);

      int httpCode = http.POST(payload);
      String response = http.getString();
      http.end();

      log("HTTP response code: " + String(httpCode));
      log("HTTP response body: " + response);

      if (httpCode == 200 || httpCode == 201) {
        JsonDocument doc;
        deserializeJson(doc, response);
        String name = doc["name"] | "User";
        triggerSuccess(name);
      } else {
        log("Server rejected scan. Saving offline as fallback.");
        saveOffline(cardData);
        triggerOfflineSave(countOfflineLogs());
      }
    } else {
      // --- OFFLINE PATH ---
      log("Offline mode. Saving card locally.");
      saveOffline(cardData);
      triggerOfflineSave(countOfflineLogs());
    }
  } else {
    log("MIFARE Read FAILED");
    triggerError("Read Failed");
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(1000);
  showReadyScreen();
}
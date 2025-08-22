#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "time.h"
#include <map>

// RFID Pins (ESP32)
#define RST_PIN 22
#define SS_PIN  21

MFRC522 mfrc522(SS_PIN, RST_PIN);

// WiFi credentials
const char* ssid     = "OPPO";          
const char* password = "123456789";     

// Supabase API details
const char* supabaseUrl = "https://dlwecdlpsxiitodxssws.supabase.co/rest/v1/attendance";
const char* supabaseKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImRsd2VjZGxwc3hpaXRvZHhzc3dzIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTU1OTM1NDgsImV4cCI6MjA3MTE2OTU0OH0.xaaPCgxvw69e4TYDKfDlP1JuHPHBzu21ndeUD7pw-ng";  

// NTP config for IST
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800; // IST
const int daylightOffset_sec = 0;

// Track last action of each card
std::map<String, bool> cardStatus;       // true = checked in, false = checked out
std::map<String, String> lastCheckInTime; // store last check-in timestamp

// ----------- Get Current IST Time -----------
String getTimeIST() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01T00:00:00+05:30";
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buf) + "+05:30";
}

// ----------- Connect WiFi -----------
void connectWiFi() {
  Serial.print("🌐 Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 20) {
    delay(500);
    Serial.print(".");
    retryCount++;
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\n✅ WiFi Connected! IP: " + WiFi.localIP().toString());
  else {
    Serial.println("\n❌ Failed WiFi. Retrying...");
    delay(5000);
    connectWiFi();
  }
}

// ----------- Send Data to Supabase -----------
void sendToSupabase(String cardUID, String role, bool checkIn) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ WiFi Disconnected");
    return;
  }

  HTTPClient http;
  String now = getTimeIST();

  if (checkIn) {
    // INSERT new row for check-in
    String json = "{\"card_uid\":\"" + cardUID + "\", \"role\":\"" + role + "\", \"check_in\":\"" + now + "\", \"duration\":0}";
    http.begin(supabaseUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", supabaseKey);
    http.addHeader("Authorization", "Bearer " + String(supabaseKey));
    int code = http.POST(json);
    if (code > 0) Serial.printf("📡 Check-in Response: %d\n", code);
    else Serial.printf("❌ Check-in Error: %s\n", http.errorToString(code).c_str());
    http.end();

    lastCheckInTime[cardUID] = now; // store check-in time locally
  } else {
    // Calculate duration
    long durationSec = 0;
    if (lastCheckInTime.find(cardUID) != lastCheckInTime.end()) {
      struct tm tm_start, tm_end;
      strptime(lastCheckInTime[cardUID].c_str(), "%Y-%m-%dT%H:%M:%S", &tm_start);
      time_t t_start = mktime(&tm_start);

      strptime(now.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_end);
      time_t t_now = mktime(&tm_end);

      durationSec = difftime(t_now, t_start);
    }

    // PATCH the record where check_out is null
    String updateUrl = String(supabaseUrl) + "?card_uid=eq." + cardUID + "&check_out=is.null";
    String json = "{\"check_out\":\"" + now + "\", \"duration\":" + String(durationSec) + "}";
    http.begin(updateUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", supabaseKey);
    http.addHeader("Authorization", "Bearer " + String(supabaseKey));
    int code = http.PATCH(json);
    if (code > 0) Serial.printf("📡 Check-out Response: %d\n", code);
    else Serial.printf("❌ Check-out Error: %s\n", http.errorToString(code).c_str());
    http.end();
  }
}

// ----------- Setup -----------
void setup() {
  Serial.begin(115200);
  SPI.begin();
  mfrc522.PCD_Init();
  connectWiFi();

  // NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) Serial.println("❌ Time Sync Failed");
  else Serial.println("⏰ Time Sync Done (IST)");

  Serial.println("Place your RFID card...");
}

// ----------- Loop -----------
void loop() {
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  // Read card UID
  String cardUID = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    cardUID += String(mfrc522.uid.uidByte[i], HEX);
  }
  cardUID.toUpperCase();

  Serial.println("Card UID: " + cardUID);

  // Assign role
    // Assign role based on UID
  String role = "";
  if (cardUID == "BFD171F") role = "Master";     // BF D1 07 1F
  else if (cardUID == "B2F97C0") role = "Guest"; // B2 F9 7C 00
  else if (cardUID == "AF4D991F") role = "Cleaning"; // AF 4D 99 1F
  else role = "Unknown";  // fallback if new card detected


  // Determine check-in or check-out
  bool isCheckIn = true;
  if (cardStatus.find(cardUID) != cardStatus.end()) {
    isCheckIn = !cardStatus[cardUID];
  }
  cardStatus[cardUID] = isCheckIn;

  if (isCheckIn) {
    Serial.println("✅ " + role + " Checked IN at " + getTimeIST());
    sendToSupabase(cardUID, role, true);
  } else {
    Serial.println("✅ " + role + " Checked OUT at " + getTimeIST());
    sendToSupabase(cardUID, role, false);
  }
}
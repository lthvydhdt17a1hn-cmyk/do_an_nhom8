#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <time.h> 
#include <WebServer.h> 

#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// =========================================================
// WIFI + FIREBASE CẤU HÌNH
// =========================================================
#define WIFI_SSID        "Thích Màu Lam"
#define WIFI_PASSWORD    "28042004"
#define API_KEY          "AIzaSyC2xbEX5YGxJSaRJSeyjIb9wIKqHY138qw"
#define DATABASE_URL     "https://smartfarmesp32-58a90-default-rtdb.asia-southeast1.firebasedatabase.app/"

// =========================================================
// CẤU HÌNH PHẦN CỨNG
// =========================================================
#define DHTPIN          23
#define DHTTYPE         DHT11

#define RELAY_FAN_PIN   5
#define SERVO_FAN_PIN   26

#define RELAY_LIGHT_PIN 18  
#define RELAY_PUMP_PIN  19  
#define SERVO_FEED_PIN  25  

#define LDR_PIN         34  
#define WATER_PIN       35  

#define TEMP_ON         25.0
#define TEMP_OFF        24.0
#define LDR_THRESHOLD   2000 

// Cấu hình NTP
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; 
const int   daylightOffset_sec = 0;

WebServer server(80);

DHT dht(DHTPIN, DHTTYPE);
Servo servoFan;
Servo servoFeed; 

FirebaseData fbdoStream;
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// =========================================================
// BIẾN HỆ THỐNG (Đồng bộ theo JSON mới)
// =========================================================
int autoMode = 0; // 0: Thủ công, 1: Tự động
bool fanState = false;
bool lastFanState = false;

bool lightState = false;
bool lastLightState = false;

bool pumpState = false;
bool lastPumpState = false;

int targetHour = 7;         
int targetMinute = 30;      
bool alreadyFedToday = false; 

bool firebaseReady = false;
float lastTempSent = -999;
int lastFeedMocCount = 0; 

// TIMER
unsigned long tDHT = 0;
unsigned long tServo = 0;
unsigned long tWiFi = 0;
unsigned long tFirebase = 0;
unsigned long tSensors = 0; 
unsigned long tHistoryLog = 0; 

// SERVO QUẠT
int servoPos = 0;
int servoStep = 1;

// BIẾN PHỤC VỤ XOAY SERVO CHO ĂN NON-BLOCKING
bool isFeeding = false;
unsigned long feedingStartTime = 0;

// =========================================================
// HÀM ĐIỀU KHIỂN PHẦN CỨNG (ACTIVE LOW)
// =========================================================
void applyHardware() {
  // 1. Quạt
  if (fanState != lastFanState) {
    digitalWrite(RELAY_FAN_PIN, fanState ? LOW : HIGH);
    Serial.println(fanState ? "[RELAY] QUAT BAT" : "[RELAY] QUAT TAT");
    lastFanState = fanState;
  }
  // 2. Đèn
  if (lightState != lastLightState) {
    digitalWrite(RELAY_LIGHT_PIN, lightState ? LOW : HIGH);
    Serial.println(lightState ? "[RELAY] DEN BAT" : "[RELAY] DEN TAT");
    lastLightState = lightState;
  }
  // 3. Máy bơm
  if (pumpState != lastPumpState) {
    digitalWrite(RELAY_PUMP_PIN, pumpState ? LOW : HIGH);
    Serial.println(pumpState ? "[RELAY] BOM BAT" : "[RELAY] BOM TAT");
    lastPumpState = pumpState;
  }
}

// CẬP NHẬT TRẠNG THÁI THIẾT BỊ LÊN FIREBASE (Dạng số 0/1)
void updateStatesToFirebase() {
  if (!Firebase.ready()) return;
  Firebase.RTDB.setInt(&fbdo, "/SmartFarmESP32/thiet_bi/quat_gio", fanState ? 1 : 0);
  Firebase.RTDB.setInt(&fbdo, "/SmartFarmESP32/thiet_bi/den_suoi", lightState ? 1 : 0);
  Firebase.RTDB.setInt(&fbdo, "/SmartFarmESP32/thiet_bi/may_bom", pumpState ? 1 : 0);
}

// GHI LOG LỊCH SỬ KHI TỰ ĐỘNG CHO ĂN
void logFeedingToHistory(float luongCamMocNay) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  char path[100];
  char timeStr[10];
  sprintf(path, "/SmartFarmESP32/cam_history/%04d/%02d/%02d/mốc_%02d", 
          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, ++lastFeedMocCount);
  sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

  FirebaseJson json;
  json.add("time", timeStr);
  json.add("value", luongCamMocNay);
  Firebase.RTDB.setJSON(&fbdo, path, &json);

  // Cập nhật luôn tổng lượng cám tiêu thụ trong ngày lên màn hình chính
  int currentTotalCam = 0;
  if(Firebase.RTDB.getInt(&fbdo, "/SmartFarmESP32/cam_tieu_thu")) {
    currentTotalCam = fbdo.intData();
  }
  Firebase.RTDB.setInt(&fbdo, "/SmartFarmESP32/cam_tieu_thu", currentTotalCam + luongCamMocNay);
  Firebase.RTDB.setInt(&fbdo, "/SmartFarmESP32/thiet_bi/may_cho_an", 0);
}

// KÍCH HOẠT HÀNH ĐỘNG CHO ĂN
void startFeeding() {
  if (!isFeeding) {
    isFeeding = true;
    feedingStartTime = millis();
    servoFeed.write(180); 
    Firebase.RTDB.setInt(&fbdo, "/SmartFarmESP32/thiet_bi/may_cho_an", 1);
    Serial.println("[CHO AN] Dang mo cua thuc an...");
  }
}

void processFeedingServo() {
  if (isFeeding) {
    if (millis() - feedingStartTime >= 3000) { 
      servoFeed.write(0); 
      isFeeding = false;
      Serial.println("[CHO AN] Da dong cua thuc an. Hoan thanh!");
      logFeedingToHistory(15.0); // Ví dụ mỗi lần mở xả 15kg cám
    }
  }
}

// GHI LOG NHIỆT ĐỘ VÀO HISTORY (MỖI 2 TIẾNG HOẶC ĐỊNH KỲ)
void logTempToHistory(float tempVal) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  static int mocTempCount = 1;
  char path[100];
  char timeStr[10];
  sprintf(path, "/SmartFarmESP32/nhiet_do_history/%04d/%02d/%02d/mốc_%02d", 
          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, mocTempCount++);
  sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

  FirebaseJson json;
  json.add("time", timeStr);
  json.add("value", tempVal);
  Firebase.RTDB.setJSON(&fbdo, path, &json);
  
  if(mocTempCount > 5) mocTempCount = 1; // Reset mốc luân phiên nếu cần
}

// =========================================================
// XỬ LÝ HTTP REQUEST TỪ APP
// =========================================================
void handleFanOn() {
  if (autoMode == 1) { server.send(403, "text/plain", "ERROR: AUTO MODE IS ON"); return; }
  fanState = true; applyHardware(); updateStatesToFirebase();
  server.send(200, "text/plain", "FAN ON SUCCESS");
}

void handleFanOff() {
  if (autoMode == 1) { server.send(403, "text/plain", "ERROR: AUTO MODE IS ON"); return; }
  fanState = false; applyHardware(); updateStatesToFirebase();
  server.send(200, "text/plain", "FAN OFF SUCCESS");
}

void handleLightOn() {
  if (autoMode == 1) { server.send(403, "text/plain", "ERROR: AUTO MODE IS ON"); return; }
  lightState = true; applyHardware(); updateStatesToFirebase();
  server.send(200, "text/plain", "LIGHT ON SUCCESS");
}

void handleLightOff() {
  if (autoMode == 1) { server.send(403, "text/plain", "ERROR: AUTO MODE IS ON"); return; }
  lightState = false; applyHardware(); updateStatesToFirebase();
  server.send(200, "text/plain", "LIGHT OFF SUCCESS");
}

void handlePumpOn() {
  if (autoMode == 1) { server.send(403, "text/plain", "ERROR: AUTO MODE IS ON"); return; }
  pumpState = true; applyHardware(); updateStatesToFirebase();
  server.send(200, "text/plain", "PUMP ON SUCCESS");
}

void handlePumpOff() {
  if (autoMode == 1) { server.send(403, "text/plain", "ERROR: AUTO MODE IS ON"); return; }
  pumpState = false; applyHardware(); updateStatesToFirebase();
  server.send(200, "text/plain", "PUMP OFF SUCCESS");
}

void handleFeed() {
  startFeeding();
  server.send(200, "text/plain", "FEEDING STARTED");
}

// =========================================================
// CALLBACK FIREBASE STREAM (Lắng nghe thay đổi thực tế)
// =========================================================
void streamCallback(FirebaseStream data) {
  String path = data.dataPath();
  Serial.print("\n[FIREBASE UPDATE] Path thay doi: "); Serial.println(path);

  if (path == "/che_do") autoMode = data.intData();
  
  if (path == "/thiet_bi/quat_gio") fanState = (data.intData() == 1);
  if (path == "/thiet_bi/den_suoi") lightState = (data.intData() == 1);
  if (path == "/thiet_bi/may_bom") pumpState = (data.intData() == 1);
  if (path == "/thiet_bi/may_cho_an") {
    if (data.intData() == 1) startFeeding();
  }
  
  applyHardware();
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("[STREAM] TIMEOUT!");
}

// =========================================================
// KẾT NỐI MẠNG
// =========================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("\n[WIFI] Connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n[WIFI] Connected. IP: "); Serial.println(WiFi.localIP());
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); 
  }
}

void connectFirebase() {
  if (firebaseReady) return;
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  fbdo.setBSSLBufferSize(4096, 1024);
  fbdoStream.setResponseSize(2048);

  if (Firebase.RTDB.beginStream(&fbdoStream, "/SmartFarmESP32")) {
    Firebase.RTDB.setStreamCallback(&fbdoStream, streamCallback, streamTimeoutCallback);
    firebaseReady = true;
    Serial.println("[FIREBASE] Stream connected successfully!");
  }
}

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200);
  dht.begin();

  pinMode(RELAY_FAN_PIN, OUTPUT);
  pinMode(RELAY_LIGHT_PIN, OUTPUT);
  pinMode(RELAY_PUMP_PIN, OUTPUT);
  digitalWrite(RELAY_FAN_PIN, HIGH);   
  digitalWrite(RELAY_LIGHT_PIN, HIGH); 
  digitalWrite(RELAY_PUMP_PIN, HIGH);  

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  
  servoFan.setPeriodHertz(50);
  servoFan.attach(SERVO_FAN_PIN, 500, 2400);
  servoFan.write(90);

  servoFeed.setPeriodHertz(50);
  servoFeed.attach(SERVO_FEED_PIN, 500, 2400);
  servoFeed.write(0); 

  connectWiFi();
  connectFirebase();

  server.on("/fan/on", handleFanOn);
  server.on("/fan/off", handleFanOff);
  server.on("/light/on", handleLightOn);
  server.on("/light/off", handleLightOff);
  server.on("/pump/on", handlePumpOn);
  server.on("/pump/off", handlePumpOff);
  server.on("/feed", handleFeed); 

  server.begin();
  Serial.println("[SERVER] HTTP Server Online!");
}

// =========================================================
// LOOP CHÍNH
// =========================================================
void loop() {
  server.handleClient();

  if (millis() - tWiFi > 10000) { tWiFi = millis(); connectWiFi(); }
  if (millis() - tFirebase > 10000) { tFirebase = millis(); if (!Firebase.ready()) { firebaseReady = false; connectFirebase(); } }
  Firebase.RTDB.readStream(&fbdoStream);

  processFeedingServo();

  // --- ĐỌC CẢM BIẾN LDR & WATER (MỖI 2 GIÂY) ---
  if (millis() - tSensors > 2000) {
    tSensors = millis();
    int ldrValue = analogRead(LDR_PIN);
    int rawWater = analogRead(WATER_PIN);
    int waterPercent = constrain(map(rawWater, 0, 3000, 0, 100), 0, 100); 

    if (autoMode == 1) { 
      // Tự động bật đèn sưởi khi trời tối
      if (ldrValue > LDR_THRESHOLD) lightState = true;
      else lightState = false;

      // Tự động bật máy bơm khi thiếu nước
      if (waterPercent <= 10) pumpState = true; 
      else if (waterPercent >= 100) pumpState = false; 
    }

    if (Firebase.ready()) {
      Firebase.RTDB.setInt(&fbdo, "/SmartFarmESP32/do_am", (int)dht.readHumidity());
    }

    applyHardware(); 
    updateStatesToFirebase();
  }

  // --- HẸN GIỜ TỰ ĐỘNG CHO ĂN QUA NTP TIME ---
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    if (timeinfo.tm_hour == targetHour && timeinfo.tm_min == targetMinute) {
      if (!alreadyFedToday) {
        startFeeding();
        alreadyFedToday = true; 
      }
    } else {
      if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) {
        alreadyFedToday = false; // Reset mốc ngày mới
        lastFeedMocCount = 0; 
      }
    }
  }

  // --- ĐỌC CẢM BIẾN NHIỆT ĐỘ DHT11 ---
  if (millis() - tDHT > 2000) {
    tDHT = millis();
    float temp = dht.readTemperature();
    if (!isnan(temp)) {
      if (abs(temp - lastTempSent) >= 0.2) {
        lastTempSent = temp;
        if (Firebase.ready()) Firebase.RTDB.setFloat(&fbdo, "/SmartFarmESP32/nhiet_do", temp);
      }

      if (autoMode == 1) {
        if (temp >= TEMP_ON) fanState = true;
        else if (temp <= TEMP_OFF) fanState = false;
      }
    }
  }

  // --- ĐỊNH KỲ GHI HISTORY NHIỆT ĐỘ (VÍ DỤ MỖI 1 TIẾNG = 3600000ms) ---
  // Để test nhanh bạn có thể sửa thành 60000 (1 phút) để xem dữ liệu nhảy
  if (millis() - tHistoryLog > 3600000) {
    tHistoryLog = millis();
    float temp = dht.readTemperature();
    if (!isnan(temp) && Firebase.ready()) {
      logTempToHistory(temp);
    }
  }

  // --- ĐIỀU KHIỂN SERVO QUẠT XOAY ---
  if (fanState) {
    if (millis() - tServo >= 20) {
      tServo = millis();
      servoFan.write(servoPos);
      servoPos += servoStep;
      if (servoPos >= 180 || servoPos <= 0) servoStep = -servoStep;
    }
  } else {
    if (servoPos != 90) { servoPos = 90; servoFan.write(90); }
  }
}
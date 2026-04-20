/*
 * ESP8266 Motion Security System
 * Version: 8.0.0 - Futuristic Dashboard
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <Ticker.h>
#include <time.h>
#include <ArduinoJson.h>

// ============= CONFIGURATION =============
#define FIRMWARE_VERSION "8.0.0"

// Pin Definitions
#define PIN_RED_LED     0   // GPIO0 - D3 (Active LOW)
#define PIN_GREEN_LED   14  // GPIO14 - D5
#define PIN_BLUE_LED    15  // GPIO15 - D8
#define PIN_PIR_SENSOR  12  // GPIO12 - D6

// System Constants
#define EEPROM_SIZE      512
#define MOTION_TIMEOUT   5000      // 5 seconds no motion = end
#define FIVE_SEC_CHECK   10000     // Check at 10 seconds after start
#define WARMUP_TIME      10000     // 10 seconds sensor warmup
#define MOTION_CHECK_MS  100
#define NTP_TIMEOUT      5000

// ============= GLOBAL OBJECTS =============
ESP8266WebServer server(80);
WiFiManager wifiManager;
Ticker ledTimer;

// ============= DATA STRUCTURES =============
struct SystemConfig {
  char device_name[32];
  char sensor_name[32];
  char sensor_number[8];
  char api_endpoint[128];
  char wifi_ssid[32];
  char wifi_password[64];
  char start_time[8];        // Format: "HH:MM" (24-hour)
  char end_time[8];          // Format: "HH:MM" (24-hour)
  uint8_t buzzer_enabled;
  uint8_t led_enabled;
  uint32_t magic_number;
} config = {
  "Motion_Sensor",
  "PIR_Sensor",
  "1",
  "https://zms.zisprink.com/api/motion-event",
  "",
  "",
  "00:00",
  "23:59",
  1, 1,
  0xABCD1234
};

// Motion state
struct {
  uint32_t active_start;
  uint32_t last_trigger;
  uint32_t total_events;
  uint32_t current_duration;
  char start_time[20];
  char end_time[20];
  char event_id[30];
  uint8_t active;
  uint8_t passed_five_sec;
} motion = {0};

// System status
struct {
  uint32_t uptime;
  uint32_t successful_tx;
  uint32_t failed_tx;
  char last_tx[20];
  char last_error[64];
  char current_ssid[32];
  char current_time[20];
  uint8_t wifi_connected;
  uint8_t ntp_synced;
} status = {0};

// ============= LED CONTROL FUNCTIONS =============
void setLEDs(uint8_t red, uint8_t green, uint8_t blue) {
  // Red LED is active LOW, others are active HIGH
  digitalWrite(PIN_RED_LED, red ? LOW : HIGH);
  digitalWrite(PIN_GREEN_LED, green ? HIGH : LOW);
  digitalWrite(PIN_BLUE_LED, blue ? HIGH : LOW);
}

void setRedLED(uint8_t on) { digitalWrite(PIN_RED_LED, on ? LOW : HIGH); }
void setGreenLED(uint8_t on) { digitalWrite(PIN_GREEN_LED, on ? HIGH : LOW); }
void setBlueLED(uint8_t on) { digitalWrite(PIN_BLUE_LED, on ? HIGH : LOW); }

void ledBlink(uint8_t red, uint8_t green, uint8_t blue, int duration_ms) {
  setLEDs(red, green, blue);
  delay(duration_ms);
  setLEDs(0, 0, 0);
}

void ledPatternMotionDetect() {
  // Quick red flash for motion detection
  setLEDs(1, 0, 0);
  delay(100);
  setLEDs(0, 0, 1);
  delay(100);
  setLEDs(1, 0, 0);
  delay(100);
  setLEDs(0, 0, 1);
  delay(100);
  setLEDs(1, 0, 0);
  delay(100);
  setLEDs(0, 0, 0);
}

void ledPatternMotionActive() {
  // Breathing blue effect during active motion
  static uint32_t lastBlink = 0;
  static uint8_t state = 0;
  
  if (millis() - lastBlink > 500) {
    lastBlink = millis();
    state = !state;
    if (state) {
      setLEDs(0, 0, 1);  // Blue on
    } else {
      setLEDs(0, 0, 0);  // Blue off
    }
  }
}

void ledPatternDataSend() {
  // Green flash for successful data send
  setGreenLED(1);
  delay(200);
  setGreenLED(0);
  delay(100);
  setGreenLED(1);
  delay(200);
  setGreenLED(0);
}

void ledPatternDataFail() {
  // Red flash for failed data send
  setRedLED(1);
  delay(300);
  setRedLED(0);
  delay(100);
  setRedLED(1);
  delay(300);
  setRedLED(0);
}

void ledPatternWifiConnecting() {
  // Blue blinking during WiFi connection
  static uint32_t lastBlink = 0;
  if (millis() - lastBlink > 500) {
    lastBlink = millis();
    setBlueLED(!digitalRead(PIN_BLUE_LED));
  }
}

void ledPatternWifiConnected() {
  // Solid green when WiFi connected
  setLEDs(0, 1, 0);
}

void ledPatternSensorReady() {
  // Solid green indicates sensor ready
  setLEDs(0, 1, 0);
}

void ledPatternAPMode() {
  // Red LED on during AP mode
  setLEDs(1, 0, 0);
}

void ledPatternNTPError() {
  // Yellow (Red + Green) for NTP error
  setLEDs(1, 1, 0);
  delay(1000);
  setLEDs(0, 0, 0);
}

// ============= FUNCTION PROTOTYPES =============
void loadConfig();
void saveConfig();
void connectWiFi();
void configModeCallback(WiFiManager* mgr);
bool syncNTP();
void saveWiFiCredentials();
void getTimeString(char* buffer, size_t len);
bool inTimeRange();
void checkMotion();
bool sendEvent(const char* type, uint32_t duration, const char* time_str);
void setupWebServer();
void handleRoot();
void handleAPIStatus();
void handleAPILastEvent();
void handleAPIGetConfig();
void handleAPISaveConfig();
void handleAPITest();
void handleAPIRestart();
void handleAPIReset();
void handleSaveWiFi();
void setError(const char* error);
void printSystemInfo();
String getDeviceId();
void testIndicators();

// ============= EEPROM MANAGEMENT =============
void saveConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();
  Serial.println(F("✓ Config saved"));
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);
  
  if (config.magic_number != 0xABCD1234) {
    saveConfig();
  }
  EEPROM.end();
  Serial.println(F("✓ Config loaded"));
}

void saveWiFiCredentials() {
  strcpy(config.wifi_ssid, WiFi.SSID().c_str());
  strcpy(config.wifi_password, WiFi.psk().c_str());
  saveConfig();
  Serial.println(F("✓ WiFi saved"));
}

// ============= UTILITY FUNCTIONS =============
String getDeviceId() {
  char buf[16];
  sprintf(buf, "%08X", ESP.getChipId());
  return String(buf);
}

void getTimeString(char* buffer, size_t len) {
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  
  if (tm_info->tm_year > 100) {
    strftime(buffer, len, "%Y-%m-%d %H:%M:%S", tm_info);
  } else {
    uint32_t uptime = millis() / 1000;
    snprintf(buffer, len, "UPTIME %02u:%02u:%02u", 
             (uptime / 3600) % 24, (uptime / 60) % 60, uptime % 60);
  }
}

bool inTimeRange() {
  if (!status.ntp_synced) return true;  // If time not synced, always allow
  
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  
  int cur_minutes = tm_info->tm_hour * 60 + tm_info->tm_min;
  
  int sh, sm, eh, em;
  sscanf(config.start_time, "%d:%d", &sh, &sm);
  sscanf(config.end_time, "%d:%d", &eh, &em);
  
  int start_minutes = sh * 60 + sm;
  int end_minutes = eh * 60 + em;
  
  if (start_minutes <= end_minutes) {
    // Normal range (e.g., 00:00 to 23:59)
    return (cur_minutes >= start_minutes && cur_minutes <= end_minutes);
  } else {
    // Overnight range (e.g., 22:00 to 06:00)
    return (cur_minutes >= start_minutes || cur_minutes <= end_minutes);
  }
}

void setError(const char* error) {
  strncpy(status.last_error, error, sizeof(status.last_error) - 1);
  status.last_error[sizeof(status.last_error) - 1] = '\0';
  Serial.printf("❌ %s\n", error);
}

void printSystemInfo() {
  Serial.printf("\n📊 v%s | Heap:%u | ID:%s\n", 
    FIRMWARE_VERSION, ESP.getFreeHeap(), getDeviceId().c_str());
}

void testIndicators() {
  // Test all LEDs
  setLEDs(1, 0, 0); delay(200);
  setLEDs(0, 1, 0); delay(200);
  setLEDs(0, 0, 1); delay(200);
  setLEDs(0, 0, 0);
}

// ============= NTP SYNC =============
bool syncNTP() {
  Serial.print(F("⏰ NTP..."));
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  
  uint32_t start = millis();
  while (millis() - start < NTP_TIMEOUT) {
    delay(200);
    if (time(nullptr) > 100000) {
      char ts[30];
      getTimeString(ts, sizeof(ts));
      Serial.printf(" ✓ %s\n", ts);
      status.ntp_synced = 1;
      return true;
    }
    Serial.print(".");
  }
  Serial.println(F(" ✗"));
  status.ntp_synced = 0;
  ledPatternNTPError();
  return false;
}

// ============= WIFI MANAGEMENT =============
void configModeCallback(WiFiManager* mgr) {
  Serial.println(F("📱 AP Mode"));
  ledPatternAPMode();
}

void connectWiFi() {
  Serial.println(F("\n📡 WiFi..."));
  
  if (config.wifi_ssid[0]) {
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
      ledPatternWifiConnecting();
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("✓ %s | %s\n", config.wifi_ssid, WiFi.localIP().toString().c_str());
      ledPatternWifiConnected();
      strcpy(status.current_ssid, WiFi.SSID().c_str());
      syncNTP();
      return;
    }
  }
  
  wifiManager.setTimeout(60);
  wifiManager.setAPCallback(configModeCallback);
  if (!wifiManager.autoConnect("Motion-Sensor-AP")) {
    ESP.restart();
  }
  
  saveWiFiCredentials();
  ledPatternWifiConnected();
  strcpy(status.current_ssid, WiFi.SSID().c_str());
  Serial.printf("✓ %s\n", WiFi.localIP().toString().c_str());
  syncNTP();
}

// ============= URL HELPER FUNCTIONS =============
bool parseUrl(const char* url, char* host, int hostSize, const char** path) {
  const char* baseUrl = url;
  if (strncmp(baseUrl, "https://", 8) == 0) baseUrl += 8;
  else if (strncmp(baseUrl, "http://", 7) == 0) baseUrl += 7;
  
  const char* pathStart = strchr(baseUrl, '/');
  if (!pathStart) pathStart = "/api/motion-event";
  
  int hostLen = pathStart - baseUrl;
  if (hostLen >= hostSize) return false;
  
  strncpy(host, baseUrl, hostLen);
  host[hostLen] = '\0';
  *path = pathStart;
  return true;
}

void urlEncodeSpaces(char* str) {
  for (char* p = str; *p; p++) {
    if (*p == ' ') {
      memmove(p + 3, p + 1, strlen(p));
      *p++ = '%';
      *p++ = '2';
      *p = '0';
    }
  }
}

// ============= SINGLE SEND EVENT FUNCTION =============
bool sendEvent(const char* type, uint32_t duration, const char* time_str) {
  //=============================================================================
  // BASIC CHECKS
  //=============================================================================
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("❌ WiFi not connected"));
    return false;
  }

  if (!inTimeRange()) {
    Serial.println(F("⏰ Outside allowed time"));
    return false;
  }

  if (strlen(config.api_endpoint) < 10) {
    Serial.println(F("❌ Invalid API endpoint"));
    return false;
  }

  Serial.printf("Heap before request: %d\n", ESP.getFreeHeap());

  //=============================================================================
  // DEVICE ID (MAC)
  //=============================================================================
  char deviceId[13];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  sprintf(deviceId, "%02X%02X%02X%02X%02X%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  //=============================================================================
  // BUILD JSON PAYLOAD
  //=============================================================================
  String json = "{";

  if (type[0] == 'S') {
    json += "\"event_type\":\"START\",";
    json += "\"device_id\":\"" + String(deviceId) + "\",";
    json += "\"device_name\":\"" + String(config.device_name) + "\",";
    json += "\"sensor_number\":\"" + String(config.sensor_number) + "\",";
    json += "\"sensor_name\":\"" + String(config.sensor_name) + "\",";
    json += "\"detected_at\":\"" + String(time_str) + "\"";

    Serial.println("📡 Sending START event");
  }
  else {
    if (!motion.event_id[0]) {
      Serial.println(F("❌ No event_id for END"));
      return false;
    }

    json += "\"event_type\":\"END\",";
    json += "\"event_id\":\"" + String(motion.event_id) + "\",";
    json += "\"duration\":" + String(duration);

    Serial.println("📡 Sending END event");
  }

  json += "}";

  //=============================================================================
  // HTTPS CLIENT (STABLE CONFIG)
  //=============================================================================
  WiFiClientSecure client;
  client.setInsecure();              // skip cert validation
  client.setTimeout(8000);
  client.setBufferSizes(512, 512);

  HTTPClient http;

  if (!http.begin(client, config.api_endpoint)) {
    Serial.println(F("❌ HTTP begin failed"));
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-ID", deviceId);

  //=============================================================================
  // SEND REQUEST
  //=============================================================================
  int httpCode = http.POST(json);

  Serial.printf("🌐 HTTP Code: %d\n", httpCode);

  if (httpCode <= 0) {
    Serial.printf("❌ HTTP error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    ledPatternDataFail();
    return false;
  }

  //=============================================================================
  // RESPONSE
  //=============================================================================
  String payload = http.getString();
  Serial.printf("📩 Response: %s\n", payload.c_str());

  bool success = false;

  if (httpCode == HTTP_CODE_OK) {
    success = true;
    status.successful_tx++;
    getTimeString(status.last_tx, sizeof(status.last_tx));
    ledPatternDataSend();

    //=============================================================================
    // PARSE event_id (FOR START)
    //=============================================================================
    if (type[0] == 'S') {
      int start = payload.indexOf("\"event_id\":\"");

      if (start != -1) {
        start += 12;
        int end = payload.indexOf("\"", start);

        if (end != -1) {
          String id = payload.substring(start, end);

          if (id.length() < sizeof(motion.event_id)) {
            id.toCharArray(motion.event_id, sizeof(motion.event_id));
            Serial.printf("✅ Event ID saved: %s\n", motion.event_id);
          }
        }
      }
    }

    Serial.println("✅ Event sent successfully");
  }
  else {
    status.failed_tx++;
    ledPatternDataFail();
    Serial.printf("❌ Server returned: %d\n", httpCode);
  }

  http.end();
  return success;
}

// ============= FIXED MOTION DETECTION FUNCTION =============
void checkMotion() {
  static int lastState = LOW;
  static uint32_t lastCheck = 0;
  static uint8_t warmedUp = 0;
  static uint32_t fiveSecCheckTime = 0;
  
  uint32_t now = millis();
  
  // Rate limiting
  if (now - lastCheck < MOTION_CHECK_MS) return;
  lastCheck = now;
  
  // Warmup period
  if (!warmedUp) {
    if (now > WARMUP_TIME) {
      warmedUp = 1;
      Serial.println(F("✓ Motion sensor ready"));
      ledPatternSensorReady();
    }
    return;  // Don't check motion until warmed up
  }
  
  // Read current PIR state
  int cur = digitalRead(PIN_PIR_SENSOR);
  
  // DEBUG: Print state every 10 seconds
  static uint32_t lastDebugPrint = 0;
  if (now - lastDebugPrint > 10000) {
    lastDebugPrint = now;
    Serial.printf("PIR: %d, Motion active: %s, Event #%u\n", cur, motion.active ? "YES" : "NO", motion.total_events);
  }
  
  //=====================================================================
  // MOTION START DETECTED (LOW → HIGH transition)
  //=====================================================================
  if (!motion.active && cur == HIGH && lastState == LOW) {
    motion.active = 1;
    motion.active_start = now;
    motion.last_trigger = now;
    motion.total_events++;
    motion.passed_five_sec = 0;
    motion.event_id[0] = 0;  // Clear old event_id
    
    getTimeString(motion.start_time, sizeof(motion.start_time));
    motion.end_time[0] = 0;
    
    Serial.printf("\n🔴 MOTION #%u START at %s\n", motion.total_events, motion.start_time);
    Serial.printf("   Will check again in %us\n", FIVE_SEC_CHECK/1000);
    
    ledPatternMotionDetect();

    // Send START event
    if (!sendEvent("S", 0, motion.start_time)) {
      Serial.println(F("⚠ START send failed"));
    }
    
    fiveSecCheckTime = now + FIVE_SEC_CHECK;
  }
  
  //=====================================================================
  // MOTION ACTIVE - update last_trigger on any motion
  //=====================================================================
  else if (motion.active && cur == HIGH) {
    motion.last_trigger = now;
    ledPatternMotionActive();
  }
  
  //=====================================================================
  // CHECK 5-SECOND MARK (only once)
  //=====================================================================
  if (motion.active && !motion.passed_five_sec && now >= fiveSecCheckTime) {
    motion.passed_five_sec = 1;
    
    if (cur == LOW) {
      // No motion at 5-second mark - end immediately
      Serial.println(F("   5s reached - NO MOTION, ending now"));
      
      uint32_t duration = (now - motion.active_start) / 1000;
      motion.current_duration = duration;
      getTimeString(motion.end_time, sizeof(motion.end_time));
      
      Serial.printf("\n🟢 MOTION #%u END at %s (duration: %us)\n", 
                    motion.total_events, motion.end_time, duration);
      
      ledPatternMotionDetect();
      
      // Send END event
      if (!sendEvent("E", duration, motion.end_time)) {
        Serial.println(F("⚠ END send failed"));
      }
      
      motion.active = 0;
      setLEDs(0, 1, 0);  // Back to green (sensor ready)
    } else {
      // Motion still present at 5-second mark
      Serial.println(F("   5s reached - MOTION STILL PRESENT, continuing"));
    }
  }
  
  //=====================================================================
  // NORMAL MOTION TIMEOUT (after passing 5-second check)
  //=====================================================================
  else if (motion.active && motion.passed_five_sec && cur == LOW && 
           (now - motion.last_trigger > MOTION_TIMEOUT)) {
    
    uint32_t duration = (now - motion.active_start) / 1000;
    motion.current_duration = duration;
    getTimeString(motion.end_time, sizeof(motion.end_time));
    
    Serial.printf("\n🟢 MOTION #%u END at %s (duration: %us)\n", 
                  motion.total_events, motion.end_time, duration);
    
    ledPatternMotionDetect();
    
    // Send END event
    if (!sendEvent("E", duration, motion.end_time)) {
      Serial.println(F("⚠ END send failed"));
    }
    
    motion.active = 0;
    setLEDs(0, 1, 0);  // Back to green (sensor ready)
  }
  
  // Update last state for edge detection
  lastState = cur;
}

// ============= WEB HANDLERS - FUTURISTIC DASHBOARD =============
void handleRoot() {
  server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=yes">
  <title>⚡ MOTION SECURITY SYSTEM</title>
  <link rel="stylesheet" href="/style.css">
  <link href="https://fonts.googleapis.com/css2?family=Orbitron:wght@400;500;600;700;800;900&display=swap" rel="stylesheet">
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: 'Orbitron', sans-serif;
      background: radial-gradient(circle at 20% 30%, #0a0f1e, #03050b);
      min-height: 100vh;
      padding: 20px;
      color: #e0e0ff;
      position: relative;
      overflow-x: hidden;
    }
    body::before {
      content: '';
      position: absolute;
      top: -50%;
      left: -50%;
      width: 200%;
      height: 200%;
      background: repeating-linear-gradient(
        45deg,
        transparent,
        transparent 20px,
        rgba(0, 255, 255, 0.03) 20px,
        rgba(0, 255, 255, 0.03) 40px
      );
      animation: scan 20s linear infinite;
      pointer-events: none;
      z-index: 0;
    }
    @keyframes scan {
      0% { transform: translate(0, 0) rotate(0deg); }
      100% { transform: translate(50px, 50px) rotate(10deg); }
    }
    .container {
      max-width: 1000px;
      margin: 0 auto;
      position: relative;
      z-index: 10;
    }
    .card {
      background: rgba(10, 20, 40, 0.75);
      backdrop-filter: blur(15px);
      -webkit-backdrop-filter: blur(15px);
      border: 1px solid rgba(0, 255, 255, 0.3);
      border-radius: 30px;
      padding: 25px;
      margin-bottom: 20px;
      box-shadow: 0 20px 40px rgba(0, 0, 0, 0.8), 0 0 0 2px rgba(0, 255, 255, 0.1), 0 0 30px rgba(0, 255, 255, 0.3);
      transition: all 0.3s ease;
      position: relative;
      overflow: hidden;
    }
    .card::after {
      content: '';
      position: absolute;
      top: -2px;
      left: -2px;
      right: -2px;
      bottom: -2px;
      background: linear-gradient(45deg, #00ffff, #ff00ff, #00ffff);
      border-radius: 32px;
      z-index: -2;
      opacity: 0.3;
      filter: blur(10px);
    }
    .card::before {
      content: '';
      position: absolute;
      top: 0;
      left: -100%;
      width: 100%;
      height: 100%;
      background: linear-gradient(90deg, transparent, rgba(255, 255, 255, 0.1), transparent);
      transition: left 0.5s;
      z-index: 1;
      pointer-events: none;
    }
    .card:hover::before {
      left: 100%;
    }
    .card:hover {
      box-shadow: 0 20px 40px rgba(0, 0, 0, 0.9), 0 0 0 3px cyan, 0 0 50px cyan;
    }
    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 15px;
    }
    .header h1 {
      font-size: 2.2em;
      font-weight: 800;
      text-transform: uppercase;
      background: linear-gradient(45deg, #00ffff, #ff00ff, #00ffff);
      -webkit-background-clip: text;
      background-clip: text;
      color: transparent;
      text-shadow: 0 0 20px cyan, 0 0 40px magenta;
      letter-spacing: 3px;
    }
    .badge {
      display: inline-block;
      padding: 8px 16px;
      border-radius: 50px;
      font-size: 0.9em;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 1.5px;
      border: 1px solid;
      box-shadow: 0 0 15px currentColor;
    }
    .badge-success { background: rgba(0, 255, 0, 0.15); border-color: #0f0; color: #0f0; text-shadow: 0 0 10px #0f0; }
    .badge-danger { background: rgba(255, 0, 0, 0.15); border-color: #f00; color: #f00; text-shadow: 0 0 10px #f00; }
    .badge-warning { background: rgba(255, 255, 0, 0.15); border-color: #ff0; color: #ff0; text-shadow: 0 0 10px #ff0; }
    .badge-info { background: rgba(0, 255, 255, 0.15); border-color: #0ff; color: #0ff; text-shadow: 0 0 10px #0ff; }
    .stats-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
      gap: 20px;
      margin: 20px 0;
    }
    .stat-card {
      background: rgba(0, 20, 40, 0.7);
      border: 1px solid rgba(0, 255, 255, 0.5);
      border-radius: 20px;
      padding: 20px;
      text-align: center;
      backdrop-filter: blur(5px);
      transition: 0.3s;
      position: relative;
      overflow: hidden;
    }
    .stat-card::before {
      content: '';
      position: absolute;
      top: 0;
      left: 0;
      width: 100%;
      height: 2px;
      background: linear-gradient(90deg, transparent, cyan, magenta, cyan, transparent);
      animation: scanline 3s linear infinite;
    }
    @keyframes scanline {
      0% { transform: translateX(-100%); }
      100% { transform: translateX(100%); }
    }
    .stat-card:hover {
      transform: translateY(-5px) scale(1.02);
      border-color: magenta;
      box-shadow: 0 0 30px magenta;
    }
    .stat-label {
      font-size: 0.8em;
      color: #8a8aff;
      text-transform: uppercase;
      letter-spacing: 2px;
    }
    .stat-value {
      font-size: 2.5em;
      font-weight: 900;
      margin-top: 5px;
      text-shadow: 0 0 20px currentColor;
    }
    .stat-unit {
      font-size: 0.5em;
      opacity: 0.7;
    }
    .motion-active { color: #ff5555; animation: pulse 1s infinite; }
    .motion-inactive { color: #55ff55; }
    @keyframes pulse {
      0% { text-shadow: 0 0 10px #f00, 0 0 30px #f00; }
      50% { text-shadow: 0 0 30px #f00, 0 0 60px #f00; }
      100% { text-shadow: 0 0 10px #f00, 0 0 30px #f00; }
    }
    .event-card {
      background: rgba(0, 0, 0, 0.5);
      border: 1px solid cyan;
      border-radius: 15px;
      padding: 15px;
    }
    .event-row {
      display: flex;
      justify-content: space-between;
      padding: 10px 0;
      border-bottom: 1px solid rgba(0, 255, 255, 0.3);
      font-size: 1em;
    }
    .event-row:last-child { border-bottom: none; }
    .event-label { color: #aaaaff; font-weight: 500; }
    .event-value { color: cyan; font-weight: 700; text-shadow: 0 0 5px cyan; }
    .wifi-section {
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 15px;
    }
    .wifi-info { display: flex; align-items: center; gap: 15px; }
    .wifi-icon { font-size: 2em; filter: drop-shadow(0 0 10px cyan); }
    .wifi-ssid { font-weight: 700; color: cyan; }
    .wifi-ip { font-size: 0.9em; color: #aaaaff; }
    .form-row {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 20px;
      margin-bottom: 20px;
    }
    .form-group { margin-bottom: 15px; }
    label {
      display: block;
      font-size: 0.8em;
      color: #aaaaff;
      text-transform: uppercase;
      letter-spacing: 2px;
      margin-bottom: 8px;
    }
    input, select {
      width: 100%;
      padding: 12px 15px;
      background: rgba(0, 10, 20, 0.8);
      border: 2px solid rgba(0, 255, 255, 0.5);
      border-radius: 15px;
      color: cyan;
      font-family: 'Orbitron', monospace;
      font-size: 1em;
      transition: all 0.3s;
    }
    input:focus, select:focus {
      outline: none;
      border-color: magenta;
      box-shadow: 0 0 20px magenta;
    }
    input[type="checkbox"] {
      width: 20px;
      height: 20px;
      margin-right: 10px;
      accent-color: cyan;
    }
    .checkbox-group {
      display: flex;
      gap: 30px;
      margin: 20px 0;
    }
    .checkbox {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: 1em;
    }
    .button-group {
      display: flex;
      gap: 15px;
      flex-wrap: wrap;
      margin-top: 20px;
    }
    .btn {
      padding: 14px 25px;
      border: none;
      border-radius: 50px;
      font-family: 'Orbitron', sans-serif;
      font-weight: 700;
      font-size: 0.9em;
      text-transform: uppercase;
      letter-spacing: 2px;
      cursor: pointer;
      transition: all 0.3s;
      flex: 1;
      min-width: 120px;
      position: relative;
      overflow: hidden;
      border: 1px solid transparent;
      background: linear-gradient(45deg, #0a0f1e, #1a1f3e);
      color: cyan;
      box-shadow: 0 0 15px rgba(0, 255, 255, 0.5);
    }
    .btn::before {
      content: '';
      position: absolute;
      top: 0;
      left: -100%;
      width: 100%;
      height: 100%;
      background: linear-gradient(90deg, transparent, rgba(255, 255, 255, 0.2), transparent);
      transition: left 0.5s;
    }
    .btn:hover::before { left: 100%; }
    .btn:hover {
      transform: translateY(-3px);
      box-shadow: 0 0 30px cyan, 0 0 60px magenta;
      border-color: magenta;
    }
    .btn-primary { background: linear-gradient(45deg, #0a2a4a, #1a3a6a); }
    .btn-warning { background: linear-gradient(45deg, #4a0a0a, #6a1a1a); color: #ffaaaa; }
    .message {
      position: fixed;
      top: 30px;
      right: 30px;
      padding: 15px 30px;
      border-radius: 50px;
      font-size: 1em;
      z-index: 2000;
      display: none;
      animation: slideIn 0.3s, glowPulse 2s infinite;
      border: 1px solid;
    }
    @keyframes slideIn {
      from { transform: translateX(100%) rotate(10deg); opacity: 0; }
      to { transform: translateX(0) rotate(0); opacity: 1; }
    }
    @keyframes glowPulse {
      0% { box-shadow: 0 0 10px currentColor; }
      50% { box-shadow: 0 0 30px currentColor; }
      100% { box-shadow: 0 0 10px currentColor; }
    }
    .message.success { background: rgba(0, 50, 0, 0.9); color: #0f0; border-color: #0f0; }
    .message.error { background: rgba(50, 0, 0, 0.9); color: #f00; border-color: #f00; }
    .footer {
      text-align: center;
      color: rgba(170, 170, 255, 0.5);
      font-size: 0.8em;
      margin-top: 30px;
      text-shadow: 0 0 10px rgba(0, 255, 255, 0.3);
    }
    @media (max-width: 600px) {
      .form-row { grid-template-columns: 1fr; }
      .button-group { flex-direction: column; }
      .header h1 { font-size: 1.5em; }
    }
  </style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="header">
      <div>
        <h1>⚡ MOTION SECURITY SYSTEM</h1>
        <p style="color: #aaaaff; letter-spacing: 2px;">v8.0.0 | MOTION SENSOR</p>
      </div>
      <div>
        <span class="badge" id="connectionStatus">● INITIALIZING</span>
      </div>
    </div>
    <div style="display: flex; gap: 15px; margin-top: 10px; flex-wrap: wrap;">
      <span class="badge badge-info" id="deviceName">-</span>
      <span class="badge badge-info">HEAP: <span id="freeHeap">-</span></span>
    </div>
  </div>

  <div class="card">
    <div class="wifi-section">
      <div class="wifi-info">
        <span class="wifi-icon">📡</span>
        <div>
          <div class="wifi-ssid" id="wifiSSID">-</div>
          <div class="wifi-ip" id="wifiIP">-</div>
        </div>
      </div>
      <div>
        <span class="badge" id="timeRangeStatus">⏰ RANGE: ACTIVE</span>
      </div>
    </div>
  </div>

  <div class="stats-grid">
    <div class="stat-card">
      <div class="stat-label">MOTION STATUS</div>
      <div class="stat-value" id="motionStatus">INACTIVE</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">TOTAL EVENTS</div>
      <div class="stat-value" id="totalEvents">0</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">PENDING</div>
      <div class="stat-value" id="pendingStatus">0</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">LAST TX</div>
      <div class="stat-value" id="lastTx">-</div>
    </div>
  </div>

  <div class="stats-grid">
    <div class="stat-card">
      <div class="stat-label">SUCCESS</div>
      <div class="stat-value" id="successTx">0</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">FAILED</div>
      <div class="stat-value" id="failedTx">0</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">CURRENT TIME (24h)</div>
      <div class="stat-value" id="currentDateTime">--:--:--</div>
    </div>
  </div>

  <div class="card">
    <h3 style="color: cyan; margin-bottom: 15px;">📡 LAST EVENT TELEMETRY</h3>
    <div class="event-card">
      <div class="event-row"><span class="event-label">Event #</span><span class="event-value" id="eventNumber">-</span></div>
      <div class="event-row"><span class="event-label">Event ID</span><span class="event-value" id="eventId">-</span></div>
      <div class="event-row"><span class="event-label">Start</span><span class="event-value" id="eventStart">-</span></div>
      <div class="event-row"><span class="event-label">End</span><span class="event-value" id="eventEnd">-</span></div>
      <div class="event-row"><span class="event-label">Duration</span><span class="event-value" id="eventDuration">-</span></div>
      <div class="event-row"><span class="event-label">Status</span><span class="event-value" id="eventStatus">-</span></div>
    </div>
  </div>

  <div class="card">
    <h3 style="color: cyan; margin-bottom: 15px;">⚙️ CONFIGURATION SETTING</h3>
    <form id="configForm">
      <div class="form-row">
        <div class="form-group"><label>DEVICE NAME</label><input id="deviceNameInput" placeholder="Motion_Sensor"></div>
        <div class="form-group"><label>SENSOR NAME</label><input id="sensorNameInput" placeholder="PIR_Sensor"></div>
      </div>
      <div class="form-row">
        <div class="form-group"><label>SENSOR #</label><input id="sensorNumberInput" value="1"></div>
        <div class="form-group"><label>API ENDPOINT</label><input id="apiEndpointInput" placeholder="https://..."></div>
      </div>
      <div class="form-row">
        <div class="form-group"><label>WIFI SSID</label><input id="wifiSsidInput"></div>
        <div class="form-group"><label>WIFI PASS</label><input type="password" id="wifiPasswordInput"></div>
      </div>
      <div class="form-row">
        <div class="form-group"><label>START TIME (24h)</label><input type="time" id="startTimeInput" value="00:00"></div>
        <div class="form-group"><label>END TIME (24h)</label><input type="time" id="endTimeInput" value="23:59"></div>
      </div>
    
      <div class="button-group">
        <button type="button" class="btn btn-primary" onclick="saveConfig()">💾 SAVE CONFIG</button>
        <button type="button" class="btn btn-primary" onclick="saveWiFi()">📡 SAVE WIFI</button>
      </div>
      <div class="button-group">
        <button type="button" class="btn btn-primary" onclick="testMotion()">🔬 TEST MOTION</button>
        <button type="button" class="btn btn-primary" onclick="resetStats()">📊 RESET STATS</button>
        <button type="button" class="btn btn-warning" onclick="restartDevice()">🔄 RESTART</button>
      </div>
    </form>
  </div>
  <div class="footer">⚡ NEURAL MOTION CORE v8.0.0 | QUANTUM SENSOR ARRAY ⚡</div>
</div>
<div id="message" class="message"></div>
<script src="/script.js"></script>
</body>
</html>
  )rawliteral");
}

void handleCSS() {
  server.send(200, "text/css", "");
}

void handleJS() {
  server.send(200, "application/javascript", R"rawliteral(
let si, lastEventTimer;

document.addEventListener('DOMContentLoaded', function() {
  updateStatus();
  updateLastEvent();
  loadConfig();
  
  si = setInterval(updateStatus, 2000);
  lastEventTimer = setInterval(updateLastEvent, 3000);
});

function updateStatus() {
  fetch('/api/status')
    .then(response => response.json())
    .then(data => {
      const motionEl = document.getElementById('motionStatus');
      if (data.motion) {
        motionEl.innerHTML = 'ACTIVE';
        motionEl.className = 'stat-value motion-active';
      } else {
        motionEl.innerHTML = 'INACTIVE';
        motionEl.className = 'stat-value motion-inactive';
      }
      
      document.getElementById('totalEvents').innerHTML = data.total_events;
      document.getElementById('pendingStatus').innerHTML = data.pending ? 'PENDING' : 'NONE';
      document.getElementById('lastTx').innerHTML = data.last_tx || 'NEVER';
      document.getElementById('successTx').innerHTML = data.success;
      document.getElementById('failedTx').innerHTML = data.failed;
      document.getElementById('currentDateTime').innerHTML = data.current_time || '--:--:--';
      document.getElementById('freeHeap').innerHTML = data.free_heap + ' B';
      
      document.getElementById('wifiSSID').innerHTML = data.ssid || 'NOT CONNECTED';
      document.getElementById('wifiIP').innerHTML = data.ip || '0.0.0.0';
      
      const connEl = document.getElementById('connectionStatus');
      if (data.wifi) {
        connEl.innerHTML = '● ONLINE';
        connEl.className = 'badge badge-success';
      } else {
        connEl.innerHTML = '● OFFLINE';
        connEl.className = 'badge badge-danger';
      }
      
      const rangeEl = document.getElementById('timeRangeStatus');
      if (data.in_time_range) {
        rangeEl.innerHTML = '⏰ RANGE: ACTIVE';
        rangeEl.className = 'badge badge-success';
      } else {
        rangeEl.innerHTML = '⏰ RANGE: INACTIVE';
        rangeEl.className = 'badge badge-warning';
      }
    })
    .catch(error => {
      console.log('Status error:', error);
    });
}

function updateLastEvent() {
  fetch('/api/last-event')
    .then(response => response.json())
    .then(data => {
      document.getElementById('eventNumber').innerHTML = '#' + (data.event_number || '0');
      document.getElementById('eventId').innerHTML = data.event_id || '-';
      document.getElementById('eventStart').innerHTML = data.start_time || '-';
      document.getElementById('eventEnd').innerHTML = data.end_time || '-';
      document.getElementById('eventDuration').innerHTML = (data.duration || '0') + 's';
      
      const statusEl = document.getElementById('eventStatus');
      if (data.active) {
        statusEl.innerHTML = 'ACTIVE';
        statusEl.className = 'event-value motion-active';
      } else {
        statusEl.innerHTML = 'ENDED';
        statusEl.className = 'event-value';
      }
    })
    .catch(error => {
      console.log('Last event error:', error);
    });
}

function loadConfig() {
  fetch('/api/config')
    .then(response => response.json())
    .then(data => {
      document.getElementById('deviceNameInput').value = data.device_name || '';
      document.getElementById('sensorNameInput').value = data.sensor_name || '';
      document.getElementById('sensorNumberInput').value = data.sensor_number || '1';
      document.getElementById('apiEndpointInput').value = data.api_endpoint || '';
      document.getElementById('wifiSsidInput').value = data.wifi_ssid || '';
      document.getElementById('startTimeInput').value = data.start_time || '00:00';
      document.getElementById('endTimeInput').value = data.end_time || '23:59'; 
      document.getElementById('deviceName').innerHTML = data.device_name || '-';
    })
    .catch(error => {
      console.log('Config error:', error);
    });
}

function saveConfig() {
  const fd = new FormData();
  fd.append('device_name', document.getElementById('deviceNameInput').value);
  fd.append('sensor_name', document.getElementById('sensorNameInput').value);
  fd.append('sensor_number', document.getElementById('sensorNumberInput').value);
  fd.append('api_endpoint', document.getElementById('apiEndpointInput').value);
  fd.append('wifi_ssid', document.getElementById('wifiSsidInput').value);
  
  const wp = document.getElementById('wifiPasswordInput').value;
  if (wp) fd.append('wifi_password', wp);
  
  fd.append('start_time', document.getElementById('startTimeInput').value);
  fd.append('end_time', document.getElementById('endTimeInput').value);

  fetch('/api/config', { method: 'POST', body: fd })
    .then(response => response.json())
    .then(data => {
      if (data.success) {
        showMessage('✓ CONFIGURATION SAVED', 'success');
        loadConfig();
      }
    })
    .catch(error => showMessage('✗ SAVE FAILED', 'error'));
}

function saveWiFi() {
  const s = document.getElementById('wifiSsidInput').value;
  const p = document.getElementById('wifiPasswordInput').value;
  if (!s) { showMessage('ENTER SSID', 'error'); return; }
  
  const fd = new FormData();
  fd.append('ssid', s);
  fd.append('password', p);
  
  fetch('/save-wifi', { method: 'POST', body: fd })
    .then(response => response.text())
    .then(html => { 
      document.open(); 
      document.write(html); 
      document.close(); 
    })
    .catch(error => showMessage('✗ WIFI SAVE FAILED', 'error'));
}

function testMotion() {
  fetch('/api/test', { method: 'POST' })
    .then(response => response.json())
    .then(data => {
      if (data.success) {
        showMessage('✓ TEST TRIGGERED', 'success');
      } else {
        showMessage('⚠ ' + data.message, 'error');
      }
    })
    .catch(error => showMessage('✗ TEST FAILED', 'error'));
}

function resetStats() {
  if (confirm('RESET ALL STATISTICS?')) {
    fetch('/api/reset', { method: 'POST' })
      .then(() => showMessage('✓ STATISTICS RESET', 'success'))
      .catch(() => showMessage('✗ RESET FAILED', 'error'));
  }
}

function restartDevice() {
  if (confirm('RESTART DEVICE?')) {
    fetch('/api/restart', { method: 'POST' })
      .then(() => showMessage('🔄 RESTARTING...', 'success'))
      .catch(() => showMessage('✗ RESTART FAILED', 'error'));
  }
}

function showMessage(text, type) {
  const m = document.getElementById('message');
  m.innerHTML = text;
  m.className = 'message ' + type + ' show';
  setTimeout(() => m.classList.remove('show'), 3000);
}
  )rawliteral");
}

void handleAPIStatus() {
  status.uptime = millis() / 1000;
  getTimeString(status.current_time, sizeof(status.current_time));
  
  StaticJsonDocument<256> doc;
  doc["wifi"] = (WiFi.status() == WL_CONNECTED);
  doc["ssid"] = status.current_ssid;
  doc["ntp"] = status.ntp_synced;
  doc["motion"] = motion.active;
  doc["last_tx"] = status.last_tx;
  doc["last_error"] = status.last_error;
  doc["uptime"] = status.uptime;
  doc["total_events"] = motion.total_events;
  doc["success"] = status.successful_tx;
  doc["failed"] = status.failed_tx;
  doc["pending"] = 0;
  doc["ip"] = WiFi.localIP().toString();
  doc["version"] = FIRMWARE_VERSION;
  doc["current_time"] = status.current_time;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["time_range_start"] = config.start_time;
  doc["time_range_end"] = config.end_time;
  doc["in_time_range"] = inTimeRange();
  
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAPILastEvent() {
  StaticJsonDocument<256> doc;
  doc["event_number"] = motion.total_events;
  doc["event_id"] = motion.event_id;
  doc["start_time"] = motion.start_time;
  doc["end_time"] = motion.active ? "Active" : motion.end_time;
  doc["duration"] = motion.active ? (millis() - motion.active_start)/1000 : motion.current_duration;
  doc["active"] = motion.active;
  
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAPIGetConfig() {
  StaticJsonDocument<256> doc;
  doc["device_name"] = config.device_name;
  doc["sensor_name"] = config.sensor_name;
  doc["sensor_number"] = config.sensor_number;
  doc["api_endpoint"] = config.api_endpoint;
  doc["wifi_ssid"] = config.wifi_ssid;
  doc["start_time"] = config.start_time;
  doc["end_time"] = config.end_time;
  doc["buzzer"] = config.buzzer_enabled;
  doc["led"] = config.led_enabled;
  
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAPISaveConfig() {
  if(server.hasArg("device_name")) strlcpy(config.device_name, server.arg("device_name").c_str(), sizeof(config.device_name));
  if(server.hasArg("sensor_name")) strlcpy(config.sensor_name, server.arg("sensor_name").c_str(), sizeof(config.sensor_name));
  if(server.hasArg("sensor_number")) strlcpy(config.sensor_number, server.arg("sensor_number").c_str(), sizeof(config.sensor_number));
  if(server.hasArg("api_endpoint")) strlcpy(config.api_endpoint, server.arg("api_endpoint").c_str(), sizeof(config.api_endpoint));
  if(server.hasArg("wifi_ssid") && server.arg("wifi_ssid").length()) strlcpy(config.wifi_ssid, server.arg("wifi_ssid").c_str(), sizeof(config.wifi_ssid));
  if(server.hasArg("wifi_password") && server.arg("wifi_password").length()) strlcpy(config.wifi_password, server.arg("wifi_password").c_str(), sizeof(config.wifi_password));
  if(server.hasArg("start_time")) strlcpy(config.start_time, server.arg("start_time").c_str(), sizeof(config.start_time));
  if(server.hasArg("end_time")) strlcpy(config.end_time, server.arg("end_time").c_str(), sizeof(config.end_time));
  
  config.buzzer_enabled = server.arg("buzzer") == "on" ? 1 : 0;
  config.led_enabled = server.arg("led") == "on" ? 1 : 0;
  
  saveConfig();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleAPITest() {
  if (!motion.active) {
    motion.active = 1;
    motion.active_start = millis();
    motion.last_trigger = millis();
    motion.total_events++;
    motion.passed_five_sec = 0;
    
    getTimeString(motion.start_time, sizeof(motion.start_time));
    motion.end_time[0] = 0;
    
    Serial.printf("\n🔴 #%u TEST START at %s\n", motion.total_events, motion.start_time);
    
    ledPatternMotionDetect();
    
    if (!sendEvent("S", 0, motion.start_time)) {
      Serial.println(F("⚠ TEST START send failed"));
    }
    
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Test triggered\"}");
    
    static Ticker testTimer;
    testTimer.once(2, []() {
      if (motion.active) {
        uint32_t duration = (millis() - motion.active_start) / 1000;
        getTimeString(motion.end_time, sizeof(motion.end_time));
        
        Serial.printf("\n🟢 #%u TEST END at %s (duration: %us)\n", 
                      motion.total_events, motion.end_time, duration);
        
        sendEvent("E", duration, motion.end_time);
        
        motion.active = 0;
        setLEDs(0, 1, 0);
      }
    });
  } else {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Motion active\"}");
  }
}

void handleAPIRestart() {
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Restarting\"}");
  delay(500);
  ESP.restart();
}

void handleAPIReset() {
  motion.total_events = 0;
  status.successful_tx = 0;
  status.failed_tx = 0;
  motion.event_id[0] = 0;
  status.last_error[0] = 0;
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Reset\"}");
}

void handleSaveWiFi() {
  if (server.hasArg("ssid")) {
    strlcpy(config.wifi_ssid, server.arg("ssid").c_str(), sizeof(config.wifi_ssid));
    if (server.hasArg("password")) strlcpy(config.wifi_password, server.arg("password").c_str(), sizeof(config.wifi_password));
    saveConfig();
    server.send(200, "text/html", 
      "<html><body style='background:#0a0f1e; color:cyan; font-family:Orbitron; text-align:center; padding:50px;'>"
      "<h1>✅ WIFI SAVED!</h1><p>RESTARTING...</p>"
      "<script>setTimeout(()=>window.location.href='/',3000);</script></body></html>");
    delay(1000);
    ESP.restart();
  } else server.send(400, "text/plain", "Missing SSID");
}

void handleNotFound() {
  server.send(404, "text/plain", "404 - NOT FOUND");
}

// ============= SETUP WEBSERVER =============
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/style.css", handleCSS);
  server.on("/script.js", handleJS);
  server.on("/api/status", handleAPIStatus);
  server.on("/api/last-event", handleAPILastEvent);
  server.on("/api/config", HTTP_GET, handleAPIGetConfig);
  server.on("/api/config", HTTP_POST, handleAPISaveConfig);
  server.on("/api/test", HTTP_POST, handleAPITest);
  server.on("/api/reset", HTTP_POST, handleAPIReset);
  server.on("/api/restart", HTTP_POST, handleAPIRestart);
  server.on("/save-wifi", HTTP_POST, handleSaveWiFi);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println(F("✓ Web server ready"));
}

// ============= SETUP =============
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial.println(F("\n⚡ NEURAL MOTION CORE v8.0.0"));
  Serial.println(F("────────────────────────────"));
  
  pinMode(PIN_RED_LED, OUTPUT);
  pinMode(PIN_GREEN_LED, OUTPUT);
  pinMode(PIN_BLUE_LED, OUTPUT);
  pinMode(PIN_PIR_SENSOR, INPUT);
  
  // Initialize all LEDs OFF
  setLEDs(0, 0, 0);
  
  printSystemInfo();
  testIndicators();
  loadConfig();
  connectWiFi();
  setupWebServer();
  
  Serial.printf("📡 http://%s\n\n", WiFi.localIP().toString().c_str());
}

// ============= LOOP =============
void loop() {
  server.handleClient();
  
  static uint32_t lastWifi = 0, lastTime = 0;
  uint32_t now = millis();
  
  if (now - lastWifi > 5000) {
    status.wifi_connected = (WiFi.status() == WL_CONNECTED);
    if (status.wifi_connected) strcpy(status.current_ssid, WiFi.SSID().c_str());
    lastWifi = now;
  }
  
  if (now - lastTime > 1000) {
    getTimeString(status.current_time, sizeof(status.current_time));
    lastTime = now;
  }
  
  checkMotion();
  
  delay(5);
}
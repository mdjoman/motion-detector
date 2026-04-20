/*
 * ESP8266 Motion Security System
 * Version: 6.7.0 - COMPLETE FIXED VERSION
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <time.h>

// ============= CONFIGURATION =============
#define FIRMWARE_VERSION "6.7.0"

// Pin Definitions
#define PIN_RED_LED     0   // GPIO0 - D3 (Active LOW)
#define PIN_GREEN_LED   14  // GPIO14 - D5
#define PIN_BLUE_LED    15  // GPIO15 - D8
#define PIN_PIR_SENSOR  12  // GPIO12 - D6
#define PIN_BUZZER      13  // GPIO13 - D7

// System Constants
#define EEPROM_SIZE      512
#define MOTION_TIMEOUT   5000      // 5 seconds no motion = end
#define WARMUP_TIME      15000     // 15 seconds sensor warmup
#define MOTION_CHECK_MS  200       // Check every 200ms
#define NTP_TIMEOUT      5000

// ============= GLOBAL OBJECTS =============
ESP8266WebServer server(80);
WiFiManager wifiManager;

// ============= DATA STRUCTURES =============
struct SystemConfig {
  char device_name[32];
  char sensor_name[32];
  char sensor_number[8];
  char api_endpoint[128];
  char wifi_ssid[32];
  char wifi_password[64];
  char start_time[8];
  char end_time[8];
  uint8_t buzzer_enabled;
  uint8_t led_enabled;
  uint32_t magic_number;
} config;

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
  uint32_t free_heap;
  uint32_t min_free_heap;
} status = {0};

// Cached values
static char deviceId[13] = "";
static char apiHost[64] = "";
static char apiPath[64] = "/api/motion-event";
static bool apiParsed = false;

// ============= FUNCTION PROTOTYPES =============
void loadConfig();
void saveConfig();
void connectWiFi();
void configModeCallback(WiFiManager* mgr);
bool syncNTP();
void saveWiFiCredentials();
void getTimeString(char* buffer, size_t len);
void checkMotion();
bool sendEvent(const char* type, uint32_t duration, const char* start, const char* end);
bool inTimeRange();
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
void handleCSS();
void handleJS();
void setError(const char* error);
void printSystemInfo();
void getDeviceIdString();
void parseApiEndpoint();
void testIndicators();
void testMotionSensor();

// ============= EEPROM MANAGEMENT =============
void saveConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();
  Serial.println(F("✓ Config saved"));
  apiParsed = false;  // Invalidate cache
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);
  
  if (config.magic_number != 0xABCD1234) {
    strcpy(config.device_name, "Motion_Sensor");
    strcpy(config.sensor_name, "PIR_Sensor");
    strcpy(config.sensor_number, "1");
    strcpy(config.api_endpoint, "https://zms.zisprink.com/api/motion-event");
    strcpy(config.wifi_ssid, "");
    strcpy(config.wifi_password, "");
    strcpy(config.start_time, "00:00");
    strcpy(config.end_time, "23:59");
    config.buzzer_enabled = 1;
    config.led_enabled = 1;
    config.magic_number = 0xABCD1234;
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
void getDeviceIdString() {
  if (deviceId[0] == 0) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    sprintf(deviceId, "%02X%02X%02X%02X%02X%02X", 
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("📱 Device ID: %s\n", deviceId);
  }
}

void parseApiEndpoint() {
  if (apiParsed) return;
  
  const char* url = config.api_endpoint;
  const char* base = url;
  
  if (strncmp(base, "https://", 8) == 0) base += 8;
  else if (strncmp(base, "http://", 7) == 0) base += 7;
  
  const char* pathStart = strchr(base, '/');
  if (pathStart) {
    int hostLen = pathStart - base;
    if (hostLen < (int)sizeof(apiHost)) {
      strncpy(apiHost, base, hostLen);
      apiHost[hostLen] = 0;
      strncpy(apiPath, pathStart, sizeof(apiPath) - 1);
    }
  } else {
    strncpy(apiHost, base, sizeof(apiHost) - 1);
  }
  
  apiParsed = true;
  Serial.printf("🌐 API Host: %s\n", apiHost);
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
  if (!status.ntp_synced) return true;
  
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  
  int cur = tm_info->tm_hour * 60 + tm_info->tm_min;
  int sh, sm, eh, em;
  sscanf(config.start_time, "%d:%d", &sh, &sm);
  sscanf(config.end_time, "%d:%d", &eh, &em);
  
  int start = sh * 60 + sm;
  int end = eh * 60 + em;
  
  return (start <= end) ? (cur >= start && cur <= end) : (cur >= start || cur <= end);
}

void setError(const char* error) {
  strncpy(status.last_error, error, sizeof(status.last_error) - 1);
  status.last_error[sizeof(status.last_error) - 1] = '\0';
  Serial.printf("❌ %s\n", error);
}

void printSystemInfo() {
  getDeviceIdString();
  Serial.printf("📊 v%s | Free Heap: %u bytes\n", FIRMWARE_VERSION, ESP.getFreeHeap());
}

void testIndicators() {
  Serial.println(F("🔧 Testing LEDs & Buzzer..."));
  digitalWrite(PIN_RED_LED, LOW); delay(200); digitalWrite(PIN_RED_LED, HIGH);
  digitalWrite(PIN_GREEN_LED, HIGH); delay(200); digitalWrite(PIN_GREEN_LED, LOW);
  digitalWrite(PIN_BLUE_LED, HIGH); delay(200); digitalWrite(PIN_BLUE_LED, LOW);
  digitalWrite(PIN_BUZZER, HIGH); delay(100); digitalWrite(PIN_BUZZER, LOW);
  Serial.println(F("✓ Test complete"));
}

void testMotionSensor() {
  Serial.println(F("\n🔍 Testing PIR Sensor..."));
  Serial.println(F("Wave your hand in front of the sensor for 10 seconds"));
  
  int highCount = 0;
  int lowCount = 0;
  
  for (int i = 0; i < 20; i++) {
    int val = digitalRead(PIN_PIR_SENSOR);
    if (val == HIGH) highCount++;
    else lowCount++;
    
    Serial.print(val == HIGH ? "1" : "0");
    if ((i + 1) % 10 == 0) Serial.println();
    
    delay(500);
  }
  
  Serial.printf("Results: HIGH=%d, LOW=%d - ", highCount, lowCount);
  if (highCount > 2) {
    Serial.println(F("✅ Sensor WORKING!"));
  } else {
    Serial.println(F("❌ Sensor NOT detecting! Check wiring to GPIO12 (D6)"));
  }
}

// ============= NTP SYNC =============
bool syncNTP() {
  Serial.print(F("⏰ Syncing time..."));
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
  Serial.println(F(" ✗ Failed"));
  status.ntp_synced = 0;
  return false;
}

// ============= WIFI MANAGEMENT =============
void configModeCallback(WiFiManager* mgr) {
  Serial.println(F("📱 AP Mode - Connect to 'Motion-Sensor-AP'"));
  digitalWrite(PIN_RED_LED, LOW);
}

void connectWiFi() {
  Serial.println(F("\n📡 Connecting to WiFi..."));
  
  if (config.wifi_ssid[0]) {
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
      digitalWrite(PIN_BLUE_LED, !digitalRead(PIN_BLUE_LED));
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n✓ Connected to %s\n", config.wifi_ssid);
      Serial.printf("  IP: %s\n", WiFi.localIP().toString().c_str());
      digitalWrite(PIN_GREEN_LED, HIGH);
      strcpy(status.current_ssid, WiFi.SSID().c_str());
      getDeviceIdString();
      syncNTP();
      return;
    }
    Serial.println(F("\n✗ Failed to connect"));
  }
  
  Serial.println(F("Starting WiFi Manager..."));
  wifiManager.setTimeout(60);
  wifiManager.setAPCallback(configModeCallback);
  
  if (!wifiManager.autoConnect("Motion-Sensor-AP")) {
    Serial.println(F("✗ WiFi Manager failed - restarting"));
    ESP.restart();
  }
  
  saveWiFiCredentials();
  digitalWrite(PIN_GREEN_LED, HIGH);
  strcpy(status.current_ssid, WiFi.SSID().c_str());
  Serial.printf("✓ Connected to %s\n", WiFi.SSID().c_str());
  Serial.printf("  IP: %s\n", WiFi.localIP().toString().c_str());
  getDeviceIdString();
  syncNTP();
}

// ============= SEND EVENT FUNCTION =============
bool sendEvent(const char* type, uint32_t duration, const char* start, const char* end) {
  
  digitalWrite(PIN_BLUE_LED, LOW);
  
  if (WiFi.status() != WL_CONNECTED) { 
    digitalWrite(PIN_BLUE_LED, HIGH);
    Serial.println(F("✗ WiFi not connected"));
    return false;
  }
  
  if (!inTimeRange()) { 
    digitalWrite(PIN_BLUE_LED, HIGH);
    Serial.println(F("⏰ Outside time range"));
    return false;
  }
  
  if (strlen(config.api_endpoint) < 10) { 
    digitalWrite(PIN_BLUE_LED, HIGH);
    Serial.println(F("✗ API endpoint not configured"));
    return false;
  }
  
  getDeviceIdString();
  parseApiEndpoint();
  
  char url[384];
  int urlLen = 0;
  
  if (type[0] == 'S') {
    urlLen = snprintf(url, sizeof(url),
      "%s?event_type=START&device_id=%s&device_name=%s&sensor_number=%s&sensor_name=%s&detected_at=%s",
      apiPath, deviceId, config.device_name, config.sensor_number, config.sensor_name, start);
    Serial.printf("📤 Sending START...\n");
  } 
  else {
    if (!motion.event_id[0]) {
      digitalWrite(PIN_BLUE_LED, HIGH);
      Serial.println(F("✗ No event_id for END"));
      return false;
    }
    urlLen = snprintf(url, sizeof(url),
      "%s?event_type=END&duration=%u&detected_at=%s&event_id=%s",
      apiPath, duration, end, motion.event_id);
    Serial.printf("📤 Sending END for event %s...\n", motion.event_id);
  }
  
  if (urlLen <= 0) {
    digitalWrite(PIN_BLUE_LED, HIGH);
    return false;
  }
  
  // URL encode spaces
  for (char* p = url; *p; p++) {
    if (*p == ' ') {
      memmove(p + 3, p + 1, strlen(p));
      *p++ = '%';
      *p++ = '2';
      *p = '0';
    }
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3000);
  
  if (!client.connect(apiHost, 443)) {
    status.failed_tx++;
    digitalWrite(PIN_RED_LED, LOW); delay(20); digitalWrite(PIN_RED_LED, HIGH);
    digitalWrite(PIN_BLUE_LED, HIGH);
    Serial.printf("✗ Connection to %s failed\n", apiHost);
    return false;
  }
  
  char request[512];
  int reqLen = snprintf(request, sizeof(request),
    "GET %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "X-Device-ID: %s\r\n"
    "Connection: close\r\n"
    "\r\n",
    url, apiHost, deviceId);
  
  client.write((uint8_t*)request, reqLen);
  
  uint32_t timeout = millis();
  while (!client.available() && millis() - timeout < 3000) {
    delay(1);
    yield();
  }
  
  bool success = false;
  
  if (client.available()) {
    char statusLine[16];
    int lineLen = client.readBytesUntil('\n', statusLine, sizeof(statusLine) - 1);
    statusLine[lineLen] = 0;
    
    if (strstr(statusLine, "200")) {
      success = true;
      status.successful_tx++;
      getTimeString(status.last_tx, sizeof(status.last_tx));
      Serial.println(F("✓ Server response: 200 OK"));
      
      if (type[0] == 'S') {
        // Skip headers
        while (client.available()) {
          char line[64];
          int llen = client.readBytesUntil('\n', line, sizeof(line) - 1);
          line[llen] = 0;
          if (llen <= 2 && (line[0] == '\r' || line[0] == '\n')) break;
        }
        
        // Read JSON body
        if (client.available()) {
          char body[128];
          int blen = client.readBytes(body, sizeof(body) - 1);
          body[blen] = 0;
          
          char* idStart = strstr(body, "\"event_id\":\"");
          if (idStart) {
            idStart += 11;
            char* idEnd = strchr(idStart, '"');
            if (idEnd) {
              int idLen = idEnd - idStart;
              if (idLen > 0 && idLen < 30) {
                strncpy(motion.event_id, idStart, idLen);
                motion.event_id[idLen] = 0;
                Serial.printf("✓ Event ID: %s\n", motion.event_id);
              }
            }
          }
        }
      }
      
      digitalWrite(PIN_GREEN_LED, LOW); delay(10); digitalWrite(PIN_GREEN_LED, HIGH);
    } else {
      status.failed_tx++;
      Serial.printf("✗ Server error: %s\n", statusLine);
      digitalWrite(PIN_RED_LED, LOW); delay(20); digitalWrite(PIN_RED_LED, HIGH);
    }
  } else {
    status.failed_tx++;
    Serial.println(F("✗ No response from server"));
    digitalWrite(PIN_RED_LED, LOW); delay(20); digitalWrite(PIN_RED_LED, HIGH);
  }
  
  client.stop();
  digitalWrite(PIN_BLUE_LED, HIGH);
  
  return success;
}

// ============= FIXED MOTION DETECTION =============
void checkMotion() {
  static int lastState = LOW;
  static uint32_t lastCheck = 0;
  static uint8_t warmedUp = 0;
  static uint32_t lastDebug = 0;
  
  uint32_t now = millis();
  
  // Rate limiting
  if (now - lastCheck < MOTION_CHECK_MS) return;
  lastCheck = now;
  
  // Warmup period
  if (!warmedUp) {
    if (now > WARMUP_TIME) {
      warmedUp = 1;
      Serial.println(F("\n✓ Motion sensor READY"));
      digitalWrite(PIN_GREEN_LED, HIGH);
    }
    
    // Blink during warmup
    if ((now / 500) % 2) {
      digitalWrite(PIN_BLUE_LED, HIGH);
    } else {
      digitalWrite(PIN_BLUE_LED, LOW);
    }
    return;
  }
  
  // Read PIR sensor
  int currentState = digitalRead(PIN_PIR_SENSOR);
  
  // Debug output every 10 seconds
  if (now - lastDebug > 10000) {
    lastDebug = now;
    Serial.printf("PIR: %d | Motion: %s | Events: %u | Heap: %u\n", 
                  currentState,
                  motion.active ? "ACTIVE" : "inactive",
                  motion.total_events,
                  ESP.getFreeHeap());
  }
  
  // ===== MOTION START DETECTED =====
  if (!motion.active && currentState == HIGH && lastState == LOW) {
    // Debounce
    delay(100);
    if (digitalRead(PIN_PIR_SENSOR) == HIGH) {
      motion.active = 1;
      motion.active_start = now;
      motion.last_trigger = now;
      motion.total_events++;
      
      getTimeString(motion.start_time, sizeof(motion.start_time));
      motion.end_time[0] = 0;
      
      Serial.printf("\n🔴 MOTION #%u START at %s\n", 
                    motion.total_events, motion.start_time);
      
      // Visual feedback
      digitalWrite(PIN_RED_LED, LOW);      // Red ON
      digitalWrite(PIN_BLUE_LED, HIGH);    // Blue ON
      delay(100);
      digitalWrite(PIN_BLUE_LED, LOW);
      
      if (config.buzzer_enabled) {
        digitalWrite(PIN_BUZZER, HIGH);
        delay(50);
        digitalWrite(PIN_BUZZER, LOW);
      }
      
      // Send to server
      sendEvent("S", 0, motion.start_time, "");
    }
  }
  
  // ===== MOTION ACTIVE =====
  else if (motion.active) {
    if (currentState == HIGH) {
      motion.last_trigger = now;
      // Heartbeat LED
      digitalWrite(PIN_BLUE_LED, (millis() % 500) < 50 ? HIGH : LOW);
    }
    
    // ===== MOTION END DETECTED =====
    if (now - motion.last_trigger > MOTION_TIMEOUT) {
      motion.active = 0;
      motion.current_duration = (now - motion.active_start) / 1000;
      if (motion.current_duration < 1) motion.current_duration = 1;
      
      getTimeString(motion.end_time, sizeof(motion.end_time));
      
      Serial.printf("🟢 MOTION #%u END after %us at %s\n", 
                    motion.total_events,
                    motion.current_duration,
                    motion.end_time);
      
      // Visual feedback
      digitalWrite(PIN_RED_LED, HIGH);      // Red OFF
      digitalWrite(PIN_GREEN_LED, LOW);
      digitalWrite(PIN_GREEN_LED, HIGH);    // Green blink
      delay(100);
      digitalWrite(PIN_GREEN_LED, LOW);
      
      if (config.buzzer_enabled) {
        digitalWrite(PIN_BUZZER, HIGH);
        delay(50);
        digitalWrite(PIN_BUZZER, LOW);
      }
      
      digitalWrite(PIN_BLUE_LED, LOW);
      
      // Send to server
      sendEvent("E", motion.current_duration, motion.start_time, motion.end_time);
    }
  }
  
  lastState = currentState;
}

// ============= WEB HANDLERS =============
void handleAPIStatus() {
  status.uptime = millis() / 1000;
  status.free_heap = ESP.getFreeHeap();
  if (status.free_heap < status.min_free_heap || status.min_free_heap == 0) {
    status.min_free_heap = status.free_heap;
  }
  getTimeString(status.current_time, sizeof(status.current_time));
  
  StaticJsonDocument<384> doc;
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
  doc["ip"] = WiFi.localIP().toString();
  doc["version"] = FIRMWARE_VERSION;
  doc["current_time"] = status.current_time;
  doc["free_heap"] = status.free_heap;
  doc["min_heap"] = status.min_free_heap;
  
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
    // Simulate motion
    motion.active = 1;
    motion.active_start = millis();
    motion.last_trigger = millis();
    motion.total_events++;
    
    getTimeString(motion.start_time, sizeof(motion.start_time));
    
    Serial.printf("\n🔴 TEST #%u START\n", motion.total_events);
    
    digitalWrite(PIN_RED_LED, LOW);
    digitalWrite(PIN_BLUE_LED, HIGH); delay(100); digitalWrite(PIN_BLUE_LED, LOW);
    digitalWrite(PIN_BUZZER, HIGH); delay(50); digitalWrite(PIN_BUZZER, LOW);
    
    sendEvent("S", 0, motion.start_time, "");
    
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Test triggered\"}");
    
    // Auto-end after 3 seconds
    static Ticker testTimer;
    testTimer.once(3, []() {
      if (motion.active) {
        uint32_t duration = (millis() - motion.active_start) / 1000;
        if (duration < 1) duration = 1;
        
        getTimeString(motion.end_time, sizeof(motion.end_time));
        
        Serial.printf("🟢 TEST #%u END (%us)\n", motion.total_events, duration);
        
        digitalWrite(PIN_RED_LED, HIGH);
        digitalWrite(PIN_GREEN_LED, HIGH); delay(100); digitalWrite(PIN_GREEN_LED, LOW);
        digitalWrite(PIN_BUZZER, HIGH); delay(50); digitalWrite(PIN_BUZZER, LOW);
        
        sendEvent("E", duration, motion.start_time, motion.end_time);
        
        motion.active = 0;
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
      "<html><body><h2>✅ WiFi Saved!</h2><p>Restarting...</p>"
      "<script>setTimeout(()=>window.location.href='/',3000);</script></body></html>");
    delay(1000);
    ESP.restart();
  } else server.send(400, "text/plain", "Missing SSID");
}

void handleCSS() {
  server.send(200, "text/css", R"rawliteral(
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:sans-serif;background:linear-gradient(135deg,#667eea,#764ba2);padding:12px;color:#333;min-height:100vh}
.container{max-width:800px;margin:0 auto}
.card{background:white;border-radius:15px;padding:15px;margin-bottom:15px;box-shadow:0 10px 30px rgba(0,0,0,0.2)}
.header{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
.header h1{font-size:24px;background:linear-gradient(135deg,#667eea,#764ba2);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.datetime{font-size:14px;color:#666;background:#f0f0f0;padding:5px 10px;border-radius:20px}
.badge{padding:4px 8px;border-radius:20px;font-size:12px;font-weight:500}
.badge-success{background:#d4edda;color:#155724}
.badge-danger{background:#f8d7da;color:#721c24}
.badge-info{background:#cce5ff;color:#004085}
.stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin:10px 0}
.stat-card{background:#f8f9fa;border-radius:12px;padding:15px;text-align:center}
.stat-label{font-size:12px;color:#666;text-transform:uppercase}
.stat-value{font-size:24px;font-weight:700;color:#333;margin-top:5px}
.motion-active{color:#dc3545;animation:pulse 1.5s infinite}
@keyframes pulse{0%{opacity:1}50%{opacity:0.5}100%{opacity:1}}
.event-card{background:#f8f9fa;border-radius:10px;padding:10px}
.event-row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #e0e0e0;font-size:14px}
.wifi-section{background:#f8f9fa;border-radius:10px;padding:12px;display:flex;justify-content:space-between}
.form-row{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:10px}
.form-group{margin-bottom:10px}
label{display:block;font-size:13px;margin-bottom:5px}
input{width:100%;padding:10px;border:2px solid #e0e0e0;border-radius:8px}
.button-group{display:flex;gap:10px;margin-top:15px}
.btn{padding:10px 16px;border:none;border-radius:8px;cursor:pointer;flex:1}
.btn-primary{background:linear-gradient(135deg,#667eea,#764ba2);color:white}
.btn-secondary{background:#6c757d;color:white}
.btn-info{background:#17a2b8;color:white}
.btn-warning{background:#dc3545;color:white}
.message{position:fixed;top:20px;right:20px;padding:15px;border-radius:8px;display:none}
.message.show{display:block}
.message.success{background:#d4edda;color:#155724}
.message.error{background:#f8d7da;color:#721c24}
@media(max-width:600px){.form-row{grid-template-columns:1fr}.button-group{flex-direction:column}}
  )rawliteral");
}

void handleJS() {
  server.send(200, "application/javascript", R"rawliteral(
let si; document.addEventListener('DOMContentLoaded',function(){
  uS(); uL(); lC(); si=setInterval(uS,2000); setInterval(uL,3000);
});
function uS(){fetch('/api/status').then(r=>r.json()).then(d=>{
  document.getElementById('motionStatus').innerHTML=d.motion?'ACTIVE':'INACTIVE';
  document.getElementById('motionStatus').className='stat-value'+(d.motion?' motion-active':'');
  document.getElementById('totalEvents').innerHTML=d.total_events;
  document.getElementById('pendingStatus').innerHTML='<span class="badge badge-success">None</span>';
  document.getElementById('wifiSSID').innerHTML=`<div><span class="wifi-ssid">${d.ssid||'Not connected'}</span><span class="wifi-ip"> ${d.ip}</span></div>`;
  document.getElementById('connectionStatus').innerHTML=d.wifi?'● Connected':'● Disconnected';
  document.getElementById('connectionStatus').className='badge '+(d.wifi?'badge-success':'badge-danger');
  document.getElementById('currentDateTime').innerHTML=d.current_time||'No time';
  document.getElementById('freeHeap').innerHTML=d.free_heap+' bytes';
  document.getElementById('minHeap').innerHTML=d.min_heap+' bytes';
});}
function uL(){fetch('/api/last-event').then(r=>r.json()).then(d=>{
  document.getElementById('eventNumber').innerHTML='#'+d.event_number;
  document.getElementById('eventId').innerHTML=d.event_id||'-';
  document.getElementById('eventStart').innerHTML=d.start_time;
  document.getElementById('eventEnd').innerHTML=d.end_time;
  document.getElementById('eventDuration').innerHTML=d.duration+'s';
  document.getElementById('eventStatus').innerHTML=d.active?'<span class="badge badge-danger">ACTIVE</span>':'<span class="badge badge-success">ENDED</span>';
});}
function lC(){fetch('/api/config').then(r=>r.json()).then(d=>{
  document.getElementById('deviceNameInput').value=d.device_name;
  document.getElementById('sensorNameInput').value=d.sensor_name;
  document.getElementById('sensorNumberInput').value=d.sensor_number;
  document.getElementById('apiEndpointInput').value=d.api_endpoint;
  document.getElementById('wifiSsidInput').value=d.wifi_ssid;
  document.getElementById('startTimeInput').value=d.start_time;
  document.getElementById('endTimeInput').value=d.end_time;
  document.getElementById('buzzerEnabled').checked=d.buzzer==1;
  document.getElementById('ledEnabled').checked=d.led==1;
});}
function saveConfig(){
  const fd=new FormData();
  fd.append('device_name',document.getElementById('deviceNameInput').value);
  fd.append('sensor_name',document.getElementById('sensorNameInput').value);
  fd.append('sensor_number',document.getElementById('sensorNumberInput').value);
  fd.append('api_endpoint',document.getElementById('apiEndpointInput').value);
  fd.append('wifi_ssid',document.getElementById('wifiSsidInput').value);
  const wp=document.getElementById('wifiPasswordInput').value;if(wp)fd.append('wifi_password',wp);
  fd.append('start_time',document.getElementById('startTimeInput').value);
  fd.append('end_time',document.getElementById('endTimeInput').value);
  fd.append('buzzer',document.getElementById('buzzerEnabled').checked?'on':'off');
  fd.append('led',document.getElementById('ledEnabled').checked?'on':'off');
  fetch('/api/config',{method:'POST',body:fd}).then(r=>r.json()).then(d=>{if(d.success){showMessage('✓ Saved','success');lC();}});
}
function saveWiFi(){
  const s=document.getElementById('wifiSsidInput').value,p=document.getElementById('wifiPasswordInput').value;
  if(!s){showMessage('Enter SSID','error');return;}
  const fd=new FormData();fd.append('ssid',s);fd.append('password',p);
  fetch('/save-wifi',{method:'POST',body:fd}).then(r=>r.text()).then(h=>{document.open();document.write(h);document.close();});
}
function testMotion(){fetch('/api/test',{method:'POST'}).then(r=>r.json()).then(d=>showMessage(d.success?'✓ Test':'⚠ '+d.message,d.success?'success':'error'));}
function resetStats(){if(confirm('Reset?')){fetch('/api/reset',{method:'POST'}).then(()=>showMessage('✓ Reset','success'));}}
function restartDevice(){if(confirm('Restart?')){fetch('/api/restart',{method:'POST'}).then(()=>showMessage('🔄 Restarting...','success'));}}
function showMessage(t,c){const m=document.getElementById('message');m.innerHTML=t;m.className='message '+c+' show';setTimeout(()=>m.classList.remove('show'),3000);}
  )rawliteral");
}

void handleRoot() {
  server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Motion Sensor</title><link rel="stylesheet" href="/style.css"></head>
<body>
<div class="container">
  <div class="card"><div class="header"><div><h1>⚡ Motion Sensor</h1><p>v6.7.0</p></div><div class="datetime" id="currentDateTime">-</div></div>
    <div style="display:flex;gap:10px;margin:5px 0"><span class="badge badge-info" id="deviceName">-</span><span id="connectionStatus" class="badge badge-success">● Connected</span></div>
    <div style="font-size:12px;color:#666">Free Heap: <span id="freeHeap">-</span> | Min: <span id="minHeap">-</span></div>
  </div>
  <div class="card"><div class="wifi-section">📶 <span id="wifiSSID">Loading...</span></div></div>
  <div class="card"><div class="stats-grid">
    <div class="stat-card"><div class="stat-label">Motion</div><div class="stat-value" id="motionStatus">-</div></div>
    <div class="stat-card"><div class="stat-label">Events</div><div class="stat-value" id="totalEvents">0</div></div>
    <div class="stat-card"><div class="stat-label">Pending</div><div class="stat-value" id="pendingStatus">None</div></div>
  </div></div>
  <div class="card"><h3>📅 Last Event</h3><div class="event-card">
    <div class="event-row"><span>Event #</span><span id="eventNumber">-</span></div>
    <div class="event-row"><span>Event ID</span><span id="eventId">-</span></div>
    <div class="event-row"><span>Start</span><span id="eventStart">-</span></div>
    <div class="event-row"><span>End</span><span id="eventEnd">-</span></div>
    <div class="event-row"><span>Duration</span><span id="eventDuration">-</span></div>
    <div class="event-row"><span>Status</span><span id="eventStatus">-</span></div>
  </div></div>
  <div class="card"><h3>⚙️ Config</h3>
    <form id="configForm">
      <div class="form-row"><div class="form-group"><label>Device</label><input id="deviceNameInput"></div><div class="form-group"><label>Sensor</label><input id="sensorNameInput"></div></div>
      <div class="form-row"><div class="form-group"><label>#</label><input id="sensorNumberInput" value="1"></div><div class="form-group"><label>API</label><input id="apiEndpointInput"></div></div>
      <div class="form-row"><div class="form-group"><label>SSID</label><input id="wifiSsidInput"></div><div class="form-group"><label>Pass</label><input type="password" id="wifiPasswordInput"></div></div>
      <div class="form-row"><div class="form-group"><label>Start</label><input type="time" id="startTimeInput" value="00:00"></div><div class="form-group"><label>End</label><input type="time" id="endTimeInput" value="23:59"></div></div>
      <div style="display:flex;gap:20px;margin:10px 0">
        <label><input type="checkbox" id="buzzerEnabled"> Buzzer</label>
        <label><input type="checkbox" id="ledEnabled" checked> LED</label>
      </div>
      <div class="button-group">
        <button class="btn btn-primary" type="button" onclick="saveConfig()">Save Config</button>
        <button class="btn btn-info" type="button" onclick="saveWiFi()">Save WiFi</button>
      </div>
      <div class="button-group">
        <button class="btn btn-secondary" type="button" onclick="testMotion()">Test Motion</button>
        <button class="btn btn-secondary" type="button" onclick="resetStats()">Reset Stats</button>
        <button class="btn btn-warning" type="button" onclick="restartDevice()">Restart</button>
      </div>
    </form>
  </div>
</div>
<div id="message" class="message"></div>
<script src="/script.js"></script>
</body>
</html>
  )rawliteral");
}

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
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  
  server.begin();
  Serial.println(F("✓ Web server ready"));
}

// ============= SETUP =============
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.flush();
  
  Serial.println(F("\n================================"));
  Serial.println(F("⚡ MOTION SENSOR v6.7.0"));
  Serial.println(F("================================"));
  
  pinMode(PIN_RED_LED, OUTPUT); digitalWrite(PIN_RED_LED, HIGH);
  pinMode(PIN_GREEN_LED, OUTPUT); digitalWrite(PIN_GREEN_LED, LOW);
  pinMode(PIN_BLUE_LED, OUTPUT); digitalWrite(PIN_BLUE_LED, LOW);
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_PIR_SENSOR, INPUT);
  
  testIndicators();
  loadConfig();
  connectWiFi();
  testMotionSensor();
  setupWebServer();
  
  Serial.println(F("================================"));
  Serial.printf("📡 http://%s\n", WiFi.localIP().toString().c_str());
  Serial.println(F("Ready for motion detection!\n"));
}

// ============= LOOP =============
void loop() {
  server.handleClient();
  
  static uint32_t lastWifi = 0, lastTime = 0, lastHeapCheck = 0;
  static uint32_t minHeap = 999999;
  
  uint32_t now = millis();
  
  if (now - lastWifi > 5000) {
    status.wifi_connected = (WiFi.status() == WL_CONNECTED);
    if (status.wifi_connected) {
      strcpy(status.current_ssid, WiFi.SSID().c_str());
    }
    lastWifi = now;
  }
  
  if (now - lastTime > 1000) {
    getTimeString(status.current_time, sizeof(status.current_time));
    lastTime = now;
  }
  
  // Heap monitoring every 5 minutes
  if (now - lastHeapCheck > 300000) {
    lastHeapCheck = now;
    uint32_t heap = ESP.getFreeHeap();
    if (heap < minHeap) {
      minHeap = heap;
      status.min_free_heap = minHeap;
    }
    Serial.printf("📊 Heap: %u bytes (min: %u)\n", heap, minHeap);
    
    // Auto-reboot if heap critically low
    if (heap < 5000) {
      Serial.println(F("🔥 CRITICAL HEAP - Rebooting in 3 seconds..."));
      for (int i = 0; i < 3; i++) {
        digitalWrite(PIN_RED_LED, LOW);
        delay(500);
        digitalWrite(PIN_RED_LED, HIGH);
        delay(500);
      }
      ESP.restart();
    }
  }
  
  checkMotion();
  
  delay(5);
}
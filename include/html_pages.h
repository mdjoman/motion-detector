#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <DNSServer.h>
#include <time.h>
#include "html_pages.h"

// ============= CONFIGURATION =============
#define FIRMWARE_VERSION "6.0.0"

// Pin Definitions
#define PIN_BLUE_LED    0   // GPIO0 - Built-in LED
#define PIN_GREEN_LED   5   // GPIO5 - D1
#define PIN_RED_LED     4   // GPIO4 - D2
#define PIN_PIR_SENSOR  12  // GPIO12 - D6
#define PIN_BUZZER      15  // GPIO15 - D8

// System Constants
#define EEPROM_SIZE      4096
#define MOTION_TIMEOUT   5000      // 5 seconds without motion ends event
#define WARMUP_TIME      10000     // 10 seconds sensor warmup
#define HTTP_TIMEOUT     10000
#define WIFI_RETRY_DELAY 30000
#define MOTION_CHECK_MS  50
#define RETRY_DELAY      30000
#define MAX_RETRIES      5
#define NTP_TIMEOUT      15000
#define QUEUE_MAX_SIZE   100
#define ADMIN_TIMEOUT_MS 1800000   // 30 minutes

// AP Mode Configuration
#define AP_IP "192.168.4.1"
#define AP_GATEWAY "192.168.4.1"
#define AP_SUBNET "255.255.255.0"

// File Paths
#define CONFIG_FILE     "/config.json"
#define QUEUE_FILE      "/queue.json"
#define EVENTS_FILE     "/events.json"

// ============= GLOBAL OBJECTS =============
ESP8266WebServer server(80);
WiFiManager wifiManager;
Ticker ledTimer;
Ticker buzzerTimer;
Ticker retryTimer;
DNSServer dnsServer;

// ============= DATA STRUCTURES =============
struct SystemConfig {
  char device_name[32] = "Motion Sensor";
  char sensor_name[32] = "PIR Sensor";
  char api_endpoint[256] = "";
  char wifi_ssid[32] = "";
  char wifi_password[64] = "";
  char admin_username[32] = "admin";
  char admin_password[32] = "admin123";
  char sensor_id[32] = "";
  uint32_t sync_interval = 30000;
  uint8_t start_hour = 0;
  uint8_t start_minute = 0;
  uint8_t end_hour = 23;
  uint8_t end_minute = 59;
  bool time_range_enabled = false;
  bool buzzer_enabled = true;
  uint32_t magic = 0xDEADBEEF;
} config;

struct EventData {
  String id;
  String type;
  String start_time;
  String end_time;
  uint32_t duration;
  uint32_t event_number;
  bool synced;
  bool queued;
  uint32_t timestamp;
};

struct QueueItem {
  String data;
  uint8_t retry_count;
  uint32_t next_retry;
  bool valid;
};

// Motion state
struct {
  bool active = false;
  uint32_t active_start = 0;
  uint32_t last_trigger = 0;
  uint32_t total_events = 0;
  uint32_t motion_count = 0;
  uint32_t current_duration = 0;
  char start_time[20] = "";
  char end_time[20] = "";
} motion;

// Queue storage
QueueItem eventQueue[QUEUE_MAX_SIZE];
uint8_t queueHead = 0;
uint8_t queueTail = 0;
uint8_t queueCount = 0;

// System status
struct {
  bool wifi_connected = false;
  bool ntp_synced = false;
  bool admin_logged_in = false;
  bool ap_mode = false;
  unsigned long admin_timeout = 0;
  unsigned long start_time = 0;
  unsigned long last_sync = 0;
  char last_tx[20] = "";
  char last_error[64] = "";
  int16_t rssi = 0;
  uint32_t free_heap = 0;
  uint32_t uptime = 0;
  uint32_t successful_tx = 0;
  uint32_t failed_tx = 0;
  uint32_t queued_events = 0;
  uint32_t retry_count = 0;
  char current_ssid[32] = "";
  char current_time[20] = "";
  char current_date[12] = "";
} status;

// ============= FUNCTION PROTOTYPES =============
void loadConfig();
bool saveConfig();
void loadQueue();
bool saveQueue();
void setupWiFi();
void startAPMode();
void checkWiFi();
bool syncNTP();
void updateTimeDisplay();

void checkMotion();
void startMotionEvent();
void endMotionEvent();
void getTimeString(char* buffer, size_t len);
void getISOString(char* buffer, size_t len);
String generateEventID();

bool sendEvent(const char* type, uint32_t duration, const char* start, const char* end);
void queueEvent(const String& data);
bool processQueue();
bool shouldSendToServer();
bool inTimeRange();

void setupWebServer();
void handleRoot();
void handleLogin();
void handleLogout();
void handleSetup();
void handleSaveSetup();
void handleAPIConfig();
void handleAPIGetConfig();
void handleAPIStatus();
void handleAPIEvents();
void handleAPISync();
void handleAPIClearQueue();
void handleAPIResetStats();
void handleAPIRestart();
void handleAPIFactoryReset();
void handleAPIDiagnostics();
void handleNotFound();

bool checkAuth();

void blinkLED(int pin, int duration);
void beepBuzzer(int duration);
void setError(const char* error);
void printSystemInfo();
String getTimestamp();
String getTimeString();
String getDeviceID();

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
  
  if (config.magic != 0xDEADBEEF) {
    Serial.println(F("📝 Loading default config"));
    strcpy(config.device_name, "Motion Sensor");
    strcpy(config.sensor_name, "PIR Sensor");
    strcpy(config.api_endpoint, "");
    strcpy(config.wifi_ssid, "");
    strcpy(config.wifi_password, "");
    strcpy(config.admin_username, "admin");
    strcpy(config.admin_password, "admin123");
    strcpy(config.sensor_id, "");
    config.sync_interval = 30000;
    config.start_hour = 0;
    config.start_minute = 0;
    config.end_hour = 23;
    config.end_minute = 59;
    config.time_range_enabled = false;
    config.buzzer_enabled = true;
    config.magic = 0xDEADBEEF;
    saveConfig();
  }
  
  EEPROM.end();
  Serial.println(F("✓ Config loaded"));
}

void loadQueue() {
  // In-memory queue only for now
  queueCount = 0;
  queueHead = 0;
  queueTail = 0;
  status.queued_events = 0;
}

bool saveQueue() {
  return true; // Not implemented for now
}

// ============= UTILITY FUNCTIONS =============
void safeDelay(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    delay(10);
    yield();
  }
}

void blinkLED(int pin, int duration) {
  digitalWrite(pin, (pin == PIN_BLUE_LED) ? LOW : HIGH);
  ledTimer.once_ms(duration, [pin]() {
    digitalWrite(pin, (pin == PIN_BLUE_LED) ? HIGH : LOW);
  });
}

void beepBuzzer(int duration) {
  if (!config.buzzer_enabled) return;
  digitalWrite(PIN_BUZZER, HIGH);
  buzzerTimer.once_ms(duration, []() {
    digitalWrite(PIN_BUZZER, LOW);
  });
}

void setError(const char* error) {
  strcpy(status.last_error, error);
  Serial.printf("❌ Error: %s\n", error);
}

String getDeviceID() {
  if (strlen(config.sensor_id) > 0) {
    return String(config.sensor_id);
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "ESP-%08X", ESP.getChipId());
  return String(buf);
}

String getTimestamp() {
  time_t now = time(nullptr);
  if (now < 1600000000) {
    unsigned long uptime = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "UPTIME-%08lu", uptime);
    return String(buf);
  }
  struct tm* tm_info = localtime(&now);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
  return String(buf);
}

String getTimeString() {
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  if (tm_info->tm_year > 100) {
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    return String(buf);
  }
  unsigned long uptime = millis() / 1000;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", 
           (uptime / 3600) % 24, (uptime / 60) % 60, uptime % 60);
  return String(buf);
}

void getTimeString(char* buffer, size_t len) {
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  if (tm_info->tm_year > 100) {
    strftime(buffer, len, "%H:%M:%S", tm_info);
  } else {
    unsigned long uptime = millis() / 1000;
    snprintf(buffer, len, "%02lu:%02lu:%02lu", 
             (uptime / 3600) % 24, (uptime / 60) % 60, uptime % 60);
  }
}

void getISOString(char* buffer, size_t len) {
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  if (tm_info->tm_year > 100) {
    strftime(buffer, len, "%Y-%m-%d %H:%M:%S", tm_info);
  } else {
    snprintf(buffer, len, "%lu", millis() / 1000);
  }
}

void updateTimeDisplay() {
  if (status.ntp_synced) {
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    strftime(status.current_time, sizeof(status.current_time), "%H:%M:%S", tm_info);
    strftime(status.current_date, sizeof(status.current_date), "%Y-%m-%d", tm_info);
  } else {
    unsigned long uptime = millis() / 1000;
    snprintf(status.current_time, sizeof(status.current_time), "%02lu:%02lu:%02lu",
             (uptime / 3600) % 24, (uptime / 60) % 60, uptime % 60);
    strcpy(status.current_date, "No Date");
  }
}

String generateEventID() {
  uint32_t chip = ESP.getChipId();
  uint32_t time = millis();
  uint32_t rand = os_random();
  char buf[64];
  snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%04x-%08lx%04x",
           (unsigned long)chip,
           (unsigned int)(time >> 16),
           (unsigned int)(time & 0xFFFF),
           (unsigned int)(rand >> 16),
           (unsigned long)chip,
           (unsigned int)(rand & 0xFFFF));
  return String(buf);
}

void printSystemInfo() {
  Serial.println(F("\n📊 System Info"));
  Serial.println(F("──────────────────"));
  Serial.printf("Version: %s\n", FIRMWARE_VERSION);
  Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Device ID: %s\n", getDeviceID().c_str());
  Serial.println(F("──────────────────\n"));
}

// ============= TIME FUNCTIONS =============
bool syncNTP() {
  Serial.print(F("⏰ Syncing NTP..."));
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  
  time_t now = time(nullptr);
  uint32_t start = millis();
  
  while (millis() - start < NTP_TIMEOUT) {
    delay(500);
    now = time(nullptr);
    if (now > 100000) {
      struct tm* tm_info = localtime(&now);
      char timeStr[30];
      strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", tm_info);
      Serial.printf(" ✓ %s\n", timeStr);
      status.ntp_synced = true;
      updateTimeDisplay();
      return true;
    }
    Serial.print(".");
  }
  
  Serial.println(F(" ✗ Timeout"));
  status.ntp_synced = false;
  setError("NTP sync failed");
  return false;
}

bool inTimeRange() {
  if (!config.time_range_enabled) return true;
  if (!status.ntp_synced) return true;
  
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  int current = tm_info->tm_hour * 60 + tm_info->tm_min;
  int start = config.start_hour * 60 + config.start_minute;
  int end = config.end_hour * 60 + config.end_minute;
  
  if (start <= end) {
    return (current >= start && current <= end);
  } else {
    return (current >= start || current <= end);
  }
}

bool shouldSendToServer() {
  if (strlen(config.api_endpoint) == 0) return false;
  if (!status.wifi_connected || status.ap_mode) return false;
  return inTimeRange();
}

// ============= WIFI MANAGEMENT =============
void startAPMode() {
  WiFi.mode(WIFI_AP);
  
  IPAddress ip, gw, subnet;
  ip.fromString(AP_IP);
  gw.fromString(AP_GATEWAY);
  subnet.fromString(AP_SUBNET);
  
  WiFi.softAPConfig(ip, gw, subnet);
  WiFi.softAP(config.device_name, "");
  
  status.ap_mode = true;
  
  Serial.println(F("\n=== AP Mode ==="));
  Serial.printf("SSID: %s\n", config.device_name);
  Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.println(F("===============\n"));
  
  dnsServer.start(53, "*", ip);
  
  for (int i = 0; i < 3; i++) {
    blinkLED(PIN_BLUE_LED, 200);
    delay(300);
  }
}

void checkWiFi() {
  static bool wasConnected = false;
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  
  if (isConnected) {
    if (!wasConnected) {
      digitalWrite(PIN_GREEN_LED, HIGH);
      Serial.printf("\n✓ WiFi connected: %s\n", WiFi.localIP().toString().c_str());
      strcpy(status.current_ssid, WiFi.SSID().c_str());
      blinkLED(PIN_BLUE_LED, 300);
      beepBuzzer(100);
      
      if (!status.ntp_synced) {
        syncNTP();
      }
    }
    status.wifi_connected = true;
    status.rssi = WiFi.RSSI();
  } else {
    if (wasConnected) {
      digitalWrite(PIN_GREEN_LED, LOW);
      Serial.println(F("\n⚠ WiFi disconnected"));
      status.ntp_synced = false;
    }
    status.wifi_connected = false;
  }
  wasConnected = isConnected;
}

void setupWiFi() {
  Serial.println(F("\n📡 Setting up WiFi..."));
  
  if (strlen(config.wifi_ssid) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    
    Serial.printf("Connecting to %s...", config.wifi_ssid);
    
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
      delay(500);
      Serial.print(".");
      blinkLED(PIN_BLUE_LED, 50);
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
      status.ap_mode = false;
      digitalWrite(PIN_GREEN_LED, HIGH);
      strcpy(status.current_ssid, WiFi.SSID().c_str());
      Serial.printf("✓ Connected! IP: %s\n", WiFi.localIP().toString().c_str());
      syncNTP();
      return;
    }
  }
  
  startAPMode();
}

// ============= MOTION DETECTION =============
void startMotionEvent() {
  motion.active = true;
  motion.active_start = millis();
  motion.last_trigger = millis();
  motion.total_events++;
  motion.motion_count++;
  
  getTimeString(motion.start_time, sizeof(motion.start_time));
  motion.end_time[0] = '\0';
  
  Serial.printf("\n🔴 MOTION #%u START at %s\n", 
                motion.total_events, motion.start_time);
  
  digitalWrite(PIN_RED_LED, HIGH);
  beepBuzzer(200);
  
  String json = createEventJSON("START", motion.start_time, 0, "");
  
  if (shouldSendToServer()) {
    if (sendEvent("START", 0, motion.start_time, "")) {
      status.successful_tx++;
      blinkLED(PIN_BLUE_LED, 100);
    } else {
      queueEvent(json);
    }
  } else {
    queueEvent(json);
  }
}

void endMotionEvent() {
  motion.active = false;
  motion.current_duration = (millis() - motion.active_start) / 1000;
  
  getTimeString(motion.end_time, sizeof(motion.end_time));
  
  Serial.printf("🟢 MOTION #%u END at %s (Duration: %us)\n", 
                motion.total_events, motion.end_time, motion.current_duration);
  
  digitalWrite(PIN_RED_LED, LOW);
  
  String json = createEventJSON("END", motion.start_time, motion.current_duration, motion.end_time);
  
  if (shouldSendToServer()) {
    if (sendEvent("END", motion.current_duration, motion.start_time, motion.end_time)) {
      status.successful_tx++;
      blinkLED(PIN_BLUE_LED, 100);
    } else {
      queueEvent(json);
    }
  } else {
    queueEvent(json);
  }
}

void checkMotion() {
  static int lastState = LOW;
  static uint32_t lastCheck = 0;
  static uint32_t lastDebounce = 0;
  static bool warmedUp = false;
  
  uint32_t now = millis();
  
  if (now - lastCheck < MOTION_CHECK_MS) return;
  lastCheck = now;
  
  if (!warmedUp) {
    if (now > WARMUP_TIME) {
      warmedUp = true;
      Serial.println(F("✓ Sensor warmup complete"));
    } else {
      return;
    }
  }
  
  int reading = digitalRead(PIN_PIR_SENSOR);
  
  if (reading != lastState) {
    lastDebounce = now;
  }
  
  if ((now - lastDebounce) > 50) {
    if (reading != lastState) {
      lastState = reading;
      
      if (!motion.active && reading == HIGH) {
        startMotionEvent();
      }
    }
  }
  
  if (motion.active && (now - motion.last_trigger > MOTION_TIMEOUT)) {
    endMotionEvent();
  }
}

String createEventJSON(const char* type, const char* start, uint32_t duration, const char* end) {
  StaticJsonDocument<512> doc;
  doc["event_id"] = generateEventID();
  doc["device_id"] = getDeviceID();
  doc["device_name"] = config.device_name;
  doc["sensor_name"] = config.sensor_name;
  doc["timestamp"] = getTimestamp();
  doc["type"] = type;
  doc["event_number"] = motion.total_events;
  doc["start_time"] = start;
  
  if (strcmp(type, "END") == 0) {
    doc["end_time"] = end;
    doc["duration"] = duration;
  }
  
  doc["rssi"] = status.rssi;
  doc["uptime"] = millis() / 1000;
  doc["firmware"] = FIRMWARE_VERSION;
  
  String json;
  serializeJson(doc, json);
  return json;
}

// ============= EVENT HANDLING =============
bool sendEvent(const char* type, uint32_t duration, const char* start, const char* end) {
  if (WiFi.status() != WL_CONNECTED || status.ap_mode) {
    return false;
  }
  
  if (strlen(config.api_endpoint) == 0) {
    return false;
  }
  
  WiFiClient client;
  HTTPClient http;
  
  client.setTimeout(HTTP_TIMEOUT);
  client.setNoDelay(true);
  
  StaticJsonDocument<512> doc;
  
  char mac[18];
  strcpy(mac, WiFi.macAddress().c_str());
  
  char timestamp[25];
  getISOString(timestamp, sizeof(timestamp));
  
  doc["device_id"] = mac;
  doc["device_name"] = config.device_name;
  doc["sensor_name"] = config.sensor_name;
  doc["timestamp"] = timestamp;
  doc["event_type"] = type;
  doc["event_number"] = motion.total_events;
  
  if (strcmp(type, "START") == 0) {
    doc["status"] = "ACTIVE";
    doc["start_time"] = start;
  } else {
    doc["status"] = "ENDED";
    doc["duration"] = duration;
    doc["start_time"] = start;
    doc["end_time"] = end;
  }
  
  doc["signal"] = status.rssi;
  doc["uptime"] = millis() / 1000;
  doc["firmware"] = FIRMWARE_VERSION;
  
  String payload;
  serializeJson(doc, payload);
  
  http.setTimeout(HTTP_TIMEOUT);
  http.setReuse(false);
  
  if (!http.begin(client, config.api_endpoint)) {
    http.end();
    return false;
  }
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  http.addHeader("X-Device-ID", getDeviceID());
  
  int httpCode = http.POST(payload);
  String response = http.getString();
  
  bool success = false;
  
  if (httpCode > 0) {
    if (httpCode >= 200 && httpCode < 300) {
      char timeStr[16];
      getTimeString(timeStr, sizeof(timeStr));
      strcpy(status.last_tx, timeStr);
      
      Serial.printf("✓ %s sent (HTTP %d)\n", type, httpCode);
      success = true;
      
      status.last_error[0] = '\0';
    } else {
      Serial.printf("✗ %s failed (HTTP %d): %s\n", type, httpCode, response.c_str());
      status.failed_tx++;
    }
  } else {
    Serial.printf("✗ %s error: %d\n", type, httpCode);
    status.failed_tx++;
  }
  
  http.end();
  client.stop();
  
  return success;
}

void queueEvent(const String& data) {
  if (queueCount >= QUEUE_MAX_SIZE) {
    Serial.println(F("[QUEUE] Full, dropping oldest"));
    queueHead = (queueHead + 1) % QUEUE_MAX_SIZE;
    queueCount--;
  }
  
  eventQueue[queueTail].data = data;
  eventQueue[queueTail].retry_count = 0;
  eventQueue[queueTail].next_retry = 0;
  eventQueue[queueTail].valid = true;
  
  queueTail = (queueTail + 1) % QUEUE_MAX_SIZE;
  queueCount++;
  status.queued_events = queueCount;
  status.retry_count++;
  
  Serial.printf("[QUEUE] Event queued (%d)\n", queueCount);
}

bool processQueue() {
  if (queueCount == 0) return true;
  if (WiFi.status() != WL_CONNECTED || status.ap_mode) return false;
  if (!inTimeRange()) return false;
  
  uint32_t now = millis() / 1000;
  uint8_t processed = 0;
  uint8_t idx = queueHead;
  bool success = false;
  
  while (processed < queueCount) {
    if (eventQueue[idx].valid && eventQueue[idx].next_retry <= now) {
      // Parse the JSON to get event details for sending
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, eventQueue[idx].data);
      
      bool sendSuccess = false;
      if (!err) {
        const char* type = doc["type"];
        const char* start = doc["start_time"];
        uint32_t duration = doc["duration"] | 0;
        const char* end = doc["end_time"] | "";
        
        if (strcmp(type, "START") == 0) {
          sendSuccess = sendEvent("START", 0, start, "");
        } else {
          sendSuccess = sendEvent("END", duration, start, end);
        }
      }
      
      if (sendSuccess) {
        eventQueue[idx].valid = false;
        queueHead = (queueHead + 1) % QUEUE_MAX_SIZE;
        queueCount--;
        status.queued_events = queueCount;
        status.successful_tx++;
        success = true;
        blinkLED(PIN_BLUE_LED, 50);
      } else {
        eventQueue[idx].retry_count++;
        uint32_t delay = 60;
        if (eventQueue[idx].retry_count <= 6) {
          delay = 60 * (1 << (eventQueue[idx].retry_count - 1));
        } else {
          delay = 3600;
        }
        eventQueue[idx].next_retry = now + delay;
        idx = (idx + 1) % QUEUE_MAX_SIZE;
      }
    } else {
      idx = (idx + 1) % QUEUE_MAX_SIZE;
    }
    processed++;
  }
  
  return success;
}

// ============= AUTHENTICATION =============
bool checkAuth() {
  if (!status.admin_logged_in || millis() > status.admin_timeout) {
    status.admin_logged_in = false;
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
    return false;
  }
  status.admin_timeout = millis() + ADMIN_TIMEOUT_MS;
  return true;
}

// ============= WEB SERVER HANDLERS =============
void handleRoot() {
  if (status.ap_mode) {
    server.send(200, "text/html", setup_html);
  } else if (status.admin_logged_in) {
    server.sendHeader("Location", "/admin");
    server.send(302, "text/plain", "");
  } else {
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
  }
}

void handleLogin() {
  if (server.method() == HTTP_GET) {
    server.send(200, "text/html", login_html);
    return;
  }
  
  if (server.hasArg("username") && server.hasArg("password")) {
    String username = server.arg("username");
    String password = server.arg("password");
    
    if (username == config.admin_username && password == config.admin_password) {
      status.admin_logged_in = true;
      status.admin_timeout = millis() + ADMIN_TIMEOUT_MS;
      server.sendHeader("Location", "/admin");
      server.send(302, "text/plain", "");
      return;
    }
  }
  
  server.sendHeader("Location", "/login?error=1");
  server.send(302, "text/plain", "");
}

void handleLogout() {
  status.admin_logged_in = false;
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSetup() {
  server.send(200, "text/html", setup_html);
}

void handleSaveSetup() {
  if (server.hasArg("ssid")) {
    strlcpy(config.wifi_ssid, server.arg("ssid").c_str(), sizeof(config.wifi_ssid));
    
    if (server.hasArg("password")) {
      strlcpy(config.wifi_password, server.arg("password").c_str(), sizeof(config.wifi_password));
    }
    
    saveConfig();
    
    server.send(200, "text/plain", "WiFi settings saved. Device will restart.");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing SSID");
  }
}

void handleAPIGetConfig() {
  if (!checkAuth()) return;
  
  StaticJsonDocument<1024> doc;
  doc["device_name"] = config.device_name;
  doc["sensor_name"] = config.sensor_name;
  doc["sensor_id"] = config.sensor_id;
  doc["api_url"] = config.api_endpoint;
  doc["wifi_ssid"] = config.wifi_ssid;
  doc["start_hour"] = config.start_hour;
  doc["start_minute"] = config.start_minute;
  doc["end_hour"] = config.end_hour;
  doc["end_minute"] = config.end_minute;
  doc["time_range_enabled"] = config.time_range_enabled;
  doc["admin_username"] = config.admin_username;
  doc["admin_password"] = "********";
  doc["sync_interval"] = config.sync_interval;
  doc["buzzer_enabled"] = config.buzzer_enabled;
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleAPIConfig() {
  if (!checkAuth()) return;
  
  bool changed = false;
  
  if (server.hasArg("device_name")) {
    strlcpy(config.device_name, server.arg("device_name").c_str(), sizeof(config.device_name));
    changed = true;
  }
  
  if (server.hasArg("sensor_name")) {
    strlcpy(config.sensor_name, server.arg("sensor_name").c_str(), sizeof(config.sensor_name));
    changed = true;
  }
  
  if (server.hasArg("sensor_id")) {
    strlcpy(config.sensor_id, server.arg("sensor_id").c_str(), sizeof(config.sensor_id));
    changed = true;
  }
  
  if (server.hasArg("api_url")) {
    strlcpy(config.api_endpoint, server.arg("api_url").c_str(), sizeof(config.api_endpoint));
    changed = true;
  }
  
  if (server.hasArg("wifi_ssid")) {
    strlcpy(config.wifi_ssid, server.arg("wifi_ssid").c_str(), sizeof(config.wifi_ssid));
    changed = true;
  }
  
  if (server.hasArg("wifi_password") && server.arg("wifi_password").length() > 0) {
    strlcpy(config.wifi_password, server.arg("wifi_password").c_str(), sizeof(config.wifi_password));
    changed = true;
  }
  
  if (server.hasArg("start_hour")) {
    config.start_hour = server.arg("start_hour").toInt();
    changed = true;
  }
  
  if (server.hasArg("start_minute")) {
    config.start_minute = server.arg("start_minute").toInt();
    changed = true;
  }
  
  if (server.hasArg("end_hour")) {
    config.end_hour = server.arg("end_hour").toInt();
    changed = true;
  }
  
  if (server.hasArg("end_minute")) {
    config.end_minute = server.arg("end_minute").toInt();
    changed = true;
  }
  
  if (server.hasArg("time_range_enabled")) {
    config.time_range_enabled = (server.arg("time_range_enabled") == "true");
    changed = true;
  }
  
  if (server.hasArg("admin_username")) {
    strlcpy(config.admin_username, server.arg("admin_username").c_str(), sizeof(config.admin_username));
    changed = true;
  }
  
  if (server.hasArg("admin_password") && server.arg("admin_password").length() > 0) {
    strlcpy(config.admin_password, server.arg("admin_password").c_str(), sizeof(config.admin_password));
    changed = true;
  }
  
  if (server.hasArg("sync_interval")) {
    config.sync_interval = server.arg("sync_interval").toInt();
    changed = true;
  }
  
  if (server.hasArg("buzzer_enabled")) {
    config.buzzer_enabled = (server.arg("buzzer_enabled") == "true");
    changed = true;
  }
  
  if (changed) {
    saveConfig();
    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Configuration saved\"}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No changes\"}");
  }
}

void handleAPIStatus() {
  if (!checkAuth()) return;
  
  status.uptime = (millis() - status.start_time) / 1000;
  status.free_heap = ESP.getFreeHeap();
  updateTimeDisplay();
  
  FSInfo fs;
  LittleFS.info(fs);
  
  StaticJsonDocument<1024> doc;
  
  doc["motion_active"] = motion.active;
  doc["motion_count"] = motion.motion_count;
  doc["current_duration"] = motion.active ? (millis() - motion.active_start) / 1000 : motion.current_duration;
  doc["total_events"] = motion.total_events;
  doc["synced_events"] = status.successful_tx;
  doc["failed_events"] = status.failed_tx;
  doc["queued_events"] = status.queued_events;
  doc["last_tx"] = status.last_tx;
  
  doc["wifi_connected"] = status.wifi_connected;
  doc["ap_mode"] = status.ap_mode;
  doc["rssi"] = status.rssi;
  doc["ip"] = status.ap_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  doc["ssid"] = status.ap_mode ? config.device_name : WiFi.SSID();
  
  doc["uptime"] = status.uptime;
  doc["free_heap"] = status.free_heap;
  doc["free_heap_kb"] = status.free_heap / 1024;
  doc["fs_used"] = fs.usedBytes;
  doc["fs_total"] = fs.totalBytes;
  doc["fs_used_kb"] = fs.usedBytes / 1024;
  doc["fs_total_kb"] = fs.totalBytes / 1024;
  
  doc["start_hour"] = config.start_hour;
  doc["start_minute"] = config.start_minute;
  doc["end_hour"] = config.end_hour;
  doc["end_minute"] = config.end_minute;
  doc["in_time_range"] = inTimeRange();
  doc["current_time"] = String(status.current_date) + " " + String(status.current_time);
  
  doc["sensor_id"] = getDeviceID();
  doc["error_msg"] = status.last_error;
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleAPIEvents() {
  if (!checkAuth()) return;
  
  StaticJsonDocument<4096> doc;
  JsonArray events = doc.to<JsonArray>();
  
  uint8_t idx = queueHead;
  for (uint8_t i = 0; i < queueCount; i++) {
    if (eventQueue[idx].valid) {
      StaticJsonDocument<512> eventDoc;
      DeserializationError err = deserializeJson(eventDoc, eventQueue[idx].data);
      if (!err) {
        eventDoc["queued"] = true;
        eventDoc["retry_count"] = eventQueue[idx].retry_count;
        events.add(eventDoc);
      }
    }
    idx = (idx + 1) % QUEUE_MAX_SIZE;
  }
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleAPISync() {
  if (!checkAuth()) return;
  
  int sent = 0;
  if (status.wifi_connected && !status.ap_mode && inTimeRange()) {
    sent = processQueue() ? queueCount : 0;
  }
  
  StaticJsonDocument<128> res;
  res["status"] = "success";
  res["sent"] = sent;
  res["remaining"] = status.queued_events;
  
  String json;
  serializeJson(res, json);
  server.send(200, "application/json", json);
}

void handleAPIClearQueue() {
  if (!checkAuth()) return;
  
  queueHead = 0;
  queueTail = 0;
  queueCount = 0;
  status.queued_events = 0;
  
  for (int i = 0; i < QUEUE_MAX_SIZE; i++) {
    eventQueue[i].valid = false;
  }
  
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Queue cleared\"}");
}

void handleAPIResetStats() {
  if (!checkAuth()) return;
  
  motion.total_events = 0;
  motion.motion_count = 0;
  status.successful_tx = 0;
  status.failed_tx = 0;
  status.retry_count = 0;
  status.last_error[0] = '\0';
  
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Statistics reset\"}");
}

void handleAPIRestart() {
  if (!checkAuth()) return;
  
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Restarting\"}");
  delay(1000);
  ESP.restart();
}

void handleAPIFactoryReset() {
  if (!checkAuth()) return;
  
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Factory reset, restarting\"}");
  delay(1000);
  
  // Reset config to defaults
  config.magic = 0;
  saveConfig();
  
  ESP.restart();
}

void handleAPIDiagnostics() {
  if (!checkAuth()) return;
  
  FSInfo fs;
  LittleFS.info(fs);
  
  String diag = "=== Motion System Diagnostics ===\n";
  diag += "Version: " + String(FIRMWARE_VERSION) + "\n";
  diag += "Device: " + String(config.device_name) + "\n";
  diag += "Sensor ID: " + getDeviceID() + "\n";
  diag += "Uptime: " + String(status.uptime) + "s\n";
  diag += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes\n";
  diag += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "\n";
  diag += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";
  diag += "IP: " + (status.ap_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\n";
  diag += "MAC: " + WiFi.macAddress() + "\n";
  diag += "AP Mode: " + String(status.ap_mode ? "Yes" : "No") + "\n";
  diag += "NTP: " + String(status.ntp_synced ? "Synced" : "Not synced") + "\n";
  diag += "Current Time: " + getTimestamp() + "\n";
  diag += "Motion Active: " + String(motion.active ? "Yes" : "No") + "\n";
  diag += "Total Events: " + String(motion.total_events) + "\n";
  diag += "Motion Count: " + String(motion.motion_count) + "\n";
  diag += "Synced: " + String(status.successful_tx) + "\n";
  diag += "Failed: " + String(status.failed_tx) + "\n";
  diag += "Queued: " + String(status.queued_events) + "\n";
  diag += "Retries: " + String(status.retry_count) + "\n";
  diag += "Last TX: " + String(status.last_tx) + "\n";
  diag += "Last Error: " + String(status.last_error) + "\n";
  diag += "Time Range: " + String(inTimeRange() ? "Active" : "Inactive") + "\n";
  diag += "FS Used: " + String(fs.usedBytes / 1024) + " KB\n";
  diag += "FS Total: " + String(fs.totalBytes / 1024) + " KB\n";
  diag += "================================\n";
  
  server.send(200, "text/plain", diag);
}

void handleNotFound() {
  server.send(404, "text/plain", "404 - Not Found");
}

void setupWebServer() {
  // HTML pages
  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/setup", HTTP_GET, handleSetup);
  server.on("/admin", HTTP_GET, []() {
    if (checkAuth()) {
      server.send(200, "text/html", admin_html);
    }
  });
  
  // Setup API
  server.on("/save-setup", HTTP_POST, handleSaveSetup);
  
  // Configuration API
  server.on("/api/config", HTTP_GET, handleAPIGetConfig);
  server.on("/api/config", HTTP_POST, handleAPIConfig);
  
  // Status API
  server.on("/api/status", HTTP_GET, handleAPIStatus);
  server.on("/api/events", HTTP_GET, handleAPIEvents);
  server.on("/api/sync", HTTP_POST, handleAPISync);
  server.on("/api/clear-queue", HTTP_POST, handleAPIClearQueue);
  server.on("/api/reset-stats", HTTP_POST, handleAPIResetStats);
  server.on("/api/restart", HTTP_POST, handleAPIRestart);
  server.on("/api/factory-reset", HTTP_POST, handleAPIFactoryReset);
  server.on("/api/diagnostics", HTTP_GET, handleAPIDiagnostics);
  
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println(F("✓ Web server started"));
}

// ============= SETUP =============
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial.println(F("\n================================"));
  Serial.println(F("Motion Detection System v6.0"));
  Serial.println(F("================================\n"));
  
  status.start_time = millis();
  
  pinMode(PIN_BLUE_LED, OUTPUT);
  pinMode(PIN_GREEN_LED, OUTPUT);
  pinMode(PIN_RED_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_PIR_SENSOR, INPUT_PULLUP);
  
  digitalWrite(PIN_BLUE_LED, HIGH);
  digitalWrite(PIN_GREEN_LED, LOW);
  digitalWrite(PIN_RED_LED, LOW);
  digitalWrite(PIN_BUZZER, LOW);
  
  printSystemInfo();
  
  // Test LEDs
  digitalWrite(PIN_RED_LED, HIGH); delay(200);
  digitalWrite(PIN_GREEN_LED, HIGH); delay(200);
  digitalWrite(PIN_BLUE_LED, LOW); delay(200);
  digitalWrite(PIN_RED_LED, LOW);
  digitalWrite(PIN_GREEN_LED, LOW);
  digitalWrite(PIN_BLUE_LED, HIGH);
  
  loadConfig();
  loadQueue();
  
  setupWiFi();
  setupWebServer();
  
  if (status.ap_mode) {
    dnsServer.start(53, "*", WiFi.softAPIP());
  }
  
  Serial.println(F("\n✓ System ready"));
  if (status.ap_mode) {
    Serial.printf("AP Mode: http://%s/setup\n", WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("Admin: http://%s/admin\n", WiFi.localIP().toString().c_str());
    Serial.printf("Login: admin / %s\n", config.admin_password);
  }
  Serial.println(F("================================\n"));
}

// ============= LOOP =============
void loop() {
  static uint32_t lastWiFiCheck = 0;
  static uint32_t lastQueueCheck = 0;
  static uint32_t lastDisplay = 0;
  
  uint32_t now = millis();
  
  if (status.ap_mode) {
    dnsServer.processNextRequest();
  }
  
  server.handleClient();
  
  if (now - lastWiFiCheck > 5000) {
    checkWiFi();
    lastWiFiCheck = now;
  }
  
  checkMotion();
  
  if (now - lastQueueCheck > 5000) {
    lastQueueCheck = now;
    if (status.wifi_connected && !status.ap_mode && inTimeRange() && queueCount > 0) {
      processQueue();
    }
  }
  
  if (now - lastDisplay > 30000) {
    lastDisplay = now;
    updateTimeDisplay();
    Serial.printf("[Status] Motion:%s Events:%d/%d/%d WiFi:%d RSSI:%d\n",
                  motion.active ? "ACTIVE" : "INACTIVE",
                  motion.total_events, status.successful_tx, status.queued_events,
                  status.wifi_connected, status.rssi);
  }
  
  delay(10);
}
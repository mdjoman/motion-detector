/*
 * ESP8266 Motion Security System with Buzzer
 * Version: 8.4.0.0 - Real-time Security System
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <Ticker.h>
#include <time.h>
#include <ArduinoJson.h>

// ============= CONFIGURATION =============
#define FIRMWARE_VERSION "8.4.0"

// Pin Definitions
#define PIN_RED_LED     0   // GPIO0 - D3 (Active LOW)
#define PIN_GREEN_LED   14  // GPIO14 - D5 (WiFi status indicator)
#define PIN_PIR_SENSOR  12  // GPIO12 - D6
#define PIN_BUZZER      13  // GPIO13 - D7 (Active HIGH)

// System Constants
#define EEPROM_SIZE      512
#define WARMUP_TIME      10000
#define MOTION_CHECK_MS  100
#define NTP_TIMEOUT      5000

// Default values
#define DEFAULT_BUZZER_DELAY_MS  60000
#define DEFAULT_BUZZER_DURATION  500

// ============= GLOBAL OBJECTS =============
ESP8266WebServer server(80);
WiFiManager wifiManager;

// ============= DATA STRUCTURES =============
struct SystemConfig {
  char device_name[32];
  char sensor_name[32];
  char sensor_number[8];
  char wifi_ssid[32];
  char wifi_password[64];
  uint8_t buzzer_enabled;
  uint8_t led_enabled;
  uint32_t buzzer_delay_ms;
  uint32_t buzzer_duration_ms;
  uint32_t magic_number;
} config = {
  "Motion_Sensor",
  "PIR_Sensor",
  "1",
  "",
  "",
  1, 1,
  DEFAULT_BUZZER_DELAY_MS,
  DEFAULT_BUZZER_DURATION,
  0xABCD1234
};

// Motion state
struct {
  uint32_t total_events;
  uint32_t last_buzzer_time;
  uint8_t motion_detected;
  uint32_t current_duration;
  uint32_t motion_start_time;
} motion = {0};

// System status
struct {
  uint32_t uptime;
  char current_ssid[32];
  char current_time[20];
  uint8_t wifi_connected;
  uint8_t ntp_synced;
} status = {0};

// ============= FUNCTION PROTOTYPES =============
void loadConfig();
void saveConfig();
void connectWiFi();
void configModeCallback(WiFiManager* mgr);
bool syncNTP();
void saveWiFiCredentials();
void getTimeString(char* buffer, size_t len);
void checkMotion();
void setupWebServer();
void handleRoot();
void handleAPIStatus();
void handleAPITest();
void handleAPIRestart();
void handleAPIReset();
void handleAPIGetConfig();
void handleAPISaveConfig();
void handleStartAPMode();
void handleSaveWiFi();
void handleEvents();
String getEventData();
void printSystemInfo();
String getDeviceId();
void testIndicators();
void ledPatternMotionDetect();
void ledPatternMotionActive();
void ledPatternWifiConnecting();
void ledPatternWifiConnected();
void ledPatternWifiDisconnected();
void ledPatternSensorReady();
void ledPatternAPMode();
void ledAllOff();
void setRedLED(uint8_t on);
void setGreenLED(uint8_t on);
void buzzerOn();
void buzzerOff();
void buzzerBeep(int duration_ms);
void buzzerTest();

// ============= LED CONTROL FUNCTIONS =============
void setRedLED(uint8_t on) { 
  digitalWrite(PIN_RED_LED, on ? LOW : HIGH); 
}

void setGreenLED(uint8_t on) { 
  digitalWrite(PIN_GREEN_LED, on ? HIGH : LOW); 
}

void ledPatternMotionDetect() {
  setRedLED(1);
  delay(100);
  setRedLED(0);
  delay(100);
  setRedLED(1);
  delay(100);
  setRedLED(0);
}

void ledPatternMotionActive() {
  setRedLED(1);
}

void ledPatternWifiConnecting() {
  static uint32_t lastBlink = 0;
  if (millis() - lastBlink > 200) {
    lastBlink = millis();
    setGreenLED(!digitalRead(PIN_GREEN_LED));
  }
}

void ledPatternWifiConnected() {
  setGreenLED(1);
  setRedLED(0);
}

void ledPatternWifiDisconnected() {
  static uint32_t lastBlink = 0;
  if (millis() - lastBlink > 1000) {
    lastBlink = millis();
    setGreenLED(!digitalRead(PIN_GREEN_LED));
  }
}

void ledPatternSensorReady() {
  if (status.wifi_connected) setGreenLED(1);
  setRedLED(0);
}

void ledPatternAPMode() {
  setRedLED(1);
  setGreenLED(0);
}

void ledAllOff() {
  setRedLED(0);
  setGreenLED(0);
}

// ============= BUZZER CONTROL =============
void buzzerOn() { digitalWrite(PIN_BUZZER, HIGH); }
void buzzerOff() { digitalWrite(PIN_BUZZER, LOW); }

void buzzerBeep(int duration_ms) {
  buzzerOn();
  delay(duration_ms);
  buzzerOff();
}

void buzzerTest() {
  Serial.println(F("Testing buzzer..."));
  buzzerBeep(config.buzzer_duration_ms);
  delay(300);
  buzzerBeep(config.buzzer_duration_ms);
  delay(300);
  buzzerBeep(config.buzzer_duration_ms);
}

// ============= EEPROM MANAGEMENT =============
void saveConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();
  Serial.println(F("Config saved"));
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);
  
  if (config.magic_number != 0xABCD1234) {
    strcpy(config.device_name, "Motion_Sensor");
    strcpy(config.sensor_name, "PIR_Sensor");
    strcpy(config.sensor_number, "1");
    config.buzzer_enabled = 1;
    config.led_enabled = 1;
    config.buzzer_delay_ms = DEFAULT_BUZZER_DELAY_MS;
    config.buzzer_duration_ms = DEFAULT_BUZZER_DURATION;
    saveConfig();
  }
  EEPROM.end();
  Serial.println(F("Config loaded"));
}

void saveWiFiCredentials() {
  strcpy(config.wifi_ssid, WiFi.SSID().c_str());
  strcpy(config.wifi_password, WiFi.psk().c_str());
  saveConfig();
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

void printSystemInfo() {
  Serial.printf("\n v%s | Heap:%u | ID:%s\n", 
    FIRMWARE_VERSION, ESP.getFreeHeap(), getDeviceId().c_str());
}

void testIndicators() {
  setRedLED(1); delay(200);
  setRedLED(0); delay(200);
  setGreenLED(1); delay(200);
  setGreenLED(0);
  buzzerTest();
}

// ============= NTP SYNC =============
bool syncNTP() {
  Serial.print(F("NTP..."));
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  
  uint32_t start = millis();
  while (millis() - start < NTP_TIMEOUT) {
    delay(200);
    if (time(nullptr) > 100000) {
      char ts[30];
      getTimeString(ts, sizeof(ts));
      Serial.printf(" %s\n", ts);
      status.ntp_synced = 1;
      return true;
    }
    Serial.print(".");
  }
  Serial.println(F(" FAIL"));
  status.ntp_synced = 0;
  return false;
}

// ============= WIFI MANAGEMENT =============
void configModeCallback(WiFiManager* mgr) {
  Serial.println(F("AP Mode Active"));
  ledPatternAPMode();
}

void connectWiFi() {
  Serial.println(F("\nWiFi..."));
  
  if (config.wifi_ssid[0]) {
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
      ledPatternWifiConnecting();
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("%s | %s\n", config.wifi_ssid, WiFi.localIP().toString().c_str());
      ledPatternWifiConnected();
      strcpy(status.current_ssid, WiFi.SSID().c_str());
      syncNTP();
      return;
    }
  }
  
  wifiManager.setTimeout(120);
  wifiManager.setAPCallback(configModeCallback);
  if (!wifiManager.autoConnect("Motion-Sensor-AP", "12345678")) {
    ESP.restart();
  }
  
  saveWiFiCredentials();
  ledPatternWifiConnected();
  strcpy(status.current_ssid, WiFi.SSID().c_str());
  Serial.printf("%s\n", WiFi.localIP().toString().c_str());
  syncNTP();
}

void startAPMode() {
  Serial.println(F("Starting AP Mode..."));
  wifiManager.resetSettings();
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.startConfigPortal("Motion-Sensor-AP", "12345678");
}

// ============= MOTION DETECTION =============
void checkMotion() {
  static uint32_t lastCheck = 0;
  static uint8_t warmedUp = 0;
  static uint8_t lastMotionState = 0;
  
  uint32_t now = millis();
  
  if (now - lastCheck < MOTION_CHECK_MS) return;
  lastCheck = now;
  
  if (!warmedUp) {
    if (now > WARMUP_TIME) {
      warmedUp = 1;
      Serial.println(F("Motion sensor ready"));
      ledPatternSensorReady();
    }
    return;
  }
  
  int cur = digitalRead(PIN_PIR_SENSOR);
  uint8_t currentMotionState = (cur == HIGH);
  
  // Check for state change
  if (currentMotionState != lastMotionState) {
    if (currentMotionState) {
      // Motion started
      motion.motion_detected = 1;
      motion.total_events++;
      motion.motion_start_time = now;
      motion.last_buzzer_time = 0;
      
      Serial.printf("\n🔴 MOTION #%u DETECTED at %s\n", motion.total_events, status.current_time);
      ledPatternMotionDetect();
      
      // Immediate buzzer on motion start
      if (config.buzzer_enabled) {
        buzzerBeep(config.buzzer_duration_ms);
        motion.last_buzzer_time = now;
      }
    } else {
      // Motion ended
      uint32_t duration = (now - motion.motion_start_time) / 1000;
      Serial.printf("\n🟢 MOTION #%u ENDED (duration: %us)\n", motion.total_events, duration);
      motion.motion_detected = 0;
      buzzerOff();
      ledPatternSensorReady();
    }
  }
  
  // Handle continuous motion buzzer with delay
  if (motion.motion_detected && config.buzzer_enabled) {
    if (now - motion.last_buzzer_time >= config.buzzer_delay_ms) {
      buzzerBeep(config.buzzer_duration_ms);
      motion.last_buzzer_time = now;
    }
  }
  
  // Update LED for active motion
  if (motion.motion_detected) {
    ledPatternMotionActive();
  }
  
  lastMotionState = currentMotionState;
}

// ============= SSE EVENT STREAM =============
String getEventData() {
  StaticJsonDocument<256> doc;
  status.uptime = millis() / 1000;
  getTimeString(status.current_time, sizeof(status.current_time));
  
  doc["motion"] = motion.motion_detected;
  doc["total_events"] = motion.total_events;
  doc["current_time"] = status.current_time;
  doc["uptime_hours"] = status.uptime / 3600;
  doc["wifi"] = (WiFi.status() == WL_CONNECTED);
  doc["ssid"] = status.current_ssid;
  doc["device_name"] = config.device_name;
  doc["buzzer_delay"] = config.buzzer_delay_ms / 1000;
  
  String output;
  serializeJson(doc, output);
  return output;
}

void handleEvents() {
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Content-Type", "text/event-stream");
  server.sendHeader("Connection", "keep-alive");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  
  String eventData = "data: " + getEventData() + "\n\n";
  server.send(200, "text/event-stream", eventData);
}

// ============= WEB HANDLERS =============
void handleRoot() {
  server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>REAL-TIME SECURITY SYSTEM</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background: linear-gradient(135deg, #1a1a2e, #16213e);
      min-height: 100vh;
      padding: 20px;
      color: #eee;
    }
    .container { max-width: 1200px; margin: 0 auto; }
    .card {
      background: rgba(255, 255, 255, 0.1);
      backdrop-filter: blur(10px);
      border-radius: 20px;
      padding: 25px;
      margin-bottom: 20px;
      transition: all 0.3s ease;
    }
    .header { text-align: center; margin-bottom: 20px; }
    .header h1 { font-size: 2em; color: #ff6b6b; }
    .status-card { text-align: center; padding: 30px; }
    .motion-status {
      font-size: 3em;
      font-weight: bold;
      margin: 20px 0;
      padding: 20px;
      border-radius: 15px;
      transition: all 0.1s ease;
    }
    .motion-active {
      background: #ff4444;
      color: white;
      box-shadow: 0 0 30px rgba(255, 68, 68, 0.5);
      animation: pulse 0.5s infinite;
    }
    .motion-inactive {
      background: #44ff44;
      color: #1a1a2e;
    }
    @keyframes pulse {
      0% { transform: scale(1); opacity: 1; }
      50% { transform: scale(1.02); opacity: 0.9; }
      100% { transform: scale(1); opacity: 1; }
    }
    .stats-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
      gap: 15px;
      margin: 20px 0;
    }
    .stat-card {
      background: rgba(0, 0, 0, 0.5);
      border-radius: 15px;
      padding: 15px;
      text-align: center;
      transition: transform 0.3s ease;
    }
    .stat-card:hover { transform: translateY(-5px); }
    .stat-label { font-size: 0.9em; opacity: 0.8; }
    .stat-value { font-size: 1.8em; font-weight: bold; margin-top: 5px; }
    .btn {
      padding: 12px 24px;
      border: none;
      border-radius: 10px;
      font-size: 1em;
      cursor: pointer;
      transition: all 0.3s;
      margin: 5px;
    }
    .btn-test { background: #ff6b6b; color: white; }
    .btn-test:hover { background: #ff4444; transform: scale(1.05); }
    .btn-reset { background: #ffa500; color: white; }
    .btn-reset:hover { background: #ff8c00; transform: scale(1.05); }
    .btn-restart { background: #4a4a4a; color: white; }
    .btn-restart:hover { background: #333; transform: scale(1.05); }
    .btn-save { background: #00ff00; color: #1a1a2e; }
    .btn-save:hover { background: #00cc00; transform: scale(1.05); }
    .btn-ap { background: #ff00ff; color: white; }
    .btn-ap:hover { background: #cc00cc; transform: scale(1.05); }
    .button-group {
      display: flex;
      gap: 15px;
      justify-content: center;
      flex-wrap: wrap;
      margin-top: 20px;
    }
    .wifi-status {
      display: inline-block;
      padding: 5px 10px;
      border-radius: 10px;
      font-size: 0.8em;
      margin-top: 10px;
    }
    .wifi-connected { background: #44ff44; color: #1a1a2e; }
    .wifi-disconnected { background: #ff4444; color: white; }
    .form-group { margin-bottom: 20px; }
    label {
      display: block;
      margin-bottom: 8px;
      font-weight: bold;
      color: #ff6b6b;
    }
    input, select {
      width: 100%;
      padding: 12px;
      background: rgba(0, 0, 0, 0.5);
      border: 1px solid #ff6b6b;
      border-radius: 10px;
      color: white;
      font-size: 1em;
    }
    input:focus { outline: none; border-color: #00ff00; }
    .config-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
      gap: 20px;
    }
    .footer {
      text-align: center;
      margin-top: 30px;
      opacity: 0.7;
      font-size: 0.8em;
    }
    .real-time-badge {
      background: #ff0000;
      color: white;
      padding: 5px 10px;
      border-radius: 20px;
      font-size: 0.7em;
      display: inline-block;
      margin-left: 10px;
      animation: blink 1s infinite;
    }
    @keyframes blink {
      0% { opacity: 1; }
      50% { opacity: 0.5; }
      100% { opacity: 1; }
    }
    .connection-status {
      position: fixed;
      top: 10px;
      right: 10px;
      padding: 5px 10px;
      border-radius: 10px;
      font-size: 0.7em;
      z-index: 1000;
    }
    .connected { background: #00ff00; color: black; }
    .disconnected { background: #ff0000; color: white; }
  </style>
</head>
<body>
  <div id="connStatus" class="connection-status connected">● LIVE</div>
  
  <!-- Alarm Sound - Using multiple reliable sources -->
  <audio id="alertSound" loop preload="auto">
    <source src="https://drive.google.com/uc?export=download&id=1PDYsSXHCYH3z3JImci9kbLgnVGII2cuG" type="audio/mpeg">
  </audio>
  
  <div class="container">
    <div class="card">
      <div class="header">
        <h1>SECURITY SYSTEM <span class="real-time-badge">REAL-TIME</span></h1>
        <p>v8.4.0 | <span id="deviceName">Motion Sensor</span></p>
        <div id="wifiBadge" class="wifi-status">CHECKING...</div>
      </div>
    </div>
    
    <div class="card status-card">
      <h2>MOTION STATUS</h2>
      <div class="motion-status" id="motionStatus">CHECKING...</div>
      <div class="volume-control" style="margin-top: 15px;">
        <span>Alert Volume:</span>
        <input type="range" id="volume" min="0" max="1" step="0.1" value="0.5">
        <span id="volumeValue">50%</span>
      </div>
      <div style="margin-top: 10px;">
        <button class="btn btn-test" onclick="testSound()" style="background: #666; padding: 5px 15px; font-size: 0.8em;">🔊 TEST SOUND</button>
      </div>
    </div>
    
    <div class="stats-grid">
      <div class="stat-card">
        <div class="stat-label">TOTAL EVENTS</div>
        <div class="stat-value" id="totalEvents">0</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">CURRENT TIME</div>
        <div class="stat-value" id="currentTime">--:--:--</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">UPTIME</div>
        <div class="stat-value" id="uptime">0h</div>
      </div>
    </div>
    
    <div class="card">
      <h2>SYSTEM CONFIGURATION</h2>
      <div class="config-grid">
        <div class="form-group">
          <label>Device Name</label>
          <input type="text" id="deviceNameInput" placeholder="Device Name">
        </div>
        <div class="form-group">
          <label>Sensor Name</label>
          <input type="text" id="sensorNameInput" placeholder="Sensor Name">
        </div>
        <div class="form-group">
          <label>Sensor Number</label>
          <input type="text" id="sensorNumberInput" placeholder="Sensor Number">
        </div>
        <div class="form-group">
          <label>WiFi SSID</label>
          <input type="text" id="wifiSsidInput" placeholder="WiFi SSID">
        </div>
        <div class="form-group">
          <label>WiFi Password</label>
          <input type="password" id="wifiPasswordInput" placeholder="WiFi Password">
        </div>
        <div class="form-group">
          <label>Buzzer Delay (seconds)</label>
          <input type="number" id="buzzerDelayInput" placeholder="Delay between beeps">
        </div>
        <div class="form-group">
          <label>Buzzer Duration (ms)</label>
          <input type="number" id="buzzerDurationInput" placeholder="Beep duration">
        </div>
        <div class="form-group">
          <label>
            <input type="checkbox" id="buzzerEnabledInput"> Enable Buzzer
          </label>
        </div>
      </div>
      
      <div class="button-group">
        <button class="btn btn-save" onclick="saveConfig()">SAVE CONFIGURATION</button>
        <button class="btn btn-ap" onclick="startAPMode()">START AP MODE</button>
      </div>
      
      <div class="button-group">
        <button class="btn btn-test" onclick="testMotion()">TEST MOTION</button>
        <button class="btn btn-reset" onclick="resetStats()">RESET STATS</button>
        <button class="btn btn-restart" onclick="restartDevice()">RESTART</button>
      </div>
    </div>
    
    <div class="footer">
      Motion Sensor Security System | Real-time Updates via SSE | Danger Alarm Sound
    </div>
  </div>
  
  <script>
    let lastMotionState = false;
    let alertAudio = document.getElementById('alertSound');
    let volumeSlider = document.getElementById('volume');
    let volumeValue = document.getElementById('volumeValue');
    let eventSource = null;
    
    alertAudio.volume = 0.5;
    
    volumeSlider.addEventListener('input', function() {
      alertAudio.volume = this.value;
      volumeValue.innerHTML = Math.round(this.value * 100) + '%';
    });
    
    function playAlert() {
      alertAudio.currentTime = 0;
      let playPromise = alertAudio.play();
      if (playPromise !== undefined) {
        playPromise.catch(error => {
          console.log('Audio play error:', error);
          // Try to reload and play again
          alertAudio.load();
          setTimeout(() => {
            alertAudio.play().catch(e => console.log('Still cannot play:', e));
          }, 100);
        });
      }
    }
    
    function stopAlert() {
      alertAudio.pause();
      alertAudio.currentTime = 0;
    }
    
    function testSound() {
      playAlert();
      setTimeout(() => {
        if (!lastMotionState) {
          stopAlert();
        }
      }, 3000);
    }
    
    // Real-time event source
    function initEventSource() {
      if (eventSource) {
        eventSource.close();
      }
      
      eventSource = new EventSource('/events');
      
      eventSource.onopen = function() {
        console.log('SSE Connected');
        document.getElementById('connStatus').className = 'connection-status connected';
        document.getElementById('connStatus').innerHTML = '● LIVE';
      };
      
      eventSource.onerror = function() {
        console.log('SSE Error');
        document.getElementById('connStatus').className = 'connection-status disconnected';
        document.getElementById('connStatus').innerHTML = '● RECONNECTING...';
        setTimeout(initEventSource, 3000);
      };
      
      eventSource.onmessage = function(event) {
        const data = JSON.parse(event.data);
        updateUI(data);
      };
    }
    
    function updateUI(data) {
      const motionEl = document.getElementById('motionStatus');
      if (data.motion) {
        motionEl.innerHTML = '🚨 MOTION DETECTED 🚨';
        motionEl.className = 'motion-status motion-active';
        if (!lastMotionState) {
          playAlert();
        }
        lastMotionState = true;
      } else {
        motionEl.innerHTML = '✅ NO MOTION';
        motionEl.className = 'motion-status motion-inactive';
        if (lastMotionState) {
          stopAlert();
        }
        lastMotionState = false;
      }
      
      document.getElementById('totalEvents').innerHTML = data.total_events;
      document.getElementById('currentTime').innerHTML = data.current_time || '--:--:--';
      document.getElementById('uptime').innerHTML = data.uptime_hours + 'h';
      document.getElementById('deviceName').innerHTML = data.device_name;
      
      const wifiBadge = document.getElementById('wifiBadge');
      if (data.wifi) {
        wifiBadge.innerHTML = 'WiFi: ' + data.ssid;
        wifiBadge.className = 'wifi-status wifi-connected';
      } else {
        wifiBadge.innerHTML = 'WiFi: DISCONNECTED';
        wifiBadge.className = 'wifi-status wifi-disconnected';
      }
    }
    
    function loadConfig() {
      fetch('/api/config')
        .then(response => response.json())
        .then(data => {
          document.getElementById('deviceNameInput').value = data.device_name;
          document.getElementById('sensorNameInput').value = data.sensor_name;
          document.getElementById('sensorNumberInput').value = data.sensor_number;
          document.getElementById('wifiSsidInput').value = data.wifi_ssid;
          document.getElementById('buzzerDelayInput').value = data.buzzer_delay;
          document.getElementById('buzzerDurationInput').value = data.buzzer_duration;
          document.getElementById('buzzerEnabledInput').checked = data.buzzer_enabled;
        })
        .catch(error => console.log('Error:', error));
    }
    
    function saveConfig() {
      const formData = new FormData();
      formData.append('device_name', document.getElementById('deviceNameInput').value);
      formData.append('sensor_name', document.getElementById('sensorNameInput').value);
      formData.append('sensor_number', document.getElementById('sensorNumberInput').value);
      formData.append('wifi_ssid', document.getElementById('wifiSsidInput').value);
      formData.append('wifi_password', document.getElementById('wifiPasswordInput').value);
      formData.append('buzzer_delay', document.getElementById('buzzerDelayInput').value);
      formData.append('buzzer_duration', document.getElementById('buzzerDurationInput').value);
      formData.append('buzzer_enabled', document.getElementById('buzzerEnabledInput').checked ? '1' : '0');
      
      fetch('/api/config', { method: 'POST', body: formData })
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            alert('Configuration saved successfully!');
            loadConfig();
          } else {
            alert('Save failed!');
          }
        })
        .catch(error => alert('Error saving config: ' + error));
    }
    
    function startAPMode() {
      if (confirm('Start AP Mode? You will need to reconnect to the new WiFi network.')) {
        fetch('/api/apmode', { method: 'POST' })
          .then(() => {
            alert('AP Mode started. Connect to "Motion-Sensor-AP" with password "12345678"');
            setTimeout(() => window.location.href = '/', 5000);
          })
          .catch(error => alert('Error: ' + error));
      }
    }
    
    function testMotion() {
      fetch('/api/test', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            alert('Test motion triggered!');
            playAlert();
            setTimeout(() => { if (!lastMotionState) stopAlert(); }, 3000);
          } else {
            alert(data.message);
          }
        })
        .catch(error => alert('Test failed: ' + error));
    }
    
    function resetStats() {
      if (confirm('Reset all statistics?')) {
        fetch('/api/reset', { method: 'POST' })
          .then(() => {
            alert('Statistics reset');
          })
          .catch(() => alert('Reset failed'));
      }
    }
    
    function restartDevice() {
      if (confirm('Restart device?')) {
        fetch('/api/restart', { method: 'POST' })
          .then(() => alert('Restarting...'))
          .catch(() => alert('Restart failed'));
      }
    }
    
    // Initialize
    initEventSource();
    loadConfig();
  </script>
</body>
</html>
  )rawliteral");
}
void handleAPIStatus() {
  status.uptime = millis() / 1000;
  getTimeString(status.current_time, sizeof(status.current_time));
  
  StaticJsonDocument<256> doc;
  doc["wifi"] = (WiFi.status() == WL_CONNECTED);
  doc["ssid"] = status.current_ssid;
  doc["motion"] = motion.motion_detected;
  doc["uptime"] = status.uptime;
  doc["uptime_hours"] = status.uptime / 3600;
  doc["total_events"] = motion.total_events;
  doc["ip"] = WiFi.localIP().toString();
  doc["version"] = FIRMWARE_VERSION;
  doc["current_time"] = status.current_time;
  doc["device_name"] = config.device_name;
  doc["buzzer_enabled"] = config.buzzer_enabled;
  doc["buzzer_delay"] = config.buzzer_delay_ms / 1000;
  doc["buzzer_duration"] = config.buzzer_duration_ms;
  
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAPIGetConfig() {
  StaticJsonDocument<256> doc;
  doc["device_name"] = config.device_name;
  doc["sensor_name"] = config.sensor_name;
  doc["sensor_number"] = config.sensor_number;
  doc["wifi_ssid"] = config.wifi_ssid;
  doc["buzzer_enabled"] = config.buzzer_enabled;
  doc["buzzer_delay"] = config.buzzer_delay_ms / 1000;
  doc["buzzer_duration"] = config.buzzer_duration_ms;
  
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAPISaveConfig() {
  if (server.hasArg("device_name")) 
    strlcpy(config.device_name, server.arg("device_name").c_str(), sizeof(config.device_name));
  if (server.hasArg("sensor_name")) 
    strlcpy(config.sensor_name, server.arg("sensor_name").c_str(), sizeof(config.sensor_name));
  if (server.hasArg("sensor_number")) 
    strlcpy(config.sensor_number, server.arg("sensor_number").c_str(), sizeof(config.sensor_number));
  if (server.hasArg("wifi_ssid") && server.arg("wifi_ssid").length()) 
    strlcpy(config.wifi_ssid, server.arg("wifi_ssid").c_str(), sizeof(config.wifi_ssid));
  if (server.hasArg("wifi_password") && server.arg("wifi_password").length()) 
    strlcpy(config.wifi_password, server.arg("wifi_password").c_str(), sizeof(config.wifi_password));
  if (server.hasArg("buzzer_delay")) 
    config.buzzer_delay_ms = atoi(server.arg("buzzer_delay").c_str()) * 1000;
  if (server.hasArg("buzzer_duration")) 
    config.buzzer_duration_ms = atoi(server.arg("buzzer_duration").c_str());
  if (server.hasArg("buzzer_enabled")) 
    config.buzzer_enabled = server.arg("buzzer_enabled") == "1" ? 1 : 0;
  
  saveConfig();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleAPITest() {
  if (!motion.motion_detected) {
    motion.motion_detected = 1;
    motion.total_events++;
    motion.last_buzzer_time = 0;
    motion.motion_start_time = millis();
    
    Serial.printf("\n🔴 TEST #%u START\n", motion.total_events);
    
    ledPatternMotionDetect();
    
    if (config.buzzer_enabled) {
      buzzerBeep(config.buzzer_duration_ms);
      motion.last_buzzer_time = millis();
    }
    
    static Ticker testTimer;
    testTimer.once(3, []() {
      if (motion.motion_detected) {
        Serial.printf("\n🟢 TEST #%u END\n", motion.total_events);
        motion.motion_detected = 0;
        buzzerOff();
        ledPatternSensorReady();
      }
    });
    
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Test motion triggered\"}");
  } else {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Motion already active\"}");
  }
}

void handleAPIRestart() {
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Restarting\"}");
  delay(500);
  ESP.restart();
}

void handleAPIReset() {
  motion.total_events = 0;
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Reset\"}");
}

void handleStartAPMode() {
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Starting AP Mode\"}");
  delay(500);
  startAPMode();
}

void handleSaveWiFi() {
  if (server.hasArg("ssid")) {
    strlcpy(config.wifi_ssid, server.arg("ssid").c_str(), sizeof(config.wifi_ssid));
    if (server.hasArg("password")) 
      strlcpy(config.wifi_password, server.arg("password").c_str(), sizeof(config.wifi_password));
    saveConfig();
    server.send(200, "text/html", 
      "<html><body style='background:#1a1a2e; color:white; text-align:center; padding:50px;'>"
      "<h1>WIFI SAVED!</h1><p>RESTARTING...</p>"
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
  server.on("/events", handleEvents);
  server.on("/api/status", handleAPIStatus);
  server.on("/api/config", HTTP_GET, handleAPIGetConfig);
  server.on("/api/config", HTTP_POST, handleAPISaveConfig);
  server.on("/api/test", HTTP_POST, handleAPITest);
  server.on("/api/reset", HTTP_POST, handleAPIReset);
  server.on("/api/restart", HTTP_POST, handleAPIRestart);
  server.on("/api/apmode", HTTP_POST, handleStartAPMode);
  server.on("/save-wifi", HTTP_POST, handleSaveWiFi);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println(F("Web server ready"));
}

// ============= SETUP =============
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial.println(F("\n🔔 MOTION SECURITY SYSTEM v8.4.0 - REAL-TIME"));
  Serial.println(F(""));
  
  pinMode(PIN_RED_LED, OUTPUT);
  pinMode(PIN_GREEN_LED, OUTPUT);
  pinMode(PIN_PIR_SENSOR, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  
  ledAllOff();
  buzzerOff();
  
  printSystemInfo();
  testIndicators();
  loadConfig();
  connectWiFi();
  setupWebServer();
  
  Serial.printf("Web: http://%s\n\n", WiFi.localIP().toString().c_str());
  Serial.printf("Buzzer: %dms beep, %d min delay\n", config.buzzer_duration_ms, config.buzzer_delay_ms/60000);
  Serial.printf("Real-time updates: ACTIVE (SSE)\n");
}

// ============= LOOP =============
void loop() {
  server.handleClient();
  
  static uint32_t lastWifi = 0, lastTime = 0;
  uint32_t now = millis();
  
  if (now - lastWifi > 5000) {
    bool wasConnected = status.wifi_connected;
    status.wifi_connected = (WiFi.status() == WL_CONNECTED);
    
    if (status.wifi_connected) {
      strcpy(status.current_ssid, WiFi.SSID().c_str());
      if (!wasConnected) ledPatternWifiConnected();
    } else {
      ledPatternWifiDisconnected();
    }
    lastWifi = now;
  }
  
  if (now - lastTime > 1000) {
    getTimeString(status.current_time, sizeof(status.current_time));
    lastTime = now;
  }
  
  checkMotion();
  
  delay(5);
}
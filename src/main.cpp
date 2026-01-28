#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// Pin Definitions
#define PIR_PIN D5
#define BUZZER_PIN D6
#define LED_PIN D4  // Built-in LED (inverted logic)

// Configuration Structure
struct Config {
  char ssid[32];
  char password[32];
  char apiEndpoint[128];
  int sensorNumber;
  int detectionInterval;
  int buzzerDuration;
  bool buzzerEnabled;
  int timezoneOffset;
  bool apModeEnabled;  // If true, create AP for initial setup
};

Config config;

// Global Objects
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient *timeClient = nullptr;

// State Variables
bool motionDetected = false;
bool lastMotionState = false;
bool inAPMode = false;
unsigned long lastDetectionTime = 0;
unsigned long lastDataSendTime = 0;
unsigned long buzzerStartTime = 0;
bool buzzerActive = false;
String currentStatus = "EMPTY";
String deviceStatus = "BOOTING";

// EEPROM Settings
const int EEPROM_SIZE = 512;
const int CONFIG_ADDRESS = 0;
const char *CONFIG_VERSION = "V3.0";

// Function Prototypes
void setupPins();
void loadConfig();
void saveConfig();
void connectToWiFi();
void startAPMode();
void setupWebServer();
void handleRoot();
void handleConfig();
void handleSave();
void handleStatus();
void handleReset();
void handleWiFiScan();
void sendSensorData();
String getFormattedTime();
void handleMotion();
void controlBuzzer();
void updateDeviceStatus();

// Setup Function
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n" + String(50, '='));
  Serial.println("    ESP8266 Motion Sensor with Buzzer");
  Serial.println(String(50, '='));
  
  // Initialize pins
  setupPins();
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("❌ LittleFS Mount Failed");
  }
  
  // Load configuration
  loadConfig();
  
  // Update device status
  updateDeviceStatus();
  
  // Setup web server (always accessible)
  setupWebServer();
  
  // Try to connect to WiFi
  if (strlen(config.ssid) > 0 && strcmp(config.ssid, "YourWiFiSSID") != 0) {
    connectToWiFi();
  } else {
    Serial.println("⚠️ No WiFi configured. Starting AP mode...");
    startAPMode();
  }
  
  Serial.println("\n✅ System Initialized");
  Serial.println("🌐 Access configuration at: http://" + 
                 (inAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()));
  Serial.println(String(50, '='));
}

// Main Loop
void loop() {
  // Handle web server requests
  server.handleClient();
  
  // Only process sensor if connected to WiFi
  if (!inAPMode && WiFi.status() == WL_CONNECTED) {
    // Update NTP time
    if (timeClient) {
      timeClient->update();
    }
    
    // Handle motion detection
    handleMotion();
    
    // Control buzzer
    controlBuzzer();
    
    // Periodic status update
    if (millis() - lastDataSendTime > (config.detectionInterval * 1000)) {
      sendSensorData();
      lastDataSendTime = millis();
    }
  } else if (inAPMode) {
    // Blink LED slowly in AP mode
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 1000) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      lastBlink = millis();
    }
  }
  
  delay(50);
}

// ========== Pin Setup ==========
void setupPins() {
  pinMode(PIR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, HIGH); // Turn off LED
}

// ========== WiFi Connection ==========
void connectToWiFi() {
  deviceStatus = "CONNECTING_WIFI";
  Serial.println("\n📡 Connecting to WiFi...");
  Serial.println("SSID: " + String(config.ssid));
  
  WiFi.begin(config.ssid, config.password);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    deviceStatus = "CONNECTED";
    inAPMode = false;
    Serial.println("\n✅ WiFi Connected!");
    Serial.println("IP Address: " + WiFi.localIP().toString());
    
    // Initialize NTP client
    if (!timeClient) {
      timeClient = new NTPClient(ntpUDP, "pool.ntp.org", 
                                 config.timezoneOffset * 3600, 60000);
      timeClient->begin();
    }
    
    digitalWrite(LED_PIN, HIGH);
  } else {
    deviceStatus = "WIFI_FAILED";
    Serial.println("\n❌ WiFi Connection Failed");
    Serial.println("Starting AP mode as fallback...");
    startAPMode();
  }
}

void startAPMode() {
  deviceStatus = "AP_MODE";
  inAPMode = true;
  
  // Create unique AP name
  String apName = "MotionSensor_" + String(ESP.getChipId(), HEX);
  apName.toUpperCase();
  
  Serial.println("\n📡 Starting AP Mode...");
  Serial.println("AP Name: " + apName);
  Serial.println("Password: config1234");
  
  WiFi.softAP(apName.c_str(), "config1234");
  
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
  Serial.println("Connect to this AP and go to the IP above");
}

// ========== Configuration Functions ==========
void loadConfig() {
  Serial.println("\n📂 Loading configuration...");
  
  EEPROM.get(CONFIG_ADDRESS, config);
  
  // Check config version
  String version;
  for (int i = 0; i < strlen(CONFIG_VERSION); i++) {
    version += char(EEPROM.read(CONFIG_ADDRESS + sizeof(config) + i));
  }
  
  if (version != CONFIG_VERSION) {
    Serial.println("⚠️ Using default configuration");
    
    // Default values
    strcpy(config.ssid, "");
    strcpy(config.password, "");
    strcpy(config.apiEndpoint, "http://192.168.1.100:3000/api/data");
    config.sensorNumber = 1;
    config.detectionInterval = 30;
    config.buzzerDuration = 3;
    config.buzzerEnabled = true;
    config.timezoneOffset = 5;
    config.apModeEnabled = false;
    
    saveConfig();
  }
  
  Serial.println("✅ Configuration loaded");
}

void saveConfig() {
  Serial.println("💾 Saving configuration...");
  
  EEPROM.put(CONFIG_ADDRESS, config);
  
  // Save version marker
  for (int i = 0; i < strlen(CONFIG_VERSION); i++) {
    EEPROM.write(CONFIG_ADDRESS + sizeof(config) + i, CONFIG_VERSION[i]);
  }
  
  EEPROM.commit();
  Serial.println("✅ Configuration saved");
}

// ========== Motion Detection ==========
void handleMotion() {
  int pirValue = digitalRead(PIR_PIN);
  motionDetected = (pirValue == HIGH);
  
  if (motionDetected != lastMotionState) {
    if (motionDetected) {
      currentStatus = "PRESENT";
      Serial.println("🚨 Motion Detected!");
      
      // Activate buzzer
      if (config.buzzerEnabled) {
        buzzerActive = true;
        buzzerStartTime = millis();
        digitalWrite(BUZZER_PIN, HIGH);
        Serial.println("🔊 Buzzer ON");
      }
      
      // Send immediate data
      sendSensorData();
      lastDetectionTime = millis();
    } else {
      currentStatus = "EMPTY";
      Serial.println("✅ Area Clear");
      sendSensorData();
    }
    lastMotionState = motionDetected;
  }
  
  // LED indicator
  digitalWrite(LED_PIN, motionDetected ? LOW : HIGH);
}

void controlBuzzer() {
  if (buzzerActive && config.buzzerEnabled) {
    if (millis() - buzzerStartTime > (config.buzzerDuration * 1000)) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerActive = false;
      Serial.println("🔇 Buzzer OFF");
    }
  }
}

// ========== Data Sending ==========
void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED || strlen(config.apiEndpoint) == 0) {
    Serial.println(F("⚠️ Cannot send: No WiFi or API endpoint"));
    return;
  }

  String timestamp = getFormattedTime();

  // ---- JSON Payload ----
  StaticJsonDocument<512> jsonDoc;
  jsonDoc["device_id"] = String(F("motion_")) + String(ESP.getChipId(), HEX);
  jsonDoc["sensor_number"] = config.sensorNumber;
  jsonDoc["timestamp"] = timestamp;
  jsonDoc["status"] = currentStatus;
  jsonDoc["motion_detected"] = motionDetected;

  String payload;
  serializeJson(jsonDoc, payload);

  Serial.println(F("\n📤 Sending to API:"));
  Serial.println(payload);

  // ---- HTTP POST (ESP8266) ----
  WiFiClient client;
  HTTPClient http;

  http.setTimeout(5000);  // 5s timeout (important!)
  http.begin(client, config.apiEndpoint);
  http.addHeader(F("Content-Type"), F("application/json"));
  http.addHeader(F("Device-ID"),
                 String(F("ESP8266_Motion_")) + String(config.sensorNumber));

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    Serial.printf("✅ HTTP Response: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.println(F("Response: "));
      Serial.println(response);
    }
  } else {
    Serial.printf("❌ HTTP Error: %s\n",
                  http.errorToString(httpCode).c_str());
  }

  http.end();   // ALWAYS free heap
}

String getFormattedTime() {
  if (!timeClient || WiFi.status() != WL_CONNECTED) {
    return "N/A";
  }
  
  timeClient->update();
  unsigned long epochTime = timeClient->getEpochTime();
  
  struct tm *ptm = gmtime((time_t *)&epochTime);
  
  char timeString[25];
  sprintf(timeString, "%04d-%02d-%02d %02d:%02d:%02d",
          ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
          (ptm->tm_hour + config.timezoneOffset) % 24, ptm->tm_min, ptm->tm_sec);
  
  return String(timeString);
}

// ========== Web Server Handlers ==========
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/wifi-scan", HTTP_GET, handleWiFiScan);
  server.on("/connect", HTTP_POST, []() {
    // Handle direct WiFi connection
    if (server.hasArg("ssid") && server.hasArg("password")) {
      strcpy(config.ssid, server.arg("ssid").c_str());
      strcpy(config.password, server.arg("password").c_str());
      saveConfig();
      
      server.send(200, "text/html", 
        "<html><body><h2>Connecting to WiFi...</h2>"
        "<p>Device will restart and connect to " + String(config.ssid) + "</p>"
        "<script>setTimeout(() => location.href='/', 5000);</script></body></html>");
      
      delay(1000);
      ESP.restart();
    }
  });
  
  server.on("/test-api", HTTP_GET, []() {
    sendSensorData();
    server.send(200, "text/plain", "Test data sent to API");
  });
  
  server.on("/test-buzzer", HTTP_POST, []() {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(1000);
    digitalWrite(BUZZER_PIN, LOW);
    server.send(200, "text/plain", "Buzzer tested");
  });
  
  server.begin();
  Serial.println("✅ Web server started on port 80");
}

void updateDeviceStatus() {
  if (inAPMode) {
    deviceStatus = "AP_MODE";
  } else if (WiFi.status() == WL_CONNECTED) {
    deviceStatus = "CONNECTED";
  } else if (WiFi.status() == WL_CONNECT_FAILED) {
    deviceStatus = "CONNECT_FAILED";
  } else if (WiFi.status() == WL_IDLE_STATUS) {
    deviceStatus = "IDLE";
  } else {
    deviceStatus = "UNKNOWN";
  }
}

void handleRoot() {
  updateDeviceStatus();
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Motion Sensor Control Panel</title>
  <style>
    :root {
      --primary: #4CAF50;
      --secondary: #2196F3;
      --danger: #f44336;
      --warning: #ff9800;
      --dark: #333;
      --light: #f8f9fa;
    }
    
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
    }
    
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      padding: 20px;
    }
    
    .container {
      max-width: 1200px;
      margin: 0 auto;
      background: white;
      border-radius: 20px;
      box-shadow: 0 20px 60px rgba(0,0,0,0.3);
      overflow: hidden;
    }
    
    .header {
      background: linear-gradient(135deg, var(--primary), var(--secondary));
      color: white;
      padding: 30px;
      text-align: center;
    }
    
    .header h1 {
      font-size: 2.5rem;
      margin-bottom: 10px;
    }
    
    .header p {
      opacity: 0.9;
      font-size: 1.1rem;
    }
    
    .content {
      padding: 30px;
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(350px, 1fr));
      gap: 30px;
    }
    
    .card {
      background: var(--light);
      border-radius: 15px;
      padding: 25px;
      box-shadow: 0 5px 15px rgba(0,0,0,0.08);
      transition: transform 0.3s, box-shadow 0.3s;
    }
    
    .card:hover {
      transform: translateY(-5px);
      box-shadow: 0 10px 25px rgba(0,0,0,0.15);
    }
    
    .card h3 {
      color: var(--dark);
      margin-bottom: 20px;
      padding-bottom: 10px;
      border-bottom: 2px solid var(--primary);
      display: flex;
      align-items: center;
      gap: 10px;
    }
    
    .card h3 i {
      color: var(--primary);
    }
    
    .status-grid {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: 15px;
    }
    
    .status-item {
      background: white;
      padding: 15px;
      border-radius: 10px;
      border-left: 4px solid var(--primary);
    }
    
    .status-label {
      font-size: 0.9rem;
      color: #666;
      margin-bottom: 5px;
    }
    
    .status-value {
      font-size: 1.2rem;
      font-weight: bold;
      color: var(--dark);
    }
    
    .motion-status {
      text-align: center;
      padding: 30px;
      border-radius: 15px;
      margin: 20px 0;
      font-size: 1.5rem;
      font-weight: bold;
      transition: all 0.3s;
    }
    
    .status-present {
      background: linear-gradient(135deg, #d4edda, #c3e6cb);
      color: #155724;
      border: 3px solid #155724;
      animation: pulse 2s infinite;
    }
    
    .status-empty {
      background: linear-gradient(135deg, #f8d7da, #f5c6cb);
      color: #721c24;
      border: 3px solid #721c24;
    }
    
    @keyframes pulse {
      0% { transform: scale(1); }
      50% { transform: scale(1.02); }
      100% { transform: scale(1); }
    }
    
    .btn-group {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-top: 20px;
    }
    
    .btn {
      padding: 12px 24px;
      border: none;
      border-radius: 50px;
      font-size: 1rem;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.3s;
      text-decoration: none;
      display: inline-flex;
      align-items: center;
      gap: 8px;
    }
    
    .btn-primary {
      background: var(--primary);
      color: white;
    }
    
    .btn-primary:hover {
      background: #45a049;
      transform: translateY(-2px);
      box-shadow: 0 5px 15px rgba(76, 175, 80, 0.3);
    }
    
    .btn-secondary {
      background: var(--secondary);
      color: white;
    }
    
    .btn-secondary:hover {
      background: #1976D2;
      transform: translateY(-2px);
    }
    
    .btn-warning {
      background: var(--warning);
      color: white;
    }
    
    .btn-warning:hover {
      background: #e68900;
      transform: translateY(-2px);
    }
    
    .btn-danger {
      background: var(--danger);
      color: white;
    }
    
    .btn-danger:hover {
      background: #d32f2f;
      transform: translateY(-2px);
    }
    
    .info-grid {
      display: grid;
      gap: 15px;
    }
    
    .info-item {
      display: flex;
      justify-content: space-between;
      padding: 12px 0;
      border-bottom: 1px solid #eee;
    }
    
    .info-label {
      font-weight: 600;
      color: #555;
    }
    
    .info-value {
      font-family: 'Courier New', monospace;
      color: var(--dark);
    }
    
    .form-group {
      margin-bottom: 20px;
    }
    
    .form-label {
      display: block;
      margin-bottom: 8px;
      font-weight: 600;
      color: #555;
    }
    
    .form-input {
      width: 100%;
      padding: 12px 15px;
      border: 2px solid #ddd;
      border-radius: 10px;
      font-size: 1rem;
      transition: border-color 0.3s;
    }
    
    .form-input:focus {
      outline: none;
      border-color: var(--primary);
      box-shadow: 0 0 0 3px rgba(76, 175, 80, 0.1);
    }
    
    .toggle-switch {
      position: relative;
      display: inline-block;
      width: 60px;
      height: 34px;
    }
    
    .toggle-switch input {
      opacity: 0;
      width: 0;
      height: 0;
    }
    
    .toggle-slider {
      position: absolute;
      cursor: pointer;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background-color: #ccc;
      transition: .4s;
      border-radius: 34px;
    }
    
    .toggle-slider:before {
      position: absolute;
      content: "";
      height: 26px;
      width: 26px;
      left: 4px;
      bottom: 4px;
      background-color: white;
      transition: .4s;
      border-radius: 50%;
    }
    
    input:checked + .toggle-slider {
      background-color: var(--primary);
    }
    
    input:checked + .toggle-slider:before {
      transform: translateX(26px);
    }
    
    .alert {
      padding: 15px;
      border-radius: 10px;
      margin: 20px 0;
      display: flex;
      align-items: center;
      gap: 10px;
    }
    
    .alert-success {
      background: #d4edda;
      color: #155724;
      border: 1px solid #c3e6cb;
    }
    
    .alert-warning {
      background: #fff3cd;
      color: #856404;
      border: 1px solid #ffeaa7;
    }
    
    .alert-info {
      background: #d1ecf1;
      color: #0c5460;
      border: 1px solid #bee5eb;
    }
    
    .footer {
      text-align: center;
      padding: 20px;
      color: #666;
      border-top: 1px solid #eee;
      font-size: 0.9rem;
    }
    
    @media (max-width: 768px) {
      .content {
        grid-template-columns: 1fr;
        padding: 15px;
      }
      
      .header {
        padding: 20px;
      }
      
      .header h1 {
        font-size: 2rem;
      }
    }
  </style>
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0/css/all.min.css">
</head>
<body>
  <div class="container">
    <div class="header">
      <h1><i class="fas fa-satellite-dish"></i> Motion Sensor Control</h1>
      <p>Device IP: )rawliteral";
  html += inAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  html += R"rawliteral( | Sensor #)rawliteral";
  html += config.sensorNumber;
  html += R"rawliteral( | ESP ID: )rawliteral";
  html += String(ESP.getChipId(), HEX);
  html += R"rawliteral(</p>
    </div>
    
    <div class="content">
      <!-- Status Card -->
      <div class="card">
        <h3><i class="fas fa-chart-line"></i> Real-time Status</h3>
        
        <div class="motion-status )rawliteral";
  html += (currentStatus == "PRESENT" ? "status-present" : "status-empty");
  html += R"rawliteral(">
          <i class="fas )rawliteral";
  html += (currentStatus == "PRESENT" ? "fa-running" : "fa-check-circle");
  html += R"rawliteral("></i>
          )rawliteral";
  html += (currentStatus == "PRESENT" ? "MOTION DETECTED" : "NO MOTION");
  html += R"rawliteral(
        </div>
        
        <div class="status-grid">
          <div class="status-item">
            <div class="status-label">Device Status</div>
            <div class="status-value">)rawliteral";
  html += deviceStatus;
  html += R"rawliteral(</div>
          </div>
          
          <div class="status-item">
            <div class="status-label">WiFi RSSI</div>
            <div class="status-value">)rawliteral";
  html += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm" : "N/A";
  html += R"rawliteral(</div>
          </div>
          
          <div class="status-item">
            <div class="status-label">Buzzer</div>
            <div class="status-value">)rawliteral";
  html += buzzerActive ? "ACTIVE" : (config.buzzerEnabled ? "READY" : "DISABLED");
  html += R"rawliteral(</div>
          </div>
          
          <div class="status-item">
            <div class="status-label">Uptime</div>
            <div class="status-value">)rawliteral";
  html += String(millis() / 1000);
  html += R"rawliteral( s</div>
          </div>
        </div>
        
        <div class="btn-group">
          <button onclick="sendTestData()" class="btn btn-primary">
            <i class="fas fa-paper-plane"></i> Send Test Data
          </button>
          <button onclick="testBuzzer()" class="btn btn-warning">
            <i class="fas fa-bell"></i> Test Buzzer
          </button>
          <a href="/status" class="btn btn-secondary">
            <i class="fas fa-info-circle"></i> System Info
          </a>
        </div>
      </div>
      
      <!-- Connection Card -->
      <div class="card">
        <h3><i class="fas fa-wifi"></i> Network Configuration</h3>
        
        )rawliteral";
  
  if (inAPMode) {
    html += R"rawliteral(
        <div class="alert alert-warning">
          <i class="fas fa-exclamation-triangle"></i>
          <div>
            <strong>Access Point Mode</strong>
            <p>Connect to WiFi: MotionSensor_)rawliteral";
    html += String(ESP.getChipId(), HEX);
    html += R"rawliteral( (Password: config1234)</p>
          </div>
        </div>
    )rawliteral";
  }
  
  html += R"rawliteral(
        
        <div class="info-grid">
          <div class="info-item">
            <span class="info-label">WiFi SSID:</span>
            <span class="info-value">)rawliteral";
  html += strlen(config.ssid) > 0 ? config.ssid : "Not Configured";
  html += R"rawliteral(</span>
          </div>
          
          <div class="info-item">
            <span class="info-label">IP Address:</span>
            <span class="info-value">)rawliteral";
  html += inAPMode ? WiFi.softAPIP().toString() : 
          (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "N/A");
  html += R"rawliteral(</span>
          </div>
          
          <div class="info-item">
            <span class="info-label">API Endpoint:</span>
            <span class="info-value" style="font-size: 0.9rem;">)rawliteral";
  html += strlen(config.apiEndpoint) > 0 ? config.apiEndpoint : "Not Set";
  html += R"rawliteral(</span>
          </div>
          
          <div class="info-item">
            <span class="info-label">Last Update:</span>
            <span class="info-value">)rawliteral";
  html += getFormattedTime();
  html += R"rawliteral(</span>
          </div>
        </div>
        
        <div class="btn-group">
          <a href="/config" class="btn btn-primary">
            <i class="fas fa-cog"></i> Edit Configuration
          </a>
          <a href="/wifi-scan" class="btn btn-secondary">
            <i class="fas fa-search"></i> Scan WiFi
          </a>
          <form action="/reset" method="POST" style="display: inline;">
            <button type="submit" class="btn btn-danger" onclick="return confirm('Restart device?')">
              <i class="fas fa-redo"></i> Restart
            </button>
          </form>
        </div>
      </div>
      
      <!-- Quick Settings Card -->
      <div class="card">
        <h3><i class="fas fa-sliders-h"></i> Quick Settings</h3>
        
        <form action="/save" method="POST">
          <div class="form-group">
            <label class="form-label">Sensor Number:</label>
            <input type="number" name="sensorNumber" class="form-input" 
                   value=")rawliteral";
  html += String(config.sensorNumber);
  html += R"rawliteral(" min="1" max="100" required>
          </div>
          
          <div class="form-group">
            <label class="form-label">Detection Interval (seconds):</label>
            <input type="range" name="detectionInterval" class="form-input" 
                   min="5" max="300" step="5" value=")rawliteral";
  html += String(config.detectionInterval);
  html += R"rawliteral(" oninput="this.nextElementSibling.value = this.value + 's'">
            <output>)rawliteral";
  html += String(config.detectionInterval);
  html += R"rawliteral(s</output>
          </div>
          
          <div class="form-group">
            <div style="display: flex; justify-content: space-between; align-items: center;">
              <label class="form-label">Enable Buzzer:</label>
              <label class="toggle-switch">
                <input type="checkbox" name="buzzerEnabled" )rawliteral";
  html += config.buzzerEnabled ? "checked" : "";
  html += R"rawliteral(>
                <span class="toggle-slider"></span>
              </label>
            </div>
          </div>
          
          <div class="form-group">
            <label class="form-label">Buzzer Duration (seconds):</label>
            <input type="number" name="buzzerDuration" class="form-input" 
                   value=")rawliteral";
  html += String(config.buzzerDuration);
  html += R"rawliteral(" min="1" max="30" required>
          </div>
          
          <div class="btn-group">
            <button type="submit" class="btn btn-primary">
              <i class="fas fa-save"></i> Save Settings
            </button>
          </div>
        </form>
      </div>
      
      <!-- Actions Card -->
      <div class="card">
        <h3><i class="fas fa-bolt"></i> Quick Actions</h3>
        
        <div class="btn-group" style="flex-direction: column; gap: 10px;">
          <a href="/config" class="btn btn-secondary" style="justify-content: center;">
            <i class="fas fa-cogs"></i> Full Configuration
          </a>
          
          <button onclick="location.reload()" class="btn btn-secondary" style="justify-content: center;">
            <i class="fas fa-sync-alt"></i> Refresh Page
          </button>
          
          <button onclick="sendTestData()" class="btn btn-primary" style="justify-content: center;">
            <i class="fas fa-cloud-upload-alt"></i> Send Data Now
          </button>
          
          <button onclick="testBuzzer()" class="btn btn-warning" style="justify-content: center;">
            <i class="fas fa-bell"></i> Test Buzzer (1s)
          </button>
          
          <form action="/reset" method="POST" style="width: 100%;">
            <button type="submit" class="btn btn-danger" style="width: 100%; justify-content: center;"
                    onclick="return confirm('Restart the device?')">
              <i class="fas fa-power-off"></i> Restart Device
            </button>
          </form>
        </div>
      </div>
    </div>
    
    <div class="footer">
      <p>ESP8266 Motion Sensor | Firmware v)rawliteral";
  html += CONFIG_VERSION;
  html += R"rawliteral( | Free Heap: )rawliteral";
  html += String(ESP.getFreeHeap());
  html += R"rawliteral( bytes</p>
      <p>Auto-refresh in <span id="countdown">30</span> seconds</p>
    </div>
  </div>
  
  <script>
    function sendTestData() {
      fetch('/test-api')
        .then(response => response.text())
        .then(data => {
          alert('Data sent to API!\n' + data);
        })
        .catch(error => {
          alert('Error: ' + error);
        });
    }
    
    function testBuzzer() {
      fetch('/test-buzzer', { method: 'POST' })
        .then(response => response.text())
        .then(data => {
          alert('Buzzer tested: ' + data);
        });
    }
    
    // Auto-refresh countdown
    let seconds = 30;
    const countdownEl = document.getElementById('countdown');
    
    const countdown = setInterval(() => {
      seconds--;
      countdownEl.textContent = seconds;
      
      if (seconds <= 0) {
        clearInterval(countdown);
        location.reload();
      }
    }, 1000);
    
    // Update motion status with WebSocket-like polling
    function updateMotionStatus() {
      fetch('/status')
        .then(response => response.json())
        .then(data => {
          const statusEl = document.querySelector('.motion-status');
          if (data.motion_detected) {
            statusEl.innerHTML = '<i class="fas fa-running"></i> MOTION DETECTED';
            statusEl.className = 'motion-status status-present';
          } else {
            statusEl.innerHTML = '<i class="fas fa-check-circle"></i> NO MOTION';
            statusEl.className = 'motion-status status-empty';
          }
        });
    }
    
    // Update every 2 seconds
    setInterval(updateMotionStatus, 2000);
  </script>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
}

void handleConfig() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Device Configuration</title>
  <style>
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      padding: 20px;
    }
    
    .container {
      max-width: 800px;
      margin: 0 auto;
      background: white;
      border-radius: 20px;
      box-shadow: 0 20px 60px rgba(0,0,0,0.3);
      overflow: hidden;
    }
    
    .header {
      background: linear-gradient(135deg, #4CAF50, #2196F3);
      color: white;
      padding: 30px;
      text-align: center;
    }
    
    .header h1 {
      font-size: 2.5rem;
      margin-bottom: 10px;
    }
    
    .content {
      padding: 40px;
    }
    
    .form-section {
      margin-bottom: 40px;
      padding-bottom: 30px;
      border-bottom: 2px solid #eee;
    }
    
    .form-section h3 {
      color: #333;
      margin-bottom: 25px;
      padding-bottom: 15px;
      border-bottom: 3px solid #4CAF50;
      display: flex;
      align-items: center;
      gap: 10px;
    }
    
    .form-group {
      margin-bottom: 25px;
    }
    
    .form-label {
      display: block;
      margin-bottom: 10px;
      font-weight: 600;
      color: #555;
      font-size: 1.1rem;
    }
    
    .form-input {
      width: 100%;
      padding: 15px;
      border: 2px solid #ddd;
      border-radius: 10px;
      font-size: 1rem;
      transition: all 0.3s;
    }
    
    .form-input:focus {
      outline: none;
      border-color: #4CAF50;
      box-shadow: 0 0 0 3px rgba(76, 175, 80, 0.1);
      transform: translateY(-2px);
    }
    
    .form-select {
      width: 100%;
      padding: 15px;
      border: 2px solid #ddd;
      border-radius: 10px;
      font-size: 1rem;
      background: white;
    }
    
    .toggle-container {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 15px;
      background: #f8f9fa;
      border-radius: 10px;
    }
    
    .toggle-switch {
      position: relative;
      display: inline-block;
      width: 60px;
      height: 34px;
    }
    
    .toggle-switch input {
      opacity: 0;
      width: 0;
      height: 0;
    }
    
    .slider {
      position: absolute;
      cursor: pointer;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background-color: #ccc;
      transition: .4s;
      border-radius: 34px;
    }
    
    .slider:before {
      position: absolute;
      content: "";
      height: 26px;
      width: 26px;
      left: 4px;
      bottom: 4px;
      background-color: white;
      transition: .4s;
      border-radius: 50%;
    }
    
    input:checked + .slider {
      background-color: #4CAF50;
    }
    
    input:checked + .slider:before {
      transform: translateX(26px);
    }
    
    .btn-group {
      display: flex;
      gap: 15px;
      margin-top: 40px;
      flex-wrap: wrap;
    }
    
    .btn {
      padding: 15px 30px;
      border: none;
      border-radius: 50px;
      font-size: 1.1rem;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.3s;
      text-decoration: none;
      display: inline-flex;
      align-items: center;
      gap: 10px;
      flex: 1;
      min-width: 200px;
      justify-content: center;
    }
    
    .btn-primary {
      background: linear-gradient(135deg, #4CAF50, #45a049);
      color: white;
    }
    
    .btn-primary:hover {
      transform: translateY(-3px);
      box-shadow: 0 10px 20px rgba(76, 175, 80, 0.3);
    }
    
    .btn-secondary {
      background: linear-gradient(135deg, #6c757d, #5a6268);
      color: white;
    }
    
    .btn-secondary:hover {
      transform: translateY(-3px);
    }
    
    .btn-danger {
      background: linear-gradient(135deg, #f44336, #d32f2f);
      color: white;
    }
    
    .form-help {
      font-size: 0.9rem;
      color: #666;
      margin-top: 5px;
      font-style: italic;
    }
    
    @media (max-width: 768px) {
      .content {
        padding: 20px;
      }
      
      .btn {
        min-width: 100%;
      }
    }
  </style>
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0/css/all.min.css">
</head>
<body>
  <div class="container">
    <div class="header">
      <h1><i class="fas fa-cogs"></i> Device Configuration</h1>
      <p>Configure your motion sensor settings</p>
    </div>
    
    <div class="content">
      <form action="/save" method="POST">
        <!-- WiFi Settings -->
        <div class="form-section">
          <h3><i class="fas fa-wifi"></i> WiFi Settings</h3>
          
          <div class="form-group">
            <label class="form-label">WiFi SSID:</label>
            <input type="text" name="ssid" class="form-input" 
                   value=")rawliteral";
  html += config.ssid;
  html += R"rawliteral(" placeholder="Your WiFi network name" required>
            <div class="form-help">Leave empty to stay in AP mode</div>
          </div>
          
          <div class="form-group">
            <label class="form-label">WiFi Password:</label>
            <input type="password" name="password" class="form-input" 
                   value=")rawliteral";
  html += config.password;
  html += R"rawliteral(" placeholder="WiFi password">
          </div>
          
          <div class="form-group">
            <a href="/wifi-scan" class="btn btn-secondary" style="display: inline-block; width: auto;">
              <i class="fas fa-search"></i> Scan for WiFi Networks
            </a>
          </div>
        </div>
        
        <!-- API Settings -->
        <div class="form-section">
          <h3><i class="fas fa-cloud-upload-alt"></i> API Settings</h3>
          
          <div class="form-group">
            <label class="form-label">API Endpoint URL:</label>
            <input type="text" name="apiEndpoint" class="form-input" 
                   value=")rawliteral";
  html += config.apiEndpoint;
  html += R"rawliteral(" placeholder="http://your-server.com/api/data" required>
            <div class="form-help">Where to send motion data</div>
          </div>
          
          <div class="form-group">
            <label class="form-label">Sensor Number:</label>
            <input type="number" name="sensorNumber" class="form-input" 
                   value=")rawliteral";
  html += String(config.sensorNumber);
  html += R"rawliteral(" min="1" max="100" required>
            <div class="form-help">Unique ID for this sensor</div>
          </div>
        </div>
        
        <!-- Sensor Settings -->
        <div class="form-section">
          <h3><i class="fas fa-sensor"></i> Sensor Settings</h3>
          
          <div class="form-group">
            <label class="form-label">Detection Interval (seconds):</label>
            <input type="range" name="detectionInterval" class="form-input" 
                   min="5" max="300" step="5" value=")rawliteral";
  html += String(config.detectionInterval);
  html += R"rawliteral(" oninput="this.nextElementSibling.innerHTML = this.value + ' seconds'">
            <span>)rawliteral";
  html += String(config.detectionInterval);
  html += R"rawliteral( seconds</span>
            <div class="form-help">How often to send status updates</div>
          </div>
          
          <div class="form-group">
            <div class="toggle-container">
              <label class="form-label">Enable Buzzer:</label>
              <label class="toggle-switch">
                <input type="checkbox" name="buzzerEnabled" )rawliteral";
  html += config.buzzerEnabled ? "checked" : "";
  html += R"rawliteral(>
                <span class="slider"></span>
              </label>
            </div>
          </div>
          
          <div class="form-group">
            <label class="form-label">Buzzer Duration (seconds):</label>
            <input type="number" name="buzzerDuration" class="form-input" 
                   value=")rawliteral";
  html += String(config.buzzerDuration);
  html += R"rawliteral(" min="1" max="30" required>
            <div class="form-help">How long the buzzer sounds when motion detected</div>
          </div>
        </div>
        
        <!-- System Settings -->
        <div class="form-section">
          <h3><i class="fas fa-clock"></i> System Settings</h3>
          
          <div class="form-group">
            <label class="form-label">Timezone (GMT):</label>
            <select name="timezoneOffset" class="form-select">
              <option value="0" )rawliteral"; html += (config.timezoneOffset == 0 ? "selected" : ""); html += R"rawliteral(>GMT 0 (London)</option>
              <option value="5" )rawliteral"; html += (config.timezoneOffset == 5 ? "selected" : ""); html += R"rawliteral(>GMT+5 (Pakistan)</option>
              <option value="8" )rawliteral"; html += (config.timezoneOffset == 8 ? "selected" : ""); html += R"rawliteral(>GMT+8 (China, Singapore)</option>
              <option value="5.5" )rawliteral"; html += (config.timezoneOffset == 5.5 ? "selected" : ""); html += R"rawliteral(>GMT+5:30 (India)</option>
              <option value="-5" )rawliteral"; html += (config.timezoneOffset == -5 ? "selected" : ""); html += R"rawliteral(>GMT-5 (New York)</option>
              <option value="-8" )rawliteral"; html += (config.timezoneOffset == -8 ? "selected" : ""); html += R"rawliteral(>GMT-8 (California)</option>
            </select>
          </div>
        </div>
        
        <!-- Action Buttons -->
        <div class="btn-group">
          <a href="/" class="btn btn-secondary">
            <i class="fas fa-arrow-left"></i> Back to Dashboard
          </a>
          
          <button type="submit" class="btn btn-primary">
            <i class="fas fa-save"></i> Save All Settings
          </button>
          
          <button type="button" class="btn btn-danger" onclick="if(confirm('Reset to factory defaults?')) location.href='/reset'">
            <i class="fas fa-trash"></i> Factory Reset
          </button>
        </div>
      </form>
    </div>
  </div>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
}

void handleSave() {
  // Save all configuration
  if (server.hasArg("ssid")) strcpy(config.ssid, server.arg("ssid").c_str());
  if (server.hasArg("password")) strcpy(config.password, server.arg("password").c_str());
  if (server.hasArg("apiEndpoint")) strcpy(config.apiEndpoint, server.arg("apiEndpoint").c_str());
  if (server.hasArg("sensorNumber")) config.sensorNumber = server.arg("sensorNumber").toInt();
  if (server.hasArg("detectionInterval")) config.detectionInterval = server.arg("detectionInterval").toInt();
  if (server.hasArg("buzzerEnabled")) config.buzzerEnabled = (server.arg("buzzerEnabled") == "on");
  if (server.hasArg("buzzerDuration")) config.buzzerDuration = server.arg("buzzerDuration").toInt();
  if (server.hasArg("timezoneOffset")) config.timezoneOffset = server.arg("timezoneOffset").toFloat();
  
  saveConfig();
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta http-equiv="refresh" content="3;url=/">
  <style>
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 20px;
    }
    
    .message {
      background: white;
      padding: 50px;
      border-radius: 20px;
      box-shadow: 0 20px 60px rgba(0,0,0,0.3);
      text-align: center;
      max-width: 500px;
    }
    
    .success-icon {
      font-size: 80px;
      color: #4CAF50;
      margin-bottom: 20px;
    }
    
    h2 {
      color: #333;
      margin-bottom: 20px;
    }
    
    p {
      color: #666;
      margin-bottom: 30px;
      font-size: 1.1rem;
    }
    
    .countdown {
      font-size: 1.2rem;
      font-weight: bold;
      color: #4CAF50;
    }
  </style>
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0/css/all.min.css">
</head>
<body>
  <div class="message">
    <div class="success-icon">
      <i class="fas fa-check-circle"></i>
    </div>
    <h2>✅ Settings Saved Successfully!</h2>
    <p>Your configuration has been saved to the device.</p>
    <p>Device will restart and apply new settings.</p>
    <p class="countdown">Redirecting in 3 seconds...</p>
  </div>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
  
  delay(1000);
  ESP.restart();
}

void handleStatus() {
  StaticJsonDocument<1024> jsonDoc;
  
  jsonDoc["device_id"] = "motion_" + String(ESP.getChipId(), HEX);
  jsonDoc["sensor_number"] = config.sensorNumber;
  jsonDoc["firmware_version"] = CONFIG_VERSION;
  jsonDoc["ip_address"] = inAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  jsonDoc["mac_address"] = WiFi.macAddress();
  jsonDoc["wifi_ssid"] = config.ssid;
  jsonDoc["wifi_status"] = deviceStatus;
  jsonDoc["wifi_rssi"] = WiFi.RSSI();
  jsonDoc["motion_status"] = currentStatus;
  jsonDoc["motion_detected"] = motionDetected;
  jsonDoc["buzzer_active"] = buzzerActive;
  jsonDoc["buzzer_enabled"] = config.buzzerEnabled;
  jsonDoc["last_update"] = getFormattedTime();
  jsonDoc["uptime_ms"] = millis();
  jsonDoc["free_heap"] = ESP.getFreeHeap();
  jsonDoc["api_endpoint"] = config.apiEndpoint;
  jsonDoc["detection_interval"] = config.detectionInterval;
  jsonDoc["timezone"] = "GMT+" + String(config.timezoneOffset);
  
  String response;
  serializeJson(jsonDoc, response);
  
  server.send(200, "application/json", response);
}

void handleReset() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta http-equiv="refresh" content="3;url=/">
  <style>
    body { font-family: Arial; padding: 50px; text-align: center; }
    .message { background: #d4edda; color: #155724; padding: 20px; border-radius: 5px; }
  </style>
</head>
<body>
  <div class="message">
    <h2>🔄 Device Restarting...</h2>
    <p>Please wait while the device restarts.</p>
  </div>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
  
  delay(1000);
  ESP.restart();
}

void handleWiFiScan() {
  Serial.println("Scanning for WiFi networks...");
  
  int n = WiFi.scanNetworks();
  
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>WiFi Networks</title>";
  html += "<style>";
  html += "body { font-family: Arial; padding: 20px; }";
  html += "table { width: 100%; border-collapse: collapse; margin: 20px 0; }";
  html += "th, td { border: 1px solid #ddd; padding: 12px; text-align: left; }";
  html += "th { background-color: #4CAF50; color: white; }";
  html += "tr:hover { background-color: #f5f5f5; }";
  html += ".btn { background: #4CAF50; color: white; padding: 10px 20px; text-decoration: none; border-radius: 5px; }";
  html += "</style>";
  html += "</head><body>";
  html += "<h2>Available WiFi Networks</h2>";
  
  if (n == 0) {
    html += "<p>No WiFi networks found.</p>";
  } else {
    html += "<table>";
    html += "<tr><th>SSID</th><th>Signal</th><th>Encryption</th><th>Action</th></tr>";
    
    for (int i = 0; i < n; ++i) {
      html += "<tr>";
      html += "<td>" + WiFi.SSID(i) + "</td>";
      html += "<td>" + String(WiFi.RSSI(i)) + " dBm</td>";
      html += "<td>" + String((WiFi.encryptionType(i) == ENC_TYPE_NONE) ? "Open" : "Secured") + "</td>";
      html += "<td><a href='/config' class='btn'>Use This</a></td>";
      html += "</tr>";
    }
    
    html += "</table>";
  }
  
  html += "<br><a href='/config' class='btn'>← Back to Config</a>";
  html += "<br><br><form action='/connect' method='POST'>";
  html += "<h3>Manual WiFi Setup</h3>";
  html += "<input type='text' name='ssid' placeholder='SSID' required>";
  html += "<input type='password' name='password' placeholder='Password' required>";
  html += "<button type='submit' class='btn'>Connect</button>";
  html += "</form>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}
#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// Pin Definitions
#define PIR_PIN 14
#define BUZZER_PIN 12
#define LED_PIN 2

// Configuration Structure
struct Config {
  char ssid[32];
  char password[32];
  char apiEndpoint[128];
  int sensorNumber;
};

Config config;

// Global Objects
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient *timeClient = nullptr;

// State Variables
bool motionDetected = false;
bool inAPMode = false;

// EEPROM Settings
const int EEPROM_SIZE = 512;
const int CONFIG_ADDRESS = 0;

// Function Prototypes
void setupPins();
void loadConfig();
void saveConfig();
void connectToWiFi();
void startAPMode();
void setupWebServer();
void handleRoot();
void handleSave();
void sendSensorData();
String getFormattedTime();
void handleMotion();

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Motion Sensor Starting...");

  setupPins();
  EEPROM.begin(EEPROM_SIZE);
  loadConfig();
  setupWebServer();

  if (strlen(config.ssid) > 0) {
    connectToWiFi();
  } else {
    Serial.println("No WiFi config. Starting AP mode...");
    startAPMode();
  }

  Serial.println("System Ready");
  Serial.print("Web Interface: http://");
  Serial.println(inAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
}

// ========== Main Loop ==========
void loop() {
  server.handleClient();

  if (!inAPMode && WiFi.status() == WL_CONNECTED) {
    if (timeClient) timeClient->update();
    handleMotion();
  }

  delay(50);
}

// ========== Pin Setup ==========
void setupPins() {
  pinMode(PIR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, HIGH); // LED OFF (HIGH = no motion)
}

// ========== WiFi Connection ==========
void connectToWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(config.ssid);

  WiFi.begin(config.ssid, config.password);
  WiFi.setAutoReconnect(true);

  int attempts = 0;
  bool ledState = false;

  // Try to connect 20 times (10 seconds)
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? LOW : HIGH);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    inAPMode = false;
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    if (!timeClient) {
      timeClient = new NTPClient(
        ntpUDP,
        "pool.ntp.org",
        6 * 3600, // UTC+6 for Bangladesh
        60000
      );
      timeClient->begin();
    }
    timeClient->update();
    digitalWrite(LED_PIN, HIGH); // LED OFF
  } else {
    Serial.println("\nWiFi Connection Failed");
    digitalWrite(LED_PIN, HIGH); // LED OFF
    startAPMode();
  }
}

// ========== Access Point Mode ==========
void startAPMode() {
  inAPMode = true;
  String apName = "Sensor_" + String(ESP.getChipId(), HEX);
  WiFi.softAP(apName.c_str(), "12345678");

  Serial.println("AP Mode Started");
  Serial.print("SSID: ");
  Serial.println(apName);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP().toString());
}

// ========== Config Load/Save ==========
void loadConfig() {
  EEPROM.get(CONFIG_ADDRESS, config);

  // Check if SSID looks valid (basic validation)
  if (strlen(config.ssid) == 0 || strlen(config.ssid) > 31) {
    Serial.println("EEPROM empty or invalid, loading defaults");

    strcpy(config.ssid, "");
    strcpy(config.password, "");
    strcpy(config.apiEndpoint, "https://zms.zisprink.com/api/motion-detection");
    config.sensorNumber = 1;

    saveConfig();
  } else {
    Serial.println("Config loaded from EEPROM");
  }
}


void saveConfig() {
  EEPROM.put(CONFIG_ADDRESS, config);
  EEPROM.commit();
  Serial.println("Config saved to EEPROM");
}


// ========== Motion Detection ==========
void handleMotion() {
  static bool lastMotionState = false;
  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 200; // ms

  bool currentReading = (digitalRead(PIR_PIN) == HIGH);

  if (currentReading != lastMotionState) lastDebounceTime = millis();

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (currentReading != motionDetected) {
      motionDetected = currentReading;

      if (motionDetected) {
        digitalWrite(LED_PIN, LOW);
        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("🚨 Motion DETECTED (START)");
      } else {
        digitalWrite(LED_PIN, HIGH);
        Serial.println("✅ Motion CLEAR (STOP)");
      }

      sendSensorData();
    }
  }

  lastMotionState = currentReading;
}

// ========== Send Data ==========
void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED || strlen(config.apiEndpoint) == 0) return;

  String timestamp = getFormattedTime();

  StaticJsonDocument<200> jsonDoc;
  jsonDoc["device_id"] = "sensor_" + String(ESP.getChipId(), HEX);
  jsonDoc["sensor_number"] = config.sensorNumber;
  jsonDoc["timestamp"] = timestamp;
  jsonDoc["motion_detected"] = motionDetected;

  String payload;
  serializeJson(jsonDoc, payload);

  WiFiClient client;
  HTTPClient http;

  http.begin(client, config.apiEndpoint );
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.print("Data sent: ");
    Serial.println(httpCode);
  }

  http.end();
}

// ========== Get Time ==========
String getFormattedTime() {
  if (!timeClient || WiFi.status() != WL_CONNECTED) return "N/A";

  timeClient->update();
  unsigned long epochTime = timeClient->getEpochTime();
  struct tm *ptm = gmtime((time_t *)&epochTime);

  char timeString[20];
  sprintf(timeString, "%04d-%02d-%02d %02d:%02d:%02d",
          ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
          (ptm->tm_hour + 6) % 24, ptm->tm_min, ptm->tm_sec);

  return String(timeString);
}

// ========== Web Server ==========
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Web server started");
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Sensor Configuration</title>
<style>
body { font-family: Arial; background:#f0f0f0; margin:0; padding:20px; display:flex; justify-content:center; align-items:center; min-height:100vh; }
.container { background:white; border-radius:10px; box-shadow:0 2px 10px rgba(0,0,0,0.1); padding:30px; max-width:400px; width:100%; }
h1 { color:#333; text-align:center; margin-bottom:30px; }
.form-group { margin-bottom:20px; }
label { display:block; margin-bottom:5px; color:#555; font-weight:bold; }
input[type=text], input[type=password], input[type=number] { width:100%; padding:10px; border:1px solid #ddd; border-radius:5px; font-size:16px; }
button { width:100%; padding:12px; background:#4CAF50; color:white; border:none; border-radius:5px; font-size:16px; cursor:pointer; margin-top:10px; }
button:hover { background:#45a049; }
.info { background:#f8f9fa; border-radius:5px; padding:15px; margin-top:20px; font-size:14px; color:#666; }
.info strong { color:#333; }
</style>
</head>
<body>
<div class="container">
<h1>Sensor Configuration</h1>
<form action="/save" method="POST">
<div class="form-group">
<label for="ssid">WiFi SSID:</label>
<input type="text" id="ssid" name="ssid" value=")rawliteral";
  html += config.ssid;
  html += R"rawliteral(" placeholder='Enter WiFi name' required>
</div>
<div class="form-group">
<label for='password'>WiFi Password:</label>
<input type='password' id='password' name='password' value=")rawliteral";
  html += config.password;
  html += R"rawliteral(" placeholder='Enter WiFi password'>
</div>
<div class='form-group'>
<label for='apiEndpoint'>API Endpoint:</label>
<input type='text' id='apiEndpoint' name='apiEndpoint' value=")rawliteral";
  html += config.apiEndpoint;
  html += R"rawliteral(" placeholder='http://server.com/api' required>
</div>
<div class='form-group'>
<label for='sensorNumber'>Sensor Number:</label>
<input type='number' id='sensorNumber' name='sensorNumber' value=")rawliteral";
  html += String(config.sensorNumber);
  html += R"rawliteral(" min='1' required>
</div>
<button type='submit'>Save Configuration</button>
</form>
<div class='info'>
<strong>Device Info:</strong><br>
ID: )rawliteral";
  html += String(ESP.getChipId(), HEX);
  html += R"rawliteral(<br>
IP: )rawliteral";
  html += inAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  html += R"rawliteral(<br>
Mode: )rawliteral";
  html += inAPMode ? "Access Point" : "WiFi Client";
  html += R"rawliteral(</div></div></body></html>)rawliteral";

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid")) strcpy(config.ssid, server.arg("ssid").c_str());
  if (server.hasArg("password")) strcpy(config.password, server.arg("password").c_str());
  if (server.hasArg("apiEndpoint")) strcpy(config.apiEndpoint, server.arg("apiEndpoint").c_str());
  if (server.hasArg("sensorNumber")) config.sensorNumber = server.arg("sensorNumber").toInt();

  saveConfig();

  server.send(200, "text/html",
    "<html><body><h2>✓ Configuration Saved</h2><p>Device will restart...</p></body></html>");

  delay(2000);
  ESP.restart();
}

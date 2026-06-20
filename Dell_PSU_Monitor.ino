#include <Wire.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <U8g2lib.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>

// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// ── OTA settings ─────────────────────────────────────────────────────────────
const char* otaHostname = "dell-psu-monitor";
const char* otaPassword = "psu1234";   // Change before deploying

// ── MQTT / Home Assistant settings ───────────────────────────────────────────
const char* mqttServer   = ""; // Change to your broker IP
const int   mqttPort     = 1883;
const char* mqttUser     = "";              // Leave empty if no auth
const char* mqttPass     = "";
const char* mqttClientId = "dell_psu_monitor";

#define MQTT_STATE_TOPIC        "dell_psu/state"
#define MQTT_AVAILABILITY_TOPIC "dell_psu/availability"
#define MQTT_COMMAND_TOPIC      "dell_psu/command"
#define HA_DISCOVERY_PREFIX     "homeassistant"

// I2C pins
#define SDA_PIN 21
#define SCL_PIN 22

// PSU control pins
#define PS_ON_PIN 23
#define PS_PRESENT_PIN 5

// Physical button pin (directly connect button between this pin and GND)
#define BUTTON_PIN 13

// PSU status input pins
#define AC_OK_PIN 19
#define PWR_GOOD_PIN 18

// PMBus address
#define PSU_ADDR 0x58

// System draw offset (ESP32 + step-down converter consumption)
// Adjust these values to match your actual system draw
const float SYSTEM_CURRENT_DRAW = 0;  // Amps - your ESP32 + stepdown draws ~0.9A
const float SYSTEM_POWER_DRAW = 0;    // Watts - your ESP32 + stepdown draws ~7W

// OLED address (typically 0x3C or 0x3D)
#define OLED_ADDR 0x3C

// PMBus commands
#define PMBUS_VOUT_MODE 0x20
#define PMBUS_STATUS_WORD 0x79
#define PMBUS_READ_VIN 0x88
#define PMBUS_READ_IIN 0x89
#define PMBUS_READ_VCAP 0x8A
#define PMBUS_READ_VOUT 0x8B
#define PMBUS_READ_IOUT 0x8C
#define PMBUS_READ_TEMP1 0x8D
#define PMBUS_READ_TEMP2 0x8E
#define PMBUS_READ_TEMP3 0x8F
#define PMBUS_READ_FAN1 0x90
#define PMBUS_READ_POUT 0x96
#define PMBUS_READ_PIN 0x97
#define PMBUS_MFR_MODEL 0x9A
#define PMBUS_MFR_SERIAL 0x9E

// OLED Display (SH1106 128x64 I2C)
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

// Web server
AsyncWebServer server(80);
AsyncEventSource events("/events");

// MQTT
WiFiClient    wifiClient;
PubSubClient  mqttClient(wifiClient);
unsigned long lastMqttReconnect = 0;
bool          haDiscoveryPublished = false;

// Global variables
float vIn = 0, iIn = 0, pIn = 0, vCap = 0;
float vOut = 0, iOut = 0, pOut = 0;
float iLoad = 0, pLoad = 0;  // Load current/power (output minus system draw)
float temp1 = 0, temp2 = 0, temp3 = 0;
int fan1 = 0;
int8_t voutExponent = -9;

bool acOkState = false;
bool pwrGoodState = false;
String psuStatus = "INIT";
int statusCode = 0;

String psuModel = "";
String psuSerial = "";

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 1000;

// PSU power control
bool psuEnabled = false;  // Tracks if PSU output is enabled

// Button debounce
unsigned long lastButtonPress = 0;
const unsigned long DEBOUNCE_DELAY = 250;
bool lastButtonState = HIGH;


// HTML page
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Dell PSU Monitor</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: 'Segoe UI', system-ui, sans-serif;
      background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
      color: #fff;
      min-height: 100vh;
      padding: 20px;
    }
    .header {
      text-align: center;
      padding: 20px 0 30px;
    }
    .header h1 {
      font-size: 28px;
      font-weight: 300;
      color: #fff;
      margin-bottom: 5px;
    }
    .header .model {
      color: #888;
      font-size: 14px;
    }
    .status-bar {
      display: flex;
      justify-content: center;
      gap: 20px;
      margin-bottom: 30px;
      flex-wrap: wrap;
    }
    .status-badge {
      padding: 10px 25px;
      border-radius: 25px;
      font-weight: 600;
      font-size: 14px;
      text-transform: uppercase;
      letter-spacing: 1px;
    }
    .status-ok { background: #00c853; color: #000; }
    .status-warn { background: #ffc107; color: #000; }
    .status-err { background: #ff5252; color: #fff; }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
      gap: 20px;
      max-width: 1400px;
      margin: 0 auto;
    }
    .card {
      background: rgba(255,255,255,0.05);
      border-radius: 16px;
      padding: 20px;
      backdrop-filter: blur(10px);
      border: 1px solid rgba(255,255,255,0.1);
    }
    .card-title {
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: 2px;
      color: #888;
      margin-bottom: 15px;
    }
    .card-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 8px 0;
      border-bottom: 1px solid rgba(255,255,255,0.05);
    }
    .card-row:last-child { border-bottom: none; }
    .card-label { color: #aaa; font-size: 14px; }
    .card-value {
      font-size: 20px;
      font-weight: 600;
      font-family: 'Consolas', monospace;
    }
    .card-unit { font-size: 12px; color: #888; margin-left: 3px; }
    .big-value {
      font-size: 48px;
      font-weight: 300;
      text-align: center;
      padding: 20px 0;
    }
    .big-value .unit { font-size: 20px; color: #888; }
    .temp-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 10px;
      text-align: center;
    }
    .temp-item .value {
      font-size: 28px;
      font-weight: 300;
      font-family: 'Consolas', monospace;
    }
    .temp-item .label {
      font-size: 11px;
      color: #888;
      margin-top: 5px;
    }
    .signal-grid {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: 15px;
    }
    .signal-item {
      text-align: center;
      padding: 15px;
      border-radius: 10px;
      background: rgba(0,0,0,0.2);
    }
    .signal-item .label {
      font-size: 11px;
      color: #888;
      margin-bottom: 8px;
    }
    .signal-item .value {
      font-size: 16px;
      font-weight: 600;
    }
    .signal-ok { color: #00c853; }
    .signal-err { color: #ff5252; }
    .power-btn {
      width: 80px;
      height: 80px;
      border-radius: 50%;
      border: 4px solid #333;
      background: linear-gradient(145deg, #1a1a1a, #2d2d2d);
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      margin: 0 auto 20px;
      transition: all 0.3s ease;
      box-shadow: 0 4px 15px rgba(0,0,0,0.3);
    }
    .power-btn:hover { transform: scale(1.05); }
    .power-btn:active { transform: scale(0.95); }
    .power-btn.on {
      border-color: #00c853;
      box-shadow: 0 0 20px rgba(0,200,83,0.5);
    }
    .power-btn.off {
      border-color: #ff5252;
      box-shadow: 0 0 20px rgba(255,82,82,0.3);
    }
    .power-icon {
      width: 36px;
      height: 36px;
      stroke: #666;
      stroke-width: 3;
      fill: none;
    }
    .power-btn.on .power-icon { stroke: #00c853; }
    .power-btn.off .power-icon { stroke: #ff5252; }
    .power-label {
      text-align: center;
      font-size: 12px;
      color: #888;
      margin-bottom: 20px;
      text-transform: uppercase;
      letter-spacing: 2px;
    }
    @media (max-width: 600px) {
      .big-value { font-size: 36px; }
      .card { padding: 15px; }
    }
  </style>
</head>
<body>
  <div class="header">
    <h1>Dell PSU Monitor</h1>
    <div class="model"><span id="model">--</span> | S/N: <span id="serial">--</span></div>
  </div>

  <div class="status-bar">
    <div id="psu-status" class="status-badge status-warn">Connecting...</div>
    <div id="ac-status" class="status-badge status-warn">AC: --</div>
    <div id="pwr-status" class="status-badge status-warn">PWR: --</div>
  </div>

  <button id="power-btn" class="power-btn off" onclick="togglePower()">
    <svg class="power-icon" viewBox="0 0 24 24">
      <path d="M12 3v9"/>
      <path d="M18.4 6.6a9 9 0 1 1-12.8 0"/>
    </svg>
  </button>
  <div id="power-label" class="power-label">PSU OFF</div>

  <div class="grid">
    <div class="card">
      <div class="card-title">Load Power</div>
      <div class="big-value"><span id="pload">--</span><span class="unit">W</span></div>
    </div>

    <div class="card">
      <div class="card-title">Load (External)</div>
      <div class="card-row">
        <span class="card-label">Voltage</span>
        <span class="card-value"><span id="vout">--</span><span class="card-unit">V</span></span>
      </div>
      <div class="card-row">
        <span class="card-label">Current</span>
        <span class="card-value"><span id="iload">--</span><span class="card-unit">A</span></span>
      </div>
      <div class="card-row">
        <span class="card-label">Power</span>
        <span class="card-value"><span id="pload2">--</span><span class="card-unit">W</span></span>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Total Output (incl. system)</div>
      <div class="card-row">
        <span class="card-label">Current</span>
        <span class="card-value"><span id="iout">--</span><span class="card-unit">A</span></span>
      </div>
      <div class="card-row">
        <span class="card-label">Power</span>
        <span class="card-value"><span id="pout">--</span><span class="card-unit">W</span></span>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Input</div>
      <div class="card-row">
        <span class="card-label">Voltage</span>
        <span class="card-value"><span id="vin">--</span><span class="card-unit">V</span></span>
      </div>
      <div class="card-row">
        <span class="card-label">Current</span>
        <span class="card-value"><span id="iin">--</span><span class="card-unit">A</span></span>
      </div>
      <div class="card-row">
        <span class="card-label">Power</span>
        <span class="card-value"><span id="pin">--</span><span class="card-unit">W</span></span>
      </div>
      <div class="card-row">
        <span class="card-label">PFC Cap</span>
        <span class="card-value"><span id="vcap">--</span><span class="card-unit">V</span></span>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Temperatures</div>
      <div class="temp-grid">
        <div class="temp-item">
          <div class="value"><span id="temp1">--</span>&deg;</div>
          <div class="label">PRIMARY</div>
        </div>
        <div class="temp-item">
          <div class="value"><span id="temp2">--</span>&deg;</div>
          <div class="label">SECONDARY</div>
        </div>
        <div class="temp-item">
          <div class="value"><span id="temp3">--</span>&deg;</div>
          <div class="label">AMBIENT</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Fan Speed</div>
      <div class="big-value"><span id="fan">--</span><span class="unit">RPM</span></div>
    </div>

    <div class="card">
      <div class="card-title">Signals</div>
      <div class="signal-grid">
        <div class="signal-item">
          <div class="label">AC OK</div>
          <div class="value" id="ac-sig">--</div>
        </div>
        <div class="signal-item">
          <div class="label">PWR GOOD</div>
          <div class="value" id="pwr-sig">--</div>
        </div>
      </div>
    </div>
  </div>

  <script>
    const evtSource = new EventSource('/events');

    evtSource.onmessage = function(e) {
      const d = JSON.parse(e.data);

      document.getElementById('vin').textContent = d.vin.toFixed(0);
      document.getElementById('iin').textContent = d.iin.toFixed(2);
      document.getElementById('pin').textContent = d.pin.toFixed(0);
      document.getElementById('vcap').textContent = d.vcap.toFixed(0);
      document.getElementById('vout').textContent = d.vout.toFixed(2);
      document.getElementById('iout').textContent = d.iout.toFixed(1);
      document.getElementById('pout').textContent = d.pout.toFixed(0);
      document.getElementById('iload').textContent = d.iload.toFixed(1);
      document.getElementById('pload').textContent = d.pload.toFixed(0);
      document.getElementById('pload2').textContent = d.pload.toFixed(0);
      document.getElementById('temp1').textContent = d.t1.toFixed(0);
      document.getElementById('temp2').textContent = d.t2.toFixed(0);
      document.getElementById('temp3').textContent = d.t3.toFixed(0);
      document.getElementById('fan').textContent = d.fan;
      document.getElementById('model').textContent = d.model;
      document.getElementById('serial').textContent = d.serial;

      const psuStat = document.getElementById('psu-status');
      psuStat.textContent = d.status;
      psuStat.className = 'status-badge ' + (d.sc === 1 ? 'status-ok' : d.sc === 2 ? 'status-err' : 'status-warn');

      const acStat = document.getElementById('ac-status');
      const acSig = document.getElementById('ac-sig');
      if (d.ac) {
        acStat.textContent = 'AC: OK';
        acStat.className = 'status-badge status-ok';
        acSig.textContent = 'OK';
        acSig.className = 'value signal-ok';
      } else {
        acStat.textContent = 'AC: FAIL';
        acStat.className = 'status-badge status-err';
        acSig.textContent = 'FAIL';
        acSig.className = 'value signal-err';
      }

      const pwrStat = document.getElementById('pwr-status');
      const pwrSig = document.getElementById('pwr-sig');
      if (d.pwr) {
        pwrStat.textContent = 'PWR: GOOD';
        pwrStat.className = 'status-badge status-ok';
        pwrSig.textContent = 'GOOD';
        pwrSig.className = 'value signal-ok';
      } else {
        pwrStat.textContent = 'PWR: BAD';
        pwrStat.className = 'status-badge status-err';
        pwrSig.textContent = 'BAD';
        pwrSig.className = 'value signal-err';
      }

      // Update power button state
      const powerBtn = document.getElementById('power-btn');
      const powerLabel = document.getElementById('power-label');
      if (d.psuon) {
        powerBtn.className = 'power-btn on';
        powerLabel.textContent = 'PSU ON';
      } else {
        powerBtn.className = 'power-btn off';
        powerLabel.textContent = 'PSU OFF';
      }
    };

    evtSource.onerror = function() {
      document.getElementById('psu-status').textContent = 'Disconnected';
      document.getElementById('psu-status').className = 'status-badge status-err';
    };

    function togglePower() {
      fetch('/toggle').then(r => r.text()).then(t => console.log('PSU:', t));
    }
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n========================================");
  Serial.println("    Dell PSU Monitor - Custom UI");
  Serial.println("========================================\n");

  // Setup control output pins
  pinMode(PS_ON_PIN, OUTPUT);
  pinMode(PS_PRESENT_PIN, OUTPUT);
  digitalWrite(PS_PRESENT_PIN, LOW);
  digitalWrite(PS_ON_PIN, LOW);  // LOW = transistor off = PSU off at startup

  // Setup physical button (connect between BUTTON_PIN and GND)
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Setup status input pins
  pinMode(AC_OK_PIN, INPUT_PULLDOWN);
  pinMode(PWR_GOOD_PIN, INPUT_PULLDOWN);

  // Initialize I2C (using external pull-ups)
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);
  Serial.println("[I2C] Initialized at 50kHz");

  // Initialize OLED
  display.begin();
  display.setFont(u8g2_font_6x10_tf);
  display.clearBuffer();
  display.drawStr(20, 30, "Dell PSU Monitor");
  display.drawStr(35, 45, "Starting...");
  display.sendBuffer();
  Serial.println("[OLED] Initialized");

  delay(1000);

  // Read device info
  Serial.println("[PSU] Reading device info...");
  psuModel = readString(PMBUS_MFR_MODEL);
  psuSerial = readString(PMBUS_MFR_SERIAL);
  Serial.printf("  Model:  %s\n", psuModel.c_str());
  Serial.printf("  Serial: %s\n", psuSerial.c_str());

  // Read VOUT_MODE
  uint8_t voutMode = readByte(PMBUS_VOUT_MODE);
  voutExponent = voutMode & 0x1F;
  if (voutExponent > 15) voutExponent -= 32;

  // Connect to WiFi
  Serial.println("\n[WiFi] Connecting...");

  display.clearBuffer();
  display.drawStr(20, 30, "Connecting WiFi");
  display.sendBuffer();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());

    display.clearBuffer();
    display.drawStr(10, 25, "WiFi Connected!");
    display.drawStr(10, 45, WiFi.localIP().toString().c_str());
    display.sendBuffer();
    delay(2000);
  } else {
    Serial.println("\n[WiFi] Failed! Starting AP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Dell_PSU_Monitor", "12345678");
    Serial.print("[WiFi] AP IP: ");
    Serial.println(WiFi.softAPIP());

    display.clearBuffer();
    display.drawStr(10, 25, "AP Mode Active");
    display.drawStr(10, 45, "192.168.4.1");
    display.sendBuffer();
    delay(2000);
  }

  // Setup web server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  // Toggle PSU power endpoint
  server.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
    psuEnabled = !psuEnabled;
    digitalWrite(PS_ON_PIN, psuEnabled ? HIGH : LOW);  // HIGH = transistor on = PSU on
    Serial.printf("[PSU] Power %s\n", psuEnabled ? "ON" : "OFF");
    request->send(200, "text/plain", psuEnabled ? "ON" : "OFF");
  });

  events.onConnect([](AsyncEventSourceClient *client) {
    Serial.println("[WEB] Client connected");
  });
  server.addHandler(&events);

  server.begin();
  Serial.println("[WEB] Server started");

  // ── OTA setup ────────────────────────────────────────────────────────────
  ArduinoOTA.setHostname(otaHostname);
  ArduinoOTA.setPassword(otaPassword);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.println("[OTA] Start: " + type);
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(20, 28, "OTA Update");
    display.drawStr(20, 44, "Starting...");
    display.sendBuffer();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    uint8_t pct = progress / (total / 100);
    Serial.printf("[OTA] %u%%\r", pct);
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(20, 22, "OTA Update");
    char buf[20];
    snprintf(buf, sizeof(buf), "Progress: %u%%", pct);
    display.drawStr(10, 38, buf);
    // Progress bar
    display.drawFrame(4, 48, 120, 10);
    display.drawBox(4, 48, (uint8_t)(120 * pct / 100), 10);
    display.sendBuffer();
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Complete");
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(20, 35, "OTA Complete!");
    display.sendBuffer();
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    const char* msg = "Unknown error";
    if      (error == OTA_AUTH_ERROR)    msg = "Auth failed";
    else if (error == OTA_BEGIN_ERROR)   msg = "Begin failed";
    else if (error == OTA_CONNECT_ERROR) msg = "Connect failed";
    else if (error == OTA_RECEIVE_ERROR) msg = "Receive failed";
    else if (error == OTA_END_ERROR)     msg = "End failed";
    Serial.println(msg);
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(10, 28, "OTA Error:");
    display.drawStr(10, 44, msg);
    display.sendBuffer();
  });

  ArduinoOTA.begin();
  Serial.println("[OTA] Ready — hostname: " + String(otaHostname));

  // ── MQTT setup ───────────────────────────────────────────────────────────
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setBufferSize(512);
  mqttClient.setCallback(mqttCallback);
  if (WiFi.status() == WL_CONNECTED) {
    mqttConnect();
  }

  Serial.println("========================================\n");
}

void loop() {
  ArduinoOTA.handle();

  // MQTT: reconnect if needed (non-blocking, try every 5 s)
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      unsigned long now = millis();
      if (now - lastMqttReconnect > 5000) {
        lastMqttReconnect = now;
        mqttConnect();
      }
    } else {
      mqttClient.loop();
    }
  }

  // Handle physical button press (active LOW with internal pullup)
  bool buttonState = digitalRead(BUTTON_PIN);
  if (buttonState == LOW && lastButtonState == HIGH) {
    if (millis() - lastButtonPress > DEBOUNCE_DELAY) {
      lastButtonPress = millis();
      psuEnabled = !psuEnabled;
      digitalWrite(PS_ON_PIN, psuEnabled ? HIGH : LOW);  // HIGH = transistor on = PSU on
      Serial.printf("[BUTTON] PSU Power %s\n", psuEnabled ? "ON" : "OFF");
    }
  }
  lastButtonState = buttonState;

  if (millis() - lastUpdate >= UPDATE_INTERVAL) {
    lastUpdate = millis();

    // Read hardware signals
    acOkState = digitalRead(AC_OK_PIN);
    pwrGoodState = digitalRead(PWR_GOOD_PIN);

    float tempVal;

    // Read input measurements
    tempVal = readLinear11(PMBUS_READ_VIN);
    if (tempVal > 0) vIn = tempVal;

    tempVal = readLinear11(PMBUS_READ_IIN);
    if (tempVal >= 0) iIn = tempVal;

    tempVal = readLinear11(PMBUS_READ_PIN);
    if (tempVal >= 0) pIn = tempVal;

    tempVal = readLinear11(PMBUS_READ_VCAP);
    if (tempVal > 0) vCap = tempVal;

    // Read output measurements
    // Always update output values - allow 0 when PSU is off
    tempVal = readLinear16(PMBUS_READ_VOUT);
    vOut = (tempVal >= 0 && tempVal < 100) ? tempVal : 0;  // Valid range check

    tempVal = readLinear11(PMBUS_READ_IOUT);
    iOut = (tempVal >= 0 && tempVal < 200) ? tempVal : 0;

    tempVal = readLinear11(PMBUS_READ_POUT);
    pOut = (tempVal >= 0 && tempVal < 5000) ? tempVal : 0;

    // If PWR_GOOD is low, force current/power to 0 (but keep voltage visible)
    if (!pwrGoodState) {
      iOut = 0;
      pOut = 0;
    }

    // Calculate load current/power (subtract system draw)
    iLoad = max(0.0f, iOut - SYSTEM_CURRENT_DRAW);
    pLoad = max(0.0f, pOut - SYSTEM_POWER_DRAW);

    // Read temperatures
    tempVal = readLinear11(PMBUS_READ_TEMP1);
    if (tempVal > -40 && tempVal < 150) temp1 = tempVal;

    tempVal = readLinear11(PMBUS_READ_TEMP2);
    if (tempVal > -40 && tempVal < 150) temp2 = tempVal;

    tempVal = readLinear11(PMBUS_READ_TEMP3);
    if (tempVal > -40 && tempVal < 150) temp3 = tempVal;

    // Read fan - allow 0 when PSU/fan is off
    int tempFan = (int)readLinear11(PMBUS_READ_FAN1);
    fan1 = (tempFan >= 0 && tempFan < 30000) ? tempFan : 0;

    // If output is off, fan is likely off too
    if (!pwrGoodState) {
      fan1 = 0;
    }

    // Update status
    uint16_t status = readWord(PMBUS_STATUS_WORD);
    updateStatus(status);

    // Update OLED display
    updateOLED();

    // Send JSON to web clients
    String json = "{";
    json += "\"vin\":" + String(vIn, 1) + ",";
    json += "\"iin\":" + String(iIn, 2) + ",";
    json += "\"pin\":" + String(pIn, 0) + ",";
    json += "\"vcap\":" + String(vCap, 0) + ",";
    json += "\"vout\":" + String(vOut, 2) + ",";
    json += "\"iout\":" + String(iOut, 1) + ",";
    json += "\"pout\":" + String(pOut, 0) + ",";
    json += "\"iload\":" + String(iLoad, 1) + ",";
    json += "\"pload\":" + String(pLoad, 0) + ",";
    json += "\"t1\":" + String(temp1, 0) + ",";
    json += "\"t2\":" + String(temp2, 0) + ",";
    json += "\"t3\":" + String(temp3, 0) + ",";
    json += "\"fan\":" + String(fan1) + ",";
    json += "\"ac\":" + String(acOkState ? "true" : "false") + ",";
    json += "\"pwr\":" + String(pwrGoodState ? "true" : "false") + ",";
    json += "\"status\":\"" + psuStatus + "\",";
    json += "\"sc\":" + String(statusCode) + ",";
    json += "\"model\":\"" + psuModel + "\",";
    json += "\"serial\":\"" + psuSerial + "\",";
    json += "\"psuon\":" + String(psuEnabled ? "true" : "false");
    json += "}";

    events.send(json.c_str(), "message", millis());

    // Publish to MQTT / Home Assistant
    if (mqttClient.connected()) {
      publishMQTTState();
    }

    // Serial output
    Serial.printf("IN: %.0fV %.2fA %.0fW | LOAD: %.1fA %.0fW | TOTAL: %.1fA %.0fW | T: %.0f/%.0f/%.0fC | FAN: %dRPM\n",
                  vIn, iIn, pIn, iLoad, pLoad, iOut, pOut, temp1, temp2, temp3, fan1);
  }

  yield();
}

void updateOLED() {
  display.clearBuffer();
  char buf[40];

  // Row 1: Status bar
  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(0, 6, psuStatus.c_str());
  display.drawStr(50, 6, acOkState ? "AC:OK" : "AC:--");
  display.drawStr(85, 6, pwrGoodState ? "PWR:OK" : "PWR:--");
  display.drawHLine(0, 8, 128);

  // Row 2: Load (external) - big emphasis
  display.setFont(u8g2_font_6x12_tf);
  snprintf(buf, sizeof(buf), "LOAD: %.1fV %.1fA %.0fW", vOut, iLoad, pLoad);
  display.drawStr(0, 20, buf);

  // Row 3: Input
  display.setFont(u8g2_font_5x7_tf);
  snprintf(buf, sizeof(buf), "IN: %.0fV %.1fA %.0fW PFC:%.0fV", vIn, iIn, pIn, vCap);
  display.drawStr(0, 30, buf);

  // Row 4: Temperatures
  snprintf(buf, sizeof(buf), "TEMP: %.0f/%.0f/%.0fC (P/S/A)", temp1, temp2, temp3);
  display.drawStr(0, 40, buf);

  // Row 5: Fan + Power state
  snprintf(buf, sizeof(buf), "FAN: %d RPM  [%s]", fan1, psuEnabled ? "ON" : "OFF");
  display.drawStr(0, 50, buf);

  // Row 6: IP address
  display.setFont(u8g2_font_5x7_tf);
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(buf, sizeof(buf), "IP: %s", WiFi.localIP().toString().c_str());
  } else {
    snprintf(buf, sizeof(buf), "AP: 192.168.4.1");
  }
  display.drawStr(0, 62, buf);

  display.sendBuffer();
}

void updateStatus(uint16_t status) {
  bool voutFault   = status & 0x8000;
  bool ioutFault   = status & 0x4000;
  bool inputFault  = status & 0x2000;
  bool fanFault    = status & 0x0400;
  bool off         = status & 0x0040;
  bool voutOV      = status & 0x0020;
  bool ioutOC      = status & 0x0010;
  bool vinUV       = status & 0x0008;
  bool tempFault   = status & 0x0004;

  if (off && inputFault) {
    psuStatus = "PROT";
    statusCode = 2;
  } else if (voutFault) {
    psuStatus = "VOUT FAULT";
    statusCode = 2;
  } else if (ioutFault) {
    psuStatus = "IOUT FAULT";
    statusCode = 2;
  } else if (inputFault) {
    psuStatus = "INPUT FAULT";
    statusCode = 2;
  } else if (tempFault) {
    psuStatus = "OVERTEMP";
    statusCode = 2;
  } else if (voutOV) {
    psuStatus = "OVERVOLT";
    statusCode = 2;
  } else if (ioutOC) {
    psuStatus = "OVERCURR";
    statusCode = 2;
  } else if (vinUV) {
    psuStatus = "INPUT UV";
    statusCode = 2;
  } else if (fanFault) {
    psuStatus = "FAN FAULT";
    statusCode = 0;
  } else if (off) {
    psuStatus = "OFF";
    statusCode = 0;
  } else if (iOut > 0.5) {
    psuStatus = "RUNNING";
    statusCode = 1;
  } else if (vOut > 10) {
    psuStatus = "STANDBY";
    statusCode = 1;
  } else {
    psuStatus = "NO OUTPUT";
    statusCode = 0;
  }
}

//=============================================================================
// PMBus Read Functions
//=============================================================================
uint8_t readByte(uint8_t cmd) {
  Wire.beginTransmission(PSU_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return 0;
  delay(5);
  Wire.requestFrom((uint8_t)PSU_ADDR, (uint8_t)1);
  if (Wire.available()) return Wire.read();
  return 0;
}

uint16_t readWord(uint8_t cmd) {
  Wire.beginTransmission(PSU_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) {
    delay(2);
    return 0;
  }
  delay(10);
  Wire.requestFrom((uint8_t)PSU_ADDR, (uint8_t)2);
  if (Wire.available() >= 2) {
    uint8_t low = Wire.read();
    uint8_t high = Wire.read();
    return low | (high << 8);
  }
  return 0;
}

String readString(uint8_t cmd) {
  Wire.beginTransmission(PSU_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return "";
  delay(10);
  Wire.requestFrom((uint8_t)PSU_ADDR, (uint8_t)32);
  if (!Wire.available()) return "";
  uint8_t len = Wire.read();
  if (len == 0 || len > 31) return "";
  String result = "";
  for (int i = 0; i < len && Wire.available(); i++) {
    char c = Wire.read();
    if (c >= 32 && c < 127) result += c;
  }
  result.trim();
  return result;
}

float readLinear11(uint8_t cmd) {
  uint16_t raw = readWord(cmd);
  if (raw == 0 || raw == 0xFFFF) return 0;
  int16_t exp = (raw >> 11) & 0x1F;
  if (exp > 15) exp -= 32;
  uint16_t mantissa = raw & 0x7FF;
  return mantissa * pow(2, exp);
}

float readLinear16(uint8_t cmd) {
  uint16_t raw = readWord(cmd);
  if (raw == 0 || raw == 0xFFFF) return 0;
  return raw * pow(2, voutExponent);
}

//=============================================================================
// MQTT / Home Assistant
//=============================================================================

// Called when HA sends a message to MQTT_COMMAND_TOPIC
// Payload: "ON" or "OFF"
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] Cmd received on %s: %s\n", topic, msg.c_str());

  if (strcmp(topic, MQTT_COMMAND_TOPIC) == 0) {
    if (msg == "ON" && !psuEnabled) {
      psuEnabled = true;
      digitalWrite(PS_ON_PIN, HIGH);
      Serial.println("[MQTT] PSU ON via HA");
    } else if (msg == "OFF" && psuEnabled) {
      psuEnabled = false;
      digitalWrite(PS_ON_PIN, LOW);
      Serial.println("[MQTT] PSU OFF via HA");
    }
  }
}

// Helper: publish one sensor discovery payload
static void publishSensorDisc(const char* id, const char* name,
                               const char* valueTpl, const char* unit,
                               const char* devClass, const char* icon) {
  String topic = String(HA_DISCOVERY_PREFIX) + "/sensor/" +
                 mqttClientId + "_" + id + "/config";

  // Build compact JSON using a stack buffer to avoid heap fragmentation
  String p = "{";
  p += "\"name\":\"" + String(name) + "\",";
  p += "\"uniq_id\":\"" + String(mqttClientId) + "_" + id + "\",";
  p += "\"stat_t\":\"" MQTT_STATE_TOPIC "\",";
  p += "\"val_tpl\":\"" + String(valueTpl) + "\",";
  if (unit[0])     p += "\"unit_of_meas\":\"" + String(unit) + "\",";
  if (devClass[0]) p += "\"dev_cla\":\"" + String(devClass) + "\",";
  if (icon[0])     p += "\"icon\":\"" + String(icon) + "\",";
  p += "\"avty_t\":\"" MQTT_AVAILABILITY_TOPIC "\",";
  p += "\"device\":{\"ids\":[\"dell_psu_monitor\"],\"name\":\"Dell PSU Monitor\","
       "\"mf\":\"Dell\",\"mdl\":\"" + psuModel + "\"}}";

  mqttClient.publish(topic.c_str(), p.c_str(), true);
}

static void publishBinarySensorDisc(const char* id, const char* name,
                                    const char* valueTpl, const char* devClass) {
  String topic = String(HA_DISCOVERY_PREFIX) + "/binary_sensor/" +
                 mqttClientId + "_" + id + "/config";
  String p = "{";
  p += "\"name\":\"" + String(name) + "\",";
  p += "\"uniq_id\":\"" + String(mqttClientId) + "_" + id + "\",";
  p += "\"stat_t\":\"" MQTT_STATE_TOPIC "\",";
  p += "\"val_tpl\":\"" + String(valueTpl) + "\",";
  p += "\"payload_on\":\"true\",\"payload_off\":\"false\",";
  if (devClass[0]) p += "\"dev_cla\":\"" + String(devClass) + "\",";
  p += "\"avty_t\":\"" MQTT_AVAILABILITY_TOPIC "\",";
  p += "\"device\":{\"ids\":[\"dell_psu_monitor\"],\"name\":\"Dell PSU Monitor\","
       "\"mf\":\"Dell\",\"mdl\":\"" + psuModel + "\"}}";
  mqttClient.publish(topic.c_str(), p.c_str(), true);
}

static void publishSwitchDisc() {
  String topic = String(HA_DISCOVERY_PREFIX) + "/switch/" +
                 mqttClientId + "_power/config";
  String p = "{";
  p += "\"name\":\"PSU Power\",";
  p += "\"uniq_id\":\"" + String(mqttClientId) + "_power\",";
  p += "\"stat_t\":\"" MQTT_STATE_TOPIC "\",";
  p += "\"val_tpl\":\"{{ value_json.psuon }}\",";
  p += "\"payload_on\":\"true\",\"payload_off\":\"false\",";
  p += "\"cmd_t\":\"" MQTT_COMMAND_TOPIC "\",";
  p += "\"payload_press_on\":\"ON\",\"payload_press_off\":\"OFF\",";
  p += "\"icon\":\"mdi:power\",";
  p += "\"avty_t\":\"" MQTT_AVAILABILITY_TOPIC "\",";
  p += "\"device\":{\"ids\":[\"dell_psu_monitor\"],\"name\":\"Dell PSU Monitor\","
       "\"mf\":\"Dell\",\"mdl\":\"" + psuModel + "\"}}";
  mqttClient.publish(topic.c_str(), p.c_str(), true);
}

void publishHADiscovery() {
  Serial.println("[MQTT] Publishing HA discovery...");

  // Input
  publishSensorDisc("vin",   "Input Voltage",  "{{ value_json.vin }}",   "V",   "voltage",     "mdi:sine-wave");
  publishSensorDisc("iin",   "Input Current",  "{{ value_json.iin }}",   "A",   "current",     "mdi:current-ac");
  publishSensorDisc("pin",   "Input Power",    "{{ value_json.pin }}",   "W",   "power",       "mdi:lightning-bolt");
  publishSensorDisc("vcap",  "PFC Cap Voltage","{{ value_json.vcap }}",  "V",   "voltage",     "mdi:capacitor");

  // Output
  publishSensorDisc("vout",  "Output Voltage", "{{ value_json.vout }}",  "V",   "voltage",     "mdi:flash");
  publishSensorDisc("iout",  "Output Current", "{{ value_json.iout }}",  "A",   "current",     "mdi:current-dc");
  publishSensorDisc("pout",  "Output Power",   "{{ value_json.pout }}",  "W",   "power",       "mdi:lightning-bolt-outline");

  // Load
  publishSensorDisc("iload", "Load Current",   "{{ value_json.iload }}", "A",   "current",     "mdi:current-dc");
  publishSensorDisc("pload", "Load Power",     "{{ value_json.pload }}", "W",   "power",       "mdi:power-plug");

  // Thermal
  publishSensorDisc("temp1", "Temp Primary",   "{{ value_json.t1 }}",    "°C",  "temperature", "mdi:thermometer");
  publishSensorDisc("temp2", "Temp Secondary", "{{ value_json.t2 }}",    "°C",  "temperature", "mdi:thermometer");
  publishSensorDisc("temp3", "Temp Ambient",   "{{ value_json.t3 }}",    "°C",  "temperature", "mdi:thermometer-low");
  publishSensorDisc("fan",   "Fan Speed",      "{{ value_json.fan }}",   "RPM", "",            "mdi:fan");
  publishSensorDisc("status","PSU Status",     "{{ value_json.status }}","",    "",            "mdi:information-outline");

  // Binary sensors
  publishBinarySensorDisc("ac_ok",   "AC OK",    "{{ value_json.ac }}",  "power");
  publishBinarySensorDisc("pwr_good","PWR Good",  "{{ value_json.pwr }}", "power");

  // Switch for PSU on/off
  publishSwitchDisc();

  Serial.println("[MQTT] HA discovery complete");
}

bool mqttConnect() {
  Serial.printf("[MQTT] Connecting to %s:%d...\n", mqttServer, mqttPort);

  bool ok;
  if (mqttUser[0]) {
    ok = mqttClient.connect(mqttClientId, mqttUser, mqttPass,
                            MQTT_AVAILABILITY_TOPIC, 0, true, "offline");
  } else {
    ok = mqttClient.connect(mqttClientId, nullptr, nullptr,
                            MQTT_AVAILABILITY_TOPIC, 0, true, "offline");
  }

  if (ok) {
    Serial.println("[MQTT] Connected");
    mqttClient.publish(MQTT_AVAILABILITY_TOPIC, "online", true);
    mqttClient.subscribe(MQTT_COMMAND_TOPIC);

    if (!haDiscoveryPublished) {
      publishHADiscovery();
      haDiscoveryPublished = true;
    }
  } else {
    Serial.printf("[MQTT] Failed, rc=%d\n", mqttClient.state());
  }
  return ok;
}

void publishMQTTState() {
  // Re-use the same JSON already sent to EventSource clients
  String json = "{";
  json += "\"vin\":"    + String(vIn, 1)    + ",";
  json += "\"iin\":"    + String(iIn, 2)    + ",";
  json += "\"pin\":"    + String(pIn, 0)    + ",";
  json += "\"vcap\":"   + String(vCap, 0)   + ",";
  json += "\"vout\":"   + String(vOut, 2)   + ",";
  json += "\"iout\":"   + String(iOut, 1)   + ",";
  json += "\"pout\":"   + String(pOut, 0)   + ",";
  json += "\"iload\":"  + String(iLoad, 1)  + ",";
  json += "\"pload\":"  + String(pLoad, 0)  + ",";
  json += "\"t1\":"     + String(temp1, 0)  + ",";
  json += "\"t2\":"     + String(temp2, 0)  + ",";
  json += "\"t3\":"     + String(temp3, 0)  + ",";
  json += "\"fan\":"    + String(fan1)       + ",";
  json += "\"ac\":"     + String(acOkState   ? "true" : "false") + ",";
  json += "\"pwr\":"    + String(pwrGoodState ? "true" : "false") + ",";
  json += "\"status\":\"" + psuStatus + "\",";
  json += "\"psuon\":"  + String(psuEnabled  ? "true" : "false");
  json += "}";
  mqttClient.publish(MQTT_STATE_TOPIC, json.c_str());
}

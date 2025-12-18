/*
 * VDA IR Control Firmware
 * Supports:
 * - Olimex ESP32-POE-ISO (Ethernet/PoE) - compile with USE_ETHERNET
 * - ESP32 DevKit (WiFi) - compile with USE_WIFI
 *
 * Features:
 * - HTTP REST API for Home Assistant integration
 * - IR transmission on configurable GPIO pins
 * - IR learning/receiving on input-only GPIO pins
 * - mDNS discovery
 * - Persistent configuration storage
 * - Captive portal for WiFi setup (WiFi boards)
 * - LED status indication
 */

#include <Arduino.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <DNSServer.h>
#include <Update.h>

#ifdef USE_ETHERNET
  #include <ETH.h>
#else
  #include <WiFi.h>
#endif

// ============ LED Configuration ============
#ifdef USE_ETHERNET
  // Olimex ESP32-POE-ISO doesn't have a user LED on a standard pin
  #define STATUS_LED_PIN -1
#else
  // ESP32 DevKit has built-in LED on GPIO2
  #define STATUS_LED_PIN 2
#endif

// Firmware version
#define FIRMWARE_VERSION "1.2.1"

// LED States
enum LedState {
  LED_OFF,
  LED_ON,
  LED_BLINK_SLOW,    // Booting / Connecting
  LED_BLINK_FAST,    // AP Mode ready
  LED_BLINK_PATTERN  // Error
};

LedState currentLedState = LED_OFF;
unsigned long lastLedToggle = 0;
bool ledOn = false;

// ============ Captive Portal ============
#ifdef USE_WIFI
  DNSServer dnsServer;
  const byte DNS_PORT = 53;
  bool captivePortalActive = false;
#endif

// ============ WiFi Credentials (for DevKit) ============
#ifdef USE_WIFI
  // Default credentials - can be changed via serial or web interface
  String wifiSSID = "";
  String wifiPassword = "";
  bool wifiConfigured = false;
#endif

// ============ Available GPIO Pins for IR ============
#ifdef USE_ETHERNET
  // Olimex ESP32-POE-ISO pins (Ethernet reserves some GPIOs)
  const int OUTPUT_CAPABLE_PINS[] = {0, 1, 2, 3, 4, 5, 13, 14, 15, 16, 32, 33};
  const int OUTPUT_CAPABLE_COUNT = 12;
#else
  // ESP32 DevKit - more GPIOs available (no Ethernet)
  // Note: GPIO2 is used for LED, so we exclude it from IR use when LED is enabled
  const int OUTPUT_CAPABLE_PINS[] = {4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
  const int OUTPUT_CAPABLE_COUNT = 18;
#endif

// Input-only pins (for IR receiver) - same on both boards
const int INPUT_ONLY_PINS[] = {34, 35, 36, 39};
const int INPUT_ONLY_COUNT = 4;

// ============ Port Configuration ============
struct PortConfig {
  int gpio;
  String mode;  // "ir_output", "ir_input", "disabled"
  String name;
};

#ifdef USE_ETHERNET
  #define MAX_PORTS 16
#else
  #define MAX_PORTS 22
#endif

PortConfig ports[MAX_PORTS];
int portCount = 0;

// ============ IR Objects ============
IRsend* irSenders[MAX_PORTS] = {nullptr};
IRrecv* irReceiver = nullptr;
decode_results irResults;
int activeReceiverPort = -1;

// ============ Serial Bridge Configuration ============
HardwareSerial SerialBridge(1);  // UART1 for serial bridge
bool serialBridgeEnabled = false;
int serialBridgeRxPin = -1;
int serialBridgeTxPin = -1;
int serialBridgeBaud = 115200;
String serialBridgeBuffer = "";

// ============ Board Configuration ============
String boardId = "";
String boardName = "VDA IR Controller";
bool adopted = false;

// ============ Global Objects ============
WebServer server(80);  // Changed to port 80 for captive portal compatibility
Preferences preferences;
bool networkConnected = false;
bool apMode = false;

// ============ Function Declarations ============
void initNetwork();
void setupWebServer();
void loadConfig();
void saveConfig();
void initPorts();
void initIRSender(int portIndex);
void initIRReceiver(int gpio);
String getLocalIP();
String getMacAddress();
void initLED();
void setLedState(LedState state);
void updateLED();

#ifdef USE_ETHERNET
  void onEthEvent(WiFiEvent_t event);
#else
  void onWiFiEvent(WiFiEvent_t event);
  void handleWiFiConfig();
  void startAPMode();
  void handleCaptivePortal();
  String generateSetupPage();
  String generateSuccessPage();
#endif

// ============ HTTP Handlers ============
void handleRoot();
void handleInfo();
void handleStatus();
void handlePorts();
void handleConfigurePort();
void handleAdopt();
void handleSendIR();
void handleTestOutput();
void handleLearningStart();
void handleLearningStop();
void handleLearningStatus();
void handleNotFound();

// Serial Bridge Handlers
void handleSerialConfig();
void handleSerialSend();
void handleSerialRead();
void handleSerialStatus();
void handleOTAPage();
void handleOTAUpload();
void handleOTAComplete();
void initSerialBridge(int rxPin, int txPin, int baud);

// ============ Setup ============
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize LED first for visual feedback
  initLED();
  setLedState(LED_BLINK_SLOW);  // Indicate booting

  Serial.println("\n\n========================================");
  Serial.printf("   VDA IR Control Firmware v%s\n", FIRMWARE_VERSION);
#ifdef USE_ETHERNET
  Serial.println("   Mode: Ethernet (ESP32-POE-ISO)");
#else
  Serial.println("   Mode: WiFi (ESP32 DevKit)");
#endif
  Serial.println("========================================\n");

  // Load saved configuration
  loadConfig();

  // Initialize network
  initNetwork();

  // Wait for network connection
  Serial.println("Waiting for network...");
  int timeout = 0;
  int maxTimeout = 100;  // 10 seconds for Ethernet, will be longer for WiFi AP mode

#ifdef USE_WIFI
  if (!wifiConfigured) {
    Serial.println("No WiFi configured - starting AP mode...");
    startAPMode();
    maxTimeout = 50;  // Shorter wait for AP mode
  }
#endif

  while (!networkConnected && timeout < maxTimeout) {
    delay(100);
    updateLED();
    timeout++;
  }

  if (networkConnected) {
    // Setup mDNS
    String mdnsName = boardId.length() > 0 ? boardId : "vda-ir-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (MDNS.begin(mdnsName.c_str())) {
      MDNS.addService("http", "tcp", 80);
      MDNS.addService("vda-ir", "tcp", 80);
      Serial.printf("mDNS: %s.local\n", mdnsName.c_str());
    }

    // Setup web server
    setupWebServer();

    // Initialize ports
    initPorts();

    // Set LED state based on mode
    if (apMode) {
      setLedState(LED_BLINK_FAST);  // AP mode ready
      Serial.println("\n=== AP Mode Ready! ===");
      Serial.println("Connect to WiFi network shown above");
      Serial.println("Then open http://192.168.4.1 in your browser");
    } else {
      setLedState(LED_ON);  // Connected and ready
      Serial.println("\n=== Ready! ===");
    }

    Serial.printf("IP Address: %s\n", getLocalIP().c_str());
    Serial.printf("Board ID: %s\n", boardId.c_str());
    Serial.printf("HTTP Server: http://%s/\n", getLocalIP().c_str());
  } else {
    Serial.println("ERROR: Network connection failed!");
    setLedState(LED_BLINK_PATTERN);  // Error state
#ifdef USE_WIFI
    Serial.println("Starting AP mode for configuration...");
    startAPMode();
    setupWebServer();
    setLedState(LED_BLINK_FAST);
#endif
  }
}

// ============ Loop ============
void loop() {
  // Handle DNS for captive portal
#ifdef USE_WIFI
  if (captivePortalActive) {
    dnsServer.processNextRequest();
  }
#endif

  server.handleClient();

  // Check for IR signals if receiver is active
  if (irReceiver != nullptr && irReceiver->decode(&irResults)) {
    Serial.println("IR Signal Received!");
    serialPrintUint64(irResults.value, HEX);
    Serial.println();
    irReceiver->resume();
  }

  // Update LED state
  updateLED();

  delay(1);
}

// ============ LED Functions ============
void initLED() {
#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  Serial.printf("Status LED initialized on GPIO%d\n", STATUS_LED_PIN);
#endif
}

void setLedState(LedState state) {
  currentLedState = state;
  lastLedToggle = millis();

#if STATUS_LED_PIN >= 0
  if (state == LED_OFF) {
    digitalWrite(STATUS_LED_PIN, LOW);
    ledOn = false;
  } else if (state == LED_ON) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    ledOn = true;
  }
#endif
}

void updateLED() {
#if STATUS_LED_PIN >= 0
  unsigned long now = millis();

  switch (currentLedState) {
    case LED_BLINK_SLOW:
      if (now - lastLedToggle >= 500) {
        ledOn = !ledOn;
        digitalWrite(STATUS_LED_PIN, ledOn ? HIGH : LOW);
        lastLedToggle = now;
      }
      break;

    case LED_BLINK_FAST:
      if (now - lastLedToggle >= 150) {
        ledOn = !ledOn;
        digitalWrite(STATUS_LED_PIN, ledOn ? HIGH : LOW);
        lastLedToggle = now;
      }
      break;

    case LED_BLINK_PATTERN:
      // Double blink pattern for error
      {
        unsigned long cycle = (now / 100) % 10;
        bool shouldBeOn = (cycle == 0 || cycle == 2);
        if (shouldBeOn != ledOn) {
          ledOn = shouldBeOn;
          digitalWrite(STATUS_LED_PIN, ledOn ? HIGH : LOW);
        }
      }
      break;

    default:
      break;
  }
#endif
}

// ============ Network Initialization ============
#ifdef USE_ETHERNET

void initNetwork() {
  WiFi.onEvent(onEthEvent);
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE);
}

void onEthEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH: Started");
      ETH.setHostname(boardId.length() > 0 ? boardId.c_str() : "vda-ir-controller");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH: Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.printf("ETH: Got IP - %s\n", ETH.localIP().toString().c_str());
      Serial.printf("ETH: MAC - %s\n", ETH.macAddress().c_str());
      networkConnected = true;
      setLedState(LED_ON);
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH: Disconnected");
      networkConnected = false;
      setLedState(LED_BLINK_SLOW);
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH: Stopped");
      networkConnected = false;
      setLedState(LED_OFF);
      break;
    default:
      break;
  }
}

String getLocalIP() {
  return ETH.localIP().toString();
}

String getMacAddress() {
  return ETH.macAddress();
}

#else  // USE_WIFI

void initNetwork() {
  WiFi.onEvent(onWiFiEvent);

  if (wifiConfigured && wifiSSID.length() > 0) {
    Serial.printf("Connecting to WiFi: %s\n", wifiSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(boardId.length() > 0 ? boardId.c_str() : "vda-ir-controller");
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  }
}

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("WiFi: Started");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("WiFi: Connected");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("WiFi: Got IP - %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("WiFi: MAC - %s\n", WiFi.macAddress().c_str());
      networkConnected = true;
      apMode = false;
      setLedState(LED_ON);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi: Disconnected");
      networkConnected = false;
      setLedState(LED_BLINK_SLOW);
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("WiFi AP: Started");
      networkConnected = true;
      apMode = true;
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println("WiFi AP: Client connected");
      break;
    default:
      break;
  }
}

void startAPMode() {
  String apName = "VDA-IR-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  apName.toUpperCase();

  Serial.printf("Starting AP: %s (password: vda-ir-setup)\n", apName.c_str());

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName.c_str(), "vda-ir-setup");

  // Start DNS server for captive portal
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  captivePortalActive = true;

  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("Captive portal DNS started - all domains redirect to setup page");

  networkConnected = true;
  apMode = true;
  setLedState(LED_BLINK_FAST);
}

String getLocalIP() {
  if (WiFi.getMode() == WIFI_AP || apMode) {
    return WiFi.softAPIP().toString();
  }
  return WiFi.localIP().toString();
}

String getMacAddress() {
  return WiFi.macAddress();
}

// ============ Captive Portal Page ============
String generateSetupPage() {
  String apName = "VDA-IR-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  apName.toUpperCase();

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>VDA IR Control Setup</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 20px;
    }
    .container {
      background: white;
      border-radius: 16px;
      padding: 32px;
      width: 100%;
      max-width: 400px;
      box-shadow: 0 20px 60px rgba(0,0,0,0.3);
    }
    .logo {
      text-align: center;
      margin-bottom: 24px;
    }
    .logo h1 {
      color: #1a1a2e;
      font-size: 24px;
      margin-bottom: 8px;
    }
    .logo p {
      color: #666;
      font-size: 14px;
    }
    .device-id {
      background: #f0f4f8;
      border-radius: 8px;
      padding: 12px;
      text-align: center;
      margin-bottom: 24px;
      font-family: monospace;
      font-size: 14px;
      color: #1a1a2e;
    }
    .form-group {
      margin-bottom: 20px;
    }
    label {
      display: block;
      margin-bottom: 8px;
      color: #333;
      font-weight: 500;
    }
    select, input[type="password"], input[type="text"] {
      width: 100%;
      padding: 12px 16px;
      border: 2px solid #e0e0e0;
      border-radius: 8px;
      font-size: 16px;
      transition: border-color 0.2s;
    }
    select:focus, input:focus {
      outline: none;
      border-color: #4a90d9;
    }
    .password-container {
      position: relative;
    }
    .toggle-password {
      position: absolute;
      right: 12px;
      top: 50%;
      transform: translateY(-50%);
      background: none;
      border: none;
      cursor: pointer;
      color: #666;
      font-size: 14px;
    }
    button[type="submit"] {
      width: 100%;
      padding: 14px;
      background: linear-gradient(135deg, #4a90d9 0%, #357abd 100%);
      color: white;
      border: none;
      border-radius: 8px;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      transition: transform 0.2s, box-shadow 0.2s;
    }
    button[type="submit"]:hover {
      transform: translateY(-2px);
      box-shadow: 0 4px 12px rgba(74, 144, 217, 0.4);
    }
    button[type="submit"]:disabled {
      background: #ccc;
      cursor: not-allowed;
      transform: none;
      box-shadow: none;
    }
    .scanning {
      text-align: center;
      padding: 20px;
      color: #666;
    }
    .spinner {
      border: 3px solid #f3f3f3;
      border-top: 3px solid #4a90d9;
      border-radius: 50%;
      width: 24px;
      height: 24px;
      animation: spin 1s linear infinite;
      margin: 0 auto 12px;
    }
    @keyframes spin {
      0% { transform: rotate(0deg); }
      100% { transform: rotate(360deg); }
    }
    .network-item {
      display: flex;
      align-items: center;
      padding: 12px;
      border: 2px solid #e0e0e0;
      border-radius: 8px;
      margin-bottom: 8px;
      cursor: pointer;
      transition: border-color 0.2s, background 0.2s;
    }
    .network-item:hover {
      border-color: #4a90d9;
      background: #f8fafc;
    }
    .network-item.selected {
      border-color: #4a90d9;
      background: #e8f4fd;
    }
    .network-name {
      flex: 1;
      font-weight: 500;
    }
    .network-signal {
      color: #666;
      font-size: 12px;
    }
    .signal-icon {
      margin-left: 8px;
    }
    .refresh-btn {
      background: none;
      border: none;
      color: #4a90d9;
      cursor: pointer;
      font-size: 14px;
      margin-bottom: 12px;
    }
    .error {
      background: #fee;
      color: #c00;
      padding: 12px;
      border-radius: 8px;
      margin-bottom: 16px;
      font-size: 14px;
    }
    .led-indicator {
      display: inline-block;
      width: 12px;
      height: 12px;
      background: #4ade80;
      border-radius: 50%;
      margin-right: 8px;
      animation: pulse 2s infinite;
    }
    @keyframes pulse {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.5; }
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="logo">
      <h1>🎛️ VDA IR Control</h1>
      <p>WiFi Setup</p>
    </div>

    <div class="device-id">
      <span class="led-indicator"></span>
      )rawliteral" + apName + R"rawliteral(
    </div>

    <div id="networks-container">
      <div class="scanning">
        <div class="spinner"></div>
        Scanning for networks...
      </div>
    </div>

    <form id="wifi-form" style="display:none;">
      <div class="form-group">
        <label>WiFi Network</label>
        <select id="ssid" name="ssid" required>
          <option value="">Select a network...</option>
        </select>
      </div>

      <div class="form-group">
        <label>Password</label>
        <div class="password-container">
          <input type="password" id="password" name="password" placeholder="Enter WiFi password">
          <button type="button" class="toggle-password" onclick="togglePassword()">Show</button>
        </div>
      </div>

      <button type="submit" id="connect-btn">Connect</button>
    </form>
  </div>

  <script>
    let networks = [];

    async function scanNetworks() {
      try {
        const response = await fetch('/wifi/scan');
        const data = await response.json();
        networks = data.networks || [];
        displayNetworks();
      } catch (error) {
        document.getElementById('networks-container').innerHTML =
          '<div class="error">Failed to scan networks. Please refresh the page.</div>';
      }
    }

    function displayNetworks() {
      const container = document.getElementById('networks-container');
      const form = document.getElementById('wifi-form');
      const select = document.getElementById('ssid');

      if (networks.length === 0) {
        container.innerHTML = '<div class="error">No networks found. Please try again.</div>' +
          '<button class="refresh-btn" onclick="scanNetworks()">🔄 Scan Again</button>';
        return;
      }

      container.innerHTML = '<button class="refresh-btn" onclick="scanNetworks()">🔄 Scan Again</button>';

      select.innerHTML = '<option value="">Select a network...</option>';
      networks.forEach(net => {
        const signal = net.rssi > -50 ? '▓▓▓▓' : net.rssi > -70 ? '▓▓▓░' : net.rssi > -80 ? '▓▓░░' : '▓░░░';
        const option = document.createElement('option');
        option.value = net.ssid;
        option.textContent = net.ssid + ' ' + signal + (net.secure ? ' 🔒' : '');
        select.appendChild(option);
      });

      form.style.display = 'block';
    }

    function togglePassword() {
      const input = document.getElementById('password');
      const btn = document.querySelector('.toggle-password');
      if (input.type === 'password') {
        input.type = 'text';
        btn.textContent = 'Hide';
      } else {
        input.type = 'password';
        btn.textContent = 'Show';
      }
    }

    document.getElementById('wifi-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const btn = document.getElementById('connect-btn');
      const ssid = document.getElementById('ssid').value;
      const password = document.getElementById('password').value;

      if (!ssid) {
        alert('Please select a network');
        return;
      }

      btn.disabled = true;
      btn.textContent = 'Connecting...';

      try {
        const response = await fetch('/wifi/config', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ ssid, password })
        });

        const data = await response.json();

        if (data.success) {
          document.querySelector('.container').innerHTML = `
            <div class="logo">
              <h1>✅ WiFi Configured!</h1>
              <p>The device is restarting...</p>
            </div>
            <div style="text-align:center; color:#666; margin-top:20px;">
              <p>The device will connect to <strong>${ssid}</strong></p>
              <p style="margin-top:12px;">You can close this page and find the device on your network.</p>
              <p style="margin-top:12px; font-size:12px;">Look for it at: <code>vda-ir-XXXXXX.local</code></p>
            </div>
          `;
        } else {
          throw new Error(data.error || 'Configuration failed');
        }
      } catch (error) {
        btn.disabled = false;
        btn.textContent = 'Connect';
        alert('Failed to configure WiFi: ' + error.message);
      }
    });

    // Start scanning on page load
    scanNetworks();
  </script>
</body>
</html>
)rawliteral";

  return html;
}

void handleWiFiConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  if (error) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  String newSSID = doc["ssid"] | "";
  String newPassword = doc["password"] | "";

  if (newSSID.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"SSID required\"}");
    return;
  }

  wifiSSID = newSSID;
  wifiPassword = newPassword;
  wifiConfigured = true;

  // Save to preferences
  preferences.begin("vda-ir", false);
  preferences.putString("wifiSSID", wifiSSID);
  preferences.putString("wifiPass", wifiPassword);
  preferences.putBool("wifiConf", true);
  preferences.end();

  StaticJsonDocument<128> response;
  response["success"] = true;
  response["message"] = "WiFi configured. Rebooting...";

  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);

  Serial.println("WiFi configured. Rebooting...");
  setLedState(LED_BLINK_SLOW);
  delay(1000);
  ESP.restart();
}

void handleCaptivePortal() {
  // Serve the setup page for captive portal detection URLs
  server.send(200, "text/html", generateSetupPage());
}

#endif

// ============ Configuration ============
void loadConfig() {
  preferences.begin("vda-ir", true);

  boardId = preferences.getString("boardId", "");
  boardName = preferences.getString("boardName", "VDA IR Controller");
  adopted = preferences.getBool("adopted", false);
  portCount = preferences.getInt("portCount", 0);

#ifdef USE_WIFI
  wifiSSID = preferences.getString("wifiSSID", "");
  wifiPassword = preferences.getString("wifiPass", "");
  wifiConfigured = preferences.getBool("wifiConf", false);
#endif

  // Generate default board ID if not set
  if (boardId.length() == 0) {
    boardId = "vda-ir-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  }

  // Load port configurations
  for (int i = 0; i < portCount && i < MAX_PORTS; i++) {
    String key = "port" + String(i);
    ports[i].gpio = preferences.getInt((key + "_gpio").c_str(), 0);
    ports[i].mode = preferences.getString((key + "_mode").c_str(), "disabled");
    ports[i].name = preferences.getString((key + "_name").c_str(), "");
  }

  // If no ports configured, set up defaults
  if (portCount == 0) {
    // Add all available GPIO pins as disabled by default
    for (int i = 0; i < OUTPUT_CAPABLE_COUNT && portCount < MAX_PORTS; i++) {
      ports[portCount].gpio = OUTPUT_CAPABLE_PINS[i];
      ports[portCount].mode = "disabled";
      ports[portCount].name = "";
      portCount++;
    }
    for (int i = 0; i < INPUT_ONLY_COUNT && portCount < MAX_PORTS; i++) {
      ports[portCount].gpio = INPUT_ONLY_PINS[i];
      ports[portCount].mode = "disabled";
      ports[portCount].name = "";
      portCount++;
    }
  }

  preferences.end();

  Serial.printf("Loaded config: boardId=%s, ports=%d\n", boardId.c_str(), portCount);
}

void saveConfig() {
  preferences.begin("vda-ir", false);

  preferences.putString("boardId", boardId);
  preferences.putString("boardName", boardName);
  preferences.putBool("adopted", adopted);
  preferences.putInt("portCount", portCount);

  for (int i = 0; i < portCount; i++) {
    String key = "port" + String(i);
    preferences.putInt((key + "_gpio").c_str(), ports[i].gpio);
    preferences.putString((key + "_mode").c_str(), ports[i].mode);
    preferences.putString((key + "_name").c_str(), ports[i].name);
  }

  preferences.end();
  Serial.println("Configuration saved");
}

// ============ Port Initialization ============
void initPorts() {
  for (int i = 0; i < portCount; i++) {
    if (ports[i].mode == "ir_output") {
      initIRSender(i);
    } else if (ports[i].mode == "ir_input") {
      initIRReceiver(ports[i].gpio);
    }
  }
}

void initIRSender(int portIndex) {
  if (irSenders[portIndex] != nullptr) {
    delete irSenders[portIndex];
  }
  irSenders[portIndex] = new IRsend(ports[portIndex].gpio);
  irSenders[portIndex]->begin();
  Serial.printf("IR Sender initialized on GPIO%d\n", ports[portIndex].gpio);
}

void initIRReceiver(int gpio) {
  if (irReceiver != nullptr) {
    delete irReceiver;
  }
  irReceiver = new IRrecv(gpio);
  irReceiver->enableIRIn();
  activeReceiverPort = gpio;
  Serial.printf("IR Receiver initialized on GPIO%d\n", gpio);
}

// ============ Web Server Setup ============
void setupWebServer() {
  // Root handler - serve setup page in AP mode, info otherwise
  server.on("/", HTTP_GET, handleRoot);

  // API endpoints
  server.on("/info", HTTP_GET, handleInfo);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/ports", HTTP_GET, handlePorts);
  server.on("/ports/configure", HTTP_POST, handleConfigurePort);
  server.on("/adopt", HTTP_POST, handleAdopt);
  server.on("/send_ir", HTTP_POST, handleSendIR);
  server.on("/test_output", HTTP_POST, handleTestOutput);
  server.on("/learning/start", HTTP_POST, handleLearningStart);
  server.on("/learning/stop", HTTP_POST, handleLearningStop);
  server.on("/learning/status", HTTP_GET, handleLearningStatus);

  // Serial bridge endpoints
  server.on("/serial/config", HTTP_POST, handleSerialConfig);
  server.on("/serial/send", HTTP_POST, handleSerialSend);
  server.on("/serial/read", HTTP_GET, handleSerialRead);
  server.on("/serial/status", HTTP_GET, handleSerialStatus);

  // OTA Update routes (available for both WiFi and Ethernet)
  server.on("/update", HTTP_GET, handleOTAPage);
  server.on("/update", HTTP_POST, handleOTAComplete, handleOTAUpload);

#ifdef USE_WIFI
  server.on("/wifi/config", HTTP_POST, handleWiFiConfig);
  server.on("/wifi/scan", HTTP_GET, []() {
    Serial.println("Scanning WiFi networks...");
    int n = WiFi.scanNetworks();
    Serial.printf("Found %d networks\n", n);

    StaticJsonDocument<2048> doc;
    JsonArray networks = doc.createNestedArray("networks");
    for (int i = 0; i < n && i < 20; i++) {
      JsonObject net = networks.createNestedObject();
      net["ssid"] = WiFi.SSID(i);
      net["rssi"] = WiFi.RSSI(i);
      net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  // Captive portal detection endpoints
  server.on("/generate_204", HTTP_GET, handleCaptivePortal);  // Android
  server.on("/gen_204", HTTP_GET, handleCaptivePortal);        // Android
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);  // Apple
  server.on("/library/test/success.html", HTTP_GET, handleCaptivePortal);  // Apple
  server.on("/ncsi.txt", HTTP_GET, handleCaptivePortal);       // Windows
  server.on("/connecttest.txt", HTTP_GET, handleCaptivePortal); // Windows
  server.on("/fwlink", HTTP_GET, handleCaptivePortal);         // Windows
#endif

  server.onNotFound(handleNotFound);

  server.enableCORS(true);
  server.begin();
  Serial.println("HTTP server started on port 80");
}

// ============ OTA Update Handlers ============
void handleOTAPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Firmware Update</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 40px; background: #1a1a2e; color: #eee; }
    .container { max-width: 500px; margin: 0 auto; background: #16213e; padding: 30px; border-radius: 10px; }
    h1 { color: #e94560; margin-top: 0; }
    form { margin-top: 20px; }
    input[type="file"] { margin: 15px 0; color: #eee; }
    input[type="submit"] { background: #e94560; color: white; border: none; padding: 12px 30px; border-radius: 5px; cursor: pointer; font-size: 16px; }
    input[type="submit"]:hover { background: #ff6b6b; }
    .info { background: #0f3460; padding: 15px; border-radius: 5px; margin-bottom: 20px; }
    .warning { background: #614a19; padding: 10px; border-radius: 5px; margin-top: 15px; font-size: 14px; }
    #progress { display: none; margin-top: 20px; }
    .progress-bar { background: #0f3460; border-radius: 5px; height: 30px; overflow: hidden; }
    .progress-fill { background: #e94560; height: 100%; width: 0%; transition: width 0.3s; }
    .progress-text { text-align: center; margin-top: 10px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Firmware Update</h1>
    <div class="info">
      <strong>Current Version:</strong> )rawliteral" + String(FIRMWARE_VERSION) + R"rawliteral(<br>
      <strong>Device:</strong> )rawliteral" + boardName + R"rawliteral(
    </div>
    <form method="POST" action="/update" enctype="multipart/form-data" id="uploadForm">
      <div>Select firmware file (.bin):</div>
      <input type="file" name="firmware" accept=".bin" required>
      <br>
      <input type="submit" value="Upload & Update">
    </form>
    <div class="warning">
      ⚠️ Do not disconnect power during update. Settings will be preserved.
    </div>
    <div id="progress">
      <div class="progress-bar"><div class="progress-fill" id="progressFill"></div></div>
      <div class="progress-text" id="progressText">Uploading... 0%</div>
    </div>
  </div>
  <script>
    document.getElementById('uploadForm').addEventListener('submit', function(e) {
      e.preventDefault();
      var form = e.target;
      var formData = new FormData(form);
      var xhr = new XMLHttpRequest();
      document.getElementById('progress').style.display = 'block';
      xhr.upload.addEventListener('progress', function(e) {
        if (e.lengthComputable) {
          var pct = Math.round((e.loaded / e.total) * 100);
          document.getElementById('progressFill').style.width = pct + '%';
          document.getElementById('progressText').textContent = 'Uploading... ' + pct + '%';
        }
      });
      xhr.addEventListener('load', function() {
        if (xhr.status === 200) {
          document.getElementById('progressText').textContent = 'Update complete! Rebooting...';
          setTimeout(function() { location.reload(); }, 5000);
        } else {
          document.getElementById('progressText').textContent = 'Update failed: ' + xhr.responseText;
        }
      });
      xhr.addEventListener('error', function() {
        document.getElementById('progressText').textContent = 'Upload failed. Please try again.';
      });
      xhr.open('POST', '/update');
      xhr.send(formData);
    });
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleOTAUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA Update Start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA Update Success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void handleOTAComplete() {
  if (Update.hasError()) {
    server.send(500, "text/plain", "Update failed!");
  } else {
    server.send(200, "text/plain", "Update successful! Rebooting...");
    delay(1000);
    ESP.restart();
  }
}

// ============ HTTP Handlers ============
void handleRoot() {
#ifdef USE_WIFI
  if (apMode) {
    server.send(200, "text/html", generateSetupPage());
    return;
  }
#endif
  // In normal mode, redirect to /info or show a simple status
  handleInfo();
}

void handleInfo() {
  StaticJsonDocument<512> doc;

  doc["board_id"] = boardId;
  doc["board_name"] = boardName;
  doc["mac_address"] = getMacAddress();
  doc["ip_address"] = getLocalIP();
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["adopted"] = adopted;
  doc["total_ports"] = portCount;

#ifdef USE_ETHERNET
  doc["connection_type"] = "ethernet";
#else
  doc["connection_type"] = "wifi";
  doc["wifi_configured"] = wifiConfigured;
  if (apMode) {
    doc["wifi_mode"] = "ap";
  } else {
    doc["wifi_mode"] = "station";
    doc["wifi_ssid"] = wifiSSID;
  }
#endif

  int outputCount = 0, inputCount = 0;
  for (int i = 0; i < portCount; i++) {
    if (ports[i].mode == "ir_output") outputCount++;
    if (ports[i].mode == "ir_input") inputCount++;
  }
  doc["output_count"] = outputCount;
  doc["input_count"] = inputCount;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleStatus() {
  StaticJsonDocument<256> doc;

  doc["board_id"] = boardId;
  doc["online"] = true;
  doc["uptime_seconds"] = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["network_connected"] = networkConnected;

#ifdef USE_WIFI
  if (!apMode && WiFi.getMode() == WIFI_STA) {
    doc["wifi_rssi"] = WiFi.RSSI();
  }
#endif

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handlePorts() {
  StaticJsonDocument<2048> doc;

  doc["total_ports"] = portCount;
  JsonArray portsArray = doc.createNestedArray("ports");

  for (int i = 0; i < portCount; i++) {
    JsonObject port = portsArray.createNestedObject();
    port["port"] = ports[i].gpio;
    port["gpio"] = ports[i].gpio;
    port["mode"] = ports[i].mode;
    port["name"] = ports[i].name;
    port["gpio_name"] = "GPIO" + String(ports[i].gpio);

    // Check if input-only
    bool isInputOnly = false;
    for (int j = 0; j < INPUT_ONLY_COUNT; j++) {
      if (INPUT_ONLY_PINS[j] == ports[i].gpio) {
        isInputOnly = true;
        break;
      }
    }
    port["can_input"] = true;
    port["can_output"] = !isInputOnly;
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleConfigurePort() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  if (error) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  int gpio = doc["port"] | -1;
  String mode = doc["mode"] | "";
  String name = doc["name"] | "";

  // Find port by GPIO
  int portIndex = -1;
  for (int i = 0; i < portCount; i++) {
    if (ports[i].gpio == gpio) {
      portIndex = i;
      break;
    }
  }

  if (portIndex == -1) {
    server.send(400, "application/json", "{\"error\":\"Invalid GPIO\"}");
    return;
  }

  // Check if trying to set output on input-only pin
  if (mode == "ir_output") {
    for (int i = 0; i < INPUT_ONLY_COUNT; i++) {
      if (INPUT_ONLY_PINS[i] == gpio) {
        server.send(400, "application/json", "{\"error\":\"GPIO is input-only\"}");
        return;
      }
    }
  }

  // Update port config
  ports[portIndex].mode = mode;
  ports[portIndex].name = name;

  // Reinitialize port
  if (mode == "ir_output") {
    initIRSender(portIndex);
  } else if (mode == "ir_input") {
    initIRReceiver(gpio);
  }

  // Save config
  saveConfig();

  StaticJsonDocument<256> response;
  response["success"] = true;
  response["port"] = gpio;
  response["mode"] = mode;
  response["name"] = name;

  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);
}

void handleAdopt() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  StaticJsonDocument<256> doc;
  deserializeJson(doc, server.arg("plain"));

  String newBoardId = doc["board_id"] | "";
  String newBoardName = doc["board_name"] | "";

  if (newBoardId.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"board_id required\"}");
    return;
  }

  boardId = newBoardId;
  boardName = newBoardName.length() > 0 ? newBoardName : boardId;
  adopted = true;

  saveConfig();

  // Update mDNS
  MDNS.end();
  MDNS.begin(boardId.c_str());
  MDNS.addService("http", "tcp", 80);

  StaticJsonDocument<128> response;
  response["success"] = true;
  response["board_id"] = boardId;

  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);

  Serial.printf("Board adopted as: %s (%s)\n", boardId.c_str(), boardName.c_str());
}

void handleSendIR() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  StaticJsonDocument<256> doc;
  deserializeJson(doc, server.arg("plain"));

  int output = doc["output"] | -1;
  String code = doc["code"] | "";
  String protocol = doc["protocol"] | "nec";

  // Find port index
  int portIndex = -1;
  for (int i = 0; i < portCount; i++) {
    if (ports[i].gpio == output && ports[i].mode == "ir_output") {
      portIndex = i;
      break;
    }
  }

  if (portIndex == -1 || irSenders[portIndex] == nullptr) {
    server.send(400, "application/json", "{\"error\":\"Invalid output or not configured\"}");
    return;
  }

  // Parse and send IR code
  uint64_t codeValue = strtoull(code.c_str(), nullptr, 16);

  if (protocol == "nec") {
    irSenders[portIndex]->sendNEC(codeValue);
  } else if (protocol == "samsung") {
    irSenders[portIndex]->sendSAMSUNG(codeValue);
  } else if (protocol == "sony") {
    irSenders[portIndex]->sendSony(codeValue);
  } else if (protocol == "rc5") {
    irSenders[portIndex]->sendRC5(codeValue);
  } else if (protocol == "rc6") {
    irSenders[portIndex]->sendRC6(codeValue);
  } else if (protocol == "lg") {
    irSenders[portIndex]->sendLG(codeValue);
  } else if (protocol == "panasonic") {
    irSenders[portIndex]->sendPanasonic(0x4004, codeValue);  // Standard Panasonic address
  } else if (protocol == "pioneer") {
    // Pioneer: exact timing from IRremoteESP8266 library
    // kPioneerHdrMark=8506, kPioneerHdrSpace=4191
    // kPioneerBitMark=568, kPioneerOneSpace=1542, kPioneerZeroSpace=487
    // kPioneerMinGap=25181, kPioneerMinCommandLength=84906
    // 40kHz, MSBfirst=true, duty=33
    irSenders[portIndex]->sendGeneric(
      8506, 4191,     // Header mark, space (µs)
      568, 1542,      // Bit mark, one space
      568, 487,       // Bit mark, zero space
      568, 25181,     // Footer mark, gap
      84906,          // Min command length
      codeValue, 32,  // Data, bits
      40,             // 40kHz frequency
      true, 0, 33     // MSBfirst=true, no repeat, 33% duty
    );
  } else {
    // Send as raw NEC by default
    irSenders[portIndex]->sendNEC(codeValue);
  }

  Serial.printf("Sent IR code 0x%llX via GPIO%d\n", codeValue, output);

  server.send(200, "application/json", "{\"success\":true}");
}

void handleTestOutput() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  StaticJsonDocument<128> doc;
  deserializeJson(doc, server.arg("plain"));

  int output = doc["output"] | -1;
  int duration = doc["duration_ms"] | 500;

  // Find port
  int portIndex = -1;
  for (int i = 0; i < portCount; i++) {
    if (ports[i].gpio == output) {
      portIndex = i;
      break;
    }
  }

  if (portIndex == -1) {
    server.send(400, "application/json", "{\"error\":\"Invalid output\"}");
    return;
  }

  // Send test pattern (simple carrier burst)
  pinMode(output, OUTPUT);
  for (int i = 0; i < duration; i++) {
    digitalWrite(output, HIGH);
    delayMicroseconds(13);
    digitalWrite(output, LOW);
    delayMicroseconds(13);
  }

  Serial.printf("Test signal sent on GPIO%d for %dms\n", output, duration);

  server.send(200, "application/json", "{\"success\":true}");
}

void handleLearningStart() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  StaticJsonDocument<128> doc;
  deserializeJson(doc, server.arg("plain"));

  int port = doc["port"] | 34;  // Default to GPIO34

  // Initialize receiver on specified port
  initIRReceiver(port);

  StaticJsonDocument<128> response;
  response["success"] = true;
  response["port"] = port;

  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);

  Serial.printf("Learning mode started on GPIO%d\n", port);
}

void handleLearningStop() {
  if (irReceiver != nullptr) {
    irReceiver->disableIRIn();
  }
  activeReceiverPort = -1;

  server.send(200, "application/json", "{\"success\":true}");
  Serial.println("Learning mode stopped");
}

void handleLearningStatus() {
  StaticJsonDocument<512> doc;

  doc["active"] = (activeReceiverPort >= 0);
  doc["port"] = activeReceiverPort;

  // Check if we received a code
  if (irReceiver != nullptr && irReceiver->decode(&irResults)) {
    JsonObject receivedCode = doc.createNestedObject("received_code");
    receivedCode["protocol"] = typeToString(irResults.decode_type);
    receivedCode["code"] = "0x" + uint64ToString(irResults.value, HEX);
    receivedCode["bits"] = irResults.bits;

    irReceiver->resume();
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// ============ Serial Bridge Handlers ============

void initSerialBridge(int rxPin, int txPin, int baud) {
  if (serialBridgeEnabled) {
    SerialBridge.end();
  }

  serialBridgeRxPin = rxPin;
  serialBridgeTxPin = txPin;
  serialBridgeBaud = baud;

  SerialBridge.begin(baud, SERIAL_8N1, rxPin, txPin);
  serialBridgeEnabled = true;
  serialBridgeBuffer = "";

  Serial.printf("Serial bridge initialized: RX=%d, TX=%d, Baud=%d\n", rxPin, txPin, baud);
}

void handleSerialConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  if (error) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  int rxPin = doc["rx_pin"] | -1;
  int txPin = doc["tx_pin"] | -1;
  int baud = doc["baud_rate"] | 115200;

  if (rxPin < 0 || txPin < 0) {
    server.send(400, "application/json", "{\"error\":\"rx_pin and tx_pin required\"}");
    return;
  }

  // Validate pins based on board type
#ifdef USE_ETHERNET
  // Olimex: Recommended UART1 on GPIO9 (RX) / GPIO10 (TX)
  Serial.printf("Olimex board: Configuring serial on RX=%d, TX=%d\n", rxPin, txPin);
#else
  // DevKit: UART1 on GPIO16/17 or UART2 on GPIO25/26
  Serial.printf("DevKit board: Configuring serial on RX=%d, TX=%d\n", rxPin, txPin);
#endif

  initSerialBridge(rxPin, txPin, baud);

  StaticJsonDocument<256> response;
  response["success"] = true;
  response["rx_pin"] = rxPin;
  response["tx_pin"] = txPin;
  response["baud_rate"] = baud;

  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);
}

void handleSerialSend() {
  if (!serialBridgeEnabled) {
    server.send(400, "application/json", "{\"error\":\"Serial bridge not configured\"}");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  if (error) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  String data = doc["data"] | "";
  String format = doc["format"] | "text";
  String lineEnding = doc["line_ending"] | "none";
  int timeout = doc["timeout"] | 1000;  // Response timeout in ms
  bool waitResponse = doc["wait_response"] | true;

  if (data.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"data required\"}");
    return;
  }

  // Clear any pending data in the buffer
  while (SerialBridge.available()) {
    SerialBridge.read();
  }
  serialBridgeBuffer = "";

  // Send data
  if (format == "hex") {
    // Parse hex string and send bytes
    for (size_t i = 0; i < data.length(); i += 2) {
      if (i + 1 < data.length()) {
        String byteStr = data.substring(i, i + 2);
        uint8_t b = (uint8_t)strtol(byteStr.c_str(), nullptr, 16);
        SerialBridge.write(b);
      }
    }
  } else {
    // Send as text
    SerialBridge.print(data);
  }

  // Add line ending
  if (lineEnding == "cr") {
    SerialBridge.write('\r');
  } else if (lineEnding == "lf") {
    SerialBridge.write('\n');
  } else if (lineEnding == "crlf") {
    SerialBridge.write('\r');
    SerialBridge.write('\n');
  } else if (lineEnding == "!") {
    SerialBridge.write('!');
  }

  Serial.printf("Serial sent: %s (format=%s, ending=%s)\n", data.c_str(), format.c_str(), lineEnding.c_str());

  // Wait for response if requested
  String response = "";
  if (waitResponse && timeout > 0) {
    unsigned long start = millis();
    while (millis() - start < (unsigned long)timeout) {
      while (SerialBridge.available()) {
        char c = SerialBridge.read();
        response += c;
        // Check for common terminators
        if (c == '\n' || c == '\r' || c == '!') {
          // Give a little more time for additional data
          delay(50);
          while (SerialBridge.available()) {
            response += (char)SerialBridge.read();
          }
          break;
        }
      }
      if (response.length() > 0) {
        break;
      }
      delay(10);
    }
  }

  // Trim response
  response.trim();

  Serial.printf("Serial response: %s\n", response.c_str());

  StaticJsonDocument<512> respDoc;
  respDoc["success"] = true;
  respDoc["response"] = response;
  respDoc["response_length"] = response.length();

  String respStr;
  serializeJson(respDoc, respStr);
  server.send(200, "application/json", respStr);
}

void handleSerialRead() {
  if (!serialBridgeEnabled) {
    server.send(400, "application/json", "{\"error\":\"Serial bridge not configured\"}");
    return;
  }

  // Read any available data
  String data = "";
  while (SerialBridge.available()) {
    data += (char)SerialBridge.read();
  }

  StaticJsonDocument<512> response;
  response["success"] = true;
  response["data"] = data;
  response["length"] = data.length();

  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);
}

void handleSerialStatus() {
  StaticJsonDocument<256> response;

  response["enabled"] = serialBridgeEnabled;
  response["rx_pin"] = serialBridgeRxPin;
  response["tx_pin"] = serialBridgeTxPin;
  response["baud_rate"] = serialBridgeBaud;
  response["available"] = serialBridgeEnabled ? SerialBridge.available() : 0;

#ifdef USE_ETHERNET
  response["board_type"] = "olimex_poe_iso";
  JsonObject recommended = response.createNestedObject("recommended_pins");
  recommended["uart1_rx"] = 9;
  recommended["uart1_tx"] = 10;
#else
  response["board_type"] = "esp32_devkit";
  JsonObject recommended = response.createNestedObject("recommended_pins");
  recommended["uart1_rx"] = 16;
  recommended["uart1_tx"] = 17;
  recommended["uart2_rx"] = 25;
  recommended["uart2_tx"] = 26;
#endif

  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);
}

void handleNotFound() {
#ifdef USE_WIFI
  // In AP mode, redirect all unknown requests to the setup page (captive portal)
  if (apMode) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "Redirecting to setup...");
    return;
  }
#endif
  server.send(404, "application/json", "{\"error\":\"Not found\"}");
}

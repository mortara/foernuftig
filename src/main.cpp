#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <EEPROM.h>

#if __has_include("secrets.local.h")
#include "secrets.local.h"
#else
#include "secrets.h"
#endif

constexpr uint8_t PIN_OUT_D2 = D2;
constexpr uint8_t PIN_OUT_D3 = D3;
constexpr uint8_t PIN_OUT_D4 = D4;

constexpr uint8_t PIN_IN_D1 = D1;
constexpr uint8_t PIN_IN_D5 = D5;
constexpr uint8_t PIN_IN_D6 = D6;
constexpr uint8_t PIN_IN_D7 = D7;

constexpr uint32_t ADC_INTERVAL_MS = 100;
constexpr float ADC_DELTA = 0.1f;
constexpr float ADC_LED_OFF_THRESHOLD = 0.2f;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
constexpr uint16_t DNS_PORT = 53;

// Safe-boot: EEPROM layout
constexpr uint16_t EEPROM_SIZE        = 8;
constexpr uint16_t EEPROM_ADDR_MAGIC  = 0;   // uint16_t: 0xAB42 = valid
constexpr uint16_t EEPROM_ADDR_BOOTS  = 2;   // uint8_t:  rapid-reboot counter
constexpr uint8_t  SAFE_BOOT_MAGIC_HI = 0xAB;
constexpr uint8_t  SAFE_BOOT_MAGIC_LO = 0x42;
constexpr uint8_t  SAFE_BOOT_THRESHOLD = 3;  // 3 rapid reboots → rescue mode
constexpr uint32_t SAFE_BOOT_WINDOW_MS = 10000; // ms after boot to count as rapid
bool rescueMode = false;

ESP8266WebServer server(80);
DNSServer dnsServer;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

int fanSpeed = 0;
float lastAdcPublished = -1.0f;
float ledAdcValue = 0.0f;
bool ledSensorOn = false;
unsigned long lastAdcReadMs = 0;

bool fallbackApActive = false;
unsigned long lastMqttReconnectAttemptMs = 0;
bool mqttDiscoveryPublished = false;
int lastPublishedFanSpeed = -1;
float lastPublishedAdc = -1.0f;
int8_t lastPublishedLedSensorOn = -1;

String topicBase;
String topicFanSpeedState;
String topicFanSpeedCommand;
String topicAdcState;
String topicFilterState;
String topicRestartCommand;

struct ButtonState {
  uint8_t pin;
  uint8_t targetSpeed;
  bool lastRawPressed;
  bool stablePressed;
  unsigned long lastChangeMs;
};

ButtonState buttons[] = {
  {PIN_IN_D1, 0, false, false, 0},
  {PIN_IN_D5, 1, false, false, 0},
  {PIN_IN_D6, 2, false, false, 0},
  {PIN_IN_D7, 3, false, false, 0},
};

bool mqttEnabled() {
  return MQTT_HOST[0] != '\0';
}

bool hasMqttAuth() {
  return MQTT_USER[0] != '\0';
}

void publishMqttState();

void writeInvertedOutput(uint8_t pin, bool logicalOn) {
  digitalWrite(pin, logicalOn ? LOW : HIGH);
}

void applyFanSpeed(int speed) {
  if (speed < 0) {
    speed = 0;
  }
  if (speed > 3) {
    speed = 3;
  }

  fanSpeed = speed;

  switch (fanSpeed) {
    case 0:
      writeInvertedOutput(PIN_OUT_D4, false);
      writeInvertedOutput(PIN_OUT_D3, false);
      writeInvertedOutput(PIN_OUT_D2, false);
      break;
    case 1:
      writeInvertedOutput(PIN_OUT_D4, true);
      writeInvertedOutput(PIN_OUT_D3, false);
      writeInvertedOutput(PIN_OUT_D2, false);
      break;
    case 2:
      writeInvertedOutput(PIN_OUT_D4, false);
      writeInvertedOutput(PIN_OUT_D3, true);
      writeInvertedOutput(PIN_OUT_D2, false);
      break;
    default:
      writeInvertedOutput(PIN_OUT_D4, false);
      writeInvertedOutput(PIN_OUT_D3, false);
      writeInvertedOutput(PIN_OUT_D2, true);
      break;
  }

  Serial.printf("Fan speed set to %d\n", fanSpeed);
  publishMqttState();
}

void updateAdcSensor() {
  const unsigned long now = millis();
  if (now - lastAdcReadMs < ADC_INTERVAL_MS) {
    return;
  }
  lastAdcReadMs = now;

  const float adc = analogRead(A0) / 1023.0f;
  if (lastAdcPublished < 0.0f || fabsf(adc - lastAdcPublished) >= ADC_DELTA) {
    ledAdcValue = adc;
    lastAdcPublished = adc;
    ledSensorOn = !(adc < ADC_LED_OFF_THRESHOLD);
    Serial.printf("ADC=%.3f -> ledSensorOn=%s\n", ledAdcValue, ledSensorOn ? "true" : "false");
    publishMqttState();
  }
}

void pollButtons() {
  const unsigned long now = millis();

  for (size_t i = 0; i < (sizeof(buttons) / sizeof(buttons[0])); ++i) {
    ButtonState& button = buttons[i];
    const bool rawPressed = digitalRead(button.pin) == LOW;

    if (rawPressed != button.lastRawPressed) {
      button.lastRawPressed = rawPressed;
      button.lastChangeMs = now;
    }

    if ((now - button.lastChangeMs) >= BUTTON_DEBOUNCE_MS && rawPressed != button.stablePressed) {
      button.stablePressed = rawPressed;
      if (button.stablePressed) {
        applyFanSpeed(button.targetSpeed);
      }
    }
  }
}

String buildStatusJson() {
  String json = "{";
  json += "\"wifi_connected\":";
  json += WiFi.status() == WL_CONNECTED ? "true" : "false";
  json += ",\"fallback_ap_active\":";
  json += fallbackApActive ? "true" : "false";
  json += ",\"ip\":\"";
  json += (fallbackApActive ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
  json += "\",\"fan_speed\":";
  json += fanSpeed;
  json += ",\"led_adc\":";
  json += String(ledAdcValue, 3);
  json += ",\"led_sensor_on\":";
  json += ledSensorOn ? "true" : "false";
  json += "}";
  return json;
}

void handleRoot() {
  String html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>FOERNUFTIG</title><style>body{font-family:Arial,sans-serif;max-width:720px;margin:2rem auto;padding:0 1rem;}button{padding:.6rem 1rem;margin:.2rem;}</style></head><body>";
  html += "<h1>FOERNUFTIG Controller</h1>";
  html += "<p>WiFi mode: <strong>" + String(fallbackApActive ? "Fallback AP" : "Station") + "</strong></p>";
  html += "<p>Fan speed: <strong>" + String(fanSpeed) + "</strong></p>";
  html += "<p>ADC A0: <strong>" + String(ledAdcValue, 3) + "</strong> | Filter sensor: <strong>" + String(ledSensorOn ? "ON" : "OFF") + "</strong></p>";
  html += "<p><a href='/status'>JSON status</a></p>";
  html += "<p><a href='/fan?speed=0'><button>Off</button></a><a href='/fan?speed=1'><button>1</button></a><a href='/fan?speed=2'><button>2</button></a><a href='/fan?speed=3'><button>3</button></a></p>";
  html += "<p><a href='/restart'><button>Restart</button></a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleStatus() {
  server.send(200, "application/json", buildStatusJson());
}

void sendCaptiveRedirect() {
  const String target = String("http://") + WiFi.softAPIP().toString() + "/";
  server.sendHeader("Location", target, true);
  server.send(302, "text/plain", "");
}

void handleCaptiveProbe() {
  if (fallbackApActive) {
    sendCaptiveRedirect();
    return;
  }
  server.send(204, "text/plain", "");
}

void handleSetFan() {
  if (!server.hasArg("speed")) {
    server.send(400, "application/json", "{\"error\":\"missing speed\"}");
    return;
  }

  const int requestedSpeed = server.arg("speed").toInt();
  if (requestedSpeed < 0 || requestedSpeed > 3) {
    server.send(400, "application/json", "{\"error\":\"speed must be 0..3\"}");
    return;
  }

  applyFanSpeed(requestedSpeed);
  server.send(200, "application/json", buildStatusJson());
}

void handleRestart() {
  server.send(200, "text/plain", "Restarting...");
  delay(150);
  ESP.restart();
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/fan", HTTP_GET, handleSetFan);
  server.on("/restart", HTTP_GET, handleRestart);
  server.on("/generate_204", HTTP_GET, handleCaptiveProbe);
  server.on("/fwlink", HTTP_GET, handleCaptiveProbe);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveProbe);
  server.on("/ncsi.txt", HTTP_GET, handleCaptiveProbe);
  server.onNotFound([]() {
    if (fallbackApActive) {
      sendCaptiveRedirect();
      return;
    }
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
  Serial.println("Web server started on port 80");
}

void connectToWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print('.');
    ++attempts;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    return;
  }

  Serial.println();
  Serial.println("WiFi connection failed. Starting fallback AP...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASSWORD);
  fallbackApActive = true;
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.print("Fallback AP IP: ");
  Serial.println(WiFi.softAPIP());
}

// --- Safe-boot counter ---------------------------------------------------
void safeBootSetup() {
  EEPROM.begin(EEPROM_SIZE);

  const uint8_t magicHi = EEPROM.read(EEPROM_ADDR_MAGIC);
  const uint8_t magicLo = EEPROM.read(EEPROM_ADDR_MAGIC + 1);
  const bool valid = (magicHi == SAFE_BOOT_MAGIC_HI && magicLo == SAFE_BOOT_MAGIC_LO);

  uint8_t boots = valid ? EEPROM.read(EEPROM_ADDR_BOOTS) : 0;
  boots++;
  Serial.printf("Safe-boot counter: %u\n", boots);

  EEPROM.write(EEPROM_ADDR_MAGIC,     SAFE_BOOT_MAGIC_HI);
  EEPROM.write(EEPROM_ADDR_MAGIC + 1, SAFE_BOOT_MAGIC_LO);
  EEPROM.write(EEPROM_ADDR_BOOTS, boots);
  EEPROM.commit();

  if (boots >= SAFE_BOOT_THRESHOLD) {
    rescueMode = true;
    Serial.println("*** RESCUE MODE: only OTA active ***");
  }
}

void safeBootClear() {
  EEPROM.write(EEPROM_ADDR_BOOTS, 0);
  EEPROM.commit();
}

// --- OTA setup ------------------------------------------------------------
void setupOta() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);

  if (OTA_PASSWORD[0] != '\0') {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    // Suspend all application tasks so nothing interferes with the flash write
    mqttClient.disconnect();
    server.stop();
    if (fallbackApActive) {
      dnsServer.stop();
    }
    ESP.wdtDisable(); // disable soft-watchdog; ArduinoOTA feeds the HW watchdog itself
    Serial.println("OTA: starting flash…");
  });

  ArduinoOTA.onEnd([]() {
    ESP.wdtEnable(0);
    safeBootClear(); // new firmware booted OK → reset counter
    Serial.println("OTA: done — rebooting");
  });

  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    Serial.printf("OTA: %u%%\r", done * 100 / total);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    ESP.wdtEnable(0);
    const char* msg = "unknown";
    switch (error) {
      case OTA_AUTH_ERROR:    msg = "auth failed";    break;
      case OTA_BEGIN_ERROR:   msg = "begin failed";   break;
      case OTA_CONNECT_ERROR: msg = "connect failed"; break;
      case OTA_RECEIVE_ERROR: msg = "receive failed"; break;
      case OTA_END_ERROR:     msg = "end failed";     break;
    }
    Serial.printf("OTA error: %s\n", msg);
  });

  ArduinoOTA.begin();
  Serial.println("OTA ready");
}

void initMqttTopics() {
  topicBase = MQTT_BASE_TOPIC;
  topicFanSpeedState = topicBase + "/fan/speed/state";
  topicFanSpeedCommand = topicBase + "/fan/speed/set";
  topicAdcState = topicBase + "/sensor/adc/state";
  topicFilterState = topicBase + "/binary/filter/state";
  topicRestartCommand = topicBase + "/restart/set";
}

void publishMqttDiscovery() {
  if (!mqttClient.connected() || mqttDiscoveryPublished) {
    return;
  }

  const String node = DEVICE_NAME;
  const String name = DEVICE_FRIENDLY_NAME;
  const String availabilityTopic = topicBase + "/status";

  String speedCfg = "{";
  speedCfg += "\"name\":\"" + name + " Fan Speed\",";
  speedCfg += "\"uniq_id\":\"" + node + "_fan_speed\",";
  speedCfg += "\"stat_t\":\"" + topicFanSpeedState + "\",";
  speedCfg += "\"cmd_t\":\"" + topicFanSpeedCommand + "\",";
  speedCfg += "\"min\":0,\"max\":3,\"step\":1,";
  speedCfg += "\"avty_t\":\"" + availabilityTopic + "\",";
  speedCfg += "\"dev\":{\"ids\":[\"" + node + "\"],\"name\":\"" + name + "\",\"mf\":\"Custom\",\"mdl\":\"ESP8266\"}";
  speedCfg += "}";

  String adcCfg = "{";
  adcCfg += "\"name\":\"" + name + " LED ADC\",";
  adcCfg += "\"uniq_id\":\"" + node + "_led_adc\",";
  adcCfg += "\"stat_t\":\"" + topicAdcState + "\",";
  adcCfg += "\"unit_of_meas\":\"ratio\",";
  adcCfg += "\"avty_t\":\"" + availabilityTopic + "\",";
  adcCfg += "\"dev\":{\"ids\":[\"" + node + "\"]}";
  adcCfg += "}";

  String filterCfg = "{";
  filterCfg += "\"name\":\"" + name + " Filter\",";
  filterCfg += "\"uniq_id\":\"" + node + "_filter\",";
  filterCfg += "\"stat_t\":\"" + topicFilterState + "\",";
  filterCfg += "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",";
  filterCfg += "\"dev_cla\":\"problem\",";
  filterCfg += "\"avty_t\":\"" + availabilityTopic + "\",";
  filterCfg += "\"dev\":{\"ids\":[\"" + node + "\"]}";
  filterCfg += "}";

  String restartCfg = "{";
  restartCfg += "\"name\":\"" + name + " Restart\",";
  restartCfg += "\"uniq_id\":\"" + node + "_restart\",";
  restartCfg += "\"cmd_t\":\"" + topicRestartCommand + "\",";
  restartCfg += "\"pl_prs\":\"RESTART\",";
  restartCfg += "\"avty_t\":\"" + availabilityTopic + "\",";
  restartCfg += "\"dev\":{\"ids\":[\"" + node + "\"]}";
  restartCfg += "}";

  mqttClient.publish(("homeassistant/number/" + node + "/fan_speed/config").c_str(), speedCfg.c_str(), true);
  mqttClient.publish(("homeassistant/sensor/" + node + "/led_adc/config").c_str(), adcCfg.c_str(), true);
  mqttClient.publish(("homeassistant/binary_sensor/" + node + "/filter/config").c_str(), filterCfg.c_str(), true);
  mqttClient.publish(("homeassistant/button/" + node + "/restart/config").c_str(), restartCfg.c_str(), true);

  mqttDiscoveryPublished = true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; ++i) {
    message += static_cast<char>(payload[i]);
  }

  const String topicString(topic);
  if (topicString == topicFanSpeedCommand) {
    const int requested = message.toInt();
    if (requested >= 0 && requested <= 3) {
      applyFanSpeed(requested);
    }
    return;
  }

  if (topicString == topicRestartCommand && message == "RESTART") {
    ESP.restart();
  }
}

void setupMqtt() {
  if (!mqttEnabled()) {
    return;
  }

  initMqttTopics();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

void publishMqttState() {
  if (!mqttEnabled() || !mqttClient.connected()) {
    return;
  }

  if (fanSpeed != lastPublishedFanSpeed) {
    mqttClient.publish(topicFanSpeedState.c_str(), String(fanSpeed).c_str(), true);
    lastPublishedFanSpeed = fanSpeed;
  }

  if (lastPublishedAdc < 0.0f || fabsf(ledAdcValue - lastPublishedAdc) >= ADC_DELTA) {
    mqttClient.publish(topicAdcState.c_str(), String(ledAdcValue, 3).c_str(), true);
    lastPublishedAdc = ledAdcValue;
  }

  const int8_t ledState = ledSensorOn ? 1 : 0;
  if (ledState != lastPublishedLedSensorOn) {
    mqttClient.publish(topicFilterState.c_str(), ledSensorOn ? "ON" : "OFF", true);
    lastPublishedLedSensorOn = ledState;
  }
}

void ensureMqttConnected() {
  if (!mqttEnabled() || WiFi.status() != WL_CONNECTED || fallbackApActive) {
    return;
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  const unsigned long now = millis();
  if (now - lastMqttReconnectAttemptMs < 5000) {
    return;
  }
  lastMqttReconnectAttemptMs = now;

  const String clientId = String(DEVICE_NAME) + "-" + String(ESP.getChipId(), HEX);
  const String availabilityTopic = topicBase + "/status";

  bool connected = false;
  if (hasMqttAuth()) {
    connected = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD, availabilityTopic.c_str(), 0, true, "offline");
  } else {
    connected = mqttClient.connect(clientId.c_str(), availabilityTopic.c_str(), 0, true, "offline");
  }

  if (!connected) {
    return;
  }

  mqttClient.publish(availabilityTopic.c_str(), "online", true);
  mqttClient.subscribe(topicFanSpeedCommand.c_str());
  mqttClient.subscribe(topicRestartCommand.c_str());

  publishMqttDiscovery();
  publishMqttState();
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Booting...");

  safeBootSetup();

  if (rescueMode) {
    // Rescue mode: WiFi + OTA only — no application code that could crash again
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Rescue: connecting WiFi");
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
      // Last resort: open rescue AP
      WiFi.softAP("FOERNUFTIG-RESCUE", FALLBACK_AP_PASSWORD);
      Serial.println("Rescue AP: FOERNUFTIG-RESCUE");
    } else {
      Serial.printf("Rescue IP: %s\n", WiFi.localIP().toString().c_str());
    }
    setupOta();
    return; // skip all application setup
  }

  pinMode(PIN_OUT_D2, OUTPUT);
  pinMode(PIN_OUT_D3, OUTPUT);
  pinMode(PIN_OUT_D4, OUTPUT);
  pinMode(PIN_IN_D1, INPUT_PULLUP);
  pinMode(PIN_IN_D5, INPUT_PULLUP);
  pinMode(PIN_IN_D6, INPUT_PULLUP);
  pinMode(PIN_IN_D7, INPUT_PULLUP);

  applyFanSpeed(0);

  connectToWifi();
  if (WiFi.status() == WL_CONNECTED || fallbackApActive) {
    safeBootClear(); // WiFi kam hoch → sauberer Boot
  }
  setupOta();
  setupMqtt();
  setupWebServer();
}

void loop() {
  if (rescueMode) {
    ArduinoOTA.handle();
    return;
  }

  pollButtons();
  updateAdcSensor();

  if (fallbackApActive) {
    dnsServer.processNextRequest();
  }

  ensureMqttConnected();
  server.handleClient();
  ArduinoOTA.handle();
}
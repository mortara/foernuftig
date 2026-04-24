#pragma once

// Copy this file to include/secrets.local.h and put your real credentials there.
// If include/secrets.local.h exists it is preferred automatically.

static const char* WIFI_SSID = "WELAHN2G";
static const char* WIFI_PASSWORD = "dukommsthiernichtrein";

static const char* OTA_HOSTNAME = "foernuftig-esp8266";
static const char* OTA_PASSWORD = ""; // leer = kein Passwort (nicht empfohlen)
static const char* FALLBACK_AP_SSID = "Foernuftig Fallback Hotspot";
static const char* FALLBACK_AP_PASSWORD = "CHANGE_ME_1234";

static const char* DEVICE_NAME = "foernuftig";
static const char* DEVICE_FRIENDLY_NAME = "FOERNUFTIG";

// Leave MQTT_HOST empty to disable MQTT/Home Assistant discovery.
static const char* MQTT_HOST = "192.168.10.101";
static const uint16_t MQTT_PORT = 1883;
static const char* MQTT_USER = "homeassistant";
static const char* MQTT_PASSWORD = "hapake79";
static const char* MQTT_BASE_TOPIC = "foernuftig";

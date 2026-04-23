# foernuftig

ESP8266 replacement firmware for the FOERNUFTIG setup (without ESPHome).

## Features

- WiFi station mode with fallback AP
- OTA updates via ArduinoOTA
- Web UI on port 80
- Button-based fan control (D1/D5/D6/D7)
- 3-step fan output control (D2/D3/D4, inverted)
- ADC sensor sampling on A0 with delta filtering
- Optional MQTT + Home Assistant discovery

## Pin mapping

- Inputs:
	- D1: fan off
	- D5: fan speed 1
	- D6: fan speed 2
	- D7: fan speed 3
- Outputs (inverted logic):
	- D4: fan speed 1
	- D3: fan speed 2
	- D2: fan speed 3

## Configuration

1. Copy `include/secrets.local.h.example` to `include/secrets.local.h`.
2. Set your WiFi credentials.
3. Optionally set MQTT values for Home Assistant.

`include/secrets.local.h` is ignored by git.

If no local file exists, defaults from `include/secrets.h` are used.

## Web endpoints

- `/` status page with controls
- `/status` JSON status
- `/fan?speed=0..3` set fan speed
- `/restart` restart MCU

## Home Assistant via MQTT (optional)

Set `MQTT_HOST` in `include/secrets.local.h`.
When MQTT is enabled, the firmware publishes discovery for:

- Number entity: fan speed (0-3)
- Sensor entity: ADC value
- Binary sensor entity: filter status (`problem`)
- Button entity: restart
# Waveshare ESP32-S3-Touch-LCD-2 + INMP441
+ added ESP32-C5 configuration with a microphone and battery
ESPHome configuration for the **V1 board**: INMP441 microphone with a local
HTTP WAV stream (16 kHz, mono, PCM16), Home Assistant controls, and display
status. Optional camera preview at 1024×768.

## Wiring

Disconnect power before wiring. Use **3.3 V**, not 5 V. Disconnect any UART
adapter from GPIO43/44.

| INMP441 | ESP32-S3-Touch-LCD-2 V1 |
|---|---|
| VDD / VCC | 3V3 — P2 pin 1 |
| GND | GND — P2 pin 2 or 13 |
| SCK / BCLK | GPIO43 / TX — P2 pin 3 |
| WS / LRCLK | GPIO44 / RX — P2 pin 4 |
| SD | GPIO12 — P2 pin 10; **camera disconnected** |
| L/R | GND — left channel |

For **camera + microphone on V1 only**, connect SD to **GPIO18 — P1 pin 6**
instead, and select `mic_sd_pin: GPIO18` in `microphone-http.yaml`.
All other connections stay the same. GPIO12 is the default microphone-only
profile; GPIO18 enables the original camera and preview with a microphone
status overlay.

Pin reference: [Waveshare V1 schematic](https://files.waveshare.com/wiki/ESP32-S3-LCD-2/ESP32-S3-Touch-LCD-2-SchDoc.pdf).

## Installation

1. Extract all files together into your ESPHome configuration directory,
   preserving `components/` and `profiles/`.
2. Copy `secrets.example.yaml` to `secrets.yaml`, or add the missing entries
   to your existing secrets file. Enter your Wi-Fi details, OTA password,
   API encryption key, random stream token, and the LAN subnet containing
   Home Assistant and your playback PC. Keep `secrets.yaml` private.
   The API secret name is `esp_encryption_key`.
3. Select `GPIO12` or `GPIO18` under `substitutions.mic_sd_pin`.
4. Validate and compile with ESPHome **2026.10.0**:

   ```sh
   esphome config microphone-http.yaml
   esphome compile microphone-http.yaml
   ```

5. Install through ESPHome using your normal USB/OTA procedure, then add
   the device to Home Assistant through the ESPHome integration.
6. Turn on **Microphone Stream** in Home Assistant. It defaults to OFF
   after every restart. Start with **Microphone Gain = 1**.

Both profiles compile with ESPHome 2026.10.0 / ESP-IDF 5.5.6. Microphone-only
audio was confirmed working by the user; simultaneous camera/audio and
VisionState Live still require testing. No touchscreen or LVGL configuration
is included.

## Listen / connect VisionState

Use this stream URL with your device IP and private token:

```text
http://DEVICE_IP:8081/audio.wav?token=YOUR_TOKEN
```

PC playback:

```sh
ffplay -nodisp "http://DEVICE_IP:8081/audio.wav?token=YOUR_TOKEN"
```

Enter the same URL as the sound stream source in VisionState and check its
Live card. Stop PC playback first: **only one audio client is allowed**.
Disable clip recording in VisionState if no audio should be saved.

The ESP32 does not record audio to storage or upload it to a cloud service.
Access requires the token and a client address within `audio_allowed_subnet`.
Use a trusted LAN without public port forwarding; HTTP is unencrypted.
A red **MIC POSILA** display indicator shows active audio transmission.

Troubleshooting: **403** = client outside the allowed subnet; **401** = invalid
token; **409** = another audio client; **503** = stream switched off.

Generate a private token locally:

```sh
python -c "import secrets; print(secrets.token_urlsafe(32))"
```

<img width="1934" height="1401" alt="2465" src="https://github.com/user-attachments/assets/210b41da-fbcf-4d92-add3-fca94d39f57c" />
<img width="1512" height="2016" alt="2467" src="https://github.com/user-attachments/assets/20bb8c2c-a997-4db4-8822-14a4ec58372e" />



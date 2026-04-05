# ESP32-S3 Smart Keychain & Display

## About This Project
This project is an interactive, multi-functional smart display built around the **Seeed Studio XIAO ESP32S3**. Designed for a round TFT LCD (GC9A01A), it serves as a digital keychain or desk clock with several advanced features:
- **Real-Time Clock:** Synchronizes time over Wi-Fi (NTP) with an elegant analog/digital watch face.
- **Media Player:** Decodes and plays custom animations or images directly from PSRAM limits.
- **Web Media Uploader:** Built-in Captive Portal/Web Server to upload new media files over Wi-Fi.
- **Spotify Remote:** Acts as a BLE controller connecting to your phone, featuring live lyric tracking and media controls.
- **Touch Navigation:** Uses a single touch sensor to smoothly navigate the UI, featuring single-tap and double-tap gestures.

## TODO List
| Feature / Task | Status | Notes |
| :--- | :---: | :--- |
| Migrate core logic to XIAO ESP32S3 | ✅ Done | Updated pins and PSRAM configuration |
| Hardware Touch Sensor Integration | ✅ Done | Replaced physical BOOT button |
| Fix Spotify Remote Lyrics truncation | ✅ Done | Improved byte-handling and line wrap |
| Fix Background RTC Clock Sync | ✅ Done | Wi-Fi disconnect no longer freezes UI |
| Software Reboot RTC Wipe | ✅ Done | Fixed leftover broken state |
| Integrate SD Card Module | ✅ Done | Done |
| **Deep Sleep / Power Saving Mode** | ⏳ Pending | Maximize battery life (400mAh / 800mAh) |
|**MQTT Function** | ⏳ Pending | For Sending message via MQTT |
|**LVGL Ui Design** | ⏳ Pending | For better UI experience |

<div align="center">
  <h1 style="color:#e2a754;">This is FireFly !</h1>
</div>

<div align="center">
  <img src="https://github.com/Moocchi/Struktur_data/blob/main/%20Gif%20and%20Image/Firefly.gif" 
       style="width:75%;">
</div>
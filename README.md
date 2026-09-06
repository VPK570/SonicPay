# SonicPay 🔊💳

> **Offline Acoustic Micropayments via Encrypted Audio Chirps**  
> *36-Hour Hackathon Project*

SonicPay is an offline-first micropayment system that enables secure, contactless transactions without internet, cellular connectivity, or Bluetooth. It uses high-frequency **M-FSK (Multiple Frequency-Shift Keying)** audio chirps (12–15 kHz) to transmit Ed25519-signed transaction payloads from a mobile application to a receiver terminal (ESP32).

---

## 🌟 Key Features

- **📶 100% Offline**: No cellular data, Wi-Fi, internet connection, or Bluetooth pairing required.
- **🔐 Cryptographically Secure**: Transactions are signed on-device using **Ed25519** asymmetric cryptography (via `tweetnacl`).
- **🛡️ Replay Protection**: Monotonic nonce management persisted via `AsyncStorage` prevents transaction replay attacks.
- **🎵 Acoustic Data Transfer**: Data encoded into M-FSK audio tones in the 12 kHz – 15 kHz frequency band, designed to be audible yet masked in ambient room noise.
- **⚡ Real-Time Hardware Demodulation**: Hardware terminal running on ESP32 using the **Goertzel algorithm** for low-latency DSP tone detection and state-machine decoding.

---

## 🏗️ System Architecture

```
┌────────────────────────────────────────────────────────┐
│               SonicPay Mobile App (Client)             │
│  (React Native / Expo + expo-audio + tweetnacl)        │
└──────────────────────────┬─────────────────────────────┘
                           │
             High-Frequency Audio Chirp
             (M-FSK: 12 - 15 kHz Tones)
                           │
                           ▼
┌────────────────────────────────────────────────────────┐
│             SonicPay Receiver Terminal                 │
│      (ESP32 + ADC Microphone + Goertzel DSP)           │
└──────────────────────────┴─────────────────────────────┘
```

---

## 📜 Chirp Protocol Specification

| Parameter | Specification |
|---|---|
| **Encoding Scheme** | M-FSK (Multiple Frequency-Shift Keying) |
| **Tone Frequencies** | 8 tones: `12.0kHz`, `12.4kHz`, `12.8kHz`, `13.2kHz`, `13.6kHz`, `14.0kHz`, `14.4kHz`, `14.8kHz` |
| **Symbol Capacity** | 3 bits per symbol (8 distinct tones) |
| **Symbol Duration** | 80 ms per symbol (3,200 samples @ 40 kHz) |
| **Preamble** | 3 consecutive F0 (`12.0 kHz`) symbols |
| **Postamble** | 2 consecutive F0 (`12.0 kHz`) symbols |
| **Payload Format** | `Merchant ID` + `Amount` + `Timestamp` + `Monotonic Nonce` + `Ed25519 Signature` |

---

## 📂 Repository Layout

```
.
├── SonicPay/                   # Mobile Client Application (React Native / Expo)
│   ├── App.tsx                 # Entry Point
│   ├── screens/                # UI Screens (HomeScreen keypad & transaction view)
│   ├── hooks/                  # Logic Hooks (useChirp, useCrypto, useNonce)
│   └── package.json            # React Native dependencies
├── hardware/                   # Hardware Terminal Firmware (ESP32 ESP-IDF)
│   ├── main/main.c             # FreeRTOS tasks, ADC sampling, Goertzel algorithm & state machine
│   └── CMakeLists.txt          # Build system configuration
├── docs/                       # Project Documentation & Specifications
│   └── ROADMAP.md              # Hackathon build roadmap
├── AGENTS.md                   # Hackathon build protocol & guidelines
└── README.md                   # Project overview & documentation
```

---

## 🚀 Getting Started

### 📱 Mobile App Setup (`SonicPay`)

**Prerequisites:** Node.js (v18+) and Expo Go / Android Studio / Xcode.

1. Navigate to the app directory:
   ```bash
   cd SonicPay
   ```

2. Install dependencies:
   ```bash
   npm install
   ```

3. Start the Expo development server:
   ```bash
   npx expo start
   ```

4. Scan the QR code using Expo Go on your mobile device (ensure speaker volume is set adequately).

---

### 📟 Hardware Receiver Setup (`hardware`)

**Prerequisites:** ESP-IDF v5.x development framework configured for ESP32.

1. Connect your ESP32 board equipped with an analog microphone module on ADC1 Channel 6.

2. Navigate to the hardware directory:
   ```bash
   cd hardware
   ```

3. Build and flash the firmware:
   ```bash
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

4. The serial monitor will output real-time Goertzel tone detections, preamble locking status, and decoded hex/raw payload strings upon receiving a chirp.

---

## 🛠️ Tech Stack

- **Mobile Client**: React Native, Expo Managed Workflow, `expo-audio`, `tweetnacl`, `@react-native-async-storage/async-storage`
- **Hardware Firmware**: ESP32 C (ESP-IDF v5.x), FreeRTOS, ESP32 ADC Driver, GPTimer, Goertzel Algorithm DSP

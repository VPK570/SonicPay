# SonicPay — Build Roadmap

Customer-side React Native (Expo) app that generates encrypted audio chirps for offline payments, paired with an ESP32 hardware payment terminal.

---

## Piece 1 — Project Scaffold + Keypad UI

- [x] Expo managed workflow initialized
- [x] HomeScreen with amount entry keypad
- [x] Send Payment button & teal sleek UI design

**Deps:** expo, react-native
**Gotcha:** Clean responsive layout with modern UPI-style UX.

---

## Piece 2 — Nonce Management (AsyncStorage)

- [x] Install `@react-native-async-storage/async-storage`
- [x] Nonce store: load, increment, persist
- [x] Reset nonce on corruption (try/catch fallback)
- [x] Nonce displayed on screen for debug / transaction tracking

**Deps:** `@react-native-async-storage/async-storage`
**Gotcha:** AsyncStorage is async — wrap in try/catch. Handle corruption gracefully (reset to 0 on error).

---

## Piece 3 — Ed25519 Key Generation + Transaction Signing

- [x] Install `tweetnacl`
- [x] Generate keypair on launch, persist pubkey to AsyncStorage
- [x] Sign transaction payload: `amount_paise + nonce` with 64-byte Ed25519 signature
- [x] Verify detached signature (64 bytes) matches ESP32 Monocypher cryptographic verification
- [x] Display pubkey (truncated) on screen for debug

**Deps:** `tweetnacl`
**Gotcha:** `sign.detached()` returns 64-byte signature — matches ESP32 Monocypher expectations.

---

## Piece 4 — M-FSK Chirp Generation

- [x] Install `expo-audio` / PCM WAV audio generation
- [x] Implement 8-FSK encoder: map 3 bits per symbol to frequency tones
- [x] Frequency range: Audio Frequencies (2050–3600 Hz) / 12–15 kHz configurable
- [x] Generate audio WAV buffer with tone sequence & preamble/postamble sync
- [x] Set audio mode: speaker output via `setAudioModeAsync()`
- [x] Play chirp, verify audible output

**Deps:** `expo-audio`
**Gotcha:** `setAudioModeAsync()` must be called before playback.

---

## Piece 5 — Full Transaction Flow & Hardware Terminal Integration

- [x] Wire keypad → nonce → sign → encode → chirp
- [x] Status feedback: signing... → encoding... → playing... → done
- [x] Error handling: failed sign, audio init failure, etc.
- [x] Transaction summary display (amount, pubkey, nonce, timestamp)
- [x] Hardware ESP32 terminal firmware with Goertzel DSP, Monocypher crypto, SPIFFS ledger persistence, and SSD1306 OLED UI

**Deps:** Pieces 2, 3, 4 + ESP32 C firmware
**Gotcha:** No internet — all verification and transaction processing happens 100% on-device.

---

## Piece 6 — Polish & Terminal Integration

- [x] Sleek multi-screen state transitions (Keypad → Transmitting Chirp → Success / Error)
- [x] Nonce-based replay attack rejection on hardware terminal
- [x] SSD1306 OLED display status feedback, LED status indicator, piezoceramic audio feedback on terminal
- [x] SPIFFS ledger storage for flash-persistent transaction logs on hardware terminal

---

## Tech Stack

| Component | Choice | Why |
|-----------|--------|-----|
| Mobile Framework | React Native + Expo (managed) | Cross-platform rapid UI & audio playback |
| Mobile Crypto | tweetnacl | Lightweight Ed25519 signing |
| Mobile Storage | AsyncStorage | Local nonce persistence |
| Hardware Terminal | ESP32 (ESP-IDF v5.x) | Low-power microcontroller with hardware ADC & timers |
| Hardware DSP | Goertzel Algorithm | Fast multi-frequency tone detection without full FFT overhead |
| Hardware Crypto | Monocypher | C-native Ed25519 signature verification |
| Hardware Display | SSD1306 OLED (I2C) | Clear real-time status output for payment terminal |
| Hardware Storage | SPIFFS Flash Ledger | On-device persistent transaction ledger |

---

## Chirp Format

```
Payload = Amount (2 bytes) + Nonce (2 bytes) + Ed25519 Signature (64 bytes) + CRC16 (2 bytes)
Encoding = 8-FSK (3 bits per symbol, 187 data symbols + preamble/postamble)
```

Terminal-side ESP32 demodulates via Goertzel DSP and validates cryptographic signatures for offline transactions.


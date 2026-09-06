# Agents — Acoustic Micropayments Terminal

36-hour hackathon. Customer-side React Native (Expo) app that generates encrypted audio chirps for offline payments.

## Build Protocol

Build piece-by-piece. **Stop after each piece and wait for user confirmation before proceeding.**

| # | Piece | What it proves |
|---|-------|----------------|
| 1 | Project scaffold + amount entry UI | Expo boots, UI renders |
| 2 | Nonce management (AsyncStorage) | Persistence works across app restarts |
| 3 | Ed25519 key gen + transaction signing | Crypto produces verifiable signatures |
| 4 | M-FSK chirp generation | Tones encode/decode bits, audible output works |
| 5 | Full transaction flow | Sign → encode → chirp → status feedback |
| 6 | Polish | Animations, error states, replay demo mode |

## Tech Stack

- **React Native + Expo** (managed workflow)
- **expo-audio** for audio output (hooks-based API: `useAudioPlayer`, replaces deprecated expo-av)
- **tweetnacl** for Ed25519 signing (lighter than noble-ed25519, hackathon-friendly)
- **@react-native-async-storage/async-storage** for nonce persistence
- **No backend.** All logic on-device.

## Chirp Encoding

- **M-FSK** (Multiple Frequency-Shift Keying)
- **Frequency range: 12–15 kHz** (audible but masked in noisy environments)
- **Payload:** Merchant ID + Amount + Timestamp + Monotonic nonce + Ed25519 signature
- Terminal-side ESP32 demodulates via FFT — chirp format must match firmware expectations

## Gotchas

- **expo-audio** requires `setAudioModeAsync()` before playback — set speaker output, not earpiece
- **tweetnacl** `sign.detached()` returns 64-byte signature — ESP32 firmware expects this exact size
- **AsyncStorage** is async — wrap in try/catch, handle corruption (reset nonce on error)
- **No internet.** Don't import anything that phones home. Expo dev tools will want connectivity — ignore.
- **12-15 kHz** is above typical speech range but test with real speakers — cheap phone speakers roll off above 14 kHz

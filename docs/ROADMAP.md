# SonicPay — Build Roadmap

Customer-side React Native (Expo) app that generates encrypted audio chirps for offline payments.

---

## Piece 1 — Project Scaffold + Keypad UI

- [x] Expo managed workflow initialized
- [x] HomeScreen with amount entry keypad
- [x] Send Payment button (placeholder)

**Deps:** expo, react-native
**Gotcha:** None yet — pure UI.

---

## Piece 2 — Nonce Management (AsyncStorage)

- [ ] Install `@react-native-async-storage/async-storage`
- [ ] Nonce store: load, increment, persist
- [ ] Reset nonce on corruption (try/catch fallback)
- [ ] Nonce displayed on screen for debug

**Deps:** `@react-native-async-storage/async-storage`
**Gotcha:** AsyncStorage is async — wrap in try/catch. Handle corruption gracefully (reset to 0 on error).

---

## Piece 3 — Ed25519 Key Generation + Transaction Signing

- [ ] Install `tweetnacl`
- [ ] Generate keypair on first launch, persist pubkey to AsyncStorage
- [ ] Sign transaction payload: `merchantId + amount + timestamp + nonce`
- [ ] Verify detached signature is 64 bytes (ESP32 firmware expects this)
- [ ] Display pubkey (truncated) on screen for debug

**Deps:** `tweetnacl`
**Gotcha:** `sign.detached()` returns 64-byte signature — must match ESP32 expectations exactly.

---

## Piece 4 — M-FSK Chirp Generation

- [ ] Install `expo-audio` (hooks-based API: `useAudioPlayer`)
- [ ] Implement M-FSK encoder: map bits to frequency tones
- [ ] Frequency range: 12–15 kHz (audible but masked in noisy envs)
- [ ] Generate audio buffer with tone sequence
- [ ] Set audio mode: speaker output (not earpiece) via `setAudioModeAsync()`
- [ ] Play chirp, verify audible output

**Deps:** `expo-audio`
**Gotcha:** `setAudioModeAsync()` must be called before playback. 12-15 kHz rolls off on cheap phone speakers — test with real hardware.

---

## Piece 5 — Full Transaction Flow

- [ ] Wire keypad → nonce → sign → encode → chirp
- [ ] Status feedback: signing... → encoding... → playing... → done
- [ ] Error handling: failed sign, audio init failure, etc.
- [ ] Transaction summary display (amount, pubkey, nonce)

**Deps:** Pieces 2, 3, 4
**Gotcha:** No internet — don't import anything that phones home. Expo dev tools will want connectivity.

---

## Piece 6 — Polish

- [ ] Animations (button press, status transitions)
- [ ] Error states with recovery
- [ ] Replay attack demo mode for judges (replay same chirp, show nonce rejection)
- [ ] Visual feedback during chirp playback (waveform/indicator)

**No new deps.** Pure UI/logic polish.

---

## Tech Stack

| Component | Choice | Why |
|-----------|--------|-----|
| Framework | React Native + Expo (managed) | Hackathon speed |
| Audio | expo-audio | Hooks-based, replaces deprecated expo-av |
| Crypto | tweetnacl | Ed25519, lighter than noble-ed25519 |
| Storage | AsyncStorage | Simple key-value, no setup |
| Backend | None | All logic on-device |

## Chirp Format

```
Payload = MerchantID + Amount + Timestamp + Nonce + Ed25519 Signature
Encoding = M-FSK (12–15 kHz)
```

Terminal-side ESP32 demodulates via FFT — chirp format must match firmware expectations.

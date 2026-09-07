import { useCallback, useRef, useEffect } from 'react';
import { createAudioPlayer, setAudioModeAsync } from 'expo-audio';
import { File, Paths } from 'expo-file-system';

// ---------------------------------------------------------------------------
// 8-FSK Acoustic Modem — SonicPay v2
//
// Frequency plan (9 tones total):
//   F0 = 2050 Hz  — sync only (preamble + postamble, never data)
//   F1 = 2200 Hz  — data value 0  (bits 000)
//   F2 = 2400 Hz  — data value 1  (bits 001)
//   F3 = 2600 Hz  — data value 2  (bits 010)
//   F4 = 2800 Hz  — data value 3  (bits 011)
//   F5 = 3000 Hz  — data value 4  (bits 100)
//   F6 = 3200 Hz  — data value 5  (bits 101)
//   F7 = 3400 Hz  — data value 6  (bits 110)
//   F8 = 3600 Hz  — data value 7  (bits 111)
//
// Packet: [amount:2][nonce:2][signature:64][crc:2] = 70 bytes = 560 bits
// 8-FSK: 3 bits per symbol → ceil(560/3) = 187, rounded up to 188 for clean boundary
// Symbol duration : 50 ms
// Guard interval  : 10 ms silent between symbols
// Preamble        : 3 consecutive F0 hits
// Postamble       : 2 consecutive F0 hits
// ---------------------------------------------------------------------------

const SAMPLE_RATE = 44100; // Phone WAV sample rate (ESP32 ADC is 36096 — separate)

const PREAMBLE_FREQ = 2050; // F0 — sync tone
const DATA_FREQS = [        // F1–F8 — data tones, index = 3-bit symbol value (0–7)
  2200, // 000
  2400, // 001
  2600, // 010
  2800, // 011
  3000, // 100
  3200, // 101
  3400, // 110
  3600, // 111
];

const SYMBOL_DURATION_MS = 50; // ms per data/sync symbol
const GUARD_DURATION_MS  = 10; // ms silent guard between symbols

// 70-byte packet → 560 bits → ceil(560/3) = 187 symbols
const PACKET_BYTES  = 70;
const TOTAL_BITS    = PACKET_BYTES * 8; // 560
const BITS_PER_SYM  = 3;
const NUM_DATA_SYMS = Math.ceil(TOTAL_BITS / BITS_PER_SYM); // 187

// ---------------------------------------------------------------------------
// CRC-16/CCITT — exported so HomeScreen can compute packet CRC
// ---------------------------------------------------------------------------
export function crc16_ccitt(data: Uint8Array): number {
  let crc = 0xffff;
  for (let i = 0; i < data.length; i++) {
    crc ^= data[i] << 8;
    for (let j = 0; j < 8; j++) {
      if ((crc & 0x8000) !== 0) {
        crc = ((crc << 1) ^ 0x1021) & 0xffff;
      } else {
        crc = (crc << 1) & 0xffff;
      }
    }
  }
  return crc & 0xffff;
}

// ---------------------------------------------------------------------------
// Bit helpers
// ---------------------------------------------------------------------------

/** Convert a Uint8Array to an array of bits (MSB-first per byte). */
function bytesToBits(bytes: Uint8Array): number[] {
  const bits: number[] = [];
  for (let i = 0; i < bytes.length; i++) {
    const byte = bytes[i];
    for (let b = 7; b >= 0; b--) {
      bits.push((byte >> b) & 1);
    }
  }
  return bits;
}

/**
 * Pack bits into 8-FSK symbols (3 bits each).
 * The bit array is padded with trailing zeros to a multiple of 3.
 * Returns exactly NUM_DATA_SYMS (188) symbols for a 70-byte packet.
 */
function bitsToSymbols8(bits: number[]): number[] {
  // Pad to next multiple of BITS_PER_SYM
  const padded = [...bits];
  while (padded.length % BITS_PER_SYM !== 0) padded.push(0);

  const symbols: number[] = [];
  for (let i = 0; i < padded.length; i += BITS_PER_SYM) {
    const val = (padded[i] << 2) | (padded[i + 1] << 1) | padded[i + 2];
    symbols.push(val); // 0–7
  }
  return symbols;
}

// ---------------------------------------------------------------------------
// Audio synthesis
// ---------------------------------------------------------------------------

/**
 * Generate a single tone with a 5 ms raised-cosine fade in/out to reduce
 * spectral splatter at symbol boundaries.
 */
function generateTone(freq: number, durationMs: number, sampleRate: number): Float32Array {
  const numSamples  = Math.floor(sampleRate * durationMs / 1000);
  const samples     = new Float32Array(numSamples);
  const rampSamples = Math.floor(sampleRate * 0.005); // 5 ms ramp

  for (let i = 0; i < numSamples; i++) {
    let amp = 1.0;
    if (i < rampSamples) {
      amp = 0.5 * (1 - Math.cos(Math.PI * i / rampSamples));
    } else if (i > numSamples - rampSamples) {
      amp = 0.5 * (1 - Math.cos(Math.PI * (numSamples - i) / rampSamples));
    }
    samples[i] = amp * Math.sin(2 * Math.PI * freq * i / sampleRate);
  }
  return samples;
}

function generateSilence(durationMs: number, sampleRate: number): Float32Array {
  return new Float32Array(Math.floor(sampleRate * durationMs / 1000));
}

// ---------------------------------------------------------------------------
// WAV builder — 16-bit PCM, mono
// ---------------------------------------------------------------------------
function buildWAV(samples: Float32Array, sampleRate: number): string {
  const numSamples = samples.length;
  const dataSize   = numSamples * 2; // 16-bit samples
  const buffer     = new ArrayBuffer(44 + dataSize);
  const view       = new DataView(buffer);

  const writeStr = (offset: number, str: string) => {
    for (let i = 0; i < str.length; i++) view.setUint8(offset + i, str.charCodeAt(i));
  };

  writeStr(0, 'RIFF');
  view.setUint32(4, 36 + dataSize, true);
  writeStr(8, 'WAVE');
  writeStr(12, 'fmt ');
  view.setUint32(16, 16, true);           // PCM chunk size
  view.setUint16(20, 1, true);            // PCM format
  view.setUint16(22, 1, true);            // mono
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true); // byte rate
  view.setUint16(32, 2, true);            // block align
  view.setUint16(34, 16, true);           // bits per sample
  writeStr(36, 'data');
  view.setUint32(40, dataSize, true);

  for (let i = 0; i < numSamples; i++) {
    const s = Math.max(-1, Math.min(1, samples[i]));
    view.setInt16(44 + i * 2, s < 0 ? s * 0x8000 : s * 0x7fff, true);
  }

  // Encode to base64 in 8 kB chunks to avoid stack overflow on large payloads
  const bytes = new Uint8Array(buffer);
  let binary = '';
  for (let i = 0; i < bytes.length; i += 8192) {
    binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
  }
  return btoa(binary);
}

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------
export function useChirp() {
  const playingRef  = useRef(false);
  const playerRef   = useRef<any>(null);
  const intervalRef = useRef<ReturnType<typeof setInterval> | null>(null);

  useEffect(() => {
    return () => {
      if (intervalRef.current) clearInterval(intervalRef.current);
      if (playerRef.current) {
        playerRef.current.remove();
        playerRef.current = null;
      }
    };
  }, []);

  /**
   * Encode and play a 70-byte SonicPay packet as an 8-FSK chirp.
   *
   * Packet layout (caller's responsibility):
   *   [amount:2][nonce:2][signature:64][crc:2] = 70 bytes
   *
   * Acoustic structure:
   *   3 × F0 sync  →  188 × data symbols  →  2 × F0 sync
   *   Each slot = 50 ms tone + 10 ms silence
   *
   * Total transmission time:
   *   193 slots × 60 ms = 11.58 s
   */
  const playChirp = useCallback(async (payload: Uint8Array) => {
    if (payload.length !== PACKET_BYTES) {
      throw new Error(`[useChirp] Expected ${PACKET_BYTES}-byte packet, got ${payload.length}`);
    }
    if (playingRef.current) return;
    playingRef.current = true;

    try {
      const bits    = bytesToBits(payload);
      const symbols = bitsToSymbols8(bits); // → 188 symbols

      // Pre-compute slot sizes
      const toneSamples  = Math.floor(SAMPLE_RATE * SYMBOL_DURATION_MS / 1000); // 2205
      const guardSamples = Math.floor(SAMPLE_RATE * GUARD_DURATION_MS  / 1000); // 441
      const slotSamples  = toneSamples + guardSamples;                           // 2646

      const numSlots     = 6 + NUM_DATA_SYMS + 2; // 6 pre + 187 data + 2 post = 195
      const totalSamples = numSlots * slotSamples;
      const allSamples   = new Float32Array(totalSamples);

      let offset = 0;

      const writeSlot = (freq: number) => {
        const tone  = generateTone(freq, SYMBOL_DURATION_MS, SAMPLE_RATE);
        const guard = generateSilence(GUARD_DURATION_MS, SAMPLE_RATE);
        allSamples.set(tone, offset);  offset += tone.length;
        allSamples.set(guard, offset); offset += guard.length;
      };

      // Preamble: 6 × F0 (2050 Hz) for robust sync lock
      for (let p = 0; p < 6; p++) writeSlot(PREAMBLE_FREQ);

      // Data: 188 × 8-FSK symbols mapped to F1–F8 (2200–3600 Hz)
      for (const sym of symbols) writeSlot(DATA_FREQS[sym]);

      // Postamble: 2 × F0 (2050 Hz)
      for (let p = 0; p < 2; p++) writeSlot(PREAMBLE_FREQ);

      const wavBase64 = buildWAV(allSamples, SAMPLE_RATE);
      console.log(`[SonicPay System Log] WAV generated: ${allSamples.length} samples (${(allSamples.length / SAMPLE_RATE).toFixed(2)}s)`);

      // Write WAV to cache and play via expo-audio
      const file = new File(Paths.cache, 'chirp.wav');
      await file.write(wavBase64, { encoding: 'base64' });
      console.log(`[SonicPay System Log] Saved WAV to cache: ${file.uri}`);

      await setAudioModeAsync({
        playsInSilentMode: true,
        allowsRecording:   false,
      });

      let player: any = null;
      try {
        player = createAudioPlayer(file.uri);
      } catch {
        player = createAudioPlayer({ uri: file.uri });
      }
      playerRef.current = player;
      player.volume = 1.0;
      player.play();
      console.log(`[SonicPay System Log] Audio playback started.`);

      // Poll until playback actually starts, then poll until it ends
      await new Promise<void>((resolve) => {
        let hasStarted = false;
        const startTime = Date.now();
        intervalRef.current = setInterval(() => {
          if (player.playing) {
            hasStarted = true;
          }
          // If player has started and now stopped, OR if 15 seconds have passed (safety timeout)
          if ((hasStarted && !player.playing) || (Date.now() - startTime > 15000)) {
            if (intervalRef.current) clearInterval(intervalRef.current);
            intervalRef.current = null;
            try { player.remove(); } catch {}
            playerRef.current = null;
            console.log(`[SonicPay System Log] Audio playback finished.`);
            resolve();
          }
        }, 100);
      });
    } catch (err: any) {
      console.error('[SonicPay System Log] Audio playback failed:', err?.message || String(err), err?.stack || '');
      if (intervalRef.current) { clearInterval(intervalRef.current); intervalRef.current = null; }
      if (playerRef.current)   { try { playerRef.current.remove(); } catch {} playerRef.current = null; }
      throw err;
    } finally {
      playingRef.current = false;
    }
  }, []);

  return { playChirp };
}

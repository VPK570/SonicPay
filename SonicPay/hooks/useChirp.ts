import { useCallback, useRef, useEffect } from 'react';
import { createAudioPlayer, setAudioModeAsync } from 'expo-audio';
import { File, Paths } from 'expo-file-system';

const SAMPLE_RATE = 44100;
// 8-FSK tones matching ESP32 Piece 3 firmware (12000 - 14800 Hz, 400 Hz step)
const FREQS = [12000, 12400, 12800, 13200, 13600, 14000, 14400, 14800];
const PREAMBLE_FREQ = 12000; // F0 tone
const SYMBOL_DURATION_MS = 80;  // must match firmware SYMBOL_SAMPLES / SAMPLE_RATE
const SYMBOL_SAMPLES = Math.floor(SAMPLE_RATE * SYMBOL_DURATION_MS / 1000);
const PREAMBLE_MS = 240; // 3 symbols × 80ms — must match firmware PREAMBLE_SYMS
const POSTAMBLE_MS = 160; // 2 symbols × 80ms — must match firmware POSTAMBLE_SYMS

function textToBits(str: string): number[] {
  const bits: number[] = [];
  for (let i = 0; i < str.length; i++) {
    const byte = str.charCodeAt(i) & 0xff;
    for (let b = 7; b >= 0; b--) {
      bits.push((byte >> b) & 1);
    }
  }
  return bits;
}

function bitsToSymbols(bits: number[]): number[] {
  const symbols: number[] = [];
  for (let i = 0; i < bits.length; i += 3) {
    let val = 0;
    for (let j = 0; j < 3; j++) {
      val = (val << 1) | (bits[i + j] || 0);
    }
    symbols.push(val);
  }
  return symbols;
}

function generateTone(freq: number, durationMs: number, sampleRate: number): Float32Array {
  const numSamples = Math.floor(sampleRate * durationMs / 1000);
  const samples = new Float32Array(numSamples);
  for (let i = 0; i < numSamples; i++) {
    samples[i] = Math.sin(2 * Math.PI * freq * i / sampleRate);
  }
  return samples;
}

function buildWAV(samples: Float32Array, sampleRate: number): string {
  const numSamples = samples.length;
  const dataSize = numSamples * 2;
  const buffer = new ArrayBuffer(44 + dataSize);
  const view = new DataView(buffer);

  const writeString = (offset: number, str: string) => {
    for (let i = 0; i < str.length; i++) {
      view.setUint8(offset + i, str.charCodeAt(i));
    }
  };

  writeString(0, 'RIFF');
  view.setUint32(4, 36 + dataSize, true);
  writeString(8, 'WAVE');
  writeString(12, 'fmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeString(36, 'data');
  view.setUint32(40, dataSize, true);

  for (let i = 0; i < numSamples; i++) {
    const s = Math.max(-1, Math.min(1, samples[i]));
    view.setInt16(44 + i * 2, s < 0 ? s * 0x8000 : s * 0x7fff, true);
  }

  const bytes = new Uint8Array(buffer);
  let binary = '';
  for (let i = 0; i < bytes.length; i += 8192) {
    binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
  }
  return btoa(binary);
}

export function useChirp() {
  const playingRef = useRef(false);
  const playerRef = useRef<any>(null);
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

  const playChirp = useCallback(async (base64Payload: string) => {
    if (playingRef.current) return;
    playingRef.current = true;

    try {
      const bits = textToBits(base64Payload);
      const symbols = bitsToSymbols(bits);

      const preamble = generateTone(PREAMBLE_FREQ, PREAMBLE_MS, SAMPLE_RATE);
      const postamble = generateTone(PREAMBLE_FREQ, POSTAMBLE_MS, SAMPLE_RATE);

      const totalSamples = preamble.length + symbols.length * SYMBOL_SAMPLES + postamble.length;
      const allSamples = new Float32Array(totalSamples);

      let offset = 0;
      allSamples.set(preamble, offset);
      offset += preamble.length;

      for (const sym of symbols) {
        const tone = generateTone(FREQS[sym], SYMBOL_DURATION_MS, SAMPLE_RATE);
        allSamples.set(tone, offset);
        offset += tone.length;
      }

      allSamples.set(postamble, offset);

      const wavBase64 = buildWAV(allSamples, SAMPLE_RATE);

      const file = new File(Paths.cache, 'chirp.wav');
      await file.write(wavBase64, { encoding: 'base64' });

      await setAudioModeAsync({
        playsInSilentMode: true,
        allowsRecording: false,
      });

      const player = createAudioPlayer({ uri: file.uri });
      playerRef.current = player;
      player.volume = 1.0;
      player.play();

      await new Promise<void>((resolve) => {
        intervalRef.current = setInterval(() => {
          if (!player.playing) {
            if (intervalRef.current) clearInterval(intervalRef.current);
            intervalRef.current = null;
            player.remove();
            playerRef.current = null;
            resolve();
          }
        }, 50);
      });
    } catch (err) {
      console.error('Chirp playback failed:', err);
      if (intervalRef.current) {
        clearInterval(intervalRef.current);
        intervalRef.current = null;
      }
      if (playerRef.current) {
        playerRef.current.remove();
        playerRef.current = null;
      }
    } finally {
      playingRef.current = false;
    }
  }, []);

  return { playChirp };
}

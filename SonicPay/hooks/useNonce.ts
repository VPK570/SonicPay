import { useState, useEffect, useRef, useCallback } from 'react';
import AsyncStorage from '@react-native-async-storage/async-storage';

const NONCE_KEY = 'sonicpay_nonce';

/**
 * Monotonic u16 nonce persisted in AsyncStorage.
 *
 * Race-condition fix: nonce starts as null and `nonceLoaded` stays false until
 * the AsyncStorage read resolves. HomeScreen must check nonceLoaded before
 * allowing transmission — incrementNonce() will throw if called too early.
 */
export function useNonce() {
  // null = AsyncStorage not yet read (blocks transmission)
  const [nonce, setNonce] = useState<number | null>(null);
  const nonceRef = useRef<number | null>(null);

  useEffect(() => {
    (async () => {
      let initial = 0;
      try {
        const value = await AsyncStorage.getItem(NONCE_KEY);
        if (value !== null) {
          const parsed = parseInt(value, 10);
          if (!isNaN(parsed) && parsed >= 0) {
            initial = parsed;
          } else {
            // Corrupted value — reset to 0
            console.warn('[useNonce] Corrupted nonce in storage, resetting to 0');
          }
        }
        // First-run: value is null, initial stays 0
      } catch (e) {
        // AsyncStorage error — reset to 0
        console.warn('[useNonce] AsyncStorage read error, resetting nonce:', e);
        initial = 0;
      }

      nonceRef.current = initial;
      setNonce(initial);

      // Persist the (possibly reset) value so next boot reads cleanly
      try {
        await AsyncStorage.setItem(NONCE_KEY, String(initial));
      } catch (e) {
        console.warn('[useNonce] Failed to persist initial nonce:', e);
      }
    })();
  }, []);

  /**
   * Atomically increment and persist the nonce.
   * Returns the new nonce value (u16, wraps at 65535 → 0).
   * Throws if AsyncStorage has not yet resolved.
   */
  const incrementNonce = useCallback(async (): Promise<number> => {
    if (nonceRef.current === null) {
      throw new Error('[useNonce] Nonce not loaded — wait for nonceLoaded before sending');
    }
    const next = (nonceRef.current + 1) & 0xffff; // u16 wrap
    nonceRef.current = next;
    setNonce(next);
    try {
      await AsyncStorage.setItem(NONCE_KEY, String(next));
    } catch (e) {
      console.warn('[useNonce] Failed to persist nonce:', e);
    }
    return next;
  }, []);

  return {
    nonce: nonce ?? 0,
    nonceLoaded: nonce !== null,
    incrementNonce,
  };
}

import { useState, useEffect, useRef, useCallback } from 'react';
import AsyncStorage from '@react-native-async-storage/async-storage';

const NONCE_KEY = 'sonicpay_nonce';

export function useNonce() {
  const [nonce, setNonce] = useState(1);
  const nonceRef = useRef(1);

  useEffect(() => {
    AsyncStorage.getItem(NONCE_KEY).then(value => {
      if (value !== null) {
        const n = parseInt(value, 10);
        if (!isNaN(n)) {
          nonceRef.current = n;
          setNonce(n);
        }
      }
    });
  }, []);

  const incrementNonce = useCallback(async () => {
    const next = nonceRef.current + 1;
    nonceRef.current = next;
    setNonce(next);
    await AsyncStorage.setItem(NONCE_KEY, String(next));
    return next;
  }, []);

  return { incrementNonce, nonce };
}

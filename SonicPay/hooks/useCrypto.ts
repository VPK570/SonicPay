import { useState, useEffect, useCallback } from 'react';
import AsyncStorage from '@react-native-async-storage/async-storage';
import nacl from 'tweetnacl';
import util from 'tweetnacl-util';
import { getRandomValues } from 'expo-crypto';

// ponytail: tweetnacl auto-detect fails on Hermes, wire PRNG manually
nacl.setPRNG((x, n) => {
  const v = new Uint8Array(n);
  getRandomValues(v);
  for (let i = 0; i < n; i++) x[i] = v[i];
});

const PUBKEY_KEY    = 'sonicpay_pubkey';
const SECRETKEY_KEY = 'sonicpay_secretkey';

export function useCrypto() {
  const [keypair, setKeypair] = useState<nacl.SignKeyPair | null>(null);

  useEffect(() => {
    (async () => {
      try {
        const [pubB64, secB64] = await AsyncStorage.multiGet([PUBKEY_KEY, SECRETKEY_KEY]);
        if (pubB64[1] && secB64[1]) {
          setKeypair({
            publicKey: util.decodeBase64(pubB64[1]),
            secretKey: util.decodeBase64(secB64[1]),
          });
          return;
        }
      } catch {
        // Corrupted storage — generate fresh keypair
        console.warn('[useCrypto] Corrupted keypair in storage, generating new one');
      }
      const kp = nacl.sign.keyPair();
      setKeypair(kp);
      try {
        await AsyncStorage.multiSet([
          [PUBKEY_KEY,    util.encodeBase64(kp.publicKey)],
          [SECRETKEY_KEY, util.encodeBase64(kp.secretKey)],
        ]);
      } catch (e) {
        console.warn('[useCrypto] Failed to persist keypair:', e);
      }
    })();
  }, []);

  /**
   * Sign raw bytes with the Ed25519 private key.
   * Returns a 64-byte detached signature (Uint8Array).
   *
   * Used by HomeScreen to sign [amount_paise:2][nonce:2] before acoustic TX.
   * The ESP32 firmware verifies this signature using the hardcoded public key.
   */
  const signRaw = useCallback((message: Uint8Array): Uint8Array => {
    if (!keypair) throw new Error('[useCrypto] Keypair not loaded');
    return nacl.sign.detached(message, keypair.secretKey); // always 64 bytes
  }, [keypair]);

  /**
   * Sign a JSON-serialisable payload (kept for diagnostic / demo use).
   * @deprecated — acoustic TX uses signRaw() instead.
   */
  const signTransaction = useCallback(
    (payload: Record<string, unknown>): { payload: string; signature: string } => {
      if (!keypair) throw new Error('[useCrypto] Keypair not loaded');
      const payloadStr  = JSON.stringify(payload);
      const payloadBytes = util.decodeUTF8(payloadStr);
      const sig = nacl.sign.detached(payloadBytes, keypair.secretKey);
      return {
        payload:   util.encodeBase64(payloadBytes),
        signature: util.encodeBase64(sig),
      };
    },
    [keypair],
  );

  const publicKey = keypair ? util.encodeBase64(keypair.publicKey) : null;

  return { publicKey, signRaw, signTransaction, isReady: keypair !== null };
}

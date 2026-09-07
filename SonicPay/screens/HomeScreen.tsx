import React, { useState } from 'react';
import { View, Text, TouchableOpacity, StyleSheet, ActivityIndicator } from 'react-native';
import { useNonce } from '../hooks/useNonce';
import { useCrypto } from '../hooks/useCrypto';
import { useChirp, crc16_ccitt } from '../hooks/useChirp';

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const KEYS = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', '⌫'];

// Max amount is constrained by uint16 paise: 65535 paise = ₹655.35
const MAX_AMOUNT_RUPEES = 655.35;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

type Status = 'idle' | 'transmitting' | 'success' | 'error';

interface LogEntry {
  amountDisplay: string; // e.g. "12.50"
  amountPaise: number;   // integer paise stored in packet
  nonce: number;
  timestamp: number;
  sigPreview: string;    // first 8 hex chars of signature
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Format a raw Uint8Array as a hex string (first N bytes). */
function toHexPreview(bytes: Uint8Array, len: number): string {
  return Array.from(bytes.subarray(0, len))
    .map(b => b.toString(16).padStart(2, '0'))
    .join('');
}

/**
 * Build the 70-byte SonicPay v2 acoustic packet.
 *
 *   [amount_paise : 2 bytes, big-endian uint16]
 *   [nonce        : 2 bytes, big-endian uint16]
 *   [signature    : 64 bytes, Ed25519 detached over [amount_paise:2][nonce:2]]
 *   [crc16_ccitt  : 2 bytes, big-endian uint16, over first 68 bytes]
 *
 * Total: 70 bytes → 560 bits → 188 × 8-FSK symbols
 */
function buildPacket(amountPaise: number, nonce: number, signature: Uint8Array): Uint8Array {
  if (signature.length !== 64) {
    throw new Error(`buildPacket: signature must be 64 bytes, got ${signature.length}`);
  }

  const packet = new Uint8Array(70);

  // [amount_paise : 2]
  packet[0] = (amountPaise >> 8) & 0xff;
  packet[1] =  amountPaise       & 0xff;

  // [nonce : 2]
  packet[2] = (nonce >> 8) & 0xff;
  packet[3] =  nonce       & 0xff;

  // [signature : 64]
  packet.set(signature, 4);

  // [crc16 : 2] — computed over the first 68 bytes
  const crc = crc16_ccitt(packet.subarray(0, 68));
  packet[68] = (crc >> 8) & 0xff;
  packet[69] =  crc       & 0xff;

  return packet;
}

// ---------------------------------------------------------------------------
// Component
// ---------------------------------------------------------------------------

export default function HomeScreen() {
  const [amount, setAmount] = useState('0');
  const [status, setStatus] = useState<Status>('idle');
  const [log, setLog]       = useState<LogEntry[]>([]);

  const { nonce, nonceLoaded, incrementNonce } = useNonce();
  const { signRaw, isReady }                   = useCrypto();
  const { playChirp }                          = useChirp();

  // Send button is only active when both async inits are done
  const canSend = isReady && nonceLoaded && status === 'idle';

  // ---------------------------------------------------------------------------
  // Keypad handler
  // ---------------------------------------------------------------------------
  const handlePress = (key: string) => {
    if (status !== 'idle') return;

    if (key === '⌫') {
      setAmount(prev => (prev.length > 1 ? prev.slice(0, -1) : '0'));
      return;
    }

    if (key === '.') {
      setAmount(prev => (prev.includes('.') ? prev : prev + '.'));
      return;
    }

    setAmount(prev => {
      if (prev === '0') return key;
      // Allow at most 2 decimal places (paise)
      const dotIndex = prev.indexOf('.');
      if (dotIndex !== -1 && prev.length - dotIndex >= 3) return prev;
      return prev + key;
    });
  };

  // ---------------------------------------------------------------------------
  // Send handler
  // ---------------------------------------------------------------------------
  const handleSend = async () => {
    // Parse & validate amount
    const floatAmount = parseFloat(amount);
    if (isNaN(floatAmount) || floatAmount <= 0) return;
    if (floatAmount > MAX_AMOUNT_RUPEES) return;

    // Guard: should be unreachable if UI gates correctly, but be defensive
    if (!isReady || !nonceLoaded) return;

    setStatus('transmitting');

    try {
      // --- Amount encoding ---
      // Store as integer paise to avoid floating-point truncation.
      // Math.round() handles values like 12.10 that floor() would truncate.
      const amountPaise = Math.round(floatAmount * 100); // uint16, max 65535

      // --- Nonce ---
      const newNonce = await incrementNonce();

      // --- Signing ---
      // Message = [amount_paise:2][nonce:2] (4 bytes, no CRC)
      // ESP32 verifies this exact 4-byte message with the hardcoded public key.
      const message = new Uint8Array(4);
      message[0] = (amountPaise >> 8) & 0xff;
      message[1] =  amountPaise       & 0xff;
      message[2] = (newNonce >> 8)    & 0xff;
      message[3] =  newNonce          & 0xff;

      const signature = signRaw(message); // 64-byte Ed25519 detached signature

      // --- Packet assembly ---
      const packet = buildPacket(amountPaise, newNonce, signature);

      console.log(
        `[HomeScreen] TX  amount=${amountPaise}p (₹${floatAmount.toFixed(2)})` +
        `  nonce=${newNonce}  sig=${toHexPreview(signature, 4)}...` +
        `  crc=${((packet[68] << 8) | packet[69]).toString(16).padStart(4, '0')}`,
      );

      // --- Acoustic transmission ---
      await playChirp(packet);

      // --- Update log ---
      setLog(prev => [
        {
          amountDisplay: floatAmount.toFixed(2),
          amountPaise,
          nonce: newNonce,
          timestamp: Date.now(),
          sigPreview: toHexPreview(signature, 4),
        },
        ...prev,
      ].slice(0, 3));

      setStatus('success');
      setTimeout(() => {
        setStatus('idle');
        setAmount('0');
      }, 3000);
    } catch (err) {
      console.error('[HomeScreen] Payment failed:', err);
      setStatus('error');
    }
  };

  // ---------------------------------------------------------------------------
  // Render — success
  // ---------------------------------------------------------------------------
  if (status === 'success') {
    const last = log[0];
    return (
      <View style={styles.container}>
        <Text style={styles.statusText}>Payment Sent ✓</Text>
        <Text style={styles.successAmount}>₹{last?.amountDisplay ?? amount}</Text>
        <Text style={styles.successSub}>nonce {last?.nonce}</Text>
      </View>
    );
  }

  // ---------------------------------------------------------------------------
  // Render — error
  // ---------------------------------------------------------------------------
  if (status === 'error') {
    return (
      <View style={styles.container}>
        <Text style={styles.errorText}>Transmission failed.{'\n'}Try again.</Text>
        <TouchableOpacity style={styles.sendButton} onPress={() => setStatus('idle')}>
          <Text style={styles.sendText}>OK</Text>
        </TouchableOpacity>
      </View>
    );
  }

  // ---------------------------------------------------------------------------
  // Render — main
  // ---------------------------------------------------------------------------
  const floatAmount   = parseFloat(amount);
  const amountInvalid = isNaN(floatAmount) || floatAmount <= 0 || floatAmount > MAX_AMOUNT_RUPEES;

  return (
    <View style={styles.container}>
      {/* Initialising indicator shown until both crypto + nonce are ready */}
      {(!isReady || !nonceLoaded) && (
        <View style={styles.initBanner}>
          <ActivityIndicator size="small" color="#007AFF" style={{ marginRight: 6 }} />
          <Text style={styles.initText}>Initialising…</Text>
        </View>
      )}

      {/* Amount display */}
      <Text style={[styles.display, amountInvalid && floatAmount > MAX_AMOUNT_RUPEES && styles.displayError]}>
        ₹{amount}
      </Text>
      {floatAmount > MAX_AMOUNT_RUPEES && (
        <Text style={styles.limitText}>Max ₹{MAX_AMOUNT_RUPEES.toFixed(2)}</Text>
      )}

      {/* Keypad */}
      <View style={styles.keypad}>
        {KEYS.map(key => (
          <TouchableOpacity
            key={key}
            style={[styles.key, status !== 'idle' && { opacity: 0.5 }]}
            disabled={status !== 'idle'}
            onPress={() => handlePress(key)}
          >
            <Text style={styles.keyText}>{key}</Text>
          </TouchableOpacity>
        ))}
      </View>

      {/* Send button */}
      <TouchableOpacity
        style={[
          styles.sendButton,
          (!canSend || amountInvalid) && styles.sendButtonDisabled,
        ]}
        disabled={!canSend || amountInvalid}
        onPress={handleSend}
      >
        {status === 'transmitting' ? (
          <View style={styles.row}>
            <ActivityIndicator size="small" color="#fff" style={{ marginRight: 8 }} />
            <Text style={styles.sendText}>Transmitting…</Text>
          </View>
        ) : (
          <Text style={styles.sendText}>Send Payment</Text>
        )}
      </TouchableOpacity>

      {/* Transaction log */}
      {log.length > 0 && (
        <View style={styles.logContainer}>
          <Text style={styles.logTitle}>Recent Transactions</Text>
          {log.map((entry, i) => (
            <Text key={i} style={styles.logEntry}>
              ₹{entry.amountDisplay} · nonce {entry.nonce} · {new Date(entry.timestamp).toLocaleTimeString()} · sig:{entry.sigPreview}…
            </Text>
          ))}
        </View>
      )}
    </View>
  );
}

// ---------------------------------------------------------------------------
// Styles
// ---------------------------------------------------------------------------
const styles = StyleSheet.create({
  container: {
    flex: 1,
    justifyContent: 'center',
    padding: 20,
  },
  initBanner: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    marginBottom: 8,
  },
  initText: {
    fontSize: 13,
    color: '#888',
  },
  display: {
    fontSize: 48,
    textAlign: 'right',
    marginBottom: 4,
    fontWeight: '300',
  },
  displayError: {
    color: '#FF3B30',
  },
  limitText: {
    textAlign: 'right',
    fontSize: 12,
    color: '#FF3B30',
    marginBottom: 12,
  },
  keypad: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    justifyContent: 'center',
    gap: 10,
    marginBottom: 10,
  },
  key: {
    width: 70,
    height: 70,
    justifyContent: 'center',
    alignItems: 'center',
    backgroundColor: '#f0f0f0',
    borderRadius: 8,
  },
  keyText: {
    fontSize: 24,
  },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
  },
  sendButton: {
    marginTop: 20,
    padding: 16,
    backgroundColor: '#007AFF',
    borderRadius: 8,
    alignItems: 'center',
  },
  sendButtonDisabled: {
    backgroundColor: '#a0c4f1',
  },
  sendText: {
    color: '#fff',
    fontSize: 18,
    fontWeight: '600',
  },
  statusText: {
    fontSize: 28,
    textAlign: 'center',
    marginBottom: 10,
  },
  successAmount: {
    fontSize: 64,
    textAlign: 'center',
    fontWeight: '300',
  },
  successSub: {
    textAlign: 'center',
    fontSize: 14,
    color: '#888',
    marginTop: 4,
  },
  errorText: {
    fontSize: 22,
    textAlign: 'center',
    marginBottom: 24,
    color: '#FF3B30',
    lineHeight: 32,
  },
  logContainer: {
    marginTop: 24,
    borderTopWidth: 1,
    borderTopColor: '#e0e0e0',
    paddingTop: 10,
  },
  logTitle: {
    fontSize: 13,
    color: '#888',
    marginBottom: 6,
  },
  logEntry: {
    fontSize: 12,
    color: '#555',
    marginBottom: 4,
    fontVariant: ['tabular-nums'],
  },
});

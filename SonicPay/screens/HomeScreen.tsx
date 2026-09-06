import React, { useState } from 'react';
import { View, Text, TouchableOpacity, StyleSheet } from 'react-native';
import { useNonce } from '../hooks/useNonce';
import { useCrypto } from '../hooks/useCrypto';
import { useChirp } from '../hooks/useChirp';

const KEYS = ['1','2','3','4','5','6','7','8','9','.','0','⌫'];

type Status = 'idle' | 'transmitting' | 'success' | 'error';

interface LogEntry {
  amount: string;
  timestamp: number;
  sigPreview: string;
}

export default function HomeScreen() {
  const [amount, setAmount] = useState('0');
  const [status, setStatus] = useState<Status>('idle');
  const [log, setLog] = useState<LogEntry[]>([]);
  const { incrementNonce } = useNonce();
  const { signTransaction, isReady } = useCrypto();
  const { playChirp } = useChirp();

  const handlePress = (key: string) => {
    if (status !== 'idle') return;
    if (key === '⌫') {
      setAmount(prev => prev.length > 1 ? prev.slice(0, -1) : '0');
    } else if (key === '.') {
      setAmount(prev => prev.includes('.') ? prev : prev + '.');
    } else {
      setAmount(prev => {
        if (prev === '0') return key;
        const dotIndex = prev.indexOf('.');
        if (dotIndex !== -1 && prev.length - dotIndex >= 2) return prev;
        return prev + key;
      });
    }
  };

  const handleSend = async () => {
    const value = parseFloat(amount);
    if (isNaN(value) || value <= 0 || value > 10000 || !isReady) return;

    setStatus('transmitting');

    try {
      const newNonce = await incrementNonce();
      const { payload, signature } = signTransaction({
        merchantId: 'DEMO_MERCHANT_001',
        amount,
        timestamp: Date.now(),
        nonce: newNonce,
      });

      const chirpData = JSON.stringify({ payload, signature });
      await playChirp(chirpData);

      setLog(prev => [
        { amount, timestamp: Date.now(), sigPreview: signature.slice(0, 8) },
        ...prev,
      ].slice(0, 3));

      setStatus('success');
      setTimeout(() => { setStatus('idle'); setAmount('0'); }, 3000);
    } catch (err) {
      console.error('Payment failed:', err);
      setStatus('error');
    }
  };

  if (status === 'success') {
    return (
      <View style={styles.container}>
        <Text style={styles.statusText}>Payment Sent ✓</Text>
        <Text style={styles.successAmount}>₹{amount}</Text>
      </View>
    );
  }

  if (status === 'error') {
    return (
      <View style={styles.container}>
        <Text style={styles.errorText}>Failed. Try again.</Text>
        <TouchableOpacity style={styles.sendButton} onPress={() => setStatus('idle')}>
          <Text style={styles.sendText}>OK</Text>
        </TouchableOpacity>
      </View>
    );
  }

  return (
    <View style={styles.container}>
      <Text style={styles.display}>₹{amount}</Text>

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

      <TouchableOpacity
        style={[styles.sendButton, status === 'transmitting' && { opacity: 0.5 }]}
        disabled={status === 'transmitting'}
        onPress={handleSend}
      >
        <Text style={styles.sendText}>{status === 'transmitting' ? 'Transmitting...' : 'Send Payment'}</Text>
      </TouchableOpacity>

      {log.length > 0 && (
        <View style={styles.logContainer}>
          <Text style={styles.logTitle}>Recent Transactions</Text>
          {log.map((entry, i) => (
            <Text key={i} style={styles.logEntry}>
              ₹{entry.amount} • {new Date(entry.timestamp).toLocaleTimeString()} • sig:{entry.sigPreview}
            </Text>
          ))}
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    justifyContent: 'center',
    padding: 20,
  },
  display: {
    fontSize: 48,
    textAlign: 'right',
    marginBottom: 20,
    fontWeight: '300',
  },
  keypad: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    justifyContent: 'center',
    gap: 10,
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
  sendButton: {
    marginTop: 30,
    padding: 16,
    backgroundColor: '#007AFF',
    borderRadius: 8,
    alignItems: 'center',
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
  errorText: {
    fontSize: 24,
    textAlign: 'center',
    marginBottom: 20,
    color: '#FF3B30',
  },
  logContainer: {
    marginTop: 30,
    borderTopWidth: 1,
    borderTopColor: '#e0e0e0',
    paddingTop: 10,
  },
  logTitle: {
    fontSize: 14,
    color: '#888',
    marginBottom: 6,
  },
  logEntry: {
    fontSize: 13,
    color: '#555',
    marginBottom: 4,
  },
});

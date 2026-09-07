import React, { useState, useEffect, useRef } from 'react';
import {
  View,
  Text,
  TouchableOpacity,
  StyleSheet,
  SafeAreaView,
  Animated,
  Easing,
  StatusBar,
  Dimensions,
  ImageBackground,
} from 'react-native';
import { useNonce } from '../hooks/useNonce';
import { useCrypto } from '../hooks/useCrypto';
import { useChirp, crc16_ccitt } from '../hooks/useChirp';

const { width } = Dimensions.get('window');

// ---------------------------------------------------------------------------
// Constants & Types
// ---------------------------------------------------------------------------

const KEYS = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', '⌫'];
const MAX_AMOUNT_RUPEES = 655.35;

type ScreenState = 'splash' | 'keypad' | 'processing' | 'success' | 'error';

interface TransactionRecord {
  amountDisplay: string;
  amountPaise: number;
  nonce: number;
  timestamp: number;
  txId: string;
}

// ---------------------------------------------------------------------------
// Helper: 70-byte packet builder
// ---------------------------------------------------------------------------

function buildPacket(amountPaise: number, nonce: number, signature: Uint8Array): Uint8Array {
  const packet = new Uint8Array(70);
  packet[0] = (amountPaise >> 8) & 0xff;
  packet[1] =  amountPaise       & 0xff;
  packet[2] = (nonce >> 8) & 0xff;
  packet[3] =  nonce       & 0xff;
  packet.set(signature, 4);

  const crc = crc16_ccitt(packet.subarray(0, 68));
  packet[68] = (crc >> 8) & 0xff;
  packet[69] =  crc       & 0xff;

  return packet;
}

// ---------------------------------------------------------------------------
// Component: Processing Screen Ring Animation (Figma Screen 2 / iPhone 17 - 4)
// ---------------------------------------------------------------------------

function ProcessingRings() {
  const pulseAnim = useRef(new Animated.Value(0.95)).current;

  useEffect(() => {
    const pulse = Animated.loop(
      Animated.sequence([
        Animated.timing(pulseAnim, {
          toValue: 1.15,
          duration: 1000,
          easing: Easing.inOut(Easing.ease),
          useNativeDriver: true,
        }),
        Animated.timing(pulseAnim, {
          toValue: 0.95,
          duration: 1000,
          easing: Easing.inOut(Easing.ease),
          useNativeDriver: true,
        }),
      ])
    );
    pulse.start();
    return () => pulse.stop();
  }, [pulseAnim]);

  return (
    <View style={styles.processingCircleContainer}>
      {/* Outer subtle ring */}
      <Animated.View
        style={[
          styles.processingOuterRing,
          { transform: [{ scale: pulseAnim }] },
        ]}
      />
      {/* Center Teal Circle with Histogram Bar Chart Icon */}
      <View style={styles.processingTealCircle}>
        <View style={styles.histogramRow}>
          <View style={[styles.histoBar, { height: 14 }]} />
          <View style={[styles.histoBar, { height: 28 }]} />
          <View style={[styles.histoBar, { height: 18 }]} />
        </View>
      </View>
    </View>
  );
}

// ---------------------------------------------------------------------------
// Component: Checkmark Success Icon (Figma Screen 3 / iPhone 17 - 5)
// ---------------------------------------------------------------------------

function SuccessCheckmark() {
  const scaleAnim = useRef(new Animated.Value(0)).current;

  useEffect(() => {
    Animated.spring(scaleAnim, {
      toValue: 1,
      friction: 5,
      tension: 100,
      useNativeDriver: true,
    }).start();
  }, [scaleAnim]);

  return (
    <View style={styles.successCircleContainer}>
      <View style={styles.successOuterRing} />
      <Animated.View
        style={[
          styles.successTealCircle,
          { transform: [{ scale: scaleAnim }] },
        ]}
      >
        <Text style={styles.checkmarkIconText}>✓</Text>
      </Animated.View>
    </View>
  );
}

// ---------------------------------------------------------------------------
// Main Screen Component
// ---------------------------------------------------------------------------

export default function HomeScreen() {
  const [screenState, setScreenState] = useState<ScreenState>('splash');
  const [amount, setAmount]           = useState('0');
  const [errorMessage, setErrorMessage] = useState<string | null>(null);
  const [txRecord, setTxRecord]       = useState<TransactionRecord | null>(null);

  const { nonce, nonceLoaded, incrementNonce } = useNonce();
  const { signRaw, isReady }                   = useCrypto();
  const { playChirp }                          = useChirp();

  // Splash auto-transition
  useEffect(() => {
    const timer = setTimeout(() => {
      setScreenState('keypad');
    }, 1500);
    return () => clearTimeout(timer);
  }, []);

  const handleKeyPress = (key: string) => {
    if (screenState !== 'keypad') return;

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
      const dotIndex = prev.indexOf('.');
      if (dotIndex !== -1 && prev.length - dotIndex >= 3) return prev;
      return prev + key;
    });
  };

  const handleSendPayment = async () => {
    setErrorMessage(null);
    const floatVal = parseFloat(amount);

    if (isNaN(floatVal) || floatVal <= 0) {
      console.warn('[SonicPay System Log] Invalid payment amount entered:', amount);
      return;
    }
    if (floatVal > MAX_AMOUNT_RUPEES) {
      console.warn('[SonicPay System Log] Amount exceeds max limit ₹655.35:', floatVal);
      return;
    }
    if (!isReady) {
      const err = 'Ed25519 Crypto Keypair is still initialising.';
      console.error('[SonicPay System Log] Error:', err);
      setErrorMessage(err);
      setScreenState('error');
      return;
    }
    if (!nonceLoaded) {
      const err = 'Nonce storage is still loading.';
      console.error('[SonicPay System Log] Error:', err);
      setErrorMessage(err);
      setScreenState('error');
      return;
    }

    setScreenState('processing');

    try {
      const amountPaise = Math.round(floatVal * 100);
      console.log(`[SonicPay System Log] Initiating transaction: ₹${floatVal.toFixed(2)} (${amountPaise} paise)`);

      const newNonce = await incrementNonce();
      console.log(`[SonicPay System Log] Monotonic nonce assigned: #${newNonce}`);

      const message = new Uint8Array(4);
      message[0] = (amountPaise >> 8) & 0xff;
      message[1] =  amountPaise       & 0xff;
      message[2] = (newNonce >> 8)    & 0xff;
      message[3] =  newNonce          & 0xff;

      const signature = signRaw(message);
      console.log(`[SonicPay System Log] Ed25519 signature generated: 64 bytes`);

      const packet = buildPacket(amountPaise, newNonce, signature);
      console.log(`[SonicPay System Log] 70-byte packet ready. Transmitting 187 symbols over 8-FSK…`);

      const generatedTxId = `45${Math.floor(100000000 + Math.random() * 900000000)}`;

      // Acoustic playback (~11.2s)
      await playChirp(packet);
      console.log(`[SonicPay System Log] Chirp transmission completed successfully.`);

      setTxRecord({
        amountDisplay: floatVal.toFixed(2),
        amountPaise,
        nonce: newNonce,
        timestamp: Date.now(),
        txId: generatedTxId,
      });

      setScreenState('success');
    } catch (err: any) {
      const errStr = err?.message || String(err);
      console.error('[SonicPay System Log] PAYMENT TRANSMISSION FAILED:', errStr, err?.stack || '');
      setErrorMessage(errStr);
      setScreenState('error');
    }
  };

  const handleDone = () => {
    setAmount('0');
    setErrorMessage(null);
    setScreenState('keypad');
  };

  // ---------------------------------------------------------------------------
  // 1. Splash Screen
  // ---------------------------------------------------------------------------
  if (screenState === 'splash') {
    return (
      <View style={styles.splashBody}>
        <StatusBar barStyle="light-content" backgroundColor="#0E8B7D" />
        <View style={styles.splashLogoCard}>
          <Text style={styles.rupeeSplashLogo}>₹</Text>
        </View>
        <Text style={styles.splashBrandText}>SONICPAY</Text>
      </View>
    );
  }

  // ---------------------------------------------------------------------------
  // 2. Processing Screen (Reference: Screenshot Screen 2)
  // ---------------------------------------------------------------------------
  if (screenState === 'processing') {
    return (
      <SafeAreaView style={styles.processingBody}>
        <StatusBar barStyle="dark-content" backgroundColor="#FFFFFF" />
        <View style={styles.processingCenterContent}>
          <ProcessingRings />
          <Text style={styles.processingStatusText}>PAYMENT PROCESSING</Text>
        </View>
      </SafeAreaView>
    );
  }

  // ---------------------------------------------------------------------------
  // 3. Success Screen (Reference: Screenshot Screen 3)
  // ---------------------------------------------------------------------------
  if (screenState === 'success') {
    const formattedDate = txRecord
      ? new Date(txRecord.timestamp).toLocaleString('en-US', {
          day: 'numeric',
          month: 'short',
          hour: '2-digit',
          minute: '2-digit',
          hour12: true,
        })
      : '';

    return (
      <SafeAreaView style={styles.successBody}>
        <StatusBar barStyle="dark-content" backgroundColor="#FFFFFF" />
        <View style={styles.successCenterContent}>
          <SuccessCheckmark />
          <Text style={styles.successStatusText}>PAYMENT SUCESSFUL</Text>

          <View style={styles.successDetailBox}>
            <Text style={styles.paidToTitle}>PAID TO DEMO_MERCHANT</Text>
            <Text style={styles.paidHandleText}>q891239139@xx</Text>
          </View>

          <TouchableOpacity style={styles.doneBtn} onPress={handleDone}>
            <Text style={styles.doneBtnText}>DONE</Text>
          </TouchableOpacity>
        </View>

        <View style={styles.successFooter}>
          <Text style={styles.footerDateText}>{formattedDate}</Text>
          <Text style={styles.footerTxIdText}>UPI transaction ID: {txRecord?.txId}</Text>
        </View>
      </SafeAreaView>
    );
  }

  // ---------------------------------------------------------------------------
  // 4. Error Screen with Detailed System Logging Output
  // ---------------------------------------------------------------------------
  if (screenState === 'error') {
    return (
      <SafeAreaView style={styles.errorBody}>
        <StatusBar barStyle="dark-content" backgroundColor="#FFFFFF" />
        <View style={styles.errorCenterContent}>
          <View style={styles.errorIconCircle}>
            <Text style={styles.errorIconText}>!</Text>
          </View>
          <Text style={styles.errorTitleText}>Payment Error</Text>
          <Text style={styles.errorMessageText}>{errorMessage || 'Chirp audio transmission failed.'}</Text>
          <TouchableOpacity style={styles.doneBtn} onPress={handleDone}>
            <Text style={styles.doneBtnText}>TRY AGAIN</Text>
          </TouchableOpacity>
        </View>
      </SafeAreaView>
    );
  }

  // ---------------------------------------------------------------------------
  // 5. Main Keypad Screen (Reference: Screenshot Screen 1)
  // ---------------------------------------------------------------------------
  const floatVal      = parseFloat(amount);
  const isValInvalid  = isNaN(floatVal) || floatVal <= 0 || floatVal > MAX_AMOUNT_RUPEES;
  const isReadyToSend = isReady && nonceLoaded && !isValInvalid;

  return (
    <SafeAreaView style={styles.mainContainer}>
      <StatusBar barStyle="light-content" backgroundColor="#0E8B7D" />

      {/* Top Header */}
      <View style={styles.headerRow}>
        <View style={styles.logoRow}>
          <Text style={styles.headerLogoIcon}>₹</Text>
          <Text style={styles.headerBrandText}>SONICPAY</Text>
        </View>
        <TouchableOpacity style={styles.bellBtn}>
          <Text style={styles.bellIconText}>🔔</Text>
        </TouchableOpacity>
      </View>

      {/* White Curved Sheet */}
      <View style={styles.sheetContainer}>
        {/* Merchant Info */}
        <View style={styles.merchantSection}>
          <Text style={styles.payingTitle}>Paying xxxxx xxxxx</Text>
          <View style={styles.bankingRow}>
            <Text style={styles.shieldIconText}>🛡</Text>
            <Text style={styles.bankingNameText}>Banking name: xxxx</Text>
          </View>
        </View>

        {/* Amount Display */}
        <View style={styles.amountDisplaySection}>
          <Text style={styles.rupeeSymbol}>₹</Text>
          <Text style={styles.amountValueText}>{amount}</Text>
        </View>
        {floatVal > MAX_AMOUNT_RUPEES && (
          <Text style={styles.limitWarningText}>Max limit ₹{MAX_AMOUNT_RUPEES.toFixed(2)}</Text>
        )}

        {/* Keypad Container (Grey Rounded Box) */}
        <View style={styles.keypadBox}>
          <View style={styles.grid}>
            {KEYS.map(key => (
              <TouchableOpacity
                key={key}
                style={styles.gridBtn}
                onPress={() => handleKeyPress(key)}
              >
                <Text style={styles.gridBtnText}>{key}</Text>
              </TouchableOpacity>
            ))}
          </View>
        </View>

        {/* Send Action Button */}
        <TouchableOpacity
          style={[styles.sendActionBtn, !isReadyToSend && styles.sendActionBtnDisabled]}
          disabled={!isReadyToSend}
          onPress={handleSendPayment}
        >
          <Text style={styles.sendActionBtnText}>SEND</Text>
        </TouchableOpacity>
      </View>
    </SafeAreaView>
  );
}

// ---------------------------------------------------------------------------
// Styles: 100% Match to Screenshot Reference
// ---------------------------------------------------------------------------

const styles = StyleSheet.create({
  // Splash Screen
  splashBody: {
    flex: 1,
    backgroundColor: '#0E8B7D',
    justifyContent: 'center',
    alignItems: 'center',
  },
  splashLogoCard: {
    width: 150,
    height: 150,
    borderRadius: 36,
    backgroundColor: '#149983',
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 20,
  },
  rupeeSplashLogo: {
    fontSize: 72,
    fontWeight: '700',
    color: '#FFFFFF',
  },
  splashBrandText: {
    fontSize: 28,
    fontWeight: '700',
    color: '#FFFFFF',
    letterSpacing: 4,
  },

  // Main Keypad Screen (Screen 1)
  mainContainer: {
    flex: 1,
    backgroundColor: '#0E8B7D',
  },
  headerRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    paddingHorizontal: 24,
    paddingTop: 16,
    paddingBottom: 24,
  },
  logoRow: {
    flexDirection: 'row',
    alignItems: 'center',
  },
  headerLogoIcon: {
    fontSize: 22,
    fontWeight: '700',
    color: '#FFFFFF',
  },
  headerBrandText: {
    fontSize: 20,
    fontWeight: '700',
    color: '#FFFFFF',
    marginLeft: 6,
    letterSpacing: 1,
  },
  bellBtn: {
    padding: 6,
  },
  bellIconText: {
    fontSize: 18,
    color: '#FFFFFF',
  },
  sheetContainer: {
    flex: 1,
    backgroundColor: '#FFFFFF',
    borderTopLeftRadius: 40,
    borderTopRightRadius: 40,
    paddingHorizontal: 24,
    paddingTop: 32,
    paddingBottom: 24,
    justifyContent: 'space-between',
  },
  merchantSection: {
    alignItems: 'center',
  },
  payingTitle: {
    fontSize: 22,
    fontWeight: '500',
    color: '#111827',
  },
  bankingRow: {
    flexDirection: 'row',
    alignItems: 'center',
    marginTop: 4,
  },
  shieldIconText: {
    fontSize: 12,
    marginRight: 4,
  },
  bankingNameText: {
    fontSize: 13,
    color: '#6B7280',
  },
  amountDisplaySection: {
    flexDirection: 'row',
    justifyContent: 'center',
    alignItems: 'baseline',
    marginVertical: 10,
  },
  rupeeSymbol: {
    fontSize: 42,
    fontWeight: '500',
    color: '#6B7280',
    marginRight: 8,
  },
  amountValueText: {
    fontSize: 76,
    fontWeight: '500',
    color: '#111827',
  },
  limitWarningText: {
    textAlign: 'center',
    color: '#EF4444',
    fontSize: 12,
  },
  keypadBox: {
    backgroundColor: '#CCCCCC',
    borderRadius: 24,
    padding: 12,
    alignSelf: 'center',
    width: width - 48,
  },
  grid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    justifyContent: 'space-between',
  },
  gridBtn: {
    width: (width - 48 - 24 - 20) / 3,
    height: 64,
    backgroundColor: '#FFFFFF',
    borderRadius: 14,
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 8,
  },
  gridBtnText: {
    fontSize: 32,
    fontWeight: '400',
    color: '#111827',
  },
  sendActionBtn: {
    backgroundColor: '#0E8B7D',
    borderRadius: 16,
    paddingVertical: 16,
    alignItems: 'center',
  },
  sendActionBtnDisabled: {
    backgroundColor: '#9CA3AF',
  },
  sendActionBtnText: {
    color: '#FFFFFF',
    fontSize: 24,
    fontWeight: '600',
    letterSpacing: 1,
  },

  // Processing Screen (Screen 2)
  processingBody: {
    flex: 1,
    backgroundColor: '#FFFFFF',
    justifyContent: 'center',
    alignItems: 'center',
  },
  processingCenterContent: {
    alignItems: 'center',
  },
  processingCircleContainer: {
    width: 180,
    height: 180,
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 36,
  },
  processingOuterRing: {
    position: 'absolute',
    width: 170,
    height: 170,
    borderRadius: 85,
    borderWidth: 1,
    borderColor: '#99F6E4',
  },
  processingTealCircle: {
    width: 110,
    height: 110,
    borderRadius: 55,
    backgroundColor: '#0E8B7D',
    justifyContent: 'center',
    alignItems: 'center',
  },
  histogramRow: {
    flexDirection: 'row',
    alignItems: 'flex-end',
    gap: 6,
  },
  histoBar: {
    width: 7,
    backgroundColor: '#FFFFFF',
    borderRadius: 2,
  },
  processingStatusText: {
    fontSize: 20,
    fontWeight: '700',
    color: '#111827',
    letterSpacing: 1,
  },

  // Success Screen (Screen 3)
  successBody: {
    flex: 1,
    backgroundColor: '#FFFFFF',
    justifyContent: 'space-between',
    paddingVertical: 32,
  },
  successCenterContent: {
    flex: 1,
    justifyContent: 'center',
    alignItems: 'center',
    paddingHorizontal: 24,
  },
  successCircleContainer: {
    width: 160,
    height: 160,
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 24,
  },
  successOuterRing: {
    position: 'absolute',
    width: 150,
    height: 150,
    borderRadius: 75,
    borderWidth: 1,
    borderColor: '#99F6E4',
  },
  successTealCircle: {
    width: 110,
    height: 110,
    borderRadius: 55,
    backgroundColor: '#0E8B7D',
    justifyContent: 'center',
    alignItems: 'center',
  },
  checkmarkIconText: {
    fontSize: 54,
    fontWeight: '700',
    color: '#FFFFFF',
  },
  successStatusText: {
    fontSize: 20,
    fontWeight: '700',
    color: '#111827',
    letterSpacing: 1,
    marginBottom: 16,
  },
  successDetailBox: {
    alignItems: 'center',
    marginBottom: 32,
  },
  paidToTitle: {
    fontSize: 14,
    fontWeight: '600',
    color: '#111827',
  },
  paidHandleText: {
    fontSize: 13,
    color: '#6B7280',
    marginTop: 2,
  },
  doneBtn: {
    backgroundColor: '#0E8B7D',
    paddingVertical: 14,
    paddingHorizontal: 44,
    borderRadius: 24,
  },
  doneBtnText: {
    color: '#FFFFFF',
    fontSize: 16,
    fontWeight: '600',
    letterSpacing: 1,
  },
  successFooter: {
    alignItems: 'center',
    paddingBottom: 12,
  },
  footerDateText: {
    fontSize: 14,
    fontWeight: '600',
    color: '#111827',
  },
  footerTxIdText: {
    fontSize: 13,
    color: '#6B7280',
    marginTop: 2,
  },

  // Error Screen
  errorBody: {
    flex: 1,
    backgroundColor: '#FFFFFF',
    justifyContent: 'center',
    alignItems: 'center',
  },
  errorCenterContent: {
    alignItems: 'center',
    paddingHorizontal: 32,
  },
  errorIconCircle: {
    width: 80,
    height: 80,
    borderRadius: 40,
    backgroundColor: '#FEE2E2',
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 16,
  },
  errorIconText: {
    fontSize: 40,
    fontWeight: '700',
    color: '#EF4444',
  },
  errorTitleText: {
    fontSize: 22,
    fontWeight: '700',
    color: '#111827',
    marginBottom: 8,
  },
  errorMessageText: {
    fontSize: 14,
    color: '#6B7280',
    textAlign: 'center',
    marginBottom: 24,
    lineHeight: 20,
  },
});

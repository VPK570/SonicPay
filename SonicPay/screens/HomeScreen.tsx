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
  ScrollView,
  Platform,
} from 'react-native';
import { useNonce } from '../hooks/useNonce';
import { useCrypto } from '../hooks/useCrypto';
import { useChirp, crc16_ccitt } from '../hooks/useChirp';

const { width } = Dimensions.get('window');

// ---------------------------------------------------------------------------
// Constants & Types
// ---------------------------------------------------------------------------

const KEYS = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', '⌫'];
const MAX_AMOUNT_RUPEES = 10000;

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

function buildPacket(amountRupees: number, nonce: number, signature: Uint8Array): Uint8Array {
  const packet = new Uint8Array(70);
  packet[0] = (amountRupees >> 8) & 0xff;
  packet[1] =  amountRupees       & 0xff;
  packet[2] = (nonce >> 8) & 0xff;
  packet[3] =  nonce       & 0xff;
  packet.set(signature, 4);

  const crc = crc16_ccitt(packet.subarray(0, 68));
  packet[68] = (crc >> 8) & 0xff;
  packet[69] =  crc       & 0xff;

  return packet;
}

function formatToWords(num: number): string {
  if (isNaN(num) || num === 0) return 'Zero Rupees';
  if (num < 1000) return `₹${num.toLocaleString('en-IN')} Rupees`;
  if (num < 100000) return `₹${(num / 1000).toFixed(1)} Thousand Rupees`;
  return `₹${(num / 100000).toFixed(2)} Lakh Rupees`;
}

// ---------------------------------------------------------------------------
// Component: Processing Screen Visualizer (Acoustic Beat Sync & EQ Bars)
// ---------------------------------------------------------------------------

function ProcessingVisualizer() {
  const pulseAnim = useRef(new Animated.Value(0.96)).current;
  const eqBar1 = useRef(new Animated.Value(18)).current;
  const eqBar2 = useRef(new Animated.Value(34)).current;
  const eqBar3 = useRef(new Animated.Value(26)).current;
  const eqBar4 = useRef(new Animated.Value(14)).current;

  useEffect(() => {
    // Single circle breathing animation (expanding & contracting)
    const pulse = Animated.loop(
      Animated.sequence([
        Animated.timing(pulseAnim, {
          toValue: 1.15,
          duration: 900,
          easing: Easing.out(Easing.ease),
          useNativeDriver: true,
        }),
        Animated.timing(pulseAnim, {
          toValue: 0.95,
          duration: 900,
          easing: Easing.in(Easing.ease),
          useNativeDriver: true,
        }),
      ])
    );

    // 4 Animated equalizer bars
    const createEqLoop = (anim: Animated.Value, minH: number, maxH: number, duration: number) => {
      return Animated.loop(
        Animated.sequence([
          Animated.timing(anim, {
            toValue: maxH,
            duration,
            easing: Easing.linear,
            useNativeDriver: false,
          }),
          Animated.timing(anim, {
            toValue: minH,
            duration,
            easing: Easing.linear,
            useNativeDriver: false,
          }),
        ])
      );
    };

    const eq1 = createEqLoop(eqBar1, 12, 28, 300);
    const eq2 = createEqLoop(eqBar2, 16, 36, 220);
    const eq3 = createEqLoop(eqBar3, 14, 30, 270);
    const eq4 = createEqLoop(eqBar4, 10, 22, 350);

    pulse.start();
    eq1.start();
    eq2.start();
    eq3.start();
    eq4.start();

    return () => {
      pulse.stop();
      eq1.stop();
      eq2.stop();
      eq3.stop();
      eq4.stop();
    };
  }, [pulseAnim, eqBar1, eqBar2, eqBar3, eqBar4]);

  return (
    <View style={styles.visualizerContainer}>
      {/* Single Breathing Core Circle */}
      <Animated.View
        style={[
          styles.singleBreathingOrb,
          { transform: [{ scale: pulseAnim }] },
        ]}
      >
        {/* Animated Bar Soundwave / Acoustic Equalizer */}
        <View style={styles.eqRow}>
          <Animated.View style={[styles.eqBar, { height: eqBar1 }]} />
          <Animated.View style={[styles.eqBar, { height: eqBar2, backgroundColor: '#ffffff' }]} />
          <Animated.View style={[styles.eqBar, { height: eqBar3, backgroundColor: '#ccfbf1' }]} />
          <Animated.View style={[styles.eqBar, { height: eqBar4 }]} />
        </View>

        <Text style={styles.sonicRailText}>SONIC RAIL</Text>
      </Animated.View>
    </View>
  );
}

// ---------------------------------------------------------------------------
// Component: Success Checkmark Badge
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
    <View style={styles.successBadgeContainer}>
      <View style={styles.successOuterGlow} />
      <Animated.View
        style={[
          styles.successBadgeCircle,
          { transform: [{ scale: scaleAnim }] },
        ]}
      >
        <Text style={styles.checkmarkSvgText}>✓</Text>
      </Animated.View>
    </View>
  );
}

// ---------------------------------------------------------------------------
// Main Screen Component
// ---------------------------------------------------------------------------

export default function HomeScreen() {
  const [screenState, setScreenState] = useState<ScreenState>('splash');
  const [amount, setAmount]           = useState('1');
  const [errorMessage, setErrorMessage] = useState<string | null>(null);
  const [txRecord, setTxRecord]       = useState<TransactionRecord | null>(null);
  const [soundEnabled, setSoundEnabled] = useState(true);
  const [isCopied, setIsCopied]       = useState(false);
  const [isSubmitting, setIsSubmitting] = useState(false);
  const [retryCount, setRetryCount]   = useState(0);

  // Shake animation for invalid amount
  const shakeAnim = useRef(new Animated.Value(0)).current;

  const triggerShake = () => {
    shakeAnim.setValue(0);
    Animated.sequence([
      Animated.timing(shakeAnim, { toValue: 8,  duration: 60, useNativeDriver: true }),
      Animated.timing(shakeAnim, { toValue: -8, duration: 60, useNativeDriver: true }),
      Animated.timing(shakeAnim, { toValue: 6,  duration: 50, useNativeDriver: true }),
      Animated.timing(shakeAnim, { toValue: -6, duration: 50, useNativeDriver: true }),
      Animated.timing(shakeAnim, { toValue: 0,  duration: 40, useNativeDriver: true }),
    ]).start();
  };

  const { nonce, nonceLoaded, incrementNonce } = useNonce();
  const { signRaw, isReady }                   = useCrypto();
  const { playChirp }                          = useChirp();

  // Splash auto-transition
  useEffect(() => {
    const timer = setTimeout(() => {
      setScreenState('keypad');
    }, 1200);
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
      if (prev.replace('.', '').length >= 7) return prev;
      return prev + key;
    });
  };

  const handleSendPayment = async () => {
    setErrorMessage(null);
    const floatVal = parseFloat(amount);

    if (isNaN(floatVal) || floatVal <= 0) {
      triggerShake();
      console.warn('[SonicPay System Log] Invalid payment amount entered:', amount);
      return;
    }
    if (floatVal > MAX_AMOUNT_RUPEES) {
      triggerShake();
      console.warn('[SonicPay System Log] Amount exceeds max limit ₹10,000:', floatVal);
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

    setIsSubmitting(true);
    setScreenState('processing');

    try {
      const amountRupees = Math.round(floatVal);
      const amountPaise  = Math.round(floatVal * 100);
      console.log(`[SonicPay System Log] Initiating transaction: ₹${floatVal.toFixed(2)} (${amountRupees} rupees)`);

      const newNonce = await incrementNonce();
      console.log(`[SonicPay System Log] Monotonic nonce assigned: #${newNonce}`);

      const message = new Uint8Array(4);
      message[0] = (amountRupees >> 8) & 0xff;
      message[1] =  amountRupees       & 0xff;
      message[2] = (newNonce >> 8)    & 0xff;
      message[3] =  newNonce          & 0xff;

      const signature = signRaw(message);
      console.log(`[SonicPay System Log] Ed25519 signature generated: 64 bytes`);

      const packet = buildPacket(amountRupees, newNonce, signature);
      console.log(`[SonicPay System Log] 70-byte packet ready. Transmitting over 8-FSK audio chirp…`);

      const generatedTxId = `45${Math.floor(1000000000 + Math.random() * 9000000000)}`;

      // Acoustic playback (~11.2s)
      await playChirp(packet);
      console.log(`[SonicPay System Log] Chirp transmission completed successfully.`);

      setTxRecord({
        amountDisplay: floatVal.toLocaleString('en-IN', { minimumFractionDigits: 0, maximumFractionDigits: 2 }),
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
      setRetryCount(prev => prev + 1);
      setScreenState('error');
    } finally {
      setIsSubmitting(false);
    }
  };

  const handleResetFlow = () => {
    setAmount('1');
    setErrorMessage(null);
    setIsCopied(false);
    setRetryCount(0);
    setScreenState('keypad');
  };

  const handleShareReceipt = () => {
    setIsCopied(true);
    setTimeout(() => setIsCopied(false), 2000);
  };

  // ---------------------------------------------------------------------------
  // 1. Splash Screen
  // ---------------------------------------------------------------------------
  if (screenState === 'splash') {
    return (
      <View style={styles.splashBody}>
        <StatusBar barStyle="light-content" backgroundColor="#128a84" />
        <View style={styles.splashLogoCard}>
          <Text style={styles.rupeeSplashLogo}>₹</Text>
        </View>
        <Text style={styles.splashBrandText}>SONICPAY</Text>
        <Text style={styles.splashTagline}>ACOUSTIC OFFLINE PAYMENTS</Text>
      </View>
    );
  }

  // ---------------------------------------------------------------------------
  // 2. Processing Screen
  // ---------------------------------------------------------------------------
  if (screenState === 'processing') {
    const floatVal = parseFloat(amount) || 0;
    return (
      <SafeAreaView style={styles.processingBody}>
        <StatusBar barStyle="dark-content" backgroundColor="#FFFFFF" />

        {/* Visualizer Orb Container */}
        <View style={styles.processingContent}>
          <ProcessingVisualizer />

          {/* Status Pills & Descriptions */}
          <View style={styles.processingStatusPill}>
            <Text style={styles.waveformIcon}>〰️</Text>
            <Text style={styles.processingPillText}>SONIC ACOUSTIC RAIL</Text>
          </View>

          <Text style={styles.processingTitle}>PAYMENT PROCESSING</Text>
          <Text style={styles.processingSubtext}>
            Transacting via Sonic Acoustic Rail...
          </Text>

          <Text style={styles.transferringText}>
            Transferring <Text style={styles.boldAmount}>₹{floatVal.toLocaleString('en-IN')}</Text> to Aarav Kapoor
          </Text>
        </View>

        {/* Footnote Security Badge */}
        <View style={styles.securityFootnote}>
          <Text style={styles.shieldIcon}>🛡️</Text>
          <Text style={styles.securityText}>256-Bit Bank-Grade Encryption</Text>
        </View>
      </SafeAreaView>
    );
  }

  // ---------------------------------------------------------------------------
  // 3. Payment Success Screen
  // ---------------------------------------------------------------------------
  if (screenState === 'success') {
    const formattedDate = txRecord
      ? new Date(txRecord.timestamp).toLocaleDateString('en-IN', {
          day: 'numeric',
          month: 'short',
          hour: '2-digit',
          minute: '2-digit',
        })
      : '';

    const floatVal = parseFloat(amount) || 0;

    return (
      <SafeAreaView style={styles.successBody}>
        <StatusBar barStyle="dark-content" backgroundColor="#FFFFFF" />

        <ScrollView contentContainerStyle={styles.successScrollContainer}>
          {/* Checkmark & Header */}
          <View style={styles.successTopSection}>
            <SuccessCheckmark />
            <Text style={styles.successHeading}>PAYMENT SUCCESSFUL!</Text>
            <Text style={styles.successAmountText}>₹{floatVal.toLocaleString('en-IN')}</Text>

            <View style={styles.paidToCard}>
              <Text style={styles.paidToLabel}>PAID TO</Text>
              <Text style={styles.paidToName}>Aarav Kapoor</Text>
              <Text style={styles.paidToUpi}>aarav.kapoor@okhdfcbank</Text>
            </View>
          </View>

          {/* Receipt Details Card */}
          <View style={styles.receiptCard}>
            <View style={styles.receiptRow}>
              <Text style={styles.receiptLabel}>Date & Time</Text>
              <Text style={styles.receiptValue}>{formattedDate}</Text>
            </View>
            <View style={styles.receiptDivider} />
            <View style={styles.receiptRow}>
              <Text style={styles.receiptLabel}>UPI Ref No.</Text>
              <Text style={styles.receiptValueMono}>{txRecord?.txId}</Text>
            </View>
            <View style={styles.receiptDivider} />
            <View style={styles.receiptRow}>
              <Text style={styles.receiptLabel}>Debited From</Text>
              <Text style={styles.receiptValue}>HDFC Bank •• 4912</Text>
            </View>
          </View>
        </ScrollView>

        {/* Bottom Action Buttons */}
        <View style={styles.successFooterButtons}>
          <TouchableOpacity style={styles.shareBtn} onPress={handleShareReceipt}>
            <Text style={styles.shareBtnIcon}>🔗</Text>
            <Text style={styles.shareBtnText}>{isCopied ? 'Receipt Copied!' : 'Share Receipt'}</Text>
          </TouchableOpacity>

          <TouchableOpacity style={styles.primaryActionBtn} onPress={handleResetFlow}>
            <Text style={styles.primaryActionBtnText}>Make Another Payment</Text>
          </TouchableOpacity>
        </View>
      </SafeAreaView>
    );
  }

  // ---------------------------------------------------------------------------
  // 4. Error Screen
  // ---------------------------------------------------------------------------
  if (screenState === 'error') {
    const isHardwareError = errorMessage?.toLowerCase().includes('audio') || errorMessage?.toLowerCase().includes('chirp');
    return (
      <SafeAreaView style={styles.errorBody}>
        <StatusBar barStyle="dark-content" backgroundColor="#FFFFFF" />
        <View style={styles.errorCenterContent}>
          <View style={styles.errorIconCircle}>
            <Text style={styles.errorIconText}>✕</Text>
          </View>
          <Text style={styles.errorTitleText}>Transmission Failed</Text>
          <Text style={styles.errorMessageText}>
            {isHardwareError
              ? 'Acoustic chirp could not be played.\nEnsure speaker volume is up and try again.'
              : (errorMessage || 'An unexpected error occurred.')}
          </Text>
          {retryCount > 0 && (
            <View style={styles.retryBadge}>
              <Text style={styles.retryBadgeText}>Attempt #{retryCount}</Text>
            </View>
          )}
          <TouchableOpacity style={[styles.primaryActionBtn, { marginTop: 24, width: '100%' }]} onPress={() => {
            setErrorMessage(null);
            setScreenState('processing');
            handleSendPayment();
          }}>
            <Text style={styles.primaryActionBtnText}>↺  Retry Transmission</Text>
          </TouchableOpacity>
          <TouchableOpacity style={[styles.shareBtn, { marginTop: 10, width: '100%' }]} onPress={handleResetFlow}>
            <Text style={styles.shareBtnText}>← Change Amount</Text>
          </TouchableOpacity>
        </View>
      </SafeAreaView>
    );
  }

  // ---------------------------------------------------------------------------
  // 5. Main Keypad Screen
  // ---------------------------------------------------------------------------
  const floatVal      = parseFloat(amount);
  const isValInvalid  = isNaN(floatVal) || floatVal <= 0 || floatVal > MAX_AMOUNT_RUPEES;
  const isReadyToSend = isReady && nonceLoaded && !isValInvalid;

  return (
    <SafeAreaView style={styles.mainContainer}>
      <StatusBar barStyle="light-content" backgroundColor="#128a84" />

      {/* Top Header */}
      <View style={styles.headerRow}>
        <View style={styles.logoContainer}>
          <View style={styles.logoIconBox}>
            <Text style={styles.logoRupeeText}>₹</Text>
          </View>
          <Text style={styles.headerBrandTitle}>SonicPay</Text>
        </View>

        <View style={styles.headerActions}>
          <TouchableOpacity
            style={styles.headerIconBtn}
            onPress={() => setSoundEnabled(prev => !prev)}
          >
            <Text style={styles.headerIconText}>{soundEnabled ? '🔊' : '🔇'}</Text>
          </TouchableOpacity>

          <View style={styles.bellBtnWrapper}>
            <TouchableOpacity style={styles.headerIconBtn}>
              <Text style={styles.headerIconText}>🔔</Text>
            </TouchableOpacity>
            <View style={styles.notificationDot} />
          </View>
        </View>
      </View>

      {/* White Card Container */}
      <View style={styles.sheetContainer}>
        {/* Recipient Profile Card */}
        <View style={styles.recipientCard}>
          <View style={styles.recipientLeft}>
            <View style={styles.avatarCircle}>
              <Text style={styles.avatarText}>AK</Text>
            </View>
            <View>
              <View style={styles.recipientNameRow}>
                <Text style={styles.recipientName}>Aarav Kapoor</Text>
                <Text style={styles.verifiedCheck}>✓</Text>
              </View>
              <Text style={styles.recipientUpi}>UPI ID: aarav.kapoor@okhdfcbank</Text>
            </View>
          </View>
          <View style={styles.verifiedBadge}>
            <Text style={styles.verifiedBadgeText}>Verified</Text>
          </View>
        </View>

        {/* Amount Display Section — shakes on invalid tap */}
        <Animated.View style={[styles.amountDisplaySection, { transform: [{ translateX: shakeAnim }] }]}>
          <View style={styles.amountRow}>
            <Text style={styles.currencySymbol}>₹</Text>
            <Text style={styles.amountValueText}>{amount}</Text>
          </View>
          <Text style={styles.amountInWordsText}>{formatToWords(floatVal)}</Text>

          {isValInvalid && (
            <Text style={styles.errorMsgText}>
              {floatVal > MAX_AMOUNT_RUPEES
                ? `Max amount limit is ₹${MAX_AMOUNT_RUPEES.toLocaleString('en-IN')}`
                : 'Enter an amount greater than ₹0'}
            </Text>
          )}
        </Animated.View>

        {/* Tactile Keypad Frame */}
        <View style={styles.keypadFrame}>
          <View style={styles.keypadGrid}>
            {KEYS.map(key => (
              <TouchableOpacity
                key={key}
                activeOpacity={0.7}
                style={styles.keypadBtn}
                onPress={() => handleKeyPress(key)}
              >
                <Text style={styles.keypadBtnText}>{key}</Text>
              </TouchableOpacity>
            ))}
          </View>
        </View>

        {/* Bank & Pay Button Section */}
        <View style={styles.actionSection}>
          <View style={styles.bankSourceRow}>
            <View style={styles.bankSourceLeft}>
              <Text style={styles.bankBuildingIcon}>🏦</Text>
              <Text style={styles.bankText}>HDFC Bank •• 4912</Text>
            </View>
            <TouchableOpacity>
              <Text style={styles.changeBankText}>Change</Text>
            </TouchableOpacity>
          </View>

          <TouchableOpacity
            style={[styles.payBtn, (!isReadyToSend || isSubmitting) && styles.payBtnDisabled]}
            disabled={!isReadyToSend || isSubmitting}
            onPress={handleSendPayment}
          >
            {isSubmitting ? (
              <Text style={styles.payBtnText}>Transmitting chirp…</Text>
            ) : (
              <>
                <Text style={styles.payBtnText}>Pay Securely</Text>
                <Text style={styles.payBtnArrow}>➔</Text>
              </>
            )}
          </TouchableOpacity>
        </View>
      </View>
    </SafeAreaView>
  );
}

// ---------------------------------------------------------------------------
// Stylesheet - 100% Styled to Match Interactive Payment Interface
// ---------------------------------------------------------------------------

const styles = StyleSheet.create({
  // Splash Screen
  splashBody: {
    flex: 1,
    backgroundColor: '#128a84',
    justifyContent: 'center',
    alignItems: 'center',
  },
  splashLogoCard: {
    width: 110,
    height: 110,
    borderRadius: 28,
    backgroundColor: 'rgba(255, 255, 255, 0.2)',
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 20,
  },
  rupeeSplashLogo: {
    fontSize: 56,
    fontWeight: '700',
    color: '#FFFFFF',
  },
  splashBrandText: {
    fontSize: 26,
    fontWeight: '700',
    color: '#FFFFFF',
    letterSpacing: 3,
  },
  splashTagline: {
    fontSize: 11,
    fontWeight: '600',
    color: 'rgba(255, 255, 255, 0.75)',
    letterSpacing: 2,
    marginTop: 6,
  },

  // Main Keypad View
  mainContainer: {
    flex: 1,
    backgroundColor: '#128a84',
  },
  headerRow: {
    paddingHorizontal: 22,
    paddingTop: 14,
    paddingBottom: 16,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  logoContainer: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
  },
  logoIconBox: {
    width: 34,
    height: 34,
    borderRadius: 10,
    backgroundColor: 'rgba(255, 255, 255, 0.22)',
    justifyContent: 'center',
    alignItems: 'center',
  },
  logoRupeeText: {
    color: '#FFFFFF',
    fontSize: 16,
    fontWeight: 'bold',
  },
  headerBrandTitle: {
    color: '#FFFFFF',
    fontSize: 20,
    fontWeight: '700',
    letterSpacing: 1,
  },
  headerActions: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
  },
  headerIconBtn: {
    width: 36,
    height: 36,
    borderRadius: 18,
    backgroundColor: 'rgba(255, 255, 255, 0.15)',
    justifyContent: 'center',
    alignItems: 'center',
  },
  headerIconText: {
    fontSize: 16,
  },
  bellBtnWrapper: {
    position: 'relative',
  },
  notificationDot: {
    position: 'absolute',
    top: 6,
    right: 6,
    width: 8,
    height: 8,
    borderRadius: 4,
    backgroundColor: '#fbbf24',
  },

  // White Card Sheet
  sheetContainer: {
    flex: 1,
    backgroundColor: '#FFFFFF',
    borderTopLeftRadius: 36,
    borderTopRightRadius: 36,
    paddingHorizontal: 20,
    paddingTop: 20,
    paddingBottom: 16,
    justifyContent: 'space-between',
  },

  // Recipient Card
  recipientCard: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    backgroundColor: '#f8fafc',
    borderWidth: 1,
    borderColor: '#f1f5f9',
    borderRadius: 18,
    paddingHorizontal: 14,
    paddingVertical: 12,
  },
  recipientLeft: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
  },
  avatarCircle: {
    width: 44,
    height: 44,
    borderRadius: 22,
    backgroundColor: '#128a84',
    justifyContent: 'center',
    alignItems: 'center',
  },
  avatarText: {
    color: '#FFFFFF',
    fontWeight: 'bold',
    fontSize: 16,
  },
  recipientNameRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
  },
  recipientName: {
    fontSize: 15,
    fontWeight: '600',
    color: '#0f172a',
  },
  verifiedCheck: {
    fontSize: 12,
    color: '#128a84',
    fontWeight: 'bold',
  },
  recipientUpi: {
    fontSize: 11,
    color: '#64748b',
    marginTop: 1,
  },
  verifiedBadge: {
    backgroundColor: '#f0fdf4',
    borderWidth: 1,
    borderColor: '#bbf7d0',
    paddingHorizontal: 10,
    paddingVertical: 4,
    borderRadius: 12,
  },
  verifiedBadgeText: {
    color: '#16a34a',
    fontSize: 11,
    fontWeight: '600',
  },

  // Amount Display
  amountDisplaySection: {
    alignItems: 'center',
    marginVertical: 12,
  },
  amountRow: {
    flexDirection: 'row',
    alignItems: 'baseline',
    gap: 4,
  },
  currencySymbol: {
    fontSize: 28,
    fontWeight: '500',
    color: '#94a3b8',
  },
  amountValueText: {
    fontSize: 52,
    fontWeight: '700',
    color: '#0f172a',
    letterSpacing: -1,
  },
  amountInWordsText: {
    fontSize: 12,
    fontWeight: '600',
    color: '#94a3b8',
    textTransform: 'uppercase',
    marginTop: 4,
  },
  errorMsgText: {
    fontSize: 12,
    fontWeight: '600',
    color: '#f43f5e',
    marginTop: 6,
  },

  // Keypad
  keypadFrame: {
    backgroundColor: '#f8fafc',
    borderRadius: 24,
    borderWidth: 1,
    borderColor: '#e2e8f0',
    padding: 12,
    marginHorizontal: 8,
  },
  keypadGrid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    justifyContent: 'space-between',
    rowGap: 10,
  },
  keypadBtn: {
    width: '31%',
    height: 56,
    backgroundColor: '#FFFFFF',
    borderWidth: 1,
    borderColor: '#e2e8f0',
    borderBottomWidth: 3,
    borderBottomColor: '#cbd5e1',
    borderRadius: 16,
    justifyContent: 'center',
    alignItems: 'center',
  },
  keypadBtnText: {
    fontSize: 24,
    fontWeight: '500',
    color: '#0f172a',
  },

  // Action Buttons
  actionSection: {
    gap: 10,
    marginTop: 10,
  },
  bankSourceRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingHorizontal: 6,
  },
  bankSourceLeft: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
  },
  bankBuildingIcon: {
    fontSize: 14,
  },
  bankText: {
    fontSize: 12,
    color: '#64748b',
  },
  changeBankText: {
    fontSize: 12,
    color: '#128a84',
    fontWeight: '600',
  },
  payBtn: {
    height: 52,
    backgroundColor: '#128a84',
    borderRadius: 16,
    flexDirection: 'row',
    justifyContent: 'center',
    alignItems: 'center',
    gap: 8,
    shadowColor: '#128a84',
    shadowOffset: { width: 0, height: 4 },
    shadowOpacity: 0.25,
    shadowRadius: 8,
    elevation: 4,
  },
  payBtnDisabled: {
    backgroundColor: '#94a3b8',
    shadowOpacity: 0,
    elevation: 0,
  },
  payBtnText: {
    color: '#FFFFFF',
    fontSize: 17,
    fontWeight: '600',
  },
  payBtnArrow: {
    color: '#FFFFFF',
    fontSize: 16,
  },

  // Processing View
  processingBody: {
    flex: 1,
    backgroundColor: '#FFFFFF',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingVertical: 30,
    paddingHorizontal: 24,
  },
  processingContent: {
    alignItems: 'center',
    justifyContent: 'center',
    flex: 1,
    width: '100%',
  },
  visualizerContainer: {
    width: 220,
    height: 220,
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 30,
  },
  singleBreathingOrb: {
    width: 136,
    height: 136,
    borderRadius: 68,
    backgroundColor: '#128a84',
    justifyContent: 'center',
    alignItems: 'center',
    shadowColor: '#128a84',
    shadowOffset: { width: 0, height: 12 },
    shadowOpacity: 0.4,
    shadowRadius: 24,
    elevation: 12,
    borderWidth: 1.5,
    borderColor: 'rgba(255, 255, 255, 0.3)',
  },
  eqRow: {
    flexDirection: 'row',
    alignItems: 'flex-end',
    justifyContent: 'center',
    gap: 6,
    height: 40,
  },
  eqBar: {
    width: 6,
    backgroundColor: '#99f6e4',
    borderRadius: 3,
  },
  sonicRailText: {
    color: '#ccfbf1',
    fontSize: 10,
    fontFamily: Platform.OS === 'ios' ? 'Courier' : 'monospace',
    fontWeight: 'bold',
    letterSpacing: 2,
    marginTop: 8,
  },
  processingStatusPill: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
    backgroundColor: '#f0fdf4',
    borderWidth: 1,
    borderColor: '#ccfbf1',
    paddingHorizontal: 12,
    paddingVertical: 5,
    borderRadius: 14,
    marginBottom: 12,
  },
  waveformIcon: {
    fontSize: 10,
  },
  processingPillText: {
    color: '#0d9488',
    fontSize: 11,
    fontWeight: '700',
    letterSpacing: 1,
  },
  processingTitle: {
    fontSize: 22,
    fontWeight: '700',
    color: '#0f172a',
    marginBottom: 6,
  },
  processingSubtext: {
    fontSize: 12,
    color: '#128a84',
    textAlign: 'center',
    paddingHorizontal: 20,
    marginBottom: 8,
  },
  transferringText: {
    fontSize: 14,
    color: '#64748b',
    marginTop: 4,
  },
  boldAmount: {
    color: '#0f172a',
    fontWeight: 'bold',
  },
  securityFootnote: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
    paddingBottom: 10,
  },
  shieldIcon: {
    fontSize: 14,
  },
  securityText: {
    fontSize: 12,
    color: '#64748b',
    fontWeight: '500',
  },

  // Success View
  successBody: {
    flex: 1,
    backgroundColor: '#FFFFFF',
  },
  successScrollContainer: {
    alignItems: 'center',
    paddingHorizontal: 24,
    paddingTop: 30,
    paddingBottom: 20,
  },
  successTopSection: {
    alignItems: 'center',
    width: '100%',
  },
  successBadgeContainer: {
    width: 100,
    height: 100,
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 16,
  },
  successOuterGlow: {
    position: 'absolute',
    width: 100,
    height: 100,
    borderRadius: 50,
    backgroundColor: '#ccfbf1',
    opacity: 0.5,
  },
  successBadgeCircle: {
    width: 80,
    height: 80,
    borderRadius: 40,
    backgroundColor: '#128a84',
    justifyContent: 'center',
    alignItems: 'center',
    shadowColor: '#128a84',
    shadowOffset: { width: 0, height: 6 },
    shadowOpacity: 0.3,
    shadowRadius: 12,
    elevation: 6,
  },
  checkmarkSvgText: {
    color: '#FFFFFF',
    fontSize: 40,
    fontWeight: 'bold',
  },
  successHeading: {
    fontSize: 20,
    fontWeight: '800',
    color: '#0f172a',
    letterSpacing: 1,
  },
  successAmountText: {
    fontSize: 38,
    fontWeight: '800',
    color: '#128a84',
    marginTop: 6,
  },
  paidToCard: {
    width: '100%',
    backgroundColor: '#f8fafc',
    borderWidth: 1,
    borderColor: '#f1f5f9',
    borderRadius: 16,
    paddingVertical: 14,
    paddingHorizontal: 16,
    alignItems: 'center',
    marginTop: 20,
  },
  paidToLabel: {
    fontSize: 10,
    fontWeight: '700',
    color: '#94a3b8',
    letterSpacing: 1,
  },
  paidToName: {
    fontSize: 16,
    fontWeight: '700',
    color: '#0f172a',
    marginTop: 2,
  },
  paidToUpi: {
    fontSize: 12,
    color: '#64748b',
    marginTop: 2,
  },

  // Receipt Card
  receiptCard: {
    width: '100%',
    backgroundColor: '#f8fafc',
    borderWidth: 1,
    borderColor: '#f1f5f9',
    borderRadius: 16,
    padding: 16,
    marginTop: 16,
  },
  receiptRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  receiptLabel: {
    fontSize: 12,
    color: '#64748b',
  },
  receiptValue: {
    fontSize: 12,
    fontWeight: '600',
    color: '#0f172a',
  },
  receiptValueMono: {
    fontSize: 12,
    fontWeight: '600',
    color: '#0f172a',
    fontFamily: Platform.OS === 'ios' ? 'Courier' : 'monospace',
  },
  receiptDivider: {
    height: 1,
    backgroundColor: '#e2e8f0',
    marginVertical: 10,
  },

  // Success Footer Buttons
  successFooterButtons: {
    paddingHorizontal: 24,
    paddingBottom: 20,
    gap: 10,
  },
  shareBtn: {
    height: 48,
    backgroundColor: '#f1f5f9',
    borderRadius: 14,
    flexDirection: 'row',
    justifyContent: 'center',
    alignItems: 'center',
    gap: 8,
  },
  shareBtnIcon: {
    fontSize: 14,
  },
  shareBtnText: {
    fontSize: 14,
    fontWeight: '600',
    color: '#334155',
  },
  primaryActionBtn: {
    height: 50,
    backgroundColor: '#128a84',
    borderRadius: 14,
    justifyContent: 'center',
    alignItems: 'center',
  },
  primaryActionBtnText: {
    color: '#FFFFFF',
    fontSize: 16,
    fontWeight: '600',
  },

  // Error View
  errorBody: {
    flex: 1,
    backgroundColor: '#FFFFFF',
    justifyContent: 'center',
    alignItems: 'center',
    paddingHorizontal: 24,
  },
  errorCenterContent: {
    alignItems: 'center',
    width: '100%',
  },
  errorIconCircle: {
    width: 72,
    height: 72,
    borderRadius: 36,
    backgroundColor: '#ffe4e6',
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 16,
  },
  errorIconText: {
    fontSize: 36,
    fontWeight: 'bold',
    color: '#e11d48',
  },
  errorTitleText: {
    fontSize: 20,
    fontWeight: '700',
    color: '#0f172a',
    marginBottom: 8,
  },
  errorMessageText: {
    fontSize: 13,
    color: '#64748b',
    textAlign: 'center',
    marginBottom: 24,
  },
  retryBadge: {
    backgroundColor: '#fef3c7',
    borderWidth: 1,
    borderColor: '#fde68a',
    paddingHorizontal: 14,
    paddingVertical: 5,
    borderRadius: 20,
    marginBottom: 4,
  },
  retryBadgeText: {
    fontSize: 12,
    fontWeight: '600',
    color: '#92400e',
  },
});

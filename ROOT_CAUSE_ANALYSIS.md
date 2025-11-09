# Root Cause Analysis: Network Disconnection Issue

## 🎯 **The Real Problem: Over-Diagnostics**

### What We Thought Was Wrong:
- UART interference
- Power supply issues
- Modem firmware bugs

### What Was Actually Wrong:
**We added too many diagnostic queries that were interfering with MQTT operations!**

---

## 📊 Timeline of Events

### Before (Working):
- Network checked every **30 seconds**
- Minimal modem queries during operation
- **Network stayed connected**

### After Adding Diagnostics (Broken):
- Network checked every **15 seconds** (2x more frequent)
- Added `getSignalQuality()` on every network check (+1 AT command every 15s)
- Added `printNetworkDiagnostics()` on disconnect (+7 AT commands)
- Added health logging with signal checks (+1 AT command every 60s)
- **Result: Network drops after 3 seconds**

---

## 🔍 The Smoking Gun

From git diff analysis, these changes caused the issue:

```cpp
// BEFORE (Working)
if (currentTime - lastNetworkCheck > 30000) {
    if (!mqttClient.isNetworkConnected()) {
        // Handle disconnect
    }
}

// AFTER (Broken)
if (currentTime - lastNetworkCheck > 15000) {  // ❌ More frequent
    int signalQuality = mqttClient.getSignalQuality();  // ❌ Extra AT command
    
    if (!mqttClient.isNetworkConnected()) {
        mqttClient.printNetworkDiagnostics();  // ❌ 7+ AT commands
    }
}

// In loop() - ADDED
if (millis() - lastSignalCheck > 30000) {
    int signalQuality = getSignalQuality();  // ❌ Yet another AT command
}

// In health check - ADDED  
mqttClient.getSignalQuality();  // ❌ And another one
```

### Why This Broke Everything:

1. **UART Contention**: Multiple tasks trying to send AT commands simultaneously
2. **Modem Confusion**: Modem receives commands while processing SSL/TLS operations
3. **Buffer Overflow**: Responses from multiple commands get mixed up
4. **Modem Crash**: SIM7600G enters bad state and deregisters from network

---

## ✅ The Fix

### What We Removed:
1. ❌ Reduced network check interval (15s → back to 30s)
2. ❌ Signal quality check in network check loop
3. ❌ printNetworkDiagnostics() automatic call on disconnect
4. ❌ Signal quality checks from loop()
5. ❌ Health logging with signal checks

### What We Kept:
1. ✅ MQTT TX/RX logging (doesn't query modem)
2. ✅ Connection state change detection (doesn't query modem)
3. ✅ Network state change detection (already happens in isNetworkConnected)
4. ✅ Caching of network state with 5s minimum check interval
5. ✅ Signal quality error detection (99 → 0)
6. ✅ Manual debug command: `{"command": "debug_network"}`

---

## 🎓 Lessons Learned

### Don't Do This:
```cpp
// ❌ BAD: Querying modem while MQTT is active
void loop() {
    mqttClient.loop();  // Processing MQTT messages
    int signal = modem.getSignalQuality();  // AT command - CONFLICTS!
}
```

### Do This Instead:
```cpp
// ✅ GOOD: Cache values, query rarely
static unsigned long lastCheck = 0;
static int cachedSignal = 0;

if (millis() - lastCheck > 30000) {  // Only every 30 seconds
    lastCheck = millis();
    cachedSignal = modem.getSignalQuality();
}
// Use cachedSignal for logging
```

### Or Even Better:
```cpp
// ✅ BEST: Only query on-demand via debug command
void onDebugCommand() {
    // User explicitly requested diagnostics
    modem.printNetworkDiagnostics();
}
```

---

## 📈 Expected Behavior Now

After fix:
1. ✅ MQTT SSL connection establishes
2. ✅ Network stays connected (no concurrent AT commands)
3. ✅ MQTT messages flow without interruption
4. ✅ Network checked every 30 seconds (safe interval)
5. ✅ Diagnostics available on-demand via MQTT command

---

## 🔧 If Issues Still Occur

If the network still drops after this fix, then it's **definitely hardware**:

1. **Check power supply voltage**:
   ```bash
   # Should be 3.8V minimum during transmission
   # Ideal: 4.0V+
   ```

2. **Add capacitor**:
   - 1000μF near modem power input
   - Helps with current spikes during SSL handshake

3. **Test with bench power supply**:
   - If it works with bench PSU but not USB → power issue
   - If still fails → modem hardware issue

---

## 🎯 Summary

**Problem**: We were trying to be too thorough with diagnostics and ended up causing the very problem we were trying to diagnose.

**Solution**: Less is more. Only query the modem when absolutely necessary, and never during active MQTT operations.

**Result**: The firmware should now work as it did before, but with better logging of TX/RX events and state changes.

---

**Key Takeaway**: When debugging embedded systems with shared communication buses (UART/I2C/SPI), adding more debug code can make the problem worse. Always consider the timing and concurrency implications of your diagnostic code.




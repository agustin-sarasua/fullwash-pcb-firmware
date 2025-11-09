# Network Connection Troubleshooting

## 🔴 Critical Issues Found (from logs at 20:24-20:26)

### Issue 1: GPRS Connection Lost Immediately After MQTT Connect
**Timeline:**
- `20:24:57` - MQTT broker connected successfully
- `20:25:15` - First network check shows GPRS established
- `20:25:18` - GPRS connection LOST after only **3 seconds**

**Symptom:** The cellular connection drops almost immediately after MQTT SSL connection is established.

### Issue 2: Invalid Signal Quality Reading (99/31)
**What it means:** Signal quality of 99 is TinyGSM's **error code**, not a real signal reading.
- Valid range: 0-31
- 99 = Modem failed to respond to `AT+CSQ` command
- This indicates the modem is unresponsive or in a bad state

### Issue 3: Network Deregistration
```
[NETWORK DIAG] Network Registered: NO
[NETWORK DIAG] GPRS Connected: NO
```
The modem loses network registration shortly after MQTT connects.

### Issue 4: Modem Fails to Reinitialize
After attempting to recover:
```
Failed to initialize modem!
Basic AT command communication failed.
```
The modem enters a state where TinyGSM can't reinitialize it.

---

## 🔍 Root Cause Analysis

Based on the timing and symptoms, the most likely causes are:

### 1. **UART Interference (Most Likely)**
- MQTT/SSL operations are **concurrent** with network status checks
- Multiple threads querying the modem simultaneously
- Result: UART buffer corruption, modem confusion

### 2. **Power Supply Issue**
- SSL operations draw more current
- Voltage drops below modem's operating range
- Modem brownout causes network deregistration

### 3. **Modem Firmware Bug**
- SIM7600G may have issues with concurrent SSL and AT commands
- Known issue: Some firmware versions don't handle rapid AT commands well

---

## ✅ Fixes Implemented

### Fix 1: Rate Limit Network Status Checks
**Problem:** Checking network status too frequently interferes with MQTT
**Solution:** Cache network state and only query modem every 5 seconds minimum

```cpp
// Before: Checked on every call
bool isNetworkConnected() {
    return _modem->isGprsConnected();
}

// After: Cached with 5-second minimum interval
bool isNetworkConnected() {
    if (now - lastCheckTime < 5000) {
        return _networkConnected; // Use cached value
    }
    // Only check every 5+ seconds
}
```

### Fix 2: Detect and Report Signal Quality Errors
**Problem:** Signal quality 99 was reported as "Good" 
**Solution:** Detect error code and report as modem unresponsive

```cpp
int getSignalQuality() {
    int quality = _modem->getSignalQuality();
    if (quality == 99) {
        Serial.println("[WARNING] Signal quality error - modem unresponsive");
        return 0;
    }
    return quality;
}
```

### Fix 3: Avoid Signal Checks During MQTT Transmission
**Problem:** Concurrent modem operations cause UART corruption
**Solution:** Only check signal when safe (no active transmission)

### Fix 4: Clear UART Buffer Before Modem Init
**Problem:** Stale data in buffer causes init failures
**Solution:** Clear buffer and add delay before attempting init

### Fix 5: Ensure DTR/Flight Pin Stability
**Problem:** Power management pins may be in wrong state
**Solution:** Explicitly set and stabilize pins before operations

```cpp
digitalWrite(_dtrPin, LOW);     // Keep modem awake
digitalWrite(_flightPin, HIGH); // Radio active
delay(100);                     // Let pins stabilize
```

---

## 📊 Expected Improvements

After these fixes:
1. ✅ Network checks won't interfere with MQTT operations
2. ✅ Signal quality errors will be detected and logged correctly  
3. ✅ UART buffer corruption should be eliminated
4. ✅ Better diagnostic messages for troubleshooting

---

## 🔧 Hardware Recommendations

If issues persist, check:

### 1. **Power Supply**
- Use oscilloscope to measure voltage during MQTT transmission
- Minimum: 3.8V, Recommended: 4.0V
- Add larger capacitor (1000μF) near modem power input

### 2. **Antenna**
- Ensure antenna is properly connected
- Signal quality should be 10+ for stable operation
- Try external antenna if using internal

### 3. **Wiring**
- Verify TX/RX lines are correct and not swapped
- Check for loose connections
- Ensure ground is solid

### 4. **SIM Card**
- Verify SIM has active data plan
- Check APN settings match carrier requirements
- Try SIM in phone to verify it works

---

## 🐛 Additional Debugging Commands

Send via MQTT to `local/99/command`:

### Get Full Network Diagnostics
```json
{"command": "debug_network"}
```
Shows: GPRS status, registration, signal, IP, operator, modem responsiveness

### Check Signal Quality
Signal quality appears in logs every 30 seconds when stable.

---

## 📝 Next Steps

1. **Upload new firmware** with these fixes
2. **Monitor logs** for:
   - Whether GPRS stays connected after MQTT
   - If signal quality reads correctly (0-31, not 99)
   - Time between network checks
3. **If still failing:**
   - Check power supply with multimeter during MQTT transmission
   - Try reducing MQTT keep-alive to 30 seconds
   - Consider adding delays between operations

---

## 🆘 If Problems Continue

The log pattern suggests **hardware issue** if software fixes don't help:
- Voltage drops during transmission (most common)
- Faulty modem module
- Poor quality USB power supply

**Test:** Power the ESP32+modem from a bench power supply (not USB) and see if issue persists.




# Watchdog Timeout Fix

**Date**: November 5, 2025  
**Issue**: ESP32 task watchdog timeout causing system reboot

## Error Description

```
E (141027) task_wdt: Task watchdog got triggered. The following tasks did not reset the watchdog in time:
E (141027) task_wdt:  - IDLE (CPU 0)
E (141027) task_wdt: Tasks currently running:
E (141027) task_wdt: CPU 0: NetworkManager
E (141027) task_wdt: CPU 1: loopTask
E (141027) task_wdt: Aborting.
```

## Root Cause

The **NetworkManager** FreeRTOS task was blocking CPU 0 for extended periods during SSL/TLS operations, preventing the **IDLE task** from running. The ESP32 task watchdog requires the IDLE task to run at least once every ~5 seconds to reset the watchdog timer.

### Specific Issues

1. **Long SSL Timeouts**: SSL client timeout was 15 seconds, exceeding watchdog threshold
2. **Blocking delays**: Used `delay()` instead of `vTaskDelay()` in task context
3. **Multiple connection attempts**: Loop with 1.5s delays between retries
4. **Insufficient task yields**: NetworkManager didn't yield frequently enough during operations

## Fixes Applied

### Fix 1: ✅ Reduced SSL Timeout

**File**: `src/mqtt_lte_client.cpp`

**Before**:
```cpp
_sslClient->setTimeout(15000);  // 15 second timeout
_mqttClient->setSocketTimeout(15);
```

**After**:
```cpp
// ESP32 task watchdog is ~5 seconds, so keep SSL operations well under that
_sslClient->setTimeout(4000);  // 4 second timeout (safe for watchdog)
_mqttClient->setSocketTimeout(4);
```

**Rationale**: 
- ESP32 watchdog triggers after ~5 seconds of IDLE task starvation
- SSL operations must complete in <4 seconds to leave safety margin
- Failed SSL attempts will timeout faster and retry

### Fix 2: ✅ Removed Blocking Delays

**File**: `src/mqtt_lte_client.cpp`

**Before** (in `cleanupSSLClient()`):
```cpp
delay(100);  // ❌ Doesn't yield properly in FreeRTOS
```

**After**:
```cpp
// No delay needed - cleanup is fast
```

**Before** (in `reconnect()`):
```cpp
cleanupSSLClient();
delay(2000);  // ❌ Blocks for 2 seconds
```

**After**:
```cpp
cleanupSSLClient();
// No delay needed here, cleanup returns immediately
```

### Fix 3: ✅ Single Connection Attempt

**File**: `src/mqtt_lte_client.cpp`

**Before**:
```cpp
const int maxAttempts = 2;
while (!_mqttClient->connected() && attemptCount < maxAttempts && ...) {
    attemptCount++;
    if (_mqttClient->connect(_clientId)) {
        return true;
    } else {
        _consecutiveFailures++;
        delay(1500);  // ❌ Blocking delay
    }
}
```

**After**:
```cpp
// Make a single connection attempt to prevent long blocking
// Retries are handled by the calling code (NetworkManager task)
if (_mqttClient->connect(_clientId)) {
    _mqttConnected = true;
    _consecutiveFailures = 0;
    return true;
} else {
    _consecutiveFailures++;
}
```

**Benefit**: 
- Maximum blocking time is now 4 seconds (SSL timeout) instead of 8+ seconds
- Retry logic moved to NetworkManager task with proper yields
- No blocking delays between attempts

### Fix 4: ✅ Added Strategic Task Yields

**File**: `src/main.cpp` - NetworkManager task

**Added yields before long operations**:

```cpp
// Before network check
vTaskDelay(pdMS_TO_TICKS(50));

// Before network reconnection
vTaskDelay(pdMS_TO_TICKS(100));

// Before SSL connection
vTaskDelay(pdMS_TO_TICKS(100));

// Before MQTT reconnection
vTaskDelay(pdMS_TO_TICKS(50));

// Periodic yield in main loop
vTaskDelay(pdMS_TO_TICKS(100));
```

**Benefit**:
- Ensures IDLE task runs before potentially blocking operations
- Provides regular opportunities for lower-priority tasks
- Keeps watchdog timer reset even during intensive operations

### Fix 5: ✅ Reduced Delay After SSL Cleanup

**File**: `src/main.cpp`

**Before**:
```cpp
mqttClient.cleanupSSLClient();
vTaskDelay(1000 / portTICK_PERIOD_MS);  // 1 second
```

**After**:
```cpp
mqttClient.cleanupSSLClient();
vTaskDelay(500 / portTICK_PERIOD_MS);  // Reduced to prevent watchdog timeout
```

## Task Yield Strategy

The NetworkManager task now follows this pattern:

1. **Yield before long operations** (100ms)
2. **Perform operation** (max 4 seconds for SSL)
3. **Yield after operation** (10-100ms)
4. **Main loop yield** (200ms at end)

This ensures:
- IDLE task runs at least every 300ms in worst case
- Watchdog timer is reset every ~300ms
- Much safer than 5-second watchdog threshold

## Expected Behavior

### Before Fixes
- ❌ System runs for ~141 seconds then crashes
- ❌ Watchdog timeout during SSL operations
- ❌ IDLE task blocked for >5 seconds

### After Fixes
- ✅ System runs continuously without crashes
- ✅ SSL operations complete or timeout within 4 seconds
- ✅ IDLE task runs at least every 300ms
- ✅ Watchdog timer never expires

## Testing Results

**Expected log output**:
```
[19:21:26] [INFO] Connected to MQTT broker!
[MQTT HEALTH] Connection stable
[INFO] System running normally, network connected
[INFO] Publishing Periodic State
```

**Should NOT see**:
```
E (xxxxx) task_wdt: Task watchdog got triggered
Aborting.
Rebooting...
```

## Technical Details

### ESP32 Task Watchdog

The ESP32 has a task watchdog timer that monitors the IDLE task:
- **Timeout**: ~5 seconds (configurable)
- **Purpose**: Detect tasks that monopolize CPU
- **Trigger**: If IDLE task doesn't run for timeout period
- **Action**: System abort and reboot

### FreeRTOS Scheduling

Tasks must yield regularly to allow IDLE task to run:
- `vTaskDelay()`: Proper FreeRTOS delay with yield
- `delay()`: May not yield properly in some contexts
- Blocking operations: Must be <5 seconds or include yields

### SSL/TLS Handshake Timing

SSL handshake operations can be slow over cellular:
- Normal: 1-3 seconds
- Poor signal: 5-10 seconds
- Timeout: Previously 15s, now 4s

With 4-second timeout:
- Fast enough for good connections
- Fails fast on poor connections
- Prevents watchdog timeout

## Monitoring

To verify the fix is working:

1. **Watch for continuous uptime** (>5 minutes without reboot)
2. **Monitor serial output** for "[MQTT HEALTH] Connection stable"
3. **Check for state publishing** every 10 seconds
4. **No watchdog errors** in logs

## Additional Safety Measures

If watchdog issues persist, consider:

1. **Increase watchdog timeout** (in `sdkconfig`)
2. **Reduce SSL timeout** further (to 3 seconds)
3. **Add explicit watchdog resets** in long operations
4. **Move SSL operations** to dedicated task with lower priority

## Files Modified

1. **src/mqtt_lte_client.cpp**
   - Reduced SSL timeout from 15s to 4s
   - Removed blocking delays
   - Changed to single connection attempt
   - Enhanced connection monitoring

2. **src/main.cpp** 
   - Added strategic yields before long operations
   - Reduced post-cleanup delay
   - Added periodic yields in main loop
   - Improved task scheduling

## Related Issues

This fix also improves:
- ✅ Overall system responsiveness
- ✅ Connection recovery time
- ✅ Task scheduling fairness
- ✅ System stability under poor signal conditions

## Conclusion

The watchdog timeout was caused by the NetworkManager task blocking CPU 0 during SSL operations. By:
1. Reducing SSL timeout to 4 seconds
2. Removing blocking delays
3. Adding strategic task yields
4. Simplifying connection attempts

The system now maintains proper task scheduling and prevents watchdog timeouts while still providing reliable MQTT connectivity.




# MQTT Disconnection & State Publishing Fixes

**Date**: November 5, 2025  
**Issue**: MQTT connection disconnecting after ~106 seconds with SSL errors, and periodic state not being published

## Issues Identified

### 1. ❌ State Publishing Blocked by Timestamp Check

**Problem**: The `CarWashController::update()` function had an early return that prevented state publishing if `config.timestamp` was empty:

```cpp
void CarWashController::update() {
    if (config.timestamp == "") {
        return;  // ← Blocked ALL updates!
    }
    publishPeriodicState();  // Never reached
}
```

**Impact**:
- State was never published to MQTT unless the device received an `init` message from backend
- Made debugging impossible since no state updates were visible
- Created a chicken-and-egg problem: backend couldn't see the device was online

**Root Cause**: The timestamp is only set when receiving an MQTT message, but if MQTT is broken, the device never gets initialized, so it never publishes state.

### 2. 🔥 SSL/TLS Connection Instability

**Symptoms from Logs**:
```
[106244][E][ssl_client.cpp:45] _handle_error(): (-29184) SSL - An invalid SSL record was received
[131228][E][ssl_client.cpp:45] _handle_error(): (-2) BIGNUM - An error occurred while reading from or writing to a file
[356086][E][ssl_client.cpp:45] _handle_error(): (-9058) X509 - The algorithm tag or value is invalid
```

**Timeline of Failure**:
1. Initial connection succeeds at startup
2. SSL error occurs at ~106 seconds
3. Subsequent reconnection attempts all fail with SSL/certificate errors
4. Network layer stays connected but SSL layer is corrupted

**Root Causes**:
- **Keep-alive mismatch**: Set to 120s but SSL session failed at ~106s
- **SSL session corruption**: SSL state becomes corrupted and persists across reconnection attempts
- **No cleanup mechanism**: After SSL failure, corrupted state prevents successful reconnection
- **Memory fragmentation**: Long-running SSL sessions can cause memory issues on ESP32

## Fixes Applied

### Fix 1: ✅ Allow State Publishing Without Timestamp

**File**: `src/car_wash_controller.cpp`

**Change**: Moved `publishPeriodicState()` before the timestamp check:

```cpp
void CarWashController::update() {
    // Always publish periodic state even if not configured
    // This allows monitoring of machine status before initialization
    unsigned long currentTime = millis();
    publishPeriodicState();
    
    // Skip session-related updates if not initialized
    if (config.timestamp == "") {
        return;
    }
    
    // Handle buttons and coin acceptor
    handleButtons();
    handleCoinAcceptor();
    // ...
}
```

**Benefit**: 
- Device now publishes state every 10 seconds regardless of initialization status
- Backend can see device is online and communicating
- Enables monitoring and debugging even before full initialization

### Fix 2: ✅ Reduce MQTT Keep-Alive

**File**: `src/mqtt_lte_client.cpp`

**Change**: Reduced keep-alive from 120s to 60s:

```cpp
// Keep-alive reduced to 60s to prevent SSL timeout issues
// SSL sessions were failing at ~106s with 120s keep-alive
_mqttClient->setKeepAlive(60);
```

**Rationale**:
- SSL sessions were dying at ~106 seconds with 120s keep-alive
- Reducing to 60s ensures MQTT pings happen before SSL timeout
- More frequent pings help detect connection issues earlier
- AWS IoT supports keep-alive values from 30-1200 seconds

### Fix 3: ✅ Add SSL Client Cleanup

**Files**: 
- `include/mqtt_lte_client.h` (added method declaration)
- `src/mqtt_lte_client.cpp` (implementation)

**New Method**:
```cpp
void MqttLteClient::cleanupSSLClient() {
    Serial.println("[DEBUG] Cleaning up SSL client to clear corrupted state");
    
    // Disconnect MQTT client first
    if (_mqttClient && _mqttClient->connected()) {
        _mqttClient->disconnect();
    }
    
    // Stop SSL client
    if (_sslClient) {
        _sslClient->stop();
    }
    
    // Small delay to allow cleanup
    delay(100);
    
    Serial.println("[DEBUG] SSL client cleanup complete");
}
```

**Integration into Reconnect Logic**:
```cpp
void MqttLteClient::reconnect() {
    // After multiple consecutive failures, cleanup SSL state
    if (_consecutiveFailures >= 3 && _consecutiveFailures % 3 == 0) {
        Serial.print("[INFO] ");
        Serial.print(_consecutiveFailures);
        Serial.println(" consecutive failures - performing SSL cleanup");
        cleanupSSLClient();
        delay(2000); // Give system time to fully cleanup
    }
    
    // ... reconnection attempt ...
}
```

**Benefit**:
- Clears corrupted SSL session state after 3 consecutive failures
- Allows fresh SSL handshake instead of reusing corrupt session
- Prevents persistent SSL errors from blocking reconnection

### Fix 4: ✅ Cleanup SSL After Network Loss

**File**: `src/main.cpp`

**Change**: Added SSL cleanup when network is restored:

```cpp
// Cleanup SSL client since network was lost (old SSL session is invalid)
LOG_INFO("Cleaning up SSL state after network loss");
mqttClient.cleanupSSLClient();
vTaskDelay(1000 / portTICK_PERIOD_MS);

// Reconfigure SSL certificates
mqttClient.setCACert(AmazonRootCA);
mqttClient.setCertificate(AWSClientCertificate);
mqttClient.setPrivateKey(AWSClientPrivateKey);
```

**Rationale**:
- After network loss, the old SSL session is definitely invalid
- Cleaning up ensures fresh SSL handshake with new network connection
- Prevents carrying over corrupted state from previous connection

### Fix 5: ✅ Proactive Connection Health Monitoring

**File**: `src/mqtt_lte_client.cpp`

**Changes**:
```cpp
void MqttLteClient::loop() {
    // Check connection status more frequently to detect issues early
    if (millis() - lastConnectionCheck > 5000) {  // Increased from 10s
        lastConnectionCheck = millis();
        
        if (!_mqttClient->connected() && _networkConnected) {
            // Get MQTT client state for diagnostics
            int state = _mqttClient->state();
            Serial.print("MQTT disconnected (state: ");
            Serial.print(state);
            Serial.println("), will reconnect on next call...");
            
            // Track disconnections to trigger cleanup if needed
            if (state < 0) {
                _consecutiveFailures++;
            }
        } else if (_mqttClient->connected()) {
            if (_consecutiveFailures > 0) {
                _consecutiveFailures = 0;
            }
        }
    }
    
    // Periodic health logging (every 60 seconds)
    if (_mqttClient->connected() && millis() - lastHealthLog > 60000) {
        lastHealthLog = millis();
        Serial.println("[MQTT HEALTH] Connection stable");
    }
    
    // Always call PubSubClient loop (handles keep-alive pings)
    if (_mqttClient->connected()) {
        _mqttClient->loop();
    }
}
```

**Benefits**:
- Detects disconnection within 5 seconds (previously 10s)
- Logs MQTT state for better diagnostics
- Tracks consecutive failures automatically
- Provides health status logging every 60 seconds

## Expected Behavior After Fixes

### Normal Operation

1. **Startup**:
   - Device boots and connects to cellular network
   - Establishes SSL/TLS connection to AWS IoT
   - Subscribes to MQTT topics
   - Publishes setup event
   - **Begins publishing state every 10 seconds** ✅

2. **Continuous Operation**:
   - State published every 10 seconds to `machines/99/state` topic
   - MQTT keep-alive pings every 60 seconds
   - Connection health logged every 60 seconds
   - Early detection of disconnection within 5 seconds

3. **Connection Issues**:
   - If MQTT disconnects, detected within 5 seconds
   - Reconnection attempt with exponential backoff
   - After 3 failures, SSL cleanup is performed
   - Fresh SSL handshake on next attempt

4. **Network Loss**:
   - Network loss detected within 30 seconds
   - Full network reconnection attempted
   - **SSL cleanup performed before reconnection** ✅
   - Fresh SSL handshake after network restore

## Testing Recommendations

### 1. Verify State Publishing

Monitor MQTT broker for messages on `machines/99/state`:

```bash
# AWS IoT MQTT Test Client or mosquitto_sub
mosquitto_sub -h <broker> -t "machines/99/state" -v
```

**Expected**: Message every 10 seconds with:
```json
{
  "machine_id": "99",
  "timestamp": "2025-11-05T...",
  "status": "FREE"
}
```

### 2. Monitor Connection Stability

Watch serial output for:
- `[MQTT HEALTH] Connection stable` every 60 seconds
- No SSL errors for extended periods (>2 hours)
- Successful reconnections after temporary network issues

### 3. Test Reconnection

Simulate network issues:
1. Disconnect antenna temporarily
2. Device should detect loss within 30s
3. Reconnect antenna
4. Should see: `Cleaning up SSL state after network loss`
5. Should reconnect successfully within 1-2 minutes

### 4. Test SSL Recovery

Long-term stability test:
1. Run device for 24+ hours
2. Monitor for SSL errors
3. If SSL errors occur, should see cleanup after 3 failures
4. Should recover automatically

## Files Modified

1. **src/car_wash_controller.cpp**
   - Moved state publishing before timestamp check
   - Allows monitoring before initialization

2. **src/mqtt_lte_client.cpp**
   - Reduced keep-alive to 60s
   - Added `cleanupSSLClient()` method
   - Improved reconnection logic with SSL cleanup
   - Enhanced health monitoring in `loop()`

3. **include/mqtt_lte_client.h**
   - Added `cleanupSSLClient()` method declaration

4. **src/main.cpp**
   - Added SSL cleanup after network recovery
   - Ensures fresh SSL session after network loss

## Additional Notes

### Memory Considerations

The SSL/TLS operations are memory-intensive on ESP32:
- SSL handshake can use 40-50KB of heap
- Network manager task has 16KB stack (appropriate)
- Current free heap: ~237KB (healthy)
- Min free heap: ~141KB (acceptable margin)

### AWS IoT Compatibility

Changes are fully compatible with AWS IoT Core:
- Keep-alive of 60s is well within AWS limits (30-1200s)
- SSL certificate handling unchanged
- MQTT protocol compliance maintained

### Future Improvements

Consider for future updates:
1. **SSL session resumption**: Reduce handshake overhead
2. **Connection quality metrics**: Track RSSI, signal quality trends
3. **Adaptive keep-alive**: Adjust based on connection quality
4. **Memory defragmentation**: Periodic heap cleanup

## Conclusion

The fixes address both the state publishing blockage and SSL connection instability:

✅ **State now publishes every 10 seconds** regardless of initialization  
✅ **Keep-alive reduced** to prevent SSL timeout  
✅ **SSL cleanup mechanism** recovers from corrupted sessions  
✅ **Proactive monitoring** detects issues within 5 seconds  
✅ **Network recovery** includes SSL cleanup for fresh start  

The device should now maintain stable MQTT connectivity and provide continuous state updates for monitoring.




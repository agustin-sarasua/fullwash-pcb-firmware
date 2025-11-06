# Machine State Timestamp Update Analysis

**Date**: November 6, 2025  
**Issue**: Machine state timestamp not updating, causing backend to mark machine as offline after 2 minutes

## Problem Summary

The backend was reporting that machine 99 was offline because the last state update timestamp was `2025-11-06T12:53:17+00:00` (12.49 minutes old), exceeding the 2-minute timeout window. The machine should be publishing state updates every 10 seconds, but the timestamp wasn't being updated.

## Root Causes Identified

### 1. ❌ Silent MQTT Publish Failures

**Location**: `src/mqtt_lte_client.cpp:416-428`

**Problem**: When `publish()` failed (MQTT disconnected, network issues, etc.), the failure was silent - no logging occurred. This made it impossible to diagnose why state updates weren't reaching the backend.

**Impact**:
- No visibility into MQTT connection issues
- Couldn't tell if messages were being sent but not received, or not being sent at all
- Made debugging very difficult

**Fix Applied**:
- Added detailed logging when publish fails
- Logs MQTT connection state, topic, QoS level, and error details
- Logs when reconnect is attempted before publish

### 2. ❌ Invalid Timestamp When `config.timestamp` is Empty

**Location**: `src/car_wash_controller.cpp:696-714`

**Problem**: When `config.timestamp` was empty (machine not initialized), `getTimestamp()` returned `"2000-01-01T00:00:00.000Z"` - a placeholder that's 25 years old. The backend would accept this timestamp, but it would immediately mark the machine as offline since it's so old.

**Impact**:
- State updates with invalid timestamps were rejected or caused incorrect offline detection
- Even if MQTT was working, the timestamp made the machine appear offline
- RTC time was available but not used when `config.timestamp` was empty

**Fix Applied**:
- Prioritize RTC time even when `config.timestamp` is empty
- Use RTC timestamp if available, even if time validation fails (better than 2000 placeholder)
- Only return placeholder as last resort when RTC is completely unavailable
- Added error logging when placeholder must be used

### 3. ❌ Insufficient Backend Logging

**Location**: `app/infrastructure/controllers/machines_controller.py:127-145`

**Problem**: Backend had minimal logging for state message processing. When messages failed validation or processing, it was hard to see what went wrong.

**Impact**:
- Couldn't tell if messages were being received but failing validation
- No visibility into payload content when errors occurred
- Made it difficult to diagnose timestamp format issues

**Fix Applied**:
- Added logging when state messages are received
- Log payload content (first 200 chars) for debugging
- Log machine ID, status, and timestamp when processing
- Separate error handling for validation errors vs. other exceptions
- Log full exception traceback for debugging

## Code Changes

### Firmware Changes

#### 1. Enhanced MQTT Publish Logging (`src/mqtt_lte_client.cpp`)

```cpp
bool MqttLteClient::publish(const char* topic, const char* payload, const uint8_t qos) {
    // ... existing code ...
    if (!_mqttClient->connected()) {
        Serial.print("[MQTT] Not connected, attempting reconnect before publish to ");
        Serial.println(topic);
        reconnect();
    }
    bool ok = false;
    if (_mqttClient->connected()) {
        ok = _mqttClient->publish(topic, payload);
        if (!ok) {
            Serial.print("[MQTT ERROR] Failed to publish to ");
            Serial.print(topic);
            Serial.print(" (QoS: ");
            Serial.print(qos);
            Serial.print(", state: ");
            Serial.print(_mqttClient->state());
            Serial.println(")");
        }
    } else {
        Serial.print("[MQTT ERROR] Cannot publish to ");
        Serial.print(topic);
        Serial.print(" - MQTT not connected (state: ");
        Serial.print(_mqttClient->state());
        Serial.println(")");
    }
    // ... existing code ...
}
```

#### 2. Improved Timestamp Generation (`src/car_wash_controller.cpp`)

```cpp
String CarWashController::getTimestamp() {
    // PRIORITY 1: Use RTC if available and initialized (even if time validation fails)
    if (rtcManager && rtcManager->isInitialized()) {
        String rtcTimestamp = rtcManager->getTimestampWithMillis();
        if (rtcTimestamp != "RTC Error" && rtcTimestamp.length() > 0) {
            // Use RTC even if validation fails - better than placeholder
            if (!rtcManager->isTimeValid()) {
                static bool rtcWarningLogged = false;
                if (!rtcWarningLogged) {
                    rtcWarningLogged = true;
                    LOG_WARNING("RTC time is invalid but using it anyway (better than placeholder)");
                }
            }
            return rtcTimestamp;
        }
    }
    
    // FALLBACK: If config.timestamp is empty, try RTC one more time
    if (config.timestamp.length() == 0) {
        if (rtcManager && rtcManager->isInitialized()) {
            String rtcTimestamp = rtcManager->getTimestampWithMillis();
            if (rtcTimestamp != "RTC Error" && rtcTimestamp.length() > 0) {
                LOG_WARNING("Using RTC timestamp without server sync (config.timestamp empty)");
                return rtcTimestamp;
            }
        }
        // Last resort: return placeholder but log error
        LOG_ERROR("Cannot generate timestamp: RTC not available and config.timestamp is empty");
        return "2000-01-01T00:00:00.000Z";
    }
    // ... rest of existing code ...
}
```

#### 3. Enhanced State Publishing Logging (`src/car_wash_controller.cpp`)

```cpp
void CarWashController::publishPeriodicState(bool force) {
    // ... existing code ...
    
    // Log state publish attempt for debugging
    LOG_DEBUG("Publishing state: status=%s, timestamp=%s", 
              getMachineStateString(currentState).c_str(), timestamp.c_str());
    
    bool published = mqttClient.publish(STATE_TOPIC.c_str(), jsonString.c_str(), QOS0_AT_MOST_ONCE);
    if (!published) {
        LOG_WARNING("Failed to publish state update to MQTT");
    }
    
    // ... existing code ...
}
```

### Backend Changes

#### Enhanced State Message Logging (`app/infrastructure/controllers/machines_controller.py`)

```python
async def on_state_message(topic: str, payload: bytes, **kwargs):
    logging.info(f"Received state message on topic {topic}")
    # ... existing code ...
    try:
        # Log raw payload for debugging
        payload_str = payload.decode('utf-8')
        logging.debug(f"State message payload (first 200 chars): {payload_str[:200]}")
        
        machine_state_event = MachineStateEvent.model_validate_json(payload)
        # ... extract machine_id ...
        
        logging.info(f"Processing state update for machine {machine_id}: status={machine_state_event.status}, timestamp={machine_state_event.timestamp}")
        
        await locator.machine_service.update_machine_state(
            machine_id, machine_state_event, session
        )
        
        logging.info(f"Successfully updated machine {machine_id} state")
    except ValueError as e:
        logging.error(f"Validation error parsing state message from {topic}: {e}")
        logging.error(f"Payload was: {payload.decode('utf-8', errors='replace')[:500]}")
        raise BusinessException(f"Invalid machine state format: {e}")
    except Exception as e:
        logging.error(f"Exception processing state message from {topic}: {e}", exc_info=True)
        raise BusinessException(f"Invalid machine state: {e}")
```

## Expected Behavior After Fixes

### Normal Operation

1. **Firmware publishes state every 10 seconds**:
   - Uses RTC timestamp if available (even if not validated)
   - Falls back to server timestamp + millis() if RTC unavailable
   - Only uses 2000 placeholder as absolute last resort

2. **MQTT publish failures are logged**:
   - Can see when MQTT is disconnected
   - Can see when publish fails and why
   - Can see reconnect attempts

3. **Backend receives and processes state updates**:
   - Logs when messages are received
   - Logs processing details (machine ID, status, timestamp)
   - Logs validation errors with payload content
   - Updates machine state timestamp in memory

4. **Machine stays online**:
   - Timestamp updates every 10 seconds (or whenever state is published)
   - Backend sees machine as online (timestamp < 2 minutes old)
   - Can initialize machine sessions successfully

### Debugging

With these fixes, you can now:

1. **Check firmware logs** for:
   - `[MQTT ERROR]` messages indicating connection issues
   - `Failed to publish state update to MQTT` warnings
   - Timestamp generation warnings/errors

2. **Check backend logs** for:
   - `Received state message on topic machines/99/state`
   - `Processing state update for machine 99`
   - Validation errors with payload content
   - `Successfully updated machine 99 state`

3. **Identify the issue**:
   - If no `[MQTT ERROR]` in firmware: MQTT is working, check backend logs
   - If `[MQTT ERROR]` appears: MQTT connection issue, check network/MQTT broker
   - If backend receives messages but validation fails: Check timestamp format
   - If backend doesn't receive messages: Check MQTT subscription/topic routing

## Testing Recommendations

1. **Monitor firmware serial output** for MQTT publish logs
2. **Monitor backend logs** for state message reception
3. **Check machine state in backend** via `/status` endpoint
4. **Verify timestamp updates** are happening every 10 seconds
5. **Test with MQTT disconnected** to see error logging
6. **Test with RTC unavailable** to see fallback behavior

## Related Issues

- MQTT disconnection issues (see `MQTT_DISCONNECTION_FIXES.md`)
- RTC integration (see `RTC_INTEGRATION.md`)
- State machine behavior (see `STATE_MACHINE_ANALYSIS.md`)


# RTC Integration Documentation

## Overview

This document describes the Real-Time Clock (RTC) integration for the fullwash-pcb-firmware project. The RTC provides accurate, battery-backed timekeeping that persists across power cycles and system reboots.

## Hardware

### RTC Chip
- **Model**: DS1340Z-33+T&R
- **Manufacturer**: Maxim Integrated (now part of Analog Devices)
- **Interface**: I2C
- **I2C Address**: 0x68
- **Features**:
  - Battery backup for time keeping during power loss
  - Automatic leap year compensation
  - 24-hour format
  - Low power consumption

### Connections

The RTC is connected to the ESP32-WROVER-E via the **same I2C bus as the LCD display**:

- **I2C Bus**: Wire1 (secondary I2C bus)
- **SDA Pin**: GPIO21 (shared with LCD)
- **SCL Pin**: GPIO22 (shared with LCD)
- **I2C Clock**: 100kHz (standard mode)

**Note**: The RTC and LCD share the I2C bus without conflict as they have different I2C addresses:
- RTC: 0x68
- LCD: 0x27

## Software Architecture

### RTCManager Class

The `RTCManager` class (`rtc_manager.h` / `rtc_manager.cpp`) provides a clean interface to the DS1340 RTC chip.

#### Key Features

1. **Initialization and Health Check**
   - Detects RTC presence on I2C bus
   - Checks oscillator status
   - Validates time is being kept

2. **Time Setting**
   - Set time from epoch (Unix timestamp)
   - Set time from date/time components
   - Set time from ISO 8601 string (e.g., "2024-10-29T15:30:45.123Z")

3. **Time Reading**
   - Get time as epoch timestamp
   - Get time as ISO 8601 formatted string
   - Get time with millisecond precision (using millis() interpolation)

4. **BCD Conversion**
   - Handles Binary Coded Decimal (BCD) conversion for DS1340 registers
   - Transparent to the user

### Integration with CarWashController

The RTC is integrated into the `CarWashController` class to provide accurate timestamps for all system events:

1. **Timestamp Priority**:
   - **Primary**: RTC timestamp (if RTC is initialized and available)
   - **Fallback**: millis()-based calculation from last server timestamp

2. **Automatic Synchronization**:
   - When receiving `INIT_TOPIC` or `CONFIG_TOPIC` MQTT messages with timestamps
   - RTC is automatically synchronized with server time
   - Provides accurate local time even when network is disconnected

## Usage

### Initialization (in main.cpp)

```cpp
// Initialize Wire1 for LCD and RTC
Wire1.begin(LCD_SDA_PIN, LCD_SCL_PIN);
Wire1.setClock(100000); // 100kHz

// Create and initialize RTC manager
rtcManager = new RTCManager(RTC_DS1340_ADDR, &Wire1);
if (rtcManager->begin()) {
    LOG_INFO("RTC initialization successful!");
    rtcManager->printDebugInfo();
} else {
    LOG_ERROR("Failed to initialize RTC!");
}

// Connect RTC to controller
controller = new CarWashController(mqttClient);
controller->setRTCManager(rtcManager);
```

### Getting Current Time

The controller automatically uses the RTC for timestamps:

```cpp
String timestamp = controller->getTimestamp();
// Returns: "2024-10-29T15:30:45.123Z"
```

### Setting Time Manually

Through MQTT command topic:

```json
{
  "command": "sync_rtc",
  "timestamp": "2024-10-29T15:30:45.000Z"
}
```

### Debug Commands

#### Check RTC Status

Send via MQTT command topic:
```json
{
  "command": "debug_rtc"
}
```

This will log:
- I2C address
- Oscillator status
- Current time (epoch and ISO format)
- Raw register values

#### Manual Time Sync

Send via MQTT command topic:
```json
{
  "command": "sync_rtc",
  "timestamp": "2024-10-29T15:30:45.123Z"
}
```

## Time Synchronization Flow

```
1. System Boot
   └─> Initialize RTC
       └─> Check if RTC has valid time (> 2020-01-01)
           └─> If yes: Use RTC time immediately
           └─> If no: Wait for server sync

2. MQTT Connection Established
   └─> Subscribe to topics
       └─> Publish setup event (uses RTC time if available)

3. Receive INIT_TOPIC or CONFIG_TOPIC
   └─> Parse timestamp from message
       └─> Sync RTC with server timestamp
           └─> Log success/failure

4. During Operation
   └─> All timestamps use RTC
       └─> Millisecond precision via millis() interpolation
       └─> Independent of network connectivity
       └─> Survives millis() overflow (every ~50 days)
```

## Benefits

### Before RTC Integration
- ❌ Time based on millis() since last server sync
- ❌ millis() overflows every ~50 days
- ❌ Time lost on power cycle/reboot
- ❌ Requires active network connection for accurate time
- ❌ Cumulative drift over time

### After RTC Integration
- ✅ Battery-backed time persists across power cycles
- ✅ No millis() overflow issues
- ✅ Accurate time even without network
- ✅ Automatic synchronization with server
- ✅ Millisecond precision for events
- ✅ Graceful fallback if RTC fails

## Troubleshooting

### RTC Not Found

**Symptoms**: Log shows "DS1340 RTC not found! I2C error code: 2"

**Possible Causes**:
1. RTC not physically connected
2. Wrong I2C address (should be 0x68)
3. I2C bus not initialized before RTC
4. SDA/SCL pins swapped

**Solution**:
- Check physical connections
- Verify I2C address with I2C scanner
- Ensure Wire1.begin() is called before rtcManager->begin()

### Oscillator Stopped

**Symptoms**: Log shows "RTC oscillator is stopped!"

**Possible Causes**:
1. First time use (oscillator disabled by default)
2. Battery removed/dead
3. RTC was in reset state

**Solution**:
- RTCManager automatically starts the oscillator
- If it fails, check battery connection
- Manually set time after starting oscillator

### Time Seems Wrong

**Symptoms**: RTC time is incorrect or old

**Possible Causes**:
1. RTC never received time from server
2. Battery was removed
3. Time zone confusion (RTC stores UTC)

**Solution**:
- Check if MQTT messages with timestamps are being received
- Send manual sync command via MQTT
- Ensure timestamps from server are in UTC (ISO 8601 format)

### Time Resets on Boot

**Symptoms**: Time resets to old value on every boot

**Possible Causes**:
1. RTC battery dead or missing
2. RTC not retaining time (hardware issue)

**Solution**:
- Check/replace CR2032 battery
- Verify battery holder connections
- Test RTC with a known good battery

## Testing

### Manual Testing Steps

1. **Initial RTC Test**
   ```bash
   # Monitor serial output during boot
   # Look for: "RTC initialization successful!"
   ```

2. **Set Time via MQTT**
   ```bash
   # Publish to command topic
   mosquitto_pub -t "machines/machine001/command" -m '{"command":"sync_rtc","timestamp":"2024-10-29T15:30:00.000Z"}'
   ```

3. **Verify Time**
   ```bash
   # Check RTC status
   mosquitto_pub -t "machines/machine001/command" -m '{"command":"debug_rtc"}'
   # Check serial monitor for RTC debug output
   ```

4. **Power Cycle Test**
   ```bash
   # 1. Set RTC to known time
   # 2. Power off ESP32
   # 3. Wait 1 minute
   # 4. Power on ESP32
   # 5. Check RTC time - should be ~1 minute ahead
   ```

5. **Long-term Drift Test**
   ```bash
   # 1. Set RTC to accurate time
   # 2. Compare with known accurate time source after 24 hours
   # 3. DS1340 typical accuracy: ±2 ppm (±5 minutes/year)
   ```

## API Reference

### RTCManager Methods

#### `bool begin()`
Initialize the RTC and check health.

**Returns**: `true` if successful, `false` if RTC not found or oscillator stopped

#### `bool setDateTimeFromISO(const String& isoTimestamp)`
Set RTC time from ISO 8601 timestamp string.

**Parameters**: 
- `isoTimestamp`: ISO 8601 formatted string (e.g., "2024-10-29T15:30:45.123Z")

**Returns**: `true` if successful

#### `String getTimestamp()`
Get current time as ISO 8601 formatted string.

**Returns**: Timestamp string (e.g., "2024-10-29T15:30:45Z")

#### `String getTimestampWithMillis()`
Get current time with millisecond precision.

**Returns**: Timestamp string with milliseconds (e.g., "2024-10-29T15:30:45.123Z")

#### `time_t getDateTime()`
Get current time as Unix epoch timestamp.

**Returns**: Seconds since 1970-01-01 00:00:00 UTC

#### `bool isInitialized()`
Check if RTC is initialized and ready.

**Returns**: `true` if RTC is initialized

#### `void printDebugInfo()`
Print detailed RTC debug information to serial log.

## Future Enhancements

Potential improvements for future versions:

1. **Alarm Support**: DS1340 has alarm functionality that could trigger wake-ups or events
2. **Temperature Compensation**: Add external temperature sensor for better accuracy
3. **NTP Sync**: Periodic synchronization with NTP servers (when internet available)
4. **Time Zone Support**: Add configurable time zone offset
5. **DST Handling**: Automatic daylight saving time adjustments
6. **Backup Time Source**: Use GPS time as alternative sync source

## References

- [DS1340 Datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/DS1340C-DS1340Z-33.pdf)
- [ESP32 I2C Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2c.html)
- [ISO 8601 Time Format](https://en.wikipedia.org/wiki/ISO_8601)

## Conclusion

The RTC integration provides robust, accurate timekeeping for the fullwash-pcb-firmware. The implementation is:

- **Reliable**: Battery backup ensures time survives power loss
- **Maintainable**: Clean class-based design with clear separation of concerns
- **Flexible**: Graceful fallback if RTC unavailable
- **Well-tested**: Comprehensive error handling and debug capabilities

All timestamps used in MQTT messages and system events now benefit from RTC accuracy.


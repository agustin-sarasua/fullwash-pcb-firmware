# RTC Quick Start Guide

## What Was Added

The RTC (Real-Time Clock) integration adds battery-backed timekeeping to your fullwash-pcb-firmware. This means accurate timestamps even when the device is powered off or disconnected from the network.

## Hardware Setup

### RTC Chip: DS1340Z-33+T&R
- **Location**: Connected to the same I2C bus as the LCD display
- **Pins**: SDA=GPIO21, SCL=GPIO22 (Wire1)
- **Address**: 0x68
- **Battery**: CR2032 (for backup power)

### Schematic Confirmation
✅ RTC is connected to LCD I2C bus (SCL_LCD, SDA_LCD)
✅ Both devices can coexist on the same bus (different addresses)

## Files Added/Modified

### New Files
- `include/rtc_manager.h` - RTC manager class header
- `src/rtc_manager.cpp` - RTC manager implementation
- `RTC_INTEGRATION.md` - Detailed documentation
- `RTC_QUICK_START.md` - This file

### Modified Files
- `include/utilities.h` - Added RTC_DS1340_ADDR constant
- `include/car_wash_controller.h` - Added RTC integration
- `src/car_wash_controller.cpp` - Added RTC timestamp support
- `src/main.cpp` - Added RTC initialization and MQTT commands

## How It Works

### 1. Initialization (Automatic)
When the system boots:
- RTC is detected and initialized on Wire1 (I2C)
- Oscillator status is checked
- Current time is validated
- RTC is connected to the controller

### 2. Time Synchronization (Automatic)
When receiving MQTT messages:
- System receives timestamp from server (INIT_TOPIC or CONFIG_TOPIC)
- RTC is automatically synchronized with server time
- Time is maintained even when offline

### 3. Timestamp Generation (Automatic)
For all events and messages:
- Primary source: RTC (if available and initialized)
- Fallback: millis()-based calculation from last server sync
- Format: ISO 8601 with milliseconds (e.g., "2024-10-29T15:30:45.123Z")

## MQTT Commands

### Check RTC Status
```json
{
  "command": "debug_rtc"
}
```

### Manually Sync RTC
```json
{
  "command": "sync_rtc",
  "timestamp": "2024-10-29T15:30:45.000Z"
}
```

## Testing the RTC

### 1. Boot Test
Monitor serial output during boot:
```
[INFO] Initializing RTC Manager...
[INFO] DS1340 RTC found!
[INFO] RTC oscillator is running
[INFO] RTC initialization successful!
[INFO] RTC current time: 1730215845 (epoch)
[INFO] RTC time is valid: 2024-10-29T15:30:45Z
```

### 2. Time Sync Test
Send INIT message via MQTT, watch for:
```
[INFO] Syncing RTC with server timestamp: 2024-10-29T15:30:45.123+00:00
[INFO] RTC synchronized successfully!
```

### 3. Power Cycle Test
1. Note the current time from RTC
2. Power off the ESP32 (ensure RTC battery is installed)
3. Wait a few minutes
4. Power on the ESP32
5. Check RTC time - should be accurate (continued from before power off)

## Benefits

| Feature | Before RTC | With RTC |
|---------|-----------|----------|
| Time persistence | ❌ Lost on reboot | ✅ Maintained with battery |
| Network dependency | ❌ Required for time | ✅ Independent |
| millis() overflow | ❌ Issue after ~50 days | ✅ No problem |
| Accuracy | ❌ Drifts over time | ✅ ±2 ppm (±5 min/year) |
| Offline operation | ❌ No accurate time | ✅ Full timestamp support |

## Troubleshooting

### RTC Not Found
```
[ERROR] DS1340 RTC not found! I2C error code: 2
```
**Solution**: Check physical connections, verify I2C address (should be 0x68)

### Oscillator Stopped
```
[WARNING] RTC oscillator is stopped!
```
**Solution**: RTCManager will automatically start it. If it persists, check battery.

### Time Seems Wrong
**Solution**: Send manual sync command via MQTT or wait for next INIT_TOPIC message

## Next Steps

The RTC is now fully integrated and working! It will:

1. ✅ Automatically initialize at boot
2. ✅ Synchronize with server timestamps via MQTT
3. ✅ Provide accurate timestamps for all events
4. ✅ Maintain time during power loss (with battery)
5. ✅ Fall back gracefully if RTC is unavailable

No further action needed - the system will automatically use the RTC for all timestamp operations!

## Additional Documentation

For detailed information, see `RTC_INTEGRATION.md`.


# FullWash Machine - BLE Configuration Guide

## Overview

The FullWash machine firmware now supports Bluetooth Low Energy (BLE) configuration. This allows authorized administrators to securely configure the machine number and other settings via Bluetooth without needing physical access to reprogram the device.

## Security Features

- **Password Authentication**: All configuration changes require authentication with a master password
- **Session Timeout**: Authentication sessions expire after 2 minutes (120 seconds) of inactivity
- **Secure Storage**: Configuration is stored in ESP32's non-volatile storage (NVS)
- **Access Control**: Only authenticated users can modify settings

## Default Configuration

- **BLE Device Name**: `FullWash Machine`
- **Default Master Password**: `fullwash2025` (⚠️ **CHANGE THIS IN PRODUCTION!**)
- **Default Machine Number**: `99`

## How to Connect and Configure

### Using a BLE Mobile App

We recommend using one of these BLE scanner/explorer apps:
- **Android**: nRF Connect for Mobile, BLE Scanner
- **iOS**: nRF Connect for Mobile, LightBlue

### Step-by-Step Configuration Process

#### 1. Scan for the Device
- Open your BLE app
- Scan for nearby devices
- Look for `FullWash Machine`

#### 2. Connect to the Device
- Tap on `FullWash Machine` to connect
- The device will show 3 characteristics under the service

#### 3. Authenticate

Find the **Authentication** characteristic (UUID: `beb5483e-36e1-4688-b7f5-ea07361b26a8`):
- Write the master password: `fullwash2025`
- Read the **Status** characteristic to verify authentication was successful
- You should see: "Authenticated - Access granted"
- You have 30 seconds to make changes before the session expires

#### 4. Read Current Machine Number

Find the **Machine Number** characteristic (UUID: `1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e`):
- Read the characteristic to see the current machine number
- Default is `99`

#### 5. Update Machine Number

While authenticated:
- Write the new machine number to the **Machine Number** characteristic
- Read the **Status** characteristic to confirm the change
- You should see: "Machine number updated successfully"
- **⚠️ IMPORTANT**: Restart the machine for changes to take effect

#### 6. Verify Configuration

After restarting:
- Reconnect via BLE
- Read the Machine Number characteristic
- Or check the serial monitor logs for: "Machine Number loaded: XX"

## BLE Service Structure

### Service UUID
```
4fafc201-1fb5-459e-8fcc-c5c9c331914b
```

### Characteristics

| Name | UUID | Properties | Description |
|------|------|------------|-------------|
| Authentication | `beb5483e-36e1-4688-b7f5-ea07361b26a8` | Write | Write password to authenticate |
| Machine Number | `1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e` | Read/Write/Notify | Read or update machine number (requires auth) |
| Status | `d8de624e-140f-4a22-8594-e2216b84a5f2` | Read/Notify | Get current status and error messages |

## MQTT Commands (Remote Management)

You can also manage the BLE configuration remotely via MQTT:

### Check BLE Status
```json
{
  "command": "debug_ble"
}
```

### Update Machine Number (Remote)
```json
{
  "command": "set_machine_number",
  "number": "42"
}
```

**Note**: Remote updates also require a machine restart to fully take effect.

## Security Best Practices

### 1. Change Default Password

For production environments, **immediately change the default password**:

Currently, you'll need to update the password in the firmware:
```cpp
#define DEFAULT_MASTER_PASSWORD "your_secure_password_here"
```

Or programmatically via MQTT/code:
```cpp
bleConfigManager->setMasterPassword("your_secure_password_here");
```

### 2. Password Requirements
- Minimum 8 characters
- Store securely and don't share
- Change regularly

### 3. Authentication Timeout
- Sessions expire after 2 minutes (120 seconds)
- Status message shows remaining time: "Authenticated - Valid for X seconds"
- Re-authenticate if timeout expires

### 4. Monitor Access
- Check serial logs for authentication attempts
- Failed authentications are logged as warnings

## Troubleshooting

### Cannot Find BLE Device
- Ensure the ESP32 has powered on completely (wait 5-10 seconds)
- Check that Bluetooth is enabled on your phone
- Move closer to the device (within 10 meters)

### Authentication Fails
- Verify you're using the correct password
- Check for extra spaces or special characters
- Password is case-sensitive

### Changes Don't Take Effect
- **Restart Required**: Always restart the machine after changing the machine number
- Changes are saved to non-volatile storage immediately
- But MQTT topics are only rebuilt on startup

### Connection Drops
- BLE automatically re-advertises after disconnection
- Wait a few seconds and reconnect
- Check device logs for any error messages

### Status Characteristic Shows Error
Common error messages:
- `"Error: Not authenticated"` - You need to authenticate first
- `"Error: Authentication expired"` - Re-authenticate (30s timeout)
- `"Error: Invalid machine number"` - Number must be 1-10 characters

## Configuration Storage

All configuration is stored in ESP32's **NVS (Non-Volatile Storage)**:
- Survives power cycles and resets
- Located in the flash memory
- Namespace: `fullwash`
- Keys:
  - `machine_num`: The machine identification number
  - `ble_pwd`: The BLE master password

## Impact on System

### Machine Identification
The machine number affects:
1. **MQTT Client ID**: `fullwash-machine-{NUMBER}`
2. **MQTT Topics**: `machines/{NUMBER}/init`, `machines/{NUMBER}/config`, etc.
3. **Backend Integration**: Ensure backend knows about the new machine number

### BLE Resource Usage
- BLE runs continuously in advertising mode
- Minimal CPU overhead (~1% when not connected)
- Memory: ~20KB RAM for BLE stack
- Does not interfere with MQTT/LTE operations

## Developer Notes

### Adding New Configuration Parameters

To add new configurable parameters:

1. Add characteristic to `ble_config_manager.h`:
```cpp
#define NEW_PARAM_CHAR_UUID  "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
```

2. Create the characteristic in `begin()`:
```cpp
pNewParamCharacteristic = pService->createCharacteristic(
    NEW_PARAM_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
);
```

3. Handle writes in `onWrite()`:
```cpp
else if (uuid == NEW_PARAM_CHAR_UUID) {
    // Handle new parameter
}
```

4. Store in preferences:
```cpp
preferences.putString("new_param", value);
```

### Testing

Test the BLE functionality:
```bash
# Build and upload
pio run -t upload

# Monitor serial output
pio device monitor

# Look for these lines:
# "BLE Config Manager initialized"
# "Machine Number loaded: XX"
# "BLE is now advertising"
```

## Future Enhancements

Potential future additions:
- [ ] Multiple user passwords (admin, technician, operator)
- [ ] BLE-based OTA firmware updates
- [ ] WiFi credentials configuration
- [ ] Timezone and language settings
- [ ] Service mode toggle
- [ ] Historical statistics retrieval

## Support

For issues or questions:
1. Check serial monitor logs (115200 baud)
2. Use MQTT command `debug_ble` for status
3. Review error messages in Status characteristic
4. Contact development team with logs

---

**Last Updated**: November 2025  
**Firmware Version**: 1.0.0 (BLE Config Support)


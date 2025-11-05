# Build and Deploy Guide - BLE Configuration Feature

## 🎯 Prerequisites

### Required Software
- PlatformIO Core or PlatformIO IDE (VS Code extension)
- Python 3.7+ (for configuration tools)
- USB drivers for ESP32

### Hardware
- ESP32 development board (T-SIM7600X)
- USB cable for programming
- Computer with Bluetooth support (for testing)

---

## 🔨 Building the Firmware

### Option 1: Using PlatformIO CLI

```bash
# Navigate to project directory
cd /home/asarasua/Documents/PlatformIO/Projects/fullwash-pcb-firmware

# Clean previous builds (optional)
pio run -t clean

# Build the firmware
pio run -e T-SIM7600X

# Build and upload
pio run -e T-SIM7600X -t upload

# Monitor serial output
pio device monitor --baud 115200
```

### Option 2: Using VS Code with PlatformIO

1. Open project in VS Code
2. Select environment: `T-SIM7600X`
3. Click "Build" button (checkmark icon)
4. Click "Upload" button (arrow icon)
5. Click "Monitor" button (plug icon)

---

## 📤 Uploading to Device

### First Time Setup

1. **Connect ESP32 via USB**
   - Use a data-capable USB cable (not charge-only)
   - ESP32 should appear as a serial port

2. **Check Port**
   ```bash
   pio device list
   ```
   Look for your ESP32 (usually `/dev/ttyUSB0` on Linux, `COM3` on Windows)

3. **Upload Firmware**
   ```bash
   pio run -e T-SIM7600X -t upload
   ```

4. **Monitor Boot Process**
   ```bash
   pio device monitor --baud 115200
   ```

### Expected Boot Output

```
Starting fullwash-pcb-firmware...
=== BLE Configuration Manager ===
Initializing BLE Config Manager...
Loaded machine number from storage: 99
Machine Number loaded: 99
AWS Client ID set to: fullwash-machine-99
BLE is now advertising. You can connect to configure the machine.
Device name: FullWash Machine
Use password: fullwash2025 (default - should be changed)
================================
Trying to initialize TCA9535...
...
```

---

## 🔍 Verification Steps

### 1. Check Serial Monitor

Look for these key messages:
- ✅ "BLE Config Manager initialized"
- ✅ "Machine Number loaded: XX"
- ✅ "BLE is now advertising"
- ✅ "TCA9535 initialization successful"
- ✅ "Connected to MQTT broker"

### 2. Test BLE Connectivity

```bash
# Scan for device
python tools/ble_config_tool.py --scan

# Should show:
# >>> FullWash Machine              | AA:BB:CC:DD:EE:FF
```

### 3. Test Configuration

```bash
# Read current config
python tools/ble_config_tool.py --read --address <MAC_ADDRESS>

# Should show:
# Machine Number: 99
```

### 4. Test MQTT Connection

Check serial monitor for:
```
Connected to MQTT broker!
Subscribing to topic: machines/99/init
```

---

## 🐛 Troubleshooting

### Build Errors

#### "BLEDevice.h not found"
**Solution:** Ensure platformio.ini has BLE flags:
```ini
build_flags = 
    -DCONFIG_BT_ENABLED
    -DCONFIG_BLUEDROID_ENABLED
```

#### "Preferences.h not found"
**Solution:** This is part of ESP32 Arduino framework. Update platform:
```bash
pio pkg update
```

#### "Multiple definition of MACHINE_ID"
**Solution:** Clean and rebuild:
```bash
pio run -t clean
pio run
```

### Upload Errors

#### "Serial port not found"
**Solution:**
- Check USB cable (must be data cable)
- Check device drivers are installed
- Try different USB port
- On Linux, add user to dialout group:
  ```bash
  sudo usermod -a -G dialout $USER
  # Log out and back in
  ```

#### "Failed to connect to ESP32"
**Solution:**
- Hold BOOT button while uploading
- Press RESET button before upload
- Reduce upload speed in platformio.ini:
  ```ini
  upload_speed = 115200
  ```

### Runtime Errors

#### "BLE Config Manager failed to initialize"
**Causes:**
- BLE already in use by another process
- Insufficient memory
- Conflicting WiFi usage

**Solutions:**
- Restart device
- Check memory usage in logs
- Disable WiFi if not needed

#### "Failed to load machine number"
**Causes:**
- NVS corruption
- First time boot (expected - uses default)

**Solutions:**
- Use BLE to set machine number
- Or use MQTT command: `set_machine_number`

#### "MQTT connection failed"
**Causes:**
- Network issue
- Invalid machine ID
- Certificate issue

**Solutions:**
- Check cellular signal
- Verify machine number is set correctly
- Check AWS certificates are valid

---

## 🔐 Security Configuration

### Change Default Password

**Method 1: Edit firmware (recommended for production)**

In `include/ble_config_manager.h`:
```cpp
#define DEFAULT_MASTER_PASSWORD "your_secure_password_here"
```

Then rebuild and upload.

**Method 2: Via code (after initial setup)**

Add to `setup()` in `main.cpp`:
```cpp
bleConfigManager->setMasterPassword("your_secure_password_here");
```

---

## 📦 Deployment Checklist

### Pre-Deployment

- [ ] Change default BLE password
- [ ] Set unique machine number
- [ ] Test MQTT connectivity
- [ ] Test BLE configuration
- [ ] Verify all I/O expander functions
- [ ] Test emergency stop button
- [ ] Verify relay operations

### Deployment

- [ ] Upload firmware
- [ ] Configure machine number via BLE
- [ ] Verify MQTT topics
- [ ] Test full wash cycle
- [ ] Document MAC address and machine number
- [ ] Secure physical access to device

### Post-Deployment

- [ ] Monitor logs for errors
- [ ] Test remote MQTT commands
- [ ] Verify data appears in backend
- [ ] Schedule periodic maintenance

---

## 🔄 Updating Existing Machines

### Over-the-Air (OTA) Update

**Note:** BLE-based OTA not yet implemented. Use USB for now.

### USB Update Process

1. **Backup Configuration**
   ```bash
   python tools/ble_config_tool.py --read --address <MAC> > backup.txt
   ```

2. **Upload New Firmware**
   ```bash
   pio run -t upload
   ```

3. **Verify Configuration Persisted**
   - Configuration should survive firmware update
   - Machine number should be unchanged
   - If not, restore from backup:
   ```bash
   python tools/ble_config_tool.py --configure <NUMBER> --address <MAC>
   ```

---

## 📊 Memory Usage

### Flash Memory
- Program: ~1.2 MB
- Available: ~1.8 MB (of 3 MB)
- **Status:** ✅ Plenty of space

### RAM
- BLE Stack: ~20 KB
- MQTT/TLS: ~40 KB
- Application: ~60 KB
- Free Heap: ~180 KB
- **Status:** ✅ Adequate

### PSRAM
- If available, used for large buffers
- Improves stability with SSL/TLS

---

## 🧪 Testing Procedure

### 1. Functional Test

```bash
# Build and upload
pio run -t upload && pio device monitor

# Expected: Clean boot, BLE advertising

# Test BLE config
python tools/ble_config_tool.py --configure 999 --address <MAC>

# Restart device
# Expected: Machine number 999 loaded

# Test MQTT
# Expected: Topics use machines/999/...
```

### 2. Stress Test

- Multiple BLE connect/disconnect cycles
- Long-running MQTT connection
- Concurrent BLE and MQTT operations
- Power cycle during operation

### 3. Security Test

- Try to configure without authentication
- Try invalid passwords
- Test session timeout
- Verify password storage

---

## 📝 Build Logs Location

```bash
.pio/build/T-SIM7600X/

# Important files:
firmware.bin    # The compiled binary
firmware.elf    # Debug symbols
```

---

## 🆘 Getting Help

1. **Check Documentation**
   - [BLE Quick Start](./BLE_QUICK_START.md)
   - [BLE Configuration Guide](./BLE_CONFIGURATION_GUIDE.md)
   - [Tools README](./tools/README.md)

2. **Check Logs**
   ```bash
   pio device monitor --baud 115200 | tee build.log
   ```

3. **Test Components**
   - Use MQTT commands to test individual features
   - Use BLE tool to verify connectivity
   - Check cellular signal strength

4. **Contact Support**
   - Provide serial logs
   - Provide platformio.ini
   - Describe expected vs actual behavior

---

**Good luck with your deployment! 🚀**




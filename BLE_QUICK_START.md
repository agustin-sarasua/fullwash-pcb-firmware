# BLE Configuration - Quick Start

## 🚀 Quick Setup (5 minutes)

### Option A: Using Mobile App (Easiest)

**📖 For detailed step-by-step instructions, see [BLE_APP_INSTRUCTIONS.md](./BLE_APP_INSTRUCTIONS.md)**

1. **Download BLE App**
   - Android/iOS: "nRF Connect" from app store

2. **Scan & Connect**
   - Open app → Tap "Scan"
   - Find "FullWash Machine"
   - Tap "Connect"

3. **Find the Service**
   - After connecting, look for service UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
   - Tap to expand it - you'll see 3 characteristics

4. **Authenticate (REQUIRED FIRST!)**
   - Find characteristic named **"Authentication"** (or UUID ending in `...26a8`)
   - **Tap on it** → Tap **"Write"** button
   - Enter: `fullwash2025` (as Text/UTF-8)
   - Tap "Send"
   - **Check Status characteristic** (named "Status" or UUID ending `...84a5f2`) → Read it
   - Should say: "Authenticated - Valid for 120 seconds"
   - ⏱️ You have **2 minutes** to configure before re-authentication is needed

5. **Set Machine Number**
   - Find characteristic with UUID ending in `...be87e` (Machine Number)
   - **Tap on it** → Tap **"Write"** button
   - Enter your number (e.g., `42`)
   - Tap "Send"
   - **Check Status** → Should say "Machine number updated successfully"

6. **Restart Machine**
   - Power cycle the device
   - Configuration is now active!

**💡 Tip:** If you can't find the write buttons, tap on each characteristic first, then look for write options in the menu.

---

### Option B: Using Python Script (Automated)

```bash
# Install tool
pip install bleak

# Scan for machines
python tools/ble_config_tool.py --scan

# Configure (use MAC from scan)
python tools/ble_config_tool.py --configure 42 --address AA:BB:CC:DD:EE:FF

# Restart machine and verify
python tools/ble_config_tool.py --read --address AA:BB:CC:DD:EE:FF
```

---

## 📱 BLE Characteristics

| What | Name (Now Shows Properly!) | UUID (last 4) | Action |
|------|---------------------------|---------------|--------|
| Authenticate | "Authentication - Write master password here" | `...26a8` | Write password |
| Machine Number | "Machine Number - Read/Write machine ID (requires auth)" | `...be87e` | Read/Write number |
| Status | "Status - Read authentication and operation status" | `...84a5f2` | Read status messages |

**Note:** Characteristics now show user-friendly names instead of "Unknown Characteristic"!

---

## 🔐 Default Credentials

- **Device Name**: `FullWash Machine`
- **Password**: `fullwash2025`
- **Machine Number**: `99`

⚠️ **IMPORTANT**: Change default password in production!

---

## ⚡ Common Issues

| Problem | Solution |
|---------|----------|
| Can't find device | Wait 10 seconds after power-on |
| Auth fails | Check password, it's case-sensitive |
| Changes don't work | Did you restart the machine? |
| Connection drops | Normal - device auto-reconnects |

---

## 📖 Full Documentation

- [Complete BLE Configuration Guide](./BLE_CONFIGURATION_GUIDE.md)
- [Python Tool Documentation](./tools/README.md)

---

## 💡 What Gets Configured?

When you set machine number to `42`:
- ✅ MQTT Client ID: `fullwash-machine-42`
- ✅ MQTT Topics: `machines/42/...`
- ✅ Stored permanently in device
- ✅ Survives power cycles

---

## 🔧 MQTT Debug Commands

Connect via MQTT command topic:

```json
{"command": "debug_ble"}
```

Returns current BLE configuration status.

```json
{"command": "set_machine_number", "number": "42"}
```

Changes machine number remotely (restart required).

---

**Need Help?** See [BLE_CONFIGURATION_GUIDE.md](./BLE_CONFIGURATION_GUIDE.md)


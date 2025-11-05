# FullWash Machine - Configuration Tools

This directory contains tools for configuring and managing FullWash machines.

## BLE Configuration Tool

`ble_config_tool.py` - Python script for automated BLE configuration of FullWash machines.

### Installation

1. **Install Python 3.7 or higher**

2. **Install required packages:**
```bash
pip install bleak
```

### Usage

#### Scan for Nearby Machines
```bash
python ble_config_tool.py --scan
```

This will list all BLE devices and highlight FullWash machines.

Example output:
```
Scanning for BLE devices...
============================================================
Found Devices:
============================================================
>>> FullWash Machine              | AA:BB:CC:DD:EE:FF
    Other Device                   | 11:22:33:44:55:66
============================================================

✓ Found 1 FullWash machine(s)
```

#### Read Current Configuration
```bash
python ble_config_tool.py --read --address AA:BB:CC:DD:EE:FF
```

Add custom password if changed from default:
```bash
python ble_config_tool.py --read --address AA:BB:CC:DD:EE:FF --password your_password
```

#### Configure Machine Number
```bash
python ble_config_tool.py --configure 42 --address AA:BB:CC:DD:EE:FF
```

This will:
1. Connect to the device
2. Authenticate with the master password
3. Update the machine number to 42
4. Verify the change
5. Remind you to restart the machine

#### Full Example Workflow
```bash
# 1. Scan for devices
python ble_config_tool.py --scan

# 2. Read current config (use address from scan)
python ble_config_tool.py --read --address AA:BB:CC:DD:EE:FF

# 3. Configure new machine number
python ble_config_tool.py --configure 101 --address AA:BB:CC:DD:EE:FF

# 4. Verify (after restarting the machine)
python ble_config_tool.py --read --address AA:BB:CC:DD:EE:FF
```

### Batch Configuration Script

For configuring multiple machines, create a bash script:

```bash
#!/bin/bash
# configure_fleet.sh - Configure multiple machines

# Array of machine addresses and numbers
declare -A MACHINES=(
    ["AA:BB:CC:DD:EE:01"]=101
    ["AA:BB:CC:DD:EE:02"]=102
    ["AA:BB:CC:DD:EE:03"]=103
)

PASSWORD="fullwash2025"

for MAC in "${!MACHINES[@]}"; do
    NUMBER=${MACHINES[$MAC]}
    echo "Configuring machine $NUMBER at $MAC..."
    python ble_config_tool.py --configure $NUMBER --address $MAC --password $PASSWORD
    echo "---"
done

echo "All machines configured. Please restart each machine."
```

Make it executable:
```bash
chmod +x configure_fleet.sh
./configure_fleet.sh
```

### Troubleshooting

#### "No FullWash machines found"
- Ensure machine is powered on and BLE is initialized (wait 5-10 seconds)
- Move closer to the device (within 10 meters)
- Check that your computer's Bluetooth is enabled

#### "Authentication failed"
- Verify the password is correct
- Password is case-sensitive
- Default is `fullwash2025`

#### "bleak" module not found
```bash
pip install bleak
```

#### Permission denied (Linux)
```bash
sudo python ble_config_tool.py --scan
# Or add your user to bluetooth group:
sudo usermod -a -G bluetooth $USER
```

#### Connection timeout
- Device may already be connected to another client
- Restart the machine
- Wait a few seconds between connection attempts

### Platform-Specific Notes

#### Linux
May require root or bluetooth group membership:
```bash
sudo usermod -a -G bluetooth $USER
# Log out and back in for changes to take effect
```

#### macOS
Should work out of the box with Python 3.7+

#### Windows
Requires Windows 10 version 1809 or later with Bluetooth LE support

### Advanced Usage

#### Custom Password Configuration

If you've changed the master password in the firmware, always specify it:

```bash
python ble_config_tool.py --configure 42 \
    --address AA:BB:CC:DD:EE:FF \
    --password "MySecurePassword123"
```

#### Integration with CI/CD

You can use this tool in automated deployment scripts:

```python
import subprocess
import json

def configure_machine(mac_address, machine_number, password):
    result = subprocess.run([
        'python', 'ble_config_tool.py',
        '--configure', str(machine_number),
        '--address', mac_address,
        '--password', password
    ], capture_output=True, text=True)
    
    return result.returncode == 0
```

## Future Tools

Planned additions:
- Web-based configuration interface
- Mobile app for iOS/Android
- Bulk firmware update tool
- Network diagnostics tool
- Configuration backup/restore

## Support

For issues or questions:
1. Check the main [BLE Configuration Guide](../BLE_CONFIGURATION_GUIDE.md)
2. Review error messages in the tool output
3. Check device serial logs (115200 baud)
4. Contact development team

---

**Last Updated**: November 2025




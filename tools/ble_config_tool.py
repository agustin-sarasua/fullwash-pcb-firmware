#!/usr/bin/env python3
"""
FullWash Machine BLE Configuration Tool

This script allows you to configure FullWash machines via Bluetooth Low Energy.
It can be used for automated setup of multiple machines or troubleshooting.

Requirements:
    pip install bleak

Usage:
    python ble_config_tool.py --scan                    # Scan for nearby devices
    python ble_config_tool.py --configure 42            # Set machine number to 42
    python ble_config_tool.py --read                    # Read current configuration
    python ble_config_tool.py --password mypass         # Use custom password
"""

import asyncio
import argparse
import sys
from bleak import BleakScanner, BleakClient

# BLE Service and Characteristic UUIDs
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
AUTH_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
MACHINE_NUM_CHAR_UUID = "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"
STATUS_CHAR_UUID = "d8de624e-140f-4a22-8594-e2216b84a5f2"

# Default values
DEFAULT_DEVICE_NAME = "FullWash Machine"
DEFAULT_PASSWORD = "fullwash2025"


class FullWashBLEConfig:
    """BLE Configuration Manager for FullWash Machines"""
    
    def __init__(self, device_address, password=DEFAULT_PASSWORD):
        self.device_address = device_address
        self.password = password
        self.client = None
        
    async def connect(self):
        """Connect to the BLE device"""
        print(f"Connecting to {self.device_address}...")
        self.client = BleakClient(self.device_address)
        await self.client.connect()
        print("Connected successfully!")
        
    async def disconnect(self):
        """Disconnect from the BLE device"""
        if self.client and self.client.is_connected:
            await self.client.disconnect()
            print("Disconnected.")
            
    async def authenticate(self):
        """Authenticate with the device"""
        print("Authenticating...")
        await self.client.write_gatt_char(AUTH_CHAR_UUID, self.password.encode())
        
        # Wait a bit for authentication to process
        await asyncio.sleep(0.5)
        
        # Read status to verify
        status = await self.client.read_gatt_char(STATUS_CHAR_UUID)
        status_str = status.decode('utf-8')
        print(f"Status: {status_str}")
        
        if "Authenticated" in status_str:
            print("✓ Authentication successful!")
            return True
        else:
            print("✗ Authentication failed!")
            return False
            
    async def read_machine_number(self):
        """Read the current machine number"""
        machine_num = await self.client.read_gatt_char(MACHINE_NUM_CHAR_UUID)
        return machine_num.decode('utf-8')
        
    async def write_machine_number(self, number):
        """Write a new machine number"""
        print(f"Setting machine number to: {number}")
        await self.client.write_gatt_char(MACHINE_NUM_CHAR_UUID, str(number).encode())
        
        # Wait for write to complete
        await asyncio.sleep(0.5)
        
        # Read status to verify
        status = await self.client.read_gatt_char(STATUS_CHAR_UUID)
        status_str = status.decode('utf-8')
        print(f"Status: {status_str}")
        
        if "success" in status_str.lower():
            print("✓ Machine number updated successfully!")
            print("⚠️  RESTART THE MACHINE for changes to take effect")
            return True
        else:
            print("✗ Failed to update machine number")
            return False
            
    async def get_status(self):
        """Get current status"""
        status = await self.client.read_gatt_char(STATUS_CHAR_UUID)
        return status.decode('utf-8')


async def scan_devices():
    """Scan for nearby BLE devices"""
    print("Scanning for BLE devices...")
    devices = await BleakScanner.discover(timeout=5.0)
    
    fullwash_devices = []
    
    print("\n" + "="*60)
    print("Found Devices:")
    print("="*60)
    
    for device in devices:
        is_fullwash = DEFAULT_DEVICE_NAME in (device.name or "")
        if is_fullwash:
            fullwash_devices.append(device)
            
        # Print all devices, but highlight FullWash machines
        prefix = ">>> " if is_fullwash else "    "
        name = device.name or "(Unknown)"
        print(f"{prefix}{name:30} | {device.address}")
    
    print("="*60)
    
    if fullwash_devices:
        print(f"\n✓ Found {len(fullwash_devices)} FullWash machine(s)")
        return fullwash_devices
    else:
        print("\n✗ No FullWash machines found")
        print("\nTroubleshooting:")
        print("  1. Ensure the machine is powered on")
        print("  2. Wait 5-10 seconds after power-on")
        print("  3. Move closer to the device")
        print("  4. Check that Bluetooth is enabled")
        return []


async def read_configuration(device_address, password):
    """Read current configuration from device"""
    config = FullWashBLEConfig(device_address, password)
    
    try:
        await config.connect()
        
        # Try to authenticate
        if await config.authenticate():
            # Read machine number
            machine_num = await config.read_machine_number()
            print(f"\n📋 Current Configuration:")
            print(f"   Machine Number: {machine_num}")
        
    except Exception as e:
        print(f"Error: {e}")
    finally:
        await config.disconnect()


async def configure_machine(device_address, machine_number, password):
    """Configure machine with new settings"""
    config = FullWashBLEConfig(device_address, password)
    
    try:
        await config.connect()
        
        # Authenticate
        if not await config.authenticate():
            print("Authentication failed. Cannot configure machine.")
            return False
            
        # Read current configuration
        current_num = await config.read_machine_number()
        print(f"Current machine number: {current_num}")
        
        # Write new configuration
        if await config.write_machine_number(machine_number):
            # Verify by reading back
            new_num = await config.read_machine_number()
            print(f"Verified new machine number: {new_num}")
            return True
        else:
            return False
            
    except Exception as e:
        print(f"Error: {e}")
        return False
    finally:
        await config.disconnect()


def main():
    parser = argparse.ArgumentParser(
        description="FullWash Machine BLE Configuration Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --scan
  %(prog)s --configure 42 --address AA:BB:CC:DD:EE:FF
  %(prog)s --read --address AA:BB:CC:DD:EE:FF --password mypassword
        """
    )
    
    parser.add_argument('--scan', action='store_true',
                        help='Scan for nearby FullWash machines')
    parser.add_argument('--read', action='store_true',
                        help='Read current configuration')
    parser.add_argument('--configure', metavar='NUMBER',
                        help='Configure machine with specified number')
    parser.add_argument('--address', metavar='MAC',
                        help='BLE device MAC address (required for --read and --configure)')
    parser.add_argument('--password', default=DEFAULT_PASSWORD,
                        help=f'Master password (default: {DEFAULT_PASSWORD})')
    
    args = parser.parse_args()
    
    # Validate arguments
    if args.scan:
        asyncio.run(scan_devices())
        
    elif args.read:
        if not args.address:
            print("Error: --address is required for --read")
            parser.print_help()
            sys.exit(1)
        asyncio.run(read_configuration(args.address, args.password))
        
    elif args.configure:
        if not args.address:
            print("Error: --address is required for --configure")
            parser.print_help()
            sys.exit(1)
        asyncio.run(configure_machine(args.address, args.configure, args.password))
        
    else:
        parser.print_help()


if __name__ == "__main__":
    main()














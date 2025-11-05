# How to Configure FullWash Machine via BLE App

## 📱 Step-by-Step Instructions for nRF Connect App

### Part 1: Connect to the Device

1. **Open nRF Connect App**
   - Launch the app on your phone
   - Tap "Scan" button (bottom of screen)

2. **Find Your Device**
   - Look for device named: **"FullWash Machine"**
   - You'll see it listed with a MAC address (e.g., AA:BB:CC:DD:EE:FF)
   - Tap on "FullWash Machine" to connect

3. **Connect**
   - Wait for connection (usually 2-3 seconds)
   - You should see "Connected" status

4. **View Services**
   - After connecting, you'll see a list of services
   - Look for service with UUID starting with: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
   - Tap on this service to expand it

---

### Part 2: Authenticate (REQUIRED FIRST STEP)

After expanding the service, you'll see **3 characteristics**. Look for the one with UUID ending in `...26a8` - this is the **Authentication** characteristic.

**To authenticate:**

1. **Tap on the Authentication characteristic** (UUID ending in `...26a8`)
   - You should see it shows "Write" permission
   - The current value might show "Enter password" or similar

2. **Tap the "Write" button** (or pencil icon, depending on app version)
   - You'll see an input field appear

3. **Enter the password: `fullwash2025`**
   - Type it exactly (case-sensitive)
   - Make sure there are no extra spaces

4. **Select write type:**
   - Choose "Text" or "UTF-8 String" (not hex)
   - Tap "Send" or "Write"

5. **Check Status:**
   - After writing, tap on the **Status characteristic** (UUID ending in `...84a5f2`)
   - Tap "Read" button
   - You should see: **"Authenticated - Valid for 120 seconds"**
   - ⏱️ You have **2 minutes** to configure the machine before re-authentication is needed
   - If you see "Authentication failed", check the password and try again

---

### Part 3: Read Current Machine Number

1. **Find the Machine Number characteristic** (UUID ending in `...be87e`)
   - This characteristic has both Read and Write permissions

2. **Tap on it**, then tap **"Read"** button
   - You'll see the current machine number (default is "99")

---

### Part 4: Set New Machine Number

**⚠️ IMPORTANT: You MUST authenticate first (Part 2) before you can write!**

1. **Make sure you're authenticated:**
   - Check Status characteristic shows "Authenticated - Access granted"
   - You have 30 seconds after authentication

2. **Tap on Machine Number characteristic** (UUID ending in `...be87e`)

3. **Tap "Write" button**

4. **Enter your new machine number:**
   - Type the number (e.g., "42", "101", etc.)
   - Use numbers only (no letters or special characters)
   - Maximum 10 digits

5. **Select write type:**
   - Choose "Text" or "UTF-8 String"
   - Tap "Send" or "Write"

6. **Verify the change:**
   - Tap "Read" button on the same characteristic
   - You should see your new number

7. **Check Status:**
   - Read the Status characteristic again
   - Should show: **"Machine number updated successfully"**
   - ⚠️ **RESTART THE MACHINE** for changes to take effect!

---

## 🔍 Visual Guide: What You Should See

### Service View (After Connecting)
```
📱 FullWash Machine
   └─ Service: 4fafc201-1fb5-459e-8fcc-c5c9c331914b
      ├─ Characteristic: beb5483e-...26a8  [WRITE] ← Authentication
      ├─ Characteristic: 1c95d5e3-...be87e [READ/WRITE/NOTIFY] ← Machine Number
      └─ Characteristic: d8de624e-...84a5f2 [READ/NOTIFY] ← Status
```

### Characteristic Names (Now with User-Friendly Names!)
The characteristics now show proper names instead of "Unknown Characteristic":
- **Authentication** (`...26a8`) - Shows as "Authentication - Write master password here"
- **Machine Number** (`...be87e`) - Shows as "Machine Number - Read/Write machine ID (requires auth)"
- **Status** (`...84a5f2`) - Shows as "Status - Read authentication and operation status"

---

## 🎯 Quick Reference: The 3 Characteristics

| Characteristic | UUID (last 4) | What to Do |
|----------------|---------------|------------|
| **Authentication** | `...26a8` | **WRITE** password: `fullwash2025` |
| **Machine Number** | `...be87e` | **READ** current number, **WRITE** new number (after auth) |
| **Status** | `...84a5f2` | **READ** to see messages (auth status, errors, etc.) |

---

## ❓ Troubleshooting

### "I don't see any characteristics"
- Make sure you tapped on the service to expand it
- Some apps require double-tap
- Scroll down if you don't see all characteristics

### "I can't write to Machine Number"
- **Did you authenticate first?** You MUST write to Authentication characteristic first
- Check Status characteristic - should say "Authenticated - Valid for X seconds"
- If it says "Not authenticated" or "Authentication expired", authenticate again
- **Note:** Authentication lasts for 2 minutes (120 seconds) - if it expired, just re-authenticate

### "Write button is grayed out / not available"
- Make sure you're connected (not just scanning)
- Try disconnecting and reconnecting
- Some apps require you to tap the characteristic first, then tap "Write" from a menu

### "Authentication failed"
- Check password is exactly: `fullwash2025` (case-sensitive)
- Make sure you selected "Text" or "UTF-8" write type (not hex)
- Check for extra spaces before/after password

### "Changes don't work"
- **Did you restart the machine?** Configuration changes require a restart
- Check serial monitor logs to verify the change was saved

### "I see the characteristics but they're empty"
- This is normal - tap "Read" button to read the current values
- Some apps don't auto-read on connect

---

## 📱 Alternative Apps (if nRF Connect doesn't work)

### Android Alternatives:
1. **BLE Scanner** - Similar interface
2. **Bluetooth LE Explorer** - Another option

### iOS Alternatives:
1. **LightBlue** - Popular iOS BLE app
2. **nRF Connect** - Available on iOS too

### General Steps (All Apps):
1. Scan → Connect to "FullWash Machine"
2. Find service with UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
3. Find characteristic ending in `...26a8` → Write password
4. Find characteristic ending in `...be87e` → Write machine number
5. Check characteristic ending in `...84a5f2` → Read status

---

## 🔐 Security Notes

- Password is case-sensitive: `fullwash2025`
- Authentication expires after **2 minutes (120 seconds)** of inactivity
- You must re-authenticate after each disconnect/reconnect
- The status message will show how many seconds remain: "Authenticated - Valid for X seconds"
- Failed authentication attempts are logged

---

## ✅ Success Checklist

After following these steps, you should:
- [ ] Connected to "FullWash Machine"
- [ ] Authenticated successfully (Status shows "Authenticated")
- [ ] Read current machine number
- [ ] Written new machine number
- [ ] Status shows "Machine number updated successfully"
- [ ] **Restarted the machine**
- [ ] Verified new number is active (check serial monitor or read again)

---

## 📞 Still Having Issues?

1. **Check Serial Monitor:**
   - Connect USB to computer
   - Open serial monitor at 115200 baud
   - Look for BLE connection messages
   - Check for authentication logs

2. **Try Python Tool:**
   ```bash
   python tools/ble_config_tool.py --scan
   python tools/ble_config_tool.py --read --address <MAC>
   ```

3. **Verify Device:**
   - Make sure device is powered on
   - Wait 10 seconds after power-on for BLE to initialize
   - Check device name appears as "FullWash Machine"

---

**Need more help?** Check [BLE_CONFIGURATION_GUIDE.md](./BLE_CONFIGURATION_GUIDE.md)


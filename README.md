# Configurable parameters

SIM_PIN
APN
MACHINE_ID


TOKEN_TIME
USER_INACTIVE_TIMEOUT

COIN_PROCESS_COOLDOWN
COIN_EDGE_WINDOW


 1. Configuration Manager with WiFi Portal:
    - The ESP32 creates an access point (AP) with a password when in setup mode
    - Users can connect to this AP and visit 192.168.4.1 to access the configuration page
    - The configuration page allows setting SIM card PIN, network APN, MACHINE_ID, TOKEN_TIME, and
  USER_INACTIVE_TIMEOUT
    - All settings are stored in non-volatile memory using the ESP32's Preferences library
  2. Two Ways to Enter Setup Mode:
    - By holding BUTTON1 at boot time (immediately enters setup mode)
    - By holding BUTTON1 for 5 seconds during normal operation
  3. User Interface for Setup:
    - The LCD display shows the WiFi setup information
    - The web portal is user-friendly with clear labels and validation
    - Configuration changes can be saved without rebooting or with an automatic reboot
  4. Persistence and Integration:
    - All settings are stored in non-volatile memory and survive power cycles
    - The system loads settings automatically at boot
    - MQTT topics are updated when MACHINE_ID changes
    - Network credentials (APN, SIM PIN) are applied to the cellular connection

  To use the configuration:
  1. Press and hold BUTTON1 while powering on the device (or hold for 5 seconds during operation)
  2. Connect to the "FullWash-Setup" WiFi network with password "fullwash123"
  3. Open a browser and navigate to 192.168.4.1
  4. Configure the parameters and save
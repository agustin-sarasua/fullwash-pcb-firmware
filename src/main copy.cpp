// /*
//  * ESP32 Fullwash Controller with TinyGSM
//  * 
//  * Features:
//  * - Connects to the internet using SIM7600G cellular module
//  * - Controls 7 relays via TCA9535 I/O Expander
//  * - Handles 6 buttons for user input
//  * - Interfaces with a coin accepter
//  * 
//  * Pin Mapping (from schematic):
//  * - ESP32 to I2C: IO18 (SCL), IO19 (SDA), IO23 (INT)
//  * - ESP32 to SIM7600G: IO26 (TX), IO27 (RX), IO25 (FLIGHTMODE), IO32 (DTR), IO33 (RI)
//  * - ESP32 to LCD: IO22 (SCL_LCD), IO21 (SDA_LCD)
//  * - ESP32 to Micro SD: IO13 (DAT3), IO15 (CMD), IO14 (CLK), IO2 (DAT0)
//  */

// #define TINY_GSM_MODEM_SIM7600
// #define TINY_GSM_RX_BUFFER 1024

// #include <Arduino.h>
// #include <Wire.h>
// #include <TinyGsmClient.h>
// #include <ArduinoHttpClient.h>
// #include <ArduinoJson.h>

// // ESP32 PIN Definitions
// #define ESP32_SDA       19
// #define ESP32_SCL       18
// #define ESP32_INT       23
// #define GSM_TX_PIN      26  // ESP32 TX -> SIM7600G RX
// #define GSM_RX_PIN      27  // ESP32 RX <- SIM7600G TX
// #define GSM_FLIGHT_PIN  25
// #define GSM_DTR_PIN     32
// #define GSM_RI_PIN      33
// #define LCD_SDA_PIN     21
// #define LCD_SCL_PIN     22

// // TCA9535 I/O Expander Configuration
// #define IO_EXPANDER_ADDR   0x20
// #define INPUT_PORT0        0x00
// #define INPUT_PORT1        0x01
// #define OUTPUT_PORT0       0x02
// #define OUTPUT_PORT1       0x03
// #define POLARITY_PORT0     0x04
// #define POLARITY_PORT1     0x05
// #define CONFIG_PORT0       0x06
// #define CONFIG_PORT1       0x07

// // I/O Expander pin definitions (using port and bit encoding)
// // Port 0 pins (0x0n)
// #define BT6_PIN         0x00  // P00
// #define BT5_PIN         0x01  // P01
// #define BT4_PIN         0x02  // P02
// #define BT3_PIN         0x03  // P03
// #define BT2_PIN         0x04  // P04
// #define BT1_PIN         0x05  // P05
// #define COIN_SIG_PIN    0x06  // P06
// #define COIN_CNT_PIN    0x07  // P07

// // Port 1 pins (0x1n)
// #define RELAY1_PIN      0x10  // P10 - Clear water
// #define RELAY2_PIN      0x11  // P11 - Foam
// #define RELAY3_PIN      0x12  // P12 - Vacuum
// #define RELAY4_PIN      0x13  // P13 - Handwashing
// #define RELAY5_PIN      0x14  // P14 - Inflatable
// #define RELAY6_PIN      0x15  // P15 - Disinfect
// #define RELAY7_PIN      0x16  // P16 - Lighting

// // Maps for buttons and relays with names
// const uint8_t buttonPins[6] = {BT1_PIN, BT2_PIN, BT3_PIN, BT4_PIN, BT5_PIN, BT6_PIN};
// const char* buttonNames[6] = {"Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6"};

// const uint8_t relayPins[7] = {RELAY1_PIN, RELAY2_PIN, RELAY3_PIN, RELAY4_PIN, RELAY5_PIN, RELAY6_PIN, RELAY7_PIN};
// const char* relayNames[7] = {"Clear Water", "Foam", "Vacuum", "Handwashing", "Inflatable", "Disinfect", "Lighting"};

// // GSM Configuration
// #define SerialAT          Serial1
// #define GSM_BAUD_RATE     115200
// const char apn[]          = "internet";  // Your carrier's APN
// const char gprsUser[]     = "";          // GPRS username if needed
// const char gprsPass[]     = "";          // GPRS password if needed
// const char simPIN[]       = "3846";          // SIM card PIN if needed (empty if not PIN-protected)

// // Server settings for test API
// const char server[]       = "worldtimeapi.org";
// const int port            = 80;
// const char resource[]     = "/api/ip";

// // Global instances
// TinyGsm modem(SerialAT);
// TinyGsmClient gsmClient(modem);
// HttpClient http(gsmClient, server, port);

// // Function prototypes
// void setupIOExpander();
// void setupGSM();
// bool connectGSM();
// bool fetchTimeFromInternet();
// void writeIOExpanderReg(uint8_t reg, uint8_t data);
// uint8_t readIOExpanderReg(uint8_t reg);
// void setRelay(uint8_t relay, bool state);
// bool getButtonState(uint8_t button);
// void displayModemInfo();
// void sequentialRelayDemo();

// bool getRelayState(uint8_t relay) {
//   // Extract port and bit
//   uint8_t port = (relay >> 4) & 0x0F;
//   uint8_t bit = relay & 0x07;
  
//   // Ensure this is a valid relay (only on Port 1)
//   if (port != 1 || bit > 6) {
//     Serial.println("Invalid relay pin");
//     return false;
//   }
  
//   // Read current port state
//   uint8_t portState = readIOExpanderReg(OUTPUT_PORT1);
  
//   // Return the state of the specific bit
//   return (portState & (1 << bit)) != 0;
// }


// bool digitalReadFromExpander(uint8_t pin) {
//   // Extract port and bit
//   uint8_t port = (pin >> 4) & 0x0F;
//   uint8_t bit = pin & 0x07;
  
//   // Read from the appropriate port
//   uint8_t portState;
//   if (port == 0) {
//     portState = readIOExpanderReg(INPUT_PORT0);
//   } else if (port == 1) {
//     portState = readIOExpanderReg(INPUT_PORT1);
//   } else {
//     Serial.println("Invalid port");
//     return false;
//   }
  
//   // Return the state of the specific bit
//   return (portState & (1 << bit)) != 0;
// }

// void setup() {
//   // Initialize serial monitor
//   Serial.begin(115200);
//   delay(1000);
  
//   Serial.println("\n=== ESP32 Fullwash Controller ===");
  
//   // Initialize I2C for I/O expander
//   Wire.begin(ESP32_SDA, ESP32_SCL);
  
//   // Set up IO expander
//   setupIOExpander();
  
//   // Setup GSM module
//   setupGSM();
  
//   // Try to connect to internet
//   if (connectGSM()) {
//     Serial.println("Successfully connected to mobile network");
    
//     // Fetch data as a demonstration
//     if (fetchTimeFromInternet()) {
//       Serial.println("Successfully fetched data from internet");
      
//       // Turn on a relay to indicate success
//       setRelay(RELAY1_PIN, true);
//       Serial.println("Turned on Relay 1 (Clear Water) to indicate success");
//       delay(2000);
      
//       // Run a demo sequence with the relays
//       sequentialRelayDemo();
//     } else {
//       Serial.println("Failed to fetch data from internet");
//     }
//   } else {
//     Serial.println("Failed to connect to mobile network");
//   }
// }

// void loop() {
//   // Poll buttons and toggle corresponding relays
//   for (int i = 0; i < 6; i++) {
//     if (getButtonState(buttonPins[i])) {
//       Serial.print(buttonNames[i]);
//       Serial.println(" pressed");
      
//       // If we have a corresponding relay (first 6 buttons), toggle it
//       if (i < 7) {
//         bool currentState = getRelayState(relayPins[i]);
//         setRelay(relayPins[i], !currentState);
//         Serial.print("Toggled ");
//         Serial.print(relayNames[i]);
//         Serial.println(currentState ? " OFF" : " ON");
//       }
      
//       // Simple debounce
//       delay(200);
//     }
//   }
  
//   // Check coin accepter (simple demonstration)
//   if (digitalReadFromExpander(COIN_SIG_PIN) == LOW) {
//     Serial.println("Coin detected!");
//     delay(100);
//   }
  
//   // Main loop delay
//   delay(50);
// }

// void setupIOExpander() {
//   Serial.println("Setting up I/O Expander...");
  
//   // Configure Port 0 (buttons and coin accepter) as inputs
//   writeIOExpanderReg(CONFIG_PORT0, 0xFF);  // 1 = input
  
//   // Configure Port 1 (relays) as outputs
//   writeIOExpanderReg(CONFIG_PORT1, 0x80);  // 0 = output, bit 7 left as input
  
//   // Initialize all relays to OFF
//   writeIOExpanderReg(OUTPUT_PORT1, 0x00);
  
//   Serial.println("I/O Expander initialized");
// }

// void setupGSM() {
//   Serial.println("Setting up GSM module...");
  
//   // Initialize serial communication with modem
//   SerialAT.begin(GSM_BAUD_RATE, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  
//   // Configure control pins
//   pinMode(GSM_FLIGHT_PIN, OUTPUT);
//   pinMode(GSM_DTR_PIN, OUTPUT);
//   pinMode(GSM_RI_PIN, INPUT);
  
//   // Set initial pin states
//   digitalWrite(GSM_FLIGHT_PIN, HIGH);  // Disable flight mode
//   digitalWrite(GSM_DTR_PIN, LOW);      // DTR active
  
//   // Give the modem time to initialize
//   delay(3000);
  
//   // Restart the modem
//   Serial.println("Restarting modem...");
//   modem.restart();
  
//   // Display modem information
//   displayModemInfo();
// }

// bool connectGSM() {
//   Serial.println("Connecting to cellular network...");
  
//   // Unlock SIM card with PIN if needed
//   if (strlen(simPIN) > 0) {
//     Serial.println("Unlocking SIM card with PIN...");
//     if (!modem.simUnlock(simPIN)) {
//       Serial.println("Failed to unlock SIM card with provided PIN");
//       return false;
//     }
//     Serial.println("SIM card unlocked successfully");
//   } else {
//     Serial.println("No SIM PIN provided - assuming SIM is not PIN-protected");
//   }
  
//   // Wait for network registration
//   Serial.println("Waiting for network registration...");
//   if (!modem.waitForNetwork(60000L)) {
//     Serial.println("Network registration failed");
//     return false;
//   }
  
//   Serial.println("Network registered");
  
//   // Connect to APN for internet
//   Serial.print("Connecting to APN: ");
//   Serial.println(apn);
  
//   if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
//     Serial.println("APN connection failed");
//     return false;
//   }
  
//   Serial.println("GPRS connected");
  
//   return true;
// }

// bool fetchTimeFromInternet() {
//   Serial.print("Connecting to ");
//   Serial.println(server);
  
//   // Make HTTP GET request
//   Serial.println("Performing HTTP GET request...");
//   http.connectionKeepAlive();  // Important for TinyGSM
  
//   int err = http.get(resource);
//   if (err != 0) {
//     Serial.print("HTTP GET error: ");
//     Serial.println(err);
//     return false;
//   }
  
//   // Read response status and headers
//   int status = http.responseStatusCode();
//   Serial.print("Response status code: ");
//   Serial.println(status);
  
//   if (status != 200) {
//     http.stop();
//     return false;
//   }
  
//   // Parse response data
//   DynamicJsonDocument doc(2048);
//   DeserializationError error = deserializeJson(doc, http.responseBody());
  
//   if (error) {
//     Serial.print("deserializeJson() failed: ");
//     Serial.println(error.f_str());
//     http.stop();
//     return false;
//   }
  
//   // Extract and display the data
//   Serial.println("===== Current time from Internet =====");
//   Serial.print("DateTime: ");
//   if (doc.containsKey("datetime")) {
//     Serial.println(doc["datetime"].as<String>());
//   }
  
//   Serial.print("Timezone: ");
//   if (doc.containsKey("timezone")) {
//     Serial.println(doc["timezone"].as<String>());
//   }
  
//   http.stop();
//   return true;
// }

// void writeIOExpanderReg(uint8_t reg, uint8_t data) {
//   Wire.beginTransmission(IO_EXPANDER_ADDR);
//   Wire.write(reg);
//   Wire.write(data);
//   Wire.endTransmission();
// }

// uint8_t readIOExpanderReg(uint8_t reg) {
//   Wire.beginTransmission(IO_EXPANDER_ADDR);
//   Wire.write(reg);
//   Wire.endTransmission();
  
//   Wire.requestFrom(IO_EXPANDER_ADDR, 1);
//   if (Wire.available()) {
//     return Wire.read();
//   }
//   return 0;
// }

// void setRelay(uint8_t relay, bool state) {
//   // Extract port and bit
//   uint8_t port = (relay >> 4) & 0x0F;
//   uint8_t bit = relay & 0x07;
  
//   // Ensure this is a valid relay (only on Port 1)
//   if (port != 1 || bit > 6) {
//     Serial.println("Invalid relay pin");
//     return;
//   }
  
//   // Read current port state
//   uint8_t portState = readIOExpanderReg(OUTPUT_PORT1);
  
//   // Modify the bit
//   if (state) {
//     portState |= (1 << bit);  // Set bit
//   } else {
//     portState &= ~(1 << bit); // Clear bit
//   }
  
//   // Write back the port state
//   writeIOExpanderReg(OUTPUT_PORT1, portState);
// }



// bool getButtonState(uint8_t button) {
//   // Extract port and bit
//   uint8_t port = (button >> 4) & 0x0F;
//   uint8_t bit = button & 0x07;
  
//   // Ensure this is a valid button (only on Port 0)
//   if (port != 0 || bit > 7) {
//     Serial.println("Invalid button pin");
//     return false;
//   }
  
//   // Read current port state
//   uint8_t portState = readIOExpanderReg(INPUT_PORT0);
  
//   // Buttons are active low, so we invert the logic
//   return ((portState & (1 << bit)) == 0);
// }


// void displayModemInfo() {
//   String modemInfo = modem.getModemInfo();
//   Serial.print("Modem Info: ");
//   Serial.println(modemInfo);
  
//   String imei = modem.getIMEI();
//   Serial.print("IMEI: ");
//   Serial.println(imei);
  
//   String simCCID = modem.getSimCCID();
//   Serial.print("SIM CCID: ");
//   Serial.println(simCCID);
  
//   String cop = modem.getOperator();
//   Serial.print("Operator: ");
//   Serial.println(cop);
  
//   int csq = modem.getSignalQuality();
//   Serial.print("Signal Quality: ");
//   Serial.print(csq);
//   Serial.println(" dBm");
// }

// void sequentialRelayDemo() {
//   Serial.println("Running relay demo sequence...");
  
//   // Turn off all relays first
//   for (int i = 0; i < 7; i++) {
//     setRelay(relayPins[i], false);
//   }
//   delay(1000);
  
//   // Sequence through each relay
//   for (int i = 0; i < 7; i++) {
//     Serial.print("Activating ");
//     Serial.println(relayNames[i]);
    
//     setRelay(relayPins[i], true);
//     delay(1000);
//     setRelay(relayPins[i], false);
//     delay(500);
//   }
  
//   // Flash all relays a few times
//   for (int j = 0; j < 3; j++) {
//     // All on
//     for (int i = 0; i < 7; i++) {
//       setRelay(relayPins[i], true);
//     }
//     delay(500);
    
//     // All off
//     for (int i = 0; i < 7; i++) {
//       setRelay(relayPins[i], false);
//     }
//     delay(500);
//   }
  
//   Serial.println("Relay demo completed");
// }
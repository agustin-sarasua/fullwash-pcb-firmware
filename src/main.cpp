#include <Wire.h>
#include <Arduino.h>
#include <PubSubClient.h>
#include <ArduinoHttpClient.h>

#include "utilities.h"

#include "certs/AmazonRootCA.h"
#include "certs/AWSClientCertificate.h"
#include "certs/AWSClientPrivateKey.h"

// Server details
const char backendServer[]   = "api-sbx.fullwash.uy";
const int  backendPort       = 443;

const char server[]   = "a3foc0mc6v7ap0-ats.iot.us-east-1.amazonaws.com";
const char resource[] = "/topics/fullwash-machine-001%2Ftest";
const char* AWS_CLIENT_ID = "fullwash-machine-001";
const int  port       = 8443;

// GSM connection settings
const char apn[] = "internet"; // Replace with your carrier's APN if needed
const char gprsUser[] = "";
const char gprsPass[] = "";
const char pin[] = "3846";

// #define DUMP_AT_COMMANDS
#ifdef DUMP_AT_COMMANDS
#include <StreamDebugger.h>
StreamDebugger debugger(SerialAT, SerialMon);
TinyGsm        modem(debugger);
#else
TinyGsm        modem(SerialAT);
#endif

TinyGsmClientSecure client(modem, 0);
HttpClient          http(client, server, port);

// Button states
bool buttonState = false;
bool lastButtonState = false;

void clearModemBuffer() {
  delay(100);
  while (SerialAT.available()) {
    SerialAT.read();
  }
}

// Add this helper function for AT command testing
bool testModemAT() {
  SerialMon.println("Testing direct AT communication with modem...");
  
  clearModemBuffer();

  // Flush any pending data
  while (SerialAT.available()) {
    SerialAT.read();
  }
  
  // Send AT command
  SerialMon.println("Sending: AT");
  SerialAT.println("AT");
  
  // Wait for response
  unsigned long start = millis();
  String response = "";
  
  while (millis() - start < 3000) {
    if (SerialAT.available()) {
      char c = SerialAT.read();
      response += c;
    }
  }
  
  SerialMon.println("Response: " + response);
  
  if (response.indexOf("OK") != -1) {
    SerialMon.println("Modem responded to AT command successfully!");
    return true;
  } else {
    SerialMon.println("Modem failed to respond to AT command properly.");
    return false;
  }
}

void powerOnModem() {
  SerialMon.println("Powering on SIM7600G module...");
  
  // Configure control pins
  pinMode(MODEM_PWRKEY, OUTPUT);
  pinMode(MODEM_DTR, OUTPUT);
  pinMode(MODEM_FLIGHT, OUTPUT);
  
  // Set DTR (Data Terminal Ready) active
  digitalWrite(MODEM_DTR, LOW);
  
  // Make sure flight mode is disabled
  digitalWrite(MODEM_FLIGHT, HIGH); 
  
  // SIM7600G power on sequence (based on datasheet)
  digitalWrite(MODEM_PWRKEY, LOW);  // Ensure PWRKEY starts LOW
  delay(1000);
  
  digitalWrite(MODEM_PWRKEY, HIGH); // Pull PWRKEY HIGH
  delay(2000);                      // Hold for >1 second
  
  digitalWrite(MODEM_PWRKEY, LOW);  // Release PWRKEY
  
  SerialMon.println("Waiting for modem to initialize...");
  delay(10000);  // Wait longer for the modem to boot
  
  clearModemBuffer();

  // Test AT command communication
  bool atSuccess = testModemAT();
  
  if (!atSuccess) {
    SerialMon.println("Trying alternative power on sequence...");
    
    // Alternative power on sequence sometimes needed for SIM7600
    digitalWrite(MODEM_PWRKEY, HIGH);
    delay(3000);
    digitalWrite(MODEM_PWRKEY, LOW);
    delay(5000);
    
    clearModemBuffer();

    // Test AT command again
    atSuccess = testModemAT();
    
    if (!atSuccess) {
      SerialMon.println("Still unable to communicate with modem!");
      SerialMon.println("Possible issues:");
      SerialMon.println("1. Check power supply to modem");
      SerialMon.println("2. Check UART connections (TX/RX)");
      SerialMon.println("3. Modem might not be powered properly");
    }
  }
}

bool initModemAndConnectNetwork() {
  SerialMon.println("Initializing modem...");
  
  // Try to initialize the modem
  if (!modem.init()) {
    SerialMon.println("Failed to initialize modem!");
    
    // Check if we can talk to the modem directly
    bool basicComm = testModemAT();
    
    if (basicComm) {
      SerialMon.println("Modem responds to AT commands but TinyGSM init failed.");
      SerialMon.println("This could be a TinyGSM library compatibility issue with SIM7600G.");
      
      // Try an alternative approach - direct AT commands
      SerialMon.println("Trying simplified initialization with direct AT commands...");
      
      // Send some basic config AT commands
      SerialAT.println("AT+CFUN=1");  // Set full functionality
      delay(1000);
      
      SerialAT.println("AT+CREG?");   // Check network registration
      delay(1000);
      
      // Try to connect to GPRS directly
      SerialMon.print("Connecting to ");
      SerialMon.print(apn);
      SerialMon.println(" using direct AT commands...");
      
      // Set the APN
      SerialAT.print("AT+CGDCONT=1,\"IP\",\"");
      SerialAT.print(apn);
      SerialAT.println("\"");
      delay(1000);
      
      return false;  // Still return false to indicate TinyGSM init failed
    } else {
      SerialMon.println("Basic AT command communication failed.");
      SerialMon.println("Possible hardware issue - check wiring and power.");
      return false;
    }
  }
  
  // Get modem info
  String modemInfo = modem.getModemInfo();
  SerialMon.print("Modem Info: ");
  SerialMon.println(modemInfo);
  
  // Set network mode to automatic (2G/3G/4G)
  String networkMode;
  networkMode = modem.setNetworkMode(2);
  SerialMon.print("Network mode set: ");
  SerialMon.println(networkMode);

  if (GSM_PIN && modem.getSimStatus() != 3) {
      modem.simUnlock(GSM_PIN);
  }
  
  // Wait for network connection
  SerialMon.print("Waiting for network...");
  if (!modem.waitForNetwork(60000L)) {
    SerialMon.println(" fail");
    return false;
  }
  SerialMon.println(" success");
  
  if (modem.isNetworkConnected()) {
    SerialMon.println("Network connected");
  } else {
    SerialMon.println("Network connection failed");
    return false;
  }
  
  // Connect to GPRS
  SerialMon.print("Connecting to ");
  SerialMon.print(apn);
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    SerialMon.println(" fail");
    return false;
  }
  SerialMon.println(" success");
  
  if (modem.isGprsConnected()) {
    SerialMon.println("GPRS connected");
    
    // Get IP address
    String ip = modem.localIP().toString();
    SerialMon.print("IP address: ");
    SerialMon.println(ip);
    
    return true;
  } else {
    SerialMon.println("GPRS connection failed");
    return false;
  }
}

// Write to TCA9535 register
void writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TCA9535_ADDR);
  Wire.write(reg);
  Wire.write(value);
  uint8_t error = Wire.endTransmission();
  
  if (error != 0) {
    Serial.print("Error writing to register 0x");
    Serial.print(reg, HEX);
    Serial.print(": Error code ");
    Serial.println(error);
  }
}

// Read from TCA9535 register
uint8_t readRegister(uint8_t reg) {
  Wire.beginTransmission(TCA9535_ADDR);
  Wire.write(reg);
  uint8_t error = Wire.endTransmission();
  
  if (error != 0) {
    Serial.print("Error setting register to read 0x");
    Serial.print(reg, HEX);
    Serial.print(": Error code ");
    Serial.println(error);
    return 0;
  }
  
  uint8_t bytesReceived = Wire.requestFrom(TCA9535_ADDR, 1);
  if (bytesReceived != 1) {
    Serial.print("Error reading from register 0x");
    Serial.print(reg, HEX);
    Serial.print(": Requested 1 byte, received ");
    Serial.println(bytesReceived);
    return 0;
  }
  
  return Wire.read();
}

// Initialize the TCA9535 and check if it's responding
bool initTCA9535() {
  Wire.beginTransmission(TCA9535_ADDR);
  uint8_t error = Wire.endTransmission();
  
  Serial.print("TCA9535 initialization result: ");
  Serial.println(error == 0 ? "Success" : "Failed");
  
  if (error != 0) {
    Serial.print("I2C error code: ");
    Serial.println(error);
    // Error codes:
    // 0: success
    // 1: data too long
    // 2: NACK on address
    // 3: NACK on data
    // 4: other error
  }
  
  return (error == 0);
}

// Function to control a specific relay
void setRelay(uint8_t relay, bool state) {
  if (relay > 7) return; // Validate relay number
  
  uint8_t relayState = readRegister(OUTPUT_PORT1);
  
  if (state) {
    // Turn ON relay
    relayState |= (1 << relay);
  } else {
    // Turn OFF relay
    relayState &= ~(1 << relay);
  }
  
  writeRegister(OUTPUT_PORT1, relayState);
}

// Function to read a specific button
bool readButton(uint8_t button) {
  if (button > 5) return false; // Validate button number
  
  uint8_t portValue = readRegister(INPUT_PORT0);
  return !(portValue & (1 << button)); // Buttons are active LOW
}

void setup() {
  Serial.begin(115200);
  delay(1000); // Give time for serial to initialize
  
  // Set up the built-in LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn ON LED to show power
  
  Serial.println("\n\n");
  Serial.println("======================================");
  Serial.println("ESP32 TCA9535 I/O Expander Debug Mode");
  Serial.println("======================================");
  
  // Initialize I2C
  Serial.println("Starting I2C initialization...");
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.println("I2C initialized");
  
  // Set INT pin as input
  pinMode(INT_PIN, INPUT_PULLUP);
  Serial.println("INT pin configured");
  
  // Blink LED to show we've reached this point
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(100);
    digitalWrite(LED_PIN, HIGH);
    delay(100);
  }
  
  // Initialize the TCA9535 I/O expander
  Serial.println("Trying to initialize TCA9535...");
  bool initSuccess = initTCA9535();
  
  if (!initSuccess) {
    Serial.println("Failed to initialize TCA9535!");
    Serial.println("Will continue without initialization. Check connections.");
    
    // Blink LED rapidly to indicate error
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(100);
    }
    
    // Continue anyway - don't get stuck in a loop
  } else {
    Serial.println("TCA9535 initialization successful!");
    
    // Configure Port 0 (buttons) as inputs (1 = input, 0 = output)
    Serial.println("Configuring Port 0 as inputs...");
    writeRegister(CONFIG_PORT0, 0xFF);
    
    // Configure Port 1 (relays) as outputs (1 = input, 0 = output)
    Serial.println("Configuring Port 1 as outputs...");
    writeRegister(CONFIG_PORT1, 0x00);
    
    // Initialize all relays to OFF state
    Serial.println("Setting all relays to OFF...");
    writeRegister(OUTPUT_PORT1, 0x00);
    
    Serial.println("TCA9535 fully initialized. Ready to control relays and read buttons.");
  }

  // Initialize modem serial with proper parameters
  // SerialAT.begin(115200, SERIAL_8N1, MODEM_TX, MODEM_RX);
  SerialAT.begin(115200, SERIAL_8N1, MODEM_TX, MODEM_RX);
  delay(1000);
  
  // Power on the modem with improved sequence
  powerOnModem();

  // Try to initialize and connect the modem
  bool modemInitialized = initModemAndConnectNetwork();
  if (modemInitialized) {
    // Set up SSL/TLS on the SIM7600G modem
    client.setCACert(AmazonRootCA);
    client.setCertificate(AWSClientCertificate);
    client.setPrivateKey(AWSClientPrivateKey);


    // Define the JSON payload for the POST request
    const char* postData = "{\"test\":\"test\"}";
    int postDataLength = strlen(postData);
    
    SerialMon.print(F("Performing HTTPS POST request... "));
    http.connectionKeepAlive();  // Currently, this is needed for HTTPS
    
    // Use the startRequest method that takes all parameters at once
    // This includes URL, method, content type, content length, and body
    int err = http.startRequest(
        resource,
        HTTP_METHOD_POST,
        "application/json",
        postDataLength,
        (const byte*)postData
    );
    if (err != 0) {
      SerialMon.println(F("failed to connect"));
      delay(10000);
      return;
    }

    int status = http.responseStatusCode();
    SerialMon.print(F("Response status code: "));
    SerialMon.println(status);
    if (!status) {
      delay(10000);
      return;
    }

    SerialMon.println(F("Response Headers:"));
    while (http.headerAvailable()) {
      String headerName  = http.readHeaderName();
      String headerValue = http.readHeaderValue();
      SerialMon.println("    " + headerName + " : " + headerValue);
    }

    int length = http.contentLength();
    if (length >= 0) {
      SerialMon.print(F("Content length is: "));
      SerialMon.println(length);
    }
    if (http.isResponseChunked()) {
      SerialMon.println(F("The response is chunked"));
    }

    String body = http.responseBody();
    SerialMon.println(F("Response:"));
    SerialMon.println(body);

    SerialMon.print(F("Body length is: "));
    SerialMon.println(body.length());

    // Shutdown
    http.stop();
    SerialMon.println(F("Server disconnected"));
   
  }

  if (!modemInitialized) {
    SerialMon.println("Trying alternative baud rate (9600)...");
    SerialMon.flush();
    SerialAT.updateBaudRate(9600);
    delay(1000);
    
    // Test with the new baud rate
    testModemAT();
    
    modemInitialized = initModemAndConnectNetwork();
  }
  
  // Final blink pattern to indicate setup complete
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(200);
    digitalWrite(LED_PIN, HIGH);
    delay(200);
  }
}

void loop() {
  static unsigned long lastPrintTime = 0;
  static bool connected = false;
  static unsigned long lastConnectionAttempt = 0;
  static unsigned long lastStatusCheck = 0;
  uint32_t lastReconnectAttempt = 0;
  
  // Print debug info every 3 seconds
  if (!connected && (millis() - lastConnectionAttempt > 30000)) {
    lastConnectionAttempt = millis();
    lastPrintTime = millis();

    SerialMon.println("Attempting to connect to cellular network...");
    connected = initModemAndConnectNetwork();

    if (connected) {
      SerialMon.println("Successfully connected to the internet!");

      // Blink the built-in LED to also indicate success
      for (int i = 0; i < 5; i++) {
        digitalWrite(LED_PIN, LOW);
        delay(100);
        digitalWrite(LED_PIN, HIGH);
        delay(100);
      }
    }

    Serial.println("==== Debug Info ====");
    
    // Read and print all button states
    uint8_t portValue = readRegister(INPUT_PORT0);
    Serial.print("Port 0 Value: 0b");
    for (int i = 7; i >= 0; i--) {
      Serial.print((portValue & (1 << i)) ? "1" : "0");
    }
    Serial.println();
    
    // Try reading a specific button
    Serial.print("Button 1 state: ");
    Serial.println(readButton(BUTTON1) ? "PRESSED" : "NOT PRESSED");
    
    // Read and print relay states
    uint8_t relayState = readRegister(OUTPUT_PORT1);
    Serial.print("Port 1 Value: 0b");
    for (int i = 7; i >= 0; i--) {
      Serial.print((relayState & (1 << i)) ? "1" : "0");
    }
    Serial.println();
    
    // Blink the LED to show the program is running
    digitalWrite(LED_PIN, LOW);
    delay(50);
    digitalWrite(LED_PIN, HIGH);
  }

  if (connected && (millis() - lastStatusCheck > 10000)) {
    lastStatusCheck = millis();
    
    if (modem.isGprsConnected()) {
      SerialMon.println("Still connected to cellular network");
    } else {
      SerialMon.println("Lost connection to cellular network. Will attempt to reconnect.");
      connected = false;
      
      // Turn off the lighting relay to indicate lost connection
      // setRelay(RELAY7, false);
      // SerialMon.println("Lighting relay turned OFF to indicate lost connection");
    }
  }
  
  // Read button state (BT1)
  uint8_t portValue = readRegister(INPUT_PORT0);
  buttonState = !(portValue & (1 << BUTTON1)); // Buttons are active LOW
  
  // Check if button state changed from not pressed to pressed
  if (buttonState && !lastButtonState) {
    Serial.println("Button pressed! Toggling Relay 1 (clear water)");
    
    // Toggle Relay 1
    uint8_t relayState = readRegister(OUTPUT_PORT1);
    if (relayState & (1 << RELAY1)) {
      // Turn off relay
      relayState &= ~(1 << RELAY1);
      Serial.println("Relay OFF");
    } else {
      // Turn on relay
      relayState |= (1 << RELAY1);
      Serial.println("Relay ON");
    }
    
    // Write new relay state
    writeRegister(OUTPUT_PORT1, relayState);
  }
  
  // Save last button state
  lastButtonState = buttonState;
  
  // Small delay to debounce button
  delay(50);
}
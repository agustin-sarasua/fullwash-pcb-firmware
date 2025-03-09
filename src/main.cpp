#include <Wire.h>
#include <Arduino.h>

// TCA9535 I2C address 
// Scanner found the device at address 0x24
#define TCA9535_ADDR 0x24

// TCA9535 register addresses
#define INPUT_PORT0      0x00
#define INPUT_PORT1      0x01
#define OUTPUT_PORT0     0x02
#define OUTPUT_PORT1     0x03
#define POLARITY_PORT0   0x04
#define POLARITY_PORT1   0x05
#define CONFIG_PORT0     0x06
#define CONFIG_PORT1     0x07

// Define pins on ESP32
#define I2C_SDA_PIN      19
#define I2C_SCL_PIN      18
#define INT_PIN          23  // Interrupt pin from IO expander

// Define a built-in LED pin for visual debugging
#define LED_PIN          12   // Blue LED connected to IO12 per schematic

// Define button and relay pins on TCA9535
#define BUTTON1          0   // BT1 is on P00
#define BUTTON2          1   // BT2 is on P01
#define BUTTON3          2   // BT3 is on P02
#define BUTTON4          3   // BT4 is on P03
#define BUTTON5          4   // BT5 is on P04
#define BUTTON6          5   // BT6 is on P05

// Relay pins on TCA9535 (Port 1)
#define RELAY1           0   // P10 - clear water
#define RELAY2           1   // P11 - Foam
#define RELAY3           2   // P12 - vacuum
#define RELAY4           3   // P13 - handwashing
#define RELAY5           4   // P14 - inflatable
#define RELAY6           5   // P15 - disinfect
#define RELAY7           6   // P16 - lighting

// Button states
bool buttonState = false;
bool lastButtonState = false;

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
  
  // Print debug info every 3 seconds
  if (millis() - lastPrintTime > 3000) {
    lastPrintTime = millis();
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
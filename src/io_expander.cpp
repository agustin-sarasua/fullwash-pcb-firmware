#include "io_expander.h"
#include "utilities.h"

IoExpander::IoExpander(uint8_t address, int sdaPin, int sclPin, int intPin)
    : _address(address), _sdaPin(sdaPin), _sclPin(sclPin), _intPin(intPin), _initialized(false) {
}

bool IoExpander::begin() {
    // Initialize I2C
    Wire.begin(_sdaPin, _sclPin);
    
    // Set INT pin as input
    pinMode(_intPin, INPUT_PULLUP);
    Serial.println("INT pin configured");
    
    // Check if device is responding
    Wire.beginTransmission(_address);
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
        return false;
    }
    
    _initialized = true;
    return true;
}

void IoExpander::writeRegister(uint8_t reg, uint8_t value) {
    if (!_initialized) return;
    
    Wire.beginTransmission(_address);
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

uint8_t IoExpander::readRegister(uint8_t reg) {
    if (!_initialized) return 0;
    
    // Add more debugging for INPUT_PORT0 reads
    bool isInputPortRead = (reg == INPUT_PORT0);
    // if (isInputPortRead) {
    //     Serial.print("Reading INPUT_PORT0... ");
    // }
    
    Wire.beginTransmission(_address);
    Wire.write(reg);
    uint8_t error = Wire.endTransmission();
    
    if (error != 0) {
        Serial.print("Error setting register to read 0x");
        Serial.print(reg, HEX);
        Serial.print(": Error code ");
        Serial.println(error);
        return 0;
    }
    
    uint8_t bytesReceived = Wire.requestFrom(_address, 1);
    if (bytesReceived != 1) {
        Serial.print("Error reading from register 0x");
        Serial.print(reg, HEX);
        Serial.print(": Requested 1 byte, received ");
        Serial.println(bytesReceived);
        return 0;
    }
    
    uint8_t value = Wire.read();
    
    // if (isInputPortRead) {
    //     Serial.printf("Value: 0x%02X | Binary: ", value);
    //     for (int i = 7; i >= 0; i--) {
    //         Serial.print((value & (1 << i)) ? "1" : "0");
    //     }
    //     Serial.println();
    // }
    
    return value;
}

void IoExpander::setRelay(uint8_t relay, bool state) {
    if (!_initialized || relay > 7) return; // Validate relay number
    
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

bool IoExpander::readButton(uint8_t button) {
    if (!_initialized || button > 5) {
        Serial.printf("Button read error: initialized=%d, button=%d\n", _initialized, button);
        return false; // Validate button number
    }
    uint8_t portValue = readRegister(INPUT_PORT0);
    
    // Force more debugging - print raw port value for each button read
    // Serial.printf("Reading Button %d (pin): Raw port value: 0x%02X, Bit value: %d\n", 
    //              button, portValue, (portValue & (1 << button)) ? 1 : 0);
    
    // For button 4 (BUTTON4 = 3), check specifically bit 3
    // if (button == BUTTON4) {
    //     Serial.printf("BUTTON4 detail: pin=%d, raw bit=%d, mask=0x%02X, test=%d\n", 
    //                 button, 
    //                 (portValue & (1 << button)) ? 1 : 0,
    //                 (1 << button),
    //                 (portValue & (1 << button)));
    // }
    bool buttonState = !(portValue & (1 << button)); // Buttons are active LOW
    
    // // Log button state for all buttons
    // Serial.printf("Button %d (pin): Raw port value: 0x%02X, State: %s\n", 
    //              button, portValue, buttonState ? "PRESSED" : "RELEASED");
    
    return buttonState;
}

void IoExpander::configurePortAsInput(uint8_t port, uint8_t mask) {
    if (!_initialized) return;
    
    uint8_t configReg = (port == 0) ? CONFIG_PORT0 : CONFIG_PORT1;
    
    Serial.printf("Configuring port %d as INPUT with mask: 0x%02X | Binary: ", port, mask);
    for (int i = 7; i >= 0; i--) {
        Serial.print((mask & (1 << i)) ? "1" : "0");
    }
    Serial.println();
    
    writeRegister(configReg, mask); // 1 = input in config register
    
    // Verify configuration was applied correctly
    uint8_t readBack = readRegister(configReg);
    Serial.printf("Config verification - Port %d config read back: 0x%02X | Binary: ", port, readBack);
    for (int i = 7; i >= 0; i--) {
        Serial.print((readBack & (1 << i)) ? "1" : "0");
    }
    Serial.println();
    
    if (readBack != mask) {
        Serial.printf("ERROR: Port %d config mismatch! Wrote 0x%02X but read back 0x%02X\n", port, mask, readBack);
    }
}

void IoExpander::configurePortAsOutput(uint8_t port, uint8_t mask) {
    if (!_initialized) return;
    
    uint8_t configReg = (port == 0) ? CONFIG_PORT0 : CONFIG_PORT1;
    uint8_t configValue = ~mask; // 0 = output in config register
    
    Serial.printf("Configuring port %d as OUTPUT with mask: 0x%02X | Config value: 0x%02X | Binary: ", 
                 port, mask, configValue);
    for (int i = 7; i >= 0; i--) {
        Serial.print((configValue & (1 << i)) ? "1" : "0");
    }
    Serial.println();
    
    writeRegister(configReg, configValue);
    
    // Verify configuration was applied correctly
    uint8_t readBack = readRegister(configReg);
    Serial.printf("Config verification - Port %d config read back: 0x%02X | Binary: ", port, readBack);
    for (int i = 7; i >= 0; i--) {
        Serial.print((readBack & (1 << i)) ? "1" : "0");
    }
    Serial.println();
    
    if (readBack != configValue) {
        Serial.printf("ERROR: Port %d config mismatch! Wrote 0x%02X but read back 0x%02X\n", port, configValue, readBack);
    }
}

bool IoExpander::toggleRelay(uint8_t relay) {
    if (!_initialized || relay > 7) return false;
    
    uint8_t relayState = readRegister(OUTPUT_PORT1);
    bool newState = !(relayState & (1 << relay));
    
    setRelay(relay, newState);
    return newState;
}

void IoExpander::printDebugInfo() {
    if (!_initialized) {
        Serial.println("IoExpander not initialized");
        return;
    }
    
    Serial.println("==== Debug Info ====");
    
    // Read and print all button states
    uint8_t portValue = readRegister(INPUT_PORT0);
    Serial.print("Port 0 Value: 0b");
    for (int i = 7; i >= 0; i--) {
        Serial.print((portValue & (1 << i)) ? "1" : "0");
    }
    Serial.println();
    
    // Read and print relay states
    uint8_t relayState = readRegister(OUTPUT_PORT1);
    Serial.print("Port 1 Value: 0b");
    for (int i = 7; i >= 0; i--) {
        Serial.print((relayState & (1 << i)) ? "1" : "0");
    }
    Serial.println();
}
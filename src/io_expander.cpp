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
    
    return Wire.read();
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
    if (!_initialized || button > 5) return false; // Validate button number
    
    uint8_t portValue = readRegister(INPUT_PORT0);
    return !(portValue & (1 << button)); // Buttons are active LOW
}

void IoExpander::configurePortAsInput(uint8_t port, uint8_t mask) {
    if (!_initialized) return;
    
    uint8_t configReg = (port == 0) ? CONFIG_PORT0 : CONFIG_PORT1;
    writeRegister(configReg, mask); // 1 = input in config register
}

void IoExpander::configurePortAsOutput(uint8_t port, uint8_t mask) {
    if (!_initialized) return;
    
    uint8_t configReg = (port == 0) ? CONFIG_PORT0 : CONFIG_PORT1;
    writeRegister(configReg, ~mask); // 0 = output in config register
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
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
    LOG_DEBUG("INT pin configured");
    
    // Check if device is responding
    Wire.beginTransmission(_address);
    uint8_t error = Wire.endTransmission();
    
    LOG_INFO("TCA9535 initialization result: %s", error == 0 ? "Success" : "Failed");
    
    if (error != 0) {
        LOG_ERROR("I2C error code: %d", error);
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
        LOG_ERROR("Error writing to register 0x%02X: Error code %d", reg, error);
    }
}

uint8_t IoExpander::readRegister(uint8_t reg) {
    if (!_initialized) return 0;
    
    // Add more debugging for INPUT_PORT0 reads
    bool isInputPortRead = (reg == INPUT_PORT0);
    // if (isInputPortRead) {
    //     LOG_DEBUG("Reading INPUT_PORT0...");
    // }
    
    Wire.beginTransmission(_address);
    Wire.write(reg);
    uint8_t error = Wire.endTransmission();
    
    if (error != 0) {
        LOG_ERROR("Error setting register to read 0x%02X: Error code %d", reg, error);
        return 0;
    }
    
    uint8_t bytesReceived = Wire.requestFrom(_address, 1);
    if (bytesReceived != 1) {
        LOG_ERROR("Error reading from register 0x%02X: Requested 1 byte, received %d", reg, bytesReceived);
        return 0;
    }
    
    uint8_t value = Wire.read();
    
    // if (isInputPortRead) {
    //     LOG_DEBUG("Value: 0x%02X | Binary: %d%d%d%d%d%d%d%d", value,
    //            (value & 0x80) ? 1 : 0, (value & 0x40) ? 1 : 0,
    //            (value & 0x20) ? 1 : 0, (value & 0x10) ? 1 : 0,
    //            (value & 0x08) ? 1 : 0, (value & 0x04) ? 1 : 0,
    //            (value & 0x02) ? 1 : 0, (value & 0x01) ? 1 : 0);
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
        LOG_ERROR("Button read error: initialized=%d, button=%d", _initialized, button);
        return false; // Validate button number
    }
    uint8_t portValue = readRegister(INPUT_PORT0);
    
    // Force more debugging - print raw port value for each button read
    // LOG_DEBUG("Reading Button %d (pin): Raw port value: 0x%02X, Bit value: %d", 
    //          button, portValue, (portValue & (1 << button)) ? 1 : 0);
    
    // For button 4 (BUTTON4 = 3), check specifically bit 3
    // if (button == BUTTON4) {
    //     LOG_DEBUG("BUTTON4 detail: pin=%d, raw bit=%d, mask=0x%02X, test=%d", 
    //              button, 
    //              (portValue & (1 << button)) ? 1 : 0,
    //              (1 << button),
    //              (portValue & (1 << button)));
    // }
    bool buttonState = !(portValue & (1 << button)); // Buttons are active LOW
    
    // // Log button state for all buttons
    // LOG_DEBUG("Button %d (pin): Raw port value: 0x%02X, State: %s", 
    //          button, portValue, buttonState ? "PRESSED" : "RELEASED");
    
    return buttonState;
}

void IoExpander::configurePortAsInput(uint8_t port, uint8_t mask) {
    if (!_initialized) return;
    
    uint8_t configReg = (port == 0) ? CONFIG_PORT0 : CONFIG_PORT1;
    
    LOG_DEBUG("Configuring port %d as INPUT with mask: 0x%02X | Binary: %d%d%d%d%d%d%d%d", 
             port, mask,
             (mask & 0x80) ? 1 : 0, (mask & 0x40) ? 1 : 0,
             (mask & 0x20) ? 1 : 0, (mask & 0x10) ? 1 : 0,
             (mask & 0x08) ? 1 : 0, (mask & 0x04) ? 1 : 0,
             (mask & 0x02) ? 1 : 0, (mask & 0x01) ? 1 : 0);
    
    writeRegister(configReg, mask); // 1 = input in config register
    
    // Verify configuration was applied correctly
    uint8_t readBack = readRegister(configReg);
    LOG_DEBUG("Config verification - Port %d config read back: 0x%02X | Binary: %d%d%d%d%d%d%d%d", 
             port, readBack,
             (readBack & 0x80) ? 1 : 0, (readBack & 0x40) ? 1 : 0,
             (readBack & 0x20) ? 1 : 0, (readBack & 0x10) ? 1 : 0,
             (readBack & 0x08) ? 1 : 0, (readBack & 0x04) ? 1 : 0,
             (readBack & 0x02) ? 1 : 0, (readBack & 0x01) ? 1 : 0);
    
    if (readBack != mask) {
        LOG_ERROR("Port %d config mismatch! Wrote 0x%02X but read back 0x%02X", port, mask, readBack);
    }
}

void IoExpander::configurePortAsOutput(uint8_t port, uint8_t mask) {
    if (!_initialized) return;
    
    uint8_t configReg = (port == 0) ? CONFIG_PORT0 : CONFIG_PORT1;
    uint8_t configValue = ~mask; // 0 = output in config register
    
    LOG_DEBUG("Configuring port %d as OUTPUT with mask: 0x%02X | Config value: 0x%02X | Binary: %d%d%d%d%d%d%d%d", 
             port, mask, configValue,
             (configValue & 0x80) ? 1 : 0, (configValue & 0x40) ? 1 : 0,
             (configValue & 0x20) ? 1 : 0, (configValue & 0x10) ? 1 : 0,
             (configValue & 0x08) ? 1 : 0, (configValue & 0x04) ? 1 : 0,
             (configValue & 0x02) ? 1 : 0, (configValue & 0x01) ? 1 : 0);
    
    writeRegister(configReg, configValue);
    
    // Verify configuration was applied correctly
    uint8_t readBack = readRegister(configReg);
    LOG_DEBUG("Config verification - Port %d config read back: 0x%02X | Binary: %d%d%d%d%d%d%d%d", 
             port, readBack,
             (readBack & 0x80) ? 1 : 0, (readBack & 0x40) ? 1 : 0,
             (readBack & 0x20) ? 1 : 0, (readBack & 0x10) ? 1 : 0,
             (readBack & 0x08) ? 1 : 0, (readBack & 0x04) ? 1 : 0,
             (readBack & 0x02) ? 1 : 0, (readBack & 0x01) ? 1 : 0);
    
    if (readBack != configValue) {
        LOG_ERROR("Port %d config mismatch! Wrote 0x%02X but read back 0x%02X", port, configValue, readBack);
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
        LOG_WARNING("IoExpander not initialized");
        return;
    }
    
    LOG_DEBUG("==== IO Expander Debug Info ====");
    
    // Read and print all button states
    uint8_t portValue = readRegister(INPUT_PORT0);
    LOG_DEBUG("Port 0 Value: 0b%d%d%d%d%d%d%d%d",
            (portValue & 0x80) ? 1 : 0, (portValue & 0x40) ? 1 : 0,
            (portValue & 0x20) ? 1 : 0, (portValue & 0x10) ? 1 : 0,
            (portValue & 0x08) ? 1 : 0, (portValue & 0x04) ? 1 : 0,
            (portValue & 0x02) ? 1 : 0, (portValue & 0x01) ? 1 : 0);
    
    // Read and print relay states
    uint8_t relayState = readRegister(OUTPUT_PORT1);
    LOG_DEBUG("Port 1 Value: 0b%d%d%d%d%d%d%d%d",
            (relayState & 0x80) ? 1 : 0, (relayState & 0x40) ? 1 : 0,
            (relayState & 0x20) ? 1 : 0, (relayState & 0x10) ? 1 : 0,
            (relayState & 0x08) ? 1 : 0, (relayState & 0x04) ? 1 : 0,
            (relayState & 0x02) ? 1 : 0, (relayState & 0x01) ? 1 : 0);
}
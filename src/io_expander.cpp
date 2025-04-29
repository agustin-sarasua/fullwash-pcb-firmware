#include "io_expander.h"
#include "utilities.h"

IoExpander::IoExpander(uint8_t address, int sdaPin, int sclPin, int intPin)
    : _address(address), _sdaPin(sdaPin), _sclPin(sclPin), _intPin(intPin), 
      _initialized(false), _lastInterruptTime(0), _lastCoinCheckTime(0),
      _consecutiveHighReadings(0) {
}

bool IoExpander::begin() {
    // Initialize I2C
    Wire.begin(_sdaPin, _sclPin);
    
    // Set INT pin as input with pullup
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
    return value;
}

void IoExpander::setRelay(uint8_t relay, bool state) {
    if (!_initialized || relay > 7) return;
    
    uint8_t currentValue = readRegister(OUTPUT_PORT1);
    uint8_t newValue;
    
    if (state) {
        newValue = currentValue | (1 << relay);
    } else {
        newValue = currentValue & ~(1 << relay);
    }
    
    writeRegister(OUTPUT_PORT1, newValue);
}

bool IoExpander::readButton(uint8_t button) {
    if (!_initialized || button > 5) {
        LOG_ERROR("Button read error: initialized=%d, button=%d", _initialized, button);
        return false; // Validate button number
    }
    uint8_t portValue = readRegister(INPUT_PORT0);
    bool buttonState = !(portValue & (1 << button)); // Buttons are active LOW
    return buttonState;
}

void IoExpander::configurePortAsInput(uint8_t port, uint8_t mask) {
    if (!_initialized) return;
    
    uint8_t configReg = (port == 0) ? CONFIG_PORT0 : CONFIG_PORT1;
    uint8_t currentConfig = readRegister(configReg);
    uint8_t newConfig = currentConfig | mask; // 1 = input
    writeRegister(configReg, newConfig);
}

void IoExpander::configurePortAsOutput(uint8_t port, uint8_t mask) {
    if (!_initialized) return;
    
    uint8_t configReg = (port == 0) ? CONFIG_PORT0 : CONFIG_PORT1;
    uint8_t currentConfig = readRegister(configReg);
    uint8_t newConfig = currentConfig & ~mask; // 0 = output
    writeRegister(configReg, newConfig);
}

void IoExpander::printDebugInfo() {
    if (!_initialized) {
        LOG_INFO("IO Expander not initialized");
        return;
    }
    
    LOG_INFO("IO Expander Debug Info:");
    LOG_INFO("Input Port 0: 0x%02X", readRegister(INPUT_PORT0));
    LOG_INFO("Input Port 1: 0x%02X", readRegister(INPUT_PORT1));
    LOG_INFO("Output Port 0: 0x%02X", readRegister(OUTPUT_PORT0));
    LOG_INFO("Output Port 1: 0x%02X", readRegister(OUTPUT_PORT1));
    LOG_INFO("Config Port 0: 0x%02X", readRegister(CONFIG_PORT0));
    LOG_INFO("Config Port 1: 0x%02X", readRegister(CONFIG_PORT1));
}

bool IoExpander::toggleRelay(uint8_t relay) {
    if (!_initialized || relay > 7) return false;
    
    uint8_t currentValue = readRegister(OUTPUT_PORT1);
    bool currentState = (currentValue & (1 << relay)) != 0;
    setRelay(relay, !currentState);
    return !currentState;
}

void IoExpander::enableInterrupt(uint8_t port, uint8_t pinMask) {
    if (!_initialized) {
        LOG_ERROR("Cannot enable interrupt - IO Expander not initialized");
        return;
    }
    
    // TCA9535 doesn't have explicit interrupt configuration registers
    // The interrupt is triggered when any input pin changes from the previously read value
    // Just make sure the pins are configured as inputs
    uint8_t configReg = (port == 0) ? CONFIG_PORT0 : CONFIG_PORT1;
    uint8_t currentConfig = readRegister(configReg);
    
    // Set the specified pins as inputs (1 = input in config register)
    uint8_t newConfig = currentConfig | pinMask;
    writeRegister(configReg, newConfig);
    
    LOG_INFO("Enabled interrupt monitoring for port %d with mask: 0x%02X", port, pinMask);
    
    // Perform an initial read of the port to establish a baseline
    // This is crucial for interrupt detection to work properly
    uint8_t inputReg = (port == 0) ? INPUT_PORT0 : INPUT_PORT1;
    uint8_t initialValue = readRegister(inputReg);
    
    LOG_DEBUG("Initial port %d value: 0x%02X", port, initialValue);
}

void IoExpander::setInterruptCallback(std::function<void(uint8_t)> callback) {
    _interruptCallback = callback;
}

void IoExpander::handleInterrupt() {
    if (!_initialized) return;
    
    unsigned long currentTime = millis();
    
    // Simple debounce - ignore interrupts that happen too quickly
    if (currentTime - _lastInterruptTime < DEBOUNCE_INTERVAL) {
        return;
    }

    // The TCA9535 interrupt is active LOW, so we check if the pin is LOW
    if (digitalRead(_intPin) == LOW) {
        // Read the input port to see what changed
        uint8_t portValue = readRegister(INPUT_PORT0);
        
        LOG_DEBUG("Interrupt detected! Port 0 Value: 0x%02X", portValue);
        
        // If we have a callback registered, call it with the port value
        if (_interruptCallback) {
            _interruptCallback(portValue);
        }
        
        // Update last interrupt time
        _lastInterruptTime = currentTime;
    }
}

void IoExpander::setCoinDetectionCallback(std::function<void()> callback) {
    _coinDetectionCallback = callback;
}

bool IoExpander::isCoinDetected() {
    if (!_initialized) return false;
    
    unsigned long currentTime = millis();
    
    // Check if enough time has passed since last check
    if (currentTime - _lastCoinCheckTime < COIN_CHECK_INTERVAL) {
        return false;
    }
    
    _lastCoinCheckTime = currentTime;
    
    // Read the coin signal pin
    uint8_t portValue = readRegister(INPUT_PORT0);
    bool coinSignal = !(portValue & (1 << COIN_SIG)); // Active LOW signal
    
    // Simple threshold-based detection
    if (coinSignal) {
        _consecutiveHighReadings++;
        if (_consecutiveHighReadings >= MIN_CONSECUTIVE_READINGS) {
            _consecutiveHighReadings = 0; // Reset counter
            if (_coinDetectionCallback) {
                _coinDetectionCallback();
            }
            return true;
        }
    } else {
        _consecutiveHighReadings = 0; // Reset counter if signal is low
    }
    
    return false;
}
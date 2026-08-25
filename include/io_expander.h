#ifndef IO_EXPANDER_H
#define IO_EXPANDER_H

#include <Arduino.h>
#include <Wire.h>
#include "logger.h"

class IoExpander {
public:
    // Constructor
    IoExpander(uint8_t address, int sdaPin, int sclPin, int intPin);
    
    // Initialize the I/O expander
    bool begin();
    
    // Write to register
    void writeRegister(uint8_t reg, uint8_t value);

    // Read from register. Returns 0 on I2C error - callers that must
    // distinguish an error from a genuine 0x00 should use the overload below.
    uint8_t readRegister(uint8_t reg);

    // Read from register with explicit error reporting. Returns false (and
    // leaves value untouched) on I2C error, so callers can discard the sample.
    bool readRegister(uint8_t reg, uint8_t& value);
    
    // Set relay state
    void setRelay(uint8_t relay, bool state);
    
    // Read button state
    bool readButton(uint8_t button);
    
    // Configure ports
    void configurePortAsInput(uint8_t port, uint8_t mask);
    void configurePortAsOutput(uint8_t port, uint8_t mask);
    
    // Debug info
    void printDebugInfo();
    
    // Toggle relay and return new state
    bool toggleRelay(uint8_t relay);
    
    // Check if a coin signal has been detected
    bool isCoinSignalDetected();
    
    // Set coin signal flag (called from FreeRTOS task)
    void setCoinSignal(uint8_t sig);
    
    // Clear the coin signal flag
    void clearCoinSignalFlag();

    // Public coin counter for debugging
    unsigned int _intCnt;
    uint8_t _portVal;

private:
    uint8_t _address;
    int _sdaPin;
    int _sclPin;
    int _intPin;

    bool _initialized;
    volatile bool _coinSignalDetected;
};

#endif // IO_EXPANDER_H
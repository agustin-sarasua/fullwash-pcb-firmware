#ifndef IO_EXPANDER_H
#define IO_EXPANDER_H

#include <Arduino.h>
#include <Wire.h>

class IoExpander {
public:
    // Constructor
    IoExpander(uint8_t address, int sdaPin, int sclPin, int intPin);
    
    // Initialize the I/O expander
    bool begin();
    
    // Write to register
    void writeRegister(uint8_t reg, uint8_t value);
    
    // Read from register
    uint8_t readRegister(uint8_t reg);
    
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

private:
    uint8_t _address;
    int _sdaPin;
    int _sclPin;
    int _intPin;
    bool _initialized;
};

#endif // IO_EXPANDER_H
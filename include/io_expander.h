#ifndef IO_EXPANDER_H
#define IO_EXPANDER_H

#include <Arduino.h>
#include <Wire.h>
#include "logger.h"
#include <functional>

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
    
    // Enable interrupt handler for specific port and pins
    void enableInterrupt(uint8_t port, uint8_t pinMask);
    
    // Set callback function for interrupt
    void setInterruptCallback(std::function<void(uint8_t)> callback);
    
    // Handle interrupt (called from ISR or main loop)
    void handleInterrupt();
    
    // Check if a coin signal has been detected
    bool isCoinSignalDetected();
    
    // Set coin signal flag (called from FreeRTOS task)
    void setCoinSignal(uint8_t sig);
    
    // Clear the coin signal flag
    void clearCoinSignalFlag();
    
    // Button detection methods
    bool isButtonDetected();
    uint8_t getDetectedButtonId();
    void setButtonFlag(uint8_t buttonId, bool state);
    void clearButtonFlag();
    
    // Public interrupt counter for debugging
    unsigned int _intCnt;
    uint8_t _portVal;

private:
    uint8_t _address;
    int _sdaPin;
    int _sclPin;
    int _intPin;
    
    bool _initialized;
    std::function<void(uint8_t)> _interruptCallback;
    unsigned long _lastInterruptTime;
    volatile bool _coinSignalDetected;
    
    // Button detection variables
    volatile bool _buttonDetected;
    volatile uint8_t _detectedButtonId;
    unsigned long _lastButtonTime[6]; // 6 buttons max
    
    static const unsigned long DEBOUNCE_INTERVAL = 50; // 50ms debounce
};

#endif // IO_EXPANDER_H
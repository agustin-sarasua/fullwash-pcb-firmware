#ifndef IO_EXPANDER_H
#define IO_EXPANDER_H

#include <Arduino.h>
#include <Wire.h>
#include "config.h"

// Structure to hold button states
struct ButtonState {
  bool button1;
  bool button2;
  bool button3;
  bool button4;
  bool button5;
  bool button6;
  
  ButtonState() : 
    button1(false), 
    button2(false), 
    button3(false), 
    button4(false), 
    button5(false), 
    button6(false) {}
};

// Structure to hold relay states
struct RelayState {
  bool relay1;  // Clear water
  bool relay2;  // Soap
  bool relay3;  // Wax
  bool relay4;  // High pressure
  bool relay5;  // Brush
  bool relay6;  // Reserved
  bool relay7;  // Reserved
  bool relay8;  // Lighting
  
  RelayState() : 
    relay1(false), 
    relay2(false), 
    relay3(false), 
    relay4(false), 
    relay5(false), 
    relay6(false), 
    relay7(false), 
    relay8(false) {}
};

class IOExpander {
public:
  IOExpander();
  
  // Initialize the I/O expander
  bool begin();
  
  // Read the state of all buttons
  ButtonState readButtons();
  
  // Read a specific button
  bool readButton(uint8_t button);
  
  // Set the state of a specific relay
  void setRelay(uint8_t relay, bool state);
  
  // Set all relays according to RelayState
  void setRelays(const RelayState& state);
  
  // Get current relay states
  RelayState getRelayStates();

private:
  bool initialized;
  RelayState currentRelayState;
  
  // Write to TCA9535 register
  void writeRegister(uint8_t reg, uint8_t value);
  
  // Read from TCA9535 register
  uint8_t readRegister(uint8_t reg);
};

#endif // IO_EXPANDER_H
#ifndef MACHINE_STATE_H
#define MACHINE_STATE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "io_expander.h"

// Machine operation modes
enum OperationMode {
  MODE_STANDBY,
  MODE_CLEAR_WATER,
  MODE_SOAP,
  MODE_WAX,
  MODE_HIGH_PRESSURE,
  MODE_BRUSH
};

// Machine state class
class MachineState {
public:
  MachineState();
  
  // Initialize machine state
  void begin();
  
  // Update machine state based on time and conditions
  // Returns true if state changed
  bool update();
  
  // Process button events and update state accordingly
  // Returns true if state changed
  bool processButtonEvents(const ButtonState& buttons);
  
  // Update machine state from backend response
  bool updateFromBackend(const String& jsonResponse);
  
  // Get relay states based on current machine state
  RelayState getRelayStates();
  
  // Get JSON payload for action event
  String getActionEventPayload();
  
  // Get JSON payload for machine state
  String getStatePayload();
  
  // Get current operation mode
  OperationMode getMode() const;
  
  // Get time remaining in current operation (in seconds)
  int getTimeRemaining() const;

private:
  OperationMode mode;
  unsigned long modeStartTime;
  unsigned long modeDuration;  // in milliseconds
  bool active;
  int credits;
  ButtonState lastButtonState;
  
  // Helper to generate JSON string
  String generateJson();
  
  // Set operation mode with duration
  void setMode(OperationMode newMode, unsigned long duration);
};

#endif // MACHINE_STATE_H
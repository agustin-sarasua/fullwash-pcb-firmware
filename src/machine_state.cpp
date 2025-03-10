#include "machine_state.h"

MachineState::MachineState() {
  mode = MODE_STANDBY;
  modeStartTime = 0;
  modeDuration = 0;
  active = false;
  credits = 0;
}

void MachineState::begin() {
  // Initialize to standby mode
  mode = MODE_STANDBY;
  active = false;
  credits = 0;
}

bool MachineState::update() {
  // Check if current mode has timed out
  if (active && mode != MODE_STANDBY) {
    unsigned long elapsedTime = millis() - modeStartTime;
    
    if (elapsedTime >= modeDuration) {
      // Mode has completed, return to standby
      OperationMode oldMode = mode;
      mode = MODE_STANDBY;
      active = false;
      
      Serial.print("Mode ");
      Serial.print(oldMode);
      Serial.println(" timed out, returning to standby");
      
      return true; // State changed
    }
  }
  
  return false; // No state change
}

bool MachineState::processButtonEvents(const ButtonState& buttons) {
  bool stateChanged = false;
  
  // Check for button press events (transition from not pressed to pressed)
  if (buttons.button1 && !lastButtonState.button1) {
    Serial.println("Button 1 pressed - Clear Water");
    if (mode != MODE_CLEAR_WATER) {
      setMode(MODE_CLEAR_WATER, 60000); // 60 seconds
      stateChanged = true;
    }
  }
  
  if (buttons.button2 && !lastButtonState.button2) {
    Serial.println("Button 2 pressed - Soap");
    if (mode != MODE_SOAP) {
      setMode(MODE_SOAP, 45000); // 45 seconds
      stateChanged = true;
    }
  }
  
  if (buttons.button3 && !lastButtonState.button3) {
    Serial.println("Button 3 pressed - Wax");
    if (mode != MODE_WAX) {
      setMode(MODE_WAX, 30000); // 30 seconds
      stateChanged = true;
    }
  }
  
  if (buttons.button4 && !lastButtonState.button4) {
    Serial.println("Button 4 pressed - High Pressure");
    if (mode != MODE_HIGH_PRESSURE) {
      setMode(MODE_HIGH_PRESSURE, 90000); // 90 seconds
      stateChanged = true;
    }
  }
  
  if (buttons.button5 && !lastButtonState.button5) {
    Serial.println("Button 5 pressed - Brush");
    if (mode != MODE_BRUSH) {
      setMode(MODE_BRUSH, 60000); // 60 seconds
      stateChanged = true;
    }
  }
  
  // Save current button state
  lastButtonState = buttons;
  
  return stateChanged;
}

bool MachineState::updateFromBackend(const String& jsonResponse) {
  // Parse JSON response
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, jsonResponse);
  
  if (error) {
    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());
    return false;
  }
  
  // Extract state information
  bool stateChanged = false;
  
  // Check for new operation mode
  if (doc.containsKey("mode")) {
    int newModeValue = doc["mode"];
    OperationMode newMode = static_cast<OperationMode>(newModeValue);
    
    if (newMode != mode) {
      Serial.print("Mode updated from backend: ");
      Serial.println(newModeValue);
      
      // Get duration if provided, otherwise use default
      unsigned long duration = 60000; // Default 60 seconds
      if (doc.containsKey("duration")) {
        duration = doc["duration"].as<unsigned long>() * 1000; // Convert to ms
      }
      
      setMode(newMode, duration);
      stateChanged = true;
    }
  }
  
  // Check for credits update
  if (doc.containsKey("credits")) {
    int newCredits = doc["credits"];
    if (newCredits != credits) {
      credits = newCredits;
      Serial.print("Credits updated from backend: ");
      Serial.println(credits);
      stateChanged = true;
    }
  }
  
  // Check for active status update
  if (doc.containsKey("active")) {
    bool newActive = doc["active"];
    if (newActive != active) {
      active = newActive;
      Serial.print("Active state updated from backend: ");
      Serial.println(active ? "Active" : "Inactive");
      stateChanged = true;
      
      // If machine was deactivated, return to standby
      if (!active && mode != MODE_STANDBY) {
        mode = MODE_STANDBY;
        Serial.println("Machine deactivated, returning to standby");
      }
    }
  }
  
  return stateChanged;
}

RelayState MachineState::getRelayStates() {
  RelayState relays;
  
  // Set relays based on current mode
  if (active) {
    switch (mode) {
      case MODE_CLEAR_WATER:
        relays.relay1 = true; // Clear water relay
        break;
        
      case MODE_SOAP:
        relays.relay2 = true; // Soap relay
        break;
        
      case MODE_WAX:
        relays.relay3 = true; // Wax relay
        break;
        
      case MODE_HIGH_PRESSURE:
        relays.relay4 = true; // High pressure relay
        break;
        
      case MODE_BRUSH:
        relays.relay5 = true; // Brush relay
        break;
        
      case MODE_STANDBY:
      default:
        // All relays off in standby
        break;
    }
    
    // Lighting relay is on whenever machine is active
    relays.relay8 = true;
  }
  
  return relays;
}

String MachineState::getActionEventPayload() {
  // Create JSON document
  DynamicJsonDocument doc(256);
  
  doc["machineId"] = MACHINE_ID;
  doc["event"] = "button_press";
  
  // Add current mode
  switch (mode) {
    case MODE_CLEAR_WATER:
      doc["action"] = "clear_water";
      break;
    case MODE_SOAP:
      doc["action"] = "soap";
      break;
    case MODE_WAX:
      doc["action"] = "wax";
      break;
    case MODE_HIGH_PRESSURE:
      doc["action"] = "high_pressure";
      break;
    case MODE_BRUSH:
      doc["action"] = "brush";
      break;
    case MODE_STANDBY:
    default:
      doc["action"] = "standby";
      break;
  }
  
  doc["timestamp"] = millis(); // Use system uptime as timestamp
  
  // Serialize to JSON string
  String payload;
  serializeJson(doc, payload);
  
  return payload;
}

String MachineState::getStatePayload() {
  // Create JSON document
  DynamicJsonDocument doc(256);
  
  doc["machineId"] = MACHINE_ID;
  doc["active"] = active;
  doc["credits"] = credits;
  
  // Add current mode
  switch (mode) {
    case MODE_CLEAR_WATER:
      doc["mode"] = "clear_water";
      break;
    case MODE_SOAP:
      doc["mode"] = "soap";
      break;
    case MODE_WAX:
      doc["mode"] = "wax";
      break;
    case MODE_HIGH_PRESSURE:
      doc["mode"] = "high_pressure";
      break;
    case MODE_BRUSH:
      doc["mode"] = "brush";
      break;
    case MODE_STANDBY:
    default:
      doc["mode"] = "standby";
      break;
  }
  
  // Add time remaining
  if (active && mode != MODE_STANDBY) {
    unsigned long elapsedTime = millis() - modeStartTime;
    if (elapsedTime < modeDuration) {
      doc["timeRemaining"] = (modeDuration - elapsedTime) / 1000; // Convert to seconds
    } else {
      doc["timeRemaining"] = 0;
    }
  } else {
    doc["timeRemaining"] = 0;
  }
  
  // Serialize to JSON string
  String payload;
  serializeJson(doc, payload);
  
  return payload;
}

OperationMode MachineState::getMode() const {
  return mode;
}

int MachineState::getTimeRemaining() const {
  if (!active || mode == MODE_STANDBY) {
    return 0;
  }
  
  unsigned long elapsedTime = millis() - modeStartTime;
  if (elapsedTime < modeDuration) {
    return (modeDuration - elapsedTime) / 1000; // Convert to seconds
  }
  
  return 0;
}

void MachineState::setMode(OperationMode newMode, unsigned long duration) {
  mode = newMode;
  modeStartTime = millis();
  modeDuration = duration;
  active = (newMode != MODE_STANDBY);
  
  Serial.print("Machine mode set to: ");
  Serial.print(static_cast<int>(mode));
  Serial.print(" for ");
  Serial.print(duration / 1000);
  Serial.println(" seconds");
}
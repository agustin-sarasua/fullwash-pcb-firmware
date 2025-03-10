#include "io_expander.h"

IOExpander::IOExpander() {
  initialized = false;
}

bool IOExpander::begin() {
  // Initialize I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  
  // Set INT pin as input
  pinMode(INT_PIN, INPUT_PULLUP);
  
  // Check if TCA9535 is responding
  Wire.beginTransmission(TCA9535_ADDR);
  uint8_t error = Wire.endTransmission();
  
  Serial.print("TCA9535 initialization result: ");
  Serial.println(error == 0 ? "Success" : "Failed");
  
  if (error != 0) {
    Serial.print("I2C error code: ");
    Serial.println(error);
    return false;
  }
  
  // Configure Port 0 (buttons) as inputs (1 = input, 0 = output)
  writeRegister(CONFIG_PORT0, 0xFF);
  
  // Configure Port 1 (relays) as outputs (1 = input, 0 = output)
  writeRegister(CONFIG_PORT1, 0x00);
  
  // Initialize all relays to OFF state
  writeRegister(OUTPUT_PORT1, 0x00);
  
  initialized = true;
  return true;
}

ButtonState IOExpander::readButtons() {
  ButtonState state;
  
  if (!initialized) return state;
  
  uint8_t portValue = readRegister(INPUT_PORT0);
  
  // Buttons are active LOW
  state.button1 = !(portValue & (1 << BUTTON1));
  state.button2 = !(portValue & (1 << BUTTON2));
  state.button3 = !(portValue & (1 << BUTTON3));
  state.button4 = !(portValue & (1 << BUTTON4));
  state.button5 = !(portValue & (1 << BUTTON5));
  state.button6 = !(portValue & (1 << BUTTON6));
  
  return state;
}

bool IOExpander::readButton(uint8_t button) {
  if (!initialized || button > 5) return false;
  
  uint8_t portValue = readRegister(INPUT_PORT0);
  return !(portValue & (1 << button)); // Buttons are active LOW
}

void IOExpander::setRelay(uint8_t relay, bool state) {
  if (!initialized || relay > 7) return;
  
  uint8_t relayState = readRegister(OUTPUT_PORT1);
  
  if (state) {
    // Turn ON relay
    relayState |= (1 << relay);
  } else {
    // Turn OFF relay
    relayState &= ~(1 << relay);
  }
  
  writeRegister(OUTPUT_PORT1, relayState);
  
  // Update current relay state
  switch (relay) {
    case RELAY1: currentRelayState.relay1 = state; break;
    case RELAY2: currentRelayState.relay2 = state; break;
    case RELAY3: currentRelayState.relay3 = state; break;
    case RELAY4: currentRelayState.relay4 = state; break;
    case RELAY5: currentRelayState.relay5 = state; break;
    case RELAY6: currentRelayState.relay6 = state; break;
    case RELAY7: currentRelayState.relay7 = state; break;
  }
}

void IOExpander::setRelays(const RelayState& state) {
  if (!initialized) return;
  
  uint8_t relayByte = 0;
  
  // Build relay byte from individual relay states
  if (state.relay1) relayByte |= (1 << RELAY1);
  if (state.relay2) relayByte |= (1 << RELAY2);
  if (state.relay3) relayByte |= (1 << RELAY3);
  if (state.relay4) relayByte |= (1 << RELAY4);
  if (state.relay5) relayByte |= (1 << RELAY5);
  if (state.relay6) relayByte |= (1 << RELAY6);
  if (state.relay7) relayByte |= (1 << RELAY7);
    
  // Write to the I/O expander
  writeRegister(OUTPUT_PORT1, relayByte);
  
  // Update current relay state
  currentRelayState = state;
}

RelayState IOExpander::getRelayStates() {
  if (!initialized) return RelayState();
  
  // Read directly from I/O expander for accurate state
  uint8_t relayByte = readRegister(OUTPUT_PORT1);
  
  RelayState state;
  state.relay1 = (relayByte & (1 << RELAY1)) != 0;
  state.relay2 = (relayByte & (1 << RELAY2)) != 0;
  state.relay3 = (relayByte & (1 << RELAY3)) != 0;
  state.relay4 = (relayByte & (1 << RELAY4)) != 0;
  state.relay5 = (relayByte & (1 << RELAY5)) != 0;
  state.relay6 = (relayByte & (1 << RELAY6)) != 0;
  state.relay7 = (relayByte & (1 << RELAY7)) != 0;
    
  // Update current state
  currentRelayState = state;
  
  return state;
}

void IOExpander::writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TCA9535_ADDR);
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

uint8_t IOExpander::readRegister(uint8_t reg) {
  Wire.beginTransmission(TCA9535_ADDR);
  Wire.write(reg);
  uint8_t error = Wire.endTransmission();
  
  if (error != 0) {
    Serial.print("Error setting register to read 0x");
    Serial.print(reg, HEX);
    Serial.print(": Error code ");
    Serial.println(error);
    return 0;
  }
  
  uint8_t bytesReceived = Wire.requestFrom(TCA9535_ADDR, 1);
  if (bytesReceived != 1) {
    Serial.print("Error reading from register 0x");
    Serial.print(reg, HEX);
    Serial.print(": Requested 1 byte, received ");
    Serial.println(bytesReceived);
    return 0;
  }
  
  return Wire.read();
}
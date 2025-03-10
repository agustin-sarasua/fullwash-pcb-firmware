#include <Arduino.h>
#include "config.h"
#include "modem.h"
#include "io_expander.h"
#include "mqtt_client.h"
#include "http_client.h"
#include "machine_state.h"

// Global objects
ModemManager modemManager;
IOExpander ioExpander;
AppHTTPClient httpClient;
MachineState machineState;

// Timing variables
unsigned long lastButtonCheckTime = 0;
unsigned long lastStateUpdateTime = 0;
unsigned long lastNetworkCheckTime = 0;
unsigned long lastBackendPollTime = 0;

void setup() {
  // Initialize serial communication
  Serial.begin(115200);
  delay(1000);  // Allow serial to initialize
  
  Serial.println("\n\n");
  Serial.println("======================================");
  Serial.println("Car Washing Machine Controller");
  Serial.println("======================================");
  
  // Set up built-in LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // Turn on LED to show power
  
  // Initialize I/O expander
  if (!ioExpander.begin()) {
    Serial.println("Warning: Failed to initialize I/O expander. Check connections.");
    // Blink LED to indicate error
    for (int i = 0; i < 5; i++) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(100);
    }
  }
  
  // Initialize modem
  if (!modemManager.begin()) {
    Serial.println("Error: Failed to initialize modem. System may not function properly.");
    // Blink LED rapidly to indicate critical error
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(50);
    }
  }
  
  // Connect to network
  if (modemManager.connectNetwork()) {
   
    // Initialize HTTP client
    httpClient.begin(&modemManager);
    
    // Blink LED to indicate successful connection
    for (int i = 0; i < 3; i++) {
      digitalWrite(LED_PIN, LOW);
      delay(200);
      digitalWrite(LED_PIN, HIGH);
      delay(200);
    }
  }
  
  // Initialize machine state
  machineState.begin();
  
  // Final blink to indicate setup complete
  digitalWrite(LED_PIN, LOW);
  delay(500);
  digitalWrite(LED_PIN, HIGH);
}

void loop() {
  // Current time for timing operations
  unsigned long currentTime = millis();
  
  // Check network connectivity periodically
  if (currentTime - lastNetworkCheckTime > NETWORK_CHECK_INTERVAL) {
    lastNetworkCheckTime = currentTime;
    
    if (!modemManager.isConnected()) {
      Serial.println("Network connection lost. Attempting to reconnect...");
      modemManager.connectNetwork();
    }
  }
  
  // Check for button presses
  if (currentTime - lastButtonCheckTime > BUTTON_CHECK_INTERVAL) {
    lastButtonCheckTime = currentTime;
    
    // Read all buttons
    ButtonState buttons = ioExpander.readButtons();
    
    // Process button events
    if (machineState.processButtonEvents(buttons)) {
      // Button state changed, update relays according to machine state
      ioExpander.setRelays(machineState.getRelayStates());
      
    //   // Publish event to MQTT
      if (modemManager.isConnected()) {
        String topic = String(MACHINE_ID) + "/action-event";
        String payload = machineState.getActionEventPayload();
    //     mqttClient.publish(topic.c_str(), payload.c_str());
      }
    }
  }
  
  // Update machine state periodically
  if (currentTime - lastStateUpdateTime > STATE_UPDATE_INTERVAL) {
    lastStateUpdateTime = currentTime;
    
    // Update machine state
    // if (machineState.update()) {
    //   // State changed, update relays
    //   ioExpander.setRelays(machineState.getRelayStates());
      
    //   // Publish state to MQTT
    //   if (modemManager.isConnected()) {
    //     String topic = String(MACHINE_ID) + "/machine-state";
    //     String payload = machineState.getStatePayload();
    //     // mqttClient.publish(topic.c_str(), payload.c_str());
    //   }
    // }
  }
  
  // Poll backend for machine state periodically
  if (currentTime - lastBackendPollTime > BACKEND_POLL_INTERVAL) {
    lastBackendPollTime = currentTime;
    
    if (modemManager.isConnected()) {
      String endpoint = "/machines/" + String(MACHINE_ID) + "/state";
      String response;
      
      if (httpClient.get(endpoint, response)) {
        // machineState.updateFromBackend(response);
        
        // Update relays based on state from backend
        // ioExpander.setRelays(machineState.getRelayStates());
      }
    }
  }
  
  // Blink LED occasionally to show the program is running
  if ((currentTime / 1000) % 5 == 0) {
    digitalWrite(LED_PIN, LOW);
    delay(50);
    digitalWrite(LED_PIN, HIGH);
  }
  
  // Small delay to prevent CPU hogging
  delay(10);
}
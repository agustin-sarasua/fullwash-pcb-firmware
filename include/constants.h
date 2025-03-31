#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <WString.h>
#include "utilities.h"
#include "logger.h"

// Default Log Level - Change to LOG_DEBUG for more verbose output or LOG_ERROR for production
#define DEFAULT_LOG_LEVEL LOG_INFO

// Pin Definitions
const int NUM_BUTTONS = 6;  // 6 buttons from BUTTON1 to BUTTON6 defined in utilities.h
const int STOP_BUTTON_PIN = BUTTON6;  // Use BUTTON6 as stop button
const int BUTTON_INDICES[] = {BUTTON1, BUTTON2, BUTTON3, BUTTON4, BUTTON5};  // Button indices for the I/O expander
const int LED_PIN_INIT = LED_PIN;  // Use the built-in LED for init state
const int RUNNING_LED_PIN = LED_PIN;  // Use same LED for running state

// Relay mapping - one-to-one with buttons (relay 7 not used)
const int RELAY_INDICES[] = {RELAY1, RELAY2, RELAY3, RELAY4, RELAY5};

// Timing Constants (can be overridden by configuration)
extern const unsigned long TOKEN_TIME;         // Time per token (2 minutes)
extern const unsigned long USER_INACTIVE_TIMEOUT; // Timeout for user inactivity

// MQTT Topics
extern const char* MACHINE_ID;

// Function declaration
String buildTopicName(const char* machineId, const char* eventType);

// MQTT Topics
extern String INIT_TOPIC;
extern String CONFIG_TOPIC;
extern String ACTION_TOPIC;
extern String STATE_TOPIC;
extern String COMMAND_TOPIC;

// Update topics when MACHINE_ID changes
void updateMqttTopics();

// QoS Levels
const uint32_t QOS0_AT_MOST_ONCE = 0;
const uint32_t QOS1_AT_LEAST_ONCE = 1;

#endif // CONSTANTS_H
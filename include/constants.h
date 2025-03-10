#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <WString.h>

// Pin Definitions
const int NUM_BUTTONS = 1;
const int STOP_BUTTON_PIN = 32;
const int BUTTON_PINS[] = {22};
const int LED_PIN_INIT = 18;
const int RUNNING_LED_PIN = 21;

// Timing Constants
const unsigned long STATE_RUNNING_TIME = 120000; // 2 minutes in milliseconds
const unsigned long TOKEN_TIME = 120000;         // Time per token (2 minutes)
const unsigned long USER_INACTIVE_TIMEOUT = 120000; // Timeout for user inactivity

// MQTT Topics
extern const char* MACHINE_ID;

// Function declaration
String buildTopicName(const char* machineId, const char* eventType);

// MQTT Topics
extern const String INIT_TOPIC;
extern const String CONFIG_TOPIC;
extern const String ACTION_TOPIC;
extern const String STATE_TOPIC;

// QoS Levels
const uint32_t QOS0_AT_MOST_ONCE = 0;
const uint32_t QOS1_AT_LEAST_ONCE = 1;

#endif // CONSTANTS_H
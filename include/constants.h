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

// Timing Constants
// NOTE: These values are divided by 6 for testing/debugging (20 seconds instead of 2 minutes)
// For production, remove "/ 6" to get 120000 ms = 2 minutes
const unsigned long STATE_RUNNING_TIME = 120000 / 6; // Currently 20 seconds (was 2 minutes)
const unsigned long TOKEN_TIME = 120000 / 6;         // Currently 20 seconds (was 2 minutes)
const unsigned long USER_INACTIVE_TIMEOUT = 120000 / 6; // Currently 20 seconds (was 2 minutes)

// MQTT Topics
extern String MACHINE_ID;  // Changed to String to allow dynamic loading

// Function declarations
String buildTopicName(const String& machineId, const char* eventType, const String& environment = "prod");
void updateMQTTTopics(const String& machineId, const String& environment = "prod");  // New function to update topics dynamically

// MQTT Topics (will be initialized dynamically)
extern String INIT_TOPIC;
extern String CONFIG_TOPIC;
extern String ACTION_TOPIC;
extern String STATE_TOPIC;
extern String COMMAND_TOPIC;

// QoS Levels
const uint32_t QOS0_AT_MOST_ONCE = 0;
const uint32_t QOS1_AT_LEAST_ONCE = 1;

// MQTT Message Queue Configuration
const int MQTT_QUEUE_SIZE = 50;  // Maximum number of messages to buffer
const int MQTT_MESSAGE_MAX_SIZE = 512;  // Maximum size for topic + payload

// MQTT Message Structure for Queue
struct MqttMessage {
    char topic[128];        // Topic name
    char payload[384];      // Message payload (JSON)
    uint8_t qos;           // Quality of Service (0 or 1)
    bool isCritical;       // Flag for message priority
    unsigned long timestamp;  // When message was created
};

#endif // CONSTANTS_H
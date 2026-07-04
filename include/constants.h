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
// Each token is 2 minutes (120000 ms)
const unsigned long TOKEN_TIME = 120000;         // 2 minutes per token
// Grace period: 30 seconds to press a button before token consumption begins/continues
const unsigned long GRACE_PERIOD_TIMEOUT = 30000; // 30 seconds
// Base inactivity timeout (used as minimum, actual timeout is calculated dynamically based on tokens)
const unsigned long BASE_INACTIVE_TIMEOUT = 30000; // 30 seconds base
// Session timeout in paused/idle state: 30s grace + 2min (1 token) = 150 seconds total
const unsigned long SESSION_END_TIMEOUT = 150000; // 2 minutes 30 seconds

// Coin Detection Constants
// Hardware contract (see docs/hardware-schematic-description.md): COIN_SIG is
// ACTIVE-HIGH. R64 (10k) pulls the line LOW at idle; the acceptor switch
// connects it to 3.3V while a coin passes, producing one HIGH pulse of a few
// tens of ms (with contact bounce on both edges).
// Startup delay before coin detection is active (prevents false triggers at boot)
const unsigned long COIN_STARTUP_DELAY = 3000;    // 3 seconds
// After the startup delay, the line must be continuously idle (LOW) this long
// before the detector arms - a single clean sample is not enough at boot
const unsigned long COIN_STARTUP_QUIET_MS = 1000;
// Minimum time between valid coin insertions
const unsigned long COIN_COOLDOWN_MS = 800;       // 800ms - allows rapid successive insertions
// Interval between coin signal polling reads
const unsigned long COIN_POLL_INTERVAL_MS = 5;    // 5ms polling interval
// Accepted coin pulse width window: shorter = noise spike, longer = stuck
// switch or wiring fault
const unsigned long COIN_MIN_PULSE_MS = 20;
const unsigned long COIN_MAX_PULSE_MS = 500;
// After a pulse ends, the line must stay idle this long before the next pulse
// can start (absorbs break bounce of the switch contact)
const unsigned long COIN_IDLE_REARM_MS = 50;

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
extern String GET_STATE_TOPIC;

// QoS Levels
const uint32_t QOS0_AT_MOST_ONCE = 0;
const uint32_t QOS1_AT_LEAST_ONCE = 1;

// MQTT Message Queue Configuration
const int MQTT_QUEUE_SIZE = 100;  // Maximum number of messages to buffer (increased to handle bursts)
const int MQTT_MESSAGE_MAX_SIZE = 512;  // Maximum size for topic + payload

// MQTT Message Structure for Queue
struct MqttMessage {
    char topic[128];        // Topic name
    char payload[384];      // Message payload (JSON)
    uint8_t qos;           // Quality of Service (0 or 1)
    bool isCritical;       // Flag for message priority
    unsigned long timestamp;  // When message was created
};

// Diagnostic flags
const bool ENABLE_NETWORK_MANAGER_DIAGNOSTICS = true; // Set to true to enable diagnostic messages in Network Manager task and MQTT client
const bool ENABLE_BUTTON_DIAGNOSTICS = false; // Set to true to enable diagnostic messages for button detection and handling

#endif // CONSTANTS_H
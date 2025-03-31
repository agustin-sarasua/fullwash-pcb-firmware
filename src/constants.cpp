#include "constants.h"

// MQTT Topics
const char* MACHINE_ID = "99";

// Default timing constants
const unsigned long TOKEN_TIME = 120000;         // 2 minutes in milliseconds
const unsigned long USER_INACTIVE_TIMEOUT = 120000; // 2 minutes in milliseconds

// Function definition
String buildTopicName(const char* machineId, const char* eventType) {
    return String("machines/") + machineId + "/" + eventType;
}

// MQTT Topics (mutable to allow updates after configuration changes)
String INIT_TOPIC = buildTopicName(MACHINE_ID, "init");
String CONFIG_TOPIC = buildTopicName(MACHINE_ID, "config");
String ACTION_TOPIC = buildTopicName(MACHINE_ID, "action");
String STATE_TOPIC = buildTopicName(MACHINE_ID, "state");
String COMMAND_TOPIC = buildTopicName(MACHINE_ID, "command");

// Update all MQTT topics when MACHINE_ID changes
void updateMqttTopics() {
    INIT_TOPIC = buildTopicName(MACHINE_ID, "init");
    CONFIG_TOPIC = buildTopicName(MACHINE_ID, "config");
    ACTION_TOPIC = buildTopicName(MACHINE_ID, "action");
    STATE_TOPIC = buildTopicName(MACHINE_ID, "state");
    COMMAND_TOPIC = buildTopicName(MACHINE_ID, "command");
}
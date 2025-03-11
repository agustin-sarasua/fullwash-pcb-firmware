#include "constants.h"

// MQTT Topics
const char* MACHINE_ID = "99";

// Function definition
String buildTopicName(const char* machineId, const char* eventType) {
    return String("machines/") + machineId + "/" + eventType;
}

// MQTT Topics
const String INIT_TOPIC = buildTopicName(MACHINE_ID, "init");
const String CONFIG_TOPIC = buildTopicName(MACHINE_ID, "config");
const String ACTION_TOPIC = buildTopicName(MACHINE_ID, "action");
const String STATE_TOPIC = buildTopicName(MACHINE_ID, "state");
#include "constants.h"

// MQTT Topics - will be loaded dynamically from BLE config
String MACHINE_ID = "99";  // Default value

// Function definitions
String buildTopicName(const String& machineId, const char* eventType, const String& environment) {
    String prefix;
    if (environment == "local") {
        prefix = "local/";
    } else {
        prefix = "machines/";
    }
    return prefix + machineId + "/" + eventType;
}

void updateMQTTTopics(const String& machineId, const String& environment) {
    MACHINE_ID = machineId;
    INIT_TOPIC = buildTopicName(MACHINE_ID, "init", environment);
    CONFIG_TOPIC = buildTopicName(MACHINE_ID, "config", environment);
    ACTION_TOPIC = buildTopicName(MACHINE_ID, "action", environment);
    STATE_TOPIC = buildTopicName(MACHINE_ID, "state", environment);
    COMMAND_TOPIC = buildTopicName(MACHINE_ID, "command", environment);
    GET_STATE_TOPIC = buildTopicName(MACHINE_ID, "get_state", environment);
    
    LOG_INFO("MQTT topics updated for machine ID: %s, environment: %s", MACHINE_ID.c_str(), environment.c_str());
}

// MQTT Topics - initialized with default, will be updated dynamically
String INIT_TOPIC = buildTopicName(MACHINE_ID, "init", "prod");
String CONFIG_TOPIC = buildTopicName(MACHINE_ID, "config", "prod");
String ACTION_TOPIC = buildTopicName(MACHINE_ID, "action", "prod");
String STATE_TOPIC = buildTopicName(MACHINE_ID, "state", "prod");
String COMMAND_TOPIC = buildTopicName(MACHINE_ID, "command", "prod");
String GET_STATE_TOPIC = buildTopicName(MACHINE_ID, "get_state", "prod");
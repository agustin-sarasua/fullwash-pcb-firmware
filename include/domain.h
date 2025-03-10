#ifndef DOMAIN_H
#define DOMAIN_H

#include <WString.h>

// Struct for machine configuration
struct MachineConfig {
    String sessionId;
    String userId;
    String userName;
    int tokens;
    String timestamp;
    unsigned long timestampMillis;
    bool isLoaded;
};

// Enum for machine states
enum MachineState {
    STATE_FREE,
    STATE_IDLE,
    STATE_RUNNING,
    STATE_PAUSED
};

// Enum for machine actions
enum MachineAction {
    ACTION_SETUP,
    ACTION_START,
    ACTION_STOP,
    ACTION_PAUSE,
    ACTION_RESUME
};

// Function declarations
String getMachineActionString(MachineAction action);
String getMachineStateString(MachineState state);

// Enum for trigger types
enum TriggerType {
    MANUAL,
    AUTOMATIC
};

// Enum for machine button names
enum MachineButtonName {
    BUTTON_1,
    BUTTON_2,
    BUTTON_3,
    BUTTON_4,
    BUTTON_5
};

#endif // DOMAIN_H
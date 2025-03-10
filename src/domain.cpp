#include "domain.h"

// Function definitions
String getMachineActionString(MachineAction action) {
    switch (action) {
        case ACTION_SETUP: return "SETUP";
        case ACTION_START: return "START";
        case ACTION_STOP: return "STOP";
        case ACTION_PAUSE: return "PAUSE";
        case ACTION_RESUME: return "RESUME";
        default: return "START";
    }
}

String getMachineStateString(MachineState state) {
    switch (state) {
        case STATE_FREE: return "FREE";
        case STATE_IDLE: return "IDLE";
        case STATE_RUNNING: return "RUNNING";
        case STATE_PAUSED: return "PAUSED";
        default: return "IDLE";
    }
}
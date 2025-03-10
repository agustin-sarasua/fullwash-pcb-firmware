#ifndef CAR_WASH_CONTROLLER_H
#define CAR_WASH_CONTROLLER_H

#include "mqtt_lte_client.h"
#include "utilities.h"
#include <ArduinoJson.h>
#include <TimeLib.h>
#include <algorithm>
#include "domain.h"
#include "constants.h"

class CarWashController {
public:
    CarWashController(MqttLteClient& client);
    void handleMqttMessage(const char* topic, const uint8_t* payload, uint32_t len);
    void handleButtons();
    void pauseMachine();
    void resumeMachine(int buttonIndex);
    void stopMachine(TriggerType triggerType = AUTOMATIC);
    void activateButton(int buttonIndex, TriggerType triggerType = MANUAL);
    void tokenExpired();
    void update();
    void publishMachineSetupActionEvent();

private:
    MqttLteClient& mqttClient;
    MachineState currentState;
    MachineConfig config;
    
    unsigned long lastActionTime;
    unsigned long tokenStartTime;
    int activeButton;
    unsigned long tokenTimeElapsed;
    unsigned long pauseStartTime;

    static const unsigned long DEBOUNCE_DELAY = 50;
    unsigned long lastDebounceTime[NUM_BUTTONS + 1];
    int lastButtonState[NUM_BUTTONS + 1];

    unsigned long lastStatePublishTime;
    const unsigned long STATE_PUBLISH_INTERVAL = 10000;

    unsigned long getSecondsLeft();
    String getTimestamp();
    void publishActionEvent(int buttonIndex, MachineAction machineAction, TriggerType triggerType = MANUAL);
    void publishPeriodicState(bool force = false);
};

#endif
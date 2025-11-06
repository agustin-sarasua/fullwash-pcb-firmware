#ifndef CAR_WASH_CONTROLLER_H
#define CAR_WASH_CONTROLLER_H

#include "mqtt_lte_client.h"
#include "utilities.h"
#include <ArduinoJson.h>
#include <TimeLib.h>
#include <algorithm>
#include "domain.h"
#include "constants.h"
#include "logger.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Forward declaration
class RTCManager;

// External reference to the MQTT publish queue (defined in main.cpp)
extern QueueHandle_t xMqttPublishQueue;

class CarWashController {
public:
    CarWashController(MqttLteClient& client);
    void handleMqttMessage(const char* topic, const uint8_t* payload, uint32_t len);
    void handleButtons();
    void handleCoinAcceptor();
    void pauseMachine();
    void resumeMachine(int buttonIndex);
    void stopMachine(TriggerType triggerType = AUTOMATIC);
    void activateButton(int buttonIndex, TriggerType triggerType = MANUAL);
    void tokenExpired();
    void update();
    void publishMachineSetupActionEvent();
    void publishCoinInsertedEvent();
    
    // Debug method to simulate a coin insertion
    void simulateCoinInsertion();
    
    // Getter methods
    MachineState getCurrentState() const;
    bool isMachineLoaded() const;
    String getTimestamp();
    void setLogLevel(LogLevel level);
    
    // RTC integration
    void setRTCManager(RTCManager* rtc);
    
    // Additional getters for LCD display
    String getUserName() const { return config.userName; }
    int getTokensLeft() const { return config.tokens; }
    unsigned long getTimeToInactivityTimeout() const;
    unsigned long getSecondsLeft();

private:
    MqttLteClient& mqttClient;
    RTCManager* rtcManager;  // Pointer to RTC manager for accurate timestamps
    MachineState currentState;
    MachineConfig config;
    
    unsigned long lastActionTime;
    unsigned long tokenStartTime;
    int activeButton;
    unsigned long tokenTimeElapsed;
    unsigned long pauseStartTime;
    unsigned long lastPauseResumeTime; // Track last pause/resume to prevent rapid toggling

    static const unsigned long DEBOUNCE_DELAY = 100;
    static const unsigned long PAUSE_RESUME_COOLDOWN = 500; // Minimum time between pause/resume (500ms)
    static const unsigned long COIN_DEBOUNCE_DELAY = 50;
    static const unsigned long COIN_PROCESS_COOLDOWN = 2000; // 1000ms (1 second) between accepted coins
    static const unsigned long COIN_EDGE_WINDOW = 500;      // Maximum window for detecting multiple edges as one coin
    static const int COIN_MIN_EDGES = 2;                    // Minimum number of edges to detect a coin
    static const unsigned long COUNTER_ACTIVE_DURATION = 120; // How long counter signal is typically active
    unsigned long lastDebounceTime[NUM_BUTTONS + 1];
    int lastButtonState[NUM_BUTTONS + 1];
    unsigned long lastCoinDebounceTime;
    unsigned long lastCoinProcessedTime; // Track when a coin was last successfully processed
    int lastCoinState;

    unsigned long lastStatePublishTime;
    const unsigned long STATE_PUBLISH_INTERVAL = 10000;

    void diagnosticCoinSignal();
    void processCoinInsertion(unsigned long currentTime);

    void publishActionEvent(int buttonIndex, MachineAction machineAction, TriggerType triggerType = MANUAL);
    void publishPeriodicState(bool force = false);
    
    // Helper method to queue MQTT messages for the dedicated publisher task
    bool queueMqttMessage(const char* topic, const char* payload, uint8_t qos, bool isCritical);
};

#endif
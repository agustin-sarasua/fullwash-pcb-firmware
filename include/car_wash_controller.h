#ifndef CAR_WASH_CONTROLLER_H
#define CAR_WASH_CONTROLLER_H

#include "utilities.h"
#include <ArduinoJson.h>
#include <algorithm>
#include "domain.h"
#include "constants.h"
#include "logger.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// A single debounced, edge-triggered button press event, produced by
// TaskButtonDetector and consumed by CarWashController::handleButtons().
struct ButtonEvent {
    uint8_t buttonId;      // 0-4 = function buttons 1-5, 5 = STOP button
    uint32_t pressedAtMs;  // millis() at edge detection
};

// External reference to the button event queue (defined in main.cpp)
extern QueueHandle_t xButtonEventQueue;

class CarWashController {
public:
    CarWashController();
    // Returns true if this call actually loaded the machine with tokens (a real wash
    // session started), false otherwise — in particular, false for the machine-99
    // "set new machine ID" setup path, which intentionally does not load tokens. The BLE
    // loader (ble_machine_loader.cpp) uses this to avoid reporting "Success: Machine
    // loaded" back over BLE when nothing was actually loaded.
    //
    // Named handleMqttMessage() for historical reasons: this used to be invoked from an
    // MQTT subscription callback. That transport is gone (BLE-only now); the BLE loader
    // is the only remaining caller, using a locally-built INIT payload.
    bool handleMqttMessage(const char* topic, const uint8_t* payload, uint32_t len);
    void handleButtons();
    void handleCoinAcceptor();
    void pauseMachine();
    void resumeMachine(int buttonIndex);
    void stopMachine(TriggerType triggerType = AUTOMATIC);
    void activateButton(int buttonIndex, TriggerType triggerType = MANUAL);
    void tokenExpired();
    void update();

    // Debug method to simulate a coin insertion
    void simulateCoinInsertion();
    
    // Debug method to print current relay states
    void printRelayStates();
    
    // Getter methods
    MachineState getCurrentState() const;
    bool isMachineLoaded() const;
    String getTimestamp();
    void setLogLevel(LogLevel level);
    
    // Additional getters for LCD display
    String getUserName() const { return config.userName; }
    int getTokensLeft() const { return config.tokens; }
    unsigned long getTimeToInactivityTimeout() const;
    unsigned long getSecondsLeft();
    unsigned long getGracePeriodSecondsLeft() const; // Get remaining grace period time (0 if not active)
    int getActiveButton() const { return activeButton; } // Get current active button index (-1 if none)

private:
    MachineState currentState;
    MachineConfig config;
    
    unsigned long lastActionTime;
    unsigned long tokenStartTime;
    int activeButton;
    unsigned long tokenTimeElapsed;
    unsigned long pauseStartTime;
    unsigned long lastPauseResumeTime; // Track last pause/resume to prevent rapid toggling
    unsigned long lastFunctionSwitchTime; // Track last function switch to prevent same button press from pausing immediately after switch
    unsigned long gracePeriodStartTime; // Track when grace period started (for IDLE or PAUSED)
    bool gracePeriodActive; // Whether the grace period is currently active
    int tokensConsumedCount; // Track how many tokens have been consumed in current session
    unsigned long lastSessionLoadTime; // millis() when the current session was loaded; used to
                                        // discard stale ButtonEvents queued before this session existed

    static const unsigned long PAUSE_RESUME_COOLDOWN = 500; // Minimum time between pause/resume (500ms)
    static const unsigned long FUNCTION_SWITCH_COOLDOWN = 500; // Minimum time after function switch before same button can pause (500ms)
    static const unsigned long BOUNCE_DUPLICATE_WINDOW_MS = 200; // Window after activation/function-switch in which a duplicate button event is treated as a bounce, not a new press
    // Note: Coin detection constants moved to constants.h (COIN_STARTUP_DELAY, COIN_COOLDOWN_MS, etc.)
    unsigned long lastCoinProcessedTime; // Track when a coin was last successfully processed (informational)

    void processButtonEvent(const ButtonEvent& evt); // Handle one debounced button press event
    void processCoinInsertion(unsigned long currentTime);
    void autoConsumeToken(); // Automatically consume a token and transition to PAUSED
    void consumeNextToken(); // Consume next token when current one expires
    void switchFunction(int newButtonIndex); // Switch to a different function while RUNNING
    unsigned long getInactivityTimeout() const; // Calculate dynamic inactivity timeout based on tokens

};

#endif
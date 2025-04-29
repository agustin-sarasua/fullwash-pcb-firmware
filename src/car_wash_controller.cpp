#include "car_wash_controller.h"
#include "io_expander.h"

CarWashController::CarWashController(MqttLteClient& client)
    : mqttClient(client),
      currentState(STATE_FREE),
      lastActionTime(0),
      activeButton(-1),
      tokenStartTime(0),
      lastStatePublishTime(0),
      tokenTimeElapsed(0),
      pauseStartTime(0) {
          
    // Initialize button states
    for (int i = 0; i < NUM_BUTTONS + 1; i++) {
        lastDebounceTime[i] = 0;
        lastButtonState[i] = HIGH;
    }
}

void CarWashController::handleMqttMessage(const char* topic, const uint8_t* payload, uint32_t len) {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, payload, len);
    
    if (error) {
        LOG_ERROR("Failed to parse MQTT message: %s", error.c_str());
        return;
    }
    
    if (String(topic) == INIT_TOPIC) {
        LOG_INFO("Received initialization message");
        config.sessionId = doc["sessionId"].as<String>();
        config.userId = doc["userId"].as<String>();
        config.userName = doc["userName"].as<String>();
        config.tokens = doc["tokens"].as<int>();
        config.physicalTokens = 0;  // No physical tokens when session is initialized remotely
        config.timestamp = doc["timestamp"].as<String>();
        config.timestampMillis = millis();
        config.isLoaded = true;
        currentState = STATE_IDLE;
        lastActionTime = millis();
        LOG_INFO("Switching on LED");
        digitalWrite(LED_PIN_INIT, HIGH);
        LOG_INFO("Machine loaded with new configuration");
    } else if (String(topic) == CONFIG_TOPIC) {
        LOG_DEBUG("Updating machine configuration timestamp: %s", doc["timestamp"].as<String>().c_str());
        config.timestamp = doc["timestamp"].as<String>();
        config.timestampMillis = millis();
        config.sessionId = "";
        config.userId = "";
        config.userName = "";
        config.tokens = 0;
        config.physicalTokens = 0;
        config.isLoaded = false;
    } else {
        LOG_WARNING("Unknown topic: %s", topic);
    }
}

void CarWashController::handleButtons() {
    // Get reference to the IO expander (assumed to be a global or accessible)
    extern IoExpander ioExpander;

    // Read all 5 function buttons - using simple approach
    for (int i = 0; i < NUM_BUTTONS-1; i++) {  // -1 because last button is STOP button
        int buttonPin = BUTTON_INDICES[i];
        
        // Check if button is pressed (active LOW in the IO expander)
        bool buttonPressed = ioExpander.readButton(buttonPin);
        
        // Handle button press with debouncing
        if (buttonPressed) {
            // If button wasn't pressed before or enough time has passed since last action
            if (lastButtonState[i] == HIGH || 
                (millis() - lastDebounceTime[i]) > DEBOUNCE_DELAY * 5) {
                
                // Record time of this press
                lastDebounceTime[i] = millis();
                lastButtonState[i] = LOW;  // Now pressed (active LOW)
                
                LOG_INFO("Button %d pressed and debounced!", i+1);
                
                // Process button action
                if (config.isLoaded) {
                    if (currentState == STATE_IDLE) {
                        LOG_INFO("Activating button %d in IDLE state", i+1);
                        activateButton(i, MANUAL);
                    } else if (currentState == STATE_RUNNING && i == activeButton) {
                        LOG_INFO("Pausing machine - same button pressed while running");
                        pauseMachine();
                    } else if (currentState == STATE_PAUSED) {
                        LOG_INFO("Resuming machine with button %d", i+1);
                        resumeMachine(i);
                    } else {
                        LOG_WARNING("Button %d pressed but no action taken. State=%d, activeButton=%d", 
                                   i+1, currentState, activeButton);
                    }
                } else {
                    LOG_WARNING("Button %d pressed but machine not loaded", i+1);
                }
            }
        } else {
            lastButtonState[i] = HIGH;  // Button released
        }
    }
    
    // Handle STOP button separately
    int stopButtonPin = BUTTON_INDICES[NUM_BUTTONS-1];
    bool stopButtonPressed = ioExpander.readButton(stopButtonPin);
    
    if (stopButtonPressed) {
        if (lastButtonState[NUM_BUTTONS-1] == HIGH || 
            (millis() - lastDebounceTime[NUM_BUTTONS-1]) > DEBOUNCE_DELAY * 5) {
            
            lastDebounceTime[NUM_BUTTONS-1] = millis();
            lastButtonState[NUM_BUTTONS-1] = LOW;
            
            LOG_INFO("STOP button pressed and debounced!");
            
            if (currentState == STATE_RUNNING || currentState == STATE_PAUSED) {
                stopMachine(MANUAL);
            } else {
                LOG_WARNING("STOP button pressed but machine not running or paused");
            }
        }
    } else {
        lastButtonState[NUM_BUTTONS-1] = HIGH;
    }
}

void CarWashController::pauseMachine() {
    if (currentState != STATE_RUNNING) {
        LOG_WARNING("Cannot pause - machine not running");
        return;
    }
    
    LOG_INFO("Pausing machine...");
    
    // Turn off the active relay
    extern IoExpander ioExpander;
    ioExpander.setRelay(RELAY_INDICES[activeButton], false);
    
    currentState = STATE_PAUSED;
    pauseStartTime = millis();
    lastActionTime = millis();
    
    publishActionEvent(activeButton, ACTION_PAUSE);
}

void CarWashController::resumeMachine(int buttonIndex) {
    if (currentState != STATE_PAUSED) {
        LOG_WARNING("Cannot resume - machine not paused");
        return;
    }
    
    LOG_INFO("Resuming machine with button %d", buttonIndex+1);
    
    // Turn on the relay for the specified button
    extern IoExpander ioExpander;
    ioExpander.setRelay(RELAY_INDICES[buttonIndex], true);
    
    currentState = STATE_RUNNING;
    activeButton = buttonIndex;
    lastActionTime = millis();
    
    // Add the pause duration to the token start time
    tokenStartTime += (millis() - pauseStartTime);
    
    publishActionEvent(buttonIndex, ACTION_RESUME);
}

void CarWashController::stopMachine(TriggerType triggerType) {
    LOG_INFO("Stopping machine...");
    
    if (currentState == STATE_RUNNING || currentState == STATE_PAUSED) {
        // Turn off the active relay
        extern IoExpander ioExpander;
        ioExpander.setRelay(RELAY_INDICES[activeButton], false);
    }
    
    config.isLoaded = false;
    currentState = STATE_FREE;
    activeButton = -1;
    tokenStartTime = 0;
    tokenTimeElapsed = 0;
    pauseStartTime = 0;
    
    // Publish stop event
    if (activeButton >= 0) {
        publishActionEvent(activeButton, ACTION_STOP, triggerType);
    }
}

void CarWashController::activateButton(int buttonIndex, TriggerType triggerType) {
    LOG_INFO("Activating button: %d", buttonIndex+1);  // +1 for human-readable button number

    if (config.tokens <= 0) {
        LOG_WARNING("Cannot activate - no tokens left");
        return;
    }

    digitalWrite(RUNNING_LED_PIN, HIGH);
    currentState = STATE_RUNNING;
    activeButton = buttonIndex;
    
    // Turn on the corresponding relay
    extern IoExpander ioExpander;
    ioExpander.setRelay(RELAY_INDICES[buttonIndex], true);
    
    lastActionTime = millis();
    tokenStartTime = millis();
    tokenTimeElapsed = 0;
    
    config.tokens--;

    publishActionEvent(buttonIndex, ACTION_START, triggerType);
}

void CarWashController::tokenExpired() {
    if (activeButton >= 0) {
        // Turn off the active relay
        extern IoExpander ioExpander;
        ioExpander.setRelay(RELAY_INDICES[activeButton], false);
    }
    activeButton = -1;
    currentState = STATE_IDLE;
    lastActionTime = millis();
}

void CarWashController::update() {
    unsigned long currentTime = millis();
    
    // Check if we need to publish periodic state
    if (currentTime - lastStatePublishTime > STATE_PUBLISH_INTERVAL) {
        publishPeriodicState();
        lastStatePublishTime = currentTime;
    }
    
    // Check for token expiration if machine is running
    if (currentState == STATE_RUNNING && config.tokens > 0) {
        tokenTimeElapsed = currentTime - tokenStartTime;
        if (tokenTimeElapsed >= TOKEN_DURATION) {
            LOG_INFO("Token expired");
            tokenExpired();
        }
    }
}

void CarWashController::publishMachineSetupActionEvent() {
    publishActionEvent(-1, ACTION_SETUP);
}

MachineState CarWashController::getCurrentState() const {
    return currentState;
}

bool CarWashController::isMachineLoaded() const {
    return config.isLoaded;
}

String CarWashController::getTimestamp() {
    return config.timestamp;
}

void CarWashController::setLogLevel(LogLevel level) {
    Logger::setLogLevel(level);
}

unsigned long CarWashController::getTimeToInactivityTimeout() const {
    return USER_INACTIVE_TIMEOUT - (millis() - lastActionTime);
}

unsigned long CarWashController::getSecondsLeft() {
    if (currentState != STATE_RUNNING) return 0;
    return (TOKEN_DURATION - tokenTimeElapsed) / 1000;
}

void CarWashController::publishActionEvent(int buttonIndex, MachineAction machineAction, TriggerType triggerType) {
    StaticJsonDocument<256> doc;
    
    doc["sessionId"] = config.sessionId;
    doc["userId"] = config.userId;
    doc["userName"] = config.userName;
    doc["action"] = getMachineActionString(machineAction);
    doc["triggerType"] = triggerType == MANUAL ? "MANUAL" : "AUTOMATIC";
    doc["timestamp"] = getTimestamp();
    
    if (buttonIndex >= 0) {
        doc["buttonIndex"] = buttonIndex;
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    mqttClient.publish(ACTION_TOPIC.c_str(), jsonString.c_str());
}

void CarWashController::publishPeriodicState(bool force) {
    if (!force && !config.isLoaded) return;
    
    StaticJsonDocument<256> doc;
    
    doc["sessionId"] = config.sessionId;
    doc["userId"] = config.userId;
    doc["userName"] = config.userName;
    doc["state"] = getMachineStateString(currentState);
    doc["tokens"] = config.tokens;
    doc["timestamp"] = getTimestamp();
    
    if (currentState == STATE_RUNNING) {
        doc["activeButton"] = activeButton;
        doc["secondsLeft"] = getSecondsLeft();
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    mqttClient.publish(STATE_TOPIC.c_str(), jsonString.c_str());
}
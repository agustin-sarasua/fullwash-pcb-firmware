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
      pauseStartTime(0),
      lastCoinDebounceTime(0),
      lastCoinProcessedTime(0),
      lastCoinState(HIGH) {
          
    // Force a read of the coin signal pin at startup to initialize correctly
    extern IoExpander ioExpander;
    uint8_t rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
    bool initialCoinSignal = !(rawPortValue0 & (1 << COIN_SIG));
    lastCoinState = initialCoinSignal ? LOW : HIGH;
    LOG_INFO("Coin detector initialized with state: %s", initialCoinSignal ? "ACTIVE" : "INACTIVE");
    
    // Check coin counter pin - optional hardware counter for coins
    bool counterSignal = !(rawPortValue0 & (1 << COIN_CNT));
    LOG_INFO("Coin counter initialized with state: %s", counterSignal ? "ACTIVE" : "INACTIVE");
    
    // IMPORTANT: Initialize these static variables to prevent false triggers at startup
    // We'll skip any coin signals that happen in the first 2 seconds after boot
    lastCoinProcessedTime = millis();
    lastCoinDebounceTime = millis();

    // Initialize LED pins - using built-in LED
    pinMode(LED_PIN_INIT, OUTPUT);
    digitalWrite(LED_PIN_INIT, LOW);

    // Initialize button debounce times - we'll use the IO expander to read buttons
    for (int i = 0; i < NUM_BUTTONS; i++) {
        lastDebounceTime[i] = 0;
        lastButtonState[i] = HIGH;  // Buttons are active low
    }

    // Set up for the stop button (BUTTON6)
    lastDebounceTime[NUM_BUTTONS-1] = 0;  // NUM_BUTTONS-1 is the index for the stop button
    lastButtonState[NUM_BUTTONS-1] = HIGH;

    config.isLoaded = false;
    config.physicalTokens = 0;
}

void CarWashController::handleMqttMessage(const char* topic, const uint8_t* payload, unsigned len) {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload, len);
    if (error) {
        LOG_ERROR("Failed to parse JSON");
        return;
    }
    if (String(topic) == INIT_TOPIC) {
        config.sessionId = doc["session_id"].as<String>();
        config.userId = doc["user_id"].as<String>();
        config.userName = doc["user_name"].as<String>();
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

    // Dump raw port values for debugging
    uint8_t rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
    
    // Print raw state for debugging
    
    // Read all 5 function buttons - using simple approach
    for (int i = 0; i < NUM_BUTTONS-1; i++) {  // -1 because last button is STOP button
        int buttonPin = BUTTON_INDICES[i];
        
        // Check if button is pressed (active LOW in the IO expander)
        bool buttonPressed = !(rawPortValue0 & (1 << buttonPin));
        
      
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
                    LOG_WARNING("Button press ignored - config not loaded!");
                }
            }
        } else {
            // Button is released
            if (lastButtonState[i] == LOW) {
                Serial.printf("Button %d RELEASED\n", i+1);
            }
            lastButtonState[i] = HIGH;  // Not pressed (idle HIGH)
        }
    }

    // Handle stop button (BUTTON6) - using the same approach as other buttons
    bool stopButtonPressed = !(rawPortValue0 & (1 << STOP_BUTTON_PIN));
    
    // Print debug for stop button
    LOG_DEBUG("STOP Button (pin %d) state: %s\n", 
                 STOP_BUTTON_PIN, stopButtonPressed ? "PRESSED" : "RELEASED");
    
    // Handle stop button press with debouncing
    if (stopButtonPressed) {
        // If button wasn't pressed before or enough time has passed
        if (lastButtonState[NUM_BUTTONS-1] == HIGH || 
            (millis() - lastDebounceTime[NUM_BUTTONS-1]) > DEBOUNCE_DELAY * 5) {
            
            // Record time of this press
            lastDebounceTime[NUM_BUTTONS-1] = millis();
            lastButtonState[NUM_BUTTONS-1] = LOW;  // Now pressed (active LOW)
            
            // Print debug info
            Serial.printf("** STOP BUTTON PRESSED AND DEBOUNCED **\n");
            LOG_INFO("STOP button pressed and debounced!");
            
            // Process button action
            if (currentState == STATE_RUNNING) {
                LOG_INFO("Stopping machine via STOP button");
                stopMachine(MANUAL);
            } else {
                LOG_WARNING("STOP button pressed but ignored - not in running state (state=%d)", currentState);
            }
        }
    } else {
        // Button is released
        if (lastButtonState[NUM_BUTTONS-1] == LOW) {
            Serial.printf("STOP Button RELEASED\n");
        }
        lastButtonState[NUM_BUTTONS-1] = HIGH;  // Not pressed (idle HIGH)
    }
}

void CarWashController::pauseMachine() {
    LOG_INFO("Pausing machine");
    if (activeButton >= 0) {
        // Read relay state before deactivation
        extern IoExpander ioExpander;
        uint8_t relayStateBefore = ioExpander.readRegister(OUTPUT_PORT1);
        LOG_DEBUG("Pausing - Relay port state before: 0x%02X", relayStateBefore);
        
        // Turn off the active relay
        ioExpander.setRelay(RELAY_INDICES[activeButton], false);
        
        // Verify relay state after deactivation
        uint8_t relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
        LOG_DEBUG("Pausing - Relay port state after: 0x%02X", relayStateAfter);
        
        // Check relay bit is actually cleared
        bool relayBitCleared = (relayStateAfter & (1 << RELAY_INDICES[activeButton])) == 0;
        if (relayBitCleared) {
            LOG_DEBUG("Relay %d successfully deactivated for pause", activeButton+1);
        } else {
            LOG_ERROR("Failed to deactivate relay %d for pause!", activeButton+1);
        }
    }
    currentState = STATE_PAUSED;
    lastActionTime = millis();
    pauseStartTime = millis();
    tokenTimeElapsed += (pauseStartTime - tokenStartTime);
   
    // Publish pause event
    publishActionEvent(activeButton, ACTION_PAUSE, MANUAL);
}

void CarWashController::resumeMachine(int buttonIndex) {
    LOG_INFO("Resuming machine with button %d", buttonIndex+1);
    activeButton = buttonIndex;
    
    // Turn on the corresponding relay when resuming
    extern IoExpander ioExpander;
    uint8_t relayStateBefore = ioExpander.readRegister(OUTPUT_PORT1);
    LOG_DEBUG("Resuming - Relay port state before: 0x%02X", relayStateBefore);
   
    // Turn on the relay for the active button
    ioExpander.setRelay(RELAY_INDICES[buttonIndex], true);
    
    // Verify relay state after activation
    uint8_t relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
    LOG_DEBUG("Resuming - Relay port state after: 0x%02X", relayStateAfter);
    
    // Check if relay bit was actually set
    bool relayBitSet = (relayStateAfter & (1 << RELAY_INDICES[buttonIndex])) != 0;
    if (relayBitSet) {
        LOG_DEBUG("Relay %d successfully activated for resume", buttonIndex+1);
    } else {
        LOG_ERROR("Failed to activate relay %d for resume!", buttonIndex+1);
    }
    
    currentState = STATE_RUNNING;
    lastActionTime = millis();
    tokenStartTime = millis();
    
    // Publish resume event
    publishActionEvent(buttonIndex, ACTION_RESUME, MANUAL);
}

void CarWashController::stopMachine(TriggerType triggerType) {
    if (activeButton >= 0) {
        // Add relay state debugging
        extern IoExpander ioExpander;
        uint8_t relayStateBefore = ioExpander.readRegister(OUTPUT_PORT1);
        LOG_DEBUG("Stopping - Relay port state before: 0x%02X", relayStateBefore);
        
        // Turn off the active relay
        ioExpander.setRelay(RELAY_INDICES[activeButton], false);
        
        // Verify relay state after deactivation
        uint8_t relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
        LOG_DEBUG("Stopping - Relay port state after: 0x%02X", relayStateAfter);
        
        // Check relay bit is actually cleared
        bool relayBitCleared = (relayStateAfter & (1 << RELAY_INDICES[activeButton])) == 0;
        if (relayBitCleared) {
            LOG_DEBUG("Relay %d successfully deactivated for stop", activeButton+1);
        } else {
            LOG_ERROR("Failed to deactivate relay %d for stop!", activeButton+1);
        }
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
    
    // Debug before turning on relay
    LOG_DEBUG("Attempting to activate relay %d (relay index: %d)", buttonIndex+1, RELAY_INDICES[buttonIndex]);
    
    // Read current relay state before activation
    extern IoExpander ioExpander;
    uint8_t relayStateBefore = ioExpander.readRegister(OUTPUT_PORT1);
    LOG_DEBUG("Relay port state before activation: 0x%02X", relayStateBefore);
    
    // Turn on the corresponding relay
    ioExpander.setRelay(RELAY_INDICES[buttonIndex], true);
    
    // Verify relay state after activation
    uint8_t relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
    LOG_DEBUG("Relay port state after activation: 0x%02X", relayStateAfter);
    
    // Check if relay bit was actually set
    bool relayBitSet = (relayStateAfter & (1 << RELAY_INDICES[buttonIndex])) != 0;
    if (relayBitSet) {
        LOG_DEBUG("Relay %d successfully activated (bit %d set)", buttonIndex+1, RELAY_INDICES[buttonIndex]);
    } else {
        LOG_ERROR("Failed to activate relay %d! Bit %d not set", buttonIndex+1, RELAY_INDICES[buttonIndex]);
    }
    
    lastActionTime = millis();
    tokenStartTime = millis();
    tokenTimeElapsed = 0;
    
    // Prioritize using physical tokens first
    if (config.physicalTokens > 0) {
        config.physicalTokens--;
    }
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
    // publishTokenExpiredEvent();
}

void CarWashController::handleCoinAcceptor() {
    // Get reference to the IO expander
    extern IoExpander ioExpander;

    // Get current time for all timing operations
    unsigned long currentTime = millis();
    
    // Skip startup period to avoid false triggers
    static bool startupPeriod = true;
    if (startupPeriod) {
        if (currentTime < 3000) { // Skip first 3 seconds
            return;
        }
        startupPeriod = false;
        LOG_INFO("Coin detector startup period over, now actively monitoring");
    }
    
    // Read raw port value
    uint8_t rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
    
    // Focus on SIGNAL pin only - from logs it's clear SIG pin is toggling with coins
    bool coinSignalActiveLow = !(rawPortValue0 & (1 << COIN_SIG));
    
    // *** SIMPLIFIED APPROACH BASED ON FINAL LOGS ***
    // Looking at your logs, we see a pattern when coins are inserted:
    // LOW -> HIGH quickly followed by HIGH -> LOW
    
    // Static variables to track transitions
    static unsigned long lastTransitionTime = 0;
    static unsigned long lastHighToLowTime = 0;
    
    // Detect signal changes
    if (coinSignalActiveLow != lastCoinState) {
        // Log all transitions
        LOG_INFO("Coin signal changed: %s -> %s", 
                lastCoinState ? "LOW" : "HIGH", 
                coinSignalActiveLow ? "LOW" : "HIGH");
        
        unsigned long timeSinceLastTransition = currentTime - lastTransitionTime;
        lastTransitionTime = currentTime;
        
        // FALLING EDGE (HIGH to LOW) after a RISING EDGE (LOW to HIGH)
        // This seems to be the pattern when a coin is inserted
        if (coinSignalActiveLow && !lastCoinState) {
            // This is a HIGH to LOW transition
            
            // Check if it happened quickly after a LOW to HIGH transition
            // (which would indicate a coin insertion)
            unsigned long timeSinceLastLowToHigh = currentTime - lastTransitionTime + timeSinceLastTransition;
            
            if (timeSinceLastTransition < 200) {  // Less than 200ms between transitions
                LOG_INFO("Quick transition pattern detected - likely a coin!");
                
                // Check if this could be a unique coin insertion (not just signal bounce)
                if (currentTime - lastCoinProcessedTime > COIN_PROCESS_COOLDOWN) {
                    LOG_INFO("Processing coin insertion");
                    processCoinInsertion(currentTime);
                } else {
                    LOG_INFO("Skipping coin - too soon after last (%lu ms < %lu ms)",
                           currentTime - lastCoinProcessedTime, COIN_PROCESS_COOLDOWN);
                }
            }
            
            lastHighToLowTime = currentTime;
        }
        
        // Update state tracking
        lastCoinState = coinSignalActiveLow;
    }
    
    // Additional method: if no transitions are detected for some time, 
    // but we observe changes every ~500-1000ms when coins are inserted,
    // add a simple timer-based detection as a fallback
    static unsigned long lastCoinCheckTime = 0;
    if (currentTime - lastCoinCheckTime > 250) {  // Check every 250ms
        lastCoinCheckTime = currentTime;
        
        // If no coin processed recently, check for activity
        if (currentTime - lastCoinProcessedTime > COIN_PROCESS_COOLDOWN) {
            // Count transitions in the last second
            static int recentTransitions = 0;
            static unsigned long transitionWindowStart = 0;
            
            // Reset counter periodically
            if (currentTime - transitionWindowStart > 1000) {
                transitionWindowStart = currentTime;
                
                // If we saw 2-4 transitions in the last second, that's likely a coin
                if (recentTransitions >= 2 && recentTransitions <= 6) {
                    LOG_INFO("Detected %d transitions in 1 second - likely a coin!", recentTransitions);
                    
                    if (currentTime - lastCoinProcessedTime > COIN_PROCESS_COOLDOWN) {
                        LOG_INFO("Processing coin based on transition frequency");
                        processCoinInsertion(currentTime);
                    }
                }
                
                recentTransitions = 0;
            }
            
            // Count this transition if one occurred
            if (currentTime - lastTransitionTime < 250) {
                recentTransitions++;
            }
        }
    }
    
    // Periodic debug info
    static unsigned long lastDebugTime = 0;
    if (currentTime - lastDebugTime > 5000) { // Every 5 seconds to reduce noise
        lastDebugTime = currentTime;
        LOG_DEBUG("Coin state: SIG=%s, LastProcessed=%lums ago", 
                coinSignalActiveLow ? "LOW" : "HIGH",
                currentTime - lastCoinProcessedTime);
    }
}

// Helper method to handle the business logic of a coin insertion
void CarWashController::processCoinInsertion(unsigned long currentTime) {
    LOG_INFO("Coin detected!");
    
    // Update activity tracking - crucial for debouncing and cooldown periods
    lastActionTime = currentTime;
    lastCoinProcessedTime = currentTime; // This is critical for the cooldown between coins
    
    // Update or create session
    if (config.isLoaded) {
        LOG_INFO("Adding physical token to existing session");
        config.physicalTokens++;
        config.tokens++;
    } else {
        LOG_INFO("Creating new manual session from coin insertion");
        
        char sessionIdBuffer[30];
        sprintf(sessionIdBuffer, "manual_%lu", currentTime);
        
        config.sessionId = String(sessionIdBuffer);
        config.userId = "unknown";
        config.userName = "";
        config.physicalTokens = 1;
        config.tokens = 1;
        config.isLoaded = true;
        
        currentState = STATE_IDLE;
        digitalWrite(LED_PIN_INIT, HIGH);
    }
    
    publishCoinInsertedEvent();
}

void CarWashController::update() {
    // Serial.println("CarWashController::update");
    if (config.timestamp == "") {
        return;
    }
    
    // Handle buttons and coin acceptor
    handleButtons();
    handleCoinAcceptor();
    
    unsigned long currentTime = millis();
    publishPeriodicState();
    
    if (currentState == STATE_RUNNING) {
        unsigned long totalElapsedTime = tokenTimeElapsed + (currentTime - tokenStartTime);
        if (totalElapsedTime >= TOKEN_TIME) {
            LOG_INFO("Token expired");
            tokenExpired();
            return;
        }
    }
    
    if (currentTime - lastActionTime > USER_INACTIVE_TIMEOUT && currentState != STATE_FREE) {
        LOG_INFO("Stopping machine due to user inactivity");
        stopMachine(AUTOMATIC);
    }
}

void CarWashController::publishMachineSetupActionEvent() {
    LOG_INFO("Publishing Machine Setup Action Event");

    StaticJsonDocument<512> doc;
    doc["machine_id"] = MACHINE_ID;
    doc["action"] = getMachineActionString(ACTION_SETUP);
    doc["timestamp"] = getTimestamp();
    
    String jsonString;
    serializeJson(doc, jsonString);
    mqttClient.publish(ACTION_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE);
}

unsigned long CarWashController::getSecondsLeft() {
    if (currentState != STATE_RUNNING && currentState != STATE_PAUSED) {
        return 0;
    }

    unsigned long currentTime = millis();
    unsigned long totalElapsedTime;

    if (currentState == STATE_RUNNING) {
        totalElapsedTime = tokenTimeElapsed + (currentTime - tokenStartTime);
    } else { // PAUSED
        totalElapsedTime = tokenTimeElapsed;
    }

    if (totalElapsedTime >= TOKEN_TIME) {
        return 0;
    }

    return (TOKEN_TIME - totalElapsedTime) / 1000;
}

String CarWashController::getTimestamp() {
    // First, let's add some debug logging
    LOG_DEBUG("Raw timestamp: %s", config.timestamp.c_str());
    LOG_DEBUG("Timestamp millis: %lu", config.timestampMillis);
    LOG_DEBUG("Current millis: %lu", millis());
    
    // If timestamp is empty, return early
    if (config.timestamp.length() == 0) {
        return "No timestamp";
    }
    
    // Manual string parsing for reliability
    String ts = config.timestamp;
    int tPos = ts.indexOf('T');
    int dotPos = ts.indexOf('.');
    int plusPos = ts.indexOf('+');
    
    // Verify we have the expected format
    if (tPos <= 0 || dotPos <= 0 || plusPos <= 0) {
        LOG_ERROR("Timestamp format invalid");
        return "Invalid format";
    }
    
    // Extract date components
    int year = ts.substring(0, 4).toInt();
    int month = ts.substring(5, 7).toInt();
    int day = ts.substring(8, 10).toInt();
    
    // Extract time components
    int hour = ts.substring(tPos+1, tPos+3).toInt();
    int minute = ts.substring(tPos+4, tPos+6).toInt();
    int second = ts.substring(tPos+7, tPos+9).toInt();
    
    // Extract microseconds
    long microseconds = 0;
    if (dotPos > 0 && plusPos > dotPos) {
        microseconds = ts.substring(dotPos+1, plusPos).toInt();
    }
    
    // Debug the parsed components
    LOG_DEBUG("Parsed: %d-%02d-%02d %02d:%02d:%02d.%06ld", 
              year, month, day, hour, minute, second, microseconds);
    
    // Set up the time structure
    tmElements_t tm;
    tm.Year = year - 1970;
    tm.Month = month;
    tm.Day = day;
    tm.Hour = hour;
    tm.Minute = minute;
    tm.Second = second;
    
    // Convert to epoch time
    time_t serverEpoch = makeTime(tm);
    LOG_DEBUG("Server epoch: %lu", (unsigned long)serverEpoch);
    
    // Calculate time elapsed since timestamp was set
    unsigned long millisOffset = 0;
    if (config.timestampMillis > 0) {
        millisOffset = millis() - config.timestampMillis;
    }
    LOG_DEBUG("Millis offset: %lu", millisOffset);
    
    // Adjust the time with the elapsed milliseconds
    time_t adjustedTime = serverEpoch + (millisOffset / 1000);
    int milliseconds = (microseconds / 1000) + (millisOffset % 1000);
    while (milliseconds >= 1000) {
        adjustedTime += 1;
        milliseconds -= 1000;
    }
    
    // Convert back to time components
    tmElements_t adjustedTm;
    breakTime(adjustedTime, adjustedTm);
    
    // Format the result
    char isoTimestamp[40];
    sprintf(isoTimestamp, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
            adjustedTm.Year + 1970, adjustedTm.Month, adjustedTm.Day,
            adjustedTm.Hour, adjustedTm.Minute, adjustedTm.Second, milliseconds);
    
    LOG_DEBUG("Formatted timestamp: %s", isoTimestamp);
    
    return String(isoTimestamp);
}

void CarWashController::publishCoinInsertedEvent() {
    if (!config.isLoaded) return;

    StaticJsonDocument<512> doc;

    doc["machine_id"] = MACHINE_ID;
    doc["timestamp"] = getTimestamp();
    doc["action"] = getMachineActionString(ACTION_TOKEN_INSERTED);
    doc["trigger_type"] = "MANUAL";
    doc["session_id"] = config.sessionId;
    doc["user_id"] = config.userId;
    doc["token_channel"] = "PHYSICAL";
    doc["tokens_left"] = config.tokens;
    doc["physical_tokens"] = config.physicalTokens;

    String jsonString;
    serializeJson(doc, jsonString);

    mqttClient.publish(ACTION_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE);
}

// Debug method to directly simulate a coin insertion
void CarWashController::simulateCoinInsertion() {
    LOG_INFO("Simulating coin insertion (DEBUG METHOD)");
    processCoinInsertion(millis());
}

void CarWashController::publishActionEvent(int buttonIndex, MachineAction machineAction, TriggerType triggerType) {
    if (!config.isLoaded) return;

    StaticJsonDocument<512> doc;

    doc["machine_id"] = MACHINE_ID;
    doc["timestamp"] = getTimestamp();
    doc["action"] = getMachineActionString(machineAction);
    doc["trigger_type"] = (triggerType == MANUAL) ? "MANUAL" : "AUTOMATIC";
    doc["button_name"] = String("BUTTON_") + String(buttonIndex + 1);
    doc["session_id"] = config.sessionId;
    doc["user_id"] = config.userId;
    doc["token_channel"] = (config.physicalTokens > 0) ? "PHYSICAL" : "DIGITAL";
    doc["tokens_left"] = config.tokens;
    doc["physical_tokens"] = config.physicalTokens;

    if (currentState == STATE_RUNNING) {
        doc["seconds_left"] = getSecondsLeft();
    }

    String jsonString;
    serializeJson(doc, jsonString);

    mqttClient.publish(ACTION_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE);
}

void CarWashController::publishPeriodicState(bool force) {
    if (force || millis() - lastStatePublishTime >= STATE_PUBLISH_INTERVAL) {
        LOG_INFO("Publishing Periodic State");
        StaticJsonDocument<512> doc;
        doc["machine_id"] = MACHINE_ID;
        doc["timestamp"] = getTimestamp();
        doc["status"] = getMachineStateString(currentState);
        LOG_DEBUG("Status: %s", doc["status"].as<String>().c_str());

        if (config.isLoaded) {
            JsonObject sessionMetadata = doc.createNestedObject("session_metadata");
            sessionMetadata["session_id"] = config.sessionId;
            sessionMetadata["user_id"] = config.userId;
            sessionMetadata["user_name"] = config.userName;
            sessionMetadata["tokens_left"] = config.tokens;
            sessionMetadata["physical_tokens"] = config.physicalTokens;
            sessionMetadata["timestamp"] = config.timestamp;
            if (tokenStartTime > 0) {
                sessionMetadata["seconds_left"] = getSecondsLeft();
            }
        }

        String jsonString;
        serializeJson(doc, jsonString);
        mqttClient.publish(STATE_TOPIC.c_str(), jsonString.c_str(), QOS0_AT_MOST_ONCE);

        lastStatePublishTime = millis();
    }
}

// Add getter methods for controller state access
MachineState CarWashController::getCurrentState() const {
    return currentState;
}

bool CarWashController::isMachineLoaded() const {
    return config.isLoaded;
}

void CarWashController::setLogLevel(LogLevel level) {
    LOG_INFO("Changing log level from %s to %s", 
           Logger::getLogLevel() == LOG_DEBUG ? "DEBUG" :
           Logger::getLogLevel() == LOG_INFO ? "INFO" :
           Logger::getLogLevel() == LOG_WARNING ? "WARNING" :
           Logger::getLogLevel() == LOG_ERROR ? "ERROR" : "NONE",
           level == LOG_DEBUG ? "DEBUG" :
           level == LOG_INFO ? "INFO" :
           level == LOG_WARNING ? "WARNING" :
           level == LOG_ERROR ? "ERROR" : "NONE");
    
    Logger::setLogLevel(level);
}

unsigned long CarWashController::getTimeToInactivityTimeout() const {
    if (currentState == STATE_FREE || !config.isLoaded) {
        return 0;
    }
    
    unsigned long elapsedTime = millis() - lastActionTime;
    if (elapsedTime >= USER_INACTIVE_TIMEOUT) {
        return 0;
    }
    
    return USER_INACTIVE_TIMEOUT - elapsedTime;
}

// getTokensLeft and getUserName are implemented as inline methods in the header
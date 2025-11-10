#include "car_wash_controller.h"
#include "io_expander.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// External mutex for ioExpander access (defined in main.cpp)
extern SemaphoreHandle_t xIoExpanderMutex;

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
      lastCoinState(HIGH),
      lastPauseResumeTime(0),
      lastFunctionSwitchTime(0),
      gracePeriodStartTime(0),
      gracePeriodActive(false),
      tokensConsumedCount(0) {
          
    // Force a read of the coin signal pin at startup to initialize correctly
    extern IoExpander ioExpander;
    uint8_t rawPortValue0 = 0;
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
        xSemaphoreGive(xIoExpanderMutex);
    } else {
        LOG_ERROR("COIN INIT: Failed to acquire mutex for initial coin state read!");
    }
    
    // EXTENSIVE DEBUG: Log the raw values in different formats
    LOG_INFO("=== COIN DETECTOR INITIALIZATION ===");
    LOG_INFO("COIN INIT: Raw port value: 0x%02X | Binary: %d%d%d%d%d%d%d%d", 
           rawPortValue0,
           (rawPortValue0 & 0x80) ? 1 : 0, (rawPortValue0 & 0x40) ? 1 : 0,
           (rawPortValue0 & 0x20) ? 1 : 0, (rawPortValue0 & 0x10) ? 1 : 0,
           (rawPortValue0 & 0x08) ? 1 : 0, (rawPortValue0 & 0x04) ? 1 : 0,
           (rawPortValue0 & 0x02) ? 1 : 0, (rawPortValue0 & 0x01) ? 1 : 0);
    LOG_INFO("COIN INIT: COIN_SIG (bit %d) raw bit value = %d", COIN_SIG, (rawPortValue0 & (1 << COIN_SIG)) ? 1 : 0);
    
    // Initialize coin signal state correctly
    // When coin is present: Pin is LOW (bit=0) = ACTIVE
    // When no coin: Pin is HIGH (bit=1) = INACTIVE
    bool initialCoinSignal = ((rawPortValue0 & (1 << COIN_SIG)) == 0); // LOW = coin present = ACTIVE
    // CRITICAL FIX: lastCoinState must match the actual initial signal state
    // If signal is ACTIVE (LOW), lastCoinState should be LOW
    // If signal is INACTIVE (HIGH), lastCoinState should be HIGH
    lastCoinState = initialCoinSignal ? LOW : HIGH; // Store actual state to prevent false edge detection
    
    LOG_INFO("COIN INIT: initialCoinSignal (active when LOW) = %s", initialCoinSignal ? "ACTIVE (LOW/0)" : "INACTIVE (HIGH/1)");
    LOG_INFO("COIN INIT: lastCoinState initialized to = %s (for edge detection)", lastCoinState == LOW ? "LOW" : "HIGH");
    LOG_INFO("COIN INIT: COIN_PROCESS_COOLDOWN = %lu ms", COIN_PROCESS_COOLDOWN);
    LOG_INFO("COIN INIT: COIN_EDGE_WINDOW = %lu ms", COIN_EDGE_WINDOW);
    LOG_INFO("COIN INIT: COIN_MIN_EDGES = %d", COIN_MIN_EDGES);
    
    // IMPORTANT: Initialize these static variables to prevent false triggers at startup
    // We'll skip any coin signals that happen in the first 2 seconds after boot
    unsigned long initTime = millis();
    lastCoinProcessedTime = initTime;
    lastCoinDebounceTime = initTime;
    LOG_INFO("COIN INIT: Timers initialized at %lu ms (will ignore coins for first 3 seconds)", initTime);
    LOG_INFO("=== END COIN DETECTOR INITIALIZATION ===");

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
    // Handle get_state topic first (doesn't require JSON parsing)
    if (String(topic) == GET_STATE_TOPIC) {
        LOG_INFO("Received get_state request, publishing state on demand");
        // Publish state on demand with high priority when get_state message is received
        // publishStateOnDemand();
        return;
    }
    
    // Other topics require JSON parsing
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
        config.physicalTokens = 0;
        
        // Extract timestamp from INIT_TOPIC message if present
        if (doc.containsKey("timestamp") && doc["timestamp"].as<String>().length() > 0) {
            config.timestamp = doc["timestamp"].as<String>();
            LOG_INFO("Timestamp from INIT_TOPIC: %s", config.timestamp.c_str());
        } else {
            config.timestamp = ""; // No timestamp available
            LOG_WARNING("No timestamp available in INIT_TOPIC");
        }
        
        config.isLoaded = true;
        currentState = STATE_IDLE;
        lastActionTime = millis();
        gracePeriodStartTime = millis(); // Start 30-second grace period
        gracePeriodActive = true;
        // Reset token timing variables to ensure clean state
        tokenStartTime = 0;
        tokenTimeElapsed = 0;
        pauseStartTime = 0;
        activeButton = -1;
        tokensConsumedCount = 0; // Reset consumed token counter
        LOG_INFO("Machine loaded - 30-second grace period started");
        LOG_INFO("Switching on LED");
        digitalWrite(LED_PIN_INIT, HIGH);
        LOG_INFO("Machine loaded with new configuration");
        
        // CRITICAL: Publish state immediately after loading with high priority (QOS1)
        // This ensures the backend receives the IDLE state quickly so the app can detect it
        // publishStateOnDemand();
    } else if (String(topic) == CONFIG_TOPIC) {
        LOG_INFO("Received config message from server");
        config.timestamp = doc["timestamp"].as<String>();
        
        // Note: Config no longer clears session data
        // Session is only cleared on STOP action or timeout
    } else {
        LOG_WARNING("Unknown topic: %s", topic);
    }
}

void CarWashController::handleButtons() {
    // Get reference to the IO expander (assumed to be a global or accessible)
    extern IoExpander ioExpander;

    // Fast path: consume button flags set by the ButtonDetector task
    // This ensures short presses (that may be missed by raw polling) are handled
    if (ioExpander.isButtonDetected()) {
        uint8_t detectedId = ioExpander.getDetectedButtonId();
        bool buttonProcessed = false;

        LOG_INFO("Button flag detected: button %d, currentState=%d, activeButton=%d, isLoaded=%d, timestamp='%s'", 
                detectedId + 1, currentState, activeButton, config.isLoaded, 
                config.timestamp.length() > 0 ? config.timestamp.c_str() : "(empty)");

        ioExpander.clearButtonFlag();

        // Explicit check: Buttons should not work when machine is FREE
        // This ensures buttons are ignored even if config.isLoaded is somehow true
        if (currentState == STATE_FREE) {
            LOG_WARNING("Button %d press ignored - machine is FREE (config.isLoaded=%d)", 
                       detectedId + 1, config.isLoaded);
            return; // Skip processing and raw polling
        }

        // Function buttons (0..NUM_BUTTONS-2)
        // Button 5 = index 4, NUM_BUTTONS = 6, so NUM_BUTTONS - 1 = 5
        // So detectedId < 5 means buttons 0-4 (buttons 1-5)
        if (detectedId < NUM_BUTTONS - 1) {
            LOG_INFO("Processing function button %d (detectedId=%d, NUM_BUTTONS-1=%d)", 
                    detectedId + 1, detectedId, NUM_BUTTONS - 1);
            if (config.isLoaded) {
                if (currentState == STATE_IDLE) {
                    LOG_INFO("Activating button %d from IDLE state", detectedId + 1);
                    activateButton(detectedId, MANUAL);
                    buttonProcessed = true;
                } else if (currentState == STATE_RUNNING) {
                    // Same button pause the machine, different button switches function
                    LOG_INFO("Button %d pressed while RUNNING (activeButton=%d)", 
                            detectedId + 1, activeButton + 1);
                    if (activeButton == -1 || (int)detectedId == activeButton) {
                        // Same button pressed - pause the machine
                        unsigned long currentTime = millis();
                        
                        // CRITICAL FIX: Check if this is the same button press that just activated the machine
                        // If tokenStartTime was set very recently (within 200ms), this is likely the same press
                        // that activated from IDLE, so we should ignore it to prevent immediate pause
                        if (tokenStartTime != 0) {
                            unsigned long timeSinceActivation;
                            if (currentTime >= tokenStartTime) {
                                timeSinceActivation = currentTime - tokenStartTime;
                            } else {
                                timeSinceActivation = (0xFFFFFFFFUL - tokenStartTime) + currentTime + 1;
                            }
                            
                            // If activation happened very recently (within 200ms), ignore this pause request
                            // This prevents the flag handler from stopping immediately after raw polling activated
                            if (timeSinceActivation < 200) {
                                LOG_INFO("Button %d pressed while RUNNING - ignoring (just activated %lu ms ago, likely same press)", 
                                       detectedId + 1, timeSinceActivation);
                                // Still reset inactivity timeout
                                lastActionTime = currentTime;
                                buttonProcessed = true;
                                return; // Skip raw polling
                            }
                        }
                        
                        // CRITICAL FIX: Check if we just switched to this button - if so, ignore pause request
                        // This prevents the same button press that triggered the switch from also triggering a pause
                        unsigned long timeSinceFunctionSwitch;
                        if (currentTime >= lastFunctionSwitchTime) {
                            timeSinceFunctionSwitch = currentTime - lastFunctionSwitchTime;
                        } else {
                            timeSinceFunctionSwitch = (0xFFFFFFFFUL - lastFunctionSwitchTime) + currentTime + 1;
                        }
                        
                        if (timeSinceFunctionSwitch < FUNCTION_SWITCH_COOLDOWN) {
                            LOG_INFO("Button %d pressed while RUNNING - ignoring (just switched to this button %lu ms ago, likely same press)", 
                                   detectedId + 1, timeSinceFunctionSwitch);
                            // Still reset inactivity timeout
                            lastActionTime = currentTime;
                            buttonProcessed = true;
                            return; // Skip raw polling
                        }
                        
                        // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                        lastActionTime = currentTime;
                        
                        if (activeButton == -1) {
                            LOG_WARNING("activeButton is -1 in RUNNING state - setting to pressed button %d", detectedId + 1);
                            // Set activeButton to the pressed button to fix the tracking
                            activeButton = detectedId;
                        }
                        LOG_INFO("Pausing machine - same button pressed while running");
                        pauseMachine();
                        buttonProcessed = true;
                    } else {
                        // Different button pressed - switch to new function (keep running, switch relay)
                        lastActionTime = millis();
                        LOG_INFO("Button %d pressed while RUNNING (activeButton=%d) - switching function", 
                                   detectedId + 1, activeButton + 1);
                        switchFunction(detectedId);
                        buttonProcessed = true;
                    }
                } else if (currentState == STATE_PAUSED) {
                    // Same button resumes, different button switches function and resumes
                    unsigned long currentTime = millis();
                    
                    // CRITICAL FIX: Prevent rapid pause/resume toggling
                    unsigned long timeSinceLastPauseResume;
                    if (currentTime >= lastPauseResumeTime) {
                        timeSinceLastPauseResume = currentTime - lastPauseResumeTime;
                    } else {
                        timeSinceLastPauseResume = (0xFFFFFFFFUL - lastPauseResumeTime) + currentTime + 1;
                    }
                    
                    // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                    lastActionTime = currentTime;
                    
                    if (timeSinceLastPauseResume < PAUSE_RESUME_COOLDOWN) {
                        LOG_WARNING("Button %d pressed while PAUSED - ignoring (cooldown: %lu ms < %lu ms)", 
                                   detectedId + 1, timeSinceLastPauseResume, PAUSE_RESUME_COOLDOWN);
                    } else {
                        if (activeButton == -1 || (int)detectedId == activeButton) {
                            // Same button (or no active button) - resume with same button
                            if (activeButton == -1) {
                                LOG_WARNING("activeButton is -1 in PAUSED state - allowing resume anyway (button %d)", detectedId + 1);
                                // Set activeButton to the pressed button to fix the tracking
                                activeButton = detectedId;
                            }
                            LOG_INFO("Button %d: Resuming from PAUSED state (same button)", detectedId + 1);
                            resumeMachine(detectedId);
                            lastPauseResumeTime = currentTime;
                            buttonProcessed = true;
                        } else {
                            // Different button pressed - switch function and resume
                            LOG_INFO("Button %d pressed while PAUSED (activeButton=%d) - switching function and resuming", 
                                   detectedId + 1, activeButton + 1);
                            // First deactivate the old relay if there was one
                            if (activeButton >= 0) {
                                extern IoExpander ioExpander;
                                if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                                    ioExpander.setRelay(RELAY_INDICES[activeButton], false);
                                    LOG_INFO("Deactivated relay %d (button %d)", activeButton + 1, activeButton + 1);
                                    xSemaphoreGive(xIoExpanderMutex);
                                }
                            }
                            // Now resume with the new button
                            resumeMachine(detectedId);
                            lastPauseResumeTime = currentTime;
                            lastFunctionSwitchTime = currentTime;
                            buttonProcessed = true;
                        }
                    }
                } else {
                    // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                    if (config.isLoaded && currentState != STATE_FREE) {
                        lastActionTime = millis();
                    }
                    LOG_WARNING("Flag press on button %d ignored. State=%d, activeButton=%d",
                                detectedId + 1, currentState, activeButton);
                    // Flag already cleared above - no need to process
                }
            } else {
                LOG_WARNING("Button %d press ignored - config not loaded", detectedId + 1);
                // Flag already cleared above - no delayed processing
            }
        } else if (detectedId == NUM_BUTTONS - 1) {
            // Stop button
            // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
            lastActionTime = millis();
            if (config.isLoaded) {
                stopMachine(MANUAL);
                buttonProcessed = true;
            } else {
                LOG_WARNING("STOP button press ignored - config not loaded");
                // Flag already cleared above - no delayed processing
            }
        }

        // We've handled a flagged press; skip raw polling this cycle
        return;
    }

    // Protect IO expander access with mutex
    uint8_t rawPortValue0 = 0;
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
        xSemaphoreGive(xIoExpanderMutex);
    } else {
        LOG_WARNING("Failed to acquire IO expander mutex in handleButtons()");
        return; // Skip button handling if mutex not available
    }
    
    // Print raw state for debugging
    
    // Read all 5 function buttons - using simple approach
    for (int i = 0; i < NUM_BUTTONS-1; i++) {  // -1 because last button is STOP button
        int buttonPin = BUTTON_INDICES[i];
        
        // Check if button is pressed (active LOW in the IO expander)
        bool buttonPressed = !(rawPortValue0 & (1 << buttonPin));
        
      
        // Handle button press with debouncing
        if (buttonPressed) {
            // If button wasn't pressed before or enough time has passed since last action
            unsigned long timeSinceLastDebounce = millis() - lastDebounceTime[i];
            bool wasReleased = (lastButtonState[i] == HIGH);
            if (wasReleased || timeSinceLastDebounce > DEBOUNCE_DELAY * 5) {
                
                // Record time of this press
                lastDebounceTime[i] = millis();
                lastButtonState[i] = LOW;  // Now pressed (active LOW)
                
                LOG_INFO("Button %d raw polling: pressed (state transition: %s, time since last: %lu ms)", 
                        i + 1, wasReleased ? "HIGH->LOW" : "repeat press", timeSinceLastDebounce);
                
                // Explicit check: Buttons should not work when machine is FREE
                if (currentState == STATE_FREE) {
                    LOG_DEBUG("Button %d ignored - machine is FREE", i + 1);
                    continue; // Skip this button, check others
                }
                
                // Process button action
                if (config.isLoaded) {
                    if (currentState == STATE_IDLE) {
                        LOG_INFO("Button %d: Activating from IDLE state", i + 1);
                        activateButton(i, MANUAL);
                    } else if (currentState == STATE_RUNNING) {
                        // Same button stops machine, different button switches function
                        LOG_INFO("Button %d pressed while RUNNING (activeButton=%d) - raw polling", 
                                i + 1, activeButton + 1);
                        if (activeButton == -1 || i == activeButton) {
                            // Same button pressed - stop the machine
                            unsigned long currentTime = millis();
                            
                            // CRITICAL FIX: Check if this is the same button press that just activated the machine
                            // If tokenStartTime was set very recently (within 200ms), this is likely the same press
                            // that activated from IDLE, so we should ignore it to prevent immediate stop
                            if (tokenStartTime != 0) {
                                unsigned long timeSinceActivation;
                                if (currentTime >= tokenStartTime) {
                                    timeSinceActivation = currentTime - tokenStartTime;
                                } else {
                                    timeSinceActivation = (0xFFFFFFFFUL - tokenStartTime) + currentTime + 1;
                                }
                                
                                // If activation happened very recently (within 200ms), ignore this stop request
                                // This prevents double-processing of the same button press
                                if (timeSinceActivation < 200) {
                                    LOG_INFO("Button %d pressed while RUNNING - ignoring (just activated %lu ms ago, likely same press) - raw polling", 
                                           i + 1, timeSinceActivation);
                                    // Still reset inactivity timeout
                                    lastActionTime = currentTime;
                                    continue; // Skip to next button
                                }
                            }
                            
                            // CRITICAL FIX: Check if we just switched to this button - if so, ignore pause request
                            // This prevents the same button press that triggered the switch from also triggering a pause
                            unsigned long timeSinceFunctionSwitch;
                            if (currentTime >= lastFunctionSwitchTime) {
                                timeSinceFunctionSwitch = currentTime - lastFunctionSwitchTime;
                            } else {
                                timeSinceFunctionSwitch = (0xFFFFFFFFUL - lastFunctionSwitchTime) + currentTime + 1;
                            }
                            
                            if (timeSinceFunctionSwitch < FUNCTION_SWITCH_COOLDOWN) {
                                LOG_INFO("Button %d pressed while RUNNING - ignoring (just switched to this button %lu ms ago, likely same press) - raw polling", 
                                       i + 1, timeSinceFunctionSwitch);
                                // Still reset inactivity timeout
                                lastActionTime = currentTime;
                                continue; // Skip to next button
                            }
                            
                            // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                            lastActionTime = currentTime;
                            
                            if (activeButton == -1) {
                                LOG_WARNING("activeButton is -1 in RUNNING state - setting to pressed button %d - raw polling", i + 1);
                                // Set activeButton to the pressed button to fix the tracking
                                activeButton = i;
                            }
                            LOG_INFO("Pausing machine - same button pressed while running (raw polling)");
                            pauseMachine();
                        } else {
                            // Different button pressed - switch to new function (keep running, switch relay)
                            lastActionTime = millis();
                            LOG_INFO("Button %d pressed while RUNNING (activeButton=%d) - switching function (raw polling)", 
                                       i + 1, activeButton + 1);
                            switchFunction(i);
                        }
                    } else if (currentState == STATE_PAUSED) {
                        // Same button resumes, different button switches function and resumes
                        unsigned long currentTime = millis();
                        
                        // CRITICAL FIX: Prevent rapid pause/resume toggling
                        unsigned long timeSinceLastPauseResume;
                        if (currentTime >= lastPauseResumeTime) {
                            timeSinceLastPauseResume = currentTime - lastPauseResumeTime;
                        } else {
                            timeSinceLastPauseResume = (0xFFFFFFFFUL - lastPauseResumeTime) + currentTime + 1;
                        }
                        
                        // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                        lastActionTime = currentTime;
                        
                        if (timeSinceLastPauseResume < PAUSE_RESUME_COOLDOWN) {
                            LOG_WARNING("Button %d pressed while PAUSED - ignoring (cooldown: %lu ms < %lu ms) - raw polling", 
                                       i + 1, timeSinceLastPauseResume, PAUSE_RESUME_COOLDOWN);
                        } else {
                            if (activeButton == -1 || i == activeButton) {
                                // Same button (or no active button) - resume with same button
                                if (activeButton == -1) {
                                    LOG_WARNING("activeButton is -1 in PAUSED state - allowing resume anyway (button %d) - raw polling", i + 1);
                                    // Set activeButton to the pressed button to fix the tracking
                                    activeButton = i;
                                }
                                LOG_INFO("Button %d: Resuming from PAUSED state (same button) - raw polling", i + 1);
                                resumeMachine(i);
                                lastPauseResumeTime = currentTime;
                            } else {
                                // Different button pressed - switch function and resume
                                LOG_INFO("Button %d pressed while PAUSED (activeButton=%d) - switching function and resuming (raw polling)", 
                                       i + 1, activeButton + 1);
                                // First deactivate the old relay if there was one
                                extern IoExpander ioExpander;
                                if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                                    ioExpander.setRelay(RELAY_INDICES[activeButton], false);
                                    LOG_INFO("Deactivated relay %d (button %d)", activeButton + 1, activeButton + 1);
                                    xSemaphoreGive(xIoExpanderMutex);
                                }
                                // Now resume with the new button
                                resumeMachine(i);
                                lastPauseResumeTime = currentTime;
                                lastFunctionSwitchTime = currentTime;
                            }
                        }
                    }
                } else {
                    LOG_WARNING("Button %d ignored - config not loaded", i + 1);
                }
            } else {
                LOG_DEBUG("Button %d raw polling: pressed but debounced (time since last: %lu ms, need %lu ms)", 
                         i + 1, timeSinceLastDebounce, DEBOUNCE_DELAY * 5);
            }
        } else {
            // Button is released
            if (lastButtonState[i] == LOW) {
                LOG_DEBUG("Button %d raw polling: released (LOW->HIGH)", i + 1);
            }
            lastButtonState[i] = HIGH;  // Not pressed (idle HIGH)
        }
    }

    // Handle stop button (BUTTON6) - using the same approach as other buttons
    bool stopButtonPressed = !(rawPortValue0 & (1 << STOP_BUTTON_PIN));
    
    // Print debug for stop button
    // LOG_DEBUG("STOP Button (pin %d) state: %s\n", 
    //              STOP_BUTTON_PIN, stopButtonPressed ? "PRESSED" : "RELEASED");
    
    // Handle stop button press with debouncing
    if (stopButtonPressed) {
        // If button wasn't pressed before or enough time has passed
        if (lastButtonState[NUM_BUTTONS-1] == HIGH || 
            (millis() - lastDebounceTime[NUM_BUTTONS-1]) > DEBOUNCE_DELAY * 5) {
            
            // Record time of this press
            lastDebounceTime[NUM_BUTTONS-1] = millis();
            lastButtonState[NUM_BUTTONS-1] = LOW;  // Now pressed (active LOW)
            
            // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
            lastActionTime = millis();
            
            // Process button action - stop button should log out user whenever machine is loaded
            if (config.isLoaded) {
                stopMachine(MANUAL);
            }
        }
    } else {
        // Button is released
        lastButtonState[NUM_BUTTONS-1] = HIGH;  // Not pressed (idle HIGH)
    }
}

void CarWashController::pauseMachine() {
    if (activeButton >= 0) {
        extern IoExpander ioExpander;
        
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Turn off the active relay
            ioExpander.setRelay(RELAY_INDICES[activeButton], false);
            
            // Verify relay state after deactivation
            uint8_t relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
            xSemaphoreGive(xIoExpanderMutex);
            
            // Check relay bit is actually cleared
            bool relayBitCleared = (relayStateAfter & (1 << RELAY_INDICES[activeButton])) == 0;
            if (!relayBitCleared) {
                LOG_ERROR("Failed to deactivate relay %d for pause!", activeButton+1);
            }
        } else {
            LOG_WARNING("Failed to acquire IO expander mutex in pauseMachine()");
            return;
        }
    }
    currentState = STATE_PAUSED;
    lastActionTime = millis();
    pauseStartTime = millis();
    
    // CRITICAL FIX: Update lastPauseResumeTime to prevent the button flag that caused this pause
    // from immediately triggering a resume when processed
    lastPauseResumeTime = millis();
    
    // Start the 30-second grace period when pausing
    gracePeriodStartTime = millis();
    gracePeriodActive = true;
    LOG_INFO("Machine paused - 30-second grace period started");
    
    // CRITICAL FIX: Only accumulate elapsed time if tokenStartTime is valid (not 0)
    // Also handle millis() overflow correctly
    if (tokenStartTime != 0) {
        unsigned long elapsedSinceStart;
        if (pauseStartTime >= tokenStartTime) {
            elapsedSinceStart = pauseStartTime - tokenStartTime;
        } else {
            // Overflow occurred - calculate correctly
            elapsedSinceStart = (0xFFFFFFFFUL - tokenStartTime) + pauseStartTime + 1;
        }
        tokenTimeElapsed += elapsedSinceStart;
    } else {
        // If tokenStartTime is 0, something went wrong - log warning but don't corrupt tokenTimeElapsed
        LOG_WARNING("pauseMachine() called with tokenStartTime=0, skipping time accumulation");
    }
   
    // Publish pause event
    // publishActionEvent(activeButton, ACTION_PAUSE, MANUAL);
}

void CarWashController::resumeMachine(int buttonIndex) {
    LOG_INFO("Resuming machine with button %d (relay %d, bit %d)", 
             buttonIndex+1, buttonIndex+1, RELAY_INDICES[buttonIndex]);
    activeButton = buttonIndex;
    
    extern IoExpander ioExpander;
    
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Read relay state before activation
        uint8_t relayStateBefore = ioExpander.readRegister(OUTPUT_PORT1);
        LOG_INFO("Relay state BEFORE resume: 0x%02X (binary: %d%d%d%d%d%d%d%d)", 
                 relayStateBefore,
                 (relayStateBefore & 0x80) ? 1 : 0, (relayStateBefore & 0x40) ? 1 : 0,
                 (relayStateBefore & 0x20) ? 1 : 0, (relayStateBefore & 0x10) ? 1 : 0,
                 (relayStateBefore & 0x08) ? 1 : 0, (relayStateBefore & 0x04) ? 1 : 0,
                 (relayStateBefore & 0x02) ? 1 : 0, (relayStateBefore & 0x01) ? 1 : 0);
        
        // Turn on the relay for the active button
        ioExpander.setRelay(RELAY_INDICES[buttonIndex], true);
        
        // Verify relay state after activation
        uint8_t relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
        xSemaphoreGive(xIoExpanderMutex);
        
        LOG_INFO("Relay state AFTER resume: 0x%02X (binary: %d%d%d%d%d%d%d%d)", 
                 relayStateAfter,
                 (relayStateAfter & 0x80) ? 1 : 0, (relayStateAfter & 0x40) ? 1 : 0,
                 (relayStateAfter & 0x20) ? 1 : 0, (relayStateAfter & 0x10) ? 1 : 0,
                 (relayStateAfter & 0x08) ? 1 : 0, (relayStateAfter & 0x04) ? 1 : 0,
                 (relayStateAfter & 0x02) ? 1 : 0, (relayStateAfter & 0x01) ? 1 : 0);
        
        // Check if relay bit was actually set
        bool relayBitSet = (relayStateAfter & (1 << RELAY_INDICES[buttonIndex])) != 0;
        if (relayBitSet) {
            LOG_INFO("Relay %d (bit %d) successfully activated for button %d", 
                     buttonIndex+1, RELAY_INDICES[buttonIndex], buttonIndex+1);
        } else {
            LOG_ERROR("Failed to activate relay %d (bit %d) for resume! Expected bit %d to be set in 0x%02X", 
                     buttonIndex+1, RELAY_INDICES[buttonIndex], RELAY_INDICES[buttonIndex], relayStateAfter);
        }
    } else {
        LOG_WARNING("Failed to acquire IO expander mutex in resumeMachine()");
        return;
    }
    
    currentState = STATE_RUNNING;
    lastActionTime = millis();
    tokenStartTime = millis();
    
    // Clear grace period when resuming
    gracePeriodActive = false;
    gracePeriodStartTime = 0;
    LOG_INFO("Machine resumed - grace period cleared");
    
    // Publish resume event
    // publishActionEvent(buttonIndex, ACTION_RESUME, MANUAL);
}

void CarWashController::stopMachine(TriggerType triggerType) {
    // Capture activeButton before resetting it (needed for stop event)
    int buttonToStop = activeButton;
    
    if (activeButton >= 0) {
        extern IoExpander ioExpander;
        
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Turn off the active relay
            ioExpander.setRelay(RELAY_INDICES[activeButton], false);
            
            // Verify relay state after deactivation
            uint8_t relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
            xSemaphoreGive(xIoExpanderMutex);
            
            // Check relay bit is actually cleared
            bool relayBitCleared = (relayStateAfter & (1 << RELAY_INDICES[activeButton])) == 0;
            if (!relayBitCleared) {
                LOG_ERROR("Failed to deactivate relay %d for stop!", activeButton+1);
            }
        } else {
            LOG_WARNING("Failed to acquire IO expander mutex in stopMachine()");
        }
    }
    
    config.isLoaded = false;
    currentState = STATE_FREE;
    activeButton = -1;
    tokenStartTime = 0;
    tokenTimeElapsed = 0;
    pauseStartTime = 0;
    lastPauseResumeTime = 0;
    lastFunctionSwitchTime = 0;
    gracePeriodStartTime = 0;
    gracePeriodActive = false;
    tokensConsumedCount = 0; // Reset consumed tokens counter
    
    // // Publish stop event (only if we had an active button before stopping)
    // if (buttonToStop >= 0) {
    //     publishActionEvent(buttonToStop, ACTION_STOP, triggerType);
    // }
}

void CarWashController::activateButton(int buttonIndex, TriggerType triggerType) {
    // CRITICAL FIX: Ensure we're in IDLE state before activating
    // This prevents issues where activateButton might be called from wrong state
    if (currentState != STATE_IDLE) {
        LOG_ERROR("activateButton() called from wrong state: %d (expected STATE_IDLE). Resetting to IDLE first.", currentState);
        currentState = STATE_IDLE;
        activeButton = -1;
        tokenStartTime = 0;
        tokenTimeElapsed = 0;
        pauseStartTime = 0;
        lastPauseResumeTime = 0;
        lastFunctionSwitchTime = 0;
    }
    
    if (config.tokens <= 0) {
        LOG_WARNING("Cannot activate button %d - no tokens left (tokens=%d)", buttonIndex+1, config.tokens);
        return;
    }

    // CRITICAL: Update lastActionTime IMMEDIATELY when button is pressed
    // This resets the inactivity timeout right away, ensuring the display updates correctly
    unsigned long currentTime = millis();
    lastActionTime = currentTime;
    
    digitalWrite(RUNNING_LED_PIN, HIGH);
    currentState = STATE_RUNNING;
    activeButton = buttonIndex;
    tokenStartTime = currentTime;
    tokenTimeElapsed = 0;
    gracePeriodStartTime = 0;
    gracePeriodActive = false; // Clear grace period when starting to run
    
    extern IoExpander ioExpander;
    
    LOG_INFO("Activating button %d (relay %d, bit %d)", 
             buttonIndex+1, buttonIndex+1, RELAY_INDICES[buttonIndex]);
    
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Read relay state before activation
        uint8_t relayStateBefore = ioExpander.readRegister(OUTPUT_PORT1);
        LOG_INFO("Relay state BEFORE activation: 0x%02X (binary: %d%d%d%d%d%d%d%d)", 
                 relayStateBefore,
                 (relayStateBefore & 0x80) ? 1 : 0, (relayStateBefore & 0x40) ? 1 : 0,
                 (relayStateBefore & 0x20) ? 1 : 0, (relayStateBefore & 0x10) ? 1 : 0,
                 (relayStateBefore & 0x08) ? 1 : 0, (relayStateBefore & 0x04) ? 1 : 0,
                 (relayStateBefore & 0x02) ? 1 : 0, (relayStateBefore & 0x01) ? 1 : 0);
        
        // Turn on the corresponding relay
        ioExpander.setRelay(RELAY_INDICES[buttonIndex], true);
        
        // Verify relay state after activation
        uint8_t relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
        xSemaphoreGive(xIoExpanderMutex);
        
        LOG_INFO("Relay state AFTER activation: 0x%02X (binary: %d%d%d%d%d%d%d%d)", 
                 relayStateAfter,
                 (relayStateAfter & 0x80) ? 1 : 0, (relayStateAfter & 0x40) ? 1 : 0,
                 (relayStateAfter & 0x20) ? 1 : 0, (relayStateAfter & 0x10) ? 1 : 0,
                 (relayStateAfter & 0x08) ? 1 : 0, (relayStateAfter & 0x04) ? 1 : 0,
                 (relayStateAfter & 0x02) ? 1 : 0, (relayStateAfter & 0x01) ? 1 : 0);
        
        // Check if relay bit was actually set
        bool relayBitSet = (relayStateAfter & (1 << RELAY_INDICES[buttonIndex])) != 0;
        if (relayBitSet) {
            LOG_INFO("Relay %d (bit %d) successfully activated for button %d", 
                     buttonIndex+1, RELAY_INDICES[buttonIndex], buttonIndex+1);
        } else {
            LOG_ERROR("Failed to activate relay %d (bit %d)! Expected bit %d to be set in 0x%02X", 
                     buttonIndex+1, RELAY_INDICES[buttonIndex], RELAY_INDICES[buttonIndex], relayStateAfter);
        }
        
        // Verify port configuration
        uint8_t configPort1 = ioExpander.readRegister(CONFIG_PORT1);
        bool pinConfiguredAsOutput = ((configPort1 & (1 << RELAY_INDICES[buttonIndex])) == 0);
        if (!pinConfiguredAsOutput) {
            LOG_ERROR("CRITICAL: Relay %d pin (P1%d, bit %d) is configured as INPUT instead of OUTPUT!", 
                     buttonIndex+1, RELAY_INDICES[buttonIndex], RELAY_INDICES[buttonIndex]);
            LOG_ERROR("Port 1 Config register: 0x%02X (should have bit %d = 0 for output)", 
                     configPort1, RELAY_INDICES[buttonIndex]);
        }
        
        // Print full relay state diagnostic
        printRelayStates();
    } else {
        LOG_WARNING("Failed to acquire IO expander mutex in activateButton()");
        // Even if mutex fails, we've already updated state and lastActionTime
        // The relay activation will be retried on next update cycle if needed
    }
    
    // Prioritize using physical tokens first
    if (config.physicalTokens > 0) {
        config.physicalTokens--;
    }
    config.tokens--;

    // publishActionEvent(buttonIndex, ACTION_START, triggerType);
}

void CarWashController::tokenExpired() {
    LOG_INFO("Token expired - tokens remaining: %d, currentState: %d", config.tokens, currentState);
    
    // Check if we have more tokens to automatically consume
    if (config.tokens > 0) {
        LOG_INFO("Auto-consuming next token (%d tokens remaining)", config.tokens);
        consumeNextToken();
        return;
    }
    
    // No more tokens - turn off relay and finish
    if (activeButton >= 0) {
        extern IoExpander ioExpander;
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            ioExpander.setRelay(RELAY_INDICES[activeButton], false);
            xSemaphoreGive(xIoExpanderMutex);
        } else {
            LOG_WARNING("Failed to acquire IO expander mutex in tokenExpired()");
        }
    }
    activeButton = -1;
    
    LOG_INFO("Token expired and no tokens remaining, finishing immediately");
    stopMachine(AUTOMATIC);
}

void CarWashController::handleCoinAcceptor() {
    // Get reference to the IO expander
    extern IoExpander ioExpander;

    // Get current time for all timing operations
    unsigned long currentTime = millis();
    
    // Skip startup period to avoid false triggers
    static bool startupPeriod = true;
    static unsigned long startupBeginTime = 0;
    if (startupPeriod) {
        if (startupBeginTime == 0) {
            startupBeginTime = currentTime;
            LOG_INFO("COIN: Startup period started at %lu ms (will monitor after 3000ms)", currentTime);
        }
        // CRITICAL FIX: Check elapsed time since startup, not absolute time
        // This ensures we always wait 3 seconds after first call, regardless of when it's called
        unsigned long elapsedSinceStartup;
        if (currentTime >= startupBeginTime) {
            elapsedSinceStartup = currentTime - startupBeginTime;
        } else {
            // Handle millis() overflow
            elapsedSinceStartup = (0xFFFFFFFFUL - startupBeginTime) + currentTime + 1;
        }
        
        if (elapsedSinceStartup < 3000) { // Skip first 3 seconds after initialization
            static unsigned long lastStartupLog = 0;
            if (currentTime - lastStartupLog > 1000) {
                lastStartupLog = currentTime;
                LOG_DEBUG("COIN: Still in startup period (%lu ms remaining)", 3000 - elapsedSinceStartup);
            }
            return;
        }
        startupPeriod = false;
        LOG_INFO("COIN: Startup period over at %lu ms (elapsed: %lu ms), now actively monitoring", 
                currentTime, elapsedSinceStartup);
    }
    
    // FIXED: Check if the interrupt handler detected a coin signal
    if (ioExpander.isCoinSignalDetected()) {
        LOG_INFO("COIN: *** Interrupt-based coin signal detected! *** (time: %lu ms)", currentTime);
        
        // Read the current state to get more details
        uint8_t rawPortValue0 = 0;
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
            LOG_INFO("COIN: Interrupt detected - Port0=0x%02X, COIN_SIG bit=%d", 
                    rawPortValue0, (rawPortValue0 & (1 << COIN_SIG)) ? 1 : 0);
            ioExpander.clearCoinSignalFlag();
            LOG_DEBUG("COIN: Cleared coin signal flag");
            xSemaphoreGive(xIoExpanderMutex);
        } else {
            LOG_WARNING("COIN: Failed to acquire IO expander mutex in handleCoinAcceptor()");
            return;
        }
        
        // For your hardware configuration, 3.3V = active coin, 0.05V = no coin
        // When a coin passes (3.3V), the TCA9535 reads LOW (bit=0)
        // When no coin is present (0.05V), the TCA9535 reads HIGH (bit=1)
        bool coinSignalActive = ((rawPortValue0 & (1 << COIN_SIG)) == 0);
        
        LOG_INFO("COIN: Interrupt - Coin signal state: %s (raw bit: %d)", 
                coinSignalActive ? "ACTIVE (LOW/0 - coin present)" : "INACTIVE (HIGH/1 - no coin)",
                (rawPortValue0 & (1 << COIN_SIG)) ? 1 : 0);
        
        unsigned long timeSinceLastCoin = currentTime - lastCoinProcessedTime;
        LOG_INFO("COIN: Time since last coin: %lu ms (cooldown: %lu ms)", 
                timeSinceLastCoin, COIN_PROCESS_COOLDOWN);
        
        // Only process the coin if it's been long enough since the last coin
        if (timeSinceLastCoin > COIN_PROCESS_COOLDOWN) {
            LOG_INFO("COIN: *** Processing coin from interrupt detection ***");
            processCoinInsertion(currentTime);
        } else {
            LOG_WARNING("COIN: Ignoring coin signal - too soon after last coin (%lu ms < %lu ms cooldown)",
                    timeSinceLastCoin, COIN_PROCESS_COOLDOWN);
        }
        
        return;
    }
    
    // FALLBACK: Still include polling method as a backup
    // This is especially important during development or if interrupts aren't reliable
    
    // Read raw port value
    uint8_t rawPortValue0 = 0;
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
        xSemaphoreGive(xIoExpanderMutex);
    } else {
        static unsigned long lastMutexWarning = 0;
        if (currentTime - lastMutexWarning > 5000) {
            LOG_WARNING("COIN: Failed to acquire IO expander mutex in handleCoinAcceptor() fallback");
            lastMutexWarning = currentTime;
        }
        return;
    }
    
    // Get current state of coin signal pin with correct logic
    // When coin is present: Pin is LOW (bit=0) = ACTIVE
    // When no coin: Pin is HIGH (bit=1) = INACTIVE
    bool coinSignalActive = ((rawPortValue0 & (1 << COIN_SIG)) == 0);
    
    // Periodic detailed state logging
    static unsigned long lastStateLog = 0;
    static uint8_t lastLoggedPortValue = 0xFF;
    if (currentTime - lastStateLog > 2000 || rawPortValue0 != lastLoggedPortValue) {
        if (rawPortValue0 != lastLoggedPortValue) {
            LOG_INFO("COIN: Polling - Port0 changed: 0x%02X -> 0x%02X", lastLoggedPortValue, rawPortValue0);
            lastLoggedPortValue = rawPortValue0;
        }
        LOG_DEBUG("COIN: Polling - Port0=0x%02X, COIN_SIG bit=%d, Signal=%s, lastCoinState=%s", 
                rawPortValue0,
                (rawPortValue0 & (1 << COIN_SIG)) ? 1 : 0,
                coinSignalActive ? "ACTIVE (LOW/0)" : "INACTIVE (HIGH/1)",
                lastCoinState == LOW ? "LOW" : "HIGH");
        lastStateLog = currentTime;
    }
    
    // Static variables to track edges and timing patterns
    static unsigned long lastEdgeTime = 0;
    static int edgeCount = 0;
    static unsigned long edgeWindowStart = 0;
    
    // Signal-based edge detection (COIN_SIG pin only)
    // Convert lastCoinState from HIGH/LOW to boolean for comparison
    bool lastCoinStateBool = (lastCoinState == LOW);
    
    if (coinSignalActive != lastCoinStateBool) {
        unsigned long timeSinceLastEdge = (lastEdgeTime > 0) ? (currentTime - lastEdgeTime) : 0;
        lastEdgeTime = currentTime;
        
        LOG_INFO("COIN: *** EDGE DETECTED *** %s -> %s (time: %lu ms, since last edge: %lu ms)", 
                lastCoinStateBool ? "ACTIVE (coin present, LOW/0)" : "INACTIVE (no coin, HIGH/1)", 
                coinSignalActive ? "ACTIVE (coin present, LOW/0)" : "INACTIVE (no coin, HIGH/1)",
                currentTime, timeSinceLastEdge);
        
        // Multi-edge detection logic for coins that generate multiple pulses
        // Track ALL edges (both rising and falling) to detect coin patterns
        if (edgeCount == 0 || currentTime - edgeWindowStart > 1000) {
            if (edgeCount > 0) {
                LOG_DEBUG("COIN: Starting new edge window (previous had %d edges)", edgeCount);
            }
            edgeWindowStart = currentTime;
            edgeCount = 1;
            LOG_DEBUG("COIN: Edge window started, edgeCount=1");
        } else {
            edgeCount++;
            unsigned long windowDuration = currentTime - edgeWindowStart;
            
            LOG_DEBUG("COIN: Edge #%d in window (duration: %lu ms, window limit: %lu ms)", 
                    edgeCount, windowDuration, COIN_EDGE_WINDOW);
            
            unsigned long timeSinceLastCoin = currentTime - lastCoinProcessedTime;
            bool meetsMinEdges = (edgeCount >= COIN_MIN_EDGES);
            bool withinWindow = (windowDuration < COIN_EDGE_WINDOW);
            bool pastCooldown = (timeSinceLastCoin > COIN_PROCESS_COOLDOWN);
            
            LOG_DEBUG("COIN: Pattern check - edges: %d/%d, window: %lu/%lu ms, cooldown: %lu/%lu ms", 
                    edgeCount, COIN_MIN_EDGES, windowDuration, COIN_EDGE_WINDOW, 
                    timeSinceLastCoin, COIN_PROCESS_COOLDOWN);
            
            if (meetsMinEdges && withinWindow && pastCooldown) {
                LOG_INFO("COIN: *** Detected coin pattern: %d edges in %lu ms window ***", 
                        edgeCount, windowDuration);
                processCoinInsertion(currentTime);
                edgeCount = 0;
                edgeWindowStart = 0;
            } else {
                if (!meetsMinEdges) LOG_DEBUG("COIN: Pattern rejected - not enough edges");
                if (!withinWindow) LOG_DEBUG("COIN: Pattern rejected - window too long");
                if (!pastCooldown) LOG_DEBUG("COIN: Pattern rejected - cooldown not met");
            }
            
            if (edgeCount > 10) {
                LOG_WARNING("COIN: Too many edges (%d), resetting edge counter", edgeCount);
                edgeCount = 0;
                edgeWindowStart = 0;
            }
        }
        
        // With pull-up resistor:
        // Default state (no coin): Pin is pulled HIGH (bit=1) = INACTIVE
        // When coin is inserted: Pin is connected to ground/LOW (bit=0) = ACTIVE
        // We detect when the pin goes from INACTIVE (HIGH) to ACTIVE (LOW)
        // This is the primary detection method - single falling edge
        if (coinSignalActive && !lastCoinStateBool) {
            unsigned long timeSinceLastCoin = currentTime - lastCoinProcessedTime;
            LOG_INFO("COIN: *** FALLING EDGE DETECTED - Pin went from INACTIVE (HIGH/1) to ACTIVE (LOW/0) ***");
            LOG_INFO("COIN: Time since last coin: %lu ms (cooldown: %lu ms)", 
                    timeSinceLastCoin, COIN_PROCESS_COOLDOWN);
            
            if (timeSinceLastCoin > COIN_PROCESS_COOLDOWN) {
                LOG_INFO("COIN: *** Processing coin insertion from falling edge ***");
                processCoinInsertion(currentTime);
                edgeCount = 0; // Reset edge count after valid coin
                edgeWindowStart = 0; // Reset window
            } else {
                LOG_WARNING("COIN: Ignoring coin signal - too soon after last coin (%lu ms < %lu ms cooldown)",
                        timeSinceLastCoin, COIN_PROCESS_COOLDOWN);
            }
        }
        
        // Update lastCoinState (store as HIGH/LOW)
        uint8_t oldLastCoinState = lastCoinState;
        lastCoinState = coinSignalActive ? LOW : HIGH;
        LOG_DEBUG("COIN: Updated lastCoinState: %s -> %s", 
                oldLastCoinState == LOW ? "LOW" : "HIGH",
                lastCoinState == LOW ? "LOW" : "HIGH");
    }
    
    // Reset edge detection if it's been too long
    if (edgeCount > 0 && currentTime - edgeWindowStart > 1000) {
        LOG_WARNING("COIN: Resetting incomplete edge pattern after timeout (had %d edges, window: %lu ms)", 
                edgeCount, currentTime - edgeWindowStart);
        edgeCount = 0;
        edgeWindowStart = 0;
    }
    
    // Periodic debug logging
    static unsigned long lastDebugTime = 0;
    if (currentTime - lastDebugTime > 5000) {
        lastDebugTime = currentTime;
        unsigned long timeSinceLastCoin = currentTime - lastCoinProcessedTime;
        LOG_INFO("COIN: Status - Signal=%s, EdgeCount=%d, LastCoin=%lu ms ago, Cooldown=%lu ms, InterruptFlag=%s", 
                coinSignalActive ? "ACTIVE (LOW/0)" : "INACTIVE (HIGH/1)",
                edgeCount,
                timeSinceLastCoin,
                COIN_PROCESS_COOLDOWN,
                ioExpander.isCoinSignalDetected() ? "SET" : "CLEAR");
    }
}

// Helper method to handle the business logic of a coin insertion
void CarWashController::processCoinInsertion(unsigned long currentTime) {
    LOG_INFO("COIN: ========================================");
    LOG_INFO("COIN: *** COIN DETECTED AND PROCESSED! ***");
    LOG_INFO("COIN: Time: %lu ms", currentTime);
    LOG_INFO("COIN: ========================================");
    
    // Update activity tracking - crucial for debouncing and cooldown periods
    unsigned long oldLastCoinProcessedTime = lastCoinProcessedTime;
    lastActionTime = currentTime;
    lastCoinProcessedTime = currentTime; // This is critical for the cooldown between coins
    LOG_DEBUG("COIN: Updated lastCoinProcessedTime: %lu -> %lu", oldLastCoinProcessedTime, currentTime);
    
    // Update or create session
    if (config.isLoaded) {
        LOG_INFO("COIN: Adding physical token to existing session (tokens: %d -> %d)", 
                config.tokens, config.tokens + 1);
        config.physicalTokens++;
        config.tokens++;
        // Reset grace period when token is added
        if (currentState == STATE_IDLE) {
            gracePeriodStartTime = currentTime;
            gracePeriodActive = true;
            LOG_INFO("COIN: Reset grace period - 30 seconds to press button");
        }
    } else {
        LOG_INFO("COIN: Creating new anonymous session from coin insertion");
        
        char sessionIdBuffer[30];
        sprintf(sessionIdBuffer, "manual_%lu", currentTime);
        
        config.sessionId = String(sessionIdBuffer);
        config.userId = "unknown";
        config.userName = "";
        config.physicalTokens = 1;
        config.tokens = 1;
        config.isLoaded = true;
        tokensConsumedCount = 0;
        
        currentState = STATE_IDLE;
        gracePeriodStartTime = currentTime; // Start 30-second grace period
        gracePeriodActive = true;
        digitalWrite(LED_PIN_INIT, HIGH);
        
        LOG_INFO("COIN: Anonymous session created - sessionId='%s', userId='unknown', tokens=%d, state=IDLE", 
                config.sessionId.c_str(), config.tokens);
        LOG_INFO("COIN: Grace period started - 30 seconds to press button");
    }
    
    publishCoinInsertedEvent();
}

void CarWashController::autoConsumeToken() {
    if (config.tokens <= 0) {
        LOG_WARNING("autoConsumeToken called but no tokens available");
        return;
    }
    
    LOG_INFO("Auto-consuming token after 30-second grace period expired in IDLE state");
    LOG_INFO("Tokens before: %d, staying in IDLE and starting token consumption", config.tokens);
    
    // Consume a token
    if (config.physicalTokens > 0) {
        config.physicalTokens--;
    }
    config.tokens--;
    tokensConsumedCount++;
    
    // Stay in IDLE state, but start token timer
    // Grace period is over, start consuming token time while remaining in IDLE
    currentState = STATE_IDLE; // Explicitly set to IDLE (though we're already there)
    tokenStartTime = millis();
    tokenTimeElapsed = 0;
    gracePeriodActive = false; // Grace period is over, start consuming token time
    gracePeriodStartTime = 0;
    activeButton = -1; // No button active yet
    lastActionTime = millis();
    
    LOG_INFO("Token auto-consumed: tokens_left=%d, state=IDLE, consuming token time", config.tokens);
    
    // Don't publish pause event since we're staying in IDLE
    // No relay state change, so no action event needed
}

void CarWashController::consumeNextToken() {
    if (config.tokens <= 0) {
        LOG_WARNING("consumeNextToken called but no tokens available");
        return;
    }
    
    MachineState previousState = currentState;
    LOG_INFO("Consuming next token - tokens before: %d, current state: %d", config.tokens, currentState);
    
    // Consume a token
    if (config.physicalTokens > 0) {
        config.physicalTokens--;
    }
    config.tokens--;
    tokensConsumedCount++;
    
    // Reset token timer for the new token
    tokenStartTime = millis();
    tokenTimeElapsed = 0;
    
    // Stay in the same state (RUNNING stays RUNNING, PAUSED stays PAUSED)
    // If relay was on, it stays on. If relay was off, it stays off.
    lastActionTime = millis();
    
    LOG_INFO("Next token consumed: tokens_left=%d, state remains=%d, activeButton=%d", 
             config.tokens, currentState, activeButton);
    
    // // Publish continuation event based on state
    // if (previousState == STATE_RUNNING && activeButton >= 0) {
    //     publishActionEvent(activeButton, ACTION_RESUME, AUTOMATIC);
    // } else if (previousState == STATE_PAUSED) {
    //     publishActionEvent(activeButton >= 0 ? activeButton : -1, ACTION_PAUSE, AUTOMATIC);
    // }
}

void CarWashController::switchFunction(int newButtonIndex) {
    if (currentState != STATE_RUNNING) {
        LOG_WARNING("switchFunction called but not in RUNNING state");
        return;
    }
    
    LOG_INFO("Switching from button %d to button %d", activeButton + 1, newButtonIndex + 1);
    
    extern IoExpander ioExpander;
    
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Turn off the current relay if there is one
        if (activeButton >= 0) {
            ioExpander.setRelay(RELAY_INDICES[activeButton], false);
            LOG_INFO("Deactivated relay %d (button %d)", activeButton + 1, activeButton + 1);
        }
        
        // Turn on the new relay
        ioExpander.setRelay(RELAY_INDICES[newButtonIndex], true);
        LOG_INFO("Activated relay %d (button %d)", newButtonIndex + 1, newButtonIndex + 1);
        
        xSemaphoreGive(xIoExpanderMutex);
    } else {
        LOG_WARNING("Failed to acquire IO expander mutex in switchFunction()");
        return;
    }
    
    // // Publish stop event for old button if there was one
    // if (activeButton >= 0) {
    //     publishActionEvent(activeButton, ACTION_STOP, MANUAL);
    // }
    
    // Update active button
    activeButton = newButtonIndex;
    
    // Track when we switched functions to prevent same button press from pausing immediately
    lastFunctionSwitchTime = millis();
    
    // Keep the token timer running (don't reset tokenStartTime or tokenTimeElapsed)
    // Just update last action time
    lastActionTime = millis();
    
    // Publish start event for new button
    // publishActionEvent(newButtonIndex, ACTION_START, MANUAL);
    
    LOG_INFO("Function switched successfully - now running button %d", newButtonIndex + 1);
}

unsigned long CarWashController::getInactivityTimeout() const {
    // Total timeout = 30 seconds + tokens_remaining * 2 minutes
    // But if we're in RUNNING or PAUSED, we use tokens consumed + current token in progress
    unsigned long timeout = BASE_INACTIVE_TIMEOUT; // 30 seconds base
    
    if (config.isLoaded) {
        // Add time for remaining tokens (tokens not yet consumed)
        timeout += config.tokens * TOKEN_TIME;
        
        // If currently consuming a token (RUNNING or PAUSED), add time for that token
        // Note: The token is already decremented from config.tokens when we start using it
        if (currentState == STATE_RUNNING || currentState == STATE_PAUSED) {
            timeout += TOKEN_TIME;
        }
    }
    
    return timeout;
}

void CarWashController::update() {
    unsigned long currentTime = millis();
    
    // Periodic timeout debugging logs (every 5 seconds)
    static unsigned long lastTimeoutLogTime = 0;
    unsigned long logElapsed;
    if (lastTimeoutLogTime == 0 || currentTime >= lastTimeoutLogTime) {
        logElapsed = currentTime - lastTimeoutLogTime;
    } else {
        logElapsed = (0xFFFFFFFFUL - lastTimeoutLogTime) + currentTime + 1;
    }
    
    if (logElapsed >= 5000) { // Log every 5 seconds
        // Log token timeout variables
        if ((currentState == STATE_RUNNING || currentState == STATE_PAUSED) && tokenStartTime != 0) {
            unsigned long totalElapsedTime;
            unsigned long runningTime;
            if (currentState == STATE_RUNNING) {
                if (currentTime >= tokenStartTime) {
                    runningTime = currentTime - tokenStartTime;
                } else {
                    runningTime = (0xFFFFFFFFUL - tokenStartTime) + currentTime + 1;
                }
                totalElapsedTime = tokenTimeElapsed + runningTime;
            } else { // STATE_PAUSED
                totalElapsedTime = tokenTimeElapsed;
            }
            
            bool tokenExpired = (totalElapsedTime >= TOKEN_TIME);
            // LOG_INFO("[TOKEN_TIMEOUT] state=%d, tokenStartTime=%lu, tokenTimeElapsed=%lu, currentTime=%lu, runningTime=%lu, totalElapsedTime=%lu, TOKEN_TIME=%lu, expired=%d",
            //         currentState, tokenStartTime, tokenTimeElapsed, currentTime, 
            //         (currentState == STATE_RUNNING) ? runningTime : 0UL, 
            //         totalElapsedTime, TOKEN_TIME, tokenExpired ? 1 : 0);
        } else {
            // LOG_INFO("[TOKEN_TIMEOUT] state=%d, tokenStartTime=%lu (not active)", currentState, tokenStartTime);
        }
        
        // Log user inactivity timeout variables
        if (currentState != STATE_FREE && config.isLoaded) {
            unsigned long elapsedTime;
            if (currentTime >= lastActionTime) {
                elapsedTime = currentTime - lastActionTime;
            } else {
                elapsedTime = (0xFFFFFFFFUL - lastActionTime) + currentTime + 1;
            }
            
            unsigned long inactivityTimeout = getInactivityTimeout();
            bool inactivityExpired = (elapsedTime >= inactivityTimeout);
            // LOG_INFO("[INACTIVITY_TIMEOUT] state=%d, isLoaded=%d, lastActionTime=%lu, currentTime=%lu, elapsedTime=%lu, inactivityTimeout=%lu, expired=%d",
            //         currentState, config.isLoaded ? 1 : 0, lastActionTime, currentTime, 
            //         elapsedTime, inactivityTimeout, inactivityExpired ? 1 : 0);
        } else {
            // LOG_INFO("[INACTIVITY_TIMEOUT] state=%d, isLoaded=%d (not active)", currentState, config.isLoaded ? 1 : 0);
        }
        
        lastTimeoutLogTime = currentTime;
    }
    
    // PRIORITY 0: Check grace period expiration
    // Grace period is active in two states:
    // 1. IDLE: After 30 seconds, auto-consume token and start counting token time (stay in IDLE)
    // 2. PAUSED: After 30 seconds, end grace period and continue consuming token time
    if (gracePeriodActive && gracePeriodStartTime != 0) {
        unsigned long gracePeriodElapsed;
        if (currentTime >= gracePeriodStartTime) {
            gracePeriodElapsed = currentTime - gracePeriodStartTime;
        } else {
            gracePeriodElapsed = (0xFFFFFFFFUL - gracePeriodStartTime) + currentTime + 1;
        }
        
        if (gracePeriodElapsed >= GRACE_PERIOD_TIMEOUT) {
            if (currentState == STATE_IDLE && config.isLoaded && config.tokens > 0) {
                LOG_INFO("Grace period expired in IDLE (%lu ms >= %lu ms), auto-consuming token", 
                         gracePeriodElapsed, GRACE_PERIOD_TIMEOUT);
                autoConsumeToken();
                // Don't return - continue with normal update cycle
            } else if (currentState == STATE_PAUSED) {
                LOG_INFO("Grace period expired in PAUSED (%lu ms >= %lu ms), resuming token consumption", 
                         gracePeriodElapsed, GRACE_PERIOD_TIMEOUT);
                gracePeriodActive = false;
                gracePeriodStartTime = 0;
                // Token time will now start counting down since grace period is over
                // We need to reset tokenStartTime to current time so the elapsed time calculation is correct
                tokenStartTime = currentTime;
                LOG_INFO("Token consumption resumed - timer reset");
            }
        }
    }
    
    // PRIORITY 1: Check inactivity timeout (dynamic based on tokens)
    // This ensures logout happens as soon as timeout is reached, before any other operations
    // CRITICAL: Check this before anything else to prevent delays from blocking operations
    // CRITICAL: Use consistent currentTime to avoid timing discrepancies
    if (currentState != STATE_FREE && config.isLoaded) {
        // Handle potential millis() overflow (wraps every ~50 days)
        unsigned long elapsedTime;
        if (currentTime >= lastActionTime) {
            elapsedTime = currentTime - lastActionTime;
        } else {
            // Overflow occurred - calculate correctly
            elapsedTime = (0xFFFFFFFFUL - lastActionTime) + currentTime + 1;
        }
        
        unsigned long inactivityTimeout = getInactivityTimeout();
        if (elapsedTime >= inactivityTimeout) {
            LOG_INFO("Inactivity timeout reached (%lu ms >= %lu ms), logging out user", elapsedTime, inactivityTimeout);
            stopMachine(AUTOMATIC);
            // Return immediately after logout to ensure state change is processed
            // This prevents any blocking operations (like network I/O) from delaying the logout
            return;
        }
    }
    

    
    // Always handle coin acceptor - coins can create anonymous sessions when machine is not loaded
    handleCoinAcceptor();
    
    // Only handle buttons when machine is loaded (buttons require a loaded session)
    if (config.isLoaded) {
        handleButtons();
    } else {
        // Log when buttons are skipped (only in debug mode to avoid spam)
        static unsigned long lastSkipLog = 0;
        if (currentTime - lastSkipLog > 5000) { // Log every 5 seconds max
            LOG_DEBUG("Skipping button handling - machine not loaded (isLoaded=%d, timestamp empty=%d). Coins can still be inserted to create anonymous session.", 
                     config.isLoaded, config.timestamp.length() == 0);
            lastSkipLog = currentTime;
        }
    }
    
    currentTime = millis();
    
    // Check token expiration for IDLE (with consumed token), RUNNING, or PAUSED states
    if ((currentState == STATE_IDLE || currentState == STATE_RUNNING || currentState == STATE_PAUSED) && tokenStartTime != 0) {
        unsigned long totalElapsedTime;
        if (currentState == STATE_RUNNING) {
            // Handle potential millis() overflow
            unsigned long runningTime;
            if (currentTime >= tokenStartTime) {
                runningTime = currentTime - tokenStartTime;
            } else {
                // Overflow occurred - calculate correctly
                runningTime = (0xFFFFFFFFUL - tokenStartTime) + currentTime + 1;
            }
            totalElapsedTime = tokenTimeElapsed + runningTime;
        } else if (currentState == STATE_IDLE) {
            // In IDLE with an active token (grace period expired), token time counts down
            unsigned long idleTime;
            if (currentTime >= tokenStartTime) {
                idleTime = currentTime - tokenStartTime;
            } else {
                // Overflow occurred - calculate correctly
                idleTime = (0xFFFFFFFFUL - tokenStartTime) + currentTime + 1;
            }
            totalElapsedTime = tokenTimeElapsed + idleTime;
        } else { // STATE_PAUSED
            // When paused with grace period active, elapsed time is frozen at tokenTimeElapsed
            // When paused with grace period expired, token time counts down
            if (gracePeriodActive) {
                // Grace period is active - token time is frozen
                totalElapsedTime = tokenTimeElapsed;
            } else {
                // Grace period expired - token time counts down
                unsigned long pausedTime;
                if (currentTime >= tokenStartTime) {
                    pausedTime = currentTime - tokenStartTime;
                } else {
                    // Overflow occurred - calculate correctly
                    pausedTime = (0xFFFFFFFFUL - tokenStartTime) + currentTime + 1;
                }
                totalElapsedTime = tokenTimeElapsed + pausedTime;
            }
        }
        
        if (totalElapsedTime >= TOKEN_TIME) {
            LOG_INFO("Token time expired (%lu ms >= %lu ms), calling tokenExpired()", totalElapsedTime, TOKEN_TIME);
            tokenExpired();
            // Re-check inactivity timeout after token expires (user might be logged out)
            // CRITICAL: Re-capture currentTime after tokenExpired() as it may have taken time
            currentTime = millis();
            if (currentState != STATE_FREE && config.isLoaded) {
                // Handle potential millis() overflow
                unsigned long elapsedTime;
                if (currentTime >= lastActionTime) {
                    elapsedTime = currentTime - lastActionTime;
                } else {
                    // Overflow occurred - calculate correctly
                    elapsedTime = (0xFFFFFFFFUL - lastActionTime) + currentTime + 1;
                }
                unsigned long inactivityTimeout = getInactivityTimeout();
                if (elapsedTime >= inactivityTimeout) {
                    LOG_INFO("Inactivity timeout also reached after token expiry, logging out (%lu ms >= %lu ms)", 
                             elapsedTime, inactivityTimeout);
                    stopMachine(AUTOMATIC);
                    return;
                }
            }
        }
    }
    
    // NOTE: Periodic state publishing has been removed.
    // State is now published on demand with high priority when:
    // - A message is received on /init topic
    // - A message is received on /get_state topic
}

void CarWashController::publishMachineSetupActionEvent() {

    StaticJsonDocument<512> doc;
    doc["machine_id"] = MACHINE_ID;
    doc["action"] = getMachineActionString(ACTION_SETUP);
    doc["timestamp"] = getTimestamp();
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Queue message for publishing (critical - machine setup event)
    queueMqttMessage(ACTION_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE, true);
}

unsigned long CarWashController::getSecondsLeft() {
    // Return token time for IDLE (with active token), RUNNING, or PAUSED states
    if (currentState != STATE_IDLE && currentState != STATE_RUNNING && currentState != STATE_PAUSED) {
        return 0;
    }

    // CRITICAL FIX: Return 0 if tokenStartTime is not valid (not initialized)
    if (tokenStartTime == 0) {
        return 0;
    }

    unsigned long currentTime = millis();
    unsigned long totalElapsedTime;

    if (currentState == STATE_RUNNING) {
        // Handle potential millis() overflow
        unsigned long runningTime;
        if (currentTime >= tokenStartTime) {
            runningTime = currentTime - tokenStartTime;
        } else {
            // Overflow occurred - calculate correctly
            runningTime = (0xFFFFFFFFUL - tokenStartTime) + currentTime + 1;
        }
        totalElapsedTime = tokenTimeElapsed + runningTime;
    } else if (currentState == STATE_IDLE) {
        // In IDLE with an active token (grace period expired), token time counts down
        unsigned long idleTime;
        if (currentTime >= tokenStartTime) {
            idleTime = currentTime - tokenStartTime;
        } else {
            // Overflow occurred - calculate correctly
            idleTime = (0xFFFFFFFFUL - tokenStartTime) + currentTime + 1;
        }
        totalElapsedTime = tokenTimeElapsed + idleTime;
    } else { // PAUSED
        // When paused with grace period active, elapsed time is frozen at tokenTimeElapsed
        // When paused with grace period expired, token time counts down
        if (gracePeriodActive) {
            // Grace period is active - token time is frozen
            totalElapsedTime = tokenTimeElapsed;
        } else {
            // Grace period expired - token time counts down
            unsigned long pausedTime;
            if (currentTime >= tokenStartTime) {
                pausedTime = currentTime - tokenStartTime;
            } else {
                // Overflow occurred - calculate correctly
                pausedTime = (0xFFFFFFFFUL - tokenStartTime) + currentTime + 1;
            }
            totalElapsedTime = tokenTimeElapsed + pausedTime;
        }
    }

    // Calculate remaining time for current token
    unsigned long currentTokenRemainingMs = 0;
    if (totalElapsedTime < TOKEN_TIME) {
        currentTokenRemainingMs = TOKEN_TIME - totalElapsedTime;
    }
    
    // Add time from remaining tokens (tokens not yet consumed)
    unsigned long remainingTokensTimeMs = config.tokens * TOKEN_TIME;
    
    // Total time remaining = current token remaining + remaining tokens time
    unsigned long totalRemainingMs = currentTokenRemainingMs + remainingTokensTimeMs;
    
    // Return total remaining seconds (rounded down)
    return totalRemainingMs / 1000;
}

String CarWashController::getTimestamp() {
    // Return default timestamp (using millis() for relative time tracking)
    return "2000-01-01T00:00:00.000Z";
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

    // Queue message for publishing (critical - coin/token event)
    queueMqttMessage(ACTION_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE, true);
}

// Debug method to directly simulate a coin insertion
void CarWashController::simulateCoinInsertion() {
    processCoinInsertion(millis());
}

void CarWashController::printRelayStates() {
    extern IoExpander ioExpander;
    
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Read output states
        uint8_t relayStatePort1 = ioExpander.readRegister(OUTPUT_PORT1);
        uint8_t relayStatePort0 = ioExpander.readRegister(OUTPUT_PORT0);
        
        // Read configuration registers to verify port setup
        uint8_t configPort0 = ioExpander.readRegister(CONFIG_PORT0);
        uint8_t configPort1 = ioExpander.readRegister(CONFIG_PORT1);
        
        xSemaphoreGive(xIoExpanderMutex);
        
        LOG_INFO("=== RELAY STATES & CONFIGURATION ===");
        LOG_INFO("Port 0 Output Value: 0x%02X (binary: %d%d%d%d%d%d%d%d)",
                 relayStatePort0,
                 (relayStatePort0 & 0x80) ? 1 : 0, (relayStatePort0 & 0x40) ? 1 : 0,
                 (relayStatePort0 & 0x20) ? 1 : 0, (relayStatePort0 & 0x10) ? 1 : 0,
                 (relayStatePort0 & 0x08) ? 1 : 0, (relayStatePort0 & 0x04) ? 1 : 0,
                 (relayStatePort0 & 0x02) ? 1 : 0, (relayStatePort0 & 0x01) ? 1 : 0);
        LOG_INFO("Port 0 Config: 0x%02X (1=input, 0=output)", configPort0);
        
        LOG_INFO("Port 1 Output Value: 0x%02X (binary: %d%d%d%d%d%d%d%d)",
                 relayStatePort1,
                 (relayStatePort1 & 0x80) ? 1 : 0, (relayStatePort1 & 0x40) ? 1 : 0,
                 (relayStatePort1 & 0x20) ? 1 : 0, (relayStatePort1 & 0x10) ? 1 : 0,
                 (relayStatePort1 & 0x08) ? 1 : 0, (relayStatePort1 & 0x04) ? 1 : 0,
                 (relayStatePort1 & 0x02) ? 1 : 0, (relayStatePort1 & 0x01) ? 1 : 0);
        LOG_INFO("Port 1 Config: 0x%02X (1=input, 0=output)", configPort1);
        
        LOG_INFO("Individual Relay States (Port 1):");
        for (int i = 0; i < 5; i++) {
            bool relayOn = (relayStatePort1 & (1 << RELAY_INDICES[i])) != 0;
            bool pinConfiguredAsOutput = ((configPort1 & (1 << RELAY_INDICES[i])) == 0);
            LOG_INFO("  Button %d -> Relay %d (P1%d, bit %d): %s [Config: %s]", 
                     i+1, i+1, RELAY_INDICES[i], RELAY_INDICES[i], 
                     relayOn ? "ON" : "OFF",
                     pinConfiguredAsOutput ? "OUTPUT" : "INPUT (WRONG!)");
        }
        
        LOG_INFO("Port 0 Pins (checking if relays might be here):");
        LOG_INFO("  P00 (clearwater?): Output=%d, Config=%s", 
                 (relayStatePort0 & 0x01) ? 1 : 0,
                 (configPort0 & 0x01) ? "INPUT" : "OUTPUT");
        LOG_INFO("  P04 (inflatable?): Output=%d, Config=%s", 
                 (relayStatePort0 & 0x10) ? 1 : 0,
                 (configPort0 & 0x10) ? "INPUT" : "OUTPUT");
        
        LOG_INFO("Active Button: %d", activeButton >= 0 ? activeButton + 1 : -1);
        LOG_INFO("====================================");
    } else {
        LOG_WARNING("Failed to acquire IO expander mutex in printRelayStates()");
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
    
    unsigned long currentTime = millis();
    // Handle potential millis() overflow (wraps every ~50 days)
    unsigned long elapsedTime;
    if (currentTime >= lastActionTime) {
        elapsedTime = currentTime - lastActionTime;
    } else {
        // Overflow occurred - calculate correctly
        elapsedTime = (0xFFFFFFFFUL - lastActionTime) + currentTime + 1;
    }
    
    // Get dynamic inactivity timeout based on tokens
    unsigned long inactivityTimeout = getInactivityTimeout();
    
    // Return 0 when elapsed >= timeout (matches the >= check in update())
    // This ensures display shows 00:00 exactly when logout happens
    if (elapsedTime >= inactivityTimeout) {
        return 0;
    }
    
    // Return remaining time in milliseconds
    unsigned long remaining = inactivityTimeout - elapsedTime;
    return remaining;
}

unsigned long CarWashController::getGracePeriodSecondsLeft() const {
    if (!gracePeriodActive || gracePeriodStartTime == 0) {
        return 0;
    }
    
    unsigned long currentTime = millis();
    unsigned long elapsed;
    if (currentTime >= gracePeriodStartTime) {
        elapsed = currentTime - gracePeriodStartTime;
    } else {
        // Handle millis() overflow
        elapsed = (0xFFFFFFFFUL - gracePeriodStartTime) + currentTime + 1;
    }
    
    // Return 0 if grace period has expired
    if (elapsed >= GRACE_PERIOD_TIMEOUT) {
        return 0;
    }
    
    // Return remaining seconds (rounded down)
    unsigned long remainingMs = GRACE_PERIOD_TIMEOUT - elapsed;
    return remainingMs / 1000;
}

// getTokensLeft and getUserName are implemented as inline methods in the header

/**
 * Helper method to queue MQTT messages for the dedicated publisher task
 * This prevents blocking the controller when MQTT operations are slow or connection is down
 * 
 * @param topic The MQTT topic to publish to
 * @param payload The message payload (JSON string)
 * @param qos Quality of Service (0 or 1)
 * @param isCritical Whether this message should be buffered and retried when disconnected
 * @return true if message was queued successfully, false if queue is full
 */
bool CarWashController::queueMqttMessage(const char* topic, const char* payload, uint8_t qos, bool isCritical) {
    // Check if queue exists
    if (xMqttPublishQueue == NULL) {
        LOG_ERROR("MQTT publish queue not initialized!");
        return false;
    }
    
    // Create message structure
    MqttMessage msg;
    
    // Copy topic (ensure null termination)
    strncpy(msg.topic, topic, sizeof(msg.topic) - 1);
    msg.topic[sizeof(msg.topic) - 1] = '\0';
    
    // Copy payload (ensure null termination)
    strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    msg.payload[sizeof(msg.payload) - 1] = '\0';
    
    // Set QoS and priority
    msg.qos = qos;
    msg.isCritical = isCritical;
    msg.timestamp = millis();
    
    // Check if payload was truncated
    if (strlen(payload) >= sizeof(msg.payload)) {
        LOG_WARNING("MQTT payload truncated (max size: %d bytes)", sizeof(msg.payload) - 1);
    }
    
    // Try to queue the message (non-blocking)
    if (xQueueSend(xMqttPublishQueue, &msg, 0) == pdTRUE) {
        LOG_DEBUG("Queued MQTT message: topic=%s, qos=%d, critical=%d", topic, qos, isCritical);
        return true;
    } else {
        LOG_WARNING("MQTT queue full, cannot queue message to %s (queue: %d/%d)", 
                   topic, uxQueueMessagesWaiting(xMqttPublishQueue), MQTT_QUEUE_SIZE);
        return false;
    }
}
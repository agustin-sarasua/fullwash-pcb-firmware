#include "car_wash_controller.h"
#include "io_expander.h"
#include "rtc_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// External mutex for ioExpander access (defined in main.cpp)
extern SemaphoreHandle_t xIoExpanderMutex;

CarWashController::CarWashController(MqttLteClient& client)
    : mqttClient(client),
      rtcManager(nullptr),
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
      lastPauseResumeTime(0) {
          
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

void CarWashController::setRTCManager(RTCManager* rtc) {
    rtcManager = rtc;
    LOG_INFO("RTC Manager connected to controller");
}

void CarWashController::handleMqttMessage(const char* topic, const uint8_t* payload, unsigned len) {
    // Handle get_state topic first (doesn't require JSON parsing)
    if (String(topic) == GET_STATE_TOPIC) {
        LOG_INFO("Received get_state request, publishing state on demand");
        // Publish state on demand with high priority when get_state message is received
        publishStateOnDemand();
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
        // If not present, try to get it from RTC if available
        if (doc.containsKey("timestamp") && doc["timestamp"].as<String>().length() > 0) {
            config.timestamp = doc["timestamp"].as<String>();
            LOG_INFO("Timestamp from INIT_TOPIC: %s", config.timestamp.c_str());
        } else if (rtcManager && rtcManager->isInitialized()) {
            // Fallback to RTC timestamp if INIT_TOPIC doesn't have one
            config.timestamp = rtcManager->getTimestampWithMillis();
            if (config.timestamp != "RTC Error" && config.timestamp.length() > 0) {
                LOG_INFO("Using RTC timestamp: %s", config.timestamp.c_str());
            } else {
                config.timestamp = ""; // RTC not available or invalid
                LOG_WARNING("RTC timestamp not available, leaving timestamp empty");
            }
        } else {
            config.timestamp = ""; // No timestamp available
            LOG_WARNING("No timestamp available in INIT_TOPIC and RTC not initialized");
        }
        
        config.isLoaded = true;
        currentState = STATE_IDLE;
        lastActionTime = millis();
        // Reset token timing variables to ensure clean state
        tokenStartTime = 0;
        tokenTimeElapsed = 0;
        pauseStartTime = 0;
        activeButton = -1;
        LOG_INFO("Switching on LED");
        digitalWrite(LED_PIN_INIT, HIGH);
        LOG_INFO("Machine loaded with new configuration");
        
        // CRITICAL: Publish state immediately after loading with high priority (QOS1)
        // This ensures the backend receives the IDLE state quickly so the app can detect it
        publishStateOnDemand();
    } else if (String(topic) == CONFIG_TOPIC) {
        LOG_INFO("Received config message from server");
        config.timestamp = doc["timestamp"].as<String>();
        

        // Sync RTC with server timestamp if RTC is available
        // This is now only sent when RTC time is invalid, so always sync when received
        if (rtcManager && rtcManager->isInitialized() && config.timestamp.length() > 0) {
            LOG_INFO("Syncing RTC with server timestamp: %s", config.timestamp.c_str());
            if (rtcManager->setDateTimeFromISO(config.timestamp)) {
                LOG_INFO("RTC synchronized successfully!");
                rtcManager->printDebugInfo();
            } else {
                LOG_WARNING("Failed to sync RTC with server timestamp");
            }
        } else if (!rtcManager || !rtcManager->isInitialized()) {
            LOG_WARNING("Cannot sync RTC - RTC not initialized");
        }
        
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

        // CRITICAL FIX: Always clear the flag immediately to prevent delayed processing
        // The old logic kept flags when state was wrong, causing 20-second delays
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
                    // CRITICAL FIX: Allow pause on button press when RUNNING
                    // If activeButton is -1 (shouldn't happen, but handle gracefully), allow any button to pause
                    // Otherwise, only allow the active button to pause
                    LOG_INFO("Button %d pressed while RUNNING (activeButton=%d)", 
                            detectedId + 1, activeButton + 1);
                    if (activeButton == -1 || (int)detectedId == activeButton) {
                        // CRITICAL FIX: Prevent rapid pause/resume toggling
                        unsigned long currentTime = millis();
                        unsigned long timeSinceLastPauseResume;
                        if (currentTime >= lastPauseResumeTime) {
                            timeSinceLastPauseResume = currentTime - lastPauseResumeTime;
                        } else {
                            timeSinceLastPauseResume = (0xFFFFFFFFUL - lastPauseResumeTime) + currentTime + 1;
                        }
                        
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
                            // This prevents the flag handler from pausing immediately after raw polling activated
                            if (timeSinceActivation < 200) {
                                LOG_INFO("Button %d pressed while RUNNING - ignoring (just activated %lu ms ago, likely same press)", 
                                       detectedId + 1, timeSinceActivation);
                                // Still reset inactivity timeout
                                lastActionTime = currentTime;
                                buttonProcessed = true;
                                return; // Skip raw polling
                            }
                        }
                        
                        // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                        lastActionTime = currentTime;
                        
                        if (timeSinceLastPauseResume < PAUSE_RESUME_COOLDOWN) {
                            LOG_WARNING("Button %d pressed while RUNNING - ignoring (cooldown: %lu ms < %lu ms)", 
                                       detectedId + 1, timeSinceLastPauseResume, PAUSE_RESUME_COOLDOWN);
                        } else {
                            if (activeButton == -1) {
                                LOG_WARNING("activeButton is -1 in RUNNING state - allowing pause anyway (button %d)", detectedId + 1);
                                // Set activeButton to the pressed button to fix the tracking
                                activeButton = detectedId;
                            }
                            LOG_INFO("Pausing machine - button matches active button");
                            pauseMachine();
                            lastPauseResumeTime = currentTime;
                            buttonProcessed = true;
                        }
                    } else {
                        // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                        lastActionTime = millis();
                        LOG_WARNING("Button %d pressed while RUNNING (activeButton=%d) - ignoring", 
                                   detectedId + 1, activeButton + 1);
                    }
                } else if (currentState == STATE_PAUSED) {
                    // CRITICAL FIX: Only resume if the button matches the active button
                    // This prevents accidentally activating a different button when paused
                    // If user wants a different button, they should stop first
                    if (activeButton == -1 || (int)detectedId == activeButton) {
                        // CRITICAL FIX: Prevent rapid pause/resume toggling
                        unsigned long currentTime = millis();
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
                            if (activeButton == -1) {
                                LOG_WARNING("activeButton is -1 in PAUSED state - allowing resume anyway (button %d)", detectedId + 1);
                                // Set activeButton to the pressed button to fix the tracking
                                activeButton = detectedId;
                            }
                            LOG_INFO("Resuming machine - button matches active button");
                            resumeMachine(detectedId);
                            lastPauseResumeTime = currentTime;
                            buttonProcessed = true;
                        }
                    } else {
                        // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                        lastActionTime = millis();
                        LOG_WARNING("Button %d pressed while PAUSED (activeButton=%d) - ignoring (must press same button to resume)", 
                                   detectedId + 1, activeButton + 1);
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
                        // CRITICAL FIX: Allow pause on button press when RUNNING
                        // If activeButton is -1 (shouldn't happen, but handle gracefully), allow any button to pause
                        // Otherwise, only allow the active button to pause
                        LOG_INFO("Button %d pressed while RUNNING (activeButton=%d) - raw polling", 
                                i + 1, activeButton + 1);
                        if (activeButton == -1 || i == activeButton) {
                            // CRITICAL FIX: Prevent rapid pause/resume toggling
                            unsigned long currentTime = millis();
                            unsigned long timeSinceLastPauseResume;
                            if (currentTime >= lastPauseResumeTime) {
                                timeSinceLastPauseResume = currentTime - lastPauseResumeTime;
                            } else {
                                timeSinceLastPauseResume = (0xFFFFFFFFUL - lastPauseResumeTime) + currentTime + 1;
                            }
                            
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
                                // This prevents double-processing of the same button press
                                if (timeSinceActivation < 200) {
                                    LOG_INFO("Button %d pressed while RUNNING - ignoring (just activated %lu ms ago, likely same press) - raw polling", 
                                           i + 1, timeSinceActivation);
                                    // Still reset inactivity timeout
                                    lastActionTime = currentTime;
                                    continue; // Skip to next button
                                }
                            }
                            
                            // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                            lastActionTime = currentTime;
                            
                            if (timeSinceLastPauseResume < PAUSE_RESUME_COOLDOWN) {
                                LOG_WARNING("Button %d pressed while RUNNING - ignoring (cooldown: %lu ms < %lu ms) - raw polling", 
                                           i + 1, timeSinceLastPauseResume, PAUSE_RESUME_COOLDOWN);
                            } else {
                                if (activeButton == -1) {
                                    LOG_WARNING("activeButton is -1 in RUNNING state - allowing pause anyway (button %d) - raw polling", i + 1);
                                    // Set activeButton to the pressed button to fix the tracking
                                    activeButton = i;
                                }
                                LOG_INFO("Pausing machine - button matches active button (raw polling)");
                                pauseMachine();
                                lastPauseResumeTime = currentTime;
                            }
                        } else {
                            // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                            lastActionTime = millis();
                            LOG_WARNING("Button %d pressed while RUNNING (activeButton=%d) - ignoring (raw polling)", 
                                       i + 1, activeButton + 1);
                        }
                    } else if (currentState == STATE_PAUSED) {
                        // CRITICAL FIX: Only resume if the button matches the active button
                        // This prevents accidentally activating a different button when paused
                        if (activeButton == -1 || i == activeButton) {
                            // CRITICAL FIX: Prevent rapid pause/resume toggling
                            unsigned long currentTime = millis();
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
                                if (activeButton == -1) {
                                    LOG_WARNING("activeButton is -1 in PAUSED state - allowing resume anyway (button %d) - raw polling", i + 1);
                                    // Set activeButton to the pressed button to fix the tracking
                                    activeButton = i;
                                }
                                LOG_INFO("Button %d: Resuming from PAUSED state", i + 1);
                                resumeMachine(i);
                                lastPauseResumeTime = currentTime;
                            }
                        } else {
                            // CRITICAL FIX: Reset inactivity timeout on ANY user action, even if ignored
                            lastActionTime = millis();
                            LOG_WARNING("Button %d pressed while PAUSED (activeButton=%d) - ignoring (must press same button to resume) - raw polling", 
                                       i + 1, activeButton + 1);
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
    publishActionEvent(activeButton, ACTION_PAUSE, MANUAL);
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
    
    // Publish resume event
    publishActionEvent(buttonIndex, ACTION_RESUME, MANUAL);
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
    
    // Publish stop event (only if we had an active button before stopping)
    if (buttonToStop >= 0) {
        publishActionEvent(buttonToStop, ACTION_STOP, triggerType);
    }
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

    publishActionEvent(buttonIndex, ACTION_START, triggerType);
}

void CarWashController::tokenExpired() {
    if (activeButton >= 0) {
        // Turn off the active relay
        extern IoExpander ioExpander;
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            ioExpander.setRelay(RELAY_INDICES[activeButton], false);
            xSemaphoreGive(xIoExpanderMutex);
        } else {
            LOG_WARNING("Failed to acquire IO expander mutex in tokenExpired()");
        }
    }
    activeButton = -1;
    currentState = STATE_IDLE;
    
    // CRITICAL FIX: If no tokens remain, finish immediately instead of waiting for inactivity timeout
    if (config.tokens <= 0) {
        LOG_INFO("Token expired and no tokens remaining (tokens=%d), finishing immediately", config.tokens);
        stopMachine(AUTOMATIC);
        return;
    }
    
    // CRITICAL FIX: Reset inactivity timeout when token expires and machine goes to IDLE
    // This gives the user a fresh timeout period to start a new token after the previous one finished
    lastActionTime = millis();
    // CRITICAL FIX: Reset pause/resume tracking when token expires to ensure clean state
    // This prevents stale pause/resume state from affecting the next button press
    lastPauseResumeTime = 0;
    tokenStartTime = 0;
    tokenTimeElapsed = 0;
    pauseStartTime = 0;
    // publishTokenExpiredEvent();
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
        
        currentState = STATE_IDLE;
        digitalWrite(LED_PIN_INIT, HIGH);
        
        LOG_INFO("COIN: Anonymous session created - sessionId='%s', userId='unknown', tokens=%d, state=IDLE", 
                config.sessionId.c_str(), config.tokens);
    }
    
    publishCoinInsertedEvent();
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
            
            bool inactivityExpired = (elapsedTime >= USER_INACTIVE_TIMEOUT);
            // LOG_INFO("[INACTIVITY_TIMEOUT] state=%d, isLoaded=%d, lastActionTime=%lu, currentTime=%lu, elapsedTime=%lu, USER_INACTIVE_TIMEOUT=%lu, expired=%d",
            //         currentState, config.isLoaded ? 1 : 0, lastActionTime, currentTime, 
            //         elapsedTime, USER_INACTIVE_TIMEOUT, inactivityExpired ? 1 : 0);
        } else {
            // LOG_INFO("[INACTIVITY_TIMEOUT] state=%d, isLoaded=%d (not active)", currentState, config.isLoaded ? 1 : 0);
        }
        
        lastTimeoutLogTime = currentTime;
    }
    
    // PRIORITY 0: Check inactivity timeout FIRST (highest priority - must happen immediately)
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
        
        if (elapsedTime >= USER_INACTIVE_TIMEOUT) {
            LOG_INFO("Inactivity timeout reached (%lu ms >= %lu ms), logging out user", elapsedTime, USER_INACTIVE_TIMEOUT);
            stopMachine(AUTOMATIC);
            // Return immediately after logout to ensure state change is processed
            // This prevents any blocking operations (like network I/O) from delaying the logout
            return;
        }
    }
    
    // PRIORITY 1: Handle buttons and coin acceptor (time-critical, must be responsive)
    // CRITICAL: Handle buttons BEFORE any blocking operations to ensure immediate response
    // CRITICAL FIX: Always handle buttons if machine is loaded, even if timestamp is empty
    // This ensures buttons work immediately after machine is loaded
    
    // Throttle log message to avoid spam (update() runs very frequently)
    static unsigned long lastButtonLogTime = 0;
    bool shouldLog = false;
     if (lastButtonLogTime == 0) {
        shouldLog = true;
        lastButtonLogTime = currentTime;
    } else {
        // Handle potential millis() overflow
        unsigned long elapsed;
        if (currentTime >= lastButtonLogTime) {
            elapsed = currentTime - lastButtonLogTime;
        } else {
            elapsed = (0xFFFFFFFFUL - lastButtonLogTime) + currentTime + 1;
        }
        if (elapsed > 5000) { // Log every 5 seconds max
            shouldLog = true;
            lastButtonLogTime = currentTime;
        }
    }
    
    if (shouldLog) {
        LOG_INFO("Handling buttons and coin acceptor - isLoaded=%d, timestamp='%s'", config.isLoaded, config.timestamp.c_str());
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
    
    // CRITICAL FIX: Re-capture currentTime after handling buttons
    // This prevents timing bugs where tokenStartTime is set AFTER currentTime was captured
    // If activateButton() was just called, tokenStartTime will be newer than the old currentTime
    // Re-capturing ensures we use the correct time for token expiration checks
    currentTime = millis();
    
    if ((currentState == STATE_RUNNING || currentState == STATE_PAUSED) && tokenStartTime != 0) {
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
        } else { // STATE_PAUSED
            // When paused, elapsed time is frozen at tokenTimeElapsed
            totalElapsedTime = tokenTimeElapsed;
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
                if (elapsedTime >= USER_INACTIVE_TIMEOUT) {
                    LOG_INFO("Inactivity timeout also reached after token expiry, logging out");
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
    
    // Include RTC status to help backend decide if time sync is needed
    if (rtcManager) {
        doc["rtc_valid"] = rtcManager->isTimeValid();
        if (rtcManager->isInitialized()) {
            doc["rtc_initialized"] = true;
        } else {
            doc["rtc_initialized"] = false;
        }
    } else {
        doc["rtc_valid"] = false;
        doc["rtc_initialized"] = false;
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Queue message for publishing (critical - machine setup event)
    queueMqttMessage(ACTION_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE, true);
}

unsigned long CarWashController::getSecondsLeft() {
    if (currentState != STATE_RUNNING && currentState != STATE_PAUSED) {
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
    } else { // PAUSED
        // When paused, elapsed time is frozen at tokenTimeElapsed
        totalElapsedTime = tokenTimeElapsed;
    }

    // Return 0 when elapsed >= timeout (matches the >= check in update())
    if (totalElapsedTime >= TOKEN_TIME) {
        return 0;
    }

    // Return remaining seconds (rounded down)
    unsigned long remainingMs = TOKEN_TIME - totalElapsedTime;
    return remainingMs / 1000;
}

String CarWashController::getTimestamp() {
    // Use RTC if available and initialized
    if (rtcManager && rtcManager->isInitialized()) {
        String rtcTimestamp = rtcManager->getTimestampWithMillis();
        // Return RTC timestamp if valid, otherwise fall through to default
        if (rtcTimestamp != "RTC Error" && rtcTimestamp.length() > 0) {
            return rtcTimestamp;
        }
    }
    
    // Return default timestamp if RTC is not available or invalid
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

    // Include seconds_left for both RUNNING and PAUSED states
    // (getSecondsLeft() handles both states correctly)
    if (currentState == STATE_RUNNING || currentState == STATE_PAUSED) {
        doc["seconds_left"] = getSecondsLeft();
    }

    String jsonString;
    serializeJson(doc, jsonString);

    // Queue message for publishing (critical - button action events)
    queueMqttMessage(ACTION_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE, true);
}

void CarWashController::publishPeriodicState(bool force) {
    unsigned long currentPublishTime = millis();
    // Handle potential millis() overflow for publish interval check
    unsigned long publishElapsed;
    if (currentPublishTime >= lastStatePublishTime) {
        publishElapsed = currentPublishTime - lastStatePublishTime;
    } else {
        publishElapsed = (0xFFFFFFFFUL - lastStatePublishTime) + currentPublishTime + 1;
    }
    
    if (force || publishElapsed >= STATE_PUBLISH_INTERVAL) {
        // CRITICAL: Re-check timeout before potentially blocking MQTT operation
        // This ensures timeout is not delayed by network I/O
        unsigned long currentTime = millis();
        if (currentState != STATE_FREE && config.isLoaded) {
            // Handle potential millis() overflow
            unsigned long elapsedTime;
            if (currentTime >= lastActionTime) {
                elapsedTime = currentTime - lastActionTime;
            } else {
                // Overflow occurred - calculate correctly
                elapsedTime = (0xFFFFFFFFUL - lastActionTime) + currentTime + 1;
            }
            if (elapsedTime >= USER_INACTIVE_TIMEOUT) {
                // Timeout reached - stop immediately and skip MQTT publish
                LOG_INFO("Inactivity timeout reached in publishPeriodicState (%lu ms >= %lu ms)", elapsedTime, USER_INACTIVE_TIMEOUT);
                stopMachine(AUTOMATIC);
                return;
            }
        }
        
        StaticJsonDocument<512> doc;
        doc["machine_id"] = MACHINE_ID;
        String timestamp = getTimestamp();
        doc["timestamp"] = timestamp;
        doc["status"] = getMachineStateString(currentState);

        if (config.isLoaded) {
            JsonObject sessionMetadata = doc.createNestedObject("session_metadata");
            sessionMetadata["session_id"] = config.sessionId;
            sessionMetadata["user_id"] = config.userId;
            sessionMetadata["user_name"] = config.userName;
            sessionMetadata["tokens_left"] = config.tokens;
            sessionMetadata["physical_tokens"] = config.physicalTokens;
            sessionMetadata["timestamp"] = config.timestamp;
            // Include seconds_left for RUNNING and PAUSED states
            // (getSecondsLeft() returns 0 for other states, so this is safe)
            if (currentState == STATE_RUNNING || currentState == STATE_PAUSED) {
                sessionMetadata["seconds_left"] = getSecondsLeft();
            }
        }

        String jsonString;
        serializeJson(doc, jsonString);
        
        // Log state publish attempt for debugging
        LOG_INFO("Publishing state: status=%s, timestamp=%s", 
                  getMachineStateString(currentState).c_str(), timestamp.c_str());
        
        // CRITICAL: State messages must be sent immediately, NOT queued
        // State messages represent the current machine state and should always reflect
        // the most recent state. Queuing them would cause stale state information
        // to be sent to the backend, which could lead to incorrect state tracking.
        // Use non-blocking publish with timeout to avoid blocking the main loop
        bool published = mqttClient.publishNonBlocking(STATE_TOPIC.c_str(), jsonString.c_str(), QOS0_AT_MOST_ONCE, 500);
        
        // Update lastStatePublishTime regardless of publish result
        // If publish failed, we'll try again on next update() call
        lastStatePublishTime = millis();
        
        if (!published) {
            LOG_WARNING("State publish failed (MQTT may be busy or disconnected), will retry on next update()");
        }
        
        // CRITICAL: Re-check timeout after potentially blocking MQTT operation
        // This ensures timeout is not missed even if MQTT publish blocked
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
            if (elapsedTime >= USER_INACTIVE_TIMEOUT) {
                LOG_INFO("Inactivity timeout reached after MQTT publish (%lu ms >= %lu ms)", elapsedTime, USER_INACTIVE_TIMEOUT);
                stopMachine(AUTOMATIC);
                return;
            }
        }
    }
}

void CarWashController::publishStateOnDemand() {
    // CRITICAL: Re-check timeout before potentially blocking MQTT operation
    // This ensures timeout is not delayed by network I/O
    unsigned long currentTime = millis();
    if (currentState != STATE_FREE && config.isLoaded) {
        // Handle potential millis() overflow
        unsigned long elapsedTime;
        if (currentTime >= lastActionTime) {
            elapsedTime = currentTime - lastActionTime;
        } else {
            // Overflow occurred - calculate correctly
            elapsedTime = (0xFFFFFFFFUL - lastActionTime) + currentTime + 1;
        }
        if (elapsedTime >= USER_INACTIVE_TIMEOUT) {
            // Timeout reached - stop immediately and skip MQTT publish
            LOG_INFO("Inactivity timeout reached in publishStateOnDemand (%lu ms >= %lu ms)", elapsedTime, USER_INACTIVE_TIMEOUT);
            stopMachine(AUTOMATIC);
            return;
        }
    }
    
    StaticJsonDocument<512> doc;
    doc["machine_id"] = MACHINE_ID;
    String timestamp = getTimestamp();
    doc["timestamp"] = timestamp;
    doc["status"] = getMachineStateString(currentState);

    if (config.isLoaded) {
        JsonObject sessionMetadata = doc.createNestedObject("session_metadata");
        sessionMetadata["session_id"] = config.sessionId;
        sessionMetadata["user_id"] = config.userId;
        sessionMetadata["user_name"] = config.userName;
        sessionMetadata["tokens_left"] = config.tokens;
        sessionMetadata["physical_tokens"] = config.physicalTokens;
        sessionMetadata["timestamp"] = config.timestamp;
        // Include seconds_left for RUNNING and PAUSED states
        // (getSecondsLeft() returns 0 for other states, so this is safe)
        if (currentState == STATE_RUNNING || currentState == STATE_PAUSED) {
            sessionMetadata["seconds_left"] = getSecondsLeft();
        }
    }

    String jsonString;
    serializeJson(doc, jsonString);
    
    // Log state publish attempt for debugging
    LOG_INFO("Publishing state on demand: status=%s, timestamp=%s", 
              getMachineStateString(currentState).c_str(), timestamp.c_str());
    
    // CRITICAL: On-demand state messages use QOS1 (AT_LEAST_ONCE) with high priority
    // This ensures the backend receives the state update reliably and quickly
    // Use longer timeout (1000ms) for high-priority messages
    bool published = mqttClient.publishNonBlocking(STATE_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE, 1000);
    
    // Update lastStatePublishTime to prevent immediate re-publish
    lastStatePublishTime = millis();
    
    if (!published) {
        LOG_WARNING("On-demand state publish failed (MQTT may be busy or disconnected)");
    } else {
        LOG_INFO("State published successfully on demand");
    }
    
    // CRITICAL: Re-check timeout after potentially blocking MQTT operation
    // This ensures timeout is not missed even if MQTT publish blocked
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
        if (elapsedTime >= USER_INACTIVE_TIMEOUT) {
            LOG_INFO("Inactivity timeout reached after on-demand MQTT publish (%lu ms >= %lu ms)", elapsedTime, USER_INACTIVE_TIMEOUT);
            stopMachine(AUTOMATIC);
            return;
        }
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
    
    // Return 0 when elapsed >= timeout (matches the >= check in update())
    // This ensures display shows 00:00 exactly when logout happens
    if (elapsedTime >= USER_INACTIVE_TIMEOUT) {
        return 0;
    }
    
    // Return remaining time in milliseconds
    unsigned long remaining = USER_INACTIVE_TIMEOUT - elapsedTime;
    return remaining;
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
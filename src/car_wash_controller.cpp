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
      lastCoinState(HIGH) {
          
    // Force a read of the coin signal pin at startup to initialize correctly
    extern IoExpander ioExpander;
    uint8_t rawPortValue0 = 0;
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
        xSemaphoreGive(xIoExpanderMutex);
    }
    
    // Initialize coin signal state correctly
    // When coin is present: Pin is LOW (bit=0) = ACTIVE
    // When no coin: Pin is HIGH (bit=1) = INACTIVE
    bool initialCoinSignal = ((rawPortValue0 & (1 << COIN_SIG)) == 0); // LOW = coin present = ACTIVE
    lastCoinState = initialCoinSignal ? LOW : HIGH; // Store as HIGH/LOW for edge detection
    
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

void CarWashController::setRTCManager(RTCManager* rtc) {
    rtcManager = rtc;
    LOG_INFO("RTC Manager connected to controller");
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
                        if (activeButton == -1) {
                            LOG_WARNING("activeButton is -1 in RUNNING state - allowing pause anyway (button %d)", detectedId + 1);
                            // Set activeButton to the pressed button to fix the tracking
                            activeButton = detectedId;
                        }
                        LOG_INFO("Pausing machine - button matches active button");
                        pauseMachine();
                        buttonProcessed = true;
                    } else {
                        LOG_WARNING("Button %d pressed while RUNNING (activeButton=%d) - ignoring", 
                                   detectedId + 1, activeButton + 1);
                    }
                } else if (currentState == STATE_PAUSED) {
                    resumeMachine(detectedId);
                    buttonProcessed = true;
                } else {
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
                            if (activeButton == -1) {
                                LOG_WARNING("activeButton is -1 in RUNNING state - allowing pause anyway (button %d) - raw polling", i + 1);
                                // Set activeButton to the pressed button to fix the tracking
                                activeButton = i;
                            }
                            LOG_INFO("Pausing machine - button matches active button (raw polling)");
                            pauseMachine();
                        } else {
                            LOG_WARNING("Button %d pressed while RUNNING (activeButton=%d) - ignoring (raw polling)", 
                                       i + 1, activeButton + 1);
                        }
                    } else if (currentState == STATE_PAUSED) {
                        LOG_INFO("Button %d: Resuming from PAUSED state", i + 1);
                        resumeMachine(i);
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
    activeButton = buttonIndex;
    
    extern IoExpander ioExpander;
    
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Turn on the relay for the active button
        ioExpander.setRelay(RELAY_INDICES[buttonIndex], true);
        
        // Verify relay state after activation
        uint8_t relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
        xSemaphoreGive(xIoExpanderMutex);
        
        // Check if relay bit was actually set
        bool relayBitSet = (relayStateAfter & (1 << RELAY_INDICES[buttonIndex])) != 0;
        if (!relayBitSet) {
            LOG_ERROR("Failed to activate relay %d for resume!", buttonIndex+1);
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
    
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Turn on the corresponding relay
        ioExpander.setRelay(RELAY_INDICES[buttonIndex], true);
        
        // Verify relay state after activation
        uint8_t relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
        xSemaphoreGive(xIoExpanderMutex);
        
        // Check if relay bit was actually set
        bool relayBitSet = (relayStateAfter & (1 << RELAY_INDICES[buttonIndex])) != 0;
        if (!relayBitSet) {
            LOG_ERROR("Failed to activate relay %d! Bit %d not set", buttonIndex+1, RELAY_INDICES[buttonIndex]);
        }
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
    // CRITICAL FIX: Do NOT reset lastActionTime here - inactivity timeout should continue
    // from the last actual user action, not reset when token expires
    // This ensures the inactivity timeout works correctly on subsequent timeouts
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
    static unsigned long startupEndTime = 0;
    if (startupPeriod) {
        if (currentTime < 5000) { // Skip first 5 seconds
            return;
        }
        // Re-initialize coin state at the end of startup period to avoid false edge detection
        uint8_t rawPortValue0 = 0;
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
            xSemaphoreGive(xIoExpanderMutex);
        }
        bool initialCoinSignal = ((rawPortValue0 & (1 << COIN_SIG)) == 0);
        lastCoinState = initialCoinSignal ? LOW : HIGH;
        lastCoinProcessedTime = currentTime; // Reset cooldown timer
        startupEndTime = currentTime;
        startupPeriod = false;
        return; // Skip this cycle to avoid immediate false detection
    }
    
    // Additional grace period after startup - ignore any edge detection for 1 second after startup ends
    if (startupEndTime > 0 && (currentTime - startupEndTime) < 1000) {
        // Re-read and update state during grace period to prevent false edge detection
        uint8_t rawPortValue0 = 0;
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
            xSemaphoreGive(xIoExpanderMutex);
        }
        bool currentCoinSignal = ((rawPortValue0 & (1 << COIN_SIG)) == 0);
        lastCoinState = currentCoinSignal ? LOW : HIGH;
        return;
    }
    startupEndTime = 0; // Clear grace period flag after it expires
    
    // FIXED: Check if the interrupt handler detected a coin signal
    if (ioExpander.isCoinSignalDetected()) {
        uint8_t rawPortValue0 = 0;
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
            ioExpander.clearCoinSignalFlag();
            xSemaphoreGive(xIoExpanderMutex);
        } else {
            LOG_WARNING("Failed to acquire IO expander mutex in handleCoinAcceptor()");
            return;
        }
        
        // Only process the coin if it's been long enough since the last coin
        if (currentTime - lastCoinProcessedTime > COIN_PROCESS_COOLDOWN) {
            processCoinInsertion(currentTime);
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
        LOG_WARNING("Failed to acquire IO expander mutex in handleCoinAcceptor() fallback");
        return;
    }
    
    // Get current state of coin signal pin with correct logic
    // When coin is present: Pin is LOW (bit=0) = ACTIVE
    // When no coin: Pin is HIGH (bit=1) = INACTIVE
    bool coinSignalActive = ((rawPortValue0 & (1 << COIN_SIG)) == 0);
    
    // Static variables to track edges and timing patterns
    static unsigned long lastEdgeTime = 0;
    static int edgeCount = 0;
    static unsigned long edgeWindowStart = 0;
    
    // Signal-based edge detection (COIN_SIG pin only)
    // Convert lastCoinState from HIGH/LOW to boolean for comparison
    bool lastCoinStateBool = (lastCoinState == LOW);
    
    if (coinSignalActive != lastCoinStateBool) {
        unsigned long timeSinceLastEdge = currentTime - lastEdgeTime;
        lastEdgeTime = currentTime;
        
        // Multi-edge detection logic for coins that generate multiple pulses
        // Track ALL edges (both rising and falling) to detect coin patterns
        if (edgeCount == 0 || currentTime - edgeWindowStart > 1000) {
            edgeWindowStart = currentTime;
            edgeCount = 1;
        } else {
            edgeCount++;
            
            unsigned long windowDuration = currentTime - edgeWindowStart;
            
            if (edgeCount >= COIN_MIN_EDGES && windowDuration < COIN_EDGE_WINDOW && 
                currentTime - lastCoinProcessedTime > COIN_PROCESS_COOLDOWN) {
                
                processCoinInsertion(currentTime);
                edgeCount = 0;
                edgeWindowStart = 0;
            }
            
            if (edgeCount > 10) {
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
            if (currentTime - lastCoinProcessedTime > COIN_PROCESS_COOLDOWN) {
                processCoinInsertion(currentTime);
                edgeCount = 0; // Reset edge count after valid coin
                edgeWindowStart = 0; // Reset window
            }
        }
        
        // Update lastCoinState (store as HIGH/LOW)
        lastCoinState = coinSignalActive ? LOW : HIGH;
    }
    
    // Reset edge detection if it's been too long
    if (edgeCount > 0 && currentTime - edgeWindowStart > 1000) {
        edgeCount = 0;
        edgeWindowStart = 0;
    }
}

// Helper method to handle the business logic of a coin insertion
void CarWashController::processCoinInsertion(unsigned long currentTime) {
    // Update activity tracking - crucial for debouncing and cooldown periods
    lastActionTime = currentTime;
    lastCoinProcessedTime = currentTime; // This is critical for the cooldown between coins
    
    // Update or create session
    if (config.isLoaded) {
        config.physicalTokens++;
        config.tokens++;
    } else {
        
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
            LOG_INFO("[TOKEN_TIMEOUT] state=%d, tokenStartTime=%lu, tokenTimeElapsed=%lu, currentTime=%lu, runningTime=%lu, totalElapsedTime=%lu, TOKEN_TIME=%lu, expired=%d",
                    currentState, tokenStartTime, tokenTimeElapsed, currentTime, 
                    (currentState == STATE_RUNNING) ? runningTime : 0UL, 
                    totalElapsedTime, TOKEN_TIME, tokenExpired ? 1 : 0);
        } else {
            LOG_INFO("[TOKEN_TIMEOUT] state=%d, tokenStartTime=%lu (not active)", currentState, tokenStartTime);
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
            LOG_INFO("[INACTIVITY_TIMEOUT] state=%d, isLoaded=%d, lastActionTime=%lu, currentTime=%lu, elapsedTime=%lu, USER_INACTIVE_TIMEOUT=%lu, expired=%d",
                    currentState, config.isLoaded ? 1 : 0, lastActionTime, currentTime, 
                    elapsedTime, USER_INACTIVE_TIMEOUT, inactivityExpired ? 1 : 0);
        } else {
            LOG_INFO("[INACTIVITY_TIMEOUT] state=%d, isLoaded=%d (not active)", currentState, config.isLoaded ? 1 : 0);
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
    
    if (config.isLoaded) {
        handleButtons();
        handleCoinAcceptor();
    } else {
        // Log when buttons are skipped (only in debug mode to avoid spam)
        static unsigned long lastSkipLog = 0;
        if (currentTime - lastSkipLog > 5000) { // Log every 5 seconds max
            LOG_INFO("Skipping button handling - machine not loaded (isLoaded=%d, timestamp empty=%d)", 
                     config.isLoaded, config.timestamp.length() == 0);
            lastSkipLog = currentTime;
        }
    }
    
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
    
    // PRIORITY 3: Publish periodic state LAST (non-critical, can wait, may block for network I/O)
    // This allows monitoring of machine status even before initialization
    // CRITICAL: This may block, so it's done last to not delay button/timeout handling
    // CRITICAL: Re-check timeout before potentially blocking MQTT operation
    publishPeriodicState();
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
        
        // Queue message for publishing (non-critical - periodic state updates)
        // State updates are not critical and can be dropped if queue is full
        bool queued = queueMqttMessage(STATE_TOPIC.c_str(), jsonString.c_str(), QOS0_AT_MOST_ONCE, false);
        
        // Only update lastStatePublishTime if message was successfully queued
        // If queue is full and message was dropped, we'll try again on next update() call
        // This prevents skipping state updates when queue is temporarily full
        if (queued) {
            lastStatePublishTime = millis();
        } else {
            LOG_WARNING("State publish queued failed (queue full), will retry on next update()");
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
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
        config.physicalTokens = 0;  // No physical tokens when session is initialized remotely
        config.timestamp = doc["timestamp"].as<String>();
        
        // Only store timestampMillis if RTC not available (for fallback timestamp calculation)
        // If RTC is available, we don't need millis() offset since RTC is the primary time source
        if (rtcManager && rtcManager->isInitialized()) {
            config.timestampMillis = 0;  // Not needed, RTC handles timestamps
        } else {
            config.timestampMillis = millis();  // Store for fallback timestamp calculation
        }
        
        // Sync RTC with server timestamp if RTC is available
        if (rtcManager && rtcManager->isInitialized() && config.timestamp.length() > 0) {
            LOG_INFO("Syncing RTC with server timestamp: %s", config.timestamp.c_str());
            if (rtcManager->setDateTimeFromISO(config.timestamp)) {
                LOG_INFO("RTC synchronized successfully!");
                rtcManager->printDebugInfo();
            } else {
                LOG_WARNING("Failed to sync RTC with server timestamp");
            }
        }
        
        config.isLoaded = true;
        currentState = STATE_IDLE;
        lastActionTime = millis();
        LOG_INFO("Switching on LED");
        digitalWrite(LED_PIN_INIT, HIGH);
        LOG_INFO("Machine loaded with new configuration");
    } else if (String(topic) == CONFIG_TOPIC) {
        LOG_INFO("Received config message from server");
        config.timestamp = doc["timestamp"].as<String>();
        
        // Only store timestampMillis if RTC not available (for fallback timestamp calculation)
        // If RTC is available, we don't need millis() offset since RTC is the primary time source
        if (rtcManager && rtcManager->isInitialized()) {
            config.timestampMillis = 0;  // Not needed, RTC handles timestamps
        } else {
            config.timestampMillis = millis();  // Store for fallback timestamp calculation
        }
        
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

        // Explicit check: Buttons should not work when machine is FREE
        // This ensures buttons are ignored even if config.isLoaded is somehow true
        if (currentState == STATE_FREE) {
            LOG_WARNING("Button %d press ignored - machine is FREE (config.isLoaded=%d)", 
                       detectedId + 1, config.isLoaded);
            ioExpander.clearButtonFlag();
            return; // Skip processing and raw polling
        }

        // Function buttons (0..NUM_BUTTONS-2)
        if (detectedId < NUM_BUTTONS - 1) {
            if (config.isLoaded) {
                if (currentState == STATE_IDLE) {
                    activateButton(detectedId, MANUAL);
                    buttonProcessed = true;
                } else if (currentState == STATE_RUNNING && (int)detectedId == activeButton) {
                    pauseMachine();
                    buttonProcessed = true;
                } else if (currentState == STATE_PAUSED) {
                    resumeMachine(detectedId);
                    buttonProcessed = true;
                } else if (currentState == STATE_FREE) {
                    // This should never happen due to early return above, but keep for safety
                    LOG_WARNING("Flag press on button %d ignored - machine is FREE", detectedId + 1);
                    buttonProcessed = true; // Clear flag
                } else {
                    LOG_WARNING("Flag press on button %d ignored. State=%d, activeButton=%d",
                                detectedId + 1, currentState, activeButton);
                    // Config is loaded but state is wrong - clear flag to prevent infinite retries
                    buttonProcessed = true; // Mark as "processed" (even though ignored) to clear flag
                }
            } else {
                LOG_WARNING("Button flag ignored - config not loaded, will retry on next update");
                // Don't clear flag - keep it so we can process it when config is loaded
                return; // Skip raw polling and return early
            }
        } else if (detectedId == NUM_BUTTONS - 1) {
            // Stop button
            if (config.isLoaded) {
                stopMachine(MANUAL);
                buttonProcessed = true;
            } else {
                LOG_WARNING("STOP button flag ignored - config not loaded, will retry on next update");
                // Don't clear flag - keep it so we can process it when config is loaded
                return; // Skip raw polling and return early
            }
        }

        // Only clear the flag if we successfully processed the button
        // This prevents losing button presses when machine is in wrong state
        if (buttonProcessed) {
            ioExpander.clearButtonFlag();
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
            if (lastButtonState[i] == HIGH || 
                (millis() - lastDebounceTime[i]) > DEBOUNCE_DELAY * 5) {
                
                // Record time of this press
                lastDebounceTime[i] = millis();
                lastButtonState[i] = LOW;  // Now pressed (active LOW)
                
                // Explicit check: Buttons should not work when machine is FREE
                if (currentState == STATE_FREE) {
                    continue; // Skip this button, check others
                }
                
                // Process button action
                if (config.isLoaded) {
                    if (currentState == STATE_IDLE) {
                        activateButton(i, MANUAL);
                    } else if (currentState == STATE_RUNNING && i == activeButton) {
                        pauseMachine();
                    } else if (currentState == STATE_PAUSED) {
                        resumeMachine(i);
                    }
                }
            }
        } else {
            // Button is released
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
    tokenTimeElapsed += (pauseStartTime - tokenStartTime);
   
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
    // Serial.println("CarWashController::update");
    
    unsigned long currentTime = millis();
    
    // PRIORITY 0: Check inactivity timeout FIRST (highest priority - must happen immediately)
    // This ensures logout happens as soon as timeout is reached, before any other operations
    // CRITICAL: Check this before anything else to prevent delays from blocking operations
    if (currentState != STATE_FREE && config.isLoaded) {
        unsigned long elapsedTime = currentTime - lastActionTime;
        if (elapsedTime >= USER_INACTIVE_TIMEOUT) {
            stopMachine(AUTOMATIC);
            // Return immediately after logout to ensure state change is processed
            // This prevents any blocking operations (like network I/O) from delaying the logout
            return;
        }
    }
    
    // PRIORITY 1: Handle buttons and coin acceptor (time-critical, must be responsive)
    // Skip session-related updates if not initialized
    if (config.timestamp != "") {
        handleButtons();
        handleCoinAcceptor();
    }
    
    // PRIORITY 2: Handle state machine timeouts (time-critical)
    if (currentState == STATE_RUNNING) {
        unsigned long totalElapsedTime = tokenTimeElapsed + (currentTime - tokenStartTime);
        if (totalElapsedTime >= TOKEN_TIME) {
            tokenExpired();
            // Don't return here - still publish state after token expires
        }
    }
    
    // PRIORITY 3: Publish periodic state LAST (non-critical, can wait, may block for network I/O)
    // This allows monitoring of machine status even before initialization
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
    // PRIORITY 1: Use RTC if available and initialized (even if time validation fails)
    // This ensures we always have a current timestamp for state updates
    if (rtcManager && rtcManager->isInitialized()) {
        String rtcTimestamp = rtcManager->getTimestampWithMillis();
        // Only use RTC if it returns a valid timestamp (not "RTC Error")
        if (rtcTimestamp != "RTC Error" && rtcTimestamp.length() > 0) {
            // Log warning if time is invalid but still use it (better than placeholder)
            if (!rtcManager->isTimeValid()) {
                static bool rtcWarningLogged = false;
                if (!rtcWarningLogged) {
                    rtcWarningLogged = true;
                    LOG_WARNING("RTC time is invalid but using it anyway (better than placeholder)");
                }
            }
            return rtcTimestamp;
        }
    }
    
    // FALLBACK: Use millis()-based calculation if RTC is not available
    // If timestamp is empty, we can't calculate relative time, so use RTC even if invalid
    if (config.timestamp.length() == 0) {
        // Try RTC one more time even if not validated - better than 2000 placeholder
        if (rtcManager && rtcManager->isInitialized()) {
            String rtcTimestamp = rtcManager->getTimestampWithMillis();
            if (rtcTimestamp != "RTC Error" && rtcTimestamp.length() > 0) {
                LOG_WARNING("Using RTC timestamp without server sync (config.timestamp empty)");
                return rtcTimestamp;
            }
        }
        // Last resort: return placeholder but log error
        LOG_ERROR("Cannot generate timestamp: RTC not available and config.timestamp is empty");
        return "2000-01-01T00:00:00.000Z";  // Invalid timestamp placeholder
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
    
    // Calculate time elapsed since timestamp was set
    // Handle millis() overflow safely (overflow occurs every ~50 days on ESP32)
    unsigned long millisOffset = 0;
    if (config.timestampMillis > 0) {
        unsigned long currentMillis = millis();
        // Check for overflow: if currentMillis < config.timestampMillis, overflow occurred
        if (currentMillis >= config.timestampMillis) {
            // Normal case: no overflow
            millisOffset = currentMillis - config.timestampMillis;
        } else {
            // Overflow occurred - calculate correctly accounting for wraparound
            // On ESP32, unsigned long is 32-bit, so max value is 4,294,967,295
            // Formula: (max_value - old_value) + new_value + 1
            const unsigned long MAX_ULONG = 0xFFFFFFFFUL;  // 4,294,967,295
            millisOffset = (MAX_ULONG - config.timestampMillis) + currentMillis + 1;
            
            // Safety check: if more than 2 days have passed, timestamp is probably stale
            // This could indicate a serious problem (power loss, system restart, etc.)
            if (millisOffset > 86400000UL * 2) {  // 2 days in milliseconds
                LOG_ERROR("Timestamp calculation error: millis() overflow detected and timestamp appears stale (>2 days)");
            }
        }
    }
    
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

    mqttClient.publish(ACTION_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE);
}

void CarWashController::publishPeriodicState(bool force) {
    if (force || millis() - lastStatePublishTime >= STATE_PUBLISH_INTERVAL) {
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
        LOG_DEBUG("Publishing state: status=%s, timestamp=%s", 
                  getMachineStateString(currentState).c_str(), timestamp.c_str());
        
        bool published = mqttClient.publish(STATE_TOPIC.c_str(), jsonString.c_str(), QOS0_AT_MOST_ONCE);
        if (!published) {
            LOG_WARNING("Failed to publish state update to MQTT");
        }

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
    
    unsigned long currentTime = millis();
    unsigned long elapsedTime = currentTime - lastActionTime;
    unsigned long remaining = (elapsedTime >= USER_INACTIVE_TIMEOUT) ? 0 : (USER_INACTIVE_TIMEOUT - elapsedTime);
    
    // No logging to reduce overhead
    
    // Return 0 when elapsed >= timeout (matches the >= check in update())
    // This ensures display shows 00:00 exactly when logout happens
    if (elapsedTime >= USER_INACTIVE_TIMEOUT) {
        return 0;
    }
    
    // Return remaining time in milliseconds
    return remaining;
}

// getTokensLeft and getUserName are implemented as inline methods in the header
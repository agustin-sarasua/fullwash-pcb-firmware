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
    
    // EXTENSIVE DEBUG: Log the raw values in different formats
    LOG_INFO("STARTUP DEBUG - Raw port value: 0x%02X | Binary: %d%d%d%d%d%d%d%d", 
           rawPortValue0,
           (rawPortValue0 & 0x80) ? 1 : 0, (rawPortValue0 & 0x40) ? 1 : 0,
           (rawPortValue0 & 0x20) ? 1 : 0, (rawPortValue0 & 0x10) ? 1 : 0,
           (rawPortValue0 & 0x08) ? 1 : 0, (rawPortValue0 & 0x04) ? 1 : 0,
           (rawPortValue0 & 0x02) ? 1 : 0, (rawPortValue0 & 0x01) ? 1 : 0);
    LOG_INFO("STARTUP DEBUG - COIN_SIG (bit %d) = %d", COIN_SIG, (rawPortValue0 & (1 << COIN_SIG)) ? 1 : 0);
    
    // Initialize coin signal state correctly
    // When coin is present: Pin is LOW (bit=0) = ACTIVE
    // When no coin: Pin is HIGH (bit=1) = INACTIVE
    bool initialCoinSignal = ((rawPortValue0 & (1 << COIN_SIG)) == 0); // LOW = coin present = ACTIVE
    lastCoinState = initialCoinSignal ? LOW : HIGH; // Store as HIGH/LOW for edge detection
    LOG_INFO("Coin detector initialized with state: %s", initialCoinSignal ? "ACTIVE (LOW)" : "INACTIVE (HIGH)");
    
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
                
                LOG_INFO("Button %d pressed and debounced!", i+1);
                
                // Process button action
                if (config.isLoaded) {
                    LOG_INFO("Button %d: config.isLoaded=true, currentState=%d, tokens=%d", 
                            i+1, currentState, config.tokens);
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
                    LOG_WARNING("Button press ignored - config not loaded! (config.isLoaded=false)");
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
            
            // Print debug info
            Serial.printf("** STOP BUTTON PRESSED AND DEBOUNCED **\n");
            LOG_INFO("STOP button pressed and debounced!");
            
            // Process button action - stop button should log out user whenever machine is loaded
            if (config.isLoaded) {
                LOG_INFO("Logging out user via STOP button (currentState=%d)", currentState);
                stopMachine(MANUAL);
            } else {
                LOG_WARNING("STOP button pressed but ignored - machine not loaded (config.isLoaded=false)");
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
        uint8_t relayStateBefore = 0;
        uint8_t relayStateAfter = 0;
        
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            relayStateBefore = ioExpander.readRegister(OUTPUT_PORT1);
            LOG_DEBUG("Pausing - Relay port state before: 0x%02X", relayStateBefore);
            
            // Turn off the active relay
            ioExpander.setRelay(RELAY_INDICES[activeButton], false);
            
            // Verify relay state after deactivation
            relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
            xSemaphoreGive(xIoExpanderMutex);
        } else {
            LOG_WARNING("Failed to acquire IO expander mutex in pauseMachine()");
            return;
        }
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
    uint8_t relayStateBefore = 0;
    uint8_t relayStateAfter = 0;
    
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        relayStateBefore = ioExpander.readRegister(OUTPUT_PORT1);
        LOG_DEBUG("Resuming - Relay port state before: 0x%02X", relayStateBefore);
       
        // Turn on the relay for the active button
        ioExpander.setRelay(RELAY_INDICES[buttonIndex], true);
        
        // Verify relay state after activation
        relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
        xSemaphoreGive(xIoExpanderMutex);
    } else {
        LOG_WARNING("Failed to acquire IO expander mutex in resumeMachine()");
        return;
    }
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
        uint8_t relayStateBefore = 0;
        uint8_t relayStateAfter = 0;
        
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            relayStateBefore = ioExpander.readRegister(OUTPUT_PORT1);
            LOG_DEBUG("Stopping - Relay port state before: 0x%02X", relayStateBefore);
            
            // Turn off the active relay
            ioExpander.setRelay(RELAY_INDICES[activeButton], false);
            
            // Verify relay state after deactivation
            relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
            xSemaphoreGive(xIoExpanderMutex);
        } else {
            LOG_WARNING("Failed to acquire IO expander mutex in stopMachine()");
        }
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
    LOG_INFO("Activating button: %d (triggerType=%s)", buttonIndex+1, triggerType == MANUAL ? "MANUAL" : "AUTOMATIC");

    if (config.tokens <= 0) {
        LOG_WARNING("Cannot activate button %d - no tokens left (tokens=%d)", buttonIndex+1, config.tokens);
        return;
    }
    
    LOG_INFO("Activating button %d: tokens=%d, state=%d -> STATE_RUNNING", buttonIndex+1, config.tokens, currentState);

    digitalWrite(RUNNING_LED_PIN, HIGH);
    currentState = STATE_RUNNING;
    activeButton = buttonIndex;
    
    // Debug before turning on relay
    LOG_DEBUG("Attempting to activate relay %d (relay index: %d)", buttonIndex+1, RELAY_INDICES[buttonIndex]);
    
    // Read current relay state before activation
    extern IoExpander ioExpander;
    uint8_t relayStateBefore = 0;
    uint8_t relayStateAfter = 0;
    
    if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        relayStateBefore = ioExpander.readRegister(OUTPUT_PORT1);
        LOG_DEBUG("Relay port state before activation: 0x%02X", relayStateBefore);
        
        // Turn on the corresponding relay
        ioExpander.setRelay(RELAY_INDICES[buttonIndex], true);
        
        // Verify relay state after activation
        relayStateAfter = ioExpander.readRegister(OUTPUT_PORT1);
        xSemaphoreGive(xIoExpanderMutex);
    } else {
        LOG_WARNING("Failed to acquire IO expander mutex in activateButton()");
        return;
    }
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
        LOG_INFO("Coin detector startup period over, now actively monitoring");
        LOG_INFO("Coin signal state re-initialized: %s", initialCoinSignal ? "ACTIVE (LOW)" : "INACTIVE (HIGH)");
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
        LOG_INFO("Interrupt-based coin signal detected!");
        
        // Read the current state to get more details
        uint8_t rawPortValue0 = 0;
        if (xIoExpanderMutex != NULL && xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
            ioExpander.clearCoinSignalFlag();
            xSemaphoreGive(xIoExpanderMutex);
        } else {
            LOG_WARNING("Failed to acquire IO expander mutex in handleCoinAcceptor()");
            return;
        }
        
        // For your hardware configuration, 3.3V = active coin, 0.05V = no coin
        // When a coin passes (3.3V), the TCA9535 reads LOW (bit=0)
        // When no coin is present (0.05V), the TCA9535 reads HIGH (bit=1)
        bool coinSignalActive = ((rawPortValue0 & (1 << COIN_SIG)) == 0);
        
        LOG_DEBUG("Coin signal state: %s", 
                coinSignalActive ? "ACTIVE (LOW/0 - coin present)" : "INACTIVE (HIGH/1 - no coin)");
        
        // Only process the coin if it's been long enough since the last coin
        if (currentTime - lastCoinProcessedTime > COIN_PROCESS_COOLDOWN) {
            LOG_INFO("Processing coin from interrupt detection");
            processCoinInsertion(currentTime);
        } else {
            LOG_INFO("Ignoring coin signal - too soon after last coin (%lu ms)",
                    currentTime - lastCoinProcessedTime);
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
        LOG_INFO("Coin signal edge: %s -> %s", 
                lastCoinStateBool ? "ACTIVE (coin present, LOW/0)" : "INACTIVE (no coin, HIGH/1)", 
                coinSignalActive ? "ACTIVE (coin present, LOW/0)" : "INACTIVE (no coin, HIGH/1)");
        
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
                
                LOG_INFO("Detected coin pattern: %d edges in %lu ms window", 
                        edgeCount, windowDuration);
                
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
            LOG_INFO("COIN INSERTED - Pin went from INACTIVE (HIGH/1) to ACTIVE (LOW/0)");
            
            if (currentTime - lastCoinProcessedTime > COIN_PROCESS_COOLDOWN) {
                LOG_INFO("Processing coin insertion");
                processCoinInsertion(currentTime);
                edgeCount = 0; // Reset edge count after valid coin
                edgeWindowStart = 0; // Reset window
            } else {
                LOG_DEBUG("Ignoring coin signal - too soon after last coin (%lu ms ago)",
                        currentTime - lastCoinProcessedTime);
            }
        }
        
        // Update lastCoinState (store as HIGH/LOW)
        lastCoinState = coinSignalActive ? LOW : HIGH;
    }
    
    // Reset edge detection if it's been too long
    if (edgeCount > 0 && currentTime - edgeWindowStart > 1000) {
        LOG_DEBUG("Resetting incomplete edge pattern after timeout");
        edgeCount = 0;
        edgeWindowStart = 0;
    }
    
    // Periodic debug logging
    static unsigned long lastDebugTime = 0;
    if (currentTime - lastDebugTime > 5000) {
        lastDebugTime = currentTime;
        LOG_DEBUG("Coin acceptor: Signal=%s, EdgeCount=%d, LastProcess=%lums ago", 
                coinSignalActive ? "ACTIVE (LOW/0)" : "INACTIVE (HIGH/1)",
                edgeCount,
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
    
    // Always publish periodic state even if not configured
    // This allows monitoring of machine status before initialization
    unsigned long currentTime = millis();
    publishPeriodicState();
    
    // Skip session-related updates if not initialized
    if (config.timestamp == "") {
        return;
    }
    
    // Handle buttons and coin acceptor
    handleButtons();
    handleCoinAcceptor();
    
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
    // PRIORITY 1: Use RTC if available, initialized, AND time is valid
    if (rtcManager && rtcManager->isInitialized() && rtcManager->isTimeValid()) {
        String rtcTimestamp = rtcManager->getTimestampWithMillis();
        LOG_DEBUG("Using RTC timestamp: %s", rtcTimestamp.c_str());
        return rtcTimestamp;
    }
    
    // Log why RTC is not being used
    if (rtcManager && rtcManager->isInitialized() && !rtcManager->isTimeValid()) {
        time_t rtcTime = rtcManager->getDateTime();
        LOG_WARNING("RTC is initialized but time is invalid (epoch: %lu). Falling back to millis() based timestamp", 
                   (unsigned long)rtcTime);
        LOG_WARNING("RTC timestamp would be: %s", rtcManager->getTimestampWithMillis().c_str());
    } else if (!rtcManager || !rtcManager->isInitialized()) {
        LOG_DEBUG("RTC not available or not initialized, using millis() based timestamp");
    }
    
    // FALLBACK: Use millis()-based calculation if RTC is not available or invalid
    LOG_DEBUG("Raw timestamp: %s", config.timestamp.c_str());
    LOG_DEBUG("Timestamp millis: %lu", config.timestampMillis);
    LOG_DEBUG("Current millis: %lu", millis());
    
    // If timestamp is empty, return early
    if (config.timestamp.length() == 0) {
        LOG_WARNING("Cannot generate timestamp: RTC time invalid and config.timestamp is empty. "
                   "Machine needs to receive INIT or CONFIG message to set timestamp.");
        // Return a placeholder that indicates the issue
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
                // Still return the calculation, but log the warning
            } else {
                LOG_DEBUG("Millis overflow detected, calculated offset: %lu ms", millisOffset);
            }
        }
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
    
    LOG_DEBUG("Formatted timestamp (millis-based): %s", isoTimestamp);
    
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
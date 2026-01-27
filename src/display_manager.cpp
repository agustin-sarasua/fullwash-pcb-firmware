#include "display_manager.h"
#include "utilities.h"
#include "constants.h"

DisplayManager::DisplayManager(uint8_t sdaPin, uint8_t sclPin)
    : _lastState(STATE_FREE), _lastSecondsLeft(0), _lastTokensLeft(0), 
      _lastUpdateTime(0), _i2cMutex(NULL) {
    
    // Wire1 should be initialized in main.cpp before creating DisplayManager
    _wire = &Wire1;
    
    LOG_INFO("Initializing 7-segment display on pins SDA=%d, SCL=%d", sdaPin, sclPin);
    
    // Create the CH453S driver
    _display = new CH453SDriver(*_wire);
    
    // Initialize the display (mutex will be set later via setI2CMutex)
    if (_display->begin(10)) {  // Medium brightness
        LOG_INFO("CH453S 7-segment display initialized successfully");
        displayInit();
    } else {
        LOG_ERROR("Failed to initialize CH453S 7-segment display!");
    }
}

void DisplayManager::setI2CMutex(SemaphoreHandle_t mutex) {
    _i2cMutex = mutex;
    if (_display) {
        _display->setI2CMutex(mutex);
    }
}

void DisplayManager::update(CarWashController* controller) {
    if (!controller || !_display) return;
    
    unsigned long now = millis();
    MachineState currentState = controller->getCurrentState();
    bool stateChanged = (currentState != _lastState);
    
    // Always update immediately when state changes
    if (stateChanged) {
        _lastUpdateTime = now;
    } else {
        // Throttle updates to every 500ms for smooth countdown display
        if (now - _lastUpdateTime < 500) {
            return;
        }
        _lastUpdateTime = now;
    }
    
    // Update display based on current state
    switch (currentState) {
        case STATE_FREE:
            displayFreeState();
            break;
        case STATE_IDLE:
            displayIdleState(controller);
            break;
        case STATE_RUNNING:
            displayRunningState(controller);
            break;
        case STATE_PAUSED:
            displayPausedState(controller);
            break;
        default:
            LOG_WARNING("[DISPLAY] Unknown state %d, showing FREE", currentState);
            displayFreeState();
            break;
    }
    
    _lastState = currentState;
}

void DisplayManager::clearAll() {
    if (_display) {
        _display->clear();
    }
}

void DisplayManager::displayInit() {
    if (_display) {
        // Show dashes during initialization
        _display->displayDashes(true);   // Top display
        _display->displayDashes(false);  // Bottom display
    }
}

void DisplayManager::displayError() {
    if (_display) {
        // Show "Err" pattern or dashes
        _display->setCharacter(0, 'E', false);
        _display->setCharacter(1, 'r', false);
        _display->setCharacter(2, 'r', false);
        _display->setCharacter(3, ' ', false);
        _display->displayDashes(false);
    }
}

void DisplayManager::setBrightness(uint8_t brightness) {
    if (_display) {
        _display->setBrightness(brightness);
    }
}

void DisplayManager::displayFreeState() {
    if (!_display) return;
    
    // Clear displays when machine is free
    // Show "----" to indicate ready state or blank
    // Top display: Coins, Bottom display: Time
    _display->displayDashes(true);   // Top: "----" (coins)
    _display->displayDashes(false);  // Bottom: "----" (time)

    // IMPORTANT: reset cached values so that when a new session starts right after a timeout,
    // the next IDLE update forces a redraw even if the numeric value matches the previous one
    // (e.g., 1 token -> 120s could be the same value as just before timing out).
    _lastSecondsLeft = 0xFFFFFFFFUL;
    _lastTokensLeft = -10000.0f;
}

void DisplayManager::displayIdleState(CarWashController* controller) {
    if (!_display || !controller) return;
    
    // Get time and tokens info
    unsigned long secondsLeft = controller->getSecondsLeft();
    int tokensLeft = controller->getTokensLeft();
    unsigned long gracePeriodSeconds = controller->getGracePeriodSecondsLeft();
    
    // Grace period active:
    // - No token countdown is running yet (controller tokenStartTime is 0),
    // - Display should show full tokens and full time.
    if (gracePeriodSeconds > 0) {
        secondsLeft = (tokensLeft * TOKEN_TIME) / 1000;
        updateTimeDisplay(secondsLeft);
        updateTokensDisplay(tokensLeft, 0, TOKEN_TIME / 1000);
        return;
    }

    // Grace period JUST expired but controller hasn't started the 2-minute countdown yet.
    // In that brief window, getSecondsLeft() returns 0, and the previous logic could
    // momentarily show an incorrect token value (e.g., 2.00) due to double-counting.
    // Holding the previous display for a cycle avoids the visible glitch.
    if (secondsLeft == 0) {
        return;
    }

    // Grace period expired - token is being consumed in IDLE.
    updateTimeDisplay(secondsLeft);

    // Calculate remaining time in the current token consistently with RUNNING/PAUSED:
    // secondsLeft = currentTokenRemaining + (tokensLeft * tokenTimeSeconds)
    unsigned long tokenTimeSeconds = TOKEN_TIME / 1000;
    unsigned long secondsInCurrentToken = 0;
    if (secondsLeft > 0) {
        unsigned long futureTokensTime = tokensLeft * tokenTimeSeconds;
        if (secondsLeft > futureTokensTime) {
            secondsInCurrentToken = secondsLeft - futureTokensTime;
        }
    }
    updateTokensDisplay(tokensLeft, secondsInCurrentToken, tokenTimeSeconds);
}

void DisplayManager::displayRunningState(CarWashController* controller) {
    if (!_display || !controller) return;
    
    // Get current values
    unsigned long secondsLeft = controller->getSecondsLeft();
    int tokensLeft = controller->getTokensLeft();
    
    // Update time display (top display) - shows seconds remaining
    updateTimeDisplay(secondsLeft);
    
    // Update tokens display (bottom display) - shows token fraction
    // When running, we need to calculate the fraction based on time used
    unsigned long tokenTimeSeconds = TOKEN_TIME / 1000;  // Convert ms to seconds
    
    // secondsLeft includes time from current token + future tokens
    // We need to extract how much is left in the current token
    unsigned long secondsInCurrentToken = 0;
    if (secondsLeft > 0) {
        // Calculate remaining time in current token
        // If secondsLeft > tokensLeft * tokenTime, there's a current token being consumed
        unsigned long futureTokensTime = tokensLeft * tokenTimeSeconds;
        if (secondsLeft > futureTokensTime) {
            secondsInCurrentToken = secondsLeft - futureTokensTime;
        }
    }
    
    updateTokensDisplay(tokensLeft, secondsInCurrentToken, tokenTimeSeconds);
}

void DisplayManager::displayPausedState(CarWashController* controller) {
    if (!_display || !controller) return;
    
    // Get current values
    unsigned long secondsLeft = controller->getSecondsLeft();
    int tokensLeft = controller->getTokensLeft();
    unsigned long gracePeriodSeconds = controller->getGracePeriodSecondsLeft();
    
    // Update time display (top display) - shows seconds remaining
    updateTimeDisplay(secondsLeft);
    
    // Update tokens display (bottom display) - shows token fraction
    unsigned long tokenTimeSeconds = TOKEN_TIME / 1000;
    
    // Calculate remaining time in current token
    unsigned long secondsInCurrentToken = 0;
    if (secondsLeft > 0) {
        unsigned long futureTokensTime = tokensLeft * tokenTimeSeconds;
        if (secondsLeft > futureTokensTime) {
            secondsInCurrentToken = secondsLeft - futureTokensTime;
        }
    }
    
    updateTokensDisplay(tokensLeft, secondsInCurrentToken, tokenTimeSeconds);
}

void DisplayManager::updateTimeDisplay(unsigned long seconds) {
    if (!_display) return;
    
    // Limit to 9999 seconds (about 166 minutes)
    if (seconds > 9999) seconds = 9999;
    
    // Only update if value changed
    if (seconds != _lastSecondsLeft) {
        _display->displayTopNumber((uint16_t)seconds, false);
        _lastSecondsLeft = seconds;
    }
}

void DisplayManager::updateTokensDisplay(int tokensRemaining, unsigned long secondsLeftInCurrentToken, unsigned long tokenTimeSeconds) {
    if (!_display) return;
    
    // Calculate token fraction
    float tokenFraction = calculateTokenFraction(tokensRemaining, secondsLeftInCurrentToken, tokenTimeSeconds);
    
    // Only update if value changed significantly (to avoid flicker)
    if (abs(tokenFraction - _lastTokensLeft) > 0.01f) {
        _display->displayBottomDecimal(tokenFraction, 2);
        _lastTokensLeft = tokenFraction;
    }
}

float DisplayManager::calculateTokenFraction(int tokensRemaining, unsigned long secondsLeftInCurrentToken, unsigned long tokenTimeSeconds) {
    // Total tokens = whole tokens remaining + fraction of current token
    // 
    // Example: User inserted 2 tokens (each token = 120 seconds)
    // - At start: 2.0 tokens (240 seconds)
    // - After 60 seconds: 1.5 tokens (180 seconds left = 1 token + 60/120 = 1.5)
    // - After 120 seconds: 1.0 tokens (120 seconds left)
    // - After 180 seconds: 0.5 tokens (60 seconds left)
    // - After 240 seconds: 0.0 tokens (0 seconds left)
    
    float fraction = (float)tokensRemaining;
    
    if (secondsLeftInCurrentToken > 0 && tokenTimeSeconds > 0) {
        // Add the fraction of the current token being consumed
        fraction += (float)secondsLeftInCurrentToken / (float)tokenTimeSeconds;
    }
    
    return fraction;
}

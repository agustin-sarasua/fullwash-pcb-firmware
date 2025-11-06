#include "display_manager.h"
#include "utilities.h"

DisplayManager::DisplayManager(uint8_t address, uint8_t columns, uint8_t rows, uint8_t sdaPin, uint8_t sclPin)
    : _columns(columns), _rows(rows), lastState(STATE_FREE), lastTokens(0), 
      lastSecondsLeft(0), lastUpdateTime(0), lcd(address, columns, rows, Wire1), _i2cMutex(NULL) {
    
    // Wire1 is already initialized in main.cpp
    _wire = &Wire1;
    
    // Create a log message about our initialization attempt
    LOG_INFO("Initializing LCD at 0x%02X on pins SDA=%d, SCL=%d", address, sdaPin, sclPin);
    
    // Initialize the LCD (mutex will be set later via setI2CMutex)
    lcd.begin();
    
    // Initial display
    lcd.clear();
    displayCentered("FULLWASH", 0);
    displayCentered("Initializing...", 1);
    
    LOG_INFO("LCD Display initialized on address 0x%02X", address);
}

void DisplayManager::setI2CMutex(SemaphoreHandle_t mutex) {
    _i2cMutex = mutex;
    lcd.setI2CMutex(mutex);
}

void DisplayManager::update(CarWashController* controller) {
    if (!controller) return;
    
    // Always update at least once per second for dynamic content (countdown timers)
    // This task is called every 500ms, so throttling ensures minimum 1Hz refresh
    unsigned long now = millis();
    MachineState currentState = controller->getCurrentState();
    MachineState previousState = lastState;
    bool stateChanged = (currentState != previousState);
    
    // CRITICAL: Always update immediately when state changes
    // This ensures display updates to FREE state as soon as timeout happens
    if (stateChanged) {
        lastUpdateTime = now;
        // Continue to update...
    } else {
        // Only throttle if state hasn't changed
        // Check if we're near timeout - update more frequently for accurate countdown
        unsigned long timeToTimeout = controller->getTimeToInactivityTimeout();
        unsigned long timeToTimeoutSeconds = timeToTimeout / 1000;
        bool nearTimeout = (timeToTimeoutSeconds > 0 && timeToTimeoutSeconds <= 5); // Update every 500ms when <= 5 seconds
        bool timeoutReached = (timeToTimeout == 0 && currentState != STATE_FREE); // Timeout is 0 but state hasn't changed yet
        
        // CRITICAL: If timeout reached 0 but state is still IDLE/PAUSED, force immediate update
        // This ensures display updates as soon as timeout expires, even if controller hasn't processed it yet
        // With the fix in update(), state should change immediately, but this is a safety measure
        if (timeoutReached) {
            lastUpdateTime = now;
            // Continue to update - this will show 00:00, but next update (within 500ms) should catch state change
            // The controller update() now checks timeout first and returns immediately after logout
        } else {
            // Update more frequently (every 500ms) when near timeout to avoid showing stale 00:00
            // When timeout is exactly 0, we want to update immediately to catch the state change
            unsigned long updateInterval = nearTimeout ? 500 : 1000;
            unsigned long timeSinceLastUpdate = now - lastUpdateTime;
            
            if (timeSinceLastUpdate < updateInterval) {
                return; // Throttle updates (removed excessive logging)
            }
            
            lastUpdateTime = now;
        }
    }
    
    // NOTE: I2C mutex is now handled internally by the LCD library
    // The DisplayManager no longer needs to manage the mutex at this level
    // This prevents double-locking and ensures all LCD operations are protected
    
    // Update display based on current state
    switch (currentState) {
        case STATE_FREE:
            displayFreeState();
            break;
        case STATE_IDLE:
            displayIdleState(controller, stateChanged);
            break;
        case STATE_RUNNING:
            displayRunningState(controller);
            break;
        case STATE_PAUSED:
            displayPausedState(controller, previousState);
            break;
        default:
            // Fallback to free state for unknown states
            LOG_WARNING("[DISPLAY] Unknown state %d, showing FREE", currentState);
            displayFreeState();
            break;
    }
    
    // Record last state after updating the display
    lastState = currentState;
}

void DisplayManager::clearLine(uint8_t line) {
    if (line >= _rows) return;
    
    lcd.setCursor(0, line);
    for (uint8_t i = 0; i < _columns; i++) {
        lcd.print(" ");
    }
    lcd.setCursor(0, line);
}

void DisplayManager::displayCentered(const String& text, uint8_t line) {
    if (line >= _rows) return;
    
    clearLine(line);
    
    int padding = (_columns - text.length()) / 2;
    if (padding < 0) padding = 0;
    
    lcd.setCursor(padding, line);
    lcd.print(text);
}

String DisplayManager::formatTime(unsigned long seconds) {
    unsigned long minutes = seconds / 60;
    unsigned long remainingSeconds = seconds % 60;
    
    char timeBuffer[6]; // MM:SS + null terminator
    sprintf(timeBuffer, "%02lu:%02lu", minutes, remainingSeconds);
    
    return String(timeBuffer);
}

void DisplayManager::displayFreeState() {
    lcd.clear();
    displayCentered("MAQUINA LIBRE", 0);
    lcd.setCursor(0, 1);
    lcd.print("Para cargar fichas:");
    lcd.setCursor(0, 2);
    lcd.print("1. Usa la APP");
    lcd.setCursor(0, 3);
    lcd.print("2. Inserte fichas");
}

void DisplayManager::displayIdleState(CarWashController* controller, bool stateChanged) {
    // Always refresh - get current values and display them
    int tokens = controller->getTokensLeft();
    String userName = controller->getUserName();
    unsigned long userInactivityTimeMs = controller->getTimeToInactivityTimeout();
    unsigned long userInactivityTime = userInactivityTimeMs / 1000;
    
    // Clear and redraw entire screen
    lcd.clear();
    
    // Truncate username if it's too long
    if (userName.length() > 16) {
        userName = userName.substring(0, 13) + "...";
    }
    
    String helloMsg = "Hola " + userName;
    lcd.setCursor(0, 0);
    lcd.print(helloMsg);
    
    lcd.setCursor(0, 1);
    lcd.print("Fichas: ");
    lcd.print(tokens);
    
    lcd.setCursor(0, 2);
    lcd.print("Salida en: ");
    String timeStr = formatTime(userInactivityTime);
    lcd.print(timeStr);
    
    // No logging to reduce overhead
    
    lcd.setCursor(0, 3);
    lcd.print("Pulse boton");
}

void DisplayManager::displayRunningState(CarWashController* controller) {
    // Always refresh - get current values and display them
    int tokens = controller->getTokensLeft();
    String userName = controller->getUserName();
    unsigned long secondsLeft = controller->getSecondsLeft();
    
    // Clear and redraw entire screen
    lcd.clear();
    
    // Truncate username if it's too long
    if (userName.length() > 16) {
        userName = userName.substring(0, 13) + "...";
    }
    
    String helloMsg = "Hola " + userName;
    lcd.setCursor(0, 0);
    lcd.print(helloMsg);
    
    lcd.setCursor(0, 1);
    lcd.print("Fichas: ");
    lcd.print(tokens);
    
    lcd.setCursor(0, 2);
    lcd.print("Tiempo: ");
    lcd.print(formatTime(secondsLeft));
    
    displayCentered("LAVANDO", 3);
}

void DisplayManager::displayPausedState(CarWashController* controller, MachineState previousState) {
    // Always refresh - get current values and display them
    int tokens = controller->getTokensLeft();
    String userName = controller->getUserName();
    unsigned long secondsLeft = controller->getSecondsLeft();
    unsigned long userInactivityTime = controller->getTimeToInactivityTimeout() / 1000;
    
    // Clear and redraw entire screen
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("Salida en: ");
    lcd.print(formatTime(userInactivityTime));
    
    lcd.setCursor(0, 1);
    lcd.print("Fichas: ");
    lcd.print(tokens);
    
    lcd.setCursor(0, 2);
    lcd.print("Tiempo: ");
    lcd.print(formatTime(secondsLeft));
    
    displayCentered("PAUSADA", 3);
}
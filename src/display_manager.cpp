#include "display_manager.h"
#include "utilities.h"

DisplayManager::DisplayManager(uint8_t address, uint8_t columns, uint8_t rows, uint8_t sdaPin, uint8_t sclPin)
    : _columns(columns), _rows(rows), lastState(STATE_FREE), lastTokens(0), 
      lastSecondsLeft(0), lastUpdateTime(0), lcd(address, columns, rows, Wire1) {
    
    // Wire1 is already initialized in main.cpp
    _wire = &Wire1;
    
    // Create a log message about our initialization attempt
    LOG_INFO("Initializing LCD at 0x%02X on pins SDA=%d, SCL=%d", address, sdaPin, sclPin);
    
    // Initialize the LCD
    lcd.begin();
    
    // Initial display
    lcd.clear();
    displayCentered("FULLWASH", 0);
    displayCentered("Initializing...", 1);
    
    LOG_INFO("LCD Display initialized on address 0x%02X", address);
}

void DisplayManager::update(CarWashController* controller) {
    if (!controller) return;
    
    MachineState currentState = controller->getCurrentState();
    MachineState previousState = lastState;
    bool stateChanged = (currentState != lastState);
    
    // Only update when state changes or every second
    unsigned long currentTime = millis();
    bool shouldUpdate = stateChanged || 
                        (currentTime - lastUpdateTime >= 1000);
    
    if (!shouldUpdate) return;
    
    lastUpdateTime = currentTime;
    lastState = currentState;
    
    // Update display based on current state
    // Pass stateChanged flag so display functions know if this is a state transition
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
            displayFreeState();
            break;
    }
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
    // Check if tokens or username changed
    int tokens = controller->getTokensLeft();
    String userName = controller->getUserName();
    unsigned long userInactivityTime = controller->getTimeToInactivityTimeout() / 1000;
    
    bool contentChanged = (tokens != lastTokens) || 
                         (userName != lastUserName) ||
                         (userInactivityTime != lastSecondsLeft);
    
    // Always update on state change (e.g., FREE -> IDLE transition)
    // If state hasn't changed, update() was called because 1 second passed
    // In IDLE state, we should always refresh to show countdown updates
    // Only skip if absolutely nothing changed (very rare case)
    if (!stateChanged && !contentChanged) {
        // Double-check: if inactivity timeout, tokens, and username are all unchanged, skip
        if (userInactivityTime == lastSecondsLeft && 
            tokens == lastTokens && 
            userName == lastUserName) {
            return;
        }
    }
    
    lastTokens = tokens;
    lastUserName = userName;
    lastSecondsLeft = userInactivityTime;
    
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
    lcd.print(formatTime(userInactivityTime));
    
    lcd.setCursor(0, 3);
    lcd.print("Pulse boton");
}

void DisplayManager::displayRunningState(CarWashController* controller) {
    // Always update during running state to show countdown timer
    // The update() method already throttles calls to once per second
    int tokens = controller->getTokensLeft();
    String userName = controller->getUserName();
    unsigned long secondsLeft = controller->getSecondsLeft();
    
    // Update stored values
    lastTokens = tokens;
    lastUserName = userName;
    lastSecondsLeft = secondsLeft;
    
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
    // Get current values
    int tokens = controller->getTokensLeft();
    String userName = controller->getUserName();
    unsigned long secondsLeft = controller->getSecondsLeft();

    // Update stored values
    lastTokens = tokens;
    lastUserName = userName;
    lastSecondsLeft = secondsLeft;
    
    unsigned long userInactivityTime = controller->getTimeToInactivityTimeout() / 1000;
    // Clear display and redraw everything when state changes
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
    
    // Make sure PAUSADA is visible by forcing redraw
    displayCentered("PAUSADA", 3);

}
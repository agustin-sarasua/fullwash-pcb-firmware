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
    
    // Only update when state changes or every second
    unsigned long currentTime = millis();
    bool shouldUpdate = (currentState != lastState) || 
                        (currentTime - lastUpdateTime >= 1000);
    
    if (!shouldUpdate) return;
    
    lastUpdateTime = currentTime;
    lastState = currentState;
    
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

void DisplayManager::displayIdleState(CarWashController* controller) {
    // Check if tokens or username changed
    int tokens = controller->getTokensLeft();
    String userName = controller->getUserName();
    unsigned long userInactivityTime = controller->getTimeToInactivityTimeout() / 1000;
    
    bool contentChanged = (tokens != lastTokens) || 
                         (userName != lastUserName) ||
                         (userInactivityTime != lastSecondsLeft);
    
    if (!contentChanged && lastState == STATE_IDLE) return;
    
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
    // Check if tokens or username or time changed
    int tokens = controller->getTokensLeft();
    String userName = controller->getUserName();
    unsigned long secondsLeft = controller->getSecondsLeft();
    
    bool contentChanged = (tokens != lastTokens) || 
                         (userName != lastUserName) ||
                         (secondsLeft != lastSecondsLeft);
    
    if (!contentChanged && lastState == STATE_RUNNING) return;
    
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

// void DisplayManager::displayPausedState(CarWashController* controller) {
//     // Always update display when transitioning to PAUSED state
//     int tokens = controller->getTokensLeft();
//     String userName = controller->getUserName();
//     unsigned long secondsLeft = controller->getSecondsLeft();
    
//     // Force update when entering PAUSED state from another state
//     bool forceUpdate = (lastState != STATE_PAUSED);
//     bool contentChanged = (tokens != lastTokens) || 
//                          (userName != lastUserName) ||
//                          (secondsLeft != lastSecondsLeft);
    
//     if (!contentChanged && !forceUpdate && lastState == STATE_PAUSED) return;
    
//     lastTokens = tokens;
//     lastUserName = userName;
//     lastSecondsLeft = secondsLeft;
    
//     // Clear display and redraw everything
//     lcd.clear();
    
//     // Truncate username if it's too long
//     if (userName.length() > 16) {
//         userName = userName.substring(0, 13) + "...";
//     }
    
//     String helloMsg = "Hola " + userName;
//     lcd.setCursor(0, 0);
//     lcd.print(helloMsg);
    
//     lcd.setCursor(0, 1);
//     lcd.print("Fichas: ");
//     lcd.print(tokens);
    
//     lcd.setCursor(0, 2);
//     lcd.print("Tiempo: ");
//     lcd.print(formatTime(secondsLeft));
    
//     // Make sure PAUSADA is visible by forcing redraw
//     lcd.setCursor(0, 3);
//     lcd.print("                "); // Clear line
//     displayCentered("PAUSADA", 3);
// }
// Modified method signature to accept previous state
void DisplayManager::displayPausedState(CarWashController* controller, MachineState previousState) {
    // Get current values
    int tokens = controller->getTokensLeft();
    String userName = controller->getUserName();
    unsigned long secondsLeft = controller->getSecondsLeft();
    
    // TRUE state transition detection - was it a different state before?
    bool stateJustChanged = (previousState != STATE_PAUSED);
    
    // Check if content changed
    bool contentChanged = (tokens != lastTokens) || 
                         (userName != lastUserName) ||
                         (secondsLeft != lastSecondsLeft);
    
    // Always update if state just changed or content changed
    if (!stateJustChanged && !contentChanged) return;
    
    // Update stored values
    lastTokens = tokens;
    lastUserName = userName;
    lastSecondsLeft = secondsLeft;
    
    // Clear display and redraw everything when state changes
    if (stateJustChanged) {
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
        
        // Make sure PAUSADA is visible by forcing redraw
        displayCentered("PAUSADA", 3);
    } else if (contentChanged) {
        // Perform selective updates without clearing the screen
        // Only update the parts that changed
        
        // Update tokens if changed
        if (tokens != lastTokens) {
            lcd.setCursor(8, 1);  // Position after "Fichas: "
            lcd.print("    ");    // Clear the space
            lcd.setCursor(8, 1);
            lcd.print(tokens);
        }
        
        // Update time if changed
        if (secondsLeft != lastSecondsLeft) {
            lcd.setCursor(8, 2);  // Position after "Tiempo: "
            lcd.print("        "); // Clear the space
            lcd.setCursor(8, 2);
            lcd.print(formatTime(secondsLeft));
        }
    }
}
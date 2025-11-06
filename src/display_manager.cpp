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
    
    // Update if state changed OR if at least 1 second has passed
    // This ensures smooth countdowns while avoiding excessive I2C traffic
    if (!stateChanged && (now - lastUpdateTime) < 1000) {
        return;
    }
    lastUpdateTime = now;
    
    // Protect I2C access with mutex (shared with RTC)
    bool mutexTaken = false;
    if (_i2cMutex != NULL) {
        mutexTaken = (xSemaphoreTake(_i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE);
        if (!mutexTaken) {
            LOG_WARNING("Failed to acquire I2C mutex for display update");
            return;
        }
    }
    
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
            displayFreeState();
            break;
    }
    
    // Release mutex
    if (mutexTaken && _i2cMutex != NULL) {
        xSemaphoreGive(_i2cMutex);
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
    unsigned long userInactivityTime = controller->getTimeToInactivityTimeout() / 1000;
    
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
    lcd.print(formatTime(userInactivityTime));
    
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
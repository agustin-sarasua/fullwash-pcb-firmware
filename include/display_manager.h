#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "lcd_i2c_custom.h"
#include "car_wash_controller.h"
#include "domain.h"
#include "logger.h"

class DisplayManager {
public:
    // Initialize the LCD display
    DisplayManager(uint8_t address, uint8_t columns, uint8_t rows, uint8_t sdaPin, uint8_t sclPin);
    
    // Set I2C mutex for thread-safe access to Wire1
    void setI2CMutex(SemaphoreHandle_t mutex);
    
    // Update display based on machine state
    void update(CarWashController* controller);
    
    // Clear specific line
    void clearLine(uint8_t line);
    
    // Display centered text on specific line
    void displayCentered(const String& text, uint8_t line);
    
    // Format seconds into MM:SS
    String formatTime(unsigned long seconds);
    
    // Get button name from button index
    String getButtonName(int buttonIndex);

private:
    // Display specific screens based on machine state
    void displayFreeState();
    void displayIdleState(CarWashController* controller, bool stateChanged);
    void displayRunningState(CarWashController* controller);
    // void displayPausedState(CarWashController* controller);
    void displayPausedState(CarWashController* controller, MachineState previousState);
    
    LcdI2cCustom lcd;
    uint8_t _columns;
    uint8_t _rows;
    TwoWire* _wire;
    SemaphoreHandle_t _i2cMutex;  // Mutex for Wire1 access
    
    // Track the last state to avoid unnecessary redraw
    MachineState lastState;
    String lastUserName;
    int lastTokens;
    unsigned long lastSecondsLeft;
    unsigned long lastUpdateTime;
    
    // Button name mapping
    static const char* BUTTON_NAMES[5];
};

#endif // DISPLAY_MANAGER_H
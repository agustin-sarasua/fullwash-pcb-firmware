#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include "lcd_i2c_custom.h"
#include "car_wash_controller.h"
#include "domain.h"
#include "logger.h"

class DisplayManager {
public:
    // Initialize the LCD display
    DisplayManager(uint8_t address, uint8_t columns, uint8_t rows, uint8_t sdaPin, uint8_t sclPin);
    
    // Update display based on machine state
    void update(CarWashController* controller);
    
    // Clear specific line
    void clearLine(uint8_t line);
    
    // Display centered text on specific line
    void displayCentered(const String& text, uint8_t line);
    
    // Format seconds into MM:SS
    String formatTime(unsigned long seconds);
    
    // Clear the entire display
    void clear();
    
    // Set cursor position
    void setCursor(uint8_t col, uint8_t row);
    
    // Print text at current cursor position
    void print(const String& text);

private:
    // Display specific screens based on machine state
    void displayFreeState();
    void displayIdleState(CarWashController* controller);
    void displayRunningState(CarWashController* controller);
    // void displayPausedState(CarWashController* controller);
    void displayPausedState(CarWashController* controller, MachineState previousState);
    
    LcdI2cCustom lcd;
    uint8_t _columns;
    uint8_t _rows;
    TwoWire* _wire;
    
    // Track the last state to avoid unnecessary redraw
    MachineState lastState;
    String lastUserName;
    int lastTokens;
    unsigned long lastSecondsLeft;
    unsigned long lastUpdateTime;
};

#endif // DISPLAY_MANAGER_H
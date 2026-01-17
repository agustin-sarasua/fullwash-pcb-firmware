#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "ch453s_driver.h"
#include "car_wash_controller.h"
#include "domain.h"
#include "logger.h"

/**
 * Display Manager for dual 4-digit 7-segment LED displays
 * 
 * Hardware:
 * - 2x 4-digit 7-segment LED displays (SR440801N/32)
 * - CH453S LED driver IC (I2C controlled)
 * 
 * Display Layout:
 * - Top display (digits 0-3): Tokens left as decimal (e.g., "01.50" for 1.5 tokens)
 * - Bottom display (digits 4-7): Time left in MM.SS format (e.g., "02.00" for 2:00)
 * 
 * Token Display Logic:
 * - Tokens are displayed as fractions based on time consumed
 * - Example: 2 tokens loaded, each token = 2 minutes (120 seconds)
 *   - After 1 minute used: 1.5 tokens remaining
 *   - After 2 minutes used: 1.0 tokens remaining
 */
class DisplayManager {
public:
    /**
     * Initialize the 7-segment display manager
     * @param sdaPin I2C SDA pin
     * @param sclPin I2C SCL pin
     */
    DisplayManager(uint8_t sdaPin, uint8_t sclPin);
    
    /**
     * Set I2C mutex for thread-safe access to Wire1
     * @param mutex FreeRTOS semaphore handle
     */
    void setI2CMutex(SemaphoreHandle_t mutex);
    
    /**
     * Update display based on machine state
     * @param controller Pointer to car wash controller
     */
    void update(CarWashController* controller);
    
    /**
     * Clear all displays
     */
    void clearAll();
    
    /**
     * Display initialization message
     */
    void displayInit();
    
    /**
     * Display error state
     */
    void displayError();
    
    /**
     * Set display brightness
     * @param brightness Brightness level 0-15
     */
    void setBrightness(uint8_t brightness);

private:
    CH453SDriver* _display;
    TwoWire* _wire;
    SemaphoreHandle_t _i2cMutex;
    
    // Track last values to avoid unnecessary redraws
    MachineState _lastState;
    unsigned long _lastSecondsLeft;
    float _lastTokensLeft;
    unsigned long _lastUpdateTime;
    
    /**
     * Display FREE state (machine available)
     * Shows "----" on both displays or blank
     */
    void displayFreeState();
    
    /**
     * Display IDLE state (loaded, waiting for button)
     * @param controller Pointer to controller for getting state
     */
    void displayIdleState(CarWashController* controller);
    
    /**
     * Display RUNNING state (actively washing)
     * @param controller Pointer to controller for getting state
     */
    void displayRunningState(CarWashController* controller);
    
    /**
     * Display PAUSED state (temporarily stopped)
     * @param controller Pointer to controller for getting state
     */
    void displayPausedState(CarWashController* controller);
    
    /**
     * Update time display (top display)
     * @param seconds Time remaining in seconds
     */
    void updateTimeDisplay(unsigned long seconds);
    
    /**
     * Update tokens display (bottom display)
     * Shows tokens as a decimal fraction
     * @param tokensRemaining Integer tokens remaining (not yet started)
     * @param secondsLeftInCurrentToken Seconds remaining in current token (0 if none)
     * @param tokenTimeSeconds Total seconds per token
     */
    void updateTokensDisplay(int tokensRemaining, unsigned long secondsLeftInCurrentToken, unsigned long tokenTimeSeconds);
    
    /**
     * Calculate token fraction from time
     * @param tokensRemaining Whole tokens not yet consumed
     * @param secondsLeftInCurrentToken Seconds remaining in current token
     * @param tokenTimeSeconds Total seconds per token
     * @return Tokens as a decimal (e.g., 1.5)
     */
    float calculateTokenFraction(int tokensRemaining, unsigned long secondsLeftInCurrentToken, unsigned long tokenTimeSeconds);
};

#endif // DISPLAY_MANAGER_H

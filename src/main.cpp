#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "io_expander.h"
#include "utilities.h"
#include "constants.h"
#include "car_wash_controller.h"
#include "logger.h"
#include "display_manager.h"
#include "ble_machine_loader.h"

// Wire1 is already defined in the ESP32 Arduino framework

// Create IO Expander
IoExpander ioExpander(TCA9535_ADDR, I2C_SDA_PIN, I2C_SCL_PIN, INT_PIN);


// Create controller
CarWashController* controller;

// Create display manager
DisplayManager* display;

// Create BLE machine loader
BLEMachineLoader* bleMachineLoader;

// FreeRTOS task handles
TaskHandle_t TaskCoinDetectorHandle = NULL;
TaskHandle_t TaskButtonDetectorHandle = NULL;
TaskHandle_t TaskWatchdogHandle = NULL;
TaskHandle_t TaskDisplayUpdateHandle = NULL;

// FreeRTOS mutexes for shared resources
SemaphoreHandle_t xIoExpanderMutex = NULL;
SemaphoreHandle_t xControllerMutex = NULL;
SemaphoreHandle_t xI2CMutex = NULL;  // For Wire1 (LCD)

// FreeRTOS queue of debounced button press events (TaskButtonDetector -> CarWashController)
QueueHandle_t xButtonEventQueue = NULL;

/**
 * FreeRTOS Task: Coin Detector
 *
 * Hardware contract (verified with the raw edge logger on real hardware,
 * 2026-07-04): COIN_SIG (TCA9535 P06) is ACTIVE-HIGH. The line idles LOW
 * (R64 pull-down); a coin produces one HIGH pulse of ~80-130 ms.
 *
 * Detection state machine (polls PORT0 every COIN_POLL_INTERVAL_MS):
 * - ARMING:   after COIN_STARTUP_DELAY, the line must be continuously LOW
 *             for COIN_STARTUP_QUIET_MS before detection arms - boot-time
 *             noise can never register as a coin
 * - IDLE:     armed, waiting for a rising edge
 * - IN_PULSE: measuring the HIGH pulse. Accepted only if its width lands in
 *             [COIN_MIN_PULSE_MS, COIN_MAX_PULSE_MS]; shorter = noise spike,
 *             longer = stuck switch / wiring fault
 * - REARM:    line must stay LOW for COIN_IDLE_REARM_MS before the next
 *             pulse may start (absorbs break bounce)
 *
 * Failed reads (mutex timeout or I2C error) are discarded without advancing
 * the state machine, so a bus glitch cannot fabricate or truncate a pulse.
 */
void TaskCoinDetector(void *pvParameters) {
  const TickType_t xDelay = COIN_POLL_INTERVAL_MS / portTICK_PERIOD_MS;

  enum DetectState { ARMING, IDLE, IN_PULSE, REARM, FAULT };
  DetectState state = ARMING;

  unsigned long quietSince = 0;        // Start of continuous-LOW window (ARMING/REARM), 0 = not started
  unsigned long pulseStart = 0;        // Rising edge timestamp (pulse start)
  unsigned long lastCoinTime = 0;      // End of last accepted pulse, for cooldown
  unsigned long skippedSamples = 0;    // Mutex timeouts + I2C errors, for field diagnostics

  // Raw edge logger: reports every level change on COIN_SIG before any
  // filtering, so polarity/pulse-shape issues are visible in field logs
  bool rawInit = false;
  bool rawLastHigh = false;
  unsigned long rawLastEdge = 0;

  // Wait for IO expander to be initialized
  vTaskDelay(1000 / portTICK_PERIOD_MS);

  unsigned long startTime = millis();
  LOG_INFO("Coin detector task started (active-HIGH, pulse window %lu-%lu ms)",
           COIN_MIN_PULSE_MS, COIN_MAX_PULSE_MS);

  for(;;) {
    vTaskDelay(xDelay);
    unsigned long now = millis();

    // Ignore the line entirely right after boot
    if (now - startTime < COIN_STARTUP_DELAY) {
      continue;
    }

    uint8_t portVal = 0;
    bool readOk = false;
    if (xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      readOk = ioExpander.readRegister(INPUT_PORT0, portVal);
      xSemaphoreGive(xIoExpanderMutex);
    }
    if (!readOk) {
      skippedSamples++;
      if (skippedSamples % 500 == 0) {
        LOG_WARNING("COIN: %lu samples skipped so far (mutex contention / I2C errors)", skippedSamples);
      }
      continue;
    }

    bool lineHigh = (portVal & (1 << COIN_SIG)) != 0;

    if (!rawInit) {
      rawInit = true;
      rawLastHigh = lineHigh;
      rawLastEdge = now;
      LOG_INFO("COIN RAW: initial line level %s (Port0=0x%02X)", lineHigh ? "HIGH" : "LOW", portVal);
    } else if (lineHigh != rawLastHigh) {
      LOG_INFO("COIN RAW: %s -> %s after %lu ms", rawLastHigh ? "HIGH" : "LOW",
               lineHigh ? "HIGH" : "LOW", now - rawLastEdge);
      rawLastHigh = lineHigh;
      rawLastEdge = now;
    }

    // Active-HIGH: line idles LOW (R64 pull-down), coin drives it HIGH
    bool coinActive = lineHigh;

    switch (state) {
      case ARMING:
        if (coinActive) {
          quietSince = 0;  // Line not idle - restart the quiet window
        } else if (quietSince == 0) {
          quietSince = now;
        } else if (now - quietSince >= COIN_STARTUP_QUIET_MS) {
          state = IDLE;
          LOG_INFO("COIN: line idle for %lu ms - detector armed", COIN_STARTUP_QUIET_MS);
        }
        break;

      case IDLE:
        if (coinActive) {
          pulseStart = now;
          state = IN_PULSE;
        }
        break;

      case IN_PULSE:
        if (coinActive) {
          if (now - pulseStart > COIN_MAX_PULSE_MS) {
            LOG_WARNING("COIN: signal HIGH for over %lu ms - stuck switch or wiring fault, ignoring",
                        COIN_MAX_PULSE_MS);
            state = FAULT;
          }
        } else {
          unsigned long width = now - pulseStart;
          if (width < COIN_MIN_PULSE_MS) {
            LOG_DEBUG("COIN: %lu ms spike rejected as noise (min %lu ms)", width, COIN_MIN_PULSE_MS);
          } else if (lastCoinTime != 0 && now - lastCoinTime < COIN_COOLDOWN_MS) {
            LOG_WARNING("COIN: %lu ms pulse ignored - within %lu ms cooldown", width, COIN_COOLDOWN_MS);
          } else {
            ioExpander.setCoinSignal(1);
            ioExpander._intCnt++;
            lastCoinTime = now;
            LOG_INFO("COIN DETECTED #%u (pulse width %lu ms)", ioExpander._intCnt, width);
          }
          quietSince = now;
          state = REARM;
        }
        break;

      case REARM:
        if (coinActive) {
          quietSince = now;  // Break bounce - keep waiting for the line to settle
        } else if (now - quietSince >= COIN_IDLE_REARM_MS) {
          state = IDLE;
        }
        break;

      case FAULT:
        if (!coinActive) {
          quietSince = now;
          state = REARM;
          LOG_INFO("COIN: signal released after fault, re-arming");
        }
        break;
    }
  }
}

/**
 * FreeRTOS Task: Button Detector
 *
 * This task is the sole source of button input. It monitors all button pins
 * (BUTTON1-6) for press events and is the only producer of ButtonEvents.
 *
 * Detection Logic:
 * - Polls PORT0 register every 10ms to get current button states
 * - Detects button press events (HIGH->LOW transition, buttons are active LOW)
 * - Applies a 50ms per-button debounce, then enqueues one ButtonEvent per
 *   physical press onto xButtonEventQueue for CarWashController to consume
 *
 * Thread Safety:
 * - Uses mutex protection when accessing ioExpander
 * - xQueueSend is safe to call without additional locking
 */
void TaskButtonDetector(void *pvParameters) {
    const TickType_t xDelay = 10 / portTICK_PERIOD_MS; // 10ms for very responsive button detection
    const unsigned long BUTTON_DEBOUNCE_MS = 50;
    uint8_t lastPortValue = 0xFF; // All buttons released initially (active LOW)
    unsigned long lastPressTime[NUM_BUTTONS] = {0};

    // Wait for IO expander to be initialized
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    LOG_INFO("Button detector task started");

    if (ENABLE_BUTTON_DIAGNOSTICS) {
        LOG_INFO("[BUTTON DIAG] Button detector task initialized");
    }

    for(;;) {
        // CRITICAL FIX: Poll continuously, not just when INT_PIN is LOW
        // The interrupt pin may not fire reliably, so we poll every cycle
        // This ensures button presses are detected immediately
        uint8_t currentPortValue = 0;
        uint8_t pendingButtons[NUM_BUTTONS];
        int pendingCount = 0;

        // Protect IO expander access with mutex
        // Reduced timeout to 20ms for faster failure and better responsiveness
        if (xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            // Discard failed reads: an I2C error must not be interpreted as
            // 0x00, which would look like all buttons pressed at once
            bool readOk = ioExpander.readRegister(INPUT_PORT0, currentPortValue);

            // Check if any button state changed
            if (readOk && currentPortValue != lastPortValue) {
                unsigned long now = millis();

                // Check each button for press events (transition from HIGH to LOW)
                for (int i = 0; i < NUM_BUTTONS; i++) {
                    int buttonPin;
                    if (i < NUM_BUTTONS - 1) {
                        buttonPin = BUTTON_INDICES[i]; // Function buttons 1-5
                    } else {
                        buttonPin = STOP_BUTTON_PIN;   // Stop button (BUTTON6)
                    }

                    bool currentButtonPressed = !(currentPortValue & (1 << buttonPin));
                    bool lastButtonPressed = !(lastPortValue & (1 << buttonPin));

                    // Detect button press (transition from released to pressed)
                    if (currentButtonPressed && !lastButtonPressed) {
                        unsigned long timeSincePress = now - lastPressTime[i];
                        if (timeSincePress > BUTTON_DEBOUNCE_MS) {
                            LOG_INFO("Button %d transition detected: HIGH->LOW (pressed)", i + 1);
                            lastPressTime[i] = now;
                            pendingButtons[pendingCount++] = i;
                        } else {
                            LOG_DEBUG("Button %d press ignored - too soon (debounce: %lu ms since last, need %lu ms)",
                                     i + 1, timeSincePress, BUTTON_DEBOUNCE_MS);
                        }
                    } else if (!currentButtonPressed && lastButtonPressed) {
                        // Button released - log for debugging
                        LOG_DEBUG("Button %d transition detected: LOW->HIGH (released)", i + 1);
                    }
                }

                lastPortValue = currentPortValue;
            }

            xSemaphoreGive(xIoExpanderMutex);
        } else {
            // Mutex contention is normal - no logging needed
        }
        // Removed warning log to reduce overhead - mutex contention is normal

        // Enqueue outside the mutex to keep I2C mutex hold time minimal
        for (int p = 0; p < pendingCount; p++) {
            uint8_t buttonId = pendingButtons[p];
            ButtonEvent evt{buttonId, millis()};
            if (xButtonEventQueue != NULL) {
                if (xQueueSend(xButtonEventQueue, &evt, 0) == pdTRUE) {
                    LOG_INFO("Button %d press queued", buttonId + 1);
                } else {
                    LOG_WARNING("Button event queue full - dropping button %d press", buttonId + 1);
                }
            }
        }

        vTaskDelay(xDelay); // Wait 10ms before next check
    }
}


/**
 * FreeRTOS Task: Display Update
 * 
 * This task handles LCD display updates independently to ensure reliable refresh rate.
 * Updates the display at least once per second, or more frequently for dynamic content
 * like countdown timers.
 * 
 * Priority: 3 (Medium-high priority - ensures display stays responsive)
 */
void TaskDisplayUpdate(void *pvParameters) {
    const TickType_t xDisplayDelay = pdMS_TO_TICKS(500); // Update every 500ms for smooth countdowns
    unsigned long lastForcedUpdate = 0;
    
    LOG_INFO("Display update task started");
    
    // Wait for controller and display to be initialized
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    for(;;) {
        if (display && controller) {
            // Update display - mutex protection is handled inside DisplayManager
            display->update(controller);
            
            // Force update at least once per second even if throttled internally
            unsigned long now = millis();
            if (now - lastForcedUpdate >= 1000) {
                lastForcedUpdate = now;
                // Force a refresh by clearing lastUpdateTime (handled in display->update)
            }
        }
        
        vTaskDelay(xDisplayDelay);
    }
}


/**
 * FreeRTOS Task: System Watchdog
 * 
 * This task monitors system health and task status:
 * - Monitors all FreeRTOS tasks for crashes or hangs
 * - Checks stack usage for all tasks
 * - Monitors system resources (heap, etc.)
 * - Logs system health status
 * 
 * Priority: 1 (Lowest priority - monitoring only)
 */
void TaskWatchdog(void *pvParameters) {
    const TickType_t xWatchdogDelay = pdMS_TO_TICKS(10000); // Check every 10 seconds
    
    LOG_INFO("Watchdog task started");
    
    // Wait for all tasks to be created
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    
    for(;;) {
        // Check coin detector task
        if (TaskCoinDetectorHandle != NULL) {
            eTaskState coinTaskState = eTaskGetState(TaskCoinDetectorHandle);
            if (coinTaskState == eDeleted || coinTaskState == eInvalid) {
                LOG_ERROR("Coin detector task died! State: %d", coinTaskState);
                // Task crashed - would need to restart, but for now just log
            } else {
                UBaseType_t stackHighWater = uxTaskGetStackHighWaterMark(TaskCoinDetectorHandle);
                if (stackHighWater < 512) { // Less than 256 bytes free
                    LOG_WARNING("Coin detector task stack low: %d bytes remaining", stackHighWater);
                }
            }
        }
        
        // Check button detector task
        if (TaskButtonDetectorHandle != NULL) {
            eTaskState buttonTaskState = eTaskGetState(TaskButtonDetectorHandle);
            if (buttonTaskState == eDeleted || buttonTaskState == eInvalid) {
                LOG_ERROR("Button detector task died! State: %d", buttonTaskState);
            } else {
                UBaseType_t stackHighWater = uxTaskGetStackHighWaterMark(TaskButtonDetectorHandle);
                if (stackHighWater < 512) {
                    LOG_WARNING("Button detector task stack low: %d bytes remaining", stackHighWater);
                }
            }
        }
        // Monitor heap usage
        size_t freeHeap = ESP.getFreeHeap();
        size_t minFreeHeap = ESP.getMinFreeHeap();
        size_t totalHeap = ESP.getHeapSize();
        
        if (freeHeap < 10000) { // Less than 10KB free
            LOG_WARNING("Heap memory low: %d bytes free (min: %d, total: %d)", 
                       freeHeap, minFreeHeap, totalHeap);
        }
        
        // Periodic health check (no logging to reduce overhead)
        static int checkCount = 0;
        checkCount++;
        if (checkCount >= 5) {
            checkCount = 0;
        }
        
        vTaskDelay(xWatchdogDelay);
    }
}


// =============================================================================
// DOUBLE-TAP RESET DETECTION FOR FACTORY RESET
// =============================================================================
// Press the reset button twice within 3 seconds to trigger a factory reset.
// This will reset the machine_id to "99" (installation mode).
// =============================================================================

#define DOUBLE_RESET_NAMESPACE "dblreset"
#define DOUBLE_RESET_FLAG_KEY "flag"
#define DOUBLE_RESET_WINDOW_MS 3000  // 3 seconds window for double-tap detection

/**
 * Perform factory reset - resets machine to installation mode (machine_id = 99)
 * This clears all configuration and reboots the device.
 */
void performFactoryReset() {
  Serial.println("\n");
  Serial.println("==============================================");
  Serial.println("  FACTORY RESET TRIGGERED (DOUBLE-TAP RESET)");
  Serial.println("==============================================");
  Serial.println("Resetting machine to installation mode...");
  
  // Clear the double-reset flag first
  Preferences resetPrefs;
  resetPrefs.begin(DOUBLE_RESET_NAMESPACE, false);
  resetPrefs.putBool(DOUBLE_RESET_FLAG_KEY, false);
  resetPrefs.end();
  
  // Reset configuration to defaults
  Preferences configPrefs;
  configPrefs.begin(PREFS_NAMESPACE, false);
  configPrefs.clear();  // Clear all keys in the namespace
  configPrefs.putString(PREFS_MACHINE_NUM, "99");
  configPrefs.putString(PREFS_ENVIRONMENT, "prod");
  configPrefs.putString(PREFS_BLE_PASSWORD, DEFAULT_MASTER_PASSWORD);
  configPrefs.end();
  
  Serial.println("Configuration reset complete!");
  Serial.println("Machine ID set to: 99 (installation mode)");
  Serial.println("Environment set to: prod");
  Serial.println("BLE password reset to default");
  Serial.println("==============================================");
  Serial.println("Rebooting in 2 seconds...");
  Serial.println("==============================================\n");
  
  // Blink LED rapidly to indicate factory reset
  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < 20; i++) {
    digitalWrite(LED_PIN, i % 2);
    delay(100);
  }
  
  // Reboot the device
  ESP.restart();
}

/**
 * Check for double-tap reset and handle the reset window.
 * Must be called at the very beginning of setup().
 * 
 * @return true if normal boot should continue, false if factory reset was triggered
 */
bool checkDoubleResetAndWait() {
  Preferences resetPrefs;
  resetPrefs.begin(DOUBLE_RESET_NAMESPACE, false);
  
  // Check if the double-reset flag is already set (from previous boot)
  bool flagWasSet = resetPrefs.getBool(DOUBLE_RESET_FLAG_KEY, false);
  
  if (flagWasSet) {
    // Double-reset detected! This is the second reset within the window
    Serial.println("\n*** DOUBLE-RESET DETECTED! ***");
    resetPrefs.end();
    performFactoryReset();
    return false;  // Will never reach here due to ESP.restart()
  }
  
  // First reset - set the flag
  resetPrefs.putBool(DOUBLE_RESET_FLAG_KEY, true);
  resetPrefs.end();
  
  // Visual feedback: fast blink during reset window
  pinMode(LED_PIN, OUTPUT);
  Serial.println("Double-reset window active (press RESET again within 3s for factory reset)...");
  
  // Wait for the reset window with LED blinking
  unsigned long startTime = millis();
  bool ledState = true;
  while (millis() - startTime < DOUBLE_RESET_WINDOW_MS) {
    digitalWrite(LED_PIN, ledState);
    ledState = !ledState;
    delay(150);  // Fast blink to indicate reset window is active
  }
  
  // Window passed without second reset - clear the flag
  resetPrefs.begin(DOUBLE_RESET_NAMESPACE, false);
  resetPrefs.putBool(DOUBLE_RESET_FLAG_KEY, false);
  resetPrefs.end();
  
  // Turn LED back on to show normal boot
  digitalWrite(LED_PIN, HIGH);
  Serial.println("Double-reset window closed. Continuing normal boot...");
  
  return true;  // Continue with normal setup
}

void setup() {
  // Initialize serial FIRST for debug output during double-reset detection
  Serial.begin(115200);
  delay(100);  // Brief delay for serial to stabilize
  
  // =========================================================================
  // DOUBLE-TAP RESET DETECTION - Must be FIRST thing in setup!
  // Press reset twice within 3 seconds to trigger factory reset
  // =========================================================================
  checkDoubleResetAndWait();
  
  // Initialize Logger with default log level (after double-reset check)
  Logger::init(DEFAULT_LOG_LEVEL, 115200);
  delay(500); // Brief delay after logger init
  
  LOG_INFO("Starting fullwash-pcb-firmware...");
  
  // Check if machine is already configured by loading from preferences
  LOG_INFO("=== Checking Machine Configuration ===");

  // Machine number / environment provisioning goes entirely through the BLE Machine
  // Loader's machine-99 setup path (see car_wash_controller.cpp, the
  // `MACHINE_ID == "99"` branch of handleMqttMessage) - there used to be a second,
  // separate provisioning service here (BLEConfigManager, a distinct GATT service at
  // 4fafc201-...) but its begin() was never actually called, so that service never
  // advertised and tools/ble_config_tool.py (which targeted it) never worked against
  // this firmware. Removed rather than wired up, to avoid two competing provisioning
  // paths.
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true); // Read-only mode
  String machineNum = prefs.getString(PREFS_MACHINE_NUM, "99");
  String environment = prefs.getString(PREFS_ENVIRONMENT, "prod");
  prefs.end();

  // Sets INIT_TOPIC/CONFIG_TOPIC/etc (constants.cpp) - a naming leftover from the MQTT
  // days, but still live: ble_machine_loader.cpp's processLoadCommand() constructs a
  // local INIT payload and passes it to handleMqttMessage(INIT_TOPIC, ...) to reuse the
  // existing load logic, so INIT_TOPIC must still be set correctly per machine/environment.
  updateMQTTTopics(machineNum, environment);
  LOG_INFO("====================================");
  
  // Set up the built-in LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn ON LED to show power
  
  // Initialize the I/O expander
  LOG_INFO("Trying to initialize TCA9535...");
  bool initSuccess = ioExpander.begin();

  // Retry a few times before giving up - a genuine wiring/hardware fault won't heal by
  // retrying, but a transient I2C bus glitch at power-on might.
  for (int attempt = 0; !initSuccess && attempt < 5; attempt++) {
    LOG_WARNING("TCA9535 init attempt %d failed, retrying...", attempt + 1);
    delay(200);
    initSuccess = ioExpander.begin();
  }

  if (!initSuccess) {
    // Previously this logged an error and fell through to "continue anyway", which
    // meant the mutexes, the button/coin queues, and the detector tasks below were
    // never created. The machine then silently: (1) accepted coins mechanically with
    // no software able to detect or credit them, since TaskCoinDetector never started;
    // (2) ignored every button press, since TaskButtonDetector never started; (3) gave
    // no signal to the operator that anything was wrong beyond a log line nobody was
    // watching. A device that dispenses a paid service must fail loudly, not run in a
    // half-working state indefinitely. So: halt here. setup() never returns, so loop()
    // and every FreeRTOS task below never start - no relay control, no coin
    // acceptance, no button response - while blinking a distinct fault pattern so a
    // technician on site can tell this apart from a healthy boot, and keep retrying in
    // case the fault is transient (e.g. a connector reseated after a power cycle).
    LOG_ERROR("TCA9535 initialization failed after retries - halting, will not accept coins or serve washes.");
    while (true) {
      // Fault pattern: 3 fast blinks, pause. A healthy boot's LED is solid on.
      for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(80);
        digitalWrite(LED_PIN, LOW);
        delay(80);
      }
      delay(1500);

      LOG_WARNING("Retrying TCA9535 initialization...");
      if (ioExpander.begin()) {
        LOG_INFO("TCA9535 recovered - restarting to reinitialize cleanly.");
        ESP.restart();
      }
    }
  }

  {
    LOG_INFO("TCA9535 initialization successful!");
    
    // Configure Port 0 (buttons) as inputs (1 = input, 0 = output)
    ioExpander.configurePortAsInput(0, 0xFF);
    
    // Configure Port 1 (relays) as outputs (1 = input, 0 = output)
    ioExpander.configurePortAsOutput(1, 0xFF);
    
    // Initialize all relays to OFF state
    ioExpander.writeRegister(OUTPUT_PORT1, 0x00);
    
    // Verify Port 1 configuration
    uint8_t configPort1Verify = ioExpander.readRegister(CONFIG_PORT1);
    LOG_INFO("Port 1 Configuration Register: 0x%02X (should be 0x00 for all outputs)", configPort1Verify);
    if (configPort1Verify != 0x00) {
        LOG_ERROR("WARNING: Port 1 not fully configured as outputs! Some pins may be inputs.");
        LOG_ERROR("Port 1 Config: 0x%02X (binary: %d%d%d%d%d%d%d%d)", 
                 configPort1Verify,
                 (configPort1Verify & 0x80) ? 1 : 0, (configPort1Verify & 0x40) ? 1 : 0,
                 (configPort1Verify & 0x20) ? 1 : 0, (configPort1Verify & 0x10) ? 1 : 0,
                 (configPort1Verify & 0x08) ? 1 : 0, (configPort1Verify & 0x04) ? 1 : 0,
                 (configPort1Verify & 0x02) ? 1 : 0, (configPort1Verify & 0x01) ? 1 : 0);
    }
    
    // Verify initial relay state
    uint8_t initialRelayState = ioExpander.readRegister(OUTPUT_PORT1);
    LOG_INFO("Initial Port 1 Output State: 0x%02X (all relays should be OFF)", initialRelayState);
    
    // Note: the TCA9535 INT pin is not used - coin and button detection is
    // done by polling tasks (TaskCoinDetector / TaskButtonDetector)
    pinMode(INT_PIN, INPUT_PULLUP);

    // Read initial state
    uint8_t initialPortValue = ioExpander.readRegister(INPUT_PORT0);
    LOG_INFO("Initial port value: 0x%02X", initialPortValue);
    LOG_INFO("Initial COIN_SIG state: %d (0 = idle, 1 = coin/noise present)", (initialPortValue & (1 << COIN_SIG)) ? 1 : 0);
    
    LOG_INFO("TCA9535 fully initialized. Ready to control relays and read buttons.");
    
  // Initialize FreeRTOS mutexes for shared resources
  LOG_INFO("Initializing FreeRTOS mutexes...");
  xIoExpanderMutex = xSemaphoreCreateMutex();
  xControllerMutex = xSemaphoreCreateMutex();
  xI2CMutex = xSemaphoreCreateMutex();  // For Wire1 (LCD)
  
  if (xIoExpanderMutex == NULL || xControllerMutex == NULL || xI2CMutex == NULL) {
        LOG_ERROR("Failed to create mutexes!");
    } else {
        LOG_INFO("Mutexes created successfully");
    }
    
    // Initialize FreeRTOS queue for debounced button press events.
    // Sized above NUM_BUTTONS (6) so a burst where every button transitions
    // in the same 10ms poll pass can still be fully enqueued without drops.
    LOG_INFO("Initializing button event queue...");
    xButtonEventQueue = xQueueCreate(8, sizeof(ButtonEvent));

    if (xButtonEventQueue == NULL) {
        LOG_ERROR("Failed to create button event queue!");
    } else {
        LOG_INFO("Button event queue created successfully (size: 8)");
    }

    // Create FreeRTOS tasks for dedicated interrupt handling
    LOG_INFO("Creating FreeRTOS tasks for coin and button detection...");
    
    xTaskCreate(
        TaskCoinDetector,           // Task function
        "CoinDetection",            // Task name
        4096,                       // Stack size (increased from 2048 to prevent overflow)
        NULL,                       // Task parameters
        2,                          // Priority above button task: the coin pulse is the
                                    // shortest signal on the board and must not lose
                                    // mutex arbitration to slower pollers
        &TaskCoinDetectorHandle     // Task handle
    );
    
    xTaskCreate(
        TaskButtonDetector,         // Task function
        "ButtonDetector",           // Task name (for debugging)
        4096,                       // Stack size (bytes)
        NULL,                       // Task parameters
        1,                          // Priority (5 = highest priority for immediate button response)
        &TaskButtonDetectorHandle   // Task handle
    );
    
    LOG_INFO("Coin detection task created successfully!");
    LOG_INFO("=== READY FOR COIN DETECTION ===");
    LOG_INFO("Insert coins to test detection...");
  }
  // Initialize Wire1 for the 7-segment display
  LOG_INFO("Initializing Wire1 (I2C) for 7-segment display...");
  Wire1.begin(DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
  Wire1.setClock(100000); // Set I2C clock to 100kHz (standard mode)
  
  // Initialize the controller
  controller = new CarWashController();
  
  // Initialize the 7-segment display
  display = new DisplayManager(DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
  // Set I2C mutex for display manager
  display->setI2CMutex(xI2CMutex);
  
  
  // Initialize BLE Machine Loader for direct machine loading
  LOG_INFO("Initializing BLE Machine Loader...");
  bleMachineLoader = new BLEMachineLoader();
  if (bleMachineLoader->begin(machineNum, controller)) {
    LOG_INFO("BLE Machine Loader initialized successfully!");
    LOG_INFO("Device name: FullWash-%s", machineNum.c_str());
    LOG_INFO("Machine will advertise via BLE when FREE");
  } else {
    LOG_ERROR("Failed to initialize BLE Machine Loader");
  }
  
  // Create Watchdog task (monitors system health)
  LOG_INFO("Creating Watchdog task...");
  xTaskCreatePinnedToCore(
      TaskWatchdog,                 // Task function
      "Watchdog",                   // Task name
      2048,                         // Stack size (bytes)
      NULL,                         // Task parameters
      1,                            // Priority (1 = lowest priority - monitoring only)
      &TaskWatchdogHandle,          // Task handle
      1                             // Pin to core 1 (APP CPU)
  );
  
  // Create Display Update task (handles LCD refresh independently)
  LOG_INFO("Creating Display Update task...");
  xTaskCreatePinnedToCore(
      TaskDisplayUpdate,            // Task function
      "DisplayUpdate",              // Task name
      4096,                         // Stack size (bytes) - needs more for String operations
      NULL,                         // Task parameters
      3,                            // Priority (3 = medium-high, same as coin detector)
      &TaskDisplayUpdateHandle,     // Task handle
      0                             // Pin to core 0 (PRO CPU) - keep display responsive
  );
  
  
  LOG_INFO("All FreeRTOS tasks created successfully");
  
  // Final blink pattern to indicate setup complete
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(200);
    digitalWrite(LED_PIN, HIGH);
    delay(200);
  }
}

void loop() {
  // Timing variables for various operations
  static unsigned long lastIoDebugCheck = 0;
  static unsigned long lastLedToggle = 0;
  static unsigned long lastBleUpdate = 0;
  static bool ledState = HIGH;
  static bool lastMachineFree = true;
  
  // Current time
  unsigned long currentTime = millis();
  
  // Update BLE machine loader state and advertising
  if (bleMachineLoader && currentTime - lastBleUpdate > 1000) {
    lastBleUpdate = currentTime;
    bleMachineLoader->update();
    
    // Manage BLE advertising based on machine state
    if (controller) {
      bool isMachineFree = (controller->getCurrentState() == STATE_FREE);
      
      // Start advertising when machine becomes FREE
      if (isMachineFree && !lastMachineFree) {
        LOG_INFO("Machine is FREE - starting BLE advertising");
        bleMachineLoader->startAdvertising();
      }
      // Stop advertising when machine is no longer FREE
      else if (!isMachineFree && lastMachineFree) {
        LOG_INFO("Machine is loaded - stopping BLE advertising");
        bleMachineLoader->stopAdvertising();
      }
      
      lastMachineFree = isMachineFree;
    }
  }
  
  // NOTE: Network operations disabled - using BLE only
  
    // Periodic check (no logging to reduce overhead)
    if (currentTime - lastIoDebugCheck > 4000) {  // Every 4 seconds
        lastIoDebugCheck = currentTime;
    }

  // NOTE: Coin and button detection is done by FreeRTOS tasks (TaskCoinDetector and TaskButtonDetector)

  // Run controller update - now processes flags set by FreeRTOS tasks
  // CRITICAL: Call update() continuously for responsive button handling
  // Removed timing check to ensure update() runs as frequently as possible
  // This ensures button flags are processed immediately when set
if (controller) {
      controller->update();
  }
  
  
  // NOTE: Display updates are now handled by TaskDisplayUpdate FreeRTOS task
  // Removed from main loop to ensure consistent refresh rate
  
  // Handle LED indicator
  // Simple pattern for BLE mode
  if (controller && controller->isMachineLoaded()) {
    // Solid LED when machine is loaded
    digitalWrite(LED_PIN, HIGH);
    ledState = HIGH;
  } else if (bleMachineLoader && bleMachineLoader->isConnected()) {
    // Fast blink when BLE client is connected
    if (currentTime - lastLedToggle > 300) {
      lastLedToggle = currentTime;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else {
    // Slow blink when waiting for BLE connection
    if (currentTime - lastLedToggle > 1000) {
      lastLedToggle = currentTime;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
  
  // Use FreeRTOS delay to yield to other tasks without blocking
  // This allows FreeRTOS to schedule button/coin detector tasks and network tasks
  // 1 tick = typically 10ms, but we use 1ms to ensure frequent updates
  vTaskDelay(pdMS_TO_TICKS(1));
}
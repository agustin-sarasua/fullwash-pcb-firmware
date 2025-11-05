#include <Arduino.h>
#include <Wire.h>
#include "mqtt_lte_client.h"
#include "io_expander.h"
#include "utilities.h"
#include "constants.h"
#include "car_wash_controller.h"
#include "logger.h"
#include "display_manager.h"
#include "rtc_manager.h"

#include "certs/AmazonRootCA.h"
#include "certs/AWSClientCertificate.h"
#include "certs/AWSClientPrivateKey.h"

// Wire1 is already defined in the ESP32 Arduino framework

// Server details
const char* AWS_BROKER = "a3foc0mc6v7ap0-ats.iot.us-east-1.amazonaws.com";
const char* AWS_CLIENT_ID = "fullwash-machine-001";
const uint16_t AWS_BROKER_PORT = 8883;

// GSM connection settings
// const char apn[] = "internet";
const char apn[] = "antel.lte"; // Replace with your carrier's APN if needed
const char gprsUser[] = "";
const char gprsPass[] = "";
const char pin[] = "0281";

// Create MQTT LTE client
MqttLteClient mqttClient(SerialAT, MODEM_PWRKEY, MODEM_DTR, MODEM_FLIGHT, MODEM_TX, MODEM_RX);

// Create IO Expander
IoExpander ioExpander(TCA9535_ADDR, I2C_SDA_PIN, I2C_SCL_PIN, INT_PIN);

// Create RTC Manager (uses Wire1, shared with LCD)
RTCManager* rtcManager;

// Create controller
CarWashController* controller;

// Create display manager
DisplayManager* display;

// FreeRTOS task handles
TaskHandle_t TaskCoinDetectorHandle = NULL;
TaskHandle_t TaskButtonDetectorHandle = NULL;
TaskHandle_t TaskNetworkManagerHandle = NULL;
TaskHandle_t TaskWatchdogHandle = NULL;

// FreeRTOS mutexes for shared resources
SemaphoreHandle_t xIoExpanderMutex = NULL;
SemaphoreHandle_t xControllerMutex = NULL;

/**
 * FreeRTOS Task: Coin Detector
 * 
 * This task monitors the coin acceptor signal pin (COIN_SIG) for state changes.
 * It runs independently to detect coin insertions reliably without blocking the main loop.
 * 
 * Detection Logic:
 * - Monitors INT_PIN (active LOW when hardware detects a change)
 * - Reads PORT0 register to get current coin signal state
 * - Detects HIGH->LOW transition (coin insertion)
 * - Sets coin signal flag for controller to process
 * 
 * Thread Safety:
 * - Uses mutex protection when accessing ioExpander
 */
void TaskCoinDetector(void *pvParameters) {
    const TickType_t xDelay = 50 / portTICK_PERIOD_MS; // 50ms polling interval
    uint8_t coins_sig_state = (1 << COIN_SIG); // Initialize to HIGH (no coin)
    uint8_t _portVal = 0;
    
    // Wait for IO expander to be fully initialized
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    
    LOG_INFO("Coin detector task started");
    
    for(;;) {
        // Check if interrupt pin is LOW (hardware detected a change)
        if (digitalRead(INT_PIN) == LOW) {
            // Protect IO expander access with mutex
            if (xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                _portVal = ioExpander.readRegister(INPUT_PORT0);
                
                // Check if COIN_SIG state has changed
                if (coins_sig_state != (_portVal & (1 << COIN_SIG))) {
                    // Detect HIGH->LOW transition (coin inserted)
                    if (coins_sig_state == (1 << COIN_SIG)) {
                        ioExpander.setCoinSignal(1);
                        ioExpander._intCnt++;
                        LOG_DEBUG("Interrupt detected! Port 0 Value: 0x%02X, coins times: %d", 
                                 _portVal, ioExpander._intCnt);
                    }
                    
                    // Update state for next comparison
                    coins_sig_state = (_portVal & (1 << COIN_SIG));
                }
                
                xSemaphoreGive(xIoExpanderMutex);
            } else {
                LOG_WARNING("Failed to acquire IO expander mutex in coin detector task");
            }
        }
        
        vTaskDelay(xDelay); // Wait 50ms before next check
    }
}

/**
 * FreeRTOS Task: Button Detector
 * 
 * This task monitors all button pins (BUTTON1-6) for press events.
 * It runs independently with faster polling for responsive button detection.
 * 
 * Detection Logic:
 * - Monitors INT_PIN (active LOW when button state changes)
 * - Reads PORT0 register to get current button states
 * - Detects button press events (HIGH->LOW transition, buttons are active LOW)
 * - Sets button flags with debouncing for controller to process
 * 
 * Thread Safety:
 * - Uses mutex protection when accessing ioExpander
 */
void TaskButtonDetector(void *pvParameters) {
    const TickType_t xDelay = 20 / portTICK_PERIOD_MS; // 20ms for responsive button detection
    uint8_t lastPortValue = 0xFF; // All buttons released initially (active LOW)
    
    // Wait for IO expander to be initialized
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    
    LOG_INFO("Button detector task started");
    
    for(;;) {
        // Check if interrupt pin is LOW (button state change detected)
        if (digitalRead(INT_PIN) == LOW) {
            uint8_t currentPortValue = 0;
            
            // Protect IO expander access with mutex
            if (xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                currentPortValue = ioExpander.readRegister(INPUT_PORT0);
                
                // Check if any button state changed
                if (currentPortValue != lastPortValue) {
                    LOG_DEBUG("Button state change detected. Port: 0x%02X -> 0x%02X", 
                             lastPortValue, currentPortValue);
                    
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
                            LOG_INFO("Button %d pressed detected in task", i + 1);
                            ioExpander.setButtonFlag(i, true);
                        }
                    }
                    
                    lastPortValue = currentPortValue;
                }
                
                xSemaphoreGive(xIoExpanderMutex);
            } else {
                LOG_WARNING("Failed to acquire IO expander mutex in button detector task");
            }
        }
        
        vTaskDelay(xDelay); // Wait 20ms before next check
    }
}

/**
 * FreeRTOS Task: Network Manager
 * 
 * This task handles all network and MQTT operations to prevent blocking the main loop.
 * It manages:
 * - Network connection monitoring
 * - MQTT connection and reconnection
 * - MQTT message processing
 * - Network status updates
 * 
 * Priority: 2 (Medium priority - important but not critical like hardware tasks)
 */
void TaskNetworkManager(void *pvParameters) {
    const TickType_t xNetworkCheckDelay = pdMS_TO_TICKS(30000);  // Check network every 30 seconds
    const TickType_t xMqttCheckDelay = pdMS_TO_TICKS(5000);      // Check MQTT every 5 seconds
    const TickType_t xReconnectDelay = pdMS_TO_TICKS(60000);     // Reconnect attempt interval
    
    unsigned long lastNetworkCheck = 0;
    unsigned long lastConnectionAttempt = 0;
    unsigned long lastMqttReconnectAttempt = 0;
    unsigned long lastStatusCheck = 0;
    
    LOG_INFO("Network manager task started");
    
    // Wait a bit for system to initialize
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    // Log initial stack high water mark
    UBaseType_t initialStackHighWater = uxTaskGetStackHighWaterMark(NULL);
    LOG_INFO("Network task initial stack high water mark: %d bytes", initialStackHighWater);
    
    for(;;) {
        unsigned long currentTime = millis();
        
        // Check network status periodically
        if (currentTime - lastNetworkCheck > 30000) {
            lastNetworkCheck = currentTime;
            
            if (!mqttClient.isNetworkConnected()) {
                LOG_WARNING("Lost cellular network connection");
                
                // Only attempt reconnection every 60 seconds
                if (currentTime - lastConnectionAttempt > 60000) {
                    lastConnectionAttempt = currentTime;
                    
                    LOG_INFO("Attempting to reconnect to cellular network...");
                    
                    // Monitor stack before network operation
                    UBaseType_t stackBefore = uxTaskGetStackHighWaterMark(NULL);
                    LOG_DEBUG("Stack before network init: %d bytes remaining", stackBefore);
                    
                    if (mqttClient.begin(apn, gprsUser, gprsPass, pin)) {
                        LOG_INFO("Successfully reconnected to cellular network!");
                        
                        // Validate IP address
                        String ip = mqttClient.getLocalIP();
                        if (mqttClient.isValidIP(ip)) {
                            LOG_INFO("Valid IP address obtained: %s", ip.c_str());
                        } else {
                            LOG_ERROR("Invalid IP address: %s - skipping MQTT connection attempt", ip.c_str());
                            vTaskDelay(10000 / portTICK_PERIOD_MS); // Wait 10s before retry
                            continue; // Skip to next iteration
                        }
                        
                        // Log signal quality
                        int signalQuality = mqttClient.getSignalQuality();
                        LOG_INFO("Signal quality: %d/31", signalQuality);
                        
                        // Monitor stack after network operation
                        UBaseType_t stackAfter = uxTaskGetStackHighWaterMark(NULL);
                        LOG_DEBUG("Stack after network init: %d bytes remaining", stackAfter);
                        
                        // Reconfigure SSL certificates
                        mqttClient.setCACert(AmazonRootCA);
                        mqttClient.setCertificate(AWSClientCertificate);
                        mqttClient.setPrivateKey(AWSClientPrivateKey);
                        
                        // Monitor stack before SSL connection (this is the critical part)
                        UBaseType_t stackBeforeSSL = uxTaskGetStackHighWaterMark(NULL);
                        LOG_DEBUG("Stack before MQTT SSL connect: %d bytes remaining", stackBeforeSSL);
                        
                        // Connect to MQTT broker with improved error handling
                        if (mqttClient.connect(AWS_BROKER, AWS_BROKER_PORT, AWS_CLIENT_ID)) {
                            LOG_INFO("MQTT broker connection restored!");
                            
                            // Monitor stack after SSL connection
                            UBaseType_t stackAfterSSL = uxTaskGetStackHighWaterMark(NULL);
                            LOG_DEBUG("Stack after MQTT SSL connect: %d bytes remaining", stackAfterSSL);
                            mqttClient.subscribe(INIT_TOPIC.c_str());
                            mqttClient.subscribe(CONFIG_TOPIC.c_str());
                            mqttClient.subscribe(COMMAND_TOPIC.c_str());
                            
                            // Notify that we're back online
                            if (controller) {
                                vTaskDelay(4000 / portTICK_PERIOD_MS);
                                controller->publishMachineSetupActionEvent();
                            }
                        } else {
                            LOG_ERROR("Failed to connect to MQTT broker after network recovery");
                            LOG_INFO("Waiting 30 seconds before next attempt to allow SSL state to clear");
                            vTaskDelay(30000 / portTICK_PERIOD_MS); // Wait 30s after SSL failure
                        }
                    } else {
                        LOG_ERROR("Failed to reconnect to cellular network");
                        vTaskDelay(10000 / portTICK_PERIOD_MS); // Wait 10s before retry
                    }
                }
            } else {
                // Network is connected, but check MQTT connection
                if (!mqttClient.isConnected()) {
                    // Only attempt MQTT reconnection every 15 seconds
                    if (currentTime - lastMqttReconnectAttempt > 15000) {
                        lastMqttReconnectAttempt = currentTime;
                        LOG_WARNING("Network connected but MQTT disconnected, attempting to reconnect...");
                        mqttClient.reconnect();
                    }
                }
            }
        }
        
        // Process MQTT messages if connected
        if (mqttClient.isNetworkConnected()) {
            mqttClient.loop();
            
            // Status message every 60 seconds if connected
            if (currentTime - lastStatusCheck > 60000) {
                lastStatusCheck = currentTime;
                
                // Monitor stack usage during normal operation
                UBaseType_t stackHighWater = uxTaskGetStackHighWaterMark(NULL);
                int signalQuality = mqttClient.getSignalQuality();
                LOG_INFO("System running normally, network connected. Stack: %d bytes, Signal: %d/31", 
                         stackHighWater, signalQuality);
            }
        }
        
        // Small delay to prevent task from consuming too much CPU
        vTaskDelay(xMqttCheckDelay);
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
                if (stackHighWater < 256) { // Less than 256 bytes free
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
                if (stackHighWater < 256) {
                    LOG_WARNING("Button detector task stack low: %d bytes remaining", stackHighWater);
                }
            }
        }
        
        // Check network manager task
        if (TaskNetworkManagerHandle != NULL) {
            eTaskState networkTaskState = eTaskGetState(TaskNetworkManagerHandle);
            if (networkTaskState == eDeleted || networkTaskState == eInvalid) {
                LOG_ERROR("Network manager task died! State: %d", networkTaskState);
            } else {
                UBaseType_t stackHighWater = uxTaskGetStackHighWaterMark(TaskNetworkManagerHandle);
                if (stackHighWater < 1024) {  // Network task needs more headroom for SSL/TLS
                    LOG_WARNING("Network manager task stack low: %d bytes remaining", stackHighWater);
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
        
        // Periodic health status (every 5 checks = 50 seconds)
        static int checkCount = 0;
        checkCount++;
        if (checkCount >= 5) {
            checkCount = 0;
            LOG_INFO("System health check - Free heap: %d bytes, Min free: %d bytes", 
                    freeHeap, minFreeHeap);
        }
        
        vTaskDelay(xWatchdogDelay);
    }
}

void mqtt_callback(char *topic, byte *payload, unsigned int len) {
    LOG_INFO("Message arrived from topic: %s", topic);
    
    // Handle command topic specially for changing log level or debug commands
    if (String(topic) == COMMAND_TOPIC) {
        // Parse command JSON
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, payload, len);
        
        if (!error && doc.containsKey("command")) {
            String command = doc["command"].as<String>();
            
            if (command == "set_log_level" && doc.containsKey("level")) {
                String level = doc["level"].as<String>();
                
                if (level == "DEBUG") {
                    controller->setLogLevel(LOG_DEBUG);
                } else if (level == "INFO") {
                    controller->setLogLevel(LOG_INFO);
                } else if (level == "WARNING") {
                    controller->setLogLevel(LOG_WARNING);
                } else if (level == "ERROR") {
                } else if (level == "NONE") {
                    controller->setLogLevel(LOG_NONE);
                }
            }
            // Add test command for simulating coin insertion
            else if (command == "simulate_coin") {
                LOG_INFO("Received command to simulate coin insertion");
                controller->simulateCoinInsertion();
            }
            // Add advanced coin signal simulation options
            else if (command == "test_coin_signal" && doc.containsKey("pattern")) {
                String pattern = doc["pattern"].as<String>();
                LOG_INFO("Testing coin acceptor with pattern: %s", pattern.c_str());
                
                extern IoExpander ioExpander;
                
                if (pattern == "high_low_high") {
                    // Simulate a SIG pin toggling HIGH->LOW->HIGH
                    LOG_INFO("Simulating HIGH->LOW->HIGH pattern");
                    // We can't directly set input pins, so this is for testing only
                    controller->simulateCoinInsertion();
                }
                else if (pattern == "toggle") {
                    // Just toggle the coin trigger function
                    LOG_INFO("Simply toggling the coin detector");
                    controller->simulateCoinInsertion();
                }
                else if (pattern == "counter") {
                    // Trigger based on CNT pin
                    LOG_INFO("Simulating coin counter pulse");
                    controller->simulateCoinInsertion();
                }
                else if (pattern == "debug") {
                    // Special diagnostic mode to read the raw coin signals
                    LOG_INFO("=== COIN ACCEPTOR DIAGNOSTIC ===");
                    
                    // Read raw port value
                    uint8_t rawPortValue0 = ioExpander.readRegister(INPUT_PORT0);
                    
                    // Log the raw values in different formats
                    LOG_INFO("Raw port value: 0x%02X | Binary: %d%d%d%d%d%d%d%d", 
                           rawPortValue0,
                           (rawPortValue0 & 0x80) ? 1 : 0, (rawPortValue0 & 0x40) ? 1 : 0,
                           (rawPortValue0 & 0x20) ? 1 : 0, (rawPortValue0 & 0x10) ? 1 : 0,
                           (rawPortValue0 & 0x08) ? 1 : 0, (rawPortValue0 & 0x04) ? 1 : 0,
                           (rawPortValue0 & 0x02) ? 1 : 0, (rawPortValue0 & 0x01) ? 1 : 0);
                    
                    // Check the COIN_SIG bit specifically
                    bool coin_bit = (rawPortValue0 & (1 << COIN_SIG)) ? 1 : 0;
                    LOG_INFO("COIN_SIG (bit %d) = %d", COIN_SIG, coin_bit);
                    
                    // Hardware with 100KOhm pull-up resistor:
                    // Bit=1 (HIGH): No coin present (default state with pull-up) = INACTIVE
                    // Bit=0 (LOW): Coin inserted (pull-down when coin connects to ground) = ACTIVE
                    bool coinActive = ((rawPortValue0 & (1 << COIN_SIG)) == 0);
                    
                    LOG_INFO("Current coin state: %s", 
                            coinActive ? "ACTIVE (coin present, LOW/0)" : "INACTIVE (no coin, HIGH/1)");
                    
                    // Explain hardware configuration
                    LOG_INFO("Hardware config: 100KOhm pull-up resistor");
                    LOG_INFO("- Default state (no coin): Pin pulled HIGH (bit=1) = INACTIVE");
                    LOG_INFO("- Coin inserted: Pin connected to ground/LOW (bit=0) = ACTIVE");
                }
            }
            // Add debug command to print IO expander state
            else if (command == "debug_io") {
                LOG_INFO("Printing IO expander debug info");
                extern IoExpander ioExpander;
                ioExpander.printDebugInfo();
            }
            // Add debug command to print RTC state
            else if (command == "debug_rtc") {
                LOG_INFO("Printing RTC debug info");
                extern RTCManager* rtcManager;
                if (rtcManager && rtcManager->isInitialized()) {
                    rtcManager->printDebugInfo();
                } else {
                    LOG_WARNING("RTC is not initialized");
                }
            }
            // Add command to manually set RTC time from server
            else if (command == "sync_rtc" && doc.containsKey("timestamp")) {
                String timestamp = doc["timestamp"].as<String>();
                LOG_INFO("Manual RTC sync requested with timestamp: %s", timestamp.c_str());
                extern RTCManager* rtcManager;
                if (rtcManager && rtcManager->isInitialized()) {
                    if (rtcManager->setDateTimeFromISO(timestamp)) {
                        LOG_INFO("RTC synchronized successfully!");
                        rtcManager->printDebugInfo();
                    } else {
                        LOG_ERROR("Failed to sync RTC");
                    }
                } else {
                    LOG_WARNING("RTC is not initialized");
                }
            }
        }
    } else if (controller) {
        LOG_DEBUG("Handling MQTT message...");
        controller->handleMqttMessage(topic, payload, len);
    }
}

void setup() {
  // Initialize Logger with default log level
  Logger::init(DEFAULT_LOG_LEVEL, 115200);
  delay(1000); // Give time for serial to initialize
  
  LOG_INFO("Starting fullwash-pcb-firmware...");
  
  // Set up the built-in LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn ON LED to show power
  
  // Initialize the I/O expander
  LOG_INFO("Trying to initialize TCA9535...");
  bool initSuccess = ioExpander.begin();
  
  if (!initSuccess) {
    LOG_ERROR("Failed to initialize TCA9535!");
    LOG_WARNING("Will continue without initialization. Check connections.");
    
    // Blink LED rapidly to indicate error
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(100);
    }
    
    // Continue anyway - don't get stuck in a loop
  } else {
    LOG_INFO("TCA9535 initialization successful!");
    
    // Configure Port 0 (buttons) as inputs (1 = input, 0 = output)
    LOG_DEBUG("Configuring Port 0 as inputs...");
    ioExpander.configurePortAsInput(0, 0xFF);
    
    // Configure Port 1 (relays) as outputs (1 = input, 0 = output)
    LOG_DEBUG("Configuring Port 1 as outputs...");
    ioExpander.configurePortAsOutput(1, 0xFF);
    
    // Initialize all relays to OFF state
    LOG_DEBUG("Setting all relays to OFF...");
    ioExpander.writeRegister(OUTPUT_PORT1, 0x00);
    
    // Enable interrupt for all input pins (buttons + coin acceptor)
    LOG_INFO("Enabling interrupt for all input pins (buttons + coin acceptor)...");
    // Enable interrupt for all pins on port 0: buttons (0-5) + coin acceptor (6-7)
    ioExpander.enableInterrupt(0, 0xFF); // All 8 pins
    
    LOG_INFO("TCA9535 fully initialized. Ready to control relays and read buttons.");
    
    // Configure INT_PIN with internal pull-up for reliable detection
    pinMode(INT_PIN, INPUT_PULLUP);
    
    // Initialize FreeRTOS mutexes for shared resources
    LOG_INFO("Initializing FreeRTOS mutexes...");
    xIoExpanderMutex = xSemaphoreCreateMutex();
    xControllerMutex = xSemaphoreCreateMutex();
    
    if (xIoExpanderMutex == NULL || xControllerMutex == NULL) {
        LOG_ERROR("Failed to create mutexes!");
    } else {
        LOG_INFO("Mutexes created successfully");
    }
    
    // Create FreeRTOS tasks for dedicated interrupt handling
    LOG_INFO("Creating FreeRTOS tasks for coin and button detection...");
    
    xTaskCreate(
        TaskCoinDetector,           // Task function
        "CoinDetector",             // Task name (for debugging)
        2048,                       // Stack size (bytes)
        NULL,                       // Task parameters
        3,                          // Priority (3 = medium-high priority for hardware)
        &TaskCoinDetectorHandle     // Task handle
    );
    
    xTaskCreate(
        TaskButtonDetector,         // Task function
        "ButtonDetector",           // Task name (for debugging)
        2048,                       // Stack size (bytes)
        NULL,                       // Task parameters
        4,                          // Priority (4 = higher than coin task for responsiveness)
        &TaskButtonDetectorHandle   // Task handle
    );
    
    LOG_INFO("FreeRTOS tasks created successfully (CoinDetector: priority 3, ButtonDetector: priority 4)");
  }
  // Initialize Wire1 for the LCD display and RTC (shared I2C bus)
  LOG_INFO("Initializing Wire1 (I2C) for LCD and RTC...");
  Wire1.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Wire1.setClock(100000); // Set I2C clock to 100kHz (standard mode)
  
  // Initialize the RTC manager
  LOG_INFO("Initializing RTC Manager...");
  rtcManager = new RTCManager(RTC_DS1340_ADDR, &Wire1);
  
  if (rtcManager->begin()) {
    LOG_INFO("RTC initialization successful!");
    rtcManager->printDebugInfo();
  } else {
    LOG_ERROR("Failed to initialize RTC!");
    LOG_WARNING("System will continue without RTC. Timestamps may be inaccurate.");
  }
  
  // Initialize the controller with RTC
  controller = new CarWashController(mqttClient);
  controller->setRTCManager(rtcManager);
  
  // Initialize the display with correct LCD pins
  display = new DisplayManager(LCD_ADDR, LCD_COLS, LCD_ROWS, LCD_SDA_PIN, LCD_SCL_PIN);
  // Initialize MQTT client with callback
  mqttClient.setCallback(mqtt_callback);
  mqttClient.setBufferSize(512);

  // Initialize modem and connect to network (in setup, network task will handle reconnections)
  LOG_INFO("Initializing modem and connecting to network...");
  if (mqttClient.begin(apn, gprsUser, gprsPass, pin)) {
    // Configure SSL certificates
    mqttClient.setCACert(AmazonRootCA);
    mqttClient.setCertificate(AWSClientCertificate);
    mqttClient.setPrivateKey(AWSClientPrivateKey);
    
    // Connect to MQTT broker
    LOG_INFO("Connecting to MQTT broker...");
    if (mqttClient.connect(AWS_BROKER, AWS_BROKER_PORT, AWS_CLIENT_ID)) {
      LOG_INFO("Connected to MQTT broker!");
      
      LOG_DEBUG("Subscribing to topic: %s", INIT_TOPIC.c_str());
      bool result = mqttClient.subscribe(INIT_TOPIC.c_str());
      LOG_DEBUG("Subscription result: %s", result ? "true" : "false");
      
      result = mqttClient.subscribe(CONFIG_TOPIC.c_str());
      LOG_DEBUG("Subscription result: %s", result ? "true" : "false");
      
      result = mqttClient.subscribe(COMMAND_TOPIC.c_str());
      LOG_DEBUG("Subscription to command topic: %s", result ? "true" : "false");
      
      delay(4000);
      LOG_INFO("Publishing Setup Action Event...");
      controller->publishMachineSetupActionEvent();
    } else {
      LOG_ERROR("Failed to connect to MQTT broker");
    }
  } else {
    LOG_ERROR("Failed to initialize modem");
  }
  
  // Create Network Manager task (handles all network/MQTT operations)
  LOG_INFO("Creating Network Manager task...");
  xTaskCreate(
      TaskNetworkManager,           // Task function
      "NetworkManager",             // Task name
      16384,                        // Stack size (bytes) - SSL/TLS requires large stack (16KB)
      NULL,                         // Task parameters
      2,                            // Priority (2 = medium priority)
      &TaskNetworkManagerHandle     // Task handle
  );
  
  // Create Watchdog task (monitors system health)
  LOG_INFO("Creating Watchdog task...");
  xTaskCreate(
      TaskWatchdog,                 // Task function
      "Watchdog",                   // Task name
      2048,                         // Stack size (bytes)
      NULL,                         // Task parameters
      1,                            // Priority (1 = lowest priority - monitoring only)
      &TaskWatchdogHandle           // Task handle
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
  static unsigned long lastButtonCheck = 0;
  static unsigned long lastIoDebugCheck = 0;
  static unsigned long lastLedToggle = 0;
  static bool ledState = HIGH;
  
  // Current time
  unsigned long currentTime = millis();
  
  // NOTE: Network operations are now handled by TaskNetworkManager
  // No need to check network status or process MQTT messages here
  
  // Periodically check I/O expander raw values for debugging
  if (currentTime - lastIoDebugCheck > 4000) {  // Every 4 seconds
    lastIoDebugCheck = currentTime;
    LOG_DEBUG("Machine state: %s, Machine loaded: %d, Formatted: %s", 
             getMachineStateString(controller->getCurrentState()),
             controller->isMachineLoaded(),
             controller->getTimestamp().c_str());  
  }

  // NOTE: Interrupt handling is now done by FreeRTOS tasks (TaskCoinDetector and TaskButtonDetector)
  // The old ioExpander.handleInterrupt() call is no longer needed
  
  // Run controller update - now processes flags set by FreeRTOS tasks
  if (currentTime - lastButtonCheck > 50) {
    lastButtonCheck = currentTime;
    
    if (controller) {
      controller->update();
    }
  }
  
  // Update display with current state
  if (display && controller) {
    display->update(controller);
  }
  
  // Handle LED indicator
  // Blink pattern based on connection status
  // Note: Network status is checked less frequently to avoid blocking
  if (mqttClient.isConnected()) {
    // Solid LED when fully connected
    digitalWrite(LED_PIN, HIGH);
    ledState = HIGH;
  } else if (mqttClient.isNetworkConnected()) {
    // Slow blink when network is connected but MQTT is not
    if (currentTime - lastLedToggle > 1000) {
      lastLedToggle = currentTime;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else {
    // Fast blink when not connected to network
    if (currentTime - lastLedToggle > 300) {
      lastLedToggle = currentTime;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
  
  // Small delay to prevent loop from consuming too much CPU
  // FreeRTOS will schedule other tasks during this delay
  delay(10);
}
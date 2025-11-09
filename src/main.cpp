#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "mqtt_lte_client.h"
#include "io_expander.h"
#include "utilities.h"
#include "constants.h"
#include "car_wash_controller.h"
#include "logger.h"
#include "display_manager.h"
#include "rtc_manager.h"
#include "ble_config_manager.h"
#include "ble_machine_loader.h"

// LTE/MQTT functionality commented out - using BLE only
// #include "certs/AmazonRootCA.h"
// #include "certs/AWSClientCertificate.h"
// #include "certs/AWSClientPrivateKey.h"

// Wire1 is already defined in the ESP32 Arduino framework

// Server details
const char* AWS_BROKER = "a3foc0mc6v7ap0-ats.iot.us-east-1.amazonaws.com";
String AWS_CLIENT_ID = "fullwash-machine-001";  // Will be updated dynamically based on BLE config
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

// Create BLE config manager
BLEConfigManager* bleConfigManager;

// Create BLE machine loader
BLEMachineLoader* bleMachineLoader;

// FreeRTOS task handles
TaskHandle_t TaskCoinDetectorHandle = NULL;
TaskHandle_t TaskButtonDetectorHandle = NULL;
TaskHandle_t TaskNetworkManagerHandle = NULL;
TaskHandle_t TaskWatchdogHandle = NULL;
TaskHandle_t TaskDisplayUpdateHandle = NULL;
TaskHandle_t TaskMqttPublisherHandle = NULL;

// FreeRTOS mutexes for shared resources
SemaphoreHandle_t xIoExpanderMutex = NULL;
SemaphoreHandle_t xControllerMutex = NULL;
SemaphoreHandle_t xI2CMutex = NULL;  // For Wire1 (LCD and RTC)

// FreeRTOS queue for MQTT message publishing
QueueHandle_t xMqttPublishQueue = NULL;

/**
 * FreeRTOS Task: Coin Detector (Simplified Working Version)
 * 
 * This task monitors the coin acceptor signal pin (COIN_SIG) for state changes.
 * Uses a simple state machine to detect HIGH->LOW transitions (coin insertion).
 * 
 * Detection Logic:
 * - Polls INT_PIN every 50ms
 * - When INT_PIN is LOW, reads PORT0 register
 * - Detects HIGH->LOW transition on COIN_SIG bit
 * - Sets coin signal flag for controller to process
 */
void TaskCoinDetector(void *pvParameters) {
  const TickType_t xDelay = 50 / portTICK_PERIOD_MS; // 50ms转换为FreeRTOS ticks
  uint8_t coins_sig_state = (1 << COIN_SIG);
  uint8_t _portVal = 0;
  for(;;) {
    // 任务A的工作代码
    if (digitalRead(INT_PIN) == LOW)
    {
        _portVal = ioExpander.readRegister(INPUT_PORT0);
        if(coins_sig_state != (_portVal & (1 << COIN_SIG)))
        {
            // 1--->0 硬币型号产生
            if(coins_sig_state == (1 << COIN_SIG))
            {
                ioExpander.setCoinSignal(1);
                ioExpander._intCnt++;
                LOG_INFO("COIN DETECTED! Port 0 Value: 0x%02X, coin count: %d", _portVal, ioExpander._intCnt);
                
                // Additional debug info
                bool coin_bit = (_portVal & (1 << COIN_SIG)) ? 1 : 0;
                LOG_INFO("COIN_SIG (bit %d) = %d", COIN_SIG, coin_bit);
                LOG_INFO("Coin transition: HIGH->LOW (coin inserted)");
            }

            coins_sig_state = (_portVal & (1 << COIN_SIG));
        }
    }
    
    vTaskDelay(xDelay); // 延迟50ms
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
    const TickType_t xDelay = 10 / portTICK_PERIOD_MS; // 10ms for very responsive button detection
    uint8_t lastPortValue = 0xFF; // All buttons released initially (active LOW)
    
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
        
        // Protect IO expander access with mutex
        // Reduced timeout to 20ms for faster failure and better responsiveness
        if (xSemaphoreTake(xIoExpanderMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            currentPortValue = ioExpander.readRegister(INPUT_PORT0);
            
            // Check if any button state changed
            if (currentPortValue != lastPortValue) {
                
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
                        LOG_INFO("Button %d transition detected: HIGH->LOW (pressed)", i + 1);
                        ioExpander.setButtonFlag(i, true);
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
        
        vTaskDelay(xDelay); // Wait 10ms before next check
    }
}

/**
 * FreeRTOS Task: Network Manager
 * 
 * COMMENTED OUT - Using BLE only, no LTE/MQTT
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
/* DISABLED - BLE ONLY MODE
void TaskNetworkManager(void *pvParameters) {
    // SMART CONNECTIVITY CHECKING: Check less frequently when things are working
    // Network checks are now handled by smart checking in mqtt_lte_client
    // - When connected: check every 120 seconds (reduced from 45s)
    // - When disconnected: check every 30 seconds
    const TickType_t xNetworkCheckDelayConnected = pdMS_TO_TICKS(120000);  // 2 minutes when connected
    const TickType_t xNetworkCheckDelayDisconnected = pdMS_TO_TICKS(30000);  // 30 seconds when disconnected
    const TickType_t xMqttCheckDelay = pdMS_TO_TICKS(15000);      // Check MQTT every 15 seconds (reduced frequency)
    const TickType_t xReconnectDelay = pdMS_TO_TICKS(60000);     // Reconnect attempt interval
    
    unsigned long lastNetworkCheck = 0;
    unsigned long lastConnectionAttempt = 0;
    unsigned long lastMqttReconnectAttempt = 0;
    unsigned long lastStatusCheck = 0;
    bool wasNetworkConnected = false;  // Track previous state for adaptive checking
    
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        LOG_INFO("Network manager task started");
    }
    
    // Wait a bit for system to initialize
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    for(;;) {
        unsigned long currentTime = millis();
        
        // SMART CONNECTIVITY CHECKING: Adaptive check interval based on connection state
        // Check less frequently when connected to reduce mutex contention
        TickType_t networkCheckInterval = wasNetworkConnected ? 
            xNetworkCheckDelayConnected : xNetworkCheckDelayDisconnected;
        
        // Check network status periodically - reduced frequency when connected
        // The mqtt_lte_client now handles smart checking based on publish failures
        if (currentTime - lastNetworkCheck > networkCheckInterval) {
            lastNetworkCheck = currentTime;
            
            // Yield before potentially long network operations
            vTaskDelay(pdMS_TO_TICKS(50));
            
            bool networkConnected = mqttClient.isNetworkConnected();
            wasNetworkConnected = networkConnected;  // Update state tracking
            
            if (!networkConnected) {
                if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                    LOG_WARNING("Lost cellular network connection");
                }
                
                // Only attempt reconnection every 60 seconds
                if (currentTime - lastConnectionAttempt > 60000) {
                    lastConnectionAttempt = currentTime;
                    
                    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                        LOG_INFO("Attempting to reconnect to cellular network...");
                    }
                    
                    // Yield before network operation to let IDLE task run
                    vTaskDelay(pdMS_TO_TICKS(100));
                    
                    // Try to recover the modem connection
                    if (mqttClient.begin(apn, gprsUser, gprsPass, pin)) {
                        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                            LOG_INFO("Successfully reconnected to cellular network!");
                        }
                        
                        // Validate IP address
                        String ip = mqttClient.getLocalIP();
                        if (mqttClient.isValidIP(ip)) {
                            // IP is valid, continue
                        } else {
                            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                                LOG_ERROR("Invalid IP address: %s - skipping MQTT connection attempt", ip.c_str());
                            }
                            vTaskDelay(10000 / portTICK_PERIOD_MS); // Wait 10s before retry
                            continue; // Skip to next iteration
                        }
                        
                        // Cleanup SSL client since network was lost (old SSL session is invalid)
                        mqttClient.cleanupSSLClient();
                        vTaskDelay(500 / portTICK_PERIOD_MS); // Reduced from 1000ms to prevent watchdog timeout
                        
                        // Reconfigure SSL certificates
                        mqttClient.setCACert(AmazonRootCA);
                        mqttClient.setCertificate(AWSClientCertificate);
                        mqttClient.setPrivateKey(AWSClientPrivateKey);
                        
                        // Yield before SSL connection (this can take several seconds)
                        vTaskDelay(pdMS_TO_TICKS(100));
                        
                        // Connect to MQTT broker with improved error handling
                        if (mqttClient.connect(AWS_BROKER, AWS_BROKER_PORT, AWS_CLIENT_ID.c_str())) {
                            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                                LOG_INFO("MQTT broker connection restored!");
                            }
                            
                            mqttClient.subscribe(INIT_TOPIC.c_str());
                            mqttClient.subscribe(CONFIG_TOPIC.c_str());
                            mqttClient.subscribe(COMMAND_TOPIC.c_str());
                            mqttClient.subscribe(GET_STATE_TOPIC.c_str());
                            
                            // Notify that we're back online
                            if (controller) {
                                vTaskDelay(4000 / portTICK_PERIOD_MS);
                                controller->publishMachineSetupActionEvent();
                            }
                        } else {
                            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                                LOG_ERROR("Failed to connect to MQTT broker after network recovery");
                            }
                            vTaskDelay(30000 / portTICK_PERIOD_MS); // Wait 30s after SSL failure
                        }
                    } else {
                        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                            LOG_ERROR("Failed to reconnect to cellular network");
                        }
                        vTaskDelay(10000 / portTICK_PERIOD_MS); // Wait 10s before retry
                    }
                }
            } else {
                // Network is connected, but check MQTT connection
                if (!mqttClient.isConnected()) {
                    // Only attempt MQTT reconnection every 15 seconds
                    if (currentTime - lastMqttReconnectAttempt > 15000) {
                        lastMqttReconnectAttempt = currentTime;
                        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                            LOG_WARNING("Network connected but MQTT disconnected, attempting to reconnect...");
                        }
                        
                        // Yield before reconnection attempt (SSL operations can block)
                        vTaskDelay(pdMS_TO_TICKS(50));
                        mqttClient.reconnect();
                    }
                }
            }
        }
        
        // Yield periodically even when not checking network
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Process MQTT messages if connected
        // SMART CONNECTIVITY CHECKING: Call loop() less frequently to reduce mutex contention
        // - When connected: call every 15 seconds (increased from 5s to reduce mutex contention)
        // - Skip loop() if publisher queue has messages waiting (publisher has priority)
        // - loop() handles its own smart checking internally
        static unsigned long lastLoopCall = 0;
        unsigned long now = millis();
        if (wasNetworkConnected && (now - lastLoopCall > 15000)) {
            // Check if publisher queue has messages waiting - if so, skip loop() to let publisher run first
            UBaseType_t queueDepth = 0;
            if (xMqttPublishQueue != NULL) {
                queueDepth = uxQueueMessagesWaiting(xMqttPublishQueue);
            }
            
            // Only call loop() if publisher queue is empty or very low
            // This prevents blocking the publisher task when it has work to do
            if (queueDepth == 0) {
                lastLoopCall = now;
                // Yield to higher priority tasks (publisher) before potentially blocking operation
                vTaskDelay(pdMS_TO_TICKS(50));
                
                mqttClient.loop();
            } else {
                // Publisher has messages waiting - skip loop() this time
                // Will try again on next iteration (after 15s delay)
                if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS && queueDepth > 3) {
                    LOG_DEBUG("Skipping MQTT loop() - publisher queue has %d messages waiting", queueDepth);
                }
            }
        }
        
        // Longer delay to prevent task from consuming too much CPU
        // This allows lower priority tasks (including IDLE task) to run
        // CRITICAL: Must delay here to prevent watchdog timeout
        vTaskDelay(pdMS_TO_TICKS(200));  // Increased to 200ms to give more time for IDLE task
    }
}
*/ // END DISABLED - BLE ONLY MODE

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
 * FreeRTOS Task: MQTT Publisher
 * 
 * COMMENTED OUT - Using BLE only, no LTE/MQTT
 * 
 * This task handles all MQTT message publishing in a dedicated task to prevent
 * blocking the main loop and other critical tasks. It:
 * - Consumes messages from xMqttPublishQueue
 * - Publishes messages when MQTT connection is available
 * - Buffers critical messages when disconnected (up to queue limit)
 * - Implements retry logic for failed publishes
 * 
 * Priority: 2 (Medium priority - important for data delivery)
 */
/* DISABLED - BLE ONLY MODE
void TaskMqttPublisher(void *pvParameters) {
    const TickType_t xQueueWaitTime = pdMS_TO_TICKS(100);  // Wait up to 100ms for messages
    const int MAX_RETRY_COUNT = 3;  // Maximum retry attempts for critical messages
    MqttMessage msg;
    
    LOG_INFO("MQTT Publisher task started");
    
    // Wait for MQTT client to be initialized
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    
    // Statistics counters
    unsigned long messagesPublished = 0;
    unsigned long messagesDropped = 0;
    unsigned long lastStatsLog = 0;
    
    // Track retry count per message using timestamp as identifier
    unsigned long lastMessageTimestamp = 0;
    int currentRetryCount = 0;
    
    for(;;) {
        // Check if there are messages waiting - process them quickly if queue is building up
        UBaseType_t queueDepth = 0;
        if (xMqttPublishQueue != NULL) {
            queueDepth = uxQueueMessagesWaiting(xMqttPublishQueue);
        }
        
        // Try to receive a message from the queue
        // Use shorter timeout if queue is building up to process faster
        // Reduced threshold from 10 to 3 to catch queue buildup earlier
        TickType_t waitTime = (queueDepth > 3) ? pdMS_TO_TICKS(5) : xQueueWaitTime;
        if (xQueueReceive(xMqttPublishQueue, &msg, waitTime) == pdTRUE) {
            // Check if this is a retry of the same message
            bool isRetry = (msg.timestamp == lastMessageTimestamp);
            if (!isRetry) {
                // New message - reset retry counter
                currentRetryCount = 0;
                lastMessageTimestamp = msg.timestamp;
            }
            // Note: retry count is incremented when re-queuing, not here
            
            // Check if MQTT is connected
            if (mqttClient.isConnected()) {
                // CRITICAL FIX: Use shorter timeout (50ms) to prevent blocking loop()
                // If mutex is held by loop(), we'll fail fast and retry
                // This prevents the publisher from monopolizing the mutex
                bool published = mqttClient.publishNonBlocking(msg.topic, msg.payload, msg.qos, 50);
                
                if (published) {
                    messagesPublished++;
                    LOG_DEBUG("Published MQTT message to %s (QoS: %d)", msg.topic, msg.qos);
                    // Reset retry tracking on success
                    lastMessageTimestamp = 0;
                    currentRetryCount = 0;
                } else {
                    // Publish failed - could be due to mutex contention or actual failure
                    // Re-queue the message to retry (both critical and non-critical)
                    // This handles mutex contention gracefully
                    if (currentRetryCount < MAX_RETRY_COUNT) {
                        // Try to re-queue for retry (put back at front for faster retry)
                        if (uxQueueSpacesAvailable(xMqttPublishQueue) > 0) {
                            if (xQueueSendToFront(xMqttPublishQueue, &msg, 0) == pdTRUE) {
                                currentRetryCount++;  // Increment retry count when re-queuing
                                if (msg.isCritical) {
                                    LOG_INFO("Re-queued critical message for retry (%d/%d) - mutex may have been busy", 
                                            currentRetryCount, MAX_RETRY_COUNT);
                                } else {
                                    LOG_DEBUG("Re-queued message for retry (%d/%d) - mutex may have been busy", 
                                            currentRetryCount, MAX_RETRY_COUNT);
                                }
                                // Update lastMessageTimestamp so we recognize this as a retry next time
                                lastMessageTimestamp = msg.timestamp;
                                // CRITICAL: Longer delay (200ms) to give loop() priority to acquire mutex
                                // This prevents publisher from immediately re-acquiring and blocking loop()
                                vTaskDelay(pdMS_TO_TICKS(200));
                            } else {
                                messagesDropped++;
                                LOG_WARNING("Failed to re-queue message");
                            }
                        } else {
                            messagesDropped++;
                            LOG_WARNING("Queue full, cannot retry message");
                        }
                    } else {
                        // Max retries reached
                        messagesDropped++;
                        if (msg.isCritical) {
                            LOG_WARNING("Critical message dropped after %d retries: %s", 
                                       currentRetryCount, msg.topic);
                        } else {
                            LOG_DEBUG("Non-critical message dropped after %d retries: %s", 
                                     currentRetryCount, msg.topic);
                        }
                        // Reset retry tracking
                        lastMessageTimestamp = 0;
                        currentRetryCount = 0;
                    }
                }
            } else {
                // MQTT not connected - buffer critical messages only
                if (msg.isCritical && currentRetryCount < MAX_RETRY_COUNT) {
                    // Try to re-queue critical messages if there's space
                    if (uxQueueSpacesAvailable(xMqttPublishQueue) > (MQTT_QUEUE_SIZE / 4)) {
                        // Only buffer if queue is less than 75% full
                        if (xQueueSendToBack(xMqttPublishQueue, &msg, 0) == pdTRUE) {
                            LOG_DEBUG("Buffered critical message (MQTT disconnected, retry %d/%d)", 
                                     currentRetryCount + 1, MAX_RETRY_COUNT);
                        } else {
                            messagesDropped++;
                            LOG_WARNING("Failed to buffer critical message");
                        }
                    } else {
                        messagesDropped++;
                        LOG_WARNING("Queue too full (>75%%), dropping message to prevent overflow");
                        lastMessageTimestamp = 0;
                        currentRetryCount = 0;
                    }
                } else {
                    // Non-critical or max retries reached
                    messagesDropped++;
                    if (msg.isCritical) {
                        LOG_WARNING("Critical message dropped (disconnected, max retries)");
                    } else {
                        LOG_DEBUG("Non-critical message dropped (MQTT disconnected)");
                    }
                    lastMessageTimestamp = 0;
                    currentRetryCount = 0;
                }
                
                // Wait longer when disconnected to reduce queue pressure
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            
            // Re-check queue depth after processing (message was dequeued, so depth decreased)
            UBaseType_t remainingQueueDepth = 0;
            if (xMqttPublishQueue != NULL) {
                remainingQueueDepth = uxQueueMessagesWaiting(xMqttPublishQueue);
            }
            
            // CRITICAL FIX: Always yield significantly to give loop() a chance to acquire mutex
            // The old logic tried to process messages too fast, monopolizing the mutex
            // Now we prioritize letting loop() run to process incoming messages
            if (remainingQueueDepth > 5) {
                vTaskDelay(pdMS_TO_TICKS(10));  // Still delay to let loop() run
            } else if (remainingQueueDepth > 0) {
                vTaskDelay(pdMS_TO_TICKS(50));  // Longer delay to prioritize loop()
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));  // Even longer when queue is empty
            }
        } else {
            // No message received - re-check queue depth in case it changed
            UBaseType_t currentQueueDepth = 0;
            if (xMqttPublishQueue != NULL) {
                currentQueueDepth = uxQueueMessagesWaiting(xMqttPublishQueue);
            }
            
            // If queue is building up, don't wait - check again immediately
            if (currentQueueDepth > 0) {
                // Queue has messages - process them quickly
                vTaskDelay(pdMS_TO_TICKS(1));
            } else {
                // No messages - normal periodic delay
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        
        // Periodic statistics logging (every 60 seconds)
        unsigned long now = millis();
        if (now - lastStatsLog > 60000) {
            lastStatsLog = now;
            UBaseType_t currentQueueDepth = (xMqttPublishQueue != NULL) ? 
                uxQueueMessagesWaiting(xMqttPublishQueue) : 0;
            LOG_INFO("MQTT Publisher stats: Published=%lu, Dropped=%lu, Queue=%d/%d", 
                    messagesPublished, messagesDropped, 
                    currentQueueDepth, MQTT_QUEUE_SIZE);
        }
    }
}
*/ // END DISABLED - BLE ONLY MODE

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
        
        // Check MQTT publisher task
        if (TaskMqttPublisherHandle != NULL) {
            eTaskState mqttTaskState = eTaskGetState(TaskMqttPublisherHandle);
            if (mqttTaskState == eDeleted || mqttTaskState == eInvalid) {
                LOG_ERROR("MQTT Publisher task died! State: %d", mqttTaskState);
            } else {
                UBaseType_t stackHighWater = uxTaskGetStackHighWaterMark(TaskMqttPublisherHandle);
                if (stackHighWater < 512) {
                    LOG_WARNING("MQTT Publisher task stack low: %d bytes remaining", stackHighWater);
                }
            }
        }
        
        // Monitor MQTT queue depth
        if (xMqttPublishQueue != NULL) {
            UBaseType_t queueDepth = uxQueueMessagesWaiting(xMqttPublishQueue);
            if (queueDepth > (MQTT_QUEUE_SIZE * 0.8)) {  // More than 80% full
                LOG_WARNING("MQTT queue nearly full: %d/%d messages", queueDepth, MQTT_QUEUE_SIZE);
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

/* DISABLED - BLE ONLY MODE
void mqtt_callback(char *topic, byte *payload, unsigned int len) {
    // MQTT message received - handled by controller
    
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
            // Add command to get network diagnostics
            else if (command == "debug_network") {
                LOG_INFO("Printing network diagnostics");
                extern MqttLteClient mqttClient;
                mqttClient.printNetworkDiagnostics();
            }
            // Add command to get BLE configuration status
            else if (command == "debug_ble") {
                LOG_INFO("=== Configuration Status ===");
                extern BLEConfigManager* bleConfigManager;
                
                // Read from persistent storage
                Preferences debugPrefs;
                debugPrefs.begin(PREFS_NAMESPACE, true);
                String storedMachineNum = debugPrefs.getString(PREFS_MACHINE_NUM, "99");
                String storedEnv = debugPrefs.getString(PREFS_ENVIRONMENT, "prod");
                debugPrefs.end();
                
                LOG_INFO("Stored Machine Number: %s", storedMachineNum.c_str());
                LOG_INFO("Stored Environment: %s", storedEnv.c_str());
                LOG_INFO("Current MACHINE_ID: %s", MACHINE_ID.c_str());
                LOG_INFO("Current AWS_CLIENT_ID: %s", AWS_CLIENT_ID.c_str());
                LOG_INFO("BLE Status: %s", bleConfigManager && bleConfigManager->isInitialized() ? "Active" : "Deinitialized (saves memory)");
                LOG_INFO("Free Heap: %d bytes", ESP.getFreeHeap());
                LOG_INFO("============================");
            }
            // Add command to remotely update machine number (for authorized users)
            else if (command == "set_machine_number" && doc.containsKey("number")) {
                String newNumber = doc["number"].as<String>();
                LOG_INFO("Remote machine number change requested: %s", newNumber.c_str());
                
                // Update directly in Preferences (BLE might be deinitialized)
                Preferences updatePrefs;
                updatePrefs.begin(PREFS_NAMESPACE, false);
                
                // Get current environment
                String currentEnv = updatePrefs.getString(PREFS_ENVIRONMENT, "prod");
                
                // Update machine number
                size_t written = updatePrefs.putString(PREFS_MACHINE_NUM, newNumber);
                updatePrefs.end();
                
                if (written > 0) {
                    LOG_INFO("Machine number updated successfully in storage: %s", newNumber.c_str());
                    LOG_INFO("*** RESTART REQUIRED FOR CHANGES TO TAKE EFFECT ***");
                    
                    // Update runtime variables (will be lost on restart, but good for immediate use)
                    updateMQTTTopics(newNumber, currentEnv);
                    AWS_CLIENT_ID = String("fullwash-machine-") + newNumber;
                    LOG_INFO("AWS Client ID updated to: %s", AWS_CLIENT_ID.c_str());
                    LOG_INFO("NOTE: Restart device to fully apply changes");
                } else {
                    LOG_ERROR("Failed to update machine number in storage");
                }
            }
            // Add command to remotely update environment (for authorized users)
            else if (command == "set_environment" && doc.containsKey("environment")) {
                String newEnv = doc["environment"].as<String>();
                LOG_INFO("Remote environment change requested: %s", newEnv.c_str());
                
                // Update directly in Preferences (BLE might be deinitialized)
                Preferences updatePrefs;
                updatePrefs.begin(PREFS_NAMESPACE, false);
                
                // Get current machine number
                String currentMachineNum = updatePrefs.getString(PREFS_MACHINE_NUM, "99");
                
                // Update environment
                size_t written = updatePrefs.putString(PREFS_ENVIRONMENT, newEnv);
                updatePrefs.end();
                
                if (written > 0) {
                    LOG_INFO("Environment updated successfully in storage: %s", newEnv.c_str());
                    LOG_INFO("*** RESTART REQUIRED FOR CHANGES TO TAKE EFFECT ***");
                    
                    // Update runtime variables (will be lost on restart, but good for immediate use)
                    updateMQTTTopics(currentMachineNum, newEnv);
                    LOG_INFO("NOTE: Restart device to fully apply changes");
                } else {
                    LOG_ERROR("Failed to update environment in storage");
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
        controller->handleMqttMessage(topic, payload, len);
    }
}
*/ // END DISABLED - BLE ONLY MODE

void setup() {
  // Initialize Logger with default log level
  Logger::init(DEFAULT_LOG_LEVEL, 115200);
  delay(1000); // Give time for serial to initialize
  
  LOG_INFO("Starting fullwash-pcb-firmware...");
  
  // Check if machine is already configured by loading from preferences
  LOG_INFO("=== Checking Machine Configuration ===");
  bleConfigManager = new BLEConfigManager();
  
  // Load configuration without initializing BLE yet
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true); // Read-only mode
  String machineNum = prefs.getString(PREFS_MACHINE_NUM, "99");
  String environment = prefs.getString(PREFS_ENVIRONMENT, "prod");
  prefs.end();
  
//   bool needsConfiguration = (machineNum == "99"); // Default value means not configured
  
//   if (needsConfiguration) {
//     LOG_INFO("Machine NOT configured (machine number: %s)", machineNum.c_str());
//     LOG_INFO("=== INITIAL SETUP MODE ===");
//     LOG_INFO("Starting BLE for initial configuration...");
    
//     // Initialize BLE and wait for configuration
//     if (bleConfigManager->begin()) {
//       LOG_INFO("BLE is now advertising. Please connect to configure the machine.");
//       LOG_INFO("Device name: %s", BLE_DEVICE_NAME);
//       LOG_INFO("Use password: %s (default - should be changed)", DEFAULT_MASTER_PASSWORD);
//       LOG_INFO("Waiting for configuration... MQTT will connect after setup.");
      
//       // Wait for machine number to be changed from default
//       LOG_INFO("Setup will continue once machine number is set via BLE.");
//       unsigned long bleStartTime = millis();
//       // const unsigned long BLE_SETUP_TIMEOUT = 150000; // 2.5 minutes timeout
//       const unsigned long BLE_SETUP_TIMEOUT = 10000; // 10 seconds timeout
      
//       while (bleConfigManager->getMachineNumber() == "99") {
//         bleConfigManager->update();
//         delay(1000);
        
//         // Timeout check
//         if (millis() - bleStartTime > BLE_SETUP_TIMEOUT) {
//           LOG_WARNING("BLE setup timeout after 5 minutes. Using default configuration.");
//           break;
//         }
//       }
      
//       // Configuration complete
//       machineNum = bleConfigManager->getMachineNumber();
//       environment = bleConfigManager->getEnvironment();
//       LOG_INFO("Configuration received! Machine: %s, Environment: %s", 
//                machineNum.c_str(), environment.c_str());
//     } else {
//       LOG_ERROR("Failed to initialize BLE - using defaults");
//     }
//   } else {
//     LOG_INFO("Machine already configured: %s (environment: %s)", 
//              machineNum.c_str(), environment.c_str());
//     LOG_INFO("Skipping BLE initialization to save memory for MQTT.");
//   }
  
  // Update MQTT topics with the configuration
  updateMQTTTopics(machineNum, environment);
  AWS_CLIENT_ID = String("fullwash-machine-") + machineNum;
  LOG_INFO("AWS Client ID set to: %s", AWS_CLIENT_ID.c_str());
  LOG_INFO("====================================");
  
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
    
    // Enable interrupt for coin acceptor pins
    LOG_INFO("Enabling interrupt for coin acceptor (COIN_SIG on bit %d)...", COIN_SIG);
    ioExpander.enableInterrupt(0, 0xc0); // Enable interrupt for upper bits including COIN_SIG
    
    // Configure interrupt pin
    pinMode(INT_PIN, INPUT_PULLUP);
    LOG_INFO("Interrupt pin %d configured with pull-up", INT_PIN);
    
    // Read initial state
    uint8_t initialPortValue = ioExpander.readRegister(INPUT_PORT0);
    LOG_INFO("Initial port value: 0x%02X", initialPortValue);
    LOG_INFO("Initial COIN_SIG state: %d", (initialPortValue & (1 << COIN_SIG)) ? 1 : 0);
    
    LOG_INFO("TCA9535 fully initialized. Ready to control relays and read buttons.");
    
  // Initialize FreeRTOS mutexes for shared resources
  LOG_INFO("Initializing FreeRTOS mutexes...");
  xIoExpanderMutex = xSemaphoreCreateMutex();
  xControllerMutex = xSemaphoreCreateMutex();
  xI2CMutex = xSemaphoreCreateMutex();  // For Wire1 (LCD and RTC)
  
  if (xIoExpanderMutex == NULL || xControllerMutex == NULL || xI2CMutex == NULL) {
        LOG_ERROR("Failed to create mutexes!");
    } else {
        LOG_INFO("Mutexes created successfully");
    }
    
    // Initialize FreeRTOS queue for MQTT message publishing
    LOG_INFO("Initializing MQTT publish queue...");
    xMqttPublishQueue = xQueueCreate(MQTT_QUEUE_SIZE, sizeof(MqttMessage));
    
    if (xMqttPublishQueue == NULL) {
        LOG_ERROR("Failed to create MQTT publish queue!");
    } else {
        LOG_INFO("MQTT publish queue created successfully (size: %d)", MQTT_QUEUE_SIZE);
    }
    
    // Create FreeRTOS tasks for dedicated interrupt handling
    LOG_INFO("Creating FreeRTOS tasks for coin and button detection...");
    
    xTaskCreate(
        TaskCoinDetector,           // Task function
        "CoinDetection",            // Task name
        2048,                       // Stack size
        NULL,                       // Task parameters
        1,                          // Priority
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
  // Initialize Wire1 for the LCD display and RTC (shared I2C bus)
  LOG_INFO("Initializing Wire1 (I2C) for LCD and RTC...");
  Wire1.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Wire1.setClock(100000); // Set I2C clock to 100kHz (standard mode)
  
  // Initialize the RTC manager
  LOG_INFO("Initializing RTC Manager...");
  rtcManager = new RTCManager(RTC_DS1340_ADDR, &Wire1);
  
  // Set I2C mutex for RTC manager (must be done after mutex creation)
  if (rtcManager && xI2CMutex != NULL) {
    rtcManager->setI2CMutex(xI2CMutex);
  }
  
  if (rtcManager->begin()) {
    LOG_INFO("RTC initialization successful!");
    rtcManager->printDebugInfo();
    // Connect RTC to logger for timestamps
    Logger::setRTCManager(rtcManager);
  } else {
    LOG_ERROR("Failed to initialize RTC!");
    LOG_WARNING("System will continue without RTC. Timestamps may be inaccurate.");
  }
  
  // Initialize the controller with RTC
  controller = new CarWashController(mqttClient);
  controller->setRTCManager(rtcManager);
  
  // Initialize the display with correct LCD pins
  display = new DisplayManager(LCD_ADDR, LCD_COLS, LCD_ROWS, LCD_SDA_PIN, LCD_SCL_PIN);
  // Set I2C mutex for display manager (shared with RTC)
  display->setI2CMutex(xI2CMutex);
  
  // MQTT initialization commented out - using BLE only
  /* DISABLED - BLE ONLY MODE
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
    if (mqttClient.connect(AWS_BROKER, AWS_BROKER_PORT, AWS_CLIENT_ID.c_str())) {
      LOG_INFO("Connected to MQTT broker!");
      
      mqttClient.subscribe(INIT_TOPIC.c_str());
      mqttClient.subscribe(CONFIG_TOPIC.c_str());
      mqttClient.subscribe(COMMAND_TOPIC.c_str());
      mqttClient.subscribe(GET_STATE_TOPIC.c_str());
      
      delay(4000);
      LOG_INFO("Publishing Setup Action Event...");
      controller->publishMachineSetupActionEvent();
    } else {
      LOG_ERROR("Failed to connect to MQTT broker");
    }
  } else {
    LOG_ERROR("Failed to initialize modem");
  }
  */ // END DISABLED - BLE ONLY MODE
  
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
  
  // Network Manager task commented out - using BLE only
  /* DISABLED - BLE ONLY MODE
  // Create Network Manager task (handles all network/MQTT operations)
  LOG_INFO("Creating Network Manager task...");
  xTaskCreatePinnedToCore(
      TaskNetworkManager,           // Task function
      "NetworkManager",             // Task name
      16384,                        // Stack size (bytes) - SSL/TLS requires large stack (16KB)
      NULL,                         // Task parameters
      2,                            // Priority (2 = medium priority)
      &TaskNetworkManagerHandle,    // Task handle
      1                             // Pin to core 1 (APP CPU)
  );
  */ // END DISABLED - BLE ONLY MODE
  
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
  
  // MQTT Publisher task commented out - using BLE only
  /* DISABLED - BLE ONLY MODE
  // Create MQTT Publisher task (handles all MQTT publishing)
  LOG_INFO("Creating MQTT Publisher task...");
  xTaskCreatePinnedToCore(
      TaskMqttPublisher,            // Task function
      "MqttPublisher",              // Task name
      8192,                         // Stack size (bytes) - needs space for MQTT operations
      NULL,                         // Task parameters
      2,                            // Priority (2 = SAME as NetworkManager, not higher - prevents monopolizing mutex)
      &TaskMqttPublisherHandle,     // Task handle
      1                             // Pin to core 1 (APP CPU) - same as network operations
  );
  */ // END DISABLED - BLE ONLY MODE
  
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

  // NOTE: Interrupt handling is now done by FreeRTOS tasks (TaskCoinDetector and TaskButtonDetector)
  // The old ioExpander.handleInterrupt() call is no longer needed
  
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
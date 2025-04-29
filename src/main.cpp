#include <Arduino.h>
#include <Wire.h>
#include "mqtt_lte_client.h"
#include "io_expander.h"
#include "utilities.h"
#include "constants.h"
#include "car_wash_controller.h"
#include "logger.h"
#include "display_manager.h"

#include "certs/AmazonRootCA.h"
#include "certs/AWSClientCertificate.h"
#include "certs/AWSClientPrivateKey.h"

// Wire1 is already defined in the ESP32 Arduino framework

// Server details
const char* AWS_BROKER = "a3foc0mc6v7ap0-ats.iot.us-east-1.amazonaws.com";
const char* AWS_CLIENT_ID = "fullwash-machine-001";
const uint16_t AWS_BROKER_PORT = 8883;

// GSM connection settings
const char apn[] = "internet"; // Replace with your carrier's APN if needed
const char gprsUser[] = "";
const char gprsPass[] = "";
const char pin[] = "3846";

// Create MQTT LTE client
MqttLteClient mqttClient(SerialAT, MODEM_PWRKEY, MODEM_DTR, MODEM_FLIGHT, MODEM_TX, MODEM_RX);

// Create IO Expander
IoExpander ioExpander(TCA9535_ADDR, I2C_SDA_PIN, I2C_SCL_PIN, INT_PIN);

// Create controller
CarWashController* controller;

// Create display manager
DisplayManager* display;

// Button states
bool buttonState = false;
bool lastButtonState = false;

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
                    controller->setLogLevel(LOG_ERROR);
                } else if (level == "NONE") {
                    controller->setLogLevel(LOG_NONE);
                }
            }
            // Add debug command to print IO expander state
            else if (command == "debug_io") {
                LOG_INFO("Printing IO expander debug info");
                extern IoExpander ioExpander;
                ioExpander.printDebugInfo();
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
    
    // Set up coin detection callback
    ioExpander.setCoinDetectionCallback([]() {
        LOG_INFO("Coin detected!");
    });
    
    LOG_INFO("TCA9535 fully initialized. Ready to control relays and read buttons.");
  }
  controller = new CarWashController(mqttClient);
  
  // Initialize Wire1 for the LCD display
  Wire1.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  
  // Initialize the display with correct LCD pins
  display = new DisplayManager(LCD_ADDR, LCD_COLS, LCD_ROWS, LCD_SDA_PIN, LCD_SCL_PIN);
  
  // Initialize MQTT client with callback
  mqttClient.setCallback(mqtt_callback);
  
  // Set up SSL certificates
  mqttClient.setCACert(AmazonRootCA);
  mqttClient.setCertificate(AWSClientCertificate);
  mqttClient.setPrivateKey(AWSClientPrivateKey);
  
  // Connect to cellular network
  LOG_INFO("Connecting to cellular network...");
  if (mqttClient.begin(apn, gprsUser, gprsPass, pin)) {
    LOG_INFO("Connected to cellular network!");
    
    // Connect to MQTT broker
    LOG_INFO("Connecting to MQTT broker...");
    if (mqttClient.connect(AWS_BROKER, AWS_BROKER_PORT, AWS_CLIENT_ID)) {
      LOG_INFO("Connected to MQTT broker!");
      
      // Subscribe to topics
      mqttClient.subscribe(INIT_TOPIC.c_str());
      mqttClient.subscribe(CONFIG_TOPIC.c_str());
      mqttClient.subscribe(COMMAND_TOPIC.c_str());
      
      // Publish initial state
      controller->publishMachineSetupActionEvent();
    } else {
      LOG_ERROR("Failed to connect to MQTT broker!");
    }
  } else {
    LOG_ERROR("Failed to connect to cellular network!");
  }
}

void loop() {
  // Timing variables for various operations
  static unsigned long lastStatusCheck = 0;
  static unsigned long lastNetworkCheck = 0;
  static unsigned long lastConnectionAttempt = 0;
  static unsigned long lastButtonCheck = 0;
  static unsigned long lastMqttReconnectAttempt = 0;
  
  // Button state tracking
  static bool buttonState = false;
  static bool lastButtonState = false;
  
  // Current time
  unsigned long currentTime = millis();
  
  // Check network status every 30 seconds
  if (currentTime - lastNetworkCheck > 30000) {
    lastNetworkCheck = currentTime;
    
    if (!mqttClient.isNetworkConnected()) {
      LOG_WARNING("Lost cellular network connection");
      
      // Only attempt reconnection every 60 seconds to avoid overwhelming the modem
      if (currentTime - lastConnectionAttempt > 60000) {
        lastConnectionAttempt = currentTime;
        
        LOG_INFO("Attempting to reconnect to cellular network...");
        if (mqttClient.begin(apn, gprsUser, gprsPass, pin)) {
          LOG_INFO("Successfully reconnected to cellular network!");
          
          // Reconfigure SSL certificates
          mqttClient.setCACert(AmazonRootCA);
          mqttClient.setCertificate(AWSClientCertificate);
          mqttClient.setPrivateKey(AWSClientPrivateKey);
          
          // Connect to MQTT broker
          if (mqttClient.connect(AWS_BROKER, AWS_BROKER_PORT, AWS_CLIENT_ID)) {
            LOG_INFO("MQTT broker connection restored!");
            mqttClient.subscribe(INIT_TOPIC.c_str());
            mqttClient.subscribe(CONFIG_TOPIC.c_str());
            mqttClient.subscribe(COMMAND_TOPIC.c_str());
            
            // Notify that we're back online
            if (controller) {
                delay(4000);
                controller->publishMachineSetupActionEvent();
            }
          }
        }
      }
    }
  }
  
  // Handle MQTT client loop
  mqttClient.loop();
  
  // Handle buttons and update controller
  if (controller) {
    controller->handleButtons();
    controller->update();
  }
  
  // Check for coin detection
  ioExpander.isCoinDetected();
  
  // Update display with current controller state
  if (display && controller) {
    display->update(controller);
  }
}
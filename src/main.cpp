#include <Arduino.h>
#include "mqtt_lte_client.h"
#include "io_expander.h"
#include "utilities.h"
#include "constants.h"
#include "car_wash_controller.h"

#include "certs/AmazonRootCA.h"
#include "certs/AWSClientCertificate.h"
#include "certs/AWSClientPrivateKey.h"

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

CarWashController* controller;

// Button states
bool buttonState = false;
bool lastButtonState = false;


void mqtt_callback(char *topic, byte *payload, unsigned int len) {
  Serial.print("Message arrived [");
    if (controller) {
        controller->handleMqttMessage(topic, payload, len);
    }
}

void setup() {
  Serial.begin(115200);
  delay(1000); // Give time for serial to initialize
  
  // Set up the built-in LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn ON LED to show power
  
  Serial.println("\n\n");
  Serial.println("======================================");
  Serial.println("ESP32 TCA9535 I/O Expander Debug Mode");
  Serial.println("======================================");
  
  // // Blink LED to show we've reached this point
  // for (int i = 0; i < 3; i++) {
  //   digitalWrite(LED_PIN, LOW);
  //   delay(100);
  //   digitalWrite(LED_PIN, HIGH);
  //   delay(100);
  // }
  
  // Initialize the I/O expander
  Serial.println("Trying to initialize TCA9535...");
  bool initSuccess = ioExpander.begin();
  
  if (!initSuccess) {
    Serial.println("Failed to initialize TCA9535!");
    Serial.println("Will continue without initialization. Check connections.");
    
    // Blink LED rapidly to indicate error
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(100);
    }
    
    // Continue anyway - don't get stuck in a loop
  } else {
    Serial.println("TCA9535 initialization successful!");
    
    // Configure Port 0 (buttons) as inputs (1 = input, 0 = output)
    Serial.println("Configuring Port 0 as inputs...");
    ioExpander.configurePortAsInput(0, 0xFF);
    
    // Configure Port 1 (relays) as outputs (1 = input, 0 = output)
    Serial.println("Configuring Port 1 as outputs...");
    ioExpander.configurePortAsOutput(1, 0xFF);
    
    // Initialize all relays to OFF state
    Serial.println("Setting all relays to OFF...");
    ioExpander.writeRegister(OUTPUT_PORT1, 0x00);
    
    Serial.println("TCA9535 fully initialized. Ready to control relays and read buttons.");
  }
  controller = new CarWashController(mqttClient);
  // Initialize MQTT client with callback
  mqttClient.setCallback(mqtt_callback);
  mqttClient.setBufferSize(512);

  // Initialize modem and connect to network
  Serial.println("Initializing modem and connecting to network...");
  if (mqttClient.begin(apn, gprsUser, gprsPass, pin)) {
    // Configure SSL certificates
    mqttClient.setCACert(AmazonRootCA);
    mqttClient.setCertificate(AWSClientCertificate);
    mqttClient.setPrivateKey(AWSClientPrivateKey);
    
    // Connect to MQTT broker
    Serial.println("Connecting to MQTT broker...");
    if (mqttClient.connect(AWS_BROKER, AWS_BROKER_PORT, AWS_CLIENT_ID)) {
      Serial.println("Connected to MQTT broker!");
      
      // // Subscribe to test topic
      // mqttClient.subscribe("machines/machine001/test");
      
      // // Publish hello message
      // mqttClient.publish("machines/machine001/data", "hello world", QOS1_AT_LEAST_ONCE);
      // Serial.print("Subscribing to topic:");
      // Serial.println(INIT_TOPIC.c_str());
      bool result = mqttClient.subscribe(INIT_TOPIC.c_str());
      Serial.print("Subscription result:");
      Serial.println(result);
      result = mqttClient.subscribe(CONFIG_TOPIC.c_str());
      Serial.print("Subscription result:");
      Serial.println(result);
      controller->publishMachineSetupActionEvent();
    } else {
      Serial.println("Failed to connect to MQTT broker");
    }
  } else {
    Serial.println("Failed to initialize modem");
  }
  
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
      Serial.println("Lost cellular network connection");
      
      // Only attempt reconnection every 60 seconds to avoid overwhelming the modem
      if (currentTime - lastConnectionAttempt > 60000) {
        lastConnectionAttempt = currentTime;
        
        Serial.println("Attempting to reconnect to cellular network...");
        if (mqttClient.begin(apn, gprsUser, gprsPass, pin)) {
          Serial.println("Successfully reconnected to cellular network!");
          
          // Reconfigure SSL certificates
          mqttClient.setCACert(AmazonRootCA);
          mqttClient.setCertificate(AWSClientCertificate);
          mqttClient.setPrivateKey(AWSClientPrivateKey);
          
          // Connect to MQTT broker
          if (mqttClient.connect(AWS_BROKER, AWS_BROKER_PORT, AWS_CLIENT_ID)) {
            Serial.println("MQTT broker connection restored!");
            mqttClient.subscribe(INIT_TOPIC.c_str());
            mqttClient.subscribe(CONFIG_TOPIC.c_str());
            
            // Notify that we're back online
            if (controller) {
              // controller->publishMachineStatusEvent("ONLINE");
            }
          }
        }
      }
    } else {
      // Network is connected, but check MQTT connection
      if (!mqttClient.isConnected()) {
        // Only attempt MQTT reconnection every 15 seconds
        if (currentTime - lastMqttReconnectAttempt > 15000) {
          lastMqttReconnectAttempt = currentTime;
          Serial.println("Network connected but MQTT disconnected, attempting to reconnect...");
          
          // MQTT client will handle resubscribing to topics internally
          mqttClient.reconnect();
        }
      }
    }
  }
  
  // If connected to network, process MQTT messages
  if (mqttClient.isNetworkConnected()) {
    // Always process MQTT messages when connected (this handles incoming messages)
    mqttClient.loop();
    
    // Run controller update if available
    if (controller) {
      // controller->update();
    }
    
    // Status message every 60 seconds if connected
    if (currentTime - lastStatusCheck > 60000) {
      lastStatusCheck = currentTime;
      Serial.println("System running normally, network connected");
      
      // Optional: publish a heartbeat message
      if (mqttClient.isConnected() && controller) {
        // controller->publishHeartbeat();
      }
    }
  }
  
  // Read and handle button presses (check every 50ms for debouncing)
  if (currentTime - lastButtonCheck > 50) {
    lastButtonCheck = currentTime;
    
    // Read button with error handling
    bool buttonReadSuccess = true;
    buttonState = ioExpander.readButton(BUTTON1);
    
    // Handle I/O expander errors if your IoExpander class supports error reporting
    if (!buttonReadSuccess) {
      Serial.println("Error reading button state, skipping button handling");
    } else {
      // Check if button state changed from not pressed to pressed
      if (buttonState && !lastButtonState) {
        Serial.println("Button pressed! Toggling Relay 1 (clear water)");
        
        // Toggle Relay 1 and get the new state
        bool newState = ioExpander.toggleRelay(RELAY1);
        Serial.println(newState ? "Relay ON" : "Relay OFF");
        
        // Publish relay state change to MQTT if connected
        if (mqttClient.isConnected()) {
          mqttClient.publish("machines/machine001/relay", newState ? "ON" : "OFF", QOS1_AT_LEAST_ONCE);
        }
      }
      
      // Save last button state
      lastButtonState = buttonState;
    }
  }
  
  // Handle LED indicator
  // Blink pattern based on connection status
  static unsigned long lastLedToggle = 0;
  static bool ledState = HIGH;
  
  if (mqttClient.isConnected()) {
    // Solid LED when fully connected
    digitalWrite(LED_PIN, HIGH);
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
}
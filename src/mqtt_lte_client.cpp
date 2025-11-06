#include "mqtt_lte_client.h"
#include "constants.h"
#include <freertos/semphr.h>

MqttLteClient::MqttLteClient(HardwareSerial& modemSerial, int pwrKeyPin, int dtrPin, int flightPin, 
                           int txPin, int rxPin)
    : _modemSerial(modemSerial), _pwrKeyPin(pwrKeyPin), _dtrPin(dtrPin), _flightPin(flightPin),
      _txPin(txPin), _rxPin(rxPin), _initialized(false), _networkConnected(false), _mqttConnected(false) {
    _mutex = xSemaphoreCreateRecursiveMutex();
    
    _modem = new TinyGsm(_modemSerial);
    _gsmClient = new TinyGsmClient(*_modem);
    _sslClient = new SSLClient(_gsmClient);
    
    // Configure SSL client with short timeouts to prevent watchdog issues
    // ESP32 task watchdog is ~5 seconds, so keep SSL operations well under that
    _sslClient->setTimeout(4000);  // 4 second timeout for SSL operations (safe for watchdog)
    
    _mqttClient = new PubSubClient(*_sslClient);
    _mqttClient->setSocketTimeout(4);  // 4 second timeout for socket operations

    _subscribedTopics.reserve(5);
}

bool MqttLteClient::begin(const char* apn, const char* user, const char* pass, const char* pin) {
    _apn = apn;
    _user = user;
    _pass = pass;
    _pin = pin;
    
    // Initialize modem serial
    _modemSerial.begin(115200, SERIAL_8N1, _txPin, _rxPin);
    delay(1000);
    
    // Power on the modem with improved sequence
    powerOnModem();
    
    // Try to initialize and connect the modem
    bool modemInitialized = initModemAndConnectNetwork();
    
    if (!modemInitialized) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("Trying alternative baud rate (9600)...");
        }
        Serial.flush();
        _modemSerial.updateBaudRate(9600);
        delay(1000);
        
        // Test with the new baud rate
        testModemAT();
        
        modemInitialized = initModemAndConnectNetwork();
    }
    
    _initialized = modemInitialized;
    return _initialized;
}

void MqttLteClient::powerOnModem() {
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println("Powering on SIM7600G module...");
    }
    
    // Configure control pins
    pinMode(_pwrKeyPin, OUTPUT);
    pinMode(_dtrPin, OUTPUT);
    pinMode(_flightPin, OUTPUT);
    
    // CRITICAL FIX: Ensure proper DTR and Flight pin states
    // DTR LOW = active (modem stays awake)
    // Flight HIGH = flight mode disabled (radio active)
    digitalWrite(_dtrPin, LOW);     // Keep modem awake
    digitalWrite(_flightPin, HIGH); // Disable flight mode
    delay(100); // Let pins stabilize
    
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println("[MODEM] Control pins set: DTR=LOW (active), FLIGHT=HIGH (disabled)");
    }
    
    // SIM7600G power on sequence (based on datasheet)
    digitalWrite(_pwrKeyPin, LOW);  // Ensure PWRKEY starts LOW
    delay(1000);
    
    digitalWrite(_pwrKeyPin, HIGH); // Pull PWRKEY HIGH
    delay(2000);                    // Hold for >1 second
    
    digitalWrite(_pwrKeyPin, LOW);  // Release PWRKEY
    
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println("Waiting for modem to initialize...");
    }
    delay(10000);  // Wait longer for the modem to boot
    
    clearModemBuffer();
    
    // Test AT command communication
    bool atSuccess = testModemAT();
    
    if (!atSuccess) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("Trying alternative power on sequence...");
        }
        
        // Alternative power on sequence sometimes needed for SIM7600
        digitalWrite(_pwrKeyPin, HIGH);
        delay(3000);
        digitalWrite(_pwrKeyPin, LOW);
        delay(5000);
        
        clearModemBuffer();
        
        // Test AT command again
        atSuccess = testModemAT();
        
        if (!atSuccess) {
            // Always show critical errors
            Serial.println("Still unable to communicate with modem!");
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("Possible issues:");
                Serial.println("1. Check power supply to modem");
                Serial.println("2. Check UART connections (TX/RX)");
                Serial.println("3. Modem might not be powered properly");
            }
        }
    }
}

void MqttLteClient::clearModemBuffer() {
    delay(100);
    while (_modemSerial.available()) {
        _modemSerial.read();
    }
}

bool MqttLteClient::testModemAT() {
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println("Testing direct AT communication with modem...");
    }
    
    clearModemBuffer();
    
    // Flush any pending data
    while (_modemSerial.available()) {
        _modemSerial.read();
    }
    
    // Send AT command
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println("Sending: AT");
    }
    _modemSerial.println("AT");
    
    // Wait for response
    unsigned long start = millis();
    String response = "";
    
    while (millis() - start < 3000) {
        if (_modemSerial.available()) {
            char c = _modemSerial.read();
            response += c;
        }
    }
    
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println("Response: " + response);
    }
    
    if (response.indexOf("OK") != -1) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("Modem responded to AT command successfully!");
        }
        return true;
    } else {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("Modem failed to respond to AT command properly.");
        }
        return false;
    }
}

bool MqttLteClient::initModemAndConnectNetwork() {
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println("Initializing modem...");
    }
    
    // CRITICAL FIX: Clear any pending data before init
    clearModemBuffer();
    delay(500);
    
    // Try to initialize the modem
    if (!_modem->init()) {
        // Always show critical errors
        Serial.println("Failed to initialize modem!");
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("[MODEM] TinyGSM init failed - modem may be in bad state");
        }
        
        // Check if we can talk to the modem directly
        bool basicComm = testModemAT();
        
        if (basicComm) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("Modem responds to AT commands but TinyGSM init failed.");
                Serial.println("This could be a TinyGSM library compatibility issue with SIM7600G.");
                
                // Try an alternative approach - direct AT commands
                Serial.println("Trying simplified initialization with direct AT commands...");
            }
            
            // Send some basic config AT commands
            _modemSerial.println("AT+CFUN=1");  // Set full functionality
            delay(1000);
            
            _modemSerial.println("AT+CREG?");   // Check network registration
            delay(1000);
            
            // Try to connect to GPRS directly
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.print("Connecting to ");
                Serial.print(_apn);
                Serial.println(" using direct AT commands...");
            }
            
            // Set the APN
            _modemSerial.print("AT+CGDCONT=1,\"IP\",\"");
            _modemSerial.print(_apn);
            _modemSerial.println("\"");
            delay(1000);
            
            return false;  // Still return false to indicate TinyGSM init failed
        } else {
            // Always show critical errors
            Serial.println("Basic AT command communication failed.");
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("Possible hardware issue - check wiring and power.");
            }
            return false;
        }
    }
    
    // Get modem info
    String modemInfo = _modem->getModemInfo();
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.print("Modem Info: ");
        Serial.println(modemInfo);
    }
    
    // Set network mode to automatic (2G/3G/4G)
    String networkMode;
    networkMode = _modem->setNetworkMode(2);
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.print("Network mode set: ");
        Serial.println(networkMode);
    }
    
    if (_pin && _modem->getSimStatus() != 3) {
        _modem->simUnlock(_pin);
    }
    
    // Wait for network connection
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.print("Waiting for network...");
    }
    if (!_modem->waitForNetwork(60000L)) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println(" fail");
        }
        return false;
    }
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println(" success");
    }
    
    if (_modem->isNetworkConnected()) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("Network connected");
        }
    } else {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("Network connection failed");
        }
        return false;
    }
    
    // Connect to GPRS
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.print("Connecting to ");
        Serial.print(_apn);
    }
    if (!_modem->gprsConnect(_apn, _user, _pass)) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println(" fail");
        }
        return false;
    }
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println(" success");
    }
    
    if (_modem->isGprsConnected()) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("GPRS connected");
        }
        
        // CRITICAL FIX: Configure modem to prevent GPRS connection timeout
        // Some modems timeout GPRS connections after 30-60 seconds of inactivity
        // Configure keep-alive and disable auto-disconnect
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("[MODEM] Configuring GPRS keep-alive settings...");
        }
        clearModemBuffer();
        
        // Set TCP keep-alive (if supported) - sends keep-alive packets every 60 seconds
        _modemSerial.println("AT+CIPKEEPALIVE=1,60");
        delay(500);
        clearModemBuffer();
        
        // Disable auto-disconnect on inactivity (if supported by modem)
        // AT+CIPCLOSE=0 means don't auto-close connections
        _modemSerial.println("AT+CIPCLOSE=0");
        delay(500);
        clearModemBuffer();
        
        // Get and validate IP address
        String ip = _modem->localIP().toString();
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.print("IP address: ");
            Serial.println(ip);
        }
        
        // Validate IP address before declaring success
        if (!isValidIP(ip)) {
            // Always show critical errors
            Serial.print("Invalid IP address received: ");
            Serial.println(ip);
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("GPRS connection appears unstable, will retry");
            }
            return false;
        }
        
        // Get and log signal quality
        int signalQuality = _modem->getSignalQuality();
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.print("Signal quality: ");
            Serial.print(signalQuality);
            Serial.println("/31");
            
            if (signalQuality < 10) {
                Serial.println("WARNING: Poor signal quality detected, connection may be unstable");
            }
        }
        
        _networkConnected = true;
        return true;
    } else {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("GPRS connection failed");
        }
        return false;
    }
}

void MqttLteClient::setCACert(const char* caCert) {
    // These operations are quick, but use timeout for consistency
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(1000); // 1 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("[MQTT] Failed to acquire mutex for setCACert");
            }
            return;
        }
    }
    _sslClient->setCACert(caCert);
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

void MqttLteClient::setCertificate(const char* clientCert) {
    // These operations are quick, but use timeout for consistency
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(1000); // 1 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("[MQTT] Failed to acquire mutex for setCertificate");
            }
            return;
        }
    }
    _sslClient->setCertificate(clientCert);
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

void MqttLteClient::setPrivateKey(const char* privateKey) {
    // These operations are quick, but use timeout for consistency
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(1000); // 1 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("[MQTT] Failed to acquire mutex for setPrivateKey");
            }
            return;
        }
    }
    _sslClient->setPrivateKey(privateKey);
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

void MqttLteClient::setCallback(void (*callback)(char*, byte*, unsigned int)) {
    // These operations are quick, but use timeout for consistency
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(1000); // 1 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("[MQTT] Failed to acquire mutex for setCallback");
            }
            return;
        }
    }
    _callback = callback;
    _mqttClient->setCallback(_callback);
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

bool MqttLteClient::connect(const char* broker, uint16_t port, const char* clientId) {
    // CRITICAL FIX: Use timeout instead of portMAX_DELAY to prevent deadlock
    // Connection can take several seconds due to SSL handshake, but we need timeout protection
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(10000); // 10 second timeout for connection
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("[MQTT] Failed to acquire mutex for connect");
            }
            return false;
        }
    }
    
    _broker = broker;
    _port = port;
    _clientId = clientId;
    
    _mqttClient->setServer(_broker, _port);
    
    // Keep-alive reduced to 60s to prevent SSL timeout issues
    // SSL sessions were failing at ~106s with 120s keep-alive
    _mqttClient->setKeepAlive(60);

    // Check signal quality before attempting SSL connection
    int signalQuality = getSignalQuality();
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS && signalQuality > 0 && signalQuality < 10) {
        Serial.print("WARNING: Poor signal quality (");
        Serial.print(signalQuality);
        Serial.println("/31) - SSL connection may fail");
    }

    // Make a single connection attempt to prevent long blocking
    // Retries are handled by the calling code (NetworkManager task)
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.print("Attempting MQTT connection...");
    }
    
    // Attempt to connect (this can block for up to 4 seconds during SSL handshake)
    if (_mqttClient->connect(_clientId)) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("connected");
        }
        _mqttConnected = true;
        _consecutiveFailures = 0; // Reset failure counter on success
        if (_mutex) xSemaphoreGiveRecursive(_mutex);
        return true;
    } else {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.print("failed, rc=");
            Serial.println(_mqttClient->state());
        }
        _consecutiveFailures++;
    }
    
    // If we've had multiple consecutive failures, log warning
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS && _consecutiveFailures > 3) {
        Serial.print("WARNING: ");
        Serial.print(_consecutiveFailures);
        Serial.println(" consecutive SSL/MQTT connection failures");
        Serial.println("This may indicate certificate issues or very poor signal");
    }
    
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return false;
}

void MqttLteClient::cleanupSSLClient() {
    // CRITICAL FIX: Use timeout instead of portMAX_DELAY to prevent deadlock
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(2000); // 2 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("[DEBUG] Failed to acquire mutex for SSL cleanup");
            }
            return;
        }
    }
    
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println("[DEBUG] Cleaning up SSL client to clear corrupted state");
    }
    
    // Disconnect MQTT client first
    if (_mqttClient && _mqttClient->connected()) {
        _mqttClient->disconnect();
    }
    
    // Stop SSL client
    if (_sslClient) {
        _sslClient->stop();
    }
    
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.println("[DEBUG] SSL client cleanup complete");
    }
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

void MqttLteClient::reconnect() {
    // Use timeout to prevent blocking publisher task
    // Reconnection can take several seconds due to SSL handshake
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(100); // Quick check, if busy skip this attempt
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            // Mutex busy (likely publisher is sending) - skip this reconnect attempt
            return;
        }
    }
    
    // Loop until we're reconnected or timeout
    unsigned long now = millis();
    if (now - _lastReconnectAttempt < _reconnectInterval) {
        if (_mutex) xSemaphoreGiveRecursive(_mutex);
        return; // Don't try too frequently
    }
    _lastReconnectAttempt = now;

    // After multiple consecutive failures, cleanup SSL state
    // This helps recover from corrupted SSL sessions
    if (_consecutiveFailures >= 3 && _consecutiveFailures % 3 == 0) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.print("[INFO] ");
            Serial.print(_consecutiveFailures);
            Serial.println(" consecutive failures - performing SSL cleanup");
        }
        // We already hold the mutex, call internal cleanup directly
        if (_mqttClient && _mqttClient->connected()) {
            _mqttClient->disconnect();
        }
        if (_sslClient) {
            _sslClient->stop();
        }
        // Note: No delay needed here, cleanup returns immediately
    }

    // Make ONE attempt per call instead of blocking in a loop
    // This prevents watchdog timeouts by allowing the FreeRTOS task to yield
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
        Serial.print("Attempting MQTT connection...");
    }
    
    // Attempt to connect (this is a non-blocking call with timeout)
    if (_mqttClient->connect(_clientId)) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("connected");
        }
        _mqttConnected = true;
        _consecutiveFailures = 0; // Reset on success
        
        // Re-subscribe to all previously subscribed topics
        for (const String& topic : _subscribedTopics) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.print("Re-subscribing to topic: ");
                Serial.println(topic);
            }
            _mqttClient->subscribe(topic.c_str());
        }
        _reconnectInterval = 5000; // Reset interval on success
    } else {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.print("failed, rc=");
            Serial.println(_mqttClient->state());
        }
        _consecutiveFailures++;
        
        // Exponential backoff - double the interval up to 2 minutes
        _reconnectInterval = min(_reconnectInterval * 2, 120000);
    }
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

bool MqttLteClient::publish(const char* topic, const char* payload, const uint8_t qos) {
    // CRITICAL FIX: Use timeout instead of portMAX_DELAY to prevent deadlock
    // If mutex is held by loop() for too long, this will fail gracefully
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(2000); // 2 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.print("[MQTT] Failed to acquire mutex for publish to ");
                Serial.println(topic);
            }
            return false;
        }
    }
    
    // Don't attempt reconnect in blocking publish - it can take too long
    // Reconnection should be handled by NetworkManager task
    bool ok = false;
    if (_mqttClient->connected()) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.print("[MQTT TX] Topic: ");
            Serial.print(topic);
            Serial.print(" | Payload size: ");
            Serial.print(strlen(payload));
            Serial.print(" bytes | QoS: ");
            Serial.println(qos);
        }
        
        ok = _mqttClient->publish(topic, payload);
        if (!ok) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.print("[MQTT ERROR] Failed to publish to ");
                Serial.print(topic);
                Serial.print(" (QoS: ");
                Serial.print(qos);
                Serial.print(", state: ");
                Serial.print(_mqttClient->state());
                Serial.println(")");
            }
        } else {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("[MQTT TX] ✓ Message sent successfully");
            }
        }
    } else {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.print("[MQTT ERROR] Cannot publish to ");
            Serial.print(topic);
            Serial.print(" - MQTT not connected (state: ");
            Serial.print(_mqttClient->state());
            Serial.println(")");
        }
    }
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return ok;
}

bool MqttLteClient::publishNonBlocking(const char* topic, const char* payload, const uint8_t qos, TickType_t timeoutMs) {
    // Try to acquire mutex with timeout - if we can't get it immediately, skip publish
    // This prevents blocking critical operations like button handling
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(timeoutMs);
        unsigned long startWait = millis();
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            // Mutex not available - skip publish to avoid blocking
            unsigned long waitTime = millis() - startWait;
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.print("[MQTT TX] Non-blocking publish skipped (mutex busy for ");
                Serial.print(waitTime);
                Serial.println("ms)");
            }
            return false;
        }
        unsigned long actualWaitTime = millis() - startWait;
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS && actualWaitTime > 100) {
            Serial.print("[MQTT TX] Mutex acquired after ");
            Serial.print(actualWaitTime);
            Serial.println("ms wait");
        }
    }
    
    // Don't attempt reconnect in non-blocking mode - it's too slow
    // Just try to publish if already connected
    bool ok = false;
    if (_mqttClient->connected()) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.print("[MQTT TX] (non-blocking) Topic: ");
            Serial.print(topic);
            Serial.print(" | Payload size: ");
            Serial.print(strlen(payload));
            Serial.print(" bytes | QoS: ");
            Serial.println(qos);
        }
        
        ok = _mqttClient->publish(topic, payload);
        if (!ok) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.print("[MQTT] Non-blocking publish failed to ");
                Serial.println(topic);
            }
        } else {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("[MQTT TX] ✓ Non-blocking message sent successfully");
            }
        }
    } else {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.println("[MQTT TX] Non-blocking publish skipped (not connected)");
        }
    }
    
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return ok;
}

bool MqttLteClient::subscribe(const char* topic) {
    // CRITICAL FIX: Use timeout instead of portMAX_DELAY to prevent deadlock
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(2000); // 2 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.print("[MQTT] Failed to acquire mutex for subscribe to ");
                Serial.println(topic);
            }
            return false;
        }
    }
    
    // Don't attempt reconnect in subscribe - it can take too long
    // Reconnection should be handled by NetworkManager task
    bool result = false;
    if (_mqttClient->connected()) {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.print("Subscribing to topic: ");
            Serial.println(topic);
        }
        result = _mqttClient->subscribe(topic);
        if (result) {
            _subscribedTopics.push_back(String(topic));
        }
    } else {
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            Serial.print("[MQTT] Cannot subscribe to ");
            Serial.print(topic);
            Serial.println(" - MQTT not connected");
        }
    }
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return result;
}

void MqttLteClient::loop() {
    // CRITICAL FIX: Only hold mutex for the actual PubSubClient loop() call
    // This prevents blocking the publisher task during status checks and other operations
    static unsigned long lastConnectionCheck = 0;
    static unsigned long lastHealthLog = 0;
    static unsigned long lastGprsKeepAlive = 0;
    static bool wasConnected = false;
    
    // Check connection status more frequently to detect issues early
    // Do this WITHOUT mutex to avoid blocking publisher
    bool currentlyConnected = false;
    if (millis() - lastConnectionCheck > 5000) {  // Check every 5 seconds
        lastConnectionCheck = millis();
        
        // Quick check without mutex - use cached state if mutex is busy
        if (_mutex) {
            TickType_t timeoutTicks = pdMS_TO_TICKS(10); // 10ms timeout
            if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) == pdTRUE) {
                currentlyConnected = _mqttClient->connected();
                _mqttConnected = currentlyConnected; // Update cache
                xSemaphoreGiveRecursive(_mutex);
            } else {
                // Mutex busy - use cached state
                currentlyConnected = _mqttConnected;
            }
        } else {
            currentlyConnected = _mqttClient->connected();
        }
        
        // Detect connection state changes
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
            if (wasConnected && !currentlyConnected) {
                Serial.println("[MQTT] Connection LOST - was connected, now disconnected");
            } else if (!wasConnected && currentlyConnected) {
                Serial.println("[MQTT] Connection ESTABLISHED - was disconnected, now connected");
            }
        }
        wasConnected = currentlyConnected;
        
        if (!currentlyConnected && _networkConnected) {
            // Get MQTT client state for diagnostics (only if we can get mutex)
            if (_mutex && xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                int state = _mqttClient->state();
                if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                    Serial.print("MQTT disconnected (state: ");
                    Serial.print(state);
                    Serial.println("), will reconnect on next call...");
                }
                
                // Track disconnections to trigger cleanup if needed
                if (state < 0) {
                    // Negative states indicate connection errors
                    _consecutiveFailures++;
                }
                xSemaphoreGiveRecursive(_mutex);
            }
            
            // Don't call reconnect() here - let it be called from NetworkManager
            // This prevents potential blocking in the loop() function
        } else if (currentlyConnected) {
            // Reset failure counter when connected
            if (_consecutiveFailures > 0) {
                _consecutiveFailures = 0;
            }
        }
    } else {
        // Use cached state if not time to check yet
        currentlyConnected = _mqttConnected;
    }
    
    // CRITICAL FIX: Periodic GPRS keep-alive to prevent connection timeout
    // Many cellular modems timeout GPRS connections after 30-60 seconds of inactivity
    // Check IP address every 60 seconds to keep the connection alive (reduced frequency to avoid interference)
    // MUST use mutex to prevent UART interference with ongoing SSL/MQTT operations
    if (_networkConnected && _modem && millis() - lastGprsKeepAlive > 60000) {
        lastGprsKeepAlive = millis();
        
        // CRITICAL: Protect modem UART access with mutex to prevent interference
        // If mutex is busy (loop() or publish() active), skip this check to avoid UART conflict
        if (_mutex) {
            TickType_t timeoutTicks = pdMS_TO_TICKS(50); // 50ms timeout - quick check
            if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) == pdTRUE) {
                // Lightweight keep-alive: just check if we still have a valid IP
                // This is less intrusive than isGprsConnected() but still keeps the connection active
                String ip = _modem->localIP().toString();
                xSemaphoreGiveRecursive(_mutex);
                
                if (!isValidIP(ip)) {
                    // IP is invalid, connection may have dropped
                    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                        Serial.println("[NETWORK] Keep-alive check: IP address invalid, connection may be lost");
                    }
                    _networkConnected = false;
                }
                // If IP is valid, the connection is still alive (no need to log every time)
            } else {
                // Mutex busy - skip this keep-alive check to avoid UART interference
                // This prevents conflicts when loop() is doing SSL operations
                // The connection should still be alive if loop() is working
            }
        } else {
            // No mutex - check directly (shouldn't happen in normal operation)
            String ip = _modem->localIP().toString();
            if (!isValidIP(ip)) {
                if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                    Serial.println("[NETWORK] Keep-alive check: IP address invalid, connection may be lost");
                }
                _networkConnected = false;
            }
        }
    }
    
    // REMOVED: Signal quality check from loop() - causes UART interference
    // Signal quality can be checked manually via debug_network command
    
    // Periodic health logging (every 60 seconds) - no mutex needed
    if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS && currentlyConnected && millis() - lastHealthLog > 60000) {
        lastHealthLog = millis();
        Serial.println("[MQTT HEALTH] Connection stable (keep-alive: 60s)");
    }
    
    // CRITICAL: Only hold mutex for the actual PubSubClient loop() call
    // This is the only operation that needs exclusive access to the MQTT client
    // Use timeout instead of portMAX_DELAY to allow other tasks to proceed if needed
    if (currentlyConnected) {
        if (_mutex) {
            TickType_t timeoutTicks = pdMS_TO_TICKS(100); // 100ms timeout - quick check
            unsigned long loopStart = millis();
            if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) == pdTRUE) {
                // CRITICAL FIX: Limit how long loop() can run to prevent blocking
                // If loop() takes too long, it means SSL/network operations are blocking
                // The SSL client timeout is 4 seconds, so if loop() takes >5 seconds, something is wrong
                unsigned long loopCallStart = millis();
                _mqttClient->loop();  // Process incoming MQTT messages
                unsigned long loopCallDuration = millis() - loopCallStart;
                
                // Release mutex immediately after loop() completes
                unsigned long totalDuration = millis() - loopStart;
                xSemaphoreGiveRecursive(_mutex);
                
                // Log warnings if loop() took too long
                if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                    if (loopCallDuration > 5000) {
                        // loop() took >5 seconds - this is abnormal and indicates network/SSL issues
                        Serial.print("[MQTT LOOP] CRITICAL: loop() took ");
                        Serial.print(loopCallDuration);
                        Serial.println("ms - network connection may be lost or SSL operations failed");
                        Serial.println("[MQTT LOOP] This may cause GPRS connection timeout - consider reconnecting");
                        // Mark network as potentially disconnected if loop() took too long
                        // The next keep-alive check will verify the connection
                    } else if (loopCallDuration > 1000) {
                        Serial.print("[MQTT LOOP] WARNING: loop() took ");
                        Serial.print(loopCallDuration);
                        Serial.println("ms - network may be slow or SSL operations blocking");
                    } else if (totalDuration > 200) {
                        Serial.print("[MQTT LOOP] Held mutex for ");
                        Serial.print(totalDuration);
                        Serial.println("ms");
                    }
                }
            } else {
                // Mutex not available - skip this iteration (publisher has priority)
                if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                    Serial.println("[MQTT LOOP] Skipped - mutex busy (publisher active)");
                }
            }
        } else {
            _mqttClient->loop();  // No mutex, call directly (shouldn't happen)
        }
    }
}

bool MqttLteClient::isConnected() {
    // Use short timeout to prevent blocking main loop
    // If mutex is busy, return last known state to avoid blocking
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(10); // 10ms timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            // Mutex not available - return cached state to avoid blocking
            return _mqttConnected;
        }
    }
    bool c = _mqttClient->connected();
    _mqttConnected = c; // Update cached state
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return c;
}

bool MqttLteClient::isNetworkConnected() {
    static bool lastNetworkState = false;
    static unsigned long lastStateChange = 0;
    static unsigned long lastCheckTime = 0;
    
    if (_modem) {
        // CRITICAL FIX: Don't check network status too frequently
        // Checking GPRS status requires AT commands that can interfere with MQTT
        // Only check every 10 seconds minimum (increased from 5s to reduce interference)
        unsigned long now = millis();
        if (now - lastCheckTime < 10000 && lastCheckTime > 0) {
            // Return cached state to avoid excessive modem queries
            return _networkConnected;
        }
        lastCheckTime = now;
        
        // CRITICAL FIX: Use mutex protection to prevent UART interference
        // This ensures network checks don't interfere with ongoing MQTT/SSL operations
        bool currentState = false;
        if (_mutex) {
            // Use timeout to prevent blocking if mutex is held by MQTT operation
            TickType_t timeoutTicks = pdMS_TO_TICKS(100); // 100ms timeout
            if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) == pdTRUE) {
                // Check GPRS connection status while holding mutex
                currentState = _modem->isGprsConnected();
                xSemaphoreGiveRecursive(_mutex);
            } else {
                // Mutex not available - return cached state to avoid blocking
                // This prevents interference with ongoing operations
                return _networkConnected;
            }
        } else {
            // No mutex available, check directly (shouldn't happen in normal operation)
            currentState = _modem->isGprsConnected();
        }
        
        // Detect network state changes
        if (lastNetworkState != currentState) {
            unsigned long timeSinceLastChange = now - lastStateChange;
            
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                if (currentState) {
                    Serial.println("[NETWORK] ✓ GPRS connection ESTABLISHED");
                } else {
                    Serial.print("[NETWORK] ✗ GPRS connection LOST after ");
                    Serial.print(timeSinceLastChange / 1000);
                    Serial.println(" seconds");
                    Serial.println("[NETWORK] POSSIBLE CAUSES: Power supply issue, modem crash, or UART interference");
                }
            }
            
            lastNetworkState = currentState;
            lastStateChange = now;
        }
        
        _networkConnected = currentState;
    }
    return _networkConnected;
}

void MqttLteClient::setBufferSize(size_t size) {
    // These operations are quick, but use timeout for consistency
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(1000); // 1 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS) {
                Serial.println("[MQTT] Failed to acquire mutex for setBufferSize");
            }
            return;
        }
    }
    if (_mqttClient) {
        _mqttClient->setBufferSize(size);
    }
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

String MqttLteClient::getLocalIP() {
    if (_modem && _networkConnected) {
        return _modem->localIP().toString();
    }
    return "0.0.0.0";
}

int MqttLteClient::getSignalQuality() {
    if (_modem) {
        int quality = _modem->getSignalQuality();
        // TinyGSM returns 99 as error code, treat as 0 (no signal)
        if (ENABLE_NETWORK_MANAGER_DIAGNOSTICS && quality == 99) {
            Serial.println("[NETWORK WARNING] Signal quality read error (99) - modem may be unresponsive");
        }
        if (quality == 99) {
            return 0;
        }
        return quality;
    }
    return 0;
}

bool MqttLteClient::isValidIP(const String& ip) {
    // Check for invalid patterns
    if (ip.length() < 7 || ip == "0.0.0.0") {
        return false;
    }
    
    // Check for corrupted IP addresses (like "7.0.0.0" or malformed ones)
    // Valid private IPs usually start with 10., 172.16-31., or 192.168.
    // But cellular networks may use different ranges, so we just check basic validity
    
    int dotCount = 0;
    for (unsigned int i = 0; i < ip.length(); i++) {
        if (ip[i] == '.') {
            dotCount++;
        } else if (!isdigit(ip[i])) {
            // Invalid character in IP
            return false;
        }
    }
    
    // Should have exactly 3 dots
    if (dotCount != 3) {
        return false;
    }
    
    // Parse octets and validate range
    int octets[4] = {0, 0, 0, 0};
    int octetIndex = 0;
    String currentOctet = "";
    
    for (unsigned int i = 0; i < ip.length(); i++) {
        if (ip[i] == '.') {
            if (currentOctet.length() == 0 || octetIndex >= 3) {
                return false; // Empty octet or too many octets
            }
            octets[octetIndex++] = currentOctet.toInt();
            currentOctet = "";
        } else {
            currentOctet += ip[i];
        }
    }
    
    // Last octet
    if (currentOctet.length() > 0 && octetIndex == 3) {
        octets[3] = currentOctet.toInt();
    } else {
        return false;
    }
    
    // Validate each octet is in valid range (0-255)
    for (int i = 0; i < 4; i++) {
        if (octets[i] < 0 || octets[i] > 255) {
            return false;
        }
    }
    
    // Additional check: 0.x.x.x is usually invalid
    if (octets[0] == 0) {
        return false;
    }
    
    return true;
}

void MqttLteClient::printNetworkDiagnostics() {
    if (!_modem) {
        Serial.println("[NETWORK DIAG] Modem not initialized");
        return;
    }
    
    Serial.println("=== NETWORK DIAGNOSTICS ===");
    
    // Check GPRS connection
    bool gprsConnected = _modem->isGprsConnected();
    Serial.print("[NETWORK DIAG] GPRS Connected: ");
    Serial.println(gprsConnected ? "YES" : "NO");
    
    // Check network registration
    bool networkConnected = _modem->isNetworkConnected();
    Serial.print("[NETWORK DIAG] Network Registered: ");
    Serial.println(networkConnected ? "YES" : "NO");
    
    // Get signal quality
    int signalQuality = _modem->getSignalQuality();
    Serial.print("[NETWORK DIAG] Signal Quality: ");
    Serial.print(signalQuality);
    Serial.print("/31 (");
    if (signalQuality == 99) {
        Serial.println("ERROR - modem not responding to AT+CSQ)");
    } else if (signalQuality == 0) {
        Serial.println("No signal)");
    } else if (signalQuality < 10) {
        Serial.println("Poor)");
    } else if (signalQuality < 20) {
        Serial.println("Fair)");
    } else {
        Serial.println("Good)");
    }
    
    // Get IP address
    String ip = _modem->localIP().toString();
    Serial.print("[NETWORK DIAG] IP Address: ");
    Serial.print(ip);
    Serial.print(" (");
    Serial.print(isValidIP(ip) ? "Valid" : "Invalid");
    Serial.println(")");
    
    // Get operator name
    String operatorName = _modem->getOperator();
    Serial.print("[NETWORK DIAG] Operator: ");
    Serial.println(operatorName);
    
    // Test modem AT responsiveness
    Serial.print("[NETWORK DIAG] Testing modem AT responsiveness... ");
    _modemSerial.println("AT");
    unsigned long start = millis();
    String response = "";
    bool gotResponse = false;
    
    while (millis() - start < 1000) {
        if (_modemSerial.available()) {
            char c = _modemSerial.read();
            response += c;
            if (response.indexOf("OK") != -1) {
                gotResponse = true;
                break;
            }
        }
    }
    
    if (gotResponse) {
        Serial.println("OK (modem responding)");
    } else {
        Serial.println("FAILED (modem not responding!)");
        Serial.print("[NETWORK DIAG] Response received: ");
        Serial.println(response);
    }
    
    // MQTT status
    Serial.print("[NETWORK DIAG] MQTT Connected: ");
    Serial.println(_mqttConnected ? "YES" : "NO");
    
    if (_mqttClient) {
        int mqttState = _mqttClient->state();
        Serial.print("[NETWORK DIAG] MQTT State: ");
        Serial.print(mqttState);
        Serial.print(" (");
        switch(mqttState) {
            case -4: Serial.print("TIMEOUT"); break;
            case -3: Serial.print("CONNECTION_LOST"); break;
            case -2: Serial.print("CONNECT_FAILED"); break;
            case -1: Serial.print("DISCONNECTED"); break;
            case 0: Serial.print("CONNECTED"); break;
            case 1: Serial.print("BAD_PROTOCOL"); break;
            case 2: Serial.print("BAD_CLIENT_ID"); break;
            case 3: Serial.print("UNAVAILABLE"); break;
            case 4: Serial.print("BAD_CREDENTIALS"); break;
            case 5: Serial.print("UNAUTHORIZED"); break;
            default: Serial.print("UNKNOWN");
        }
        Serial.println(")");
    }
    
    Serial.print("[NETWORK DIAG] Consecutive Failures: ");
    Serial.println(_consecutiveFailures);
    
    Serial.println("=========================");
}
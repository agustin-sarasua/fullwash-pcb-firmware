#include "mqtt_lte_client.h"
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
        Serial.println("Trying alternative baud rate (9600)...");
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
    Serial.println("Powering on SIM7600G module...");
    
    // Configure control pins
    pinMode(_pwrKeyPin, OUTPUT);
    pinMode(_dtrPin, OUTPUT);
    pinMode(_flightPin, OUTPUT);
    
    // Set DTR (Data Terminal Ready) active
    digitalWrite(_dtrPin, LOW);
    
    // Make sure flight mode is disabled
    digitalWrite(_flightPin, HIGH); 
    
    // SIM7600G power on sequence (based on datasheet)
    digitalWrite(_pwrKeyPin, LOW);  // Ensure PWRKEY starts LOW
    delay(1000);
    
    digitalWrite(_pwrKeyPin, HIGH); // Pull PWRKEY HIGH
    delay(2000);                    // Hold for >1 second
    
    digitalWrite(_pwrKeyPin, LOW);  // Release PWRKEY
    
    Serial.println("Waiting for modem to initialize...");
    delay(10000);  // Wait longer for the modem to boot
    
    clearModemBuffer();
    
    // Test AT command communication
    bool atSuccess = testModemAT();
    
    if (!atSuccess) {
        Serial.println("Trying alternative power on sequence...");
        
        // Alternative power on sequence sometimes needed for SIM7600
        digitalWrite(_pwrKeyPin, HIGH);
        delay(3000);
        digitalWrite(_pwrKeyPin, LOW);
        delay(5000);
        
        clearModemBuffer();
        
        // Test AT command again
        atSuccess = testModemAT();
        
        if (!atSuccess) {
            Serial.println("Still unable to communicate with modem!");
            Serial.println("Possible issues:");
            Serial.println("1. Check power supply to modem");
            Serial.println("2. Check UART connections (TX/RX)");
            Serial.println("3. Modem might not be powered properly");
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
    Serial.println("Testing direct AT communication with modem...");
    
    clearModemBuffer();
    
    // Flush any pending data
    while (_modemSerial.available()) {
        _modemSerial.read();
    }
    
    // Send AT command
    Serial.println("Sending: AT");
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
    
    Serial.println("Response: " + response);
    
    if (response.indexOf("OK") != -1) {
        Serial.println("Modem responded to AT command successfully!");
        return true;
    } else {
        Serial.println("Modem failed to respond to AT command properly.");
        return false;
    }
}

bool MqttLteClient::initModemAndConnectNetwork() {
    Serial.println("Initializing modem...");
    
    // Try to initialize the modem
    if (!_modem->init()) {
        Serial.println("Failed to initialize modem!");
        
        // Check if we can talk to the modem directly
        bool basicComm = testModemAT();
        
        if (basicComm) {
            Serial.println("Modem responds to AT commands but TinyGSM init failed.");
            Serial.println("This could be a TinyGSM library compatibility issue with SIM7600G.");
            
            // Try an alternative approach - direct AT commands
            Serial.println("Trying simplified initialization with direct AT commands...");
            
            // Send some basic config AT commands
            _modemSerial.println("AT+CFUN=1");  // Set full functionality
            delay(1000);
            
            _modemSerial.println("AT+CREG?");   // Check network registration
            delay(1000);
            
            // Try to connect to GPRS directly
            Serial.print("Connecting to ");
            Serial.print(_apn);
            Serial.println(" using direct AT commands...");
            
            // Set the APN
            _modemSerial.print("AT+CGDCONT=1,\"IP\",\"");
            _modemSerial.print(_apn);
            _modemSerial.println("\"");
            delay(1000);
            
            return false;  // Still return false to indicate TinyGSM init failed
        } else {
            Serial.println("Basic AT command communication failed.");
            Serial.println("Possible hardware issue - check wiring and power.");
            return false;
        }
    }
    
    // Get modem info
    String modemInfo = _modem->getModemInfo();
    Serial.print("Modem Info: ");
    Serial.println(modemInfo);
    
    // Set network mode to automatic (2G/3G/4G)
    String networkMode;
    networkMode = _modem->setNetworkMode(2);
    Serial.print("Network mode set: ");
    Serial.println(networkMode);
    
    if (_pin && _modem->getSimStatus() != 3) {
        _modem->simUnlock(_pin);
    }
    
    // Wait for network connection
    Serial.print("Waiting for network...");
    if (!_modem->waitForNetwork(60000L)) {
        Serial.println(" fail");
        return false;
    }
    Serial.println(" success");
    
    if (_modem->isNetworkConnected()) {
        Serial.println("Network connected");
    } else {
        Serial.println("Network connection failed");
        return false;
    }
    
    // Connect to GPRS
    Serial.print("Connecting to ");
    Serial.print(_apn);
    if (!_modem->gprsConnect(_apn, _user, _pass)) {
        Serial.println(" fail");
        return false;
    }
    Serial.println(" success");
    
    if (_modem->isGprsConnected()) {
        Serial.println("GPRS connected");
        
        // Get and validate IP address
        String ip = _modem->localIP().toString();
        Serial.print("IP address: ");
        Serial.println(ip);
        
        // Validate IP address before declaring success
        if (!isValidIP(ip)) {
            Serial.print("Invalid IP address received: ");
            Serial.println(ip);
            Serial.println("GPRS connection appears unstable, will retry");
            return false;
        }
        
        // Get and log signal quality
        int signalQuality = _modem->getSignalQuality();
        Serial.print("Signal quality: ");
        Serial.print(signalQuality);
        Serial.println("/31");
        
        if (signalQuality < 10) {
            Serial.println("WARNING: Poor signal quality detected, connection may be unstable");
        }
        
        _networkConnected = true;
        return true;
    } else {
        Serial.println("GPRS connection failed");
        return false;
    }
}

void MqttLteClient::setCACert(const char* caCert) {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _sslClient->setCACert(caCert);
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

void MqttLteClient::setCertificate(const char* clientCert) {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _sslClient->setCertificate(clientCert);
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

void MqttLteClient::setPrivateKey(const char* privateKey) {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _sslClient->setPrivateKey(privateKey);
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

void MqttLteClient::setCallback(void (*callback)(char*, byte*, unsigned int)) {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _callback = callback;
    _mqttClient->setCallback(_callback);
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

bool MqttLteClient::connect(const char* broker, uint16_t port, const char* clientId) {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _broker = broker;
    _port = port;
    _clientId = clientId;
    
    _mqttClient->setServer(_broker, _port);
    
    // Keep-alive reduced to 60s to prevent SSL timeout issues
    // SSL sessions were failing at ~106s with 120s keep-alive
    _mqttClient->setKeepAlive(60);

    // Check signal quality before attempting SSL connection
    int signalQuality = getSignalQuality();
    if (signalQuality > 0 && signalQuality < 10) {
        Serial.print("WARNING: Poor signal quality (");
        Serial.print(signalQuality);
        Serial.println("/31) - SSL connection may fail");
    }

    // Make a single connection attempt to prevent long blocking
    // Retries are handled by the calling code (NetworkManager task)
    Serial.print("Attempting MQTT connection...");
    
    // Attempt to connect (this can block for up to 4 seconds during SSL handshake)
    if (_mqttClient->connect(_clientId)) {
        Serial.println("connected");
        _mqttConnected = true;
        _consecutiveFailures = 0; // Reset failure counter on success
        if (_mutex) xSemaphoreGiveRecursive(_mutex);
        return true;
    } else {
        Serial.print("failed, rc=");
        Serial.println(_mqttClient->state());
        _consecutiveFailures++;
    }
    
    // If we've had multiple consecutive failures, log warning
    if (_consecutiveFailures > 3) {
        Serial.print("WARNING: ");
        Serial.print(_consecutiveFailures);
        Serial.println(" consecutive SSL/MQTT connection failures");
        Serial.println("This may indicate certificate issues or very poor signal");
    }
    
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return false;
}

void MqttLteClient::cleanupSSLClient() {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    Serial.println("[DEBUG] Cleaning up SSL client to clear corrupted state");
    
    // Disconnect MQTT client first
    if (_mqttClient && _mqttClient->connected()) {
        _mqttClient->disconnect();
    }
    
    // Stop SSL client
    if (_sslClient) {
        _sslClient->stop();
    }
    
    Serial.println("[DEBUG] SSL client cleanup complete");
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

void MqttLteClient::reconnect() {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
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
        Serial.print("[INFO] ");
        Serial.print(_consecutiveFailures);
        Serial.println(" consecutive failures - performing SSL cleanup");
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
    Serial.print("Attempting MQTT connection...");
    
    // Attempt to connect (this is a non-blocking call with timeout)
    if (_mqttClient->connect(_clientId)) {
        Serial.println("connected");
        _mqttConnected = true;
        _consecutiveFailures = 0; // Reset on success
        
        // Re-subscribe to all previously subscribed topics
        for (const String& topic : _subscribedTopics) {
            Serial.print("Re-subscribing to topic: ");
            Serial.println(topic);
            _mqttClient->subscribe(topic.c_str());
        }
        _reconnectInterval = 5000; // Reset interval on success
    } else {
        Serial.print("failed, rc=");
        Serial.println(_mqttClient->state());
        _consecutiveFailures++;
        
        // Exponential backoff - double the interval up to 2 minutes
        _reconnectInterval = min(_reconnectInterval * 2, 120000);
    }
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
}

bool MqttLteClient::publish(const char* topic, const char* payload, const uint8_t qos) {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    if (!_mqttClient->connected()) {
        // Recursive lock safe
        Serial.print("[MQTT] Not connected, attempting reconnect before publish to ");
        Serial.println(topic);
        reconnect();
    }
    bool ok = false;
    if (_mqttClient->connected()) {
        ok = _mqttClient->publish(topic, payload);
        if (!ok) {
            Serial.print("[MQTT ERROR] Failed to publish to ");
            Serial.print(topic);
            Serial.print(" (QoS: ");
            Serial.print(qos);
            Serial.print(", state: ");
            Serial.print(_mqttClient->state());
            Serial.println(")");
        }
    } else {
        Serial.print("[MQTT ERROR] Cannot publish to ");
        Serial.print(topic);
        Serial.print(" - MQTT not connected (state: ");
        Serial.print(_mqttClient->state());
        Serial.println(")");
    }
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return ok;
}

bool MqttLteClient::publishNonBlocking(const char* topic, const char* payload, const uint8_t qos, TickType_t timeoutMs) {
    // Try to acquire mutex with timeout - if we can't get it immediately, skip publish
    // This prevents blocking critical operations like button handling
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(timeoutMs);
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            // Mutex not available - skip publish to avoid blocking
            return false;
        }
    }
    
    // Don't attempt reconnect in non-blocking mode - it's too slow
    // Just try to publish if already connected
    bool ok = false;
    if (_mqttClient->connected()) {
        ok = _mqttClient->publish(topic, payload);
        if (!ok) {
            Serial.print("[MQTT] Non-blocking publish failed to ");
            Serial.println(topic);
        }
    }
    
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return ok;
}

bool MqttLteClient::subscribe(const char* topic) {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    if (!_mqttClient->connected()) {
        Serial.println("Reconnecting MQTT client before subscribing...");
        reconnect();
    }
    bool result = false;
    if (_mqttClient->connected()) {
        Serial.print("Subscribing to topic: ");
        Serial.println(topic);
        result = _mqttClient->subscribe(topic);
        if (result) {
            _subscribedTopics.push_back(String(topic));
        }
    }
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return result;
}

void MqttLteClient::loop() {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    static unsigned long lastConnectionCheck = 0;
    static unsigned long lastHealthLog = 0;
    
    // Check connection status more frequently to detect issues early
    if (millis() - lastConnectionCheck > 5000) {  // Check every 5 seconds (increased from 10s)
        lastConnectionCheck = millis();
        
        if (!_mqttClient->connected() && _networkConnected) {
            // Get MQTT client state for diagnostics
            int state = _mqttClient->state();
            Serial.print("MQTT disconnected (state: ");
            Serial.print(state);
            Serial.println("), will reconnect on next call...");
            
            // Track disconnections to trigger cleanup if needed
            if (state < 0) {
                // Negative states indicate connection errors
                _consecutiveFailures++;
            }
            
            // Don't call reconnect() here - let it be called from NetworkManager
            // This prevents potential blocking in the loop() function
        } else if (_mqttClient->connected()) {
            // Reset failure counter when connected
            if (_consecutiveFailures > 0) {
                _consecutiveFailures = 0;
            }
        }
    }
    
    // Periodic health logging (every 60 seconds)
    if (_mqttClient->connected() && millis() - lastHealthLog > 60000) {
        lastHealthLog = millis();
        Serial.println("[MQTT HEALTH] Connection stable");
    }
    
    // Always call the PubSubClient loop (this handles keep-alive pings automatically)
    if (_mqttClient->connected()) {
        _mqttClient->loop();
    }
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
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
    if (_modem) {
        _networkConnected = _modem->isGprsConnected();
    }
    return _networkConnected;
}

void MqttLteClient::setBufferSize(size_t size) {
    if (_mutex) xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
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
        return _modem->getSignalQuality();
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
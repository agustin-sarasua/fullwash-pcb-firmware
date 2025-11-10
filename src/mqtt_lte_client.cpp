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
    
    // SIM7600G power on sequence (based on datasheet)
    digitalWrite(_pwrKeyPin, LOW);  // Ensure PWRKEY starts LOW
    delay(1000);
    
    digitalWrite(_pwrKeyPin, HIGH); // Pull PWRKEY HIGH
    delay(2000);                    // Hold for >1 second
    
    digitalWrite(_pwrKeyPin, LOW);  // Release PWRKEY
    
    delay(10000);  // Wait longer for the modem to boot
    
    clearModemBuffer();
    
    // Test AT command communication
    bool atSuccess = testModemAT();
    
    if (!atSuccess) {
        // Alternative power on sequence sometimes needed for SIM7600
        digitalWrite(_pwrKeyPin, HIGH);
        delay(3000);
        digitalWrite(_pwrKeyPin, LOW);
        delay(5000);
        
        clearModemBuffer();
        
        // Test AT command again
        atSuccess = testModemAT();
    }
}

void MqttLteClient::clearModemBuffer() {
    delay(100);
    while (_modemSerial.available()) {
        _modemSerial.read();
    }
}

bool MqttLteClient::testModemAT() {
    clearModemBuffer();
    
    // Flush any pending data
    while (_modemSerial.available()) {
        _modemSerial.read();
    }
    
    // Send AT command
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
    
    if (response.indexOf("OK") != -1) {
        return true;
    } else {
        return false;
    }
}

bool MqttLteClient::initModemAndConnectNetwork() {
    // CRITICAL FIX: Clear any pending data before init
    clearModemBuffer();
    delay(500);
    
    // Try to initialize the modem
    if (!_modem->init()) {
        // Check if we can talk to the modem directly
        bool basicComm = testModemAT();
        
        if (basicComm) {
            // Send some basic config AT commands
            _modemSerial.println("AT+CFUN=1");  // Set full functionality
            delay(1000);
            
            _modemSerial.println("AT+CREG?");   // Check network registration
            delay(1000);
            
            // Set the APN
            _modemSerial.print("AT+CGDCONT=1,\"IP\",\"");
            _modemSerial.print(_apn);
            _modemSerial.println("\"");
            delay(1000);
            
            return false;  // Still return false to indicate TinyGSM init failed
        } else {
            return false;
        }
    }
    
    // Get modem info
    String modemInfo = _modem->getModemInfo();
    
    // Set network mode to automatic (2G/3G/4G)
    String networkMode;
    networkMode = _modem->setNetworkMode(2);
    
    if (_pin && _modem->getSimStatus() != 3) {
        _modem->simUnlock(_pin);
    }
    
    // Wait for network connection
    if (!_modem->waitForNetwork(60000L)) {
        return false;
    }
    
    if (!_modem->isNetworkConnected()) {
        return false;
    }
    
    // Connect to GPRS
    if (!_modem->gprsConnect(_apn, _user, _pass)) {
        return false;
    }
    
    if (_modem->isGprsConnected()) {
        // CRITICAL FIX: Configure modem to prevent GPRS connection timeout
        // Some modems timeout GPRS connections after 30-60 seconds of inactivity
        // Configure keep-alive and disable auto-disconnect
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
        
        // Validate IP address before declaring success
        if (!isValidIP(ip)) {
            return false;
        }
        
        _networkConnected = true;
        return true;
    } else {
        return false;
    }
}

void MqttLteClient::setCACert(const char* caCert) {
    // These operations are quick, but use timeout for consistency
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(1000); // 1 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
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

    // Make a single connection attempt to prevent long blocking
    // Retries are handled by the calling code (NetworkManager task)
    
    // Attempt to connect (this can block for up to 4 seconds during SSL handshake)
    if (_mqttClient->connect(_clientId)) {
        _mqttConnected = true;
        _consecutiveFailures = 0; // Reset SSL failure counter on success
        _consecutivePublishFailures = 0; // Reset publish failure counter on success
        if (_mutex) xSemaphoreGiveRecursive(_mutex);
        return true;
    } else {
        _consecutiveFailures++;
    }
    
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return false;
}

void MqttLteClient::cleanupSSLClient() {
    // CRITICAL FIX: Use timeout instead of portMAX_DELAY to prevent deadlock
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(2000); // 2 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            return;
        }
    }
    
    // Disconnect MQTT client first
    if (_mqttClient && _mqttClient->connected()) {
        _mqttClient->disconnect();
    }
    
    // Stop SSL client
    if (_sslClient) {
        _sslClient->stop();
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
    
    // Attempt to connect (this is a non-blocking call with timeout)
    if (_mqttClient->connect(_clientId)) {
        _mqttConnected = true;
        _consecutiveFailures = 0; // Reset SSL failure counter on success
        _consecutivePublishFailures = 0; // Reset publish failure counter on success
        
        // Re-subscribe to all previously subscribed topics
        for (const String& topic : _subscribedTopics) {
            _mqttClient->subscribe(topic.c_str());
        }
        _reconnectInterval = 5000; // Reset interval on success
    } else {
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
            return false;
        }
    }
    
    // Don't attempt reconnect in blocking publish - it can take too long
    // Reconnection should be handled by NetworkManager task
    bool ok = false;
    if (_mqttClient->connected()) {
        ok = _mqttClient->publish(topic, payload);
        if (!ok) {
            notifyPublishFailure();  // Track failure for smart connectivity checking
        } else {
            // Reset failure counter on success
            _consecutivePublishFailures = 0;
        }
    } else {
        notifyPublishFailure();  // Track failure even when not connected
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
                return false;
        }
    }
    
    // Don't attempt reconnect in non-blocking mode - it's too slow
    // Just try to publish if already connected
    bool ok = false;
    if (_mqttClient->connected()) {
        ok = _mqttClient->publish(topic, payload);
        if (!ok) {
            notifyPublishFailure();  // Track failure for smart connectivity checking
        } else {
            // Reset failure counter on success
            _consecutivePublishFailures = 0;
        }
    } else {
        notifyPublishFailure();  // Track failure even when not connected
    }
    
    if (_mutex) xSemaphoreGiveRecursive(_mutex);
    return ok;
}

bool MqttLteClient::subscribe(const char* topic) {
    // CRITICAL FIX: Use timeout instead of portMAX_DELAY to prevent deadlock
    if (_mutex) {
        TickType_t timeoutTicks = pdMS_TO_TICKS(2000); // 2 second timeout
        if (xSemaphoreTakeRecursive(_mutex, timeoutTicks) != pdTRUE) {
            return false;
        }
    }
    
    // Don't attempt reconnect in subscribe - it can take too long
    // Reconnection should be handled by NetworkManager task
    bool result = false;
    if (_mqttClient->connected()) {
        result = _mqttClient->subscribe(topic);
        if (result) {
            _subscribedTopics.push_back(String(topic));
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
    
    // SMART CONNECTIVITY CHECKING: Check less frequently when things are working
    // - When connected and no failures: check every 30 seconds (reduced from 5s)
    // - When failures occur: check more frequently (handled by forceConnectivityCheck)
    unsigned long checkInterval = 30000;  // Default: 30 seconds when things are working
    if (_consecutivePublishFailures > 0) {
        // If there are failures, check more frequently
        checkInterval = 5000;  // 5 seconds when failures detected
    }
    
    // Check connection status - reduced frequency to avoid mutex contention
    // Do this WITHOUT mutex to avoid blocking publisher
    bool currentlyConnected = false;
    if (millis() - lastConnectionCheck > checkInterval) {
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
        
        wasConnected = currentlyConnected;
        
        if (!currentlyConnected && _networkConnected) {
            // Get MQTT client state for diagnostics (only if we can get mutex)
            if (_mutex && xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                int state = _mqttClient->state();
                
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
            // Reset failure counters when connected
            if (_consecutiveFailures > 0) {
                _consecutiveFailures = 0;
            }
            if (_consecutivePublishFailures > 0) {
                _consecutivePublishFailures = 0;
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
                _networkConnected = false;
            }
        }
    }
    
    // CRITICAL: Only hold mutex for the actual PubSubClient loop() call
    // This is the only operation that needs exclusive access to the MQTT client
    // Use timeout instead of portMAX_DELAY to allow other tasks to proceed if needed
    if (currentlyConnected) {
        if (_mutex) {
            // CRITICAL FIX: Increased timeout to 500ms to be more aggressive
            // Loop() must succeed to process incoming messages, so we wait longer
            TickType_t timeoutTicks = pdMS_TO_TICKS(500); // 500ms timeout - more persistent
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
                
                // Track if loop() is consistently taking too long
                static unsigned long lastSlowLoopTime = 0;
                static int consecutiveSlowLoops = 0;
                
                // Track slow loops for internal monitoring
                if (loopCallDuration > 5000) {
                    consecutiveSlowLoops++;
                    lastSlowLoopTime = millis();
                } else if (loopCallDuration > 1000) {
                    consecutiveSlowLoops++;
                    lastSlowLoopTime = millis();
                } else {
                    // Reset counter on successful fast loop
                    if (loopCallDuration < 500) {
                        consecutiveSlowLoops = 0;
                    }
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
        // SMART CONNECTIVITY CHECKING: Check less frequently when things are working
        // - When connected and no failures: check every 60 seconds (increased from 10s)
        // - When failures occur or forced check: check immediately
        unsigned long now = millis();
        unsigned long minCheckInterval = 60000;  // Default: 60 seconds when things are working
        
        // If there are publish failures or a forced check was requested, check more frequently
        if (_consecutivePublishFailures >= 3 || (now - _lastForcedConnectivityCheck < 10000)) {
            minCheckInterval = 5000;  // 5 seconds when failures detected
        }
        
        if (now - lastCheckTime < minCheckInterval && lastCheckTime > 0) {
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
    // Diagnostic function - output removed per user request
    // This function can be used for debugging but no longer prints to Serial
}

void MqttLteClient::notifyPublishFailure() {
    // Track consecutive publish failures
    unsigned long now = millis();
    
    // Reset counter if last failure was more than 30 seconds ago (likely transient issue)
    if (now - _lastPublishFailureTime > 30000) {
        _consecutivePublishFailures = 0;
    }
    
    _consecutivePublishFailures++;
    _lastPublishFailureTime = now;
    
    // If we've had 3 consecutive failures, trigger a connectivity check
    if (_consecutivePublishFailures >= 3) {
        forceConnectivityCheck();
    }
}

void MqttLteClient::forceConnectivityCheck() {
    // Mark that we need to check connectivity
    _lastForcedConnectivityCheck = millis();
    
    // Reset the static lastCheckTime in isNetworkConnected() by forcing a check
    // This will be handled on the next call to isNetworkConnected()
}
#include "mqtt_lte_client.h"

MqttLteClient::MqttLteClient(HardwareSerial& modemSerial, int pwrKeyPin, int dtrPin, int flightPin, 
                           int txPin, int rxPin)
    : _modemSerial(modemSerial), _pwrKeyPin(pwrKeyPin), _dtrPin(dtrPin), _flightPin(flightPin),
      _txPin(txPin), _rxPin(rxPin), _initialized(false), _networkConnected(false), _mqttConnected(false) {
    
    _modem = new TinyGsm(_modemSerial);
    _gsmClient = new TinyGsmClient(*_modem);
    _sslClient = new SSLClient(_gsmClient);
    _mqttClient = new PubSubClient(*_sslClient);

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
        
        // Get IP address
        String ip = _modem->localIP().toString();
        Serial.print("IP address: ");
        Serial.println(ip);
        
        _networkConnected = true;
        return true;
    } else {
        Serial.println("GPRS connection failed");
        return false;
    }
}

void MqttLteClient::setCACert(const char* caCert) {
    _sslClient->setCACert(caCert);
}

void MqttLteClient::setCertificate(const char* clientCert) {
    _sslClient->setCertificate(clientCert);
}

void MqttLteClient::setPrivateKey(const char* privateKey) {
    _sslClient->setPrivateKey(privateKey);
}

void MqttLteClient::setCallback(void (*callback)(char*, byte*, unsigned int)) {
    _callback = callback;
    _mqttClient->setCallback(_callback);
}

bool MqttLteClient::connect(const char* broker, uint16_t port, const char* clientId) {
    _broker = broker;
    _port = port;
    _clientId = clientId;
    
    _mqttClient->setServer(_broker, _port);
    
    _mqttClient->setKeepAlive(120); // Increase from default 15s to 120s

    // Loop until we're reconnected or timeout
    unsigned long startTime = millis();
    while (!_mqttClient->connected() && millis() - startTime < 10000) {
        Serial.print("Attempting MQTT connection...");
        
        // Attempt to connect
        if (_mqttClient->connect(_clientId)) {
            Serial.println("connected");
            _mqttConnected = true;
            return true;
        } else {
            Serial.print("failed, rc=");
            Serial.print(_mqttClient->state());
            Serial.println("...trying again");
            delay(1000);
        }
    }
    
    return false;
}

void MqttLteClient::reconnect() {
    // Loop until we're reconnected or timeout
    unsigned long now = millis();
    if (now - _lastReconnectAttempt < _reconnectInterval) {
        return; // Don't try too frequently
    }
    _lastReconnectAttempt = now;

    unsigned long startTime = millis();
    bool reconnected = false;
    
    while (!_mqttClient->connected() && millis() - startTime < 10000) {
        Serial.print("Attempting MQTT connection...");
        
        // Attempt to connect
        if (_mqttClient->connect(_clientId)) {
            Serial.println("connected");
            _mqttConnected = true;
            reconnected = true;
            break;  // Exit the loop on successful connection
        } else {
            Serial.print("failed, rc=");
            Serial.print(_mqttClient->state());
            Serial.println("...trying again");
            delay(1000);
        }
    }
    
    // If successfully reconnected, re-subscribe to all previously subscribed topics
    if (!reconnected) {
        // Exponential backoff - double the interval up to 2 minutes
        _reconnectInterval = min(_reconnectInterval * 2, 120000);
    } else {
        for (const String& topic : _subscribedTopics) {
            Serial.print("Re-subscribing to topic: ");
            Serial.println(topic);
            _mqttClient->subscribe(topic.c_str());
        }
        _reconnectInterval = 5000;
    }
}

bool MqttLteClient::publish(const char* topic, const char* payload, const uint8_t qos) {
    Serial.println("Publishing message...");
    if (!_mqttClient->connected()) {
        reconnect();
    }
    
    if (_mqttClient->connected()) {
        return _mqttClient->publish(topic, payload, qos);
    }
    
    return false;
}

bool MqttLteClient::subscribe(const char* topic) {
    if (!_mqttClient->connected()) {
        Serial.println("Reconnecting MQTT client before subscribing...");
        reconnect();
    }
    
    if (_mqttClient->connected()) {
        Serial.print("Subscribing to topic: ");
        Serial.println(topic);
        bool result = _mqttClient->subscribe(topic);
        
        if (result) {
            // Store the topic in our list of subscribed topics for re-subscription after reconnect
            _subscribedTopics.push_back(String(topic));
        }
        
        return result;
    }
    
    return false;
}

void MqttLteClient::loop() {
    static unsigned long lastConnectionCheck = 0;
    // Only check connection status periodically to avoid overwhelming the client
    if (millis() - lastConnectionCheck > 5000) {  // Check every 5 seconds
        lastConnectionCheck = millis();
        
        if (!_mqttClient->connected() && _networkConnected) {
            Serial.println("MQTT disconnected, reconnecting...");
            reconnect();
        }
    }
    // Always call the PubSubClient loop
    if (_mqttClient->connected()) {
        _mqttClient->loop();
    }
}

bool MqttLteClient::isConnected() {
    return _mqttClient->connected();
}

bool MqttLteClient::isNetworkConnected() {
    if (_modem) {
        _networkConnected = _modem->isGprsConnected();
    }
    return _networkConnected;
}

void MqttLteClient::setBufferSize(size_t size) {
    if (_mqttClient) {
        _mqttClient->setBufferSize(size);
    }
}
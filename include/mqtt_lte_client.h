#ifndef MQTT_LTE_CLIENT_H
#define MQTT_LTE_CLIENT_H

#include <Arduino.h>
#include "utilities.h"
#include "SSLClient.h"
#include <PubSubClient.h>

class MqttLteClient {
public:
    // Constructor with required pins
    MqttLteClient(HardwareSerial& modemSerial, int pwrKeyPin, int dtrPin, int flightPin, 
                 int txPin, int rxPin);
    
    // Initialize modem and network
    bool begin(const char* apn, const char* user = "", const char* pass = "", const char* pin = "");
    
    // Configure SSL/TLS
    void setCACert(const char* caCert);
    void setCertificate(const char* clientCert);
    void setPrivateKey(const char* privateKey);
    
    // MQTT functions
    void setCallback(void (*callback)(char*, byte*, unsigned int));
    bool connect(const char* broker, uint16_t port, const char* clientId);
    bool publish(const char* topic, const char* payload);
    bool subscribe(const char* topic);
    void loop();
    
    // Status checks
    bool isConnected();
    bool isNetworkConnected();
    
private:
    // Private methods
    void powerOnModem();
    bool testModemAT();
    void clearModemBuffer();
    bool initModemAndConnectNetwork();
    void reconnect();
    
    // References to hardware
    HardwareSerial& _modemSerial;
    int _pwrKeyPin;
    int _dtrPin;
    int _flightPin;
    int _txPin;
    int _rxPin;
    
    // Network configuration
    const char* _apn;
    const char* _user;
    const char* _pass;
    const char* _pin;
    
    // MQTT configuration
    const char* _broker;
    uint16_t _port;
    const char* _clientId;
    void (*_callback)(char*, byte*, unsigned int);
    
    // Components
    TinyGsm* _modem;
    TinyGsmClient* _gsmClient;
    SSLClient* _sslClient;
    PubSubClient* _mqttClient;
    
    // State tracking
    bool _initialized;
    bool _networkConnected;
    bool _mqttConnected;
};

#endif // MQTT_LTE_CLIENT_H
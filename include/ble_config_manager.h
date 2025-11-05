#ifndef BLE_CONFIG_MANAGER_H
#define BLE_CONFIG_MANAGER_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include "logger.h"

// BLE Service and Characteristic UUIDs
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define AUTH_CHAR_UUID         "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define MACHINE_NUM_CHAR_UUID  "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"
#define ENVIRONMENT_CHAR_UUID  "2d95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"
#define STATUS_CHAR_UUID       "d8de624e-140f-4a22-8594-e2216b84a5f2"

// Default master password (should be changed for production)
#define DEFAULT_MASTER_PASSWORD "fullwash2025"

// Preferences namespace
#define PREFS_NAMESPACE "fullwash"
#define PREFS_MACHINE_NUM "machine_num"
#define PREFS_ENVIRONMENT "environment"
#define PREFS_BLE_PASSWORD "ble_pwd"

// BLE Device name
#define BLE_DEVICE_NAME "FullWash Machine"

// Authentication timeout (2 minutes - 120 seconds)
#define AUTH_TIMEOUT_MS 120000

class BLEConfigManager : public BLEServerCallbacks, public BLECharacteristicCallbacks {
private:
    BLEServer* pServer;
    BLEService* pService;
    BLECharacteristic* pAuthCharacteristic;
    BLECharacteristic* pMachineNumCharacteristic;
    BLECharacteristic* pEnvironmentCharacteristic;
    BLECharacteristic* pStatusCharacteristic;
    
    Preferences preferences;
    
    bool deviceConnected;
    bool authenticated;
    unsigned long authTimestamp;
    String machineNumber;
    String environment;
    String masterPassword;
    
    // Callback handlers
    void onWrite(BLECharacteristic* pCharacteristic) override;
    void onConnect(BLEServer* pServer) override;
    void onDisconnect(BLEServer* pServer) override;
    
    // Helper methods
    bool verifyPassword(const String& password);
    void updateStatusCharacteristic(const String& status);
    void resetAuthentication();
    
public:
    BLEConfigManager();
    ~BLEConfigManager();
    
    // Initialize BLE and load stored configuration
    bool begin();
    
    // Check if authentication is still valid
    bool isAuthenticated();
    
    // Get current machine number
    String getMachineNumber();
    
    // Set machine number (requires authentication)
    bool setMachineNumber(const String& number);
    
    // Get current environment
    String getEnvironment();
    
    // Set environment (requires authentication)
    bool setEnvironment(const String& env);
    
    // Check if a device is connected
    bool isConnected();
    
    // Update method to check auth timeout
    void update();
    
    // Get/Set master password
    String getMasterPassword();
    void setMasterPassword(const String& password);
    
    // Reset to defaults
    void resetToDefaults();
    
    // Deinitialize BLE to free memory (useful when MQTT needs more heap)
    void deinit();
    bool isInitialized();
    
private:
    bool bleInitialized;
};

#endif // BLE_CONFIG_MANAGER_H


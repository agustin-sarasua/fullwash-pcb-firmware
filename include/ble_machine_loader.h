#ifndef BLE_MACHINE_LOADER_H
#define BLE_MACHINE_LOADER_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "logger.h"

// BLE Service UUID for Machine Loading
#define MACHINE_LOAD_SERVICE_UUID      "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define USER_ID_CHAR_UUID              "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define USER_NAME_CHAR_UUID            "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define TOKENS_CHAR_UUID               "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
#define LOAD_COMMAND_CHAR_UUID         "6e400005-b5a3-f393-e0a9-e50e24dcca9e"  // Format: "LOAD|authToken"
#define LOAD_STATUS_CHAR_UUID          "6e400006-b5a3-f393-e0a9-e50e24dcca9e"
#define MACHINE_STATE_CHAR_UUID        "6e400007-b5a3-f393-e0a9-e50e24dcca9e"

// BLE Authorization Secret (must match backend BLE_AUTH_SECRET)
// Default for development - MUST be changed in production
#define BLE_AUTH_SECRET "fullwash-ble-secret-2025-change-in-production"

// BLE Device name for machine loading
#define BLE_MACHINE_DEVICE_NAME "FullWash-"

// Forward declaration
class CarWashController;

// Machine loading data structure
struct MachineLoadData {
    String userId;
    String userName;
    int tokens;
    String authToken;  // Authorization token from backend
    unsigned long tokenReceivedTime;  // When token was received (millis())
    bool loadRequested;
    bool loadComplete;
    String errorMessage;
};

class BLEMachineLoader : public BLEServerCallbacks, public BLECharacteristicCallbacks {
private:
    BLEServer* pServer;
    BLEService* pService;
    BLECharacteristic* pUserIdCharacteristic;
    BLECharacteristic* pUserNameCharacteristic;
    BLECharacteristic* pTokensCharacteristic;
    BLECharacteristic* pLoadCommandCharacteristic;  // Format: "LOAD|authToken"
    BLECharacteristic* pLoadStatusCharacteristic;
    BLECharacteristic* pMachineStateCharacteristic;
    
    bool deviceConnected;
    bool bleInitialized;
    MachineLoadData loadData;
    CarWashController* controller;  // Reference to controller for loading machine
    String machineId;  // Machine ID for advertising name
    
    // Callback handlers
    void onWrite(BLECharacteristic* pCharacteristic) override;
    void onConnect(BLEServer* pServer) override;
    void onDisconnect(BLEServer* pServer) override;
    
    // Helper methods
    void updateLoadStatusCharacteristic(const String& status);
    void updateMachineStateCharacteristic(const String& state);
    void resetLoadData();
    void processLoadCommand();
    bool validateAuthToken(const String& token, const String& userId, const String& machineId, int tokens);
    
public:
    BLEMachineLoader();
    ~BLEMachineLoader();
    
    // Initialize BLE for machine loading
    bool begin(const String& machineId, CarWashController* ctrl);
    
    // Start/stop advertising
    void startAdvertising();
    void stopAdvertising();
    
    // Check if a device is connected
    bool isConnected();
    
    // Check if initialized
    bool isInitialized();
    
    // Deinitialize BLE to save memory
    void deinit();
    
    // Update method (call regularly)
    void update();
    
    // Check if load is complete
    bool isLoadComplete();
    
    // Get load data
    MachineLoadData getLoadData();
};

#endif // BLE_MACHINE_LOADER_H


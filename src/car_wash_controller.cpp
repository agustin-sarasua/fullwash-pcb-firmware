#include "car_wash_controller.h"


CarWashController::CarWashController(MqttLteClient& client)
    : mqttClient(client),
      currentState(STATE_FREE),
      lastActionTime(0),
      activeButton(-1),
      tokenStartTime(0),
      lastStatePublishTime(0),
      tokenTimeElapsed(0),
      pauseStartTime(0) {

    pinMode(LED_PIN_INIT, OUTPUT);
    digitalWrite(LED_PIN_INIT, LOW);

    pinMode(RUNNING_LED_PIN, OUTPUT);
    digitalWrite(RUNNING_LED_PIN, LOW);

    for (int i = 0; i < NUM_BUTTONS; i++) {
        pinMode(BUTTON_PINS[i], INPUT_PULLUP);
        lastDebounceTime[i] = 0;
        lastButtonState[i] = HIGH;
    }

    pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);
    lastDebounceTime[NUM_BUTTONS] = 0;
    lastButtonState[NUM_BUTTONS] = HIGH;

    config.isLoaded = false;
}

void CarWashController::handleMqttMessage(const char* topic, const uint8_t* payload, unsigned len) {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload, len);
    if (error) {
        Serial.println("Failed to parse JSON");
        return;
    }
    if (String(topic) == INIT_TOPIC) {
        config.sessionId = doc["session_id"].as<String>();
        config.userId = doc["user_id"].as<String>();
        config.userName = doc["user_name"].as<String>();
        config.tokens = doc["tokens"].as<int>();
        config.timestamp = doc["timestamp"].as<String>();
        config.timestampMillis = millis();
        config.isLoaded = true;
        currentState = STATE_IDLE;
        lastActionTime = millis();
        Serial.println("Switching on LED");
        digitalWrite(LED_PIN_INIT, HIGH);
        Serial.println("Machine loaded with new configuration");
    } else if (String(topic) == CONFIG_TOPIC) {
        Serial.print("Updating machine configuration timestamp: ");
        Serial.println(doc["timestamp"].as<String>());
        config.timestamp = doc["timestamp"].as<String>();
        config.timestampMillis = millis();
        config.sessionId = "";
        config.userId = "";
        config.userName = "";
        config.tokens = 0;
        config.isLoaded = false;
    } else {
        Serial.print("Unknown topic: ");
        Serial.println(String(topic));
    }
}

void CarWashController::handleButtons() {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        int reading = digitalRead(BUTTON_PINS[i]);
        if (reading != lastButtonState[i]) {
            lastDebounceTime[i] = millis();
        }
        if ((millis() - lastDebounceTime[i]) > DEBOUNCE_DELAY) {
            if (reading == LOW && config.isLoaded) {
                Serial.print("Button pressed: ");
                Serial.println(String(i));
                if (currentState == STATE_IDLE) {
                    activateButton(i, MANUAL);
                } else if (currentState == STATE_RUNNING && i == activeButton) {
                    pauseMachine();
                } else if (currentState == STATE_PAUSED) {
                    resumeMachine(i);
                }
            }
        }
        lastButtonState[i] = reading;
    }

    int stopReading = digitalRead(STOP_BUTTON_PIN);
    if (stopReading != lastButtonState[NUM_BUTTONS]) {
        lastDebounceTime[NUM_BUTTONS] = millis();
    }
    if ((millis() - lastDebounceTime[NUM_BUTTONS]) > DEBOUNCE_DELAY) {
        if (stopReading == LOW && currentState == STATE_RUNNING) {
            stopMachine(MANUAL);
        }
    }
    lastButtonState[NUM_BUTTONS] = stopReading;
}

void CarWashController::pauseMachine() {
    Serial.println("Pausing machine");
    if (activeButton >= 0) {
        // digitalWrite(RELAY_PINS[activeButton], LOW);
    }
    currentState = STATE_PAUSED;
    lastActionTime = millis();
    pauseStartTime = millis();
    tokenTimeElapsed += (pauseStartTime - tokenStartTime);
    digitalWrite(RUNNING_LED_PIN, LOW);
}

void CarWashController::resumeMachine(int buttonIndex) {
    Serial.println("Resuming machine");
    activeButton = buttonIndex;
    currentState = STATE_RUNNING;
    lastActionTime = millis();
    tokenStartTime = millis();
    digitalWrite(RUNNING_LED_PIN, HIGH);
}

void CarWashController::stopMachine(TriggerType triggerType) {
    if (activeButton >= 0) {
        // digitalWrite(RELAY_PINS[activeButton], LOW);
    }
    
    config.isLoaded = false;
    currentState = STATE_FREE;
    digitalWrite(LED_PIN_INIT, LOW);
    activeButton = -1;
    tokenStartTime = 0;
    tokenTimeElapsed = 0;
    pauseStartTime = 0;
}

void CarWashController::activateButton(int buttonIndex, TriggerType triggerType) {
    Serial.print("Activating button: ");
    Serial.println(String(BUTTON_PINS[buttonIndex]));

    if (config.tokens <= 0) return;

    digitalWrite(RUNNING_LED_PIN, HIGH);
    currentState = STATE_RUNNING;
    activeButton = buttonIndex;
    // digitalWrite(RELAY_PINS[buttonIndex], HIGH);
    lastActionTime = millis();
    tokenStartTime = millis();
    tokenTimeElapsed = 0;
    config.tokens--;

    publishActionEvent(buttonIndex, ACTION_START, triggerType);
}

void CarWashController::tokenExpired() {
    if (activeButton >= 0) {
        // digitalWrite(RELAY_PINS[activeButton], LOW);
    }
    activeButton = -1;
    currentState = STATE_IDLE;
    lastActionTime = millis();
    digitalWrite(RUNNING_LED_PIN, LOW);
    // publishTokenExpiredEvent();
}

void CarWashController::update() {
    if (config.timestamp == "") {
        return;
    }
    handleButtons();
    unsigned long currentTime = millis();
    publishPeriodicState();
    if (currentState == STATE_RUNNING) {
        unsigned long totalElapsedTime = tokenTimeElapsed + (currentTime - tokenStartTime);
        if (totalElapsedTime >= TOKEN_TIME) {
            Serial.println("Token expired");
            tokenExpired();
            return;
        }
    }
    if (currentTime - lastActionTime > USER_INACTIVE_TIMEOUT && currentState != STATE_FREE) {
        Serial.println("Stopping machine due to user inactivity");
        stopMachine(AUTOMATIC);
    }
}

void CarWashController::publishMachineSetupActionEvent() {
    Serial.println("Publishing Machine Setup Action Event");

    StaticJsonDocument<512> doc;
    doc["machine_id"] = MACHINE_ID;
    doc["action"] = getMachineActionString(ACTION_SETUP);
    doc["timestamp"] = getTimestamp();
    
    String jsonString;
    serializeJson(doc, jsonString);
    mqttClient.publish(ACTION_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE);
}

unsigned long CarWashController::getSecondsLeft() {
    if (currentState != STATE_RUNNING && currentState != STATE_PAUSED) {
        return 0;
    }

    unsigned long currentTime = millis();
    unsigned long totalElapsedTime;

    if (currentState == STATE_RUNNING) {
        totalElapsedTime = tokenTimeElapsed + (currentTime - tokenStartTime);
    } else { // PAUSED
        totalElapsedTime = tokenTimeElapsed;
    }

    if (totalElapsedTime >= TOKEN_TIME) {
        return 0;
    }

    return (TOKEN_TIME - totalElapsedTime) / 1000;
}

String CarWashController::getTimestamp() {
    if (config.timestamp == "")
        return "";

    int year, month, day, hour, minute, second;
    float fractional_seconds;

    sscanf(config.timestamp.c_str(), "%d-%d-%dT%d:%d:%fZ",
           &year, &month, &day, &hour, &minute, &fractional_seconds);
    second = static_cast<int>(fractional_seconds);

    tmElements_t tm;
    tm.Year = year - 1970;
    tm.Month = month;
    tm.Day = day;
    tm.Hour = hour;
    tm.Minute = minute;
    tm.Second = second;

    time_t serverEpoch = makeTime(tm);

    unsigned long millisOffset = millis() - config.timestampMillis;
    time_t adjustedTime = serverEpoch + (millisOffset / 1000);
    int extraMilliseconds = millisOffset % 1000;

    tmElements_t adjustedTm;
    breakTime(adjustedTime, adjustedTm);

    char isoTimestamp[30];
    sprintf(isoTimestamp, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
            adjustedTm.Year + 1970, adjustedTm.Month, adjustedTm.Day,
            adjustedTm.Hour, adjustedTm.Minute, adjustedTm.Second, extraMilliseconds);

    return String(isoTimestamp);
}

void CarWashController::publishActionEvent(int buttonIndex, MachineAction machineAction, TriggerType triggerType) {
    if (!config.isLoaded) return;

    StaticJsonDocument<512> doc;

    doc["machine_id"] = MACHINE_ID;
    doc["timestamp"] = getTimestamp();
    doc["action"] = getMachineActionString(machineAction);
    doc["trigger_type"] = (triggerType == MANUAL) ? "MANUAL" : "AUTOMATIC";
    doc["button_name"] = String("BUTTON_") + String(buttonIndex + 1);
    doc["session_id"] = config.sessionId;
    doc["user_id"] = config.userId;
    doc["token_channel"] = "PHYSICAL";
    doc["tokens_left"] = config.tokens;

    if (currentState == STATE_RUNNING) {
        doc["seconds_left"] = getSecondsLeft();
    }

    String jsonString;
    serializeJson(doc, jsonString);

    mqttClient.publish(ACTION_TOPIC.c_str(), jsonString.c_str(), QOS1_AT_LEAST_ONCE);
}

void CarWashController::publishPeriodicState(bool force) {
    if (force || millis() - lastStatePublishTime >= STATE_PUBLISH_INTERVAL) {
        Serial.println("Publishing Periodic State");
        StaticJsonDocument<512> doc;
        doc["machine_id"] = MACHINE_ID;
        doc["timestamp"] = getTimestamp();
        doc["status"] = getMachineStateString(currentState);
        Serial.print("Status: ");
        Serial.println(doc["status"].as<String>());

        if (config.isLoaded) {
            JsonObject sessionMetadata = doc.createNestedObject("session_metadata");
            sessionMetadata["session_id"] = config.sessionId;
            sessionMetadata["user_id"] = config.userId;
            sessionMetadata["user_name"] = config.userName;
            sessionMetadata["tokens_left"] = config.tokens;
            sessionMetadata["timestamp"] = config.timestamp;
            if (tokenStartTime > 0) {
                sessionMetadata["seconds_left"] = getSecondsLeft();
            }
        }

        String jsonString;
        serializeJson(doc, jsonString);
        mqttClient.publish(STATE_TOPIC.c_str(), jsonString.c_str(), QOS0_AT_MOST_ONCE);

        lastStatePublishTime = millis();
    }
}
# State Machine Analysis

## State Definitions

The machine has 4 states:
1. **STATE_FREE**: Machine is not loaded, no user session, cannot accept button presses
2. **STATE_IDLE**: Machine is loaded with tokens, ready to start washing, waiting for user input
3. **STATE_RUNNING**: Machine is actively washing, relay is ON, timer is counting down
4. **STATE_PAUSED**: Machine was paused (user pressed same button while running), relay is OFF, timer is paused

## State Transitions

### 1. STATE_FREE → STATE_IDLE

**Trigger**: Machine configuration loaded via MQTT `init` topic

**Location**: `CarWashController::handleMqttMessage()` line 83-115

**Conditions**:
- Receives MQTT message on `INIT_TOPIC`
- Parses JSON with: `session_id`, `user_id`, `user_name`, `tokens`, `timestamp`
- Sets `config.isLoaded = true`
- Sets `currentState = STATE_IDLE`
- Updates `lastActionTime`

**Alternative**: Physical coin insertion (line 719-722)
- If `config.isLoaded == false` and coin is inserted
- Creates manual session with 1 token
- Sets `config.isLoaded = true`
- Sets `currentState = STATE_IDLE`

```cpp
// Line 110-111
config.isLoaded = true;
currentState = STATE_IDLE;
```

---

### 2. STATE_IDLE → STATE_RUNNING

**Trigger**: User presses a function button (1-5)

**Location**: `CarWashController::handleButtons()` → `activateButton()` line 437-494

**Conditions**:
- `config.isLoaded == true` ✅
- `currentState == STATE_IDLE` ✅
- `config.tokens > 0` ✅ (checked in `activateButton()` line 440)

**Actions**:
- Sets `currentState = STATE_RUNNING`
- Sets `activeButton = buttonIndex`
- Turns ON relay for selected button
- Decrements `config.tokens` (line 491)
- Decrements `config.physicalTokens` if > 0 (line 488-490)
- Starts timer: `tokenStartTime = millis()`
- Publishes ACTION_START event

```cpp
// Line 440-448
if (config.tokens <= 0) {
    LOG_WARNING("Cannot activate button %d - no tokens left");
    return;
}
currentState = STATE_RUNNING;
activeButton = buttonIndex;
```

---

### 3. STATE_RUNNING → STATE_PAUSED

**Trigger**: User presses the same button that is currently running

**Location**: `CarWashController::handleButtons()` line 168-170

**Conditions**:
- `config.isLoaded == true` ✅
- `currentState == STATE_RUNNING` ✅
- `detectedId == activeButton` ✅ (same button)

**Actions**:
- Sets `currentState = STATE_PAUSED`
- Turns OFF relay
- Calculates elapsed time: `tokenTimeElapsed += (pauseStartTime - tokenStartTime)`
- Sets `pauseStartTime = millis()`
- Publishes ACTION_PAUSE event

```cpp
// Line 340-342
currentState = STATE_PAUSED;
pauseStartTime = millis();
tokenTimeElapsed += (pauseStartTime - tokenStartTime);
```

---

### 4. STATE_PAUSED → STATE_RUNNING

**Trigger**: User presses any function button while paused

**Location**: `CarWashController::handleButtons()` → `resumeMachine()` line 349-388

**Conditions**:
- `config.isLoaded == true` ✅
- `currentState == STATE_PAUSED` ✅

**Actions**:
- Sets `currentState = STATE_RUNNING`
- Sets `activeButton = buttonIndex` (can be different button)
- Turns ON relay for selected button
- Resets timer: `tokenStartTime = millis()` (elapsed time already saved)
- Publishes ACTION_RESUME event

```cpp
// Line 382-384
currentState = STATE_RUNNING;
lastActionTime = millis();
tokenStartTime = millis();
```

---

### 5. STATE_RUNNING → STATE_IDLE

**Trigger**: Token expires (timer runs out)

**Location**: `CarWashController::update()` → `tokenExpired()` line 496-511

**Conditions**:
- `currentState == STATE_RUNNING` ✅
- `totalElapsedTime >= TOKEN_TIME` ✅ (checked in `update()` line 746-750)

**Actions**:
- Turns OFF relay
- Sets `activeButton = -1`
- Sets `currentState = STATE_IDLE`
- Updates `lastActionTime`

**Note**: Token is already decremented when entering STATE_RUNNING, so this is just cleanup.

```cpp
// Line 508-509
currentState = STATE_IDLE;
lastActionTime = millis();
```

---

### 6. ANY STATE → STATE_FREE

**Trigger**: 
1. User presses STOP button (BUTTON 6)
2. User inactivity timeout
3. (Theoretically any stop condition)

**Location**: `CarWashController::stopMachine()` line 390-435

**Conditions**:
- STOP button: `config.isLoaded == true` (line 181-182)
- Inactivity timeout: `currentState != STATE_FREE` AND `currentTime - lastActionTime > USER_INACTIVE_TIMEOUT` (line 754-756)

**Actions**:
- Turns OFF relay if `activeButton >= 0`
- Sets `config.isLoaded = false` ⚠️
- Sets `currentState = STATE_FREE`
- Resets: `activeButton = -1`, `tokenStartTime = 0`, `tokenTimeElapsed = 0`, `pauseStartTime = 0`
- Publishes ACTION_STOP event

```cpp
// Line 424-425
config.isLoaded = false;
currentState = STATE_FREE;
```

---

## Button Handling Logic

### Current Implementation

**Location**: `CarWashController::handleButtons()` line 149-204

**Flow**:
1. Check if button flag is set (from ButtonDetector task)
2. If flag exists:
   - Get button ID
   - **Clear flag** (after fix: only if processed)
   - Check `config.isLoaded`
   - If loaded, check state and process
   - If not loaded, keep flag for retry

3. If no flag, do raw polling (fallback)

### Button Processing by State

**STATE_FREE**:
- ❌ Buttons should be ignored
- Current: Flag is kept if `config.isLoaded == false` (correct)
- Issue: **No explicit STATE_FREE check** - relies on `config.isLoaded`

**STATE_IDLE**:
- ✅ Buttons 1-5: Activate button → STATE_RUNNING
- ✅ Button 6 (STOP): Ignored (already FREE/IDLE)
- Condition: `currentState == STATE_IDLE` (line 165)

**STATE_RUNNING**:
- ✅ Same button: Pause → STATE_PAUSED
- ❌ Different button: Ignored (line 172-173)
- ✅ Button 6 (STOP): Stop → STATE_FREE

**STATE_PAUSED**:
- ✅ Any button 1-5: Resume → STATE_RUNNING
- ✅ Button 6 (STOP): Stop → STATE_FREE

---

## Issues Found

### 1. Missing STATE_FREE Check in Button Handling

**Problem**: The code relies on `config.isLoaded` to determine if buttons should work, but there's no explicit check for `currentState == STATE_FREE`.

**Current Code** (line 162-177):
```cpp
if (config.isLoaded) {
    // Process button based on state
} else {
    // Keep flag, don't process
}
```

**Issue**: If somehow `config.isLoaded == true` but `currentState == STATE_FREE`, buttons would still be processed (though this shouldn't happen in normal flow).

**Recommendation**: Add explicit STATE_FREE check:
```cpp
if (currentState == STATE_FREE) {
    LOG_WARNING("Button press ignored - machine is FREE");
    ioExpander.clearButtonFlag();
    return;
}
```

### 2. State Consistency

**Observation**: `stopMachine()` always sets both:
- `config.isLoaded = false`
- `currentState = STATE_FREE`

So STATE_FREE and `!config.isLoaded` are always together. However, for safety, explicit state checks are better.

---

## Summary

### Valid State Transitions:
- ✅ FREE → IDLE (via MQTT init or coin)
- ✅ IDLE → RUNNING (button press with tokens)
- ✅ RUNNING → PAUSED (same button press)
- ✅ PAUSED → RUNNING (any button press)
- ✅ RUNNING → IDLE (token expires)
- ✅ ANY → FREE (stop button or timeout)

### Button Action Requirements:
- ✅ Machine must be loaded (`config.isLoaded == true`)
- ✅ Machine must be in IDLE state to start washing
- ✅ Must have tokens (`config.tokens > 0`)
- ⚠️ Missing: Explicit STATE_FREE check (relies on `config.isLoaded`)

### Current Fix Status:
✅ Fixed: Button flag is now preserved when `config.isLoaded == false` and processed when machine becomes ready
✅ Fixed: Added explicit STATE_FREE check to ensure buttons are ignored when machine is FREE

---

## State Transition Diagram

```
                    [MQTT INIT Topic]
                    OR Coin Insertion
                    + config.isLoaded = true
                    + tokens > 0
                          │
                          ▼
                  ┌───────────────┐
                  │  STATE_FREE   │
                  │               │
                  │ config.isLoaded│
                  │    = false    │
                  │               │
                  │ Buttons: ❌   │
                  └───────────────┘
                          │
                    [Config Loaded]
                          │
                          ▼
                  ┌───────────────┐
                  │  STATE_IDLE   │
                  │               │
                  │ config.isLoaded│
                  │    = true     │
                  │ tokens > 0    │
                  │               │
                  │ Buttons: ✅   │
                  │ (Start wash)  │
                  └───────────────┘
                          │
                    [Button Press]
                    + tokens > 0
                          │
                          ▼
                  ┌───────────────┐
                  │ STATE_RUNNING │
                  │               │
                  │ Relay: ON     │
                  │ Timer: Active │
                  │               │
                  │ Same button:  │
                  │   → PAUSED    │
                  │               │
                  │ Token expires:│
                  │   → IDLE      │
                  └───────────────┘
                          │
                    [Same Button]
                    OR
                    [Token Expires]
                          │
        ┌─────────────────┴─────────────────┐
        │                                   │
        ▼                                   ▼
┌───────────────┐                  ┌───────────────┐
│ STATE_PAUSED  │                  │  STATE_IDLE   │
│               │                  │               │
│ Relay: OFF    │                  │ (Ready again) │
│ Timer: Paused │                  │               │
│               │                  │ Buttons: ✅   │
│ Any button:   │                  └───────────────┘
│   → RUNNING   │
└───────────────┘
        │
  [Any Button]
        │
        ▼
┌───────────────┐
│ STATE_RUNNING │
└───────────────┘

         [STOP Button]
         OR
         [Inactivity Timeout]
                  │
                  ▼
         ┌───────────────┐
         │  STATE_FREE   │
         │               │
         │ config.isLoaded│
         │    = false    │
         │               │
         │ Buttons: ❌   │
         └───────────────┘
```

---

## Button Action Matrix

| State     | Config Loaded | Tokens | Button 1-5 | Button 6 (STOP) | Result                      |
|-----------|---------------|--------|------------|-----------------|-----------------------------|
| FREE      | false         | 0      | ❌ Ignore  | ❌ Ignore       | No action                   |
| FREE      | true          | any    | ❌ Ignore*  | ✅ Stop         | → FREE (theoretical case)   |
| IDLE      | true          | > 0    | ✅ Start   | ✅ Stop         | → RUNNING or → FREE         |
| IDLE      | true          | 0      | ❌ Ignore  | ✅ Stop         | Cannot start (no tokens)    |
| RUNNING   | true          | any    | ⚠️ Same→Pause<br>❌ Diff→Ignore | ✅ Stop | → PAUSED or → FREE |
| PAUSED    | true          | any    | ✅ Resume  | ✅ Stop         | → RUNNING or → FREE          |

\* Should never happen in normal flow, but explicitly checked for safety

---

## Implementation Details

### Button Handling Order (Priority):

1. **Early STATE_FREE Check** (NEW - Line 161-166)
   - If `currentState == STATE_FREE`, ignore button immediately
   - Clear flag and return
   - Prevents any processing when machine is FREE

2. **Config Loaded Check**
   - If `config.isLoaded == false`, keep flag for retry
   - Return early (don't clear flag)

3. **State-Specific Processing**
   - IDLE: Activate button (if tokens > 0)
   - RUNNING: Pause if same button, ignore if different
   - PAUSED: Resume with any button

4. **Flag Clearing**
   - Only clear flag if button was processed
   - Prevents losing button presses during initialization

### Key Safety Features:

✅ **Explicit STATE_FREE check** - Buttons cannot work when FREE
✅ **Config loaded check** - Buttons only work when machine has tokens
✅ **Token validation** - `activateButton()` checks `tokens > 0` before activation
✅ **State validation** - Buttons only work in IDLE, RUNNING, or PAUSED states
✅ **Flag preservation** - Button presses during initialization are preserved


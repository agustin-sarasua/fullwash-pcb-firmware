# Timeout Display Fix

## Issue
The countdown for timeout on the screen shows a value (e.g., "00:15" or "00:00"), but the actual logout happens much later. The screen remains at "00:00" for a while after it reaches zero before the user is actually logged out.

## Root Causes

### 1. Timing Mismatch Between Display and Logout
- **Display calculation**: `getTimeToInactivityTimeout()` returns 0 when `elapsedTime >= USER_INACTIVE_TIMEOUT`
- **Logout check**: `update()` only triggered logout when `elapsedTime > USER_INACTIVE_TIMEOUT` (using `>` instead of `>=`)
- **Result**: Display showed "00:00" but logout didn't happen until elapsed > timeout, creating a gap

### 2. Slow Display Refresh Rate
- Display only updated every 1 second normally
- When countdown reached 0, display might not update immediately
- Even after logout triggered, display could take up to 1 second to refresh

### 3. Controller Update Interval
- Controller `update()` runs every 50ms
- Even with `>=` check, there's a small delay (up to 50ms) between when condition becomes true and when it's checked

## Fixes Applied

### Fix 1: Synchronized Timeout Check ✅
**File**: `src/car_wash_controller.cpp` line 774-779

Changed from `>` to `>=` to match the display calculation:
```cpp
// Before:
if (currentTime - lastActionTime > USER_INACTIVE_TIMEOUT && currentState != STATE_FREE) {

// After:
if (currentTime - lastActionTime >= USER_INACTIVE_TIMEOUT && currentState != STATE_FREE) {
```

This ensures logout happens exactly when countdown reaches 0, not after.

### Fix 2: Faster Display Updates Near Timeout ✅
**File**: `src/display_manager.cpp` line 40-50

Added logic to update display more frequently when countdown is <= 5 seconds:
```cpp
// Check if we're near timeout - update more frequently for accurate countdown
unsigned long timeToTimeout = controller->getTimeToInactivityTimeout();
unsigned long timeToTimeoutSeconds = timeToTimeout / 1000;
bool nearTimeout = (timeToTimeoutSeconds > 0 && timeToTimeoutSeconds <= 5);

// Update more frequently (every 500ms) when near timeout
unsigned long updateInterval = nearTimeout ? 500 : 1000;
```

This ensures:
- Display updates every 500ms when countdown <= 5 seconds
- Display updates every 1 second otherwise
- Prevents stale "00:00" display

### Fix 3: Improved Documentation ✅
**File**: `include/constants.h` line 22-26

Added comments explaining the timeout value:
```cpp
// NOTE: These values are divided by 6 for testing/debugging (20 seconds instead of 2 minutes)
// For production, remove "/ 6" to get 120000 ms = 2 minutes
const unsigned long USER_INACTIVE_TIMEOUT = 120000 / 6; // Currently 20 seconds
```

## Current Timeout Configuration

- **USER_INACTIVE_TIMEOUT**: `120000 / 6 = 20000 ms = 20 seconds`
- **Note**: The `/ 6` appears to be for testing. For production (2 minutes), remove `/ 6`

## Expected Behavior After Fix

1. **Countdown Display**: Shows accurate remaining time (e.g., "00:20", "00:19", ..., "00:01", "00:00")
2. **At Zero**: Display shows "00:00" and logout happens immediately (within 50ms)
3. **State Transition**: Machine transitions to FREE state and display updates immediately (within 500ms)
4. **No Stale Display**: Display won't show "00:00" for extended periods

## Testing

To verify the fix:
1. Load machine with tokens (IDLE state)
2. Wait and observe countdown (should count down from 20 seconds)
3. When countdown reaches "00:00", logout should happen immediately
4. Display should update to FREE state within 500ms
5. Should NOT see "00:00" displayed for more than ~50ms after timeout

## Timeline

- **0-15 seconds**: Display updates every 1 second
- **15-20 seconds**: Display updates every 500ms (near timeout)
- **At 20 seconds**: `getTimeToInactivityTimeout()` returns 0, display shows "00:00"
- **At 20 seconds**: `update()` detects `elapsed >= timeout`, triggers logout
- **Within 50ms**: State changes to FREE
- **Within 500ms**: Display updates to show FREE state

## Related Files

- `src/car_wash_controller.cpp` - Timeout check and calculation
- `src/display_manager.cpp` - Display refresh logic
- `include/constants.h` - Timeout constant definition


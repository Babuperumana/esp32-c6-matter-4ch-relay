/*
 * =====================================================================================
 * Project: ESP32-C6 4-Channel Matter-over-Wi-Fi Smart Relay Board
 * Framework: Arduino ESP32 Core v3.0.x or higher
 * Author: Expert IoT Engineer
 * 
 * Target Board: ESP32-C6 DevKit (e.g., ESP32-C6-DevKitC-1)
 * Features:
 *   - 4 Independent Matter On/Off Plugin Endpoints (Switch / Outlet)
 *   - Native Wi-Fi Matter commissioning (BLE Provisioning -> Wi-Fi IPv6)
 *   - 4 Physical Momentary Push Buttons with non-blocking software debouncing
 *   - Power-outage state persistence via ESP32 NVS (Preferences)
 *   - Long-press Factory Reset / Decommissioning via BOOT button (GPIO 9)
 *   - Direct integration with Home Assistant, Amazon Alexa, Google Home, Apple Home
 * =====================================================================================
 */

#include <Arduino.h>
#include <Matter.h>
#include <Preferences.h>

// ==========================================
// 1. HARDWARE PIN DEFINITIONS (ESP32-C6 Safe)
// ==========================================
// Relay Outputs (Active LOW by default for standard optocoupled relay modules)
const uint8_t RELAY_PINS[4] = {18, 19, 20, 21};

// Push Button Inputs (Connected between GPIO and GND, using internal pull-up)
const uint8_t BUTTON_PINS[4] = {4, 5, 6, 7};

// Factory Reset / Commissioning Reset Button (DevKit BOOT button)
const uint8_t RESET_BUTTON_PIN = 9;

// Relay Logic Configuration:
// Most 4-ch 5V relay modules are Active-LOW (LOW = Relay ON, HIGH = Relay OFF).
// Change to HIGH if your relay module is Active-HIGH.
const uint8_t RELAY_ACTIVE_LEVEL = LOW;

// ==========================================
// 2. MATTER ENDPOINT & STATE OBJECTS
// ==========================================
// 4 Independent Matter On/Off Plugin Endpoints
MatterOnOffPlugin plugin1;
MatterOnOffPlugin plugin2;
MatterOnOffPlugin plugin3;
MatterOnOffPlugin plugin4;

MatterOnOffPlugin* plugins[4] = {&plugin1, &plugin2, &plugin3, &plugin4};

// State storage
bool relayStates[4] = {false, false, false, false};

// Button debouncing state machine
uint8_t lastButtonReadings[4] = {HIGH, HIGH, HIGH, HIGH};
uint8_t debouncedButtonStates[4] = {HIGH, HIGH, HIGH, HIGH};
unsigned long lastDebounceTime[4] = {0, 0, 0, 0};
const unsigned long DEBOUNCE_DELAY_MS = 50;

// Factory reset button tracking
unsigned long resetPressStartTime = 0;
bool resetButtonPressed = false;
const unsigned long FACTORY_RESET_HOLD_MS = 6000; // Hold for 6s to decommission

// NVS Persistence
Preferences prefs;
const char* NVS_NAMESPACE = "relay_cfg";

// Forward declarations
void applyRelayState(uint8_t index, bool state, bool syncToMatter);
void saveStateToNVS(uint8_t index, bool state);
void loadStatesFromNVS();

// ==========================================
// 3. MATTER CONTROLLER CALLBACKS
// ==========================================
// Triggered when an ecosystem controller (Alexa, Google Home, HA) updates a state
bool onPlugin1Change(bool newState) {
  Serial.printf("[Matter] Remote command -> Endpoint 1: %s\n", newState ? "ON" : "OFF");
  applyRelayState(0, newState, false); // false = already updated by Matter
  return true;
}

bool onPlugin2Change(bool newState) {
  Serial.printf("[Matter] Remote command -> Endpoint 2: %s\n", newState ? "ON" : "OFF");
  applyRelayState(1, newState, false);
  return true;
}

bool onPlugin3Change(bool newState) {
  Serial.printf("[Matter] Remote command -> Endpoint 3: %s\n", newState ? "ON" : "OFF");
  applyRelayState(2, newState, false);
  return true;
}

bool onPlugin4Change(bool newState) {
  Serial.printf("[Matter] Remote command -> Endpoint 4: %s\n", newState ? "ON" : "OFF");
  applyRelayState(3, newState, false);
  return true;
}

// ==========================================
// 4. RELAY CONTROL & STATE PERSISTENCE
// ==========================================
void applyRelayState(uint8_t index, bool state, bool syncToMatter) {
  if (index >= 4) return;

  relayStates[index] = state;

  // Drive physical hardware GPIO
  uint8_t pinLevel = (state ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
  digitalWrite(RELAY_PINS[index], pinLevel);

  // If triggered locally by physical button, inform the Matter fabric
  if (syncToMatter && plugins[index] != nullptr) {
    plugins[index]->setOnOff(state);
  }

  // Persist to NVS so the relay returns to this state after a power outage
  saveStateToNVS(index, state);

  Serial.printf("[Hardware] Relay %d set to %s (GPIO %d = %s)\n", 
                index + 1, state ? "ON" : "OFF", RELAY_PINS[index], pinLevel == HIGH ? "HIGH" : "LOW");
}

void saveStateToNVS(uint8_t index, bool state) {
  char key[8];
  snprintf(key, sizeof(key), "ch%d", index);
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBool(key, state);
  prefs.end();
}

void loadStatesFromNVS() {
  prefs.begin(NVS_NAMESPACE, true);
  for (uint8_t i = 0; i < 4; i++) {
    char key[8];
    snprintf(key, sizeof(key), "ch%d", i);
    relayStates[i] = prefs.getBool(key, false); // default to OFF
  }
  prefs.end();
}

// ==========================================
// 5. SETUP FUNCTION
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000); // Allow USB-Serial connection to stabilize

  Serial.println("\n========================================================");
  Serial.println("   ESP32-C6 4-Channel Matter-over-Wi-Fi Relay Board    ");
  Serial.println("========================================================");

  // Load previous states from NVS
  loadStatesFromNVS();

  // Initialize Relay GPIOs (Set initial level BEFORE setting pinMode to avoid startup clicking)
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t initialLevel = (relayStates[i] ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
    digitalWrite(RELAY_PINS[i], initialLevel);
    pinMode(RELAY_PINS[i], OUTPUT);
    Serial.printf("Configured Relay %d: GPIO %d (Initial State: %s)\n", 
                  i + 1, RELAY_PINS[i], relayStates[i] ? "ON" : "OFF");
  }

  // Initialize Physical Push Buttons with Internal Pull-Up Resistors
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    Serial.printf("Configured Button %d: GPIO %d\n", i + 1, BUTTON_PINS[i]);
  }

  // Initialize BOOT Button for Factory Reset
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  // Register Matter Remote Callback Listeners
  plugin1.onChange(onPlugin1Change);
  plugin2.onChange(onPlugin2Change);
  plugin3.onChange(onPlugin3Change);
  plugin4.onChange(onPlugin4Change);

  // Initialize individual Matter Endpoints with their recovered states
  Serial.println("\nInitializing Matter Endpoints...");
  plugin1.begin(relayStates[0]);
  plugin2.begin(relayStates[1]);
  plugin3.begin(relayStates[2]);
  plugin4.begin(relayStates[3]);

  // Start the Core Matter Protocol Stack
  Serial.println("Starting Matter Protocol Stack...");
  Matter.begin();

  // Display Commissioning Credentials
  Serial.println("\n---------------- MATTER ONBOARDING INFO ----------------");
  if (!Matter.isDeviceCommissioned()) {
    Serial.println("Status: >> NOT COMMISSIONED << (Ready for Pairing)");
    Serial.println();
    Serial.print("Manual 11-digit Pairing Code: ");
    Serial.println(Matter.getManualPairingCode());
    Serial.println();
    Serial.println("Scan QR Code via setup link (open in browser):");
    Serial.println(Matter.getOnboardingQRCodeUrl());
    Serial.println("--------------------------------------------------------\n");
    Serial.println("Open the Alexa, Google Home, or Home Assistant app,");
    Serial.println("tap 'Add Device' -> 'Matter', and scan the QR code.");
  } else {
    Serial.println("Status: >> ALREADY COMMISSIONED <<");
    Serial.println("Connected to Matter Fabric(s) and operational.");
    Serial.println("To re-commission: Hold BOOT button (GPIO 9) for 6 seconds.");
    Serial.println("--------------------------------------------------------\n");
  }
}

// ==========================================
// 6. MAIN LOOP (Local Button & Reset Handling)
// ==========================================
void loop() {
  unsigned long currentMillis = millis();

  // ----------------------------------------------------
  // A. Scan and Debounce 4 Physical Push Buttons
  // ----------------------------------------------------
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t rawReading = digitalRead(BUTTON_PINS[i]);

    if (rawReading != lastButtonReadings[i]) {
      lastDebounceTime[i] = currentMillis; // Reset debounce timer
    }

    if ((currentMillis - lastDebounceTime[i]) > DEBOUNCE_DELAY_MS) {
      // If the state has been stable longer than debounce threshold
      if (rawReading != debouncedButtonStates[i]) {
        debouncedButtonStates[i] = rawReading;

        // Trigger action on Falling Edge (Button Press -> LOW)
        if (debouncedButtonStates[i] == LOW) {
          bool newToggleState = !relayStates[i];
          Serial.printf("\n[Button %d Pressed] Toggling Channel %d -> %s\n", 
                        i + 1, i + 1, newToggleState ? "ON" : "OFF");
          applyRelayState(i, newToggleState, true); // true = sync back to Matter
        }
      }
    }
    lastButtonReadings[i] = rawReading;
  }

  // ----------------------------------------------------
  // B. Monitor Factory Reset / Decommissioning Button
  // ----------------------------------------------------
  uint8_t resetReading = digitalRead(RESET_BUTTON_PIN);
  if (resetReading == LOW) {
    if (!resetButtonPressed) {
      resetButtonPressed = true;
      resetPressStartTime = currentMillis;
      Serial.println("\n[Warning] BOOT button held down. Keep holding for 6s to Factory Reset...");
    } else {
      if ((currentMillis - resetPressStartTime) >= FACTORY_RESET_HOLD_MS) {
        Serial.println("\n========================================================");
        Serial.println(" [FACTORY RESET] 6s Hold Detected! Decommissioning...   ");
        Serial.println(" Wiping Matter fabrics and restarting device...         ");
        Serial.println("========================================================");
        delay(500);
        Matter.decommission(); // Clears all fabrics, credentials, and reboots ESP32
      }
    }
  } else {
    resetButtonPressed = false;
  }

  // Yield to FreeRTOS to allow Matter Wi-Fi/BLE background tasks to run smoothly
  vTaskDelay(pdMS_TO_TICKS(10));
}

/*
   Matter 6-Button Switch — Seeed Studio XIAO MG24

   Six momentary Matter switches on D0-D5 (active-low, internal pull-up).
   Physical press fires InitialPress; physical release fires ShortRelease
   (previousPosition=1) so HomeKit "single press" automations trigger correctly.
   EM1 light sleep is used between button presses to save power.

   Compatible board: Seeed Studio XIAO MG24 (Matter)

   Based on the Silicon Labs / Seeed Studio Matter switch example.
*/

#include <Matter.h>
#include <MatterSwitch.h>
#include <app/clusters/switch-server/switch-server.h>
#include "sl_power_manager.h"
#include <semphr.h>

// ── MatterSwitchWithId — exposes the Matter endpoint ID ──────────────────────
//
// The library's DeviceSwitch::HandleSwitchDeviceStatusChanged() calls
// SwitchServer::OnShortRelease(endpoint, this->current_position), but by the
// time that callback fires, current_position has already been updated to 0.
// HomeKit's "single press" automation requires ShortRelease.previousPosition > 0
// (it must match the NewPosition from the preceding InitialPress).  Passing 0
// makes HomeKit discard the event, so automations never fire.
//
// Fix: call SwitchServer::Instance().OnShortRelease(eid, 1) directly from
// handle_button_release(), under the chip-stack lock, with the correct
// previousPosition.  The base_matter_device pointer needed for the endpoint ID
// is protected in ArduinoMatterAppliance; this thin subclass exposes it.
class MatterSwitchWithId : public MatterSwitch {
public:
  chip::EndpointId endpoint_id() const {
    return base_matter_device ? base_matter_device->GetEndpointId()
                              : chip::kInvalidEndpointId;
  }
};

// ── Pin definitions (XIAO MG24 silk-screen labels) ──────────────────────────
#define BTN_ON        D0
#define BTN_OFF       D1
#define BTN_DIM_UP    D2
#define BTN_DIM_DOWN  D3
#define BTN_SCENE1    D4
#define BTN_SCENE2    D5

// ── Timing ───────────────────────────────────────────────────────────────────
#define DEBOUNCE_MS   50UL    // debounce window (ms)
#define AWAKE_MS      10000UL // idle time before entering EM1 sleep

// ── Matter endpoints ─────────────────────────────────────────────────────────
MatterSwitchWithId matter_switch_on;
MatterSwitchWithId matter_switch_off;
MatterSwitchWithId matter_switch_dim_up;
MatterSwitchWithId matter_switch_dim_down;
MatterSwitchWithId matter_switch_scene1;
MatterSwitchWithId matter_switch_scene2;

#define NUM_BUTTONS 6

MatterSwitchWithId* const endpoints[NUM_BUTTONS] = {
  &matter_switch_on,
  &matter_switch_off,
  &matter_switch_dim_up,
  &matter_switch_dim_down,
  &matter_switch_scene1,
  &matter_switch_scene2
};

// ── Button state (debounce) ───────────────────────────────────────────────────
static const uint8_t button_pins[NUM_BUTTONS] = {
  BTN_ON, BTN_OFF, BTN_DIM_UP, BTN_DIM_DOWN, BTN_SCENE1, BTN_SCENE2
};
static bool          btn_stable_high[NUM_BUTTONS];
static bool          btn_raw_high[NUM_BUTTONS];
static unsigned long btn_last_change_ms[NUM_BUTTONS];

// Tracks whether InitialPress was sent for each button; gates ShortRelease so
// an orphaned release after wake-from-sleep is silently dropped.
static bool          btn_press_sent[NUM_BUTTONS];

// ── Sleep tracking ────────────────────────────────────────────────────────────
static unsigned long     last_activity_ms = 0;
static SemaphoreHandle_t wake_semaphore   = nullptr;

// ── Forward declarations ──────────────────────────────────────────────────────
static void handle_button_press(uint8_t index);
static void handle_button_release(uint8_t index);
static void enter_light_sleep();
static void wake_isr();

// =============================================================================
void setup()
{
  Serial.begin(115200);
  Serial.println("Matter 6-button switch");

  // Built-in LED: force it ON (do not expose as a Matter bulb)
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LED_BUILTIN_ACTIVE);

  // Configure buttons as input with internal pull-up (active-low)
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pinMode(button_pins[i], INPUT_PULLUP);
    btn_stable_high[i]    = true;
    btn_raw_high[i]       = true;
    btn_last_change_ms[i] = 0;
    btn_press_sent[i]     = false;
  }

  // Initialise Matter stack and all six switch endpoints
  Matter.begin();
  matter_switch_on.begin();
  matter_switch_off.begin();
  matter_switch_dim_up.begin();
  matter_switch_dim_down.begin();
  matter_switch_scene1.begin();
  matter_switch_scene2.begin();

  // Create wake semaphore for EM1 sleep
  wake_semaphore = xSemaphoreCreateBinary();
  if (wake_semaphore == nullptr) {
    Serial.println("ERROR: failed to create wake semaphore");
    while (1) {}
  }

  // Commission device if not already done
  if (!Matter.isDeviceCommissioned()) {
    Serial.println("Matter device is not commissioned.");
    Serial.println("Commission it to your Matter hub with the manual pairing code or QR code.");
    Serial.printf("Manual pairing code: %s\n", Matter.getManualPairingCode().c_str());
    Serial.printf("QR code URL: %s\n", Matter.getOnboardingQRCodeUrl().c_str());
  }

  while (!Matter.isDeviceCommissioned()) {
    delay(200);
  }

  Serial.println("Waiting for Thread network...");
  while (!Matter.isDeviceThreadConnected()) {
    delay(200);
  }
  Serial.println("Connected to Thread network");

  Serial.println("Waiting for Matter device discovery...");
  while (!matter_switch_on.is_online()) {
    delay(200);
  }
  Serial.println("Matter switch device is now online");

  last_activity_ms = millis();
}

// =============================================================================
void loop()
{
  // Keep LED always ON (re-assert in case anything else changes it)
  digitalWrite(LED_BUILTIN, LED_BUILTIN_ACTIVE);

  unsigned long now = millis();

  // Debounced button polling — detects both press (falling) and release (rising).
  // Both edges share the same outer debounce check: btn_last_change_ms is
  // updated on any raw signal change, and an edge is only confirmed once the
  // signal has remained stable for DEBOUNCE_MS milliseconds.
  for (int i = 0; i < NUM_BUTTONS; i++) {
    bool raw = (digitalRead(button_pins[i]) == HIGH);
    if (raw != btn_raw_high[i]) {
      btn_raw_high[i]       = raw;
      btn_last_change_ms[i] = now;
    }
    if ((now - btn_last_change_ms[i]) > DEBOUNCE_MS) {
      if (!raw && btn_stable_high[i]) {
        // Falling edge confirmed — button pressed
        last_activity_ms = now;
        handle_button_press(i);
      } else if (raw && !btn_stable_high[i]) {
        // Rising edge confirmed — button released
        last_activity_ms = now;
        handle_button_release(i);
      }
      btn_stable_high[i] = raw;
    }
  }

  // Enter EM1 light sleep after idle
  if ((millis() - last_activity_ms) > AWAKE_MS) {
    enter_light_sleep();
  }
}

// -----------------------------------------------------------------------------
// handle_button_press() — send InitialPress to the matching Matter endpoint
// -----------------------------------------------------------------------------
static void handle_button_press(uint8_t index)
{
  static const char* const names[NUM_BUTTONS] = {
    "BTN ON", "BTN OFF", "BTN DIM UP", "BTN DIM DOWN", "BTN SCENE1", "BTN SCENE2"
  };
  if (index >= NUM_BUTTONS) return;
  Serial.printf("Button pressed  - %s\n", names[index]);
  endpoints[index]->set_state(true);  // → InitialPress(newPosition=1)
  btn_press_sent[index] = true;
}

// -----------------------------------------------------------------------------
// handle_button_release() — send ShortRelease with previousPosition=1
//
// The library's DeviceSwitch::HandleSwitchDeviceStatusChanged() calls
// SwitchServer::OnShortRelease(endpoint, this->current_position), but by the
// time that fires, current_position has already been set to 0.  HomeKit
// discards a ShortRelease whose previousPosition=0 (it doesn't match the
// InitialPress at position 1), so "single press" automations never trigger.
//
// We bypass the broken library path and call OnShortRelease(eid, 1) directly
// under the chip-stack lock.  The library's own OnShortRelease(eid, 0) still
// fires as a side-effect of the internal attribute write; HomeKit silently
// discards that spurious event because previousPosition=0 ≠ InitialPress
// position=1.  The correct event (previousPosition=1) triggers the automation.
// -----------------------------------------------------------------------------
static void handle_button_release(uint8_t index)
{
  static const char* const names[NUM_BUTTONS] = {
    "BTN ON", "BTN OFF", "BTN DIM UP", "BTN DIM DOWN", "BTN SCENE1", "BTN SCENE2"
  };
  if (index >= NUM_BUTTONS) return;
  if (!btn_press_sent[index]) return;  // ignore release if no press was registered (e.g. after wake-from-sleep)
  btn_press_sent[index] = false;

  Serial.printf("Button released - %s\n", names[index]);

  chip::EndpointId eid = endpoints[index]->endpoint_id();
  if (eid == chip::kInvalidEndpointId) return;

  // StackLock is a RAII guard: acquires PlatformMgr().LockChipStack() on
  // construction and releases it on destruction, even if an exception is thrown.
  chip::DeviceLayer::StackLock lock;
  chip::app::Clusters::SwitchServer::Instance().OnShortRelease(eid, 1 /* previousPosition */);
}

// -----------------------------------------------------------------------------
// enter_light_sleep() — block in EM1 until a button GPIO interrupt fires
// -----------------------------------------------------------------------------
static void enter_light_sleep()
{
  Serial.println("Entering EM1 light sleep...");
  Serial.flush();

  if (wake_semaphore == nullptr) return;
  xSemaphoreTake(wake_semaphore, 0); // drain any stale token

  for (int i = 0; i < NUM_BUTTONS; i++) {
    attachInterrupt(digitalPinToInterrupt(button_pins[i]), wake_isr, FALLING);
  }

  sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);

  xSemaphoreTake(wake_semaphore, portMAX_DELAY);

  sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1);

  for (int i = 0; i < NUM_BUTTONS; i++) {
    detachInterrupt(digitalPinToInterrupt(button_pins[i]));
  }

  unsigned long wake_ms = millis();
  for (int i = 0; i < NUM_BUTTONS; i++) {
    btn_last_change_ms[i] = wake_ms;
    btn_raw_high[i]       = (digitalRead(button_pins[i]) == HIGH);
    btn_stable_high[i]    = btn_raw_high[i];
  }

  last_activity_ms = wake_ms;
  Serial.println("Awake");
}

// -----------------------------------------------------------------------------
// wake_isr() — gives wake_semaphore from interrupt context to unblock the task
// -----------------------------------------------------------------------------
static void wake_isr()
{
  if (wake_semaphore == nullptr) return;
  BaseType_t higher_priority_task_woken = pdFALSE;
  xSemaphoreGiveFromISR(wake_semaphore, &higher_priority_task_woken);
  portYIELD_FROM_ISR(higher_priority_task_woken);
}

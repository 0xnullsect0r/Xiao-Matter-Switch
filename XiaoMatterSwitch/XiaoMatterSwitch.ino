/*
   Matter 6-Button Switch — Seeed Studio XIAO MG24

   Six momentary Matter switches on D0-D5 (active-low, internal pull-up).
   Each button press sends a brief ON pulse to its Matter switch endpoint so
   HomeKit automations trigger on "turns on".  EM1 light sleep is used between
   button presses to save power.

   Compatible board: Seeed Studio XIAO MG24 (Matter)

   Based on the Silicon Labs / Seeed Studio Matter switch example.
*/

#include <Matter.h>
#include <MatterSwitch.h>
#include "sl_power_manager.h"
#include <semphr.h>

// ── Pin definitions (XIAO MG24 silk-screen labels) ──────────────────────────
#define BTN_ON        D0
#define BTN_OFF       D1
#define BTN_DIM_UP    D2
#define BTN_DIM_DOWN  D3
#define BTN_SCENE1    D4
#define BTN_SCENE2    D5

// ── Timing ───────────────────────────────────────────────────────────────────
#define DEBOUNCE_MS   50UL    // debounce window
#define PULSE_MS      150UL   // momentary ON-pulse width
#define AWAKE_MS      10000UL // idle time before entering EM1 sleep

// ── Matter endpoints ─────────────────────────────────────────────────────────
MatterSwitch matter_switch_on;
MatterSwitch matter_switch_off;
MatterSwitch matter_switch_dim_up;
MatterSwitch matter_switch_dim_down;
MatterSwitch matter_switch_scene1;
MatterSwitch matter_switch_scene2;

#define NUM_BUTTONS 6

MatterSwitch* const endpoints[NUM_BUTTONS] = {
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

// ── Momentary pulse tracking ──────────────────────────────────────────────────
static bool          pulse_pending[NUM_BUTTONS];
static unsigned long pulse_start_ms[NUM_BUTTONS];

// ── Sleep tracking ────────────────────────────────────────────────────────────
static unsigned long     last_activity_ms = 0;
static SemaphoreHandle_t wake_semaphore   = nullptr;

// ── Forward declarations ──────────────────────────────────────────────────────
static void handle_button_press(uint8_t index);
static void enter_light_sleep();
static void wake_isr();

// =============================================================================
void setup()
{
  Serial.begin(115200);
  Serial.println("Matter 6-button switch");

  // Configure buttons as input with internal pull-up (active-low)
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pinMode(button_pins[i], INPUT_PULLUP);
    btn_stable_high[i]    = true;
    btn_raw_high[i]       = true;
    btn_last_change_ms[i] = 0;
    pulse_pending[i]      = false;
    pulse_start_ms[i]     = 0;
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
  unsigned long now = millis();

  // Reset momentary pulses after PULSE_MS
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (pulse_pending[i] && (now - pulse_start_ms[i]) >= PULSE_MS) {
      endpoints[i]->set_state(false);
      pulse_pending[i] = false;
    }
  }

  // Debounced button polling
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
// handle_button_press() — fire a momentary ON pulse on the matching endpoint
// -----------------------------------------------------------------------------
static void handle_button_press(uint8_t index)
{
  static const char* const names[NUM_BUTTONS] = {
    "BTN ON", "BTN OFF", "BTN DIM UP", "BTN DIM DOWN", "BTN SCENE1", "BTN SCENE2"
  };
  if (index >= NUM_BUTTONS) return;
  Serial.printf("Button pressed - %s\n", names[index]);
  endpoints[index]->set_state(true);
  pulse_start_ms[index] = millis();
  pulse_pending[index]  = true;
}

// -----------------------------------------------------------------------------
// enter_light_sleep() — block in EM1 until a button GPIO interrupt fires
//
// EM1 is the only safe sleep level on the XIAO MG24:
//   EM2/EM3 disable the HFXO needed by the Thread radio and corrupt its state.
//   EM4 is a full reset — all RAM and Thread credentials are lost.
//   EM1 gates only the CPU clock; GPIO interrupts and radio DMA stay active.
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

  // Clamp power manager to EM1 so the Thread radio (HFXO) stays alive
  sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);

  // Yield this FreeRTOS task — tickless-idle engages and the CPU enters EM1
  xSemaphoreTake(wake_semaphore, portMAX_DELAY);

  sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1);

  for (int i = 0; i < NUM_BUTTONS; i++) {
    detachInterrupt(digitalPinToInterrupt(button_pins[i]));
  }

  // Re-anchor debounce state so held buttons don't cause spurious presses
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

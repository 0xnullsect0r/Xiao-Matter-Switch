/**
 * @file    XiaoMatterSwitch.ino
 * @brief   Seeed Studio XIAO MG24 — 6-Button Wireless Smart Switch
 *          HomeKit integration via Matter over Thread (Silicon Labs SDK)
 *
 * This device is a SWITCH CONTROLLER ONLY — it has no lights of its own.
 * Every button press produces a momentary ON pulse on the corresponding Matter
 * switch endpoint.  In HomeKit you create automations triggered by the switch
 * turning on ("When <Switch> turns on → do something") to control whatever
 * smart lights or other accessories you choose.
 *
 * ─── Implementation Note ────────────────────────────────────────────────────
 *  The Silicon Labs Arduino core (SiliconLabs/hardware/silabs) provides
 *  MatterSwitch (MatterSwitch.h) but does NOT expose the Generic Switch
 *  cluster.  Therefore each endpoint is implemented as a momentary Matter
 *  switch: on button press the endpoint is set ON, then automatically reset
 *  to OFF after PULSE_MS milliseconds.  HomeKit automations should trigger
 *  on the "turns on" event.
 *
 * ─── Matter Endpoints ───────────────────────────────────────────────────────
 *  EP 1  Matter Switch  →  ON button
 *  EP 2  Matter Switch  →  OFF button
 *  EP 3  Matter Switch  →  Dim Up button
 *  EP 4  Matter Switch  →  Dim Down button
 *  EP 5  Matter Switch  →  Scene 1 button  (configure freely in HomeKit)
 *  EP 6  Matter Switch  →  Scene 2 button  (configure freely in HomeKit)
 *
 *  Each endpoint appears as a separate switch accessory in the Home app.
 *  Assign automations to each switch independently — trigger on "turns on":
 *    ON       → Turn on [your lights]
 *    OFF      → Turn off [your lights]
 *    Dim Up   → Increase brightness of [your lights]
 *    Dim Down → Decrease brightness of [your lights]
 *    Scene 1  → Activate a scene / run a shortcut / toggle anything
 *    Scene 2  → Another scene or automation
 *
 * ─── Button Wiring ──────────────────────────────────────────────────────────
 *  Button      XIAO Pin   Connect to GND when pressed (internal pull-up active)
 *  ─────────── ─────────  ──────────────────────────────────────────────────
 *  ON          D0
 *  OFF         D1
 *  Dim Up      D2
 *  Dim Down    D3
 *  Scene 1     D4
 *  Scene 2     D5
 *
 * ─── Power / Sleep Strategy ─────────────────────────────────────────────────
 *  Energy Mode EM1 (light sleep) ONLY.
 *
 *  Known XIAO MG24 / EFR32MG24 silicon errata & SDK constraints:
 *    • EM2 / EM3 shut down the HFXO that the Thread radio requires; entering
 *      them outside the SDK-managed sleep path corrupts Thread state on wake.
 *    • EM4 is a full power-on reset — all RAM and Thread credentials lost.
 *    • EM1 gates only the CPU clock; SRAM, peripherals, and radio DMA stay
 *      alive (~1–3 mA with ICD slow-poll active).
 *
 *  Matter ICD (Intermittently Connected Device) mode keeps the Thread network
 *  connection alive with a slow-poll timer while the CPU sleeps.  Any button
 *  press wakes the CPU in microseconds via GPIO interrupt.
 *
 * ─── Board / Toolchain ──────────────────────────────────────────────────────
 *  Board Manager : Seeed XIAO MG24 board package
 *  Board         : Seeed Studio XIAO MG24 (Matter)
 *  SDK           : Silicon Labs Matter (embedded in board package)
 *                  SiliconLabs/hardware/silabs — uses MatterSwitch.h
 *
 * SPDX-License-Identifier: MIT
 */

#include <Matter.h>
#include <MatterSwitch.h>
#include "sl_power_manager.h"  // sl_power_manager_add/remove_em_requirement
#include <semphr.h>            // FreeRTOS binary semaphore for task-blocking sleep

// ─────────────────────────────────────────────────────────────────────────────
// Pin Definitions  (XIAO MG24 silk-screen labels)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint8_t BTN_ON        = D0;
static constexpr uint8_t BTN_OFF       = D1;
static constexpr uint8_t BTN_DIM_UP    = D2;
static constexpr uint8_t BTN_DIM_DOWN  = D3;
static constexpr uint8_t BTN_SCENE1    = D4;
static constexpr uint8_t BTN_SCENE2    = D5;

// ─────────────────────────────────────────────────────────────────────────────
// Timing Constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr unsigned long DEBOUNCE_MS   =  50UL;    // GPIO debounce window
static constexpr unsigned long PULSE_MS      = 150UL;    // momentary ON-pulse width
static constexpr unsigned long AWAKE_MS      = 10000UL;  // idle → sleep timeout (10 seconds)

// ─────────────────────────────────────────────────────────────────────────────
// Matter Endpoints — one MatterSwitch per button
// Each appears as a separate switch accessory in the Home app.
// Note: The Silicon Labs Arduino core does not expose the Generic Switch
// cluster; endpoints are implemented as momentary switches (ON pulse → OFF).
// ─────────────────────────────────────────────────────────────────────────────
MatterSwitch swOn;       // EP1 — ON button
MatterSwitch swOff;      // EP2 — OFF button
MatterSwitch swDimUp;    // EP3 — Dim Up button
MatterSwitch swDimDown;  // EP4 — Dim Down button
MatterSwitch swScene1;   // EP5 — Scene 1 button
MatterSwitch swScene2;   // EP6 — Scene 2 button

// Collect endpoints into an array parallel to the button array so
// handleButtonPress() can dispatch without a switch statement.
static MatterSwitch* const s_endpoints[] = {
  &swOn, &swOff, &swDimUp, &swDimDown, &swScene1, &swScene2
};

// ─────────────────────────────────────────────────────────────────────────────
// Button State Machine
// ─────────────────────────────────────────────────────────────────────────────
struct ButtonState {
  uint8_t       pin;
  bool          stableHigh;     // true = released (pull-up asserted)
  bool          rawHigh;        // last raw digitalRead value
  unsigned long lastChangeMs;   // time of last raw edge (for debounce)
};

// Order must match s_endpoints[] above.
static ButtonState s_buttons[] = {
  { BTN_ON,       true, true, 0 },
  { BTN_OFF,      true, true, 0 },
  { BTN_DIM_UP,   true, true, 0 },
  { BTN_DIM_DOWN, true, true, 0 },
  { BTN_SCENE1,   true, true, 0 },
  { BTN_SCENE2,   true, true, 0 },
};
static constexpr uint8_t NUM_BUTTONS =
    static_cast<uint8_t>(sizeof(s_buttons) / sizeof(s_buttons[0]));

static_assert(
    sizeof(s_endpoints) / sizeof(s_endpoints[0]) == NUM_BUTTONS,
    "s_endpoints and s_buttons must have the same length");

// ─────────────────────────────────────────────────────────────────────────────
// Momentary Pulse State Machine
// When a button is pressed the corresponding endpoint is set ON.  After
// PULSE_MS milliseconds loop() resets it to OFF so HomeKit automations
// trigger reliably on each press.
// ─────────────────────────────────────────────────────────────────────────────
static bool          s_pulsePending[NUM_BUTTONS] = {};
static unsigned long s_pulseStartMs[NUM_BUTTONS] = {};

// ─────────────────────────────────────────────────────────────────────────────
// Sleep Tracking
// ─────────────────────────────────────────────────────────────────────────────
static unsigned long     s_lastActivityMs  = 0;
// Binary semaphore given by wakeISR(); enterLightSleep() blocks on it so the
// FreeRTOS task truly yields and tickless-idle can engage EM1.
static SemaphoreHandle_t s_wakeSemaphore   = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Forward Declarations
// ─────────────────────────────────────────────────────────────────────────────
static void handleButtonPress(uint8_t index);
static void enterLightSleep();
static void wakeISR();   // ISR: gives s_wakeSemaphore to unblock sleeping task

// ═════════════════════════════════════════════════════════════════════════════
// setup()
// ═════════════════════════════════════════════════════════════════════════════
void setup()
{
  Serial.begin(115200);
  delay(500);   // let UART settle before first print
  Serial.println();
  Serial.println("==============================================");
  Serial.println(" XIAO MG24 — 6-Button Wireless Smart Switch");
  Serial.println("==============================================");

  // ── GPIO: internal pull-up, active-low buttons ──────────────────────────
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    pinMode(s_buttons[i].pin, INPUT_PULLUP);
  }

  // ── Matter ICD: register as sleepy end-device (SED) ─────────────────────
  // ICD slow/fast poll intervals and SED mode are configured at build time
  // via Silicon Labs SDK build-system flags (sl_matter_icd_config.h or
  // equivalent); the runtime setter APIs are not exposed by this Arduino core.

  // ── Matter stack initialisation ──────────────────────────────────────────
  Matter.begin();

  // ── Endpoint configuration ───────────────────────────────────────────────
  swOn.begin();
  swOff.begin();
  swDimUp.begin();
  swDimDown.begin();
  swScene1.begin();
  swScene2.begin();

  s_lastActivityMs = millis();

  // ── Wake semaphore ───────────────────────────────────────────────────────
  s_wakeSemaphore = xSemaphoreCreateBinary();
  if (s_wakeSemaphore == nullptr) {
    Serial.println("[ERROR] Failed to create wake semaphore — halting.");
    while (true) { /* halt: heap exhausted, device unusable */ }
  }

  // ── Commissioning guidance ───────────────────────────────────────────────
  if (!Matter.isDeviceCommissioned()) {
    Serial.println();
    Serial.println("Matter device is not commissioned.");
    Serial.println("Commission it to your Matter hub with the manual pairing code or QR code.");
    Serial.println("To add to HomeKit:");
    Serial.println("  1. Open the Apple Home app.");
    Serial.println("  2. Tap  +  ->  Add Accessory.");
    Serial.println("  3. Scan the QR code on the device label, or choose");
    Serial.println("     'More Options' and enter the setup code manually.");
    Serial.println("  6 switch accessories will be added (ON, OFF, Dim Up,");
    Serial.println("  Dim Down, Scene 1, Scene 2).  Create automations for");
    Serial.println("  each one in the Home app to control your smart lights.");
    Serial.println();
    Serial.print("Manual pairing code: ");
    Serial.println(Matter.getManualPairingCode());
    Serial.print("QR code URL: ");
    Serial.println(Matter.getOnboardingQRCodeUrl());
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
  while (!swOn.is_online()) {
    delay(200);
  }
  Serial.println("Matter device is now online");
}

// ═════════════════════════════════════════════════════════════════════════════
// loop()
// ═════════════════════════════════════════════════════════════════════════════
void loop()
{
  // ── Button polling with debounce ─────────────────────────────────────────
  unsigned long now = millis();

  // ── Reset momentary switch pulses after PULSE_MS ─────────────────────────
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    if (s_pulsePending[i] && (now - s_pulseStartMs[i]) >= PULSE_MS) {
      s_endpoints[i]->set_state(false);
      s_pulsePending[i] = false;
    }
  }

  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    bool raw = (digitalRead(s_buttons[i].pin) == HIGH);  // true = released

    // Edge detected — reset debounce timer
    if (raw != s_buttons[i].rawHigh) {
      s_buttons[i].rawHigh      = raw;
      s_buttons[i].lastChangeMs = now;
    }

    // Signal stable for DEBOUNCE_MS — check for press (HIGH → LOW transition)
    if ((now - s_buttons[i].lastChangeMs) > DEBOUNCE_MS) {
      if (!raw && s_buttons[i].stableHigh) {
        // Falling edge confirmed: button just pressed
        s_lastActivityMs = millis();
        handleButtonPress(i);
      }
      s_buttons[i].stableHigh = raw;
    }
  }

  // ── Enter EM1 light sleep after idle window ──────────────────────────────
  if ((millis() - s_lastActivityMs) > AWAKE_MS) {
    enterLightSleep();
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// handleButtonPress()
// Initiates a momentary ON pulse on the MatterSwitch endpoint matching this
// button.  The endpoint is set ON immediately; loop() will reset it to OFF
// after PULSE_MS milliseconds.  HomeKit automations should be triggered by
// the switch "turning on" event.
// ═════════════════════════════════════════════════════════════════════════════
static void handleButtonPress(uint8_t index)
{
  static const char* const names[] = {
    "[BTN] ON",
    "[BTN] OFF",
    "[BTN] DIM UP",
    "[BTN] DIM DOWN",
    "[BTN] SCENE 1",
    "[BTN] SCENE 2",
  };

  if (index < NUM_BUTTONS) {
    Serial.println(names[index]);
    s_endpoints[index]->set_state(true);
    s_pulseStartMs[index] = millis();
    s_pulsePending[index] = true;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// enterLightSleep()
//
// Enters Silicon Labs EM1 light sleep.
//
// Why EM1 and not EM2/EM3/EM4?
//   EM2 shuts down the HFXO (high-freq crystal oscillator).  The SiLabs
//   Matter SDK keeps the Thread network interface running on HFXO; entering
//   EM2 outside the SDK-managed sleep path hangs the radio and corrupts
//   Thread state on the next wake-up.
//
//   EM3 additionally gates the LFXO, breaking the RTC used by Matter
//   subscription timers.
//
//   EM4 is effectively a power-on reset — all RAM and Thread credentials lost.
//
//   EM1 gates only the CPU core clock, leaving all peripherals (GPIO
//   interrupts, radio DMA) running.  Power consumption is ~1–3 mA with the
//   Thread radio in slow-poll ICD mode — acceptable for battery operation.
//
// Sleep mechanism:
//   An explicit EM1 requirement is added via sl_power_manager before blocking,
//   guaranteeing the power manager cannot descend below EM1 even if the Thread
//   stack has not yet registered its own EM1 requirement (e.g. during early
//   boot or commissioning).  The loop task then blocks on a FreeRTOS binary
//   semaphore, yielding to the scheduler so FreeRTOS tickless-idle can engage
//   and allow the CPU to enter EM1.  Calling bare __WFI() from an active task
//   is NOT used because it returns on every scheduler tick (~1 ms), preventing
//   any real sleep.
//
// Wake sources:
//   • GPIO falling-edge interrupt on any button pin — wakeISR() gives the
//     semaphore, unblocking this task.
//   • Thread ICD slow-poll timer — handled transparently by the radio driver.
// ══════════════════════════════════════════════════════════════════════���══════
static void enterLightSleep()
{
  Serial.println("[POWER] Entering EM1 light sleep...");
  Serial.flush();   // drain UART FIFO before CPU clock is gated

  // Drain any stale semaphore token so we don't return immediately.
  if (s_wakeSemaphore == nullptr) { return; }
  xSemaphoreTake(s_wakeSemaphore, 0);

  // Attach GPIO wake interrupts (FALLING = button press to GND).
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    attachInterrupt(digitalPinToInterrupt(s_buttons[i].pin), wakeISR, FALLING);
  }

  // Prevent the power manager from going below EM1.  This is the critical
  // safety guard: without it, the EFR32 could enter EM2/EM3 (disabling HFXO)
  // during any transient window where the Thread driver's own EM1 requirement
  // is not yet registered, corrupting the radio stack.
  sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);

  // Block the FreeRTOS task indefinitely.  Unlike __WFI(), blocking here marks
  // the task as "waiting" so FreeRTOS's tickless-idle hook can run and the
  // power manager will actually enter EM1 between ticks.  wakeISR() gives this
  // semaphore, which unblocks the task and resumes execution below.
  xSemaphoreTake(s_wakeSemaphore, portMAX_DELAY);

  // Release the explicit EM1 requirement now that we are active again.
  sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1);

  // ── Execution resumes here after wake ────────────────────────────────────

  // Detach GPIO interrupts — loop() handles buttons from here.
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    detachInterrupt(digitalPinToInterrupt(s_buttons[i].pin));
  }

  // Re-anchor debounce state to avoid spurious presses if a button is still
  // held when the CPU wakes.
  unsigned long wakeMs = millis();
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    s_buttons[i].lastChangeMs = wakeMs;
    s_buttons[i].rawHigh    = (digitalRead(s_buttons[i].pin) == HIGH);
    s_buttons[i].stableHigh = s_buttons[i].rawHigh;
  }

  s_lastActivityMs = wakeMs;
  Serial.println("[POWER] Awake");
}

// ═════════════════════════════════════════════════════════════════════════════
// wakeISR()
// GPIO interrupt service routine that unblocks the sleeping loop task.
// Gives s_wakeSemaphore from ISR context so xSemaphoreTake() in
// enterLightSleep() returns and the task resumes.  portYIELD_FROM_ISR()
// triggers an immediate context switch if the task has higher priority than
// whatever was running when the interrupt fired.
// ═════════════════════════════════════════════════════════════════════════════
static void wakeISR()
{
  if (s_wakeSemaphore == nullptr) { return; }
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(s_wakeSemaphore, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

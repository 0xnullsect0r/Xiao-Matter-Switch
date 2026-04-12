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
static constexpr unsigned long DEBOUNCE_MS   =  50UL;   // GPIO debounce window
static constexpr unsigned long PULSE_MS      = 150UL;   // momentary ON-pulse width
static constexpr unsigned long AWAKE_MS      = 3000UL;  // idle → sleep timeout
static constexpr unsigned long SLOW_POLL_S   =    5UL;  // Thread ICD slow poll
static constexpr unsigned long FAST_POLL_S   =    1UL;  // Thread ICD fast poll

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
static unsigned long s_lastActivityMs = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Forward Declarations
// ─────────────────────────────────────────────────────────────────────────────
static void handleButtonPress(uint8_t index);
static void enterLightSleep();
static void wakeISR();   // ISR: no-op; CPU wakes by hardware interrupt

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

  // ── Commissioning guidance ───────────────────────────────────────────────
  if (!Matter.isDeviceCommissioned()) {
    Serial.println();
    Serial.println("Device is NOT commissioned.");
    Serial.println("To add to HomeKit:");
    Serial.println("  1. Open the Apple Home app.");
    Serial.println("  2. Tap  +  ->  Add Accessory.");
    Serial.println("  3. Scan the QR code on the device label, or choose");
    Serial.println("     'More Options' and enter the setup code manually.");
    Serial.println("  6 switch accessories will be added (ON, OFF, Dim Up,");
    Serial.println("  Dim Down, Scene 1, Scene 2).  Create automations for");
    Serial.println("  each one in the Home app to control your smart lights.");
    Serial.println();
#if defined(MATTER_SETUP_CODE)
    Serial.print("Setup code : ");
    Serial.println(Matter.getSetupCode());
#endif
#if defined(MATTER_QR_CODE_URL)
    Serial.print("QR URL     : ");
    Serial.println(Matter.getQRCodeUrl());
#endif
  } else {
    Serial.println("Device is commissioned and ready.");
  }
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
// Wake sources:
//   • GPIO falling-edge interrupt on any button pin.
//   • Thread ICD slow-poll timer (every SLOW_POLL_S seconds) — handled inside
//     Matter.sleep() / the radio driver, transparent to the application.
// ═════════════════════════════════════════════════════════════════════════════
static void enterLightSleep()
{
  Serial.println("[POWER] Entering EM1 light sleep...");
  Serial.flush();   // drain UART FIFO before gating CPU clock

  // Attach GPIO wake interrupts (FALLING = button press to GND).
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    attachInterrupt(digitalPinToInterrupt(s_buttons[i].pin), wakeISR, FALLING);
  }

  // __WFI() is the ARM Cortex-M "Wait For Interrupt" instruction.  It
  // suspends the CPU core until any interrupt fires (GPIO, RTOS tick, radio
  // DMA, …) which is functionally equivalent to EM1 on EFR32: peripherals,
  // SRAM, and the Thread radio DMA all stay powered.  The SDK holds an EM1
  // requirement while Thread is active so the power manager will not allow
  // the system to descend below EM1 anyway.
  __WFI();

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
// Minimal GPIO interrupt service routine.  The CPU wakes automatically from
// EM1 when a GPIO interrupt fires; no application-level action is required.
// ═════════════════════════════════════════════════════════════════════════════
static void wakeISR()
{
  // No-op: hardware interrupt mechanism handles CPU wake from EM1.
  // Button state is re-read in loop() after wake.
}


/**
 * @file    XiaoMatterSwitch.ino
 * @brief   Seeed Studio XIAO MG24 — 6-Button Wireless Smart Switch
 *          HomeKit integration via Matter over Thread (Silicon Labs SDK)
 *
 * ─── Matter Endpoints ───────────────────────────────────────────────────────
 *  EP 1  Dimmable Light     ON / OFF / DIM UP / DIM DOWN buttons control a
 *                           single HomeKit light (or group) via Matter attributes.
 *  EP 2  Generic Switch     Scene 1 — stateless momentary switch; triggers
 *                           HomeKit automations ("When Switch is Pressed…").
 *  EP 3  Generic Switch     Scene 2 — same as above for a second scene/automation.
 *
 * ─── Button Wiring ──────────────────────────────────────────────────────────
 *  Button      XIAO Pin   Connect to GND when pressed (internal pull-up active)
 *  ─────────── ─────────  ──────────────────────────────────────────────────
 *  ON          D0
 *  OFF         D1
 *  Dimmer UP   D2
 *  Dimmer DOWN D3
 *  Scene 1     D4
 *  Scene 2     D5
 *
 * ─── Power / Sleep Strategy ─────────────────────────────────────────────────
 *  Energy Mode EM1 (light sleep) ONLY.
 *
 *  Known XIAO MG24 / EFR32MG24 silicon errata & SDK constraints that dictate
 *  this choice:
 *    • EM2 / EM3 / EM4 shut down the high-frequency oscillator that the
 *      Silicon Labs Matter SDK requires for the Thread radio.  Entering EM2+
 *      outside of the SDK-managed "Matter sleep" path corrupts the stack.
 *    • The SiLabs Arduino core does not expose safe EM2 retention-RAM restore
 *      hooks; peripheral (USART / I2C) state is NOT restored on EM2 wake.
 *    • EM4 is a full reset; all RAM is lost.
 *    • EM1 keeps SRAM, all peripherals, and the radio stack intact while
 *      cutting CPU clock-gating power to ~1–2 mA — adequate for a coin-cell
 *      or LiPo battery-powered wall switch.
 *
 *  Matter ICD (Intermittently Connected Device) mode registers the device
 *  with the Thread network as a sleepy end-device (SED).  The Thread router
 *  buffers incoming messages; the radio polls every SLOW_POLL_S seconds.
 *  Any button press wakes the CPU via GPIO interrupt before the poll fires.
 *
 * ─── Board / Toolchain ──────────────────────────────────────────────────────
 *  Board Manager URL : (Seeed XIAO MG24 board package)
 *  Board             : Seeed Studio XIAO MG24 (Matter)
 *  SDK               : Silicon Labs Matter (embedded in board package)
 *
 * SPDX-License-Identifier: MIT
 */

#include <Matter.h>
#include <MatterLightbulb.h>
#include <MatterGenericSwitch.h>

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
// Dimmer Configuration
// ─────────────────────────────────────────────────────────────────────────────
// Matter brightness uses the 0–254 range (Cluster Level Control).
static constexpr uint8_t DIM_STEP =  25;   // brightness change per press (~10 % of 0–254 range)
static constexpr uint8_t DIM_MIN  =   1;   // never fully off via dimmer
static constexpr uint8_t DIM_MAX  = 254;

// ─────────────────────────────────────────────────────────────────────────────
// Timing Constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr unsigned long DEBOUNCE_MS   =  50UL;   // GPIO debounce window
static constexpr unsigned long AWAKE_MS      = 3000UL;  // idle → sleep timeout
static constexpr unsigned long SLOW_POLL_S   =    5UL;  // Thread ICD slow poll
static constexpr unsigned long FAST_POLL_S   =    1UL;  // Thread ICD fast poll

// ─────────────────────────────────────────────────────────────────────────────
// Matter Endpoints
// ─────────────────────────────────────────────────────────────────────────────
MatterDimmableLightbulb light;   // EP1 — dimmable light control
MatterGenericSwitch     scene1;  // EP2 — Scene 1 momentary trigger
MatterGenericSwitch     scene2;  // EP3 — Scene 2 momentary trigger

// ─────────────────────────────────────────────────────────────────────────────
// Local Light State  (mirrors what is sent to / received from HomeKit)
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t s_brightness = 127;   // initial brightness ~50 %
static bool    s_isOn       = false;

// ─────────────────────────────────────────────────────────────────────────────
// Button State Machine
// ─────────────────────────────────────────────────────────────────────────────
struct ButtonState {
  uint8_t       pin;
  bool          stableHigh;      // true = released (pull-up asserted)
  bool          rawHigh;         // last raw digitalRead value
  unsigned long lastChangeMs;    // time of last raw edge (for debounce)
};

// Order must match the case indices in handleButtonPress()
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

// ─────────────────────────────────────────────────────────────────────────────
// Sleep / Wake Tracking
// ─────────────────────────────────────────────────────────────────────────────
static unsigned long    s_lastActivityMs = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Forward Declarations
// ─────────────────────────────────────────────────────────────────────────────
static void handleButtonPress(uint8_t index);
static void applyLightState();
static void enterLightSleep();
static void IRAM_ATTR wakeISR();   // ISR: no-op; CPU wakes by hardware interrupt

// ═════════════════════════════════════════════════════════════════════════════
// setup()
// ═════════════════════════════════════════════════════════════════════════════
void setup()
{
  Serial.begin(115200);
  // Brief delay lets the UART settle and a host terminal connect before the
  // first printout.  Keep short; battery devices should not waste time here.
  delay(500);
  Serial.println();
  Serial.println("==============================================");
  Serial.println(" XIAO MG24 — 6-Button Wireless Smart Switch");
  Serial.println("==============================================");

  // ── GPIO: internal pull-up, active-low buttons ──────────────────────────
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    pinMode(s_buttons[i].pin, INPUT_PULLUP);
  }

  // ── Matter ICD: register as sleepy end-device (SED) ─────────────────────
  // These must be called BEFORE Matter.begin().
  Matter.setIcdMode(true);
  Matter.setIcdSlowPollingInterval(SLOW_POLL_S);
  Matter.setIcdFastPollingInterval(FAST_POLL_S);

  // ── Matter stack initialisation ──────────────────────────────────────────
  Matter.begin();

  // ── Endpoint configuration ───────────────────────────────────────────────
  light.begin();
  scene1.begin();
  scene2.begin();

  light.setDeviceName("Wall Switch Light");
  scene1.setDeviceName("Scene 1");
  scene2.setDeviceName("Scene 2");

  // Push initial state so HomeKit shows the correct values on first connect.
  light.setOnOff(s_isOn);
  light.setBrightness(s_brightness);

  s_lastActivityMs = millis();

  // ── Commissioning guidance ───────────────────────────────────────────────
  if (!Matter.isDeviceCommissioned()) {
    Serial.println();
    Serial.println("Device is NOT commissioned.");
    Serial.println("To add to HomeKit:");
    Serial.println("  1. Open the Apple Home app.");
    Serial.println("  2. Tap  +  →  Add Accessory.");
    Serial.println("  3. Scan the QR code on the device label, or choose");
    Serial.println("     'More Options' and enter the setup code manually.");
    Serial.println();
    // If the board package exposes a setup-code helper, print it here.
    // Some SiLabs Arduino builds expose Matter.getSetupCode() / getQRCode().
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
  // ── Pump the Matter event loop ───────────────────────────────────────────
  Matter.loop();

  // ── Sync HomeKit-initiated changes back to local state ───────────────────
  // Siri commands, automations, or the Home app can change on/off or
  // brightness independently.  Mirror those changes into our local variables
  // so the dimmer buttons start from the correct baseline.
  {
    bool    remoteOn = light.getOnOff();
    uint8_t remoteBr = light.getBrightness();

    if (remoteOn != s_isOn || remoteBr != s_brightness) {
      s_isOn       = remoteOn;
      s_brightness = remoteBr;
      s_lastActivityMs = millis();
      Serial.print("[HomeKit] power=");
      Serial.print(s_isOn ? "ON" : "OFF");
      Serial.print("  brightness=");
      Serial.print(map(s_brightness, 0, 254, 0, 100));
      Serial.println('%');
    }
  }

  // ── Button polling with debounce ─────────────────────────────────────────
  unsigned long now = millis();
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    bool raw = (digitalRead(s_buttons[i].pin) == HIGH);  // true = released

    // Edge detected — reset debounce timer
    if (raw != s_buttons[i].rawHigh) {
      s_buttons[i].rawHigh      = raw;
      s_buttons[i].lastChangeMs = now;
    }

    // Signal stable for DEBOUNCE_MS — check for press (HIGH→LOW transition)
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
// ═════════════════════════════════════════════════════════════════════════════
static void handleButtonPress(uint8_t index)
{
  switch (index) {

    // ── ON ──────────────────────────────────────────────────────────────────
    case 0:
      Serial.println("[BTN] ON");
      s_isOn = true;
      applyLightState();
      break;

    // ── OFF ─────────────────────────────────────────────────────────────────
    case 1:
      Serial.println("[BTN] OFF");
      s_isOn = false;
      applyLightState();
      break;

    // ── Dimmer UP ────────────────────────────────────────────────────────────
    case 2:
      Serial.println("[BTN] DIM UP");
      if (s_brightness <= DIM_MAX - DIM_STEP) {
        s_brightness = static_cast<uint8_t>(s_brightness + DIM_STEP);
      } else {
        s_brightness = DIM_MAX;
      }
      s_isOn = true;   // dimming up implicitly turns the light on
      applyLightState();
      break;

    // ── Dimmer DOWN ──────────────────────────────────────────────────────────
    case 3:
      Serial.println("[BTN] DIM DOWN");
      if (s_brightness >= DIM_MIN + DIM_STEP) {
        s_brightness = static_cast<uint8_t>(s_brightness - DIM_STEP);
      } else {
        s_brightness = DIM_MIN;
      }
      // Dimming down does NOT automatically turn off — HomeKit automations
      // can handle the "turn off at minimum brightness" policy if desired.
      applyLightState();
      break;

    // ── Scene 1 ──────────────────────────────────────────────────────────────
    // Sends a Matter InitialPress event on EP2.
    // In HomeKit: create an automation triggered by "Scene 1 is pressed".
    case 4:
      Serial.println("[BTN] SCENE 1 → HomeKit automation trigger");
      scene1.sendInitialPressEvent();
      break;

    // ── Scene 2 ──────────────────────────────────────────────────────────────
    case 5:
      Serial.println("[BTN] SCENE 2 → HomeKit automation trigger");
      scene2.sendInitialPressEvent();
      break;

    default:
      break;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// applyLightState()
// Writes the local on/off and brightness values to the Matter endpoint so
// HomeKit (and any bound devices) receive the update.
// ═════════════════════════════════════════════════════════════════════════════
static void applyLightState()
{
  light.setOnOff(s_isOn);
  light.setBrightness(s_brightness);
  Serial.print("  → power=");
  Serial.print(s_isOn ? "ON" : "OFF");
  Serial.print("  brightness=");
  Serial.print(map(s_brightness, 0, 254, 0, 100));
  Serial.println('%');
}

// ═════════════════════════════════════════════════════════════════════════════
// enterLightSleep()
//
// Enters Silicon Labs EM1 light sleep.
//
// Why EM1 and not EM2/EM3/EM4?
//   EM2 shuts down the HFXO (high-freq crystal oscillator).  The SiLabs
//   Matter SDK keeps the Thread network interface running on HFXO; entering
//   EM2 outside the SDK-managed sleep path would hang the radio and corrupt
//   Thread state on the next wake-up.
//
//   EM3 additionally gates the LFXO, breaking the RTC used by the Matter
//   subscription timers.
//
//   EM4 is effectively a power-on reset — all RAM and peripheral state is
//   lost.  Not appropriate for a continuously-connected Thread device.
//
//   EM1 gates only the CPU core clock, leaving all peripherals (USART, GPIO
//   interrupts, radio DMA) running.  Power consumption is typically 1–3 mA
//   with the Thread radio in slow-poll ICD mode — acceptable for a LiPo-
//   or AA-battery-powered wall switch.
//
// Wake sources:
//   • GPIO falling-edge interrupt on any button pin.
//   • Thread ICD slow-poll timer (fires every SLOW_POLL_S seconds) — handled
//     entirely inside Matter.sleep() / the radio driver, transparent to app.
// ═════════════════════════════════════════════════════════════════════════════
static void enterLightSleep()
{
  Serial.println("[POWER] Entering EM1 light sleep…");
  Serial.flush();   // drain UART FIFO before gating CPU clock

  // Attach GPIO wake interrupts for all button pins.
  // FALLING = button pressed (pull-up pin driven to GND).
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    attachInterrupt(digitalPinToInterrupt(s_buttons[i].pin), wakeISR, FALLING);
  }

  // sl_power_manager_sleep() (called internally by Matter.sleep()) requests
  // the lowest energy mode that all registered requirements allow.  With
  // Matter ICD configured and the radio stack running, the SDK keeps an EM1
  // requirement active, so the call always resolves to EM1 on this platform.
  Matter.sleep();

  // ── Execution resumes here after wake ────────────────────────────────────

  // Detach GPIO interrupts now that loop() handles buttons again.
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    detachInterrupt(digitalPinToInterrupt(s_buttons[i].pin));
  }

  // Reset the debounce timestamps to avoid spurious presses on first loop
  // iteration after wake (the pin may still be LOW while the button is held).
  unsigned long wakeMs = millis();
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    s_buttons[i].lastChangeMs = wakeMs;
    // Re-read the current pin state so the debouncer starts from reality.
    s_buttons[i].rawHigh    = (digitalRead(s_buttons[i].pin) == HIGH);
    s_buttons[i].stableHigh = s_buttons[i].rawHigh;
  }

  s_lastActivityMs = wakeMs;
  Matter.wake();
  Serial.println("[POWER] Awake");
}

// ═════════════════════════════════════════════════════════════════════════════
// wakeISR()
// Minimal GPIO interrupt service routine.
// The CPU wakes automatically from EM1 when a GPIO interrupt fires; no
// application-level action is needed inside the ISR itself.
// Marked IRAM_ATTR to ensure the ISR lives in always-retained RAM — required
// on SiLabs EFR32 cores so that cache misses cannot stall the ISR during EM1.
// ═════════════════════════════════════════════════════════════════════════════
static void IRAM_ATTR wakeISR()
{
  // No-op: waking the CPU from EM1 is handled entirely by the hardware
  // interrupt mechanism.  Button state is re-read in loop() after wake.
}

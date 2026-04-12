# XIAO Matter Switch

A battery-powered, wireless 6-button smart **switch controller** for Apple HomeKit, built on the **Seeed Studio XIAO MG24 (Matter)** development kit.  It connects to HomeKit over **Matter / Thread** and behaves like a Nanoleaf Sense+ — no hub required, just a Thread Border Router (Apple TV 4K, HomePod mini, or HomePod).

**This device has no lights of its own.**  Every button press fires a Matter button event.  You create automations in the Home app to make each button do whatever you want — turn on/off lights, adjust brightness, activate scenes, run shortcuts, etc.

---

## How It Works

Each of the 6 buttons is exposed to HomeKit as a separate **momentary Matter switch** accessory.  When you press a button, the device briefly sets the corresponding switch endpoint **ON** (for ~150 ms), then automatically resets it to **OFF**.  You create automations in the Home app that trigger when each switch **turns on**:

> **Note:** The Silicon Labs Arduino core (`SiliconLabs/hardware/silabs`) provides `MatterSwitch` but does **not** expose the Matter Generic Switch cluster.  Endpoints are therefore implemented as momentary switches — each press produces a short ON pulse so HomeKit automations can reliably detect the "turns on" transition.

| Button | Matter Endpoint | Suggested HomeKit Automation |
|--------|----------------|------------------------------|
| ON | EP1 — Matter Switch | Turn on [your lights] |
| OFF | EP2 — Matter Switch | Turn off [your lights] |
| Dim Up | EP3 — Matter Switch | Increase brightness of [your lights] |
| Dim Down | EP4 — Matter Switch | Decrease brightness of [your lights] |
| Scene 1 | EP5 — Matter Switch | Activate a scene / run a shortcut / anything |
| Scene 2 | EP6 — Matter Switch | Another scene or automation |

All 6 buttons work the same way in HomeKit — each one is a configurable switch you assign freely in automations.  Set the automation trigger to **"turns on"** (not "is pressed").

### Power
- **EM1 light sleep** between button presses (see [Sleep Strategy](#sleep-strategy)).
- **Matter ICD (Intermittently Connected Device)** mode — Thread slow-polls every 5 s while sleeping.
- Suitable for coin-cell, AA, or small LiPo battery.

---

## Hardware Shell
I would reccomend using this [Macro Pad](https://www.thingiverse.com/thing:2206672) to hold everything. This should be big enough to hold most everything, including a battery depending on which. 

---

## Hardware Requirements

| Item | Notes |
|------|-------|
| Seeed Studio XIAO MG24 (Matter) | Silicon Labs EFR32MG24 SoC |
| 6 × momentary push-buttons | Normally-open, rated for 3.3 V |
| Thread Border Router | Apple TV 4K / HomePod mini / HomePod |
| Optional: LiPo battery + JST connector | Or 3 × AAA via a 3.3 V regulator |

---

## Wiring

```
XIAO MG24          Button          GND rail
──────────         ──────          ────────
D0  ─────────────[  ON  ]─────────── GND
D1  ─────────────[  OFF ]─────────── GND
D2  ─────────────[DIM ▲ ]─────────── GND
D3  ─────────────[DIM ▼ ]─────────── GND
D4  ─────────────[SCN 1 ]─────────── GND
D5  ─────────────[SCN 2 ]─────────── GND
```

Internal pull-ups are enabled in firmware — no external resistors needed.

---

## Build & Flash

### 1. Install the Board Package

1. Open Arduino IDE → **Preferences**.
2. Add the Seeed XIAO MG24 board manager URL (check the [Seeed wiki](https://wiki.seeedstudio.com/XIAO_MG24_Getting_Started/) for the current URL).
3. **Tools → Board → Board Manager** → search **"Seeed XIAO MG24"** → Install.

### 2. Select Board & Settings

| Setting | Value |
|---------|-------|
| Board | **Seeed Studio XIAO MG24 (Matter)** |
| Upload Method | USB |

### 3. Open & Upload Sketch

1. Open `XiaoMatterSwitch/XiaoMatterSwitch.ino`.
2. Click **Upload**.
3. Open **Serial Monitor** at **115200 baud** to watch startup and commissioning messages.

---

## Commissioning (Adding to HomeKit)

1. Flash the sketch and open the Serial Monitor.
2. If the device has never been commissioned, you will see:

   ```
   Device is NOT commissioned.
   To add to HomeKit:
     1. Open the Apple Home app.
     2. Tap  +  ->  Add Accessory.
     3. Scan the QR code on the device label, or choose
        'More Options' and enter the setup code manually.
     6 button accessories will be added (ON, OFF, Dim Up,
     Dim Down, Scene 1, Scene 2).  Create automations for
     each one in the Home app to control your smart lights.
   ```

3. Open the **Home** app on your iPhone/iPad → tap **+** → **Add Accessory**.
4. Scan the QR code printed on the XIAO MG24 board or on the packaging, **or** choose *More Options* and type the numeric setup code.
5. Follow the on-screen prompts.  **Six** switch accessories will be added:
   - **ON**, **OFF**, **Dim Up**, **Dim Down**, **Scene 1**, **Scene 2**

> **Tip:** Assign all accessories to the same room as the lights they will control, then set up automations for each switch (trigger: "turns on").

---

## HomeKit Automation Setup

Do this for every button you want to use:

1. In the **Home** app, open **Automations → +**.
2. Choose **An Accessory is Controlled**.
3. Select the switch (e.g., **ON**).
4. Trigger: **"turns on"**.
5. Add the action — for example:
   - **ON** → Turn on your living room lights.
   - **OFF** → Turn off your living room lights.
   - **Dim Up** → Increase brightness of your living room lights by 10 %.
   - **Dim Down** → Decrease brightness of your living room lights by 10 %.
   - **Scene 1** → Activate a "Movie Night" scene.
   - **Scene 2** → Activate a "Reading" scene.

You can assign completely different lights or actions to each button — the switch itself does not store or track any light state.

---

## Sleep Strategy

### Why EM1 only?

The XIAO MG24 uses Silicon Labs **EFR32MG24** silicon.  There are several hardware and SDK constraints that make EM2 / EM3 / EM4 unsafe for this use-case:

| Mode | Risk on XIAO MG24 |
|------|-------------------|
| **EM2** | Shuts down HFXO.  The Matter/Thread radio stack requires HFXO; entering EM2 outside the SDK-managed sleep path corrupts Thread state and hangs the radio on wake. |
| **EM3** | Additionally gates LFXO and RTC.  Matter subscription timers stop; the device falls off the Thread network. |
| **EM4** | Power-on reset — all RAM lost.  Thread network credentials lost; recommissioning required every boot. |
| **EM1** ✅ | CPU clock gated, SRAM and all peripherals retained.  Radio DMA + ICD slow-poll timer operate normally.  ~1–3 mA with Thread radio active. |

**EM1** is enforced by calling `Matter.sleep()`, which internally calls `sl_power_manager_sleep()`.  The SDK holds an EM1 requirement while the Thread interface is up, so this call always resolves to EM1 on this platform — it is not possible to accidentally drop into EM2.

### Wake Sources

| Source | Description |
|--------|-------------|
| **GPIO falling edge** | Any button press → CPU wakes from EM1 within microseconds |
| **Thread ICD slow-poll timer** | Fires every 5 s → radio wakes, polls parent router for buffered messages, CPU stays asleep unless a message is received |

---

## File Structure

```
XiaoMatterSwitch/
└── XiaoMatterSwitch.ino    Main Arduino sketch
README.md
LICENSE
```

---

## License

MIT — see [LICENSE](LICENSE).

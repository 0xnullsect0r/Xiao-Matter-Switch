# XIAO Matter Switch

A battery-powered, wireless 6-button smart switch for Apple HomeKit, built on the **Seeed Studio XIAO MG24 (Matter)** development kit.  It connects to HomeKit over **Matter / Thread** and behaves like a Nanoleaf Sense+ — no hub required, just a Thread Border Router (Apple TV 4K, HomePod mini, or HomePod).

---

## Features

| Button | Function | HomeKit Endpoint |
|--------|----------|-----------------|
| ON | Turn light on | EP1 — Dimmable Light |
| OFF | Turn light off | EP1 — Dimmable Light |
| Dimmer ▲ | Increase brightness by ~10 % | EP1 — Dimmable Light |
| Dimmer ▼ | Decrease brightness by ~10 % | EP1 — Dimmable Light |
| Scene 1 | Trigger HomeKit Automation | EP2 — Generic Switch |
| Scene 2 | Trigger HomeKit Automation | EP3 — Generic Switch |

Scene buttons appear as **"Switch"** accessories in HomeKit.  Create automations in the Home app triggered by *"When Switch is Pressed"* to do anything — change scenes, run shortcuts, toggle other devices.

### Power
- **EM1 light sleep** between button presses (see [Sleep Strategy](#sleep-strategy)).
- **Matter ICD (Intermittently Connected Device)** mode — Thread slow-polls every 5 s while sleeping.
- Suitable for coin-cell, AA, or small LiPo battery.

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
     2. Tap  +  →  Add Accessory.
     3. Scan the QR code on the device label, or choose
        'More Options' and enter the setup code manually.
   ```

3. Open the **Home** app on your iPhone/iPad → tap **+** → **Add Accessory**.
4. Scan the QR code printed on the XIAO MG24 board or on the packaging, **or** choose *More Options* and type the numeric setup code (printed next to the QR code).
5. Follow the on-screen prompts.  Three accessories will be added:
   - **Wall Switch Light** — controls your light via EP1.
   - **Scene 1** — a switch you can use in automations.
   - **Scene 2** — a second switch for a second automation.

> **Tip:** Assign the accessories to a room and rename them to whatever makes sense for your setup (e.g., "Living Room Scene", "Movie Mode").

---

## HomeKit Automation Setup (Scene Buttons)

1. In the **Home** app, open **Automations → +**.
2. Choose **An Accessory is Controlled**.
3. Select **Scene 1** (or Scene 2).
4. Trigger: **"is pressed"** (or "initial press").
5. Add the action you want — e.g., activate a Scene, toggle a light group, lock a door.

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
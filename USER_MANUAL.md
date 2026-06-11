# User Manual

## Purpose

This manual explains how to set up, test, and operate the automated hydrogel production platform. It is written for daily operation, hardware checking, and troubleshooting.

The main rule is:

```text
The software state must match the real hardware state before using the Control page.
```

The UI can track syringe volumes, valve stage, gantry position, UV history, and pH readings, but most of these values are software records. If the hardware is moved by hand, disconnected, refilled, emptied, or reset, the software state must be updated before automatic control.

## Board Map

The system uses three Arduino Mega + RAMPS 1.4 boards, one Arduino Uno, and one ESP32 camera.

### Mega + RAMPS 1: Pumping Unit

This board controls the four source syringe pumps.

```text
RAMPS X motor socket  -> pumping syringe X
RAMPS Y motor socket  -> pumping syringe Y
RAMPS Z motor socket  -> pumping syringe Z
RAMPS E1 motor socket -> pumping syringe E1
```

All four source syringe motors are connected to this first Mega + RAMPS board.

### Mega + RAMPS 2: Selector / Valve Unit

This board controls the outlet lift and mixing valve motors.

```text
RAMPS Z motor socket -> outlet lift motor
RAMPS X motor socket -> mixing valve X motor
RAMPS Y motor socket -> mixing valve Y motor
```

The outlet lift motor is implemented and calibrated. The future outlet routing / selection motor is reserved for later mechanical completion.

### Mega + RAMPS 3: Deposition Gantry and Mixer Pumps

This board controls the XY gantry and the two mixer syringes.

```text
RAMPS X motor socket  -> gantry CoreXY motor A
RAMPS Y motor socket  -> gantry CoreXY motor B
RAMPS Z motor socket  -> mixer syringe Z
RAMPS E1 motor socket -> mixer syringe E1
```

The gantry and mixer pumps share one board so that deposition can synchronize circular gantry motion with E1 syringe dispensing.

### Arduino Uno: UV Relay

The Uno controls the UV relay.

Typical relay control wiring:

```text
Uno GND -> relay GND
Uno 5V  -> relay VCC
Uno D8  -> relay signal input
```

The relay power side depends on the UV power supply. Use the relay COM/NO contacts according to the relay module and UV supply wiring.

### ESP32: pH Camera

The ESP32 camera is connected to the PC by USB. It reads RGB/HSV color data and sends readings to the central controller through serial.

## RAMPS Motor Socket Rule

This project only uses the RAMPS motor sockets:

```text
X, Y, Z, E1
```

Do not assume other motor outputs are used unless the firmware is changed.

## Software Installation

Install Python 3.12 or another recent Python 3 version.

From the project root:

```bash
pip install -r requirements.txt
```

The main Python dependency is `pyserial`. Tkinter is included with the normal Windows Python installer.

## Firmware Upload

Upload each firmware file to the matching board with Arduino IDE.

```text
Pumping Mega/RAMPS:
Software/pumping_unit/syringe_pump_ramps_controller/syringe_pump_ramps_controller.ino

Selector / valve Mega/RAMPS:
Software/mixing/outlet_flange_valve_ramps_controller/outlet_flange_valve_ramps_controller.ino

Gantry + mixer pump Mega/RAMPS:
Software/deposition_gantry/deposition_gantry_ramps_controller/deposition_gantry_ramps_controller.ino

pH camera ESP32:
Software/ph_camera/esp32_ph_camera/esp32_ph_camera.ino

UV relay Uno:
Software/uv_curing/uv_relay_controller.ino
```

All active serial controllers use:

```text
115200 baud
```

## Default COM Ports

The current default ports are:

```text
Pumping unit: COM9
Selector / valve unit: COM8
Gantry + mixer pumps: COM7
pH camera: COM5
UV relay: COM4
```

If Windows assigns different COM ports, change them in the UI Settings page.

## Starting the UI

From the project root:

```bash
cd Software
python central_ui.py
```

The UI has three main pages:

```text
Settings  -> COM ports, pH gate, dry-run setting
Unit Test -> independent testing for each unit
Control   -> ordered hydrogel production workflow
```

## Dry-run Mode

Dry-run mode is for checking the UI and command generation.

In dry-run mode:

- The UI still builds central-controller commands.
- The terminal report still shows what would be run.
- No real serial command is sent to the hardware.
- Motors, camera, and UV should not move or turn on.

Dry-run can confirm that the UI buttons and command paths are working, but it cannot prove the hardware is working.

For real hardware testing, turn dry-run off and use the Unit Test page.

## Recommended Setup Order

Use this order when setting up the system.

1. Assemble each unit separately.
2. Upload the correct firmware to each board.
3. Connect each board to the PC.
4. Open the UI.
5. Check and update COM ports in Settings.
6. Keep dry-run on and press several buttons to check the UI command path.
7. Turn dry-run off.
8. Use Unit Test to test each unit independently.
9. Connect tubing and fluid paths after independent unit tests pass.
10. Synchronize software state with real hardware state.
11. Use the Control page for the full workflow.

## Unit Test Page

Use Unit Test before running the Control page.

Recommended checks:

- Pumping unit:
  - Confirm each source syringe motor can move.
  - Confirm aspirate and dispense directions are correct.
  - Confirm current syringe volume records are correct.

- Selector / valve unit:
  - Confirm outlet lift up/down motion.
  - Confirm mixing valve Stage 1, Stage 2, and Stage 3 movement.
  - Confirm the software stage matches the physical valve stage.

- Mixing pumps:
  - Confirm mixer syringe Z can move in both directions.
  - Confirm mixer syringe E1 can move in both directions.
  - Confirm synchronized Z/E1 pair motion works.

- Deposition gantry:
  - Confirm X/Y gantry motion.
  - Move the gantry to the physical origin and use set zero.
  - Confirm circle drawing motion.
  - Confirm synchronized deposition motion if fluid testing is safe.

- pH camera:
  - Confirm camera can turn on.
  - Confirm color and estimated pH are printed.
  - Confirm camera turns off after the check.

- UV:
  - Confirm a short timed exposure such as 5 seconds.
  - Confirm stop turns UV off immediately.

## State Synchronization

The UI status panel shows software-side state. It must match the real machine.

Important state values:

```text
Pumping syringes: current liquid volume in X/Y/Z/E1
Mixing syringes: current liquid volume in Z/E1
Mixing valve: current stage
Gantry: current X/Y position
pH: latest measured color and estimated pH
UV: current on/off state and exposure history
```

Before using the Control page:

1. Confirm the real source syringe volumes.
2. Set pumping syringe records to match the real volumes.
3. Confirm the real mixer Z/E1 volumes.
4. Use mixer syringe set-zero only when the real mixer syringes are empty.
5. Confirm the physical mixing valve stage.
6. Set the software valve stage to match the real valve stage.
7. Move the gantry to the physical origin.
8. Use gantry set zero only at the real origin.

If any displayed state is unknown, stop and resynchronize before using automatic flow buttons.

## Control Page Workflow

The Control page is arranged in the expected production order. In normal operation, press the buttons from top to bottom, then check the right-side status panel and terminal report after each step.

Typical workflow:

1. Aspirate source solution into the required pumping syringe.
2. Move the mixing valve to Stage 1.
3. Transfer source solution from the pumping unit into mixer syringe Z.
4. Disable pumping motors if they are no longer needed.
5. Move the mixing valve to Stage 2.
6. Run flow mix cycles with synchronized Z/E1 push-pull motion.
7. Run pH camera check.
8. If pH is acceptable, transfer material from mixer Z to E1.
9. Move the mixing valve to Stage 3.
10. Run deposition with synchronized gantry circle and E1 dispense.
11. Return gantry to origin.
12. Wait for gelation.
13. Run UV curing.
14. Add buffer through the mixer path.
15. Deposit buffer back onto the hydrogel area.

The Control page includes logic checks. For example:

- Pumping-to-mixer transfer should only run when the mixing valve is at Stage 1.
- Mixing cycles should only run when the mixing valve is at Stage 2.
- Deposition should only run when the mixing valve is at Stage 3.
- Pump and mixer syringe volumes are checked before motion.

## Status Panel

The status panel summarizes known software state.

Color meaning:

```text
Green text -> known state
Red text   -> unknown state
Black text -> informational value without known/unknown flag
```

Do not continue automatic workflow steps when an important unit state is unknown.

## pH Check

The pH camera check turns the camera on, takes five readings, averages them, prints the averaged color and estimated pH, then turns the camera off.

The pH calibration table is:

```text
Software/ph_color_calibration_table.csv
```

The default target pH is:

```text
7.4
```

The pH result is a color-based estimate. It should be interpreted together with experimental judgment and calibration quality.

## UV Curing

The UV page buttons send timed `start` commands or immediate `stop` commands to the Uno relay controller.

Use a short test exposure first, such as 5 seconds.

Safety notes:

- Do not look directly at UV light.
- Keep UV exposure shielded.
- Use `stop` immediately if the relay or UV behavior is unexpected.

## Emergency Stop and Recovery

Motor boards support emergency stop behavior through firmware commands.

After emergency stop:

1. Motion stops and motors may be disabled.
2. Some software states may become unknown.
3. Clear the hardware stop state with `UNLOCK` only after checking the machine.
4. Reconfirm physical position, valve stage, and syringe volumes.
5. Re-enter or reset software state before continuing.

If the UI reports a command failure, read the terminal report before pressing more control buttons.

## Manual Serial Monitor Tests

These commands can be typed in Arduino IDE Serial Monitor when testing one board directly. Use `115200 baud`.

UV relay Uno:

```text
start 5
stop
```

Gantry + mixer board:

```text
GANTRY SETZERO
GANTRY GOTO X30 Y30
GANTRY CIRCLE X30 Y30 R10 N120 L1
PUMP PAIR ZS0.1 E1S-0.1
GANTRY DEPOSIT X30 Y30 R10 V1 N120 L1
STOP
UNLOCK
```

Selector / valve board:

```text
UP
DOWN
MIX_X_CW90
MIX_X_CCW90
MIX_Y_CW180
MIX_Y_CCW180
STOP
UNLOCK
```

Pumping board:

```text
XS1
XS-1
YL1
ZL-1
E1S0.5
STOP
```

## Troubleshooting

### COM port permission denied

Possible causes:

- Arduino IDE Serial Monitor is open.
- Another Python process is using the same port.
- The board was unplugged and replugged with a new COM number.

Fix:

1. Close Arduino Serial Monitor.
2. Close other running UI or Python windows.
3. Check Device Manager for the current COM port.
4. Update the UI Settings page.

### Motor does not move

Check:

- Correct board and COM port.
- Correct firmware uploaded.
- RAMPS power connected.
- Stepper driver installed and oriented correctly.
- Motor plugged into the correct X/Y/Z/E1 socket.
- Dry-run is off.
- Terminal report does not show an error.

### Motion direction is wrong

Direction can be fixed in firmware by changing the relevant invert constant. Re-upload firmware after changing direction constants.

### pH camera does not print color

Check:

- Correct ESP32 COM port.
- Correct ESP32 firmware uploaded.
- Camera is not already locked by another program.
- The camera has enough light and can see the sample.

### UV relay does not click

Check:

- Correct Uno COM port.
- Correct UV relay firmware uploaded.
- Relay signal wire is connected to D8.
- Relay GND and Uno GND are connected.
- Serial baud is 115200.

### UI status does not match hardware

Stop automatic operation. Manually confirm hardware state, then update the software state using Unit Test or set-zero/set-stage controls.

## Shutdown and Cleaning

After an experiment:

1. Stop UV.
2. Turn off pH camera if it is still active.
3. Disable motors where possible.
4. Save any needed terminal output or experiment notes.
5. Flush or clean tubing.
6. Clean syringes.
7. Remove or seal samples.
8. Disconnect power supplies when safe.

## Final Operating Reminder

Use Unit Test to prove each unit works alone. Use Control only after hardware state and software state agree. During Control operation, press buttons in order, watch the status panel, and read the terminal report after each command.

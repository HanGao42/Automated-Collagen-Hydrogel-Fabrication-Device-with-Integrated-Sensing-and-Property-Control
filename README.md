# Automated Hydrogel Production Platform

This project is a modular hardware and software platform for automated hydrogel production. It coordinates liquid loading, valve routing, syringe mixing, pH color checking, gantry deposition, UV curing, and buffer rehydration.

The active control software is in `Software/`. CAD and mechanical design files are kept separately in `CAD/`.

## System Summary

The platform is built from six hardware units:

1. Pumping unit
   - Four source syringes driven by stepper motors.
   - Aspirates source solutions at the start of an experiment.
   - Dispenses selected source solutions into the mixer during transfer.
   - Controlled by an Arduino Mega with a RAMPS 1.4 shield.

2. Outlet selector / valve lift unit
   - Uses stepper motors to lift and lower the outlet tube.
   - The outlet lift motion is implemented and calibrated.
   - The future outlet routing motor is reserved for later mechanical completion.

3. Mixing unit
   - Two mixer syringes, named Z and E1.
   - Rotary valve stages define the fluid path.
   - Stage 1 allows incoming solution to enter mixer syringe Z.
   - Stage 2 connects Z and E1 for push-pull mixing.
   - Stage 3 routes mixed material out for deposition.

4. pH camera unit
   - ESP32 camera reads hydrogel indicator color.
   - The software estimates pH from an RGB/HSV calibration table.
   - The default target pH is 7.4.
   - During control-flow pH checks, the camera takes five readings, averages them, reports the average, and turns off.

5. Deposition gantry
   - XY gantry draws a small circle while depositing material.
   - This spreads viscous material and prevents a cone-shaped pileup.
   - The gantry and mixer Z/E1 pumps share one Arduino Mega + RAMPS board, which allows synchronized gantry motion and E1 dispensing.

6. UV curing unit
   - Arduino Uno and relay module control UV power.
   - The controller accepts timed `start <seconds>` commands and immediate `stop`.

## Main Hardware

- Arduino Mega 2560 with RAMPS 1.4 shield for the four-syringe pumping unit.
- Arduino Mega 2560 with RAMPS 1.4 shield for the outlet lift / selector valve unit.
- Arduino Mega 2560 with RAMPS 1.4 shield for the deposition gantry and mixer syringe pumps.
- Arduino Uno with relay module for UV light control.
- Freenove ESP32-S3 WROOM camera board for pH color measurement.
- Stepper motors and stepper drivers for syringe plungers, valve motion, and XY gantry motion.
- Four source syringes on the pumping board.
- Two small mixer syringes on the gantry/mixing board.
- UV light source connected through the relay module.

## Default Serial Ports

The current software defaults are:

- Pumping unit: `COM9`
- Outlet selector / valve lift: `COM8`
- Deposition gantry and mixer pumps: `COM7`
- pH camera: `COM5`
- UV relay controller: `COM4`
- Baud rate: `115200`

These ports can be changed in the UI settings page or by command-line options.

## Software Structure

```text
Software/
  central_controller.py
  central_ui.py
  central_command_reference.txt
  ph_color_calibration_table.csv

  pumping_unit/
    syringe_pump_cli.py
    syringe_pump_ramps_controller/
      syringe_pump_ramps_controller.ino
    pumping_state.json

  mixing/
    outlet_flange_valve_controller.py
    outlet_flange_valve_ramps_controller/
      outlet_flange_valve_ramps_controller.ino
    outlet_flange_valve_state.json
    mixing_pump_state.json

  deposition_gantry/
    deposition_gantry_controller.py
    deposition_gantry_ramps_controller/
      deposition_gantry_ramps_controller.ino
    deposition_gantry_state.json

  ph_camera/
    esp32_ph_camera/
      esp32_ph_camera.ino

  uv_curing/
    uv_relay_controller.ino

  ML_model/
    validation and exploratory model files
```

## Code Logic

The project uses four software layers.

### Arduino firmware

The `.ino` files are the lowest hardware-control layer. They run directly on the Arduino or ESP32 boards and convert serial text commands into physical actions.

Firmware responsibilities:

- Configure RAMPS pins, stepper driver enable pins, STEP pins, and DIR pins.
- Generate motor pulses for syringe, valve, and gantry movement.
- Control the UV relay.
- Read camera RGB/HSV data on the ESP32.
- Return readable status lines and `OK` messages to Python.
- Handle emergency commands such as `STOP` and `UNLOCK`.

The Arduino firmware does not know the full hydrogel workflow. It only performs direct hardware actions requested by Python.

### Unit Python scripts

The unit Python scripts are the hardware wrapper layer. They send serial commands to one board and maintain software-side state where needed.

- `pumping_unit/syringe_pump_cli.py`
  - Controls the four source syringe pumps.
  - Stores syringe size and current volume in `pumping_state.json`.
  - Checks syringe capacity before aspirating or dispensing.

- `mixing/outlet_flange_valve_controller.py`
  - Controls outlet lift and mixing valve stages.
  - Stores known/unknown valve state in `outlet_flange_valve_state.json`.
  - Prevents relative stage moves when the saved valve state is unknown.

- `deposition_gantry/deposition_gantry_controller.py`
  - Controls XY gantry motion and mixer Z/E1 pump commands.
  - Stores gantry position in `deposition_gantry_state.json`.
  - Sends mixer pump commands and synchronized gantry-deposition commands to the gantry RAMPS board.

### Central controller

`Software/central_controller.py` is the orchestration layer. It does not directly toggle step pins. Instead, it calls the unit scripts or opens simple serial devices, then coordinates them into guarded workflow steps.

Central controller responsibilities:

- Parse high-level commands such as `flow transfer`, `flow mix-cycle`, `flow ph-check`, and `flow deposit`.
- Check pumping syringe limits before source transfer.
- Check mixer Z/E1 limits before mixing or deposition.
- Require the correct mixing valve stage before each flow step.
- Run some unit commands in software parallel when two boards must act together.
- Read and compare pH camera data against `ph_color_calibration_table.csv`.
- Stop pH camera automatically after flow pH checks.
- Control timed UV exposure through the UV relay Arduino.

Examples:

```bash
cd Software
python central_controller.py pump status
python central_controller.py flow transfer x 2
python central_controller.py flow mix-cycle
python central_controller.py flow ph-check
python central_controller.py flow deposit 1
python central_controller.py uv start 5
```

### Graphical UI

`Software/central_ui.py` is a Tkinter interface on top of `central_controller.py`.

The UI is not an independent control system. It builds central-controller commands, runs them as child Python processes, and displays their terminal output.

The UI has:

- Settings page for COM ports, pH gate values, and dry-run mode.
- Unit Test page for independent hardware checks.
- Control page for the ordered hydrogel workflow.
- Flow buttons arranged in experiment order.
- Live state panels for pumping, selector, mixing, deposition, UV, and pH.
- Terminal report panel showing command output.

## Gantry Control Details

The deposition gantry firmware is in:

```text
Software/deposition_gantry/deposition_gantry_ramps_controller/deposition_gantry_ramps_controller.ino
```

The gantry uses a classic CoreXY-style motion model with two coupled motors:

```text
Motor A = X + Y
Motor B = X - Y
```

In the firmware this appears as:

```cpp
long aSteps = xSteps + ySteps;
long bSteps = xSteps - ySteps;
```

This means an X/Y movement is first converted to X and Y step counts, then transformed into Motor A and Motor B step counts. The firmware then pulses each motor by the required number of steps.

Important implementation points:

- The gantry does not use the full Marlin firmware.
- It does not use the AccelStepper library for the gantry motion.
- The motion planning is custom and lightweight, using RAMPS STEP/DIR pins directly.
- The implementation is inspired by common 3D-printer/CNC motion concepts:
  - STEP pulse generation.
  - DIR pin direction control.
  - Soft travel limits.
  - A simple trapezoid-style acceleration profile.
  - Manual logical zero with `SETZERO`.

The STEP pulse function is intentionally simple:

```cpp
void pulseMotor(byte stepPin) {
  digitalWrite(stepPin, HIGH);
  delayMicroseconds(STEP_HIGH_US);
  digitalWrite(stepPin, LOW);
}
```

For each step, the firmware raises the STEP pin, waits a few microseconds so the stepper driver can detect the edge, then lowers the pin again.

The gantry firmware also handles synchronized commands:

- `PUMP PAIR ZS0.1 E1S-0.1`
  - Moves mixer syringes Z and E1 at the same time.
  - Used for push-pull mixing without the delay caused by sequential commands.

- `GANTRY DEPOSIT X30 Y30 R8 V0.5 N120 L1`
  - Draws a circular gantry path while dispensing from mixer syringe E1.
  - Used during deposition so the gantry and E1 syringe move together on the same RAMPS board.

## Production Workflow

The intended automated workflow is:

1. Fill the selected pumping syringe with source solution.
2. Move the mixing valve to Stage 1.
3. Transfer solution from the pumping unit into mixer syringe Z.
4. Disable pumping motors if they are no longer needed, to reduce heating.
5. Move the mixing valve to Stage 2.
6. Run synchronized Z/E1 push-pull mixing cycles.
7. Turn on the pH camera, take five color readings, average them, and compare the average with the pH table.
8. If pH is acceptable, transfer the required volume from mixer Z to E1.
9. Move the mixing valve to Stage 3.
10. Deposit the hydrogel while the gantry draws a circle and E1 dispenses.
11. Return the gantry to origin.
12. Wait for gelation.
13. Run UV curing for the chosen duration.
14. Inject buffer through the mixer path.
15. Deposit buffer back onto the gel position for hydration.

## Safety and Limit Checks

The central controller checks:

- Pumping syringe capacity before aspirating.
- Pumping syringe current volume before dispensing.
- Mixer Z/E1 capacity before aspirating.
- Mixer Z/E1 current volume before dispensing.
- Required mixing valve stage before transfer, mixing, and deposition.
- Gantry position state before movement.
- Positive volume values for flow operations.
- pH camera timeout and automatic camera shutdown after flow pH checks.
- UV exposure duration must be positive for timed starts.

The Arduino firmware also includes:

- Emergency `STOP` and `UNLOCK` commands on motor boards.
- Gantry soft limits for X/Y travel.
- Circle segment and loop limits.
- Pump speed limits.

Direct Arduino Serial Monitor commands can bypass some Python-side state checks. For normal experiments, use the UI or `central_controller.py`.

## State Files

The JSON state files store software-side known positions and syringe contents:

- `pumping_unit/pumping_state.json`
- `mixing/mixing_pump_state.json`
- `mixing/outlet_flange_valve_state.json`
- `deposition_gantry/deposition_gantry_state.json`

These files must match the real hardware state before automated flow commands are used.

Use set-zero commands only when the hardware is physically at the corresponding zero state. If hardware is moved by hand, update the software state before running automatic commands.

## Arduino Firmware Uploads

Upload the following sketches to the matching boards:

- Pumping unit Mega/RAMPS:
  - `Software/pumping_unit/syringe_pump_ramps_controller/syringe_pump_ramps_controller.ino`

- Outlet lift / selector Mega/RAMPS:
  - `Software/mixing/outlet_flange_valve_ramps_controller/outlet_flange_valve_ramps_controller.ino`

- Deposition gantry and mixer pumps Mega/RAMPS:
  - `Software/deposition_gantry/deposition_gantry_ramps_controller/deposition_gantry_ramps_controller.ino`

- pH camera ESP32:
  - `Software/ph_camera/esp32_ph_camera/esp32_ph_camera.ino`

- UV relay Arduino Uno:
  - `Software/uv_curing/uv_relay_controller.ino`

All active controllers use `115200` baud.

## pH Calibration

The pH camera uses:

```text
Software/ph_color_calibration_table.csv
```

The central controller estimates pH by comparing the averaged RGB/HSV reading against this calibration table. The current target pH is 7.4.

The pH result is used as a process check. The current system reports whether the measured color is within the target range. Full acid/base automatic correction logic is reserved for future development.

## Installation

Install Python 3.12 or another recent Python 3 version.

Install the main runtime dependency:

```bash
pip install -r requirements.txt
```

Tkinter is included with the standard Windows Python installer.

Arduino IDE requirements:

- Install the `AccelStepper` library for the pumping unit sketch.
- Install the ESP32 board package for the Freenove ESP32-S3 pH camera sketch.
- The gantry, selector, and UV sketches otherwise use standard Arduino APIs.

## Running the UI

```bash
cd Software
python central_ui.py
```

Check COM port settings before running real hardware. The UI starts in dry-run mode by default. Uncheck dry-run only after confirming the ports, uploaded firmware, and physical hardware state.

## ML_model Folder

`Software/ML_model/` contains a validation / exploratory model. It is not part of the main hardware control loop. The model currently has large prediction error, so it should be treated only as a rough verification or analysis attempt rather than a production control component.

The ML folder has its own optional dependency list. These packages are not required to run the main automation system.

## User Manual Notes

A separate user manual should focus on operation rather than code. It should include:

- Hardware setup and wiring checks.
- Firmware upload order.
- COM port setup.
- How to initialize saved syringe volumes and gantry zero.
- How to use Unit Test safely.
- How to run the Control workflow.
- How to interpret pH, UV, and terminal messages.
- Emergency stop and recovery procedure.
- Common troubleshooting cases.

## Notes

- Keep saved JSON state files aligned with physical syringe contents and gantry position.
- Use the Unit Test page before running the full Control workflow.
- Keep dry-run enabled until ports and firmware are verified.
- The outlet routing motor logic is reserved for future mechanical completion.
- The command reference is available in `Software/central_command_reference.txt`.

import argparse
import json
import sys
import time
from pathlib import Path


STATE_FILE = Path(__file__).with_name("pumping_state.json")

MOTORS = {
    "x": {"label": "X", "arduino_prefix": "X"},
    "y": {"label": "Y", "arduino_prefix": "Y"},
    "z": {"label": "Z", "arduino_prefix": "Z"},
    "e1": {"label": "E1", "arduino_prefix": "E1"},
}

SYRINGES = {
    "small": {
        "label": "small",
        "arduino_prefix": "S",
        "capacity_ml": 5.0,
    },
    "big": {
        "label": "big",
        "arduino_prefix": "L",
        "capacity_ml": 20.0,
    },
}


# Build an empty state for all four pumping motors.
def default_state():
    return {
        "motors": {
            motor: {
                "syringe": None,
                "current_ml": None,
            }
            for motor in MOTORS
        }
    }


# Load saved syringe type and volume state from JSON.
def load_state():
    if not STATE_FILE.exists():
        return default_state()

    with STATE_FILE.open("r", encoding="utf-8") as f:
        loaded = json.load(f)

    state = default_state()

    if "motors" in loaded:
        for motor in MOTORS:
            state["motors"][motor].update(loaded["motors"].get(motor, {}))
    else:
        # Migrate the earlier two-syringe state format into Y motor if present.
        if "small" in loaded:
            state["motors"]["y"] = {
                "syringe": "small",
                "current_ml": loaded["small"].get("current_ml"),
            }

    return state


# Save current syringe state to JSON.
def save_state(state):
    with STATE_FILE.open("w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)


# Return metadata for a named motor.
def motor_info(name):
    if name not in MOTORS:
        raise ValueError(f"Unknown motor: {name}")
    return MOTORS[name]


# Return metadata for a named syringe size.
def syringe_info(name):
    if name not in SYRINGES:
        raise ValueError(f"Unknown syringe: {name}")
    return SYRINGES[name]


# Require a requested move volume to be positive.
def require_valid_volume_ml(value):
    if value <= 0:
        raise ValueError("Volume must be greater than 0 ml.")


# Require a saved current volume to be non-negative.
def require_valid_current_ml(value):
    if value < 0:
        raise ValueError("Current volume cannot be less than 0 ml.")


# Require a motor to have a known syringe and volume before moving.
def require_initialized_motor(state, motor):
    motor_state = state["motors"][motor]
    if motor_state["syringe"] is None or motor_state["current_ml"] is None:
        raise ValueError(
            f"Motor {motor} is not initialized. "
            f"Run: init {motor} small  or  init {motor} big"
        )
    return motor_state


# Send one serial command to the Arduino pump controller.
def send_to_arduino(port, baud, command, dry_run=False, wait_s=2.0, read_s=1.0):
    if dry_run:
        print(f"[dry-run] Arduino command: {command}")
        return []

    try:
        import serial
    except ImportError as exc:
        raise RuntimeError(
            "Missing pyserial. Install it with: pip install pyserial"
        ) from exc

    with serial.Serial(port, baudrate=baud, timeout=0.2) as ser:
        time.sleep(wait_s)
        ser.reset_input_buffer()
        print(f"Sending Arduino command: {command}")
        ser.write((command + "\n").encode("ascii"))
        ser.flush()

        lines = []
        deadline = time.time() + read_s
        while time.time() < deadline:
            line = ser.readline().decode(errors="replace").strip()
            if line:
                lines.append(line)
                print(f"arduino> {line}")
        return lines


# Send several serial commands through one Arduino connection.
def send_many_to_arduino(port, baud, commands, dry_run=False, wait_s=2.0, read_s=0.5):
    if dry_run:
        for command in commands:
            print(f"[dry-run] Arduino command: {command}")
        return []

    try:
        import serial
    except ImportError as exc:
        raise RuntimeError(
            "Missing pyserial. Install it with: pip install pyserial"
        ) from exc

    lines = []
    with serial.Serial(port, baudrate=baud, timeout=0.2) as ser:
        time.sleep(wait_s)
        ser.reset_input_buffer()

        for command in commands:
            print(f"Sending Arduino command: {command}")
            ser.write((command + "\n").encode("ascii"))
            ser.flush()

            deadline = time.time() + read_s
            while time.time() < deadline:
                line = ser.readline().decode(errors="replace").strip()
                if line:
                    lines.append(line)
                    print(f"arduino> {line}")

    return lines


# Check that Arduino output confirms the expected syringe move.
def confirm_arduino_move(lines, motor_label, syringe):
    syringe_label = "Small" if syringe == "small" else "Large"
    expected_start = f"{motor_label} {syringe_label} syringe move "
    return any(line.startswith(expected_start) for line in lines)


# Enable or disable all four pump motors.
def command_motor_power(args):
    action_word = "enable" if args.power_action == "enable" else "disable"
    past_word = "enabled" if args.power_action == "enable" else "disabled"
    commands = [f"{action_word}{info['arduino_prefix']}" for info in MOTORS.values()]
    expected_lines = [f"{info['label']} {past_word}." for info in MOTORS.values()]

    arduino_lines = send_many_to_arduino(
        port=args.port,
        baud=args.baud,
        commands=commands,
        dry_run=args.dry_run,
        wait_s=args.wait,
        read_s=args.read,
    )

    if args.dry_run:
        print(f"All motors {past_word} in dry-run.")
        return 0

    missing = [line for line in expected_lines if line not in arduino_lines]
    if missing:
        print(
            "WARNING: Arduino did not confirm every motor power command. "
            f"Missing confirmations: {', '.join(missing)}",
            file=sys.stderr,
        )
        return 2

    print(f"All motors {past_word}.")
    return 0


# Print saved syringe type and current volume for each motor.
def print_status(state):
    for motor, info in MOTORS.items():
        motor_state = state["motors"][motor]
        syringe = motor_state["syringe"]
        current = motor_state["current_ml"]

        if syringe is None or current is None:
            print(f"{info['label']}: not initialized")
            continue

        capacity = SYRINGES[syringe]["capacity_ml"]
        print(
            f"{info['label']}: {syringe} syringe, "
            f"{current:.3f} ml remaining / {capacity:.3f} ml capacity"
        )


# Initialize one pump motor with syringe size and current volume.
def command_init(args):
    motor_info(args.motor)
    info = syringe_info(args.syringe)
    require_valid_current_ml(args.current)

    if args.current > info["capacity_ml"]:
        print(
            f"WARNING: {args.syringe} syringe capacity is {info['capacity_ml']} ml, "
            f"but current is {args.current} ml.",
            file=sys.stderr,
        )
        return 2

    state = load_state()
    state["motors"][args.motor] = {
        "syringe": args.syringe,
        "current_ml": args.current,
    }
    save_state(state)

    print(
        f"{MOTORS[args.motor]['label']}: {args.syringe} syringe current volume "
        f"set to {args.current:.3f} / {info['capacity_ml']:.3f} ml"
    )
    return 0


# Print all saved pump states.
def command_status(_args):
    print_status(load_state())
    return 0


# Aspirate or dispense one syringe after checking saved volume limits.
def command_move(args):
    motor = args.motor
    motor_driver = motor_info(motor)
    require_valid_volume_ml(args.ml)

    state = load_state()
    motor_state = require_initialized_motor(state, motor)
    syringe = motor_state["syringe"]
    syringe_driver = syringe_info(syringe)

    current = float(motor_state["current_ml"])
    capacity = syringe_driver["capacity_ml"]

    # Convert the user action into a signed Arduino ml command.
    if args.action == "dispense":
        if args.ml > current:
            print(
                f"WARNING: refused. Motor {motor_driver['label']} has a {syringe} "
                f"syringe with only {current:.3f} ml remaining; cannot dispense "
                f"{args.ml:.3f} ml.",
                file=sys.stderr,
            )
            return 2
        new_current = current - args.ml
        arduino_ml = args.ml
    elif args.action == "aspirate":
        if current + args.ml > capacity:
            available = capacity - current
            print(
                f"WARNING: refused. Motor {motor_driver['label']} has a {syringe} "
                f"syringe and can only aspirate {available:.3f} ml before reaching "
                f"{capacity:.3f} ml capacity; requested {args.ml:.3f} ml.",
                file=sys.stderr,
            )
            return 2
        new_current = current + args.ml
        arduino_ml = -args.ml
    else:
        raise ValueError(f"Unknown action: {args.action}")

    arduino_command = (
        f"{motor_driver['arduino_prefix']}"
        f"{syringe_driver['arduino_prefix']}"
        f"{arduino_ml:.6g}"
    )
    arduino_lines = send_to_arduino(
        port=args.port,
        baud=args.baud,
        command=arduino_command,
        dry_run=args.dry_run,
        wait_s=args.wait,
        read_s=args.read,
    )

    # Only update saved state after the Arduino confirms the move.
    if not args.dry_run and not confirm_arduino_move(
        arduino_lines, motor_driver["label"], syringe
    ):
        print(
            "WARNING: Arduino did not confirm this move, so the saved volume was "
            "not changed. Upload the four-motor Arduino sketch if the board still "
            "prints 'RAMPS Y axis'.",
            file=sys.stderr,
        )
        return 2

    if not args.dry_run:
        state["motors"][motor]["current_ml"] = new_current
        save_state(state)

    if args.dry_run:
        print(
            f"{motor_driver['label']}: {syringe} syringe {args.action} "
            f"{args.ml:.3f} ml accepted in dry-run. Simulated volume: "
            f"{new_current:.3f} / {capacity:.3f} ml"
        )
    else:
        print(
            f"{motor_driver['label']}: {syringe} syringe {args.action} "
            f"{args.ml:.3f} ml accepted. Current volume: "
            f"{new_current:.3f} / {capacity:.3f} ml"
        )
    return 0


# Add common serial options to argparse subcommands that touch hardware.
def add_serial_options(parser):
    parser.add_argument(
        "--port",
        default="COM9",
        help="Arduino serial port, for example COM3 on Windows.",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Check limits and print Arduino command without opening serial.",
    )
    parser.add_argument(
        "--wait",
        type=float,
        default=2.0,
        help="Seconds to wait after opening serial. Arduino Mega may reset.",
    )
    parser.add_argument(
        "--read",
        type=float,
        default=1.0,
        help="Seconds to read Arduino serial response.",
    )


# Build the pumping unit command-line parser.
def build_parser():
    parser = argparse.ArgumentParser(
        description="Python controller for a four-motor RAMPS syringe pump."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # State-only commands configure or report saved syringe contents.
    init_parser = subparsers.add_parser(
        "init", help="Set the syringe type and current volume on one motor."
    )
    init_parser.add_argument("motor", choices=MOTORS.keys())
    init_parser.add_argument("syringe", choices=SYRINGES.keys())
    init_parser.add_argument(
        "current",
        nargs="?",
        type=float,
        default=0.0,
        help="Current volume in ml. Defaults to 0.",
    )
    init_parser.set_defaults(func=command_init)

    status_parser = subparsers.add_parser(
        "status", help="Show current volume for all motors."
    )
    status_parser.set_defaults(func=command_status)

    enable_all_parser = subparsers.add_parser(
        "enableAll", help="Enable all motors by sending enableX/enableY/enableZ/enableE1."
    )
    add_serial_options(enable_all_parser)
    enable_all_parser.set_defaults(func=command_motor_power, power_action="enable")

    disable_all_parser = subparsers.add_parser(
        "disableAll", help="Disable all motors by sending disableX/disableY/disableZ/disableE1."
    )
    add_serial_options(disable_all_parser)
    disable_all_parser.set_defaults(func=command_motor_power, power_action="disable")

    # Motion commands share motor, volume, and serial options.
    for action in ("dispense", "aspirate"):
        move_parser = subparsers.add_parser(
            action,
            help=(
                "Dispense liquid from one motor's syringe."
                if action == "dispense"
                else "Aspirate liquid into one motor's syringe."
            ),
        )
        move_parser.add_argument("motor", choices=MOTORS.keys())
        move_parser.add_argument("ml", type=float, help="Volume in ml.")
        add_serial_options(move_parser)
        move_parser.set_defaults(func=command_move, action=action)

    return parser


# Parse CLI arguments and run the selected pumping command.
def main():
    parser = build_parser()
    args = parser.parse_args()

    try:
        return args.func(args)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

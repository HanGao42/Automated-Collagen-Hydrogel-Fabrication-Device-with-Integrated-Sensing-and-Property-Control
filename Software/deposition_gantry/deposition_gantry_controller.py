import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable


BAUDRATE = 115200
DEFAULT_STATE_FILE = Path(__file__).with_name("deposition_gantry_state.json")


class GantryError(RuntimeError):
    pass


class GantryTimeout(TimeoutError):
    pass


@dataclass(frozen=True)
class SerialPortInfo:
    device: str
    description: str


@dataclass
class GantryPosition:
    x: float = 0.0
    y: float = 0.0
    known: bool = True


class GantryController:
    """Serial wrapper around the Arduino XY gantry firmware."""

    # Open the serial connection and sync saved Python position to Arduino.
    def __init__(
        self,
        port: str,
        baudrate: int = BAUDRATE,
        timeout: float = 60.0,
        serial_timeout: float = 0.2,
        startup_wait: float = 2.0,
        state_file: str | Path | None = DEFAULT_STATE_FILE,
        logger: Callable[[str], None] | None = None,
    ):
        import serial

        self.timeout = timeout
        self.logger = logger
        self.state_file = Path(state_file) if state_file is not None else None
        self.position_state = self._load_state()
        self.serial = serial.Serial(port, baudrate, timeout=serial_timeout)
        time.sleep(startup_wait)
        self.drain()
        self._sync_position_to_arduino()

    # Enter context-manager use for automatic close.
    def __enter__(self) -> "GantryController":
        return self

    # Close the serial connection when leaving a context block.
    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    # Close the Arduino serial port.
    def close(self) -> None:
        self.serial.close()

    # Clear startup serial output from the controller.
    def drain(self) -> list[str]:
        lines: list[str] = []
        end = time.time() + 1.0
        while time.time() < end:
            raw = self.serial.readline()
            if not raw:
                break

            line = raw.decode(errors="replace").strip()
            if line:
                lines.append(line)
                self._log(f"<< {line}")

        return lines

    # Read serial output for a fixed number of seconds.
    def read_for(self, seconds: float) -> list[str]:
        lines: list[str] = []
        end = time.time() + seconds
        while time.time() < end:
            raw = self.serial.readline()
            if not raw:
                continue

            line = raw.decode(errors="replace").strip()
            if line:
                lines.append(line)
                self._log(f"<< {line}")

        return lines

    # Send one command and optionally wait for an OK response.
    def send(self, command: str, wait_ok: bool = True) -> list[str]:
        text = command.strip()
        if not text:
            return []

        self._log(f">> {text}")
        self.serial.write((text + "\n").encode("ascii"))
        self.serial.flush()

        if not wait_ok:
            return []

        return self._wait_for_ok(text)

    # Execute one raw command and update Python-side position when possible.
    def execute(self, command: str) -> list[str]:
        text = command.strip()
        normalized = self._strip_module_prefix(text)
        upper = normalized.upper()

        if upper == "POS":
            self._log(self.local_position_text())
            return [self.local_position_text()]
        if text.upper() == "STOP":
            self.stop()
            return []

        self._require_known_position_for_command(text)
        lines = self.send(text)
        self._apply_successful_command(text)
        return lines

    # Set the current physical position as local X0 Y0.
    def set_zero(self) -> list[str]:
        lines = self.send("GANTRY SETZERO")
        self._set_local_position(0.0, 0.0, known=True)
        return lines

    # Alias for set_zero.
    def home(self) -> list[str]:
        return self.set_zero()

    # Return the saved Python-side gantry position.
    def position(self) -> GantryPosition:
        return GantryPosition(self.position_state.x, self.position_state.y, self.position_state.known)

    # Format the saved Python-side position for terminal output.
    def local_position_text(self) -> str:
        status = "known" if self.position_state.known else "unknown"
        return f"PY_POS X{self.position_state.x:.3f} Y{self.position_state.y:.3f} status={status}"

    # Request gantry settings from Arduino.
    def settings(self) -> list[str]:
        return self.send("GANTRY SETTINGS")

    # Set gantry maximum speed.
    def set_speed(self, speed_mm_s: float) -> list[str]:
        return self.send(f"GANTRY SPEED S{speed_mm_s:g}")

    # Set gantry acceleration.
    def set_accel(self, accel_mm_s2: float) -> list[str]:
        return self.send(f"GANTRY ACCEL S{accel_mm_s2:g}")

    # Move relative to the saved current X/Y position.
    def move_relative(self, x: float | None = None, y: float | None = None) -> list[str]:
        self._require_known_position()
        parts: list[str] = []
        if x is not None:
            parts.append(f"X{x:g}")
        if y is not None:
            parts.append(f"Y{y:g}")
        if not parts:
            raise ValueError("move_relative requires x or y.")
        lines = self.send("GANTRY " + " ".join(parts))
        self._set_local_position(
            self.position_state.x + (x or 0.0),
            self.position_state.y + (y or 0.0),
            known=True,
        )
        return lines

    # Move to an absolute X/Y position.
    def goto(self, x: float | None = None, y: float | None = None) -> list[str]:
        self._require_known_position()
        parts = ["GOTO"]
        if x is not None:
            parts.append(f"X{x:g}")
        if y is not None:
            parts.append(f"Y{y:g}")
        if len(parts) == 1:
            raise ValueError("goto requires x or y.")
        lines = self.send("GANTRY " + " ".join(parts))
        self._set_local_position(
            self.position_state.x if x is None else x,
            self.position_state.y if y is None else y,
            known=True,
        )
        return lines

    # Draw a circle around X/Y with the requested radius.
    def circle(self, x: float, y: float, radius: float, segments: int = 120, loops: int = 1) -> list[str]:
        self._require_known_position()
        lines = self.send(f"GANTRY CIRCLE X{x:g} Y{y:g} R{radius:g} N{segments} L{loops}")
        self._set_local_position(x + radius, y, known=True)
        return lines

    # Draw a circle while dispensing E1 on the Arduino side.
    def deposit(self, x: float, y: float, radius: float, volume_ml: float, segments: int = 120, loops: int = 1) -> list[str]:
        self._require_known_position()
        lines = self.send(f"GANTRY DEPOSIT X{x:g} Y{y:g} R{radius:g} V{volume_ml:g} N{segments} L{loops}")
        self._set_local_position(x + radius, y, known=True)
        return lines

    # Set mixer pump speed for Z/E1 pump commands.
    def set_pump_speed(self, speed_mm_s: float) -> list[str]:
        return self.send(f"PUMP SPEED S{speed_mm_s:g}")

    # Move one small mixer syringe by ml.
    def pump_small(self, motor: str, ml: float) -> list[str]:
        return self.send(f"PUMP {self._pump_motor_name(motor)}S{ml:g}")

    # Move Z and E1 small mixer syringes at the same time.
    def pump_pair_small(self, motor_a: str, ml_a: float, motor_b: str, ml_b: float) -> list[str]:
        return self.send(
            f"PUMP PAIR {self._pump_motor_name(motor_a)}S{ml_a:g} "
            f"{self._pump_motor_name(motor_b)}S{ml_b:g}"
        )

    # Move one large syringe by ml for calibration or debugging.
    def pump_large(self, motor: str, ml: float) -> list[str]:
        return self.send(f"PUMP {self._pump_motor_name(motor)}L{ml:g}")

    # Move one pump by raw plunger travel in mm.
    def pump_mm(self, motor: str, mm: float) -> list[str]:
        return self.send(f"PUMP {self._pump_motor_name(motor)}M{mm:g}")

    # Request pump settings from Arduino.
    def pump_settings(self) -> list[str]:
        return self.send("PUMP SETTINGS")

    # Send emergency stop and mark Python position unknown.
    def stop(self) -> None:
        self.send("STOP", wait_ok=False)
        self._set_local_position(self.position_state.x, self.position_state.y, known=False)

    # Clear Arduino emergency stop state.
    def unlock(self) -> list[str]:
        return self.send("UNLOCK")

    # Set Arduino and Python-side logical position without moving.
    def set_position(self, x: float, y: float) -> list[str]:
        lines = self.send(f"GANTRY SETPOS X{x:g} Y{y:g}")
        self._set_local_position(x, y, known=True)
        return lines

    # Read serial lines until OK, an error line, or timeout.
    def _wait_for_ok(self, command: str) -> list[str]:
        lines: list[str] = []
        deadline = time.time() + self.timeout

        while time.time() < deadline:
            raw = self.serial.readline()
            if not raw:
                continue

            line = raw.decode(errors="replace").strip()
            if not line:
                continue

            lines.append(line)
            self._log(f"<< {line}")

            if line == "OK":
                return lines
            if "rejected" in line.lower() or line.startswith("Use "):
                raise GantryError(line)
            if "EMERGENCY STOP" in line:
                raise GantryError(line)

        raise GantryTimeout(f"Timed out waiting for OK after: {command}")

    # Print diagnostic output when a logger is configured.
    def _log(self, message: str) -> None:
        if self.logger is not None:
            self.logger(message)

    # Load saved Python-side gantry position from JSON.
    def _load_state(self) -> GantryPosition:
        if self.state_file is None or not self.state_file.exists():
            return GantryPosition()

        try:
            data = json.loads(self.state_file.read_text(encoding="utf-8"))
            return GantryPosition(
                x=float(data.get("x", 0.0)),
                y=float(data.get("y", 0.0)),
                known=bool(data.get("known", True)),
            )
        except (OSError, ValueError, TypeError):
            return GantryPosition()

    # Save Python-side gantry position to JSON.
    def _save_state(self) -> None:
        if self.state_file is None:
            return

        data = {
            "x": self.position_state.x,
            "y": self.position_state.y,
            "known": self.position_state.known,
        }
        self.state_file.write_text(json.dumps(data, indent=2), encoding="utf-8")

    # Update Python-side position and persist it.
    def _set_local_position(self, x: float, y: float, known: bool) -> None:
        self.position_state = GantryPosition(x=x, y=y, known=known)
        self._save_state()
        self._log(self.local_position_text())

    # Send saved Python position to Arduino after connecting.
    def _sync_position_to_arduino(self) -> None:
        if not self.position_state.known:
            self._log("PY_POS is unknown. Move to origin and send SETZERO before motion.")
            return

        self.send(f"GANTRY SETPOS X{self.position_state.x:g} Y{self.position_state.y:g}")

    # Update local position after raw Arduino commands that moved or reset gantry.
    def _apply_successful_command(self, command: str) -> None:
        command = self._strip_module_prefix(command)
        upper = command.strip().upper()

        # Zeroing and absolute-position commands replace the saved position.
        if upper in ("SETZERO", "HOME"):
            self._set_local_position(0.0, 0.0, known=True)
            return

        if upper.startswith("SETPOS"):
            x = self._extract_value(command, "X", self.position_state.x)
            y = self._extract_value(command, "Y", self.position_state.y)
            self._set_local_position(x, y, known=True)
            return

        if upper.startswith("GOTO"):
            x = self._extract_value(command, "X", self.position_state.x)
            y = self._extract_value(command, "Y", self.position_state.y)
            self._set_local_position(x, y, known=True)
            return

        # Circle and deposit end at the circle start point after one full loop.
        if upper.startswith("CIRCLE"):
            x = self._extract_value(command, "X", None)
            y = self._extract_value(command, "Y", None)
            radius = self._extract_value(command, "R", None)
            if x is not None and y is not None and radius is not None:
                self._set_local_position(x + radius, y, known=True)
            return

        if upper.startswith("DEPOSIT"):
            x = self._extract_value(command, "X", None)
            y = self._extract_value(command, "Y", None)
            radius = self._extract_value(command, "R", None)
            if x is not None and y is not None and radius is not None:
                self._set_local_position(x + radius, y, known=True)
            return

        # Relative X/Y moves update the saved position by adding deltas.
        x = self._extract_value(command, "X", None)
        y = self._extract_value(command, "Y", None)
        if x is not None or y is not None:
            self._set_local_position(
                self.position_state.x + (x or 0.0),
                self.position_state.y + (y or 0.0),
                known=True,
            )

    # Require known position before commands that depend on current X/Y.
    def _require_known_position_for_command(self, command: str) -> None:
        command = self._strip_module_prefix(command)
        upper = command.strip().upper()
        if upper in ("SETZERO", "HOME") or upper.startswith("SETPOS"):
            return
        if upper.startswith("SPEED") or upper.startswith("ACCEL") or upper == "SETTINGS":
            return
        if upper.startswith("UNLOCK"):
            return
        if upper.startswith("GOTO") or upper.startswith("CIRCLE") or upper.startswith("DEPOSIT"):
            self._require_known_position()
            return

        x = self._extract_value(command, "X", None)
        y = self._extract_value(command, "Y", None)
        if x is not None or y is not None:
            self._require_known_position()

    # Raise if Python-side gantry position is unknown.
    def _require_known_position(self) -> None:
        if not self.position_state.known:
            raise GantryError("Python position is unknown. Move to origin and send SETZERO before motion.")

    # Normalize and validate a mixer pump motor name.
    @staticmethod
    def _pump_motor_name(motor: str) -> str:
        normalized = motor.upper()
        if normalized not in ("Z", "E1"):
            raise ValueError("Pump motor must be Z or E1.")
        return normalized

    # Remove optional GANTRY/G prefixes from raw commands.
    @staticmethod
    def _strip_module_prefix(command: str) -> str:
        text = command.strip()
        upper = text.upper()
        if upper.startswith("GANTRY "):
            return text[7:].strip()
        if upper.startswith("G "):
            return text[2:].strip()
        return text

    # Extract a numeric field such as X30 or R10 from a command string.
    @staticmethod
    def _extract_value(command: str, key: str, default: float | None) -> float | None:
        upper = command.upper()
        index = upper.rfind(key)
        if index < 0:
            return default

        start = index + 1
        while start < len(command) and command[start] == " ":
            start += 1

        end = start
        while end < len(command) and command[end] in "0123456789-+.":
            end += 1

        if end == start:
            return default

        try:
            return float(command[start:end])
        except ValueError:
            return default


# List available serial ports for setup/debugging.
def list_serial_ports() -> list[SerialPortInfo]:
    from serial.tools import list_ports

    return [
        SerialPortInfo(device=port.device, description=port.description)
        for port in list_ports.comports()
    ]


# Build the gantry command-line parser.
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Arduino XY gantry controller.")
    parser.add_argument("--port", help="Arduino serial port, for example COM7.")
    parser.add_argument("--list-ports", action="store_true", help="List available serial ports and exit.")
    parser.add_argument("--manual", action="store_true", help="Interactive mode for one command at a time.")
    parser.add_argument("--send", help="Send one raw Arduino command, for example 'GOTO X30 Y30'.")
    parser.add_argument("--circle", nargs=3, type=float, metavar=("X", "Y", "R"), help="Draw a circle at X Y with radius R.")
    parser.add_argument("--deposit", nargs=4, type=float, metavar=("X", "Y", "R", "ML"), help="Draw circle while dispensing E1 small syringe volume.")
    parser.add_argument("--pump-small", nargs=2, metavar=("MOTOR", "ML"), help="Move small syringe pump, for example: --pump-small Z 0.1")
    parser.add_argument("--pump-pair-small", nargs=4, metavar=("MOTOR_A", "ML_A", "MOTOR_B", "ML_B"), help="Move Z/E1 small syringe pumps at the same time.")
    parser.add_argument("--pump-large", nargs=2, metavar=("MOTOR", "ML"), help="Move large syringe pump, for example: --pump-large E1 -0.1")
    parser.add_argument("--pump-mm", nargs=2, metavar=("MOTOR", "MM"), help="Move pump by plunger travel in mm.")
    parser.add_argument("--pump-speed", type=float, help="Set pump speed in mm/s before the command.")
    parser.add_argument("--segments", type=int, default=120, help="Circle segments.")
    parser.add_argument("--loops", type=int, default=1, help="Circle loops.")
    parser.add_argument("--speed", type=float, help="Set max XY speed in mm/s before the command.")
    parser.add_argument("--accel", type=float, help="Set XY acceleration in mm/s^2 before the command.")
    parser.add_argument("--setzero", action="store_true", help="Set current gantry position as X0 Y0 before the command.")
    return parser


# Run an interactive command loop for manual gantry testing.
def manual_mode(gantry: GantryController) -> None:
    print("Manual mode. Type commands like SETZERO, GOTO X30 Y30, CIRCLE X30 Y30 R10 N120 L1.")
    print("Type STOP for emergency stop. Type EXIT or QUIT to leave.")

    while True:
        try:
            text = input("gantry> ").strip()
        except EOFError:
            print()
            return

        if not text:
            continue

        upper = text.upper()
        if upper in ("EXIT", "QUIT"):
            return

        try:
            if upper == "STOP":
                gantry.stop()
            else:
                gantry.execute(text)
        except (GantryError, GantryTimeout) as exc:
            print(f"Error: {exc}")


# Apply optional speed, acceleration, zero, and pump-speed settings.
def configure_motion(gantry: GantryController, args: argparse.Namespace) -> None:
    if args.speed is not None:
        gantry.set_speed(args.speed)
    if args.accel is not None:
        gantry.set_accel(args.accel)
    if args.setzero:
        gantry.set_zero()
    if args.pump_speed is not None:
        gantry.set_pump_speed(args.pump_speed)


# Print detected serial ports.
def print_ports() -> None:
    ports = list_serial_ports()
    if not ports:
        print("No serial ports found.")
        return

    for port in ports:
        print(f"{port.device}: {port.description}")


# Parse CLI arguments, connect to the gantry, and run one requested action.
def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.list_ports:
        print_ports()
        return 0

    if not args.port:
        parser.error("--port is required unless --list-ports is used.")

    gantry = None
    try:
        gantry = GantryController(args.port, logger=print)
        configure_motion(gantry, args)

        # Select exactly one action from the CLI options.
        if args.manual:
            manual_mode(gantry)
            return 0

        if args.send:
            gantry.execute(args.send)
            return 0

        if args.circle:
            x, y, radius = args.circle
            gantry.circle(x, y, radius, segments=args.segments, loops=args.loops)
            return 0

        if args.deposit:
            x, y, radius, ml = args.deposit
            gantry.deposit(x, y, radius, ml, segments=args.segments, loops=args.loops)
            return 0

        if args.pump_small:
            motor, ml = args.pump_small
            gantry.pump_small(motor, float(ml))
            return 0

        if args.pump_pair_small:
            motor_a, ml_a, motor_b, ml_b = args.pump_pair_small
            gantry.pump_pair_small(motor_a, float(ml_a), motor_b, float(ml_b))
            return 0

        if args.pump_large:
            motor, ml = args.pump_large
            gantry.pump_large(motor, float(ml))
            return 0

        if args.pump_mm:
            motor, mm = args.pump_mm
            gantry.pump_mm(motor, float(mm))
            return 0

        parser.error("Use --manual, --send, --circle, --deposit, --pump-small, --pump-pair-small, --pump-large, --pump-mm, or --list-ports.")
    except KeyboardInterrupt:
        print("\nKeyboardInterrupt: sending STOP.")
        if gantry is not None:
            try:
                gantry.stop()
            except Exception:
                pass
        return 130
    except (GantryError, GantryTimeout) as exc:
        print(f"Error: {exc}")
        return 1
    finally:
        if gantry is not None:
            gantry.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())

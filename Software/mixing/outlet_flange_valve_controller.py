from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable


BAUDRATE = 115200
DEFAULT_PORT = "COM8"
DEFAULT_STATE_FILE = Path(__file__).with_name("outlet_flange_valve_state.json")

LOW = "low"
HIGH = "high"
UNKNOWN = "unknown"
MIX_1 = "1"
MIX_2 = "2"
MIX_3 = "3"
MIX_STATES = {MIX_1, MIX_2, MIX_3}


class FlangeError(RuntimeError):
    pass


class FlangeTimeout(TimeoutError):
    pass


@dataclass
class FlangeState:
    height: str = LOW
    known: bool = True
    mix_valve_state: str = UNKNOWN
    mix_valve_known: bool = False


class FlangeController:
    # Open the serial connection and load saved flange/valve state.
    def __init__(
        self,
        port: str = DEFAULT_PORT,
        baudrate: int = BAUDRATE,
        timeout: float = 60.0,
        serial_timeout: float = 0.2,
        startup_wait: float = 2.0,
        state_file: str | Path | None = DEFAULT_STATE_FILE,
        logger: Callable[[str], None] | None = print,
    ):
        import serial

        self.timeout = timeout
        self.logger = logger
        self.state_file = Path(state_file) if state_file is not None else None
        self.state = self._load_state()
        self.serial = serial.Serial(port, baudrate, timeout=serial_timeout)
        time.sleep(startup_wait)
        self.drain()

    # Enter context-manager use for automatic close.
    def __enter__(self) -> "FlangeController":
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

    # Move the outlet tube to the high position if the current state is known.
    def up(self) -> list[str]:
        self._require_known()
        if self.state.height == HIGH:
            raise FlangeError("Already high. Refusing to move up again.")

        try:
            lines = self.send("UP")
        except Exception:
            self._set_state(UNKNOWN, known=False)
            raise

        self._set_state(HIGH, known=True)
        return lines

    # Move the outlet tube to the low position if the current state is known.
    def down(self) -> list[str]:
        self._require_known()
        if self.state.height == LOW:
            raise FlangeError("Already low. Refusing to move down again.")

        try:
            lines = self.send("DOWN")
        except Exception:
            self._set_state(UNKNOWN, known=False)
            raise

        self._set_state(LOW, known=True)
        return lines

    # Move the mixing valve to stage 1.
    def mix_valve_1(self) -> list[str]:
        return self.move_mix_valve(MIX_1)

    # Move the mixing valve to stage 2.
    def mix_valve_2(self) -> list[str]:
        return self.move_mix_valve(MIX_2)

    # Move the mixing valve to stage 3.
    def mix_valve_3(self) -> list[str]:
        return self.move_mix_valve(MIX_3)

    # Route between known mixing valve stages using calibrated motor moves.
    def move_mix_valve(self, target: str) -> list[str]:
        target = str(target).strip()
        if target not in MIX_STATES:
            raise FlangeError("Mixing valve target must be 1, 2, or 3.")

        self._require_mix_valve_known()
        current = self.state.mix_valve_state
        if current == target:
            self._log(f"PY_STATE mix_valve={target} status=known")
            return []

        # Apply one or two rotary moves depending on the current-to-target route.
        route = self._mix_valve_route(current, target)
        lines: list[str] = []
        try:
            for command in route:
                lines.extend(self.send(command))
        except Exception:
            self._set_mix_valve_state(UNKNOWN, known=False)
            raise

        self._set_mix_valve_state(target, known=True)
        return lines

    # Stop motion and mark saved states as unknown.
    def stop(self) -> None:
        self.send("STOP", wait_ok=False)
        self._set_state(UNKNOWN, known=False)
        self._set_mix_valve_state(UNKNOWN, known=False)

    # Enable motor power after a stop.
    def enable(self) -> None:
        self.send("ENABLE", wait_ok=False)

    # Alias for enabling motor power.
    def on(self) -> None:
        self.send("ON", wait_ok=False)

    # Disable motor power without changing the saved state.
    def off(self) -> None:
        self.send("OFF", wait_ok=False)

    # Clear the Arduino emergency stop state.
    def unlock(self) -> list[str]:
        return self.send("UNLOCK")

    # Print Python state and request Arduino status.
    def status(self) -> None:
        known = "known" if self.state.known else "unknown"
        print(f"PY_STATE height={self.state.height} status={known}")
        valve_known = "known" if self.state.mix_valve_known else "unknown"
        print(f"PY_STATE mix_valve={self.state.mix_valve_state} status={valve_known}")
        self.send("STATUS")

    # Request Arduino motion settings.
    def settings(self) -> list[str]:
        return self.send("SETTINGS")

    # Request a simple status response.
    def ping(self) -> list[str]:
        return self.send("STATUS")

    # Mark the outlet tube as physically low without moving it.
    def set_low(self) -> None:
        self._set_state(LOW, known=True)

    # Mark the outlet tube as physically high without moving it.
    def set_high(self) -> None:
        self._set_state(HIGH, known=True)

    # Mark the mixing valve as physically at stage 1.
    def set_mix_valve_1(self) -> None:
        self._set_mix_valve_state(MIX_1, known=True)

    # Mark the mixing valve as physically at stage 2.
    def set_mix_valve_2(self) -> None:
        self._set_mix_valve_state(MIX_2, known=True)

    # Mark the mixing valve as physically at stage 3.
    def set_mix_valve_3(self) -> None:
        self._set_mix_valve_state(MIX_3, known=True)

    # Dispatch one text command to the matching controller method.
    def execute(self, command: str) -> None:
        text = command.strip().lower()
        if text == "up":
            self.up()
        elif text == "down":
            self.down()
        elif text == "stop":
            self.stop()
        elif text == "enable":
            self.enable()
        elif text == "on":
            self.on()
        elif text == "off":
            self.off()
        elif text == "unlock":
            self.unlock()
        elif text == "status":
            self.status()
        elif text == "settings":
            self.settings()
        elif text == "set-low":
            self.set_low()
        elif text == "set-high":
            self.set_high()
        elif text in {"mix-1", "mix1", "valve-1", "valve1"}:
            self.mix_valve_1()
        elif text in {"mix-2", "mix2", "valve-2", "valve2", "mixing"}:
            self.mix_valve_2()
        elif text in {"mix-3", "mix3", "valve-3", "valve3"}:
            self.mix_valve_3()
        elif text in {"set-mix-1", "set-valve-1"}:
            self.set_mix_valve_1()
        elif text in {"set-mix-2", "set-valve-2"}:
            self.set_mix_valve_2()
        elif text in {"set-mix-3", "set-valve-3"}:
            self.set_mix_valve_3()
        else:
            raise FlangeError("Unknown command.")

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

            lower = line.lower()
            if line == "OK":
                return lines
            if (
                "unknown command" in lower
                or "send help" in lower
                or "rejected" in lower
                or "stopped." in lower
                or "emergency stop" in lower
                or line.startswith("Use ")
            ):
                raise FlangeError(line)

        raise FlangeTimeout(f"Timed out waiting for OK after: {command}")

    # Load saved Python-side flange and mixing valve state.
    def _load_state(self) -> FlangeState:
        if self.state_file is None or not self.state_file.exists():
            return FlangeState()

        try:
            data = json.loads(self.state_file.read_text(encoding="utf-8"))
            height = str(data.get("height", LOW))
            known = bool(data.get("known", height in {LOW, HIGH}))
            mix_valve_state = str(data.get("mix_valve_state", UNKNOWN))
            mix_valve_known = bool(data.get("mix_valve_known", mix_valve_state in MIX_STATES))
            if height not in {LOW, HIGH, UNKNOWN}:
                height = UNKNOWN
                known = False
            if mix_valve_state not in MIX_STATES | {UNKNOWN}:
                mix_valve_state = UNKNOWN
                mix_valve_known = False
            return FlangeState(
                height=height,
                known=known,
                mix_valve_state=mix_valve_state,
                mix_valve_known=mix_valve_known,
            )
        except (OSError, ValueError, TypeError):
            return FlangeState(UNKNOWN, known=False)

    # Save Python-side flange and mixing valve state.
    def _save_state(self) -> None:
        if self.state_file is None:
            return
        data = {
            "height": self.state.height,
            "known": self.state.known,
            "mix_valve_state": self.state.mix_valve_state,
            "mix_valve_known": self.state.mix_valve_known,
        }
        self.state_file.write_text(json.dumps(data, indent=2), encoding="utf-8")

    # Update saved outlet tube height state.
    def _set_state(self, height: str, known: bool) -> None:
        self.state.height = height
        self.state.known = known
        self._save_state()
        status = "known" if known else "unknown"
        self._log(f"PY_STATE height={height} status={status}")

    # Update saved mixing valve stage state.
    def _set_mix_valve_state(self, mix_valve_state: str, known: bool) -> None:
        self.state.mix_valve_state = mix_valve_state
        self.state.mix_valve_known = known
        self._save_state()
        status = "known" if known else "unknown"
        self._log(f"PY_STATE mix_valve={mix_valve_state} status={status}")

    # Require the outlet tube height to be known before relative motion.
    def _require_known(self) -> None:
        if not self.state.known or self.state.height not in {LOW, HIGH}:
            raise FlangeError("Python height is unknown. Confirm the flange, then run set-low or set-high.")

    # Require the mixing valve stage to be known before stage changes.
    def _require_mix_valve_known(self) -> None:
        if not self.state.mix_valve_known or self.state.mix_valve_state not in MIX_STATES:
            raise FlangeError("Python mixing valve state is unknown. Confirm the valves, then run set-mix-1, set-mix-2, or set-mix-3.")

    # Return calibrated Arduino commands for a stage-to-stage valve move.
    def _mix_valve_route(self, current: str, target: str) -> list[str]:
        routes = {
            (MIX_1, MIX_2): ["MIX_X_CW90"],
            (MIX_2, MIX_1): ["MIX_X_CCW90"],
            (MIX_2, MIX_3): ["MIX_Y_CW180"],
            (MIX_3, MIX_2): ["MIX_Y_CCW180"],
            (MIX_1, MIX_3): ["MIX_X_CW90", "MIX_Y_CW180"],
            (MIX_3, MIX_1): ["MIX_Y_CCW180", "MIX_X_CCW90"],
        }
        try:
            return routes[(current, target)]
        except KeyError as exc:
            raise FlangeError(f"Unsupported mixing valve transition: {current} -> {target}") from exc

    # Print diagnostic output when a logger is configured.
    def _log(self, message: str) -> None:
        if self.logger is not None:
            self.logger(message)


# Run an interactive command loop for manual valve testing.
def manual_mode(controller: FlangeController) -> None:
    print("Manual mode. Commands: up, down, mix-1, mix-2, mix-3, stop, enable, on, off, unlock, status, settings, set-low, set-high, set-mix-1, set-mix-2, set-mix-3.")
    print("Type stop for immediate motor power off. Type quit to leave.")

    while True:
        try:
            text = input("flange> ").strip()
        except EOFError:
            print()
            return

        if not text:
            continue

        if text.lower() in {"quit", "exit", "q"}:
            return

        try:
            controller.execute(text)
        except (FlangeError, FlangeTimeout) as exc:
            print(f"Error: {exc}")


# Build the selector and mixing valve command-line parser.
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="RAMPS Z flange controller.")
    parser.add_argument(
        "command",
        nargs="?",
        choices=[
            "up",
            "down",
            "stop",
            "enable",
            "on",
            "off",
            "unlock",
            "status",
            "settings",
            "set-low",
            "set-high",
            "mix-1",
            "mix-2",
            "mix-3",
            "mixing",
            "set-mix-1",
            "set-mix-2",
            "set-mix-3",
            "manual",
            "interactive",
        ],
        help="Command to run.",
    )
    parser.add_argument("--port", default=DEFAULT_PORT, help=f"Serial port. Default: {DEFAULT_PORT}")
    return parser


# Parse CLI arguments, connect to the controller, and run one command.
def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.command is None:
        parser.error("command is required")

    controller = None
    try:
        controller = FlangeController(args.port)

        if args.command in {"manual", "interactive"}:
            manual_mode(controller)
        else:
            controller.execute(args.command)

        return 0
    except KeyboardInterrupt:
        print("\nKeyboardInterrupt: sending STOP.")
        if controller is not None:
            try:
                controller.stop()
            except Exception:
                pass
        return 130
    except (FlangeError, FlangeTimeout) as exc:
        print(f"Error: {exc}")
        return 1
    finally:
        if controller is not None:
            controller.close()


if __name__ == "__main__":
    sys.exit(main())

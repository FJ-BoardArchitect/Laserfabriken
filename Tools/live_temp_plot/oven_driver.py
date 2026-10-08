"""Serial wrapper for the crystal TEC / PID oven controller.

Talks to the board's text command interface over USB serial. Laser-related
commands are intentionally omitted since this rig doesn't use them.
"""

import re
import time

import serial

_CRYSTAL_RE = re.compile(
    r"Crystal TEC: T=([-\d.]+) C\s+SP=\s*([-\d.]+) C\s+WL=([-\d.]+) nm\s+"
    r"k=([-\d.]+) nm/C m=([-\d.]+) nm\s+out=([-\d.]+)\s+"
    r"Kp=([-\d.]+)\s+Ki=([-\d.]+)\s+Kd=([-\d.]+)"
)
_BUCK_RE = re.compile(r"TEC buck:\s*(.+)")


class OvenDriver:
    def __init__(self, port, baudrate=115200, timeout=1.0, boot_delay=2.0):
        self.ser = serial.Serial(port, baudrate=baudrate, timeout=timeout)
        # Many USB-serial boards reset when the port is opened; give the
        # firmware time to boot before we start sending commands.
        time.sleep(boot_delay)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _send(self, cmd):
        # Read up to the firmware's "> " prompt instead of sleeping a fixed
        # amount — lets status polling run as fast as the round trip allows.
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\n").encode())
        raw = self.ser.read_until(b"> ").decode(errors="replace")
        lines = [
            line.rstrip("\r") for line in raw.split("\n")
            if line.strip() and line.strip() != ">"
        ]
        return lines

    # --- crystal PID ---
    def set_kp(self, val):
        return self._send(f"set crystal.kp {val}")

    def set_ki(self, val):
        return self._send(f"set crystal.ki {val}")

    def set_kd(self, val):
        return self._send(f"set crystal.kd {val}")

    def set_setpoint(self, temp_c):
        return self._send(f"set crystal.setpoint {temp_c}")

    def set_wavelength(self, wavelength_nm):
        return self._send(f"set crystal.wavelength {wavelength_nm}")

    def set_k(self, val):
        return self._send(f"set crystal.k {val}")

    def set_m(self, val):
        return self._send(f"set crystal.m {val}")

    # --- power / TEC control ---
    def tec_on(self):
        return self._send("crystal.tec on")

    def tec_off(self):
        return self._send("crystal.tec off")

    def five_v_on(self):
        return self._send("5v on")

    def five_v_off(self):
        return self._send("5v off")

    # --- misc ---
    def status(self):
        return self._send("status")

    def get_crystal_status(self, retries=5, retry_delay=0.05):
        """Parse the crystal TEC fields out of `status`. No crystal current
        readback exists in this firmware — only laser.current is exposed.

        The board's UART occasionally garbles a byte or two on a status
        response under fast repeated polling, so a failed parse is retried
        a few times before giving up."""
        lines = None
        for _ in range(retries):
            lines = self.status()
            match = _CRYSTAL_RE.search("\n".join(lines))
            if match:
                break
            time.sleep(retry_delay)
        else:
            raise RuntimeError(
                f"Could not parse crystal status after {retries} attempts: {lines!r}"
            )

        text = "\n".join(lines)
        (temp_c, setpoint_c, wavelength_nm, k, m, output,
         kp, ki, kd) = (float(g) for g in match.groups())

        buck_match = _BUCK_RE.search(text)
        buck_message = buck_match.group(1).strip() if buck_match else None

        return {
            "temp_c": temp_c,
            "setpoint_c": setpoint_c,
            "wavelength_nm": wavelength_nm,
            "k": k,
            "m": m,
            "output": output,
            "kp": kp,
            "ki": ki,
            "kd": kd,
            "buck_fault": bool(buck_message and "FAULT" in buck_message),
            "buck_message": buck_message,
        }

    def get_temperature(self):
        return self.get_crystal_status()["temp_c"]

    def save(self):
        return self._send("save")

    def help(self):
        return self._send("help")

import serial
import serial.tools.list_ports

def list_serial_ports():
    """Lists all available serial ports."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        print(f"Port: {port.device}, Description: {port.description}, HWID: {port.hwid}")

def find_device_by_description(description_keyword):
    """Finds a device by matching its description."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if description_keyword.lower() in port.description.lower():
            return port.device
    return None

def find_device_by_pid(pid, exclude=[], prefered_device=''):
    """Finds a device by matching its description."""
    ports = serial.tools.list_ports.comports()
    found_device = None
    for port in ports:
        if pid in port.hwid:
            if port.device in exclude:
                continue
            found_device = port.device
            if found_device == prefered_device:
                break
    return found_device
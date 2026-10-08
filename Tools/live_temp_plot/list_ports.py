"""List available serial ports, to help identify the oven controller's port."""

from serial.tools import list_ports


def main():
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return

    print("Available serial ports:")
    for p in ports:
        print(f"  {p.device}  -  {p.description}")


if __name__ == "__main__":
    main()

import sys
import time
import traceback
import serial
import json
from serial.threaded import LineReader
from serial.threaded import ReaderThread
# import serial_handler

class PrintLines(LineReader):
    # LineReader overloaded methods
    def __init__(self):
        super().__init__()
        self.TERMINATOR = b'\r\n' # Note: Order of rn must match how data is sent!
        self.temperature = 0.0

    def connection_made(self, transport):
        super(PrintLines, self).connection_made(transport)
        sys.stdout.write('Serial port opened\n')

    def handle_line(self, data):
        str = repr(data).lstrip("\'").rstrip("\'")
        if str.startswith('Crystal TEC'):
            s = str.split()
            t = float(s[2].split('=')[1])
            out = float(s[12].split('=')[1])
            sys.stdout.write('{},{},{}\n'.format(self._tset, t, out))
        # elif str.startswith('OK'):
        #     sys.stdout.write('Receivec: {}\n'.format(str))

    def connection_lost(self, exc):
        if exc:
            traceback.print_exc(exc)
        sys.stdout.write('Serial port closed\n')

    # Custom methods
    def write_cmd(self, cmd):
        self.write_line(cmd)
        time.sleep(0.1)

    def set_temp(self, t):
        self._tset = t
        self.write_cmd(f'set crystal.setpoint {t}')

class SerialTest:
    def __init__(self, port):
        ser = serial.Serial(port)
        self.ser_thread = ReaderThread(ser, PrintLines)
        self.td = self.ser_thread.__enter__()

    def run_test(self, iterations=120, delay=1):
        self.td.write_cmd('set crystal.kp -0.1')
        self.td.set_temp(25.0)
        self.td.write_cmd('crystal.tec on')

        for i in range(iterations):
            if i == 30:
                self.td.set_temp(20)
            if i == 60:
                self.td.set_temp(40)
            self.td.write_cmd('status')
            time.sleep(delay)


        self.td.write_cmd('crystal.tec off')

    def close(self):
        self.ser_thread.close()

ser = SerialTest('/dev/ttyACM0')
ser.run_test()

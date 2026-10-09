#!/usr/bin/env python3
"""Talk to the firmware's CLI over SWD (SEGGER RTT via OpenOCD + ST-Link).

Needs only the ST-Link attached -- no USB-CDC, no UART. Debug builds only
(CLI_RTT, see CMakeLists.txt and Core/Src/app/rtt.c).

    rtt_cli.py "status"                      # one command, print reply
    rtt_cli.py "hello" "set laser.power 50"  # several in sequence
    rtt_cli.py --listen 5                    # just dump output for 5 s
    rtt_cli.py --interactive                 # type lines, Ctrl-D to quit

Starts its own OpenOCD; fails if another OpenOCD/st-flash holds the probe.
"""
import argparse, socket, subprocess, sys, time

OPENOCD = ["openocd", "-f", "interface/stlink.cfg", "-f", "target/stm32g4x.cfg",
           "-c", "init",
           "-c", 'rtt setup 0x20000000 0x8000 "SEGGER RTT"',
           "-c", "rtt start",
           "-c", "rtt polling_interval 20",
           "-c", "rtt server start 9090 0"]

def read_until_idle(sock, idle_s, max_s=10.0):
    sock.settimeout(idle_s)
    out, start = b"", time.time()
    while time.time() - start < max_s:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            out += chunk
        except socket.timeout:
            break
    return out.decode("utf-8", "replace")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("commands", nargs="*")
    ap.add_argument("--listen", type=float, metavar="SECONDS")
    ap.add_argument("--interactive", action="store_true")
    ap.add_argument("--idle", type=float, default=0.4, help="reply considered complete after this many idle seconds")
    a = ap.parse_args()

    oocd = subprocess.Popen(OPENOCD, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        sock = None
        for _ in range(50):
            if oocd.poll() is not None:
                sys.stderr.write(oocd.stderr.read().decode(errors="replace"))
                sys.exit("openocd exited")
            try:
                sock = socket.create_connection(("127.0.0.1", 9090), timeout=0.2)
                break
            except OSError:
                time.sleep(0.1)
        if sock is None:
            sys.exit("could not connect to openocd rtt server")

        # whatever the firmware printed before we attached (banner etc.)
        pending = read_until_idle(sock, 0.3, 1.0)
        if pending and not a.commands and not a.interactive:
            sys.stdout.write(pending)

        if a.listen:
            sys.stdout.write(read_until_idle(sock, a.listen, a.listen)); return
        if a.interactive:
            sys.stdout.write(pending)
            for line in sys.stdin:
                sock.sendall(line.rstrip("\r\n").encode() + b"\n")
                sys.stdout.write(read_until_idle(sock, a.idle)); sys.stdout.flush()
            return
        for cmd in a.commands:
            sock.sendall(cmd.encode() + b"\n")
            reply = read_until_idle(sock, a.idle)
            sys.stdout.write(f"--- {cmd}\n{reply}\n")
    finally:
        oocd.terminate()
        try: oocd.wait(2)
        except subprocess.TimeoutExpired: oocd.kill()

if __name__ == "__main__":
    main()

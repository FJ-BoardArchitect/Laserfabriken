#!/usr/bin/env python3
"""Talk to the firmware's CLI over SWD (SEGGER RTT via OpenOCD + ST-Link).

Needs only the ST-Link attached -- no USB-CDC, no UART. Debug builds only
(CLI_RTT, see CMakeLists.txt and Core/Src/app/rtt.c).

    rtt_cli.py "status"                      # one command, print reply
    rtt_cli.py "hello" "set laser.power 50"  # several in sequence
    rtt_cli.py --listen 5                    # just dump output for 5 s
    rtt_cli.py --interactive                 # type lines, Ctrl-D to quit
    rtt_cli.py --server                      # keep OpenOCD + RTT server up (port 9090)
    rtt_cli.py --watch                       # stream everything the firmware prints, forever

Several terminals at once: run `--server` in one terminal and leave it. Every
other invocation (and `nc 127.0.0.1 9090`) then attaches to that server
instead of starting its own OpenOCD, and all of them see the same output.
Without a server running, each invocation starts a private OpenOCD; that
fails if another OpenOCD/st-flash holds the probe.
"""
import argparse, socket, subprocess, sys, time

OPENOCD = ["openocd", "-f", "interface/stlink.cfg", "-f", "target/stm32g4x.cfg",
           "-c", "init",
           "-c", 'rtt setup 0x20000000 0x8000 "SEGGER RTT"',
           "-c", "rtt start",
           "-c", "rtt polling_interval 20",
           "-c", "rtt server start 9090 0"]

def read_until_idle(sock, idle_s, max_s=20.0, stop_at_prompt=True):
    """Collect output until the CLI prompt arrives, or nothing arrives for idle_s."""
    sock.settimeout(idle_s)
    out, start = b"", time.time()
    while time.time() - start < max_s:
        if stop_at_prompt and out.endswith(b"> "):
            break
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
    ap.add_argument("--watch", action="store_true", help="print everything the firmware sends until Ctrl-C")
    ap.add_argument("--server", action="store_true", help="run OpenOCD with the RTT server until Ctrl-C; other terminals attach")
    ap.add_argument("--port", type=int, default=9090)
    ap.add_argument("--idle", type=float, default=0.4, help="reply considered complete after this many idle seconds")
    a = ap.parse_args()

    # Attach to a server another terminal already runs, if there is one.
    sock = None
    oocd = None
    try:
        sock = socket.create_connection(("127.0.0.1", a.port), timeout=0.2)
    except OSError:
        pass
    if a.server:
        if sock is not None:
            sys.exit(f"an RTT server already listens on port {a.port}")
        cmd = OPENOCD[:-1] + [f"rtt server start {a.port} 0"]
        print(f"OpenOCD + RTT server on 127.0.0.1:{a.port}. In other terminals:\n"
              f"  python3 {sys.argv[0]} --watch       stream all output live\n"
              f"  python3 {sys.argv[0]} --interactive type commands, see replies\n"
              f"  python3 {sys.argv[0]} status        one-shot commands attach here\n"
              f"Ctrl-C to stop.", flush=True)
        import signal
        child = subprocess.Popen(cmd)
        def stop(*_):
            child.terminate()
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGHUP, stop)
        try:
            sys.exit(child.wait())
        except KeyboardInterrupt:
            child.terminate()
            try: child.wait(2)
            except subprocess.TimeoutExpired: child.kill()
            sys.exit(0)
    if sock is None:
        oocd = subprocess.Popen(OPENOCD[:-1] + [f"rtt server start {a.port} 0"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        for _ in range(50):
            if sock is not None:
                break
            if oocd.poll() is not None:
                sys.stderr.write(oocd.stderr.read().decode(errors="replace"))
                sys.exit("openocd exited")
            try:
                sock = socket.create_connection(("127.0.0.1", a.port), timeout=0.2)
                break
            except OSError:
                time.sleep(0.1)
        if sock is None:
            sys.exit("could not connect to openocd rtt server")

        # whatever the firmware printed before we attached (banner etc.)
        pending = read_until_idle(sock, 0.3, 1.0, stop_at_prompt=False)
        if pending and not a.commands and not a.interactive:
            sys.stdout.write(pending)

        if a.listen:
            sys.stdout.write(read_until_idle(sock, a.listen, a.listen, stop_at_prompt=False)); return
        if a.watch:
            sys.stdout.write(pending); sys.stdout.flush()
            sock.settimeout(None)
            try:
                while True:
                    data = sock.recv(4096)
                    if not data: break
                    sys.stdout.write(data.decode(errors="replace")); sys.stdout.flush()
            except KeyboardInterrupt:
                pass
            return
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
        if oocd is not None:
            oocd.terminate()
            try: oocd.wait(2)
            except subprocess.TimeoutExpired: oocd.kill()

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Step the crystal wavelength and plot how the TEC PID follows the setpoint.

Talks to the firmware over the shared RTT server (rtt_cli.py --server, port
9090): sets `crystal.wavelength`, polls `status` and plots, live, the crystal
temperature against the setpoint the wavelength maps to (T = (λ - m) / k),
plus the PID output and the measured TEC current. Writes a CSV and a PNG.

    wl_step_test.py                                   # default steps, see DEFAULT_STEPS
    wl_step_test.py --steps 1553.42:120,1554.39:180   # wavelength_nm:seconds, in order
    wl_step_test.py --temps 25:120,30:180,25:180      # same, but given as setpoints in C
    wl_step_test.py --no-tec                          # only log/plot, don't switch the TEC
    wl_step_test.py --plot run.csv                    # re-plot a saved run

The TEC is switched on at the start and off at the end (also on Ctrl-C).
Setpoints outside --tmin/--tmax are refused before anything is sent.

Needs: python3, matplotlib (pip install matplotlib), a Debug build on the
board and `python3 Tools/rtt-cli/rtt_cli.py --server` running.
"""
import argparse, csv, os, re, socket, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TEMPS = [(25.0, 120), (30.0, 240), (35.0, 240), (25.0, 300)]   # (C, seconds)

RE_CRYSTAL = re.compile(r"Crystal TEC: T=(-?[\d.]+) C\s+SP=(-?[\d.]+) C\s+WL=([\d.]+) nm\s+k=([\d.]+) nm/C m=([\d.]+) nm\s+out=(-?[\d.]+)")
RE_DRIVE   = re.compile(r"TEC drive: cmd=(-?[\d.]+) A.*?measured V=(-?[\d.]+) V\s+I=(-?[\d.]+) A\s+EN=(\d)\s+PG=(\d)")
FIELDS = ["t_s", "wl_set_nm", "sp_C", "T_C", "out", "cmd_A", "V", "I_A", "EN", "PG"]


class Rtt:
    def __init__(self, port):
        try:
            self.s = socket.create_connection(("127.0.0.1", port), timeout=1.0)
        except OSError:
            sys.exit(f"no RTT server on port {port}: start `python3 Tools/rtt-cli/rtt_cli.py --server` first")
        self.cmd("")   # flush whatever is pending

    def cmd(self, line, max_s=3.0):
        self.s.sendall((line + "\r\n").encode())
        buf, t0 = b"", time.time()
        self.s.settimeout(0.3)
        while time.time() - t0 < max_s and not buf.endswith(b"> "):
            try:
                chunk = self.s.recv(4096)
                if not chunk:
                    sys.exit("RTT server closed the connection")
                buf += chunk
            except socket.timeout:
                if buf:
                    break
        return buf.decode("utf-8", "replace")


def parse_status(text):
    c, d = RE_CRYSTAL.search(text), RE_DRIVE.search(text)
    if not c:
        return None
    row = {"T_C": float(c[1]), "sp_C": float(c[2]), "out": float(c[6]),
           "k": float(c[4]), "m": float(c[5])}
    if d:
        row.update(cmd_A=float(d[1]), V=float(d[2]), I_A=float(d[3]), EN=int(d[4]), PG=int(d[5]))
    return row


def parse_steps(spec, as_temp):
    out = []
    for item in spec.split(","):
        v, _, s = item.partition(":")
        out.append((float(v), float(s or 120)))
    return out, as_temp


class Plot:
    def __init__(self, live):
        import matplotlib
        if not live:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        self.plt = plt
        self.live = live
        self.fig, (self.a1, self.a2) = plt.subplots(2, 1, sharex=True, figsize=(11, 7),
                                                    gridspec_kw={"height_ratios": [3, 2]})
        self.lT,  = self.a1.plot([], [], label="T (NTC)", lw=1.6)
        self.lSP, = self.a1.plot([], [], label="setpoint (from λ)", lw=1.2, ls="--", drawstyle="steps-post")
        self.a1.set_ylabel("°C"); self.a1.grid(alpha=.3); self.a1.legend(loc="upper left")
        self.lO,  = self.a2.plot([], [], label="PID out (−1…1)", lw=1.2)
        self.lI,  = self.a2.plot([], [], label="TEC I measured (A)", lw=1.2)
        self.a2.set_xlabel("time (s)"); self.a2.grid(alpha=.3); self.a2.legend(loc="upper left")
        self.title = self.fig.suptitle("Crystal TEC: wavelength steps")
        self.fig.tight_layout()
        if live:
            plt.ion(); plt.show(block=False)

    def update(self, rows, note=""):
        t = [r["t_s"] for r in rows]
        self.lT.set_data(t, [r["T_C"] for r in rows])
        self.lSP.set_data(t, [r["sp_C"] for r in rows])
        self.lO.set_data(t, [r["out"] for r in rows])
        self.lI.set_data(t, [r.get("I_A") or 0.0 for r in rows])
        for a in (self.a1, self.a2):
            a.relim(); a.autoscale_view()
        if note:
            self.title.set_text(note)
        if self.live:
            self.fig.canvas.draw_idle(); self.plt.pause(0.01)

    def save(self, path):
        self.fig.savefig(path, dpi=120)


def summarize(rows, steps_t):
    """Per step: final error, time to get within 0.5 C, overshoot."""
    print("\nstep  SP(C)   final T   error   t(±0.5 C)   overshoot")
    for i, (t0, t1, sp) in enumerate(steps_t):
        seg = [r for r in rows if t0 <= r["t_s"] < t1]
        if not seg:
            continue
        T0, Tend = seg[0]["T_C"], seg[-1]["T_C"]
        within = next((r["t_s"] - t0 for r in seg if abs(r["T_C"] - sp) <= 0.5), None)
        rising = sp >= T0
        peak = max(r["T_C"] for r in seg) if rising else min(r["T_C"] for r in seg)
        over = (peak - sp) if rising else (sp - peak)
        print(f"{i+1:>4}  {sp:6.2f}   {Tend:7.2f}  {Tend-sp:+6.2f}   "
              f"{(f'{within:6.0f} s' if within is not None else '   never'):>10}   {max(over, 0):6.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--steps", help="wavelength_nm:seconds,... (in order)")
    g.add_argument("--temps", help="setpoint_C:seconds,... converted with the board's k and m")
    ap.add_argument("--rate", type=float, default=2.0, help="samples per second (default 2)")
    ap.add_argument("--tmin", type=float, default=10.0, help="refuse setpoints below this (C)")
    ap.add_argument("--tmax", type=float, default=45.0, help="refuse setpoints above this (C)")
    ap.add_argument("--no-tec", action="store_true", help="don't switch the crystal TEC on/off")
    ap.add_argument("--no-live", action="store_true", help="no window, only save PNG")
    ap.add_argument("--out", default=os.path.join(HERE, "runs"), help="directory for CSV and PNG")
    ap.add_argument("--port", type=int, default=9090)
    ap.add_argument("--plot", metavar="CSV", help="re-plot a saved run and exit")
    a = ap.parse_args()

    if a.plot:
        with open(a.plot) as f:
            rows = [{k: (float(v) if v not in ("", None) else None) for k, v in r.items()} for r in csv.DictReader(f)]
        p = Plot(live=not a.no_live)
        p.update(rows, os.path.basename(a.plot))
        png = os.path.splitext(a.plot)[0] + ".png"
        p.save(png); print(f"saved {png}")
        if not a.no_live:
            p.plt.ioff(); p.plt.show()
        return

    rtt = Rtt(a.port)
    first = parse_status(rtt.cmd("status"))
    if not first:
        sys.exit("could not parse 'status' (Debug build on the board?)")
    k, m = first["k"], first["m"]

    if a.steps:
        plan = [(wl, s) for wl, s in parse_steps(a.steps, False)[0]]
    else:
        temps = parse_steps(a.temps, True)[0] if a.temps else DEFAULT_TEMPS
        plan = [(round(m + k * t, 2), s) for t, s in temps]
    for wl, _ in plan:
        sp = (wl - m) / k
        if not (a.tmin <= sp <= a.tmax):
            sys.exit(f"{wl:.2f} nm -> {sp:.2f} C is outside {a.tmin}..{a.tmax} C; refusing (use --tmin/--tmax)")
    total = sum(s for _, s in plan)
    print(f"k={k} nm/C  m={m} nm   start T={first['T_C']:.2f} C")
    for wl, s in plan:
        print(f"  {wl:8.2f} nm  -> SP {(wl - m) / k:6.2f} C  for {s:.0f} s")
    print(f"total {total/60:.1f} min. Ctrl-C stops and switches the TEC off.")

    os.makedirs(a.out, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    csv_path = os.path.join(a.out, f"wl_steps_{stamp}.csv")
    live = not a.no_live and bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))
    plot = Plot(live)

    rows, steps_t = [], []
    t_start = time.time()
    try:
        if not a.no_tec:
            print(rtt.cmd("crystal.tec on").strip().splitlines()[-2:][0])
        with open(csv_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS, extrasaction="ignore")
            w.writeheader()
            for i, (wl, dwell) in enumerate(plan):
                rtt.cmd(f"set crystal.wavelength {wl:.2f}")
                sp = (wl - m) / k
                t0 = time.time() - t_start
                steps_t.append((t0, t0 + dwell, sp))
                note = f"step {i+1}/{len(plan)}: {wl:.2f} nm -> SP {sp:.2f} °C"
                print(note)
                while time.time() - t_start < t0 + dwell:
                    tick = time.time()
                    r = parse_status(rtt.cmd("status"))
                    if r:
                        r.update(t_s=round(tick - t_start, 2), wl_set_nm=wl)
                        rows.append(r); w.writerow(r); f.flush()
                        plot.update(rows, note)
                        print(f"\r  t={r['t_s']:7.1f}s  T={r['T_C']:7.3f}  SP={r['sp_C']:7.3f}  "
                              f"out={r['out']:+.3f}  I={r.get('I_A', 0):.3f} A  PG={r.get('PG', '-')}",
                              end="", flush=True)
                    time.sleep(max(0.0, 1.0 / a.rate - (time.time() - tick)))
                print()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        if not a.no_tec:
            rtt.cmd("crystal.tec off")
            print("crystal TEC off")
        png = os.path.splitext(csv_path)[0] + ".png"
        plot.save(png)
        print(f"saved {csv_path}\n      {png}")
        summarize(rows, steps_t)
        if live:
            plot.plt.ioff(); plot.plt.show()


if __name__ == "__main__":
    main()

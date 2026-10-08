"""P-only autotune sweep for the crystal TEC.

For each Kp in a sweep from KP_MIN to KP_MAX (Ki=Kd=0 throughout):
  - set setpoint to HIGH_TEMP_C, wait SETTLE_SECONDS, then record deviation
    from setpoint for MEASURE_SECONDS
  - set setpoint to LOW_TEMP_C, wait SETTLE_SECONDS, then record deviation
    from setpoint for MEASURE_SECONDS
  - score = average of (mean |deviation|) across the high and low phases

Lower score = better. Each Kp point gets its own live plot window (closes
automatically once that point finishes) and is saved as a PNG. At the end,
every point's traces are overlaid on shared per-phase time axes into one
combined image, color-coded by Kp, so convergence speed/stability can be
compared directly. Everything lands in pid_sweep_plots/<timestamp>/ along
with results.csv.

Pure P control cannot fully cancel steady-state error — this sweep is meant
to find a Kp that balances that offset against oscillation, as a starting
point before tuning Ki (to remove the offset) and then Kd.
"""

import csv
import time
from pathlib import Path
from statistics import mean

import matplotlib as mpl
import matplotlib.pyplot as plt
from matplotlib.cm import ScalarMappable
from matplotlib.colors import Normalize

from oven_driver import OvenDriver

PORT = "/dev/cu.usbmodem204D359731301"

LOW_TEMP_C = 20.0
HIGH_TEMP_C = 35.0
SETTLE_SECONDS = 30.0
MEASURE_SECONDS = 30.0
POLL_INTERVAL_S = 0.1

KP_MIN = 1
KP_MAX = 10
KP_STEPS = 10
KI = 0.0
KD = 0.0

# Sweep aborts and TEC is switched off immediately if the temperature ever
# leaves this band — the firmware has no built-in limit for the crystal side.
SAFETY_MIN_TEMP_C = 5.0
SAFETY_MAX_TEMP_C = 45.0

OUTPUT_ROOT = "pid_sweep_plots"

BACKGROUND_COLOR = "#E8720C"
ACTUAL_COLOR = "#FFFFFF"
SETPOINT_COLOR = "#F5C99B"
LINE_WIDTH = 2.5


class SafetyLimitExceeded(RuntimeError):
    pass


def style_axes(ax):
    ax.set_facecolor(BACKGROUND_COLOR)
    ax.set_xlabel("Time (s)", color=ACTUAL_COLOR)
    ax.set_ylabel("Temperature (°C)", color=ACTUAL_COLOR)
    ax.tick_params(colors=ACTUAL_COLOR)
    for spine in ax.spines.values():
        spine.set_color(BACKGROUND_COLOR)


def kp_sweep_values(kp_min, kp_max, steps):
    if steps == 1:
        return [kp_min]
    step_size = (kp_max - kp_min) / (steps - 1)
    return [kp_min + i * step_size for i in range(steps)]


def run_point(oven, kp, ki, kd, index, total, out_dir):
    """Run one Kp evaluation with a live plot window. Returns
    (result_dict, history) where history maps "high"/"low" to a list of
    (phase_relative_time, temp) samples covering settle + measure."""
    oven.set_kp(kp)
    oven.set_ki(ki)
    oven.set_kd(kd)

    fig, ax = plt.subplots()
    fig.patch.set_facecolor(BACKGROUND_COLOR)
    style_axes(ax)
    temp_line, = ax.plot([], [], color=ACTUAL_COLOR, linewidth=LINE_WIDTH,
                          label="Actual Temperature")
    sp_line, = ax.plot([], [], color=SETPOINT_COLOR, linewidth=LINE_WIDTH,
                        linestyle="--", label="Set Temperature")
    ax.set_title(f"Kp={kp:.4f}  ({index}/{total})", color=ACTUAL_COLOR)
    ax.legend(loc="upper right", frameon=False, labelcolor=ACTUAL_COLOR)

    fig.show()
    plt.pause(0.01)

    point_t0 = time.monotonic()
    times, temps, setpoints = [], [], []
    history = {"high": [], "low": []}
    phase_results = {}

    def run_segment(target_c, duration_s, phase_key, phase_t0, record_into):
        seg_t0 = time.monotonic()
        while time.monotonic() - seg_t0 < duration_s:
            temp = oven.get_crystal_status()["temp_c"]
            if not (SAFETY_MIN_TEMP_C <= temp <= SAFETY_MAX_TEMP_C):
                raise SafetyLimitExceeded(
                    f"temp {temp:.2f} C outside [{SAFETY_MIN_TEMP_C}, {SAFETY_MAX_TEMP_C}] "
                    f"while targeting {target_c} C"
                )
            now_point = time.monotonic() - point_t0
            now_phase = time.monotonic() - phase_t0

            times.append(now_point)
            temps.append(temp)
            setpoints.append(target_c)
            history[phase_key].append((now_phase, temp))
            if record_into is not None:
                record_into.append(temp - target_c)

            temp_line.set_data(times, temps)
            sp_line.set_data(times, setpoints)
            ax.relim()
            ax.autoscale_view()
            plt.pause(POLL_INTERVAL_S)

    try:
        for target_c, phase_key in [(HIGH_TEMP_C, "high"), (LOW_TEMP_C, "low")]:
            oven.set_setpoint(target_c)
            phase_t0 = time.monotonic()
            run_segment(target_c, SETTLE_SECONDS, phase_key, phase_t0, record_into=None)

            measure_start_now = time.monotonic() - point_t0
            ax.axvspan(measure_start_now, measure_start_now + MEASURE_SECONDS,
                       color="white", alpha=0.08, lw=0)

            deviations = []
            run_segment(target_c, MEASURE_SECONDS, phase_key, phase_t0, record_into=deviations)

            phase_results[phase_key] = {
                "target_c": target_c,
                "mean_dev": mean(deviations),
                "mean_abs_dev": mean(abs(d) for d in deviations),
                "rms_dev": mean(d * d for d in deviations) ** 0.5,
                "max_abs_dev": max(abs(d) for d in deviations),
                "n_samples": len(deviations),
            }
    finally:
        fig.savefig(out_dir / f"kp_{index:02d}_{kp:.4f}.png", dpi=150,
                    facecolor=fig.get_facecolor())
        plt.close(fig)

    score = (phase_results["high"]["mean_abs_dev"] + phase_results["low"]["mean_abs_dev"]) / 2
    result = {"kp": kp, "ki": ki, "kd": kd,
              "high": phase_results["high"], "low": phase_results["low"], "score": score}
    return result, history


def save_combined_overlay(all_history, kp_values, out_dir):
    fig, (ax_high, ax_low) = plt.subplots(2, 1, figsize=(9, 9))
    fig.patch.set_facecolor(BACKGROUND_COLOR)
    for ax in (ax_high, ax_low):
        style_axes(ax)

    norm = Normalize(vmin=min(kp_values), vmax=max(kp_values))
    cmap = mpl.colormaps["viridis"]

    for kp in kp_values:
        color = cmap(norm(kp))
        for ax, phase_key in [(ax_high, "high"), (ax_low, "low")]:
            series = all_history[kp][phase_key]
            if not series:
                continue
            t_vals = [t for t, _ in series]
            temp_vals = [v for _, v in series]
            ax.plot(t_vals, temp_vals, color=color, linewidth=1.8)

    for ax, target_c, label in [(ax_high, HIGH_TEMP_C, "High"), (ax_low, LOW_TEMP_C, "Low")]:
        ax.axhline(target_c, color=SETPOINT_COLOR, linestyle="--", linewidth=1.5)
        ax.axvspan(SETTLE_SECONDS, SETTLE_SECONDS + MEASURE_SECONDS,
                   color="white", alpha=0.08, lw=0)
        ax.set_title(f"{label} phase (target {target_c:.0f} °C)", color=ACTUAL_COLOR)

    sm = ScalarMappable(norm=norm, cmap=cmap)
    sm.set_array([])
    cbar = fig.colorbar(sm, ax=[ax_high, ax_low])
    cbar.set_label("Kp", color=ACTUAL_COLOR)
    cbar.ax.tick_params(colors=ACTUAL_COLOR)
    cbar.outline.set_edgecolor(ACTUAL_COLOR)

    fig.savefig(out_dir / "combined_overlay.png", dpi=150, facecolor=fig.get_facecolor())
    plt.close(fig)


def main():
    kp_values = kp_sweep_values(KP_MIN, KP_MAX, KP_STEPS)
    cycle_s = SETTLE_SECONDS + MEASURE_SECONDS
    est_total_min = (KP_STEPS * 2 * cycle_s) / 60
    print(f"Sweeping {KP_STEPS} Kp values from {KP_MIN} to {KP_MAX} (Ki={KI}, Kd={KD}).")
    print(f"Estimated total time: {est_total_min:.1f} min.")

    out_dir = Path(OUTPUT_ROOT) / time.strftime("%Y%m%d_%H%M%S")
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"Output folder: {out_dir}")

    oven = OvenDriver(PORT)
    initial = oven.get_crystal_status()
    if initial["buck_fault"]:
        print(f"WARNING: TEC buck reports '{initial['buck_message']}' — proceeding anyway.")

    plt.ion()

    results = []
    all_history = {}
    csv_file = open(out_dir / "results.csv", "w", newline="")
    writer = csv.writer(csv_file)
    writer.writerow([
        "kp", "ki", "kd", "score",
        "high_mean_dev", "high_mean_abs_dev", "high_rms_dev", "high_max_abs_dev",
        "low_mean_dev", "low_mean_abs_dev", "low_rms_dev", "low_max_abs_dev",
    ])

    try:
        oven.tec_on()
        for i, kp in enumerate(kp_values):
            print(f"\n[{i + 1}/{len(kp_values)}] Kp={kp:.4f} ...")
            result, history = run_point(oven, kp, KI, KD, i + 1, len(kp_values), out_dir)
            results.append(result)
            all_history[kp] = history

            h, lo = result["high"], result["low"]
            print(f"  high({HIGH_TEMP_C:.0f}C): mean_abs_dev={h['mean_abs_dev']:.3f} C  "
                  f"rms={h['rms_dev']:.3f} C  max={h['max_abs_dev']:.3f} C")
            print(f"  low ({LOW_TEMP_C:.0f}C): mean_abs_dev={lo['mean_abs_dev']:.3f} C  "
                  f"rms={lo['rms_dev']:.3f} C  max={lo['max_abs_dev']:.3f} C")
            print(f"  score = {result['score']:.4f} C")

            writer.writerow([
                kp, KI, KD, result["score"],
                h["mean_dev"], h["mean_abs_dev"], h["rms_dev"], h["max_abs_dev"],
                lo["mean_dev"], lo["mean_abs_dev"], lo["rms_dev"], lo["max_abs_dev"],
            ])
            csv_file.flush()
    except SafetyLimitExceeded as exc:
        print(f"\nABORTED (safety limit): {exc}")
    except KeyboardInterrupt:
        print("\nInterrupted by user.")
    finally:
        oven.tec_off()
        oven.close()
        csv_file.close()

    if results:
        best = min(results, key=lambda r: r["score"])
        print(f"\nBest Kp so far: {best['kp']:.4f}  (score={best['score']:.4f} C)")
        print(f"Results + per-point plots saved to {out_dir}")
        try:
            save_combined_overlay(all_history, list(all_history.keys()), out_dir)
            print(f"Combined overlay saved to {out_dir / 'combined_overlay.png'}")
        except Exception as exc:
            print(f"(combined overlay skipped: {exc})")
    else:
        print("No completed points to report.")


if __name__ == "__main__":
    main()

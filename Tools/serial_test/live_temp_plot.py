"""Live control panel for the crystal TEC: live plot of actual vs. set
temperature, buttons to turn the TEC on/off, and text boxes to push new
PID gains — all while the plot keeps running. Close the window or
Ctrl+C to stop; the TEC is switched off automatically on exit if it was
left on.
"""

import time

import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
from matplotlib.widgets import Button, TextBox

from oven_driver import OvenDriver

PORT = "/dev/ttyACM0"
SET_TEMP_C = 50.0

# Set to a float to push new PID gains at startup, or leave as None to use
# whatever is currently set on the board.
KP = 50
KI = 0
KD = 0

BACKGROUND_COLOR = "#E8720C"
ACTUAL_COLOR = "#FFFFFF"
SETPOINT_COLOR = "#F5C99B"  # white/orange blend
ON_MARKER_COLOR = "#FFFFFF"
OFF_MARKER_COLOR = "#7A3D06"
LINE_WIDTH = 2.5


def main():
    oven = OvenDriver(PORT)

    initial = oven.get_crystal_status()
    if initial["buck_fault"]:
        print(f"WARNING: TEC buck reports '{initial['buck_message']}' — proceeding anyway.")

    if KP is not None:
        oven.set_kp(KP)
    if KI is not None:
        oven.set_ki(KI)
    if KD is not None:
        oven.set_kd(KD)
    oven.set_setpoint(SET_TEMP_C)

    times, temps, setpoints = [], [], []
    start = time.monotonic()
    tec_state = {"on": False}

    fig, ax = plt.subplots()
    fig.patch.set_facecolor(BACKGROUND_COLOR)
    ax.set_facecolor(BACKGROUND_COLOR)
    fig.subplots_adjust(bottom=0.34, top=0.82)

    temp_display = fig.suptitle("---.--- °C", color=ACTUAL_COLOR, fontsize=32,
                                 fontweight="bold")

    temp_line, = ax.plot([], [], "-", color=ACTUAL_COLOR, linewidth=LINE_WIDTH,
                          label="Actual Temperature")
    sp_line, = ax.plot([], [], "--", color=SETPOINT_COLOR, linewidth=LINE_WIDTH,
                        label="Set Temperature")

    ax.set_xlabel("Time (s)", color=ACTUAL_COLOR)
    ax.set_ylabel("Temperature (°C)", color=ACTUAL_COLOR)
    ax.tick_params(colors=ACTUAL_COLOR)
    for spine in ax.spines.values():
        spine.set_color(BACKGROUND_COLOR)

    legend = ax.legend(loc="upper right", frameon=False, labelcolor=ACTUAL_COLOR,
                        bbox_to_anchor=(0.98, 1.05))
    for line in legend.get_lines():
        line.set_linewidth(LINE_WIDTH)

    def set_title():
        ax.set_title(f"Kp={oven_gains['kp']:.4f}  Ki={oven_gains['ki']:.5f}  "
                      f"Kd={oven_gains['kd']:.4f}", color=ACTUAL_COLOR)

    oven_gains = {
        "kp": KP if KP is not None else initial["kp"],
        "ki": KI if KI is not None else initial["ki"],
        "kd": KD if KD is not None else initial["kd"],
    }
    set_title()

    # Live PID entry boxes along the bottom — edit a value and press Enter.
    box_specs = [("kp", 0.15, oven.set_kp), ("ki", 0.42, oven.set_ki), ("kd", 0.69, oven.set_kd)]
    text_boxes = []
    for key, x, setter in box_specs:
        box_ax = fig.add_axes([x, 0.05, 0.2, 0.06])
        box_ax.set_facecolor(SETPOINT_COLOR)
        box = TextBox(box_ax, f"{key} ", initial=f"{oven_gains[key]:.5f}",
                       color=SETPOINT_COLOR, hovercolor="#FFFFFF")
        box.label.set_color(ACTUAL_COLOR)
        box.text_disp.set_color(BACKGROUND_COLOR)

        def make_submit(key=key, setter=setter):
            def submit(text):
                try:
                    val = float(text)
                except ValueError:
                    print(f"Ignored — {key} needs a number, got {text!r}")
                    return
                setter(val)
                oven_gains[key] = val
                set_title()
                fig.canvas.draw_idle()
                print(f"{key} -> {val}")
            return submit

        box.on_submit(make_submit())
        text_boxes.append(box)

    # TEC on/off buttons, above the PID entry boxes.
    on_button_ax = fig.add_axes([0.15, 0.15, 0.14, 0.07])
    off_button_ax = fig.add_axes([0.31, 0.15, 0.14, 0.07])
    on_button = Button(on_button_ax, "TEC ON", color=SETPOINT_COLOR, hovercolor="#FFFFFF")
    off_button = Button(off_button_ax, "TEC OFF", color=SETPOINT_COLOR, hovercolor="#FFFFFF")
    on_button.label.set_color(BACKGROUND_COLOR)
    off_button.label.set_color(BACKGROUND_COLOR)

    toggle_markers = []

    def on_tec_on(_event):
        oven.tec_on()
        tec_state["on"] = True
        now = time.monotonic() - start
        toggle_markers.append(ax.axvline(now, color=ON_MARKER_COLOR, linestyle=":",
                                          linewidth=1.5, alpha=0.6))
        fig.canvas.draw_idle()
        print(f"[{now:6.2f}s] TEC ON")

    def on_tec_off(_event):
        oven.tec_off()
        tec_state["on"] = False
        now = time.monotonic() - start
        toggle_markers.append(ax.axvline(now, color=OFF_MARKER_COLOR, linestyle=":",
                                          linewidth=1.5, alpha=0.8))
        fig.canvas.draw_idle()
        print(f"[{now:6.2f}s] TEC OFF")

    on_button.on_clicked(on_tec_on)
    off_button.on_clicked(on_tec_off)

    # Setpoint entry box, next to the TEC buttons — the dashed line updates
    # on its own since it's redrawn from the board's actual setpoint each poll.
    sp_box_ax = fig.add_axes([0.50, 0.155, 0.2, 0.06])
    sp_box_ax.set_facecolor(SETPOINT_COLOR)
    sp_box = TextBox(sp_box_ax, "SP ", initial=f"{SET_TEMP_C:.4f}",
                      color=SETPOINT_COLOR, hovercolor="#FFFFFF")
    sp_box.label.set_color(ACTUAL_COLOR)
    sp_box.text_disp.set_color(BACKGROUND_COLOR)

    def submit_setpoint(text):
        try:
            val = float(text)
        except ValueError:
            print(f"Ignored — setpoint needs a number, got {text!r}")
            return
        oven.set_setpoint(val)
        print(f"setpoint -> {val}")

    sp_box.on_submit(submit_setpoint)

    # Reset button — clears the plotted history and restarts the time axis
    # at 0 without touching the board's setpoint, gains, or TEC state.
    reset_button_ax = fig.add_axes([0.75, 0.15, 0.14, 0.07])
    reset_button = Button(reset_button_ax, "Reset Graph", color=SETPOINT_COLOR,
                           hovercolor="#FFFFFF")
    reset_button.label.set_color(BACKGROUND_COLOR)

    def on_reset(_event):
        nonlocal start
        times.clear()
        temps.clear()
        setpoints.clear()
        for marker in toggle_markers:
            marker.remove()
        toggle_markers.clear()
        start = time.monotonic()
        temp_line.set_data([], [])
        sp_line.set_data([], [])
        ax.relim()
        ax.autoscale_view()
        fig.canvas.draw_idle()
        print("Graph reset.")

    reset_button.on_clicked(on_reset)

    def update(_frame):
        now = time.monotonic() - start
        try:
            status = oven.get_crystal_status()
        except RuntimeError as exc:
            print(f"[{now:6.2f}s] skipped bad read: {exc}")
            return temp_line, sp_line

        times.append(now)
        temps.append(status["temp_c"])
        setpoints.append(status["setpoint_c"])

        temp_line.set_data(times, temps)
        sp_line.set_data(times, setpoints)
        temp_display.set_text(f"{status['temp_c']:.3f} °C")
        ax.relim()
        ax.autoscale_view()
        return temp_line, sp_line, temp_display

    ani = FuncAnimation(fig, update, interval=1, cache_frame_data=False)

    try:
        plt.show()
    except KeyboardInterrupt:
        pass
    finally:
        if tec_state["on"]:
            oven.tec_off()
            print("Plot closed. TEC turned off.")
        else:
            print("Plot closed. TEC already off.")
        oven.close()


if __name__ == "__main__":
    main()

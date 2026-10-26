
import time
from collections import deque

import serial
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.widgets import Slider

# -------- Configuration --------
PORT = "COM9"       # Change to your ESP32's COM port
BAUD = 460800
WINDOW_SECONDS = 10
MAX_POINTS = 2500   # 250 diagnostic rows/s x 10 seconds

# Initial detector settings; sent to the ESP32 on startup.
initial_multiplier = 3.0
initial_floor = 12.0
initial_min_interval = 280

ser = serial.Serial(PORT, BAUD, timeout=0.01)
time.sleep(2)
ser.reset_input_buffer()

def send_setting(key, value):
    command = f"{key}={value}\n"
    ser.write(command.encode("ascii"))

send_setting("M", initial_multiplier)
send_setting("F", initial_floor)
send_setting("D", initial_min_interval)

times = deque(maxlen=MAX_POINTS)
raw_values = deque(maxlen=MAX_POINTS)
filtered_values = deque(maxlen=MAX_POINTS)
envelopes = deque(maxlen=MAX_POINTS)
noise_values = deque(maxlen=MAX_POINTS)
thresholds = deque(maxlen=MAX_POINTS)
event_times = deque(maxlen=200)

fig, axes = plt.subplots(3, 1, figsize=(12, 9))
plt.subplots_adjust(bottom=0.25, hspace=0.4)

raw_line, = axes[0].plot([], [], linewidth=0.7)
filtered_line, = axes[1].plot([], [], linewidth=0.8)
envelope_line, = axes[2].plot([], [], linewidth=0.8)
threshold_line, = axes[2].plot([], [], "--", linewidth=0.8)
event_points, = axes[1].plot([], [], "x", markersize=7)

axes[0].set_title("Raw microphone ADC")
axes[1].set_title("Filtered microphone signal")
axes[2].set_title("Envelope and detection threshold")

axes[0].set_ylabel("ADC counts")
axes[1].set_ylabel("Filtered amplitude")
axes[2].set_ylabel("Envelope")
axes[2].set_xlabel("Time (seconds ago)")

for ax in axes:
    ax.grid(True)
    ax.set_xlim(-WINDOW_SECONDS, 0)

status = fig.suptitle("Waiting for ESP32 data...")

# Slider axes
ax_mult = fig.add_axes([0.20, 0.16, 0.65, 0.025])
ax_floor = fig.add_axes([0.20, 0.11, 0.65, 0.025])
ax_interval = fig.add_axes([0.20, 0.06, 0.65, 0.025])

mult_slider = Slider(
    ax_mult, "Noise multiplier", 1.0, 10.0,
    valinit=initial_multiplier
)
floor_slider = Slider(
    ax_floor, "Threshold floor", 1.0, 100.0,
    valinit=initial_floor
)
interval_slider = Slider(
    ax_interval, "Min interval (ms)", 200, 1000,
    valinit=initial_min_interval, valstep=10
)

def update_multiplier(value):
    send_setting("M", value)

def update_floor(value):
    send_setting("F", value)

def update_interval(value):
    send_setting("D", value)

mult_slider.on_changed(update_multiplier)
floor_slider.on_changed(update_floor)
interval_slider.on_changed(update_interval)

def parse_line(line):
    try:
        fields = line.decode("ascii", errors="ignore").strip().split(",")
        if len(fields) != 7 or fields[0] == "ms":
            return None

        t, raw, filtered, envelope, noise, threshold, event = fields
        return (
            int(t), float(raw), float(filtered),
            float(envelope), float(noise),
            float(threshold), int(event)
        )
    except (ValueError, TypeError):
        return None

try:
    last_draw = time.monotonic()

    while plt.fignum_exists(fig.number):
        line = ser.readline()

        if line:
            row = parse_line(line)

            if row is not None:
                t, raw, filtered, envelope, noise, threshold, event = row

                # ESP32 millis() wraps after roughly 49 days.
                if times and t < times[-1] and times[-1] - t < 2**31:
                    pass
                else:
                    times.append(t / 1000.0)
                    raw_values.append(raw)
                    filtered_values.append(filtered)
                    envelopes.append(envelope)
                    noise_values.append(noise)
                    thresholds.append(threshold)

                    if event:
                        event_times.append(t / 1000.0)

        now = time.monotonic()
        if now - last_draw < 0.05:
            plt.pause(0.001)
            continue

        last_draw = now

        if len(times) < 2:
            plt.pause(0.001)
            continue

        t = np.asarray(times)
        x = t - t[-1]

        raw = np.asarray(raw_values)
        filtered = np.asarray(filtered_values)
        env = np.asarray(envelopes)
        thr = np.asarray(thresholds)

        raw_line.set_data(x, raw)
        filtered_line.set_data(x, filtered)
        envelope_line.set_data(x, env)
        threshold_line.set_data(x, thr)

        ev = np.asarray(event_times)
        if len(ev):
            visible = (ev >= t[-1] - WINDOW_SECONDS) & (ev <= t[-1])
            ev = ev[visible]

            # Approximate marker placement at zero on filtered plot.
            event_points.set_data(
                ev - t[-1],
                np.zeros(len(ev))
            )
        else:
            event_points.set_data([], [])

        for ax, values in zip(
            axes, (raw, filtered, np.concatenate([env, thr]))
        ):
            low = np.nanpercentile(values, 1)
            high = np.nanpercentile(values, 99)

            if high <= low:
                low -= 1
                high += 1

            margin = max((high - low) * 0.15, 1)
            ax.set_ylim(low - margin, high + margin)

        status.set_text(
            f"Korotkoff Debug | M={mult_slider.val:.1f} "
            f"F={floor_slider.val:.1f} "
            f"D={interval_slider.val:.0f} ms"
        )

        fig.canvas.draw_idle()
        plt.pause(0.001)

except KeyboardInterrupt:
    pass
finally:
    ser.close()
    plt.close("all")

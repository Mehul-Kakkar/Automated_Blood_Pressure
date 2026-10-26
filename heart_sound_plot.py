import serial
import numpy as np
import matplotlib.pyplot as plt
from scipy.signal import butter, sosfilt, find_peaks
from collections import deque
import time

PORT = "COM9"
BAUD = 230400
FS = 2000

WINDOW_SECONDS = 10
N = FS * WINDOW_SECONDS

MIN_GATE = 6.0
NOISE_MULTIPLIER = 2.3

MIN_BEAT_INTERVAL = 0.40
MAX_BEAT_INTERVAL = 1.70
BPM_WINDOW_SECONDS = 10
UPDATE_SECONDS = 1.0

ser = serial.Serial(PORT, BAUD, timeout=0.1)
ser.reset_input_buffer()

bandpass_sos = butter(
    4, [20, 300], btype="bandpass",
    fs=FS, output="sos"
)
filter_state = np.zeros((len(bandpass_sos), 2))

raw_buffer = np.zeros(N)
filtered_buffer = np.zeros(N)
clean_buffer = np.zeros(N)

write_index = 0
total_samples = 0
last_processed_peak = -1
last_bpm_update = time.monotonic()
last_printed_bpm = None

pending = b""
beat_times = deque()
intervals = deque(maxlen=8)

plt.ion()
fig, axes = plt.subplots(3, 1, figsize=(11, 8), sharex=True)
x = np.arange(N) / FS

raw_line, = axes[0].plot(x, raw_buffer, linewidth=0.7)
filtered_line, = axes[1].plot(x, filtered_buffer, linewidth=0.8)
clean_line, = axes[2].plot(x, clean_buffer, linewidth=0.8)

axes[0].set_title("Raw AC signal")
axes[1].set_title("Band-pass filtered: 20–300 Hz")
axes[2].set_title("Noise-gated waveform")
axes[2].set_xlabel("Time (seconds)")

for ax in axes:
    ax.set_xlim(0, WINDOW_SECONDS)
    ax.grid(True)

bpm_text = fig.suptitle("Estimated BPM: collecting data...")
plt.tight_layout()
plt.show(block=False)

try:
    while plt.fignum_exists(fig.number):
        data = pending + ser.read(1024)
        usable = len(data) - len(data) % 2
        pending = data[usable:]

        if usable == 0:
            plt.pause(0.01)
            continue

        samples = np.frombuffer(
            data[:usable], dtype="<u2"
        ).astype(float)

        raw = samples - np.mean(samples)

        filtered, filter_state = sosfilt(
            bandpass_sos, raw, zi=filter_state
        )

        centre = np.median(filtered)
        noise_estimate = (
            np.median(np.abs(filtered - centre)) / 0.6745
        )
        gate = max(
            MIN_GATE,
            NOISE_MULTIPLIER * noise_estimate
        )

        clean = np.sign(filtered) * np.maximum(
            np.abs(filtered) - gate, 0
        )

        for r, f, c in zip(raw, filtered, clean):
            raw_buffer[write_index] = r
            filtered_buffer[write_index] = f
            clean_buffer[write_index] = c
            write_index = (write_index + 1) % N

        total_samples += len(samples)

        order = np.r_[write_index:N, 0:write_index]
        ordered_raw = raw_buffer[order]
        ordered_filtered = filtered_buffer[order]
        ordered_clean = clean_buffer[order]

        # Process only after a complete window has been collected.
        if total_samples >= N:
            envelope = np.abs(ordered_filtered)
            smooth_width = max(1, int(0.025 * FS))
            envelope = np.convolve(
                envelope,
                np.ones(smooth_width) / smooth_width,
                mode="same"
            )

            prominence = max(8.0, 2.5 * noise_estimate)
            peaks, _ = find_peaks(
                envelope,
                distance=int(MIN_BEAT_INTERVAL * FS),
                prominence=prominence
            )

            # Allow time for samples after each peak to arrive.
            safe_end = N - int(0.15 * FS)

            for peak in peaks:
                if peak >= safe_end:
                    continue

                global_peak = total_samples - N + int(peak)

                if global_peak <= last_processed_peak:
                    continue

                last_processed_peak = global_peak
                peak_time = global_peak / FS

                if beat_times:
                    interval = peak_time - beat_times[-1]

                    if MIN_BEAT_INTERVAL <= interval <= MAX_BEAT_INTERVAL:
                        intervals.append(interval)
                        beat_times.append(peak_time)
                        print("Candidate beat detected")
                    else:
                        # An implausible interval starts a new sequence.
                        beat_times.clear()
                        intervals.clear()
                        beat_times.append(peak_time)
                        print("Peak detected; resetting interval history")
                else:
                    beat_times.append(peak_time)
                    print("First candidate peak; waiting for another")

        # Retain only recent beat timestamps.
        current_time = total_samples / FS
        while beat_times and current_time - beat_times[0] > BPM_WINDOW_SECONDS:
            beat_times.popleft()

        # Update the displayed BPM once per second.
        now = time.monotonic()
        bpm = None

        if now - last_bpm_update >= UPDATE_SECONDS:
            last_bpm_update = now

            if len(beat_times) >= 3:
                span = beat_times[-1] - beat_times[0]
                if span > 0:
                    bpm = 60.0 * (len(beat_times) - 1) / span

            if bpm is not None:
                bpm_text.set_text(
                    f"Estimated BPM: {bpm:.1f} | "
                    f"Window: {BPM_WINDOW_SECONDS}s"
                )

                if (
                    last_printed_bpm is None
                    or abs(bpm - last_printed_bpm) >= 1
                ):
                    print(f"Estimated BPM: {bpm:.1f}")
                    last_printed_bpm = bpm
            else:
                bpm_text.set_text(
                    "Estimated BPM: collecting reliable intervals..."
                )

        raw_line.set_ydata(ordered_raw)
        filtered_line.set_ydata(ordered_filtered)
        clean_line.set_ydata(ordered_clean)

        for ax, buf in zip(
            axes,
            (ordered_raw, ordered_filtered, ordered_clean)
        ):
            limit = max(
                np.percentile(np.abs(buf), 99) * 1.5,
                5
            )
            ax.set_ylim(-limit, limit)

        axes[2].set_ylabel(f"Gate: {gate:.1f}")

        fig.canvas.draw_idle()
        plt.pause(0.001)

except KeyboardInterrupt:
    pass
finally:
    ser.close()
    plt.close("all")

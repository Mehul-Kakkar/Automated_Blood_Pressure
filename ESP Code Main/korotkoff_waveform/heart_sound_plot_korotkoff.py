import serial
import numpy as np
import matplotlib.pyplot as plt
import time

from scipy.signal import butter, sosfilt
from collections import deque

# =====================================================
# CONFIGURATION
# =====================================================

PORT = "COM9"
BAUD = 460800
FS = 2000

WINDOW_SECONDS = 8
N = FS * WINDOW_SECONDS

READ_BYTES = 256

# =====================================================
# FILTERING
# =====================================================

LOW_HZ = 20
HIGH_HZ = 300

FILTER_ORDER = 4

ENVELOPE_MS = 8

sos = butter(
    FILTER_ORDER,
    [LOW_HZ, HIGH_HZ],
    btype="bandpass",
    fs=FS,
    output="sos"
)

zi = np.zeros((len(sos), 2))

env_alpha = 1 - np.exp(
    -1 / (FS * ENVELOPE_MS / 1000)
)

envelope = 0.0

# =====================================================
# CALIBRATION
# =====================================================

WARMUP_SECONDS = 1
CALIBRATION_SECONDS = 4

CALIBRATION_PERCENTILE = 85
CALIBRATION_MARGIN = 1.10

MIN_THRESHOLD = 0.75

THRESHOLD_SIGMA = 2.5

warmup_remaining = FS * WARMUP_SECONDS

calibration_samples = int(
    FS * CALIBRATION_SECONDS
)

calibration_data = []

calibrated = False

noise_floor = 0.0
noise_sigma = 0.0
threshold = MIN_THRESHOLD

# =====================================================
# ADAPTIVE BACKGROUND TRACKING
# =====================================================

NOISE_WINDOW_SECONDS = 2

NOISE_UPDATE_MS = 250

NOISE_PERCENTILE = 85

NOISE_MARGIN = 1.10

NOISE_RISE_ALPHA = 0.12
NOISE_FALL_ALPHA = 0.025

noise_history = deque(
    maxlen=int(FS * NOISE_WINDOW_SECONDS)
)

noise_update_samples = int(
    FS * NOISE_UPDATE_MS / 1000
)

noise_update_counter = 0

# =====================================================
# EVENT DETECTION
# =====================================================

MIN_EVENT_MS = 4
MAX_EVENT_MS = 220

END_HOLD_MS = 15

REFRACTORY_MS = 120

ARTIFACT_RECOVERY_MS = 80

min_event_samples = int(
    FS * MIN_EVENT_MS / 1000
)

max_event_samples = int(
    FS * MAX_EVENT_MS / 1000
)

end_hold_samples = int(
    FS * END_HOLD_MS / 1000
)

refractory_samples = int(
    FS * REFRACTORY_MS / 1000
)

artifact_recovery_samples = int(
    FS * ARTIFACT_RECOVERY_MS / 1000
)

in_event = False
in_artifact = False

event_start = 0
event_peak = 0.0
event_peak_index = 0

below_count = 0
artifact_below_count = 0

last_event_index = -100000

events = deque()

detected_total = 0
rejected_total = 0

# =====================================================
# DISPLAY
# =====================================================

WAVE_Y_LIMIT = 100

wave_buffer = np.zeros(N)
envelope_buffer = np.zeros(N)
threshold_buffer = np.zeros(N)

write_index = 0
total_samples = 0

pending = b""

# =====================================================
# SERIAL CONNECTION
# =====================================================

ser = serial.Serial(
    PORT,
    BAUD,
    timeout=0.05
)

ser.reset_input_buffer()

print("Connected to ESP32.")
print("Keep cuff inflated and pump stopped during calibration.")
print("Calibration will begin automatically.")

# =====================================================
# PLOTTING
# =====================================================

plt.ion()

fig, (ax1, ax2) = plt.subplots(
    2,
    1,
    figsize=(12, 8),
    sharex=True
)

x = np.arange(N) / FS

# Waveform
wave_line, = ax1.plot(
    x,
    wave_buffer,
    linewidth=0.75,
    label="Filtered waveform"
)

event_marks, = ax1.plot(
    [],
    [],
    "ro",
    markersize=6,
    label="Detected Korotkoff candidates"
)

# Envelope
env_line, = ax2.plot(
    x,
    envelope_buffer,
    linewidth=0.9,
    label="Signal envelope"
)

threshold_line, = ax2.plot(
    x,
    threshold_buffer,
    "r--",
    label="Adaptive threshold"
)

ax1.set_title("Korotkoff Acoustic Waveform")
ax1.set_ylabel("Amplitude (ADC counts)")
ax1.set_ylim(-WAVE_Y_LIMIT, WAVE_Y_LIMIT)
ax1.grid(True)
ax1.legend()

ax2.set_title("Acoustic Event Detection")
ax2.set_ylabel("Envelope amplitude")
ax2.set_xlabel("Time (seconds)")
ax2.grid(True)
ax2.legend()

for ax in (ax1, ax2):
    ax.set_xlim(0, WINDOW_SECONDS)

status = fig.suptitle(
    "Waiting for microphone samples..."
)

plt.tight_layout(rect=[0, 0, 1, 0.96])
plt.show(block=False)

# =====================================================
# NOISE ESTIMATION
# =====================================================

def update_noise():

    global noise_floor
    global noise_sigma
    global threshold

    if len(noise_history) < FS // 2:
        return

    values = np.asarray(noise_history)

    median = np.median(values)

    mad = np.median(
        np.abs(values - median)
    )

    noise_sigma = max(
        0.25,
        1.4826 * mad
    )

    percentile_value = np.percentile(
        values,
        NOISE_PERCENTILE
    )

    new_floor = max(
        percentile_value,
        median + THRESHOLD_SIGMA * noise_sigma
    )

    if noise_floor == 0:
        noise_floor = new_floor
    else:
        noise_floor += 0.15 * (
            new_floor - noise_floor
        )

    target = max(
        MIN_THRESHOLD,
        noise_floor * NOISE_MARGIN
    )

    if target > threshold:
        alpha = NOISE_RISE_ALPHA
    else:
        alpha = NOISE_FALL_ALPHA

    threshold += alpha * (
        target - threshold
    )


# =====================================================
# EVENT DETECTION
# =====================================================

def process_event(score, sample_index):

    global in_event
    global in_artifact

    global event_start
    global event_peak
    global event_peak_index

    global below_count
    global artifact_below_count

    global last_event_index

    global detected_total
    global rejected_total

    # -------------------------------------------------
    # ARTIFACT RECOVERY
    # -------------------------------------------------

    if in_artifact:

        if score < threshold * 0.65:
            artifact_below_count += 1
        else:
            artifact_below_count = 0

        if artifact_below_count >= artifact_recovery_samples:
            in_artifact = False
            artifact_below_count = 0

        return

    # -------------------------------------------------
    # EVENT START
    # -------------------------------------------------

    if not in_event:

        if (
            score >= threshold
            and sample_index - last_event_index
            >= refractory_samples
        ):

            in_event = True

            event_start = sample_index

            event_peak = score
            event_peak_index = sample_index

            below_count = 0

        return

    # -------------------------------------------------
    # TRACK ACTIVE EVENT
    # -------------------------------------------------

    if score > event_peak:
        event_peak = score
        event_peak_index = sample_index

    if score < threshold * 0.65:
        below_count += 1
    else:
        below_count = 0

    duration = sample_index - event_start

    # Long sustained event
    if duration >= max_event_samples:

        in_event = False
        in_artifact = True

        artifact_below_count = 0

        rejected_total += 1

        return

    # -------------------------------------------------
    # EVENT TERMINATION
    # -------------------------------------------------

    if below_count >= end_hold_samples:

        event_duration = (
            sample_index - below_count - event_start
        )

        if (
            min_event_samples <= event_duration
            <= max_event_samples
        ):

            events.append(
                (
                    event_peak_index,
                    event_peak
                )
            )

            last_event_index = event_peak_index

            detected_total += 1

        else:

            rejected_total += 1

        in_event = False
        below_count = 0


# =====================================================
# MAIN LOOP
# =====================================================

try:

    while plt.fignum_exists(fig.number):

        # -------------------------------------------------
        # READ BINARY SERIAL DATA
        # -------------------------------------------------

        data = pending + ser.read(READ_BYTES)

        usable = len(data) - len(data) % 2

        pending = data[usable:]

        if usable == 0:
            plt.pause(0.005)
            continue

        samples = np.frombuffer(
            data[:usable],
            dtype="<u2"
        ).astype(np.float64)

        # -------------------------------------------------
        # BANDPASS FILTER
        # -------------------------------------------------

        filtered, zi = sosfilt(
            sos,
            samples,
            zi=zi
        )

        # -------------------------------------------------
        # SAMPLE PROCESSING
        # -------------------------------------------------

        for value in filtered:

            total_samples += 1

            # Smooth absolute envelope
            envelope += env_alpha * (
                abs(value) - envelope
            )

            # ---------------------------------------------
            # WARMUP
            # ---------------------------------------------

            if warmup_remaining > 0:

                warmup_remaining -= 1

                score = envelope

            # ---------------------------------------------
            # CALIBRATION
            # ---------------------------------------------

            elif not calibrated:

                calibration_data.append(envelope)

                if len(calibration_data) >= calibration_samples:

                    calibration_array = np.asarray(
                        calibration_data
                    )

                    median = np.median(
                        calibration_array
                    )

                    mad = np.median(
                        np.abs(calibration_array - median)
                    )

                    noise_sigma = max(
                        0.25,
                        1.4826 * mad
                    )

                    noise_floor = float(
                        np.percentile(
                            calibration_array,
                            CALIBRATION_PERCENTILE
                        )
                    )

                    threshold = max(
                        MIN_THRESHOLD,
                        noise_floor * CALIBRATION_MARGIN,
                        median + THRESHOLD_SIGMA * noise_sigma
                    )

                    noise_history.extend(
                        calibration_array[-FS:].tolist()
                    )

                    calibrated = True

                    calibration_data.clear()

                    print(
                        f"Calibration complete | "
                        f"Noise floor: {noise_floor:.2f} | "
                        f"Threshold: {threshold:.2f}"
                    )

                score = envelope

            # ---------------------------------------------
            # DETECTION
            # ---------------------------------------------

            else:

                sample_index = total_samples - 1

                if not in_event and not in_artifact:

                    noise_history.append(envelope)

                    noise_update_counter += 1

                    if (
                        noise_update_counter >= noise_update_samples
                    ):

                        noise_update_counter = 0

                        update_noise()

                process_event(
                    envelope,
                    sample_index
                )

                score = envelope

            # ---------------------------------------------
            # UPDATE BUFFERS
            # ---------------------------------------------

            wave_buffer[write_index] = value

            envelope_buffer[write_index] = score

            threshold_buffer[write_index] = (
                threshold if calibrated else np.nan
            )

            write_index = (
                write_index + 1
            ) % N

        # -------------------------------------------------
        # ORDER BUFFERS
        # -------------------------------------------------

        order = np.r_[
            write_index:N,
            0:write_index
        ]

        ordered_wave = wave_buffer[order]
        ordered_env = envelope_buffer[order]
        ordered_threshold = threshold_buffer[order]

        wave_line.set_ydata(
            ordered_wave
        )

        env_line.set_ydata(
            ordered_env
        )

        threshold_line.set_ydata(
            ordered_threshold
        )

        # -------------------------------------------------
        # REMOVE OLD EVENTS
        # -------------------------------------------------

        visible_start = max(
            0,
            total_samples - N
        )

        while events and events[0][0] < visible_start:
            events.popleft()

        # -------------------------------------------------
        # DRAW EVENT MARKERS
        # -------------------------------------------------

        if events:

            event_x = np.array([
                (e[0] - visible_start) / FS
                for e in events
            ])

            event_y = np.interp(
                event_x,
                x,
                ordered_wave
            )

            event_marks.set_data(
                event_x,
                event_y
            )

        else:

            event_marks.set_data([], [])

        # -------------------------------------------------
        # AXIS SCALING
        # -------------------------------------------------

        env_max = max(
            5,
            np.nanmax(ordered_env),
            threshold * 1.2
        )

        ax2.set_ylim(
            0,
            env_max * 1.1
        )

        # -------------------------------------------------
        # STATUS
        # -------------------------------------------------

        if not calibrated:

            if warmup_remaining > 0:

                message = (
                    f"Signal settling: "
                    f"{100 * (1 - warmup_remaining / (FS * WARMUP_SECONDS)):.0f}%"
                )

            else:

                progress = (
                    len(calibration_data)
                    / calibration_samples
                ) * 100

                message = (
                    f"Calibrating pressurised background: "
                    f"{progress:.0f}%"
                )

        else:

            message = (
                f"CALIBRATED | "
                f"Noise: {noise_floor:.2f} | "
                f"Threshold: {threshold:.2f} | "
                f"Envelope: {envelope:.2f} | "
                f"Events: {detected_total} | "
                f"Artifacts: {rejected_total}"
            )

        status.set_text(message)

        fig.canvas.draw_idle()

        plt.pause(0.001)


except KeyboardInterrupt:

    pass

finally:

    ser.close()

    plt.close("all")

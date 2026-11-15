#!/usr/bin/env python3
"""Plot ESP32 CSI amplitude and flag activity, live or from a capture.

active_ap prints one line per received frame:

    CSI_DATA,AP,<mac>,<rssi>,<rate>,<noise>,<ch>,<second>,<ts>,<siglen>,
    <rxstate>,<rt_set>,<steady_ts>,<len>,[i q i q ...]

Both FreeWili USB CDC ports mirror that stream (the main CPU reads the ESP32
on UART1 and forwards it to the display, and both halves write it to USB), so
either port works:

    ./plot_csi.py                      # live from /dev/ttyACM0
    ./plot_csi.py /dev/ttyACM1         # display CPU's mirror
    ./plot_csi.py --file csi.log       # replay a capture
    ./plot_csi.py --file csi.log --save csi.png   # no window, just a PNG

Top plot:    amplitude of every subcarrier in the newest frame.
Middle:      waterfall, amplitude over the last `--history` frames.
Bottom:      two presence primitives, both normalised so they do not move
             when RSSI or gain drifts:

             activity   frame-to-frame change of the subcarrier shape --
                        someone walking through the link spikes this.
             deviation  distance from the calibrated empty-room shape --
                        stays up while someone is standing in the link.

             The first `--calibrate` frames are the empty-room baseline; the
             threshold is their median activity plus 4 MAD (clamped to
             0.05..0.8). Above it the script prints PRESENT and paints the
             label red. `--threshold` overrides the learned value.

For a believable demo: start it with the room still, let it calibrate, then
walk between the AP and the receiver. The waterfall gets a band and both
curves jump. This is a toy -- one link, no training, no classifier -- but it
is the same signal a real presence detector thresholding would use.
"""
import argparse
import collections
import math
import queue
import sys
import threading
import time

import numpy as np

try:
    import matplotlib
    if "--save" in sys.argv:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation
except ImportError:  # pragma: no cover
    sys.exit("matplotlib is required: pip install matplotlib")


def parse_csi(line):
    """Return {'amps': [...], 'rssi': int, 'mac': str} or None."""
    i = line.find("CSI_DATA")
    if i < 0:
        return None
    line = line[i:].strip()
    try:
        head, body = line.split("[", 1)
        body = body.split("]", 1)[0]
    except ValueError:
        return None

    try:
        raw = [int(v) for v in body.split()]
    except ValueError:
        return None
    if len(raw) < 2:
        return None

    # The tool stores I first, Q second; amplitude is symmetric either way.
    amps = [math.hypot(raw[k], raw[k + 1]) for k in range(0, len(raw) - 1, 2)]

    fields = head.split(",")
    def field(n, default=None):
        try:
            return fields[n]
        except IndexError:
            return default

    return {"amps": amps, "mac": field(2, "?"), "rssi": field(3, "?")}


def normalise(amps):
    """Subcarrier shape with the overall gain divided out.

    A person changes the shape; moving the board or turning up a PA changes
    the scale. Dividing by the frame mean keeps the score comparable.
    """
    return amps / (float(amps.mean()) + 1e-9)


def read_serial(port, baud, out):
    import serial
    while True:
        try:
            s = serial.Serial(port, baud, timeout=1.0)
            # On the C6's USB-Serial/JTAG, asserting *both* DTR and RTS is the
            # esptool download-mode strap: it resets the board into the
            # bootloader and no CSI ever arrives. Leave them deasserted.
            s.dtr = False
            s.rts = False
        except Exception as exc:  # noqa: BLE001 - keep retrying
            print(f"waiting for {port}: {exc}", file=sys.stderr)
            time.sleep(1.0)
            continue
        try:
            while True:
                line = s.readline()
                if line:
                    out.put(line.decode("utf-8", "replace"))
        except Exception as exc:  # noqa: BLE001 - unplugged, retry
            print(f"lost {port}: {exc}", file=sys.stderr)
        finally:
            s.close()
        time.sleep(1.0)


class Plot:
    def __init__(self, nsub, history, calibrate, threshold=None, mac=None):
        self.only_mac = mac.lower() if mac else None
        self.nsub = nsub
        self.history = history
        self.calibrate_n = max(2, calibrate)
        self.fixed_threshold = threshold

        self.frames = collections.deque(maxlen=history)
        self.last = None
        self.prev_norm = None
        self.mac = "?"
        self.rssi = "?"
        self.count = 0

        self.calib = []
        self.baseline_norm = None
        self.threshold = threshold
        self.activity = collections.deque(maxlen=600)
        self.deviation = collections.deque(maxlen=600)
        self.present = False
        self.rate = 0.0
        self.rate_mark = time.time()
        self.rate_frames = 0

        self.fig = plt.figure(figsize=(9, 9.5))
        gs = self.fig.add_gridspec(3, 1, height_ratios=[2.0, 2.0, 1.3],
                                   hspace=0.5)
        self.ax1 = self.fig.add_subplot(gs[0])
        self.ax2 = self.fig.add_subplot(gs[1])
        self.ax3 = self.fig.add_subplot(gs[2])

        self.line, = self.ax1.plot([], [], lw=1)
        self.ax1.set_xlabel("subcarrier")
        self.ax1.set_ylabel("|H|")
        self.ax1.set_xlim(0, max(nsub - 1, 1))
        self.ax1.set_ylim(0, 60)
        self.ax1.grid(alpha=0.3)
        self.title = self.ax1.set_title("waiting for CSI_DATA ...")

        self.img = self.ax2.imshow(
            np.zeros((1, nsub)), aspect="auto", origin="lower",
            cmap="viridis", vmin=0, vmax=60, interpolation="nearest")
        self.ax2.set_xlabel("subcarrier")
        self.ax2.set_ylabel("time")
        self.ax2.set_title(f"waterfall (last {history} frames)")
        self.fig.colorbar(self.img, ax=self.ax2, label="|H|")

        self.aline, = self.ax3.plot([], [], lw=1, color="tab:blue",
                                    label="activity")
        self.dline, = self.ax3.plot([], [], lw=1, color="tab:purple",
                                    label="deviation")
        self.ax3.set_xlabel("frame")
        self.ax3.set_ylabel("normalised change")
        self.ax3.set_title("presence primitives (normalised)")
        self.ax3.grid(alpha=0.3)
        self.ax3.legend(loc="upper left", fontsize=8)

        self.status = self.ax3.text(
            0.99, 0.92, "calibrating - keep the room still",
            transform=self.ax3.transAxes, fontsize=11, fontweight="bold",
            ha="right", va="top", color="tab:orange")

    # -- data -----------------------------------------------------------
    def learn(self):
        cal = np.asarray(self.calib)
        norm = np.asarray([normalise(a) for a in cal])
        self.baseline_norm = np.median(norm, axis=0)

        if self.fixed_threshold is not None:
            self.threshold = self.fixed_threshold
        else:
            steps = np.abs(np.diff(norm, axis=0)).mean(axis=1) if len(norm) > 1 \
                else np.array([0.1])
            med = float(np.median(steps))
            mad = float(np.median(np.abs(steps - med)))
            # 4 MAD above the empty-room median, but never below half the
            # median again (a very quiet channel would otherwise alarm on
            # its own noise) and never pinned to a fixed ceiling: the frame
            # mix decides the scale, not us.
            learned = med + 4.0 * mad
            # A short quiet calibration window can give a MAD near zero, which
            # puts the threshold below the channel's own frame-to-frame noise
            # and makes everything look present. Floor it at a fifth of the
            # full-scale normalised change.
            self.threshold = min(max(learned, 1.5 * med, 0.20), 2.0)
            if self.calibrate_n < 20:
                print(f"warning: calibrated on only {self.calibrate_n} frames; "
                      f"stand still and recalibrate with --calibrate 40+ for a real demo")

        self.ax3.axhline(self.threshold, color="tab:red", ls="--", lw=1,
                         label=f"threshold {self.threshold:.2f}")
        self.ax3.legend(loc="upper left", fontsize=8)
        self.status.set_text("calibrated - move something")
        self.status.set_color("tab:green")
        print(f"calibrated on {len(cal)} frames, "
              f"threshold {self.threshold:.3f}")

    def add(self, rec):
        # One link only, by default everything the radio hears. Mixing frames
        # from a dozen different APs makes the subcarrier shape jump for
        # reasons that have nothing to do with a person in the room.
        if self.only_mac and self.only_mac not in str(rec.get("mac", "")).lower():
            return
        amps = np.asarray(rec["amps"], dtype=float)
        if len(amps) != self.nsub:
            if self.last is not None:      # a reshaped frame; ignore it
                return
            self.nsub = len(amps)
            self.ax1.set_xlim(0, max(self.nsub - 1, 1))

        self.frames.append(amps)
        self.last = amps
        self.mac = rec["mac"]
        self.rssi = rec["rssi"]
        self.count += 1
        self.rate_frames += 1

        if self.baseline_norm is None:
            self.calib.append(amps)
            if len(self.calib) >= self.calibrate_n:
                self.learn()
            self.prev_norm = normalise(amps)
            return

        norm = normalise(amps)
        if self.prev_norm is not None:
            self.activity.append(float(np.abs(norm - self.prev_norm).mean()))
        self.prev_norm = norm
        self.deviation.append(float(np.abs(norm - self.baseline_norm).mean()))

        act = self.smoothed(self.activity)
        dev = self.smoothed(self.deviation)
        if not self.present and (act > self.threshold or dev > self.threshold):
            self.present = True
            print(f"PRESENT  activity {act:.2f}  deviation {dev:.2f}  "
                  f"threshold {self.threshold:.2f}")
        elif self.present and act < 0.5 * self.threshold \
                and dev < 0.5 * self.threshold:
            self.present = False
            print(f"quiet    activity {act:.2f}  deviation {dev:.2f}")

    @staticmethod
    def smoothed(dq, n=4):
        if not dq:
            return 0.0
        return float(np.mean(list(dq)[-n:]))

    # -- drawing --------------------------------------------------------
    def redraw(self):
        if self.last is None:
            return

        self.line.set_data(np.arange(self.nsub), self.last)
        self.ax1.set_ylim(0, max(10.0, float(self.last.max()) * 1.15))

        img = np.zeros((self.history, self.nsub))
        frames = list(self.frames)
        img[self.history - len(frames):] = frames
        self.img.set_data(img)
        self.img.set_clim(0, max(10.0, float(np.percentile(img, 99)) * 1.1))

        if self.activity:
            y = np.asarray(self.activity)
            self.aline.set_data(np.arange(len(y)), y)
        if self.deviation:
            y = np.asarray(self.deviation)
            self.dline.set_data(np.arange(len(y)), y)
        if self.activity or self.deviation:
            n = max(len(self.activity), len(self.deviation), 20)
            top = max([float(np.max(self.activity)) if self.activity else 0.0,
                       float(np.max(self.deviation)) if self.deviation else 0.0,
                       (self.threshold or 0.1) * 2.0, 0.2])
            self.ax3.set_xlim(0, n)
            self.ax3.set_ylim(0, top * 1.1)

        now = time.time()
        if now - self.rate_mark >= 1.0:
            self.rate = self.rate_frames / (now - self.rate_mark)
            self.rate_mark = now
            self.rate_frames = 0

        if self.baseline_norm is not None:
            act = self.smoothed(self.activity)
            dev = self.smoothed(self.deviation)
            self.status.set_text(
                ("PRESENT" if self.present else "quiet")
                + f"   activity {act:.2f}   deviation {dev:.2f}"
                + f"   threshold {self.threshold:.2f}")
            self.status.set_color("tab:red" if self.present else "tab:green")

        self.title.set_text(
            f"{self.mac}  rssi {self.rssi}  {self.nsub} subcarriers  "
            f"{self.rate:.1f} frames/s  total {self.count}")
        self.fig.canvas.draw_idle()


def live(port, baud, nsub, history, calibrate, threshold, save, mac=None):
    q = queue.Queue()
    threading.Thread(target=read_serial, args=(port, baud, q), daemon=True).start()

    p = Plot(nsub, history, calibrate, threshold, mac)

    def update(_):
        while True:
            try:
                line = q.get_nowait()
            except queue.Empty:
                break
            rec = parse_csi(line)
            if rec:
                p.add(rec)
        p.redraw()
        return []

    anim = FuncAnimation(p.fig, update, interval=100, blit=False,
                         cache_frame_data=False)
    if save:
        deadline = time.time() + 15.0
        while time.time() < deadline:
            update(0)
            time.sleep(0.1)
        p.fig.savefig(save, dpi=120)
        print(f"wrote {save}")
        return
    plt.show()
    _ = anim


def replay(path, nsub, history, calibrate, threshold, save, mac=None):
    p = Plot(nsub, history, calibrate, threshold, mac)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            rec = parse_csi(line)
            if rec:
                p.add(rec)
    if p.last is None:
        sys.exit(f"no CSI_DATA lines in {path}")
    p.redraw()
    if save:
        p.fig.savefig(save, dpi=120)
        print(f"wrote {save}  ({p.count} frames)")
    else:
        plt.show()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?", default="/dev/ttyACM0",
                    help="serial port to read (default: /dev/ttyACM0)")
    ap.add_argument("--file", help="replay a capture instead of a serial port")
    ap.add_argument("--baud", type=int, default=115200,
                    help="port baud (USB CDC ignores it; never 1200)")
    ap.add_argument("--subcarriers", type=int, default=64,
                    help="subcarriers per frame (default: 64 = 128 I/Q bytes)")
    ap.add_argument("--history", type=int, default=200,
                    help="waterfall depth in frames (default: 200)")
    ap.add_argument("--calibrate", type=int, default=20,
                    help="empty-room frames at startup (default: 20)")
    ap.add_argument("--mac", help="only use frames from this BSSID (substring, case-insensitive)")
    ap.add_argument("--threshold", type=float,
                    help="override the learned threshold (normalised, ~0.05-0.8)")
    ap.add_argument("--save", help="write a PNG instead of opening a window")
    args = ap.parse_args()

    if args.baud == 1200:
        sys.exit("1200 baud reboots a FreeWili CPU into BOOTSEL; use 115200")

    if args.file:
        replay(args.file, args.subcarriers, args.history, args.calibrate,
               args.threshold, args.save, args.mac)
    else:
        live(args.port, args.baud, args.subcarriers, args.history,
             args.calibrate, args.threshold, args.save, args.mac)


if __name__ == "__main__":
    main()

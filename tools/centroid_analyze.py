"""Does the ecology track the wandering Sun? Autocorrelation of the living-cell
centroid (from tools/centroid_log.py CSVs) over wall-clock lag. A garden that
follows the light shows anticorrelation at half a day and a positive peak at
one day.

    viz/venv/bin/python tools/centroid_analyze.py logs/meridian3-centroid.csv \
        --day-seconds 1684 --out sound/meridian3-centroid.png

--day-seconds = day_steps / average TPS during logging (e.g. 800000 / 475).
"""

import argparse
import csv

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def load(path):
    t, cx, cy = [], [], []
    with open(path) as f:
        for row in csv.DictReader(f):
            t.append(float(row["unix_time"]))
            cx.append(float(row["cx"]))
            cy.append(float(row["cy"]))
    t = np.array(t) - t[0]
    grid = np.arange(0, t[-1], 1.0)
    return grid, np.interp(grid, t, cx), np.interp(grid, t, cy)


def autocorr2d(cx, cy, max_lag):
    vx, vy = cx - cx.mean(), cy - cy.mean()
    norm = (vx * vx + vy * vy).mean()
    lags = np.arange(1, max_lag)
    r = np.array([(vx[:-l] * vx[l:] + vy[:-l] * vy[l:]).mean() / norm for l in lags])
    return lags, r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--day-seconds", type=float, required=True, help="expected day length in wall seconds (day_steps / avg TPS)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    grid, cx, cy = load(args.csv)
    max_lag = int(min(len(grid) * 0.6, args.day_seconds * 2.5))
    lags, r = autocorr2d(cx, cy, max_lag)

    day = args.day_seconds
    half_idx = int(day / 2) - 1
    day_lo, day_hi = int(day * 0.7), min(int(day * 1.3), len(r))
    peak_idx = day_lo + int(np.argmax(r[day_lo:day_hi])) if day_hi > day_lo else 0

    fig, ax = plt.subplots(figsize=(9, 4.5))
    ax.plot(lags, r, lw=1.2, color="#2c3e66")
    ax.axhline(0, color="#999", lw=0.6)
    ax.axvline(day / 2, color="#c06040", ls=":", lw=1, label=f"half day ({day / 2:.0f}s)")
    ax.axvline(day, color="#4a7a4a", ls=":", lw=1, label=f"one day ({day:.0f}s)")
    title = args.label or args.csv
    half_r = r[half_idx] if 0 <= half_idx < len(r) else float("nan")
    ax.set_title(f"{title} — centroid autocorr: r(half)={half_r:.2f}, peak r={r[peak_idx]:.2f} @ {lags[peak_idx]}s")
    ax.set_xlabel("lag (wall seconds)")
    ax.set_ylabel("2D autocorrelation")
    ax.legend(loc="upper right", fontsize=8)
    fig.tight_layout()
    fig.savefig(args.out, dpi=110)
    print(f"{args.out}: r(half-day)={half_r:.3f}, day peak r={r[peak_idx]:.3f} at {lags[peak_idx]}s (expected {day:.0f}s), n={len(grid)}s")


if __name__ == "__main__":
    main()

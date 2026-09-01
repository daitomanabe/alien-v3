"""Log the centroid of the streamed dynamic points (living cells) to a CSV.

The day/night ecology analysis (sound/meridian2-centroid.png) works on this
data: autocorrelation of the centroid position over wall-clock lag reveals
whether the ecology tracks a wandering Sun zone. Run one logger per garden:

    viz/venv/bin/python tools/centroid_log.py --server 100.70.183.86 \
        --port 12001 --out logs/meridian2-centroid.csv

Columns: unix_time, n_points, cx, cy (world units). Wall-clock maps to
timesteps via the server's TPS cap; note the cap and any GPU sharing when
comparing periods across runs.
"""

import argparse
import csv
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "viz"))
from alien_viz import GeomReceiver


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="100.70.183.86")
    ap.add_argument("--port", type=int, default=12001)
    ap.add_argument("--out", required=True)
    ap.add_argument("--interval", type=float, default=1.0)
    args = ap.parse_args()

    rx = GeomReceiver(args.server, args.port)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    new = not out.exists()
    with out.open("a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["unix_time", "n_points", "cx", "cy"])
        last_frames = -1
        while True:
            time.sleep(args.interval)
            points, _, frames = rx.latest()
            if frames == last_frames or len(points) == 0:
                continue
            last_frames = frames
            w.writerow([f"{time.time():.2f}", len(points), f"{points[:, 0].mean():.2f}", f"{points[:, 1].mean():.2f}"])
            f.flush()


if __name__ == "__main__":
    main()

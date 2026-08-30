#!/usr/bin/env python3
"""Live monitor for the ALIEN -> SuperCollider sonification parameters.

Subscribes to alien_server's OSC stream (the same one SuperCollider receives)
and plots every value that drives a synthesis parameter, labeled with its
sound mapping. Rolling 120-second window.

Usage:
    viz/venv/bin/python sound/param_monitor.py [--server 100.87.26.1] [--port 12000]
"""
import argparse
import socket
import struct
import threading
import time
from collections import deque

import dearpygui.dearpygui as dpg

WINDOW_SEC = 120.0  # live UI window; --window overrides (0 = keep everything)
NUM_SLOTS = 8

# alien's 7 ecosystem colors (approximation of the original palette order)
SLOT_COLORS = [
    (120, 180, 255),
    (255, 130, 100),
    (140, 235, 140),
    (250, 210, 90),
    (215, 130, 250),
    (110, 230, 220),
    (250, 160, 210),
    (180, 180, 180),
]


# ---------- OSC parsing (matches OscSender.h: messages + '#bundle') ----------

def _pad4(n: int) -> int:
    return (n + 3) & ~3


def parse_message(data: bytes):
    try:
        end = data.index(b"\x00")
        address = data[:end].decode("ascii", "replace")
        pos = _pad4(end + 1)
        tag_end = data.index(b"\x00", pos)
        tags = data[pos:tag_end].decode("ascii", "replace")
        pos = _pad4(tag_end + 1)
        args = []
        for tag in tags[1:]:
            if tag == "i":
                args.append(struct.unpack_from(">i", data, pos)[0])
                pos += 4
            elif tag == "f":
                args.append(struct.unpack_from(">f", data, pos)[0])
                pos += 4
            elif tag == "s":
                s_end = data.index(b"\x00", pos)
                args.append(data[pos:s_end].decode("ascii", "replace"))
                pos = _pad4(s_end + 1)
        return address, args
    except (ValueError, struct.error):
        return None, []


def parse_packet(data: bytes):
    """Yield (address, args) for a message or every message in a bundle."""
    if data.startswith(b"#bundle"):
        pos = 16
        while pos + 4 <= len(data):
            (size,) = struct.unpack_from(">I", data, pos)
            pos += 4
            if size == 0 or pos + size > len(data):
                break
            yield from parse_packet(data[pos : pos + size])
            pos += size
    elif data.startswith(b"/"):
        address, args = parse_message(data)
        if address:
            yield address, args


# ---------- data model ----------

class Series:
    def __init__(self, window=WINDOW_SEC):
        self.window = window
        self.t = deque()
        self.v = deque()

    def add(self, t, v):
        self.t.append(t)
        self.v.append(v)
        if self.window > 0:
            cutoff = t - self.window
            while self.t and self.t[0] < cutoff:
                self.t.popleft()
                self.v.popleft()

    def xy(self):
        return list(self.t), list(self.v)


class Model:
    def __init__(self, window=WINDOW_SEC):
        self.lock = threading.Lock()
        self.t0 = time.time()
        self.stats = {name: Series(window) for name in ("numCells", "totalEnergy", "tps", "attacks")}
        # per-slot lineage series; slots assigned like the SC patch (by arrival order per tick)
        self.lineage = [
            {name: Series(window) for name in ("pop", "muscle", "attack", "mut", "gen")}
            for _ in range(NUM_SLOTS)
        ]
        self.slot_lineage_id = [-1] * NUM_SLOTS
        self._tick_slot = 0
        self._last_stats_t = 0.0

    def now(self):
        return time.time() - self.t0

    def on_message(self, address, args):
        t = self.now()
        with self.lock:
            if address == "/alien/stats" and len(args) >= 6:
                self.stats["numCells"].add(t, args[0])
                self.stats["totalEnergy"].add(t, args[3])
                self.stats["tps"].add(t, args[4])
                self._tick_slot = 0  # stats message starts a tick: reset slot counter
            elif address == "/alien/lineage" and len(args) >= 9:
                slot = self._tick_slot % NUM_SLOTS
                self._tick_slot += 1
                self.slot_lineage_id[slot] = args[0]
                series = self.lineage[slot]
                series["pop"].add(t, args[1])
                series["muscle"].add(t, args[4])
                series["attack"].add(t, args[5])
                series["mut"].add(t, args[6])
                series["gen"].add(t, args[7])
            elif address == "/alien/attacks" and len(args) >= 1:
                self.stats["attacks"].add(t, args[0])


def receiver_thread(model: Model, server: str, port: int):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", 0))
    sock.settimeout(0.5)
    last_hello = 0.0
    while True:
        if time.time() - last_hello > 5:
            try:
                sock.sendto(b"/alien/subscribe\x00\x00\x00,\x00\x00\x00", (server, port))
            except OSError:
                pass
            last_hello = time.time()
        try:
            data, _ = sock.recvfrom(8192)
        except socket.timeout:
            continue
        except OSError:
            break
        for address, args in parse_packet(data):
            model.on_message(address, args)


# ---------- UI ----------

PLOTS = [
    # (tag, title with sound mapping, per-lineage?, series key)
    ("pop", "population  ->  voice amplitude (amp = sqrt(pop/popMax) * 0.14)", True, "pop"),
    ("gen", "avg generations  ->  pitch (freq * 2^(gen/1500))", True, "gen"),
    ("muscle", "muscle activity delta  ->  shimmer / tremolo depth+rate", True, "muscle"),
    ("attack", "predation energy delta  ->  grit (pink noise mix)", True, "attack"),
    ("mut", "mutation delta  ->  detune (osc spread)", True, "mut"),
    ("global", "global: cells -> reverb mix | energy -> master LPF | attacks -> percussion density", False, None),
]


def build_ui(model: Model, server: str):
    dpg.create_context()
    with dpg.window(tag="root"):
        dpg.add_text(f"ALIEN sonification parameters   (OSC from {server}:12000, same stream SuperCollider hears)")
        dpg.add_text("one colored line per lineage slot (top-8 lineages by population)", color=(160, 160, 160))
        with dpg.subplots(3, 2, label="", width=-1, height=-1, link_all_x=True):
            for tag, title, per_lineage, _key in PLOTS:
                with dpg.plot(label=title, tag=f"plot_{tag}"):
                    dpg.add_plot_axis(dpg.mvXAxis, label="s", tag=f"x_{tag}")
                    dpg.add_plot_axis(dpg.mvYAxis, label="", tag=f"y_{tag}")
                    if per_lineage:
                        for slot in range(NUM_SLOTS):
                            theme = _line_theme(SLOT_COLORS[slot])
                            dpg.add_line_series([], [], parent=f"y_{tag}", tag=f"s_{tag}_{slot}")
                            dpg.bind_item_theme(f"s_{tag}_{slot}", theme)
                    else:
                        for i, (name, color) in enumerate(
                            [("numCells", (120, 180, 255)), ("totalEnergy", (250, 210, 90)), ("attacks", (255, 100, 80)), ("tps", (140, 235, 140))]
                        ):
                            dpg.add_line_series([], [], parent=f"y_{tag}", label=name, tag=f"s_{tag}_{name}")
                            dpg.bind_item_theme(f"s_{tag}_{name}", _line_theme(color))
                        dpg.add_plot_legend(parent=f"plot_{tag}")

    dpg.create_viewport(title="ALIEN -> sound parameters", width=1500, height=950)
    dpg.setup_dearpygui()
    dpg.show_viewport()
    dpg.set_primary_window("root", True)


def _line_theme(color):
    with dpg.theme() as theme:
        with dpg.theme_component(dpg.mvLineSeries):
            dpg.add_theme_color(dpg.mvPlotCol_Line, (*color, 255), category=dpg.mvThemeCat_Plots)
    return theme


def update_ui(model: Model):
    with model.lock:
        for tag, _title, per_lineage, key in PLOTS:
            if per_lineage:
                for slot in range(NUM_SLOTS):
                    xs, ys = model.lineage[slot][key].xy()
                    dpg.set_value(f"s_{tag}_{slot}", [xs, ys])
            else:
                for name in ("numCells", "totalEnergy", "attacks", "tps"):
                    xs, ys = model.stats[name].xy()
                    # normalize energy into cell-count scale for one shared axis
                    if name == "totalEnergy" and ys:
                        ys = [v / 1000.0 for v in ys]
                    dpg.set_value(f"s_{tag}_{name}", [xs, ys])
        now = model.now()
    for tag, _t, _p, _k in PLOTS:
        dpg.set_axis_limits(f"x_{tag}", max(0.0, now - WINDOW_SEC), max(WINDOW_SEC * 0.1, now))
        dpg.fit_axis_data(f"y_{tag}")


def export_png(model: Model, path: str):
    """Static matplotlib rendition of the live plots (record keeping / docs)."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    slot_hex = ["#78b4ff", "#ff8264", "#8ceb8c", "#fad25a", "#d782fa", "#6ee6dc", "#faa0d2", "#b4b4b4"]
    fig, axes = plt.subplots(3, 2, figsize=(16, 10), sharex=True)
    fig.suptitle("ALIEN -> SuperCollider sonification parameters (rolling window)", fontsize=13)

    with model.lock:
        for ax, (tag, title, per_lineage, key) in zip(axes.flat, PLOTS):
            ax.set_title(title, fontsize=9)
            ax.grid(alpha=0.25)
            if per_lineage:
                for slot in range(NUM_SLOTS):
                    xs, ys = model.lineage[slot][key].xy()
                    if xs:
                        ax.plot(xs, ys, color=slot_hex[slot], linewidth=1.0, label=f"lineage {model.slot_lineage_id[slot]}")
            else:
                for name, color in [("numCells", "#78b4ff"), ("totalEnergy", "#fad25a"), ("attacks", "#ff6450"), ("tps", "#8ceb8c")]:
                    xs, ys = model.stats[name].xy()
                    if name == "totalEnergy":
                        ys = [v / 1000.0 for v in ys]
                    if xs:
                        ax.plot(xs, ys, color=color, linewidth=1.0, label=name)
                ax.legend(fontsize=7, loc="upper left")
    axes.flat[0].legend(fontsize=6, loc="upper left", ncols=2)
    for ax in axes[-1]:
        ax.set_xlabel("seconds")
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    print(f"exported: {path}", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="100.87.26.1")
    parser.add_argument("--port", type=int, default=12000)
    parser.add_argument("--export", default="", help="save a matplotlib PNG of all series on exit (and every --export-every s)")
    parser.add_argument("--export-every", type=float, default=0.0)
    parser.add_argument("--exit-after", type=float, default=0.0)
    parser.add_argument("--headless", action="store_true", help="no UI window; collect OSC and export only")
    parser.add_argument("--window", type=float, default=WINDOW_SEC, help="seconds of history to keep (0 = everything; use for long recordings)")
    args = parser.parse_args()

    model = Model(window=args.window)
    threading.Thread(target=receiver_thread, args=(model, args.server, args.port), daemon=True).start()

    start = time.time()

    if args.headless:
        while args.exit_after <= 0 or time.time() - start < args.exit_after:
            time.sleep(0.5)
        if args.export:
            export_png(model, args.export)
        return

    build_ui(model, args.server)

    last_update = 0.0
    last_export = start
    while dpg.is_dearpygui_running():
        now = time.time()
        if now - last_update > 0.1:
            update_ui(model)
            last_update = now
        dpg.render_dearpygui_frame()

        if args.export and args.export_every > 0 and now - last_export > args.export_every:
            last_export = now
            export_png(model, args.export)
        if args.exit_after > 0 and now - start > args.exit_after:
            break

    if args.export:
        export_png(model, args.export)
    dpg.destroy_context()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Rewrite the spatial design of an ALIEN parameters.json.

Takes a dumped parameters.json as template (keeping its ecosystem balance:
food chains, energy economy, mutation rates) and re-arranges the 11 layer
zones into an original spatial design. v1 ships one design: "vortex".

Usage:
    python3 tools/garden_env.py --base garden-dump/parameters.json \
        --out scenes/vortex-params.json --design vortex --world 3000x3000
"""
import argparse
import json
import math

# ForceField_ enum (SimulationParametersTypes.h)
FF_NONE, FF_RADIAL, FF_CENTRAL, FF_LINEAR, FF_PERLIN = 0, 1, 2, 3, 4
SHAPE_CIRC, SHAPE_RECT = 0, 1
CW, CCW = 0, 1


def fmt(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, str)):
        return str(v)
    return f"{v:.8f}"


class Params:
    def __init__(self, path):
        self.doc = json.load(open(path))
        self.sp = self.doc["Simulation parameters"]

    def set(self, path, layer, value, enabled=None):
        """Set <path...>/Layer <i>/Value (and optionally /Enabled)."""
        node = self.sp
        for key in path:
            node = node.setdefault(key, {})
        slot = node.setdefault(f"Layer {layer}", {})
        slot["Value"] = fmt(value)
        if enabled is not None:
            slot["Enabled"] = fmt(enabled)

    def set_pos(self, layer, x, y):
        node = self.sp["Location"]["Position (x,y)"].setdefault(f"Layer {layer}", {}).setdefault("Value", {})
        node["X"] = fmt(float(x))
        node["Y"] = fmt(float(y))
        vel = self.sp["Location"]["Velocity (x,y)"].setdefault(f"Layer {layer}", {}).setdefault("Value", {})
        vel["X"] = fmt(0.0)
        vel["Y"] = fmt(0.0)

    def save(self, path):
        json.dump(self.doc, open(path, "w"), indent=1)


def apply_layer(p: Params, i, name, pos, shape, fade, field):
    p.set(["General", "Layer name"], i, name)
    p.set_pos(i, *pos)
    kind = shape[0]
    p.set(["Shape", "Shape"], i, SHAPE_CIRC if kind == "circ" else SHAPE_RECT)
    if kind == "circ":
        p.set(["Shape", "Shape", "Circular", "Core radius"], i, float(shape[1]))
    else:
        node = p.sp["Shape"]["Shape"]["Rectangular"]["Core size (width,height)"].setdefault(f"Layer {i}", {}).setdefault("Value", {})
        node["X"] = fmt(float(shape[1]))
        node["Y"] = fmt(float(shape[2]))
    p.set(["Shape", "Fade-out radius"], i, float(fade))

    if field is None or field == "off":
        p.set(["Force field", "Field type"], i, FF_NONE, enabled=False)
        return
    kind = field[0]
    if kind == "perlin":
        _, strength, spatial, temporal = field
        p.set(["Force field", "Field type"], i, FF_PERLIN, enabled=True)
        p.set(["Force field", "Field type", "Perlin noise", "Strength"], i, strength)
        p.set(["Force field", "Field type", "Perlin noise", "Spatial structure size"], i, spatial)
        p.set(["Force field", "Field type", "Perlin noise", "Temporal structure size"], i, temporal)
    elif kind == "radial":
        _, strength, orientation, drift = field
        p.set(["Force field", "Field type"], i, FF_RADIAL, enabled=True)
        p.set(["Force field", "Field type", "Radial", "Strength"], i, strength)
        p.set(["Force field", "Field type", "Radial", "Orientation"], i, orientation)
        p.set(["Force field", "Field type", "Radial", "Drift angle"], i, drift)
    elif kind == "linear":
        _, strength, angle = field
        p.set(["Force field", "Field type"], i, FF_LINEAR, enabled=True)
        p.set(["Force field", "Field type", "Linear", "Strength"], i, strength)
        p.set(["Force field", "Field type", "Linear", "Angle"], i, angle)
    elif kind == "central":
        _, strength = field
        p.set(["Force field", "Field type"], i, FF_CENTRAL, enabled=True)
        p.set(["Force field", "Field type", "Central", "Strength"], i, strength)


def spiral_point(cx, cy, r0, r1, turns, t):
    """Point on an Archimedean spiral at parameter t in [0,1]."""
    theta = 2 * math.pi * turns * t
    r = r0 + (r1 - r0) * t
    return cx + r * math.cos(theta), cy + r * math.sin(theta), theta


def design_vortex(p: Params, world_w, world_h):
    """v2: dispersal restored. The upstream garden spreads offspring with
    gravity and storms; a weightless vortex turned every plant into one
    giant unsplit tree. Here strong rotation, storm-grade arm gusts and a
    central maw carry offspring to new ground."""
    cx, cy = world_w / 2, world_h / 2
    r_out = min(world_w, world_h) * 0.45

    # L0 the whole world breathes (visible large-scale Perlin churn)
    apply_layer(p, 0, "Breath", (cx, cy), ("rect", world_w, world_h), 0, ("perlin", 0.010, 120, 6000))
    # L1 the vortex: storm-grade clockwise rotation over most of the garden
    apply_layer(p, 1, "Vortex", (cx, cy), ("circ", r_out * 0.85), r_out * 0.25, ("radial", 0.05, CW, 0.0))
    # L2 lively core turbulence
    apply_layer(p, 2, "Core", (cx, cy), ("circ", r_out * 0.22), r_out * 0.12, ("perlin", 0.02, 50, 3000))
    # L3..L5 arm winds: storm-grade gusts tangential to the spiral arms
    for k, (idx, t) in enumerate([(3, 0.35), (4, 0.6), (5, 0.85)]):
        x, y, theta = spiral_point(cx, cy, r_out * 0.15, r_out, 2.6, t)
        tangent_deg = math.degrees(theta + math.pi / 2) % 360
        apply_layer(p, idx, f"Arm wind {chr(65 + k)}", (x, y), ("circ", 260), 200, ("linear", 0.004, tangent_deg))
    # L6 the maw: a narrow central sink (Central decays ~1/r^2, so this only bites nearby)
    apply_layer(p, 6, "Maw", (cx, cy), ("circ", 160), 220, ("central", 3.0))
    # L7 keeps its inherited role but is parked away, field off (was disabled upstream)
    apply_layer(p, 7, "Parked", (world_w * 0.05, world_h * 0.95), ("circ", 100), 50, "off")
    # L8/L9 inherited ground ecology (food-chain overrides), re-seated on the spiral
    x8, y8, _ = spiral_point(cx, cy, r_out * 0.15, r_out, 2.6, 0.25)
    x9, y9, _ = spiral_point(cx, cy, r_out * 0.15, r_out, 2.6, 0.75)
    apply_layer(p, 8, "Soil inner", (x8, y8), ("circ", 360), 240, "off")
    apply_layer(p, 9, "Soil outer", (x9, y9), ("circ", 360), 240, "off")
    # L10 inherited "Size Bonus" sits on the core
    apply_layer(p, 10, "Core bonus", (cx, cy), ("circ", r_out * 0.22), r_out * 0.1, "off")


def design_rain(p: Params, world_w, world_h):
    """A tall world under steady gravity. Life is seeded on sparse shelves
    near the top; offspring, energy and debris rain down through wind bands
    to sediment beds at the bottom. Vertical position is register: things
    literally fall in pitch."""
    cx = world_w / 2

    # L0 gravity everywhere (3x upstream's hanging gravity: a real, visible fall)
    apply_layer(p, 0, "Gravity", (cx, world_h / 2), ("rect", world_w, world_h), 0, ("linear", 5e-5, 180.0))
    # L1 cloud deck: churn where life is born
    apply_layer(p, 1, "Cloud", (cx, world_h * 0.12), ("rect", world_w, world_h * 0.2), world_h * 0.05, ("perlin", 0.012, 90, 5000))
    # L2/L3 crosswind bands at falling heights (opposite directions -> zigzag descent)
    apply_layer(p, 2, "Wind east", (cx, world_h * 0.4), ("rect", world_w, world_h * 0.14), world_h * 0.06, ("linear", 0.0015, 90.0))
    apply_layer(p, 3, "Wind west", (cx, world_h * 0.62), ("rect", world_w, world_h * 0.14), world_h * 0.06, ("linear", 0.0015, 270.0))
    # L4 ground gust just above the floor (stirs the sediment)
    apply_layer(p, 4, "Ground gust", (cx, world_h * 0.9), ("rect", world_w, world_h * 0.1), world_h * 0.04, ("perlin", 0.006, 60, 4000))
    # L5..L7 unused wind slots parked, fields off
    apply_layer(p, 5, "Parked A", (world_w * 0.05, world_h * 0.02), ("circ", 60), 30, "off")
    apply_layer(p, 6, "Parked B", (world_w * 0.95, world_h * 0.02), ("circ", 60), 30, "off")
    apply_layer(p, 7, "Parked C", (world_w * 0.05, world_h * 0.98), ("circ", 60), 30, "off")
    # L8/L9 inherited soil ecology on the sediment beds
    apply_layer(p, 8, "Sediment W", (world_w * 0.28, world_h * 0.92), ("circ", world_w * 0.22), world_w * 0.1, "off")
    apply_layer(p, 9, "Sediment E", (world_w * 0.72, world_h * 0.92), ("circ", world_w * 0.22), world_w * 0.1, "off")
    # L10 inherited bonus on the cloud deck (birth zone)
    apply_layer(p, 10, "Cloud bonus", (cx, world_h * 0.12), ("rect", world_w, world_h * 0.16), world_h * 0.04, "off")


def design_islands(p: Params, world_w, world_h, num_islands=8):
    """An archipelago: each island gets its own zone with its own weather.
    Lineages evolve apart; in the sonification each island keeps a fixed
    bearing (polar pan), so the archipelago becomes a fixed stereo stage."""
    cx, cy = world_w / 2, world_h / 2
    ring_r = min(world_w, world_h) * 0.33
    island_r = min(world_w, world_h) * 0.115

    weathers = [
        ("perlin", 0.010, 60, 4000),
        ("radial", 0.03, CW, 0.0),
        ("linear", 0.002, 45.0),
        "off",
        ("perlin", 0.018, 35, 2500),
        ("radial", 0.03, CCW, 0.0),
        ("linear", 0.002, 225.0),
        "off",
    ]
    for i in range(num_islands):
        angle = 2 * math.pi * i / num_islands
        x = cx + ring_r * math.cos(angle)
        y = cy + ring_r * math.sin(angle)
        apply_layer(p, i, f"Island {i}", (x, y), ("circ", island_r), island_r * 0.5, weathers[i % len(weathers)])
    # L8/L9 inherited soil ecology: two islands double as fertile ground
    x8 = cx + ring_r * math.cos(0.0)
    y8 = cy + ring_r * math.sin(0.0)
    x9 = cx + ring_r * math.cos(math.pi)
    y9 = cy + ring_r * math.sin(math.pi)
    apply_layer(p, 8, "Fertile E", (x8, y8), ("circ", island_r), island_r * 0.5, "off")
    apply_layer(p, 9, "Fertile W", (x9, y9), ("circ", island_r), island_r * 0.5, "off")
    # L10 inherited bonus on the open sea center
    apply_layer(p, 10, "Open sea", (cx, cy), ("circ", ring_r * 0.5), ring_r * 0.25, "off")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--design", default="vortex", choices=["vortex", "rain", "islands"])
    ap.add_argument("--world", default="3000x3000")
    ap.add_argument(
        "--energy-pool",
        type=float,
        default=0,
        help="external energy pool (0 = keep template). Upstream uses 12e6; a small pool starves the garden over time — famine ignites predation (seasons)",
    )
    args = ap.parse_args()
    w, h = (float(v) for v in args.world.split("x"))

    p = Params(args.base)
    if args.design == "rain":
        design_rain(p, w, h)
    elif args.design == "islands":
        design_islands(p, w, h)
    else:
        design_vortex(p, w, h)
    if args.energy_pool > 0:
        p.sp["External energy control"]["External energy amount"]["Base"]["Value"] = fmt(args.energy_pool)
        print(f"external energy pool: {args.energy_pool:.0f}")
    p.save(args.out)

    names = [p.sp["General"]["Layer name"][f"Layer {i}"]["Value"] for i in range(11)]
    print(f"design '{args.design}' written to {args.out} for world {w:.0f}x{h:.0f}")
    print("layers:", ", ".join(names))


if __name__ == "__main__":
    main()

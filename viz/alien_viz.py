#!/usr/bin/env python3
"""ALIEN custom renderer — receives the alien_server geometry stream and draws
the ecosystem in an ink-on-paper look (default) or additive glow.

The ink look is a deliberate re-interpretation of ALIEN's neon original:
warm paper, sumi-ink accumulation via Beer-Lambert absorption, cell-cell
connection lines as skeletal strokes, attack events as vermilion blots,
motion trails from decaying accumulation with a slight bleed blur.

Usage:
    viz/venv/bin/python viz/alien_viz.py [--look ink|glow] [--server HOST]
"""
import argparse
import socket
import struct
import threading
import time

import glfw
import moderngl
import numpy as np

MAGIC = 0x414C4E33
HEADER = struct.Struct("<IIHHHH")
TYPE_POINTS = 0
TYPE_LINES = 1
POINT_SIZE = 12
LINE_SIZE = 20


class GeomReceiver:
    """Subscribes to alien_server; reassembles chunked frames per payload type.
    Tolerates UDP loss: a frame is published with whatever chunks arrived."""

    def __init__(self, server: str, port: int):
        self.addr = (server, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", 0))
        self.sock.settimeout(0.5)
        self.lock = threading.Lock()
        self.points = np.zeros((0, 3), dtype=np.float32)   # x, y, packed rgba(u32-as-f32)
        self.line_verts = np.zeros((0, 3), dtype=np.float32)
        self.frames = 0
        self._chunks = {TYPE_POINTS: {}, TYPE_LINES: {}}
        self._frame_ids = {TYPE_POINTS: -1, TYPE_LINES: -1}
        self._running = True
        threading.Thread(target=self._recv_loop, daemon=True).start()
        threading.Thread(target=self._keepalive_loop, daemon=True).start()

    def _keepalive_loop(self):
        while self._running:
            try:
                self.sock.sendto(b"subscribe", self.addr)
            except OSError:
                pass
            time.sleep(5)

    def _recv_loop(self):
        while self._running:
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < HEADER.size:
                continue
            magic, frame_id, chunk_idx, num_chunks, payload_len, ptype = HEADER.unpack_from(data)
            if magic != MAGIC or ptype not in self._chunks:
                continue
            chunks = self._chunks[ptype]
            if frame_id != self._frame_ids[ptype]:
                if chunks:
                    self._publish(ptype, b"".join(chunks.values()))
                self._frame_ids[ptype] = frame_id
                chunks.clear()
            chunks[chunk_idx] = data[HEADER.size : HEADER.size + payload_len]
            if len(chunks) == num_chunks:
                self._publish(ptype, b"".join(chunks.values()))
                chunks.clear()

    def _publish(self, ptype: int, blob: bytes):
        if ptype == TYPE_POINTS:
            count = len(blob) // POINT_SIZE
            if count == 0:
                return
            raw = np.frombuffer(blob[: count * POINT_SIZE], dtype=np.uint8).reshape(count, POINT_SIZE)
            pts = np.empty((count, 3), dtype=np.float32)
            pts[:, 0:2] = raw[:, 0:8].copy().view(np.float32).reshape(count, 2)
            pts[:, 2] = raw[:, 8:12].copy().view(np.uint32).reshape(count).view(np.float32)
            with self.lock:
                self.points = pts
                self.frames += 1
        else:
            count = len(blob) // LINE_SIZE
            if count == 0:
                return
            raw = np.frombuffer(blob[: count * LINE_SIZE], dtype=np.uint8).reshape(count, LINE_SIZE)
            xy = raw[:, 0:16].copy().view(np.float32).reshape(count, 4)
            packed = raw[:, 16:20].copy().view(np.uint32).reshape(count).view(np.float32)
            verts = np.empty((count * 2, 3), dtype=np.float32)
            verts[0::2, 0:2] = xy[:, 0:2]
            verts[1::2, 0:2] = xy[:, 2:4]
            verts[0::2, 2] = packed
            verts[1::2, 2] = packed
            with self.lock:
                self.line_verts = verts

    def latest(self):
        with self.lock:
            return self.points, self.line_verts, self.frames


GEOM_VERT = """
#version 330
uniform vec2 u_world;
uniform float u_ink;
uniform float u_point_scale;
in vec2 in_pos;
in float in_packed;
out vec3 v_color;
out float v_kind;   // 0 = cell, 1 = fluid, 2 = attack
void main() {
    vec2 ndc = (in_pos / u_world) * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    uint packed = floatBitsToUint(in_packed);
    float r = float(packed & 255u) / 255.0;
    float g = float((packed >> 8) & 255u) / 255.0;
    float b = float((packed >> 16) & 255u) / 255.0;
    uint flags = (packed >> 24) & 255u;
    bool fluid = (flags & 128u) != 0u;
    bool attack = (flags & 64u) != 0u;
    v_kind = attack ? 2.0 : (fluid ? 1.0 : 0.0);
    v_color = vec3(r, g, b);
    float size = fluid ? 3.0 : (u_ink > 0.5 ? 4.0 : 5.0);
    if (attack) size = 14.0;
    gl_PointSize = size * u_point_scale;
}
"""

POINTS_FRAG = """
#version 330
uniform float u_ink;
in vec3 v_color;
in float v_kind;
out vec4 f_color;
void main() {
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;
    float fall = exp(-r2 * 3.0);
    if (u_ink > 0.5) {
        // emit ABSORPTION per channel (Beer-Lambert accumulator)
        vec3 inkColor;
        float amount;
        if (v_kind > 1.5) {          // attack: vermilion blot
            inkColor = vec3(0.78, 0.10, 0.04);
            amount = 0.45;
        } else if (v_kind > 0.5) {   // fluid: faint wash
            inkColor = vec3(0.38, 0.40, 0.46);
            amount = 0.035;
        } else {                     // cell: sumi with a trace of lineage color
            inkColor = mix(vec3(0.09, 0.09, 0.12), v_color * 0.75, 0.55);
            amount = 0.40;
        }
        vec3 absorption = (vec3(1.0) - inkColor) * amount * fall;
        f_color = vec4(absorption, 1.0);
    } else {
        float gain = (v_kind > 1.5) ? 2.0 : ((v_kind > 0.5) ? 0.35 : 1.0);
        f_color = vec4(v_color * fall * gain, 1.0);
    }
}
"""

LINES_FRAG = """
#version 330
uniform float u_ink;
in vec3 v_color;
in float v_kind;
out vec4 f_color;
void main() {
    if (u_ink > 0.5) {
        vec3 inkColor = mix(vec3(0.05, 0.05, 0.08), v_color * 0.5, 0.25);
        vec3 absorption = (vec3(1.0) - inkColor) * 0.85;
        f_color = vec4(absorption, 1.0);
    } else {
        f_color = vec4(v_color * 0.5, 1.0);
    }
}
"""

QUAD_VERT = """
#version 330
in vec2 in_pos;
out vec2 v_uv;
void main() {
    v_uv = in_pos * 0.5 + 0.5;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
"""

DECAY_FRAG = """
#version 330
uniform sampler2D u_tex;
uniform float u_decay;
uniform float u_bleed;   // 0..1 mix of 3x3 tent blur (ink bleeding into paper)
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec2 px = 1.0 / vec2(textureSize(u_tex, 0));
    vec4 c = texture(u_tex, v_uv);
    vec4 blur = c * 0.25;
    blur += texture(u_tex, v_uv + vec2( px.x, 0)) * 0.125;
    blur += texture(u_tex, v_uv + vec2(-px.x, 0)) * 0.125;
    blur += texture(u_tex, v_uv + vec2(0,  px.y)) * 0.125;
    blur += texture(u_tex, v_uv + vec2(0, -px.y)) * 0.125;
    blur += texture(u_tex, v_uv + vec2( px.x,  px.y)) * 0.0625;
    blur += texture(u_tex, v_uv + vec2(-px.x,  px.y)) * 0.0625;
    blur += texture(u_tex, v_uv + vec2( px.x, -px.y)) * 0.0625;
    blur += texture(u_tex, v_uv + vec2(-px.x, -px.y)) * 0.0625;
    f_color = mix(c, blur, u_bleed) * u_decay;
}
"""

DISPLAY_FRAG = """
#version 330
uniform sampler2D u_tex;
uniform float u_ink;
uniform float u_strength;
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec3 a = texture(u_tex, v_uv).rgb;
    if (u_ink > 0.5) {
        vec3 paper = vec3(0.956, 0.945, 0.918);   // warm washi
        // subtle paper vignette
        vec2 d = v_uv - 0.5;
        paper *= 1.0 - dot(d, d) * 0.18;
        vec3 c = paper * exp(-a * u_strength);
        f_color = vec4(c, 1.0);
    } else {
        vec3 c = a / (1.0 + a);
        c = pow(c, vec3(0.4545));
        f_color = vec4(c, 1.0);
    }
}
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="100.87.26.1")
    parser.add_argument("--port", type=int, default=12001)
    parser.add_argument("--world", default="5000x1500", help="world size WxH (matches the loaded scene)")
    parser.add_argument("--width", type=int, default=1500)
    parser.add_argument("--look", choices=["ink", "glow"], default="ink")
    parser.add_argument("--decay", type=float, default=0.0, help="trail decay per frame (0 = per-look default)")
    parser.add_argument("--snapshot", default="", help="write a PPM of the screen to this path after --snapshot-after seconds")
    parser.add_argument("--snapshot-after", type=float, default=15.0)
    parser.add_argument("--exit-after", type=float, default=0.0, help="quit after N seconds (0 = run until closed)")
    args = parser.parse_args()
    world_w, world_h = (float(v) for v in args.world.split("x"))
    win_w = args.width
    win_h = max(200, int(win_w * world_h / world_w))
    ink = args.look == "ink"
    decay = args.decay if args.decay > 0 else (0.90 if ink else 0.94)
    bleed = 0.35 if ink else 0.0
    ink_strength = 1.6

    receiver = GeomReceiver(args.server, args.port)

    if not glfw.init():
        raise SystemExit("glfw init failed")
    glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 3)
    glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 3)
    glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_CORE_PROFILE)
    glfw.window_hint(glfw.OPENGL_FORWARD_COMPAT, True)
    window = glfw.create_window(win_w, win_h, f"ALIEN v3 — {args.look}", None, None)
    glfw.make_context_current(window)
    glfw.swap_interval(1)

    ctx = moderngl.create_context()
    ctx.enable(moderngl.PROGRAM_POINT_SIZE)

    fb_w, fb_h = glfw.get_framebuffer_size(window)
    point_scale = fb_w / win_w  # retina

    points_prog = ctx.program(vertex_shader=GEOM_VERT, fragment_shader=POINTS_FRAG)
    lines_prog = ctx.program(vertex_shader=GEOM_VERT, fragment_shader=LINES_FRAG)
    decay_prog = ctx.program(vertex_shader=QUAD_VERT, fragment_shader=DECAY_FRAG)
    display_prog = ctx.program(vertex_shader=QUAD_VERT, fragment_shader=DISPLAY_FRAG)
    for prog in (points_prog, lines_prog):
        prog["u_world"].value = (world_w, world_h)
        prog["u_ink"].value = 1.0 if ink else 0.0
        prog["u_point_scale"].value = point_scale
    display_prog["u_ink"].value = 1.0 if ink else 0.0
    display_prog["u_strength"].value = ink_strength
    decay_prog["u_decay"].value = decay
    decay_prog["u_bleed"].value = bleed

    quad = ctx.buffer(np.array([-1, -1, 3, -1, -1, 3], dtype=np.float32))
    decay_vao = ctx.vertex_array(decay_prog, [(quad, "2f", "in_pos")])
    display_vao = ctx.vertex_array(display_prog, [(quad, "2f", "in_pos")])

    point_buf = ctx.buffer(reserve=POINT_SIZE * 100000, dynamic=True)
    points_vao = ctx.vertex_array(points_prog, [(point_buf, "2f 1f", "in_pos", "in_packed")])
    line_buf = ctx.buffer(reserve=12 * 100000, dynamic=True)
    lines_vao = ctx.vertex_array(lines_prog, [(line_buf, "2f 1f", "in_pos", "in_packed")])

    accum = [ctx.texture((fb_w, fb_h), 4, dtype="f2") for _ in range(2)]
    fbos = [ctx.framebuffer(color_attachments=[t]) for t in accum]
    for f in fbos:
        f.use()
        ctx.clear(0, 0, 0, 1)

    cur = 0
    last_frames = -1
    n_points = 0
    n_line_verts = 0
    fps_t0, fps_n = time.time(), 0
    start_t = time.time()
    snapshot_done = False

    while not glfw.window_should_close(window):
        glfw.poll_events()
        if glfw.get_key(window, glfw.KEY_ESCAPE) == glfw.PRESS:
            break

        pts, line_verts, frames = receiver.latest()
        if frames != last_frames:
            last_frames = frames
            if len(pts):
                data = pts.tobytes()
                if len(data) > point_buf.size:
                    point_buf.orphan(len(data) * 2)
                point_buf.write(data)
                n_points = len(pts)
            if len(line_verts):
                data = line_verts.tobytes()
                if len(data) > line_buf.size:
                    line_buf.orphan(len(data) * 2)
                line_buf.write(data)
                n_line_verts = len(line_verts)

        nxt = 1 - cur
        fbos[nxt].use()
        accum[cur].use(0)
        ctx.blend_func = moderngl.ONE, moderngl.ZERO
        ctx.enable(moderngl.BLEND)
        decay_vao.render(moderngl.TRIANGLES)
        # additive accumulation (light in glow mode, absorption in ink mode)
        ctx.blend_func = moderngl.ONE, moderngl.ONE
        if n_line_verts:
            lines_vao.render(moderngl.LINES, vertices=n_line_verts)
        if n_points:
            points_vao.render(moderngl.POINTS, vertices=n_points)
        # display
        ctx.screen.use()
        accum[nxt].use(0)
        ctx.blend_func = moderngl.ONE, moderngl.ZERO
        display_vao.render(moderngl.TRIANGLES)

        elapsed = time.time() - start_t
        if args.snapshot and not snapshot_done and elapsed > args.snapshot_after:
            snapshot_done = True
            sw, sh = ctx.screen.width, ctx.screen.height
            raw = ctx.screen.read(components=3)
            with open(args.snapshot, "wb") as f:
                f.write(f"P6\n{sw} {sh}\n255\n".encode())
                arr = np.frombuffer(raw, dtype=np.uint8).reshape(sh, sw, 3)[::-1]
                f.write(arr.tobytes())
            print(f"snapshot written: {args.snapshot} ({sw}x{sh})", flush=True)

        glfw.swap_buffers(window)
        cur = nxt

        fps_n += 1
        if time.time() - fps_t0 > 5:
            print(f"fps {fps_n / (time.time() - fps_t0):.1f}, points {n_points}, lineVerts {n_line_verts}, frames rx {frames}", flush=True)
            fps_t0, fps_n = time.time(), 0

        if args.exit_after > 0 and elapsed > args.exit_after:
            break

    glfw.terminate()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""ALIEN custom renderer: receives the alien_server geometry stream and draws
the ecosystem as additive glowing points with decaying trails.

Usage:
    viz/venv/bin/python viz/alien_viz.py [--server 100.87.26.1] [--port 12001]

The server streams whatever world region it extracts; this renderer is a
deliberate re-interpretation (not a port) of ALIEN's original look: black
field, ink-like additive glow, motion trails from temporal accumulation.
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
POINT_SIZE = 12


class GeomReceiver:
    """Subscribes to alien_server and reassembles chunked point frames."""

    def __init__(self, server: str, port: int):
        self.addr = (server, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", 0))
        self.sock.settimeout(0.5)
        self.lock = threading.Lock()
        self.points = np.zeros((0, 3), dtype=np.float32)  # x, y, packed color+flags
        self.frames = 0
        self._chunks = {}
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
        current_frame = -1
        while self._running:
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < HEADER.size:
                continue
            magic, frame_id, chunk_idx, num_chunks, payload_len, _ = HEADER.unpack_from(data)
            if magic != MAGIC:
                continue
            if frame_id != current_frame:
                # publish whatever arrived of the previous frame (UDP loss tolerant:
                # each chunk is self-contained points, order does not matter)
                if self._chunks:
                    self._publish(b"".join(self._chunks.values()))
                current_frame = frame_id
                self._chunks = {}
            self._chunks[chunk_idx] = data[HEADER.size : HEADER.size + payload_len]
            if len(self._chunks) == num_chunks:
                self._publish(b"".join(self._chunks.values()))
                self._chunks = {}

    def _publish(self, blob: bytes):
        count = len(blob) // POINT_SIZE
        if count == 0:
            return
        raw = np.frombuffer(blob[: count * POINT_SIZE], dtype=np.uint8).reshape(count, POINT_SIZE)
        xy = raw[:, 0:8].copy().view(np.float32).reshape(count, 2)
        rgbf = raw[:, 8:12].copy().view(np.uint32).reshape(count)  # packed r,g,b,flags
        pts = np.empty((count, 3), dtype=np.float32)
        pts[:, 0:2] = xy
        pts[:, 2] = rgbf.view(np.float32)
        with self.lock:
            self.points = pts
            self.frames += 1

    def latest(self):
        with self.lock:
            return self.points, self.frames


VERT = """
#version 330
uniform vec2 u_world;
in vec2 in_pos;
in float in_packed;
out vec3 v_color;
out float v_fluid;
void main() {
    vec2 ndc = (in_pos / u_world) * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    uint packed = floatBitsToUint(in_packed);
    float r = float(packed & 255u) / 255.0;
    float g = float((packed >> 8) & 255u) / 255.0;
    float b = float((packed >> 16) & 255u) / 255.0;
    uint flags = (packed >> 24) & 255u;
    v_fluid = ((flags & 128u) != 0u) ? 1.0 : 0.0;
    v_color = vec3(r, g, b);
    gl_PointSize = (v_fluid > 0.5) ? 3.0 : 5.0;
}
"""

FRAG = """
#version 330
in vec3 v_color;
in float v_fluid;
out vec4 f_color;
void main() {
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;
    float glow = exp(-r2 * 3.0);
    float gain = (v_fluid > 0.5) ? 0.35 : 1.0;
    f_color = vec4(v_color * glow * gain, 1.0);
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
in vec2 v_uv;
out vec4 f_color;
void main() {
    f_color = texture(u_tex, v_uv) * u_decay;
}
"""

DISPLAY_FRAG = """
#version 330
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec3 c = texture(u_tex, v_uv).rgb;
    c = c / (1.0 + c);            // Reinhard tone map
    c = pow(c, vec3(0.4545));     // gamma
    f_color = vec4(c, 1.0);
}
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="100.87.26.1")
    parser.add_argument("--port", type=int, default=12001)
    parser.add_argument("--world", default="5000x1500", help="world size WxH (matches the loaded scene)")
    parser.add_argument("--width", type=int, default=1500)
    parser.add_argument("--decay", type=float, default=0.94)
    parser.add_argument("--snapshot", default="", help="write a PPM of the screen to this path after --snapshot-after seconds")
    parser.add_argument("--snapshot-after", type=float, default=15.0)
    parser.add_argument("--exit-after", type=float, default=0.0, help="quit after N seconds (0 = run until closed)")
    args = parser.parse_args()
    world_w, world_h = (float(v) for v in args.world.split("x"))
    win_w = args.width
    win_h = max(200, int(win_w * world_h / world_w))

    receiver = GeomReceiver(args.server, args.port)

    if not glfw.init():
        raise SystemExit("glfw init failed")
    glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 3)
    glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 3)
    glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_CORE_PROFILE)
    glfw.window_hint(glfw.OPENGL_FORWARD_COMPAT, True)
    window = glfw.create_window(win_w, win_h, "ALIEN v3 - custom look", None, None)
    glfw.make_context_current(window)
    glfw.swap_interval(1)

    ctx = moderngl.create_context()
    ctx.enable(moderngl.PROGRAM_POINT_SIZE)

    fb_w, fb_h = glfw.get_framebuffer_size(window)

    points_prog = ctx.program(vertex_shader=VERT, fragment_shader=FRAG)
    decay_prog = ctx.program(vertex_shader=QUAD_VERT, fragment_shader=DECAY_FRAG)
    display_prog = ctx.program(vertex_shader=QUAD_VERT, fragment_shader=DISPLAY_FRAG)
    points_prog["u_world"].value = (world_w, world_h)

    quad = ctx.buffer(np.array([-1, -1, 3, -1, -1, 3], dtype=np.float32))
    decay_vao = ctx.vertex_array(decay_prog, [(quad, "2f", "in_pos")])
    display_vao = ctx.vertex_array(display_prog, [(quad, "2f", "in_pos")])

    point_buf = ctx.buffer(reserve=POINT_SIZE * 200000, dynamic=True)
    points_vao = ctx.vertex_array(points_prog, [(point_buf, "2f 1f", "in_pos", "in_packed")])

    accum = [ctx.texture((fb_w, fb_h), 4, dtype="f2") for _ in range(2)]
    fbos = [ctx.framebuffer(color_attachments=[t]) for t in accum]
    for f in fbos:
        f.use()
        ctx.clear(0, 0, 0, 1)

    cur = 0
    last_frames = -1
    n_points = 0
    fps_t0, fps_n = time.time(), 0
    start_t = time.time()
    snapshot_done = False

    while not glfw.window_should_close(window):
        glfw.poll_events()
        if glfw.get_key(window, glfw.KEY_ESCAPE) == glfw.PRESS:
            break

        pts, frames = receiver.latest()
        if frames != last_frames and len(pts):
            last_frames = frames
            data = pts.tobytes()
            if len(data) > point_buf.size:
                point_buf.orphan(len(data) * 2)
            point_buf.write(data)
            n_points = len(pts)

        nxt = 1 - cur
        # decay previous accumulation into next
        fbos[nxt].use()
        accum[cur].use(0)
        decay_prog["u_decay"].value = args.decay
        ctx.blend_func = moderngl.ONE, moderngl.ZERO
        ctx.enable(moderngl.BLEND)
        decay_vao.render(moderngl.TRIANGLES)
        # additive points on top
        if n_points:
            ctx.blend_func = moderngl.ONE, moderngl.ONE
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
            print(f"fps {fps_n / (time.time() - fps_t0):.1f}, points {n_points}, frames rx {frames}", flush=True)
            fps_t0, fps_n = time.time(), 0

        if args.exit_after > 0 and elapsed > args.exit_after:
            break

    glfw.terminate()


if __name__ == "__main__":
    main()

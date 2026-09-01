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
import subprocess
import threading
import time

import glfw
import moderngl
import numpy as np

MAGIC = 0x414C4E33
HEADER = struct.Struct("<IIHHHH")
TYPE_POINTS = 0
TYPE_LINES = 1
TYPE_INFO = 2
TYPE_STATIC = 3
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
        self.line_segs = np.zeros((0, 5), dtype=np.float32)  # x1, y1, x2, y2, packed
        self.world = None  # (w, h) announced by the server
        self.static_points = np.zeros((0, 3), dtype=np.float32)
        self.static_version = 0
        self.frames = 0
        self._chunks = {TYPE_POINTS: {}, TYPE_LINES: {}, TYPE_STATIC: {}}
        self._frame_ids = {TYPE_POINTS: -1, TYPE_LINES: -1, TYPE_STATIC: -1}
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
            if magic != MAGIC:
                continue
            if ptype == TYPE_INFO:
                if payload_len >= 8:
                    w, h = struct.unpack_from("<ff", data, HEADER.size)
                    with self.lock:
                        self.world = (w, h)
                continue
            if ptype not in self._chunks:
                continue
            chunks = self._chunks[ptype]
            if frame_id != self._frame_ids[ptype]:
                if chunks and ptype != TYPE_STATIC:
                    self._publish(ptype, b"".join(chunks.values()))
                self._frame_ids[ptype] = frame_id
                chunks.clear()
            chunks[chunk_idx] = data[HEADER.size : HEADER.size + payload_len]
            if len(chunks) == num_chunks:
                self._publish(ptype, b"".join(chunks.values()))
                chunks.clear()

    def _publish(self, ptype: int, blob: bytes):
        if ptype == TYPE_STATIC:
            count = len(blob) // POINT_SIZE
            raw = np.frombuffer(blob[: count * POINT_SIZE], dtype=np.uint8).reshape(count, POINT_SIZE)
            pts = np.empty((count, 3), dtype=np.float32)
            pts[:, 0:2] = raw[:, 0:8].copy().view(np.float32).reshape(count, 2)
            pts[:, 2] = raw[:, 8:12].copy().view(np.uint32).reshape(count).view(np.float32)
            with self.lock:
                self.static_points = pts
                self.static_version += 1
            return
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
            segs = np.empty((count, 5), dtype=np.float32)
            segs[:, 0:4] = raw[:, 0:16].copy().view(np.float32).reshape(count, 4)
            segs[:, 4] = raw[:, 16:20].copy().view(np.uint32).reshape(count).view(np.float32)
            with self.lock:
                self.line_segs = segs

    def latest(self):
        with self.lock:
            return self.points, self.line_segs, self.frames

    def announced_world(self):
        with self.lock:
            return self.world

    def latest_static(self):
        with self.lock:
            return self.static_points, self.static_version


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
    bool stat = (flags & 32u) != 0u;
    v_kind = stat ? 3.0 : (attack ? 2.0 : (fluid ? 1.0 : 0.0));
    v_color = vec3(r, g, b);
    float size = fluid ? 3.0 : (u_ink > 0.5 ? 4.0 : 5.0);
    if (stat) size = 2.6;
    if (attack) size = 14.0;
    gl_PointSize = size * u_point_scale;
}
"""

POINTS_FRAG = """
#version 330
uniform float u_ink;
uniform vec3 u_fluid_ink;
uniform float u_fluid_amount;
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
        if (v_kind > 2.5) {          // static structure: dry pale ink, always present
            inkColor = vec3(0.42, 0.42, 0.47);
            amount = 0.16;
        } else if (v_kind > 1.5) {          // attack: vermilion blot
            inkColor = vec3(0.78, 0.10, 0.04);
            amount = 0.45;
        } else if (v_kind > 0.5) {   // fluid: wash color set per look (grey ink / aizuri indigo)
            inkColor = u_fluid_ink;
            amount = u_fluid_amount;
        } else {                     // cell: sumi with a trace of lineage color
            inkColor = mix(vec3(0.09, 0.09, 0.12), v_color * 0.75, 0.55);
            amount = 0.40;
        }
        vec3 absorption = (vec3(1.0) - inkColor) * amount * fall;
        f_color = vec4(absorption, 1.0);
    } else {
        float gain = (v_kind > 2.5) ? 0.22 : ((v_kind > 1.5) ? 2.0 : ((v_kind > 0.5) ? 0.35 : 1.0));
        f_color = vec4(v_color * fall * gain, 1.0);
    }
}
"""

STROKE_VERT = """
#version 330
uniform vec2 u_world;
uniform vec2 u_viewport;
uniform float u_stroke_scale;
in vec2 in_corner;     // u in {0,1} along, v in {-1,1} across
in vec4 in_seg;        // x1,y1,x2,y2 in world units
in float in_packed;
out vec3 v_color;
out vec2 v_uv;         // pixel coords: x along (can exceed [0,len] in caps), y across
out float v_len;
out float v_halfw;
void main() {
    vec2 p1 = vec2(in_seg.x, in_seg.y) / u_world;
    vec2 p2 = vec2(in_seg.z, in_seg.w) / u_world;
    vec2 s1 = vec2(p1.x, 1.0 - p1.y) * u_viewport;
    vec2 s2 = vec2(p2.x, 1.0 - p2.y) * u_viewport;
    vec2 d = s2 - s1;
    float len = max(length(d), 1e-4);
    vec2 dir = d / len;
    vec2 nrm = vec2(-dir.y, dir.x);

    // brush width from world-space connection length: short bond = firm, thick stroke
    float lenWorld = distance(in_seg.xy, in_seg.zw);
    float halfw = clamp(4.5 - lenWorld * 1.4, 0.7, 4.0) * u_stroke_scale;

    float along = in_corner.x * (len + 2.0 * halfw) - halfw;   // extend for round caps
    vec2 sp = s1 + dir * along + nrm * (in_corner.y * halfw);
    vec2 ndc = sp / u_viewport * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);

    v_uv = vec2(along, in_corner.y * halfw);
    v_len = len;
    v_halfw = halfw;

    uint packed = floatBitsToUint(in_packed);
    v_color = vec3(float(packed & 255u), float((packed >> 8) & 255u), float((packed >> 16) & 255u)) / 255.0;
}
"""

STROKE_FRAG = """
#version 330
uniform float u_ink;
uniform float u_stroke_scale;
in vec3 v_color;
in vec2 v_uv;
in float v_len;
in float v_halfw;
out vec4 f_color;
void main() {
    // capsule distance: soft brush edge, round caps
    float along = clamp(v_uv.x, 0.0, v_len);
    float dist = length(vec2(v_uv.x - along, v_uv.y));
    float edge = 1.0 - smoothstep(v_halfw * 0.45, v_halfw, dist);
    if (edge <= 0.0) discard;

    float weight = v_halfw / (4.0 * u_stroke_scale);   // 0..1: thicker stroke = wetter ink
    if (u_ink > 0.5) {
        vec3 inkColor = mix(vec3(0.05, 0.05, 0.08), v_color * 0.5, 0.25);
        float amount = (0.10 + 0.55 * weight * weight) * edge;
        f_color = vec4((vec3(1.0) - inkColor) * amount, 1.0);
    } else {
        f_color = vec4(v_color * 0.6 * edge * (0.3 + 0.7 * weight), 1.0);
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
    parser.add_argument("--server", default="100.70.183.86")
    parser.add_argument("--port", type=int, default=12001)
    parser.add_argument("--world", default="5000x1500", help="world size WxH (matches the loaded scene)")
    parser.add_argument("--width", type=int, default=1500)
    parser.add_argument("--look", choices=["ink", "aizuri", "glow"], default="ink")
    parser.add_argument("--decay", type=float, default=0.0, help="trail decay per frame (0 = per-look default)")
    parser.add_argument("--snapshot", default="", help="write a PPM of the screen to this path after --snapshot-after seconds")
    parser.add_argument("--snapshot-after", type=float, default=15.0)
    parser.add_argument("--exit-after", type=float, default=0.0, help="quit after N seconds (0 = run until closed)")
    parser.add_argument("--record", default="", help="record the screen to this .mp4 (via ffmpeg)")
    parser.add_argument("--record-fps", type=int, default=30)
    parser.add_argument("--offscreen", action="store_true", help="render into an offscreen buffer: no window, immune to occlusion throttling, works with the display asleep")
    args = parser.parse_args()
    world_w, world_h = (float(v) for v in args.world.split("x"))
    win_w = args.width
    win_h = max(200, int(win_w * world_h / world_w))
    ink = args.look in ("ink", "aizuri")
    aizuri = args.look == "aizuri"
    decay = args.decay if args.decay > 0 else (0.90 if ink else 0.94)
    bleed = 0.35 if ink else 0.0
    ink_strength = 1.6
    fluid_ink = (0.10, 0.20, 0.52) if aizuri else (0.38, 0.40, 0.46)
    fluid_amount = 0.12 if aizuri else 0.035

    receiver = GeomReceiver(args.server, args.port)

    # the server announces its world size every frame; wait briefly so the
    # window is created with the right aspect from the start
    deadline = time.time() + 3.0
    while receiver.announced_world() is None and time.time() < deadline:
        time.sleep(0.05)
    announced = receiver.announced_world()
    if announced is not None:
        world_w, world_h = announced
        win_h = max(200, int(win_w * world_h / world_w))
        print(f"world from server: {world_w:.0f}x{world_h:.0f}", flush=True)

    if not glfw.init():
        raise SystemExit("glfw init failed")
    if args.offscreen:
        glfw.window_hint(glfw.VISIBLE, glfw.FALSE)
    # fit the window into the monitor workarea, preserving world aspect
    try:
        _, _, wa_w, wa_h = glfw.get_monitor_workarea(glfw.get_primary_monitor())
    except Exception:
        wa_w, wa_h = 1512, 950
    if win_h > wa_h - 80:
        win_h = wa_h - 80
        win_w = max(300, int(win_h * world_w / world_h))
    if win_w > wa_w - 40:
        win_w = wa_w - 40
        win_h = max(200, int(win_w * world_h / world_w))
    glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 3)
    glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 3)
    glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_CORE_PROFILE)
    glfw.window_hint(glfw.OPENGL_FORWARD_COMPAT, True)
    window = glfw.create_window(win_w, win_h, f"ALIEN v3 — {args.look}", None, None)
    glfw.make_context_current(window)
    glfw.swap_interval(1)

    ctx = moderngl.create_context()
    ctx.enable(moderngl.PROGRAM_POINT_SIZE)

    if args.offscreen:
        # fixed 2x offscreen target; immune to window/framebuffer quirks
        fb_w, fb_h = win_w * 2, win_h * 2
        point_scale = 2.0
    else:
        # use the GL-reported size: glfw's framebuffer size can disagree by a few px
        fb_w, fb_h = ctx.screen.width, ctx.screen.height
        point_scale = fb_w / glfw.get_window_size(window)[0]  # retina

    points_prog = ctx.program(vertex_shader=GEOM_VERT, fragment_shader=POINTS_FRAG)
    stroke_prog = ctx.program(vertex_shader=STROKE_VERT, fragment_shader=STROKE_FRAG)
    decay_prog = ctx.program(vertex_shader=QUAD_VERT, fragment_shader=DECAY_FRAG)
    display_prog = ctx.program(vertex_shader=QUAD_VERT, fragment_shader=DISPLAY_FRAG)
    points_prog["u_world"].value = (world_w, world_h)
    points_prog["u_ink"].value = 1.0 if ink else 0.0
    points_prog["u_point_scale"].value = point_scale
    points_prog["u_fluid_ink"].value = fluid_ink
    points_prog["u_fluid_amount"].value = fluid_amount
    stroke_prog["u_world"].value = (world_w, world_h)
    stroke_prog["u_viewport"].value = (fb_w, fb_h)
    stroke_prog["u_ink"].value = 1.0 if ink else 0.0
    stroke_prog["u_stroke_scale"].value = point_scale
    display_prog["u_ink"].value = 1.0 if ink else 0.0
    display_prog["u_strength"].value = ink_strength
    decay_prog["u_decay"].value = decay
    decay_prog["u_bleed"].value = bleed

    quad = ctx.buffer(np.array([-1, -1, 3, -1, -1, 3], dtype=np.float32))
    decay_vao = ctx.vertex_array(decay_prog, [(quad, "2f", "in_pos")])
    display_vao = ctx.vertex_array(display_prog, [(quad, "2f", "in_pos")])

    point_buf = ctx.buffer(reserve=POINT_SIZE * 100000, dynamic=True)
    points_vao = ctx.vertex_array(points_prog, [(point_buf, "2f 1f", "in_pos", "in_packed")])
    static_buf = ctx.buffer(reserve=POINT_SIZE * 200000, dynamic=True)
    static_vao = ctx.vertex_array(points_prog, [(static_buf, "2f 1f", "in_pos", "in_packed")])
    corner_buf = ctx.buffer(np.array([0, -1, 0, 1, 1, -1, 1, 1], dtype=np.float32))
    line_inst_buf = ctx.buffer(reserve=20 * 20000, dynamic=True)
    strokes_vao = ctx.vertex_array(
        stroke_prog,
        [(corner_buf, "2f", "in_corner"), (line_inst_buf, "4f 1f /i", "in_seg", "in_packed")],
    )

    accum = [ctx.texture((fb_w, fb_h), 4, dtype="f2") for _ in range(2)]
    fbos = [ctx.framebuffer(color_attachments=[t]) for t in accum]
    for f in fbos:
        f.use()
        ctx.clear(0, 0, 0, 1)

    recorder = None
    rec_next_t = 0.0

    def start_recorder():
        proc = subprocess.Popen(
            [
                "ffmpeg", "-y", "-loglevel", "error",
                "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{fb_w}x{fb_h}", "-r", str(args.record_fps), "-i", "-",
                "-vf", "vflip", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "18", "-pix_fmt", "yuv420p",
                args.record,
            ],
            stdin=subprocess.PIPE,
        )
        print(f"recording to {args.record} at {args.record_fps} fps ({fb_w}x{fb_h})", flush=True)
        return proc

    cur = 0
    last_frames = -1
    last_static_version = -1
    n_points = 0
    n_static = 0
    n_lines = 0
    fps_t0, fps_n = time.time(), 0
    start_t = time.time()
    snapshot_done = False

    if args.offscreen:
        offscreen_tex = ctx.texture((fb_w, fb_h), 4)
        screen = ctx.framebuffer(color_attachments=[offscreen_tex])
    else:
        screen = ctx.screen

    while not glfw.window_should_close(window):
        glfw.poll_events()
        if glfw.get_key(window, glfw.KEY_ESCAPE) == glfw.PRESS:
            break


        static_pts, static_version = receiver.latest_static()
        if static_version != last_static_version and len(static_pts):
            last_static_version = static_version
            data = static_pts.tobytes()
            if len(data) > static_buf.size:
                static_buf.orphan(len(data) * 2)
            static_buf.write(data)
            n_static = len(static_pts)
            print(f"static structure: {n_static} points", flush=True)

        pts, line_segs, frames = receiver.latest()
        if frames != last_frames:
            last_frames = frames
            if len(pts):
                data = pts.tobytes()
                if len(data) > point_buf.size:
                    point_buf.orphan(len(data) * 2)
                point_buf.write(data)
                n_points = len(pts)
            if len(line_segs):
                data = line_segs.tobytes()
                if len(data) > line_inst_buf.size:
                    line_inst_buf.orphan(len(data) * 2)
                line_inst_buf.write(data)
                n_lines = len(line_segs)

        nxt = 1 - cur
        fbos[nxt].use()
        accum[cur].use(0)
        ctx.blend_func = moderngl.ONE, moderngl.ZERO
        ctx.enable(moderngl.BLEND)
        decay_vao.render(moderngl.TRIANGLES)
        # additive accumulation (light in glow mode, absorption in ink mode)
        ctx.blend_func = moderngl.ONE, moderngl.ONE
        if n_static:
            static_vao.render(moderngl.POINTS, vertices=n_static)
        if n_lines:
            strokes_vao.render(moderngl.TRIANGLE_STRIP, vertices=4, instances=n_lines)
        if n_points:
            points_vao.render(moderngl.POINTS, vertices=n_points)
        # display
        screen.use()
        accum[nxt].use(0)
        ctx.blend_func = moderngl.ONE, moderngl.ZERO
        display_vao.render(moderngl.TRIANGLES)

        elapsed = time.time() - start_t
        if args.snapshot and not snapshot_done and elapsed > args.snapshot_after:
            snapshot_done = True
            sw, sh = screen.width, screen.height
            raw = screen.read(components=3)
            with open(args.snapshot, "wb") as f:
                f.write(f"P6\n{sw} {sh}\n255\n".encode())
                arr = np.frombuffer(raw, dtype=np.uint8).reshape(sh, sw, 3)[::-1]
                f.write(arr.tobytes())
            print(f"snapshot written: {args.snapshot} ({sw}x{sh})", flush=True)

        if args.record and recorder is None:
            recorder = start_recorder()
        if recorder is not None and elapsed >= rec_next_t:
            # wall-clock frame pacing: when window throttling slows drawing,
            # duplicate the frame so the video keeps real-time length
            raw = screen.read(components=3)
            try:
                while elapsed >= rec_next_t:
                    rec_next_t += 1.0 / args.record_fps
                    recorder.stdin.write(raw)
            except BrokenPipeError:
                recorder = None

        if args.offscreen:
            time.sleep(0.005)
        else:
            glfw.swap_buffers(window)
        cur = nxt

        fps_n += 1
        if time.time() - fps_t0 > 5:
            print(f"fps {fps_n / (time.time() - fps_t0):.1f}, points {n_points}, lines {n_lines}, frames rx {frames}", flush=True)
            fps_t0, fps_n = time.time(), 0

        if args.exit_after > 0 and elapsed > args.exit_after:
            break

    if recorder is not None:
        recorder.stdin.close()
        recorder.wait()
        print("recording finished", flush=True)
    glfw.terminate()


if __name__ == "__main__":
    main()

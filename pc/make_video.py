#!/usr/bin/env python3
"""
make_video.py - turn any video into video.bin for the Pico's SD card.

Three formats:

  --mode bw       (default) black & white, compressed. Made for Bad Apple!!
                  Each frame only stores which pixels changed since the last
                  one, so most frames are a few hundred bytes. Plays at 15 fps
                  by default (--fps 30 for full speed), shown full screen.

  --mode color16  16 colours, compressed the same way. Every keyframe brings
                  its own palette of the 16 colours that suit that scene best.
                  Plays at 10 fps by default, shown full screen.

  --mode color    the original raw 128x128 colour frames (32 KB each,
                  about 1 frame per second).

Needs ffmpeg on your PATH and numpy (pip install numpy).

Examples:
    python make_video.py badapple.mp4                    -> video.bin, 15 fps
    python make_video.py badapple.mp4 -o E:\\video.bin    -> straight to SD card
    python make_video.py badapple.mp4 --size 192x144     -> sharper, bigger frames
    python make_video.py myvideo.mp4 --mode color16
    python make_video.py myvideo.mp4 --mode color16 --preview --saturation 1.4
    python make_video.py myvideo.mp4 --mode color --duration 30

------------------------------------------------------------------------------
video.bin = repeated records  [u16 length, little endian][payload]

bw payload:
    byte 0   'K' keyframe (start from an all-black picture)
             'D' delta    (start from the previous picture)
    byte 1   frames per second the video should play at
    byte 2   width  in pixels (even, 2..256; 256 is stored as 0)
    byte 3   height in pixels (2..240)
    byte 4.. run lengths over the pixels in reading order (left to right,
             top to bottom), alternating "keep" / "flip", starting with
             "keep". Each length is a varint: 7 bits per byte, low bits
             first, top bit set = another byte follows. A trailing "keep"
             run is left out.

color16 payload:
    byte 0   'P' keyframe (start from all pixels = colour 0, new palette)
             'Q' delta    (start from the previous picture, same palette)
    byte 1-3 fps, width (256 stored as 0), height - same as bw
    'P' only: byte 4 = number of colours n (1..16), then n x u16 PS1 colours
    then tokens in reading order:
      0nnnnnnn          skip n+1 pixels (keep them)
      10nnnnnn data     n+1 literal pixels, 2 per byte, first in low nibble
      11nnnnnn c        n+1 pixels of colour c (low nibble of next byte)

colour payload: exactly 32768 bytes, 128x128 PS1 15-bit pixels (no header).
------------------------------------------------------------------------------
"""

import argparse
import subprocess
import sys

LINK_BYTES_PER_SEC = 24000   # rough controller-port speed at the stock 250 kHz


# ---------------------------------------------------------------------------
# ffmpeg
# ---------------------------------------------------------------------------

def detect_hdr(path):
    """Returns the transfer curve name if the video is HDR, else None."""
    try:
        r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                            "-show_entries", "stream=color_transfer",
                            "-of", "default=nw=1:nk=1", path],
                           capture_output=True, text=True)
    except FileNotFoundError:
        return None
    trc = r.stdout.strip()
    return trc if trc in ("smpte2084", "arib-std-b67") else None


def has_filters(*names):
    try:
        out = subprocess.run(["ffmpeg", "-hide_banner", "-filters"],
                             capture_output=True, text=True).stdout
    except FileNotFoundError:
        return False
    return all(f" {n} " in out for n in names)


def pre_filters(args):
    """Filters applied before scaling: HDR -> normal video, colour tweaks."""
    f = []
    if args.input and not args.test:
        trc = detect_hdr(args.input)
        if trc:
            if has_filters("zscale", "tonemap"):
                print(f"HDR video ({trc}) detected - converting to normal colours")
                f.append("zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,"
                         "tonemap=tonemap=hable:desat=0,"
                         "zscale=t=bt709:m=bt709:r=tv,format=yuv420p")
            else:
                print(f"WARNING: HDR video ({trc}) - colours will look washed out and grey.\n"
                      "         Your ffmpeg lacks the 'zscale' filter needed to fix this;\n"
                      "         turn HDR off on the phone, or use a full ffmpeg build.")
    if args.saturation != 1.0 or args.contrast != 1.0 or args.brightness != 0.0:
        f.append(f"eq=saturation={args.saturation}:contrast={args.contrast}"
                 f":brightness={args.brightness}")
    return f


def ffmpeg_frames(args, w, h, pix_fmt, bpp, vf_extra):
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error"]
    if args.start:
        cmd += ["-ss", args.start]
    if args.test:
        cmd += ["-f", "lavfi", "-i", args.test]
        if not args.duration:
            args.duration = "20"
    else:
        cmd += ["-i", args.input]
    if args.duration:
        cmd += ["-t", str(args.duration)]
    vf = ",".join([f"fps={args.fps}"] + pre_filters(args) + [vf_extra])
    cmd += ["-vf", vf,
            "-an", "-f", "rawvideo", "-pix_fmt", pix_fmt, "-"]
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    except FileNotFoundError:
        sys.exit("ffmpeg not found - install it and make sure it is on your PATH")

    size = w * h * bpp
    while True:
        buf = proc.stdout.read(size)
        if len(buf) < size:
            break
        yield buf
    if proc.wait() != 0:
        sys.exit("ffmpeg reported an error (see above)")


class Preview:
    """Collects a few decoded frames and saves them as one PNG, showing what
    the PS1 will display (no extra libraries needed)."""

    def __init__(self, path, every):
        self.path, self.every, self.frames, self.n = path, max(1, int(every)), [], 0

    def add(self, make_rgb):
        if self.path and self.n % self.every == 0 and len(self.frames) < 12:
            self.frames.append(make_rgb())
        self.n += 1

    def save(self):
        if not self.path or not self.frames:
            return
        import numpy as np
        import struct
        import zlib
        h, w, _ = self.frames[0].shape
        cols = 4
        rows = (len(self.frames) + cols - 1) // cols
        sheet = np.full((rows * (h * 2 + 4), cols * (w * 2 + 4), 3), 40, np.uint8)
        for i, f in enumerate(self.frames):
            y, x = (i // cols) * (h * 2 + 4), (i % cols) * (w * 2 + 4)
            sheet[y:y + h * 2, x:x + w * 2] = f.repeat(2, 0).repeat(2, 1)
        H, W, _ = sheet.shape
        raw = b"".join(b"\x00" + sheet[r].tobytes() for r in range(H))
        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
        with open(self.path, "wb") as fh:
            fh.write(png)
        print(f"preview of what the PS1 will show: {self.path}")


def ps1_to_rgb(ps1):
    """PS1 15-bit colours -> 8-bit RGB as the console shows them."""
    import numpy as np
    ps1 = np.asarray(ps1)
    return (np.stack([ps1 & 31, (ps1 >> 5) & 31, (ps1 >> 10) & 31], -1) * 255 // 31).astype(np.uint8)


def fit_filter(w, h):
    """Scale to fit w x h keeping the aspect ratio, black bars around."""
    return (f"scale={w}:{h}:force_original_aspect_ratio=decrease:flags=area,"
            f"pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:black")


# ---------------------------------------------------------------------------
# black & white, delta + run-length
# ---------------------------------------------------------------------------

def varint(n, out):
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return


def encode_runs(flip):
    """flip: flat bool array, True = pixel changes. Returns run-length bytes."""
    import numpy as np
    out = bytearray()
    idx = np.flatnonzero(np.diff(np.concatenate(([0], flip.view(np.int8), [0]))))
    # idx holds alternating start/end positions of the "flip" runs
    pos = 0
    for i in range(0, len(idx), 2):
        start, end = int(idx[i]), int(idx[i + 1])
        varint(start - pos, out)    # keep run
        varint(end - start, out)    # flip run
        pos = end
    return out


def make_bw(args, out):
    import numpy as np

    w, h = args.size
    prev = np.zeros(w * h, dtype=bool)
    frames = 0
    sizes = []
    keys = 0
    since_key = 0

    preview = Preview(args.preview, args.fps * 2)
    for gray in ffmpeg_frames(args, w, h, "gray", 1, fit_filter(w, h)):
        cur = np.frombuffer(gray, dtype=np.uint8) >= args.threshold
        preview.add(lambda: np.repeat((cur.reshape(h, w) * 255).astype(np.uint8)[..., None], 3, 2))

        key_runs = encode_runs(cur)
        use_key = frames == 0 or since_key >= args.keyint
        if not use_key:
            delta_runs = encode_runs(cur ^ prev)
            if len(delta_runs) >= len(key_runs):   # scene cut: key is smaller
                use_key = True

        runs = key_runs if use_key else delta_runs
        payload = bytes([ord("K") if use_key else ord("D"),
                         int(round(args.fps)), w & 0xFF, h]) + runs   # 256 -> 0
        if len(payload) > 65535:
            sys.exit("a frame is too big (>64 KB) - use a smaller --size")

        out.write(len(payload).to_bytes(2, "little"))
        out.write(payload)

        prev = cur
        frames += 1
        sizes.append(len(payload) + 2)
        if use_key:
            keys += 1
            since_key = 0
        else:
            since_key += 1
        if frames % 300 == 0:
            print(f"  {frames} frames...", flush=True)

    if not frames:
        sys.exit("no frames were produced")
    print_stats(args, sizes, keys)
    preview.save()


# ---------------------------------------------------------------------------
# 16 colours: palette per group of frames, delta + skip/literal/fill tokens
# ---------------------------------------------------------------------------

def print_stats(args, sizes, keys):
    frames = len(sizes)
    total = sum(sizes)
    secs = frames / args.fps
    need = total / secs
    print(f"wrote {args.output}: {frames} frames ({secs:.0f} s at {args.fps:g} fps), "
          f"{keys} keyframes, {total / 1024:.0f} KB")
    print(f"  frame size: average {total / frames:.0f} bytes, largest {max(sizes)} bytes")
    print(f"  needs ~{need / 1024:.1f} KB/s; the link gives ~{LINK_BYTES_PER_SEC / 1024:.0f} KB/s "
          f"at the stock clock")
    if need <= LINK_BYTES_PER_SEC:
        slow = sum(1 for s in sizes if s > LINK_BYTES_PER_SEC / args.fps)
        print(f"  -> should play at full speed "
              f"({slow} busy frames may briefly slow down)")
    else:
        print(f"  -> will play at roughly {args.fps * LINK_BYTES_PER_SEC / need:.0f} fps; "
              f"use a smaller --size, a lower --fps or a faster bus clock")


# Colours are matched in a brightness + colour-difference space where colour
# differences count extra. Plain RGB lets big grey areas take all 16 palette
# slots; this way small colourful things (a red shirt, a blue sign) get some.
CHROMA_WEIGHT = 1.6


def to_match_space(rgb):
    import numpy as np
    y = rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    return np.stack([y, (rgb[:, 2] - y) * CHROMA_WEIGHT * 0.564,
                     (rgb[:, 0] - y) * CHROMA_WEIGHT * 0.713], -1).astype(np.float32)


def from_match_space(m):
    import numpy as np
    y = m[:, 0]
    b = m[:, 1] / (CHROMA_WEIGHT * 0.564) + y
    r = m[:, 2] / (CHROMA_WEIGHT * 0.713) + y
    g = (y - 0.299 * r - 0.114 * b) / 0.587
    return np.stack([r, g, b], -1)


def kmeans_palette(samples, k, rng, iters=12):
    import numpy as np
    lum = samples[:, 0]
    order = np.argsort(lum)
    centers = samples[order[np.linspace(0, len(samples) - 1, k).astype(int)]].copy()
    for _ in range(iters):
        d = ((samples[:, None, :] - centers[None]) ** 2).sum(2)
        lab = d.argmin(1)
        for j in range(k):
            m = lab == j
            centers[j] = samples[m].mean(0) if m.any() else samples[rng.integers(len(samples))]
    return centers


def quantize(rgb, centers, prev, stability):
    """Nearest palette colour per pixel. Keeps the previous colour number when
    it is nearly as good, so still areas don't flicker and need no update."""
    import numpy as np
    d = ((rgb[:, None, :] - centers[None]) ** 2).sum(2)
    best = d.argmin(1)
    if prev is not None:
        ar = np.arange(len(best))
        keep = d[ar, prev] <= d[ar, best] + stability
        best = np.where(keep, prev, best)
    return best.astype(np.uint8)


def _runs(mask):
    """(starts, ends) of the True runs in a bool array."""
    import numpy as np
    idx = np.flatnonzero(np.diff(np.concatenate(([0], mask.view(np.int8), [0]))))
    return idx[0::2], idx[1::2]


def _emit_literals(seg, out):
    import numpy as np
    for i in range(0, len(seg), 64):
        part = seg[i:i + 64]
        out.append(0x80 | (len(part) - 1))
        if len(part) & 1:
            part = np.append(part, 0)
        out += (part[0::2] | (part[1::2] << 4)).astype(np.uint8).tobytes()


def _emit_changed(seg, out):
    """Encode a stretch of pixels that all need writing."""
    import numpy as np
    b = np.flatnonzero(np.diff(seg)) + 1
    starts = np.concatenate(([0], b))
    ends = np.concatenate((b, [len(seg)]))
    long_ = np.flatnonzero(ends - starts >= 4)      # worth a fill token
    pos = 0
    for r in long_:
        s, e = int(starts[r]), int(ends[r])
        if s > pos:
            _emit_literals(seg[pos:s], out)
        col = int(seg[s])
        n = e - s
        while n > 0:
            m = min(n, 64)
            out.append(0xC0 | (m - 1))
            out.append(col)
            n -= m
        pos = e
    if pos < len(seg):
        _emit_literals(seg[pos:], out)


def encode_tokens(cur, prev):
    import numpy as np
    changed = cur != prev
    starts, ends = _runs(changed)
    out = bytearray()
    pos = 0
    i = 0
    while i < len(starts):
        s, e = int(starts[i]), int(ends[i])
        # join changed stretches separated by tiny gaps: cheaper than a skip
        while i + 1 < len(starts) and starts[i + 1] - e <= 2:
            i += 1
            e = int(ends[i])
        gap = s - pos
        while gap > 0:
            n = min(gap, 128)
            out.append(n - 1)
            gap -= n
        _emit_changed(cur[s:e], out)
        pos = e
        i += 1
    return out


def make_color16(args, out):
    import numpy as np

    w, h = args.size
    npix = w * h
    rng = np.random.default_rng(1)
    sizes = []
    keys = 0
    preview = Preview(args.preview, args.fps * 2)

    def flush_group(group):
        nonlocal keys
        if not group:
            return
        # palette from a sample of all pixels in this group of frames
        allpix = np.concatenate(group)
        take = rng.choice(len(allpix), size=min(8000, len(allpix)), replace=False)
        centers = kmeans_palette(allpix[take], args.colors, rng)
        pal = np.clip(np.rint(from_match_space(centers)), 0, 255).astype(np.uint16)
        ps1 = (pal[:, 0] >> 3) | ((pal[:, 1] >> 3) << 5) | ((pal[:, 2] >> 3) << 10) | 0x8000
        prev = None
        for n, rgb in enumerate(group):
            cur = quantize(rgb, centers, prev, args.stability)
            if n == 0:
                payload = bytes([ord("P"), int(round(args.fps)), w & 0xFF, h, len(pal)])
                payload += ps1.astype("<u2").tobytes()
                payload += encode_tokens(cur, np.zeros(npix, np.uint8))
                keys += 1
            else:
                payload = bytes([ord("Q"), int(round(args.fps)), w & 0xFF, h])
                payload += encode_tokens(cur, prev)
            if len(payload) > 65535:
                sys.exit("a frame is too big (>64 KB) - use a smaller --size")
            out.write(len(payload).to_bytes(2, "little"))
            out.write(payload)
            sizes.append(len(payload) + 2)
            preview.add(lambda: ps1_to_rgb(ps1[cur]).reshape(h, w, 3))
            prev = cur
        if len(sizes) % 300 < len(group):
            print(f"  {len(sizes)} frames...", flush=True)

    group = []
    last = None
    for buf in ffmpeg_frames(args, w, h, "rgb24", 3, fit_filter(w, h)):
        rgb = to_match_space(np.frombuffer(buf, dtype=np.uint8).reshape(-1, 3).astype(np.float32))
        # start a new group (with a fresh palette) every --keyint frames or
        # at a scene cut
        cut = last is not None and np.abs(rgb - last).mean() > 40
        if len(group) >= args.keyint or cut:
            flush_group(group)
            group = []
        group.append(rgb)
        last = rgb
    flush_group(group)

    if not sizes:
        sys.exit("no frames were produced")
    print_stats(args, sizes, keys)
    preview.save()


# ---------------------------------------------------------------------------
# colour, raw 128x128 (the original format)
# ---------------------------------------------------------------------------

def rgb24_to_ps1(rgb: bytes) -> bytes:
    """RGB888 -> PS1 15-bit (R bits 0-4, G 5-9, B 10-14), little endian."""
    try:
        import numpy as np
        a = np.frombuffer(rgb, dtype=np.uint8).reshape(-1, 3).astype(np.uint16)
        px = (a[:, 0] >> 3) | ((a[:, 1] >> 3) << 5) | ((a[:, 2] >> 3) << 10)
        return px.astype("<u2").tobytes()
    except ImportError:
        out = bytearray(len(rgb) // 3 * 2)
        for i in range(len(rgb) // 3):
            r, g, b = rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]
            v = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
            out[2 * i] = v & 0xFF
            out[2 * i + 1] = v >> 8
        return bytes(out)


def make_color(args, out):
    frames = 0
    for rgb in ffmpeg_frames(args, 128, 128, "rgb24", 3, fit_filter(128, 128)):
        data = rgb24_to_ps1(rgb)
        out.write(len(data).to_bytes(2, "little"))
        out.write(data)
        frames += 1
    if not frames:
        sys.exit("no frames were produced")
    print(f"wrote {args.output}: {frames} colour frames, "
          f"{frames * 32770 / 1024:.0f} KB (about 1 frame per second on the PS1)")


# ---------------------------------------------------------------------------

def parse_size(s):
    try:
        w, h = (int(v) for v in s.lower().split("x"))
    except ValueError:
        raise argparse.ArgumentTypeError("use WIDTHxHEIGHT, e.g. 160x120")
    if not (2 <= w <= 256 and 2 <= h <= 240) or w % 2 or w * h > 256 * 240:
        raise argparse.ArgumentTypeError("width must be even and 2-256, height 2-240")
    return w, h


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("---")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input", nargs="?", help="any video file ffmpeg can read")
    p.add_argument("-o", "--output", default="video.bin")
    p.add_argument("--mode", choices=["bw", "color16", "color"], default="bw")
    p.add_argument("--size", type=parse_size, default=None,
                   help="picture size, default 160x120 for bw, 128x96 for color16 "
                        "(both shown enlarged 2x)")
    p.add_argument("--fps", type=float, default=None,
                   help="frames per second (default: 15 bw, 10 color16, 1 colour)")
    p.add_argument("--colors", type=int, default=16, choices=range(2, 17), metavar="2-16",
                   help="color16: colours per palette (default 16)")
    p.add_argument("--stability", type=float, default=500,
                   help="color16: higher = fewer pixel updates (smaller, steadier, "
                        "less accurate). Default 500")
    p.add_argument("--threshold", type=int, default=128,
                   help="bw: brightness 0-255 above which a pixel is white")
    p.add_argument("--keyint", type=int, default=None,
                   help="a full keyframe at least every N frames (default 30 bw, 20 color16)")
    p.add_argument("--saturation", type=float, default=1.0,
                   help="colour strength before converting, e.g. 1.5 = stronger (default 1)")
    p.add_argument("--contrast", type=float, default=1.0,
                   help="contrast before converting, e.g. 0.9 = softer (default 1)")
    p.add_argument("--brightness", type=float, default=0.0,
                   help="brightness before converting, -1..1 (default 0)")
    p.add_argument("--preview", nargs="?", const="AUTO", default=None,
                   help="also save a PNG of frames as the PS1 will show them "
                        "(default name: <output>_preview.png)")
    p.add_argument("--start", default=None, help="start time, e.g. 0:30")
    p.add_argument("--duration", default=None, help="length to convert, e.g. 60")
    p.add_argument("--test", nargs="?", const="auto", default=None,
                   help="use an ffmpeg test pattern instead of a file")
    args = p.parse_args()

    if not args.input and not args.test:
        p.error("give an input video or --test")
    if args.fps is None:
        args.fps = {"bw": 15.0, "color16": 10.0, "color": 1.0}[args.mode]
    if args.size is None:
        args.size = (128, 96) if args.mode == "color16" else (160, 120)
    if args.keyint is None:
        args.keyint = 20 if args.mode == "color16" else 30
    if args.test == "auto":
        args.test = ("mandelbrot=size=320x240:rate=30" if args.mode == "bw"
                     else "testsrc2=size=320x240:rate=30")
    if args.preview == "AUTO":
        args.preview = args.output.rsplit(".", 1)[0] + "_preview.png"
    if args.mode == "color" and args.preview:
        print("note: --preview is only available for bw and color16")
        args.preview = None
    if args.mode == "color16" and args.colors > 16:
        p.error("at most 16 colours")

    with open(args.output, "wb") as out:
        if args.mode == "bw":
            make_bw(args, out)
        elif args.mode == "color16":
            make_color16(args, out)
        else:
            make_color(args, out)
    print("copy it to the root of a FAT32/exFAT SD card as video.bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#! /usr/bin/env python3

"""Renders video overlay assets from a frame table.

Reads the CSV/NDJSON that `apps/export_overlay.cpp` writes -- one row per video
frame -- and draws the pieces an onboard edit needs, each as its own asset so
it can be placed, restyled or dropped in DaVinci Resolve without a re-render:

    track.png    the circuit outline, static, transparent
    map.mov      the trail and the driver marker, on the same canvas as
                 track.png, so the two stack with no alignment to do
    timing.mov   the lap clock, the delta meter and the last/best tiles

Both movies are RGBA QuickTime with *straight* (non-premultiplied) alpha --
set Alpha Mode to Straight in Resolve's Clip Attributes. The default codec is
Animation (qtrle), which is lossless and compresses the large transparent
areas of an overlay far better than ProRes 4444's fixed bitrate; pass
`--codec prores` if decode performance matters more than file size.

The frame rate must match the footage exactly. It is read from the exporter's
manifest, which carries it as the rational the picture actually uses
(60000/1001, not 59.94). To read a source clip's rate and start timecode:

    ffprobe -v0 -select_streams v:0 -show_entries stream=r_frame_rate \\
        -show_entries format_tags=timecode -of default=nw=1 GX010123.MP4

and pass the timecode through with `--timecode`, so Resolve can confirm the
overlay and the onboard line up.

Use `--preview <frame>` to write a single PNG of each asset and look at it
before committing to a render.
"""

import contextlib
import json
import math
import multiprocessing as mp
import subprocess

from dataclasses import dataclass
from pathlib import Path

import click
import numpy as np
import pandas as pd

from PIL import Image, ImageDraw, ImageFont


# The overlay sits over footage nobody has graded yet, so every element needs
# its own contrast: a scrim behind the text, a dark ring around the marker,
# and nothing that relies on the picture underneath being dark.
SCRIM = (0, 0, 0, 158)
INK = (255, 255, 255, 255)
INK_DIM = (195, 194, 183, 255)

# The delta is a polarity reading, so it gets a diverging pair. Teal rather
# than a pure green: against this red it clears the colour-blind separation
# target (deutan dE 9.4, vs 6.9 for #1baf7a), while still reading as the
# "faster" colour the sport has trained everyone to expect. Colour is never
# the only cue anyway -- the sign and the bar's direction both carry it.
AHEAD = (0, 168, 140, 255)
BEHIND = (227, 73, 72, 255)
BEST = (144, 133, 233, 255)

TRACK_EDGE = (255, 255, 255, 118)
START_LINE = (255, 255, 255, 235)
TRAIL = (255, 255, 255, 245)

# The map has no scrim behind it -- a panel there would box in a shape whose
# outline is the whole point. Instead every stroke is laid over a wider dark
# one, so it reads against a white pit wall as well as a shaded hairpin.
HALO = (0, 0, 0, 165)

# The map is curves and hairlines, so it wants the full supersample. The
# timing block is text plus two rounded rectangles -- FreeType antialiases the
# text natively and at 3x it is only paying for the corners, so it is capped.
TIMING_SS = 2

# How the supersampled canvas is reduced to delivery size. This *is* the
# antialiasing -- Pillow's shape drawing has none of its own -- so the filter
# is a visual decision, not a detail. BOX is a plain average of each source
# block, which for an exact integer reduction is precisely what supersampling
# asks for; LANCZOS is a windowed sinc whose negative lobes also sharpen,
# which is right for photographs and can ring beside a hard graphic edge.
RESAMPLE = {
    "box": Image.Resampling.BOX,
    "lanczos": Image.Resampling.LANCZOS,
    "bicubic": Image.Resampling.BICUBIC,
}

FONT_CANDIDATES = [
    # DIN Condensed is the face motorsport timing has used for decades, and
    # it is one sans for the whole overlay -- labels and figures alike.
    "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf",
    "/System/Library/Fonts/HelveticaNeue.ttc",
    "/System/Library/Fonts/Menlo.ttc",
]


def pick_font() -> str:
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            return path
    raise click.ClickException("no usable font found; pass --font")


@dataclass
class Layer:
    """A transparent canvas drawn supersampled and resolved down.

    Pillow's shape drawing has no antialiasing of its own, so the shapes are
    drawn at `ss` times the delivery size and resampled once at the end. Text
    goes through FreeType and is already antialiased, but drawing it at the
    same scale keeps one coordinate system for the whole layer.
    """

    width: int
    height: int
    ss: int = 3
    resample: int = Image.Resampling.LANCZOS

    def __post_init__(self) -> None:
        self.img = Image.new(
            "RGBA", (self.width * self.ss, self.height * self.ss), (0, 0, 0, 0)
        )
        # "RGBA" mode makes the draw context blend against what is already
        # there instead of overwriting the alpha, which is what layering a
        # fading trail under a marker needs.
        self.draw = ImageDraw.Draw(self.img, "RGBA")

    def s(self, value: float) -> float:
        return value * self.ss

    @contextlib.contextmanager
    def paint(self):
        """Draws a group onto a fresh sub-layer, alpha-composited on close.

        Pillow's draw operations *replace* the pixels they touch, alpha and
        all -- a 6%-white bar drawn straight onto the scrim does not tint it,
        it punches a hole in it. Anything translucent that has to sit over
        something else goes through here.
        """
        scratch = Image.new("RGBA", self.img.size, (0, 0, 0, 0))
        yield ImageDraw.Draw(scratch, "RGBA")
        self.img = Image.alpha_composite(self.img, scratch)
        self.draw = ImageDraw.Draw(self.img, "RGBA")

    def font(self, path: str, size: float) -> ImageFont.FreeTypeFont:
        return ImageFont.truetype(path, round(size * self.ss))

    def resolve(self) -> Image.Image:
        if self.ss == 1:
            return self.img
        # Pillow premultiplies RGBA before a reducing filter and converts back
        # afterwards, so this is two resamples and two mode conversions -- by
        # far the most expensive thing per frame. Supersample only what needs
        # it (see TIMING_SS).
        return self.img.resize((self.width, self.height), self.resample)


#--------------------------------- TEXT -----------------------------------#


def digit_advance(font: ImageFont.FreeTypeFont) -> float:
    return max(font.getlength(d) for d in "0123456789")


def tabular_width(text: str, font: ImageFont.FreeTypeFont) -> float:
    advance = digit_advance(font)
    return sum(advance if c.isdigit() else font.getlength(c) for c in text)


def draw_tabular(draw, xy, text: str, font, fill) -> None:
    """Draws `text` with every digit on a fixed advance.

    A running lap clock redraws sixty times a second, and with proportional
    figures each digit that changes width shifts everything after it -- the
    whole readout wobbles. Fixed advances are wrong for a static hero number
    (they leave `1` looking loose) and mandatory for one that ticks.
    """
    advance = digit_advance(font)
    x, y = xy
    for char in text:
        if char.isdigit():
            draw.text((x + (advance - font.getlength(char)) / 2, y), char,
                      font=font, fill=fill)
            x += advance
        else:
            draw.text((x, y), char, font=font, fill=fill)
            x += font.getlength(char)


def fmt_clock(seconds: float, decimals: int = 2) -> str:
    if seconds is None or not math.isfinite(seconds) or seconds < 0:
        return "--.--" if decimals == 2 else "--.---"
    minutes = int(seconds // 60)
    rest = seconds - minutes * 60
    return f"{minutes}:{rest:0{3 + decimals}.{decimals}f}"


def fmt_delta(seconds: float) -> str:
    if seconds is None or not math.isfinite(seconds):
        return "--.--"
    return f"{seconds:+.2f}"


#---------------------------------- MAP -----------------------------------#


class Projection:
    """Equirectangular fit of a set of lat/lon points onto a canvas.

    Over a few hundred metres of circuit this is indistinguishable from any
    projection that costs more, provided longitude is scaled by cos(lat) so
    the track keeps its shape.
    """

    def __init__(self, lats, lons, width, height, margin):
        self.lat0 = float(np.mean(lats))
        self.k = math.cos(math.radians(self.lat0))
        xs = (np.asarray(lons) - float(np.mean(lons))) * self.k
        ys = np.asarray(lats) - self.lat0
        self.lon0 = float(np.mean(lons))

        span_x = float(xs.max() - xs.min()) or 1e-9
        span_y = float(ys.max() - ys.min()) or 1e-9
        scale = min((width - 2 * margin) / span_x, (height - 2 * margin) / span_y)
        self.scale = scale
        self.cx = width / 2
        self.cy = height / 2
        self.mid_x = float((xs.max() + xs.min()) / 2)
        self.mid_y = float((ys.max() + ys.min()) / 2)

    def __call__(self, lat, lon):
        x = (np.asarray(lon) - self.lon0) * self.k
        y = np.asarray(lat) - self.lat0
        # Screen y grows downward; north stays up.
        return (self.cx + (x - self.mid_x) * self.scale,
                self.cy - (y - self.mid_y) * self.scale)


def load_track(path: Path) -> dict:
    """Reads the annotator's track JSON into edges and timing lines.

    Each entry of `segments` is one gate: a pair of [lat, lon] endpoints
    across the track. Threading the first endpoint of every gate gives one
    edge of the circuit and the second endpoints give the other, which is the
    outline to draw.
    """
    data = json.loads(Path(path).read_text())
    segments = np.asarray(data["segments"], dtype=float)
    if segments.ndim != 3 or segments.shape[1:] != (2, 2):
        raise click.ClickException(f"{path}: unexpected segments shape")
    return {
        "left": segments[:, 0, :],
        "right": segments[:, 1, :],
        "start": segments[0],
        "sectors": [segments[i] for i in data.get("sector_indices", [])
                    if 0 <= i < len(segments)],
    }


def stroke(layer: Layer, points, colour, width: float, halo: float = 2.0) -> None:
    """One polyline, laid over a wider dark one so it holds any background."""
    scaled = [(layer.s(x), layer.s(y)) for x, y in points]
    if halo > 0:
        layer.draw.line(scaled, fill=HALO, width=round(layer.s(width + 2 * halo)),
                        joint="curve")
    layer.draw.line(scaled, fill=colour, width=round(layer.s(width)),
                    joint="curve")


def draw_track(layer: Layer, track: dict, proj: Projection, scale: float) -> None:
    for edge in ("left", "right"):
        pts = track[edge]
        xs, ys = proj(pts[:, 0], pts[:, 1])
        # Closed: the last gate sits a metre short of the start line.
        coords = list(zip(xs, ys)) + [(xs[0], ys[0])]
        stroke(layer, coords, TRACK_EDGE, 2.0 * scale, halo=1.5 * scale)

    for gate, colour, width in ([(track["start"], START_LINE, 4.0)] +
                                [(s, TRACK_EDGE, 2.5) for s in track["sectors"]]):
        xs, ys = proj(gate[:, 0], gate[:, 1])
        stroke(layer, [(xs[0], ys[0]), (xs[1], ys[1])], colour, width * scale,
               halo=1.5 * scale)


def draw_marker(layer: Layer, x: float, y: float, heading: float,
                radius: float) -> None:
    """An arrowhead pointing along the kart's heading.

    Drawn with a dark ring rather than a hard border: over bright kerbs or a
    white pit wall a plain white marker disappears, and the ring is the same
    trick the surface gap plays between adjacent fills.
    """
    if heading is None or not math.isfinite(heading):
        heading = 0.0
    # heading_rad is a bearing: clockwise from north, and north is up.
    dx, dy = math.sin(heading), -math.cos(heading)
    px, py = -dy, dx

    def shape(r: float):
        nose = (x + dx * r * 1.55, y + dy * r * 1.55)
        left = (x - dx * r * 0.75 + px * r, y - dy * r * 0.75 + py * r)
        right = (x - dx * r * 0.75 - px * r, y - dy * r * 0.75 - py * r)
        tail = (x - dx * r * 0.3, y - dy * r * 0.3)
        return [(layer.s(a), layer.s(b)) for a, b in (nose, left, tail, right)]

    layer.draw.polygon(shape(radius * 1.32), fill=HALO[:3] + (215,))
    layer.draw.polygon(shape(radius), fill=INK)


def draw_trail(layer: Layer, xs, ys, radius: float) -> None:
    """The recent racing line, fading out behind the marker."""
    n = len(xs)
    if n < 2:
        return

    def alpha_at(i: int) -> int:
        # The oldest point is invisible, the newest full strength.
        return int(TRAIL[3] * (i / (n - 1)) ** 1.6)

    # Consecutive trail points are a fraction of a pixel apart at 60 fps, so
    # segments almost entirely overlap: haloing each one as it is drawn buries
    # the segment before it. Every halo goes down first, then every line.
    for i in range(1, n):
        if alpha_at(i) <= 2:
            continue
        fade = alpha_at(i) / TRAIL[3]
        layer.draw.line(
            [(layer.s(xs[i - 1]), layer.s(ys[i - 1])),
             (layer.s(xs[i]), layer.s(ys[i]))],
            fill=HALO[:3] + (int(HALO[3] * fade),),
            width=round(layer.s(radius + 3.2 * fade)), joint="curve")
    for i in range(1, n):
        alpha = alpha_at(i)
        if alpha <= 2:
            continue
        layer.draw.line(
            [(layer.s(xs[i - 1]), layer.s(ys[i - 1])),
             (layer.s(xs[i]), layer.s(ys[i]))],
            fill=TRAIL[:3] + (alpha,), width=round(layer.s(radius)),
            joint="curve")


#--------------------------------- TIMING ---------------------------------#


@dataclass
class TimingStyle:
    width: int
    height: int
    font_path: str
    ss: int
    scale: float = 1.0
    resample: int = Image.Resampling.LANCZOS

    def px(self, v: float) -> float:
        return v * self.scale


def draw_timing(row, style: TimingStyle, delta_range: float,
                delta_column: str) -> Image.Image:
    layer = Layer(style.width, style.height, style.ss, style.resample)
    p = style.px

    hero = layer.font(style.font_path, p(74))
    delta_face = layer.font(style.font_path, p(54))
    tile = layer.font(style.font_path, p(34))
    label = layer.font(style.font_path, p(19))

    pad = p(22)
    right = style.width - pad
    layer.draw.rounded_rectangle(
        [0, 0, layer.s(style.width) - 1, layer.s(style.height) - 1],
        radius=layer.s(p(14)), fill=SCRIM)

    delta = row.get(delta_column)
    delta_text = fmt_delta(delta)

    # Everything from here sits *over* the scrim, so it is drawn on a
    # sub-layer and composited: drawn directly it would replace the scrim's
    # alpha instead of tinting it (see Layer.paint).
    with layer.paint() as d:
        lap = row.get("lap_number")
        lap_text = (f"LAP {int(lap)}" if lap is not None and math.isfinite(lap)
                    else "OUT LAP")
        d.text((layer.s(pad), layer.s(p(13))), lap_text, font=label, fill=INK_DIM)
        d.text((layer.s(right), layer.s(p(13))), "DELTA", font=label,
               fill=INK_DIM, anchor="ra")

        draw_tabular(d, (layer.s(pad), layer.s(p(29))),
                     fmt_clock(row.get("current_lap_s"), 2), hero, INK)
        # Right-aligned off its own measured width, so the two figures never
        # collide however wide the delta gets.
        draw_tabular(d, (layer.s(right) - tabular_width(delta_text, delta_face),
                         layer.s(p(45))), delta_text, delta_face, INK)

        # --- the delta meter: a diverging bar either side of a centred zero ---
        bar_y, bar_h = p(126), p(11)
        bar_w = style.width - 2 * pad
        cx = pad + bar_w / 2

        d.rounded_rectangle(
            [layer.s(pad), layer.s(bar_y), layer.s(pad + bar_w),
             layer.s(bar_y + bar_h)],
            radius=layer.s(bar_h / 2), fill=(255, 255, 255, 38))

        if delta is not None and math.isfinite(delta):
            frac = max(-1.0, min(1.0, delta / delta_range))
            colour = BEHIND if delta > 0 else AHEAD
            x0, x1 = ((cx, cx + frac * bar_w / 2) if frac >= 0
                      else (cx + frac * bar_w / 2, cx))
            if abs(x1 - x0) >= p(2):
                d.rounded_rectangle(
                    [layer.s(x0), layer.s(bar_y), layer.s(x1),
                     layer.s(bar_y + bar_h)],
                    radius=layer.s(min(p(4), abs(x1 - x0) / 2)), fill=colour)

        # The zero tick sits over the bar in the surface colour, so the fill
        # reads as growing away from it rather than through it.
        d.rectangle(
            [layer.s(cx - p(1.5)), layer.s(bar_y - p(5)),
             layer.s(cx + p(1.5)), layer.s(bar_y + bar_h + p(5))],
            fill=(12, 12, 12, 235))

        # --- last and best ---
        tile_y = p(163)
        for i, (name, value, accent) in enumerate((
                ("LAST", row.get("last_lap_s"), None),
                ("BEST", row.get("best_lap_s"), BEST))):
            x = pad + i * bar_w / 2
            d.text((layer.s(x), layer.s(tile_y)), name, font=label,
                   fill=accent or INK_DIM)
            draw_tabular(d, (layer.s(x), layer.s(tile_y + p(18))),
                         fmt_clock(value, 3), tile, INK)

    return layer.resolve()


#--------------------------------- FFMPEG ---------------------------------#


class MovieWriter:
    """Pipes straight-alpha RGBA frames into one QuickTime file."""

    def __init__(self, path: Path, size, fps, codec: str, timecode: str | None):
        num, den = fps
        codec_args = (["-c:v", "qtrle"] if codec == "qtrle" else
                      ["-c:v", "prores_ks", "-profile:v", "4444",
                       "-pix_fmt", "yuva444p10le", "-alpha_bits", "16",
                       "-vendor", "apl0"])
        cmd = [
            "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
            "-f", "rawvideo", "-pix_fmt", "rgba",
            "-s", f"{size[0]}x{size[1]}", "-r", f"{num}/{den}",
            "-i", "-",
        ]
        if timecode:
            cmd += ["-timecode", timecode]
        cmd += codec_args + [str(path)]
        self.path = path
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)

    def write(self, image: Image.Image) -> None:
        self.proc.stdin.write(image.tobytes())

    def write_bytes(self, payload: bytes) -> None:
        self.proc.stdin.write(payload)

    def close(self) -> None:
        self.proc.stdin.close()
        if self.proc.wait() != 0:
            raise click.ClickException(f"ffmpeg failed writing {self.path}")


#---------------------------------- CLI -----------------------------------#


def read_frames(path: Path) -> pd.DataFrame:
    if path.suffix in (".ndjson", ".jsonl"):
        return pd.read_json(path, lines=True)
    return pd.read_csv(path)


@dataclass
class RenderContext:
    """Everything a frame needs, rebuildable from a plain dict.

    Frames are independent, so rendering parallelises perfectly -- but macOS
    starts worker processes by spawn, not fork, so the state cannot be
    inherited. Keeping it reconstructible from `cfg` is what lets a worker
    stand itself up from scratch.
    """

    df: pd.DataFrame
    xs: np.ndarray
    ys: np.ndarray
    circuit: dict
    proj: Projection
    style: TimingStyle
    map_px: int
    supersample: int
    scale: float
    trail_frames: int
    delta_range: float
    delta_column: str
    resample: int

    @classmethod
    def build(cls, cfg: dict) -> "RenderContext":
        df = read_frames(Path(cfg["frames"]))
        circuit = load_track(Path(cfg["track"]))
        # Fit to the circuit, not to the driven line, so the map never shifts
        # or rescales between renders of different stints.
        lats = np.concatenate([circuit["left"][:, 0], circuit["right"][:, 0]])
        lons = np.concatenate([circuit["left"][:, 1], circuit["right"][:, 1]])
        proj = Projection(lats, lons, cfg["map_px"], cfg["map_px"],
                          margin=round(26 * cfg["scale"]))
        xs, ys = proj(df["lat"].to_numpy(), df["lon"].to_numpy())
        resample = RESAMPLE[cfg["resample"]]
        style = TimingStyle(cfg["timing_px"][0], cfg["timing_px"][1], cfg["font"],
                            min(cfg["supersample"], TIMING_SS), cfg["scale"],
                            resample)
        return cls(df=df, xs=xs, ys=ys, circuit=circuit, proj=proj, style=style,
                   map_px=cfg["map_px"], supersample=cfg["supersample"],
                   scale=cfg["scale"], trail_frames=cfg["trail_frames"],
                   delta_range=cfg["delta_range"], delta_column=cfg["delta_column"],
                   resample=resample)

    def map_frame(self, i: int) -> Image.Image:
        layer = Layer(self.map_px, self.map_px, self.supersample, self.resample)
        lo = max(0, i - self.trail_frames)
        window_x, window_y = self.xs[lo:i + 1], self.ys[lo:i + 1]
        keep = np.isfinite(window_x) & np.isfinite(window_y)
        if keep.any():
            draw_trail(layer, window_x[keep], window_y[keep], 4.0 * self.scale)
            if np.isfinite(self.xs[i]):
                draw_marker(layer, self.xs[i], self.ys[i],
                            self.df["heading_rad"].iloc[i], 11.0 * self.scale)
        return layer.resolve()

    def timing_frame(self, i: int) -> Image.Image:
        return draw_timing(self.df.iloc[i].to_dict(), self.style,
                           self.delta_range, self.delta_column)

    def frame(self, asset: str, i: int) -> Image.Image:
        return self.map_frame(i) if asset == "map" else self.timing_frame(i)


_CONTEXT: RenderContext | None = None


def _init_worker(cfg: dict) -> None:
    global _CONTEXT
    _CONTEXT = RenderContext.build(cfg)


def _render_bytes(job) -> bytes:
    asset, index = job
    return _CONTEXT.frame(asset, index).tobytes()


@click.command()
@click.option("--frames", required=True, type=click.Path(exists=True, path_type=Path),
              help="Frame table from export_overlay (.csv or .ndjson).")
@click.option("--track", type=click.Path(exists=True, path_type=Path),
              help="Track JSON; defaults to the one named in the manifest.")
@click.option("--out-dir", required=True, type=click.Path(path_type=Path))
@click.option("--only", default="track,map,timing",
              help="Comma-separated subset of track,map,timing.")
@click.option("--map-size", default=420, help="Map canvas, square, in pixels.")
@click.option("--timing-size", default=(600, 236), nargs=2, type=int)
@click.option("--scale", default=1.0,
              help="Multiplies every canvas and type size; 2.0 for 4K delivery.")
@click.option("--supersample", default=3, help="Shape antialiasing factor.")
@click.option("--resample", default="box", type=click.Choice(list(RESAMPLE)),
              help="Filter used to reduce the supersampled canvas.")
@click.option("--trail-s", default=4.0, help="Seconds of racing line behind the kart.")
@click.option("--delta", "delta_column", default="delta_best_s",
              type=click.Choice(["delta_best_s", "delta_live_s"]),
              help="Which delta the meter shows.")
@click.option("--delta-range", default=1.5, help="Delta, in seconds, at full bar.")
@click.option("--codec", default="qtrle", type=click.Choice(["qtrle", "prores"]))
@click.option("--timecode", default=None, help="Start timecode, e.g. 01:00:00:00.")
@click.option("--font", "font_path", default=None)
@click.option("--jobs", default=0,
              help="Worker processes; 0 uses every core, 1 renders in-process.")
@click.option("--preview", type=int, default=None,
              help="Write a PNG of this frame instead of rendering video.")
def main(frames, track, out_dir, only, map_size, timing_size, scale, supersample,
         resample, trail_s, delta_column, delta_range, codec, timecode, font_path,
         jobs, preview):
    """Renders overlay assets from a frame table."""
    manifest_path = frames.parent / (frames.stem + ".manifest.json")
    if not manifest_path.exists():
        raise click.ClickException(f"no manifest beside {frames}")
    manifest = json.loads(manifest_path.read_text())

    fps = (manifest["fps"]["num"], manifest["fps"]["den"])
    fps_value = fps[0] / fps[1]
    track_path = track or Path(manifest["track"])
    if not Path(track_path).exists():
        raise click.ClickException(f"track {track_path} not found; pass --track")

    font_path = font_path or pick_font()
    wanted = {name.strip() for name in only.split(",") if name.strip()}
    out_dir.mkdir(parents=True, exist_ok=True)

    map_px = round(map_size * scale)
    timing_px = (round(timing_size[0] * scale), round(timing_size[1] * scale))

    cfg = {
        "frames": str(frames),
        "track": str(track_path),
        "font": font_path,
        "map_px": map_px,
        "timing_px": timing_px,
        "scale": scale,
        "supersample": supersample,
        "delta_range": delta_range,
        "delta_column": delta_column,
        "resample": resample,
        # Fixed here rather than in the worker so every process agrees, even
        # if the manifest is edited between them.
        "trail_frames": max(2, round(trail_s * fps_value)),
    }
    context = RenderContext.build(cfg)
    total = len(context.df)

    if jobs <= 0:
        jobs = mp.cpu_count()
    jobs = max(1, min(jobs, total))

    click.echo(f"{total} frames at {fps[0]}/{fps[1]} ({fps_value:.4f} fps), "
               f"font {Path(font_path).name}, {jobs} job(s)")

    if "track" in wanted:
        layer = Layer(map_px, map_px, supersample, RESAMPLE[resample])
        draw_track(layer, context.circuit, context.proj, scale)
        layer.resolve().save(out_dir / "track.png")
        click.echo(f"  track.png    {map_px}x{map_px}")

    if preview is not None:
        if not 0 <= preview < total:
            raise click.ClickException(f"--preview out of range 0..{total - 1}")
        for asset in ("map", "timing"):
            if asset in wanted:
                context.frame(asset, preview).save(out_dir / f"preview-{asset}.png")
        click.echo(f"  preview at frame {preview} written to {out_dir}")
        return

    assets = [a for a in ("map", "timing") if a in wanted]
    pool = (mp.Pool(jobs, initializer=_init_worker, initargs=(cfg,))
            if jobs > 1 and assets else None)
    try:
        for asset in assets:
            size = (map_px, map_px) if asset == "map" else timing_px
            writer = MovieWriter(out_dir / f"{asset}.mov", size, fps, codec, timecode)
            queue = [(asset, i) for i in range(total)]
            with click.progressbar(length=total, label=f"  rendering {asset}") as bar:
                if pool is None:
                    for job in queue:
                        writer.write(context.frame(*job))
                        bar.update(1)
                else:
                    # imap keeps frame order, which is the whole requirement --
                    # ffmpeg is fed a stream, not addressed frames.
                    for payload in pool.imap(_render_bytes, queue, chunksize=16):
                        writer.write_bytes(payload)
                        bar.update(1)
            writer.close()
            click.echo(f"  {asset + '.mov':12} "
                       f"{writer.path.stat().st_size / 1e6:.1f} MB")
    finally:
        if pool is not None:
            pool.terminate()
            pool.join()

    (out_dir / "overlay.manifest.json").write_text(json.dumps({
        "frames": str(frames),
        "track": str(track_path),
        "fps": {"num": fps[0], "den": fps[1]},
        "timecode": timecode,
        "codec": codec,
        "alpha": "straight",
        "delta_column": delta_column,
        "delta_range_s": delta_range,
        "resample": resample,
        "assets": {
            "track.png": [map_px, map_px],
            "map.mov": [map_px, map_px],
            "timing.mov": list(timing_px),
        },
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3

"""Exports a recorded lap pair for the landing page's 3D scene.

    scripts/export_racing_line.py tracks/daytona-milton-keynes.json \
        recordings/SESS_056.dat
    scripts/export_racing_line.py tracks/silverstone.json ~/Videos/GX01037*.MP4

Recordings are firmware .dat logs, GoPro .MP4 files (needs ffmpeg on PATH), or
a GoPro telemetry stream already copied out to a .bin. Several files are read
as one recording; a gap of over a minute between fixes starts a new session.

Finds the fastest lap that has an earlier lap to chase and a clean lap after
it, and writes it plus the following lap to landing/laps/<track>.json as
[t_s, lat, lon, km/h, delta_s, distance_fraction] samples. The delta is against
the best lap set earlier in the session (for the first lap) and against the
first lap (for the second), which is what the dashboard would have shown.
Standard library only.
"""

import json
import math
import os
import struct
import subprocess
import sys


OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "landing", "laps")
MIN_LAP_S, MAX_LAP_S = 30, 300
CLEAN_LAP = 1.08  # a lap within 8% of the session best counts as a flying lap
SESSION_GAP_S = 60
TARGET_HZ = 12.5  # plenty for a camera path; denser logs are thinned to this


def load_dat(path):
    """Firmware log: int64 boot-ms stamp + 92-byte uGnssDecUbxNavPvt_t per fix."""
    data = open(path, "rb").read()
    out = []
    for o in range(0, len(data) - 99, 100):
        (itow,) = struct.unpack_from("<I", data, o + 8)
        lon, lat = struct.unpack_from("<ii", data, o + 8 + 28)
        (gspeed,) = struct.unpack_from("<i", data, o + 8 + 64)
        out.append((itow / 1000, lat * 1e-7, lon * 1e-7, gspeed * 3.6e-3))
    return out


def load_gpmf(data):
    """GoPro telemetry: every GPS9 fix (lat, lon, alt, 2D speed, 3D speed, days, seconds, DOP, fix)."""
    out = []

    def walk(o, end):
        while o + 8 <= end:
            key, kind, size = data[o : o + 4], data[o + 4], data[o + 5]
            (repeat,) = struct.unpack_from(">H", data, o + 6)
            if key == b"\0\0\0\0":
                break
            body, n = o + 8, size * repeat
            if kind == 0:
                walk(body, body + n)
            elif key == b"GPS9" and size == 32:
                for i in range(repeat):
                    lat, lon, _, speed, _, days, secs, _, fix = struct.unpack_from(
                        ">7l2H", data, body + i * 32
                    )
                    if fix >= 2:
                        out.append(
                            (
                                days * 86400 + secs / 1000,
                                lat * 1e-7,
                                lon * 1e-7,
                                speed * 3.6e-3,
                            )
                        )
            o = body + ((n + 3) & ~3)

    walk(0, len(data))
    return out


def load(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".dat":
        return load_dat(path)
    if ext == ".bin":
        return load_gpmf(open(path, "rb").read())
    probe = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "stream=index,codec_tag_string",
            "-of",
            "csv=p=0",
            path,
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    index = next(line.split(",")[0] for line in probe.splitlines() if "gpmd" in line)
    raw = subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            path,
            "-map",
            f"0:{index}",
            "-c",
            "copy",
            "-f",
            "rawvideo",
            "-",
        ],
        check=True,
        capture_output=True,
    ).stdout
    return load_gpmf(raw)


def main(track_path, *recordings):
    track = json.load(open(track_path))
    name = os.path.splitext(os.path.basename(track_path))[0]
    gates = track["segments"]
    (la0, lo0), gate_b = gates[0]
    kx, ky = 111320 * math.cos(math.radians(la0)), 110540
    xy = lambda la, lo: ((lo - lo0) * kx, (la - la0) * ky)
    a, b = (0.0, 0.0), xy(*gate_b)

    fixes = sorted(f for path in recordings for f in load(path))
    sessions, start = [], 0
    for i in range(1, len(fixes) + 1):
        if i == len(fixes) or fixes[i][0] - fixes[i - 1][0] > SESSION_GAP_S:
            sessions.append(fixes[start:i])
            start = i

    def crossings_of(pts):
        """Start-line crossings, interpolated between fixes: (time, fix index, fraction to the next fix)."""
        pos = [xy(p[1], p[2]) for p in pts]
        out = []
        for i in range(len(pts) - 1):
            p, q = pos[i], pos[i + 1]
            d1, d2 = (q[0] - p[0], q[1] - p[1]), (b[0] - a[0], b[1] - a[1])
            den = d1[0] * d2[1] - d1[1] * d2[0]
            if abs(den) < 1e-12 or pts[i][3] < 10:
                continue
            t = ((a[0] - p[0]) * d2[1] - (a[1] - p[1]) * d2[0]) / den
            u = ((a[0] - p[0]) * d1[1] - (a[1] - p[1]) * d1[0]) / den
            if 0 <= t < 1 and -0.3 <= u <= 1.3:
                out.append((pts[i][0] + t * (pts[i + 1][0] - pts[i][0]), i, t))
        return out

    # the fastest lap anywhere that was a new session best with a clean lap after it
    pick = None
    for pts in sessions:
        crossings = crossings_of(pts)
        times = [
            crossings[k + 1][0] - crossings[k][0] for k in range(len(crossings) - 1)
        ]
        flying = [t for t in times if MIN_LAP_S < t < MAX_LAP_S]
        if len(flying) < 3:
            continue
        limit = min(flying) * CLEAN_LAP
        for k in range(1, len(times) - 1):
            earlier = [j for j in range(k) if MIN_LAP_S < times[j] < limit]
            if not (MIN_LAP_S < times[k] < limit and times[k + 1] < limit and earlier):
                continue
            if times[k] < min(times[j] for j in earlier) and (
                pick is None or times[k] < pick[0]
            ):
                pick = (
                    times[k],
                    k,
                    min(earlier, key=lambda j: times[j]),
                    pts,
                    crossings,
                    times,
                )
    if pick is None:
        sys.exit(f"no usable lap pair on {name} in {len(fixes)} fixes")
    _, best, ref, pts, crossings, times = pick
    rate = (len(pts) - 1) / (pts[-1][0] - pts[0][0])
    stride = max(1, round(rate / TARGET_HZ))

    def samples(k):
        """(t since crossing, x, y, lat, lon, kmh) for lap k, crossing to crossing."""
        (t0, i0, f0), (t1, i1, f1) = crossings[k], crossings[k + 1]
        lerp = lambda i, f, j: pts[i][j] + f * (pts[i + 1][j] - pts[i][j])
        rows = [(0.0, lerp(i0, f0, 1), lerp(i0, f0, 2), lerp(i0, f0, 3))]
        rows += [
            (pts[i][0] - t0, pts[i][1], pts[i][2], pts[i][3])
            for i in range(i0 + 1, i1 + 1)
        ]
        rows.append((t1 - t0, lerp(i1, f1, 1), lerp(i1, f1, 2), lerp(i1, f1, 3)))
        return [(t, *xy(la, lo), la, lo, v) for t, la, lo, v in rows]

    def delta_against(cur, reference):
        """Time gap at each sample of `cur`: its clock minus the reference's at the same place."""
        out, j = [], 0
        for t, x, y, *_ in cur:
            best_d, best_t = 1e18, 0.0
            for m in range(max(0, j - 5), min(len(reference) - 1, j + 60)):
                (ta, xa, ya, *_), (tb, xb, yb, *_) = reference[m], reference[m + 1]
                dx, dy = xb - xa, yb - ya
                s = max(
                    0.0,
                    min(
                        1.0,
                        ((x - xa) * dx + (y - ya) * dy) / (dx * dx + dy * dy + 1e-12),
                    ),
                )
                d = (x - xa - s * dx) ** 2 + (y - ya - s * dy) ** 2
                if d < best_d:
                    best_d, best_t, j = d, ta + s * (tb - ta), m
            out.append(t - best_t)
        out[0], out[-1] = 0.0, cur[-1][0] - reference[-1][0]
        return out

    def pack(k, reference):
        cur = samples(k)
        deltas = delta_against(cur, reference)
        dist = [0.0]
        for m in range(1, len(cur)):
            dist.append(dist[-1] + math.dist(cur[m][1:3], cur[m - 1][1:3]))
        rows = list(zip(cur, deltas, dist))
        return {
            "no": k + 1,
            "time": round(times[k], 3),
            "length_m": round(dist[-1]),
            "samples": [
                [
                    round(t, 3),
                    round(la, 7),
                    round(lo, 7),
                    round(v, 1),
                    round(d, 3),
                    round(s / dist[-1], 5),
                ]
                for (t, _, _, la, lo, v), d, s in rows[:-1:stride] + rows[-1:]
            ],
        }

    # Which way round the gates were driven: is the lap's first stretch nearer gate 1 or the last gate?
    first = samples(best)
    probe = first[len(first) // 20]
    mid = lambda g: xy((g[0][0] + g[1][0]) / 2, (g[0][1] + g[1][1]) / 2)
    nearest = min(
        range(1, len(gates)), key=lambda g: math.dist(mid(gates[g]), probe[1:3])
    )

    out = {
        "track": name,
        "source": [os.path.basename(p) for p in recordings],
        "fields": ["t_s", "lat", "lon", "kmh", "delta_s", "distance_fraction"],
        "gates_reversed": nearest > len(gates) / 2,
        "best_before": round(times[ref], 3),
        # session clock at the first lap's start line: the firmware starts it on the first crossing
        "clock_at_start": round(crossings[best][0] - crossings[0][0], 1),
        "history": {
            str(k + 1): round(times[k], 3) for k in range(max(0, best - 6), best)
        },
        "laps": [pack(best, samples(ref)), pack(best + 1, first)],
    }
    os.makedirs(OUT_DIR, exist_ok=True)
    out_path = os.path.normpath(os.path.join(OUT_DIR, f"{name}.json"))
    json.dump(out, open(out_path, "w"), separators=(",", ":"))

    print(
        f"{name}: {len(fixes)} fixes at {rate:.1f} Hz in {len(sessions)} session(s), gates reversed: {out['gates_reversed']}"
    )
    for lap in out["laps"]:
        ds = [s[4] for s in lap["samples"]]
        vs = [s[3] for s in lap["samples"]]
        print(
            f"  lap {lap['no']}: {lap['time']} s, {lap['length_m']} m, {len(lap['samples'])} samples, "
            f"{min(vs):.0f}-{max(vs):.0f} km/h, delta {min(ds):+.2f}..{max(ds):+.2f}, ends {ds[-1]:+.3f}"
        )
    print(
        f"  best before: {out['best_before']}, history: {out['history']}, clock: {out['clock_at_start']}"
    )
    print(f"  wrote {out_path} ({os.path.getsize(out_path) // 1024} KB)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])

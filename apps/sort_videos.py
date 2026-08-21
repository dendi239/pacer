#! /usr/bin/env python3

"""
Sorts GoPro videos to track-based folders.

It uses GPMF data to determine the location of the track and then moves the
video to the appropriate folder. The library itself should contain pacer.json
file with definition of tracks and their locations. You can provide location
for unknown tracks if you'd like, those are left as-is by default.
"""

import concurrent.futures
import dataclasses
import datetime
import json
import logging
import shutil
import tempfile

from pathlib import Path

import click

from pacer import CoordinateSystem, GPMFSource, GPSSample


class _Logger:
    def __call__(self, level: str, msg: str) -> None:
        print(f"{datetime.datetime.now()} [{level}] {msg}")

    def info(self, msg: str) -> None:
        self("INFO", msg)

    def warning(self, msg: str) -> None:
        self("WARN", msg)


log = _Logger()


def _from_london() -> CoordinateSystem:
    return CoordinateSystem(GPSSample(lat=51.5074, lon=-0.1278, altitude=0))


@dataclasses.dataclass
class Track:
    name: str
    location: GPSSample
    radius: float
    cs: CoordinateSystem = dataclasses.field(default_factory=_from_london)

    def __contains__(self, sample: GPSSample) -> bool:
        return self.location.distance_to(sample) <= self.radius


@dataclasses.dataclass
class Library:
    version: str
    path: Path
    tracks: tuple[Track, ...]

    def guess_track(self, sample: GPSSample) -> Track | None:
        for track in self.tracks:
            if sample in track:
                return track
        return None


def parse_library(path: Path) -> Library:
    library_definition = path / "pacer.json"
    assert library_definition.exists(), f"Definition not found at {library_definition}"

    with library_definition.open("r") as f:
        definition = json.load(f)

    assert isinstance(definition, dict), "Library definition must be a dictionary"
    assert "version" in definition, "Library definition must contain a version"
    assert "tracks" in definition, "Library definition must contain tracks"

    return Library(
        version=definition["version"],
        path=path,
        tracks=tuple(
            Track(
                name=track["name"],
                location=GPSSample(**track["location"]),
                radius=track["radius"],
            )
            for track in definition["tracks"]
        ),
    )


def get_sample_from_video(video: Path, max_samples: int = 1_000) -> GPSSample | None:
    """
    Return a point from within the video's GPS bounds.

    Tries to be somewhat stable by:
      - avoiding first couple of observation: those tend to be stale or inaccurate;
      - averaging over multiple samples to avoid outliers.
    """

    cs = _from_london()

    f = GPMFSource(str(video))
    samples = []

    def on_sample(sample: GPSSample, _index: int, _count: int) -> None:
        samples.append(sample)

    while not f.is_end() and len(samples) < max_samples:
        f.read_samples(on_sample)
        f.next()

    # we avoid first "calibration" samples as they might be inaccurate or stale
    good_samples = [s for ps, s in zip(samples, samples[1:]) if cs.distance(ps, s) > 5]

    return GPSSample(
        lat=sum(s.lat for s in good_samples) / len(good_samples),
        lon=sum(s.lon for s in good_samples) / len(good_samples),
        altitude=sum(s.altitude for s in good_samples) / len(good_samples),
    )


def process_video(
    video: Path, library: Library, extra: Path | None, workdir: Path
) -> None:
    local_path = workdir / video.name
    shutil.copy(video, local_path)
    point = get_sample_from_video(local_path)

    if (track := library.guess_track(point)) is not None:
        log.info(f"Found track {track.name} for {video.name}")
        (library.path / track.name).mkdir(exist_ok=True)
        shutil.move(str(local_path), str(library.path / track.name / video.name))
        video.unlink()
    else:
        log.info(f"Unknown track for {video}")
        if extra is not None:
            extra.mkdir(exist_ok=True)
            shutil.move(str(local_path), extra)
            video.unlink()

    local_path.unlink()


@click.command(help=__doc__)
@click.argument("source", type=click.Path(exists=True, file_okay=False, path_type=Path))
@click.option(
    "-l",
    "--library",
    help="library location to store to. Must contain `pacer.json`",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
)
@click.option("-g", "--glob", default="*.MP4", show_default=True)
@click.option(
    "--extra",
    default=None,
    help="Where to store videos without a known track."
    " If not provided, they are left in the source folder.",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
)
@click.option("-j", "--jobs", default=10, help="Number of parallel jobs")
def main(source: Path, library: Path, glob: str, jobs: int, extra: Path | None) -> None:
    logging.basicConfig()

    library = parse_library(library)
    log.info(f"Successfully parsed library with {len(library.tracks)} tracks")

    with (
        concurrent.futures.ThreadPoolExecutor(jobs) as executor,
        tempfile.TemporaryDirectory() as tempdir,
    ):
        log.info(f"Workdir: {tempdir}")

        futures = [
            executor.submit(process_video, video, library, extra, Path(tempdir))
            for video in source.glob(glob)
        ]

        for future in concurrent.futures.as_completed(futures):
            try:
                future.result()
            except Exception as e:
                click.echo(f"Error processing video: {e}", err=True)


if __name__ == "__main__":
    main()

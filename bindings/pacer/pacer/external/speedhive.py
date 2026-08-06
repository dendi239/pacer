import concurrent.futures
import dataclasses
import functools
import io

import fsspec
import pandas as pd

from pacer.util import parse_laptime_seconds


def read_speedhive_csv(url: str) -> pd.DataFrame:
    with fsspec.open(url) as f:
        data = f.read()
        data = data.replace(b'",', b" ")
    return pd.read_csv(io.BytesIO(data))


@functools.lru_cache
def _get_results_df(base_url: str) -> pd.DataFrame:
    return (
        read_speedhive_csv(f"{base_url}/csv")
        .set_index("Pos")
        .rename_axis(index=None)
        .assign(**{"Total Time": lambda d: parse_laptime_seconds(d["Total Time"])})
    )


@functools.lru_cache
def _get_competitor_data(base_url: str, pos: int) -> pd.DataFrame:

    with fsspec.open(f"{base_url}/lapdata/{pos}/csv", "rb") as f:
        data = f.read()
    df = pd.read_csv(io.BytesIO(data.replace(b'",', b" ")))
    df = df.assign(**{"Lap Time": lambda d: d["Lap Time"].pipe(parse_laptime_seconds)})
    return df


@functools.lru_cache
def _get_laptimes(base_url: str, results_df: pd.DataFrame) -> pd.DataFrame:
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        return pd.DataFrame(
            dict(
                executor.map(
                    lambda i_row: (
                        i_row[1]["Competitor"],
                        _get_competitor_data(base_url, i_row[0])["Lap Time"],
                    ),
                    results_df.iterrows(),
                )
            )
        )


@dataclasses.dataclass(frozen=True)
class SpeedhiveSession:
    session_id: int
    name: str

    def base_url(self) -> str:
        return f"https://eventresults-api.speedhive.com/api/v0.2.3/eventresults/sessions/{self.session_id}"

    def results_df(self) -> pd.DataFrame:
        return _get_results_df(self.base_url())

    def competitor_data(self, pos: int) -> pd.DataFrame:
        return _get_competitor_data(self.base_url(), pos)

    def laptimes(self) -> pd.DataFrame:
        return _get_laptimes(self.base_url(), self.results_df())

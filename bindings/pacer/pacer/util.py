import pandas as pd


def parse_laptime_seconds(s: pd.Series) -> pd.Series:
    """Parse '1:26.354' / '58.201' / '1:20:40.123' style strings to seconds (float)."""
    seconds = s.str.split(":").apply(
        lambda parts: sum(float(part) * 60**i for i, part in enumerate(reversed(parts)))
    )
    return pd.to_timedelta(seconds, unit="s")

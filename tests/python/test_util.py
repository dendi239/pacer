import pandas as pd

from pacer.util import parse_laptime_seconds


def _timedeltas(*seconds: float) -> pd.Series:
    return pd.Series(pd.to_timedelta(list(seconds), unit="s"))


def test_plain_seconds():
    result = parse_laptime_seconds(pd.Series(["58.201", "12.5"]))
    expected = _timedeltas(58.201, 12.5)
    pd.testing.assert_series_equal(result, expected)


def test_minutes_seconds():
    result = parse_laptime_seconds(pd.Series(["1:26.354", "0:05.000"]))
    expected = _timedeltas(86.354, 5.0)
    pd.testing.assert_series_equal(result, expected)


def test_hours_minutes_seconds():
    result = parse_laptime_seconds(pd.Series(["1:20:40.123"]))
    expected = _timedeltas(1 * 3600 + 20 * 60 + 40.123)
    pd.testing.assert_series_equal(result, expected)


def test_mixed_formats_in_one_series():
    result = parse_laptime_seconds(pd.Series(["1:20:40.123", "1:26.354", "58.201"]))
    expected = _timedeltas(1 * 3600 + 20 * 60 + 40.123, 86.354, 58.201)
    pd.testing.assert_series_equal(result, expected)

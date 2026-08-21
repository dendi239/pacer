"""Raw reader for the .dat session logs the Speedor firmware writes.

`pacer.load_gps_files` decodes these too, but it narrows every record to a
GPSSample, whose `timestamp_ms` is the receiver's `iTOW` — GPS time of week,
not a wall clock and not a Unix epoch. This module unpacks the underlying
`uGnssDecUbxNavPvt_t` instead, so the UTC date/time fields, the fix quality
(`fixType`, `numSV`, `hAcc`) and the firmware's own boot-relative stamp all
survive into the DataFrame.
"""

import os
import struct

import pandas as pd


# uGnssDecUbxNavPvt_t under natural alignment, matching the struct in
# apps/datparser.c and firmware/components/storage/storage.cpp.
_PVT = struct.Struct("<IH6BIii3Bx4i2I5i2I2HihH")

# DatVersion.with_timestamp prefixes each record with the int64 the firmware
# stamps at append time (esp_timer_get_time() / 1000, so milliseconds since
# boot); DatVersion.just_data is the bare struct.
_REC = struct.Struct(f"<q{_PVT.size}s")

_FIELDS = [
    "iTOW",
    "year",
    "month",
    "day",
    "hour",
    "minute",
    "sec",
    "valid",
    "tAcc",
    "nano",
    "fixType",
    "flags",
    "flags2",
    "numSV",
    "lon",
    "lat",
    "height",
    "hMSL",
    "hAcc",
    "vAcc",
    "velN",
    "velE",
    "velD",
    "gSpeed",
    "headMot",
    "sAcc",
    "headAcc",
    "pDOP",
    "flags3",
    "headVeh",
    "magDec",
    "magAcc",
]

# uGnssDecUbxNavPvtValid_t: date and time are only meaningful with both set.
_VALID_DATE_AND_TIME = 0x03


def _has_timestamp(size: int) -> bool:
    """Whether a file of `size` bytes carries the int64 prefix per record.

    Both layouts divide evenly only for sizes that are multiples of 2300, which
    a real session reaches at 23 records; prefer the format the firmware
    actually writes and let callers override.
    """
    if size % _REC.size == 0:
        return True
    if size % _PVT.size == 0:
        return False
    raise ValueError(
        f"{size} bytes is not a whole number of records "
        f"({_REC.size}-byte with timestamp, {_PVT.size}-byte bare)"
    )


def read_dat_raw(
    path: str | os.PathLike, *, with_timestamp: bool | None = None
) -> pd.DataFrame:
    """Every UBX-NAV-PVT field of a .dat log, in a DataFrame.

    Columns are the struct's own fields in receiver units, plus `boot_ms` (the
    firmware's stamp, absent as NaN for bare logs) and the derived columns
    worth having on hand:

      utc        wall clock from the receiver's date/time fields, NaT until the
                 fix resolves them — the only absolute time in the file
      t          seconds since the first record, off iTOW
      lat / lon  degrees
      altitude   metres above the ellipsoid
      speed_kmh  2D ground speed
      hAcc_m     horizontal accuracy estimate

    `with_timestamp` overrides the layout sniffed from the file size.
    """
    blob = os.fspath(path)
    with open(blob, "rb") as f:
        data = f.read()

    if with_timestamp is None:
        with_timestamp = _has_timestamp(len(data))

    if with_timestamp:
        records = _REC.iter_unpack(data)
        rows = [
            dict(zip(_FIELDS, _PVT.unpack(pvt)), boot_ms=boot) for boot, pvt in records
        ]
    else:
        rows = [dict(zip(_FIELDS, pvt)) for pvt in _PVT.iter_unpack(data)]

    df = pd.DataFrame(rows, columns=_FIELDS + ["boot_ms"])

    # nano is a signed correction to `sec`, so it may carry the time across a
    # second boundary in either direction; adding it as a timedelta is exact.
    utc = pd.to_datetime(
        df[["year", "month", "day", "hour", "minute", "sec"]].rename(
            columns={"sec": "second"}
        ),
        errors="coerce",
    ) + pd.to_timedelta(df["nano"], unit="ns")

    return df.assign(
        utc=utc.where(df["valid"] & _VALID_DATE_AND_TIME == _VALID_DATE_AND_TIME),
        t=lambda d: (d["iTOW"] - d["iTOW"].iloc[0]) / 1e3,
        lat=lambda d: d["lat"] / 1e7,
        lon=lambda d: d["lon"] / 1e7,
        altitude=lambda d: d["height"] / 1e3,
        speed_kmh=lambda d: d["gSpeed"] * 0.0036,
        hAcc_m=lambda d: d["hAcc"] / 1e3,
    )

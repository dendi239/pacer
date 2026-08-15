#!/usr/bin/env python3

"""Read/program the u-blox M10 "high performance" CPU clock over a USB-UART cable.

The high clock is what makes 10 Hz possible with four concurrent constellations
(the default clock caps 4-GNSS navigation at 5 Hz). It does not live in the
normal CFG-VALSET layers: it is burned into the module's one-time programmable
memory, and the manual is blunt that this "cannot be reverted".

So the default mode here is read-only:

    python apps/ubx_high_clock.py --port /dev/cu.usbserial-XXXX

which polls UBX-MON-VER and reports whether the high clock is already set.
Burning it takes an explicit flag and a typed confirmation:

    python apps/ubx_high_clock.py --port /dev/cu.usbserial-XXXX --program

Byte strings are copied verbatim from the SAM-M10Q integration manual
(UBX-22020019 R02, section 2.1.5); check the same section of the integration
manual for your own module before programming it.
"""

import argparse
import sys
import time

import serial

# Section 2.1.5, table 3: the two OTP writes that select the high CPU clock.
PROGRAM_FRAMES = [
    bytes.fromhex(
        "B5 62 06 41 10 00 03 00 04 1F 54 5E 79 BF 28 EF 12 05 FD FF FF FF 8F 0D"
    ),
    bytes.fromhex(
        "B5 62 06 41 1C 00 04 01 A4 10 BD 34 F9 12 28 EF 12 05 05 00 A4 40 00 B0"
        " 71 0B 0A 00 A4 40 00 D8 B8 05 DE AE"
    ),
]

# Section 2.1.5 step 5: poll of the four clock keys. Replayed verbatim rather
# than rebuilt — it asks for layer 0x04, which is not one of the documented
# VALGET layers.
VERIFY_POLL = bytes.fromhex(
    "B5 62 06 8B 14 00 00 04 00 00 01 00 A4 40 03 00 A4 40 05 00 A4 40 0A 00"
    " A4 40 4C 15"
)
EXPECTED_CLOCKS = {
    0x40A40001: 0x0B71B000,  # 192 MHz
    0x40A40003: 0x0B71B000,
    0x40A40005: 0x0B71B000,
    0x40A4000A: 0x05B8D800,  # 96 MHz
}

# The module comes up at whatever baud its own config holds; the dashboard
# firmware only ever writes the RAM layer, so a power-cycled receiver is back
# at the u-blox 9600 default. Probe the plausible ones in likeliest order.
CANDIDATE_BAUDS = [9600, 115200, 38400, 460800, 921600, 230400]

SYNC = b"\xb5\x62"


def checksum(body: bytes) -> bytes:
    a = c = 0
    for x in body:
        a = (a + x) & 0xFF
        c = (c + a) & 0xFF
    return bytes([a, c])


def frame(cls: int, msg_id: int, payload: bytes = b"") -> bytes:
    body = bytes([cls, msg_id, len(payload) & 0xFF, len(payload) >> 8]) + payload
    return SYNC + body + checksum(body)


def self_check() -> None:
    """Guards the hand-copied frames above against a transcription slip."""
    for message in PROGRAM_FRAMES + [VERIFY_POLL]:
        length = message[4] | (message[5] << 8)
        assert len(message) == 8 + length, f"bad length: {message.hex(' ')}"
        assert checksum(message[2 : 6 + length]) == message[6 + length :], (
            f"bad checksum: {message.hex(' ')}"
        )


def read_frames(port: serial.Serial, timeout_s: float):
    """Yields (cls, id, payload) for every valid UBX frame seen within the window."""
    deadline = time.time() + timeout_s
    buf = bytearray()
    while time.time() < deadline:
        chunk = port.read(256)
        if chunk:
            buf += chunk
        else:
            time.sleep(0.01)
        while True:
            start = buf.find(SYNC)
            if start < 0:
                del buf[: max(0, len(buf) - 1)]
                break
            del buf[:start]
            if len(buf) < 6:
                break
            length = buf[4] | (buf[5] << 8)
            if len(buf) < 8 + length:
                break
            candidate = bytes(buf[: 8 + length])
            del buf[: 8 + length]
            if checksum(candidate[2 : 6 + length]) == candidate[6 + length :]:
                yield candidate[2], candidate[3], candidate[6 : 6 + length]


def await_frame(port, cls, msg_id, timeout_s=2.0) -> tuple[bytes | None, bool]:
    """Waits for one message. Returns (payload, receiver_said_nak)."""
    for got_cls, got_id, payload in read_frames(port, timeout_s):
        if (got_cls, got_id) == (cls, msg_id):
            return payload, False
        if (got_cls, got_id) == (0x05, 0x00):  # ACK-NAK
            return None, True
    return None, False


def poll_version(port) -> str | None:
    """Polls UBX-MON-VER; returns the receiver's version block, or None."""
    port.reset_input_buffer()
    port.write(frame(0x0A, 0x04))
    payload, _ = await_frame(port, 0x0A, 0x04, timeout_s=1.5)
    if payload is None or len(payload) < 40:
        return None

    def text(raw: bytes) -> str:
        return raw.split(b"\x00")[0].decode("latin-1").strip()

    lines = [f"sw {text(payload[0:30])}", f"hw {text(payload[30:40])}"]
    for offset in range(40, len(payload) - 29, 30):
        extension = text(payload[offset : offset + 30])
        if extension:
            lines.append(extension)
    return "\n  ".join(lines)


def connect(path: str, baud: int | None) -> tuple[serial.Serial, str]:
    """Opens the port and finds a baud the receiver actually answers on."""
    bauds = [baud] if baud else CANDIDATE_BAUDS
    port = serial.Serial(path, bauds[0], timeout=0.2)
    for candidate in bauds:
        port.baudrate = candidate
        time.sleep(0.05)
        version = poll_version(port)
        if version:
            print(f"receiver at {candidate} baud:\n  {version}")
            return port, version
    port.close()
    tried = ", ".join(str(b) for b in bauds)
    sys.exit(
        f"no UBX-MON-VER answer on {path} (tried {tried} baud).\n"
        "Check: cable RX to module TX, cable TX to module RX, common ground, "
        "3.3 V logic, and nothing else driving the module's RX line."
    )


def report_clocks(port) -> bool | None:
    """Reads the four clock keys. True = high clock, False = default, None = no answer."""
    port.reset_input_buffer()
    port.write(VERIFY_POLL)
    payload, nak = await_frame(port, 0x06, 0x8B, timeout_s=2.0)
    if nak:
        # The keys only exist in the OTP layer once written, so a NAK here is
        # the "not programmed" answer rather than a failure of the query.
        print("  clock keys unknown to the receiver (NAK) -> default CPU clock")
        return False
    if payload is None or len(payload) < 12:
        print("  no answer to the clock-key query")
        return None
    values = {}
    for offset in range(4, len(payload) - 7, 8):
        key = int.from_bytes(payload[offset : offset + 4], "little")
        values[key] = int.from_bytes(payload[offset + 4 : offset + 8], "little")
    for key, value in values.items():
        want = EXPECTED_CLOCKS.get(key)
        mark = "high" if value == want else "default/other"
        print(f"  0x{key:08X} = 0x{value:08X} ({value / 1e6:g} MHz, {mark})")
    return all(values.get(k) == v for k, v in EXPECTED_CLOCKS.items())


def program(port) -> None:
    port.reset_input_buffer()
    acks = 0
    for i, message in enumerate(PROGRAM_FRAMES, 1):
        port.write(message)
        payload, nak = await_frame(port, 0x05, 0x01, timeout_s=3.0)
        if payload is None:
            reason = "receiver NAKed it" if nak else "no answer"
            sys.exit(f"OTP write {i}/2: {reason} — stopping, module state unclear.")
        acks += 1
        print(f"OTP write {i}/2 acknowledged")
    if acks != 2:
        sys.exit("expected two ACK-ACKs")

    # The new clock is only picked up by a hardware reset (CFG-RST resetMode
    # 0x00), not by a software restart.
    print("sending UBX-CFG-RST (hardware reset)")
    port.write(frame(0x06, 0x04, bytes([0x00, 0x00, 0x00, 0x00])))
    time.sleep(1.5)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="e.g. /dev/cu.usbserial-XXXX")
    parser.add_argument(
        "--baud", type=int, default=None, help="skip the baud probe and use this one"
    )
    parser.add_argument(
        "--program",
        action="store_true",
        help="burn the high clock into OTP (irreversible)",
    )
    args = parser.parse_args()
    self_check()

    port, _ = connect(args.port, args.baud)
    print("current clock configuration:")
    already_high = report_clocks(port)

    if not args.program:
        print(
            "\nread-only run. Re-run with --program to burn the high clock "
            "(irreversible)."
        )
        return
    if already_high:
        print("\nalready programmed for the high clock; nothing to do.")
        return

    print(
        "\nThis writes 18 bytes of one-time programmable memory. It is "
        "PERMANENT and cannot be undone, and it is only valid for u-blox M10 "
        "receivers (check section 2.1.5 of your module's integration manual)."
    )
    if input("Type 'burn' to proceed: ").strip() != "burn":
        print("aborted, nothing written.")
        return

    program(port)
    print("re-checking after reset:")
    port.close()
    port, _ = connect(args.port, args.baud)
    if report_clocks(port):
        print("high performance clock is active.")
    else:
        print("clock keys did not read back as high — power-cycle and re-run.")


if __name__ == "__main__":
    main()

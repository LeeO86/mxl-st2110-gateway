#!/usr/bin/env python3
"""Sets the kernel TAI offset (TAI - UTC, seconds) and prints it.

MXL grain indices are on CLOCK_TAI. A host that does not set the offset (chrony or
ptp4l normally do) has CLOCK_TAI equal to UTC, 37 s off. The CI runner image of
2026-11-01 no longer sets it, and the loopback test then saw every flow 37 s late.
Needs CAP_SYS_TIME (run with sudo). Usage: set_tai_offset.py [seconds, default 37]
"""
import ctypes
import sys

ADJ_TAI = 0x0080


class Timeval(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_usec", ctypes.c_long)]


class Timex(ctypes.Structure):
    # struct timex from <sys/timex.h> (x86_64 and arm64 glibc).
    _fields_ = [
        ("modes", ctypes.c_uint), ("offset", ctypes.c_long), ("freq", ctypes.c_long),
        ("maxerror", ctypes.c_long), ("esterror", ctypes.c_long), ("status", ctypes.c_int),
        ("constant", ctypes.c_long), ("precision", ctypes.c_long), ("tolerance", ctypes.c_long),
        ("time", Timeval), ("tick", ctypes.c_long), ("ppsfreq", ctypes.c_long),
        ("jitter", ctypes.c_long), ("shift", ctypes.c_int), ("stabil", ctypes.c_long),
        ("jitcnt", ctypes.c_long), ("calcnt", ctypes.c_long), ("errcnt", ctypes.c_long),
        ("stbcnt", ctypes.c_long), ("tai", ctypes.c_int), ("padding", ctypes.c_int * 11),
    ]


def adjtimex(tx):
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.adjtimex(ctypes.byref(tx)) < 0:
        raise OSError(ctypes.get_errno(), "adjtimex failed")
    return tx.tai


offset = int(sys.argv[1]) if len(sys.argv) > 1 else 37
tx = Timex(modes=ADJ_TAI, constant=offset)
adjtimex(tx)
now = adjtimex(Timex())
print(f"kernel TAI offset {now} s")
sys.exit(0 if now == offset else 1)

"""Import a disciplined chrony clock without blocking packet scheduling."""
import argparse
import csv
import math
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from .protocol import pack


def uncertainty(csv_line):
    # chronyc -c tracking's stable machine-readable field order:
    # reference,stratum,ref_time,system_offset,last_offset,rms,frequency,
    # residual_frequency,skew,root_delay,root_dispersion,update_interval,leap.
    fields = next(csv.reader([csv_line.strip()]))
    if len(fields) != 13 or fields[12] != 'Normal' or not 1 <= int(fields[1]) <= 15:
        raise ValueError('clock is not synchronized')
    if fields[0].upper() in ('7F7F0101','127.127.1.1'):
        raise ValueError('chrony local reference is not an external time source')
    offset, delay, dispersion = (float(fields[i]) for i in (3,9,10))
    if not all(math.isfinite(v) for v in (offset,delay,dispersion)) or dispersion < 0:
        raise ValueError('invalid chrony uncertainty')
    return math.ceil((abs(offset) + abs(delay)/2 + dispersion) * 1e9)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mission-clock-') as directory:
        client = socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
        client.bind(str(Path(directory)/'client'))
        client.settimeout(.5)
        while True:
            start = time.monotonic_ns()
            try:
                result = subprocess.run(['chronyc','-c','tracking'],capture_output=True,text=True,
                                        timeout=.5,check=True,env=dict(os.environ,LC_ALL='C'))
                error = uncertainty(result.stdout) + time.monotonic_ns() - start
                client.sendto(pack(dict(command='clock',utc_ns=time.time_ns(),
                                        uncertainty_ns=error,source='NTP')),args.socket)
                client.recv(4096)
            except (OSError,ValueError,subprocess.SubprocessError):
                # No sample means holdover then UNSYNCED, never false health.
                pass
            time.sleep(1)


if __name__=='__main__':main()

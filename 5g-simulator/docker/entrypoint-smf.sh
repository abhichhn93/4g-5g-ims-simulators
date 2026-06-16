#!/bin/sh
# SMF entrypoint: register with NRF first, then serve SBI on :29502.
# UPF is discovered via NRF -- if UPF_HOST is set, that's the fallback.
NRF_HOST="${NRF_HOST:-127.0.0.1}"
python3 - <<PY
import socket, time, sys
host = "$NRF_HOST"
for _ in range(30):
    try:
        socket.create_connection((host, 29510), timeout=1).close()
        sys.exit(0)
    except OSError:
        time.sleep(1)
print(f"WARNING: NRF {host}:29510 never became reachable", file=sys.stderr)
PY
exec ./smf_sim

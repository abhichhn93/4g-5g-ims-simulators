#!/bin/sh
# ue_sim entrypoint: wait for IMS server SIP port, then run a scripted
# VoLTE scenario: REGISTER all UEs, make calls, send BYE.
IMS_HOST="${IMS_HOST:-127.0.0.1}"
IMS_PORT="${IMS_PORT:-5060}"

python3 - <<PY
import socket, time, sys
host = "$IMS_HOST"
port = int("$IMS_PORT")
for _ in range(30):
    try:
        socket.create_connection((host, port), timeout=1).close()
        sys.exit(0)
    except OSError:
        time.sleep(1)
print(f"WARNING: IMS server {host}:{port} never became reachable", file=sys.stderr)
PY

# UEs to register (indexed from 1)
N="${REG_UES:-3}"
# Calls to make (pairs: UE 1 calls UE 2, UE 2 calls UE 3, ...)
CALLS="${MAKE_CALLS:-2}"

LOG_MODE="${LOG_MODE:-ENGINEER}"
{
  echo "MODE $LOG_MODE"
  echo "REG $N"
  i=1
  while [ "$i" -le "$CALLS" ]; do
    next=$((i + 1))
    echo "CALL $i $next"
    echo "BYE"
    i=$((i + 1))
  done
  echo "QUIT"
} | ./ue_sim

echo "=== UE sim done. Container stays alive for log inspection. ==="
exec sleep infinity

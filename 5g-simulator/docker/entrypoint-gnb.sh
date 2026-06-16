#!/bin/sh
# gnb_sim is an interactive CLI (reads "REG <n>" / "QUIT" from stdin).
# In a container we drive it with a scripted list of commands.
#
# AMF must be reachable first: Socket::connectTo() throws if connect()
# fails, which crashes gnb_sim immediately (see gnb_main.cpp) -- so we
# wait for AMF's N2 port before launching it.
AMF_HOST="${AMF_HOST:-127.0.0.1}"
python3 - <<PY
import socket, time, sys
host = "$AMF_HOST"
for _ in range(30):
    try:
        socket.create_connection((host, 38412), timeout=1).close()
        sys.exit(0)
    except OSError:
        time.sleep(1)
print(f"WARNING: AMF {host}:38412 never became reachable, starting anyway", file=sys.stderr)
PY

# REG_UES = how many UEs THIS gNB registers on startup (default 2).
# PDU_SESSIONS = if non-zero, each registered UE also establishes a
# PDU session after registration (triggers AMF→SMF→UPF PFCP flow).
# Scaling this Deployment to N replicas = N independent gNBs, each
# registering REG_UES UEs -- simplest "bulk registration" knob.
N="${REG_UES:-2}"
PDU="${PDU_SESSIONS:-1}"
{
  i=1
  while [ "$i" -le "$N" ]; do
    echo "REG $i"
    if [ "$PDU" -gt "0" ]; then
      echo "PDU $i 1 internet"
    fi
    i=$((i + 1))
  done
  echo "QUIT"
} | ./gnb_sim

# Keep the pod Running so `kubectl logs -f` stays attached and a
# Deployment doesn't restart-loop after this one-shot CLI run finishes.
echo "=== gNB done. Container stays alive for log inspection. ==="
exec sleep infinity

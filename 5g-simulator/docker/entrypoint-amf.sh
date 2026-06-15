#!/bin/sh
# AMF is a long-running N2 server (:38412) that also calls out to UDM
# over SBI. Run it in the foreground as PID 1 -- `docker stop` / pod
# termination then delivers SIGTERM directly to it.
#
# Wait for UDM's SBI port first: if the very first RegistrationRequest
# from a gNB arrives before UDM is reachable, amf_sim's callUdm() throws
# an *uncaught* exception and the whole process aborts (see amf_main.cpp,
# no try/catch around the SBI call) -- this avoids that race.
UDM_HOST="${UDM_HOST:-127.0.0.1}"
python3 - <<PY
import socket, time, sys
host = "$UDM_HOST"
for _ in range(30):
    try:
        socket.create_connection((host, 29503), timeout=1).close()
        sys.exit(0)
    except OSError:
        time.sleep(1)
print(f"WARNING: UDM {host}:29503 never became reachable, starting anyway", file=sys.stderr)
PY

# It writes 5g_capture.pcap to /app as UEs register. Grab it with:
#   docker cp <container>:/app/5g_capture.pcap .
#   kubectl cp <pod>:/app/5g_capture.pcap .
exec ./amf_sim

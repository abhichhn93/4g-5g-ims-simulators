# ── Stage 1: build IMS binaries ──────────────────────────────
# Multi-stage: compiler + cmake in build stage, slim runtime images.
# IMS architecture (TS 23.228):
#   P-CSCF: Proxy-CSCF — first SIP hop from UE (PDN gateway for SIP)
#   S-CSCF: Serving-CSCF — registration, session routing, invoking AS
#   HSS:    Home Subscriber Server — auth, profile, S-CSCF assignment
#   MTAS:   Multimedia Telephony Application Server — VoLTE call features
# In this sim: ims_server runs P-CSCF + S-CSCF + HSS in one process,
# ue_sim is the UE-side client (registers, makes calls, sends BYE).
FROM debian:bookworm AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends build-essential cmake && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j4

# ── IMS server (P-CSCF + S-CSCF + HSS + MTAS in one process) ─
# INTERVIEW: in real VoLTE, these are separate network nodes
# connected by Mw (SIP), Cx (Diameter over SCTP), and ISC (SIP AS)
# interfaces. This sim collapses them for portability.
FROM debian:bookworm-slim AS ims-server
WORKDIR /app
COPY --from=builder /src/build/ims_server ./ims_server
EXPOSE 5060/tcp   # SIP signalling (RFC 3261)
EXPOSE 5060/udp   # SIP over UDP (also used in real deployments)
EXPOSE 3868/tcp   # Diameter (Cx interface HSS ↔ S-CSCF)
CMD ["./ims_server"]

# ── UE simulator (SIP client: REGISTER + INVITE + BYE) ───────
# Driven by MODE + commands over stdin (same pattern as mme_sim / gnb_sim).
FROM python:3.12-slim-bookworm AS ue-sim
WORKDIR /app
COPY --from=builder /src/build/ue_sim ./ue_sim
COPY docker/entrypoint-ue.sh ./entrypoint.sh
RUN chmod +x entrypoint.sh
ENTRYPOINT ["./entrypoint.sh"]

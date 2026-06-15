# ── Stage 1: build all four 5G-core binaries ─────────────────
# Same pattern as mme-simulator/Dockerfile: a full Debian image with
# a compiler + cmake builds everything once; none of this stage is
# shipped in the final images.
FROM debian:bookworm AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends build-essential cmake && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j4

# ── Stage 2: NRF (SBI/HTTP server, port 29510 -- NF register/discover) ──
# Starts first: every other node registers with it on startup
# (common/nrf_client.h, ~10x1s retry), so ordering is self-healing.
FROM python:3.12-slim-bookworm AS nrf
WORKDIR /app
COPY --from=builder /src/build/nrf_sim ./nrf_sim
COPY docker/entrypoint-nrf.sh ./entrypoint.sh
RUN chmod +x entrypoint.sh
EXPOSE 29510
ENTRYPOINT ["./entrypoint.sh"]

# ── Stage 3: UDM (SBI/HTTP server, port 29503) ───────────────
FROM python:3.12-slim-bookworm AS udm
WORKDIR /app
COPY --from=builder /src/build/udm_sim ./udm_sim
COPY docker/entrypoint-udm.sh ./entrypoint.sh
RUN chmod +x entrypoint.sh
EXPOSE 29503
ENTRYPOINT ["./entrypoint.sh"]

# ── Stage 4: AMF (N2 server, port 38412 + SBI client to UDM) ──
# Only node that writes 5g_capture.pcap (retrieve with `kubectl cp`
# or `docker cp`, same as mme-simulator).
FROM python:3.12-slim-bookworm AS amf
WORKDIR /app
COPY --from=builder /src/build/amf_sim ./amf_sim
COPY docker/entrypoint-amf.sh ./entrypoint.sh
RUN chmod +x entrypoint.sh
EXPOSE 38412
ENTRYPOINT ["./entrypoint.sh"]

# ── Stage 5: gNB (interactive CLI -> driven non-interactively) ─
FROM python:3.12-slim-bookworm AS gnb
WORKDIR /app
COPY --from=builder /src/build/gnb_sim ./gnb_sim
COPY docker/entrypoint-gnb.sh ./entrypoint.sh
RUN chmod +x entrypoint.sh
ENTRYPOINT ["./entrypoint.sh"]

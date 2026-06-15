# ── Stage 1: build the C++ simulator ─────────────────────────
# Uses a full Debian image with a compiler + cmake. This stage is
# only used to produce the "mme_sim" binary — it is NOT shipped.
FROM debian:bookworm AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends build-essential cmake && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target mme_sim -j4

# ── Stage 2: slim runtime image ──────────────────────────────
# python3-slim is small, glibc-compatible with the builder, and gives
# us "python3 -m http.server" for free to serve the generated PCAP.
FROM python:3.12-slim-bookworm

WORKDIR /app
COPY --from=builder /src/build/mme_sim ./mme_sim
COPY docker/entrypoint.sh ./entrypoint.sh
RUN chmod +x entrypoint.sh

EXPOSE 8080
ENTRYPOINT ["./entrypoint.sh"]

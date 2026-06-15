#!/bin/sh
# NRF is the first node to start -- every other NF registers with IT,
# not the other way around, so it has no peer to wait for. Run it in
# the foreground as PID 1 so `docker stop` / pod termination delivers
# SIGTERM directly to it, and `docker logs -f` / `kubectl logs -f`
# stream its colored output live (logger.h flushes every line).
exec ./nrf_sim

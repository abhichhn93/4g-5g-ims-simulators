#!/bin/sh
# UDM is a long-running SBI/HTTP server (TS 29.503/29.509 on :29503).
# Run it in the foreground as PID 1 so `docker stop` / pod termination
# delivers SIGTERM directly to it, and `docker logs -f` / `kubectl logs -f`
# stream its colored output live (logger.h flushes every line).
exec ./udm_sim

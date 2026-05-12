# syntax=docker/dockerfile:1.7
#
# Multi-stage build for the D2 map server.
#
# Layout:
#   1. gobuild        — compiles the Linux mapserver binary
#   2. trielookupbuild — compiles the trielookup binary (portable 64-bit C++)
#   3. rendererbuild  — compiles the Vite TS client into static assets
#   4. runtime        — Debian + Wine + Xvfb; runs mapserver, which spawns
#                        `wine /app/bin/mapdump.exe` workers
#
# The C++ mapdump.exe itself is NOT built in Docker (it needs MSVC's inline
# asm for the naked-function shim in d2loader.cpp). Build it on Windows first;
# the Dockerfile picks up seedgen/build/Release/mapdump.exe from the context.
#
# The trielookup binary HAS no D2 dependency and builds cleanly on Linux —
# we compile it here so the deployed image is self-contained.
#
# The seed-lookup index (seedgen/level24.idx) is huge (~40 GB at the full
# 2.1 B seed space, 20 bytes/record) and is NOT baked into the image. It is
# mounted at runtime alongside /game — see docker-entrypoint.sh.
#
# The D2 1.13c install directory is a runtime mount at /game (read-only is
# fine). Do NOT bake game files into the image.

# ---------------------------------------------------------------------------
# Stage 1 — Go server
# ---------------------------------------------------------------------------
FROM golang:1.26-alpine AS gobuild
WORKDIR /src
COPY mapserver/go.mod mapserver/go.sum ./
COPY mapserver/*.go ./
ENV CGO_ENABLED=0
RUN go build -trimpath -ldflags="-s -w" -o /out/mapserver .

# ---------------------------------------------------------------------------
# Stage 2 — trielookup (portable 64-bit C++ binary)
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS trielookupbuild
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        g++ cmake ninja-build && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY seedgen/src/             /src/seedgen/src/
COPY seedgen/trielookup/      /src/seedgen/trielookup/
RUN cmake -S /src/seedgen/trielookup -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /build --target trielookup

# ---------------------------------------------------------------------------
# Stage 3 — Vite renderer
# ---------------------------------------------------------------------------
FROM node:22-alpine AS rendererbuild
WORKDIR /src
COPY renderer/package.json renderer/package-lock.json ./
RUN npm ci
COPY renderer/ ./
RUN npm run build

# ---------------------------------------------------------------------------
# Stage 4 — Wine runtime
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    WINEPREFIX=/root/.wine \
    WINEARCH=win32 \
    WINEDEBUG=-all \
    DISPLAY=:99

# Wine for 32-bit Windows binaries requires i386 multiarch.
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        wine wine32 xvfb tini ca-certificates procps curl && \
    rm -rf /var/lib/apt/lists/*

# Pre-create the Wine prefix at build time so the first /api/render call
# doesn't pay the 10-20s wineboot cost.
RUN set -e; \
    Xvfb :99 -screen 0 320x240x16 -nolisten tcp & XVFB_PID=$!; \
    sleep 1; \
    DISPLAY=:99 wineboot --init || true; \
    wineserver -k 2>/dev/null || true; \
    kill $XVFB_PID 2>/dev/null || true; \
    rm -f /tmp/.X99-lock /tmp/.X11-unix/X99

WORKDIR /app
COPY --from=gobuild         /out/mapserver                  /app/mapserver
COPY --from=trielookupbuild /build/trielookup               /app/bin/trielookup
COPY --from=rendererbuild   /src/dist                       /app/static
COPY seedgen/build/Release/mapdump.exe                      /app/bin/mapdump.exe
COPY docker-entrypoint.sh                                   /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh /app/bin/trielookup

# Mount the D2 1.13c install (D2Common.dll, MPQs, etc.) here.
# Mount the lookup index (seedgen/level24.idx, ~11 GB) at /index.
VOLUME ["/game", "/index"]
EXPOSE 8080

# Visibility only: marks the container unhealthy in `docker ps`. The actual
# restart-on-failure is handled by the watchdog inside docker-entrypoint.sh
# (Docker standalone won't auto-restart unhealthy containers).
HEALTHCHECK --interval=30s --timeout=5s --start-period=60s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/healthz >/dev/null || exit 1

# tini reaps the wine zombies and forwards signals to mapserver.
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/docker-entrypoint.sh"]
CMD ["--addr", ":8080", "--game", "/game"]

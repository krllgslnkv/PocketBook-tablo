#!/usr/bin/env bash
# ==============================================================================
# Build & Package Script for Tablo PocketBook 625 (Clean Autonomous Port)
# ==============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "==> 1. Compiling C++ Static ARM Renderer (800x600 for PocketBook)..."
docker run --rm -v "$ROOT":/workspace -w /workspace tablo-builder \
    arm-linux-gnueabi-g++ -static -std=gnu++17 -O2 -DNATIVE_BUILD \
    -Ifirmware/src -Ifirmware/test/test_config \
    -I.pio/libdeps/native/QRCode/src -I.pio/libdeps/native/ArduinoJson/src \
    pocketbook/tablo_render.cpp \
    firmware/src/layout.cpp firmware/src/canvas.cpp firmware/src/canvas_mem.cpp \
    firmware/src/font.cpp firmware/src/i18n.cpp firmware/src/slots.cpp \
    firmware/src/wifi_qr.cpp firmware/src/widgets/types.cpp \
    firmware/src/widgets/prims.cpp firmware/src/widgets/w_markets.cpp \
    firmware/src/widgets/w_limits.cpp firmware/src/widgets/w_air.cpp \
    firmware/src/widgets/w_limits_air.cpp firmware/src/widgets/w_mail.cpp \
    firmware/src/widgets/w_today.cpp firmware/src/widgets/w_metric.cpp \
    firmware/src/widgets/w_text.cpp firmware/src/widgets/registry.cpp \
    .pio/libdeps/native/QRCode/src/qrcode.c \
    -o pocketbook/tablo_render_arm

echo "==> 2. Compiling Go Static ARM Daemon (Web Portal + Connectors)..."
docker run --rm -v "$ROOT":/workspace -w /workspace/pocketbook/daemon golang:alpine \
    sh -c 'CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build -ldflags="-s -w" -o /workspace/pocketbook/tablo_daemon_arm main.go'

echo "==> 3. Creating distribution package in dist_pb/..."
DIST="$ROOT/dist_pb"
rm -rf "$DIST"
mkdir -p "$DIST/applications/.tablo/bin"
mkdir -p "$DIST/applications/.tablo/data"
mkdir -p "$DIST/Photo"

cp "$ROOT/pocketbook/tablo_render_arm" "$DIST/applications/.tablo/bin/tablo_render"
cp "$ROOT/pocketbook/tablo_daemon_arm" "$DIST/applications/.tablo/bin/tablo_daemon"
chmod +x "$DIST/applications/.tablo/bin/"*

cp "$ROOT/firmware/data/index.html" "$DIST/applications/.tablo/data/index.html"
cp "$ROOT/pocketbook/default_config.json" "$DIST/applications/.tablo/config.json"
cp "$ROOT/pocketbook/default_config.json" "$DIST/tablo_config.json"

echo "==> 4. Generating pre-rendered boot / live image..."
docker run --rm -v "$ROOT":/workspace -w /workspace tablo-builder \
    qemu-arm-static ./pocketbook/tablo_render_arm \
    -config pocketbook/default_config.json \
    -setup \
    -setup-url "http://pbtablo-setup.local:8080" \
    -out dist_pb/Photo/tablo_live.png \
    -rot right

cp "$DIST/Photo/tablo_live.png" "$DIST/applications/.tablo/tablo_live.png"

echo "==> 5. Creating PocketBook Tablo.app launcher script..."
cat << 'EOF' > "$DIST/applications/Tablo.app"
#!/bin/sh
# ==============================================================================
# Tablo for PocketBook 625 — Standalone Autonomous Edition with Web Setup Portal
# ==============================================================================

TABLO_DIR="/mnt/ext1/applications/.tablo"
CONFIG="/mnt/ext1/tablo_config.json"
if [ ! -f "$CONFIG" ]; then
    CONFIG="$TABLO_DIR/config.json"
fi

IMG="/mnt/ext1/Photo/tablo_live.png"
LOG="/tmp/tablo.log"

echo "=== TABLO LAUNCH $(date) ===" > "$LOG"

# 1. Ensure Photo directory exists
mkdir -p /mnt/ext1/Photo
if [ ! -s "$IMG" ] && [ -s "$TABLO_DIR/tablo_live.png" ]; then
    cp "$TABLO_DIR/tablo_live.png" "$IMG"
fi

# 2. Stop any existing background daemon before starting fresh
killall -9 tablo_daemon 2>/dev/null || true

# 3. Ensure Wi-Fi network connection
if [ -x /ebrmain/bin/netconnect ]; then
    /ebrmain/bin/netconnect >> "$LOG" 2>&1
fi

# 4. Start autonomous background daemon with Web Setup Portal on port 8080
"$TABLO_DIR/bin/tablo_daemon" \
    -port 8080 \
    -config "$CONFIG" \
    -html "$TABLO_DIR/data/index.html" \
    -renderer "$TABLO_DIR/bin/tablo_render" \
    -img "$IMG" \
    -rot right >> "$LOG" 2>&1 &

# 5. Open image viewer immediately (foreground process for PocketBook launcher)
if [ -s "$IMG" ]; then
    killall picviewer.app 2>/dev/null || true
    usleep 200000
    /ebrmain/bin/openbook "$IMG" 2>> "$LOG"
fi
EOF

chmod +x "$DIST/applications/Tablo.app"

echo "==> Packaging complete! Package structure:"
ls -lhR "$DIST"

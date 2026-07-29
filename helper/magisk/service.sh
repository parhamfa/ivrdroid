#!/system/bin/sh

export PATH="/debug_ramdisk/.magisk/busybox:$PATH"

MODDIR=${0%/*}
STATE_DIR=/data/adb/ivrdroid
BRIDGE_DIR=/data/user/0/ai.rx1.ivrdroid/files/bridge
SERVICE_PID_FILE="$STATE_DIR/service.pid"
HELPER_PID_FILE="$STATE_DIR/helper.pid"
ACTIVE_HELPER_PID=

mkdir -p "$STATE_DIR"
chmod 0700 "$STATE_DIR"

if [ -f "$SERVICE_PID_FILE" ]; then
    OLD_PID=$(head -n 1 "$SERVICE_PID_FILE")
    if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
        exit 0
    fi
fi

echo $$ >"$SERVICE_PID_FILE"
chmod 0600 "$SERVICE_PID_FILE"

stop_service() {
    if [ -n "$ACTIVE_HELPER_PID" ]; then
        kill -TERM "$ACTIVE_HELPER_PID" 2>/dev/null
        wait "$ACTIVE_HELPER_PID" 2>/dev/null
    elif [ -f "$HELPER_PID_FILE" ]; then
        HELPER_PID=$(head -n 1 "$HELPER_PID_FILE")
        if [ -n "$HELPER_PID" ]; then
            kill -TERM "$HELPER_PID" 2>/dev/null
        fi
    fi
    rm -f "$SERVICE_PID_FILE"
    exit 0
}

trap stop_service HUP INT TERM

while true; do
    if [ -d "$BRIDGE_DIR" ]; then
        "$MODDIR/bin/ivrdroid-helper" --serve &
        ACTIVE_HELPER_PID=$!
        wait "$ACTIVE_HELPER_PID"
        ACTIVE_HELPER_PID=
    fi
    sleep 2
done

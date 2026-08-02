#!/system/bin/sh

export PATH="/debug_ramdisk/.magisk/busybox:$PATH"

MODDIR=${0%/*}
STATE_DIR=/data/adb/ivrdroid
BRIDGE_DIR=/data/user/0/ai.rx1.ivrdroid/files/bridge
SERVICE_PID_FILE="$STATE_DIR/service.pid"
HELPER_PID_FILE="$STATE_DIR/helper.pid"
MODULE_DISABLE_FILE="$MODDIR/disable"
HELPER_BINARY="$MODDIR/bin/ivrdroid-helper"
ACTIVE_HELPER_PID=
SHORT_FAILURES=0
MAX_SHORT_FAILURES=7
HEALTHY_RUNTIME_SECONDS=60

mkdir -p "$STATE_DIR"
chmod 0700 "$STATE_DIR"

service_log() {
    log -t IVRdroidService "$*" 2>/dev/null
}

is_ivrdroid_process() {
    CANDIDATE_PID=$1
    [ -n "$CANDIDATE_PID" ] || return 1
    [ -r "/proc/$CANDIDATE_PID/cmdline" ] || return 1
    CANDIDATE_COMMAND=$(tr '\000' ' ' <"/proc/$CANDIDATE_PID/cmdline")
    case "$CANDIDATE_COMMAND" in
        *ivrdroid_helper/service.sh*|*ivrdroid-helper*--serve*)
            return 0
            ;;
    esac
    return 1
}

if [ -f "$SERVICE_PID_FILE" ]; then
    OLD_PID=$(head -n 1 "$SERVICE_PID_FILE")
    if is_ivrdroid_process "$OLD_PID" && kill -0 "$OLD_PID" 2>/dev/null; then
        exit 0
    fi
    rm -f "$SERVICE_PID_FILE"
fi

echo $$ >"$SERVICE_PID_FILE"
chmod 0600 "$SERVICE_PID_FILE"

stop_service() {
    if [ -n "$ACTIVE_HELPER_PID" ]; then
        kill -TERM "$ACTIVE_HELPER_PID" 2>/dev/null
        wait "$ACTIVE_HELPER_PID" 2>/dev/null
    elif [ -f "$HELPER_PID_FILE" ]; then
        HELPER_PID=$(head -n 1 "$HELPER_PID_FILE")
        if is_ivrdroid_process "$HELPER_PID"; then
            kill -TERM "$HELPER_PID" 2>/dev/null
        fi
    fi
    rm -f "$SERVICE_PID_FILE"
    exit 0
}

trap stop_service HUP INT TERM

disable_crash_loop() {
    : >"$MODULE_DISABLE_FILE"
    chmod 0644 "$MODULE_DISABLE_FILE"
    service_log \
        "Disabled IVRdroid after $SHORT_FAILURES consecutive startup failures."
    rm -f "$SERVICE_PID_FILE"
    exit 1
}

failure_backoff() {
    case "$SHORT_FAILURES" in
        1) BACKOFF_SECONDS=2 ;;
        2) BACKOFF_SECONDS=4 ;;
        3) BACKOFF_SECONDS=8 ;;
        4) BACKOFF_SECONDS=16 ;;
        5) BACKOFF_SECONDS=30 ;;
        *) BACKOFF_SECONDS=60 ;;
    esac
    sleep "$BACKOFF_SECONDS"
}

while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 2
done

while true; do
    if [ ! -d "$BRIDGE_DIR" ]; then
        sleep 2
        continue
    fi

    if ! "$HELPER_BINARY" --self-test; then
        SHORT_FAILURES=$((SHORT_FAILURES + 1))
        service_log \
            "IVRdroid self-test failed ($SHORT_FAILURES/$MAX_SHORT_FAILURES)."
        if [ "$SHORT_FAILURES" -ge "$MAX_SHORT_FAILURES" ]; then
            disable_crash_loop
        fi
        failure_backoff
        continue
    fi

    STARTED_AT=$(date +%s)
    "$HELPER_BINARY" --serve &
    ACTIVE_HELPER_PID=$!
    wait "$ACTIVE_HELPER_PID"
    HELPER_EXIT=$?
    ACTIVE_HELPER_PID=
    ENDED_AT=$(date +%s)
    HELPER_RUNTIME=$((ENDED_AT - STARTED_AT))

    if [ "$HELPER_RUNTIME" -ge "$HEALTHY_RUNTIME_SECONDS" ]; then
        SHORT_FAILURES=0
        service_log \
            "IVRdroid helper exited after a healthy ${HELPER_RUNTIME}s run; restarting."
        sleep 2
        continue
    fi

    SHORT_FAILURES=$((SHORT_FAILURES + 1))
    service_log \
        "IVRdroid helper exited code=$HELPER_EXIT after ${HELPER_RUNTIME}s " \
        "($SHORT_FAILURES/$MAX_SHORT_FAILURES)."
    if [ "$SHORT_FAILURES" -ge "$MAX_SHORT_FAILURES" ]; then
        disable_crash_loop
    fi
    failure_backoff
done

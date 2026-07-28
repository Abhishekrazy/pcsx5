#!/usr/bin/env bash
# I3.1 + B1.3: Headless crash-detect loop with frame-timing logging.
#
# Usage:
#   ./tools/run_bot_test.sh [--long-test] [--play-input=<path>] <eboot_or_dir> [title_id]
#
# When <eboot_or_dir> is a directory, the script looks for eboot.bin (or
# eboot.elf) inside it and auto-detects the title_id from sce_sys/param.json.
#
# Options:
#   --long-test          Use extended timeout (300s) for menu rendering tests.
#   --play-input=<path>  Controller replay JSON file.
#
# Exit codes:
#   0 — guest exited cleanly (replay finished or game shut down)
#   1 — crash detected (non-zero exit or crash signature in log)
#   2 — hang detected (no flip for HANG_TIMEOUT seconds)
#   3 — other error

set -euo pipefail

# ---- defaults ---------------------------------------------------------------
HANG_TIMEOUT=120
LONG_TEST=false
EBOOT=""
TITLE_ID=""
REPLAY=""
CLI="./dist/pcsx5_cli.exe"
LOG_FILE=""
REPORT_FILE=""
ARGS=()

# ---- argument parsing -------------------------------------------------------
# Parse flags first, then positional args (backward-compatible).
while [ $# -gt 0 ]; do
    case "$1" in
        --long-test)
            LONG_TEST=true
            shift
            ;;
        --play-input=*)
            REPLAY="${1#*=}"
            shift
            ;;
        --play-input)
            REPLAY="${2?missing replay path}"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--long-test] [--play-input=<path>] <eboot_or_dir> [title_id]"
            echo ""
            echo "Options:"
            echo "  --long-test          Extended timeout (300s) for menu rendering tests."
            echo "  --play-input=<path>  Controller replay JSON file."
            exit 0
            ;;
        --*)
            echo "ERROR: unknown flag: $1" >&2
            exit 3
            ;;
        *)
            if [ -z "$EBOOT" ]; then
                EBOOT="$1"
            elif [ -z "$TITLE_ID" ]; then
                TITLE_ID="$1"
            elif [ -z "$REPLAY" ]; then
                REPLAY="$1"
            else
                echo "ERROR: unexpected argument: $1" >&2
                exit 3
            fi
            shift
            ;;
    esac
done

if [ -z "$EBOOT" ]; then
    echo "ERROR: usage: $0 [--long-test] [--play-input=<path>] <eboot_or_dir> [title_id]" >&2
    exit 3
fi

# ---- directory-as-eboot support ---------------------------------------------
if [ -d "$EBOOT" ]; then
    if [ -f "$EBOOT/eboot.bin" ]; then
        EBOOT="$EBOOT/eboot.bin"
    elif [ -f "$EBOOT/eboot.elf" ]; then
        EBOOT="$EBOOT/eboot.elf"
    else
        echo "ERROR: directory '$EBOOT' does not contain eboot.bin or eboot.elf" >&2
        exit 3
    fi
    # Auto-detect title_id from sce_sys/param.json.
    if [ -z "$TITLE_ID" ]; then
        PARAM_JSON="$(dirname "$EBOOT")/sce_sys/param.json"
        if [ -f "$PARAM_JSON" ]; then
            TITLE_ID=$(grep -o '"titleId"[[:space:]]*:[[:space:]]*"[^"]*"' "$PARAM_JSON" \
                       | head -1 | sed 's/.*"titleId"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
            echo "  Auto-detected title_id: $TITLE_ID"
        fi
    fi
fi

# ---- long-test timeout ------------------------------------------------------
if [ "$LONG_TEST" = true ]; then
    HANG_TIMEOUT=300
    echo "  Long-test mode: timeout=${HANG_TIMEOUT}s"
fi

# ---- locate CLI -------------------------------------------------------------
if [ ! -x "$CLI" ]; then
    CLI="$(dirname "$0")/../dist/pcsx5_cli.exe"
fi
if [ ! -x "$CLI" ]; then
    echo "ERROR: cannot find pcsx5_cli.exe" >&2
    exit 3
fi

# ---- temp files -------------------------------------------------------------
LOG_FILE="/tmp/pcsx5_bot_$$.log"
REPORT_FILE="/tmp/pcsx5_report_$$.json"

ARGS+=(--headless "--log-file=$LOG_FILE" "--report=$REPORT_FILE")

if [ -n "$TITLE_ID" ]; then
    ARGS+=(--title-id="$TITLE_ID")
fi

if [ -n "$REPLAY" ]; then
    if [ ! -f "$REPLAY" ]; then
        echo "ERROR: replay file not found: $REPLAY" >&2
        exit 3
    fi
    ARGS+=(--play-input="$REPLAY")
    echo "  Replay: $REPLAY"
fi

echo "  CLI:     $CLI"
echo "  EBOOT:   $EBOOT"
echo "  TitleID: ${TITLE_ID:-none}"
echo "  Log:     $LOG_FILE"
echo "  Report:  $REPORT_FILE"
echo ""

# ---- run emulator -----------------------------------------------------------
set +e
START_MS=$(date +%s%3N)
timeout --kill-after=15 "$HANG_TIMEOUT" "$CLI" "${ARGS[@]}" "$EBOOT" &
CLI_PID=$!

# ---- background monitor (GPU output sampling + frame age detection) ---------
MONITOR_PID=
monitor_cleanup() {
    if [ -n "$MONITOR_PID" ]; then
        kill "$MONITOR_PID" 2>/dev/null || true
        wait "$MONITOR_PID" 2>/dev/null || true
    fi
    # Ensure child process is reaped.
    wait "$CLI_PID" 2>/dev/null || true
}
# Run monitor only for long-test or when replay is active.
if [ "$LONG_TEST" = true ] || [ -n "$REPLAY" ]; then
    (
        # Sample GPU output / frame timing every 10 seconds.
        while true; do
            sleep 10
            # Check if parent script is still alive.
            if ! kill -0 "$PPID" 2>/dev/null; then
                exit 0
            fi
            if [ -f "$LOG_FILE" ]; then
                LOG_SIZE=$(wc -c < "$LOG_FILE" 2>/dev/null || echo 0)
                # Sample the most recent [FrameTiming] log line.
                FRAME_LINE=$(grep "\[FrameTiming\]" "$LOG_FILE" 2>/dev/null | tail -1 || true)
                if [ -n "$FRAME_LINE" ]; then
                    echo "  [Monitor] Log: ${LOG_SIZE}B | $FRAME_LINE"
                else
                    echo "  [Monitor] Log: ${LOG_SIZE}B | (no frame timing yet)"
                fi
            fi
        done
    ) &
    MONITOR_PID=$!
fi

# Wait for the emulator process to finish.
wait "$CLI_PID"
EXIT_CODE=$?
END_MS=$(date +%s%3N)
DURATION_MS=$((END_MS - START_MS))

# Clean up monitor.
monitor_cleanup

set -e

echo ""
echo "=== RESULT ==="
echo "  Exit code: $EXIT_CODE"
echo "  Duration:  ${DURATION_MS}ms"

# ---- frame age / stall detection --------------------------------------------
LAST_FRAME=$(grep "\[FrameTiming\]" "$LOG_FILE" 2>/dev/null | tail -1 || true)
if [ -n "$LAST_FRAME" ]; then
    echo "  Last frame timing: $LAST_FRAME"
    # Check for zero-FPS stall.
    if echo "$LAST_FRAME" | grep -q "FPS: 0\.0"; then
        echo "  WARNING: Zero FPS detected — frame pipeline stalled"
    fi
fi

# ---- crash signature detection ----------------------------------------------
CRASH_DETECTED=false
if [ "$EXIT_CODE" -ne 0 ]; then
    CRASH_DETECTED=true
    echo "  Status:    CRASH (exit code $EXIT_CODE)"
fi

if grep -q "VEH Unhandled Exception\|GUEST APPLICATION CRASHED\|Unimplemented stub" "$LOG_FILE" 2>/dev/null; then
    if [ "$CRASH_DETECTED" = false ]; then
        echo "  Status:    CRASH (crash signature in log)"
    fi
    CRASH_DETECTED=true
fi

# ---- hang detection ---------------------------------------------------------
if [ "$EXIT_CODE" -eq 124 ] || [ "$EXIT_CODE" -eq 137 ]; then
    echo "  Status:    HANG (no flip within ${HANG_TIMEOUT}s)"
    CRASH_DETECTED=true
    EXIT_CODE=2
fi

# ---- outcome ----------------------------------------------------------------
if [ "$CRASH_DETECTED" = false ]; then
    echo "  Status:    CLEAN EXIT"
    rm -f "$LOG_FILE" "$REPORT_FILE"
    exit 0
else
    BUNDLE_DIR="pcsx5_crash/$(date -u +%Y%m%d_%H%M%S)_${TITLE_ID:-unknown}"
    mkdir -p "$BUNDLE_DIR"
    [ -f "$LOG_FILE" ]    && cp "$LOG_FILE"    "$BUNDLE_DIR/bot_log.txt"
    [ -f "$REPORT_FILE" ] && cp "$REPORT_FILE" "$BUNDLE_DIR/compat_report.json"
    echo "  Bundle:    $BUNDLE_DIR"
    exit "$EXIT_CODE"
fi

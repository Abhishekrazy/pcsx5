#!/usr/bin/env bash
# I2.1: Headless crash-detect loop
#
# Usage:  ./tools/run_bot_test.sh <eboot_path> [title_id] [replay_path]
#
# Boots the game in headless mode, optionally with a controller replay,
# waits for exit or timeout, and detects crashes/hangs.  Saves the
# crash bundle (log + compat report) on failure.
#
# Exit codes:
#   0 — guest exited cleanly (replay finished or game shut down)
#   1 — crash detected (non-zero exit or crash signature in log)
#   2 — hang detected (no flip for HANG_TIMEOUT seconds)
#   3 — other error

set -euo pipefail

EBOOT="${1:?usage: $0 <eboot_path> [title_id] [replay_path]}"
TITLE_ID="${2:-}"
REPLAY="${3:-}"
HANG_TIMEOUT=120           # seconds without a flip before declaring hang
CLI="./dist/pcsx5_cli.exe"
LOG_FILE="/tmp/pcsx5_bot_$$.log"
REPORT_FILE="/tmp/pcsx5_report_$$.json"
ARGS=()

# Locate CLI
if [ ! -x "$CLI" ]; then
    CLI="$(dirname "$0")/../dist/pcsx5_cli.exe"
fi
if [ ! -x "$CLI" ]; then
    echo "ERROR: cannot find pcsx5_cli.exe" >&2
    exit 3
fi

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

# Run the emulator with a timeout (hang protection).
# timeout sends SIGTERM, which the CLI should handle gracefully.
set +e
START_MS=$(date +%s%3N)
timeout --kill-after=15 "$HANG_TIMEOUT" "$CLI" "${ARGS[@]}" "$EBOOT" &
CLI_PID=$!
wait "$CLI_PID"
EXIT_CODE=$?
END_MS=$(date +%s%3N)
DURATION_MS=$((END_MS - START_MS))
set -e

echo ""
echo "=== RESULT ==="
echo "  Exit code: $EXIT_CODE"
echo "  Duration:  ${DURATION_MS}ms"

# Check for known crash signatures in the log.
CRASH_DETECTED=false
if [ "$EXIT_CODE" -ne 0 ]; then
    # Non-zero exit (including signal termination from timeout).
    CRASH_DETECTED=true
    echo "  Status:    CRASH (exit code $EXIT_CODE)"
fi

if grep -q "VEH Unhandled Exception\|GUEST APPLICATION CRASHED\|Unimplemented stub" "$LOG_FILE" 2>/dev/null; then
    if [ "$CRASH_DETECTED" = false ]; then
        echo "  Status:    CRASH (crash signature in log)"
    fi
    CRASH_DETECTED=true
fi

# Check for hang (timeout killed us = no flips for $HANG_TIMEOUT seconds).
if [ "$EXIT_CODE" -eq 124 ] || [ "$EXIT_CODE" -eq 137 ]; then
    echo "  Status:    HANG (no flip within ${HANG_TIMEOUT}s)"
    CRASH_DETECTED=true
    EXIT_CODE=2
fi

if [ "$CRASH_DETECTED" = false ]; then
    echo "  Status:    CLEAN EXIT"
    # Clean up logs on success.
    rm -f "$LOG_FILE" "$REPORT_FILE"
    exit 0
else
    # Save crash bundle.
    BUNDLE_DIR="pcsx5_crash/$(date -u +%Y%m%d_%H%M%S)_${TITLE_ID:-unknown}"
    mkdir -p "$BUNDLE_DIR"
    [ -f "$LOG_FILE" ]    && cp "$LOG_FILE"    "$BUNDLE_DIR/bot_log.txt"
    [ -f "$REPORT_FILE" ] && cp "$REPORT_FILE" "$BUNDLE_DIR/compat_report.json"
    echo "  Bundle:    $BUNDLE_DIR"
    exit "$EXIT_CODE"
fi

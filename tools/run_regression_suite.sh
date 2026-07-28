#!/usr/bin/env bash
# I2.2: Regression test suite — runs bot replay inputs for each title and
# tracks pass/fail history in a JSON manifest.
#
# Usage:
#   ./tools/run_regression_suite.sh [--replay-dir=<dir>] [--games-dir=<dir>]
#                                   [--manifest=<path>] [--cli=<path>]
#                                   [--update-manifest]
#
# Defaults:
#   --replay-dir     ./replays/
#   --games-dir      ./Games/
#   --manifest       ./tests/regression_manifest.json
#   --cli            (auto-detected by run_bot_test.sh)
#   --update-manifest  Persist results to the manifest JSON (off by default;
#                       pass --update-manifest to enable).
#
# Replay files are expected to be named <title_id>_<description>.json (e.g.
# CUSA12345_menu.json).  The title_id is extracted from the filename and used
# to locate the game directory under <games-dir>/<title_id>-app0/.
#
# Each replay is run through run_bot_test.sh with the corresponding game
# directory.  Results are collected and optionally persisted to the manifest.
#
# Exit codes:
#   0 — all tests passed (or no replays found)
#   1 — one or more tests failed
#   2 — configuration error

set -euo pipefail

# ---- defaults ---------------------------------------------------------------
REPLAY_DIR="./replays"
GAMES_DIR="./Games"
MANIFEST="./tests/regression_manifest.json"
CLI=""
UPDATE_MANIFEST=false
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOT_SCRIPT="${SCRIPT_DIR}/run_bot_test.sh"

# ---- arg parsing ------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --replay-dir=*)    REPLAY_DIR="${1#*=}"; shift ;;
        --games-dir=*)     GAMES_DIR="${1#*=}"; shift ;;
        --manifest=*)      MANIFEST="${1#*=}"; shift ;;
        --cli=*)           CLI="${1#*=}"; shift ;;
        --update-manifest) UPDATE_MANIFEST=true; shift ;;
        -h|--help)
            echo "Usage: $0 [--replay-dir=<dir>] [--games-dir=<dir>] [--manifest=<path>] [--cli=<path>] [--update-manifest]"
            echo ""
            echo "Scans <replay-dir> for *.json replay files and runs each through"
            echo "run_bot_test.sh against the matching game under <games-dir>."
            exit 0
            ;;
        *) echo "ERROR: unknown arg: $1" >&2; exit 2 ;;
    esac
done

# ---- validate dependencies --------------------------------------------------
if [ ! -x "$BOT_SCRIPT" ]; then
    echo "ERROR: run_bot_test.sh not found or not executable: $BOT_SCRIPT" >&2
    exit 2
fi

# ---- locate CLI (if provided, make available) --------------------------------
if [ -n "$CLI" ]; then
    # Export so run_bot_test.sh can find it (its internal lookup checks fall
    # back to the original default; we place the CLI directory on PATH).
    if [ -f "$CLI" ]; then
        CLI_DIR="$(cd "$(dirname "$CLI")" && pwd)"
        export PATH="${CLI_DIR}:${PATH}"
        echo "  CLI path set: $CLI"
    else
        echo "WARNING: --cli path does not exist, will rely on auto-detect: $CLI" >&2
    fi
fi

# ---- gather replays ---------------------------------------------------------
if [ ! -d "$REPLAY_DIR" ]; then
    echo "Replay directory not found: $REPLAY_DIR"
    echo "Creating empty manifest."
    mkdir -p "$REPLAY_DIR"
fi

REPLAY_FILES=()
while IFS= read -r -d '' f; do
    REPLAY_FILES+=("$f")
done < <(find "$REPLAY_DIR" -name '*.json' -print0 2>/dev/null || true)

if [ ${#REPLAY_FILES[@]} -eq 0 ]; then
    echo "No replay files found in $REPLAY_DIR"
    mkdir -p "$(dirname "$MANIFEST")"
    cat > "$MANIFEST" <<MANIFEST_EOF
{
    "version": 1,
    "schema": "regression-manifest",
    "last_run": null,
    "git_revision": null,
    "summary": {
        "passed": 0,
        "failed": 0,
        "total": 0
    },
    "runs": []
}
MANIFEST_EOF
    echo "Manifest written: $MANIFEST"
    exit 0
fi

echo "=== Regression Test Suite ==="
echo "  Replays:   ${#REPLAY_FILES[@]} found in ${REPLAY_DIR}"
echo "  Games dir: ${GAMES_DIR}"
echo "  Manifest:  ${MANIFEST}"
echo ""

# ---- results tracking -------------------------------------------------------
PASSED=0
FAILED=0
SKIPPED=0
RESULTS=()

RUN_TIMESTAMP=$(date -u +%Y-%m-%dT%H:%M:%SZ)
GIT_REV=""
if GIT_REV=$(git rev-parse --short HEAD 2>/dev/null); then
    :  # GIT_REV already set
else
    GIT_REV="unknown"
fi

# ---- helpers ----------------------------------------------------------------

# Locate the game directory for a given title ID.
# Checks several directory naming conventions in order:
#   1. <title_id>-app0/
#   2. <title_id>-app01/
#   3. <title_id>/  (exact match)
# Returns 0 and prints the directory path on success, 1 on failure.
find_game_dir() {
    local tid="$1"
    local base="${GAMES_DIR}"
    for suffix in "-app0" "-app01" ""; do
        local candidate="${base}/${tid}${suffix}"
        if [ -d "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    # Case-insensitive fallback: search for any subdirectory containing the
    # title ID as a prefix on platforms where the filesystem is case-insensitive.
    # (NTFS is case-insensitive by default, so this loop covers many variants.)
    if [ -d "$base" ]; then
        for d in "$base"/*/; do
            local bd
            bd="$(basename "$d")"
            # Match "<title_id>" or "<title_id>-*" at the start.
            case "$bd" in
                "${tid}"|"${tid}-"*) echo "$d"; return 0 ;;
            esac
        done
    fi
    return 1
}

# ---- run each replay --------------------------------------------------------
for replay in "${REPLAY_FILES[@]}"; do
    REPLAY_NAME=$(basename "$replay" .json)
    echo "--- Running: ${REPLAY_NAME} ---"

    # Parse title_id from filename (first segment before first underscore).
    TITLE_ID="${REPLAY_NAME%%_*}"
    # If the whole filename has no underscore, use it as-is.
    if [ "$TITLE_ID" = "$REPLAY_NAME" ]; then
        TITLE_ID=""
    fi

    if [ -z "$TITLE_ID" ]; then
        echo "  SKIP: cannot parse title_id from filename '${REPLAY_NAME}'"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    # Locate the game directory.
    GAME_DIR=""
    if ! GAME_DIR=$(find_game_dir "$TITLE_ID"); then
        echo "  SKIP: no game directory found for title '${TITLE_ID}' under ${GAMES_DIR}"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    # Verify the game directory has an executable.
    if [ ! -f "${GAME_DIR}/eboot.bin" ] && [ ! -f "${GAME_DIR}/eboot.elf" ]; then
        echo "  SKIP: '${GAME_DIR}' has no eboot.bin or eboot.elf"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    # Launch run_bot_test.sh with the replay and game directory.
    set +e
    START_MS=$(date +%s%3N)
    "${BOT_SCRIPT}" --play-input="$replay" "${GAME_DIR}" "${TITLE_ID}" 2>&1
    BOT_EXIT=$?
    END_MS=$(date +%s%3N)
    DURATION_MS=$((END_MS - START_MS))
    set -e

    if [ "$BOT_EXIT" -eq 0 ]; then
        echo "  -> PASSED (${DURATION_MS}ms)"
        PASSED=$((PASSED + 1))
    else
        echo "  -> FAILED (exit=${BOT_EXIT}, ${DURATION_MS}ms)"
        FAILED=$((FAILED + 1))
    fi

    RESULTS+=(
        "{\"title_id\":\"${TITLE_ID}\",\"replay\":\"${REPLAY_NAME}\",\"exit_code\":${BOT_EXIT},\"duration_ms\":${DURATION_MS},\"timestamp\":\"${RUN_TIMESTAMP}\",\"git_rev\":\"${GIT_REV}\"}"
    )
done

# ---- print summary ----------------------------------------------------------
echo ""
echo "=== Summary ==="
echo "  Passed:  ${PASSED}"
echo "  Failed:  ${FAILED}"
echo "  Skipped: ${SKIPPED}"
echo "  Total:   $((PASSED + FAILED + SKIPPED))"

# ---- update manifest --------------------------------------------------------
if [ "$UPDATE_MANIFEST" = true ]; then
    MANIFEST_DIR="$(dirname "$MANIFEST")"
    mkdir -p "$MANIFEST_DIR"

    # Read existing runs from the manifest if it exists.
    EXISTING_RUNS="[]"
    if [ -f "$MANIFEST" ]; then
        if command -v jq &>/dev/null; then
            EXISTING_RUNS=$(jq '.runs // []' "$MANIFEST" 2>/dev/null || echo "[]")
        fi
    fi

    # Build the new runs array as a JSON string (no jq needed for the merge).
    RUNS_JSON="["
    SEP=""
    for ((i=0; i<${#RESULTS[@]}; ++i)); do
        RUNS_JSON+="${SEP}${RESULTS[$i]}"
        SEP=","
    done
    RUNS_JSON+="]"

    # Merge existing + new runs if jq is available; otherwise just the new set.
    COMBINED="${RUNS_JSON}"
    if command -v jq &>/dev/null; then
        COMBINED=$(echo "${EXISTING_RUNS}" | jq -c ". + ${RUNS_JSON}" 2>/dev/null || echo "${RUNS_JSON}")
    fi

    cat > "$MANIFEST" <<MANIFEST_EOF
{
    "version": 1,
    "schema": "regression-manifest",
    "last_run": "${RUN_TIMESTAMP}",
    "git_revision": "${GIT_REV}",
    "summary": {
        "passed": ${PASSED},
        "failed": ${FAILED},
        "skipped": ${SKIPPED},
        "total": $((PASSED + FAILED + SKIPPED))
    },
    "runs": ${COMBINED}
}
MANIFEST_EOF

    echo "  Manifest updated: ${MANIFEST}"
fi

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0

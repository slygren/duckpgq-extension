#!/usr/bin/env bash
#
# Local test loop.
#
# CI is manual-trigger only right now (hosted runners were not picking up
# queued jobs for this private repo), so this is the real signal loop.
#
#   scripts/test_local.sh                 # whole suite
#   scripts/test_local.sh abi             # just the ABI tests
#   scripts/test_local.sh pattern         # just pattern_matching
#   scripts/test_local.sh path            # just path_finding
#   scripts/test_local.sh 'test/sql/path_finding/abi_context*'
#
# Build first with:  make release
# (export DUCKPGQ_LOCAL_EXT_DIR=/Users/sturelygren/Development/pgq if you need
#  httpfs/ducklake in the shell build -- the test suite does not.)
#
set -uo pipefail

cd "$(dirname "$0")/.."

UNITTEST=build/release/test/unittest

if [ ! -x "$UNITTEST" ]; then
    echo "error: $UNITTEST not found. Run 'make release' first." >&2
    exit 1
fi

case "${1:-all}" in
    all)     PATTERN='test/sql/*' ;;
    abi)     PATTERN='test/sql/path_finding/abi_*' ;;
    pattern) PATTERN='test/sql/pattern_matching/*' ;;
    path)    PATTERN='test/sql/path_finding/*' ;;
    *)       PATTERN="$1" ;;
esac

# Warn when the binary is older than the sources -- stale-binary results look
# exactly like real results, which has bitten this repo before.
NEWER=$(find src test -type f -newer "$UNITTEST" 2>/dev/null | wc -l | tr -d ' ')
if [ "$NEWER" -gt 0 ]; then
    echo "warning: $NEWER file(s) under src/ or test/ are newer than $UNITTEST."
    echo "         Results may not reflect current source. Run 'make release'."
    echo
fi

echo "running: $PATTERN"
echo

"$UNITTEST" "$PATTERN" 2>&1 | tee /tmp/duckpgq_test_out.txt | tail -4

echo
FAILED=$(grep -oE '^[a-z0-9_/.-]+\.test(_slow)?:[0-9]+: FAILED' /tmp/duckpgq_test_out.txt | sort -u)
if [ -n "$FAILED" ]; then
    echo "failing:"
    echo "$FAILED" | sed 's/^/  /'
fi

#!/bin/sh
# Run one consumer+producer pair. Consumer args before "--", producer args after.
#   ./run_pair.sh [consumer args] -- [producer args]
# Uses a per-run socket path so parallel runs cannot cross-talk.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SOCK="${XDG_RUNTIME_DIR:-/tmp}/dxr-dmabuf-spike.$$.sock"
CARGS=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do CARGS="$CARGS $1"; shift; done
[ $# -gt 0 ] && shift
"$HERE/build/consumer" --socket="$SOCK" $CARGS &
CPID=$!
"$HERE/build/producer" --socket="$SOCK" "$@"
PRC=$?
wait $CPID
CRC=$?
echo "run_pair: producer rc=$PRC consumer rc=$CRC"
[ $PRC -eq 0 ] && [ $CRC -eq 0 ]

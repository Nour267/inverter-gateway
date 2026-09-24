#!/bin/bash
# Integration test (DESIGN.md §9.2): run cloud server + gateway + simulator together,
# then check that the server got >= 8 valid TELEMETRY frames and 0 CRC errors.
#
# Usage: ./scripts/run_demo.sh        (needs vcan0: sudo ./scripts/setup_vcan.sh)
# Exit code: 0 = PASS, 1 = FAIL

set -e
cd "$(dirname "$0")/.."                     # run from the project folder

DURATION=12                                 # seconds the server listens
PORT=5000

if ! ip link show vcan0 > /dev/null 2>&1; then
    echo "vcan0 is missing. Run: sudo ./scripts/setup_vcan.sh"
    exit 1
fi

echo "== Building =="
make build/gateway build/inverter_sim > /dev/null

# Stop the background programs however the script ends (PASS, FAIL or Ctrl+C)
cleanup() {
    kill -INT "$GW_PID" 2> /dev/null || true
    kill "$SIM_PID" 2> /dev/null || true
    wait 2> /dev/null || true
}
trap cleanup EXIT

echo "== Starting cloud server, gateway and simulator for $DURATION s =="
python3 cloud/cloud_server.py --port "$PORT" --duration "$DURATION" --expect-min 8 &
SERVER_PID=$!
sleep 0.5                                   # let the server start listening

./build/gateway --bus vcan0 --server "127.0.0.1:$PORT" > /tmp/gateway.log 2>&1 &
GW_PID=$!
./build/inverter_sim --bus vcan0 > /dev/null 2>&1 &
SIM_PID=$!

set +e
wait "$SERVER_PID"                          # the server decides PASS or FAIL
RESULT=$?
set -e

echo "== Gateway log (last lines) =="
tail -n 3 /tmp/gateway.log
exit "$RESULT"

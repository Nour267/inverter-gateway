#!/bin/bash
# Create the virtual CAN interface vcan0.
# Run after every Windows restart:  sudo ./scripts/setup_vcan.sh

set -e   # stop at the first error

modprobe vcan                               # load the virtual CAN driver

if ! ip link show vcan0 > /dev/null 2>&1; then
    ip link add dev vcan0 type vcan         # create vcan0 (only if missing)
fi

ip link set up vcan0                        # turn it on

echo "vcan0 is up"

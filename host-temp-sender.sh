#!/bin/bash
# Host temperature sender via virtio-serial socket
# Sends CPU package, per-core temps, and GPU temp to VM every 5 seconds
SOCK=/run/host-temp.sock

while true; do
    # Wait for socket
    if [ ! -S "$SOCK" ]; then
        sleep 5
        continue
    fi

    PKG=$(sensors coretemp-isa-0000 2>/dev/null | grep 'Package' | head -1 | grep -oP '\+\K[0-9]+' | head -1)
    CORES=""
    while IFS= read -r line; do
        num=$(echo "$line" | grep -oP 'Core \K[0-9]+')
        temp=$(echo "$line" | grep -oP '\+\K[0-9]+' | head -1)
        [ -n "$num" ] && [ -n "$temp" ] && CORES="${CORES} c${num}=${temp}"
    done < <(sensors coretemp-isa-0000 2>/dev/null | grep 'Core ')

    GPU=$(nvidia-smi -i 0 --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null)

    # Send with timeout — don't hang if QEMU isn't listening
    echo "cpu=${PKG:-0} gpu=${GPU:-0}${CORES}" | timeout 3 socat - UNIX-CONNECT:"$SOCK" 2>/dev/null

    sleep 5
done

#!/bin/bash
# Host temperature sender via virtio-serial socket
SOCK=/run/host-temp.sock

while true; do
    [ ! -S "$SOCK" ] && sleep 5 && continue

    PKG=$(sensors coretemp-isa-0000 2>/dev/null | grep 'Package' | head -1 | grep -oP '\+\K[0-9]+' | head -1)
    CORES=""
    while IFS= read -r line; do
        num=$(echo "$line" | grep -oP 'Core \K[0-9]+')
        temp=$(echo "$line" | grep -oP '\+\K[0-9]+' | head -1)
        [ -n "$num" ] && [ -n "$temp" ] && CORES="${CORES} c${num}=${temp}"
    done < <(sensors coretemp-isa-0000 2>/dev/null | grep 'Core ')

    echo "cpu=${PKG:-0} gpu=0${CORES}" | socat - UNIX-CONNECT:"$SOCK" 2>/dev/null

    sleep 5
done

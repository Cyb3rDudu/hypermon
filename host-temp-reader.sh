#!/bin/bash
# Read host temps from virtio-serial and write to kernel module sysfs
VIRTIO=/dev/virtio-ports/host-temp
SYSFS=/sys/devices/platform/coretemp.0/update_temps

while true; do
    [ ! -e "$VIRTIO" ] && sleep 5 && continue
    [ ! -e "$SYSFS" ] && sleep 5 && continue

    # Read one line from virtio port
    LINE=$(head -1 "$VIRTIO" 2>/dev/null)
    [ -z "$LINE" ] && sleep 1 && continue

    # Convert all values to millidegrees
    CONVERTED=""
    for token in $LINE; do
        key=$(echo "$token" | cut -d= -f1)
        val=$(echo "$token" | cut -d= -f2)
        [ -n "$val" ] && CONVERTED="${CONVERTED} ${key}=$((val * 1000))"
    done

    echo "$CONVERTED" > "$SYSFS" 2>/dev/null
    sleep 1
done

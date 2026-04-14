#!/bin/bash
# Read host sensors from virtio-serial and write to kernel module sysfs
# Resilient to module reload (DKMS recompile) and virtio port loss
VIRTIO=/dev/virtio-ports/host-temp
SYSFS=/sys/devices/platform/coretemp.0/update_temps

while true; do
    # Wait for virtio port
    if [ ! -e "$VIRTIO" ]; then
        sleep 5
        continue
    fi

    # Wait for kernel module sysfs
    if [ ! -e "$SYSFS" ]; then
        sleep 5
        continue
    fi

    # Read with timeout — don't block forever if sender stops
    LINE=$(timeout 10 head -1 "$VIRTIO" 2>/dev/null)
    [ -z "$LINE" ] && sleep 1 && continue

    # Verify sysfs still exists (module could have been unloaded during read)
    [ ! -e "$SYSFS" ] && continue

    # Convert temperature values to millidegrees, pass fan RPMs as-is
    CONVERTED=""
    for token in $LINE; do
        key=$(echo "$token" | cut -d= -f1)
        val=$(echo "$token" | cut -d= -f2)
        # Validate: must be a number
        case "$val" in
            ''|*[!0-9]*) continue ;;
        esac
        # Fan RPMs: pass through without conversion
        case "$key" in
            f1|f2|f3)
                CONVERTED="${CONVERTED} ${key}=${val}"
                ;;
            *)
                CONVERTED="${CONVERTED} ${key}=$((val * 1000))"
                ;;
        esac
    done

    [ -n "$CONVERTED" ] && echo "$CONVERTED" > "$SYSFS" 2>/dev/null
    sleep 1
done

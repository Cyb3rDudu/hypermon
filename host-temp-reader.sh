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

    # Convert values per sensor type:
    #   temperatures: degrees -> millidegrees (*1000)
    #   fan RPMs: pass through as-is
    #   power (pw): milliwatts -> microwatts (*1000)
    CONVERTED=""
    for token in $LINE; do
        key=$(echo "$token" | cut -d= -f1)
        val=$(echo "$token" | cut -d= -f2)
        # Validate: must be a number
        case "$val" in
            ''|*[!0-9]*) continue ;;
        esac
        case "$key" in
            f1|f2|f3)
                # Fan RPMs: pass through without conversion
                CONVERTED="${CONVERTED} ${key}=${val}"
                ;;
            pw)
                # Power: milliwatts -> microwatts
                CONVERTED="${CONVERTED} ${key}=$((val * 1000))"
                ;;
            vcore|vdram|v12|v5|v33)
                # Voltages: already in millivolts, pass through
                CONVERTED="${CONVERTED} ${key}=${val}"
                ;;
            *)
                # Temperatures: degrees -> millidegrees
                CONVERTED="${CONVERTED} ${key}=$((val * 1000))"
                ;;
        esac
    done

    [ -n "$CONVERTED" ] && echo "$CONVERTED" > "$SYSFS" 2>/dev/null
    sleep 1
done

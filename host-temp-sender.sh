#!/bin/bash
# Host sensor sender via virtio-serial socket
# Sends CPU package, per-core temps, GPU temp, fan RPMs, and CPU power to VM every 5 seconds
SOCK=/run/host-temp.sock
RAPL=/sys/class/powercap/intel-rapl:0/energy_uj
PREV_ENERGY=""
PREV_TIME=""

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

    # Fan RPMs from nct6687
    FAN1=$(sensors nct6687-isa-0a20 2>/dev/null | grep 'CPU Fan' | grep -oP '[0-9]+(?= RPM)' | head -1)
    FAN2=$(sensors nct6687-isa-0a20 2>/dev/null | grep 'Pump Fan' | grep -oP '[0-9]+(?= RPM)' | head -1)
    FAN3=$(sensors nct6687-isa-0a20 2>/dev/null | grep 'System Fan #1' | grep -oP '[0-9]+(?= RPM)' | head -1)

    # CPU Power via RAPL (milliwatts from energy counter delta)
    PW=0
    if [ -r "$RAPL" ]; then
        CUR_ENERGY=$(cat "$RAPL" 2>/dev/null)
        CUR_TIME=$(date +%s%N)
        if [ -n "$PREV_ENERGY" ] && [ -n "$CUR_ENERGY" ]; then
            DELTA_E=$((CUR_ENERGY - PREV_ENERGY))
            DELTA_T=$((CUR_TIME - PREV_TIME))
            # Handle counter wraparound
            [ "$DELTA_E" -lt 0 ] && DELTA_E=0
            if [ "$DELTA_T" -gt 0 ]; then
                # energy_uj is microjoules, time is nanoseconds
                # Power (mW) = delta_uJ / delta_ns * 1e6 = delta_uJ * 1000 / (delta_ns / 1000)
                # Simplify: mW = delta_uJ * 1000000 / delta_ns
                PW=$((DELTA_E * 1000000 / DELTA_T))
            fi
        fi
        PREV_ENERGY=$CUR_ENERGY
        PREV_TIME=$CUR_TIME
    fi

    # Voltages from nct6687 — all converted to millivolts
    NCT=$(sensors nct6687-isa-0a20 2>/dev/null)
    # CPU Vcore: 604.00 mV -> 604
    VCORE=$(echo "$NCT" | grep 'CPU Vcore' | grep -oP '[0-9]+\.[0-9]+(?= mV)' | head -1)
    VCORE=${VCORE%.*}  # truncate decimals
    # DRAM: 932.00 mV -> 932
    VDRAM=$(echo "$NCT" | grep 'DRAM' | grep -oP '[0-9]+\.[0-9]+(?= mV)' | head -1)
    VDRAM=${VDRAM%.*}
    # +12V: 12.12 V -> 12120 mV
    V12=$(echo "$NCT" | grep '+12V' | grep -oP '[0-9]+\.[0-9]+(?= V)' | head -1)
    V12=$(echo "$V12" | awk '{printf "%d", $1 * 1000}')
    # +5V: 5.00 V -> 5000 mV
    V5=$(echo "$NCT" | grep '+5V' | grep -oP '[0-9]+\.[0-9]+(?= V)' | head -1)
    V5=$(echo "$V5" | awk '{printf "%d", $1 * 1000}')
    # +3.3V: 3.34 V -> 3340 mV
    V33=$(echo "$NCT" | grep '+3.3V' | grep -oP '[0-9]+\.[0-9]+(?= V)' | head -1)
    V33=$(echo "$V33" | awk '{printf "%d", $1 * 1000}')

    # Per-core CPU frequencies (KHz -> MHz, keyed by core_id)
    FREQS=""
    for cpudir in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq; do
        cpunum=$(echo "$cpudir" | grep -oP 'cpu\K[0-9]+')
        coreid=$(cat "/sys/devices/system/cpu/cpu${cpunum}/topology/core_id" 2>/dev/null)
        khz=$(cat "$cpudir" 2>/dev/null)
        if [ -n "$coreid" ] && [ -n "$khz" ]; then
            mhz=$((khz / 1000))
            FREQS="${FREQS} hz${coreid}=${mhz}"
        fi
    done

    # Send with timeout — don't hang if QEMU isn't listening
    echo "cpu=${PKG:-0} gpu=${GPU:-0}${CORES} f1=${FAN1:-0} f2=${FAN2:-0} f3=${FAN3:-0} pw=${PW} vcore=${VCORE:-0} vdram=${VDRAM:-0} v12=${V12:-0} v5=${V5:-0} v33=${V33:-0}${FREQS}" | timeout 3 socat - UNIX-CONNECT:"$SOCK" 2>/dev/null

    sleep 5
done

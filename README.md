# host-hwmon

Expose Proxmox host CPU per-core temperatures, GPU temperature and power, fan RPMs, CPU power, motherboard voltages, per-core CPU frequencies, and GPU clocks inside a QEMU VM as native hwmon sensors. btop and lm-sensors see them as `coretemp` — identical to bare metal.

**No network required** — data flows through QEMU's virtio-serial channel directly via the hypervisor.

## Architecture

```
Proxmox Host                              QEMU VM
┌──────────────────┐                     ┌──────────────────────┐
│ host-temp-sender │                     │ host-temp-reader     │
│ reads sensors +  │──virtio-serial────▶│ parses temps, writes │
│ nvidia-smi       │  Unix socket        │ to sysfs             │
│                  │                     │         │             │
└──────────────────┘                     │         ▼             │
                                         │ host_hwmon.ko (DKMS) │
                                         │ registers as         │
                                         │ coretemp-isa-0000    │
                                         │         │             │
                                         │         ▼             │
                                         │ btop / lm-sensors    │
                                         └──────────────────────┘
```

## What it does

- Reads CPU Package, per-core temperatures, and NVIDIA GPU temperature, power, and clocks on the host
- Reads fan RPMs (CPU Fan, Noctua pump, System Fan) from the motherboard sensor chip
- Calculates CPU package power from Intel RAPL energy counters
- Reads motherboard voltages (VCore, DRAM, +12V, +5V, +3.3V) from the nct6687 chip
- Reads per-core CPU frequencies from sysfs cpufreq, mapped by topology core ID
- Sends them every 5 seconds via a QEMU virtio-serial channel (no network)
- A DKMS kernel module in the VM registers as `coretemp`, exposing all sensors as standard hwmon channels
- btop displays per-core temperatures next to each CPU core bar — identical to running on bare metal

## Sensors exposed

| Sensor | hwmon type | Label | Unit |
|--------|-----------|-------|------|
| CPU Package temp | temp (Package id 0) | Package id 0 | milli-degC |
| Per-core temps | temp (Core N) | Core 0..28 | milli-degC |
| GPU temp | temp (A3000 GPU) | A3000 GPU | milli-degC |
| CPU Fan RPM | fan (fan1) | Host CPU Fan | RPM |
| Noctua pump RPM | fan (fan2) | Host Noctua | RPM |
| System Fan RPM | fan (fan3) | Host System Fan | RPM |
| CPU Package power | power (power1) | Host CPU Package | microwatts |
| GPU power | power (power2) | Host A3000 GPU | microwatts |
| CPU VCore | in (in0) | Host VCore | millivolts |
| DRAM voltage | in (in1) | Host DRAM | millivolts |
| +12V rail | in (in2) | Host +12V | millivolts |
| +5V rail | in (in3) | Host +5V | millivolts |
| +3.3V rail | in (in4) | Host +3.3V | millivolts |
| Per-core CPU freq | custom sysfs | Core 0..28 | MHz |
| GPU graphics clock | custom sysfs | Graphics | MHz |
| GPU memory clock | custom sysfs | Memory | MHz |

## Requirements

**Host (Proxmox VE):**
- lm-sensors (`sensors` command)
- socat
- nvidia-smi (optional, for GPU temp/power/clocks)

**VM (Guest):**
- Linux with DKMS support
- `linux-headers` for your kernel
- socat (for reader)

## Installation

### 1. Add virtio-serial port to VM config

On the Proxmox host, add to your VM's args in `/etc/pve/qemu-server/<vmid>.conf`:

```
args: <existing args> -chardev socket,id=hosttemp,path=/run/host-temp.sock,server=on,wait=off -device virtserialport,chardev=hosttemp,name=host-temp
```

### 2. Install sender on the host

```bash
sudo cp host-temp-sender.sh /usr/local/bin/
sudo chmod +x /usr/local/bin/host-temp-sender.sh
sudo cp host-temp-sender.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now host-temp-sender
```

### 3. Install DKMS module in the VM

```bash
sudo apt install dkms linux-headers-$(uname -r)
sudo mkdir -p /usr/src/host-hwmon-1.0
sudo cp host_hwmon.c Makefile dkms.conf /usr/src/host-hwmon-1.0/
sudo dkms install host-hwmon/1.0
sudo modprobe host_hwmon

# Auto-load on boot
echo host_hwmon | sudo tee /etc/modules-load.d/host-hwmon.conf
```

### 4. Install reader in the VM

```bash
sudo cp host-temp-reader.sh /usr/local/bin/
sudo chmod +x /usr/local/bin/host-temp-reader.sh
sudo cp host-temp-reader.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now host-temp-reader
```

### 5. Verify

```bash
sensors coretemp-isa-0000
```

Expected output:
```
coretemp-isa-0000
Adapter: ISA adapter
Host VCore:       604.00 mV
Host DRAM:        888.00 mV
Host +12V:         12.12 V
Host +5V:           5.01 V
Host +3.3V:         3.34 V
Host CPU Fan:    1791 RPM
Host Noctua:        0 RPM
Host System Fan: 1079 RPM
Host CPU Package:  19.12 W
Host A3000 GPU:    14.48 W
Package id 0:  +48.0°C
Core 0:        +46.0°C
Core 1:        +46.0°C
...
Core 28:       +46.0°C
A3000 GPU:     +48.0°C
```

Per-core CPU frequencies (custom sysfs, not shown by `sensors`):
```bash
cat /sys/devices/platform/coretemp.0/host_freqs
```

```
Core 0: 4601 MHz
Core 1: 4606 MHz
...
Core 28: 5000 MHz
```

GPU clocks:
```bash
cat /sys/devices/platform/coretemp.0/gpu_clocks
```

```
Graphics: 210 MHz
Memory: 405 MHz
```

## Data format

The sender transmits a single line every 5 seconds:
```
cpu=48 gpu=45 c0=46 ... c28=46 f1=1791 f2=0 f3=1079 pw=19120 gpw=14480 gcl=210 gmc=405 vcore=604 vdram=932 v12=12120 v5=5000 v33=3340 hz0=4601 ... hz28=5000
```

| Key | Description | Unit |
|-----|-------------|------|
| cpu | CPU Package temperature | degrees C |
| gpu | GPU temperature | degrees C |
| cN | Core N temperature | degrees C |
| f1 | CPU Fan RPM | RPM |
| f2 | Noctua (pump header) RPM | RPM |
| f3 | System Fan #1 RPM | RPM |
| pw | CPU package power | milliwatts |
| gpw | GPU power draw | milliwatts |
| gcl | GPU graphics clock | MHz |
| gmc | GPU memory clock | MHz |
| vcore | CPU VCore voltage | millivolts |
| vdram | DRAM voltage | millivolts |
| v12 | +12V rail voltage | millivolts |
| v5 | +5V rail voltage | millivolts |
| v33 | +3.3V rail voltage | millivolts |
| hzN | Core N CPU frequency | MHz |

The reader converts temperature values to millidegrees, power milliwatts to microwatts, and passes fan RPMs and voltages as-is to the kernel module via:
```
/sys/devices/platform/coretemp.0/update_temps
```

## Configuration

### Customizing for your CPU

The kernel module has the core IDs hardcoded for an Intel Core Ultra 5 245K (6P + 8E cores). Edit `host_hwmon.c` line with `core_ids[]` to match your CPU:

```c
static const int core_ids[NUM_CORES] = {0,1,2,3,4,5,6,7,8,12,16,20,24,28};
```

Also update `NUM_CORES` and `NUM_TEMP_CH` accordingly.

### Disabling GPU temperature

If you don't have an NVIDIA GPU, the sender will send `gpu=0`. The `A3000 GPU` sensor will show 0°C. To remove it, set `NUM_TEMP_CH` to `NUM_FIXED - 1 + NUM_CORES` and remove the GPU channel.

## How it works

1. **Sender** (host): Reads `sensors coretemp-isa-0000`, `sensors nct6687-isa-0a20` (fans, voltages), Intel RAPL energy counters (power), per-core CPU frequencies from sysfs, and optionally `nvidia-smi`, formats as `key=value` pairs, sends via `socat` to the QEMU virtio-serial Unix socket
2. **QEMU**: Bridges the Unix socket to `/dev/virtio-ports/host-temp` inside the VM
3. **Reader** (VM): Reads from the virtio port, converts temperatures to millidegrees, power to microwatts, passes fan RPMs and voltages as-is, writes to the kernel module's sysfs interface
4. **Kernel module**: Registers as a `coretemp` platform device with hwmon, exposing standard temperature, fan, power, and voltage sensors that btop/sensors/nvtop can read. CPU frequencies and GPU clocks are exposed via custom sysfs at `/sys/devices/platform/coretemp.0/host_freqs` and `gpu_clocks` (hwmon has no native freq type)

## Why coretemp?

btop matches hwmon sensors to CPU cores by looking for a device named `coretemp` with labels like `Package id 0`, `Core 0`, `Core 1`, etc. By mimicking this exact format, btop displays host temperatures as per-core readings — indistinguishable from running on bare metal.

## DKMS

The module auto-rebuilds on kernel updates via DKMS (`AUTOINSTALL=yes` in `dkms.conf`).

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| All temps 0°C | Restart sender on host, then reader in VM |
| `sensors` shows no coretemp | Check `lsmod \| grep host_hwmon`, run `modprobe host_hwmon` |
| Reader can't open virtio port | Check VM config has the `-chardev`/`-device` args, restart VM |
| Sender can't connect to socket | VM must be running, check `/run/host-temp.sock` exists |
| Temps stale after VM reboot | Restart `host-temp-sender` on host |
| Fans show 0 RPM | Check `sensors nct6687-isa-0a20` on host, ensure nct6687 driver is loaded |
| GPU power/clocks show 0 | Check `nvidia-smi` on host, ensure GPU is not passed through |

## License

GPL-2.0

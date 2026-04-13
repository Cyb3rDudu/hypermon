# host-hwmon

Expose Proxmox host CPU per-core temperatures and GPU temperature inside a QEMU VM as native hwmon sensors. btop and lm-sensors see them as `coretemp` — identical to bare metal.

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

- Reads CPU Package, per-core temperatures, and NVIDIA GPU temperature on the host
- Sends them every 5 seconds via a QEMU virtio-serial channel (no network)
- A DKMS kernel module in the VM registers as `coretemp`, exposing all temperatures as standard hwmon sensors
- btop displays per-core temperatures next to each CPU core bar — identical to running on bare metal

## Requirements

**Host (Proxmox VE):**
- lm-sensors (`sensors` command)
- socat
- nvidia-smi (optional, for GPU temp)

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
Package id 0:  +48.0°C
Core 0:        +46.0°C
Core 1:        +46.0°C
...
Core 28:       +46.0°C
A3000 GPU:     +48.0°C
```

## Configuration

### Customizing for your CPU

The kernel module has the core IDs hardcoded for an Intel Core Ultra 5 245K (6P + 8E cores). Edit `host_hwmon.c` line with `core_ids[]` to match your CPU:

```c
static const int core_ids[NUM_CORES] = {0,1,2,3,4,5,6,7,8,12,16,20,24,28};
```

Also update `NUM_CORES` and `NUM_CHANNELS` accordingly.

### Disabling GPU temperature

If you don't have an NVIDIA GPU, the sender will send `gpu=0`. The `A3000 GPU` sensor will show 0°C. To remove it, set `NUM_CHANNELS` to `NUM_FIXED - 1 + NUM_CORES` and remove the GPU channel.

### Data format

The sender transmits a single line every 5 seconds:
```
cpu=48 gpu=45 c0=46 c1=46 c2=46 c3=48 c4=46 c5=46 c6=46 c7=46 c8=46 c12=48 c16=46 c20=44 c24=48 c28=46
```

The reader converts values to millidegrees and writes them to the kernel module via:
```
/sys/devices/platform/coretemp.0/update_temps
```

## How it works

1. **Sender** (host): Reads `sensors coretemp-isa-0000` and optionally `nvidia-smi`, formats as `key=value` pairs, sends via `socat` to the QEMU virtio-serial Unix socket
2. **QEMU**: Bridges the Unix socket to `/dev/virtio-ports/host-temp` inside the VM
3. **Reader** (VM): Reads from the virtio port, converts to millidegrees, writes to the kernel module's sysfs interface
4. **Kernel module**: Registers as a `coretemp` platform device with hwmon, exposing standard temperature sensors that btop/sensors/nvtop can read

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

## License

GPL-2.0

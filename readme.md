# f81601a - Fintek F81601A PCIe CAN Driver (DKMS)

Driver for Fintek F81601A PCIe CAN controller (SJA1000-based), 2 CAN channels.

| Item | Value |
|---|---|
| Driver version | v1.03-20250515v2 |
| Kernel module | `f81601a` |
| PCI ID | `1c29:2004` |
| CAN channels | 2 |

## Requirements

- Linux kernel >= 4.x with SocketCAN support
- `dkms`, `build-essential`, `linux-headers-$(uname -r)`

## Installation

**From pre-built .deb (recommended):**

```bash
sudo apt install linux-headers-$(uname -r)
sudo dpkg -i f81601a-dkms_1.03.20250515_all.deb
```

**From source (build .deb locally):**

```bash
sudo apt install dkms build-essential linux-headers-$(uname -r)
make deb
sudo dpkg -i f81601a-dkms_1.03.20250515_all.deb
```

**Manual module build (no DKMS):**

```bash
sudo apt install build-essential linux-headers-$(uname -r)
make all
sudo insmod f81601a.ko
```

## Verify Hardware

```bash
lspci | grep -i f81601
dmesg | grep f81601
ls /sys/class/net | grep can
```

Expected: `can0`, `can1`

## CAN Channel Configuration

Set bitrate and bring up:

```bash
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
sudo ip link set can1 type can bitrate 500000
sudo ip link set can1 up
```

View settings:

```bash
ip -details link show can0
```

## Sending / Receiving

```bash
# Receive
candump can0
candump any

# Send standard frame
cansend can0 123#11.22.33.44.55.66.77.88

# Send extended frame (29-bit ID)
cansend can0 00000123#11.22.33.44.55.66.77.88
```

## Module Parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `enable_msi` | bool | 1 | Enable MSI interrupt |
| `max_msi_ch` | uint | 2 | Max MSI channels |
| `internal_clk` | int | -1 | Use internal clock (0/1 = 80MHz) |
| `external_clk` | uint | - | External clock frequency |
| `bus_restart_ms` | uint | - | Override bus restart timer |
| `rx_guard_time` | uint | - | RX guard time |
| `multi_tx_queue` | uint | - | Enable multi-TX queue |

Example:

```bash
sudo modprobe f81601a enable_msi=0 internal_clk=1
```

## Unloading

```bash
sudo rmmod f81601a
```

To also remove DKMS registration:

```bash
sudo dkms remove -m f81601a -v 1.03.20250515 --all
```

## Cross-Compilation (Optional)

For ARM64 cross-compilation, edit `env.sh` with your toolchain path and kernel source, then build manually:

```bash
. ./env.sh
make -C $KERNEL_SRC M=$(pwd) ARCH=$ARCH CROSS_COMPILE=$KERNEL_COMPILER modules
```

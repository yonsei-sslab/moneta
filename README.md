# Moneta: Ex-Vivo GPU Driver Fuzzing by Recalling In-Vivo Execution States

## Setup Instructions

### Prerequisites

```bash
# For QEMU
sudo apt install git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev ninja-build build-essential pkg-config cmake python-is-python3
# For guest backing image
sudo apt install libelf-dev debootstrap libslirp-dev
# For building a custom Ubuntu host
sudo apt install libncurses-dev gawk flex bison openssl libssl-dev dkms libelf-dev libudev-dev libpci-dev libiberty-dev autoconf llvm flex fakeroot build-essential crash kexec-tools makedumpfile kernel-wedge libncurses5 libncurses5-dev asciidoc binutils-dev libcap-dev default-jdk curl zstd
```

- CMake 3.7.2 or higher (`cmake -version`)
- Go 1.14.2 (`go version`)
  - Download <https://golang.org/dl/go1.14.2.linux-amd64.tar.gz>
  - Install using instructions found at: <https://golang.org/doc/install>
- Python 3

### Download source code

```bash
git clone https://github.com/yonsei-sslab/moneta.git
cd moneta
export MONPATH=$PWD # assumed by commands that follow
export PATH=$MONPATH/build/qemu/install/bin:$PATH # assumed by commands that follow
```

### Change the host Linux kernel for custom hypercall support

Build the host Linux kernel with [our patch](host/x86-64.patch) applied, install & reboot.

#### Tested environment

- [Ubuntu 22.04](https://git.launchpad.net/~ubuntu-kernel/ubuntu/+source/linux/+git/jammy/tag/?h=Ubuntu-hwe-5.19-5.19.0-40.41_22.04.1) on Intel Xeon 8358

### Setup Syzkaller

```bash
# Make sure that GOPATH env var is set, after installation.
go env -w GOPATH=$MONPATH/go
export GOPATH=$(go env GOPATH)
# Check `CGO_ENABLED=1` in your environment.
cd $MONPATH/go/src/github.com/google/syzkaller
make generate && make CC=gcc-10
```

### Generate necessary files

```bash
mkdir $MONPATH/build && cd $MONPATH/build
cmake .. && make
```

### Setup QEMU

```bash
cd $MONPATH/scripts
./build-qemu.sh
```

### Setup Guest (x86_64, Linux 6.2)

```bash
cd $MONPATH/guest
ln -s linux-6.2 linux
cd $MONPATH/scripts
./create-debian-image.sh # Create a Debian image
./build-guest-agent.sh # Build guest agent
./build-linux-guest.sh # Build Linux kernel

# /*
# For NVIDIA
cd $MONPATH
git clone https://github.com/NVIDIA/open-gpu-kernel-modules 
git -C open-gpu-kernel-modules checkout 530.41.03
git -C open-gpu-kernel-modules apply < guest/nvidia.patch
cd $MONPATH/scripts
./build-nvidia-driver.sh # Build/Setup NVIDIA driver
wget https://us.download.nvidia.com/XFree86/Linux-x86_64/530.41.03/NVIDIA-Linux-x86_64-530.41.03.run
# */

./copy-modules.sh # Copy necessary modules
```

```bash
# Start-up guest for installing userspace drivers
# Make sure to check the PCI device function number for passthrough
./qemu.sh

# guest$
moneta login: root
chmod 775 /NVIDIA-Linux-x86_64-530.41.03.run
/NVIDIA-Linux-x86_64-530.41.03.run --no-kernel-modules
# Exit QEMU after installation
```

### Generate snapshots and recordings

```bash
# guest$
strace -o moneta.trace -a 1 -s 65500 -v -xx -ff -Xraw -T --absolute-timestamps=precision:ns --syscall-times=ns -DD --moneta-n <ioctl count for snapshot> --moneta-s 1 --moneta-driver <nvidia/amdgpu/mali> <workload>
```

### Generate corpus using trace & fd

- pull the strace output files - `*trace` and `sfd_tfd*` - from the guest to host.

```bash
cd $MONPATH/go/src/github.com/google/syzkaller
syz-moneta -dir <trace_dir> -fd <fd_dir> -image <image>
mkdir -p $MONPATH/workdir/0 && cp corpus.db $MONPATH/workdir/0/
```

### Start Fuzzing

```bash
cd $MONPATH
make -C configs/syzkaller VMCNT=<number of fuzzing instances> -B
export LD_LIBRARY_PATH=$MONPATH/build/libagamotto${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
cd $MONPATH/go/src/github.com/google/syzkaller

./bin/syz-manager -config $MONPATH/configs/syzkaller/generated/<CFG_FILE>.cfg
```

## Citing our work
```bibtex
@inproceedings{jung2025moneta,
  author    = {Jung, Joonkyo and Jang, Jisoo and Jo, Yongwan and Vinck, Jonas and Voulimeneas, Alexios and Volckaert, Stijn and Song, Dokyung},
  title     = {{Moneta}: Ex-Vivo {GPU} Driver Fuzzing by Recalling In-Vivo Execution States},
  booktitle = {Proceedings of the Network and Distributed System Security Symposium (NDSS)},
  year      = {2025},
}
```

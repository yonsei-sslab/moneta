#!/usr/bin/env bash

MONPATH=$(realpath $PWD/..)
MONBUILD=$MONPATH/build
DRIVE_FILE=$MONBUILD/bullseye.qcow2
LIBAGAMOTTO=$MONPATH/build/libagamotto/libagamotto_nomain.so

ARCH=$(uname -m)

if [ $ARCH = x86_64 ]; then
	KERNEL=$MONBUILD/linux/arch/x86_64/boot/bzImage
	QEMU=$MONBUILD/qemu/install/bin/qemu-system-x86_64
elif [ $ARCH = aarch64 ]; then
	KERNEL=$MONBUILD/linux/arch/arm64/boot/Image
	QEMU=$MONBUILD/qemu/install/bin/qemu-system-aarch64
fi

set -eux

./mkfifo.sh

if [ $ARCH = x86_64 ]; then
	${QEMU} \
		-m 2G \
		-cpu host \
		-kernel $KERNEL \
		-append "console=ttyS0 root=/dev/sda earlyprintk=serial net.ifnames=0 pci=nomsi" \
		-drive file=$DRIVE_FILE,format=qcow2 \
		-enable-kvm \
		-device vfio-pci,host=08:00.0,id=gpu,multifunction=on,x-vga=on  \
		-nographic -serial mon:stdio \
		-object memory-backend-file,size=16777216,mem-path=/dev/shm/syzkaller-vm0-in,share,id=mb1 \
		-device ivshmem-plain,memdev=mb1,master=on \
		-object memory-backend-file,size=16777216,mem-path=/dev/shm/syzkaller-vm0-out,share,id=mb2 \
		-device ivshmem-plain,memdev=mb2,master=on \
		-device virtio-serial-pci,id=virtio-serial2,ioeventfd=off \
		-chardev pipe,id=ch2,path=/tmp/serial-err-vm0 \
		-device virtserialport,bus=virtio-serial2.0,chardev=ch2,name=serial2
elif [ $ARCH = aarch64 ]; then
	${QEMU} \
		-machine virt \
		-m 2G \
		-cpu host -machine gic-version=host -machine its=false \
		-kernel $KERNEL \
		-append "console=ttyAMA0 root=/dev/vda earlyprintk=serial net.ifnames=0" \
		-drive file=$DRIVE_FILE,format=qcow2 \
		-enable-kvm \
		-overcommit mem-lock=on \
		-device vfio-platform,host=fb000000.gpu,id=maligpu \
		-nographic -serial mon:stdio \
		-object memory-backend-file,size=16777216,mem-path=/dev/shm/syzkaller-vm0-in,share,id=mb1 \
		-device ivshmem-plain,memdev=mb1,master=on \
		-object memory-backend-file,size=16777216,mem-path=/dev/shm/syzkaller-vm0-out,share,id=mb2 \
		-device ivshmem-plain,memdev=mb2,master=on \
		-device virtio-serial-pci,id=virtio-serial2,ioeventfd=off \
		-chardev pipe,id=ch2,path=/tmp/serial-err-vm0 \
		-device virtserialport,bus=virtio-serial2.0,chardev=ch2,name=serial2
fi

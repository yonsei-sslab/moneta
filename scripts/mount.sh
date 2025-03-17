#!/usr/bin/env bash

MONPATH=$(realpath $PWD/..)
MONBUILD=$MONPATH/build

if [[ $# -ne 2 ]]; then
	echo "Usage: $0 <option> <image file>" >&2
	echo '    Options:' >&2
	echo '        -b  : guest base image' >&2
	echo '        -o  : guest overlay image' >&2
	echo '        -ub : unmount guest base image' >&2
	echo '        -uo : unmount guest overlay image' >&2
	exit 1
fi

IMG_TYPE=''
IMG_ACTION=''
case "$1" in
	-b)
		IMG_TYPE='base'
		IMG_ACTION='mount'
		;;
	-o)
		IMG_TYPE='overlay'
		IMG_ACTION='mount'
		;;
	-ub)
		IMG_TYPE='base'
		IMG_ACTION='unmount'
		;;
	-uo)
		IMG_TYPE='overlay'
		IMG_ACTION='unmount'
		;;
	*)
		echo "Unknown option: $1" >&2
		exit 1
		;;
esac

IMAGE=$2
if [[ "$IMAGE" != *.qcow2 ]]; then
	echo "Invalid image (.qcow2) file: $IMAGE" >&2
	exit 1
fi
MNT_DIR=/mnt/${IMAGE%.*}

if [ ! -d MNT_DIR ]; then
	sudo mkdir -p $MNT_DIR
fi

if [[ "$IMG_TYPE" == 'base' ]]; then
	if [[ "$IMG_ACTION" == 'mount' ]]; then
		# Mount base image
		mountpoint -q $MNT_DIR && sudo umount $MNT_DIR > /dev/null
		sudo mount -o loop $MONBUILD/$IMAGE $MNT_DIR
	else
		# Unmount base image
		sudo umount $MNT_DIR
	fi
else
	if [[ "$IMG_ACTION" == 'mount' ]]; then
		# Mount overlay image
		mountpoint -q $MNT_DIR && sudo umount $MNT_DIR && sudo qemu-nbd --disconnect /dev/nbd0 > /dev/null

		sudo modprobe nbd max_part=8
		sudo $MONBUILD/qemu/install/bin/qemu-nbd --connect=/dev/nbd0 $MONBUILD/$IMAGE
		sudo mount /dev/nbd0 $MNT_DIR
	else
		# Unmount overlay image
		sudo umount $MNT_DIR
		sudo $MONBUILD/qemu/install/bin/qemu-nbd --disconnect /dev/nbd0
	fi
fi

# Return mount directory for other scripts to use
if [[ "$IMG_ACTION" == 'mount' ]]; then
	echo $MNT_DIR
fi

#!/usr/bin/env bash

MONPATH=$(realpath $PWD/..)
MONBUILD=$MONPATH/build
IMAGE=bullseye.qcow2

MNT_DIR=$(./mount.sh -o $IMAGE)

if [ -d $GOPATH ]; then
	if [ $ARCH == x86_64 ]; then
		sudo cp -p $GOPATH/src/github.com/google/syzkaller/bin/linux_amd64/syz-* $MNT_DIR/
	elif [ $ARCH == aarch64 ]; then
		sudo cp -p $GOPATH/src/github.com/google/syzkaller/bin/linux_arm64/syz-* $MNT_DIR/
	fi
fi

sudo cp -p ../build/executor/syz-executor.debug $MNT_DIR/
sudo cp -p ./NVIDIA-Linux-x86_64-530.41.03.run $MNT_DIR/

./mount.sh -uo $IMAGE

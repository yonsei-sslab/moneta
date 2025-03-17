#!/usr/bin/env bash

MONPATH=$(realpath $PWD/..)
MONBUILD=$MONPATH/build

HOSTARCH=$(uname -m)

pushd $MONPATH/guest/linux

make defconfig O=$MONBUILD/linux

if [ $HOSTARCH = x86_64 ]; then
	make kvm_guest.config O=$MONBUILD/linux
	make moneta-x86.config O=$MONBUILD/linux
elif [ $HOSTARCH = aarch64 ]; then
	make moneta-arm64.config O=$MONBUILD/linux
fi

make -j $(nproc) O=$MONBUILD/linux

popd
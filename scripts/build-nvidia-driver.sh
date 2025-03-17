#!/usr/bin/env bash

MONPATH=$(realpath $PWD/..)
MONBUILD=$MONPATH/build
IMAGE=bullseye.qcow2

if [ ! -d $MONBUILD/modules/lib/modules/6.2.0 ]; then
    echo Creating $MONBUILD/modules/lib/modules/6.2.0
	mkdir -p $MONBUILD/modules/lib/modules/6.2.0
fi

D=$MONPATH/open-gpu-kernel-modules

pushd $D
export SYSSRC=$MONPATH/guest/linux
export SYSOUT=$MONBUILD/linux
make modules \
	CONFIG_KCOV=y \
	CONFIG_KCOV_INSTRUMENT_ALL=y \
	CONFIG_KASAN=y \
	CONFIG_KASAN_INLINE=y \
	CONFIG_KASAN_VMALLOC=y \
	CONFIG_UBSAN=y  \
	CONFIG_CC_HAS_UBSAN_BOUNDS=y \
	CONFIG_UBSAN_BOUNDS=y \
	CONFIG_UBSAN_ONLY_BOUNDS=y \
	CONFIG_UBSAN_SHIFT=y \
	-B -j $(nproc) CC=gcc-12
make modules_install -B -j $(nproc) INSTALL_MOD_PATH=$MONBUILD/modules M=$D
popd

MNT_DIR=$(./mount.sh -o $IMAGE)
sudo cp -rd $MONBUILD/modules/lib/modules/* $MNT_DIR/lib/modules/
echo "options nvidia NVreg_OpenRmEnableUnsupportedGpus=1" | sudo tee $MNT_DIR/etc/modprobe.d/nvreg_fix.conf > /dev/null
./mount.sh -uo $IMAGE
#!/usr/bin/env bash

MONPATH=$(realpath $PWD/..)
MONBUILD=$MONPATH/build

ARCH_QEMU=$(uname -m)
REHOST=""

display_help() {
	echo "Usage: $0 [option...] " >&2
	echo
	echo "   -a, --arch [x86_64, aarch64]	 Set architecture of QEMU to build. Default: arch that runs this script (uname -m)"
	echo "   -h, --help		 	 Display help message"
	echo "   -r, --rehost		 Rehosting ARM64 to X86_64"
	echo
}

# Get argument
while true; do
	if [ $# -eq 0 ];then
		echo $#
		break
	fi
	case "$1" in
        -h | --help)
            display_help
            exit 0
            ;;
		-a | --arch)
			ARCH_QEMU=$2
			shift 2
			;;
		-r | --rehost)
			REHOST=1
			shift 2
			;;
		-*)
			echo "Error: Unknown option: $1" >&2
			exit 1
			;;
		*)  # No more options
			break
			;;
	esac
done

set -eux

if [ ! -d $MONBUILD/qemu/install ]; then
    echo Creating $MONBUILD/qemu/install
    mkdir -p $MONBUILD/qemu/install
fi

pushd $MONBUILD/qemu

if [ $REHOST ]; then
	$MONPATH/qemu/configure --prefix=$MONBUILD/qemu/install --disable-git-update --target-list=aarch64-softmmu --with-agamotto=$MONBUILD/libagamotto --enable-debug --disable-werror --enable-arm-rehost --enable-x86-rehost
elif [ $ARCH_QEMU = x86_64 ]; then
    $MONPATH/qemu/configure --prefix=$MONBUILD/qemu/install --disable-git-update --target-list=x86_64-softmmu --with-agamotto=$MONBUILD/libagamotto --enable-debug --disable-werror
elif [ $ARCH_QEMU = aarch64 ]; then
    $MONPATH/qemu/configure --prefix=$MONBUILD/qemu/install --disable-git-update --target-list=aarch64-softmmu --with-agamotto=$MONBUILD/libagamotto --enable-debug --disable-werror --enable-arm-rehost
fi

make -j $(nproc)
make install

popd

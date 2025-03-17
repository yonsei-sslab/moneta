#!/usr/bin/env bash

MONPATH=$(realpath $PWD/..)
IMAGE=bullseye.qcow2

set -eux

# Ensure install path exists
DIR=$(./mount.sh -o $IMAGE)

INSTALL_PATH=/root/.local/moneta
DEST=${DIR}${INSTALL_PATH}

sudo mkdir -p "${DEST}/bin"

GUEST_AGENT_SRC="${MONPATH}/guest/agent"

echo 'Copying agent source into guest...' >&2
sudo cp -a "$GUEST_AGENT_SRC" "$DEST"

# Build guest agent
echo 'Building guest agent from source...' >&2
sudo -E chroot "$DIR" sh -c "cd \"${INSTALL_PATH}/agent\"; ./bootstrap; ./configure --disable-mpers; make"

# Move guest agent binary to PATH
GUEST_AGENT_OUT="${DEST}/agent/src/strace"
if sudo [ ! -x "$GUEST_AGENT_OUT" ]; then
        ./mount.sh -uo $IMAGE
        echo 'Guest agent binary not found' >&2
        exit 1
fi

echo 'Moving guest agent to place...'
if sudo [ -e "${DIR}/usr/bin/strace" ]; then
        sudo rm -rf "${DIR}/usr/bin/strace"
        echo 'Removed old strace binary' >&2
fi
sudo mv -t "${DIR}/usr/bin" "$GUEST_AGENT_OUT"
echo 'Moved guest agent binary into /usr/bin' >&2

./mount.sh -uo $IMAGE
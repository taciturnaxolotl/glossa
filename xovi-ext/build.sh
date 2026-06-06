#!/bin/bash
# Build all grimoire xovi components: extension .so + uinject + uinjectd + grimoired
set -e
cd "$(dirname "$0")"

CONTAINER="grimoire-builder"
EXT_DIR="$(pwd)/grimoire-injector"
UINJECT_DIR="$(pwd)/uinject"
UINJECTD_DIR="$(pwd)/uinjectd"
GRIMOIRED_DIR="$(pwd)/grimoired"

# Create persistent builder container if needed
if ! docker inspect $CONTAINER &>/dev/null; then
    echo "Creating builder container..."
    docker create --platform linux/amd64 --name $CONTAINER \
        -v "$(pwd):/xovi-ext" \
        -w /xovi-ext/grimoire-injector \
        eeems/remarkable-toolchain:latest-rm2 \
        bash -c "git clone https://github.com/asivery/xovi /tmp/xovi 2>/dev/null; tail -f /dev/null"
    docker start $CONTAINER
    sleep 3
fi

# Build extension .so
echo "=== Building grimoire-injector.so ==="
docker exec $CONTAINER bash -c '
export XOVI_REPO=/tmp/xovi
. /opt/codex/*/*/environment-setup-*
cd /xovi-ext/grimoire-injector
qmake6 2>/dev/null
make 2>&1
'

echo "Copying .so..."
docker cp $CONTAINER:/xovi-ext/grimoire-injector/grimoire-injector.so "$EXT_DIR/"

# Build uinject
echo "=== Building uinject ==="
docker exec $CONTAINER bash -c '
. /opt/codex/*/*/environment-setup-*
cd /xovi-ext/uinject
$CC -o uinject uinject.c -O2 $CFLAGS $LDFLAGS
echo "uinject built OK"
'

echo "Copying uinject..."
docker cp $CONTAINER:/xovi-ext/uinject/uinject "$UINJECT_DIR/"

# Build uinjectd
echo "=== Building uinjectd ==="
docker exec $CONTAINER bash -c '
. /opt/codex/*/*/environment-setup-*
cd /xovi-ext/uinjectd
$CC -o uinjectd uinjectd.c -O2 $CFLAGS $LDFLAGS -lpthread
echo "uinjectd built OK"
'

echo "Copying uinjectd..."
docker cp $CONTAINER:/xovi-ext/uinjectd/uinjectd "$UINJECTD_DIR/"

# Build grimoired
echo "=== Building grimoired ==="
docker exec $CONTAINER bash -c '
. /opt/codex/*/*/environment-setup-*
cd /xovi-ext/grimoired
$CC -o grimoired grimoired.c -O2 $CFLAGS $LDFLAGS -lpthread -lpng16 -lssl -lcrypto
echo "grimoired built OK"
'

echo "Copying grimoired..."
docker cp $CONTAINER:/xovi-ext/grimoired/grimoired "$GRIMOIRED_DIR/"

# Deploy all to device
echo "=== Deploying to device ==="
scp "$EXT_DIR/grimoire-injector.so" remarkable:/home/root/xovi/extensions.d/
ssh remarkable 'killall uinject 2>/dev/null; killall uinjectd 2>/dev/null; killall grimoired 2>/dev/null; rm -f /home/root/uinject /home/root/uinjectd /home/root/grimoired'
scp "$UINJECT_DIR/uinject" remarkable:/home/root/uinject
scp "$UINJECTD_DIR/uinjectd" remarkable:/home/root/uinjectd
scp "$GRIMOIRED_DIR/grimoired" remarkable:/home/root/grimoired
ssh remarkable '/home/root/uinjectd &'
ssh remarkable '/home/root/grimoired &'

echo "Done! Restart xochitl to load new extension."

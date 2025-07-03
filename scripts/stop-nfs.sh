#!/usr/bin/env bash
#
# stop-nfs.sh — Shut down all daemons started by start-nfs.sh
#   sudo ./stop-nfs.sh

set -euo pipefail

WORKDIR=$(pwd)
MOUNT_BASE=/srv/nfs

for pidfile in "$WORKDIR"/inst*/unfsd.pid; do
    [[ -e $pidfile ]] || continue
    pid=$(cat "$pidfile")
    echo "[*] killing unfsd pid $pid"
    kill "$pid" 2>/dev/null || true
done

echo "-----------------------------"

# clean mounts & loops
for loop in $(losetup -a | awk -F: '{print $1}'); do
    mountpt=$(lsblk -no MOUNTPOINT "$loop")
    if [[ $mountpt == $MOUNT_BASE/* ]]; then
        echo "[*] umount $mountpt && losetup -d $loop"
        if ! umount "$mountpt"; then
            echo "    [!] Failed to umount $mountpt"
            echo "    [i] Processes using it:"
            lsof "$mountpt" || true
            fuser -vm "$mountpt" || true
        fi
        if losetup "$loop" &>/dev/null; then
            losetup -d "$loop" || echo "    [!] Failed to detach $loop"
        fi
        rm -r "$mountpt" || true
    fi
done

echo "-----------------------------"

# Extra cleanup for deleted loop devices
for loop in $(losetup -a | grep '(deleted)' | awk -F: '{print $1}'); do
    echo "[!] $loop is holding a deleted file, trying force detach..."
    sudo losetup -d "$loop" || echo "    [!] Could not force detach $loop"
done

# Clean up all loop devices

sudo losetup -D 

echo "-----------------------------"

# remove all the instance directories
echo "[*] removing instance directories: $WORKDIR/inst*"
echo "removing: "
find "$WORKDIR" -maxdepth 1 -type d -name 'inst*' -print
rm -rf "$WORKDIR"/inst*

echo "All UNFS3 instances stopped and cleaned up."
echo "You can now safely run ./start-nfs.sh to start fresh instances."

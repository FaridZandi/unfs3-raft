#!/usr/bin/env bash
#
# stop-nfs.sh  —  Shut down all daemons started by start-nfs.sh
#
#   sudo ./stop-nfs.sh
#
# Looks for *.pid files inside inst*/    and kills them,
# unmounts /srv/nfs/shared<i>, and detaches loop devices.

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

# remove all the instance directories
echo "[*] removing instance directories: $WORKDIR/inst*"
echo "removing: "
find "$WORKDIR" -maxdepth 1 -type d -name 'inst*' -print
rm -rf "$WORKDIR"/inst*

echo "-----------------------------"

# clean mounts & loops
for loop in $(losetup -a | awk -F: '{print $1}'); do
    mountpt=$(lsblk -no MOUNTPOINT "$loop")
    if [[ $mountpt == $MOUNT_BASE/* ]]; then
        echo "[*] umount $mountpt && losetup -d $loop"
        umount "$mountpt" || true
        rm -r "$mountpt" || true
        losetup -d "$loop" || true
    fi
done

echo "All UNFS3 instances stopped and cleaned up."

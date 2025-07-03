#!/usr/bin/env bash
#
# start-nfs.sh  —  Launch multiple UNFS3 daemons
#
#   sudo ./start-nfs.sh <num-instances> [size-MiB]
#
#   • creates per-instance directories:  inst1/ inst2/ …
#   • puts   fs.img, exports, handle.log, raft.log, unfsd.pid   in each dir
#   • mounts each img on  /srv/nfs/shared<i>
#   • starts unfsd with   -e -H -R -i -n -m  pointing at those files
#
# REQUIREMENTS: unfsd (UNFS3), losetup, mkfs.ext4, mount.  Run as root
#               or grant the script CAP_SYS_ADMIN & CAP_NET_BIND_SERVICE.

set -euo pipefail

NUM=${1:-3}                  # how many daemons
SIZE=${2:-1024}              # size of each image (MiB)

BASE_NFS_PORT=2049           # NFS   port for instance 1
BASE_MNT_PORT=2049           # mount port for instance 1 (avoid <1024)

WORKDIR=$(pwd)               # where the script is run
MOUNT_BASE=/srv/nfs          # parent for visible shares
GLOBAL_PIDLIST=$WORKDIR/unfsd_all.pids
: > "$GLOBAL_PIDLIST"        # truncate old list

echo "removing all the folowing directories:"
echo "  $WORKDIR/inst*"
rm -rf inst*

mkdir -p "$MOUNT_BASE"

MOUNT_IMAGE=True

echo "making unfs3-raft executable"
cd .. 
make 
sudo make install
cd "$WORKDIR"


for i in $(seq 1 "$NUM"); do
    instdir=$WORKDIR/inst$i
    mkdir -p "$instdir"

    img=$instdir/fs.img
    exports=$instdir/exports
    handle=$instdir/handle.log
    raft=$instdir/raft.log
    pidfile=$instdir/unfsd.pid

    share=$MOUNT_BASE/shared$i
    nfs_port=$((BASE_NFS_PORT + i - 1))
    mnt_port=$((BASE_MNT_PORT + i - 1))

    mkdir -p "$share"
    chown faridzandi:dfrancis "$share"

    # if mount image is not needed, skip the setup
    if [[ $MOUNT_IMAGE == False ]]; then
        echo "[*] inst$i: skipping image setup, will not mount"
        echo "$share"       

    else
        echo "[*] inst$i: setting up image & mount point"

        # create image if it does not exist
        echo "[*] inst$i: creating ${SIZE} MiB image"
        truncate -s "${SIZE}M" "$img"
        mkfs.ext4 -q "$img"
        
        echo "[*] inst$i: mounting image to $share"
        loopdev=$(losetup -f)
        losetup "$loopdev" "$img"
        mount -o loop,sync "$loopdev" "$share"

        chown -R faridzandi:dfrancis "$share"
    fi 

    echo "$share 10.70.10.108(rw,sync,no_subtree_check,removable,fsid=1)" > "$exports"

    echo "[*] inst$i: launching UNFS3 on ports nfs=$nfs_port  mount=$mnt_port"
    unfsd -d -e "$exports" -i "$pidfile" -n "$nfs_port" -m "$mnt_port" -H "$handle" -R "$raft" > "$instdir/unfsd.out" 2>&1 &

    echo $! >> "$GLOBAL_PIDLIST"
    echo "    inst$i OK  →  share=$share"
done

echo
echo "All $NUM instances are up.  Global PID list: $GLOBAL_PIDLIST"

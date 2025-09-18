#!/usr/bin/env bash
set -euo pipefail

# ---------------- CONFIG ----------------
NUM=${1:-3}                  # how many daemons
MODE=${2:-ext2}              # ext2 or ext4
SIZE=${3:-4}              # size of each image (MiB)

MOUNT_IMAGE=True
USE_GDB=0
# ----------------------------------------
USER=$(id -un) # user who ran the script
GROUP=$(id -gn "$USER")  # group of the user who ran the script
uid=$(id -u "$USER")                  # or just: id -u
gid=$(id -g "$USER")   # or your primary group: id -g
CLIENT_IP=localhost
MOUNT_OPTIONS="rw,removable,insecure"
WORKDIR=$(pwd)                # where the script is run
MOUNT_BASE=/tmp/$USERNAME/srv/nfs          # parent for visible shares
nfs_port=2049
mnt_port=2049

echo "USER=$USER, GROUP=$GROUP, CLIENT_IP=$CLIENT_IP"
echo "uid=$uid, gid=$gid"
echo "MODE=$MODE"
echo "MOUNT_IMAGE=$MOUNT_IMAGE"
echo "NUM=$NUM, SIZE=${SIZE}MiB"
echo "WORKDIR=$WORKDIR"
echo "MOUNT_BASE=$MOUNT_BASE"
echo "nfs_port=$nfs_port, mnt_port=$mnt_port"
# ----------------------------------------


GLOBAL_PIDLIST=$WORKDIR/unfsd_all.pids
: > "$GLOBAL_PIDLIST"         # truncate old list

echo "removing all the following directories:"
echo "  $WORKDIR/inst*"
rm -rf inst*

mkdir -p "$MOUNT_BASE"

echo "making unfs3-raft executable"
cd ..
make
cd "$WORKDIR"



peer_list () {
    local self=$1 total=$2
    local list=""
    for j in $(seq 1 "$total"); do
        [[ $j -eq $self ]] && continue
        list+="${j},"
    done
    echo "${list%,}"
}


# if mount image is not needed, skip the setup

for i in $(seq 1 "$NUM"); do
    echo "starting instance $i"
    instdir=$WORKDIR/inst$i
    logicalshare="$MOUNT_BASE/shared"
    share="$MOUNT_BASE/shared$i"

    mkdir -p "$instdir"

    img=$instdir/fs.img
    exports=$instdir/exports
    raft=$instdir/raft.log
    pidfile=$instdir/unfsd.pid

    # -------------------------------------------------------------------------
    # Set up image and mount point
    # -------------------------------------------------------------------------
    mkdir -p "$share"
    # sudo chown $USER:$GROUP "$share"

    # if mount image is not needed, skip the setup
    if [[ $MOUNT_IMAGE == False ]]; then
        echo "[*] inst$i: skipping image setup, will not mount"
        echo "$share"
    else
        echo "[*] inst$i: setting up image & mount point"

        # create image if it does not exist
        echo "[*] inst$i: creating ${SIZE} MiB image"
        truncate -s "${SIZE}M" "$img"

        if [[ $MODE == "ext2" ]]; then
            mkfs.ext2 -q -E root_owner=${uid}:${gid} "$img"
            echo "[*] inst$i: mounting image with fuse-ext2"
            fuse-ext2 -o rw+ -o direct_io "$img" "$share"
            # sudo chown -R $USER:$GROUP "$share"
        elif [[ $MODE == "ext4" ]]; then
            mkfs.ext4 -q "$img"
            echo "[*] inst$i: mounting image with loop device"
            loopdev=$(sudo losetup -f)
            sudo losetup "$loopdev" "$img"
            sudo mount -o loop,sync "$loopdev" "$share"
            sudo chown -R $USER:$GROUP "$share"
        else
            echo "Unknown MODE=$MODE" >&2
            exit 1
        fi
    fi

    # set up exports file
    echo "$share $CLIENT_IP($MOUNT_OPTIONS)" > "$exports"

    # -------------------------------------------------------------------------
    # RAFT parameters
    # -------------------------------------------------------------------------
    node_id=$i
    peers=$(peer_list "$i" "$NUM")

    echo "[*] inst$i: launching UNFS3 on ports nfs=$nfs_port  mount=$mnt_port"
    echo "            raft: id=$node_id  peers=$peers"

    cmd_args=(
        -d # run in the foreground
        -p # disable portmapper
        -e "$exports" # exports file if normal instance
        -i "$pidfile" # pid file to help stop the instance later 
        -n "$nfs_port" # NFS port
        -m "$mnt_port" # mount port
        -R "$raft" # all raft logs will permanently be here
        -I "$node_id" # node ID for RAFT. unique per instance
        -P "$peers" # peer list for RAFT (comma-separated IDs of other nodes)
        -g "$MOUNT_BASE" # mount root for this instance
        -G "$logicalshare" # logical mount root for this instance
        -g "$share" # mount root for this instance
    )

    if [[ $USE_GDB -eq 1 ]]; then
        gdb -ex run -ex "bt" --args ../unfsd "${cmd_args[@]}" > "$instdir/unfsd.out" 2>&1 &
    else
        ../unfsd "${cmd_args[@]}" > "$instdir/unfsd.out" 2>&1 &
    fi

    echo $! >> "$GLOBAL_PIDLIST"
    echo "    inst$i OK  →  share=$share, pid=$!, exports=$exports"
done

echo
echo "All $NUM instances are up. Global PID list: $GLOBAL_PIDLIST"

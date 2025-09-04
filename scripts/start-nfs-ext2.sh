#!/usr/bin/env bash

set -euo pipefail

NUM=${1:-3}                  # how many daemons
SIZE=${2:-1024}              # size of each image (MiB)

WORKDIR=$(pwd)               # where the script is run
MOUNT_BASE=~/srv/nfs          # parent for visible shares
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
cd "$WORKDIR"

USE_GDB=0

USER=`id -un`  # user who ran the script
GROUP=`id -gn $USER`  # group of the user who ran the script
CLIENT_IP=localhost
MOUNT_OPTIONS="rw,removable,insecure"

echo "USER=$USER, GROUP=$GROUP, CLIENT_IP=$CLIENT_IP"

################################################################################
# Helper: build a comma-separated list of all IDs except $1
################################################################################
peer_list () {
    local self=$1 total=$2
    local list=""
    for j in $(seq 1 "$total"); do
        [[ $j -eq $self ]] && continue
        list+="${j},"
    done
    # trim trailing comma
    echo "${list%,}"
}

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

    nfs_port=2049 # $((2050 + i - 1))
    mnt_port=2049 # $((2050 + i - 1))

    mkdir -p "$share"
    sudo chown $USER:$GROUP "$share"

    # if mount image is not needed, skip the setup
    if [[ $MOUNT_IMAGE == False ]]; then
        echo "[*] inst$i: skipping image setup, will not mount"
        echo "$share"
    else
        echo "[*] inst$i: setting up image & mount point"

        # create image if it does not exist
        echo "[*] inst$i: creating ${SIZE} MiB image"
        truncate -s "${SIZE}M" "$img"
        mkfs.ext2 -q "$img"

        echo "[*] inst$i: mounting image to $share using fuse-ext2"
        mkdir -p "$share"
        # FUSE mounts as current user, no sudo needed
        fuse-ext2 -o rw+ -o direct_io "$img" "$share"
        # give yourself ownership just in case
        sudo chown -R $USER:$GROUP "$share"
    fi

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
        echo "[*] inst$i: running under gdb and run"
        gdb -ex run -ex "bt" --args ../unfsd "${cmd_args[@]}" > "$instdir/unfsd.out" 2>&1 &
    else
        echo "running the command:"
        echo "  ../unfsd ${cmd_args[*]} > $instdir/unfsd.out 2>&1 &"
        ../unfsd "${cmd_args[@]}" > "$instdir/unfsd.out" 2>&1 &
    fi

    echo $! >> "$GLOBAL_PIDLIST"
    this_pid=$!
    echo "    inst$i OK  →  share=$share, 
                pid=$this_pid, 
                exports=$exports"  
done

echo
echo "All $NUM instances are up.  Global PID list: $GLOBAL_PIDLIST"

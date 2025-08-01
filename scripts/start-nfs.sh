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
#   • passes       -I <id>  -P <peer-list>   (RAFT node-id & peers)
#
# REQUIREMENTS: unfsd (UNFS3), losetup, mkfs.ext4, mount.  Run as root
#               or grant the script CAP_SYS_ADMIN & CAP_NET_BIND_SERVICE.

set -euo pipefail

NUM=${1:-3}                  # how many daemons
SIZE=${2:-1024}              # size of each image (MiB)

BASE_NFS_PORT=2050           # NFS   port for instance 1
BASE_MNT_PORT=2050           # mount port for instance 1 (avoid <1024)

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
sudo make install
cd "$WORKDIR"

USE_GDB=1

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

for i in $(seq 0 "$NUM"); do
    if [[ $i -eq 0 ]]; then
        echo "special case: leader stuff." 
        instdir=$WORKDIR/global
    else
        echo "starting instance $i"
        instdir=$WORKDIR/inst$i
    fi      

    if [[ $i -eq 0 ]]; then
        share="$MOUNT_BASE/shared"
    else
        share="$MOUNT_BASE/shared$i"
    fi  

    mkdir -p "$instdir"

    img=$instdir/fs.img
    exports=$instdir/exports
    handle=$instdir/handle.log
    raft=$instdir/raft.log
    pidfile=$instdir/unfsd.pid

    nfs_port=$((BASE_NFS_PORT + i - 1))
    mnt_port=$((BASE_MNT_PORT + i - 1))

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
        mkfs.ext4 -q "$img"

        echo "[*] inst$i: mounting image to $share"
        loopdev=$(sudo losetup -f)
        sudo losetup "$loopdev" "$img"
        sudo mount -o loop,sync "$loopdev" "$share"

        sudo chown -R $USER:$GROUP "$share"
    fi

    echo "$share $CLIENT_IP($MOUNT_OPTIONS)" > "$exports"

    # -------------------------------------------------------------------------
    # RAFT parameters
    # -------------------------------------------------------------------------

    # only if i > 1 
    if [[ $i -eq 0 ]]; then
        echo "[*] global instance, skipping raft setup"
    else
        node_id=$i
        peers=$(peer_list "$i" "$NUM")

        echo "[*] inst$i: launching UNFS3 on ports nfs=$nfs_port  mount=$mnt_port"
        echo "            raft: id=$node_id  peers=$peers"
        
        cmd_args=(
            -d # run in the foreground
            -p # disable portmapper
            -e "$exports" # exports file if normal instance
            -E "$WORKDIR/global/exports"  # exports file for leader
            -i "$pidfile" # pid file to help stop the instance later 
            -n "$nfs_port" # NFS port used if normal instance
            -m "$mnt_port" # mount port used if normal instance
            -H "$handle" # all the handles generated logged here
            -R "$raft" # all raft logs will permanently be here
            -I "$node_id" # node ID for RAFT. unique per instance
            -P "$peers" # peer list for RAFT (comma-separated IDs of other nodes)
        )

        if [[ $USE_GDB -eq 1 ]]; then
            echo "[*] inst$i: running under gdb and run"
            gdb -ex run -ex "bt" --args unfsd "${cmd_args[@]}" > "$instdir/unfsd.out" 2>&1 &
        else
            unfsd "${cmd_args[@]}" > "$instdir/unfsd.out" 2>&1 &
        fi

        echo $! >> "$GLOBAL_PIDLIST"
        this_pid=$!
        echo "    inst$i OK  →  share=$share, 
                  pid=$this_pid, 
                  exports=$exports"  
    fi
done

echo
echo "All $NUM instances are up.  Global PID list: $GLOBAL_PIDLIST"

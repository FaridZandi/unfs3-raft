#!/usr/bin/env bash
#
# stop-nfs.sh — Shut down all daemons started by start-nfs.sh
#   sudo ./stop-nfs.sh

set -euo pipefail

WORKDIR=$(pwd)
# user_home=$(eval echo "~$SUDO_USER")  # home of the user who ran the script
# MOUNT_BASE="$user_home/srv/nfs"        # parent for visible shares
MOUNT_BASE=/tmp/farid/srv/nfs          # parent for visible shares


for pidfile in "$WORKDIR"/inst*/unfsd.pid; do
    [[ -e $pidfile ]] || continue
    pid=$(cat "$pidfile")
    echo "[*] killing unfsd pid $pid" in "$pidfile"
    kill "$pid" 2>/dev/null || true
done

echo "-----------------------------"

log() { printf '%s\n' "$*"; }

try_unmount() {
  local mp="$1"
  # Only attempt if it's a mount point
  if command -v findmnt >/dev/null 2>&1; then
    findmnt -rn -T "$mp" >/dev/null 2>&1 || return 0
  else
    mountpoint -q "$mp" 2>/dev/null || return 0
  fi

  log "[*] Unmount $mp"
  # Prefer fusermount3/fusermount if available
  if command -v fusermount3 >/dev/null 2>&1; then
    fusermount3 -u "$mp" 2>/dev/null || true
  elif command -v fusermount >/dev/null 2>&1; then
    fusermount -u "$mp" 2>/dev/null || true
  fi

  # Fallback to umount
  if command -v findmnt >/dev/null 2>&1 && findmnt -rn -T "$mp" >/dev/null 2>&1; then
    umount "$mp" 2>/dev/null || true
  fi
  if command -v findmnt >/dev/null 2>&1 && findmnt -rn -T "$mp" >/dev/null 2>&1; then
    log "    mount busy, lazy unmount $mp"
    umount -l "$mp" 2>/dev/null || true
  fi
}

if [[ -d "$MOUNT_BASE" ]]; then
  for mp in "$MOUNT_BASE"/shared*; do
    [[ -e "$mp" ]] || continue
    try_unmount "$mp"
    if [[ -d "$mp" ]]; then
      echo "[*] removing mount point $mp"
      rm -rf "$mp"
    fi
  done
fi

echo "-----------------------------"

# remove all the instance directories
echo "[*] removing instance directories: $WORKDIR/inst*"
echo "removing: "
find "$WORKDIR" -maxdepth 1 -type d -name 'inst*' -print
rm -rf "$WORKDIR"/inst*
rm -rf "$WORKDIR"/global

echo "All UNFS3 instances stopped and cleaned up."
echo "You can now safely run ./start-nfs.sh to start fresh instances."

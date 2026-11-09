#!/bin/sh
# platform-check.sh -- collect platform facts from a QNX target to diagnose
# "injector can't load" (unknown symbol _ZN8CameraDB11GetInstanceEv ...).
# See docs/adr/0018. Read-only: it changes nothing on the board, except that
# step 9 starts the injector for ~2 s on a scratch camera id and kills it,
# exactly as CamSyringe's own connect-time probe does.
#
# Usage (on the target, as root):   sh platform-check.sh
# Output: /tmp/platform-check-<host>-<date>.log  -- send that file back.

INSTALL=${INSTALL:-/var/opt}
LOG=/tmp/platform-check-$(hostname 2>/dev/null)-$(date +%Y%m%d-%H%M%S 2>/dev/null).log
SYM=_ZN8CameraDB11GetInstanceEv

have() { command -v "$1" >/dev/null 2>&1; }
section() { printf '\n==== %s ====\n' "$*"; }
run() { printf '$ %s\n' "$*"; sh -c "$*" 2>&1; printf '[exit %s]\n' "$?"; }

{
echo "platform-check.sh   $(date 2>/dev/null)"
echo "install dir: $INSTALL   log: $LOG"

section "1. OS / BSP"
run "uname -a"
run "uname -sr"
run "cat /etc/os-release /etc/qnx-release /etc/release /etc/version 2>/dev/null"
run "ls -l /etc/*release* /etc/*version* 2>/dev/null"
run "getconf _CS_ARCH 2>/dev/null; hostname"

section "2. Environment"
run "echo PATH=\$PATH"
run "echo LD_LIBRARY_PATH=\$LD_LIBRARY_PATH"
run "echo LD_PRELOAD=\$LD_PRELOAD"
run "cat $INSTALL/env.sh"

section "3. Installed CamSyringe target programs"
run "ls -l $INSTALL $INSTALL/bin $INSTALL/lib"
run "cat $INSTALL/version.txt $INSTALL/bin/version.txt"
run "ls -l /mnt/bin/camera"

section "4. Camera library directories"
for d in /mnt/lib64/camera /mnt/lib64 /usr/lib /lib /proc/boot $INSTALL/lib; do
    run "ls -l $d | grep -i -E 'qcx|qcarcam|camera|pmem|apdf|slog2|total|^d' "
done

section "5. Which library defines $SYM ?"
echo "(searching by raw string match, works without nm)"
for d in /mnt/lib64/camera /mnt/lib64 /usr/lib /lib /proc/boot $INSTALL/lib $INSTALL/bin /mnt/bin/camera; do
    [ -d "$d" ] || continue
    for f in "$d"/*; do
        [ -f "$f" ] || continue
        grep -l "$SYM" "$f" 2>/dev/null
    done
done
echo "(done; files above mention the symbol -- defining OR merely referencing it)"

section "6. Dynamic section / symbols of the libraries involved"
for f in /mnt/lib64/camera/libqcxplatform.so /mnt/lib64/camera/libqcxclient.so \
         /mnt/lib64/camera/libqcxosal.so /mnt/lib64/camera/libqcx.so; do
    [ -f "$f" ] || { echo "-- $f: not present"; continue; }
    echo "-- $f"
    run "ls -l $f"
    for t in readelf ntoaarch64-readelf aarch64-unknown-nto-qnx8.0.0-readelf; do
        have $t && { run "$t -d $f | grep -E 'NEEDED|SONAME|RPATH|RUNPATH'"; break; }
    done
    for t in nm ntoaarch64-nm aarch64-unknown-nto-qnx8.0.0-nm; do
        have $t && { run "$t -D $f | grep -i -E 'CameraDB|GetInstance'"; break; }
    done
done
run "which readelf nm objdump ldd use pidin strings 2>&1"

section "7. ldd of the target programs (the loader's own verdict)"
for p in qcarcam_injector qcarcam_receiver qcarcam_dispatcher qcarcam_viewer; do
    run "ldd $INSTALL/bin/$p"
done
run "ldd /mnt/bin/camera/qcxserver"
run "ldd /mnt/lib64/camera/libqcxplatform.so"
run "ldd /mnt/lib64/camera/libqcxclient.so"

section "8. Camera stack running now"
run "pidin -F '%a %b %N' 2>/dev/null | grep -i -E 'qcx|qcarcam|camera|screen|io-sock'"
run "ls -l /dev/shmem 2>/dev/null | head -30"

section "9. Injector start probe (2 s, scratch id 99, same as CamSyringe's probe)"
if [ -x $INSTALL/bin/qcarcam_injector ]; then
    $INSTALL/bin/qcarcam_injector 99 --ring-name /camsyringe_platform_check_scratch \
        --width 64 --height 64 2>&1 &
    IP=$!
    sleep 2
    kill -9 $IP 2>/dev/null
    wait $IP 2>/dev/null
else
    echo "$INSTALL/bin/qcarcam_injector not found/executable"
fi

section "10. Same probe with the camera library dir on LD_LIBRARY_PATH"
if [ -x $INSTALL/bin/qcarcam_injector ]; then
    LD_LIBRARY_PATH=/mnt/lib64/camera:$LD_LIBRARY_PATH \
    $INSTALL/bin/qcarcam_injector 99 --ring-name /camsyringe_platform_check_scratch2 \
        --width 64 --height 64 2>&1 &
    IP=$!
    sleep 2
    kill -9 $IP 2>/dev/null
    wait $IP 2>/dev/null
fi

section "11. Recent system log (camera / loader lines)"
run "slog2info 2>/dev/null | grep -i -E 'qcx|camera|ldd|unknown symbol|CameraDB' | tail -40"

echo
echo "==== end ===="
} > "$LOG" 2>&1

echo "Done. Please send this file back:  $LOG"

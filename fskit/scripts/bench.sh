#!/bin/bash
# Quick throughput / metadata benchmark of a mounted volume.
#
#   fskit/scripts/bench.sh /Volumes/Taimir_extern            # 1 GiB sequential, 500 small files
#   fskit/scripts/bench.sh /Volumes/X 4096 2000               # 4 GiB, 2000 files
#
# Writes only inside <mount>/.ttntfs-bench.<pid>/ and removes it afterwards.
# Run it once on our mount (read-write) and, for the read side, once on the
# same disk mounted by Apple's driver for a baseline. The shell running it
# needs "Removable Volumes" access (System Settings > Privacy & Security >
# Files and Folders) when the disk is external.
set -u
MP=${1:?mount point}
MIB=${2:-1024}
NFILES=${3:-500}
DIR="$MP/.ttntfs-bench.$$"
BIG="$DIR/big.bin"
mkdir -p "$DIR" || { echo "cannot create $DIR (permission? read-only?)"; exit 1; }
trap 'rm -rf "$DIR"' EXIT

fs=$(mount | grep " $MP " | sed 's/.*(\(.*\))/\1/')
echo "volume: $MP ($fs)"
echo "sequential: ${MIB} MiB, 1 MiB blocks; small files: $NFILES x 4 KiB"

t() { python3 -c 'import time;print(time.time())'; }
rate() { python3 -c "print(f'{$1/($3-$2):8.1f} MB/s')"; }

echo -n "seq write        : "; s=$(t); dd if=/dev/zero of="$BIG" bs=1m count="$MIB" 2>/dev/null && sync; e=$(t); rate "$MIB*1.048576" "$s" "$e"
# purge caches between phases so the read hits the device, not the page cache
purge 2>/dev/null || sudo -n purge 2>/dev/null || echo "   (purge unavailable, read may hit cache)"
echo -n "seq read         : "; s=$(t); dd if="$BIG" of=/dev/null bs=1m 2>/dev/null; e=$(t); rate "$MIB*1.048576" "$s" "$e"
echo -n "seq re-read      : "; s=$(t); dd if="$BIG" of=/dev/null bs=1m 2>/dev/null; e=$(t); rate "$MIB*1.048576" "$s" "$e"
echo -n "random read 4k   : "; s=$(t); python3 - "$BIG" <<'PY'
import os,random,sys,time
f=os.open(sys.argv[1],os.O_RDONLY); size=os.fstat(f).st_size; n=2000
for _ in range(n):
    os.pread(f,4096,random.randrange(0,size-4096)&~4095)
os.close(f)
PY
e=$(t); python3 -c "print(f'{2000/($e-$s):8.0f} IOPS')"
echo -n "create $NFILES files: "; s=$(t); for i in $(seq 1 "$NFILES"); do head -c 4096 /dev/zero > "$DIR/f$i"; done; sync; e=$(t); python3 -c "print(f'{$NFILES/($e-$s):8.0f} files/s')"
echo -n "stat   $NFILES files: "; s=$(t); ls -l "$DIR" > /dev/null; e=$(t); python3 -c "print(f'{$NFILES/($e-$s):8.0f} files/s')"
echo -n "delete $NFILES files: "; s=$(t); rm "$DIR"/f*; sync; e=$(t); python3 -c "print(f'{$NFILES/($e-$s):8.0f} files/s')"

#!/bin/bash
# cgroup v2 memory.peak 口径的 M（含页缓存）。
# 注意：本内核（6.18）上 "echo 0 > memory.peak" 是空操作，必须 rmdir+mkdir 重建 cgroup 才能得到单轮峰值。
set -uo pipefail
CG=/sys/fs/cgroup/mvdb_m
PROBE=/mnt/e/studyitem/数据库/minivectordb/tools/probe_TBM.py
REAL=${BIN:-/root/build-p5/minivectordb}

echo +memory > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || true
rmdir "$CG" 2>/dev/null || true
mkdir -p "$CG" || { echo "cannot create cgroup"; exit 2; }

cat > /tmp/cg_wrap.sh <<W
#!/bin/bash
echo \$\$ > /sys/fs/cgroup/mvdb_m/cgroup.procs
exec "$REAL" "\$@"
W
chmod +x /tmp/cg_wrap.sh

BIN=/tmp/cg_wrap.sh python3 "$PROBE"
echo "--- cgroup 口径（单轮）---"
echo "memory.peak = $(cat "$CG/memory.peak") B = $(( $(cat "$CG/memory.peak") / 1000000 )) MB"

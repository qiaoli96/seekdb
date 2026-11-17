#!/usr/bin/env python3
"""按赛题口径同时测 T / B / M：T=墙钟，B=设备读写字节，M=进程峰值 RSS(VmHWM)。写/查两个阶段分别统计。"""
import json, os, random, subprocess, sys, time

BIN = os.environ.get("BIN", "/root/build-p4/minivectordb")
DIM, ROWS, BATCH, QUERIES, K = 128, 10000, 100, 1000, 10
CATS = ["c0", "c1", "c2", "c3", "c4"]

def vec(r):
    return "[" + ",".join("%.6f" % r.uniform(-1, 1) for _ in range(DIM)) + "]"

def disk():
    for line in open("/proc/diskstats"):
        f = line.split()
        if len(f) > 9 and f[2] == "sdd":
            return int(f[5]) * 512, int(f[9]) * 512   # read, write bytes
    return 0, 0

def hwm(pid):
    for line in open("/proc/%d/status" % pid):
        if line.startswith("VmHWM:"):
            return int(line.split()[1]) * 1024
    return 0

def main():
    r = random.Random(20261117)
    data_dir = "/tmp/mvdb_tbm"
    os.system("rm -rf " + data_dir)
    p = subprocess.Popen([BIN, "--data-dir", data_dir], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    def req(o):
        p.stdin.write(json.dumps(o) + "\n"); p.stdin.flush()
        return json.loads(p.stdout.readline())
    assert json.loads(p.stdout.readline())["type"] == "ready"
    rid = 0
    def ex(sql):
        nonlocal rid
        rid += 1
        res = req({"id": rid, "op": "execute", "sql": sql, "search_mode": "exact"})
        assert res.get("ok"), (sql[:60], res)
        return res
    ex("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d));" % DIM)
    rows = [(i, CATS[i % 5], vec(r)) for i in range(1, ROWS + 1)]

    r0, w0 = disk(); t0 = time.perf_counter()
    for b in range(0, ROWS, BATCH):
        vals = ",".join("(%d,'%s','%s')" % x for x in rows[b:b + BATCH])
        ex("INSERT INTO docs VALUES " + vals + ";")
    t_w = time.perf_counter() - t0
    r1, w1 = disk(); hwm_w = hwm(p.pid)

    qs = [vec(r) for _ in range(QUERIES)]
    t0 = time.perf_counter()
    for q in range(QUERIES):
        where = " WHERE category = '%s'" % CATS[q % 5] if q % 2 == 0 else ""
        ex("SELECT id, L2_DISTANCE(embedding, '%s') AS d FROM docs%s ORDER BY d LIMIT %d;" % (qs[q], where, K))
    t_q = time.perf_counter() - t0
    r2, w2 = disk(); hwm_q = hwm(p.pid)
    req({"id": rid + 1, "op": "shutdown"}); p.wait(timeout=30)

    print(json.dumps({
        "T_write_s": round(t_w, 3), "T_query_s": round(t_q, 3), "T_query_ms": round(t_q / QUERIES * 1000, 3),
        "B_write_MB": round((w1 - w0) / 1e6, 2), "B_query_read_MB": round((r2 - r1) / 1e6, 2), "B_query_write_MB": round((w2 - w1) / 1e6, 2),
        "M_peak_after_write_MB": round(hwm_w / 1e6, 1), "M_peak_after_query_MB": round(hwm_q / 1e6, 1),
        "data_bytes": sum(os.path.getsize(os.path.join(data_dir, f)) for f in os.listdir(data_dir)),
    }, ensure_ascii=False))

if __name__ == "__main__":
    main()

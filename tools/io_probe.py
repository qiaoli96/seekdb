#!/usr/bin/env python3
"""测一次批量写入真正落到设备上的字节数（/proc/<pid>/io 的 write_bytes），用于评估预分配带来的写放大。"""
import json, os, random, subprocess, sys

BIN = os.environ.get("BIN", "/root/build-p4/minivectordb")
DIM, ROWS, BATCH = 128, 10000, 100

def vec(rnd):
    return "[" + ",".join("%.6f" % rnd.uniform(-1, 1) for _ in range(DIM)) + "]"

def run(tag):
    rnd = random.Random(20261117)
    data_dir = "/tmp/mvdb_io_%s" % tag
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
        r = req({"id": rid, "op": "execute", "sql": sql, "search_mode": "exact"})
        assert r.get("ok"), r
    ex("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d));" % DIM)
    def snap():
        # 设备级统计：sectors written 字段（512B 扇区）
        for line in open("/proc/diskstats"):
            f = line.split()
            if len(f) > 9 and f[2] == "sdd":
                return {"w_sectors": int(f[9]), "r_sectors": int(f[5])}
        return {"w_sectors": 0, "r_sectors": 0}

    before = snap()
    for b in range(0, ROWS, BATCH):
        vals = ",".join("(%d,'c%d','%s')" % (i, i % 5, vec(rnd)) for i in range(b + 1, b + BATCH + 1))
        ex("INSERT INTO docs VALUES " + vals + ";")
    after = snap()
    req({"id": rid + 1, "op": "shutdown"})
    p.wait(timeout=30)
    size = sum(os.path.getsize(os.path.join(data_dir, f)) for f in os.listdir(data_dir))
    dw = (after["w_sectors"] - before["w_sectors"]) * 512
    print(json.dumps({"tag": tag, "data_bytes_final": size,
                      "device_write_bytes": dw,
                      "amplification": round(dw / max(1, size), 2)}))

if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "new")

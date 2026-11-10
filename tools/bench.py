#!/usr/bin/env python3
"""性能基线：按赛题 3.3 节口径（10,000 条 x 128 维，100 批写入，1,000 次 TopK，K=10，半数带过滤）"""
import json, os, random, subprocess, sys, time, statistics

BIN = os.environ.get("BIN", "/root/build/minivectordb")
DIM, ROWS, BATCH, QUERIES, K = 128, 10000, 100, 1000, 10
CATS = ["c0", "c1", "c2", "c3", "c4"]

def vec(rnd):
    return "[" + ",".join("%.6f" % rnd.uniform(-1, 1) for _ in range(DIM)) + "]"

def main():
    rnd = random.Random(20261109)
    data_dir = "/tmp/mvdb_bench_data"
    os.system("rm -rf " + data_dir)
    p = subprocess.Popen([BIN, "--data-dir", data_dir], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    def req(obj):
        p.stdin.write(json.dumps(obj, ensure_ascii=False) + "\n"); p.stdin.flush()
        return json.loads(p.stdout.readline())
    assert json.loads(p.stdout.readline())["type"] == "ready"
    rid = 0
    def ex(sql):
        nonlocal rid
        rid += 1
        r = req({"id": rid, "op": "execute", "sql": sql, "search_mode": "exact"})
        assert r.get("ok"), (sql[:80], r)
        return r

    ex("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d));" % DIM)
    rows = [(i, CATS[i % 5], vec(rnd)) for i in range(1, ROWS + 1)]

    # 写入阶段：每批 100 条，逐批提交
    t0 = time.perf_counter()
    for b in range(0, ROWS, BATCH):
        chunk = rows[b:b + BATCH]
        vals = ",".join("(%d,'%s','%s')" % (r[0], r[1], r[2]) for r in chunk)
        ex("INSERT INTO docs VALUES " + vals + ";")
    t_write = time.perf_counter() - t0

    # 查询阶段：1000 次 TopK，半数带等值过滤
    qvecs = [vec(rnd) for _ in range(QUERIES)]
    t0 = time.perf_counter()
    for q in range(QUERIES):
        filtered = q % 2 == 0
        where = " WHERE category = '%s'" % CATS[q % 5] if filtered else ""
        ex("SELECT id, L2_DISTANCE(embedding, '%s') AS d FROM docs%s ORDER BY d LIMIT %d;" % (qvecs[q], where, K))
    t_query = time.perf_counter() - t0

    pid = rid + 1
    req({"id": pid, "op": "shutdown"})
    p.wait(timeout=30)

    size = sum(os.path.getsize(os.path.join(data_dir, f)) for f in os.listdir(data_dir))
    print(json.dumps({"write_s": round(t_write, 3), "write_per_batch_ms": round(t_write / (ROWS / BATCH) * 1000, 3),
                      "query_s": round(t_query, 3), "query_per_op_ms": round(t_query / QUERIES * 1000, 3),
                      "data_bytes": size, "rows": ROWS, "dim": DIM, "queries": QUERIES, "k": K}, ensure_ascii=False))

if __name__ == "__main__":
    main()

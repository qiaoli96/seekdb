#!/usr/bin/env python3
"""把查询拆成 无过滤/有过滤 两组，并测单次延迟；同时看第 1 次（建 arena）与稳态的差异。"""
import json, os, random, subprocess, time
BIN = os.environ.get("BIN", "/root/bv/minivectordb")
DIM, ROWS, BATCH, N, K = 128, 10000, 100, 500, 10
CATS = ["c0","c1","c2","c3","c4"]
def vec(r): return "[" + ",".join("%.6f" % r.uniform(-1,1) for _ in range(DIM)) + "]"
r = random.Random(7); dd = "/tmp/mvdb_split"; os.system("rm -rf " + dd)
p = subprocess.Popen([BIN,"--data-dir",dd], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
def req(o):
    p.stdin.write(json.dumps(o)+"\n"); p.stdin.flush(); return json.loads(p.stdout.readline())
assert json.loads(p.stdout.readline())["type"]=="ready"
rid=0
def ex(sql):
    global rid; rid+=1
    res=req({"id":rid,"op":"execute","sql":sql,"search_mode":"exact"}); assert res.get("ok"),(sql[:50],res); return res
ex("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d));"%DIM)
rows=[(i,CATS[i%5],vec(r)) for i in range(1,ROWS+1)]
for b in range(0,ROWS,BATCH):
    ex("INSERT INTO docs VALUES "+",".join("(%d,'%s','%s')"%x for x in rows[b:b+BATCH])+";")
qs=[vec(r) for _ in range(N)]
def run(filt):
    t0=time.perf_counter()
    for i in range(N):
        w = " WHERE category = '%s'"%CATS[i%5] if filt else ""
        ex("SELECT id, L2_DISTANCE(embedding,'%s') AS d FROM docs%s ORDER BY d LIMIT %d;"%(qs[i],w,K))
    return (time.perf_counter()-t0)/N*1000
first = time.perf_counter(); ex("SELECT id FROM docs ORDER BY id LIMIT 1;"); first=time.perf_counter()-first
u1=run(False); f1=run(True); u2=run(False); f2=run(True)
req({"id":rid+1,"op":"shutdown"}); p.wait(timeout=30)
print(json.dumps({"first_query_ms":round(first*1000,3),
  "unfiltered_ms":[round(u1,3),round(u2,3)],"filtered_ms":[round(f1,3),round(f2,3)]}))

#!/usr/bin/env python3
"""Generate a SQL file that fills a vector table with random float32 vectors.

The file is meant to be piped into the seekdb (MySQL protocol) client:

    python3 gen_vec_data.py --rows 200000 --dim 256 > /tmp/vectors.sql

It prints one CREATE-free INSERT per batch, so no server side function is
needed and the load is a single stream of statements.
"""
import argparse
import random
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=200000)
    ap.add_argument("--dim", type=int, default=256)
    ap.add_argument("--table", default="bench_vec")
    ap.add_argument("--batch", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=20261110)
    args = ap.parse_args()

    rnd = random.Random(args.seed)
    out = sys.stdout
    out.write("SET autocommit=0;\n")
    out.write("SET unique_checks=0;\n")

    row = 0
    while row < args.rows:
        n = min(args.batch, args.rows - row)
        values = []
        for i in range(n):
            v = ",".join("%.5f" % rnd.uniform(-1.0, 1.0) for _ in range(args.dim))
            values.append("(%d,'[%s]')" % (row + i, v))
        out.write("INSERT INTO %s (id, v) VALUES %s;\n" % (args.table, ",".join(values)))
        row += n
        if row % (args.batch * 20) == 0:
            out.write("COMMIT;\n")
            out.flush()
    out.write("COMMIT;\n")
    out.write("ANALYZE TABLE %s;\n" % args.table)


if __name__ == "__main__":
    main()

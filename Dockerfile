FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN set -eux; \
    (apt-get update -o Acquire::Retries=3 \
     && apt-get install -y --no-install-recommends g++) || { \
      sed -i -e 's@http://archive.ubuntu.com@http://mirrors.aliyun.com@g' \
             -e 's@http://security.ubuntu.com@http://mirrors.aliyun.com@g' \
             /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null || true; \
      apt-get update -o Acquire::Retries=3; \
      apt-get install -y --no-install-recommends g++; \
    }; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY minivectordb/ ./minivectordb/
RUN mkdir -p minivectordb/bin && \
    g++ -O3 -std=c++17 -DNDEBUG -static-libstdc++ -static-libgcc \
        -o minivectordb/bin/mvd \
        minivectordb/src/main.cpp minivectordb/src/db.cpp minivectordb/src/parser.cpp

# 运行时只依赖 libc/libm（libstdc++/libgcc 已静态链入），因此该阶段无需联网安装依赖。
FROM ubuntu:24.04

RUN mkdir -p /data

COPY --from=build /build/minivectordb/bin/mvd /usr/local/bin/mvd

ENV VECTORDB_DATA_DIR=/data
ENTRYPOINT ["/usr/local/bin/mvd"]
CMD ["/data"]

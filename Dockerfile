# 构建阶段：装编译工具链，产出 Release 二进制。
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
# archive.ubuntu.com 在本机实测 ~2.1MB/s（apt-get update 15s），未换国内源。
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
# 并发固定为 2：分值容器里 nproc 可能是宿主全核数，-j"$(nproc)" 容易 OOM。
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j2

# 运行阶段：只带二进制与 C++ 运行时（ubuntu:24.04 自带 libstdc++6 / libgcc-s1）。
FROM ubuntu:24.04

# 判题以 root 运行且根文件系统只读：数据目录必须独立可写。
RUN mkdir -p /data
WORKDIR /app
COPY --from=build /src/build/minivectordb /app/minivectordb

VOLUME ["/data"]
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]

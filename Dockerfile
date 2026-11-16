FROM ubuntu:24.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY minivectordb/ ./minivectordb/
RUN make -C minivectordb \
      CXXFLAGS="-O3 -std=c++17 -DNDEBUG -static-libstdc++ -static-libgcc" \
      LDFLAGS=""

FROM ubuntu:24.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /data

COPY --from=build /build/minivectordb/bin/mvd /usr/local/bin/mvd

ENV VECTORDB_DATA_DIR=/data
ENTRYPOINT ["/usr/local/bin/mvd"]

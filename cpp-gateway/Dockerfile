FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake g++ make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DGATEWAY_WARNINGS_AS_ERRORS=ON \
    && cmake --build build --parallel

FROM ubuntu:22.04

RUN groupadd --system --gid 10001 gateway \
    && useradd --system --uid 10001 --gid gateway --home-dir /nonexistent --shell /usr/sbin/nologin gateway

WORKDIR /app
COPY --from=build --chown=gateway:gateway /app/build/message_server /app/message_server

EXPOSE 9000
USER 10001:10001
CMD ["/app/message_server"]

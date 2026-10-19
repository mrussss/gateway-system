FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
ARG UBUNTU_MIRROR=""
RUN if [ -n "$UBUNTU_MIRROR" ]; then \
      sed -i \
        -e "s|http://archive.ubuntu.com/ubuntu|${UBUNTU_MIRROR%/}|g" \
        -e "s|http://security.ubuntu.com/ubuntu|${UBUNTU_MIRROR%/}|g" \
        /etc/apt/sources.list; \
    fi \
    && apt-get -o Acquire::Retries=3 update \
    && apt-get -o Acquire::Retries=3 install -y --no-install-recommends cmake g++ make ca-certificates \
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

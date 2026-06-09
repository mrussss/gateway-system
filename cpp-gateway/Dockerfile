FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake g++ make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src

RUN cmake -S . -B build && cmake --build build

FROM ubuntu:22.04

WORKDIR /app
COPY --from=build /app/build/message_server /app/message_server

EXPOSE 9000
CMD ["/app/message_server"]

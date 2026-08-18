FROM golang:1.22-bookworm AS build

WORKDIR /app
COPY go.mod go.sum ./
RUN go mod download
COPY . ./

RUN go test ./... && go build -o /out/control-plane ./cmd/control-plane

FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system --gid 10001 control-plane \
    && useradd --system --uid 10001 --gid control-plane --home-dir /nonexistent --shell /usr/sbin/nologin control-plane

WORKDIR /app
COPY --from=build --chown=control-plane:control-plane /out/control-plane /app/control-plane

EXPOSE 8080
USER 10001:10001
CMD ["/app/control-plane"]

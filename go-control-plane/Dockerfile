FROM golang:1.22-bookworm AS build

WORKDIR /app
COPY go.mod ./
COPY *.go ./

RUN go test ./... && go build -o /out/control-plane .

FROM debian:bookworm-slim

WORKDIR /app
COPY --from=build /out/control-plane /app/control-plane

EXPOSE 8080
CMD ["/app/control-plane"]

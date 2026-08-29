# syntax=docker/dockerfile:1

FROM golang:1.23-alpine AS build
WORKDIR /src

COPY go.mod go.sum ./
RUN go mod download

COPY . .
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 \
    go build -trimpath -ldflags="-s -w" -o /out/desklink-signal .

FROM alpine:3.22
RUN apk add --no-cache ca-certificates \
    && addgroup -S desklink \
    && adduser -S -G desklink -H -s /sbin/nologin desklink

COPY --from=build /out/desklink-signal /usr/local/bin/desklink-signal

USER desklink:desklink
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/desklink-signal"]

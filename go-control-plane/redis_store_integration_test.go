package main

import (
	"context"
	"os"
	"testing"
	"time"
)

func TestRedisGatewayStateContract(t *testing.T) {
	addr := os.Getenv("REDIS_TEST_ADDR")
	if addr == "" {
		t.Skip("REDIS_TEST_ADDR is not set")
	}
	storage := newRedisStore(addr)
	defer storage.Close()
	ctx := context.Background()
	if err := storage.client.FlushDB(ctx).Err(); err != nil {
		t.Fatal(err)
	}

	status, err := storage.saveMetrics(metricsReportRequest{GatewayID: "gateway-1", GatewayBootID: "boot-1", ActiveConnections: 3, TotalMessages: 9, Timestamp: time.Now().Unix()})
	if err != nil {
		t.Fatal(err)
	}
	if status.GatewayBootID != "boot-1" {
		t.Fatalf("unexpected status: %+v", status)
	}
	if kind, err := storage.client.Type(ctx, gatewayStatusKey("gateway-1")).Result(); err != nil || kind != "hash" {
		t.Fatalf("status type=%q err=%v", kind, err)
	}
	if ttl, err := storage.client.TTL(ctx, gatewayStatusKey("gateway-1")).Result(); err != nil || ttl <= 0 {
		t.Fatalf("status ttl=%s err=%v", ttl, err)
	}
	scoreBefore, err := storage.client.ZScore(ctx, "gateway:index", "gateway-1").Result()
	if err != nil {
		t.Fatal(err)
	}

	if err := storage.saveClients("gateway-1", []clientInfo{{ClientID: "client-1"}}); err != nil {
		t.Fatal(err)
	}
	scoreAfter, err := storage.client.ZScore(ctx, "gateway:index", "gateway-1").Result()
	if err != nil {
		t.Fatal(err)
	}
	if scoreBefore != scoreAfter {
		t.Fatalf("client snapshot refreshed online score: %f -> %f", scoreBefore, scoreAfter)
	}
	if ttl, err := storage.client.TTL(ctx, gatewayClientsKey("gateway-1")).Result(); err != nil || ttl <= 0 || ttl > defaultClientSnapshotTTL {
		t.Fatalf("client ttl=%s err=%v", ttl, err)
	}
}

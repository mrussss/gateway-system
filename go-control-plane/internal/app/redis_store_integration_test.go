package app

import (
	"context"
	"os"
	"sync"
	"sync/atomic"
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

func TestRedisConfigCASContract(t *testing.T) {
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
	config, err := storage.getConfig()
	if err != nil {
		t.Fatal(err)
	}
	if config.Version != 1 || config.RequestQueueCapacityDisplay != 4096 {
		t.Fatalf("unexpected initial config %+v", config)
	}
	if kind, _ := storage.client.Type(ctx, "config:active").Result(); kind != "hash" {
		t.Fatalf("config type %q", kind)
	}

	var successes atomic.Int32
	var wait sync.WaitGroup
	for index := 0; index < 20; index++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			if _, err := storage.updateConfig(1, validConfigUpdate()); err == nil {
				successes.Add(1)
			}
		}()
	}
	wait.Wait()
	if successes.Load() != 1 {
		t.Fatalf("expected one Redis CAS success, got %d", successes.Load())
	}
	config, err = storage.getConfig()
	if err != nil {
		t.Fatal(err)
	}
	if config.Version != 2 || config.MaxConnectionsPerClient != 4 || config.RequestQueueCapacityDisplay != 4096 {
		t.Fatalf("unexpected config %+v", config)
	}
}

func TestRedisAuthFailureLimiterIsAtomicAndExpiring(t *testing.T) {
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

	const attempts = 32
	var wait sync.WaitGroup
	wait.Add(attempts)
	for i := 0; i < attempts; i++ {
		go func() {
			defer wait.Done()
			if _, err := storage.recordAuthFailure("client-1", time.Second); err != nil {
				t.Errorf("record auth failure: %v", err)
			}
		}()
	}
	wait.Wait()
	limited, err := storage.authFailureLimited("client-1", attempts)
	if err != nil || !limited {
		t.Fatalf("expected limiter after %d attempts, limited=%v err=%v", attempts, limited, err)
	}
	if ttl, err := storage.client.TTL(ctx, authFailureKey("client-1")).Result(); err != nil || ttl <= 0 || ttl > time.Second {
		t.Fatalf("auth failure ttl=%s err=%v", ttl, err)
	}
	if err := storage.clearAuthFailures("client-1"); err != nil {
		t.Fatal(err)
	}
	limited, err = storage.authFailureLimited("client-1", 1)
	if err != nil || limited {
		t.Fatalf("expected successful clear, limited=%v err=%v", limited, err)
	}
}

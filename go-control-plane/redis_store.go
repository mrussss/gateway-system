package main

import (
	"context"
	"encoding/json"
	"errors"
	"log"
	"sort"
	"time"

	"github.com/redis/go-redis/v9"
)

type redisStore struct {
	client *redis.Client
}

func newRedisStore(addr string) *redisStore {
	client := redis.NewClient(&redis.Options{
		Addr: addr,
	})

	ctx, cancel := redisContext()
	defer cancel()
	if err := client.Ping(ctx).Err(); err != nil {
		log.Fatalf("redis unavailable: %v", err)
	}

	return &redisStore{client: client}
}

func (s *redisStore) saveMetrics(req metricsReportRequest) (gatewayStatusResponse, error) {
	ctx, cancel := redisContext()
	defer cancel()

	status := statusFromMetrics(req)
	if err := s.setJSON(ctx, gatewayStatusKey(req.GatewayID), status); err != nil {
		return gatewayStatusResponse{}, err
	}
	if err := s.client.SAdd(ctx, "gateways", req.GatewayID).Err(); err != nil {
		return gatewayStatusResponse{}, err
	}
	if err := s.setJSON(ctx, "gateway:status", status); err != nil {
		return gatewayStatusResponse{}, err
	}
	return status, nil
}

func (s *redisStore) getStatus() (gatewayStatusResponse, bool, error) {
	ctx, cancel := redisContext()
	defer cancel()
	return s.getStatusByKey(ctx, "gateway:status")
}

func (s *redisStore) saveClients(gatewayID string, clients []clientInfo) error {
	ctx, cancel := redisContext()
	defer cancel()

	if err := s.setJSON(ctx, gatewayClientsKey(gatewayID), clients); err != nil {
		return err
	}
	if err := s.client.SAdd(ctx, "gateways", gatewayID).Err(); err != nil {
		return err
	}
	return s.setJSON(ctx, "clients:current", clients)
}

func (s *redisStore) getClients() ([]clientInfo, error) {
	ctx, cancel := redisContext()
	defer cancel()
	return s.getClientsByKey(ctx, "clients:current")
}

func (s *redisStore) listGateways() ([]gatewayStatusResponse, error) {
	ctx, cancel := redisContext()
	defer cancel()

	gatewayIDs, err := s.client.SMembers(ctx, "gateways").Result()
	if err != nil {
		return nil, err
	}
	sort.Strings(gatewayIDs)

	statuses := make([]gatewayStatusResponse, 0, len(gatewayIDs))
	for _, gatewayID := range gatewayIDs {
		status, ok, err := s.getStatusByKey(ctx, gatewayStatusKey(gatewayID))
		if err != nil {
			return nil, err
		}
		if ok {
			statuses = append(statuses, status)
		}
	}
	sortGatewayStatuses(statuses)
	return statuses, nil
}

func (s *redisStore) getGatewayStatus(gatewayID string) (gatewayStatusResponse, bool, error) {
	ctx, cancel := redisContext()
	defer cancel()
	return s.getStatusByKey(ctx, gatewayStatusKey(gatewayID))
}

func (s *redisStore) getGatewayClients(gatewayID string) ([]clientInfo, bool, error) {
	ctx, cancel := redisContext()
	defer cancel()

	raw, err := s.client.Get(ctx, gatewayClientsKey(gatewayID)).Result()
	if errors.Is(err, redis.Nil) {
		return nil, false, nil
	}
	if err != nil {
		return nil, false, err
	}

	var clients []clientInfo
	if err := json.Unmarshal([]byte(raw), &clients); err != nil {
		return nil, false, err
	}
	return clients, true, nil
}

func (s *redisStore) setToken(clientID string, token string) error {
	ctx, cancel := redisContext()
	defer cancel()
	if err := s.client.Set(ctx, "token:"+clientID, token, 0).Err(); err != nil {
		return err
	}
	return s.client.SAdd(ctx, "tokens", clientID).Err()
}

func (s *redisStore) deleteToken(clientID string) error {
	ctx, cancel := redisContext()
	defer cancel()
	if err := s.client.Del(ctx, "token:"+clientID).Err(); err != nil {
		return err
	}
	return s.client.SRem(ctx, "tokens", clientID).Err()
}

func (s *redisStore) isAllowed(clientID string, token string) (bool, error) {
	ctx, cancel := redisContext()
	defer cancel()
	raw, err := s.client.Get(ctx, "token:"+clientID).Result()
	if errors.Is(err, redis.Nil) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	return raw == token, nil
}

func (s *redisStore) listTokens() ([]tokenEntry, error) {
	ctx, cancel := redisContext()
	defer cancel()
	clientIDs, err := s.client.SMembers(ctx, "tokens").Result()
	if err != nil {
		return nil, err
	}

	sort.Strings(clientIDs)
	entries := make([]tokenEntry, 0, len(clientIDs))
	for _, clientID := range clientIDs {
		entries = append(entries, tokenEntry{ClientID: clientID})
	}
	return entries, nil
}

func (s *redisStore) getConfig() (runtimeConfig, error) {
	ctx, cancel := redisContext()
	defer cancel()

	raw, err := s.client.Get(ctx, "config:current").Result()
	if errors.Is(err, redis.Nil) {
		cfg := defaultRuntimeConfig()
		if err := s.setJSON(ctx, "config:current", cfg); err != nil {
			return runtimeConfig{}, err
		}
		return cfg, nil
	}
	if err != nil {
		return runtimeConfig{}, err
	}

	var cfg runtimeConfig
	if err := json.Unmarshal([]byte(raw), &cfg); err != nil {
		return runtimeConfig{}, err
	}
	return cfg, nil
}

func (s *redisStore) updateConfig(req configUpdateRequest) (runtimeConfig, error) {
	current, err := s.getConfig()
	if err != nil {
		return runtimeConfig{}, err
	}

	cfg := runtimeConfig{
		Version:                       current.Version + 1,
		AuthTimeoutMS:                 req.AuthTimeoutMS,
		MaxPayloadSize:                req.MaxPayloadSize,
		MaxConnectionsPerClient:       req.MaxConnectionsPerClient,
		MaxRequestsPerClientPerSecond: req.MaxRequestsPerClientPerSecond,
		FailOpen:                      req.FailOpen,
	}

	ctx, cancel := redisContext()
	defer cancel()
	if err := s.setJSON(ctx, "config:current", cfg); err != nil {
		return runtimeConfig{}, err
	}
	return cfg, nil
}

func (s *redisStore) getStatusByKey(ctx context.Context, key string) (gatewayStatusResponse, bool, error) {
	raw, err := s.client.Get(ctx, key).Result()
	if errors.Is(err, redis.Nil) {
		return gatewayStatusResponse{}, false, nil
	}
	if err != nil {
		return gatewayStatusResponse{}, false, err
	}

	var status gatewayStatusResponse
	if err := json.Unmarshal([]byte(raw), &status); err != nil {
		return gatewayStatusResponse{}, false, err
	}
	return status, true, nil
}

func (s *redisStore) getClientsByKey(ctx context.Context, key string) ([]clientInfo, error) {
	raw, err := s.client.Get(ctx, key).Result()
	if errors.Is(err, redis.Nil) {
		return []clientInfo{}, nil
	}
	if err != nil {
		return nil, err
	}

	var clients []clientInfo
	if err := json.Unmarshal([]byte(raw), &clients); err != nil {
		return nil, err
	}
	return clients, nil
}

func (s *redisStore) setJSON(ctx context.Context, key string, value any) error {
	payload, err := json.Marshal(value)
	if err != nil {
		return err
	}
	return s.client.Set(ctx, key, payload, 0).Err()
}

func redisContext() (context.Context, context.CancelFunc) {
	return context.WithTimeout(context.Background(), 2*time.Second)
}

func gatewayStatusKey(gatewayID string) string {
	return "gateway:status:" + gatewayID
}

func gatewayClientsKey(gatewayID string) string {
	return "gateway:clients:" + gatewayID
}

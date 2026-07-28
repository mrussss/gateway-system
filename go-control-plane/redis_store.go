package main

import (
	"context"
	"encoding/json"
	"errors"
	"sort"
	"strconv"
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

	return &redisStore{client: client}
}

func (s *redisStore) Ping(ctx context.Context) error { return s.client.Ping(ctx).Err() }

func (s *redisStore) Close() error { return s.client.Close() }

func (s *redisStore) saveMetrics(req metricsReportRequest) (gatewayStatusResponse, error) {
	ctx, cancel := redisContext()
	defer cancel()

	status := statusFromMetrics(req)
	reportedAt, _ := time.Parse(time.RFC3339, status.LastReportTime)
	pipe := s.client.Pipeline()
	pipe.HSet(ctx, gatewayStatusKey(req.GatewayID), statusToRedis(status))
	pipe.Expire(ctx, gatewayStatusKey(req.GatewayID), defaultGatewayStatusTTL)
	pipe.ZAdd(ctx, "gateway:index", redis.Z{Score: float64(reportedAt.Unix()), Member: req.GatewayID})
	if _, err := pipe.Exec(ctx); err != nil {
		return gatewayStatusResponse{}, err
	}
	return status, nil
}

func (s *redisStore) getStatus() (gatewayStatusResponse, bool, error) {
	ctx, cancel := redisContext()
	defer cancel()
	ids, err := s.client.ZRevRange(ctx, "gateway:index", 0, 0).Result()
	if err != nil || len(ids) == 0 {
		return gatewayStatusResponse{}, false, err
	}
	return s.getStatusByKey(ctx, gatewayStatusKey(ids[0]))
}

func (s *redisStore) saveClients(gatewayID string, clients []clientInfo) error {
	ctx, cancel := redisContext()
	defer cancel()

	payload, err := json.Marshal(clients)
	if err != nil {
		return err
	}
	pipe := s.client.Pipeline()
	pipe.Set(ctx, gatewayClientsKey(gatewayID), payload, defaultClientSnapshotTTL)
	pipe.Set(ctx, "clients:current", payload, defaultClientSnapshotTTL)
	_, err = pipe.Exec(ctx)
	return err
}

func (s *redisStore) getClients() ([]clientInfo, error) {
	ctx, cancel := redisContext()
	defer cancel()
	return s.getClientsByKey(ctx, "clients:current")
}

func (s *redisStore) listGateways() ([]gatewayStatusResponse, error) {
	ctx, cancel := redisContext()
	defer cancel()

	gatewayIDs, err := s.client.ZRevRange(ctx, "gateway:index", 0, -1).Result()
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
		} else {
			_ = s.client.ZRem(ctx, "gateway:index", gatewayID).Err()
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
	now := nowRFC3339()
	return s.createToken(tokenRecord{tokenEntry: tokenEntry{ClientID: clientID, Generation: 1, CreatedAt: now, UpdatedAt: now}, Digest: tokenServiceFromEnv().digest(token)})
}

func (s *redisStore) deleteToken(clientID string) error {
	return s.disableToken(clientID, nowRFC3339())
}

func (s *redisStore) isAllowed(clientID string, token string) (bool, error) {
	return s.isDigestAllowed(clientID, tokenServiceFromEnv().digest(token))
}

func (s *redisStore) isDigestAllowed(clientID, digest string) (bool, error) {
	ctx, cancel := redisContext()
	defer cancel()
	values, err := s.client.HMGet(ctx, "token:"+clientID, "digest", "disabled").Result()
	if err != nil {
		return false, err
	}
	if len(values) != 2 || values[0] == nil {
		return false, nil
	}
	disabled := values[1] != nil && values[1].(string) == "1"
	return !disabled && digestEqual(values[0].(string), digest), nil
}

func (s *redisStore) createToken(record tokenRecord) error {
	ctx, cancel := redisContext()
	defer cancel()
	key := "token:" + record.ClientID
	script := redis.NewScript(`if redis.call('EXISTS', KEYS[1]) == 1 then return 0 end redis.call('HSET', KEYS[1], 'digest', ARGV[1], 'generation', 1, 'created_at', ARGV[2], 'updated_at', ARGV[2], 'disabled', 0) redis.call('SADD', KEYS[2], ARGV[3]) return 1`)
	created, err := script.Run(ctx, s.client, []string{key, "token:index"}, record.Digest, record.CreatedAt, record.ClientID).Int()
	if err != nil {
		return err
	}
	if created == 0 {
		return errTokenExists
	}
	return nil
}

func (s *redisStore) rotateToken(clientID string, expected int64, digest, updatedAt string) (tokenRecord, error) {
	ctx, cancel := redisContext()
	defer cancel()
	script := redis.NewScript(`local current=redis.call('HGET',KEYS[1],'generation'); if not current then return -1 end; if tonumber(current)~=tonumber(ARGV[1]) then return 0 end; local next=tonumber(current)+1; redis.call('HSET',KEYS[1],'digest',ARGV[2],'generation',next,'updated_at',ARGV[3],'disabled',0); return next`)
	next, err := script.Run(ctx, s.client, []string{"token:" + clientID}, expected, digest, updatedAt).Int64()
	if err != nil {
		return tokenRecord{}, err
	}
	if next == -1 {
		return tokenRecord{}, errTokenNotFound
	}
	if next == 0 {
		return tokenRecord{}, errTokenConflict
	}
	return tokenRecord{tokenEntry: tokenEntry{ClientID: clientID, Generation: next, UpdatedAt: updatedAt}, Digest: digest}, nil
}

func (s *redisStore) disableToken(clientID, updatedAt string) error {
	ctx, cancel := redisContext()
	defer cancel()
	changed, err := s.client.HSet(ctx, "token:"+clientID, "disabled", 1, "updated_at", updatedAt).Result()
	if err != nil {
		return err
	}
	if changed == 0 {
		exists, err := s.client.Exists(ctx, "token:"+clientID).Result()
		if err != nil {
			return err
		}
		if exists == 0 {
			return errTokenNotFound
		}
	}
	return nil
}

func (s *redisStore) listTokens() ([]tokenEntry, error) {
	ctx, cancel := redisContext()
	defer cancel()
	clientIDs, err := s.client.SMembers(ctx, "token:index").Result()
	if err != nil {
		return nil, err
	}

	sort.Strings(clientIDs)
	entries := make([]tokenEntry, 0, len(clientIDs))
	for _, clientID := range clientIDs {
		values, err := s.client.HGetAll(ctx, "token:"+clientID).Result()
		if err != nil {
			return nil, err
		}
		if len(values) == 0 {
			continue
		}
		generation, _ := strconv.ParseInt(values["generation"], 10, 64)
		entries = append(entries, tokenEntry{ClientID: clientID, Generation: generation, CreatedAt: values["created_at"], UpdatedAt: values["updated_at"], Disabled: values["disabled"] == "1"})
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
	values, err := s.client.HGetAll(ctx, key).Result()
	if err == nil && len(values) == 0 {
		return gatewayStatusResponse{}, false, nil
	}
	if err != nil {
		return gatewayStatusResponse{}, false, err
	}
	return statusFromRedis(values), true, nil
}

func statusToRedis(status gatewayStatusResponse) map[string]any {
	return map[string]any{
		"gateway_id": status.GatewayID, "gateway_boot_id": status.GatewayBootID,
		"process_start_time": status.ProcessStartTime, "active_connections": status.ActiveConnections,
		"total_requests": status.TotalMessages, "bytes_in": status.BytesIn, "bytes_out": status.BytesOut,
		"errors": status.ErrorCount, "request_queue_capacity": status.RequestQueueCapacity,
		"request_queue_backlog": status.RequestQueueBacklog, "request_queue_peak": status.RequestQueuePeak,
		"request_queue_rejected": status.RequestQueueRejected, "response_queue_capacity": status.ResponseQueueCapacity,
		"response_queue_backlog": status.ResponseQueueBacklog, "response_queue_peak": status.ResponseQueuePeak,
		"response_queue_rejected": status.ResponseQueueRejected, "slow_client_closed": status.SlowClientClosed,
		"stale_response_dropped": status.StaleResponseDropped, "auth_success": status.AuthSuccess,
		"auth_failure": status.AuthFailure, "runtime_config_version": status.RuntimeConfigVersion,
		"server_state": status.ServerState, "reported_at": status.LastReportTime,
	}
}

func redisInt(values map[string]string, key string) int64 {
	value, _ := strconv.ParseInt(values[key], 10, 64)
	return value
}

func statusFromRedis(v map[string]string) gatewayStatusResponse {
	return gatewayStatusResponse{
		GatewayID: v["gateway_id"], GatewayBootID: v["gateway_boot_id"], ProcessStartTime: redisInt(v, "process_start_time"),
		ActiveConnections: redisInt(v, "active_connections"), TotalMessages: redisInt(v, "total_requests"), BytesIn: redisInt(v, "bytes_in"), BytesOut: redisInt(v, "bytes_out"), ErrorCount: redisInt(v, "errors"),
		RequestQueueCapacity: redisInt(v, "request_queue_capacity"), RequestQueueBacklog: redisInt(v, "request_queue_backlog"), RequestQueuePeak: redisInt(v, "request_queue_peak"), RequestQueueRejected: redisInt(v, "request_queue_rejected"),
		ResponseQueueCapacity: redisInt(v, "response_queue_capacity"), ResponseQueueBacklog: redisInt(v, "response_queue_backlog"), ResponseQueuePeak: redisInt(v, "response_queue_peak"), ResponseQueueRejected: redisInt(v, "response_queue_rejected"),
		SlowClientClosed: redisInt(v, "slow_client_closed"), StaleResponseDropped: redisInt(v, "stale_response_dropped"), AuthSuccess: redisInt(v, "auth_success"), AuthFailure: redisInt(v, "auth_failure"), RuntimeConfigVersion: redisInt(v, "runtime_config_version"), ServerState: v["server_state"], LastReportTime: v["reported_at"],
	}
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

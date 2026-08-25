package app

import (
	"context"
	"errors"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/redis/go-redis/v9"
)

type redisStore struct {
	client *redis.Client
}

func newRedisStore(addr string) *redisStore {
	return &redisStore{client: redis.NewClient(&redis.Options{Addr: addr})}
}

func (s *redisStore) Ping(ctx context.Context) error { return s.client.Ping(ctx).Err() }

func (s *redisStore) Close() error { return s.client.Close() }

func (s *redisStore) verifyDigest(clientID, digest string) (tokenAuthDecision, error) {
	ctx, cancel := redisContext()
	defer cancel()
	values, err := s.client.HMGet(ctx, "token:"+clientID, "digest", "disabled").Result()
	if err != nil {
		return tokenAuthInvalid, err
	}
	if len(values) != 2 || values[0] == nil {
		return tokenAuthInvalid, nil
	}
	digestValue, ok := values[0].(string)
	if !ok {
		return tokenAuthInvalid, errors.New("invalid token digest")
	}
	disabled, ok := values[1].(string)
	if values[1] != nil && !ok {
		return tokenAuthInvalid, errors.New("invalid token disabled flag")
	}
	if disabled == "1" {
		return tokenAuthDisabled, nil
	}
	if !digestEqual(digestValue, digest) {
		return tokenAuthInvalid, nil
	}
	return tokenAuthAllowed, nil
}

func authFailureKey(clientID string) string { return "auth:failures:{" + clientID + "}" }

func (s *redisStore) authFailureLimited(clientID string, limit int64) (bool, error) {
	ctx, cancel := redisContext()
	defer cancel()
	count, err := s.client.Get(ctx, authFailureKey(clientID)).Int64()
	if errors.Is(err, redis.Nil) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	return count >= limit, nil
}

func (s *redisStore) recordAuthFailure(clientID string, window time.Duration) (int64, error) {
	ctx, cancel := redisContext()
	defer cancel()
	script := redis.NewScript(`local count=redis.call('INCR',KEYS[1]); if count==1 then redis.call('PEXPIRE',KEYS[1],ARGV[1]) end; return count`)
	return script.Run(ctx, s.client, []string{authFailureKey(clientID)}, window.Milliseconds()).Int64()
}

func (s *redisStore) clearAuthFailures(clientID string) error {
	ctx, cancel := redisContext()
	defer cancel()
	return s.client.Del(ctx, authFailureKey(clientID)).Err()
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
	defaults := defaultRuntimeConfig()
	initScript := redis.NewScript(`if redis.call('EXISTS',KEYS[1]) == 0 then redis.call('HSET',KEYS[1],'version',1,'max_payload_size',ARGV[1],'max_connections_per_client',ARGV[2],'max_requests_per_client_per_second',ARGV[3],'slow_client_output_limit',ARGV[4],'log_level',ARGV[5],'request_queue_capacity_display',ARGV[6]); return 1 end; redis.call('HSETNX',KEYS[1],'request_queue_capacity_display',ARGV[6]); return 0`)
	if _, err := initScript.Run(ctx, s.client, []string{"config:active"}, defaults.MaxPayloadSize, defaults.MaxConnectionsPerClient, defaults.MaxRequestsPerClientPerSecond, defaults.SlowClientOutputLimit, defaults.LogLevel, defaults.RequestQueueCapacityDisplay).Result(); err != nil {
		return runtimeConfig{}, err
	}
	values, err := s.client.HGetAll(ctx, "config:active").Result()
	if err != nil {
		return runtimeConfig{}, err
	}
	return runtimeConfig{Version: redisInt(values, "version"), MaxPayloadSize: int(redisInt(values, "max_payload_size")), MaxConnectionsPerClient: int(redisInt(values, "max_connections_per_client")), MaxRequestsPerClientPerSecond: int(redisInt(values, "max_requests_per_client_per_second")), SlowClientOutputLimit: int(redisInt(values, "slow_client_output_limit")), LogLevel: values["log_level"], RequestQueueCapacityDisplay: int(redisInt(values, "request_queue_capacity_display"))}, nil
}

func (s *redisStore) updateConfig(expectedVersion int64, req configUpdateRequest) (runtimeConfig, error) {
	ctx, cancel := redisContext()
	defer cancel()
	defaults := defaultRuntimeConfig()
	script := redis.NewScript(`local current=redis.call('HGET',KEYS[1],'version'); if not current or tonumber(current)~=tonumber(ARGV[1]) then return {0,''} end; local next=tonumber(current)+1; local display=redis.call('HGET',KEYS[1],'request_queue_capacity_display') or ARGV[7]; redis.call('HSET',KEYS[1],'version',next,'max_payload_size',ARGV[2],'max_connections_per_client',ARGV[3],'max_requests_per_client_per_second',ARGV[4],'slow_client_output_limit',ARGV[5],'log_level',ARGV[6],'request_queue_capacity_display',display); return {next,display}`)
	result, err := script.Run(ctx, s.client, []string{"config:active"}, expectedVersion, req.MaxPayloadSize, req.MaxConnectionsPerClient, req.MaxRequestsPerClientPerSecond, req.SlowClientOutputLimit, strings.ToUpper(req.LogLevel), defaults.RequestQueueCapacityDisplay).Slice()
	if err != nil {
		return runtimeConfig{}, err
	}
	if len(result) != 2 {
		return runtimeConfig{}, errors.New("invalid config CAS result")
	}
	next, ok := result[0].(int64)
	if !ok {
		return runtimeConfig{}, errors.New("invalid config CAS version")
	}
	if next == 0 {
		return runtimeConfig{}, errConfigConflict
	}
	displayText, ok := result[1].(string)
	if !ok {
		return runtimeConfig{}, errors.New("invalid config CAS display capacity")
	}
	display, err := strconv.Atoi(displayText)
	if err != nil {
		return runtimeConfig{}, errors.New("invalid config CAS display capacity")
	}
	return runtimeConfig{Version: next, MaxPayloadSize: req.MaxPayloadSize, MaxConnectionsPerClient: req.MaxConnectionsPerClient, MaxRequestsPerClientPerSecond: req.MaxRequestsPerClientPerSecond, SlowClientOutputLimit: req.SlowClientOutputLimit, LogLevel: strings.ToUpper(req.LogLevel), RequestQueueCapacityDisplay: display}, nil
}

func redisInt(values map[string]string, key string) int64 {
	value, _ := strconv.ParseInt(values[key], 10, 64)
	return value
}

func redisContext() (context.Context, context.CancelFunc) {
	return context.WithTimeout(context.Background(), 2*time.Second)
}

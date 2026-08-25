package app

import (
	"os"
	"time"
)

const (
	defaultStoreBackend = "memory"
	defaultRedisAddr    = "localhost:6379"
	storeErrorMessage   = "store error"
)

type Store interface {
	verifyDigest(clientID, digest string) (tokenAuthDecision, error)
	authFailureLimited(clientID string, limit int64) (bool, error)
	recordAuthFailure(clientID string, window time.Duration) (int64, error)
	clearAuthFailures(clientID string) error
	createToken(record tokenRecord) error
	rotateToken(clientID string, expected int64, digest, updatedAt string) (tokenRecord, error)
	disableToken(clientID string, updatedAt string) error
	listTokens() ([]tokenEntry, error)
	getConfig() (runtimeConfig, error)
	updateConfig(expectedVersion int64, req configUpdateRequest) (runtimeConfig, error)
}

type tokenAuthDecision int

const (
	tokenAuthInvalid tokenAuthDecision = iota
	tokenAuthAllowed
	tokenAuthDisabled
)

type authFailurePolicy struct {
	limit  int64
	window time.Duration
}

func authFailurePolicyFromEnv() authFailurePolicy {
	return authFailurePolicy{
		limit:  int64(readPositiveEnv("AUTH_FAILURE_LIMIT", 5)),
		window: time.Duration(readPositiveEnv("AUTH_FAILURE_WINDOW_SECONDS", 60)) * time.Second,
	}
}

func newStoreFromEnv() Store {
	backend := os.Getenv("STORE_BACKEND")
	if backend == "" {
		backend = defaultStoreBackend
	}
	if backend == "redis" {
		addr := os.Getenv("REDIS_ADDR")
		if addr == "" {
			addr = defaultRedisAddr
		}
		return newRedisStore(addr)
	}
	return newMemoryStore()
}

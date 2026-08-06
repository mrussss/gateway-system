package app

import (
	"os"
	"time"
)

const (
	defaultStoreBackend        = "memory"
	defaultRedisAddr           = "localhost:6379"
	defaultGatewayOfflineAfter = 30 * time.Second
	defaultGatewayStatusTTL    = 5 * time.Minute
	defaultClientSnapshotTTL   = 60 * time.Second
	storeErrorMessage          = "store error"
)

type Store interface {
	saveMetrics(req metricsReportRequest) (gatewayStatusResponse, error)
	getStatus() (gatewayStatusResponse, bool, error)
	saveClients(gatewayID string, clients []clientInfo) error
	getClients() ([]clientInfo, error)
	listGateways() ([]gatewayStatusResponse, error)
	getGatewayStatus(gatewayID string) (gatewayStatusResponse, bool, error)
	getGatewayClients(gatewayID string) ([]clientInfo, bool, error)
	isDigestAllowed(clientID string, digest string) (bool, error)
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

type tokenDecisionStore interface {
	verifyDigest(clientID, digest string) (tokenAuthDecision, error)
}

type authFailureStore interface {
	authFailureLimited(clientID string, limit int64) (bool, error)
	recordAuthFailure(clientID string, window time.Duration) (int64, error)
	clearAuthFailures(clientID string) error
}

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

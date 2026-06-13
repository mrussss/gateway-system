package main

import (
	"os"
	"time"
)

const (
	defaultStoreBackend        = "memory"
	defaultRedisAddr           = "localhost:6379"
	defaultGatewayOfflineAfter = 15 * time.Second
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
	setToken(clientID string, token string) error
	deleteToken(clientID string) error
	isAllowed(clientID string, token string) (bool, error)
	listTokens() ([]tokenEntry, error)
	getConfig() (runtimeConfig, error)
	updateConfig(req configUpdateRequest) (runtimeConfig, error)
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

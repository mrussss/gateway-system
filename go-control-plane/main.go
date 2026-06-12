package main

import (
	"context"
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"os"
	"sort"
	"sync"
	"time"

	"github.com/redis/go-redis/v9"
)

const (
	defaultStoreBackend = "memory"
	defaultRedisAddr    = "localhost:6379"
	storeErrorMessage   = "store error"
)

var store Store = newMemoryStore()

type Store interface {
	saveMetrics(req metricsReportRequest) (gatewayStatusResponse, error)
	getStatus() (gatewayStatusResponse, bool, error)
	saveClients(clients []clientInfo) error
	getClients() ([]clientInfo, error)
	setToken(clientID string, token string) error
	deleteToken(clientID string) error
	isAllowed(clientID string, token string) (bool, error)
	listTokens() ([]tokenEntry, error)
	getConfig() (runtimeConfig, error)
	updateConfig(req configUpdateRequest) (runtimeConfig, error)
}

type authCheckRequest struct {
	ClientID string `json:"client_id"`
	Token    string `json:"token"`
}

type authCheckResponse struct {
	Allowed bool   `json:"allowed"`
	Reason  string `json:"reason"`
}

type healthResponse struct {
	Status string `json:"status"`
}

type configReloadResponse struct {
	Success bool   `json:"success"`
	Message string `json:"message"`
	Version int64  `json:"version"`
}

type runtimeConfig struct {
	Version                       int64 `json:"version"`
	AuthTimeoutMS                 int   `json:"auth_timeout_ms"`
	MaxPayloadSize                int   `json:"max_payload_size"`
	MaxConnectionsPerClient       int   `json:"max_connections_per_client"`
	MaxRequestsPerClientPerSecond int   `json:"max_requests_per_client_per_second"`
	FailOpen                      bool  `json:"fail_open"`
}

type configUpdateRequest struct {
	AuthTimeoutMS                 int  `json:"auth_timeout_ms"`
	MaxPayloadSize                int  `json:"max_payload_size"`
	MaxConnectionsPerClient       int  `json:"max_connections_per_client"`
	MaxRequestsPerClientPerSecond int  `json:"max_requests_per_client_per_second"`
	FailOpen                      bool `json:"fail_open"`
}

type metricsReportRequest struct {
	GatewayID         string `json:"gateway_id"`
	ActiveConnections int64  `json:"active_connections"`
	TotalMessages     int64  `json:"total_messages"`
	BytesIn           int64  `json:"bytes_in"`
	BytesOut          int64  `json:"bytes_out"`
	ErrorCount        int64  `json:"error_count"`
	Timestamp         int64  `json:"timestamp"`
}

type gatewayStatusResponse struct {
	GatewayID         string `json:"gateway_id"`
	ActiveConnections int64  `json:"active_connections"`
	TotalMessages     int64  `json:"total_messages"`
	BytesIn           int64  `json:"bytes_in"`
	BytesOut          int64  `json:"bytes_out"`
	ErrorCount        int64  `json:"error_count"`
	LastReportTime    string `json:"last_report_time"`
}

type clientInfo struct {
	ClientID    string `json:"client_id"`
	RemoteAddr  string `json:"remote_addr"`
	ConnectedAt string `json:"connected_at"`
}

type clientsReportRequest struct {
	GatewayID string       `json:"gateway_id"`
	Clients   []clientInfo `json:"clients"`
}

type errorResponse struct {
	Error string `json:"error"`
}

type successResponse struct {
	Success bool `json:"success"`
}

type tokenEntry struct {
	ClientID string `json:"client_id"`
}

type tokenUpsertRequest struct {
	ClientID string `json:"client_id"`
	Token    string `json:"token"`
}

type memoryStore struct {
	mu        sync.RWMutex
	status    gatewayStatusResponse
	hasStatus bool
	clients   []clientInfo
	tokens    map[string]string
	config    runtimeConfig
}

type redisStore struct {
	client *redis.Client
}

func defaultRuntimeConfig() runtimeConfig {
	return runtimeConfig{
		Version:                       1,
		AuthTimeoutMS:                 1000,
		MaxPayloadSize:                4194314,
		MaxConnectionsPerClient:       2,
		MaxRequestsPerClientPerSecond: 100,
		FailOpen:                      false,
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

func newMemoryStore() *memoryStore {
	return &memoryStore{
		tokens: map[string]string{},
		config: defaultRuntimeConfig(),
	}
}

func newRedisStore(addr string) *redisStore {
	client := redis.NewClient(&redis.Options{
		Addr: addr,
	})

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if err := client.Ping(ctx).Err(); err != nil {
		log.Fatalf("redis unavailable: %v", err)
	}

	return &redisStore{client: client}
}

func (s *memoryStore) saveMetrics(req metricsReportRequest) (gatewayStatusResponse, error) {
	status := statusFromMetrics(req)

	s.mu.Lock()
	defer s.mu.Unlock()
	s.status = status
	s.hasStatus = true
	return status, nil
}

func (s *memoryStore) getStatus() (gatewayStatusResponse, bool, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.status, s.hasStatus, nil
}

func (s *memoryStore) saveClients(clients []clientInfo) error {
	copied := append([]clientInfo(nil), clients...)

	s.mu.Lock()
	defer s.mu.Unlock()
	s.clients = copied
	return nil
}

func (s *memoryStore) getClients() ([]clientInfo, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return append([]clientInfo(nil), s.clients...), nil
}

func (s *memoryStore) setToken(clientID string, token string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.tokens[clientID] = token
	return nil
}

func (s *memoryStore) deleteToken(clientID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.tokens, clientID)
	return nil
}

func (s *memoryStore) isAllowed(clientID string, token string) (bool, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	storedToken, ok := s.tokens[clientID]
	return ok && storedToken == token, nil
}

func (s *memoryStore) listTokens() ([]tokenEntry, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	entries := make([]tokenEntry, 0, len(s.tokens))
	for clientID := range s.tokens {
		entries = append(entries, tokenEntry{ClientID: clientID})
	}
	sort.Slice(entries, func(i, j int) bool {
		return entries[i].ClientID < entries[j].ClientID
	})
	return entries, nil
}

func (s *memoryStore) getConfig() (runtimeConfig, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.config, nil
}

func (s *memoryStore) updateConfig(req configUpdateRequest) (runtimeConfig, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.config = runtimeConfig{
		Version:                       s.config.Version + 1,
		AuthTimeoutMS:                 req.AuthTimeoutMS,
		MaxPayloadSize:                req.MaxPayloadSize,
		MaxConnectionsPerClient:       req.MaxConnectionsPerClient,
		MaxRequestsPerClientPerSecond: req.MaxRequestsPerClientPerSecond,
		FailOpen:                      req.FailOpen,
	}
	return s.config, nil
}

func (s *redisStore) saveMetrics(req metricsReportRequest) (gatewayStatusResponse, error) {
	status := statusFromMetrics(req)
	if err := s.setJSON(redisContext(), "gateway:status", status); err != nil {
		return gatewayStatusResponse{}, err
	}
	return status, nil
}

func (s *redisStore) getStatus() (gatewayStatusResponse, bool, error) {
	raw, err := s.client.Get(redisContext(), "gateway:status").Result()
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

func (s *redisStore) saveClients(clients []clientInfo) error {
	return s.setJSON(redisContext(), "clients:current", clients)
}

func (s *redisStore) getClients() ([]clientInfo, error) {
	raw, err := s.client.Get(redisContext(), "clients:current").Result()
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

func (s *redisStore) setToken(clientID string, token string) error {
	ctx := redisContext()
	if err := s.client.Set(ctx, "token:"+clientID, token, 0).Err(); err != nil {
		return err
	}
	return s.client.SAdd(ctx, "tokens", clientID).Err()
}

func (s *redisStore) deleteToken(clientID string) error {
	ctx := redisContext()
	if err := s.client.Del(ctx, "token:"+clientID).Err(); err != nil {
		return err
	}
	return s.client.SRem(ctx, "tokens", clientID).Err()
}

func (s *redisStore) isAllowed(clientID string, token string) (bool, error) {
	raw, err := s.client.Get(redisContext(), "token:"+clientID).Result()
	if errors.Is(err, redis.Nil) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	return raw == token, nil
}

func (s *redisStore) listTokens() ([]tokenEntry, error) {
	clientIDs, err := s.client.SMembers(redisContext(), "tokens").Result()
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
	ctx := redisContext()
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
	if err := s.setJSON(redisContext(), "config:current", cfg); err != nil {
		return runtimeConfig{}, err
	}
	return cfg, nil
}

func (s *redisStore) setJSON(ctx context.Context, key string, value any) error {
	payload, err := json.Marshal(value)
	if err != nil {
		return err
	}
	return s.client.Set(ctx, key, payload, 0).Err()
}

func main() {
	store = newStoreFromEnv()

	server := &http.Server{
		Addr:              ":8080",
		Handler:           routes(),
		ReadHeaderTimeout: 3 * time.Second,
		ReadTimeout:       5 * time.Second,
		WriteTimeout:      5 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	log.Printf("go control plane listening on %s", server.Addr)
	if err := server.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("server failed: %v", err)
	}
}

func routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", handleHealth)
	mux.HandleFunc("POST /auth/check", handleAuthCheck)
	mux.HandleFunc("POST /metrics/report", handleMetricsReport)
	mux.HandleFunc("GET /gateway/status", handleGatewayStatus)
	mux.HandleFunc("POST /clients/report", handleClientsReport)
	mux.HandleFunc("GET /clients", handleClients)
	mux.HandleFunc("POST /tokens", handleTokensUpsert)
	mux.HandleFunc("GET /tokens", handleTokensList)
	mux.HandleFunc("DELETE /tokens/{client_id}", handleTokensDelete)
	mux.HandleFunc("GET /config", handleConfigGet)
	mux.HandleFunc("POST /config", handleConfigUpdate)
	mux.HandleFunc("POST /config/reload", handleConfigReload)
	return mux
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, healthResponse{Status: "ok"})
}

func handleAuthCheck(w http.ResponseWriter, r *http.Request) {
	var req authCheckRequest
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, authCheckResponse{
			Allowed: false,
			Reason:  "invalid request body",
		})
		return
	}

	if req.ClientID == "" || req.Token == "" {
		writeJSON(w, http.StatusBadRequest, authCheckResponse{
			Allowed: false,
			Reason:  "client_id and token are required",
		})
		return
	}

	allowed, err := store.isAllowed(req.ClientID, req.Token)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, authCheckResponse{
			Allowed: false,
			Reason:  storeErrorMessage,
		})
		return
	}
	if !allowed {
		writeJSON(w, http.StatusOK, authCheckResponse{
			Allowed: false,
			Reason:  "invalid token",
		})
		return
	}

	writeJSON(w, http.StatusOK, authCheckResponse{
		Allowed: true,
		Reason:  "ok",
	})
}

func handleMetricsReport(w http.ResponseWriter, r *http.Request) {
	var req metricsReportRequest
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	if req.GatewayID == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "gateway_id is required"})
		return
	}

	status, err := store.saveMetrics(req)
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, status)
}

func handleGatewayStatus(w http.ResponseWriter, r *http.Request) {
	status, ok, err := store.getStatus()
	if err != nil {
		writeStoreError(w)
		return
	}
	if !ok {
		writeJSON(w, http.StatusNotFound, errorResponse{Error: "gateway status not reported"})
		return
	}

	writeJSON(w, http.StatusOK, status)
}

func handleClientsReport(w http.ResponseWriter, r *http.Request) {
	var req clientsReportRequest
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	if req.GatewayID == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "gateway_id is required"})
		return
	}

	if err := store.saveClients(req.Clients); err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"success": true})
}

func handleClients(w http.ResponseWriter, r *http.Request) {
	clients, err := store.getClients()
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, clients)
}

func handleTokensUpsert(w http.ResponseWriter, r *http.Request) {
	var req tokenUpsertRequest
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	if req.ClientID == "" || req.Token == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "client_id and token are required"})
		return
	}

	if err := store.setToken(req.ClientID, req.Token); err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, successResponse{Success: true})
}

func handleTokensList(w http.ResponseWriter, r *http.Request) {
	entries, err := store.listTokens()
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, entries)
}

func handleTokensDelete(w http.ResponseWriter, r *http.Request) {
	clientID := r.PathValue("client_id")
	if clientID == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "client_id is required"})
		return
	}

	if err := store.deleteToken(clientID); err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, successResponse{Success: true})
}

func handleConfigGet(w http.ResponseWriter, r *http.Request) {
	cfg, err := store.getConfig()
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, cfg)
}

func handleConfigUpdate(w http.ResponseWriter, r *http.Request) {
	var req configUpdateRequest
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	if err := validateConfigUpdate(req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: err.Error()})
		return
	}

	cfg, err := store.updateConfig(req)
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, cfg)
}

func handleConfigReload(w http.ResponseWriter, r *http.Request) {
	cfg, err := store.getConfig()
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, configReloadResponse{
		Success: true,
		Message: "config reload is a no-op",
		Version: cfg.Version,
	})
}

func statusFromMetrics(req metricsReportRequest) gatewayStatusResponse {
	reportTime := time.Now().UTC()
	if req.Timestamp > 0 {
		reportTime = time.Unix(req.Timestamp, 0).UTC()
	}

	return gatewayStatusResponse{
		GatewayID:         req.GatewayID,
		ActiveConnections: req.ActiveConnections,
		TotalMessages:     req.TotalMessages,
		BytesIn:           req.BytesIn,
		BytesOut:          req.BytesOut,
		ErrorCount:        req.ErrorCount,
		LastReportTime:    reportTime.Format(time.RFC3339),
	}
}

func redisContext() context.Context {
	ctx, _ := context.WithTimeout(context.Background(), 2*time.Second)
	return ctx
}

func writeStoreError(w http.ResponseWriter) {
	writeJSON(w, http.StatusInternalServerError, errorResponse{Error: storeErrorMessage})
}

func writeJSON(w http.ResponseWriter, statusCode int, payload any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	if err := json.NewEncoder(w).Encode(payload); err != nil {
		log.Printf("write json response failed: %v", err)
	}
}

func validateConfigUpdate(req configUpdateRequest) error {
	switch {
	case req.AuthTimeoutMS <= 0:
		return errors.New("auth_timeout_ms must be positive")
	case req.MaxPayloadSize <= 0:
		return errors.New("max_payload_size must be positive")
	case req.MaxConnectionsPerClient <= 0:
		return errors.New("max_connections_per_client must be positive")
	case req.MaxRequestsPerClientPerSecond <= 0:
		return errors.New("max_requests_per_client_per_second must be positive")
	default:
		return nil
	}
}

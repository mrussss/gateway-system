package main

import (
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"sync"
	"time"
)

var store = newMemoryStore()

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

func newMemoryStore() *memoryStore {
	return &memoryStore{
		tokens: map[string]string{},
		config: runtimeConfig{
			Version:                       1,
			AuthTimeoutMS:                 1000,
			MaxPayloadSize:                4194314,
			MaxConnectionsPerClient:       2,
			MaxRequestsPerClientPerSecond: 100,
			FailOpen:                      false,
		},
	}
}

func (s *memoryStore) saveMetrics(req metricsReportRequest) gatewayStatusResponse {
	reportTime := time.Now().UTC()
	if req.Timestamp > 0 {
		reportTime = time.Unix(req.Timestamp, 0).UTC()
	}

	status := gatewayStatusResponse{
		GatewayID:         req.GatewayID,
		ActiveConnections: req.ActiveConnections,
		TotalMessages:     req.TotalMessages,
		BytesIn:           req.BytesIn,
		BytesOut:          req.BytesOut,
		ErrorCount:        req.ErrorCount,
		LastReportTime:    reportTime.Format(time.RFC3339),
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	s.status = status
	s.hasStatus = true
	return status
}

func (s *memoryStore) getStatus() (gatewayStatusResponse, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.status, s.hasStatus
}

func (s *memoryStore) saveClients(clients []clientInfo) {
	copied := make([]clientInfo, len(clients))
	copy(copied, clients)

	s.mu.Lock()
	defer s.mu.Unlock()
	s.clients = copied
}

func (s *memoryStore) getClients() []clientInfo {
	s.mu.RLock()
	defer s.mu.RUnlock()

	copied := make([]clientInfo, len(s.clients))
	copy(copied, s.clients)
	return copied
}

func (s *memoryStore) setToken(clientID string, token string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.tokens[clientID] = token
}

func (s *memoryStore) deleteToken(clientID string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.tokens, clientID)
}

func (s *memoryStore) isAllowed(clientID string, token string) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	storedToken, ok := s.tokens[clientID]
	return ok && storedToken == token
}

func (s *memoryStore) listTokens() []tokenEntry {
	s.mu.RLock()
	defer s.mu.RUnlock()

	entries := make([]tokenEntry, 0, len(s.tokens))
	for clientID := range s.tokens {
		entries = append(entries, tokenEntry{ClientID: clientID})
	}
	return entries
}

func (s *memoryStore) getConfig() runtimeConfig {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.config
}

func (s *memoryStore) updateConfig(req configUpdateRequest) runtimeConfig {
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
	return s.config
}

func main() {
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

	if !store.isAllowed(req.ClientID, req.Token) {
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

	status := store.saveMetrics(req)
	writeJSON(w, http.StatusOK, status)
}

func handleGatewayStatus(w http.ResponseWriter, r *http.Request) {
	status, ok := store.getStatus()
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

	store.saveClients(req.Clients)
	writeJSON(w, http.StatusOK, map[string]bool{"success": true})
}

func handleClients(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, store.getClients())
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

	store.setToken(req.ClientID, req.Token)
	writeJSON(w, http.StatusOK, successResponse{Success: true})
}

func handleTokensList(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, store.listTokens())
}

func handleTokensDelete(w http.ResponseWriter, r *http.Request) {
	clientID := r.PathValue("client_id")
	if clientID == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "client_id is required"})
		return
	}

	store.deleteToken(clientID)
	writeJSON(w, http.StatusOK, successResponse{Success: true})
}

func handleConfigGet(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, store.getConfig())
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

	writeJSON(w, http.StatusOK, store.updateConfig(req))
}

func handleConfigReload(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, configReloadResponse{
		Success: true,
		Message: "memory config reload is a no-op",
		Version: store.getConfig().Version,
	})
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

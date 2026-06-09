package main

import (
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"sync"
	"time"
)

const validToken = "test-token"

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

type memoryStore struct {
	mu        sync.RWMutex
	status    gatewayStatusResponse
	hasStatus bool
	clients   []clientInfo
}

func newMemoryStore() *memoryStore {
	return &memoryStore{}
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

	if req.Token != validToken {
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

func writeJSON(w http.ResponseWriter, statusCode int, payload any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	if err := json.NewEncoder(w).Encode(payload); err != nil {
		log.Printf("write json response failed: %v", err)
	}
}

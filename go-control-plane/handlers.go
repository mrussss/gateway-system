package main

import (
	"encoding/json"
	"log"
	"net/http"
	"time"
)

type application struct{ store Store }

func routesWithStore(store Store) http.Handler {
	a := &application{store: store}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", a.handleHealth)
	mux.HandleFunc("GET /health/live", a.handleHealth)
	mux.HandleFunc("GET /health/ready", a.handleReady)
	mux.HandleFunc("POST /auth/check", a.handleAuthCheck)
	mux.HandleFunc("POST /metrics/report", a.handleMetricsReport)
	mux.HandleFunc("GET /gateway/status", a.handleGatewayStatus)
	mux.HandleFunc("GET /gateways", a.handleGatewaysList)
	mux.HandleFunc("GET /gateways/{gateway_id}/status", a.handleGatewayStatusByID)
	mux.HandleFunc("POST /clients/report", a.handleClientsReport)
	mux.HandleFunc("GET /clients", a.handleClients)
	mux.HandleFunc("GET /gateways/{gateway_id}/clients", a.handleGatewayClientsByID)
	mux.HandleFunc("POST /tokens", a.handleTokensUpsert)
	mux.HandleFunc("GET /tokens", a.handleTokensList)
	mux.HandleFunc("DELETE /tokens/{client_id}", a.handleTokensDelete)
	mux.HandleFunc("GET /config", a.handleConfigGet)
	mux.HandleFunc("POST /config", a.handleConfigUpdate)
	mux.HandleFunc("POST /config/reload", a.handleConfigReload)
	return middleware(mux)
}

func (a *application) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, healthResponse{Status: "ok"})
}

func (a *application) handleReady(w http.ResponseWriter, r *http.Request) {
	if checker, ok := a.store.(storeHealthChecker); ok {
		if err := checker.Ping(r.Context()); err != nil {
			writeAPIError(w, r, http.StatusServiceUnavailable, "NOT_READY", "store unavailable")
			return
		}
	}
	writeJSON(w, http.StatusOK, healthResponse{Status: "ready"})
}

func (a *application) handleAuthCheck(w http.ResponseWriter, r *http.Request) {
	var req authCheckRequest
	if err := decodeJSON(w, r, &req); err != nil {
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

	allowed, err := a.store.isAllowed(req.ClientID, req.Token)
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

func (a *application) handleMetricsReport(w http.ResponseWriter, r *http.Request) {
	var req metricsReportRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	if req.GatewayID == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "gateway_id is required"})
		return
	}

	status, err := a.store.saveMetrics(req)
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, status)
}

func (a *application) handleGatewayStatus(w http.ResponseWriter, r *http.Request) {
	status, ok, err := a.store.getStatus()
	if err != nil {
		writeStoreError(w)
		return
	}
	if !ok {
		writeJSON(w, http.StatusNotFound, errorResponse{Error: "gateway status not reported"})
		return
	}

	writeJSON(w, http.StatusOK, gatewayStatusToView(status, time.Now().UTC()))
}

func (a *application) handleGatewaysList(w http.ResponseWriter, r *http.Request) {
	statuses, err := a.store.listGateways()
	if err != nil {
		writeStoreError(w)
		return
	}
	now := time.Now().UTC()
	views := make([]gatewayStatusView, 0, len(statuses))
	for _, status := range statuses {
		views = append(views, gatewayStatusToView(status, now))
	}
	writeJSON(w, http.StatusOK, views)
}

func (a *application) handleGatewayStatusByID(w http.ResponseWriter, r *http.Request) {
	gatewayID := r.PathValue("gateway_id")
	status, ok, err := a.store.getGatewayStatus(gatewayID)
	if err != nil {
		writeStoreError(w)
		return
	}
	if !ok {
		writeJSON(w, http.StatusNotFound, errorResponse{Error: "gateway status not reported"})
		return
	}
	writeJSON(w, http.StatusOK, gatewayStatusToView(status, time.Now().UTC()))
}

func (a *application) handleClientsReport(w http.ResponseWriter, r *http.Request) {
	var req clientsReportRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	if req.GatewayID == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "gateway_id is required"})
		return
	}

	if err := a.store.saveClients(req.GatewayID, req.Clients); err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"success": true})
}

func (a *application) handleClients(w http.ResponseWriter, r *http.Request) {
	clients, err := a.store.getClients()
	if err != nil {
		writeStoreError(w)
		return
	}
	if clients == nil {
		clients = make([]clientInfo, 0)
	}
	writeJSON(w, http.StatusOK, clients)
}

func (a *application) handleGatewayClientsByID(w http.ResponseWriter, r *http.Request) {
	gatewayID := r.PathValue("gateway_id")
	clients, ok, err := a.store.getGatewayClients(gatewayID)
	if err != nil {
		writeStoreError(w)
		return
	}
	if !ok {
		writeJSON(w, http.StatusNotFound, errorResponse{Error: "gateway clients not reported"})
		return
	}
	if clients == nil {
		clients = make([]clientInfo, 0)
	}
	writeJSON(w, http.StatusOK, clients)
}

func (a *application) handleTokensUpsert(w http.ResponseWriter, r *http.Request) {
	var req tokenUpsertRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	if req.ClientID == "" || req.Token == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "client_id and token are required"})
		return
	}

	if err := a.store.setToken(req.ClientID, req.Token); err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, successResponse{Success: true})
}

func (a *application) handleTokensList(w http.ResponseWriter, r *http.Request) {
	entries, err := a.store.listTokens()
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, entries)
}

func (a *application) handleTokensDelete(w http.ResponseWriter, r *http.Request) {
	clientID := r.PathValue("client_id")
	if clientID == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "client_id is required"})
		return
	}

	if err := a.store.deleteToken(clientID); err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, successResponse{Success: true})
}

func (a *application) handleConfigGet(w http.ResponseWriter, r *http.Request) {
	cfg, err := a.store.getConfig()
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, cfg)
}

func (a *application) handleConfigUpdate(w http.ResponseWriter, r *http.Request) {
	var req configUpdateRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	if err := validateConfigUpdate(req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: err.Error()})
		return
	}

	cfg, err := a.store.updateConfig(req)
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, cfg)
}

func (a *application) handleConfigReload(w http.ResponseWriter, r *http.Request) {
	cfg, err := a.store.getConfig()
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

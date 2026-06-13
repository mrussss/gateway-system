package main

import (
	"encoding/json"
	"log"
	"net/http"
	"time"
)

func routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", handleHealth)
	mux.HandleFunc("POST /auth/check", handleAuthCheck)
	mux.HandleFunc("POST /metrics/report", handleMetricsReport)
	mux.HandleFunc("GET /gateway/status", handleGatewayStatus)
	mux.HandleFunc("GET /gateways", handleGatewaysList)
	mux.HandleFunc("GET /gateways/{gateway_id}/status", handleGatewayStatusByID)
	mux.HandleFunc("POST /clients/report", handleClientsReport)
	mux.HandleFunc("GET /clients", handleClients)
	mux.HandleFunc("GET /gateways/{gateway_id}/clients", handleGatewayClientsByID)
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

	writeJSON(w, http.StatusOK, gatewayStatusToView(status, time.Now().UTC()))
}

func handleGatewaysList(w http.ResponseWriter, r *http.Request) {
	statuses, err := store.listGateways()
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

func handleGatewayStatusByID(w http.ResponseWriter, r *http.Request) {
	gatewayID := r.PathValue("gateway_id")
	status, ok, err := store.getGatewayStatus(gatewayID)
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

	if err := store.saveClients(req.GatewayID, req.Clients); err != nil {
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

func handleGatewayClientsByID(w http.ResponseWriter, r *http.Request) {
	gatewayID := r.PathValue("gateway_id")
	clients, ok, err := store.getGatewayClients(gatewayID)
	if err != nil {
		writeStoreError(w)
		return
	}
	if !ok {
		writeJSON(w, http.StatusNotFound, errorResponse{Error: "gateway clients not reported"})
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

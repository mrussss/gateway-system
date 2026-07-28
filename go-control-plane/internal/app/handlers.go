package app

import (
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"os"
	"strconv"
	"time"
)

type application struct {
	store        Store
	tokens       tokenService
	adminToken   string
	gatewayToken string
}

func routesWithStore(store Store) http.Handler {
	a := &application{store: store, tokens: tokenServiceFromEnv(), adminToken: os.Getenv("ADMIN_TOKEN"), gatewayToken: os.Getenv("GATEWAY_SHARED_TOKEN")}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", a.handleHealth)
	mux.HandleFunc("GET /health/live", a.handleHealth)
	mux.HandleFunc("GET /health/ready", a.handleReady)
	mux.Handle("POST /auth/check", a.requireGateway(http.HandlerFunc(a.handleAuthCheck)))
	mux.Handle("POST /metrics/report", a.requireGateway(http.HandlerFunc(a.handleMetricsReport)))
	mux.Handle("GET /gateway/status", a.requireAdmin(http.HandlerFunc(a.handleGatewayStatus)))
	mux.Handle("GET /gateways", a.requireAdmin(http.HandlerFunc(a.handleGatewaysList)))
	mux.Handle("GET /gateways/{gateway_id}/status", a.requireAdmin(http.HandlerFunc(a.handleGatewayStatusByID)))
	mux.Handle("POST /clients/report", a.requireGateway(http.HandlerFunc(a.handleClientsReport)))
	mux.Handle("GET /clients", a.requireAdmin(http.HandlerFunc(a.handleClients)))
	mux.Handle("GET /gateways/{gateway_id}/clients", a.requireAdmin(http.HandlerFunc(a.handleGatewayClientsByID)))
	mux.Handle("POST /tokens", a.requireAdmin(http.HandlerFunc(a.handleTokensUpsert)))
	mux.Handle("GET /tokens", a.requireAdmin(http.HandlerFunc(a.handleTokensList)))
	mux.Handle("DELETE /tokens/{client_id}", a.requireAdmin(http.HandlerFunc(a.handleTokensDelete)))
	mux.Handle("POST /tokens/{client_id}/rotate", a.requireAdmin(http.HandlerFunc(a.handleTokensRotate)))
	mux.Handle("GET /config", a.requireAdminOrGateway(http.HandlerFunc(a.handleConfigGet)))
	mux.Handle("PUT /config", a.requireAdmin(http.HandlerFunc(a.handleConfigUpdate)))
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

	allowed, err := a.store.isDigestAllowed(req.ClientID, a.tokens.digest(req.Token))
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

	if req.ClientID == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "client_id is required"})
		return
	}
	token, err := a.tokens.generate()
	if err != nil {
		writeStoreError(w)
		return
	}
	now := nowRFC3339()
	record := tokenRecord{tokenEntry: tokenEntry{ClientID: req.ClientID, Generation: 1, CreatedAt: now, UpdatedAt: now}, Digest: a.tokens.digest(token)}
	if err := a.store.createToken(record); err != nil {
		if errors.Is(err, errTokenExists) {
			writeJSON(w, http.StatusConflict, errorResponse{Error: "token already exists"})
			return
		}
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusCreated, tokenSecretResponse{ClientID: req.ClientID, Token: token, Generation: 1})
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

	if err := a.store.disableToken(clientID, nowRFC3339()); err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, successResponse{Success: true})
}

func (a *application) handleTokensRotate(w http.ResponseWriter, r *http.Request) {
	expected, err := parseGeneration(r.Header.Get("If-Match"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid If-Match"})
		return
	}
	token, err := a.tokens.generate()
	if err != nil {
		writeStoreError(w)
		return
	}
	record, err := a.store.rotateToken(r.PathValue("client_id"), expected, a.tokens.digest(token), nowRFC3339())
	if errors.Is(err, errTokenConflict) {
		writeJSON(w, http.StatusConflict, errorResponse{Error: "token generation conflict"})
		return
	}
	if errors.Is(err, errTokenNotFound) {
		writeJSON(w, http.StatusNotFound, errorResponse{Error: "token not found"})
		return
	}
	if err != nil {
		writeStoreError(w)
		return
	}
	writeJSON(w, http.StatusOK, tokenSecretResponse{ClientID: record.ClientID, Token: token, Generation: record.Generation})
}

func (a *application) requireAdmin(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !secureBearerEqual(r.Header.Get("Authorization"), a.adminToken) {
			writeAPIError(w, r, http.StatusUnauthorized, "UNAUTHORIZED", "invalid admin token")
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (a *application) requireGateway(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if a.gatewayToken != "" && !digestEqual(r.Header.Get("X-Gateway-Token"), a.gatewayToken) {
			writeAPIError(w, r, http.StatusUnauthorized, "UNAUTHORIZED", "invalid gateway token")
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (a *application) requireAdminOrGateway(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		adminOK := secureBearerEqual(r.Header.Get("Authorization"), a.adminToken)
		gatewayOK := a.gatewayToken == "" || digestEqual(r.Header.Get("X-Gateway-Token"), a.gatewayToken)
		if !adminOK && !gatewayOK {
			writeAPIError(w, r, http.StatusUnauthorized, "UNAUTHORIZED", "invalid credentials")
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (a *application) handleConfigGet(w http.ResponseWriter, r *http.Request) {
	cfg, err := a.store.getConfig()
	if err != nil {
		writeStoreError(w)
		return
	}
	w.Header().Set("ETag", `"`+strconv.FormatInt(cfg.Version, 10)+`"`)
	writeJSON(w, http.StatusOK, cfg)
}

func (a *application) handleConfigUpdate(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("If-Match") == "" {
		writeJSON(w, http.StatusPreconditionRequired, errorResponse{Error: "If-Match is required"})
		return
	}
	expected, err := parseGeneration(r.Header.Get("If-Match"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid If-Match"})
		return
	}
	var req configUpdateRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	if err := validateConfigUpdate(req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: err.Error()})
		return
	}

	cfg, err := a.store.updateConfig(expected, req)
	if errors.Is(err, errConfigConflict) {
		writeJSON(w, http.StatusConflict, errorResponse{Error: "config version conflict"})
		return
	}
	if err != nil {
		writeStoreError(w)
		return
	}
	w.Header().Set("ETag", `"`+strconv.FormatInt(cfg.Version, 10)+`"`)
	writeJSON(w, http.StatusOK, cfg)
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

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
	authFailures authFailurePolicy
	authMetrics  *authMetrics
}

func routesWithStore(store Store) http.Handler {
	a := &application{store: store, tokens: tokenServiceFromEnv(), adminToken: os.Getenv("ADMIN_TOKEN"), gatewayToken: os.Getenv("GATEWAY_SHARED_TOKEN"), authFailures: authFailurePolicyFromEnv(), authMetrics: &authMetrics{}}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", a.handleHealth)
	mux.HandleFunc("GET /health/live", a.handleHealth)
	mux.HandleFunc("GET /health/ready", a.handleReady)
	mux.HandleFunc("GET /metrics", a.handlePrometheusMetrics)
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
		return
	}

	if req.ClientID == "" || req.Token == "" || len(req.ClientID) > 128 || len(req.Token) > 4096 {
		writeAPIError(w, r, http.StatusBadRequest, "INVALID_ARGUMENT", "client_id and token are required")
		return
	}

	limiter, hasLimiter := a.store.(authFailureStore)
	if hasLimiter {
		limited, err := limiter.authFailureLimited(req.ClientID, a.authFailures.limit)
		if err != nil {
			a.authMetrics.failureCounterErrors.Add(1)
			a.authMetrics.recordUnavailable()
			writeAuthUnavailable(w, r)
			return
		}
		if limited {
			a.authMetrics.rateLimited.Add(1)
			a.authMetrics.record(tokenAuthInvalid)
			writeJSON(w, http.StatusOK, authCheckResponse{Allowed: false, Code: "RATE_LIMITED", Reason: "rate limited"})
			return
		}
	}

	decision, err := a.verifyDigest(req.ClientID, a.tokens.digest(req.Token))
	if err != nil {
		a.authMetrics.recordUnavailable()
		writeAuthUnavailable(w, r)
		return
	}
	if decision != tokenAuthAllowed {
		count := int64(0)
		if hasLimiter {
			count, err = limiter.recordAuthFailure(req.ClientID, a.authFailures.window)
			if err != nil {
				a.authMetrics.failureCounterErrors.Add(1)
				a.authMetrics.recordUnavailable()
				writeAuthUnavailable(w, r)
				return
			}
		}

		code, reason := "INVALID_CREDENTIALS", "invalid token"
		if decision == tokenAuthDisabled {
			code, reason = "TOKEN_DISABLED", "token disabled"
		}
		if hasLimiter && count >= a.authFailures.limit {
			code, reason = "RATE_LIMITED", "rate limited"
			a.authMetrics.rateLimited.Add(1)
		}
		a.authMetrics.record(decision)
		writeJSON(w, http.StatusOK, authCheckResponse{Allowed: false, Code: code, Reason: reason})
		return
	}
	if hasLimiter {
		if err := limiter.clearAuthFailures(req.ClientID); err != nil {
			a.authMetrics.failureCounterErrors.Add(1)
			a.authMetrics.recordUnavailable()
			writeAuthUnavailable(w, r)
			return
		}
	}
	a.authMetrics.record(tokenAuthAllowed)

	writeJSON(w, http.StatusOK, authCheckResponse{
		Allowed: true,
		Code:    "OK",
		Reason:  "ok",
	})
}

func (a *application) verifyDigest(clientID, digest string) (tokenAuthDecision, error) {
	if verifier, ok := a.store.(tokenDecisionStore); ok {
		return verifier.verifyDigest(clientID, digest)
	}
	allowed, err := a.store.isDigestAllowed(clientID, digest)
	if allowed {
		return tokenAuthAllowed, err
	}
	return tokenAuthInvalid, err
}

func writeAuthUnavailable(w http.ResponseWriter, r *http.Request) {
	writeAPIError(w, r, http.StatusServiceUnavailable, "AUTH_UNAVAILABLE", "authentication service unavailable")
}

func (a *application) handleMetricsReport(w http.ResponseWriter, r *http.Request) {
	var req metricsReportRequest
	if err := decodeJSON(w, r, &req); err != nil {
		return
	}

	if req.GatewayID == "" {
		writeAPIError(w, r, http.StatusBadRequest, "INVALID_ARGUMENT", "gateway_id is required")
		return
	}

	status, err := a.store.saveMetrics(req)
	if err != nil {
		writeStoreError(w, r)
		return
	}
	writeJSON(w, http.StatusOK, status)
}

func (a *application) handleGatewayStatus(w http.ResponseWriter, r *http.Request) {
	status, ok, err := a.store.getStatus()
	if err != nil {
		writeStoreError(w, r)
		return
	}
	if !ok {
		writeAPIError(w, r, http.StatusNotFound, "NOT_FOUND", "gateway status not reported")
		return
	}

	writeJSON(w, http.StatusOK, gatewayStatusToView(status, time.Now().UTC()))
}

func (a *application) handleGatewaysList(w http.ResponseWriter, r *http.Request) {
	statuses, err := a.store.listGateways()
	if err != nil {
		writeStoreError(w, r)
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
		writeStoreError(w, r)
		return
	}
	if !ok {
		writeAPIError(w, r, http.StatusNotFound, "NOT_FOUND", "gateway status not reported")
		return
	}
	writeJSON(w, http.StatusOK, gatewayStatusToView(status, time.Now().UTC()))
}

func (a *application) handleClientsReport(w http.ResponseWriter, r *http.Request) {
	var req clientsReportRequest
	if err := decodeJSON(w, r, &req); err != nil {
		return
	}

	if req.GatewayID == "" {
		writeAPIError(w, r, http.StatusBadRequest, "INVALID_ARGUMENT", "gateway_id is required")
		return
	}

	if err := a.store.saveClients(req.GatewayID, req.Clients); err != nil {
		writeStoreError(w, r)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"success": true})
}

func (a *application) handleClients(w http.ResponseWriter, r *http.Request) {
	clients, err := a.store.getClients()
	if err != nil {
		writeStoreError(w, r)
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
		writeStoreError(w, r)
		return
	}
	if !ok {
		writeAPIError(w, r, http.StatusNotFound, "NOT_FOUND", "gateway clients not reported")
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
		return
	}

	if req.ClientID == "" {
		writeAPIError(w, r, http.StatusBadRequest, "INVALID_ARGUMENT", "client_id is required")
		return
	}
	token, err := a.tokens.generate()
	if err != nil {
		writeStoreError(w, r)
		return
	}
	now := nowRFC3339()
	record := tokenRecord{tokenEntry: tokenEntry{ClientID: req.ClientID, Generation: 1, CreatedAt: now, UpdatedAt: now}, Digest: a.tokens.digest(token)}
	if err := a.store.createToken(record); err != nil {
		if errors.Is(err, errTokenExists) {
			writeAPIError(w, r, http.StatusConflict, "CONFLICT", "token already exists")
			return
		}
		writeStoreError(w, r)
		return
	}
	writeJSON(w, http.StatusCreated, tokenSecretResponse{ClientID: req.ClientID, Token: token, Generation: 1})
}

func (a *application) handleTokensList(w http.ResponseWriter, r *http.Request) {
	entries, err := a.store.listTokens()
	if err != nil {
		writeStoreError(w, r)
		return
	}
	writeJSON(w, http.StatusOK, entries)
}

func (a *application) handleTokensDelete(w http.ResponseWriter, r *http.Request) {
	clientID := r.PathValue("client_id")
	if clientID == "" {
		writeAPIError(w, r, http.StatusBadRequest, "INVALID_ARGUMENT", "client_id is required")
		return
	}

	if err := a.store.disableToken(clientID, nowRFC3339()); err != nil {
		writeStoreError(w, r)
		return
	}
	writeJSON(w, http.StatusOK, successResponse{Success: true})
}

func (a *application) handleTokensRotate(w http.ResponseWriter, r *http.Request) {
	expected, err := parseGeneration(r.Header.Get("If-Match"))
	if err != nil {
		writeAPIError(w, r, http.StatusBadRequest, "INVALID_ARGUMENT", "invalid If-Match")
		return
	}
	token, err := a.tokens.generate()
	if err != nil {
		writeStoreError(w, r)
		return
	}
	record, err := a.store.rotateToken(r.PathValue("client_id"), expected, a.tokens.digest(token), nowRFC3339())
	if errors.Is(err, errTokenConflict) {
		writeAPIError(w, r, http.StatusConflict, "CONFLICT", "token generation conflict")
		return
	}
	if errors.Is(err, errTokenNotFound) {
		writeAPIError(w, r, http.StatusNotFound, "NOT_FOUND", "token not found")
		return
	}
	if err != nil {
		writeStoreError(w, r)
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
		writeStoreError(w, r)
		return
	}
	w.Header().Set("ETag", `"`+strconv.FormatInt(cfg.Version, 10)+`"`)
	writeJSON(w, http.StatusOK, cfg)
}

func (a *application) handleConfigUpdate(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("If-Match") == "" {
		writeAPIError(w, r, http.StatusPreconditionRequired, "PRECONDITION_REQUIRED", "If-Match is required")
		return
	}
	expected, err := parseGeneration(r.Header.Get("If-Match"))
	if err != nil {
		writeAPIError(w, r, http.StatusBadRequest, "INVALID_ARGUMENT", "invalid If-Match")
		return
	}
	var req configUpdateRequest
	if err := decodeJSON(w, r, &req); err != nil {
		return
	}

	if err := validateConfigUpdate(req); err != nil {
		writeAPIError(w, r, http.StatusBadRequest, "INVALID_ARGUMENT", err.Error())
		return
	}

	cfg, err := a.store.updateConfig(expected, req)
	if errors.Is(err, errConfigConflict) {
		writeAPIError(w, r, http.StatusConflict, "CONFLICT", "config version conflict")
		return
	}
	if err != nil {
		writeStoreError(w, r)
		return
	}
	w.Header().Set("ETag", `"`+strconv.FormatInt(cfg.Version, 10)+`"`)
	writeJSON(w, http.StatusOK, cfg)
}

func writeStoreError(w http.ResponseWriter, r *http.Request) {
	writeAPIError(w, r, http.StatusInternalServerError, "INTERNAL", storeErrorMessage)
}

func writeJSON(w http.ResponseWriter, statusCode int, payload any) {
	body, err := json.Marshal(payload)
	if err != nil {
		body = []byte(`{"code":"INTERNAL","message":"failed to encode response"}`)
		statusCode = http.StatusInternalServerError
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.WriteHeader(statusCode)
	if _, err := w.Write(body); err != nil {
		log.Printf("write json response failed: %v", err)
	}
}

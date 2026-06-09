package main

import (
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"time"
)

const validToken = "test-token"

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

func writeJSON(w http.ResponseWriter, statusCode int, payload any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	if err := json.NewEncoder(w).Encode(payload); err != nil {
		log.Printf("write json response failed: %v", err)
	}
}

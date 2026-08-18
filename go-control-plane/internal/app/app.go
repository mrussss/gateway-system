package app

import (
	"context"
	"errors"
	"log"
	"net/http"
	"os/signal"
	"syscall"
	"time"
)

// Run starts the control plane and blocks until it exits or receives a shutdown signal.
func Run() {
	config, err := loadApplicationConfigFromEnv()
	if err != nil {
		log.Fatalf("invalid startup configuration: %v", err)
	}
	metrics := newMetricsRegistry()
	appStore := newStoreFromEnv(metrics)
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	server := &http.Server{
		Addr:              ":8080",
		Handler:           routesWithConfigAndMetrics(appStore, config, metrics),
		ReadHeaderTimeout: 3 * time.Second,
		ReadTimeout:       5 * time.Second,
		WriteTimeout:      5 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	errCh := make(chan error, 1)
	go func() {
		log.Printf("go control plane listening on %s", server.Addr)
		errCh <- server.ListenAndServe()
	}()

	select {
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatalf("server failed: %v", err)
		}
	case <-ctx.Done():
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if err := server.Shutdown(shutdownCtx); err != nil {
			log.Printf("http shutdown failed: %v", err)
		}
	}
	if closer, ok := appStore.(storeCloser); ok {
		if err := closer.Close(); err != nil {
			log.Printf("store close failed: %v", err)
		}
	}
}

package app

import (
	"context"
	"errors"
	"log"
	"net"
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
	listener, err := net.Listen("tcp", server.Addr)
	if err != nil {
		log.Fatalf("server listen failed: %v", err)
	}
	log.Printf("go control plane listening on %s", listener.Addr())
	if err := serveUntilShutdown(ctx, server, listener, appStore, 10*time.Second); err != nil {
		log.Printf("control plane shutdown failed: %v", err)
	}
}

func serveUntilShutdown(
	ctx context.Context,
	server *http.Server,
	listener net.Listener,
	appStore Store,
	shutdownTimeout time.Duration,
) error {
	errCh := make(chan error, 1)
	go func() {
		errCh <- server.Serve(listener)
	}()

	var result error
	select {
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			result = err
		}
	case <-ctx.Done():
		shutdownCtx, cancel := context.WithTimeout(context.Background(), shutdownTimeout)
		if err := server.Shutdown(shutdownCtx); err != nil {
			result = err
		}
		cancel()
		if err := <-errCh; err != nil && !errors.Is(err, http.ErrServerClosed) && result == nil {
			result = err
		}
	}
	if closer, ok := appStore.(storeCloser); ok {
		if err := closer.Close(); err != nil {
			if result == nil {
				result = err
			} else {
				log.Printf("store close failed: %v", err)
			}
		}
	}
	return result
}

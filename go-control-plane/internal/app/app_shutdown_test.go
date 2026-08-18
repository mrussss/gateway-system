package app

import (
	"context"
	"io"
	"net"
	"net/http"
	"sync"
	"testing"
	"time"
)

type closeTrackingStore struct {
	Store
	once   sync.Once
	closed chan struct{}
}

func (s *closeTrackingStore) Close() error {
	s.once.Do(func() { close(s.closed) })
	return nil
}

func TestHTTPServerGracefulShutdownFinishesInflightAndClosesStore(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	entered := make(chan struct{})
	release := make(chan struct{})
	handler := http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		close(entered)
		<-release
		w.WriteHeader(http.StatusNoContent)
	})
	server := &http.Server{Handler: handler}
	storage := &closeTrackingStore{Store: newMemoryStore(), closed: make(chan struct{})}
	ctx, cancel := context.WithCancel(context.Background())
	result := make(chan error, 1)
	go func() {
		result <- serveUntilShutdown(ctx, server, listener, storage, time.Second)
	}()

	response := make(chan *http.Response, 1)
	clientError := make(chan error, 1)
	go func() {
		resp, requestErr := http.Get("http://" + listener.Addr().String())
		if requestErr != nil {
			clientError <- requestErr
			return
		}
		response <- resp
	}()
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("request did not enter handler")
	}

	cancel()
	deadline := time.Now().Add(time.Second)
	for {
		connection, dialErr := net.DialTimeout("tcp", listener.Addr().String(), 20*time.Millisecond)
		if dialErr != nil {
			break
		}
		connection.Close()
		if time.Now().After(deadline) {
			t.Fatal("listener still accepted connections during shutdown")
		}
		time.Sleep(5 * time.Millisecond)
	}
	close(release)

	select {
	case resp := <-response:
		defer resp.Body.Close()
		_, _ = io.Copy(io.Discard, resp.Body)
		if resp.StatusCode != http.StatusNoContent {
			t.Fatalf("in-flight status=%d", resp.StatusCode)
		}
	case requestErr := <-clientError:
		t.Fatalf("in-flight request failed: %v", requestErr)
	case <-time.After(time.Second):
		t.Fatal("in-flight request did not complete")
	}
	if err := <-result; err != nil {
		t.Fatalf("serveUntilShutdown: %v", err)
	}
	select {
	case <-storage.closed:
	default:
		t.Fatal("store Close was not called")
	}
}

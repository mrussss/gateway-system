package app

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log"
	"mime"
	"net/http"
	"runtime/debug"
	"time"
)

const maxRequestBodyBytes = 1 << 20

type requestIDKey struct{}

type apiErrorResponse struct {
	RequestID string `json:"request_id"`
	Code      string `json:"code"`
	Message   string `json:"message"`
}

func requestIDFromContext(ctx context.Context) string {
	value, _ := ctx.Value(requestIDKey{}).(string)
	return value
}

func newRequestID() string {
	var value [12]byte
	if _, err := rand.Read(value[:]); err != nil {
		return "req-unknown"
	}
	return "req-" + hex.EncodeToString(value[:])
}

func middleware(next http.Handler, metrics *metricsRegistry) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		started := time.Now()
		route := "unmatched"
		if matcher, ok := next.(interface {
			Handler(*http.Request) (http.Handler, string)
		}); ok {
			_, pattern := matcher.Handler(r)
			route = canonicalRoute(pattern)
		}
		metrics.httpInFlight.WithLabelValues(r.Method, route).Inc()
		defer metrics.httpInFlight.WithLabelValues(r.Method, route).Dec()

		requestID := r.Header.Get("X-Request-ID")
		if requestID == "" {
			requestID = newRequestID()
		}
		w.Header().Set("X-Request-ID", requestID)
		r = r.WithContext(context.WithValue(r.Context(), requestIDKey{}, requestID))

		response := &statusWriter{ResponseWriter: w, status: http.StatusOK}
		defer func() {
			if recovered := recover(); recovered != nil {
				metrics.panics.WithLabelValues(route).Inc()
				log.Printf("panic recovered request_id=%s error=%v stack=%s", requestID, recovered, debug.Stack())
				writeAPIError(response, r, http.StatusInternalServerError, "INTERNAL", "internal server error")
			}
			metrics.httpRequests.WithLabelValues(r.Method, route, statusLabel(response.status)).Inc()
			metrics.httpDuration.WithLabelValues(r.Method, route).Observe(time.Since(started).Seconds())
			entry, _ := json.Marshal(map[string]any{
				"request_id": requestID, "method": r.Method, "path": r.URL.Path,
				"status": response.status, "duration_ms": time.Since(started).Milliseconds(),
			})
			log.Print(string(entry))
		}()

		if r.Body != nil {
			r.Body = http.MaxBytesReader(response, r.Body, maxRequestBodyBytes)
		}
		if (r.Method == http.MethodPost || r.Method == http.MethodPut) && r.ContentLength != 0 {
			contentType := r.Header.Get("Content-Type")
			mediaType, _, err := mime.ParseMediaType(contentType)
			if err != nil || mediaType != "application/json" {
				writeAPIError(response, r, http.StatusUnsupportedMediaType, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json")
				return
			}
		}

		ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
		defer cancel()
		next.ServeHTTP(response, r.WithContext(ctx))
	})
}

type statusWriter struct {
	http.ResponseWriter
	status int
}

func (w *statusWriter) WriteHeader(status int) {
	w.status = status
	w.ResponseWriter.WriteHeader(status)
}

func writeAPIError(w http.ResponseWriter, r *http.Request, status int, code, message string) {
	writeJSON(w, status, apiErrorResponse{RequestID: requestIDFromContext(r.Context()), Code: code, Message: message})
}

func decodeJSON(w http.ResponseWriter, r *http.Request, destination any) error {
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(destination); err != nil {
		var tooLarge *http.MaxBytesError
		if errors.As(err, &tooLarge) {
			writeAPIError(w, r, http.StatusRequestEntityTooLarge, "BODY_TOO_LARGE", "request body too large")
			return err
		}
		writeAPIError(w, r, http.StatusBadRequest, "INVALID_ARGUMENT", "invalid request body")
		return err
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		writeAPIError(w, r, http.StatusBadRequest, "INVALID_ARGUMENT", "request body must contain exactly one JSON value")
		return errors.New("request body must contain exactly one JSON value")
	}
	return nil
}

type storeHealthChecker interface {
	Ping(context.Context) error
}

type storeCloser interface {
	Close() error
}

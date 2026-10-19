package app

import (
	"errors"
	"os"
	"strconv"
	"strings"
)

var errConfigConflict = errors.New("config version conflict")

func readPositiveEnv(name string, fallback int) int {
	raw := os.Getenv(name)
	if raw == "" {
		return fallback
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value <= 0 {
		return fallback
	}
	return value
}

func defaultRuntimeConfig() runtimeConfig {
	return runtimeConfig{
		Version:                       1,
		MaxPayloadSize:                1048576,
		MaxConnectionsPerClient:       2,
		MaxRequestsPerClientPerSecond: 100,
		SlowClientOutputLimit:         8388608,
		LogLevel:                      "INFO",
		RequestQueueCapacityDisplay:   4096,
	}
}

func validateConfigUpdate(req configUpdateRequest) error {
	const maxGatewayInt = 1<<31 - 1
	switch {
	case req.MaxPayloadSize < 10 || req.MaxPayloadSize > 4194314:
		return errors.New("max_payload_size is outside the supported range")
	case req.MaxConnectionsPerClient <= 0 || req.MaxConnectionsPerClient > maxGatewayInt:
		return errors.New("max_connections_per_client must be in [1, 2147483647]")
	case req.MaxRequestsPerClientPerSecond <= 0 || req.MaxRequestsPerClientPerSecond > maxGatewayInt:
		return errors.New("max_requests_per_client_per_second must be in [1, 2147483647]")
	case req.SlowClientOutputLimit < req.MaxPayloadSize+4 || req.SlowClientOutputLimit > 67108864:
		return errors.New("slow_client_output_limit must cover one frame including its length prefix and stay below the hard limit")
	case strings.ToUpper(req.LogLevel) != "DEBUG" && strings.ToUpper(req.LogLevel) != "INFO" && strings.ToUpper(req.LogLevel) != "WARN" && strings.ToUpper(req.LogLevel) != "ERROR":
		return errors.New("log_level must be DEBUG, INFO, WARN, or ERROR")
	default:
		return nil
	}
}

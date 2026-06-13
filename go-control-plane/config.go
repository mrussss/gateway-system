package main

import "errors"

func defaultRuntimeConfig() runtimeConfig {
	return runtimeConfig{
		Version:                       1,
		AuthTimeoutMS:                 1000,
		MaxPayloadSize:                4194314,
		MaxConnectionsPerClient:       2,
		MaxRequestsPerClientPerSecond: 100,
		FailOpen:                      false,
	}
}

func validateConfigUpdate(req configUpdateRequest) error {
	switch {
	case req.AuthTimeoutMS <= 0:
		return errors.New("auth_timeout_ms must be positive")
	case req.MaxPayloadSize <= 0:
		return errors.New("max_payload_size must be positive")
	case req.MaxConnectionsPerClient <= 0:
		return errors.New("max_connections_per_client must be positive")
	case req.MaxRequestsPerClientPerSecond <= 0:
		return errors.New("max_requests_per_client_per_second must be positive")
	default:
		return nil
	}
}

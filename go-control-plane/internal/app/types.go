package app

type authCheckRequest struct {
	ClientID string `json:"client_id"`
	Token    string `json:"token"`
}

type authCheckResponse struct {
	Allowed bool   `json:"allowed"`
	Code    string `json:"code"`
	Reason  string `json:"reason"`
}

type healthResponse struct {
	Status string `json:"status"`
}

type runtimeConfig struct {
	Version                       int64  `json:"version"`
	MaxPayloadSize                int    `json:"max_payload_size"`
	MaxConnectionsPerClient       int    `json:"max_connections_per_client"`
	MaxRequestsPerClientPerSecond int    `json:"max_requests_per_client_per_second"`
	SlowClientOutputLimit         int    `json:"slow_client_output_limit"`
	LogLevel                      string `json:"log_level"`
	RequestQueueCapacityDisplay   int    `json:"request_queue_capacity_display"`
}

type configUpdateRequest struct {
	MaxPayloadSize                int    `json:"max_payload_size"`
	MaxConnectionsPerClient       int    `json:"max_connections_per_client"`
	MaxRequestsPerClientPerSecond int    `json:"max_requests_per_client_per_second"`
	SlowClientOutputLimit         int    `json:"slow_client_output_limit"`
	LogLevel                      string `json:"log_level"`
}

type successResponse struct {
	Success bool `json:"success"`
}

type tokenEntry struct {
	ClientID   string `json:"client_id"`
	Generation int64  `json:"generation"`
	CreatedAt  string `json:"created_at"`
	UpdatedAt  string `json:"updated_at"`
	Disabled   bool   `json:"disabled"`
}

type tokenUpsertRequest struct {
	ClientID string `json:"client_id"`
}

type tokenRecord struct {
	tokenEntry
	Digest string `json:"-"`
}

type tokenSecretResponse struct {
	ClientID   string `json:"client_id"`
	Token      string `json:"token"`
	Generation int64  `json:"generation"`
}

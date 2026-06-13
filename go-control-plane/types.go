package main

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

type configReloadResponse struct {
	Success bool   `json:"success"`
	Message string `json:"message"`
	Version int64  `json:"version"`
}

type runtimeConfig struct {
	Version                       int64 `json:"version"`
	AuthTimeoutMS                 int   `json:"auth_timeout_ms"`
	MaxPayloadSize                int   `json:"max_payload_size"`
	MaxConnectionsPerClient       int   `json:"max_connections_per_client"`
	MaxRequestsPerClientPerSecond int   `json:"max_requests_per_client_per_second"`
	FailOpen                      bool  `json:"fail_open"`
}

type configUpdateRequest struct {
	AuthTimeoutMS                 int  `json:"auth_timeout_ms"`
	MaxPayloadSize                int  `json:"max_payload_size"`
	MaxConnectionsPerClient       int  `json:"max_connections_per_client"`
	MaxRequestsPerClientPerSecond int  `json:"max_requests_per_client_per_second"`
	FailOpen                      bool `json:"fail_open"`
}

type metricsReportRequest struct {
	GatewayID         string `json:"gateway_id"`
	ActiveConnections int64  `json:"active_connections"`
	TotalMessages     int64  `json:"total_messages"`
	BytesIn           int64  `json:"bytes_in"`
	BytesOut          int64  `json:"bytes_out"`
	ErrorCount        int64  `json:"error_count"`
	Timestamp         int64  `json:"timestamp"`
}

type gatewayStatusResponse struct {
	GatewayID         string `json:"gateway_id"`
	ActiveConnections int64  `json:"active_connections"`
	TotalMessages     int64  `json:"total_messages"`
	BytesIn           int64  `json:"bytes_in"`
	BytesOut          int64  `json:"bytes_out"`
	ErrorCount        int64  `json:"error_count"`
	LastReportTime    string `json:"last_report_time"`
}

type gatewayStatusView struct {
	GatewayID              string `json:"gateway_id"`
	ActiveConnections      int64  `json:"active_connections"`
	TotalMessages          int64  `json:"total_messages"`
	BytesIn                int64  `json:"bytes_in"`
	BytesOut               int64  `json:"bytes_out"`
	ErrorCount             int64  `json:"error_count"`
	LastReportTime         string `json:"last_report_time"`
	Online                 bool   `json:"online"`
	Status                 string `json:"status"`
	SecondsSinceLastReport int64  `json:"seconds_since_last_report"`
}

type clientInfo struct {
	ClientID    string `json:"client_id"`
	RemoteAddr  string `json:"remote_addr"`
	ConnectedAt string `json:"connected_at"`
}

type clientsReportRequest struct {
	GatewayID string       `json:"gateway_id"`
	Clients   []clientInfo `json:"clients"`
}

type errorResponse struct {
	Error string `json:"error"`
}

type successResponse struct {
	Success bool `json:"success"`
}

type tokenEntry struct {
	ClientID string `json:"client_id"`
}

type tokenUpsertRequest struct {
	ClientID string `json:"client_id"`
	Token    string `json:"token"`
}

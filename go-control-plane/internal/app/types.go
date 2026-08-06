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
}

type configUpdateRequest struct {
	MaxPayloadSize                int    `json:"max_payload_size"`
	MaxConnectionsPerClient       int    `json:"max_connections_per_client"`
	MaxRequestsPerClientPerSecond int    `json:"max_requests_per_client_per_second"`
	SlowClientOutputLimit         int    `json:"slow_client_output_limit"`
	LogLevel                      string `json:"log_level"`
}

type controlPlaneTelemetry struct {
	ControlPlaneRequestsAuth          int64 `json:"control_plane_requests_auth"`
	ControlPlaneRequestsConfig        int64 `json:"control_plane_requests_config"`
	ControlPlaneRequestsMetricsReport int64 `json:"control_plane_requests_metrics_report"`
	ControlPlaneRequestsClientsReport int64 `json:"control_plane_requests_clients_report"`
	ControlPlaneDurationTotalUS       int64 `json:"control_plane_duration_total_us"`
	ControlPlaneErrorsResolve         int64 `json:"control_plane_errors_resolve"`
	ControlPlaneErrorsDeadline        int64 `json:"control_plane_errors_deadline"`
	ControlPlaneErrorsConnect         int64 `json:"control_plane_errors_connect"`
	ControlPlaneErrorsSend            int64 `json:"control_plane_errors_send"`
	ControlPlaneErrorsReceive         int64 `json:"control_plane_errors_receive"`
	ControlPlaneErrorsProtocol        int64 `json:"control_plane_errors_protocol"`
	ControlPlaneErrorsStatus          int64 `json:"control_plane_errors_status"`
	ControlPlaneErrorsJSON            int64 `json:"control_plane_errors_json"`
	ControlPlaneErrorsOversize        int64 `json:"control_plane_errors_oversize"`
}

type metricsReportRequest struct {
	controlPlaneTelemetry
	GatewayID              string `json:"gateway_id"`
	GatewayBootID          string `json:"gateway_boot_id"`
	ProcessStartTime       int64  `json:"process_start_time"`
	ActiveConnections      int64  `json:"active_connections"`
	TotalRequests          int64  `json:"total_requests"`
	TotalMessages          int64  `json:"total_messages,omitempty"`
	BytesIn                int64  `json:"bytes_in"`
	BytesOut               int64  `json:"bytes_out"`
	ErrorCount             int64  `json:"error_count"`
	RequestQueueCapacity   int64  `json:"request_queue_capacity"`
	RequestQueueBacklog    int64  `json:"request_queue_backlog"`
	RequestQueuePeak       int64  `json:"request_queue_peak"`
	RequestQueueRejected   int64  `json:"request_queue_rejected"`
	AuthQueueCapacity      int64  `json:"auth_queue_capacity"`
	AuthQueueBacklog       int64  `json:"auth_queue_backlog"`
	AuthQueuePeak          int64  `json:"auth_queue_peak"`
	AuthQueueRejected      int64  `json:"auth_queue_rejected"`
	AuthInFlight           int64  `json:"auth_in_flight"`
	AuthTasksCancelled     int64  `json:"auth_tasks_cancelled_before_start"`
	ResponseQueueCapacity  int64  `json:"response_queue_capacity"`
	ResponseQueueBacklog   int64  `json:"response_queue_backlog"`
	ResponseQueuePeak      int64  `json:"response_queue_peak"`
	ResponseQueueRejected  int64  `json:"response_queue_rejected"`
	ResponseRejectedNormal int64  `json:"response_queue_rejected_normal"`
	ResponseRejectedAuth   int64  `json:"response_queue_rejected_auth"`
	SlowClientClosed       int64  `json:"slow_client_closed"`
	StaleResponseDropped   int64  `json:"stale_response_dropped"`
	AuthSuccess            int64  `json:"auth_success"`
	AuthFailure            int64  `json:"auth_failure"`
	AuthAllowed            int64  `json:"auth_allowed"`
	AuthDenied             int64  `json:"auth_denied"`
	AuthUnavailable        int64  `json:"auth_unavailable"`
	AuthDurationCount      int64  `json:"auth_duration_count"`
	AuthDurationTotalUS    int64  `json:"auth_duration_total_us"`
	RuntimeConfigVersion   int64  `json:"runtime_config_version"`
	ServerState            string `json:"server_state"`
	Timestamp              int64  `json:"timestamp"`
}

type gatewayStatusResponse struct {
	controlPlaneTelemetry
	GatewayID              string `json:"gateway_id"`
	GatewayBootID          string `json:"gateway_boot_id"`
	ProcessStartTime       int64  `json:"process_start_time"`
	ActiveConnections      int64  `json:"active_connections"`
	TotalMessages          int64  `json:"total_requests"`
	BytesIn                int64  `json:"bytes_in"`
	BytesOut               int64  `json:"bytes_out"`
	ErrorCount             int64  `json:"errors"`
	RequestQueueCapacity   int64  `json:"request_queue_capacity"`
	RequestQueueBacklog    int64  `json:"request_queue_backlog"`
	RequestQueuePeak       int64  `json:"request_queue_peak"`
	RequestQueueRejected   int64  `json:"request_queue_rejected"`
	AuthQueueCapacity      int64  `json:"auth_queue_capacity"`
	AuthQueueBacklog       int64  `json:"auth_queue_backlog"`
	AuthQueuePeak          int64  `json:"auth_queue_peak"`
	AuthQueueRejected      int64  `json:"auth_queue_rejected"`
	AuthInFlight           int64  `json:"auth_in_flight"`
	AuthTasksCancelled     int64  `json:"auth_tasks_cancelled_before_start"`
	ResponseQueueCapacity  int64  `json:"response_queue_capacity"`
	ResponseQueueBacklog   int64  `json:"response_queue_backlog"`
	ResponseQueuePeak      int64  `json:"response_queue_peak"`
	ResponseQueueRejected  int64  `json:"response_queue_rejected"`
	ResponseRejectedNormal int64  `json:"response_queue_rejected_normal"`
	ResponseRejectedAuth   int64  `json:"response_queue_rejected_auth"`
	SlowClientClosed       int64  `json:"slow_client_closed"`
	StaleResponseDropped   int64  `json:"stale_response_dropped"`
	AuthSuccess            int64  `json:"auth_success"`
	AuthFailure            int64  `json:"auth_failure"`
	AuthAllowed            int64  `json:"auth_allowed"`
	AuthDenied             int64  `json:"auth_denied"`
	AuthUnavailable        int64  `json:"auth_unavailable"`
	AuthDurationCount      int64  `json:"auth_duration_count"`
	AuthDurationTotalUS    int64  `json:"auth_duration_total_us"`
	RuntimeConfigVersion   int64  `json:"runtime_config_version"`
	ServerState            string `json:"server_state"`
	LastReportTime         string `json:"reported_at"`
}

type gatewayStatusView struct {
	controlPlaneTelemetry
	GatewayID              string `json:"gateway_id"`
	GatewayBootID          string `json:"gateway_boot_id"`
	ProcessStartTime       int64  `json:"process_start_time"`
	ActiveConnections      int64  `json:"active_connections"`
	TotalMessages          int64  `json:"total_requests"`
	BytesIn                int64  `json:"bytes_in"`
	BytesOut               int64  `json:"bytes_out"`
	ErrorCount             int64  `json:"errors"`
	RequestQueueCapacity   int64  `json:"request_queue_capacity"`
	RequestQueueBacklog    int64  `json:"request_queue_backlog"`
	RequestQueuePeak       int64  `json:"request_queue_peak"`
	RequestQueueRejected   int64  `json:"request_queue_rejected"`
	AuthQueueCapacity      int64  `json:"auth_queue_capacity"`
	AuthQueueBacklog       int64  `json:"auth_queue_backlog"`
	AuthQueuePeak          int64  `json:"auth_queue_peak"`
	AuthQueueRejected      int64  `json:"auth_queue_rejected"`
	AuthInFlight           int64  `json:"auth_in_flight"`
	AuthTasksCancelled     int64  `json:"auth_tasks_cancelled_before_start"`
	ResponseQueueCapacity  int64  `json:"response_queue_capacity"`
	ResponseQueueBacklog   int64  `json:"response_queue_backlog"`
	ResponseQueuePeak      int64  `json:"response_queue_peak"`
	ResponseQueueRejected  int64  `json:"response_queue_rejected"`
	ResponseRejectedNormal int64  `json:"response_queue_rejected_normal"`
	ResponseRejectedAuth   int64  `json:"response_queue_rejected_auth"`
	SlowClientClosed       int64  `json:"slow_client_closed"`
	StaleResponseDropped   int64  `json:"stale_response_dropped"`
	AuthSuccess            int64  `json:"auth_success"`
	AuthFailure            int64  `json:"auth_failure"`
	AuthAllowed            int64  `json:"auth_allowed"`
	AuthDenied             int64  `json:"auth_denied"`
	AuthUnavailable        int64  `json:"auth_unavailable"`
	AuthDurationCount      int64  `json:"auth_duration_count"`
	AuthDurationTotalUS    int64  `json:"auth_duration_total_us"`
	RuntimeConfigVersion   int64  `json:"runtime_config_version"`
	ServerState            string `json:"server_state"`
	LastReportTime         string `json:"reported_at"`
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

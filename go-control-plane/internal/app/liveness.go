package app

import (
	"sort"
	"time"
)

func statusFromMetrics(req metricsReportRequest) gatewayStatusResponse {
	reportTime := time.Now().UTC()
	if req.Timestamp > 0 {
		reportTime = time.Unix(req.Timestamp, 0).UTC()
	}

	totalRequests := req.TotalRequests
	if totalRequests == 0 {
		// Accept the pre-v2 field while deployed gateways roll forward.
		totalRequests = req.TotalMessages
	}

	return gatewayStatusResponse{
		GatewayID:             req.GatewayID,
		GatewayBootID:         req.GatewayBootID,
		ProcessStartTime:      req.ProcessStartTime,
		ActiveConnections:     req.ActiveConnections,
		TotalMessages:         totalRequests,
		BytesIn:               req.BytesIn,
		BytesOut:              req.BytesOut,
		ErrorCount:            req.ErrorCount,
		RequestQueueCapacity:  req.RequestQueueCapacity,
		RequestQueueBacklog:   req.RequestQueueBacklog,
		RequestQueuePeak:      req.RequestQueuePeak,
		RequestQueueRejected:  req.RequestQueueRejected,
		ResponseQueueCapacity: req.ResponseQueueCapacity,
		ResponseQueueBacklog:  req.ResponseQueueBacklog,
		ResponseQueuePeak:     req.ResponseQueuePeak,
		ResponseQueueRejected: req.ResponseQueueRejected,
		SlowClientClosed:      req.SlowClientClosed,
		StaleResponseDropped:  req.StaleResponseDropped,
		AuthSuccess:           req.AuthSuccess,
		AuthFailure:           req.AuthFailure,
		RuntimeConfigVersion:  req.RuntimeConfigVersion,
		ServerState:           req.ServerState,
		LastReportTime:        reportTime.Format(time.RFC3339),
	}
}

func gatewayStatusToView(status gatewayStatusResponse, now time.Time) gatewayStatusView {
	secondsSince := int64(-1)
	online := false

	lastReportTime, err := time.Parse(time.RFC3339, status.LastReportTime)
	if err == nil {
		delta := now.Sub(lastReportTime)
		if delta < 0 {
			delta = 0
		}
		secondsSince = int64(delta.Seconds())
		online = delta <= defaultGatewayOfflineAfter
	}

	state := "offline"
	if online {
		state = "online"
	}

	return gatewayStatusView{
		GatewayID:              status.GatewayID,
		GatewayBootID:          status.GatewayBootID,
		ProcessStartTime:       status.ProcessStartTime,
		ActiveConnections:      status.ActiveConnections,
		TotalMessages:          status.TotalMessages,
		BytesIn:                status.BytesIn,
		BytesOut:               status.BytesOut,
		ErrorCount:             status.ErrorCount,
		RequestQueueCapacity:   status.RequestQueueCapacity,
		RequestQueueBacklog:    status.RequestQueueBacklog,
		RequestQueuePeak:       status.RequestQueuePeak,
		RequestQueueRejected:   status.RequestQueueRejected,
		ResponseQueueCapacity:  status.ResponseQueueCapacity,
		ResponseQueueBacklog:   status.ResponseQueueBacklog,
		ResponseQueuePeak:      status.ResponseQueuePeak,
		ResponseQueueRejected:  status.ResponseQueueRejected,
		SlowClientClosed:       status.SlowClientClosed,
		StaleResponseDropped:   status.StaleResponseDropped,
		AuthSuccess:            status.AuthSuccess,
		AuthFailure:            status.AuthFailure,
		RuntimeConfigVersion:   status.RuntimeConfigVersion,
		ServerState:            status.ServerState,
		LastReportTime:         status.LastReportTime,
		Online:                 online,
		Status:                 state,
		SecondsSinceLastReport: secondsSince,
	}
}

func sortGatewayStatuses(statuses []gatewayStatusResponse) {
	sort.Slice(statuses, func(i, j int) bool {
		return statuses[i].GatewayID < statuses[j].GatewayID
	})
}

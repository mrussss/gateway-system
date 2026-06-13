package main

import (
	"sort"
	"time"
)

func statusFromMetrics(req metricsReportRequest) gatewayStatusResponse {
	reportTime := time.Now().UTC()
	if req.Timestamp > 0 {
		reportTime = time.Unix(req.Timestamp, 0).UTC()
	}

	return gatewayStatusResponse{
		GatewayID:         req.GatewayID,
		ActiveConnections: req.ActiveConnections,
		TotalMessages:     req.TotalMessages,
		BytesIn:           req.BytesIn,
		BytesOut:          req.BytesOut,
		ErrorCount:        req.ErrorCount,
		LastReportTime:    reportTime.Format(time.RFC3339),
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
		ActiveConnections:      status.ActiveConnections,
		TotalMessages:          status.TotalMessages,
		BytesIn:                status.BytesIn,
		BytesOut:               status.BytesOut,
		ErrorCount:             status.ErrorCount,
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

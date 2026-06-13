package main

import (
	"sort"
	"sync"
)

type memoryStore struct {
	mu               sync.RWMutex
	status           gatewayStatusResponse
	hasStatus        bool
	clients          []clientInfo
	statusByGateway  map[string]gatewayStatusResponse
	clientsByGateway map[string][]clientInfo
	gateways         map[string]struct{}
	tokens           map[string]string
	config           runtimeConfig
}

func newMemoryStore() *memoryStore {
	return &memoryStore{
		statusByGateway:  map[string]gatewayStatusResponse{},
		clientsByGateway: map[string][]clientInfo{},
		gateways:         map[string]struct{}{},
		tokens:           map[string]string{},
		config:           defaultRuntimeConfig(),
	}
}

func (s *memoryStore) saveMetrics(req metricsReportRequest) (gatewayStatusResponse, error) {
	status := statusFromMetrics(req)

	s.mu.Lock()
	defer s.mu.Unlock()
	s.status = status
	s.hasStatus = true
	s.statusByGateway[req.GatewayID] = status
	s.gateways[req.GatewayID] = struct{}{}
	return status, nil
}

func (s *memoryStore) getStatus() (gatewayStatusResponse, bool, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.status, s.hasStatus, nil
}

func (s *memoryStore) saveClients(gatewayID string, clients []clientInfo) error {
	copied := append([]clientInfo(nil), clients...)

	s.mu.Lock()
	defer s.mu.Unlock()
	s.clients = copied
	s.clientsByGateway[gatewayID] = copied
	s.gateways[gatewayID] = struct{}{}
	return nil
}

func (s *memoryStore) getClients() ([]clientInfo, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return append([]clientInfo(nil), s.clients...), nil
}

func (s *memoryStore) listGateways() ([]gatewayStatusResponse, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	statuses := make([]gatewayStatusResponse, 0, len(s.statusByGateway))
	for _, status := range s.statusByGateway {
		statuses = append(statuses, status)
	}
	sortGatewayStatuses(statuses)
	return statuses, nil
}

func (s *memoryStore) getGatewayStatus(gatewayID string) (gatewayStatusResponse, bool, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	status, ok := s.statusByGateway[gatewayID]
	return status, ok, nil
}

func (s *memoryStore) getGatewayClients(gatewayID string) ([]clientInfo, bool, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	clients, ok := s.clientsByGateway[gatewayID]
	if !ok {
		return nil, false, nil
	}
	return append([]clientInfo(nil), clients...), true, nil
}

func (s *memoryStore) setToken(clientID string, token string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.tokens[clientID] = token
	return nil
}

func (s *memoryStore) deleteToken(clientID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.tokens, clientID)
	return nil
}

func (s *memoryStore) isAllowed(clientID string, token string) (bool, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	storedToken, ok := s.tokens[clientID]
	return ok && storedToken == token, nil
}

func (s *memoryStore) listTokens() ([]tokenEntry, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	entries := make([]tokenEntry, 0, len(s.tokens))
	for clientID := range s.tokens {
		entries = append(entries, tokenEntry{ClientID: clientID})
	}
	sort.Slice(entries, func(i, j int) bool {
		return entries[i].ClientID < entries[j].ClientID
	})
	return entries, nil
}

func (s *memoryStore) getConfig() (runtimeConfig, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.config, nil
}

func (s *memoryStore) updateConfig(req configUpdateRequest) (runtimeConfig, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.config = runtimeConfig{
		Version:                       s.config.Version + 1,
		AuthTimeoutMS:                 req.AuthTimeoutMS,
		MaxPayloadSize:                req.MaxPayloadSize,
		MaxConnectionsPerClient:       req.MaxConnectionsPerClient,
		MaxRequestsPerClientPerSecond: req.MaxRequestsPerClientPerSecond,
		FailOpen:                      req.FailOpen,
	}
	return s.config, nil
}

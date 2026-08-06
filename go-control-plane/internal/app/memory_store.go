package app

import (
	"sort"
	"strings"
	"sync"
	"time"
)

type memoryAuthFailure struct {
	count     int64
	expiresAt time.Time
}

type memoryStore struct {
	mu               sync.RWMutex
	status           gatewayStatusResponse
	hasStatus        bool
	clients          []clientInfo
	statusByGateway  map[string]gatewayStatusResponse
	clientsByGateway map[string][]clientInfo
	gateways         map[string]struct{}
	tokens           map[string]tokenRecord
	authFailures     map[string]memoryAuthFailure
	config           runtimeConfig
}

func newMemoryStore() *memoryStore {
	return &memoryStore{
		clients:          make([]clientInfo, 0),
		statusByGateway:  map[string]gatewayStatusResponse{},
		clientsByGateway: map[string][]clientInfo{},
		gateways:         map[string]struct{}{},
		tokens:           map[string]tokenRecord{},
		authFailures:     map[string]memoryAuthFailure{},
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
	copied := append(make([]clientInfo, 0, len(clients)), clients...)

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
	return append(make([]clientInfo, 0, len(s.clients)), s.clients...), nil
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
	return append(make([]clientInfo, 0, len(clients)), clients...), true, nil
}

func (s *memoryStore) isDigestAllowed(clientID, digest string) (bool, error) {
	decision, err := s.verifyDigest(clientID, digest)
	return decision == tokenAuthAllowed, err
}

func (s *memoryStore) verifyDigest(clientID, digest string) (tokenAuthDecision, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	record, ok := s.tokens[clientID]
	if !ok {
		return tokenAuthInvalid, nil
	}
	if record.Disabled {
		return tokenAuthDisabled, nil
	}
	if !digestEqual(record.Digest, digest) {
		return tokenAuthInvalid, nil
	}
	return tokenAuthAllowed, nil
}

func (s *memoryStore) authFailureLimited(clientID string, limit int64) (bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	counter, ok := s.authFailures[clientID]
	if !ok {
		return false, nil
	}
	if !time.Now().Before(counter.expiresAt) {
		delete(s.authFailures, clientID)
		return false, nil
	}
	return counter.count >= limit, nil
}

func (s *memoryStore) recordAuthFailure(clientID string, window time.Duration) (int64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := time.Now()
	counter, ok := s.authFailures[clientID]
	if !ok || !now.Before(counter.expiresAt) {
		counter = memoryAuthFailure{expiresAt: now.Add(window)}
	}
	counter.count++
	s.authFailures[clientID] = counter
	return counter.count, nil
}

func (s *memoryStore) clearAuthFailures(clientID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.authFailures, clientID)
	return nil
}

func (s *memoryStore) createToken(record tokenRecord) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, exists := s.tokens[record.ClientID]; exists {
		return errTokenExists
	}
	s.tokens[record.ClientID] = record
	return nil
}

func (s *memoryStore) rotateToken(clientID string, expected int64, digest, updatedAt string) (tokenRecord, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	record, ok := s.tokens[clientID]
	if !ok {
		return tokenRecord{}, errTokenNotFound
	}
	if record.Generation != expected {
		return tokenRecord{}, errTokenConflict
	}
	record.Generation++
	record.Digest = digest
	record.UpdatedAt = updatedAt
	record.Disabled = false
	s.tokens[clientID] = record
	return record, nil
}

func (s *memoryStore) disableToken(clientID, updatedAt string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	record, ok := s.tokens[clientID]
	if !ok {
		return errTokenNotFound
	}
	record.Disabled = true
	record.UpdatedAt = updatedAt
	s.tokens[clientID] = record
	return nil
}

func (s *memoryStore) listTokens() ([]tokenEntry, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	entries := make([]tokenEntry, 0, len(s.tokens))
	for _, record := range s.tokens {
		entries = append(entries, record.tokenEntry)
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

func (s *memoryStore) updateConfig(expectedVersion int64, req configUpdateRequest) (runtimeConfig, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.config.Version != expectedVersion {
		return runtimeConfig{}, errConfigConflict
	}

	s.config = runtimeConfig{
		Version:                       s.config.Version + 1,
		MaxPayloadSize:                req.MaxPayloadSize,
		MaxConnectionsPerClient:       req.MaxConnectionsPerClient,
		MaxRequestsPerClientPerSecond: req.MaxRequestsPerClientPerSecond,
		SlowClientOutputLimit:         req.SlowClientOutputLimit,
		LogLevel:                      strings.ToUpper(req.LogLevel),
	}
	return s.config, nil
}

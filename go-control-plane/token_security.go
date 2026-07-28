package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"os"
	"strconv"
	"strings"
	"time"
)

var (
	errTokenExists   = errors.New("token already exists")
	errTokenNotFound = errors.New("token not found")
	errTokenConflict = errors.New("token generation conflict")
)

type tokenService struct{ pepper []byte }

func tokenServiceFromEnv() tokenService {
	pepper := os.Getenv("TOKEN_PEPPER")
	if pepper == "" {
		pepper = "development-only-token-pepper"
	}
	return tokenService{pepper: []byte(pepper)}
}

func (s tokenService) generate() (string, error) {
	value := make([]byte, 32)
	if _, err := rand.Read(value); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(value), nil
}

func (s tokenService) digest(token string) string {
	mac := hmac.New(sha256.New, s.pepper)
	_, _ = mac.Write([]byte(token))
	return hex.EncodeToString(mac.Sum(nil))
}

func digestEqual(left, right string) bool {
	return hmac.Equal([]byte(left), []byte(right))
}

func parseGeneration(value string) (int64, error) {
	value = strings.Trim(value, "\"")
	generation, err := strconv.ParseInt(value, 10, 64)
	if err != nil || generation <= 0 {
		return 0, errTokenConflict
	}
	return generation, nil
}

func nowRFC3339() string { return time.Now().UTC().Format(time.RFC3339Nano) }

func secureBearerEqual(header, expected string) bool {
	if expected == "" {
		return true
	}
	const prefix = "Bearer "
	if !strings.HasPrefix(header, prefix) {
		return false
	}
	return hmac.Equal([]byte(strings.TrimPrefix(header, prefix)), []byte(expected))
}

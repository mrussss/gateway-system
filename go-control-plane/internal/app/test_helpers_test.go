package app

import (
	"io"
	"net/http"
	"net/http/httptest"
)

func newTestRequest(method, target string, body io.Reader) *http.Request {
	request := httptest.NewRequest(method, target, body)
	if body != nil && (method == http.MethodPost || method == http.MethodPut) {
		request.Header.Set("Content-Type", "application/json")
	}
	return request
}

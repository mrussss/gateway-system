package app

import (
	"fmt"
	"os"
	"strings"
)

const (
	appEnvDevelopment           = "development"
	appEnvTest                  = "test"
	appEnvStaging               = "staging"
	appEnvProduction            = "production"
	developmentTokenPepper      = "development-only-token-pepper"
	adminTokenEnvironmentName   = "CONTROL_PLANE_ADMIN_TOKEN"
	gatewayTokenEnvironmentName = "GATEWAY_SHARED_TOKEN"
	tokenPepperEnvironmentName  = "TOKEN_PEPPER"
)

type applicationConfig struct {
	environment  string
	adminToken   string
	gatewayToken string
	tokenPepper  string
}

func loadApplicationConfigFromEnv() (applicationConfig, error) {
	environment := strings.ToLower(strings.TrimSpace(os.Getenv("APP_ENV")))
	if environment == "" {
		environment = appEnvProduction
	}
	switch environment {
	case appEnvDevelopment, appEnvTest, appEnvStaging, appEnvProduction:
	default:
		return applicationConfig{}, fmt.Errorf("APP_ENV must be development, test, staging, or production")
	}

	config := applicationConfig{
		environment:  environment,
		adminToken:   os.Getenv(adminTokenEnvironmentName),
		gatewayToken: os.Getenv(gatewayTokenEnvironmentName),
		tokenPepper:  os.Getenv(tokenPepperEnvironmentName),
	}
	if environment == appEnvDevelopment {
		if config.tokenPepper == "" {
			config.tokenPepper = developmentTokenPepper
		}
		return config, nil
	}

	missing := make([]string, 0, 3)
	if strings.TrimSpace(config.adminToken) == "" {
		missing = append(missing, adminTokenEnvironmentName)
	}
	if strings.TrimSpace(config.gatewayToken) == "" {
		missing = append(missing, gatewayTokenEnvironmentName)
	}
	if strings.TrimSpace(config.tokenPepper) == "" {
		missing = append(missing, tokenPepperEnvironmentName)
	}
	if len(missing) != 0 {
		return applicationConfig{}, fmt.Errorf("%s must be set when APP_ENV=%s",
			strings.Join(missing, ", "), environment)
	}
	return config, nil
}

func developmentApplicationConfigFromEnv() applicationConfig {
	pepper := os.Getenv(tokenPepperEnvironmentName)
	if pepper == "" {
		pepper = developmentTokenPepper
	}
	return applicationConfig{
		environment:  appEnvDevelopment,
		adminToken:   os.Getenv(adminTokenEnvironmentName),
		gatewayToken: os.Getenv(gatewayTokenEnvironmentName),
		tokenPepper:  pepper,
	}
}

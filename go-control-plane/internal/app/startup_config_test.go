package app

import (
	"strings"
	"testing"
)

func setApplicationEnvironment(t *testing.T, environment, adminToken, gatewayToken, pepper string) {
	t.Helper()
	t.Setenv("APP_ENV", environment)
	t.Setenv(adminTokenEnvironmentName, adminToken)
	t.Setenv(gatewayTokenEnvironmentName, gatewayToken)
	t.Setenv(tokenPepperEnvironmentName, pepper)
}

func TestDefaultProductionStartupRequiresEverySecret(t *testing.T) {
	setApplicationEnvironment(t, "", "", "", "")
	t.Setenv("ADMIN_TOKEN", "legacy-token-must-not-be-used")

	_, err := loadApplicationConfigFromEnv()
	if err == nil {
		t.Fatal("production configuration accepted empty secrets")
	}
	for _, name := range []string{
		adminTokenEnvironmentName,
		gatewayTokenEnvironmentName,
		tokenPepperEnvironmentName,
	} {
		if !strings.Contains(err.Error(), name) {
			t.Fatalf("missing secret error does not name %s: %v", name, err)
		}
	}
}

func TestNonDevelopmentModesAcceptExplicitSecrets(t *testing.T) {
	for _, environment := range []string{appEnvTest, appEnvStaging, appEnvProduction} {
		t.Run(environment, func(t *testing.T) {
			setApplicationEnvironment(t, environment, "admin", "gateway", "pepper")
			config, err := loadApplicationConfigFromEnv()
			if err != nil {
				t.Fatal(err)
			}
			if config.environment != environment || config.adminToken != "admin" ||
				config.gatewayToken != "gateway" || config.tokenPepper != "pepper" {
				t.Fatalf("unexpected configuration: %+v", config)
			}
		})
	}
}

func TestDevelopmentMayUseEmptySecrets(t *testing.T) {
	setApplicationEnvironment(t, appEnvDevelopment, "", "", "")
	config, err := loadApplicationConfigFromEnv()
	if err != nil {
		t.Fatal(err)
	}
	if config.tokenPepper != developmentTokenPepper {
		t.Fatalf("unexpected development pepper %q", config.tokenPepper)
	}
}

func TestApplicationEnvironmentIsValidated(t *testing.T) {
	setApplicationEnvironment(t, "unknown", "admin", "gateway", "pepper")
	if _, err := loadApplicationConfigFromEnv(); err == nil {
		t.Fatal("unknown APP_ENV was accepted")
	}
}

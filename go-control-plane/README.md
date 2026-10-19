# Go 控制面

Go 服务提供 Token 生命周期、网关 AUTH 决策、运行配置 CAS 和健康检查。Compose 运行时使用 Redis；MemoryStore 仅用于测试和显式本地开发。

## 本地运行

~~~bash
cd go-control-plane
APP_ENV=development STORE_BACKEND=memory go run ./cmd/control-plane
~~~

非 development 配置需要 CONTROL_PLANE_ADMIN_TOKEN、GATEWAY_SHARED_TOKEN 和 TOKEN_PEPPER。凭据从受保护的本地配置注入，不提交到 Git。

## API 示例

创建 Token；secret 仅在创建时返回一次：

~~~bash
curl -X POST http://localhost:8080/tokens \
  -H "Authorization: Bearer admin-secret" \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001"}'
~~~

读取配置并用返回的 ETag 更新：

~~~bash
curl -H "Authorization: Bearer admin-secret" http://localhost:8080/config
curl -X PUT http://localhost:8080/config \
  -H "Authorization: Bearer admin-secret" -H 'If-Match: "1"' \
  -H "Content-Type: application/json" \
  -d '{"max_payload_size":1048576,"max_connections_per_client":2,"max_requests_per_client_per_second":100,"slow_client_output_limit":8388608,"log_level":"INFO"}'
~~~

接口与字段范围见[API 契约](../docs/api_contract.md)。

## 行为保证

- Redis 只保存 HMAC-SHA256 Token 摘要；列表 API 不返回 secret 或摘要。
- AUTH 失败计数有 TTL，成功后清除；Store 故障以 AUTH_UNAVAILABLE 失败关闭。
- 配置更新通过 Redis Lua CAS；C++ 只应用校验通过的较新版本。
- readiness 反映 Store 可用性；Go 使用 context 与 HTTP shutdown deadline 退出。

运行 go test ./...、go test -race ./...、go vet ./...，或执行根目录 release gate。

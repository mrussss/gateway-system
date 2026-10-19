# 更新记录

当前实现以根目录 README 和 docs 为准。

## v3.0.0-final：冻结最终范围

这是主动收窄长期维护边界，不是意外回退功能。

- 移除 Prometheus 产品接口和 C++ 远程指标上报。
- 移除多网关状态、注册表和在线客户端汇总。
- 移除 Kubernetes、Kind、滚动更新和平台专用 CI。
- 保留 C++ 网络核心、AUTH 隔离、Token/Redis 鉴权、失败 TTL、配置 CAS、测试、Sanitizer 和 Benchmark。
- 旧 v2 实现仍可从 archive/v2-full 分支及 v2-full-archive 标签读取。

## v2.0.1：2026-08-19（历史）

- 未匹配路由和不支持的方法返回统一 JSON 错误、Request ID 和 Allow header。
- Redis、网络和 deadline 故障使用 HTTP 503；内部 Store 错误使用 HTTP 500。
- 历史版本加入中间件拒绝统计和访问日志。

## v2.0.0：2026-08-19（历史范围）

v2 曾包含 Prometheus、Kubernetes 部署、Redis StatefulSet、Kind 验收、多副本、远程指标、滚动更新和更广的 Benchmark。该范围已在 v3 冻结时移除，不代表当前仓库仍提供这些功能。历史代码与测试记录保留在 Git 历史中。

## v1.0.0：2026-07-28（历史基线）

早期 v1 包含 Go HTTP、Token 生命周期、Redis 状态、配置、遥测和 C++ 网关。该标签是历史快照，不表示当前代码仍维持 v1 的完整遥测范围。

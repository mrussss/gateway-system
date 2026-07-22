#pragma once

#include <cstdio>

#if defined(__GNUC__) || defined(__clang__)
#define GATEWAY_PRINTF_FORMAT(string_index, first_argument) \
    __attribute__((format(printf, string_index, first_argument)))
#else
#define GATEWAY_PRINTF_FORMAT(string_index, first_argument)
#endif

void gatewayLog(FILE *stream, const char *level, const char *format, ...)
    GATEWAY_PRINTF_FORMAT(3, 4);
void gatewayDebugLog(const char *format, ...) GATEWAY_PRINTF_FORMAT(1, 2);

#define LOG_DEBUG(...) gatewayDebugLog(__VA_ARGS__)
#define LOG_INFO(...) gatewayLog(stdout, "INFO", __VA_ARGS__)
#define LOG_ERROR(...) gatewayLog(stderr, "ERROR", __VA_ARGS__)

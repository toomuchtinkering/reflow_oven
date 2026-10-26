#pragma once
#include <stdbool.h>
#include <stddef.h>

void ota_server_start(void);
bool ota_server_is_running(void);

/* Constant-time compare; uses mbedtls_ct_memcmp on target, hand-rolled fallback for tests. */
int ota_token_eq(const char *a, const char *b, size_t n);

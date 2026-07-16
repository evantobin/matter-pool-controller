#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ESP-IDF v6 PSA initialization workaround. See the implementation for the
// upstream mbedTLS static-mutex failure it addresses.
void psa_crypto_init_patched(void);

#ifdef __cplusplus
}
#endif

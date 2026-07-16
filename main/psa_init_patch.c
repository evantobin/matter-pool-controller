/*
 * Patch for mbedTLS 4.x PSA crypto init crash in ESP-IDF v6.0.
 *
 * Problem: tf-psa-crypto/platform/threading.c statically initializes
 * PSA mutexes as:
 *   mbedtls_threading_mutex_t = { PTHREAD_MUTEX_INITIALIZER, 1, 1 }
 * But in ESP-IDF's pthread impl, this produces a pthread_mutex_t with
 * an invalid semaphore handle. When psa_crypto_init() → psa_get_initialized()
 * tries to lock the mutex, xQueueSemaphoreTake(NULL) asserts.
 *
 * Fix: Call mbedtls_mutex_init() on each PSA mutex before psa_crypto_init().
 * This replaces the statically-initialized garbage with properly-created
 * FreeRTOS semaphores via pthread_mutex_init() → xSemaphoreCreateMutex().
 */

#include <mbedtls/threading.h>
#include <psa/crypto.h>

/* Externs — these are defined in tf-psa-crypto/platform/threading.c */
extern mbedtls_threading_mutex_t mbedtls_threading_psa_rngdata_mutex;
extern mbedtls_threading_mutex_t mbedtls_threading_psa_globaldata_mutex;
extern mbedtls_threading_mutex_t mbedtls_threading_key_slot_mutex;

void psa_crypto_init_patched(void)
{
    mbedtls_mutex_init(&mbedtls_threading_psa_rngdata_mutex);
    mbedtls_mutex_init(&mbedtls_threading_psa_globaldata_mutex);
    mbedtls_mutex_init(&mbedtls_threading_key_slot_mutex);

    psa_crypto_init();
}

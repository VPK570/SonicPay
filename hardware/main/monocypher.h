/*
    Monocypher version 3.1.3
    https://monocypher.org/
    Dedicated to the public domain under CC0 1.0 Universal / BSD 2-Clause
*/
#ifndef MONOCYPHER_H
#define MONOCYPHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Constant time comparison
int crypto_verify16(const uint8_t a[16], const uint8_t b[16]);
int crypto_verify32(const uint8_t a[32], const uint8_t b[32]);
int crypto_verify64(const uint8_t a[64], const uint8_t b[64]);

// Wipe memory
void crypto_wipe(void *secret, size_t size);

// BLAKE2b / SHA-512 for Ed25519
typedef struct {
    uint64_t hash[8];
    uint64_t input_offset[2];
    uint64_t input_buf[16];
    size_t   input_idx;
    size_t   hash_size;
} crypto_blake2b_ctx;

void crypto_blake2b_general(uint8_t       *hash,      size_t hash_size,
                            const uint8_t *key,       size_t key_size,
                            const uint8_t *message,   size_t message_size);
void crypto_blake2b(uint8_t hash[64], const uint8_t *message, size_t message_size);

// Ed25519 signature & verification
void crypto_ed25519_public_key(uint8_t       public_key[32],
                               const uint8_t secret_key[32]);

void crypto_ed25519_sign(uint8_t       signature[64],
                         const uint8_t secret_key[32],
                         const uint8_t public_key[32],
                         const uint8_t *message, size_t message_size);

int crypto_ed25519_check(const uint8_t signature[64],
                         const uint8_t public_key[32],
                         const uint8_t *message, size_t message_size);

#ifdef __cplusplus
}
#endif

#endif // MONOCYPHER_H

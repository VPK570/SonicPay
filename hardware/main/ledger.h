#ifndef LEDGER_H
#define LEDGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_LEDGER_ENTRIES 500

typedef struct {
    uint16_t nonce;
    uint16_t amount_paise;
    uint32_t timestamp;
} ledger_entry_t;

// Initialize SPIFFS filesystem
bool ledger_init(void);

// Check if nonce was already seen
bool ledger_contains_nonce(uint16_t nonce);

// Add transaction to ledger (returns false if full or I/O error)
bool ledger_add_entry(uint16_t nonce, uint16_t amount_paise);

// Get entry count
size_t ledger_get_count(void);

#endif // LEDGER_H

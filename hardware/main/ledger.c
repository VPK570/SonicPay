#include "ledger.h"
#include <stdio.h>
#include <string.h>
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "LEDGER"
#define LEDGER_PATH "/spiffs/ledger.bin"

static ledger_entry_t entries[MAX_LEDGER_ENTRIES];
static size_t entry_count = 0;

static void load_ledger(void) {
    FILE *f = fopen(LEDGER_PATH, "rb");
    if (!f) {
        ESP_LOGI(TAG, "No existing ledger file found, creating new");
        entry_count = 0;
        return;
    }
    entry_count = fread(entries, sizeof(ledger_entry_t), MAX_LEDGER_ENTRIES, f);
    fclose(f);
    ESP_LOGI(TAG, "Loaded %zu ledger entries from SPIFFS", entry_count);
}

static void save_ledger(void) {
    FILE *f = fopen(LEDGER_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open ledger file for writing");
        return;
    }
    fwrite(entries, sizeof(ledger_entry_t), entry_count, f);
    fclose(f);
}

bool ledger_init(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return false;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS Partition size: total: %d, used: %d", total, used);
    }

    load_ledger();
    return true;
}

bool ledger_contains_nonce(uint16_t nonce) {
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].nonce == nonce) {
            return true;
        }
    }
    return false;
}

bool ledger_add_entry(uint16_t nonce, uint16_t amount_paise) {
    if (ledger_contains_nonce(nonce)) {
        ESP_LOGW(TAG, "Replay attack detected! Nonce %u already in ledger", nonce);
        return false;
    }
    if (entry_count >= MAX_LEDGER_ENTRIES) {
        ESP_LOGW(TAG, "Ledger full (%d entries), shifting oldest entry", MAX_LEDGER_ENTRIES);
        memmove(&entries[0], &entries[1], sizeof(ledger_entry_t) * (MAX_LEDGER_ENTRIES - 1));
        entry_count = MAX_LEDGER_ENTRIES - 1;
    }

    entries[entry_count].nonce = nonce;
    entries[entry_count].amount_paise = amount_paise;
    entries[entry_count].timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
    entry_count++;

    save_ledger();
    ESP_LOGI(TAG, "Recorded transaction #%u into SPIFFS ledger (%zu total)", nonce, entry_count);
    return true;
}

size_t ledger_get_count(void) {
    return entry_count;
}

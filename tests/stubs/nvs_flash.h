#pragma once

#define ESP_OK                      0
#define ESP_ERR_NVS_NO_FREE_PAGES   1
#define ESP_ERR_NVS_NEW_VERSION_FOUND 2

inline int nvs_flash_init() { return ESP_OK; }
inline int nvs_flash_erase() { return ESP_OK; }

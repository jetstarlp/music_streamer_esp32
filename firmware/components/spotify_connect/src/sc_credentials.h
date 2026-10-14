/*
 * Persistence of Spotify's reusable login credentials in NVS.
 *
 * The value is the small JSON document produced by
 * cspot::Context::getCredentialsJson(): {"authData": <base64>, "authType": 1,
 * "username": "..."}. It is a login secret for the user's Spotify account.
 *
 * IMPORTANT: these functions access flash. They must only be called from tasks
 * whose stack is in internal RAM - never from cspot/bell tasks (PSRAM stacks):
 * the flash driver disables the cache, and with it PSRAM, while it works.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @param[out] out_json  malloc'ed, NUL-terminated string; caller frees.
 * @return ESP_OK, ESP_ERR_NVS_NOT_FOUND when nothing is stored, or an NVS error.
 */
esp_err_t sc_credentials_load(char **out_json);

/** Store (overwrite) the credentials. No flash write if the value is unchanged. */
esp_err_t sc_credentials_save(const char *json);

/** Remove stored credentials. ESP_OK also when nothing was stored. */
esp_err_t sc_credentials_erase(void);

#ifdef __cplusplus
}
#endif

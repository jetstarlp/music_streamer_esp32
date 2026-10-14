#include "sc_credentials.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "sc_cred";

#define SC_NVS_NAMESPACE "spotify"
#define SC_NVS_KEY       "credentials"

esp_err_t sc_credentials_load(char **out_json)
{
    if (out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SC_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        /* namespace does not exist yet == nothing stored */
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NVS_NOT_FOUND : err;
    }

    size_t len = 0;
    err = nvs_get_str(nvs, SC_NVS_KEY, NULL, &len);
    if (err == ESP_OK && len > 0) {
        char *buf = malloc(len);
        if (buf == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            err = nvs_get_str(nvs, SC_NVS_KEY, buf, &len);
            if (err == ESP_OK) {
                *out_json = buf;
            } else {
                free(buf);
            }
        }
    }
    nvs_close(nvs);
    return err;
}

esp_err_t sc_credentials_save(const char *json)
{
    if (json == NULL || json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Spotify returns the same blob on every login with stored credentials:
     * avoid wearing the flash with identical writes. */
    char *current = NULL;
    if (sc_credentials_load(&current) == ESP_OK && current != NULL) {
        bool same = strcmp(current, json) == 0;
        free(current);
        if (same) {
            return ESP_OK;
        }
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SC_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(nvs, SC_NVS_KEY, json);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "credentials stored");
    } else {
        ESP_LOGE(TAG, "storing credentials failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t sc_credentials_erase(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SC_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, SC_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

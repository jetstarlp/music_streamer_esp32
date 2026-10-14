/* OTA manifest parsing and the update rule. */
#include "net_pure.h"

#include <string.h>

#include "cJSON.h"

/* Copy at most cap-1 bytes without cutting a UTF-8 sequence in half. */
static void copy_utf8(char *out, size_t cap, const char *in)
{
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) {
            n--;    /* in[n] is a continuation byte: cut before its lead byte */
        }
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

static const char *get_str(const cJSON *root, const char *key, bool required, const char **value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    *value = NULL;
    if (item == NULL) {
        return required ? "required key missing" : NULL;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return "value is not a string";
    }
    *value = item->valuestring;
    return NULL;
}

const char *net_manifest_parse(const char *json, size_t len, const char *manifest_url,
                               bool allow_http, net_manifest_t *out)
{
    if (json == NULL || out == NULL || manifest_url == NULL) {
        return "invalid arguments";
    }
    memset(out, 0, sizeof(*out));
    if (len == 0 || len > NET_MANIFEST_MAX) {
        return "manifest empty or too large";
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return "manifest is not valid JSON";
    }
    const char *err = NULL;
    const char *version = NULL, *url = NULL, *sha = NULL, *min = NULL, *notes = NULL;
    if (!cJSON_IsObject(root)) {
        err = "manifest is not a JSON object";
    }
    if (!err && (err = get_str(root, "version", true, &version)) != NULL) {
        err = "\"version\" missing or not a string";
    }
    if (!err && (err = get_str(root, "url", true, &url)) != NULL) {
        err = "\"url\" missing or not a string";
    }
    if (!err && (err = get_str(root, "sha256", true, &sha)) != NULL) {
        err = "\"sha256\" missing or not a string";
    }
    if (!err && get_str(root, "min_version", false, &min) != NULL) {
        err = "\"min_version\" is not a string";
    }
    if (!err && get_str(root, "notes", false, &notes) != NULL) {
        err = "\"notes\" is not a string";
    }
    if (!err && (strlen(version) >= sizeof(out->version) || !net_semver_valid(version))) {
        err = "\"version\" is not a valid version (expected e.g. 1.2.0)";
    }
    if (!err && min && min[0] && (strlen(min) >= sizeof(out->min_version) || !net_semver_valid(min))) {
        err = "\"min_version\" is not a valid version";
    }
    if (!err && !net_hex_decode(sha, out->sha256, sizeof(out->sha256))) {
        err = "\"sha256\" must be 64 hex digits";
    }
    if (!err && (!net_url_resolve(manifest_url, url, out->url, sizeof(out->url)) ||
                 !net_url_valid(out->url, allow_http, sizeof(out->url)))) {
        err = allow_http ? "\"url\" is not a valid http(s) URL" : "\"url\" is not a valid https URL";
    }
    if (!err) {
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
        if (size != NULL) {
            if (!cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > 16.0 * 1024 * 1024 ||
                size->valuedouble != (double)(uint32_t)size->valuedouble) {
                err = "\"size\" is not a plausible byte count";
            } else {
                out->size = (uint32_t)size->valuedouble;
            }
        }
    }
    if (!err) {
        copy_utf8(out->version, sizeof(out->version), version);
        if (min) {
            copy_utf8(out->min_version, sizeof(out->min_version), min);
        }
        if (notes) {
            copy_utf8(out->notes, sizeof(out->notes), notes);
        }
    }
    cJSON_Delete(root);
    if (err) {
        memset(out, 0, sizeof(*out));
    }
    return err;
}

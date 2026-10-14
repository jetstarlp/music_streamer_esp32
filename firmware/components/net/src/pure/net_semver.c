/* Semantic version comparison (semver 2.0.0 precedence rules). */
#include "net_pure.h"

#include <string.h>

typedef struct {
    uint32_t    core[3];
    const char *pre;       /* first char of the pre-release, NULL if none */
    size_t      pre_len;
} semver_t;

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_ident_char(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

/* Dot-separated, non-empty identifiers of [0-9A-Za-z-], `len` chars. */
static bool valid_idents(const char *s, size_t len)
{
    if (len == 0) {
        return false;
    }
    size_t ident_len = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '.') {
            if (ident_len == 0) {
                return false;
            }
            ident_len = 0;
        } else if (is_ident_char(s[i])) {
            ident_len++;
        } else {
            return false;
        }
    }
    return ident_len > 0;
}

static bool parse(const char *s, semver_t *v)
{
    if (s == NULL) {
        return false;
    }
    memset(v, 0, sizeof(*v));
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    for (int part = 0; part < 3; part++) {
        if (!is_digit(*s)) {
            return false;
        }
        uint64_t n = 0;
        while (is_digit(*s)) {
            n = n * 10 + (uint64_t)(*s - '0');
            if (n > UINT32_MAX) {
                return false;
            }
            s++;
        }
        v->core[part] = (uint32_t)n;
        if (*s != '.') {
            break;            /* "1" or "1.2": missing parts stay 0 */
        }
        if (part == 2) {
            return false;     /* "1.2.3.4" */
        }
        s++;
    }
    if (*s == '-') {
        s++;
        size_t len = strcspn(s, "+");
        if (!valid_idents(s, len)) {
            return false;
        }
        v->pre = s;
        v->pre_len = len;
        s += len;
    }
    if (*s == '+') {
        s++;
        size_t len = strlen(s);
        if (!valid_idents(s, len)) {
            return false;
        }
        s += len;
    }
    return *s == '\0';
}

/* Compare one pre-release identifier each (lengths given, no dots inside). */
static int cmp_ident(const char *a, size_t al, const char *b, size_t bl)
{
    bool a_num = true;
    bool b_num = true;
    for (size_t i = 0; i < al; i++) {
        a_num = a_num && is_digit(a[i]);
    }
    for (size_t i = 0; i < bl; i++) {
        b_num = b_num && is_digit(b[i]);
    }
    if (a_num && b_num) {
        /* Numeric: compare by value; skip leading zeros, then length decides. */
        while (al > 1 && *a == '0') {
            a++;
            al--;
        }
        while (bl > 1 && *b == '0') {
            b++;
            bl--;
        }
        if (al != bl) {
            return al < bl ? -1 : 1;
        }
        int c = memcmp(a, b, al);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (a_num != b_num) {
        return a_num ? -1 : 1;    /* numeric identifiers sort before alphanumeric ones */
    }
    size_t n = al < bl ? al : bl;
    int c = memcmp(a, b, n);
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    return al == bl ? 0 : (al < bl ? -1 : 1);
}

static int cmp_pre(const semver_t *a, const semver_t *b)
{
    if (a->pre == NULL || b->pre == NULL) {
        if (a->pre == NULL && b->pre == NULL) {
            return 0;
        }
        return a->pre == NULL ? 1 : -1;   /* release > pre-release */
    }
    size_t ai = 0;
    size_t bi = 0;
    while (ai < a->pre_len && bi < b->pre_len) {
        size_t ae = ai;
        while (ae < a->pre_len && a->pre[ae] != '.') {
            ae++;
        }
        size_t be = bi;
        while (be < b->pre_len && b->pre[be] != '.') {
            be++;
        }
        int c = cmp_ident(a->pre + ai, ae - ai, b->pre + bi, be - bi);
        if (c != 0) {
            return c;
        }
        ai = ae + 1;
        bi = be + 1;
    }
    /* All shared identifiers equal: the longer list has higher precedence. */
    bool a_more = ai < a->pre_len;
    bool b_more = bi < b->pre_len;
    if (a_more == b_more) {
        return 0;
    }
    return a_more ? 1 : -1;
}

bool net_semver_valid(const char *s)
{
    semver_t v;
    return parse(s, &v);
}

bool net_semver_compare(const char *a, const char *b, int *cmp)
{
    semver_t va;
    semver_t vb;
    if (cmp == NULL || !parse(a, &va) || !parse(b, &vb)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (va.core[i] != vb.core[i]) {
            *cmp = va.core[i] < vb.core[i] ? -1 : 1;
            return true;
        }
    }
    *cmp = cmp_pre(&va, &vb);
    return true;
}

/* The update rule (see net_pure.h). */
net_ota_decision_t net_ota_decide(const char *running, const char *offered, const char *min_version)
{
    if (offered == NULL || !net_semver_valid(offered)) {
        return NET_OTA_DECIDE_INVALID;
    }
    const bool known = running != NULL && net_semver_valid(running);
    int cmp = 0;
    if (!net_semver_compare(offered, known ? running : "0.0.0", &cmp)) {
        return NET_OTA_DECIDE_INVALID;
    }
    if (cmp <= 0) {
        return NET_OTA_DECIDE_UP_TO_DATE;
    }
    if (known && min_version != NULL && min_version[0] != '\0') {
        if (!net_semver_compare(running, min_version, &cmp)) {
            return NET_OTA_DECIDE_INVALID;
        }
        if (cmp < 0) {
            return NET_OTA_DECIDE_TOO_OLD;
        }
    }
    return NET_OTA_DECIDE_INSTALL;
}

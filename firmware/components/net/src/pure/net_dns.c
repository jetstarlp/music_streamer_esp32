/* Minimal DNS responder logic for the captive portal: every name is us. */
#include "net_pure.h"

#include <string.h>

#define DNS_HDR_LEN     12
#define DNS_TYPE_A      1
#define DNS_TYPE_ANY    255
#define DNS_CLASS_IN    1
#define DNS_CLASS_ANY   255

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

size_t net_dns_build_reply(const uint8_t *query, size_t qlen, const uint8_t ipv4[4],
                           uint8_t *out, size_t cap)
{
    if (query == NULL || ipv4 == NULL || out == NULL || qlen < DNS_HDR_LEN) {
        return 0;
    }
    const uint8_t flags_hi = query[2];
    if (flags_hi & 0x80) {
        return 0;                       /* a response, not a query */
    }
    if (((flags_hi >> 3) & 0x0F) != 0) {
        return 0;                       /* not a standard query (opcode) */
    }
    if (rd16(query + 4) != 1) {
        return 0;                       /* exactly one question */
    }

    /* Walk the question name: labels of 1..63 bytes, no compression. */
    size_t pos = DNS_HDR_LEN;
    size_t name_len = 0;
    for (;;) {
        if (pos >= qlen) {
            return 0;
        }
        uint8_t l = query[pos];
        if (l == 0) {
            pos++;
            break;
        }
        if (l & 0xC0) {
            return 0;                   /* compression pointer or reserved bits */
        }
        name_len += (size_t)l + 1;
        if (name_len > 255) {
            return 0;
        }
        pos += (size_t)l + 1;
    }
    if (pos + 4 > qlen) {
        return 0;
    }
    const uint16_t qtype = rd16(query + pos);
    const uint16_t qclass = rd16(query + pos + 2);
    const size_t question_end = pos + 4;
    const bool answer = (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY) &&
                        (qclass == DNS_CLASS_IN || qclass == DNS_CLASS_ANY);

    const size_t reply_len = question_end + (answer ? 16 : 0);
    if (reply_len > cap) {
        return 0;
    }
    memcpy(out, query, question_end);   /* header + question */
    /* QR=1, opcode 0, AA=1, TC=0, RD as asked; RA=1, RCODE=0 */
    out[2] = (uint8_t)(0x80 | 0x04 | (flags_hi & 0x01));
    out[3] = 0x80;
    wr16(out + 4, 1);                   /* QDCOUNT */
    wr16(out + 6, answer ? 1 : 0);      /* ANCOUNT */
    wr16(out + 8, 0);                   /* NSCOUNT */
    wr16(out + 10, 0);                  /* ARCOUNT: EDNS etc. dropped */
    if (answer) {
        uint8_t *a = out + question_end;
        wr16(a, 0xC000 | DNS_HDR_LEN);  /* name: pointer to the question */
        wr16(a + 2, DNS_TYPE_A);
        wr16(a + 4, DNS_CLASS_IN);
        a[6] = 0;
        a[7] = 0;
        wr16(a + 8, NET_DNS_TTL_S);
        wr16(a + 10, 4);
        memcpy(a + 12, ipv4, 4);
    }
    return reply_len;
}

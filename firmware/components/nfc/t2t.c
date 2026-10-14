/*
 * Type 2 tag read/write procedures on top of nfc_tag, see t2t.h.
 */
#include "t2t.h"

#include <string.h>

#define PAGE  NFC_TAG_T2T_PAGE_SIZE
#define FIRST NFC_TAG_T2T_FIRST_DATA_PAGE

static t2t_result_t fail_io(t2t_outcome_t *o, t2t_io_status_t st)
{
    o->res = T2T_ERR_IO;
    o->io = st;
    return o->res;
}

static t2t_result_t fail_tag(t2t_outcome_t *o, nfc_tag_err_t e)
{
    o->res = T2T_ERR_TAG;
    o->tag_err = e;
    return o->res;
}

static t2t_io_status_t rd(const t2t_io_t *io, t2t_outcome_t *o, uint16_t page, uint8_t out[16])
{
    o->reads++;
    return io->read16(io->ctx, page, out);
}

static t2t_io_status_t wr(const t2t_io_t *io, t2t_outcome_t *o, uint16_t page, const uint8_t d[4])
{
    o->writes++;
    return io->write4(io->ctx, page, d);
}

t2t_result_t t2t_read_ref(const t2t_io_t *io, uint8_t *buf, size_t cap, spotify_ref_t *ref,
                          char *link, size_t link_cap, t2t_outcome_t *out)
{
    t2t_outcome_t dummy;
    t2t_outcome_t *o = out ? out : &dummy;
    memset(o, 0, sizeof *o);
    if (link && link_cap) {
        link[0] = '\0';
    }
    if (!io || !io->read16 || !buf || cap < 12 || !ref) {
        return fail_tag(o, NFC_TAG_ERR_ARG);
    }

    uint8_t blk[16];
    t2t_io_status_t st = rd(io, o, 3, blk);   /* CC + data area bytes 0..11 */
    if (st != T2T_IO_OK) {
        return fail_io(o, st);
    }
    size_t data_size = 0;
    nfc_tag_err_t e = nfc_tag_t2t_check_cc(blk, false, &data_size);
    if (e != NFC_TAG_OK) {
        return fail_tag(o, e);
    }
    size_t limit = data_size < cap ? data_size : cap;
    size_t have = limit < 12 ? limit : 12;
    memcpy(buf, blk + 4, have);

    for (;;) {
        size_t need = nfc_tag_ndef_bytes_needed(buf, have);
        if (need <= have || have >= limit) {
            break;
        }
        st = rd(io, o, (uint16_t)(FIRST + have / PAGE), blk);
        if (st != T2T_IO_OK) {
            return fail_io(o, st);
        }
        size_t n = limit - have < 16 ? limit - have : 16;
        memcpy(buf + have, blk, n);
        have += n;
    }
    o->bytes_read = have;

    e = nfc_tag_parse_ndef_ex(buf, have, ref, link, link_cap);
    if (e != NFC_TAG_OK) {
        return fail_tag(o, e);
    }
    o->res = T2T_OK;
    return T2T_OK;
}

bool t2t_ntag_default_cc(uint16_t model, uint8_t cc[4])
{
    uint8_t size;
    switch (model) {
    case 213: size = 0x12; break;   /* 144 bytes */
    case 215: size = 0x3E; break;   /* 496 bytes */
    case 216: size = 0x6D; break;   /* 872 bytes */
    default:  return false;
    }
    cc[0] = 0xE1;   /* NDEF magic number */
    cc[1] = 0x10;   /* mapping version 1.0 */
    cc[2] = size;
    cc[3] = 0x00;   /* read/write access granted */
    return true;
}

/* Page p of the planned image (absolute page number). */
static void plan_page(const nfc_tag_t2t_plan_t *plan, uint16_t p, uint8_t pg[4])
{
    memcpy(pg, plan->img + (size_t)(p - FIRST) * PAGE, PAGE);
}

t2t_result_t t2t_write_ref(const t2t_io_t *io, const spotify_ref_t *ref, t2t_outcome_t *out)
{
    t2t_outcome_t dummy;
    t2t_outcome_t *o = out ? out : &dummy;
    memset(o, 0, sizeof *o);
    if (!io || !io->read16 || !io->write4 || !io->get_version || !spotify_ref_is_valid(ref)) {
        return fail_tag(o, NFC_TAG_ERR_ARG);
    }

    /* 1. Which NTAG? (sizes of the data and configuration areas) */
    uint8_t ver[8];
    t2t_io_status_t st = io->get_version(io->ctx, ver);
    if (st != T2T_IO_OK) {
        return fail_io(o, st);
    }
    nfc_tag_ntag_info_t info;
    nfc_tag_err_t e = nfc_tag_ntag_from_version(ver, &info);
    if (e != NFC_TAG_OK) {
        return fail_tag(o, e);
    }
    o->model = info.model;

    /* 2. Pages 0..3: static lock bytes and CC; configuration pages. */
    uint8_t head[16], cfg[16], blk[16];
    if ((st = rd(io, o, 0, head)) != T2T_IO_OK) {
        return fail_io(o, st);
    }
    if ((e = nfc_tag_t2t_check_static_lock(head + 8)) != NFC_TAG_OK) {
        return fail_tag(o, e);
    }
    if ((st = rd(io, o, info.dyn_lock_page, cfg)) != T2T_IO_OK) {
        return fail_io(o, st);
    }
    uint8_t *cc = head + 12;
    static const uint8_t zero[4] = { 0 };
    if (memcmp(cc, zero, 4) == 0) {
        /* Unformatted: the CC (one-time programmable) gets the NDEF values for
         * this model. Lock bits and a password covering page 3/4 stop us first. */
        if ((e = nfc_tag_ntag_check_config(&info, cfg, FIRST)) != NFC_TAG_OK) {
            return fail_tag(o, e);
        }
        uint8_t new_cc[4];
        t2t_ntag_default_cc(info.model, new_cc);
        if ((st = wr(io, o, 3, new_cc)) != T2T_IO_OK) {
            return fail_io(o, st);
        }
        o->cc_written = true;
        if ((st = rd(io, o, 3, blk)) != T2T_IO_OK) {
            return fail_io(o, st);
        }
        if (memcmp(blk, new_cc, 4) != 0) {
            o->res = T2T_ERR_VERIFY;
            return o->res;
        }
        memcpy(cc, blk, 4);
    }
    size_t data_size = 0;
    if ((e = nfc_tag_t2t_check_cc(cc, true, &data_size)) != NFC_TAG_OK) {
        return fail_tag(o, e);
    }

    /* 3. Plan: keeps control TLVs at the start of the data area. */
    uint8_t cur[NFC_TAG_T2T_PLAN_READ];
    for (size_t off = 0; off < sizeof cur; off += 16) {
        if ((st = rd(io, o, (uint16_t)(FIRST + off / PAGE), cur + off)) != T2T_IO_OK) {
            return fail_io(o, st);
        }
    }
    nfc_tag_t2t_plan_t plan;
    if ((e = nfc_tag_t2t_plan_write(cur, sizeof cur, data_size, ref, &plan)) != NFC_TAG_OK) {
        return fail_tag(o, e);
    }
    if ((e = nfc_tag_ntag_check_config(&info, cfg, plan.last_page)) != NFC_TAG_OK) {
        return fail_tag(o, e);
    }

    /* 4. Write in the planned order. */
    uint8_t pg[4];
    for (uint16_t p = plan.first_page; p <= plan.len_page; p++) {     /* length byte 0 */
        plan_page(&plan, p, pg);
        if (p == plan.len_page) {
            pg[plan.len_offset % PAGE] = 0x00;
        }
        if ((st = wr(io, o, p, pg)) != T2T_IO_OK) {
            return fail_io(o, st);
        }
    }
    for (uint16_t p = (uint16_t)(plan.len_page + 1); p <= plan.last_page; p++) {   /* rest */
        plan_page(&plan, p, pg);
        if ((st = wr(io, o, p, pg)) != T2T_IO_OK) {
            return fail_io(o, st);
        }
    }
    plan_page(&plan, plan.len_page, pg);                                /* real length */
    if ((st = wr(io, o, plan.len_page, pg)) != T2T_IO_OK) {
        return fail_io(o, st);
    }

    /* 5. Read back and compare. */
    for (size_t off = 0; off < plan.end; off += 16) {
        if ((st = rd(io, o, (uint16_t)(FIRST + off / PAGE), blk)) != T2T_IO_OK) {
            return fail_io(o, st);
        }
        size_t n = plan.end - off < 16 ? plan.end - off : 16;
        if (memcmp(blk, plan.img + off, n) != 0) {
            o->res = T2T_ERR_VERIFY;
            return o->res;
        }
    }
    o->res = T2T_OK;
    return T2T_OK;
}

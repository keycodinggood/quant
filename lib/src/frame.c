// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2018, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>

// IWYU pragma: no_include <picotls/../picotls.h>

#include <ev.h>
#include <picotls.h> // IWYU pragma: keep
#include <picotls/openssl.h>
#include <quant/quant.h>
#include <warpcore/warpcore.h>

#include "bitset.h"
#include "conn.h"
#include "diet.h"
#include "frame.h"
#include "marshall.h"
#include "pkt.h"
#include "pn.h"
#include "quic.h"
#include "recovery.h"
#include "stream.h"
#include "tls.h"


#define track_frame(v, ft) bit_set(NUM_FRAM_TYPES, (ft), &meta(v).frames)

#define err_close_return(c, code, ...)                                         \
    do {                                                                       \
        err_close((c), (code), __VA_ARGS__);                                   \
        return UINT16_MAX;                                                     \
    } while (0)


#define dec_chk(type, dst, buf, buf_len, pos, dst_len, ...)                    \
    __extension__({                                                            \
        const uint16_t _i =                                                    \
            dec((dst), (buf), (buf_len), (pos), (dst_len), __VA_ARGS__);       \
        if (unlikely(_i == UINT16_MAX))                                        \
            err_close_return(c, ERR_FRAME_ENC, (type), "dec %s in %s:%u",      \
                             #dst, __FILE__, __LINE__);                        \
        _i;                                                                    \
    })


#define dec_chk_buf(type, dst, buf, buf_len, pos, dst_len)                     \
    __extension__({                                                            \
        const uint16_t _i =                                                    \
            dec_buf((dst), (buf), (buf_len), (pos), (dst_len));                \
        if (unlikely(_i == UINT16_MAX))                                        \
            err_close_return(c, ERR_FRAME_ENC, (type), "dec %s in %s:%u",      \
                             #dst, __FILE__, __LINE__);                        \
        _i;                                                                    \
    })


#ifndef NDEBUG
void log_stream_or_crypto_frame(const bool rtx,
                                const struct w_iov * const v,
                                const bool in,
                                const char * const kind)
{
    const struct q_stream * const s = meta(v).stream;
    const struct q_conn * const c = s->c;
    const uint8_t type = v->buf[meta(v).stream_header_pos];

    if (s->id >= 0)
        warn(INF,
             "%sSTREAM" NRM " 0x%02x=%s%s%s%s%s id=" FMT_SID "/%" PRIu64
             " off=%" PRIu64 "/%" PRIu64 " len=%u coff=%" PRIu64 "/%" PRIu64
             " %s%s%s%s",
             in ? FRAM_IN : FRAM_OUT, type,
             is_set(F_STREAM_FIN, type) ? "FIN" : "",
             is_set(F_STREAM_FIN, type) &&
                     (is_set(F_STREAM_LEN, type) || is_set(F_STREAM_OFF, type))
                 ? "|"
                 : "",
             is_set(F_STREAM_LEN, type) ? "LEN" : "",
             is_set(F_STREAM_LEN, type) && is_set(F_STREAM_OFF, type) ? "|"
                                                                      : "",
             is_set(F_STREAM_OFF, type) ? "OFF" : "", s->id, max_sid(s->id, c),
             meta(v).stream_off, in ? s->in_data_max : s->out_data_max,
             meta(v).stream_data_len, in ? c->in_data : c->out_data,
             in ? c->tp_in.max_data : c->tp_out.max_data,
             rtx ? REV BLD GRN "[RTX]" NRM " " : "", in ? "[" : "", kind,
             in ? "]" : "");
    else
        warn(INF, "%sCRYPTO" NRM " 0x%02x off=%" PRIu64 " len=%u %s%s%s%s",
             in ? FRAM_IN : FRAM_OUT, type, meta(v).stream_off,
             meta(v).stream_data_len, rtx ? REV BLD GRN "[RTX]" NRM " " : "",
             in ? "[" : "", kind, in ? "]" : "");
}
#endif


static void __attribute__((nonnull)) trim_frame(struct pkt_meta * const p)
{
    const uint64_t diff = p->stream->in_data_off - p->stream_off;
    // warn(ERR, "diff=%" PRIu64, diff);
    p->stream_off += diff;
    p->stream_data_start += diff;
    p->stream_data_len -= diff;
}


#define handle_unknown_strm(c, sid, type, ret)                                 \
    do {                                                                       \
        if (diet_find(&(c)->closed_streams, (uint64_t)(sid))) {                \
            warn(NTE,                                                          \
                 "ignoring " #type " frame for closed strm " FMT_SID           \
                 " on %s conn %s",                                             \
                 (sid), conn_type(c), cid2str((c)->scid));                     \
            return (ret);                                                      \
        }                                                                      \
        err_close_return(c, ERR_FRAME_ENC, (type), "unknown strm %" PRId64,    \
                         (sid));                                               \
    } while (0)


static uint16_t __attribute__((nonnull))
dec_stream_or_crypto_frame(struct q_conn * const c,
                           struct w_iov * const v,
                           const uint16_t pos)
{
    meta(v).stream_header_pos = pos;

    // decode the type byte, to check whether this is a stream or crypto frame
    uint8_t t = 0;
    uint16_t i = dec_chk(t, &t, v->buf, v->len, pos, sizeof(t), "0x%02x");

    int64_t sid = 0;
    if (t == FRAM_TYPE_CRPT) {
        const epoch_t e = epoch_for_pkt_type(meta(v).hdr.type);
        sid = crpt_strm_id(e);
        meta(v).stream = c->cstreams[e];
    } else {
        i = dec_chk(t, &sid, v->buf, v->len, i, 0, FMT_SID);
        const int64_t max = max_sid(sid, c);
        if (unlikely(sid > max))
            err_close_return(c, ERR_STREAM_ID, t,
                             "sid %" PRId64 " > max %" PRId64, sid, max);
        meta(v).stream = get_stream(c, sid);
    }

    if (is_set(F_STREAM_OFF, t) || t == FRAM_TYPE_CRPT)
        i = dec_chk(t, &meta(v).stream_off, v->buf, v->len, i, 0, "%" PRIu64);
    else
        meta(v).stream_off = 0;

    uint64_t l = 0;
    if (is_set(F_STREAM_LEN, t) || t == FRAM_TYPE_CRPT) {
        i = dec_chk(t, &l, v->buf, v->len, i, 0, "%" PRIu64);
        if (unlikely(l > (uint64_t)v->len - i))
            err_close_return(c, ERR_FRAME_ENC, t, "illegal strm len");
    } else
        // stream data extends to end of packet
        l = v->len - i;

    meta(v).stream_data_start = i;
    meta(v).stream_data_len = (uint16_t)l;

    // deliver data into stream
    bool is_dup = false;
    const char * kind = BLD RED "???" NRM;

    if (unlikely(meta(v).stream_data_len == 0 && !is_set(F_STREAM_FIN, t))) {
        warn(WRN, "zero-len stream/crypto frame on sid " FMT_SID ", ignoring",
             sid);
        is_dup = true;
        goto done;
    }

    if (unlikely(meta(v).stream == 0)) {
        if (unlikely(diet_find(&c->closed_streams, (uint64_t)sid))) {
            warn(NTE,
                 "ignoring STREAM frame for closed strm " FMT_SID
                 " on %s conn %s",
                 sid, conn_type(c), cid2str(c->scid));
            return meta(v).stream_data_start + meta(v).stream_data_len;
        }

        if (unlikely(is_srv_ini(sid) != c->is_clnt))
            err_close_return(c, ERR_FRAME_ENC, t,
                             "got sid %" PRId64 " but am %s", sid,
                             conn_type(c));

        meta(v).stream = new_stream(c, sid);
    }

    // best case: new in-order data
    if (meta(v).stream->in_data_off >= meta(v).stream_off &&
        meta(v).stream->in_data_off <= meta(v).stream_off +
                                           meta(v).stream_data_len -
                                           (meta(v).stream_data_len ? 1 : 0)) {
        kind = "seq";

        if (unlikely(meta(v).stream->in_data_off > meta(v).stream_off))
            // already-received data at the beginning of the frame, trim
            trim_frame(&meta(v));

        track_bytes_in(meta(v).stream, meta(v).stream_data_len);
        meta(v).stream->in_data_off += meta(v).stream_data_len;
        sq_insert_tail(&meta(v).stream->in, v, next);

        // check if a hole has been filled that lets us dequeue ooo data
        struct pkt_meta * p = splay_min(ooo_by_off, &meta(v).stream->in_ooo);
        while (p) {
            struct pkt_meta * const nxt =
                splay_next(ooo_by_off, &meta(v).stream->in_ooo, p);

            if (unlikely(p->stream_off + p->stream_data_len <
                         meta(v).stream->in_data_off)) {
                // right edge of p < left edge of stream
                warn(WRN, "drop stale frame [%u..%u]", p->stream_off,
                     p->stream_off + p->stream_data_len);
                ensure(splay_remove(ooo_by_off, &meta(v).stream->in_ooo, p),
                       "removed");
                p = nxt;
                continue;
            }

            // right edge of p >= left edge of stream
            if (p->stream_off > meta(v).stream->in_data_off)
                // also left edge of p > left edge of stream: still a gap
                break;

            // left edge of p <= left edge of stream: overlap, trim & enqueue
            if (unlikely(p->stream->in_data_off > p->stream_off))
                trim_frame(p);
            sq_insert_tail(&meta(v).stream->in, w_iov(c->w, pm_idx(p)), next);
            meta(v).stream->in_data_off += p->stream_data_len;
            ensure(splay_remove(ooo_by_off, &meta(v).stream->in_ooo, p),
                   "removed");
            p = nxt;
        }

        // check if we have delivered a FIN, and act on it if we did
        struct w_iov * const last = sq_last(&meta(v).stream->in, w_iov, next);
        if (last) {
            if (unlikely(v != last))
                adj_iov_to_start(last);
            if (is_fin(last)) {
                strm_to_state(meta(v).stream, meta(v).stream->state <= strm_hcrm
                                                  ? strm_hcrm
                                                  : strm_clsd);
                maybe_api_return(q_readall_str, c, meta(v).stream);
                if (meta(v).stream->state == strm_clsd)
                    maybe_api_return(q_close_stream, c, meta(v).stream);

                // ACK the FIN immediately
                struct pn_space * const pn =
                    pn_for_pkt_type(c, meta(v).hdr.type);
                ev_invoke(loop, &pn->ack_alarm, 0);
            }
            if (unlikely(v != last))
                adj_iov_to_data(last);
        }

        if (t != FRAM_TYPE_CRPT) {
            do_stream_fc(meta(v).stream);
            do_conn_fc(c);
            c->have_new_data = true;
            maybe_api_return(q_read, c, 0);
        }
        goto done;
    }

    // data is a complete duplicate
    if (meta(v).stream_off + meta(v).stream_data_len <=
        meta(v).stream->in_data_off) {
        kind = RED "dup" NRM;
        is_dup = true;
        goto done;
    }

    // data is out of order - check if it overlaps with already stored ooo data
    kind = YEL "ooo" NRM;
    struct pkt_meta * p = splay_min(ooo_by_off, &meta(v).stream->in_ooo);
    while (p && p->stream_off + p->stream_data_len - 1 < meta(v).stream_off)
        p = splay_next(ooo_by_off, &meta(v).stream->in_ooo, p);

    // right edge of p >= left edge of v
    if (p &&
        p->stream_off <= meta(v).stream_off + meta(v).stream_data_len - 1) {
        // left edge of p <= right edge of v
        warn(ERR, "[%u..%u] have existing overlapping ooo data [%u..%u]",
             meta(v).stream_off, meta(v).stream_off + meta(v).stream_data_len,
             p->stream_off, p->stream_off + p->stream_data_len - 1);
        is_dup = true;
        goto done;
    }

    // this ooo data doesn't overlap with anything
    track_bytes_in(meta(v).stream, meta(v).stream_data_len);
    ensure(splay_insert(ooo_by_off, &meta(v).stream->in_ooo, &meta(v)) == 0,
           "inserted");

done:
    log_stream_or_crypto_frame(false, v, true, kind);

    if (meta(v).stream && t != FRAM_TYPE_CRPT &&
        meta(v).stream_off + meta(v).stream_data_len >
            meta(v).stream->in_data_max)
        err_close_return(c, ERR_FLOW_CONTROL, 0,
                         "stream %" PRIu64 " off %" PRIu64
                         " > in_data_max %" PRIu64,
                         meta(v).stream->id,
                         meta(v).stream_off + meta(v).stream_data_len - 1,
                         meta(v).stream->in_data_max);

    if (is_dup)
        // this indicates to callers that the w_iov was not placed in a stream
        meta(v).stream = 0;

    return meta(v).stream_data_start + meta(v).stream_data_len;
}


uint64_t shorten_ack_nr(const uint64_t ack, const uint64_t diff)
{
    ensure(diff, "no diff between ACK %" PRIu64 " and diff %" PRIu64, ack,
           diff);

    uint64_t div = (uint64_t)(powl(ceill(log10l(diff)), 10));
    div = MAX(10, div);
    if ((ack - diff) % div + diff >= div)
        div *= 10;
    return ack % div;
}


uint16_t dec_ack_frame(struct q_conn * const c,
                       const struct w_iov * const v,
                       const uint16_t pos)
{
    // we need to decode the type byte, to check for ACK_ECN
    uint8_t t = 0;
    uint16_t i = dec_chk(t, &t, v->buf, v->len, pos, sizeof(t), "0x%02x");

    uint64_t lg_ack = 0;
    // cppcheck-suppress redundantAssignment
    i = dec_chk(t, &lg_ack, v->buf, v->len, i, 0, FMT_PNR_OUT);

    uint64_t ack_delay_raw = 0;
    i = dec_chk(t, &ack_delay_raw, v->buf, v->len, i, 0, "%" PRIu64);

    // TODO: figure out a better way to handle huge ACK delays
    if (unlikely(ack_delay_raw > UINT32_MAX))
        err_close_return(c, ERR_FRAME_ENC, t, "ACK delay raw %" PRIu64,
                         ack_delay_raw);

    // handshake pkts always use the default ACK delay exponent
    const uint8_t ade =
        meta(v).hdr.type == F_LH_INIT && meta(v).hdr.type == F_LH_HSHK
            ? DEF_ACK_DEL_EXP
            : c->tp_in.ack_del_exp;
    const uint64_t ack_delay = ack_delay_raw * (1 << ade);

    struct pn_space * const pn = pn_for_pkt_type(c, meta(v).hdr.type);

    uint64_t num_blocks = 0;
    i = dec_chk(t, &num_blocks, v->buf, v->len, i, 0, "%" PRIu64);

    uint64_t lg_ack_in_block = lg_ack;
    uint64_t sm_new_acked = UINT64_MAX;
    for (uint64_t n = num_blocks + 1; n > 0; n--) {
        uint64_t gap = 0;
        uint64_t ack_block_len = 0;
        i = dec_chk(t, &ack_block_len, v->buf, v->len, i, 0, "%" PRIu64);

        // TODO: figure out a better way to handle huge ACK blocks
        if (unlikely(ack_block_len > UINT16_MAX))
            err_close_return(c, ERR_FRAME_ENC, t, "ACK block len %" PRIu64,
                             ack_block_len);

        if (unlikely(ack_block_len > lg_ack_in_block))
            err_close_return(c, ERR_FRAME_ENC, t, "ACK block len %" PRIu64,
                             " > lg_ack_in_block %" PRIu64, ack_block_len,
                             lg_ack_in_block);

        if (ack_block_len == 0) {
            if (n == num_blocks + 1)
                warn(INF,
                     FRAM_IN "ACK" NRM " lg=" FMT_PNR_OUT " delay=%" PRIu64
                             " (%" PRIu64 " usec) cnt=%" PRIu64
                             " block=%" PRIu64 " [" FMT_PNR_OUT "]",
                     lg_ack, ack_delay_raw, ack_delay, num_blocks,
                     ack_block_len, lg_ack);
            else
                warn(INF,
                     FRAM_IN "ACK" NRM " gap=%" PRIu64 " block=%" PRIu64
                             " [" FMT_PNR_OUT "]",
                     gap, ack_block_len, lg_ack_in_block);
        } else {
            if (n == num_blocks + 1)
                warn(INF,
                     FRAM_IN "ACK" NRM " lg=" FMT_PNR_OUT " delay=%" PRIu64
                             " (%" PRIu64 " usec) cnt=%" PRIu64
                             " block=%" PRIu64 " [" FMT_PNR_OUT ".." FMT_PNR_OUT
                             "]",
                     lg_ack, ack_delay_raw, ack_delay, num_blocks,
                     ack_block_len, lg_ack_in_block - ack_block_len,
                     shorten_ack_nr(lg_ack_in_block, ack_block_len));
            else
                warn(INF,
                     FRAM_IN "ACK" NRM " gap=%" PRIu64 " block=%" PRIu64
                             " [" FMT_PNR_OUT ".." FMT_PNR_OUT "]",
                     gap, ack_block_len, lg_ack_in_block - ack_block_len,
                     shorten_ack_nr(lg_ack_in_block, ack_block_len));
        }

        uint64_t ack = lg_ack_in_block;
        while (ack_block_len >= lg_ack_in_block - ack) {
            struct w_iov * const acked = find_sent_pkt(c, pn, ack);
            if (unlikely(acked == 0)) {
#ifndef FUZZING
                // this is just way too noisy when fuzzing
                if (unlikely(diet_find(&pn->acked, ack) == 0))
                    warn(ERR, "got ACK for pkt " FMT_PNR_OUT " never sent",
                         ack);
#endif
                goto skip;
            }

            if (unlikely(meta(acked).is_acked)) {
                warn(WRN, "repeated ACK for " FMT_PNR_OUT ", ignoring", ack);
                goto skip;
            }

            if (unlikely(ack == lg_ack))
                // call this only for the largest ACK in the frame
                on_ack_received_1(c, pn, acked, ack_delay);

            // this emulates FindSmallestNewlyAcked() from -recovery
            if (sm_new_acked > ack)
                sm_new_acked = meta(acked).hdr.nr;

            on_pkt_acked(c, pn, acked);

        skip:
            if (likely(ack > 0))
                ack--;
            else
                break;
        }

        if (n > 1) {
            i = dec_chk(t, &gap, v->buf, v->len, i, 0, "%" PRIu64);
            if (unlikely(ack <= gap))
                err_close_return(c, ERR_FRAME_ENC, t, "ACK gap %" PRIu64, gap);
            lg_ack_in_block = ack - gap - 1;
        }
    }

    uint64_t ect0_cnt = 0, ect1_cnt = 0, ce_cnt = 0;
    if (t == FRAM_TYPE_ACK_ECN) {
        // decode ECN
        i = dec_chk(t, &ect0_cnt, v->buf, v->len, i, 0, "%" PRIu64);
        i = dec_chk(t, &ect1_cnt, v->buf, v->len, i, 0, "%" PRIu64);
        i = dec_chk(t, &ce_cnt, v->buf, v->len, i, 0, "%" PRIu64);
        warn(INF,
             FRAM_IN "ECN" NRM " ect0=%" PRIu64 " ect1=%" PRIu64 " ce=%" PRIu64,
             ect0_cnt, ect1_cnt, ce_cnt);
        // TODO: add sanity check whether markings make sense
    }

    on_ack_received_2(c, pn, sm_new_acked);
    return i;
}


static uint16_t __attribute__((nonnull))
dec_close_frame(struct q_conn * const c,
                const struct w_iov * const v,
                const uint16_t pos)
{
    // we need to decode the type byte, since this function handles two types
    uint8_t type = 0;
    uint16_t i =
        dec_chk(type, &type, v->buf, v->len, pos, sizeof(type), "0x%02x");

    uint16_t err_code = 0;
    // cppcheck-suppress redundantAssignment
    i = dec_chk(type, &err_code, v->buf, v->len, i, sizeof(err_code), "0x%04x");

    uint64_t frame_type = 0;
    if (type == FRAM_TYPE_CONN_CLSE)
        i = dec_chk(type, &frame_type, v->buf, v->len, i, 0, "0x%" PRIx64);

    uint64_t reas_len = 0;
    i = dec_chk(type, &reas_len, v->buf, v->len, i, 0, "%" PRIu64);
    if (unlikely(i == UINT16_MAX || reas_len + i > v->len))
        err_close_return(c, ERR_FRAME_ENC, type, "illegal reason len %u",
                         reas_len);

    char reas_phr[UINT16_MAX];
    if (reas_len)
        i = dec_chk_buf(type, &reas_phr, v->buf, v->len, i, (uint16_t)reas_len);

    if (type == FRAM_TYPE_CONN_CLSE)
        warn(INF,
             FRAM_IN "CONNECTION_CLOSE" NRM " err=%s0x%04x " NRM
                     "frame=0x%" PRIx64 " rlen=%" PRIu64 " reason=%s%.*s" NRM,
             err_code ? RED : NRM, err_code, frame_type, reas_len,
             err_code ? RED : NRM, reas_len, reas_phr);
    else
        warn(INF,
             FRAM_IN "APPLICATION_CLOSE" NRM " err=%s0x%04x " NRM
                     "rlen=%" PRIu64 " reason=%s%.*s" NRM,
             err_code ? RED : NRM, err_code, reas_len, err_code ? RED : NRM,
             reas_len, reas_phr);

    if (c->state != conn_qlse) {
        if (c->state != conn_drng) {
            conn_to_state(c, conn_drng);
            c->needs_tx = false;
            enter_closing(c);
        } else
            ev_invoke(loop, &c->closing_alarm, 0);
    }
    return i;
}


static uint16_t __attribute__((nonnull))
dec_max_stream_data_frame(struct q_conn * const c,
                          const struct w_iov * const v,
                          const uint16_t pos)
{
    int64_t sid = 0;
    uint16_t i = dec_chk(FRAM_TYPE_MAX_STRM_DATA, &sid, v->buf, v->len, pos + 1,
                         0, FMT_SID);

    uint64_t max = 0;
    // cppcheck-suppress redundantAssignment
    i = dec_chk(FRAM_TYPE_MAX_STRM_DATA, &max, v->buf, v->len, i, 0,
                "%" PRIu64);

    warn(INF, FRAM_IN "MAX_STREAM_DATA" NRM " id=" FMT_SID " max=%" PRIu64, sid,
         max);

    struct q_stream * const s = get_stream(c, sid);
    if (unlikely(s == 0))
        handle_unknown_strm(c, sid, FRAM_TYPE_MAX_STRM_DATA, i);

    if (max > s->out_data_max) {
        s->out_data_max = max;
        s->blocked = false;
        c->needs_tx = true;
    } else
        warn(NTE, "MAX_STREAM_DATA %" PRIu64 " <= current value %" PRIu64, max,
             s->out_data_max);

    return i;
}


static uint16_t __attribute__((nonnull))
dec_max_stream_id_frame(struct q_conn * const c,
                        const struct w_iov * const v,
                        const uint16_t pos)
{
    int64_t max = 0;
    const uint16_t i = dec_chk(FRAM_TYPE_MAX_SID, &max, v->buf, v->len, pos + 1,
                               0, "%" PRIu64);

    if (is_srv_ini(max) == c->is_clnt)
        err_close_return(c, ERR_FRAME_ENC, FRAM_TYPE_MAX_SID,
                         "illegal MAX_STREAM_ID for %s: %u", conn_type(c), max);

    warn(INF, FRAM_IN "MAX_STREAM_ID" NRM " max=" FMT_SID " (%sdir)", max,
         is_uni(max) ? "uni" : "bi");

    int64_t * const max_streams =
        is_uni(max) ? &c->tp_out.max_uni_streams : &c->tp_out.max_bidi_streams;

    if ((max >> 2) + 1 > *max_streams) {
        *max_streams = (max >> 2) + 1;
        if (is_uni(max))
            c->sid_blocked_uni = false;
        else
            c->sid_blocked_bidi = false;
        c->needs_tx = true;
        maybe_api_return(q_rsv_stream, c, 0);

    } else
        warn(NTE, "RX'ed max_%s_streams %" PRIu64 " <= current value %" PRIu64,
             is_uni(max) ? "uni" : "bidi", max, *max_streams);

    return i;
}


static uint16_t __attribute__((nonnull))
dec_max_data_frame(struct q_conn * const c,
                   const struct w_iov * const v,
                   const uint16_t pos)
{
    uint64_t max = 0;
    const uint16_t i = dec_chk(FRAM_TYPE_MAX_DATA, &max, v->buf, v->len,
                               pos + 1, 0, "%" PRIu64);

    warn(INF, FRAM_IN "MAX_DATA" NRM " max=%" PRIu64, max);

    if (max > c->tp_out.max_data) {
        c->tp_out.max_data = max;
        c->blocked = false;
        c->needs_tx = true;
    } else
        warn(NTE, "MAX_DATA %" PRIu64 " <= current value %" PRIu64, max,
             c->tp_out.max_data);

    return i;
}


static uint16_t __attribute__((nonnull))
dec_stream_blocked_frame(struct q_conn * const c,
                         const struct w_iov * const v,
                         const uint16_t pos)
{
    int64_t sid = 0;
    uint16_t i =
        dec_chk(FRAM_TYPE_STRM_BLCK, &sid, v->buf, v->len, pos + 1, 0, FMT_SID);

    uint64_t off = 0;
    // cppcheck-suppress redundantAssignment
    i = dec_chk(FRAM_TYPE_STRM_BLCK, &off, v->buf, v->len, i, 0, "%" PRIu64);

    warn(INF, FRAM_IN "STREAM_BLOCKED" NRM " id=" FMT_SID " off=%" PRIu64, sid,
         off);

    struct q_stream * const s = get_stream(c, sid);
    if (unlikely(s == 0))
        handle_unknown_strm(c, sid, FRAM_TYPE_STRM_BLCK, i);

    do_stream_fc(s);
    return i;
}


static uint16_t __attribute__((nonnull))
dec_blocked_frame(struct q_conn * const c,
                  const struct w_iov * const v,
                  const uint16_t pos)
{
    uint64_t off = 0;
    uint16_t i =
        dec_chk(FRAM_TYPE_BLCK, &off, v->buf, v->len, pos + 1, 0, "%" PRIu64);

    warn(INF, FRAM_IN "BLOCKED" NRM " off=%" PRIu64, off);

    do_conn_fc(c);
    return i;
}


static uint16_t __attribute__((nonnull))
dec_stream_id_blocked_frame(struct q_conn * const c,
                            const struct w_iov * const v,
                            const uint16_t pos)
{
    int64_t sid = 0;
    uint16_t i =
        dec_chk(FRAM_TYPE_SID_BLCK, &sid, v->buf, v->len, pos + 1, 0, FMT_SID);

    warn(INF, FRAM_IN "STREAM_ID_BLOCKED" NRM " sid=" FMT_SID, sid);

    int64_t * const max_streams = is_uni(sid) ? &c->tp_in.new_max_uni_streams
                                              : &c->tp_in.new_max_bidi_streams;

    if ((sid >> 2) + 1 == *max_streams) {
        // let the peer open more streams
        *max_streams += 2;
        if (is_uni(sid))
            c->tx_max_sid_uni = true;
        else
            c->tx_max_sid_bidi = true;
        c->needs_tx = true;
    }

    return i;
}


static uint16_t __attribute__((nonnull))
dec_stop_sending_frame(struct q_conn * const c,
                       const struct w_iov * const v,
                       const uint16_t pos)
{
    int64_t sid = 0;
    uint16_t i =
        dec_chk(FRAM_TYPE_STOP_SEND, &sid, v->buf, v->len, pos + 1, 0, FMT_SID);

    uint16_t err_code = 0;
    // cppcheck-suppress redundantAssignment
    i = dec_chk(FRAM_TYPE_STOP_SEND, &err_code, v->buf, v->len, i,
                sizeof(err_code), "0x%04x");

    warn(INF, FRAM_IN "STOP_SENDING" NRM " id=" FMT_SID " err=%s0x%04x" NRM,
         sid, err_code ? RED : NRM, err_code);

    struct q_stream * const s = get_stream(c, sid);
    if (unlikely(s == 0))
        handle_unknown_strm(c, sid, FRAM_TYPE_STOP_SEND, i);

    return i;
}


static uint16_t __attribute__((nonnull))
dec_path_challenge_frame(struct q_conn * const c,
                         const struct w_iov * const v,
                         const uint16_t pos)
{
    uint16_t i = dec_chk(FRAM_TYPE_PATH_CHLG, &c->path_chlg_in, v->buf, v->len,
                         pos + 1, sizeof(c->path_chlg_in), "0x%" PRIx64);

    warn(INF, FRAM_IN "PATH_CHALLENGE" NRM " data=%" PRIx64, c->path_chlg_in);

    c->path_resp_out = c->path_chlg_in;
    c->needs_tx = c->tx_path_resp = true;

    return i;
}


static uint16_t __attribute__((nonnull))
dec_path_response_frame(struct q_conn * const c,
                        const struct w_iov * const v,
                        const uint16_t pos)
{
    uint16_t i = dec_chk(FRAM_TYPE_PATH_RESP, &c->path_resp_in, v->buf, v->len,
                         pos + 1, sizeof(c->path_resp_in), "0x%" PRIx64);

    warn(INF, FRAM_IN "PATH_RESPONSE" NRM " data=%" PRIx64, c->path_resp_in);

    if (c->path_resp_in == c->path_chlg_out)
        c->tx_path_chlg = false;

    return i;
}


static uint16_t __attribute__((nonnull))
dec_new_cid_frame(struct q_conn * const c,
                  const struct w_iov * const v,
                  const uint16_t pos)
{
    struct cid dcid;
    uint16_t i = dec_chk(FRAM_TYPE_NEW_CID, &dcid.len, v->buf, v->len, pos + 1,
                         sizeof(dcid.len), "%u");

    if (unlikely(dcid.len < 4 || dcid.len > MAX_CID_LEN))
        err_close_return(c, ERR_FRAME_ENC, FRAM_TYPE_NEW_CID,
                         "illegal cid len %u", dcid.len);

    i = dec_chk(FRAM_TYPE_NEW_CID, &dcid.seq, v->buf, v->len, i, 0, "%" PRIu64);
    i = dec_chk_buf(FRAM_TYPE_NEW_CID, dcid.id, v->buf, v->len, i, dcid.len);
    i = dec_chk_buf(FRAM_TYPE_NEW_CID, dcid.srt, v->buf, v->len, i,
                    sizeof(dcid.srt));

    bool dup = false;
    if (dcid.seq > c->max_cid_seq_in) {
        add_dcid(c, &dcid);
        c->max_cid_seq_in = dcid.seq;
    } else
        dup = true;

    warn(INF,
         FRAM_IN "NEW_CONNECTION_ID" NRM " seq=%" PRIu64
                 " len=%u dcid=%s tok=%s%s",
         dcid.seq, dcid.len, cid2str(&dcid),
         hex2str(dcid.srt, sizeof(dcid.srt)),
         dup ? " [" RED "dup" NRM "]" : "");

    return i;
}


static uint16_t __attribute__((nonnull))
dec_rst_stream_frame(struct q_conn * const c,
                     const struct w_iov * const v,
                     const uint16_t pos)
{
    int64_t sid = 0;
    uint16_t i =
        dec_chk(FRAM_TYPE_RST_STRM, &sid, v->buf, v->len, pos + 1, 0, FMT_SID);

    uint16_t err = 0;
    // cppcheck-suppress redundantAssignment
    i = dec_chk(FRAM_TYPE_RST_STRM, &err, v->buf, v->len, i, sizeof(err),
                "0x%04x");

    uint64_t off = 0;
    i = dec_chk(FRAM_TYPE_RST_STRM, &off, v->buf, v->len, i, 0, "%" PRIu64);

    warn(INF,
         FRAM_IN "RST_STREAM" NRM " sid=" FMT_SID " err=%s0x%04x" NRM
                 " off=%" PRIu64,
         sid, err ? RED : NRM, err, off);

    struct q_stream * const s = get_stream(c, sid);
    if (unlikely(s == 0))
        handle_unknown_strm(c, sid, FRAM_TYPE_RST_STRM, i);

    strm_to_state(s, strm_clsd);

    return i;
}


static uint16_t __attribute__((nonnull))
dec_retire_cid_frame(struct q_conn * const c,
                     const struct w_iov * const v,
                     const uint16_t pos)
{
    struct cid which;
    uint16_t i = dec_chk(FRAM_TYPE_RTIR_CID, &which.seq, v->buf, v->len,
                         pos + 1, 0, "%" PRIu64);

    warn(INF, FRAM_IN "RETIRE_CONNECTION_ID" NRM " seq=%" PRIu64, which.seq);

    struct cid * const scid = splay_find(cids_by_seq, &c->scids_by_seq, &which);
    if (unlikely(scid == 0))
        err_close_return(c, ERR_FRAME_ENC, FRAM_TYPE_RTIR_CID,
                         "no cid seq %" PRIu64, which.seq);
    else if (c->scid->seq == scid->seq) {
        struct cid * const next_scid =
            splay_next(cids_by_seq, &c->scids_by_seq, scid);
        if (unlikely(next_scid == 0))
            err_close_return(c, ERR_FRAME_ENC, FRAM_TYPE_RTIR_CID,
                             "no next scid");
        c->scid = next_scid;
        warn(ERR, "next %s", cid2str(c->scid));
    }

    free_scid(c, scid);

    // rx of RETIRE_CONNECTION_ID means we should send more
    c->tx_ncid = true;

    return i;
}


static uint16_t __attribute__((nonnull))
dec_new_token_frame(struct q_conn * const c,
                    const struct w_iov * const v,
                    const uint16_t pos)
{
    uint64_t tok_len = 0;
    uint16_t i = dec_chk(FRAM_TYPE_NEW_TOKN, &tok_len, v->buf, v->len, pos + 1,
                         0, "%" PRIu64);

    if (unlikely(tok_len > (uint64_t)(v->len - i)))
        err_close_return(c, ERR_FRAME_ENC, FRAM_TYPE_NEW_TOKN,
                         "illegal tok len");

    // TODO: actually do something with the token
    uint8_t tok[MAX_TOK_LEN];
    if (unlikely(tok_len > sizeof(tok)))
        err_close_return(c, ERR_FRAME_ENC, FRAM_TYPE_NEW_TOKN,
                         "max tok_len is %u, got %u", sizeof(tok), tok_len);
    i = dec_chk_buf(FRAM_TYPE_NEW_TOKN, tok, v->buf, v->len, i,
                    (uint16_t)tok_len);

    warn(INF, FRAM_IN "NEW_TOKEN" NRM " len=%" PRIu64 " tok=%s", tok_len,
         hex2str(tok, tok_len));

    // TODO: actually do something with this

    return i;
}


uint16_t dec_frames(struct q_conn * const c, struct w_iov ** vv)
{
    struct w_iov * v = *vv;

    uint16_t i = meta(v).hdr.hdr_len;
    uint16_t pad_start = 0;

#if !defined(NDEBUG) && !defined(FUZZING) &&                                   \
    !defined(NO_FUZZER_CORPUS_COLLECTION)
    // when called from the fuzzer, v->ip is zero
    if (v->ip)
        write_to_corpus(corpus_frm_dir, &v->buf[i], v->len - i);
#endif

    while (likely(i < v->len)) {
        uint8_t type = 0;
        dec_chk(type, &type, v->buf, v->len, i, sizeof(type), "0x%02x");

        if (pad_start && (type != FRAM_TYPE_PAD || i == v->len - 1)) {
            warn(INF, FRAM_IN "PADDING" NRM " len=%u", i - pad_start);
            pad_start = 0;
            track_frame(v, type);

        } else if (type == FRAM_TYPE_CRPT ||
                   (type >= FRAM_TYPE_STRM && type <= FRAM_TYPE_STRM_MAX)) {

            if (unlikely((has_frame(v, FRAM_TYPE_CRPT) ||
                          has_frame(v, FRAM_TYPE_STRM))) &&
                meta(v).stream) {
                // already had at least one stream or crypto frame in this
                // packet with non-duplicate data, so generate (another) copy
                warn(DBG, "addtl stream or crypto frame at pos %u, copy", i);
                struct w_iov * const vdup = w_iov_dup(v);
                pm_cpy(&meta(vdup), &meta(v), false);
                // adjust w_iov start and len to stream frame data
                v->buf = &v->buf[meta(v).stream_data_start];
                v->len = meta(v).stream_data_len;
                // continue parsing in the copied w_iov
                v = *vv = vdup;
            }

            // this is the first stream frame in this packet
            i = dec_stream_or_crypto_frame(c, v, i);
            // stream frames have multiple types, so don't enc "type"
            track_frame(v, type == FRAM_TYPE_CRPT ? FRAM_TYPE_CRPT
                                                  : FRAM_TYPE_STRM);

        } else {
            switch (type) {
            case FRAM_TYPE_ACK_ECN:
                type = FRAM_TYPE_ACK; // only enc FRAM_TYPE_ACK in bitstr_t
                // fallthrough
            case FRAM_TYPE_ACK:
                i = dec_ack_frame(c, v, i);
                break;

            case FRAM_TYPE_PAD:
                pad_start = pad_start ? pad_start : i;
                i++;
                break;

            case FRAM_TYPE_RST_STRM:
                i = dec_rst_stream_frame(c, v, i);
                break;

            case FRAM_TYPE_CONN_CLSE:
            case FRAM_TYPE_APPL_CLSE:
                i = dec_close_frame(c, v, i);
                break;

            case FRAM_TYPE_PING:
                warn(INF, FRAM_IN "PING" NRM);
                // PING frames need to be ACK'ed
                c->needs_tx = true;
                i++;
                break;

            case FRAM_TYPE_MAX_STRM_DATA:
                i = dec_max_stream_data_frame(c, v, i);
                break;

            case FRAM_TYPE_MAX_SID:
                i = dec_max_stream_id_frame(c, v, i);
                break;

            case FRAM_TYPE_MAX_DATA:
                i = dec_max_data_frame(c, v, i);
                break;

            case FRAM_TYPE_STRM_BLCK:
                i = dec_stream_blocked_frame(c, v, i);
                break;

            case FRAM_TYPE_BLCK:
                i = dec_blocked_frame(c, v, i);
                break;

            case FRAM_TYPE_SID_BLCK:
                i = dec_stream_id_blocked_frame(c, v, i);
                break;

            case FRAM_TYPE_STOP_SEND:
                i = dec_stop_sending_frame(c, v, i);
                break;

            case FRAM_TYPE_PATH_CHLG:
                i = dec_path_challenge_frame(c, v, i);
                break;

            case FRAM_TYPE_PATH_RESP:
                i = dec_path_response_frame(c, v, i);
                break;

            case FRAM_TYPE_NEW_CID:
                i = dec_new_cid_frame(c, v, i);
                break;

            case FRAM_TYPE_NEW_TOKN:
                i = dec_new_token_frame(c, v, i);
                break;

            case FRAM_TYPE_RTIR_CID:
                i = dec_retire_cid_frame(c, v, i);
                break;

            default:
                err_close_return(c, ERR_FRAME_ENC, type,
                                 "unknown frame type 0x%02x at pos %u", type,
                                 i);
            }

            if (likely(i < UINT16_MAX))
                // record this frame type in the meta data
                track_frame(v, type);
        }

        if (unlikely(i == UINT16_MAX))
            // there was an error parsing a frame
            return UINT16_MAX;
    }

    if (meta(v).stream_data_start) {
        // adjust w_iov start and len to stream frame data
        v->buf = &v->buf[meta(v).stream_data_start];
        v->len = meta(v).stream_data_len;
    }

    // track outstanding frame types in the pn space
    struct pn_space * const pn = pn_for_pkt_type(c, meta(v).hdr.type);
    bit_or(NUM_FRAM_TYPES, &pn->rx_frames, &meta(v).frames);

    return i;
}


uint16_t max_frame_len(const uint8_t type)
{
    // return max len needed to encode the given frame type
    uint16_t len = sizeof(uint8_t); // type

    switch (type) {
    case FRAM_TYPE_PAD:
    case FRAM_TYPE_PING:
        break;

    case FRAM_TYPE_RST_STRM:
        len += sizeof(uint64_t) + sizeof(uint16_t) + sizeof(uint64_t);
        break;

        // these two are never combined with stream frames, so no need to check
        // case FRAM_TYPE_CONN_CLSE:
        // case FRAM_TYPE_APPL_CLSE:

    case FRAM_TYPE_MAX_SID:
    case FRAM_TYPE_MAX_DATA:
    case FRAM_TYPE_BLCK:
    case FRAM_TYPE_SID_BLCK:
    case FRAM_TYPE_RTIR_CID:
    case FRAM_TYPE_PATH_CHLG:
    case FRAM_TYPE_PATH_RESP:
        len += sizeof(uint64_t);
        break;

    case FRAM_TYPE_MAX_STRM_DATA:
    case FRAM_TYPE_STRM_BLCK:
        len += sizeof(uint64_t) + sizeof(uint64_t);
        break;

    case FRAM_TYPE_NEW_CID:
        len += sizeof(uint64_t) + MAX_CID_LEN + SRT_LEN;
        break;

    case FRAM_TYPE_STOP_SEND:
        len += sizeof(uint64_t) + sizeof(uint16_t);
        break;

        // these two don't need to be length-checked
        // case FRAM_TYPE_STRM:
        // case FRAM_TYPE_CRPT:

    case FRAM_TYPE_NEW_TOKN:
        // only true on TX; update when make_rtry_tok() changes
        len += sizeof(uint64_t) + PTLS_MAX_DIGEST_SIZE + MAX_CID_LEN;
        break;

        // these are always first, so assume there is enough space
        // case FRAM_TYPE_ACK_ECN:
        // case FRAM_TYPE_ACK:

    default:
        die("unhandled frame type 0x%02x", type);
    }

    return len;
}


uint16_t enc_padding_frame(struct w_iov * const v,
                           const uint16_t pos,
                           const uint16_t len)
{
    if (unlikely(len == 0))
        return pos;
    warn(INF, FRAM_OUT "PADDING" NRM " len=%u", len);
    memset(&v->buf[pos], FRAM_TYPE_PAD, len);
    track_frame(v, FRAM_TYPE_PAD);
    return pos + len;
}


uint16_t enc_ack_frame(struct q_conn * const c,
                       struct pn_space * const pn,
                       struct w_iov * const v,
                       const uint16_t pos)
{
    const bool enc_ecn = pn->ect0_cnt || pn->ect1_cnt || pn->ce_cnt;
    const uint8_t type = enc_ecn ? FRAM_TYPE_ACK_ECN : FRAM_TYPE_ACK;
    track_frame(v, FRAM_TYPE_ACK);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    struct ival * b = diet_max_ival(&pn->recv);
    ensure(b, "nothing to ACK");
    meta(v).lg_acked = b->hi;
    i = enc(v->buf, v->len, i, &meta(v).lg_acked, 0, 0, FMT_PNR_IN);

    // handshake pkts always use the default ACK delay exponent
    const uint8_t ade =
        meta(v).hdr.type <= F_LH_INIT && meta(v).hdr.type >= F_LH_HSHK
            ? DEF_ACK_DEL_EXP
            : c->tp_out.ack_del_exp;
    const uint64_t ack_delay =
        (uint64_t)((ev_now(loop) - diet_timestamp(b)) * 1000000) / (1 << ade);
    i = enc(v->buf, v->len, i, &ack_delay, 0, 0, "%" PRIu64);

    meta(v).ack_block_cnt = diet_cnt(&pn->recv) - 1;
    meta(v).ack_block_pos = i =
        enc(v->buf, v->len, i, &meta(v).ack_block_cnt, 0, 0, "%" PRIu64);

    uint64_t prev_lo = 0;
    splay_foreach_rev (b, diet, &pn->recv) {
        uint64_t gap = 0;
        if (prev_lo) {
            gap = prev_lo - b->hi - 2;
            i = enc(v->buf, v->len, i, &gap, 0, 0, "%" PRIu64);
        }
        const uint64_t ack_block = b->hi - b->lo;

        if (ack_block) {
            if (prev_lo)
                warn(INF,
                     FRAM_OUT "ACK" NRM " gap=%" PRIu64 " block=%" PRIu64
                              " [" FMT_PNR_IN ".." FMT_PNR_IN "]",
                     gap, ack_block, b->lo, shorten_ack_nr(b->hi, ack_block));
            else
                warn(INF,
                     FRAM_OUT "ACK" NRM " lg=" FMT_PNR_IN " delay=%" PRIu64
                              " (%" PRIu64 " usec) cnt=%" PRIu64
                              " block=%" PRIu64 " [" FMT_PNR_IN ".." FMT_PNR_IN
                              "]",
                     meta(v).lg_acked, ack_delay, ack_delay * (1 << ade),
                     meta(v).ack_block_cnt, ack_block, b->lo,
                     shorten_ack_nr(b->hi, ack_block));

        } else {
            if (prev_lo)
                warn(INF,
                     FRAM_OUT "ACK" NRM " gap=%" PRIu64 " block=%" PRIu64
                              " [" FMT_PNR_IN "]",
                     gap, ack_block, b->hi);
            else
                warn(INF,
                     FRAM_OUT "ACK" NRM " lg=" FMT_PNR_IN " delay=%" PRIu64
                              " (%" PRIu64 " usec) cnt=%" PRIu64
                              " block=%" PRIu64 " [" FMT_PNR_IN "]",
                     meta(v).lg_acked, ack_delay, ack_delay * (1 << ade),
                     meta(v).ack_block_cnt, ack_block, meta(v).lg_acked);
        }
        i = enc(v->buf, v->len, i, &ack_block, 0, 0, "%" PRIu64);
        prev_lo = b->lo;
    }

    if (enc_ecn) {
        // encode ECN
        i = enc(v->buf, v->len, i, &pn->ect0_cnt, 0, 0, "%" PRIu64);
        i = enc(v->buf, v->len, i, &pn->ect1_cnt, 0, 0, "%" PRIu64);
        i = enc(v->buf, v->len, i, &pn->ce_cnt, 0, 0, "%" PRIu64);
        warn(INF,
             FRAM_OUT "ECN" NRM " ect0=%" PRIu64 " ect1=%" PRIu64
                      " ce=%" PRIu64,
             pn->ect0_cnt, pn->ect1_cnt, pn->ce_cnt);
    }

    // warn(DBG, "ACK encoded, stopping epoch %u ACK timer",
    //      epoch_for_pkt_type(meta(v).hdr.type));
    ev_timer_stop(loop, &pn->ack_alarm);
    bit_zero(NUM_FRAM_TYPES, &pn->rx_frames);

    return i;
}


uint16_t enc_stream_or_crypto_frame(struct q_stream * const s,
                                    struct w_iov * const v,
                                    const uint16_t pos,
                                    const bool enc_strm)
{
    const uint64_t dlen = v->len - meta(v).stream_data_start;
    uint8_t type;

    if (likely(enc_strm)) {
        ensure(!is_set(F_LONG_HDR, meta(v).hdr.flags) ||
                   meta(v).hdr.type == F_LH_0RTT,
               "sid %" PRId64 " in 0x%02x-type pkt", s->id, meta(v).hdr.type);

        ensure(dlen || s->state > strm_open,
               "no stream data or need to send FIN");

        type = FRAM_TYPE_STRM | (dlen ? F_STREAM_LEN : 0) |
               (s->out_data ? F_STREAM_OFF : 0);

        // if stream is closed locally and this is last packet, include FIN
        if (unlikely((s->state == strm_hclo || s->state == strm_clsd) &&
                     v == sq_last(&s->out, w_iov, next)))
            type |= F_STREAM_FIN;
    } else
        type = FRAM_TYPE_CRPT;

    track_frame(v, type == FRAM_TYPE_CRPT ? FRAM_TYPE_CRPT : FRAM_TYPE_STRM);

    // now that we know how long the stream frame header is, encode it
    uint16_t i = meta(v).stream_header_pos =
        meta(v).stream_data_start - 1 -
        (likely(enc_strm) ? varint_size_needed((uint64_t)s->id) : 0) -
        (dlen || unlikely(!enc_strm) ? varint_size_needed(dlen) : 0) -
        (s->out_data || unlikely(!enc_strm) ? varint_size_needed(s->out_data)
                                            : 0);
    ensure(i > pos, "not enough space for stream header (%u > %u)", i, pos);
    i = enc(v->buf, v->len, i, &type, sizeof(type), 0, "0x%02x");
    if (likely(enc_strm))
        i = enc(v->buf, v->len, i, &s->id, 0, 0, FMT_SID);
    if (s->out_data || unlikely(!enc_strm))
        i = enc(v->buf, v->len, i, &s->out_data, 0, 0, "%" PRIu64);
    if (dlen || unlikely(!enc_strm))
        enc(v->buf, v->len, i, &dlen, 0, 0, "%u");

    meta(v).stream = s; // remember stream this buf belongs to
    meta(v).stream_data_len = (uint16_t)dlen;
    meta(v).stream_off = s->out_data;

    log_stream_or_crypto_frame(false, v, false, "");
    track_bytes_out(s, dlen);
    ensure(!enc_strm || s->out_data < s->out_data_max, "exceeded fc window");

    return v->len;
}


uint16_t enc_close_frame(const struct q_conn * const c,
                         struct w_iov * const v,
                         const uint16_t pos)
{
    const uint8_t type =
        c->err_code == 0 ? FRAM_TYPE_APPL_CLSE : FRAM_TYPE_CONN_CLSE;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    i = enc(v->buf, v->len, i, &c->err_code, sizeof(c->err_code), 0, "0x%04x");
    if (type == FRAM_TYPE_CONN_CLSE)
        i = enc(v->buf, v->len, i, &c->err_frm, sizeof(c->err_frm), 0,
                "0x%02x");

    const uint64_t rlen = c->err_reason_len;
    i = enc(v->buf, v->len, i, &rlen, 0, 0, "%" PRIu64);
    if (rlen)
        i = enc_buf(v->buf, v->len, i, c->err_reason, (uint16_t)rlen);

    if (type == FRAM_TYPE_CONN_CLSE)
        warn(INF,
             FRAM_OUT "CONNECTION_CLOSE" NRM " err=%s0x%04x" NRM
                      " frame=0x%02x rlen=%" PRIu64 " reason=%s%.*s" NRM,
             c->err_code ? RED : NRM, c->err_code, c->err_frm, rlen,
             c->err_code ? RED : NRM, rlen, c->err_reason);
    else
        warn(INF,
             FRAM_OUT "APPLICATION_CLOSE" NRM " err=%s0x%04x" NRM
                      " rlen=%" PRIu64 " reason=%s%.*s" NRM,
             c->err_code ? RED : NRM, c->err_code, rlen,
             c->err_code ? RED : NRM, rlen, c->err_reason);

    return i;
}


uint16_t enc_max_stream_data_frame(struct q_stream * const s,
                                   struct w_iov * const v,
                                   const uint16_t pos)
{
    const uint8_t type = FRAM_TYPE_MAX_STRM_DATA;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    i = enc(v->buf, v->len, i, &s->id, 0, 0, FMT_SID);
    meta(v).max_stream_data_sid = s->id;

    i = enc(v->buf, v->len, i, &s->new_in_data_max, 0, 0, "%" PRIu64);
    meta(v).max_stream_data = s->new_in_data_max;

    warn(INF, FRAM_OUT "MAX_STREAM_DATA" NRM " id=" FMT_SID " max=%" PRIu64,
         s->id, s->new_in_data_max);

    // update the stream
    s->in_data_max = s->new_in_data_max;

    return i;
}


uint16_t enc_max_data_frame(struct q_conn * const c,
                            struct w_iov * const v,
                            const uint16_t pos)
{
    const uint8_t type = FRAM_TYPE_MAX_DATA;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    i = enc(v->buf, v->len, i, &c->tp_in.new_max_data, 0, 0, "%" PRIu64);
    meta(v).max_data = c->tp_in.new_max_data;

    warn(INF, FRAM_OUT "MAX_DATA" NRM " max=%" PRIu64, c->tp_in.new_max_data);

    // update connection
    c->tp_in.max_data = c->tp_in.new_max_data;

    return i;
}


uint16_t enc_max_stream_id_frame(struct q_conn * const c,
                                 struct w_iov * const v,
                                 const uint16_t pos,
                                 const bool bidi)
{
    const uint8_t type = FRAM_TYPE_MAX_SID;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    const int64_t max = (((bidi ? c->tp_in.new_max_bidi_streams
                                : c->tp_in.new_max_uni_streams) -
                          1)
                         << 2) |
                        (bidi ? 0 : STRM_FL_UNI) |
                        (c->is_clnt ? STRM_FL_SRV : 0);
    i = enc(v->buf, v->len, i, &max, 0, 0, "%" PRId64);

    warn(INF, FRAM_OUT "MAX_STREAM_ID" NRM " max=" FMT_SID, max);

    if (bidi) {
        meta(v).max_bidi_streams = c->tp_in.max_bidi_streams =
            c->tp_in.new_max_bidi_streams;
        c->tx_max_sid_bidi = false;
    } else {
        meta(v).max_uni_streams = c->tp_in.max_uni_streams =
            c->tp_in.new_max_uni_streams;
        c->tx_max_sid_uni = false;
    }

    return i;
}


uint16_t enc_stream_blocked_frame(struct q_stream * const s,
                                  const struct w_iov * const v,
                                  const uint16_t pos)
{
    const uint8_t type = FRAM_TYPE_STRM_BLCK;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    i = enc(v->buf, v->len, i, &s->id, 0, 0, FMT_SID);
    i = enc(v->buf, v->len, i, &s->out_data, 0, 0, "%" PRIu64);

    warn(INF, FRAM_OUT "STREAM_BLOCKED" NRM " id=" FMT_SID " off=%" PRIu64,
         s->id, s->out_data);

    return i;
}


uint16_t enc_blocked_frame(struct q_conn * const c,
                           const struct w_iov * const v,
                           const uint16_t pos)
{
    const uint8_t type = FRAM_TYPE_BLCK;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    const uint64_t off = c->tp_out.max_data + meta(v).stream_data_len;
    i = enc(v->buf, v->len, i, &off, 0, 0, "%" PRIu64);

    warn(INF, FRAM_OUT "BLOCKED" NRM " off=%" PRIu64, off);

    return i;
}


uint16_t enc_stream_id_blocked_frame(struct q_conn * const c,
                                     const struct w_iov * const v,
                                     const uint16_t pos,
                                     const bool bidi)
{
    const uint8_t type = FRAM_TYPE_SID_BLCK;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    const int64_t ms = (bidi ? c->next_sid_bidi : c->next_sid_uni) - 4;
    i = enc(v->buf, v->len, i, &ms, 0, 0, "%" PRId64);

    warn(INF, FRAM_OUT "STREAM_ID_BLOCKED" NRM " sid=" FMT_SID, ms);

    return i;
}


uint16_t enc_path_response_frame(struct q_conn * const c,
                                 const struct w_iov * const v,
                                 const uint16_t pos)
{
    const uint8_t type = FRAM_TYPE_PATH_RESP;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    i = enc(v->buf, v->len, i, &c->path_resp_out, sizeof(c->path_resp_out), 0,
            "0x%" PRIx64);

    warn(INF, FRAM_OUT "PATH_RESPONSE" NRM " data=%" PRIx64, c->path_resp_out);

    return i;
}


uint16_t enc_path_challenge_frame(struct q_conn * const c,
                                  const struct w_iov * const v,
                                  const uint16_t pos)
{
    const uint8_t type = FRAM_TYPE_PATH_CHLG;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    i = enc(v->buf, v->len, i, &c->path_chlg_out, sizeof(c->path_chlg_out), 0,
            "0x%" PRIx64);

    warn(INF, FRAM_OUT "PATH_CHALLENGE" NRM " data=%" PRIx64, c->path_chlg_out);

    return i;
}


uint16_t enc_new_cid_frame(struct q_conn * const c,
                           const struct w_iov * const v,
                           const uint16_t pos)
{
    const uint8_t type = FRAM_TYPE_NEW_CID;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    struct cid ncid = {.seq = ++c->max_cid_seq_out,
                       .len = c->is_clnt ? CLNT_SCID_LEN : SERV_SCID_LEN};
    ptls_openssl_random_bytes(ncid.id, sizeof(ncid.id) + sizeof(ncid.srt));
    add_scid(c, &ncid);

    i = enc(v->buf, v->len, i, &ncid.len, sizeof(ncid.len), 0, "%u");
    i = enc(v->buf, v->len, i, &ncid.seq, 0, 0, "%" PRIu64);
    i = enc_buf(v->buf, v->len, i, ncid.id, ncid.len);
    i = enc_buf(v->buf, v->len, i, &ncid.srt, sizeof(ncid.srt));

    warn(INF,
         FRAM_OUT "NEW_CONNECTION_ID" NRM " seq=%" PRIx64
                  " len=%u cid=%s tok=%s",
         ncid.seq, ncid.len, cid2str(&ncid),
         hex2str(ncid.srt, sizeof(ncid.srt)));

    c->tx_ncid = false;

    return i;
}


uint16_t enc_new_token_frame(struct q_conn * const c,
                             const struct w_iov * const v,
                             const uint16_t pos)
{
    const uint8_t type = FRAM_TYPE_NEW_TOKN;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    const uint64_t tok_len = c->tok_len;
    i = enc(v->buf, v->len, i, &tok_len, 0, 0, "%" PRIu64);
    i = enc_buf(v->buf, v->len, i, c->tok, c->tok_len);

    warn(INF, FRAM_OUT "NEW_TOKEN" NRM " len=%u tok=%s", c->tok_len,
         hex2str(c->tok, c->tok_len));

    return i;
}


uint16_t enc_retire_cid_frame(struct q_conn * const c,
                              const struct w_iov * const v,
                              const uint16_t pos,
                              struct cid * const dcid)
{
    const uint8_t type = FRAM_TYPE_RTIR_CID;
    track_frame(v, type);
    uint16_t i = enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");

    i = enc(v->buf, v->len, i, &dcid->seq, 0, 0, "%" PRIu64);

    warn(INF, FRAM_OUT "RETIRE_CONNECTION_ID" NRM " seq=%" PRIu64, dcid->seq);

    c->tx_retire_cid = false;

    return i;
}


uint16_t enc_ping_frame(const struct w_iov * const v, const uint16_t pos)
{
    const uint8_t type = FRAM_TYPE_PING;
    track_frame(v, type);
    warn(INF, FRAM_OUT "PING" NRM);
    return enc(v->buf, v->len, pos, &type, sizeof(type), 0, "0x%02x");
}

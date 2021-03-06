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

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#define klib_unused

#include <khash.h>
#include <quant/quant.h>
#include <warpcore/warpcore.h>

#include "conn.h"
#include "diet.h"
#include "pkt.h"
#include "quic.h"
#include "stream.h"


#undef STRM_STATE
#define STRM_STATE(k, v) [v] = #k


const char * const strm_state_str[] = {STRM_STATES};

struct q_stream * get_stream(struct q_conn * const c, const int64_t id)
{
    const khiter_t k = kh_get(streams_by_id, c->streams_by_id, (khint64_t)id);
    if (unlikely(k == kh_end(c->streams_by_id)))
        return 0;
    return kh_val(c->streams_by_id, k);
}


int64_t max_sid(const int64_t sid, const struct q_conn * const c)
{
    const int64_t max = is_srv_ini(sid) == c->is_clnt
                            ? (is_uni(sid) ? c->tp_in.max_uni_streams
                                           : c->tp_in.max_bidi_streams)
                            : (is_uni(sid) ? c->tp_out.max_uni_streams
                                           : c->tp_out.max_bidi_streams);
    return unlikely(max == 0)
               ? 0
               : ((max - 1) << 2) | ((STRM_FL_SRV | STRM_FL_UNI) & sid);
}


void apply_stream_limits(struct q_stream * const s)
{
    struct q_conn * const c = s->c;
    s->in_data_max = is_srv_ini(s->id) == c->is_clnt
                         ? (is_uni(s->id) ? c->tp_in.max_strm_data_uni
                                          : c->tp_in.max_strm_data_bidi_remote)
                         : (is_uni(s->id) ? c->tp_in.max_strm_data_uni
                                          : c->tp_in.max_strm_data_bidi_local);
    s->out_data_max =
        is_srv_ini(s->id) == c->is_clnt
            ? (is_uni(s->id) ? c->tp_out.max_strm_data_uni
                             : c->tp_out.max_strm_data_bidi_remote)
            : (is_uni(s->id) ? c->tp_out.max_strm_data_uni
                             : c->tp_out.max_strm_data_bidi_local);

    // if limit is less than an MTU, we are already blocked
    s->blocked = s->out_data_max < w_mtu(c->w);
}


struct q_stream * new_stream(struct q_conn * const c, const int64_t id)
{
    struct q_stream * const s = calloc(1, sizeof(*s));
    ensure(s, "could not calloc q_stream");
    sq_init(&s->out);
    sq_init(&s->in);
    s->c = c;
    s->id = id;
    strm_to_state(s, strm_open);

    if (is_uni(id))
        c->lg_sid_uni = MAX(id, c->lg_sid_uni);
    else
        c->lg_sid_bidi = MAX(id, c->lg_sid_bidi);

    if (unlikely(id < 0)) {
        c->cstreams[strm_epoch(s)] = s;
        return s;
    }

    int ret;
    const khiter_t k =
        kh_put(streams_by_id, c->streams_by_id, (khint64_t)id, &ret);
    ensure(ret >= 0, "inserted");
    kh_val(c->streams_by_id, k) = s;

    apply_stream_limits(s);
    do_stream_id_fc(c, id);

    if (is_srv_ini(id) != c->is_clnt) {
        // this is a local stream
        if (is_uni(id))
            c->next_sid_uni += 4;
        else
            c->next_sid_bidi += 4;
    }

    return s;
}


void free_stream(struct q_stream * const s)
{
    struct q_conn * const c = s->c;
    if (likely(s->id >= 0)) {
#ifndef FUZZING
        warn(DBG, "freeing strm " FMT_SID " on %s conn %s", s->id, conn_type(c),
             cid2str(c->scid));
#endif
        diet_insert(&c->closed_streams, (uint64_t)s->id, 0);
        const khiter_t k =
            kh_get(streams_by_id, c->streams_by_id, (khint64_t)s->id);
        ensure(k != kh_end(c->streams_by_id), "found");
        kh_del(streams_by_id, c->streams_by_id, k);
    } else
        s->c->cstreams[strm_epoch(s)] = 0;

    while (!splay_empty(&s->in_ooo)) {
        struct pkt_meta * const p = splay_min(ooo_by_off, &s->in_ooo);
        // warn(ERR, "idx %u", pm_idx(p));
        ensure(splay_remove(ooo_by_off, &s->in_ooo, p), "removed");
        free_iov(w_iov(c->w, pm_idx(p)));
    }
    q_free(&s->out);
    q_free(&s->in);

    free(s);
}


void track_bytes_in(struct q_stream * const s, const uint64_t n)
{
    if (s->id >= 0)
        // crypto "streams" don't count
        s->c->in_data += n;
    s->in_data += n;
}


void track_bytes_out(struct q_stream * const s, const uint64_t n)
{
    if (s->id >= 0)
        // crypto "streams" don't count
        s->c->out_data += n;
    s->out_data += n;
}


static void __attribute__((nonnull)) reset_pm(const struct w_iov_sq * const q)
{
    struct w_iov * v;
    sq_foreach (v, q, next) {
        // don't reset stream_data_start!
        memset(&meta(v), 0, offsetof(struct pkt_meta, stream_data_start));
        memset(&meta(v).stream_data_len, 0,
               sizeof(meta(v)) - offsetof(struct pkt_meta, stream_data_len));
    }
}


void reset_stream(struct q_stream * const s, const bool forget)
{
    // reset stream offsets
    s->in_data_off = s->in_data = s->out_data = 0;

    if (forget) {
        s->out_nxt = s->out_una = 0;
        q_free(&s->in);
        q_free(&s->out);

    } else {
        //
        struct w_iov * v = s->out_una;
        sq_foreach_from (v, &s->out, next) {
            if (v == s->out_nxt)
                break;
            // remove trailing padding
            v->len = meta(v).stream_data_len;
        }

        s->out_nxt = s->out_una = sq_first(&s->out);

        // reset pkt meta
        reset_pm(&s->in);
        reset_pm(&s->out);
    }
}


void do_stream_fc(struct q_stream * const s)
{
    if (s->c->state != conn_estb || s->id < 0)
        return;

    const uint64_t inc =
        is_uni(s->id) ? INIT_STRM_DATA_UNI : INIT_STRM_DATA_BIDI;

    if (s->in_data + 2 * MAX_PKT_LEN + inc > s->in_data_max) {
        s->tx_max_stream_data = s->c->needs_tx = true;
        s->new_in_data_max = s->in_data_max + 2 * inc;
    }
}


void do_stream_id_fc(struct q_conn * const c, const int64_t sid)
{

    if (is_srv_ini(sid) == c->is_clnt) {
        // this is a remote stream
        if (is_uni(sid)) {
            if ((sid >> 2) + 1 == c->tp_in.max_uni_streams) {
                c->tx_max_sid_uni = true;
                c->tp_in.new_max_uni_streams =
                    c->tp_in.max_uni_streams + INIT_MAX_UNI_STREAMS;
            }
        } else {
            if ((sid >> 2) + 1 == c->tp_in.max_bidi_streams) {
                c->tx_max_sid_bidi = true;
                c->tp_in.new_max_bidi_streams =
                    c->tp_in.max_bidi_streams + INIT_MAX_BIDI_STREAMS;
            }
        }

    } else {
        // this is a local stream
        if (is_uni(sid)) {
            if ((sid >> 2) + 1 == c->tp_out.max_uni_streams)
                c->sid_blocked_uni = true;
        } else {
            if ((sid >> 2) + 1 == c->tp_out.max_bidi_streams)
                c->sid_blocked_bidi = true;
        }
    }
}


void concat_out(struct q_stream * const s, struct w_iov_sq * const q)
{
    if (s->out_nxt == 0)
        s->out_nxt = sq_first(q);

    if (s->out_una == 0)
        s->out_una = sq_first(q);

    sq_concat(&s->out, q);
}

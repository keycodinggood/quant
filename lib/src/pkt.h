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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "conn.h"
#include "pn.h"
#include "quic.h"


#define MAX_PKT_LEN 1252
#define MIN_INI_LEN 1200

#define F_LONG_HDR 0x80

#define F_LH_INIT 0x7F
#define F_LH_RTRY 0x7E
#define F_LH_HSHK 0x7D
#define F_LH_0RTT 0x7C

#define F_SH_KYPH 0x40
#define F_SH_EXP_MASK 0x07
#define F_SH_SPIN 0x04
#define F_SH 0x30
#define F_SH_MASK 0xb8

#define ERR_NONE 0x0
#define ERR_INTERNAL 0x1
#define ERR_FLOW_CONTROL 0x3
#define ERR_STREAM_ID 0x4
// #define ERR_STREAM_STATE 0x5
// #define ERR_FINAL_OFFSET 0x6
#define ERR_FRAME_ENC 0x7
#define ERR_TRANSPORT_PARAMETER 0x8
// #define ERR_VERSION_NEGOTIATION 0x9
#define ERR_PROTOCOL_VIOLATION 0xa
#define ERR_TLS(type) (0x100 + (type))


static inline __attribute__((always_inline, const)) uint8_t
pkt_type(const uint8_t flags)
{
    return flags & (is_set(F_LONG_HDR, flags) ? ~0x80 : F_SH_MASK);
}


static inline __attribute__((always_inline, const)) uint8_t
epoch_for_pkt_type(const uint8_t type)
{
    switch (type) {
    case F_LH_INIT:
    case F_LH_RTRY:
        return 0;
    case F_LH_0RTT:
        return 1;
    case F_LH_HSHK:
        return 2;
    default:
        return 3;
    }
}


static inline __attribute__((always_inline, nonnull)) struct pn_space *
pn_for_pkt_type(struct q_conn * const c, const uint8_t t)
{
    switch (t) {
    case F_LH_INIT:
    case F_LH_RTRY:
        return &c->pn_init.pn;
    case F_LH_0RTT:
        return &c->pn_data.pn;
    case F_LH_HSHK:
        return &c->pn_hshk.pn;
    default:
        return &c->pn_data.pn;
    }
}


struct q_stream;
struct w_iov;
struct w_iov_sq;
struct w_sock;

extern bool __attribute__((nonnull))
dec_pkt_hdr_beginning(struct w_iov * const xv,
                      struct w_iov * const v,
                      const bool is_clnt,
                      struct cid * const odcid,
                      uint8_t * const tok,
                      uint16_t * const tok_len);

extern bool __attribute__((nonnull))
dec_pkt_hdr_remainder(struct w_iov * const xv,
                      struct w_iov * const v,
                      struct q_conn * const c,
                      struct w_iov_sq * const x);

extern bool __attribute__((nonnull)) enc_pkt(struct q_stream * const s,
                                             const bool rtx,
                                             const bool enc_data,
                                             struct w_iov * const v);

extern void __attribute__((nonnull)) coalesce(struct w_iov_sq * const q);

extern void __attribute__((nonnull))
tx_vneg_resp(const struct w_sock * const ws, const struct w_iov * const v);


#ifndef NDEBUG
extern void __attribute__((nonnull(1, 2)))
log_pkt(const char * const dir,
        const struct w_iov * const v,
        const uint32_t ip,
        const uint16_t port,
        const struct cid * const odcid,
        const uint8_t * const tok,
        const uint16_t tok_len);
#else
#define log_pkt(...)                                                           \
    do {                                                                       \
    } while (0)
#endif

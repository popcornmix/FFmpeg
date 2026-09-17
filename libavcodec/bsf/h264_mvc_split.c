/*
 * H.264 MVC per-view packet splitting bitstream filter
 * Copyright (c) 2026 Dom Cobley
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Split an H.264 MVC access unit into one packet per view.
 *
 * A packet is cut wherever the view of its VCL NAL units changes. The non-VCL
 * NAL units in between -- the parameter sets a dependent view activates, or
 * the delimiter and SEI of the access unit that follows -- go with the view
 * they precede. A packet with the VCL NAL units of a single view, which is
 * every packet of a stream without coded slice extensions, passes through
 * untouched.
 *
 * The one thing a view needs from the other side of a cut is the subset SPS
 * declaring the views, which some encoders place between the base view slices
 * and the dependent view slices of the access unit it first applies to. The
 * decoder settles which views it decodes when the base view picture starts,
 * so those subset SPS NAL units are copied into the base view packet as well.
 */

#include <string.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"

#include "libavcodec/bsf.h"
#include "libavcodec/bsf_internal.h"
#include "libavcodec/h264.h"

typedef struct H264MVCSplitContext {
    const AVClass *class;
    /**
     * Whether to cut at all. The decoder clears this when it decodes the base
     * view alone: the dependent view packets would carry nothing it wants,
     * and under frame threading each would still take a thread.
     */
    int       split;

    AVPacket *in;
    /** offset into ::in of the first byte not yet output */
    int       pos;
    /** length prefix size of an avcC stream, 0 for Annex B */
    int       nal_length_size;
    /** ::nal_length_size as settled for the packet in ::in */
    int       pkt_nal_length_size;
} H264MVCSplitContext;

/* NAL units 1, 2, 3, 4 and 5 carry the base view; a coded slice extension
 * carries its view_id, which is never that of the base view. */
#define BASE_VIEW 1024

static void set_extradata(H264MVCSplitContext *s, const uint8_t *data, size_t size)
{
    s->nal_length_size = (size >= 7 && data[0] == 1) ? (data[4] & 3) + 1 : 0;
}

/**
 * @return the view a VCL NAL unit belongs to, or -1 for a non-VCL NAL unit
 */
static int nal_view(const uint8_t *nal, ptrdiff_t size)
{
    switch (nal[0] & 0x1f) {
    case H264_NAL_SLICE:
    case H264_NAL_DPA:
    case H264_NAL_DPB:
    case H264_NAL_DPC:
    case H264_NAL_IDR_SLICE:
        return BASE_VIEW;
    case H264_NAL_EXTEN_SLICE:
        /* nal_unit_header_mvc_extension(), cf. H.7.3.1.1: svc_extension_flag,
         * non_idr_flag, priority_id(6), view_id(10), ... An SVC extension
         * header is not ours to interpret. */
        if (size < 4 || nal[1] & 0x80)
            return -1;
        return (nal[2] << 2) | (nal[3] >> 6);
    }
    return -1;
}

/**
 * @return a pointer to the first byte of the next 00 00 01 prefix at or after
 *         p, or end if there is none
 */
static const uint8_t *next_start_code(const uint8_t *p, const uint8_t *end)
{
    while (end - p >= 3) {
        const uint8_t *z = memchr(p, 0, end - p - 2);

        if (!z)
            break;
        if (z[1] == 0 && z[2] == 1)
            return z;
        p = z + 1;
    }
    return end;
}

#define MAX_SPS_COPY 4

typedef struct Segment {
    int size;                   ///< bytes of the first segment
    int view;                   ///< view of its VCL NAL units, -1 for none
    int has_subset_sps;         ///< whether it holds a subset SPS itself
    int nb_sps;                 ///< subset SPS NAL units found after it
    int sps_off[MAX_SPS_COPY];  ///< their spans, prefix included
    int sps_len[MAX_SPS_COPY];
} Segment;

/**
 * Find how much of buf belongs to the view its first VCL NAL unit is from, and
 * which subset SPS NAL units lie beyond that.
 *
 * seg->size is size itself if all of buf belongs to one view, or if it cannot
 * be parsed.
 */
static void find_segment(const H264MVCSplitContext *s, const uint8_t *buf,
                         int size, Segment *seg)
{
    const uint8_t *end = buf + size;
    const uint8_t *p   = buf;
    const uint8_t *run = NULL; /* start of the non-VCL run preceding p */
    int cut = -1, n;

    memset(seg, 0, sizeof(*seg));
    seg->view = -1;

    while (p < end) {
        const uint8_t *start, *nal, *next;
        int view;

        if (s->pkt_nal_length_size) {
            unsigned len = 0;

            start = p;
            if (end - p < s->pkt_nal_length_size)
                break;
            for (int i = 0; i < s->pkt_nal_length_size; i++)
                len = (len << 8) | *p++;
            if (len > end - p) {
                /* malformed: leave whatever is left uncut */
                cut = -1;
                break;
            }
            nal  = p;
            next = p + len;
        } else {
            const uint8_t *sc = next_start_code(p, end);

            if (sc == end)
                break;
            /* keep the zero_byte of a four byte start code with its NAL unit */
            start = sc > buf && sc[-1] == 0 ? sc - 1 : sc;
            nal   = sc + 3;
            next  = next_start_code(nal, end);
        }
        if (nal >= end)
            break;

        /* Which side of the cut a subset SPS falls on is only known once the
         * cut is: it may sit in the non-VCL run that ends up beyond it. */
        if ((nal[0] & 0x1f) == H264_NAL_SUB_SPS && seg->nb_sps < MAX_SPS_COPY) {
            seg->sps_off[seg->nb_sps] = start - buf;
            seg->sps_len[seg->nb_sps] = next - start;
            seg->nb_sps++;
        }

        view = nal_view(nal, next - nal);
        if (cut >= 0) {
            /* beyond the cut only the subset SPS units are of interest */
        } else if (view < 0) {
            if (seg->view >= 0 && !run)
                run = start;
        } else if (seg->view < 0) {
            /* leading non-VCL NAL units stay with the first view */
            seg->view = view;
        } else if (view != seg->view) {
            cut = (run ? run : start) - buf;
        } else {
            run = NULL;
        }
        p = next;
    }

    seg->size = cut >= 0 ? cut : size;

    /* keep only the subset SPS units beyond the cut */
    n = 0;
    for (int i = 0; i < seg->nb_sps; i++) {
        if (seg->sps_off[i] < seg->size) {
            seg->has_subset_sps = 1;
        } else {
            seg->sps_off[n] = seg->sps_off[i];
            seg->sps_len[n] = seg->sps_len[i];
            n++;
        }
    }
    seg->nb_sps = n;
}

static int h264_mvc_split_filter(AVBSFContext *ctx, AVPacket *out)
{
    H264MVCSplitContext *s = ctx->priv_data;
    Segment seg;
    int remaining, ret;

    if (!s->in->data) {
        const uint8_t *side;
        size_t side_size;

        ret = ff_bsf_get_packet_ref(ctx, s->in);
        if (ret < 0)
            return ret;
        s->pos = 0;

        if (!s->split) {
            av_packet_move_ref(out, s->in);
            return 0;
        }

        side = av_packet_get_side_data(s->in, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
        if (side)
            set_extradata(s, side, side_size);

        /* As the decoder does: an avcC stream may still carry Annex B data. */
        s->pkt_nal_length_size = s->nal_length_size;
        if (s->nal_length_size == 4 && s->in->size > 8) {
            const uint8_t *buf = s->in->data;

            if (AV_RB32(buf) == 1 && AV_RB32(buf + 5) > (unsigned)s->in->size)
                s->pkt_nal_length_size = 0;
        }
    }

    remaining = s->in->size - s->pos;
    find_segment(s, s->in->data + s->pos, remaining, &seg);

    if (!s->pos && seg.size == remaining) {
        av_packet_move_ref(out, s->in);
        return 0;
    }

    if (seg.view == BASE_VIEW && seg.nb_sps && !seg.has_subset_sps) {
        /* The subset SPS the dependent views carry with them declares the
         * views, and the decoder decides which of them to decode when the
         * base view picture starts. Give the base view a copy. */
        const uint8_t *src = s->in->data + s->pos;
        int total = seg.size;

        for (int i = 0; i < seg.nb_sps; i++)
            total += seg.sps_len[i];

        ret = av_new_packet(out, total);
        if (ret < 0)
            goto fail;
        memcpy(out->data, src, seg.size);
        total = seg.size;
        for (int i = 0; i < seg.nb_sps; i++) {
            memcpy(out->data + total, src + seg.sps_off[i], seg.sps_len[i]);
            total += seg.sps_len[i];
        }
        ret = av_packet_copy_props(out, s->in);
        if (ret < 0)
            goto fail;
    } else {
        ret = av_packet_ref(out, s->in);
        if (ret < 0)
            goto fail;
        out->data += s->pos;
        out->size  = seg.size;
    }
    /* applied along with the first segment */
    if (s->pos)
        av_packet_side_data_remove(out->side_data, &out->side_data_elems,
                                   AV_PKT_DATA_NEW_EXTRADATA);

    s->pos += seg.size;
    if (s->pos >= s->in->size)
        av_packet_unref(s->in);

    return 0;

fail:
    av_packet_unref(out);
    av_packet_unref(s->in);
    return ret;
}

static int h264_mvc_split_init(AVBSFContext *ctx)
{
    H264MVCSplitContext *s = ctx->priv_data;

    s->in = av_packet_alloc();
    if (!s->in)
        return AVERROR(ENOMEM);

    set_extradata(s, ctx->par_in->extradata, ctx->par_in->extradata_size);

    return 0;
}

static void h264_mvc_split_flush(AVBSFContext *ctx)
{
    H264MVCSplitContext *s = ctx->priv_data;

    av_packet_unref(s->in);
    s->pos = 0;
}

static void h264_mvc_split_close(AVBSFContext *ctx)
{
    H264MVCSplitContext *s = ctx->priv_data;

    av_packet_free(&s->in);
}

static const enum AVCodecID h264_mvc_split_codec_ids[] = {
    AV_CODEC_ID_H264, AV_CODEC_ID_NONE,
};

#define OFFSET(x) offsetof(H264MVCSplitContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_BSF_PARAM)
static const AVOption h264_mvc_split_options[] = {
    { "split", "cut access units into one packet per view",
        OFFSET(split), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { NULL },
};

static const AVClass h264_mvc_split_class = {
    .class_name = "h264_mvc_split",
    .item_name  = av_default_item_name,
    .option     = h264_mvc_split_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFBitStreamFilter ff_h264_mvc_split_bsf = {
    .p.name         = "h264_mvc_split",
    .p.codec_ids    = h264_mvc_split_codec_ids,
    .p.priv_class   = &h264_mvc_split_class,
    .priv_data_size = sizeof(H264MVCSplitContext),
    .init           = h264_mvc_split_init,
    .flush          = h264_mvc_split_flush,
    .close          = h264_mvc_split_close,
    .filter         = h264_mvc_split_filter,
};

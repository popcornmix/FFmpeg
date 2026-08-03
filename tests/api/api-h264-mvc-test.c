/*
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

/*
 * Decode a two view H.264 MVC Annex B stream from packets that do not start on
 * an access unit boundary: each packet holds the dependent view of one access
 * unit followed by the base view of the next, as some Matroska files do.
 *
 * The first two access units are fed once more ahead of the whole stream, so
 * that a reference picture precedes the IDR the stream then starts with, in
 * the same packet as that IDR's base view. Every frame of the second coded
 * video sequence should decode exactly as it does from aligned packets.
 *
 * Options: prefix= prepends the given bytes. second= appends the stream once
 * more with every view_id doubled and the given parameter set NAL units in
 * place of those of the same type and id. late_ps moves each subset SPS and the
 * PPSs referring to it into the packet of the next dependent view picture.
 * get_format selects all views from the callback once availability is known.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/hash.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#define MAX_NALS 4096
#define MAX_PS   8

typedef struct NAL {
    const uint8_t *data; ///< start code included
    size_t         size;
    int            type;
    int            first_slice; ///< a VCL NAL unit with first_mb_in_slice 0
} NAL;

static int read_ue(const uint8_t *buf, size_t size, size_t *bit)
{
    int zeros = 0;
    unsigned val = 0;

    while (*bit < size * 8 && !((buf[*bit >> 3] >> (7 - (*bit & 7))) & 1)) {
        zeros++;
        (*bit)++;
    }
    (*bit)++;
    if (zeros > 31)
        return -1;
    for (int i = 0; i < zeros && *bit < size * 8; i++, (*bit)++)
        val = (val << 1) | ((buf[*bit >> 3] >> (7 - (*bit & 7))) & 1);
    return (1U << zeros) - 1 + val;
}

static int split_nals(const uint8_t *buf, size_t size, NAL *nals)
{
    int nb = 0;

    for (size_t i = 0; i + 3 < size; i++) {
        size_t start, hdr;

        if (buf[i] || buf[i + 1] || buf[i + 2] != 1)
            continue;
        if (nb == MAX_NALS)
            return AVERROR(ENOMEM);

        start = i && !buf[i - 1] ? i - 1 : i;
        if (nb)
            nals[nb - 1].size = start - (nals[nb - 1].data - buf);
        nals[nb].data = buf + start;
        nals[nb].type = buf[i + 3] & 0x1f;

        /* the coded slice extension has a three byte MVC header extension */
        hdr = i + 3 + (nals[nb].type == 20 ? 4 : 1);
        if (nals[nb].type == 1 || nals[nb].type == 5 || nals[nb].type == 20) {
            size_t bit = 0;
            nals[nb].first_slice = hdr < size && !read_ue(buf + hdr, size - hdr, &bit);
        }
        nb++;
        i += 2;
    }
    if (nb)
        nals[nb - 1].size = size - (nals[nb - 1].data - buf);

    return nb;
}

static int print_frames(AVCodecContext *c, AVFrame *frame)
{
    const AVPixFmtDescriptor *desc;
    struct AVHashContext *hash;
    char sum[AV_HASH_MAX_SIZE * 2 + 1];
    int ret;

    while ((ret = avcodec_receive_frame(c, frame)) >= 0) {
        const AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIEW_ID);

        desc = av_pix_fmt_desc_get(frame->format);
        if ((ret = av_hash_alloc(&hash, "md5")) < 0)
            return ret;
        av_hash_init(hash);
        for (int p = 0; p < 3; p++) {
            int w = p ? AV_CEIL_RSHIFT(frame->width,  desc->log2_chroma_w) : frame->width;
            int h = p ? AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h) : frame->height;
            for (int y = 0; y < h; y++)
                av_hash_update(hash, frame->data[p] + y * frame->linesize[p], w);
        }
        av_hash_final_hex(hash, sum, av_hash_get_size(hash) * 2 + 1);
        av_hash_freep(&hash);

        printf("view %d, %s\n", sd ? *(const int *)sd->data : -1, sum);
        av_frame_unref(frame);
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    printf("error: %s\n", av_err2str(ret));
    return 0;
}

static int decode(AVCodecContext *c, AVFrame *frame, AVPacket *pkt)
{
    int ret = avcodec_send_packet(c, pkt);

    if (ret < 0 && ret != AVERROR_EOF)
        printf("error: %s\n", av_err2str(ret));
    return print_frames(c, frame);
}

static uint8_t *nal_header(const NAL *nal)
{
    return (uint8_t *)nal->data + (nal->data[2] == 1 ? 3 : 4);
}

static int hex_to_bytes(const char *hex, uint8_t *dst)
{
    int len = strcspn(hex, ",") / 2;

    for (int i = 0; i < len; i++)
        sscanf(hex + 2 * i, "%2hhx", &dst[i]);
    return len;
}

/* Move each subset SPS, and the PPSs referring to it, from ahead of the base
 * view slices to just before the next dependent view picture. */
static int move_late_ps(const NAL *src, int nb, NAL *dst)
{
    NAL held[16];
    int nb_held = 0, nb_dst = 0, subset_sps_id = -1;

    for (int i = 0; i < nb; i++) {
        const uint8_t *hdr = nal_header(&src[i]);
        size_t len = src[i].size - (hdr - src[i].data), bit = 0;

        if (src[i].type == 15 && nb_held < FF_ARRAY_ELEMS(held)) {
            bit = 24;
            subset_sps_id = read_ue(hdr + 1, len - 1, &bit);
            held[nb_held++] = src[i];
            continue;
        }
        if (src[i].type == 8 && nb_held && nb_held < FF_ARRAY_ELEMS(held)) {
            read_ue(hdr + 1, len - 1, &bit);
            if (read_ue(hdr + 1, len - 1, &bit) == subset_sps_id) {
                held[nb_held++] = src[i];
                continue;
            }
        }
        if (src[i].type == 20 && src[i].first_slice) {
            memcpy(dst + nb_dst, held, nb_held * sizeof(*held));
            nb_dst += nb_held;
            nb_held = 0;
        }
        dst[nb_dst++] = src[i];
    }
    return nb_dst;
}

static enum AVPixelFormat select_views(AVCodecContext *c,
                                       const enum AVPixelFormat *fmts)
{
    unsigned nb_views = 0;

    if (av_opt_get_array_size(c->priv_data, "view_ids_available", 0, &nb_views) < 0)
        return AV_PIX_FMT_NONE;
    if (nb_views > 1 && av_opt_set(c->priv_data, "view_ids", "-1", 0) < 0)
        return AV_PIX_FMT_NONE;
    return avcodec_default_get_format(c, fmts);
}

int main(int argc, char **argv)
{
    const AVCodec *codec;
    AVCodecContext *c = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    uint8_t *buf = NULL;
    NAL *nals = NULL, *nals2 = NULL, *seq = NULL, *tmp = NULL;
    FILE *file = NULL;
    const char *prefix_hex = "", *second_hex = NULL;
    long size, prefix, second = 0;
    int nb_nals, nb_nals2 = 0, nb_seq = 0, au = 0, late_ps = 0, threads, start, ret = 1;
    int use_get_format = 0;

    for (int i = 3; i < argc; i++) {
        if (!strncmp(argv[i], "prefix=", 7))
            prefix_hex = argv[i] + 7;
        else if (!strncmp(argv[i], "second=", 7) && strlen(argv[i]) > 9)
            second_hex = argv[i] + 7;
        else if (!strcmp(argv[i], "late_ps"))
            late_ps = 1;
        else if (!strcmp(argv[i], "get_format"))
            use_get_format = 1;
        else
            argc = 0;
    }
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <threads> <input file> [prefix=<hex>] "
                "[second=<hex NAL>[,<hex NAL>...]] [late_ps] [get_format]\n", argv[0]);
        return 1;
    }
    threads = FFMAX(atoi(argv[1]), 1);

    if (!(file = fopen(argv[2], "rb")) || fseek(file, 0, SEEK_END) ||
        (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) {
        fprintf(stderr, "Could not read %s\n", argv[2]);
        goto end;
    }
    prefix = strlen(prefix_hex) / 2;
    if (second_hex)
        second = size + 3 * MAX_PS + strlen(second_hex) / 2;
    buf   = av_malloc(prefix + size + second);
    nals  = av_malloc_array(MAX_NALS, sizeof(*nals));
    nals2 = av_malloc_array(MAX_NALS, sizeof(*nals2));
    seq   = av_malloc_array(3 * MAX_NALS, sizeof(*seq));
    tmp   = av_malloc_array(3 * MAX_NALS, sizeof(*tmp));
    if (!buf || !nals || !nals2 || !seq || !tmp ||
        fread(buf + prefix, 1, size, file) != size)
        goto end;
    hex_to_bytes(prefix_hex, buf);

    nb_nals = split_nals(buf, prefix + size, nals);
    if (nb_nals <= 0)
        goto end;

    /* The same stream again, with every view_id doubled, and the given subset
     * SPS and PPSs, which are to declare the doubled view_ids, in place of the
     * ones of the same type and, for a PPS, id. */
    if (second_hex) {
        uint8_t *ps = buf + prefix + 2 * size;
        NAL ps_nals[MAX_PS];
        int nb_ps = 0;

        memcpy(buf + prefix + size, buf + prefix, size);
        for (const char *hex = second_hex; *hex && nb_ps < MAX_PS; nb_ps++) {
            ps[0] = ps[1] = 0;
            ps[2] = 1;
            ps_nals[nb_ps].data = ps;
            ps_nals[nb_ps].size = 3 + hex_to_bytes(hex, ps + 3);
            ps_nals[nb_ps].type = ps[3] & 0x1f;
            ps  += ps_nals[nb_ps].size;
            hex += strcspn(hex, ",");
            hex += *hex == ',';
        }

        nb_nals2 = split_nals(buf + prefix + size, size, nals2);
        if (nb_nals2 <= 0)
            goto end;
        for (int i = 0; i < nb_nals2; i++) {
            uint8_t *hdr = nal_header(&nals2[i]);

            if (nals2[i].type == 14 || nals2[i].type == 20) {
                int view_id = 2 * (hdr[2] << 2 | hdr[3] >> 6);
                hdr[2] = view_id >> 2;
                hdr[3] = (hdr[3] & 0x3f) | (view_id & 3) << 6;
            }
            for (int j = 0; j < nb_ps; j++) {
                const uint8_t *hdr_ps = nal_header(&ps_nals[j]);
                size_t bit = 0, bit_ps = 0;

                if (ps_nals[j].type != nals2[i].type ||
                    (nals2[i].type == 8 &&
                     read_ue(hdr + 1, 1, &bit) != read_ue(hdr_ps + 1, 1, &bit_ps)))
                    continue;
                nals2[i].data = ps_nals[j].data;
                nals2[i].size = ps_nals[j].size;
            }
        }
    }

    /* The first two access units, then the whole stream. An access unit begins
     * with the first NAL unit after a VCL one that is not part of the same
     * access unit: anything but a dependent view slice or a further slice of
     * the same base view picture. */
    for (int i = 0; i < nb_nals; i++) {
        if (i && (nals[i - 1].type == 1 || nals[i - 1].type == 5 || nals[i - 1].type == 20) &&
            nals[i].type != 20 &&
            !((nals[i].type == 1 || nals[i].type == 5) && !nals[i].first_slice) &&
            ++au == 2)
            break;
        seq[nb_seq++] = nals[i];
    }
    memcpy(seq + nb_seq, nals, nb_nals * sizeof(*nals));
    nb_seq += nb_nals;
    memcpy(seq + nb_seq, nals2, nb_nals2 * sizeof(*nals2));
    nb_seq += nb_nals2;

    if (late_ps) {
        nb_seq = move_late_ps(seq, nb_seq, tmp);
        FFSWAP(NAL *, seq, tmp);
    }

    if (!(codec = avcodec_find_decoder(AV_CODEC_ID_H264)) ||
        !(c = avcodec_alloc_context3(codec)))
        goto end;
    c->thread_count    = threads;
    c->err_recognition = AV_EF_EXPLODE;
    /* no timestamps here to infer the reorder depth from */
    c->has_b_frames    = 4;
    if (use_get_format)
        c->get_format = select_views;
    else if (av_opt_set(c->priv_data, "view_ids", "-1", 0) < 0)
        goto end;
    if (avcodec_open2(c, codec, NULL) < 0)
        goto end;

    if (!(pkt = av_packet_alloc()) || !(frame = av_frame_alloc()))
        goto end;

    /* cut before the first slice of each dependent view picture, and before
     * any parameter sets moved there */
    start = 0;
    for (int i = 1; i <= nb_seq; i++) {
        size_t len = 0;

        if (i < nb_seq && !(seq[i].type == 20 && seq[i].first_slice))
            continue;
        if (i < nb_seq && late_ps)
            while (i - 1 > start && (seq[i - 1].type == 15 || seq[i - 1].type == 8))
                i--;

        for (int j = start; j < i; j++)
            len += seq[j].size;
        if (av_new_packet(pkt, len) < 0)
            goto end;
        len = 0;
        for (int j = start; j < i; j++) {
            memcpy(pkt->data + len, seq[j].data, seq[j].size);
            len += seq[j].size;
        }
        pkt->pts = pkt->dts = start;
        decode(c, frame, pkt);
        av_packet_unref(pkt);
        start = i;
        while (i + 1 < nb_seq && !(seq[i].type == 20 && seq[i].first_slice))
            i++;
    }
    decode(c, frame, NULL);
    ret = 0;

end:
    if (file)
        fclose(file);
    av_free(buf);
    av_free(nals);
    av_free(nals2);
    av_free(seq);
    av_free(tmp);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&c);
    return ret;
}

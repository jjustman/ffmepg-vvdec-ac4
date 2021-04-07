/*
 * VVC MP4 to Annex B byte stream format filter
 * copyright (c) 2015 Anton Khirnov
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

#include <string.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "bsf.h"
#include "bsf_internal.h"
#include "bytestream.h"
#include "vvc.h"

#include "libavcodec/get_bits.h"

#define MIN_VVCC_LENGTH 23

typedef struct VVCBSFContext {
    uint8_t  length_size;
    int      extradata_parsed;
} VVCBSFContext;

static int vvc_extradata_to_annexb(AVBSFContext *ctx)
{
    GetByteContext gb;
    int length_size, num_arrays, i, j;
    int ret = 0;
    int temp = 0;
    int ptl_present;

    uint8_t *new_extradata = NULL;
    size_t   new_extradata_size = 0;

    bytestream2_init(&gb, ctx->par_in->extradata, ctx->par_in->extradata_size);
    bytestream2_skip(&gb, 3);
    temp = bytestream2_get_byte(&gb);
    length_size = (( temp & 6) >> 1) + 1;
    ptl_present = temp & 1;
    if (ptl_present)
       bytestream2_skip(&gb, 7);
    bytestream2_skip(&gb, 1);
    num_arrays  = bytestream2_get_byte(&gb);

    av_log(ctx, AV_LOG_INFO, "annexb: %d, %d, %d\n", ptl_present, num_arrays, ctx->par_in->extradata_size);


    for (i = 0; i < num_arrays; i++) {
        int type = bytestream2_get_byte(&gb) & 0x3f;
        int cnt  = bytestream2_get_be16(&gb);

        av_log(ctx, AV_LOG_INFO, "TYPE: %d, cnt %d\n", type, cnt);

        if (!(type == VVC_VPS_NUT || type == VVC_SPS_NUT || type == VVC_PPS_NUT ||
              type == VVC_PREFIX_SEI_NUT || type == VVC_SUFFIX_SEI_NUT)) {
            av_log(ctx, AV_LOG_ERROR, "Invalid NAL unit type in extradata: %d\n",
                   type);
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        for (j = 0; j < cnt; j++) {
            int nalu_len = bytestream2_get_be16(&gb);

            if (4 + AV_INPUT_BUFFER_PADDING_SIZE + nalu_len > SIZE_MAX - new_extradata_size) {
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            ret = av_reallocp(&new_extradata, new_extradata_size + nalu_len + 4 + AV_INPUT_BUFFER_PADDING_SIZE);
            if (ret < 0)
                goto fail;

            AV_WB32(new_extradata + new_extradata_size, 1); // add the startcode
            bytestream2_get_buffer(&gb, new_extradata + new_extradata_size + 4, nalu_len);
            new_extradata_size += 4 + nalu_len;
            memset(new_extradata + new_extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        }
    }

    av_freep(&ctx->par_out->extradata);
    ctx->par_out->extradata      = new_extradata;
    ctx->par_out->extradata_size = new_extradata_size;

    if (!new_extradata_size)
        av_log(ctx, AV_LOG_WARNING, "No parameter sets in the extradata\n");

    return length_size;
fail:
    av_freep(&new_extradata);
    return ret;
}

static int vvc_mp4toannexb_init(AVBSFContext *ctx)
{
    VVCBSFContext *s = ctx->priv_data;
    int ret;

    if (ctx->par_in->extradata_size < MIN_VVCC_LENGTH ||
        AV_RB24(ctx->par_in->extradata) == 1           ||
        AV_RB32(ctx->par_in->extradata) == 1) {
        av_log(ctx, AV_LOG_VERBOSE,
               "The input looks like it is Annex B already\n");
    } else {
        ret = vvc_extradata_to_annexb(ctx);
        if (ret < 0)
            return ret;
        s->length_size      = ret;
        s->extradata_parsed = 1;
    }

    return 0;
}

static int vvc_mp4toannexb_filter(AVBSFContext *ctx, AVPacket *out)
{
    VVCBSFContext *s = ctx->priv_data;
    AVPacket *in;
    GetByteContext gb;

    int got_irap = 0;
    int i, ret = 0;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    if (!s->extradata_parsed) {
        av_packet_move_ref(out, in);
        av_packet_free(&in);
        return 0;
    }

    bytestream2_init(&gb, in->data, in->size);

    while (bytestream2_get_bytes_left(&gb)) {
        uint32_t nalu_size = 0;
        int      nalu_type;
        int is_irap, add_extradata, extra_size, prev_size;

        if (bytestream2_get_bytes_left(&gb) < s->length_size) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        for (i = 0; i < s->length_size; i++)
            nalu_size = (nalu_size << 8) | bytestream2_get_byte(&gb);

        if (nalu_size < 2 || nalu_size > bytestream2_get_bytes_left(&gb)) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        nalu_type = (bytestream2_peek_byte(&gb) >> 1) & 0x3f;

        /* prepend extradata to IRAP frames */
        is_irap       = nalu_type >= 16 && nalu_type <= 23;
        add_extradata = is_irap && !got_irap;
        extra_size    = add_extradata * ctx->par_out->extradata_size;
        got_irap     |= is_irap;

        if (FFMIN(INT_MAX, SIZE_MAX) < 4ULL + nalu_size + extra_size) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        prev_size = out->size;

        ret = av_grow_packet(out, 4 + nalu_size + extra_size);
        if (ret < 0)
            goto fail;

        if (extra_size)
            memcpy(out->data + prev_size, ctx->par_out->extradata, extra_size);
        AV_WB32(out->data + prev_size + extra_size, 1);
        bytestream2_get_buffer(&gb, out->data + prev_size + 4 + extra_size, nalu_size);
    }

    ret = av_packet_copy_props(out, in);
    if (ret < 0)
        goto fail;

fail:
    if (ret < 0)
        av_packet_unref(out);
    av_packet_free(&in);

    return ret;
}

static const enum AVCodecID codec_ids[] = {
    AV_CODEC_ID_VVC, AV_CODEC_ID_NONE,
};

const AVBitStreamFilter ff_vvc_mp4toannexb_bsf = {
    .name           = "vvc_mp4toannexb",
    .priv_data_size = sizeof(VVCBSFContext),
    .init           = vvc_mp4toannexb_init,
    .filter         = vvc_mp4toannexb_filter,
    .codec_ids      = codec_ids,
};

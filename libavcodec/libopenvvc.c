/*
 * VVC video Decoder
 *
 * Copyright (C) 2012 - 2021 Pierre-Loup Cabarat
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

#include <ovdec.h>
#include <ovdefs.h>
#include <ovunits.h>
#include <ovframe.h>

#include "libavutil/attributes.h"
#include "libavutil/opt.h"

#include "bytestream.h"

#include "profiles.h"
#include "avcodec.h"
#include "vvc.h"
#include "h2645_parse.h"

struct OVDecContext{
     AVClass *c;
     OVVCDec* libovvc_dec;
     int nal_length_size;
     int is_nalff;
};

static int convert_avpkt(OVPictureUnit *ovpu, const H2645Packet *pkt) {
    int i;
    ovpu->nb_nalus = pkt->nb_nals;
    ovpu->nalus = av_malloc(sizeof(*ovpu->nalus) * ovpu->nb_nalus);
    if (!ovpu->nb_nalus) {
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < ovpu->nb_nalus; ++i) {
         const H2645NAL *avnalu = &pkt->nals[i];
         OVNALUnit *ovnalu = &ovpu->nalus[i];
         ovnalu->rbsp_data = avnalu->rbsp_buffer;
         ovnalu->rbsp_size = avnalu->raw_size;
         ovnalu->epb_pos   = avnalu->skipped_bytes_pos;
         ovnalu->nb_epb    = avnalu->skipped_bytes;
         ovnalu->type = avnalu->type;
    }
    return 0;
}

static void dummy_unref(void *opaque, uint8_t *data){
}

static void ovvc_unref(void *opaque, uint8_t *data) {

    ovframe_unref(&data);
}

static void convert_frame(AVFrame *avframe, const OVFrame *ovframe) {
    avframe->data[0] = ovframe->data[0];
    avframe->data[1] = ovframe->data[1];
    avframe->data[2] = ovframe->data[2];

    avframe->linesize[0] = ovframe->linesize[0];
    avframe->linesize[1] = ovframe->linesize[1];
    avframe->linesize[2] = ovframe->linesize[2];

    avframe->width  = ovframe->width[0];
    avframe->height = ovframe->height[0];

    avframe->buf[0] = av_buffer_create(ovframe, sizeof(ovframe),
                                       ovvc_unref, NULL, 0);

    avframe->pict_type = AV_PIX_FMT_YUV420P10;

}

static int ff_vvc_decode_extradata(const uint8_t *data, int size, OVVCDec *dec,
                                   int *is_nalff, int *nal_length_size,
                                   void *logctx)
{
    int i, j, num_arrays, nal_len_size, b, has_ptl, num_sublayers;
    int ret = 0;
    GetByteContext gb;

    bytestream2_init(&gb, data, size);

    /* It seems the extradata is encoded as hvcC format.
     * Temporarily, we support configurationVersion==0 until 14496-15 3rd
     * is finalized. When finalized, configurationVersion will be 1 and we
     * can recognize hvcC by checking if avctx->extradata[0]==1 or not. */

    av_log(c, AV_LOG_WARNING, "Extra data support is experimental in openVVC.\n
Informations on pictures dimension are assumed to default parameters.
It will be set to correct value as soon as a first picture is available on the decoder output.\n");

    *is_nalff = 1;



    b = bytestream2_get_byte(&gb);

    num_sublayers = (b >> 3) & 0x7;

    nal_len_size  = ((b >> 1) & 0x3) + 1;

    has_ptl = b & 0x1;

    if (has_ptl) {
        int num_bytes_constraint_info;
        int general_profile_idc;
        int general_tier_flag;
        int ptl_num_sub_profiles;
        int temp3, temp4;
        int temp2 = bytestream2_get_be16(&gb);
        int ols_idx  = (temp2 >> 7) & 0x1ff;
        int num_sublayers  = (temp2 >> 4) & 0x7;
        int constant_frame_rate = (temp2 >> 2) & 0x3;
        int chroma_format_idc = temp2 & 0x3;
        int bit_depth_minus8 = (bytestream2_get_byte(&gb) >> 5) & 0x7;
        av_log(logctx, AV_LOG_DEBUG,
            "bit_depth_minus8 %d chroma_format_idc %d\n", bit_depth_minus8, chroma_format_idc);
        // VvcPTLRecord(num_sublayers) native_ptl
        temp3 = bytestream2_get_byte(&gb);
        num_bytes_constraint_info = (temp3) & 0x3f;
        temp4 = bytestream2_get_byte(&gb);
        general_profile_idc = (temp4 >> 1) & 0x7f;
        general_tier_flag = (temp4) & 1;
        av_log(logctx, AV_LOG_DEBUG,
            "general_profile_idc %d, num_sublayers %d num_bytes_constraint_info %d\n", general_profile_idc, num_sublayers, num_bytes_constraint_info);
        for (i = 0; i < num_bytes_constraint_info; i++)
            // unsigned int(1) ptl_frame_only_constraint_flag;
            // unsigned int(1) ptl_multi_layer_enabled_flag;
            // unsigned int(8*num_bytes_constraint_info - 2) general_constraint_info;
            bytestream2_get_byte(&gb);
        /*for (i=num_sublayers - 2; i >= 0; i--)
            unsigned int(1) ptl_sublayer_level_present_flag[i];
        for (j=num_sublayers; j<=8 && num_sublayers > 1; j++)
            bit(1) ptl_reserved_zero_bit = 0;
        */
        bytestream2_get_byte(&gb);
        /*for (i=num_sublayers-2; i >= 0; i--)
            if (ptl_sublayer_level_present_flag[i])
                unsigned int(8) sublayer_level_idc[i]; */
        ptl_num_sub_profiles = bytestream2_get_byte(&gb);

        for (j=0; j < ptl_num_sub_profiles; j++) {
            // unsigned int(32) general_sub_profile_idc[j];
            bytestream2_get_be16(&gb);
            bytestream2_get_be16(&gb);
        }

        int max_picture_width = bytestream2_get_be16(&gb); // unsigned_int(16) max_picture_width;
        int max_picture_height = bytestream2_get_be16(&gb); // unsigned_int(16) max_picture_height;
        int avg_frame_rate = bytestream2_get_be16(&gb); // unsigned int(16) avg_frame_rate; }
        av_log(logctx, AV_LOG_DEBUG,
            "max_picture_width %d, max_picture_height %d, avg_frame_rate %d\n", max_picture_width, max_picture_height, avg_frame_rate);
    }

    num_arrays  = bytestream2_get_byte(&gb);



    /* nal units in the hvcC always have length coded with 2 bytes,
     * so put a fake nal_length_size = 2 while parsing them */
    *nal_length_size = 2;

    /* Decode nal units from hvcC. */
    for (i = 0; i < num_arrays; i++) {
        int cnt;
        int type = bytestream2_get_byte(&gb) & 0x1f;

        if (type != VVC_OPI_NUT || type != VVC_DCI_NUT)
            cnt  = bytestream2_get_be16(&gb);
        else
            cnt = 1;

        av_log(logctx, AV_LOG_DEBUG,
            "nalu_type %d cnt %d\n", type, cnt);

        for (j = 0; j < cnt; j++) {
            // +2 for the nal size field

            int nalsize = bytestream2_peek_be16(&gb) + 2;
            av_log(logctx, AV_LOG_DEBUG,
               "nalsize %d \n", nalsize);


            OVPictureUnit ovpu= {0};

            if (bytestream2_get_bytes_left(&gb) < nalsize) {
                av_log(logctx, AV_LOG_ERROR,
                       "Invalid NAL unit size in extradata.\n");
                return AVERROR_INVALIDDATA;
            }

            /* FIMXE unrequired malloc */
            ovpu.nalus = av_mallocz(sizeof(OVNALUnit));
            OVNALUnit *ovnalu = &ovpu.nalus[0];
            ovnalu->rbsp_data = gb.buffer;
            ovnalu->rbsp_size = nalsize;
            {int k; uint8_t *p_data = gb.buffer;
                for (k=0; k<nalsize; k++)
                av_log(logctx, AV_LOG_DEBUG,
                    "%02x ", p_data[k]);
            }

            av_log(logctx, AV_LOG_DEBUG,
                "\n");

            ret = ovdec_submit_picture_unit(dec, &ovpu);

            av_free(ovpu.nalus);
#if 0
            ret = vvc_decode_nal_units(gb.buffer, nalsize, ps, sei, *is_nalff,
                                       *nal_length_size, err_recognition,
                                       logctx);
#endif
            if (ret < 0) {
                av_log(logctx, AV_LOG_ERROR,
                       "Decoding nal unit %d %d from hvcC failed\n",
                       type, i);
                return ret;
            }
            bytestream2_skip(&gb, nalsize);
        }
    }

    /* Now store right nal length size, that will be used to parse
     * all other nals */
    *nal_length_size = nal_len_size;

    return ret;
}

static int libovvc_decode_frame(AVCodecContext *c, void *outdata, int *outdata_size, AVPacket *avpkt) {


    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;
    OVVCDec *libovvc_dec = dec_ctx->libovvc_dec;
    OVFrame *ovframe = NULL;
    int *nb_pic_out = outdata_size;
    int ret;

    OVPictureUnit ovpu;
    H2645Packet pkt = {0};

    *nb_pic_out = 0;

    if (avpkt->side_data_elems) {
        av_log(NULL, AV_LOG_ERROR, "Unsupported side data\n");
    }

    ret = ff_h2645_packet_split(&pkt, avpkt->data, avpkt->size, c, dec_ctx->is_nalff, dec_ctx->nal_length_size, AV_CODEC_ID_VVC, 0, 0);
    if (ret < 0) {
        av_log(c, AV_LOG_ERROR, "Error splitting the input into NAL units.\n");
        return ret;
    }

    convert_avpkt(&ovpu, &pkt);

    ret = ovdec_submit_picture_unit(libovvc_dec, &ovpu);
    if (ret < 0) {
        av_free(ovpu.nalus);
        return AVERROR_INVALIDDATA;
    }

    ovdec_receive_picture(libovvc_dec, &ovframe);

    /* FIXME use ret instead of frame */
    if (ovframe) {
        c->pix_fmt = AV_PIX_FMT_YUV420P10;
        c->width   = ovframe->width[0];
        c->height  = ovframe->height[0];
        c->coded_width   = ovframe->width[0];
        c->coded_height  = ovframe->height[0];

        av_log(NULL, AV_LOG_TRACE, "Received pic with POC: %d\n", ovframe->poc);
        convert_frame(outdata, ovframe);

        *nb_pic_out = 1;
    }

    av_free(ovpu.nalus);
    return 0;
}

static int libovvc_decode_init(AVCodecContext *c) {
    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;
    OVVCDec **libovvc_dec_p = (OVVCDec**) &dec_ctx->libovvc_dec;
    int ret;

    ret = ovdec_init(libovvc_dec_p);
    if (ret < 0) {
        av_log(c, AV_LOG_ERROR, "Could not init Open VVC decoder\n");
        return AVERROR_DECODER_NOT_FOUND;
    }
    dec_ctx->is_nalff = 0;
    dec_ctx->nal_length_size = 0;

    if (c->extradata && c->extradata_size) {
        struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;
        OVVCDec *libovvc_dec = dec_ctx->libovvc_dec;
        OVFrame *ovframe = NULL;

        if (c->extradata_size > 3 && (c->extradata[0] || c->extradata[1] || c->extradata[2] > 1)) {

    OVFrame *ovframe = NULL;
            ret = ff_vvc_decode_extradata(c->extradata, c->extradata_size, libovvc_dec,
                                          &dec_ctx->is_nalff, &dec_ctx->nal_length_size, c);

    #if 0
    ovdec_receive_picture(libovvc_dec, &ovframe);

    /* FIXME use ret instead of frame */
    if (ovframe) {
        c->pix_fmt = AV_PIX_FMT_YUV420P10;
#if 1
        c->width   = ovframe->width[0];
        c->height  = ovframe->height[0];
        c->coded_width   = ovframe->width[0];
        c->coded_height  = ovframe->height[0];
#endif

        ovframe_unref(&ovframe);

    }
    #elif 0

        c->pix_fmt = AV_PIX_FMT_YUV420P10;
        c->width   = 1920;
        c->height  = 1080;
        c->coded_width   = 1920;
        c->coded_height  = 1080;
        c->framerate.num=50;
        c->framerate.den=1;
    #endif
            if (ret < 0) {
                av_log(c, AV_LOG_ERROR, "Error splitting the input into NAL units.\n");
                return ret;
            }
            av_log(c, AV_LOG_ERROR, "Experimental format\n");
        } else {
            av_log(c, AV_LOG_ERROR, "Extra data init\n");
        }
    }

        #if 0
        c->pix_fmt = AV_PIX_FMT_YUV420P10;
        c->width   = 3840;
        c->height  = 2160;
        c->coded_width   = 3840;
        c->coded_height  = 2160;
        c->framerate.num=50;
        c->framerate.den=1;
        #endif
    return 0;
}

static int libovvc_decode_free(AVCodecContext *c) {

    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;

    ovdec_close(dec_ctx->libovvc_dec);

    dec_ctx->libovvc_dec = NULL;
    return 0;
}

static void libovvc_decode_flush(AVCodecContext *c) {
            av_log(c, AV_LOG_ERROR, "FLUSH\n");

}

static int libovvc_update_thread_context(AVCodecContext *dst, const AVCodecContext *src) {

    return 0;
}

static const AVOption options[] = {
    { NULL },
};

static const AVClass libovvc_decoder_class = {
    .class_name = "Open VVC decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_libopenvvc_decoder = {
    .name                  = "ovvc",
    .long_name             = NULL_IF_CONFIG_SMALL("Open VVC(Versatile Video Coding)"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_VVC,
    .priv_data_size        = sizeof(OVVCDec*),
    .priv_class            = &libovvc_decoder_class,
    .init                  = libovvc_decode_init,
    .close                 = libovvc_decode_free,
    .decode                = libovvc_decode_frame,
    .flush                 = libovvc_decode_flush,
    .update_thread_context = ONLY_IF_THREADS_ENABLED(libovvc_update_thread_context),
    #if 0
    .capabilities          = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
    .caps_internal         = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_EXPORTS_CROPPING,
                             #endif
    .profiles              = NULL_IF_CONFIG_SMALL(ff_vvc_profiles),
};
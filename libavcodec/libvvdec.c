/**
  \ingroup HHIVVCFFmpegPlugin
  \file    HHIVVCDecContext.cpp
  \brief   This file contains the implementation of the hhi vvc VVdeC library.
  \author  christian.lehmann@hhi.fraunhofer.de
  \date    March/20/2021

  Copyright:
  2021 Fraunhofer Institute for Telecommunications, Heinrich-Hertz-Institut (HHI)
  The copyright of this software source code is the property of HHI.
  This software may be used and/or copied only with the written permission
  of HHI and in accordance with the terms and conditions stipulated
  in the agreement/contract under which the software has been supplied.
  The software distributed under this license is distributed on an "AS IS" basis,
  WITHOUT WARRANTY OF ANY KIND, either expressed or implied.

*/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


#include "libavcodec/avcodec.h"

#include "libavutil/avutil.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"



#include "vvdec/vvdec.h"

#if defined( __linux__ ) || defined( __APPLE__ )
# include <unistd.h>
#elif defined( _WIN32 )
// picks up GetSystemInfo()
# include <windows.h>
#endif

#define VVDEC_LOG_ERROR( ...) \
    { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        return AVERROR(EINVAL); \
    }

#define VVDEC_LOG_WARNING( ...) \
    { \
        av_log(avctx, AV_LOG_WARNING, __VA_ARGS__); \
    }

#define VVDEC_LOG_INFO( ...) \
    { \
        av_log(avctx, AV_LOG_INFO, __VA_ARGS__); \
    }

#define VVDEC_LOG_VERBOSE( ...) \
    { \
        av_log(avctx, AV_LOG_VERBOSE, __VA_ARGS__); \
    }

#define VVDEC_LOG_DBG( ...) \
    { \
        av_log(avctx, AV_LOG_DEBUG, __VA_ARGS__); \
    }

typedef struct VVdeCOptions {
    int upscaling_mode;
} VVdeCOptions;

typedef struct VVdeCContext {
   AVClass         *av_class;
   VVdeCOptions     options;      // decoding options
   vvdecDecoder*    vvdecDec;
   bool             bFlush;
}VVdeCContext;


static av_cold void ff_vvdec_log_callback(void *avctx, int level, const char *fmt, va_list args )
{
  vfprintf( level == 1 ? stderr : stdout, fmt, args );
}


static av_cold void ff_vvdec_printParameterInfo( AVCodecContext *avctx, vvdecParams* params )
{
  // print some encoder info
  VVDEC_LOG_DBG( "Version info: vvdec %s\n",vvdec_get_version() );
  VVDEC_LOG_DBG( "threads: %d\n",params->threads );
}

static av_cold int ff_vvdec_set_pix_fmt(AVCodecContext *avctx, vvdecFrame* frame )
{
//    static const enum AVColorRange color_ranges[] = {
//        AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG
//    };
    //avctx->color_range = color_ranges[img->range];
    //avctx->color_primaries = img->cp;

    //avctx->colorspace  = AVCOL_SPC_BT709;
    //avctx->color_trc   = AVCOL_TRC_BT709;

    switch ( frame->colorFormat )
    {
    case VVDEC_CF_YUV420_PLANAR:
        if (frame->bitDepth == 8)
        {
            avctx->pix_fmt = frame->numPlanes == 1 ?
                             AV_PIX_FMT_GRAY8 : AV_PIX_FMT_YUV420P;
            avctx->profile = FF_PROFILE_VVC_MAIN_10;
            return 0;
        }
        else if (frame->bitDepth == 10)
        {
            avctx->pix_fmt = frame->numPlanes == 1 ?
                             AV_PIX_FMT_GRAY10 : AV_PIX_FMT_YUV420P10;
            avctx->profile = FF_PROFILE_VVC_MAIN_10;
            return 0;
        }
//        else if (frame->bitDepth == 12)
//        {
//            avctx->pix_fmt = frame->numPlanes == 1 ?
//                             AV_PIX_FMT_GRAY12 : AV_PIX_FMT_YUV420P12;
//            avctx->profile = FF_PROFILE_VVC_MAIN_10;
//            return 0;
//        }
        else
        {
            return AVERROR_INVALIDDATA;
        }
//    case AOM_IMG_FMT_I422:
//        if (frame->bitDepth == 8) {
//            avctx->pix_fmt = AV_PIX_FMT_YUV422P;
//            avctx->profile = FF_PROFILE_AV1_PROFESSIONAL;
//            return 0;
//        } else if (frame->bitDepth == 10) {
//            avctx->pix_fmt = AV_PIX_FMT_YUV422P10;
//            avctx->profile = FF_PROFILE_AV1_PROFESSIONAL;
//            return 0;
//        } else if (frame->bitDepth == 12) {
//            avctx->pix_fmt = AV_PIX_FMT_YUV422P12;
//            avctx->profile = FF_PROFILE_AV1_PROFESSIONAL;
//            return 0;
//        } else {
//            return AVERROR_INVALIDDATA;
//        }
    default:
        return AVERROR_INVALIDDATA;
    }
}

/*
 * implementation of the interface functions
 */
static av_cold int ff_vvdec_decode_init(AVCodecContext *avctx)
{
  VVdeCContext *s = (VVdeCContext*)avctx->priv_data;

  VVDEC_LOG_DBG("ff_vvdec_decode_init::init() threads %d\n", avctx->thread_count );

  vvdecParams params;
  vvdec_params_default( &params );
  params.logLevel = VVDEC_DETAILS;

  if     ( av_log_get_level() >= AV_LOG_DEBUG )   params.logLevel = VVDEC_DETAILS;
  else if( av_log_get_level() >= AV_LOG_VERBOSE ) params.logLevel = VVDEC_INFO;     // VVDEC_INFO will output per picture info
  else if( av_log_get_level() >= AV_LOG_INFO )    params.logLevel = VVDEC_WARNING;  // AV_LOG_INFO is ffmpeg default
  else params.logLevel = VVDEC_SILENT;

  // set desired decoding options

  // threading
  if( avctx->thread_count > 0 )
  {
    params.threads = avctx->thread_count * 4;  // number of worker threads (should not exceed the number of physical cpu's)
  }
  else
  {
    params.threads = -1; // get max cpu´s
  }

  ff_vvdec_printParameterInfo( avctx, &params );
  s->vvdecDec = vvdec_decoder_open( &params );
  if( !s->vvdecDec )
  {
    VVDEC_LOG_ERROR( "cannot init hhi vvc decoder\n" );
    return -1;
  }

  vvdec_set_logging_callback( s->vvdecDec, ff_vvdec_log_callback );

  return 0;
}

static av_cold int ff_vvdec_decode_close(AVCodecContext *avctx)
{
  VVdeCContext *s = (VVdeCContext*)avctx->priv_data;

  if( 0 != vvdec_decoder_close(s->vvdecDec) )
  {
    av_log(avctx, AV_LOG_ERROR, "cannot close vvdec\n" );
    return -1;
  }

  return 0;
}


static av_cold int ff_vvdec_decode_frame( AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt )
{
  VVdeCContext *s = (VVdeCContext*)avctx->priv_data;

  AVFrame *pcAVFrame      = (AVFrame*)data;

  int iRet = 0;
  vvdecFrame *frame = NULL;

  if ( pcAVFrame )
  {
    if( !avpkt->size && !s->bFlush )
    {
      s->bFlush = true;
    }

    if( s->bFlush )
    {
      iRet = vvdec_flush( s->vvdecDec, &frame );
    }
    else
    {
      vvdecAccessUnit accessUnit;
      vvdec_accessUnit_default( &accessUnit );
      accessUnit.payload         = avpkt->data;
      accessUnit.payloadSize     = avpkt->size;
      accessUnit.payloadUsedSize = avpkt->size;

      accessUnit.cts = avpkt->pts; accessUnit.ctsValid = true;
      accessUnit.dts = avpkt->pts; accessUnit.dtsValid = true;

      iRet = vvdec_decode( s->vvdecDec, &accessUnit, &frame );
    }

    if( iRet < 0 )
    {
      if( iRet == VVDEC_TRY_AGAIN )
      {
        VVDEC_LOG_DBG( "vvdec::decode - more input data needed" );
      }
      else if( iRet == VVDEC_EOF )
      {
        s->bFlush = true;
        VVDEC_LOG_VERBOSE( "vvdec::decode - eof reached" );
      }
      else
      {
        VVDEC_LOG_ERROR( "error in vvdec::decode - ret:%d - %s\n", iRet, vvdec_get_last_error(s->vvdecDec) );
        return -1;
      }
    }
    else
    {
      if( NULL != frame )
      {
        #if 1
        if( frame->picAttributes )
        {
          const static char acST[3] = { 'I', 'P', 'B' };
          char c = acST[frame->picAttributes->sliceType];
          if( !frame->picAttributes->isRefPic ) c += 32;

          VVDEC_LOG_DBG( "vvdec_decode_frame SEQ %" PRId64 " TId: %d  %c-SLICE flush %d\n", frame->sequenceNumber,
              frame->picAttributes->temporalLayer, c, s->bFlush );
        }
        else
        {
          VVDEC_LOG_DBG( "vvdec_decode_frame SEQ %" PRId64 "\n", frame->sequenceNumber );
        }
        #endif

        if (( iRet = ff_vvdec_set_pix_fmt(avctx, frame)) < 0)
        {
            av_log(avctx, AV_LOG_ERROR, "Unsupported output colorspace (%d) / bit_depth (%d)\n",
                frame->colorFormat, frame->bitDepth);
            return iRet;
        }

        if( avctx->pix_fmt != AV_PIX_FMT_YUV420P && avctx->pix_fmt != AV_PIX_FMT_YUV420P10LE )
        {
          av_log(avctx, AV_LOG_ERROR, "Unsupported output colorspace (%d) / bit_depth (%d)\n",
              frame->colorFormat, frame->bitDepth );
          return iRet;
        }

        if ((int)frame->width != avctx->width || (int)frame->height != avctx->height)
        {
            av_log(avctx, AV_LOG_INFO, "dimension change! %dx%d -> %dx%d\n",
                   avctx->width, avctx->height, frame->width, frame->height);

            avctx->coded_width  = frame->width;
            avctx->coded_height = frame->height;
            avctx->width        = AV_CEIL_RSHIFT(frame->width,  avctx->lowres);
            avctx->height       = AV_CEIL_RSHIFT(frame->height, avctx->lowres);
        }

        pcAVFrame->width  = frame->width;
        pcAVFrame->height = frame->height;
        pcAVFrame->format = avctx->pix_fmt;
        pcAVFrame->interlaced_frame = 0;
        pcAVFrame->top_field_first  = 0;
        if (frame->ctsValid)
          pcAVFrame->pts = frame->cts;

        iRet = av_frame_get_buffer( pcAVFrame, 32 );
        if( iRet < 0 )
        {
          av_log(avctx, AV_LOG_ERROR, "Could not allocate the video frame data\n");
          return iRet;
        }

        /* make sure the frame data is writable */
        iRet = av_frame_make_writable( pcAVFrame );
        if( iRet < 0 )
        {
          av_log(avctx, AV_LOG_ERROR, "Could not make frame writable\n");
          return iRet;
        }

#if LIBAVCODEC_VERSION_MAJOR >= 58

        const uint8_t * src_data[4]      = { frame->planes[0].ptr, frame->planes[1].ptr, frame->planes[2].ptr, NULL };
        const int       src_linesizes[4] = { (int)frame->planes[0].stride, (int)frame->planes[1].stride, (int)frame->planes[2].stride, 0 };

        av_image_copy(pcAVFrame->data, pcAVFrame->linesize, src_data,
                      src_linesizes, avctx->pix_fmt, frame->width, frame->height );
#endif

        if( 0 != vvdec_frame_unref( s->vvdecDec, frame ) )
        {
          av_log(avctx, AV_LOG_ERROR, "cannot free picture memory\n");
        }

        *got_frame = 1;
      }
    }
  }

  return avpkt->size;
}


static const enum AVPixelFormat pix_fmts_vvc[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUV420P10LE,
    AV_PIX_FMT_NONE
};

#define OFFSET(x) offsetof(VVdeCContext, x)
#define VVDEC_FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption libvvdec_options[] = {
    {"upscaling", "RPR upscaling mode", OFFSET(options.upscaling_mode), AV_OPT_TYPE_INT, {.i64 = 0}, -1, 1, VVDEC_FLAGS, "upscaling_mode"},
        {"auto",     "Selected by the Decoder", 0, AV_OPT_TYPE_CONST, {.i64 = -1 }, INT_MIN, INT_MAX, VVDEC_FLAGS, "upscaling_mode"},
        {"off",   "Disable", 0, AV_OPT_TYPE_CONST, {.i64 =  0 }, INT_MIN, INT_MAX, VVDEC_FLAGS, "upscaling_mode"},
        {"on", "on", 0, AV_OPT_TYPE_CONST, {.i64 =  1 }, INT_MIN, INT_MAX, VVDEC_FLAGS, "upscaling_mode"},
    {NULL}
};

static const AVClass libvvdec_class = {
    "VVC decoder",
    av_default_item_name,
    libvvdec_options,
    LIBAVUTIL_VERSION_INT,
};

AVCodec ff_libvvdec_decoder = {
  .name            = "libvvdec",
  .long_name       = "H.266 / VVC Decoder VVdeC",
  .type            = AVMEDIA_TYPE_VIDEO,
  .id              = AV_CODEC_ID_VVC,
  .priv_data_size  = sizeof(VVdeCContext),
  .init            = ff_vvdec_decode_init,
  .decode          = ff_vvdec_decode_frame,
  .close           = ff_vvdec_decode_close,
  .capabilities    = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS,
  .bsfs            = "vvc_mp4toannexb",
  .caps_internal   = (1 << 7),
  .pix_fmts        = pix_fmts_vvc,
  .priv_class      = &libvvdec_class,
  .wrapper_name    = "libvvdec",

};


#ifdef __cplusplus
};
#endif

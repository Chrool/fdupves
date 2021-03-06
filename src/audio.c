/*
 * This file is part of the fdupves package
 * Copyright (C) <2008> Alf
 *
 * Contact: Alf <naihe2010@126.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */
/* @CFILE audio.c
 *
 *  Author: Alf <naihe2010@126.com>
 */

#include "audio.h"
#include "util.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include <glib.h>

audio_info *
audio_get_info (const char *file)
{
  audio_info *info;
  AVFormatContext *fmt_ctx = NULL;
  AVStream *stream = NULL;
  int s, ret;

  ret = avformat_open_input (&fmt_ctx, file, NULL, NULL);
  if (ret != 0)
    {
      g_warning (_ ("could not open: %s"), file);
      return NULL;
    }

  if (avformat_find_stream_info (fmt_ctx, NULL) < 0)
    {
      g_warning (_ ("could not find stream infomations: %s"), file);
      avformat_close_input (&fmt_ctx);
      return NULL;
    }

  s = av_find_best_stream (fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (s < 0)
    {
      g_warning (_ ("could not find audio stream: %s"), file);
      avformat_close_input (&fmt_ctx);
      return NULL;
    }

  stream = fmt_ctx->streams[s];

  info = g_malloc0 (sizeof (audio_info));

  info->name = g_path_get_basename (file);
  info->dir = g_path_get_dirname (file);
  if (stream->duration != AV_NOPTS_VALUE)
    {
      info->length = (double) (stream->duration * stream->time_base.num) / stream->time_base.den;
    }
  else
    {
      info->length = (double) (fmt_ctx->duration) / AV_TIME_BASE;
    }
  info->size[0] = stream->codecpar->width;
  info->size[1] = stream->codecpar->height;
  info->format = avcodec_get_name (stream->codecpar->codec_id);

  avformat_close_input (&fmt_ctx);

  return info;
}

void
audio_info_free (audio_info *info)
{
  g_free (info->name);
  g_free (info->dir);
  g_free (info);
}

int
audio_get_length (const char *file)
{
  audio_info *info;
  int length;

  length = 0;
  info = audio_get_info (file);
  if (info)
    {
      length = (int) info->length;
      audio_info_free (info);
    }

  return length;
}

int
audio_time_screenshot (const char *file, float offset,
		       int samples,
		       short *buffer, int buf_len)
{
  AVFormatContext *format_ctx = NULL;
  AVCodecContext *codec_ctx = NULL;
  AVCodec *codec = NULL;
  AVFrame *frame = NULL, *frame_s16 = NULL;
  AVPacket *packet = NULL;
  struct SwrContext *convert_ctx = NULL;
  int s, ret, bytes = -1, want_samples, got_samples;
  int64_t seek_target;

  g_assert (samples * sizeof (short) <= (size_t) buf_len);

  if (avformat_open_input (&format_ctx, file, NULL, NULL) != 0)
    {
      g_warning (_ ("could not open: %s"), file);
      goto end;
    }

  if (avformat_find_stream_info (format_ctx, NULL) < 0)
    {
      g_warning (_ ("could not find stream infomations: %s"), file);
      goto end;
    }

  s = av_find_best_stream (format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (s < 0)
    {
      g_warning (_ ("could not find audio stream: %s"), file);
      goto end;
    }

  codec_ctx = avcodec_alloc_context3 (NULL);
  if (codec_ctx == NULL)
    {
      g_warning (_ ("Memory error: %s"), file);
      goto end;
    }

  ret = avcodec_parameters_to_context (codec_ctx, format_ctx->streams[s]->codecpar);
  if (ret < 0)
    {
      g_warning (_ ("Memory error: %s"), file);
      goto end;
    }

  av_codec_set_pkt_timebase (codec_ctx, format_ctx->streams[s]->time_base);
  codec = avcodec_find_decoder (codec_ctx->codec_id);
  if (codec == NULL)
    {
      g_warning (_ ("Unsupported codec: %s"), file);
      goto end;
    }

  if (avcodec_open2 (codec_ctx, codec, NULL) < 0)
    {
      g_warning (_ ("Open codec error: %s"), file);
      goto end;
    }

  frame = av_frame_alloc ();
  frame_s16 = av_frame_alloc ();
  if (frame == NULL
      || frame_s16 == NULL)
    {
      g_warning (_ ("alloc frame error: %s"), file);
      goto end;
    }

  seek_target = av_rescale (offset,
			    format_ctx->streams[s]->time_base.den,
			    format_ctx->streams[s]->time_base.num);
  avformat_seek_file (format_ctx, s,
		      0, seek_target, seek_target,
		      AVSEEK_FLAG_FRAME);

  packet = av_packet_alloc ();
  if (packet == NULL)
    {
      g_warning (_ ("alloc packet error: %s"), file);
      goto end;
    }

  convert_ctx = swr_alloc_set_opts (
                                    NULL,
                                    AV_CH_LAYOUT_MONO,
                                    AV_SAMPLE_FMT_S16,
                                    AUDIO_HASH_RATE,
                                    codec_ctx->channel_layout ? codec_ctx->channel_layout: av_get_default_channel_layout (codec_ctx->channels),
                                    codec_ctx->sample_fmt,
                                    codec_ctx->sample_rate,
                                    0,
                                    NULL);
  if (convert_ctx == NULL)
    {
      g_warning ( _ ("Could not allocate resampler context\n"));
      goto end;
    }

  /* initialize the resampling context */
  if ((ret = swr_init (convert_ctx)) < 0)
    {
      g_warning ( _ ("Could not initialize resampler context\n"));
      goto end;
    }

  got_samples = 0;
  while (av_read_frame (format_ctx, packet) == 0)
    {
      if (packet->stream_index != s)
	{
          av_packet_unref (packet);
          continue;
	}

      if (avcodec_send_packet (codec_ctx, packet) != 0)
        {
          av_packet_unref (packet);
          continue;
        }

      av_packet_unref (packet);

      ret = avcodec_receive_frame (codec_ctx, frame);
      if (ret == AVERROR(EAGAIN))
        {
          continue;
        }

      if (ret != 0)
        {
          g_warning (_ ("Cannot receive frame from context"));
          bytes = -1;
          goto end;
        }

      want_samples = av_rescale_rnd
        (swr_get_delay (convert_ctx, codec_ctx->sample_rate)
         + frame->nb_samples, AUDIO_HASH_RATE, codec_ctx->sample_rate, AV_ROUND_UP);
      if (got_samples + want_samples > samples)
        {
          want_samples = samples - got_samples;
        }

      bytes = av_samples_fill_arrays (frame_s16->data, frame_s16->linesize,
                                      (uint8_t *) buffer + got_samples * sizeof (short),
                                      1, want_samples,
                                      AV_SAMPLE_FMT_S16, 1);
      if (bytes < 0)
        {
          g_warning ( _ ("Could not fill resampler buffer\n"));
          bytes = -1;
          goto end;
        }

      if ((ret = swr_convert (convert_ctx,
                              frame_s16->data, want_samples,
                              (const uint8_t **)frame->data, frame->nb_samples)) < 0)
        {
          g_warning ( _ ("Could not resample samples.\n"));
          bytes = -1;
          goto end;
        }

      got_samples += ret;
      if (got_samples == samples)
        {
          bytes = sizeof (short) * samples;
          break;
        }
    }

 end:
  if (convert_ctx)
    {
      swr_free (&convert_ctx);
    }
  if (packet)
    {
      av_packet_free (&packet);
    }
  if (frame_s16)
    {
      av_frame_free (&frame_s16);
    }
  if (frame)
    {
      av_frame_free (&frame);
    }

  if (codec_ctx)
    {
      avcodec_free_context (&codec_ctx);
    }

  if (format_ctx)
    {
      avformat_close_input (&format_ctx);
    }

  return bytes;
}

int
audio_time_screenshot_file (const char *file, float offset,
			    int samples,
			    const char *out_file)
{
  FILE *fp;
  short *buf;
  int len;

  buf = g_malloc (samples * sizeof (short));
  g_return_val_if_fail (buf, -1);

  len = audio_time_screenshot (file, offset, samples, buf, samples * sizeof (short));
  if (len <= 0)
    {
      g_free (buf);
      return -1;
    }

  fp = fopen (out_file, "wb");
  if (fp == NULL)
    {
      g_free (buf);
      g_warning ("open %s for write error: %s", out_file, strerror (errno));
      return -1;
    }

  fwrite (buf, sizeof (short), samples, fp);
  fclose (fp);

  g_free (buf);

  return 0;
}

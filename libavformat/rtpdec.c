/*
 * RTP AMR Depacketizer, RFC 3267
 * Copyright (c) 2010 Martin Storsjo
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

#include "libavutil/channel_layout.h"
#include "avformat.h"
#include "rtpdec_formats.h"
#include "libavutil/avstring.h"

static const uint8_t frame_sizes_nb[16] = {
    12, 13, 15, 17, 19, 20, 26, 31, 5, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t frame_sizes_wb[16] = {
    17, 23, 32, 36, 40, 46, 50, 58, 60, 5, 5, 0, 0, 0, 0, 0
};

// Get bits
static const uint16_t frame_sizes_nb_bandwidth_efficient[16] = {
    95, 103, 118, 134, 148, 159, 204, 244, 39, 0, 0, 0, 0, 0, 0, 0
};
static const uint16_t frame_sizes_wb_bandwidth_efficient[16] = {
    132, 177, 253, 285, 317, 365, 397, 461, 477, 40, 40, 0, 0, 0, 0, 0
};

// Get bits zero
static const uint16_t frame_sizes_nb_add_bandwidth_efficient[16] = {
    1, 1, 2, 2, 4, 1, 4, 4, 1, 0, 0, 0, 0, 0, 0, 0
};
static const uint16_t frame_sizes_wb_add_bandwidth_efficient[16] = {
    4, 7, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0
};

struct PayloadContext {
    int octet_align;
    int crc;
    int interleaving;
    int channels;
};


static uint8_t removeLowZeros(uint8_t input) {
    if (input == 0x00) {
        return 0x00;
    }

    uint8_t mask = 0x01;
    while ((input & mask) == 0) {
        input >>= 1;
    }

    return input;
}

static av_cold int amr_init(AVFormatContext *s, int st_index, PayloadContext *data)
{
    data->channels = 1;
    return 0;
}

static int amr_handle_packet_octet_aligned(AVFormatContext *ctx, PayloadContext *data,
// static int amr_handle_packet(AVFormatContext *ctx, PayloadContext *data,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    const uint16_t *frame_sizes = NULL;
    const uint16_t *frame_add_sizes = NULL;
    int ret;
    uint8_t *ptr;

    if (st->codecpar->codec_id == AV_CODEC_ID_AMR_NB) {
        frame_sizes = frame_sizes_nb_bandwidth_efficient;
        frame_add_sizes = frame_sizes_nb_add_bandwidth_efficient;
    } else if (st->codecpar->codec_id == AV_CODEC_ID_AMR_WB) {
        frame_sizes = frame_sizes_wb_bandwidth_efficient;
        frame_add_sizes = frame_sizes_wb_add_bandwidth_efficient;
    } else {
        av_log(ctx, AV_LOG_ERROR, "Bad codec ID\n");
        return AVERROR_INVALIDDATA;
    }

    if (st->codecpar->ch_layout.nb_channels != 1) {
        av_log(ctx, AV_LOG_ERROR, "Only mono AMR is supported\n");
        return AVERROR_INVALIDDATA;
    }
    av_channel_layout_default(&st->codecpar->ch_layout, 1);

    /* The AMR RTP packet consists of one header byte, followed
     * by one TOC byte for each AMR frame in the packet, followed
     * by the speech data for all the AMR frames.
     *
     * The header byte contains only a codec mode request, for
     * requesting what kind of AMR data the sender wants to
     * receive. Not used at the moment.
     */

    /* Count the number of frames in the packet. The highest bit
     * is set in a TOC byte if there are more frames following.
     */

    // Extract header value (first 4 bits)

    uint8_t toc_item_array[1024];
    uint8_t toc_item_count = 0;
    uint8_t toc_item_threshold = 8;
    uint8_t toc_item = 0x00;
    uint8_t end_tag = 1;
    uint8_t cur_bit = 0x80;
    int buf_index = 1;
    for (; buf_index < len && buf_index < 1024 && (toc_item_threshold != 8 || end_tag); cur_bit >>= 1) {
        // lowest bit, move to next buf's hignest bit
        if (cur_bit == 0x00) {
            cur_bit = 0x80;
            buf_index++;
        }
        
        uint8_t toc_item_bit = cur_bit & buf[buf_index];
//        av_log(ctx, AV_LOG_ERROR, "in loop:  buf[buf_index]=%x toc_item_bit=%x buf_index=%d len=%d toc_item_threshold=%x cur_bit=%x, end_tag=%x\n",  buf[buf_index], toc_item_bit, buf_index, len, toc_item_threshold, cur_bit, end_tag);
        if (toc_item_threshold == 8) {
            // get F
            end_tag = removeLowZeros(toc_item_bit);
            // calculate FT
            toc_item <<= 1;
            toc_item += end_tag;
        } else if (toc_item_threshold >= 1 && toc_item_threshold <= 7) {
            // calculate FT
            toc_item <<= 1;
            toc_item += removeLowZeros(toc_item_bit);
        } else {
            av_log(ctx, AV_LOG_ERROR, "invalid toc_item_threshold\n");
            return AVERROR_INVALIDDATA;
        }

        toc_item_threshold -= 1;
        // per six bits, is a toc item
        if (toc_item_threshold == 0) {
        //    toc_item = toc_item << 2;
           toc_item_array[toc_item_count] = toc_item;
           toc_item_count += 1;
           toc_item_threshold = 8;
           toc_item = 0x00;
        }
    }

    if (end_tag || buf_index > len) {
        // 如果没有终止，或者超过限度，有问题
        av_log(ctx, AV_LOG_ERROR, "invalid end_tag or buf_index\n");
        return AVERROR_INVALIDDATA;
    }

    /* Everything except the codec mode request byte should be output. */
    if ((ret = av_new_packet(pkt, len - 1)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Out of memory\n");
        return ret;
    }
    pkt->stream_index = st->index;
    ptr = pkt->data;

    uint8_t audio_data = 0x00;
    uint8_t audio_data_index = 8;
    for (int toc_index = 0; toc_index < toc_item_count; toc_index++) {
//        av_log(ctx, AV_LOG_ERROR, "toc_item_size: %d, toc_item: %x\n", toc_item_count, toc_item_array[toc_index]);
        int byte_count = 0;
        int zero_count = 0;
        int real_byte_count = 0;
        int real_zero_count = 0;
        uint8_t toc_item_mode = toc_item_array[toc_index] & 0x7C;
        *ptr++ = toc_item_mode;
        real_byte_count += 1;
        uint8_t toc_item_ft = toc_item_mode >> 3;
        av_log(ctx, AV_LOG_ERROR, "new toc_item_size: %d, toc_item: %x, toc_item_ft: %x\n", toc_item_count, toc_item_array[toc_index], toc_item_ft);
        av_log(ctx, AV_LOG_ERROR, "read in toc: %d\n", toc_item_mode);
//        av_log(ctx, AV_LOG_ERROR, "toc_item_mode=%x, toc_item_ft=%x, frame_size:%d, buf_index=%d, len=%d\n", toc_item_mode, toc_item_ft, frame_sizes[toc_item_ft], buf_index, len);
        for (int frame_size_index = 0; buf_index < len && frame_size_index < frame_sizes[toc_item_ft]; frame_size_index++, cur_bit >>= 1) {
            byte_count += 1;
//            av_log(ctx, AV_LOG_ERROR, "read byte: buf_index=%d, index=%d, size=%d\n", buf_index, frame_size_index, frame_sizes[toc_item_ft]);
            if (cur_bit == 0x00) {
                cur_bit = 0x80;
                buf_index++;
            } 
            uint8_t bit_value = removeLowZeros(cur_bit & buf[buf_index]);
            audio_data <<= 1;
            audio_data += removeLowZeros(bit_value); 
            audio_data_index -= 1;
            if (audio_data_index <= 0) {
                av_log(ctx, AV_LOG_ERROR, "%02X ", audio_data);
                real_byte_count += 1;
                *ptr++ = audio_data;
                audio_data = 0x00;
                audio_data_index = 8;
            }
        } 


        for (int zero_frame_size_index = 0; zero_frame_size_index < frame_add_sizes[toc_item_ft]; zero_frame_size_index++, cur_bit >>= 1) {
            zero_count += 1;
//            av_log(ctx, AV_LOG_ERROR, "read zero: zero_frame_size_index=%d, size=%d\n", zero_frame_size_index, frame_add_sizes[toc_item_ft]);
            // lowest bit, move to next buf's hignest bit
            if (cur_bit == 0x00) {
                cur_bit = 0x80;
                buf_index++;
            } 
            uint8_t bit_value = 0x00;
            audio_data <<= 1;
            audio_data += removeLowZeros(bit_value); 
            audio_data_index -= 1;
            if (audio_data_index <= 0) {
                av_log(ctx, AV_LOG_ERROR, "%02X ", audio_data);
                real_zero_count += 1;
                *ptr++ = audio_data;
                audio_data = 0x00;
                audio_data_index = 8;
            }
        } 
        av_log(ctx, AV_LOG_ERROR, "\n#####################################################\n");
//        av_log(ctx, AV_LOG_ERROR, "read all byte=%d, zero=%d, real byte=%d, real zero=%d\n", byte_count, zero_count, real_byte_count, real_zero_count);
    }
    return 0;
}

static int amr_handle_packet_bandwidth_efficient(AVFormatContext *ctx, PayloadContext *data,
// static int amr_handle_packet_bandwidth_efficient(AVFormatContext *ctx, PayloadContext *data,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    const uint16_t *frame_sizes = NULL;
    const uint16_t *frame_add_sizes = NULL;
    int ret;
    uint8_t *ptr;

    if (st->codecpar->codec_id == AV_CODEC_ID_AMR_NB) {
        frame_sizes = frame_sizes_nb_bandwidth_efficient;
        frame_add_sizes = frame_sizes_nb_add_bandwidth_efficient;
    } else if (st->codecpar->codec_id == AV_CODEC_ID_AMR_WB) {
        frame_sizes = frame_sizes_wb_bandwidth_efficient;
        frame_add_sizes = frame_sizes_wb_add_bandwidth_efficient;
    } else {
        av_log(ctx, AV_LOG_ERROR, "Bad codec ID\n");
        return AVERROR_INVALIDDATA;
    }

    if (st->codecpar->ch_layout.nb_channels != 1) {
        av_log(ctx, AV_LOG_ERROR, "Only mono AMR is supported\n");
        return AVERROR_INVALIDDATA;
    }
    av_channel_layout_default(&st->codecpar->ch_layout, 1);

    /* The AMR RTP packet consists of one header byte, followed
     * by one TOC byte for each AMR frame in the packet, followed
     * by the speech data for all the AMR frames.
     *
     * The header byte contains only a codec mode request, for
     * requesting what kind of AMR data the sender wants to
     * receive. Not used at the moment.
     */

    /* Count the number of frames in the packet. The highest bit
     * is set in a TOC byte if there are more frames following.
     */

    // Extract header value (first 4 bits)

    uint8_t toc_item_array[1024];
    uint8_t toc_item_count = 0;
    uint8_t toc_item_threshold = 6;
    uint8_t toc_item = 0x00;
    uint8_t end_tag = 1;
    uint8_t cur_bit = 0x08;
    uint8_t sid_count = 0;
    int buf_index = 0;
    for (; buf_index < len && buf_index < 1024 && (toc_item_threshold != 6 || end_tag); cur_bit >>= 1) {
        // lowest bit, move to next buf's hignest bit
        if (cur_bit == 0x00) {
            cur_bit = 0x80;
            buf_index++;
        }
        
        uint8_t toc_item_bit = cur_bit & buf[buf_index];
//        av_log(ctx, AV_LOG_ERROR, "in loop:  buf[buf_index]=%x toc_item_bit=%x buf_index=%d len=%d toc_item_threshold=%x cur_bit=%x, end_tag=%x\n",  buf[buf_index], toc_item_bit, buf_index, len, toc_item_threshold, cur_bit, end_tag);
        if (toc_item_threshold == 6) {
            // get F
            end_tag = removeLowZeros(toc_item_bit);
            // calculate FT
            toc_item <<= 1;
            toc_item += end_tag;
        } else if (toc_item_threshold >= 1 && toc_item_threshold <= 5) {
            // calculate FT
            toc_item <<= 1;
            toc_item += removeLowZeros(toc_item_bit);
        } else {
            av_log(ctx, AV_LOG_ERROR, "invalid toc_item_threshold\n");
            return AVERROR_INVALIDDATA;
        }

        toc_item_threshold -= 1;
        // per six bits, is a toc item
        if (toc_item_threshold == 0) {
            toc_item = toc_item << 2;
           
            uint8_t toc_item_ft = (toc_item & 0x7C) >> 3;
            av_log(ctx, AV_LOG_ERROR, "this toc_item: %x, toc_item_ft: %x\n", toc_item, toc_item_ft); 
            if (toc_item_ft == 9) {
                sid_count += 1;            
            }

            toc_item_array[toc_item_count] = toc_item;
            toc_item_count += 1;
            toc_item_threshold = 6;
            toc_item = 0x00;
        }
    }

    if (end_tag || buf_index > len) {
        // 如果没有终止，或者超过限度，有问题
        av_log(ctx, AV_LOG_ERROR, "invalid end_tag or buf_index\n");
        return AVERROR_INVALIDDATA;
    }

    // TODO: 这里需要重新计算len
    int add_len = 55 * sid_count - 1;
    int len_after_replace_sid = len + (add_len > 0 ? add_len : 0);
    av_log(ctx, AV_LOG_ERROR, "toc_item_size: %d, sid_count: %d, ori len: %d, cur len: %d\n", toc_item_count, sid_count, len, len_after_replace_sid);
    /* Everything except the codec mode request byte should be output. */
    if ((ret = av_new_packet(pkt, len_after_replace_sid)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Out of memory\n");
        return ret;
    }
    pkt->stream_index = st->index;
    ptr = pkt->data;

    uint8_t audio_data = 0x00;
    uint8_t audio_data_index = 8;
    for (int toc_index = 0; toc_index < toc_item_count; toc_index++) {
//        av_log(ctx, AV_LOG_ERROR, "toc_item_size: %d, toc_item: %x\n", toc_item_count, toc_item_array[toc_index]);
        int byte_count = 0;
        int zero_count = 0;
        int real_byte_count = 0;
        int real_zero_count = 0;
        uint8_t toc_item_mode = toc_item_array[toc_index] & 0x7C;
        real_byte_count += 1;
        uint8_t toc_item_ft = toc_item_mode >> 3;
        av_log(ctx, AV_LOG_ERROR, "new toc_item_size: %d, toc_item: %x, toc_item_ft: %x\n", toc_item_count, toc_item_array[toc_index], toc_item_ft);
        av_log(ctx, AV_LOG_ERROR, "read in toc: %d\n", toc_item_mode);
        if (toc_item_ft != 9) {
            *ptr++ = toc_item_mode; 
        }
//        av_log(ctx, AV_LOG_ERROR, "toc_item_mode=%x, toc_item_ft=%x, frame_size:%d, buf_index=%d, len=%d\n", toc_item_mode, toc_item_ft, frame_sizes[toc_item_ft], buf_index, len);
        for (int frame_size_index = 0; buf_index < len && frame_size_index < frame_sizes[toc_item_ft]; frame_size_index++, cur_bit >>= 1) {
            byte_count += 1;
//            av_log(ctx, AV_LOG_ERROR, "read byte: buf_index=%d, index=%d, size=%d\n", buf_index, frame_size_index, frame_sizes[toc_item_ft]);
            if (cur_bit == 0x00) {
                cur_bit = 0x80;
                buf_index++;
            } 
            uint8_t bit_value = removeLowZeros(cur_bit & buf[buf_index]);
            audio_data <<= 1;
            audio_data += removeLowZeros(bit_value); 
            audio_data_index -= 1;
            if (audio_data_index <= 0) {
                av_log(ctx, AV_LOG_ERROR, "%02X ", audio_data);
                real_byte_count += 1;
                if (toc_item_ft != 9) {
                    *ptr++ = audio_data;
                }
                audio_data = 0x00;
                audio_data_index = 8;
            }
        } 


        for (int zero_frame_size_index = 0; zero_frame_size_index < frame_add_sizes[toc_item_ft]; zero_frame_size_index++, cur_bit >>= 1) {
            zero_count += 1;
//            av_log(ctx, AV_LOG_ERROR, "read zero: zero_frame_size_index=%d, size=%d\n", zero_frame_size_index, frame_add_sizes[toc_item_ft]);
            // lowest bit, move to next buf's hignest bit
            if (cur_bit == 0x00) {
                cur_bit = 0x80;
                buf_index++;
            } 
            uint8_t bit_value = 0x00;
            audio_data <<= 1;
            audio_data += removeLowZeros(bit_value); 
            audio_data_index -= 1;
            if (audio_data_index <= 0) {
                av_log(ctx, AV_LOG_ERROR, "%02X ", audio_data);
                real_zero_count += 1;
                if (toc_item_ft != 9) {
                    *ptr++ = audio_data;
                }
                audio_data = 0x00;
                audio_data_index = 8;
            }
        } 
        if (toc_item_ft == 9) {
            *ptr++ = 0x44;
            av_log(ctx, AV_LOG_ERROR, "\naccounter SID frame, trying replace. current ft: %x, mode: %x\n", toc_item_ft, toc_item_mode-8);
            for (int f_index=0; f_index < 60; f_index++) {
                *ptr++ = 0x00;
            }
        }
        av_log(ctx, AV_LOG_ERROR, "\n#####################################################\n");
//        av_log(ctx, AV_LOG_ERROR, "read all byte=%d, zero=%d, real byte=%d, real zero=%d\n", byte_count, zero_count, real_byte_count, real_zero_count);
    }
    return 0;
}

static int amr_handle_packet(AVFormatContext *ctx, PayloadContext *data,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    if (data->octet_align) {
        av_log(ctx, AV_LOG_WARNING, "going to rtpdec_amr.c amr_handle_packet_octet_aligned\n");
        return amr_handle_packet_octet_aligned(ctx, data, st, pkt, timestamp, buf, len, seq, flags);
    } else {
        av_log(ctx, AV_LOG_WARNING, "going to rtpdec_amr.c amr_handle_packet_bandwidth_efficient\n");
        return amr_handle_packet_bandwidth_efficient(ctx, data, st, pkt, timestamp, buf, len, seq, flags);
    }
}

static int amr_parse_fmtp(AVFormatContext *s,
                          AVStream *stream, PayloadContext *data,
                          const char *attr, const char *value)
{
    /* Some AMR SDP configurations contain "octet-align", without
     * the trailing =1. Therefore, if the value is empty,
     * interpret it as "1".
     */
    if (!strcmp(value, "")) {
        av_log(s, AV_LOG_WARNING, "AMR fmtp attribute %s had "
                                  "nonstandard empty value\n", attr);
        value = "1";
    }
    if (!strcmp(attr, "octet-align"))
        data->octet_align = atoi(value);
    else if (!strcmp(attr, "crc"))
        data->crc = atoi(value);
    else if (!strcmp(attr, "interleaving"))
        data->interleaving = atoi(value);
    else if (!strcmp(attr, "channels"))
        data->channels = atoi(value);
    return 0;
}

static int amr_parse_sdp_line(AVFormatContext *s, int st_index,
                              PayloadContext *data, const char *line)
{
    const char *p;
    int ret;

    if (st_index < 0)
        return 0;

    /* Parse an fmtp line this one:
     * a=fmtp:97 octet-align=1; interleaving=0
     * That is, a normal fmtp: line followed by semicolon & space
     * separated key/value pairs.
     */
    if (av_strstart(line, "fmtp:", &p)) {
        ret = ff_parse_fmtp(s, s->streams[st_index], data, p, amr_parse_fmtp);
        if (data->crc ||
            data->interleaving || data->channels != 1) {
            av_log(s, AV_LOG_ERROR, "Unsupported RTP/AMR configuration!\n");
            return -1;
        }
        return ret;
    }
    return 0;
}

const RTPDynamicProtocolHandler ff_amr_nb_dynamic_handler = {
    .enc_name         = "AMR",
    .codec_type       = AVMEDIA_TYPE_AUDIO,
    .codec_id         = AV_CODEC_ID_AMR_NB,
    .priv_data_size   = sizeof(PayloadContext),
    .init             = amr_init,
    .parse_sdp_a_line = amr_parse_sdp_line,
    .parse_packet     = amr_handle_packet,
};

const RTPDynamicProtocolHandler ff_amr_wb_dynamic_handler = {
    .enc_name         = "AMR-WB",
    .codec_type       = AVMEDIA_TYPE_AUDIO,
    .codec_id         = AV_CODEC_ID_AMR_WB,
    .priv_data_size   = sizeof(PayloadContext),
    .init             = amr_init,
    .parse_sdp_a_line = amr_parse_sdp_line,
    .parse_packet     = amr_handle_packet,
};

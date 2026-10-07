/*****************************************************************************
  Copyright (C) 2018-2026 John William (Will)

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111, USA.

  This program is also available with customization/support packages.
  For more information, please contact me at cannonbeachgoonie@gmail.com

******************************************************************************/
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <string.h>
#include <errno.h>
#include "srt.h"
#include "mempool.h"
#include "srthub.h"
#include "udpsource.h"
#include "tsdecode.h"
#include "dataqueue.h"
#include "esignal.h"
#include "cJSON.h"

#define SRTHUB_MAJOR               1
#define SRTHUB_MINOR               0

#define MAX_CONFIG_SIZE            16384
#define MAX_UDP_BUFFER_READ        2048
#define MAX_SRT_PACKET_SIZE        ((188*7)+12)

#define MESSAGE_TYPE_START         0x01
#define MESSAGE_TYPE_STOP          0x02
#define MESSAGE_TYPE_RESTART       0x99

#define MAX_MSG_BUFFERS            8192
#define MAX_PACKET_BUFFERS         16384
#define MAX_PACKET_BUFFER_SIZE     1536
#define MAX_THUMBNAIL_BUFFERS      16
#define MAX_THUMBNAIL_BUFFER_SIZE  1024*1024*4
#define MAX_AUDIO_BUFFERS          128
#define MAX_AUDIO_BUFFER_SIZE      32768

#define ENABLE_THUMBNAIL

#if defined(ENABLE_THUMBNAIL)
#define THUMBNAIL_WIDTH            320
#define THUMBNAIL_HEIGHT           240
#define MAX_DECODE_WIDTH           3840
#define MAX_DECODE_HEIGHT          2160
/* A new video format has to hold for this many consecutive decoded frames
 * before it is reported as a change, so that a single corrupt sequence header
 * or SPS does not raise a spurious event.
 *
 * This counts frames the thumbnail thread actually decodes, which is at most
 * one per second and only ever a frame carrying a sequence header or SPS - so
 * each sample is a fresh signalling of the format rather than a stale one, and
 * a confirmed change reaches the event log a second or two after it happens.
 * That is the right trade for an event an operator reads, and it is why this
 * is deliberately small. */
#define VIDEO_FORMAT_CHANGE_FRAMES 2

#include "../cbffmpeg/libavcodec/avcodec.h"
#include "../cbffmpeg/libswscale/swscale.h"
#include "../cbffmpeg/libavutil/pixfmt.h"
#include "../cbffmpeg/libavutil/rational.h"
#include "../cbffmpeg/libavutil/log.h"
#include "../cbffmpeg/libavutil/opt.h"
#include "../cbffmpeg/libavutil/imgutils.h"
#include "../cbffmpeg/libavformat/avformat.h"
#include "../cbffmpeg/libavfilter/buffersink.h"
#include "../cbffmpeg/libavfilter/buffersrc.h"
#endif

static void *srthub_thumbnail_thread(void *context);

/* gettid() has no declaration in older glibc headers; the syscall is the
 * portable spelling. The value only tags pool buffers with their taker. */
static int srthub_gettid(void)
{
    return (int)syscall(SYS_gettid);
}

/* Monotonic milliseconds, for intervals that must not jump with the wall
 * clock (an NTP step back would otherwise stall them until it caught up). */
static int64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

typedef struct _srt_server_worker_output_thread_struct_ {
    int          thread;
    srthub_core_struct *core;
    SRTSOCKET    client_sock;
    char         client_address[MAX_STRING_SIZE];
    int          client_port;
} srt_server_worker_output_thread_struct;

typedef struct _srt_audio_thread_struct_ {
    srthub_core_struct *core;
    int                audio_stream;
} srt_audio_thread_struct;

typedef struct _srt_server_worker_input_thread_struct_ {
} srt_server_worker_input_thread_struct;

typedef struct _srt_server_thread_struct_ {
    char         server_address[MAX_STRING_SIZE];
    int          server_port;
    char         server_interface_name[MAX_STRING_SIZE];
    char         streamid[MAX_STRING_SIZE];
    char         passphrase[MAX_STRING_SIZE];
    int          keysize;
    srthub_core_struct *core;
} srt_server_thread_struct;

typedef struct _srt_receive_thread_caller_struct_ {
    char         server_address[MAX_STRING_SIZE];
    int          server_port;
    char         server_interface_name[MAX_STRING_SIZE];
    char         streamid[MAX_STRING_SIZE];
    char         passphrase[MAX_STRING_SIZE];
    int          keysize;
    srthub_core_struct *core;
} srt_receive_thread_caller_struct;

typedef struct _srt_receive_thread_listener_struct_ {
    char         server_address[MAX_STRING_SIZE];
    int          server_port;
    char         server_interface_name[MAX_STRING_SIZE];
    char         streamid[MAX_STRING_SIZE];
    char         passphrase[MAX_STRING_SIZE];
    int          keysize;
    srthub_core_struct *core;
} srt_receive_thread_listener_struct;

typedef struct _udp_server_thread_struct_ {
    char         destination_address[MAX_STRING_SIZE];
    int          destination_port;
    char         interface_name[MAX_STRING_SIZE];
    int          ttl;
    srthub_core_struct *core;
} udp_server_thread_struct;

typedef struct _udp_receiver_thread_struct_ {
    char         source_address[MAX_STRING_SIZE];
    int          source_port;
    char         interface_name[MAX_STRING_SIZE];
    srthub_core_struct *core;
} udp_receiver_thread_struct;

typedef struct _output_smoothing_thread_struct_ {
    int64_t      bitrate;
} output_smoothing_thread_struct;

static int is_multicast_address(const char *ipaddress)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, ipaddress, &addr) != 1) {
        return 0;
    }
    uint8_t firstByte = addr.s_addr & 0xFF;
    return (firstByte >= 224) && (firstByte <= 239);
}

int64_t realtime_clock_difference(struct timespec *now, struct timespec *start)
{
    int64_t tsec;
    int64_t tnsec;

    if (now->tv_nsec < start->tv_nsec) {
        tsec = (now->tv_sec - start->tv_sec);
        tsec--;
        tnsec = 1000000000;
        tnsec += (now->tv_nsec - start->tv_nsec);
    } else {
        tsec = now->tv_sec - start->tv_sec;
        tnsec = now->tv_nsec - start->tv_nsec;
    }

    return ((tnsec / 1000) + (tsec * 1000000));
}

static int check_for_rtp(int packetsize)
{
    int ts_packets = packetsize / 188;
    int packetsize_check = ts_packets * 188;

    if (packetsize_check != packetsize) {
        int updated_packetsize = packetsize - 12;
        int ts_packets = updated_packetsize / 188;

        packetsize_check = ts_packets * 188;
        if (packetsize_check == updated_packetsize) {
            return 1;
        }
    }
    return 0;
}

static int save_frame_as_jpeg(srthub_core_struct *srtcore, AVFrame *pFrame)
{
    const AVCodec *jpegCodec = (const AVCodec*)avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *jpegContext = avcodec_alloc_context3(jpegCodec);
    FILE *JPEG = NULL;
#define MAX_FILENAME_SIZE 256
    char temp_filename[MAX_FILENAME_SIZE];
    char actual_filename[MAX_FILENAME_SIZE];
    AVPacket *packet = NULL;
    int encodedFrame = 0;

    jpegContext->bit_rate = 500000;
    jpegContext->width = THUMBNAIL_WIDTH;
    jpegContext->height = THUMBNAIL_HEIGHT;
    jpegContext->time_base = (AVRational){1,30};
    jpegContext->framerate = (AVRational){30,1};
    jpegContext->pix_fmt = AV_PIX_FMT_YUVJ420P;

    avcodec_open2(jpegContext, jpegCodec, NULL);
    packet = av_packet_alloc();
    avcodec_send_frame(jpegContext, pFrame);
    avcodec_receive_packet(jpegContext, packet);

    snprintf(temp_filename, MAX_FILENAME_SIZE-1, "/opt/srthub/thumbnail/%d.jpg.temp", srtcore->session_identifier);
    snprintf(actual_filename, MAX_FILENAME_SIZE-1, "/opt/srthub/thumbnail/%d.jpg", srtcore->session_identifier);
    JPEG = fopen(temp_filename, "wb");
    if (JPEG) {
        fwrite(packet->data, 1, packet->size, JPEG);
        fclose(JPEG);
        rename(temp_filename, actual_filename);
    }
    av_packet_unref(packet);

    avcodec_free_context(&jpegContext);
    av_packet_free(&packet);
    return 0;
}

static int receive_frame(uint8_t *sample, int sample_size, int sample_type, uint32_t sample_flags,
                         int64_t pts, int64_t dts, int64_t last_pcr, int source,
                         int sub_source, char *lang_tag, int64_t corruption_count, int muxstreams, void *context)
{
    srthub_core_struct *srtcore = (srthub_core_struct*)context;
    dataqueue_message_struct *msg;
    int threadid = srthub_gettid();

    fprintf(stderr,"received frame (%d/%d): source=%d, sub_source=%d, type=0x%x, corruption_count=%ld, size=%d\n",
            source+1, muxstreams, source, sub_source, sample_type, corruption_count, sample_size);

    if (sample_type == STREAM_TYPE_SCTE35) {
        scte35_data_struct cue;
        time_t cue_time;
        int64_t cue_seen;
        int report_cue;

        if (!sample || sample_size != (int)sizeof(scte35_data_struct)) {
            fprintf(stderr,"receive_frame: unexpected scte35 sample size %d, ignoring\n", sample_size);
            return 0;
        }
        /* the decoder frees its copy as soon as this returns */
        memcpy(&cue, sample, sizeof(cue));

        if (!cue.parse_complete) {
            /* a section split across transport packets, or one running past
             * the payload - most of the fields are unset, so reporting it
             * would invent a cue that is not in the stream */
            fprintf(stderr,"receive_frame: incomplete scte35 section on pid %d, ignoring\n", cue.splice_pid);
            return 0;
        }

        cue_time = time(NULL);
        cue_seen = monotonic_ms() / 1000;

        /* splice_info sections repeat several times a second for the same
         * event, so only the first of each distinct cue is reported. Several
         * cues are tracked rather than just the previous one, because one
         * section can carry several segmentation descriptors that arrive
         * interleaved on every retransmission. */
        report_cue = 1;
        {
            int slot = -1;
            int i;

            for (i = 0; i < SCTE35_RECENT_CUES; i++) {
                scte35_recent_cue_struct *recent = &srtcore->scte35_recent[i];

                if (recent->valid &&
                    recent->event_id == cue.splice_event_id &&
                    recent->cue_direction == cue.cue_direction &&
                    recent->cancel == cue.cancel &&
                    recent->segmentation_type == cue.segmentation_type_id) {
                    slot = i;
                    break;
                }
            }

            if (slot >= 0) {
                if ((cue_seen - srtcore->scte35_recent[slot].last_seen) <
                    SCTE35_CUE_REPEAT_WINDOW_SECONDS) {
                    report_cue = 0;
                }
            } else {
                /* take a free slot, otherwise evict the least recently seen */
                slot = 0;
                for (i = 0; i < SCTE35_RECENT_CUES; i++) {
                    if (!srtcore->scte35_recent[i].valid) {
                        slot = i;
                        break;
                    }
                    if (srtcore->scte35_recent[i].last_seen <
                        srtcore->scte35_recent[slot].last_seen) {
                        slot = i;
                    }
                }
                srtcore->scte35_recent[slot].valid = 1;
                srtcore->scte35_recent[slot].event_id = cue.splice_event_id;
                srtcore->scte35_recent[slot].cue_direction = cue.cue_direction;
                srtcore->scte35_recent[slot].cancel = cue.cancel;
                srtcore->scte35_recent[slot].segmentation_type = cue.segmentation_type_id;
            }

            /* refreshed on every repeat, so a pre-roll that runs longer than
             * the window does not age out and get reported a second time */
            srtcore->scte35_recent[slot].last_seen = cue_seen;
        }

        if (report_cue) {
            srtcore->scte35_have_last_cue = 1;
            srtcore->scte35_last_cue_time = cue_time;
            srtcore->scte35_last_event_id = cue.splice_event_id;
            srtcore->scte35_last_cue_direction = cue.cue_direction;
            srtcore->scte35_last_cue_cancel = cue.cancel;
            srtcore->scte35_last_cue_immediate = cue.splice_immediate;
            srtcore->scte35_last_cue_duration = cue.pts_duration;
            srtcore->scte35_last_cue_command = cue.splice_command_type;
            srtcore->scte35_last_segmentation_type = cue.segmentation_type_id;
            snprintf(srtcore->scte35_last_cue_name, sizeof(srtcore->scte35_last_cue_name),
                     "%s", cue.descriptor_name);
            srtcore->scte35_cue_count++;

            fprintf(stderr,"receive_frame: scte35 cue pid=%d command=0x%x direction=%d event=%ld immediate=%d duration=%ld (%s)\n",
                    cue.splice_pid, cue.splice_command_type, cue.cue_direction,
                    (long)cue.splice_event_id, cue.splice_immediate,
                    (long)cue.pts_duration, cue.descriptor_name);

            send_signal_scte35(srtcore, &cue);
        }
        return 0;
    }

    if (sample_type == STREAM_TYPE_MPEG || sample_type == STREAM_TYPE_AC3 || sample_type == STREAM_TYPE_AAC || sample_type == STREAM_TYPE_UNKNOWN_AUDIO) {
        /* There is one decode queue per audio index. An index from the PMT
         * outside that range - a program with more audio tracks than decode
         * threads, or a stream the PMT did not make audio - would index past
         * audiodecodequeue[] into the neighbouring queues and pools. */
        if (sub_source < 0 || sub_source >= MAX_WORKER_THREADS) {
            return 0;
        }
        /* the decoders read up to AV_INPUT_BUFFER_PADDING_SIZE past the data */
        if (!sample || sample_size <= 0 ||
            sample_size > MAX_AUDIO_BUFFER_SIZE - AV_INPUT_BUFFER_PADDING_SIZE) {
            fprintf(stderr,"received frame: dropping audio sample of %d bytes (limit %d)\n",
                    sample_size, MAX_AUDIO_BUFFER_SIZE - AV_INPUT_BUFFER_PADDING_SIZE);
            return 0;
        }
        msg = (dataqueue_message_struct*)memory_take(srtcore->msgpool, threadid);
        if (msg) {
            uint8_t *buffer = (uint8_t*)memory_take(srtcore->audiopool, threadid);
            if (buffer) {
                memcpy(buffer, sample, sample_size);
                memset(buffer + sample_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                msg->buffer = (void*)buffer;
                msg->buffer_size = sample_size;
                msg->buffer_type = sample_type;
                msg->flags = muxstreams;
                msg->stream_index = sub_source;
                msg->source_discontinuity = corruption_count;
                dataqueue_put_front(srtcore->audiodecodequeue[sub_source], msg);
                msg = NULL;
            } else {
                fprintf(stderr,"received frame: audio buffers exhausted\n");
                memory_return(srtcore->msgpool, msg);
                msg = NULL;
            }
        } else {

        }
        return 0;
    }

    if (sample_type == STREAM_TYPE_H264 || sample_type == STREAM_TYPE_HEVC || sample_type == STREAM_TYPE_MPEG2) {
        if (!sample || sample_size <= 0 ||
            sample_size > MAX_THUMBNAIL_BUFFER_SIZE - AV_INPUT_BUFFER_PADDING_SIZE) {
            fprintf(stderr,"received frame: dropping video sample of %d bytes (limit %d)\n",
                    sample_size, MAX_THUMBNAIL_BUFFER_SIZE - AV_INPUT_BUFFER_PADDING_SIZE);
            return 0;
        }
        if (!srtcore->video_initialized) {
            pthread_mutex_lock(srtcore->video_init_lock);
            if (!srtcore->video_initialized) {
                fprintf(stderr,"receive_frame: video detected, initializing video resources\n");
                srtcore->videopool = memory_create(MAX_THUMBNAIL_BUFFERS, MAX_THUMBNAIL_BUFFER_SIZE);
                srtcore->thumbnail_thread_running = 1;
                pthread_create(&srtcore->thumbnail_thread_id, NULL, srthub_thumbnail_thread, srtcore);
                srtcore->video_initialized = 1;
            }
            pthread_mutex_unlock(srtcore->video_init_lock);
        }

        srtcore->thumbnail_frame_counter++;

        //#define THUMBNAIL_FRAME_INTERVAL 120
        //        if ((srtcore->thumbnail_frame_counter % THUMBNAIL_FRAME_INTERVAL) != 0) {
        //            return 0;
        //        }

        msg = (dataqueue_message_struct*)memory_take(srtcore->msgpool, threadid);
        if (msg) {
            uint8_t *buffer = (uint8_t*)memory_take(srtcore->videopool, threadid);
            if (buffer) {
                memcpy(buffer, sample, sample_size);
                memset(buffer + sample_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                msg->buffer = (void*)buffer;
                msg->buffer_size = sample_size;
                msg->buffer_type = sample_type;
                msg->flags = muxstreams;
                msg->stream_index = source;
                msg->source_discontinuity = corruption_count;
                dataqueue_put_front(srtcore->thumbnailqueue, msg);
                msg = NULL;
            } else {
                fprintf(stderr,"received frame: thumbnail buffers exhausted\n");
                memory_return(srtcore->msgpool, msg);
                msg = NULL;
            }
        } else {
            fprintf(stderr,"received frame: msg buffers exhausted, thumbnailqueue=%d\n", dataqueue_get_size(srtcore->thumbnailqueue));
        }
    }

    return 0;
}

/* Copies the PMT's elementary stream PIDs into the core struct for the main
 * loop's status writer to publish. */
static void publish_pid_summary(srthub_core_struct *srtcore, transport_data_struct *decode)
{
    pid_summary_struct summary;
    int i;

    if (get_pid_summary(decode, &summary) < 0) {
        return;
    }

    srtcore->pcr_pid = summary.pcr_pid;
    srtcore->video_pid = summary.video_pid;
    srtcore->video_stream_type = summary.video_stream_type;
    srtcore->scte35_pid = summary.scte35_pid;

    srtcore->audio_pid_count = summary.audio_pid_count;
    if (srtcore->audio_pid_count > MAX_SRTHUB_AUDIO_PIDS) {
        srtcore->audio_pid_count = MAX_SRTHUB_AUDIO_PIDS;
    }

    for (i = 0; i < MAX_SRTHUB_AUDIO_PIDS; i++) {
        int c;

        srtcore->audio_pid[i] = summary.audio_pid[i];
        srtcore->audio_stream_type[i] = summary.audio_stream_type[i];

        /* The language tag comes straight off the wire and ends up in a JSON
         * status file, so anything that is not three plain letters is dropped
         * rather than copied through. */
        memset(srtcore->audio_language[i], 0, MAX_SRTHUB_LANG_SIZE);
        for (c = 0; c < 3; c++) {
            char tag = summary.audio_language[i].lang_tag[c];
            if (!((tag >= 'a' && tag <= 'z') || (tag >= 'A' && tag <= 'Z'))) {
                memset(srtcore->audio_language[i], 0, MAX_SRTHUB_LANG_SIZE);
                break;
            }
            srtcore->audio_language[i][c] = tag;
        }
    }
}

/* The PMT goes away with the input, so stop reporting PIDs from it. */
static void clear_pid_summary(srthub_core_struct *srtcore)
{
    srtcore->pcr_pid = 0;
    srtcore->video_pid = 0;
    srtcore->video_stream_type = 0;
    srtcore->scte35_pid = 0;
    srtcore->audio_pid_count = 0;
    memset(srtcore->audio_pid, 0, sizeof(srtcore->audio_pid));
    memset(srtcore->audio_stream_type, 0, sizeof(srtcore->audio_stream_type));
    memset(srtcore->audio_language, 0, sizeof(srtcore->audio_language));
}

static int send_restart_message(srthub_core_struct *srtcore)
{
    dataqueue_message_struct *msg;
    int threadid = srthub_gettid();

    msg = (dataqueue_message_struct*)memory_take(srtcore->msgpool, threadid);
    if (msg) {
        memset(msg, 0, sizeof(dataqueue_message_struct));
        msg->flags = MESSAGE_TYPE_RESTART;
        dataqueue_put_front(srtcore->msgqueue, msg);
        msg = NULL;
    } else {
        fprintf(stderr,"send_restart_message: msg buffers exhausted\n");
    }
    return 0;
}

/* How often a listener retries a bind that failed, and how often an unchanged
 * failure is reported again, so a misconfigured service stays visible in the
 * event log without flooding it. */
#define SRT_LISTEN_RETRY_SECONDS   5
#define SRT_LISTEN_REPORT_SECONDS  3600

/* Records a listener that cannot bind, for the dashboard: the service card
 * shows this instead of looking like a healthy service with no input. */
static void write_listen_error(srthub_core_struct *srtcore, const char *address, int port, const char *reason)
{
    char filename[MAX_STRING_SIZE];
    char tempname[MAX_STRING_SIZE];
    FILE *statusfile;

    snprintf(filename, sizeof(filename), "/opt/srthub/status/srt_listen_error_%d.json", srtcore->session_identifier);
    snprintf(tempname, sizeof(tempname), "%s.temp", filename);
    statusfile = fopen(tempname, "wb");
    if (statusfile) {
        fprintf(statusfile, "{\n");
        fprintf(statusfile, "    \"address\":\"%s\",\n", address);
        fprintf(statusfile, "    \"port\":%d,\n", port);
        fprintf(statusfile, "    \"error\":\"%s\"\n", reason);
        fprintf(statusfile, "}\n");
        fclose(statusfile);
        rename(tempname, filename);
    }
}

static void clear_listen_error(srthub_core_struct *srtcore)
{
    char filename[MAX_STRING_SIZE];

    snprintf(filename, sizeof(filename), "/opt/srthub/status/srt_listen_error_%d.json", srtcore->session_identifier);
    unlink(filename);
}

/* Creates an SRT socket with the listener options and binds it to
 * address:port. Returns the socket, or SRT_INVALID_SOCK with the reason in
 * plain words. The reason never repeats the configured address unless it
 * parsed as one, so it is safe to put in the JSON event and status file. */
static SRTSOCKET srt_create_bound_listener(const char *address, int port, const int32_t *latencyms,
                                           const char *passphrase, const char *streamid,
                                           char *reason, int reason_size)
{
    SRTSOCKET sock;
    struct sockaddr_in bind_addr;
    int no = 0;
    int max_srt_packet_size = MAX_SRT_PACKET_SIZE;
    int length;

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &bind_addr.sin_addr) != 1) {
        snprintf(reason, reason_size, "the configured address is not an IPv4 address");
        return SRT_INVALID_SOCK;
    }

    sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        snprintf(reason, reason_size, "unable to create an SRT socket");
        return SRT_INVALID_SOCK;
    }

    if (srt_setsockflag(sock, SRTO_SNDSYN, &no, sizeof(no)) == SRT_ERROR ||
        (latencyms && srt_setsockflag(sock, SRTO_LATENCY, latencyms, sizeof(*latencyms)) == SRT_ERROR) ||
        srt_setsockflag(sock, SRTO_PAYLOADSIZE, &max_srt_packet_size, sizeof(max_srt_packet_size)) == SRT_ERROR) {
        goto option_failed;
    }
    length = strlen(passphrase);
    if (length >= 10 && length <= 79 &&
        srt_setsockflag(sock, SRTO_PASSPHRASE, passphrase, length) == SRT_ERROR) {
        goto option_failed;
    }
    length = strlen(streamid);
    if (length > 0 && srt_setsockflag(sock, SRTO_STREAMID, streamid, length) == SRT_ERROR) {
        goto option_failed;
    }

    if (srt_bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) == SRT_ERROR) {
        int sys_errno = 0;

        /* SRT's own message is a generic "unable to create/configure SRT
         * socket"; the system errno says what actually went wrong. */
        srt_getlasterror(&sys_errno);
        if (sys_errno == EADDRNOTAVAIL) {
            snprintf(reason, reason_size, "%s is not an address of this machine - use Caller mode to connect to a remote sender",
                     address);
        } else if (sys_errno == EADDRINUSE) {
            snprintf(reason, reason_size, "port %d is already in use on this machine", port);
        } else if (sys_errno == EACCES) {
            snprintf(reason, reason_size, "permission denied for port %d", port);
        } else {
            snprintf(reason, reason_size, "bind failed (system error %d)", sys_errno);
        }
        srt_close(sock);
        return SRT_INVALID_SOCK;
    }
    return sock;

option_failed:
    snprintf(reason, reason_size, "unable to configure the SRT socket");
    srt_close(sock);
    return SRT_INVALID_SOCK;
}

/* Binds an SRT listener, retrying every SRT_LISTEN_RETRY_SECONDS while
 * *running is set. A bind can fail for good (an address of another machine)
 * or for a while (a port another process still holds, an interface not up
 * yet); either way the service used to stop listening without a word while
 * still looking like it was running. The failure is now reported as an event
 * when it first happens, when its reason changes and hourly while it lasts,
 * and the recovery is reported too. Returns SRT_INVALID_SOCK only once
 * *running is cleared. */
static SRTSOCKET srt_bind_listener_retrying(srthub_core_struct *srtcore, int *running,
                                            const char *address, int port, const int32_t *latencyms,
                                            const char *passphrase, const char *streamid)
{
    char reason[MAX_SMALLBUF_SIZE];
    char last_reason[MAX_SMALLBUF_SIZE];
    char message[MAX_SMALLBUF_SIZE];
    const char *shown_address = address;
    struct in_addr parsed;
    int64_t last_report = 0;
    int failed = 0;
    int wait;

    /* only an address that parsed is echoed into JSON */
    if (inet_pton(AF_INET, address, &parsed) != 1) {
        shown_address = "(invalid address)";
    }
    last_reason[0] = 0;

    while (*running) {
        SRTSOCKET sock;

        reason[0] = 0;
        sock = srt_create_bound_listener(address, port, latencyms, passphrase, streamid,
                                         reason, sizeof(reason));
        if (sock != SRT_INVALID_SOCK) {
            if (failed) {
                snprintf(message, sizeof(message), "SRT Listening on %s:%d", shown_address, port);
                fprintf(stderr,"%s\n", message);
                send_signal(srtcore, SIGNAL_SRT_LISTENING, message);
            }
            clear_listen_error(srtcore);
            return sock;
        }

        if (!failed || strcmp(reason, last_reason) != 0 ||
            monotonic_ms() - last_report >= (int64_t)SRT_LISTEN_REPORT_SECONDS * 1000) {
            snprintf(message, sizeof(message), "SRT Unable to Listen on %s:%d (%s), retrying every %d seconds",
                     shown_address, port, reason, SRT_LISTEN_RETRY_SECONDS);
            fprintf(stderr,"%s\n", message);
            send_signal(srtcore, SIGNAL_SRT_LISTEN_FAILED, message);
            write_listen_error(srtcore, shown_address, port, reason);
            snprintf(last_reason, sizeof(last_reason), "%s", reason);
            last_report = monotonic_ms();
        }
        failed = 1;

        for (wait = 0; wait < SRT_LISTEN_RETRY_SECONDS * 10 && *running; wait++) {
            usleep(100000);
        }
    }
    return SRT_INVALID_SOCK;
}

static void *srt_receiver_thread_listener(void *context)
{
    srt_receive_thread_listener_struct *srtdata;
    srthub_core_struct *srtcore;
    transport_data_struct *decode = create_transport_data();
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET client_sock = SRT_INVALID_SOCK;
    char statsfilename[MAX_STRING_SIZE];
    int srt_connected = 0;
    int srterr;
    int recvbytes;
    uint32_t update_stats = 0;
    char *buffer = NULL;
    int64_t connect_start;
    SRT_TRACEBSTATS stats;
    int threadid = srthub_gettid();
    int32_t latencyms;

    if (!decode) {
        fprintf(stderr,"unable to allocate the transport stream decoder\n");
        free(context);
        return NULL;
    }

    fprintf(stderr,"srt_receiver_thread_listener: srt_startup() running, thread_id=%d\n", threadid);
    srt_startup();
    fprintf(stderr,"srt_receiver_thread_listener: srt_startup() done, thread_id=%d\n", threadid);

    srtdata = (srt_receive_thread_listener_struct*)context;
    srtcore = srtdata->core;

    latencyms = srtcore->config->latencyms;

    sprintf(statsfilename,"/opt/srthub/status/srt_receiver_%d.json", srtcore->session_identifier);

    listener = srt_bind_listener_retrying(srtcore, &srtcore->srt_receiver_thread_running,
                                          srtdata->server_address, srtdata->server_port, &latencyms,
                                          srtdata->passphrase, srtdata->streamid);
    if (listener == SRT_INVALID_SOCK) {
        /* stopped while the bind was still failing */
        goto cleanup_srt_receiver_thread_listener;
    }

    buffer = (char*)malloc(MAX_UDP_BUFFER_READ);
    if (!buffer) {
        goto cleanup_srt_receiver_thread_listener;
    }

    fprintf(stderr,"srt_receiver_thread_listener: starting main thread loop\n");

    connect_start = monotonic_ms();
    while (srtcore->srt_receiver_thread_running) {
        srterr = srt_listen(listener, 1);  // only one
        if (srterr == SRT_ERROR) {
            char signal_message[MAX_STRING_SIZE];
            snprintf(signal_message, MAX_STRING_SIZE-1, "SRT Unable to Listen on %s:%d (%s), restarting",
                     srtdata->server_address, srtdata->server_port, srt_getlasterror_str());
            fprintf(stderr,"srt_receiver_thread_listener: %s\n", signal_message);
            send_signal(srtcore, SIGNAL_SRT_LISTEN_FAILED, signal_message);
            usleep(SRT_LISTEN_RETRY_SECONDS * 1000000);
            send_restart_message(srtcore);
            goto cleanup_srt_receiver_thread_listener;
        }

        clear_pid_summary(srtcore);

        FILE *statsfile = fopen(statsfilename,"wb");
        if (statsfile) {
            fprintf(statsfile,"{\n");
            fprintf(statsfile,"    \"srt-mode\":\"Listener\",\n");
            fprintf(statsfile,"    \"srt-server-address\":\"%s\",\n", srtdata->server_address);
            fprintf(statsfile,"    \"srt-server-port\":%d,\n", srtdata->server_port);
            fprintf(statsfile,"    \"srt-connection\":0\n");
            fprintf(statsfile,"}\n");
            fclose(statsfile);
        }

        struct sockaddr_in client_addr;
        int addrlen = sizeof(client_addr);
        client_sock = srt_accept(listener,
                                 (struct sockaddr*)&client_addr,
                                 &addrlen);

        if (client_sock == SRT_INVALID_SOCK) {
            fprintf(stderr,"srt_receiver_thread_listener: invalid socket on accept\n");
            usleep(100000);
            send_restart_message(srtcore);
            goto cleanup_srt_receiver_thread_listener;
        }

        struct sockaddr_in *sa4 = (struct sockaddr_in*)&client_addr;
        char ipaddr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(sa4->sin_addr), ipaddr, sizeof(ipaddr));
        fprintf(stderr,"srt_receiver_thread_listener: client connected from %s, port %d\n", ipaddr, client_addr.sin_port);

        char signal_message[MAX_STRING_SIZE];
        snprintf(signal_message, MAX_STRING_SIZE-1, "Accepted SRT Connection Request From %s:%d",
                 ipaddr, client_addr.sin_port);
        send_signal(srtcore, SIGNAL_SRT_CONNECTED, signal_message);

        srt_connected = 1;
        while (srtcore->srt_receiver_thread_running && srt_connected) {
            SRT_MSGCTRL srtcontrol;

            //fprintf(stderr,"calling srt_recvmsg2\n");
            recvbytes = srt_recvmsg2(client_sock, buffer, MAX_PACKET_BUFFER_SIZE, &srtcontrol);
            //fprintf(stderr,"done calling srt_recvmsg2, %d\n", recvbytes);
            if (recvbytes < 0) {
                int lasterr = srt_getlasterror(NULL);
                if (lasterr == SRT_ENOCONN) {
                    int64_t delta_time_no_connection;
                    fprintf(stderr,"srt_receiver_thread_listener: SRT not connected, waiting...\n");
                    clear_pid_summary(srtcore);
                    if ((update_stats % 100)==0) {
                        FILE *statsfile = fopen(statsfilename,"wb");
                        if (statsfile) {
                            fprintf(statsfile,"{\n");
                            fprintf(statsfile,"    \"srt-mode\":\"Listener\",\n");
                            fprintf(statsfile,"    \"srt-server-address\":\"%s\",\n", srtdata->server_address);
                            fprintf(statsfile,"    \"srt-server-port\":%d,\n", srtdata->server_port);
                            fprintf(statsfile,"    \"srt-connection\":0\n");
                            fprintf(statsfile,"}\n");
                            fclose(statsfile);
                        }
                    }
                    srt_connected = 0;
                    update_stats++;

                    delta_time_no_connection = monotonic_ms() - connect_start;
                    if (delta_time_no_connection >= 5000) {
                        char signal_message[MAX_STRING_SIZE];
                        snprintf(signal_message,MAX_STRING_SIZE-1,"SRT Unable to Connect to %s:%d (Timeout, Trying Again)",
                                 srtdata->server_address,
                                 srtdata->server_port);
                        send_signal(srtcore, SIGNAL_SRT_CONNECTION_LOST, signal_message);
                        fprintf(stderr,"srt_receiver_thread_listener: SRT waiting too long for connection, is the server up?\n");
                        send_restart_message(srtcore);
                        goto cleanup_srt_receiver_thread_listener;
                    }
                    usleep(200000);
                } else if (lasterr == SRT_ECONNLOST) {
                    srt_connected = 0;
                    char signal_message[MAX_STRING_SIZE];
                    fprintf(stderr,"srt_receiver_thread_listener: SRT connection has been lost!\n");
                    snprintf(signal_message,MAX_STRING_SIZE-1,"SRT Connection Lost to %s:%d",
                             srtdata->server_address,
                             srtdata->server_port);
                    send_signal(srtcore, SIGNAL_SRT_CONNECTION_LOST, signal_message);
                    send_restart_message(srtcore);
                    goto cleanup_srt_receiver_thread_listener;
                } else {
                    srt_connected = 0;
                    fprintf(stderr,"srt_receiver_thread_listener: SRT unknown error: %s\n", srt_getlasterror_str());
                    char signal_message[MAX_STRING_SIZE];
                    snprintf(signal_message,MAX_STRING_SIZE-1,"SRT Error: %s, No Connection", srt_getlasterror_str());
                    send_signal(srtcore, SIGNAL_SRT_CONNECTION_LOST, signal_message);
                    send_restart_message(srtcore);
                    goto cleanup_srt_receiver_thread_listener;
                }
                usleep(10000);
            } else if (recvbytes == 0) {
                usleep(10000);
            } else {
                int cp;
                int tp;
                int current_state;
                int clear_it = 0;

                if (srt_connected == 0) {
                    char signal_message[MAX_STRING_SIZE];
                    snprintf(signal_message, MAX_STRING_SIZE-1, "SRT Connected to %s:%d", srtdata->server_address, srtdata->server_port);
                    send_signal(srtcore, SIGNAL_SRT_CONNECTED, signal_message);
                }
                srt_connected = 1;
                connect_start = monotonic_ms();
                if ((update_stats % 100)==0) {
                    srterr = srt_bstats(client_sock, &stats, clear_it);
                    if (srterr != SRT_ERROR) {
                        int64_t now = srt_time_now();
                        fprintf(stderr,"srt_receiver_thread_listener: received %d/%ld bytes (serversock=%d) r=%10ld l=%5d retrans=%5d ack=%d nack=%d d=%8d timestamp=%ld (now=%ld) diff=%ld\n",
                                recvbytes,
                                stats.byteRecvUniqueTotal,
                                client_sock,
                                stats.pktRecvTotal,
                                stats.pktRcvLossTotal,
                                stats.pktRetransTotal,
                                stats.pktSentACKTotal,
                                stats.pktSentNAKTotal,
                                stats.pktRcvDropTotal,
                                srtcontrol.srctime,
                                now,
                                now-srtcontrol.srctime);
                        //fprintf(stderr,"srt_receiver_thread_caller: retransmissions detected = %d\n", stats.pktRetransTotal);
                        fprintf(stderr,"srt_receiver_thread_listener: receive rate %.2f mbps @ %ld, %d\n", stats.mbpsRecvRate, stats.msTimeStamp, recvbytes);

                        /* published by the main loop in corestatus */
                        publish_pid_summary(srtcore, decode);

                        FILE *statsfile = fopen(statsfilename,"wb");
                        if (statsfile) {
                            fprintf(statsfile,"{\n");
                            fprintf(statsfile,"    \"srt-mode\":\"Listener\",\n");
                            fprintf(statsfile,"    \"srt-server-address\":\"%s\",\n", srtdata->server_address);
                            fprintf(statsfile,"    \"srt-server-port\":%d,\n", srtdata->server_port);
                            fprintf(statsfile,"    \"srt-connection\":1,\n");
                            fprintf(statsfile,"    \"srt-time\":%ld,\n", now);
                            fprintf(statsfile,"    \"client-address\":\"%s\",\n", ipaddr);
                            fprintf(statsfile,"    \"client-port\":%d,\n", client_addr.sin_port);
                            fprintf(statsfile,"    \"total-bytes-received\":%ld,\n", stats.byteRecvUniqueTotal);
                            fprintf(statsfile,"    \"packets-received\":%ld,\n", stats.pktRecvTotal);
                            fprintf(statsfile,"    \"packets-lost\":%d,\n", stats.pktRcvLossTotal);
                            fprintf(statsfile,"    \"packets-retransmitted\":%d,\n", stats.pktRetransTotal);
                            fprintf(statsfile,"    \"packets-dropped\":%d,\n", stats.pktRcvDropTotal);
                            fprintf(statsfile,"    \"loss-percentage\":%.2f,\n", stats.pktRecvTotal > 0 ? (double)stats.pktRcvLossTotal / (double)stats.pktRecvTotal * (double)100.0 : 0.0);
                            fprintf(statsfile,"    \"bitrate-kbps\":%.2f,\n", (double)stats.mbpsRecvRate * (double)1000.0);
                            fprintf(statsfile,"    \"latencyms\":%d,\n", latencyms);
                            fprintf(statsfile,"    \"transport-stream-id\":%d,\n", decode->pat_transport_stream_id);
                            fprintf(statsfile,"    \"rtt\":%.2f\n", (double)stats.msRTT);
                            fprintf(statsfile,"}\n");
                            fclose(statsfile);
                        }
                    }
                }
                update_stats++;
                tp = recvbytes / 188;
                if (check_for_rtp(recvbytes)) {
                    uint8_t *updated_buffer = (uint8_t*)buffer+12;
                    decode_packets((uint8_t*)updated_buffer, tp, decode, 0);
                } else {
                    decode_packets((uint8_t*)buffer, tp, decode, 0);
                }

                {
                    dataqueue_message_struct *msg = NULL;
                    uint8_t *obuffer = NULL;

                    msg = (dataqueue_message_struct*)memory_take(srtcore->msgpool, threadid);
                    if (msg) {
                        memset(msg, 0, sizeof(dataqueue_message_struct));
                        obuffer = (uint8_t*)memory_take(srtcore->packetpool, threadid);
                        if (obuffer) {
                            if (recvbytes > MAX_PACKET_BUFFER_SIZE) {
                                recvbytes = MAX_PACKET_BUFFER_SIZE;
                            }
                            if (check_for_rtp(recvbytes)) {
                                uint8_t *updated_buffer = (uint8_t*)buffer+12;
                                memcpy(obuffer, updated_buffer, recvbytes-12);
                                msg->buffer_size = recvbytes-12;
                            } else {
                                memcpy(obuffer, buffer, recvbytes);
                                msg->buffer_size = recvbytes;
                            }
                            msg->buffer = obuffer;
                            msg->pts = srtcontrol.srctime;
                            dataqueue_put_front(srtcore->udpserverqueue, msg);
                        } else {
                            fprintf(stderr,"srt_receiver_thread_listener: packet buffers exhausted\n");
                            memory_return(srtcore->msgpool, msg);
                            msg = NULL;
                        }
                    } else {
                        fprintf(stderr,"srt_receiver_thread_listener: msg buffers exhausted\n");
                    }
                    msg = NULL;
                }
            }
        }
    }

cleanup_srt_receiver_thread_listener:
    fprintf(stderr,"srt_receiver_thread_listener: leaving thread\n");
    if (listener != SRT_INVALID_SOCK) {
        srt_close(listener);
        listener = SRT_INVALID_SOCK;
    }
    if (client_sock != SRT_INVALID_SOCK) {
        srt_close(client_sock);
        client_sock = SRT_INVALID_SOCK;
    }
    destroy_transport_data(decode);
    free(srtdata);
    free(buffer);
    srt_cleanup();

    return NULL;
}

static void *srt_receiver_thread_caller(void *context)
{
    srt_receive_thread_caller_struct *srtdata;
    srthub_core_struct *srtcore;
    int srterr;
    int epollid = 0;
    struct sockaddr_in sa;
    int no = 0;
    int modes;
    SRTSOCKET serversock = SRT_INVALID_SOCK;
    int recvbytes;
    SRT_TRACEBSTATS stats;
    transport_data_struct *decode = create_transport_data();
    int64_t connect_start;
    int stats_size;
    uint32_t update_stats;
    char buffer[MAX_PACKET_BUFFER_SIZE];
    char statsfilename[MAX_STRING_SIZE];
    int srt_connected = 0;
    int threadid = srthub_gettid();
    int32_t latencyms;
    int max_srt_packet_size = MAX_SRT_PACKET_SIZE;

    if (!decode) {
        fprintf(stderr,"unable to allocate the transport stream decoder\n");
        free(context);
        return NULL;
    }

    srt_startup();

    srtdata = (srt_receive_thread_caller_struct*)context;
    srtcore = srtdata->core;

    latencyms = srtcore->config->latencyms;

    fprintf(stderr,"srt_receiver_thread_caller: starting srt receiver thread, %d\n", srtcore->session_identifier);

    serversock = srt_create_socket();
    if (serversock == SRT_ERROR) {
        fprintf(stderr,"srt_receiver_thread_caller: unable to srt_create_socket() successfully\n");
        destroy_transport_data(decode);
        decode = NULL;
        free(srtdata);
        srtdata = NULL;
        srt_cleanup();
        return NULL;
    }

    sprintf(statsfilename,"/opt/srthub/status/srt_receiver_%d.json", srtcore->session_identifier);

    sa.sin_family = AF_INET;
    sa.sin_port = htons(srtdata->server_port);

    fprintf(stderr,"srt_receiver_thread_caller: attempting to connect to %s:%d\n",
            srtdata->server_address,
            srtdata->server_port);

    srterr = inet_pton(AF_INET, srtdata->server_address, &sa.sin_addr);
    if (srterr != 1) {
        fprintf(stderr,"srt_receiver_thread_caller: uanble to process server_address, %s\n", srtdata->server_address);
        goto cleanup_srt_receiver_thread_caller;
    }

    epollid = srt_epoll_create();
    if (epollid == -1) {
        goto cleanup_srt_receiver_thread_caller;
    }

    fprintf(stderr,"srt_receiver_thread_caller: created epollid=%d\n", epollid);

    srterr = srt_setsockflag(serversock, SRTO_RCVSYN, &no, sizeof(no));
    if (srterr == SRT_ERROR) {
        // srt_getlasterror_str();
        goto cleanup_srt_receiver_thread_caller;
    }

    srterr = srt_setsockflag(serversock, SRTO_SNDSYN, &no, sizeof(no));
    if (srterr == SRT_ERROR) {
        // srt_getlasterror_str();
        fprintf(stderr,"srt_receiver_thread_caller: unable to proceed with srt_setsockflag()\n");
        goto cleanup_srt_receiver_thread_caller;
    }

    srterr = srt_setsockflag(serversock, SRTO_LATENCY, &latencyms, sizeof(latencyms));
    if (srterr == SRT_ERROR) {
        fprintf(stderr,"srt_receiver_thread_caller: unable to proceed with srt_setsockflag()\n");
        goto cleanup_srt_receiver_thread_caller;
    }

    srterr = srt_setsockflag(serversock, SRTO_PAYLOADSIZE, &max_srt_packet_size, sizeof(max_srt_packet_size));
    if (srterr == SRT_ERROR) {
        fprintf(stderr,"srt_receiver_thread_caller: unable to proceed with srt_setsockflag()\n");
        goto cleanup_srt_receiver_thread_caller;
    }

    int passphrase_length = strlen(srtdata->passphrase);
    if (passphrase_length >= 10 && passphrase_length <= 80) {
        srterr = srt_setsockflag(serversock, SRTO_PASSPHRASE, srtdata->passphrase, passphrase_length);
        if (srterr == SRT_ERROR) {
            // srt_getlasterror_str();
            fprintf(stderr,"srt_receiver_thread_caller: unable to proceed with srt_setsockflag()\n");
            goto cleanup_srt_receiver_thread_caller;
        }
    }

    int streamid_length = strlen(srtdata->streamid);
    if (streamid_length > 0) {
        srterr = srt_setsockflag(serversock, SRTO_STREAMID, srtdata->streamid, streamid_length);
        if (srterr == SRT_ERROR) {
            // srt_getlasterror_str();
            fprintf(stderr,"srt_receiver_thread_caller: unable to proceed with srt_setsockflag()\n");
            goto cleanup_srt_receiver_thread_caller;
        }
    }

    modes = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    srterr = srt_epoll_add_usock(epollid, serversock, &modes);
    if (srterr == SRT_ERROR) {
        // srt_getlasterror_str();
        fprintf(stderr,"srt_receiver_thread_caller: unable to proceed with srt_epoll_add_usock()\n");
        goto cleanup_srt_receiver_thread_caller;
    }

    fprintf(stderr,"srt_receiver_thread_caller: attempting to proceed with srt_connect()\n");

    srterr = srt_connect(serversock, (struct sockaddr*)&sa, sizeof(sa));
    if (srterr == SRT_ERROR) {
        fprintf(stderr,"srt_receiver_thread_caller: unable to proceed with srt_connect\n");
        // srt_getlasterror_str();
        goto cleanup_srt_receiver_thread_caller;
    }

    fprintf(stderr,"srt_receiver_thread_caller: finished with srt_connect(), serversock=%d\n", serversock);
    connect_start = monotonic_ms();

    /*
    // Set the local bind address and port
    const char* bind_address = "0.0.0.0";  // Bind to all available network interfaces
    int bind_port = 12346;  // Choose a local port

    // Configure and bind the local socket
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(bind_port);
    inet_pton(AF_INET, bind_address, &local_addr.sin_addr);

    if (srt_bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) != 0) {
        fprintf(stderr, "Error binding the socket to %s:%d.\n", bind_address, bind_port);
        srt_close(sockfd);
        srt_cleanup();
        return 1;
    }
    */

    stats_size = sizeof(stats);
    update_stats = 0;
    while (srtcore->srt_receiver_thread_running) {
        SRT_MSGCTRL srtcontrol;
        recvbytes = srt_recvmsg2(serversock, buffer, MAX_PACKET_BUFFER_SIZE, &srtcontrol);
        if (recvbytes < 0) {
            int lasterr = srt_getlasterror(NULL);
            if (lasterr == SRT_ENOCONN) {
                int64_t delta_time_no_connection;
                clear_pid_summary(srtcore);
                if ((update_stats % 100)==0) {
                    fprintf(stderr,"srt_receiver_thread_caller: SRT not connected, waiting...\n");
                    FILE *statsfile = fopen(statsfilename,"wb");
                    if (statsfile) {
                        fprintf(statsfile,"{\n");
                        fprintf(statsfile,"    \"srt-mode\":\"Caller\",\n");
                        fprintf(statsfile,"    \"srt-server-address\":\"%s\",\n", srtdata->server_address);
                        fprintf(statsfile,"    \"srt-server-port\":%d,\n", srtdata->server_port);
                        fprintf(statsfile,"    \"srt-connection\":0\n");
                        fprintf(statsfile,"}\n");
                        fclose(statsfile);
                    }
                }
                srt_connected = 0;
                update_stats++;

                delta_time_no_connection = monotonic_ms() - connect_start;
                if (delta_time_no_connection >= 5000) {
                    char signal_message[MAX_STRING_SIZE];
                    snprintf(signal_message,MAX_STRING_SIZE-1,"SRT Unable to Connect to %s:%d (Timeout, Trying Again)",
                             srtdata->server_address,
                             srtdata->server_port);
                    send_signal(srtcore, SIGNAL_SRT_CONNECTION_LOST, signal_message);
                    fprintf(stderr,"srt_receiver_thread_caller: SRT waiting too long for connection, is the server up?\n");
                    send_restart_message(srtcore);
                    goto cleanup_srt_receiver_thread_caller;
                }
                usleep(200000);
            } else if (lasterr == SRT_ECONNLOST) {
                char signal_message[MAX_STRING_SIZE];
                fprintf(stderr,"srt_receiver_thread_caller: SRT connection has been lost!\n");
                srt_connected = 0;
                snprintf(signal_message,MAX_STRING_SIZE-1,"SRT Connection Lost to %s:%d",
                         srtdata->server_address,
                         srtdata->server_port);
                send_signal(srtcore, SIGNAL_SRT_CONNECTION_LOST, signal_message);
                send_restart_message(srtcore);
                goto cleanup_srt_receiver_thread_caller;
            } else if (lasterr == SRT_EASYNCRCV) {

            } else {
                fprintf(stderr,"srt_receiver_thread_caller: SRT unknown error: %s\n", srt_getlasterror_str());
            }
            usleep(10000);
        } else if (recvbytes == 0) {
            usleep(10000);
        } else {
            int cp;
            int tp;
            int current_state;
            int clear_it = 0;

            if (srt_connected == 0) {
                char signal_message[MAX_STRING_SIZE];
                snprintf(signal_message, MAX_STRING_SIZE-1, "SRT Connected To %s:%d", srtdata->server_address, srtdata->server_port);
                send_signal(srtcore, SIGNAL_SRT_CONNECTED, signal_message);
            }
            srt_connected = 1;
            connect_start = monotonic_ms();
            if ((update_stats % 100)==0) {
                srterr = srt_bstats(serversock, &stats, clear_it);
                if (srterr != SRT_ERROR) {
                    int64_t now = srt_time_now();
                    fprintf(stderr,"srt_receiver_thread_caller: received %d/%ld bytes (serversock=%d) r=%10ld l=%5d retrans=%5d ack=%d nack=%d d=%8d timestamp=%ld (now=%ld) diff=%ld\n",
                            recvbytes,
                            stats.byteRecvUniqueTotal,
                            serversock,
                            stats.pktRecvTotal,
                            stats.pktRcvLossTotal,
                            stats.pktRetransTotal,
                            stats.pktSentACKTotal,
                            stats.pktSentNAKTotal,
                            stats.pktRcvDropTotal,
                            srtcontrol.srctime,
                            now,
                            now-srtcontrol.srctime);
                    //fprintf(stderr,"srt_receiver_thread_caller: retransmissions detected = %d\n", stats.pktRetransTotal);
                    fprintf(stderr,"srt_receiver_thread_caller: receive rate %.2f mbps @ %ld, %d\n", stats.mbpsRecvRate, stats.msTimeStamp, recvbytes);

                    /* published by the main loop in corestatus */
                    publish_pid_summary(srtcore, decode);

                    FILE *statsfile = fopen(statsfilename,"wb");
                    if (statsfile) {
                        fprintf(statsfile,"{\n");
                        fprintf(statsfile,"    \"srt-mode\":\"Caller\",\n");
                        fprintf(statsfile,"    \"srt-server-address\":\"%s\",\n", srtdata->server_address);
                        fprintf(statsfile,"    \"srt-server-port\":%d,\n", srtdata->server_port);
                        fprintf(statsfile,"    \"srt-connection\":1,\n");
                        fprintf(statsfile,"    \"srt-time\":%ld,\n", now);
                        fprintf(statsfile,"    \"client-address\":\"None\",\n");
                        fprintf(statsfile,"    \"client-port\":0,\n");
                        fprintf(statsfile,"    \"total-bytes-received\":%ld,\n", stats.byteRecvUniqueTotal);
                        fprintf(statsfile,"    \"packets-received\":%ld,\n", stats.pktRecvTotal);
                        fprintf(statsfile,"    \"packets-lost\":%d,\n", stats.pktRcvLossTotal);
                        fprintf(statsfile,"    \"packets-retransmitted\":%d,\n", stats.pktRetransTotal);
                        fprintf(statsfile,"    \"packets-dropped\":%d,\n", stats.pktRcvDropTotal);
                        fprintf(statsfile,"    \"loss-percentage\":%.2f,\n", stats.pktRecvTotal > 0 ? (double)stats.pktRcvDropTotal / (double)stats.pktRecvTotal * (double)100.0 : 0.0);
                        fprintf(statsfile,"    \"bitrate-kbps\":%.2f,\n", (double)stats.mbpsRecvRate * (double)1000.0);
                        fprintf(statsfile,"    \"latencyms\":%d,\n", latencyms);
                        fprintf(statsfile,"    \"transport-stream-id\":%d,\n", decode->pat_transport_stream_id);
                        fprintf(statsfile,"    \"rtt\":%.2f\n", (double)stats.msRTT);
                        fprintf(statsfile,"}\n");
                        fclose(statsfile);
                    }
                }
            }
            update_stats++;
            tp = recvbytes / 188;

            if (check_for_rtp(recvbytes)) {
                uint8_t *updated_buffer = (uint8_t*)buffer+12;
                decode_packets((uint8_t*)updated_buffer, tp, decode, 0);
            } else {
                decode_packets((uint8_t*)buffer, tp, decode, 0);
            }

            {
                dataqueue_message_struct *msg;
                uint8_t *obuffer;

                msg = (dataqueue_message_struct*)memory_take(srtcore->msgpool, threadid);
                if (msg) {
                    memset(msg, 0, sizeof(dataqueue_message_struct));
                    obuffer = (uint8_t*)memory_take(srtcore->packetpool, threadid);
                    if (obuffer) {
                        if (recvbytes > MAX_PACKET_BUFFER_SIZE) {
                            recvbytes = MAX_PACKET_BUFFER_SIZE;
                        }
                        if (check_for_rtp(recvbytes)) {
                            uint8_t *updated_buffer = (uint8_t*)buffer+12;
                            memcpy(obuffer, updated_buffer, recvbytes-12);
                            msg->buffer_size = recvbytes-12;
                        } else {
                            memcpy(obuffer, buffer, recvbytes);
                            msg->buffer_size = recvbytes;
                        }
                        msg->buffer = obuffer;
                        msg->pts = srtcontrol.srctime;
                        dataqueue_put_front(srtcore->udpserverqueue, msg);
                    } else {
                        fprintf(stderr,"srt_receiver_thread_caller: packet buffers exhausted\n");
                        memory_return(srtcore->msgpool, msg);
                        msg = NULL;
                    }
                } else {
                    fprintf(stderr,"srt_receiver_thread_caller: msg buffers exhausted\n");
                }
                msg = NULL;
            }
        }
    }

cleanup_srt_receiver_thread_caller:
    destroy_transport_data(decode);
    decode = NULL;
    free(srtdata);
    srtdata = NULL;
    if (epollid > 0) {
        srt_epoll_release(epollid);
        epollid = 0;
    }
    srt_close(serversock);
    srt_cleanup();

    return NULL;
}

static void *srt_server_worker_input_thread(void *context)
{
    return NULL;
}

static void *srt_server_worker_output_thread(void *context)
{
    srt_server_worker_output_thread_struct *srtdata;
    srthub_core_struct *srtcore;
    dataqueue_message_struct *msg;
    SRTSOCKET clientsock;
    int thread;
    int32_t msgno = 1;
    uint32_t update_stats;
    int sent_bytes;
    int srt_connected = 0;
    int active_workers = 0;
    int m;
    char statsfilename[MAX_STRING_SIZE];
    int64_t diff;
    int64_t total_bytes_sent = 0;
    int64_t total_packets_sent = 0;
    struct timespec server_time_stop;
    struct timespec server_time_start;

    srtdata = (srt_server_worker_output_thread_struct*)context;
    srtcore = srtdata->core;

    thread = srtdata->thread;
    clientsock = srtdata->client_sock;

    for (m = 0; m < MAX_WORKER_THREADS; m++) {
        active_workers += srtcore->srt_server_worker_thread_running[m];
    }

    update_stats = 0;
    srt_connected = 1;

    sprintf(statsfilename,"/opt/srthub/status/srt_server_thread_%d_%d.json", thread, srtcore->session_identifier);

    clock_gettime(CLOCK_MONOTONIC, &server_time_start);
    while (srtcore->srt_server_worker_thread_running[thread]) {
        msg = (dataqueue_message_struct*)dataqueue_take_back_wait(srtcore->srtserverqueue[thread], &srtcore->srt_server_worker_thread_running[thread]);

        if (!srtcore->srt_server_worker_thread_running[thread]) {
            if (msg) {
                uint8_t *buffer = (uint8_t*)msg->buffer;
                memory_return(srtcore->packetpool, buffer);
                memory_return(srtcore->msgpool, msg);
                buffer = NULL;
                msg = NULL;
            }
            goto cleanup_srt_server_worker_output_thread;
        }

        uint8_t *buffer = (uint8_t*)msg->buffer;
        int buffer_size = msg->buffer_size;
        SRT_MSGCTRL srtcontrol;

        memset(&srtcontrol, 0, sizeof(srtcontrol));
        if (msgno <= 0 || msgno > 67108863) {
            msgno = 1;
        }
        srtcontrol.msgno = msgno++;
        srtcontrol.srctime = msg->pts;
        srtcontrol.msgttl = -1;

        clock_gettime(CLOCK_MONOTONIC, &server_time_stop);
        diff = realtime_clock_difference(&server_time_stop, &server_time_start) / 1000;
        if (diff >= 1000) {
            FILE *statsfile = fopen(statsfilename,"wb");
            if (statsfile) {
                fprintf(statsfile,"{\n");
                fprintf(statsfile,"    \"thread\":%d,\n", thread);
                fprintf(statsfile,"    \"client-address\":\"%s\",\n", srtdata->client_address);
                fprintf(statsfile,"    \"client-port\":%d,\n", srtdata->client_port);
                fprintf(statsfile,"    \"total-bytes-sent\":%ld,\n", total_bytes_sent);
                fprintf(statsfile,"    \"total-packets-sent\":%ld\n", total_packets_sent);
                fprintf(statsfile,"}\n");
                fclose(statsfile);
                clock_gettime(CLOCK_MONOTONIC, &server_time_start);
            }
        }

        sent_bytes = srt_sendmsg2(clientsock, (char*)buffer, buffer_size, &srtcontrol);
        if (sent_bytes < 0) {
            int lasterr = srt_getlasterror(NULL);
            if (lasterr == SRT_ECONNLOST) {
                fprintf(stderr,"srt_server_worker_output_thread: SRT connection has been lost!\n");
                srt_connected = 0;
                send_signal(srtcore, SIGNAL_SRT_CONNECTION_LOST, "SRT connection lost");
                //send_restart_message(srtcore);
                unlink(statsfilename);
                memory_return(srtcore->packetpool, buffer);
                memory_return(srtcore->msgpool, msg);
                buffer = NULL;
                msg = NULL;
                goto cleanup_srt_server_worker_output_thread;
            } else if (lasterr == SRT_EASYNCRCV) {

            } else {
                fprintf(stderr,"srt_server_worker_output_thread: SRT unknown error: %s\n", srt_getlasterror_str());
            }
            usleep(10000);
        } else if (sent_bytes == 0) {
            usleep(10000);
        } else {
            total_packets_sent++;
            total_bytes_sent += sent_bytes;
        }
        memory_return(srtcore->packetpool, buffer);
        memory_return(srtcore->msgpool, msg);
        buffer = NULL;
        msg = NULL;
    }

cleanup_srt_server_worker_output_thread:
    srtcore->srt_server_worker_thread_running[thread] = 0;
    pthread_mutex_lock(srtcore->srtserverlock);
    msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->srtserverqueue[thread]);
    while (msg) {
        uint8_t *buffer = (uint8_t*)msg->buffer;
        memory_return(srtcore->packetpool, buffer);
        memory_return(srtcore->msgpool, msg);
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->srtserverqueue[thread]);
    }
    dataqueue_destroy(srtcore->srtserverqueue[thread]);
    srtcore->srtserverqueue[thread] = NULL;
    pthread_mutex_unlock(srtcore->srtserverlock);
    srt_close(clientsock);
    unlink(statsfilename);
    free(srtdata);
    return NULL;
}

static void *srt_server_thread_pull(void *context)
{
    srt_server_thread_struct *srtdata;
    srthub_core_struct *srtcore;
    dataqueue_message_struct *msg;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    int srterr;
    int thread = 0;
    int slots_available = 0;
    int64_t total_bytes_sent = 0;
    int64_t total_packets_sent = 0;

    srt_startup();

    srtdata = (srt_server_thread_struct*)context;
    srtcore = srtdata->core;

    listener = srt_bind_listener_retrying(srtcore, &srtcore->srt_server_thread_running,
                                          srtdata->server_address, srtdata->server_port, NULL,
                                          srtdata->passphrase, srtdata->streamid);
    if (listener == SRT_INVALID_SOCK) {
        /* stopped while the bind was still failing */
        goto cleanup_srt_server_thread_pull;
    }

    while (srtcore->srt_server_thread_running) {
        srterr = srt_listen(listener, MAX_WORKER_THREADS);
        if (srterr == SRT_ERROR) {
            char signal_message[MAX_STRING_SIZE];
            snprintf(signal_message, MAX_STRING_SIZE-1, "SRT Unable to Listen on %s:%d (%s), restarting",
                     srtdata->server_address, srtdata->server_port, srt_getlasterror_str());
            fprintf(stderr,"srt_server_thread: %s\n", signal_message);
            send_signal(srtcore, SIGNAL_SRT_LISTEN_FAILED, signal_message);
            usleep(SRT_LISTEN_RETRY_SECONDS * 1000000);
            send_restart_message(srtcore);
            goto cleanup_srt_server_thread_pull;
        }

        struct sockaddr_in client_addr;
        int addrlen = sizeof(client_addr);
        SRTSOCKET client_sock = srt_accept(listener,
                                           (struct sockaddr*)&client_addr,
                                           &addrlen);

        if (client_sock == SRT_INVALID_SOCK) {
            // flag the error
            goto cleanup_srt_server_thread_pull;
        }

        struct sockaddr_in *sa4 = (struct sockaddr_in*)&client_addr;
        char ipaddr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(sa4->sin_addr), ipaddr, sizeof(ipaddr));
        fprintf(stderr,"srt_server_thread: client connected from %s, port %d\n", ipaddr, client_addr.sin_port);

        char signal_message[MAX_STRING_SIZE];
        snprintf(signal_message, MAX_STRING_SIZE-1, "Accepted SRT Connection Request From %s:%d",
                 ipaddr, client_addr.sin_port);
        send_signal(srtcore, SIGNAL_SRT_CONNECTED, signal_message);

        // check the whitelist here, otherwise close it up and move on if it doesn't match

        slots_available = MAX_WORKER_THREADS;
        for (thread = 0; thread < MAX_WORKER_THREADS; thread++) {
            void *serverqueue;

            pthread_mutex_lock(srtcore->srtserverlock);
            serverqueue = srtcore->srtserverqueue[thread];
            pthread_mutex_unlock(srtcore->srtserverlock);

            if (serverqueue == NULL) {
                srt_server_worker_output_thread_struct *srtoutputdata;

                pthread_mutex_lock(srtcore->srtserverlock);
                srtcore->srtserverqueue[thread] = dataqueue_create();
                pthread_mutex_unlock(srtcore->srtserverlock);

                srtoutputdata = (srt_server_worker_output_thread_struct*)malloc(sizeof(srt_server_worker_output_thread_struct));
                srtoutputdata->core = srtcore;
                srtoutputdata->thread = thread;
                srtoutputdata->client_sock = client_sock;
                snprintf(srtoutputdata->client_address, MAX_STRING_SIZE-1, "%s", ipaddr);
                srtoutputdata->client_port = client_addr.sin_port;

                srtcore->srt_server_worker_thread_running[thread] = 1;
                pthread_create(&srtcore->srt_server_worker_thread_id[thread], NULL, srt_server_worker_output_thread, srtoutputdata);
                pthread_detach(srtcore->srt_server_worker_thread_id[thread]);

                break;
            } else {
                slots_available--;
            }
        }
        if (slots_available == 0) {
            // signal that we rejected a connection due to not enough server capacity
            srt_close(client_sock);
        }
    }

cleanup_srt_server_thread_pull:
    for (thread = 0; thread < MAX_WORKER_THREADS; thread++) {
        srtcore->srt_server_worker_thread_running[thread] = 0;
    }
    if (listener != SRT_INVALID_SOCK) {
        srt_close(listener);
    }
    srt_cleanup();

    return NULL;
}

static void *srt_server_thread_push(void *context)
{
    srt_server_thread_struct *srtdata;
    srthub_core_struct *srtcore;
    dataqueue_message_struct *msg;
    SRTSOCKET sendersock = SRT_INVALID_SOCK;
    struct sockaddr_in server_addr;
    struct in_addr remote_address;
    int32_t msgno = 1;
    int no = 0;
    int srterr;
    int thread = 0;
    int slots_available = 0;
    int stats_size = 0;
    int update_stats = 0;
    int sent_bytes;
    int64_t diff;
    int64_t total_bytes_sent = 0;
    int64_t total_packets_sent = 0;
    SRT_TRACEBSTATS stats;
    char statsfilename[MAX_STRING_SIZE];
    struct timespec server_time_stop;
    struct timespec server_time_start;
    int max_srt_packet_size = MAX_SRT_PACKET_SIZE;

    srt_startup();

    srtdata = (srt_server_thread_struct*)context;
    srtcore = srtdata->core;

retry_srt_server_push_connection:
    sendersock = srt_create_socket();
    if (sendersock == SRT_ERROR) {
        // flag the error
        srt_cleanup();
        return NULL;
    }

    inet_aton(srtdata->server_address, &remote_address);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(srtdata->server_port);
    server_addr.sin_addr.s_addr = remote_address.s_addr;

    srterr = srt_setsockflag(sendersock, SRTO_SNDSYN, &no, sizeof(no));
    if (srterr == SRT_ERROR) {
        // srt_getlasterror_str();
        fprintf(stderr,"srt_server_thread_push: unable to proceed with srt_setsockflag()\n");
        goto cleanup_srt_server_thread_push;
    }

    srterr = srt_setsockflag(sendersock, SRTO_RCVSYN, &no, sizeof(no));
    if (srterr == SRT_ERROR) {
        // srt_getlasterror_str();
        fprintf(stderr,"srt_server_thread_push: unable to proceed with srt_setsockflag()\n");
        goto cleanup_srt_server_thread_push;
    }

    int passphrase_length = strlen(srtdata->passphrase);
    if (passphrase_length >= 10 && passphrase_length <= 79) {
        srterr = srt_setsockflag(sendersock, SRTO_PASSPHRASE, srtdata->passphrase, passphrase_length);
        if (srterr == SRT_ERROR) {
            // srt_getlasterror_str();
            fprintf(stderr,"srt_server_thread_push: unable to proceed with srt_setsockflag()\n");
            goto cleanup_srt_server_thread_push;
        }
    }

    int streamid_length = strlen(srtdata->streamid);
    if (streamid_length > 0) {
        srterr = srt_setsockflag(sendersock, SRTO_STREAMID, srtdata->streamid, streamid_length);
        if (srterr == SRT_ERROR) {
            // srt_getlasterror_str();
            fprintf(stderr,"srt_server_thread_push: unable to proceed with srt_setsockflag()\n");
            goto cleanup_srt_server_thread_push;
        }
    }

    srterr = srt_setsockflag(sendersock, SRTO_PAYLOADSIZE, &max_srt_packet_size, sizeof(max_srt_packet_size));
    if (srterr == SRT_ERROR) {
        fprintf(stderr,"srt_server_thread: unable to proceed with srt_setsockflag()\n");
        goto cleanup_srt_server_thread_push;
    }

    /*
    srterr = srt_bind(sendersock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (srterr == SRT_ERROR) {

        // flag the error
        free(srtdata);
        srt_close(sendersock);
        srt_cleanup();
        return NULL;
    }

    sa.sin_family = AF_INET;
    sa.sin_port = htons(srtdata->server_port);

    srterr = inet_pton(AF_INET, srtdata->server_address, &sa.sin_addr);
    if (srterr != 1) {
        goto cleanup_srt_receiver_thread_caller;
    }
    */

    fprintf(stderr,"srt_server_thread_push: attempting to connect to %s:%d\n",
            srtdata->server_address,
            srtdata->server_port);

    srterr = srt_connect(sendersock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (srterr == SRT_ERROR) {
        fprintf(stderr,"srt_server_thread_push: unable to proceed with srt_connect, error=%s\n", srt_getlasterror_str());
        srt_close(sendersock);
        sendersock = SRT_INVALID_SOCK;
        usleep(1000000);
        goto retry_srt_server_push_connection;
    }

    fprintf(stderr,"srt_server_thread_push: finished with srt_connect(), serversock=%d\n", sendersock);

    sprintf(statsfilename,"/opt/srthub/status/srt_server_thread_%d_%d.json", thread, srtcore->session_identifier);

    fprintf(stderr,"srt_server_thread_push: creating srtserverqeueu\n");
    pthread_mutex_lock(srtcore->srtserverlock);
    srtcore->srtserverqueue[thread] = dataqueue_create();
    pthread_mutex_unlock(srtcore->srtserverlock);

    stats_size = sizeof(stats);
    update_stats = 0;
    thread = 0; // we're only pushing to one place for now

    fprintf(stderr,"srt_server_thread_push: starting main thread loop\n");

    clock_gettime(CLOCK_MONOTONIC, &server_time_start);
    while (srtcore->srt_server_thread_running) {
        pthread_mutex_lock(srtcore->srtserverlock);
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->srtserverqueue[thread]);
        pthread_mutex_unlock(srtcore->srtserverlock);

        while (!msg && srtcore->srt_server_thread_running) {
            usleep(1000);
            pthread_mutex_lock(srtcore->srtserverlock);
            msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->srtserverqueue[thread]);
            pthread_mutex_unlock(srtcore->srtserverlock);
        }

        if (!srtcore->srt_server_thread_running) {
            if (msg) {
                uint8_t *buffer = (uint8_t*)msg->buffer;
                memory_return(srtcore->packetpool, buffer);
                memory_return(srtcore->msgpool, msg);
                buffer = NULL;
                msg = NULL;
            }
            goto cleanup_srt_server_thread_push;
        }

        uint8_t *buffer = (uint8_t*)msg->buffer;
        int buffer_size = msg->buffer_size;
        SRT_MSGCTRL srtcontrol;

        memset(&srtcontrol, 0, sizeof(srtcontrol));
        if (msgno <= 0 || msgno > 67108863) {
            msgno = 1;
        }
        srtcontrol.msgno = msgno++;
        srtcontrol.srctime = msg->pts;
        srtcontrol.msgttl = -1;

        clock_gettime(CLOCK_MONOTONIC, &server_time_stop);
        diff = realtime_clock_difference(&server_time_stop, &server_time_start) / 1000;
        if (diff >= 1000) {
            FILE *statsfile = fopen(statsfilename,"wb");
            if (statsfile) {
                fprintf(statsfile,"{\n");
                fprintf(statsfile,"    \"thread\":%d,\n", thread);
                fprintf(statsfile,"    \"client-address\":\"%s\",\n", srtdata->server_address);
                fprintf(statsfile,"    \"client-port\":%d,\n", srtdata->server_port);
                fprintf(statsfile,"    \"total-bytes-sent\":%ld,\n", total_bytes_sent);
                fprintf(statsfile,"    \"total-packets-sent\":%ld\n", total_packets_sent);
                fprintf(statsfile,"}\n");
                fclose(statsfile);
                clock_gettime(CLOCK_MONOTONIC, &server_time_start);
            }
        }

        sent_bytes = srt_sendmsg2(sendersock, (char*)buffer, buffer_size, &srtcontrol);
        if (sent_bytes < 0) {
            int lasterr = srt_getlasterror(NULL);
            if (lasterr == SRT_ECONNLOST) {
                fprintf(stderr,"srt_server_thread_push: SRT connection has been lost!\n");
                //srt_connected = 0;
                send_signal(srtcore, SIGNAL_SRT_CONNECTION_LOST, "SRT connection lost");
                send_restart_message(srtcore);
                unlink(statsfilename);
                memory_return(srtcore->packetpool, buffer);
                memory_return(srtcore->msgpool, msg);
                buffer = NULL;
                msg = NULL;
                goto cleanup_srt_server_thread_push;
            } else if (lasterr == SRT_EASYNCRCV) {

            } else if (lasterr == SRT_ENOCONN) {
                usleep(1000000);
                fprintf(stderr,"srt_server_thread_push: SRT connection having issues, attempting to connect again to %s:%d\n",
                        srtdata->server_address,
                        srtdata->server_port);

                srterr = srt_connect(sendersock, (struct sockaddr*)&server_addr, sizeof(server_addr));
                if (srterr == SRT_ERROR) {
                    void *localqueue;
                    uint8_t *localbuffer;

                    memory_return(srtcore->packetpool, buffer);
                    memory_return(srtcore->msgpool, msg);
                    buffer = NULL;
                    msg = NULL;

                    fprintf(stderr,"srt_server_thread_push: unable to proceed with srt_connect, error=%s\n", srt_getlasterror_str());
                    if (sendersock != SRT_INVALID_SOCK) {
                        srt_close(sendersock);
                        sendersock = SRT_INVALID_SOCK;
                    }
                    pthread_mutex_lock(srtcore->srtserverlock);
                    localqueue = srtcore->srtserverqueue[thread];
                    srtcore->srtserverqueue[thread] = NULL;
                    pthread_mutex_unlock(srtcore->srtserverlock);

                    msg = (dataqueue_message_struct*)dataqueue_take_back(localqueue);
                    while (msg) {
                        localbuffer = (uint8_t*)msg->buffer;
                        memory_return(srtcore->packetpool, localbuffer);
                        memory_return(srtcore->msgpool, msg);
                        msg = (dataqueue_message_struct*)dataqueue_take_back(localqueue);
                    }
                    dataqueue_destroy(localqueue);

                    usleep(1000000);
                    goto retry_srt_server_push_connection;
                }
                clock_gettime(CLOCK_MONOTONIC, &server_time_start);
            } else {
                fprintf(stderr,"srt_server_thread_push: SRT unknown error: %s\n", srt_getlasterror_str());
                // need to handle this error
            }
            usleep(10000);
        } else if (sent_bytes == 0) {
            usleep(10000);
        } else {
            total_packets_sent++;
            total_bytes_sent += sent_bytes;
        }
        memory_return(srtcore->packetpool, buffer);
        memory_return(srtcore->msgpool, msg);
        buffer = NULL;
        msg = NULL;
    }

cleanup_srt_server_thread_push:

    pthread_mutex_lock(srtcore->srtserverlock);
    msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->srtserverqueue[thread]);
    while (msg) {
        uint8_t *buffer = (uint8_t*)msg->buffer;
        memory_return(srtcore->packetpool, buffer);
        memory_return(srtcore->msgpool, msg);
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->srtserverqueue[thread]);
    }
    dataqueue_destroy(srtcore->srtserverqueue[thread]);
    srtcore->srtserverqueue[thread] = NULL;
    pthread_mutex_unlock(srtcore->srtserverlock);
    if (sendersock != SRT_INVALID_SOCK) {
        srt_close(sendersock);
    }
    srt_cleanup();

    return NULL;
}

static void *udp_receiver_thread(void *context)
{
    udp_receiver_thread_struct *udpdata;
    srthub_core_struct *srtcore;
    dataqueue_message_struct *msg;
    fd_set sockset;
    uint8_t *udp_buffer = NULL;
    int multicast_input = 0;
    int64_t source_interruptions = 0;
    int udp_socket = 0;
    int anysignal = 0;
    int timeout_ms = 1000;
    int no_signal_count = 0;
    char signal_msg[MAX_STRING_SIZE];
    int input_signal = 0;
    char statsfilename[MAX_STRING_SIZE];
    transport_data_struct *decode = create_transport_data();
    int64_t total_bytes_received = 0;
    int64_t total_packets_received = 0;
    struct timespec receive_time_stop;
    struct timespec receive_time_start;
    struct timespec signal_check_stop;
    struct timespec signal_check_start;
    int threadid = srthub_gettid();

    if (!decode) {
        fprintf(stderr,"unable to allocate the transport stream decoder\n");
        free(context);
        return NULL;
    }

    udpdata = (udp_receiver_thread_struct*)context;
    srtcore = udpdata->core;

    udp_buffer = (uint8_t*)malloc(MAX_UDP_BUFFER_READ);
    if (!udp_buffer) {
        destroy_transport_data(decode);
        free(udpdata);
        return NULL;
    }
    multicast_input = is_multicast_address(udpdata->source_address);

    udp_socket = socket_udp_open(udpdata->interface_name,
                                 udpdata->source_address,
                                 udpdata->source_port,
                                 multicast_input, UDP_FLAG_INPUT, 1);

    // check socket
    sprintf(statsfilename,"/opt/srthub/status/udp_receiver_%d.json", srtcore->session_identifier);

    clock_gettime(CLOCK_MONOTONIC, &signal_check_start);
    while (srtcore->udp_receiver_thread_running) {
        if (no_signal_count >= 5) {
            if (udp_socket >= 0) {
                socket_udp_close(udp_socket);
            }
            udp_socket = socket_udp_open(udpdata->interface_name,
                                         udpdata->source_address,
                                         udpdata->source_port,
                                         multicast_input, UDP_FLAG_INPUT, 1);
            no_signal_count = 0;

            snprintf(signal_msg, MAX_STRING_SIZE-1, "IP %s, PORT %d, INTERFACE %s",
                     udpdata->source_address,
                     udpdata->source_port,
                     udpdata->interface_name);
            send_signal(srtcore, SIGNAL_NO_INPUT_SIGNAL, signal_msg);

            continue;
        }

        if (udp_socket < 0) {
            /* the open failed (missing interface, no socket slot, ...):
             * count it as a second without signal so the reopen below
             * retries, rather than selecting on an invalid descriptor */
            usleep(timeout_ms * 1000);
            anysignal = 0;
        } else {
            anysignal = socket_udp_ready(udp_socket, timeout_ms, &sockset);
            if (anysignal < 0) {
                usleep(10000);
                continue;
            }
        }
        if (anysignal == 0) {
            no_signal_count++;
            source_interruptions++;
            input_signal = 0;
            clear_pid_summary(srtcore);

            FILE *statsfile = fopen(statsfilename,"wb");
            if (statsfile) {
                fprintf(statsfile,"{\n");
                fprintf(statsfile,"    \"udp-source-address\":\"%s\",\n", udpdata->source_address);
                fprintf(statsfile,"    \"udp-source-port\":%d,\n", udpdata->source_port);
                fprintf(statsfile,"    \"udp-source-interface\":\"%s\",\n", udpdata->interface_name);
                fprintf(statsfile,"    \"udp-source-active\":0,\n");
                fprintf(statsfile,"    \"udp-source-kbps\":0,\n");
                fprintf(statsfile,"    \"total-bytes-received\":%ld,\n", total_bytes_received);
                fprintf(statsfile,"    \"total-packets-received\":%ld,\n", total_packets_received);
                fprintf(statsfile,"    \"multicast-input\":%d\n", multicast_input);
                fprintf(statsfile,"}\n");
                fclose(statsfile);
            }
            continue;
        }

        if (FD_ISSET(udp_socket, &sockset)) {
            int bytes_read = socket_udp_read(udp_socket, udp_buffer, MAX_UDP_BUFFER_READ);
            if (bytes_read > 0) {
                uint8_t *outputbuffer;
                int64_t source_time = srt_time_now();
                int thread;
                int tp;
                int64_t diff;
                double kbps;

                no_signal_count = 0;
                if (input_signal == 0) {
                    input_signal = 1;

                    clock_gettime(CLOCK_MONOTONIC, &receive_time_start);
                    total_bytes_received = 0;
                    total_packets_received = 0;

                    snprintf(signal_msg, MAX_STRING_SIZE-1, "Input Signal Locked To %s:%d on Interface %s",
                             udpdata->source_address,
                             udpdata->source_port,
                             udpdata->interface_name);
                    send_signal(srtcore, SIGNAL_INPUT_SIGNAL_LOCKED, signal_msg);
                }

                total_packets_received++;
                total_bytes_received += bytes_read;

                // check if rtp or something else?
                tp = bytes_read / 188;
                if (check_for_rtp(bytes_read)) {
                    uint8_t *updated_buffer = (uint8_t*)udp_buffer+12;
                    decode_packets((uint8_t*)updated_buffer, tp, decode, 0);
                } else {
                    decode_packets((uint8_t*)udp_buffer, tp, decode, 0);
                }

                clock_gettime(CLOCK_MONOTONIC, &receive_time_stop);
                diff = realtime_clock_difference(&receive_time_stop, &receive_time_start) / 1000000;
                if (diff > 0) {
                    kbps = (((double)total_bytes_received*(double)8) / (double)1000) / (double)diff;
                } else {
                    kbps = 0;
                }

                clock_gettime(CLOCK_MONOTONIC, &signal_check_stop);
                diff = realtime_clock_difference(&signal_check_stop, &signal_check_start) / 1000;
                if (diff >= 2000) {  // 2 second timeout
                    /* published by the main loop in corestatus */
                    publish_pid_summary(srtcore, decode);

                    FILE *statsfile = fopen(statsfilename,"wb");
                    if (statsfile) {
                        fprintf(statsfile,"{\n");
                        fprintf(statsfile,"    \"udp-source-address\":\"%s\",\n", udpdata->source_address);
                        fprintf(statsfile,"    \"udp-source-port\":%d,\n", udpdata->source_port);
                        fprintf(statsfile,"    \"udp-source-interface\":\"%s\",\n", udpdata->interface_name);
                        fprintf(statsfile,"    \"udp-source-active\":1,\n");
                        fprintf(statsfile,"    \"udp-source-kbps\":%.2f,\n", kbps);
                        fprintf(statsfile,"    \"total-bytes-received\":%ld,\n", total_bytes_received);
                        fprintf(statsfile,"    \"total-packets-received\":%ld,\n", total_packets_received);
                        fprintf(statsfile,"    \"multicast-input\":%d\n", multicast_input);
                        fprintf(statsfile,"}\n");
                        fclose(statsfile);
                    }
                    clock_gettime(CLOCK_MONOTONIC, &signal_check_start);
                }

                for (thread = 0; thread < MAX_WORKER_THREADS; thread++) {
                    pthread_mutex_lock(srtcore->srtserverlock);
                    if (srtcore->srtserverqueue[thread] != NULL) {
                        outputbuffer = (uint8_t*)memory_take(srtcore->packetpool, threadid);
                        if (outputbuffer) {
                            int updated_bytes_read = 0;
                            if (bytes_read > MAX_PACKET_BUFFER_SIZE) {
                                bytes_read = MAX_PACKET_BUFFER_SIZE;
                            }
                            if (check_for_rtp(bytes_read)) {
                                uint8_t *updated_buffer = (uint8_t*)udp_buffer+12;
                                memcpy(outputbuffer, updated_buffer, bytes_read-12);
                                updated_bytes_read = bytes_read-12;
                            } else {
                                memcpy(outputbuffer, udp_buffer, bytes_read);
                                updated_bytes_read = bytes_read;
                            }
                            msg = (dataqueue_message_struct*)memory_take(srtcore->msgpool, threadid);
                            if (msg) {
                                memset(msg, 0, sizeof(dataqueue_message_struct));
                                msg->buffer = outputbuffer;
                                msg->buffer_size = updated_bytes_read;
                                msg->pts = source_time;
                                dataqueue_put_front(srtcore->srtserverqueue[thread], msg);
                                msg = NULL;
                            } else {
                                fprintf(stderr,"udp_receiver_thread: msg buffers exhausted, srtserverqueue=%d\n", dataqueue_get_size(srtcore->srtserverqueue[thread]));
                                memory_return(srtcore->packetpool, outputbuffer);
                                outputbuffer = NULL;
                            }
                        }
                    }
                    pthread_mutex_unlock(srtcore->srtserverlock);
                }
            }
        }
    }
// cleanup_udp_receiver_thread:
    if (udp_socket >= 0) {
        socket_udp_close(udp_socket);
    }
    free(udp_buffer);
    destroy_transport_data(decode);
    free(udpdata);

    return NULL;
}

static void *output_smoothing_thread(void *context)
{
    output_smoothing_thread_struct *cbrdata;
    srthub_core_struct *srtcore;
    dataqueue_message_struct *msg;
    int64_t start_pcr = 0;
    int64_t anchor_pcr = 0;
    int64_t incoming_count = 0;
    int64_t pcr_position = 0;
    int64_t base_count = 0;

    while (srtcore->output_smoothing_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->smoothingqueue);

        while (!msg && srtcore->output_smoothing_thread_running) {
            usleep(1000);
            msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->smoothingqueue);
        }

        if (!srtcore->output_smoothing_thread_running) {
            if (msg) {
                uint8_t *buffer = (uint8_t*)msg->buffer;
                int buffer_size = msg->buffer_size;
                int tp = buffer_size / 188;
                int cp;
                int pid;
                int afc;
                int size;
                int64_t original_pcr;
                int64_t original_pcr_remainder;
                int64_t smooth_pcr;
                int64_t smooth_pcr_remainder;
                int64_t pcr_diff;
                int64_t smooth_pcr_position;

                for (cp = 0; cp < tp; cp++) {
                    if (buffer[0] == 0x47) {
                        pid = ((((uint16_t)buffer[1] << 8) | (uint16_t)buffer[2]) & 0x1fff);

                        if (pid == 8191) {
                            continue;
                        }
                        afc = ((buffer[3] >> 4) & 0x03);
                        size = 0;
                        if (afc & 2) {
                            if (afc == 2) {
                                size = 183;
                            } else {
                                size = buffer[4];
                            }
                            if (size > 0) {
                                original_pcr = buffer[6];
                                original_pcr = ((original_pcr << 8) | buffer[7]);
                                original_pcr = ((original_pcr << 8) | buffer[8]);
                                original_pcr = ((original_pcr << 8) | buffer[9]);
                                original_pcr = original_pcr << 1;
                                if ((buffer[10] & 0x80) != 0) {
                                    original_pcr = original_pcr | 1;
                                }
                                original_pcr_remainder = ((buffer[10] & 0x01) << 8);
                                original_pcr_remainder = original_pcr_remainder | buffer[11];

                                if (anchor_pcr == 0) {
                                    anchor_pcr = ((int64_t)original_pcr * (int64_t)300) + (int64_t)original_pcr_remainder;
                                    start_pcr = anchor_pcr;
                                    pcr_position = ((int64_t)incoming_count * (int64_t)188) + (int64_t)10;
                                    base_count = (int64_t)((((double)original_pcr * (double)300.0 * ((double)cbrdata->bitrate / (double)1000000.0) / (double)216.0) - (double)10.0) / (double)188.0);
                                    continue;
                                }

                                smooth_pcr = ((int64_t)original_pcr * (int64_t)300) + (int64_t)original_pcr_remainder;
                                smooth_pcr_position = 0;
                                fprintf(stderr,"output_smoothing_thread: original_pcr:%ld\n", original_pcr);
                            }
                        }
                    }
                }

                memory_return(srtcore->packetpool, buffer);
                memory_return(srtcore->msgpool, msg);
                buffer = NULL;
                msg = NULL;
            }
            goto cleanup_output_smoothing_thread;
        }

    }
cleanup_output_smoothing_thread:
    return NULL;
}

static void *udp_server_thread(void *context)
{
    udp_server_thread_struct *udpdata;
    srthub_core_struct *srtcore;
    char host[NI_MAXHOST];
    int sin_family = 0;
    struct in_addr output_address;
    struct in_addr interface_address;
    struct sockaddr_in bind_address;
    struct sockaddr_in destination;
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;
    int output_socket = 0;
    int yes = 1;
    dataqueue_message_struct *msg;
    char statsfilename[MAX_STRING_SIZE];
    struct timespec stats_start;
    struct timespec stats_stop;
    struct timespec signal_check_start;
    struct timespec signal_check_stop;
    int64_t diff;
    int64_t total_bytes_sent = 0;
    int64_t total_packets_sent = 0;
    int multicast_output = 0;
    int signal_outage_flagged = 0;

    sprintf(host,"127.0.0.1");

    udpdata = (udp_server_thread_struct*)context;
    srtcore = udpdata->core;

    output_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (output_socket < 0) {
        fprintf(stderr,"udp_server_thread: unable to create the output socket\n");
    }
    setsockopt(output_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    inet_aton(udpdata->destination_address, &output_address);
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = output_address.s_addr;
    destination.sin_port = htons(udpdata->destination_port);

    if (getifaddrs(&ifaddr) != 0) {
        ifaddr = NULL;
    }
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family) {
            sin_family = ifa->ifa_addr->sa_family;
        }
        if ((!strcasecmp(ifa->ifa_name,udpdata->interface_name)) && (sin_family == AF_INET)) {
            getnameinfo(ifa->ifa_addr,
                        sizeof(struct sockaddr_in),
                        host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            break;
        }
    }
    if (ifaddr) {
        freeifaddrs(ifaddr);
    }
    inet_aton(host, &interface_address);
    memset(&bind_address, 0, sizeof(bind_address));
    bind_address.sin_port = htons(INADDR_ANY);
    bind_address.sin_addr.s_addr = interface_address.s_addr;
    bind_address.sin_family = AF_INET;

    bind(output_socket, (struct sockaddr *)&bind_address, sizeof(bind_address));

    if (is_multicast_address(udpdata->destination_address)) {
        setsockopt(output_socket, IPPROTO_IP, IP_MULTICAST_IF, (char*)&interface_address, sizeof(interface_address));
        setsockopt(output_socket, IPPROTO_IP, IP_MULTICAST_TTL, &udpdata->ttl, sizeof(udpdata->ttl));
        multicast_output = 1;
    } else {
        multicast_output = 0;
    }

    sprintf(statsfilename,"/opt/srthub/status/udp_server_%d.json", srtcore->session_identifier);

    clock_gettime(CLOCK_MONOTONIC, &stats_start);
    clock_gettime(CLOCK_MONOTONIC, &signal_check_start);
    while (srtcore->udp_server_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back_wait(srtcore->udpserverqueue, &srtcore->udp_server_thread_running);

        if (!msg && srtcore->udp_server_thread_running) {
            clock_gettime(CLOCK_MONOTONIC, &signal_check_stop);
            diff = realtime_clock_difference(&signal_check_stop, &signal_check_start) / 1000;
            if (diff >= 2000) {
                FILE *statsfile = fopen(statsfilename,"wb");
                if (statsfile) {
                    fprintf(statsfile,"{\n");
                    fprintf(statsfile,"    \"udp-output-address\":\"%s\",\n", udpdata->destination_address);
                    fprintf(statsfile,"    \"udp-output-port\":%d,\n", udpdata->destination_port);
                    fprintf(statsfile,"    \"udp-output-interface\":\"%s\",\n", udpdata->interface_name);
                    fprintf(statsfile,"    \"udp-output-ttl\":%d,\n", udpdata->ttl);
                    fprintf(statsfile,"    \"udp-output-active\":0,\n");
                    fprintf(statsfile,"    \"total-bytes-sent\":%ld,\n", total_bytes_sent);
                    fprintf(statsfile,"    \"total-packets-sent\":%ld,\n", total_packets_sent);
                    fprintf(statsfile,"    \"last-buffer-size\":%d,\n", 0);
                    fprintf(statsfile,"    \"multicast-output\":%d,\n", multicast_output);
                    fprintf(statsfile,"    \"udpserver-queue\":%d\n", dataqueue_get_size(srtcore->udpserverqueue));
                    fprintf(statsfile,"}\n");
                    fclose(statsfile);
                }
                if (!signal_outage_flagged) {
                    signal_outage_flagged = 1;
                    send_signal(srtcore, SIGNAL_NO_DATA, "No Data on SRT Connection");
                }
                clock_gettime(CLOCK_MONOTONIC, &signal_check_start);
            }
            continue;
        }

        if (!srtcore->udp_server_thread_running) {
            if (msg) {
                uint8_t *buffer = (uint8_t*)msg->buffer;
                memory_return(srtcore->packetpool, buffer);
                memory_return(srtcore->msgpool, msg);
                buffer = NULL;
                msg = NULL;
            }
            goto cleanup_udp_server_thread;
        }

        if (msg) {
            uint8_t *buffer = (uint8_t*)msg->buffer;
            int buffer_size = msg->buffer_size;
            int buffer_type = msg->buffer_type;
            int64_t srctime = msg->pts;
            int ret;

            int boutput;
            int64_t scheduled_now = srt_time_now();

            //fprintf(stderr,"udp_server_thread: now:%ld srctime:%ld diff:%ld\n", scheduled_now, srctime, scheduled_now-srctime);

            boutput = sendto(output_socket, buffer, buffer_size, 0, (struct sockaddr *)&destination, sizeof(struct sockaddr_in));

            if (boutput == buffer_size) {
                signal_outage_flagged = 0;
                total_bytes_sent += boutput;
                total_packets_sent++;
                clock_gettime(CLOCK_MONOTONIC, &stats_stop);
                diff = realtime_clock_difference(&stats_stop, &stats_start) / 1000;
                if (diff >= 1000) {
                    FILE *statsfile = fopen(statsfilename,"wb");
                    if (statsfile) {
                        fprintf(statsfile,"{\n");
                        fprintf(statsfile,"    \"udp-output-address\":\"%s\",\n", udpdata->destination_address);
                        fprintf(statsfile,"    \"udp-output-port\":%d,\n", udpdata->destination_port);
                        fprintf(statsfile,"    \"udp-output-interface\":\"%s\",\n", udpdata->interface_name);
                        fprintf(statsfile,"    \"udp-output-ttl\":%d,\n", udpdata->ttl);
                        fprintf(statsfile,"    \"udp-output-active\":1,\n");
                        fprintf(statsfile,"    \"total-bytes-sent\":%ld,\n", total_bytes_sent);
                        fprintf(statsfile,"    \"total-packets-sent\":%ld,\n", total_packets_sent);
                        fprintf(statsfile,"    \"last-buffer-size\":%d,\n", buffer_size);
                        fprintf(statsfile,"    \"multicast-output\":%d,\n", multicast_output);
                        fprintf(statsfile,"    \"udpserver-queue\":%d\n", dataqueue_get_size(srtcore->udpserverqueue));
                        fprintf(statsfile,"}\n");
                        fclose(statsfile);
                    }
                    clock_gettime(CLOCK_MONOTONIC, &stats_start);
                }
            } else {
                clock_gettime(CLOCK_MONOTONIC, &stats_stop);
                diff = realtime_clock_difference(&stats_stop, &stats_start) / 1000;
                if (diff >= 1000) {
                    FILE *statsfile = fopen(statsfilename,"wb");
                    if (statsfile) {
                        fprintf(statsfile,"{\n");
                        fprintf(statsfile,"    \"udp-output-address\":\"%s\",\n", udpdata->destination_address);
                        fprintf(statsfile,"    \"udp-output-port\":%d,\n", udpdata->destination_port);
                        fprintf(statsfile,"    \"udp-output-interface\":\"%s\",\n", udpdata->interface_name);
                        fprintf(statsfile,"    \"udp-output-ttl\":%d,\n", udpdata->ttl);
                        fprintf(statsfile,"    \"udp-output-active\":0,\n");
                        fprintf(statsfile,"    \"total-bytes-sent\":%ld,\n", total_bytes_sent);
                        fprintf(statsfile,"    \"total-packets-sent\":%ld,\n", total_packets_sent);
                        fprintf(statsfile,"    \"last-buffer-size\":%d,\n", buffer_size);
                        fprintf(statsfile,"    \"multicast-output\":%d,\n", multicast_output);
                        fprintf(statsfile,"    \"udpserver-queue\":%d\n", dataqueue_get_size(srtcore->udpserverqueue));
                        fprintf(statsfile,"}\n");
                        fclose(statsfile);
                    }
                }
                clock_gettime(CLOCK_MONOTONIC, &stats_start);
            }

            memory_return(srtcore->packetpool, buffer);
            memory_return(srtcore->msgpool, msg);
            buffer = NULL;
            msg = NULL;
        }
    }
cleanup_udp_server_thread:
    close(output_socket);

    msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->udpserverqueue);
    while (msg) {
        uint8_t *buffer = (uint8_t*)msg->buffer;
        memory_return(srtcore->packetpool, buffer);
        memory_return(srtcore->msgpool, msg);
        buffer = NULL;
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->udpserverqueue);
    }

    return NULL;
}

static void *srthub_audio_thread(void *context)
{
    srt_audio_thread_struct *srtaudio = (srt_audio_thread_struct*)context;
    srthub_core_struct *srtcore = (srthub_core_struct*)srtaudio->core;
    const AVCodec *decode_codec = NULL;
    AVCodecContext *decode_avctx = NULL;
    AVPacket *decode_pkt = NULL;
    AVFrame *decode_av_frame = NULL;
    AVCodecParserContext *decode_parser = NULL;
    int audio_decoder_ready = 0;
    char statsfilename[MAX_STRING_SIZE];
    dataqueue_message_struct *msg;
    int audio_stream = srtaudio->audio_stream;
    char previous_samples_data[MAX_AUDIO_BUFFER_SIZE];
    int previous_samples = 0;
    struct timespec audio_start;
    struct timespec audio_stop;
    int64_t diff;

    sprintf(statsfilename,"/opt/srthub/status/audio_%d_%d.json", audio_stream, srtcore->session_identifier);

    fprintf(stderr,"srthub_audio_thread: audio_stream=%d\n", audio_stream);

    clock_gettime(CLOCK_MONOTONIC, &audio_start);
    while (srtcore->audio_decode_thread_running[audio_stream]) {
        msg = (dataqueue_message_struct*)dataqueue_take_back_wait(srtcore->audiodecodequeue[audio_stream], &srtcore->audio_decode_thread_running[audio_stream]);

        if (!srtcore->audio_decode_thread_running[audio_stream]) {
            if (msg) {
                uint8_t *buffer = (uint8_t*)msg->buffer;
                memory_return(srtcore->audiopool, buffer);
                memory_return(srtcore->msgpool, msg);
                msg = NULL;
                buffer = NULL;
            }
            goto cleanup_audio_decode_thread;
        }

        if (msg) {
            uint8_t *buffer = (uint8_t*)msg->buffer;
            int buffer_size = msg->buffer_size;
            int buffer_type = msg->buffer_type;
            int ret;
            uint8_t *data = buffer;
            int data_size = buffer_size;

            if (!audio_decoder_ready) {
                if (buffer_type == STREAM_TYPE_MPEG) {
                    decode_codec = (const AVCodec*)avcodec_find_decoder(AV_CODEC_ID_MP3);
                } else if (buffer_type == STREAM_TYPE_AAC) {
                    decode_codec = (const AVCodec*)avcodec_find_decoder(AV_CODEC_ID_AAC);
                } else if (buffer_type == STREAM_TYPE_AC3) {
                    decode_codec = (const AVCodec*)avcodec_find_decoder(AV_CODEC_ID_AC3);
                } else {
                    decode_codec = NULL;
                }

                if (decode_codec) {
                    decode_avctx = avcodec_alloc_context3(decode_codec);
                    decode_parser = av_parser_init(decode_codec->id);
                    avcodec_open2(decode_avctx, decode_codec, NULL);
                    decode_av_frame = av_frame_alloc();
                    decode_pkt = av_packet_alloc();
                    audio_decoder_ready = 1;
                }
            }
            if (!audio_decoder_ready) {
                memory_return(srtcore->audiopool, buffer);
                memory_return(srtcore->msgpool, msg);
                buffer = NULL;
                msg = NULL;

                /* Undecodable audio is dropped as fast as it arrives: the
                 * buffers come from a pool shared by every audio track, so
                 * pacing this loop would starve the decodable ones. Only the
                 * status write is paced. */
                clock_gettime(CLOCK_MONOTONIC, &audio_stop);
                diff = realtime_clock_difference(&audio_stop, &audio_start) / 1000;
                FILE *statsfile = NULL;
                if (diff >= 1000) {
                    statsfile = fopen(statsfilename,"wb");
                    clock_gettime(CLOCK_MONOTONIC, &audio_start);
                }
                if (statsfile) {
                    fprintf(statsfile,"{\n");
                    if (buffer_type == STREAM_TYPE_MPEG) {
                        fprintf(statsfile,"    \"audio-codec\":\"MPEG\",\n");
                    } else if (buffer_type == STREAM_TYPE_AAC) {
                        fprintf(statsfile,"    \"audio-codec\":\"AAC\",\n");
                    } else if (buffer_type == STREAM_TYPE_AC3) {
                        fprintf(statsfile,"    \"audio-codec\":\"AC3\",\n");
                    } else {
                        fprintf(statsfile,"    \"audio-codec\":\"Unknown\",\n");
                    }
                    fprintf(statsfile,"    \"audio-channels\":0,\n");
                    fprintf(statsfile,"    \"audio-samplerate\":0\n");
                    fprintf(statsfile,"}\n");
                    fclose(statsfile);
                }
                continue;
            }

            if (previous_samples > 0) {
                memcpy(data + data_size, previous_samples_data, previous_samples);
                data_size += previous_samples;
                previous_samples = 0;
            }

            //fprintf(stderr,"srthub_audio_thread(%d): processing audio, size=%d\n", audio_stream, data_size);
            while (data_size > 0) {
                int dret;

                ret = av_parser_parse2(decode_parser, decode_avctx, &decode_pkt->data, &decode_pkt->size,
                                       data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                if (ret < 0) {
                    memory_return(srtcore->audiopool, buffer);
                    memory_return(srtcore->msgpool, msg);
                    buffer = NULL;
                    buffer_size = 0;
                    msg = NULL;
                    data_size = 0;
                    break;
                }

                //fprintf(stderr,"srthub_audio_thread(%d): parsed audio, frame size for decode is %d(%d)\n", audio_stream, decode_pkt->size, ret);
                data += ret;
                data_size -= ret;

                if (decode_pkt->size > 0) {
                    dret = avcodec_send_packet(decode_avctx, decode_pkt);
                    if (dret < 0) {
                        fprintf(stderr,"srthub_audio_thread(%d): unable to decode audio packet, data_size=%d\n", audio_stream, data_size);
                        //memcpy(&previous_samples_data[previous_samples], data, data_size);
                        //previous_samples += data_size;
                        previous_samples = 0;
                        memory_return(srtcore->audiopool, buffer);
                        memory_return(srtcore->msgpool, msg);
                        buffer = NULL;
                        buffer_size = 0;
                        msg = NULL;
                        data_size = 0;
                        break;
                    }

                    while (dret >= 0) {
                        dret = avcodec_receive_frame(decode_avctx, decode_av_frame);
                        if (dret == AVERROR(EAGAIN) || dret == AVERROR_EOF) {
                            break;
                        } else if (dret < 0) {
                            memory_return(srtcore->audiopool, buffer);
                            memory_return(srtcore->msgpool, msg);
                            buffer = NULL;
                            buffer_size = 0;
                            msg = NULL;
                            data_size = 0;
                            break;
                        }
                        /*fprintf(stderr,"srthub_audio_thread(%d) audio sample decoded, sample_rate=%d, channels=%d, pkt_size=%d\n",
                          audio_stream, decode_av_frame->sample_rate, decode_av_frame->channels, decode_av_frame->pkt_size);*/

                        clock_gettime(CLOCK_MONOTONIC, &audio_stop);
                        diff = realtime_clock_difference(&audio_stop, &audio_start) / 1000;
                        if (diff >= 1000) {
                            FILE *statsfile = fopen(statsfilename,"wb");
                            if (statsfile) {
                                fprintf(statsfile,"{\n");
                                if (buffer_type == STREAM_TYPE_MPEG) {
                                    fprintf(statsfile,"    \"audio-codec\":\"MPEG\",\n");
                                } else if (buffer_type == STREAM_TYPE_AAC) {
                                    fprintf(statsfile,"    \"audio-codec\":\"AAC\",\n");
                                } else if (buffer_type == STREAM_TYPE_AC3) {
                                    fprintf(statsfile,"    \"audio-codec\":\"AC3\",\n");
                                } else {
                                    fprintf(statsfile,"    \"audio-codec\":\"Unknown\",\n");
                                }
                                fprintf(statsfile,"    \"audio-channels\":%d,\n", decode_av_frame->channels);
                                fprintf(statsfile,"    \"audio-samplerate\":%d\n", decode_av_frame->sample_rate);
                                fprintf(statsfile,"}\n");
                                fclose(statsfile);
                            }
                            clock_gettime(CLOCK_MONOTONIC, &audio_start);
                        }
                    }
                } // while (data_size > 0)
            }
            if (msg) {
                memory_return(srtcore->audiopool, buffer);
                memory_return(srtcore->msgpool, msg);
                buffer = NULL;
                buffer_size = 0;
                msg = NULL;
                data_size = 0;
            }
        }
    }
cleanup_audio_decode_thread:
    avcodec_free_context(&decode_avctx);
    av_parser_close(decode_parser);
    av_frame_free(&decode_av_frame);
    av_packet_free(&decode_pkt);
    free(srtaudio);
    return NULL;
}


// strip emulation-prevention bytes from up to max_out bytes of RBSP (payload after the 1-byte NAL header)
static int unescape_rbsp(const uint8_t *p, size_t n, uint8_t *out, int max_out) {
    int o = 0, zeros = 0;
    for (size_t i = 0; i < n && o < max_out; i++) {
        if (zeros >= 2 && p[i] == 0x03) { zeros = 0; continue; }
        out[o++] = p[i];
        zeros = (p[i] == 0) ? zeros + 1 : 0;
    }
    return o;
}

// minimal bit reader + Exp-Golomb
typedef struct { const uint8_t *d; int nbits, pos; } br_t;
static int br_u1(br_t *b){ if(b->pos>=b->nbits) return 0;
    int v=(b->d[b->pos>>3]>>(7-(b->pos&7)))&1; b->pos++; return v; }
static unsigned br_ue(br_t *b){ int z=0; while(b->pos<b->nbits && !br_u1(b)) z++;
    if(z>31) return 0;   /* not a valid code in 32 bits; 1u<<z would be undefined */
    unsigned v=0; for(int i=0;i<z;i++) v=(v<<1)|br_u1(b); return (1u<<z)-1u+v; }

// slice_type for a slice NAL (0..9); I-slice iff (slice_type % 5) == 2
static int h264_slice_type(const uint8_t *nal, size_t n)
{
    if (n < 2) return -1;
    uint8_t buf[16];
    int m = unescape_rbsp(nal + 1, n - 1, buf, sizeof buf);
    br_t b = { buf, m * 8, 0 };
    br_ue(&b);                 // first_mb_in_slice
    return (int)br_ue(&b);     // slice_type
}

// 1 if this SEI NAL contains a recovery_point message (payloadType 6)
static int h264_has_recovery_point(const uint8_t *nal, size_t n)
{
    if (n < 2) return 0;
    uint8_t buf[64];
    int m = unescape_rbsp(nal + 1, n - 1, buf, sizeof buf), i = 0;
    while (i < m) {
        int type = 0; while (i < m && buf[i] == 0xff) type += 255, i++;
        if (i < m) type += buf[i++]; else break;
        int size = 0; while (i < m && buf[i] == 0xff) size += 255, i++;
        if (i < m) size += buf[i++]; else break;
        if (type == 6) return 1;     // recovery_point
        i += size;
    }
    return 0;
}

#if defined(ENABLE_THUMBNAIL)
/* Formats the display aspect ratio implied by a coded frame size and its
 * sample aspect ratio, e.g. "16:9". A stream that does not signal a sample
 * aspect ratio is treated as having square pixels, which is what makes
 * 720x480 with a 8:9 sample ratio come out as 4:3 rather than 3:2.
 *
 * The reduced ratio is reported exactly rather than being snapped to the
 * nearest familiar one, so a stream coded at 1920x1088 reads 30:17 - which,
 * next to the resolution it is shown with, says what it needs to. */
static void format_aspect_ratio(char *out, int out_size, int width, int height,
                                AVRational sample_aspect, double *ratio_value)
{
    int ratio_num = 0;
    int ratio_den = 0;

    if (ratio_value) {
        *ratio_value = 0.0;
    }
    if (!out || out_size <= 0) {
        return;
    }
    snprintf(out, out_size, "%s", "unknown");

    if (width <= 0 || height <= 0) {
        return;
    }
    if (sample_aspect.num <= 0 || sample_aspect.den <= 0) {
        sample_aspect.num = 1;
        sample_aspect.den = 1;
    }

    av_reduce(&ratio_num, &ratio_den,
              (int64_t)width * (int64_t)sample_aspect.num,
              (int64_t)height * (int64_t)sample_aspect.den,
              1024 * 1024);
    if (ratio_num <= 0 || ratio_den <= 0) {
        return;
    }

    snprintf(out, out_size, "%d:%d", ratio_num, ratio_den);
    if (ratio_value) {
        *ratio_value = (double)ratio_num / (double)ratio_den;
    }
}

#define MAX_FORMAT_VALUE_SIZE 48

/* Tracks one video format property across decoded frames so that a change can
 * be reported once it is confirmed. "value" is the property itself (a ratio,
 * or a resolution) and "context" is the other one, carried along so a reported
 * change can say what the picture looked like on each side of it. */
typedef struct _format_change_struct_ {
    char current[MAX_FORMAT_VALUE_SIZE];
    char current_context[MAX_FORMAT_VALUE_SIZE];
    char pending[MAX_FORMAT_VALUE_SIZE];
    char pending_context[MAX_FORMAT_VALUE_SIZE];
    int  pending_count;
} format_change_struct;

/* Feeds one decoded frame's value into the tracker. Returns 1 when a change
 * has been confirmed and fills message with "<old> (<ctx>) to <new> (<ctx>)";
 * returns 0 otherwise.
 *
 * The first value seen establishes a baseline instead of counting as a change,
 * so starting a service never reports one. A differing value has to repeat for
 * VIDEO_FORMAT_CHANGE_FRAMES frames to be confirmed, and a return to the
 * current value discards whatever was pending as a glitch. */
static int format_change_update(format_change_struct *tracker,
                                const char *value, const char *context,
                                char *message, int message_size)
{
    if (!tracker || !value || value[0] == 0 || strcmp(value, "unknown") == 0) {
        return 0;
    }
    if (!context) {
        context = "";
    }

    if (tracker->current[0] == 0) {
        snprintf(tracker->current, sizeof(tracker->current), "%s", value);
        snprintf(tracker->current_context, sizeof(tracker->current_context), "%s", context);
        tracker->pending[0] = 0;
        tracker->pending_count = 0;
        return 0;
    }

    if (strcmp(value, tracker->current) == 0) {
        /* back on the reported value, so anything pending was a glitch */
        tracker->pending[0] = 0;
        tracker->pending_count = 0;
        return 0;
    }

    if (tracker->pending_count > 0 && strcmp(value, tracker->pending) == 0) {
        tracker->pending_count++;
    } else {
        snprintf(tracker->pending, sizeof(tracker->pending), "%s", value);
        snprintf(tracker->pending_context, sizeof(tracker->pending_context), "%s", context);
        tracker->pending_count = 1;
    }

    if (tracker->pending_count < VIDEO_FORMAT_CHANGE_FRAMES) {
        return 0;
    }

    if (message && message_size > 0) {
        snprintf(message, message_size, "%s (%s) to %s (%s)",
                 tracker->current, tracker->current_context,
                 tracker->pending, tracker->pending_context);
    }

    snprintf(tracker->current, sizeof(tracker->current), "%s", tracker->pending);
    snprintf(tracker->current_context, sizeof(tracker->current_context), "%s", tracker->pending_context);
    tracker->pending[0] = 0;
    tracker->pending_count = 0;

    return 1;
}

/* Formats a coded frame rate the way people write it, so the common broadcast
 * rates read as 25, 29.97, 23.98 and 59.94 rather than as ratios or as padded
 * decimals. A rate the stream did not signal reports "unknown", which the
 * change tracker ignores.
 *
 * Two decimals is the display precision, and it is also what the tracker
 * compares, so two rates closer together than 0.005 fps count as the same
 * rate. Nothing in use is that close - the nearest pair in practice is 23.98
 * and 24 - and the alternative is carrying the rational around to tell apart
 * rates no stream signals. */
static void format_frame_rate(char *out, int out_size, AVRational rate, double *rate_value)
{
    int rate_num = 0;
    int rate_den = 0;
    int i;

    if (rate_value) {
        *rate_value = 0.0;
    }
    if (!out || out_size <= 0) {
        return;
    }
    snprintf(out, out_size, "%s", "unknown");

    if (rate.num <= 0 || rate.den <= 0) {
        return;
    }
    av_reduce(&rate_num, &rate_den, (int64_t)rate.num, (int64_t)rate.den, 1024 * 1024);
    if (rate_num <= 0 || rate_den <= 0) {
        return;
    }

    snprintf(out, out_size, "%.2f", (double)rate_num / (double)rate_den);
    /* trim trailing zeros, then the point if nothing is left after it */
    if (strchr(out, '.')) {
        for (i = (int)strlen(out) - 1; i > 0; i--) {
            if (out[i] == '0') {
                out[i] = 0;
            } else if (out[i] == '.') {
                out[i] = 0;
                break;
            } else {
                break;
            }
        }
    }

    if (rate_value) {
        *rate_value = (double)rate_num / (double)rate_den;
    }
}

/* active_format values from ETSI TS 101 154 table B.1, which SMPTE 2016-1 and
 * ATSC A/53 share. Names are kept short enough to sit in a status row; "top"
 * means the active image is at the top of the coded frame, "centre" that it is
 * centred, and "protect" names the area a downstream crop should preserve. */
static const char *afd_name(int afd_code)
{
    switch (afd_code & 0x0f) {
        case 0:  return "undefined";
        case 2:  return "16:9 top";
        case 3:  return "14:9 top";
        case 4:  return ">16:9 centre";
        case 8:  return "full frame";
        case 9:  return "4:3 centre";
        case 10: return "16:9 centre";
        case 11: return "14:9 centre";
        case 13: return "4:3 protect 14:9";
        case 14: return "16:9 protect 14:9";
        case 15: return "16:9 protect 4:3";
        default: break;
    }
    return "reserved";
}

/* Reduced sample aspect ratio, "1:1" when the stream does not signal one. */
static void format_sample_aspect_ratio(char *out, int out_size, AVRational sample_aspect)
{
    int ratio_num = 0;
    int ratio_den = 0;

    if (!out || out_size <= 0) {
        return;
    }
    if (sample_aspect.num <= 0 || sample_aspect.den <= 0) {
        snprintf(out, out_size, "%s", "1:1");
        return;
    }
    av_reduce(&ratio_num, &ratio_den,
              (int64_t)sample_aspect.num, (int64_t)sample_aspect.den, 1024 * 1024);
    if (ratio_num <= 0 || ratio_den <= 0) {
        snprintf(out, out_size, "%s", "1:1");
        return;
    }
    snprintf(out, out_size, "%d:%d", ratio_num, ratio_den);
}
#endif

void *srthub_thumbnail_thread(void *context)
{
#if defined(ENABLE_THUMBNAIL)
    srthub_core_struct *srtcore = NULL;
    AVCodecContext *decode_avctx = NULL;
    const AVCodec *decode_codec = NULL;
    AVPacket *decode_pkt = NULL;
    AVFrame *decode_av_frame = NULL;
    AVCodecParserContext *decode_parser = NULL;
    enum AVPixelFormat source_format = AV_PIX_FMT_YUV420P;
    enum AVPixelFormat output_format = AV_PIX_FMT_YUV420P;
    struct SwsContext *decode_converter = NULL;
    /* change trackers for the source format properties we report on */
    format_change_struct aspect_tracker;
    format_change_struct resolution_tracker;
    format_change_struct afd_tracker;
    format_change_struct frame_rate_tracker;
    /* source geometry the scaler was built for, so it can be rebuilt when the
     * source changes */
    int converter_width = 0;
    int converter_height = 0;
    enum AVPixelFormat converter_format = AV_PIX_FMT_NONE;
    int output_allocated = 0;
    uint8_t *source_data[4];
    uint8_t *output_data[4];
    int source_stride[4];
    int output_stride[4];
    dataqueue_message_struct *msg = NULL;
    int video_decoder_ready = 0;
    int64_t decoded_frame_count = 0;
    char statsfilename[MAX_STRING_SIZE];
    char corruptiontimedate[MAX_STRING_SIZE];
    uint32_t decode_errors = 0;
    int rp = 0;
    int nt = 0;
    int64_t thumbnail_timer_start;
    int64_t thumbnail_timer_delta = 0;

    srtcore = (srthub_core_struct*)context;

    sprintf(statsfilename,"/opt/srthub/status/thumbnail_%d.json", srtcore->session_identifier);

    thumbnail_timer_start = monotonic_ms();
    memset(corruptiontimedate, 0, sizeof(corruptiontimedate));
    memset(&aspect_tracker, 0, sizeof(aspect_tracker));
    memset(&resolution_tracker, 0, sizeof(resolution_tracker));
    memset(&afd_tracker, 0, sizeof(afd_tracker));
    memset(&frame_rate_tracker, 0, sizeof(frame_rate_tracker));
    while (srtcore->thumbnail_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back_wait(srtcore->thumbnailqueue, &srtcore->thumbnail_thread_running);

        if (!srtcore->thumbnail_thread_running) {
            if (msg) {
                uint8_t *buffer = (uint8_t*)msg->buffer;
                memory_return(srtcore->videopool, buffer);
                memory_return(srtcore->msgpool, msg);
                msg = NULL;
                buffer = NULL;
            }
            goto cleanup_thumbnail_thread;
        }

        if (msg) {
            uint8_t *buffer = (uint8_t*)msg->buffer;
            int buffer_size = msg->buffer_size;
            int buffer_type = msg->buffer_type;
            int muxstreams = msg->flags;
            int stream_index = msg->stream_index;
            int corruption_count = msg->source_discontinuity;
            int ret;
            int sync_frame = 0;
            uint8_t *data = buffer;
            int data_size = buffer_size;

            corruption_count = corruption_count - 1;  // hack for the startup condition that needs to be fixed (since we always report one at startup)
            if (corruption_count < 0) {
                corruption_count = 0;
            }

            if (corruption_count > 0 && corruption_count != srtcore->last_corruption_count) {
                struct tm local_time;
                srtcore->last_corruption_time = time(NULL);
                if (localtime_r(&srtcore->last_corruption_time, &local_time)) {
                    /* the asctime() layout, without its trailing newline */
                    strftime(corruptiontimedate, sizeof(corruptiontimedate),
                             "%a %b %e %H:%M:%S %Y", &local_time);
                }
                srtcore->last_corruption_count = corruption_count;
            }

            if (!video_decoder_ready) {
                if (buffer_type == STREAM_TYPE_H264) {
                    decode_codec = (const AVCodec*)avcodec_find_decoder(AV_CODEC_ID_H264);
                } else if (buffer_type == STREAM_TYPE_MPEG2) {
                    decode_codec = (const AVCodec*)avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
                } else if (buffer_type == STREAM_TYPE_HEVC) {
                    decode_codec = (const AVCodec*)avcodec_find_decoder(AV_CODEC_ID_HEVC);
                } else if (buffer_type == STREAM_TYPE_AV1) {
                    decode_codec = (const AVCodec*)avcodec_find_decoder(AV_CODEC_ID_AV1);
                } else {
                    // unknown codec
                }
                if (decode_codec) {
                    decode_avctx = avcodec_alloc_context3(decode_codec);
                    if (!decode_parser) {
                        decode_parser = av_parser_init(decode_codec->id);
                    }
                    avcodec_open2(decode_avctx, decode_codec, NULL);
                    if (!decode_av_frame) {
                        decode_av_frame = av_frame_alloc();
                    }
                    if (!decode_pkt) {
                        decode_pkt = av_packet_alloc();
                    }
                    video_decoder_ready = 1;
                }
            }

            //fprintf(stderr,"initializing video, decoder_ready=%d\n", video_decoder_ready);

            if (!video_decoder_ready) {
                memory_return(srtcore->videopool, buffer);
                memory_return(srtcore->msgpool, msg);
                buffer = NULL;
                msg = NULL;

                FILE *statsfile = fopen(statsfilename,"wb");
                if (statsfile) {
                    fprintf(statsfile,"{\n");
                    fprintf(statsfile,"    \"width\":0,\n");
                    fprintf(statsfile,"    \"height\":0,\n");
                    fprintf(statsfile,"    \"display-aspect-ratio\":\"unknown\",\n");
                    fprintf(statsfile,"    \"display-aspect-ratio-value\":0.0000,\n");
                    fprintf(statsfile,"    \"sample-aspect-ratio\":\"unknown\",\n");
                    fprintf(statsfile,"    \"frame-rate\":\"unknown\",\n");
                    fprintf(statsfile,"    \"frame-rate-value\":0.000,\n");
                    fprintf(statsfile,"    \"afd-present\":0,\n");
                    fprintf(statsfile,"    \"afd-code\":-1,\n");
                    fprintf(statsfile,"    \"afd\":\"\",\n");
                    fprintf(statsfile,"    \"video-codec\":\"unknown\",\n");
                    fprintf(statsfile,"    \"source-format\":\"unknown\",\n");
                    fprintf(statsfile,"    \"total-streams\":%d,\n", muxstreams);
                    fprintf(statsfile,"    \"current-stream\":%d,\n", stream_index+1);
                    fprintf(statsfile,"    \"transport-source-errors\":%d,\n", corruption_count);
                    fprintf(statsfile,"    \"last-source-error\":\"%s\",\n", corruptiontimedate);
                    fprintf(statsfile,"    \"decode-errors\":%u\n", decode_errors);
                    fprintf(statsfile,"}\n");
                    fclose(statsfile);
                }
                usleep(100000);
                continue;
            }

            rp = 0;
            if (buffer_type == STREAM_TYPE_MPEG2) {
                while (rp + 4 < data_size) {
                    if (data[rp+0] == 0 &&
                        data[rp+1] == 0 &&
                        data[rp+2] == 1 &&
                        data[rp+3] == 0xb3) {
                        sync_frame = 1;
                        break;
                    }
                    rp++;
                }
            }
            if (buffer_type == STREAM_TYPE_H264) {
                int have_sps = 0;
                int have_pps = 0;
                while (rp + 3 < data_size) {
                    if (data[rp] == 0 && data[rp+1] == 0 && data[rp+2] == 1) {
                        size_t nal = rp+3;
                        int nt = (data[nal] & 0x1f);
                        switch (nt) {
                        case 7: have_sps = 1; break;       // SPS
                        case 8: have_pps = 1; break;       // PPS
                        case 5:                            // IDR
                            if (have_sps && have_pps)      // only decodable with param sets
                                sync_frame = 1;
                            break;
                        case 6:                                // SEI: recovery point?
                            if (have_sps && have_pps &&
                                h264_has_recovery_point(data + nal, data_size - nal))
                                sync_frame = 1;
                            break;
                        case 1:                                // non-IDR slice: I-slice?
                            if (have_sps && have_pps &&
                                (h264_slice_type(data + nal, data_size - nal) % 5) == 2)
                                sync_frame = 1;                // open-GOP entry
                            break;
                        }
                        if (sync_frame) break;
                        rp = nal + 1;
                    } else {
                        rp++;
                    }
                }
            }
            /*if (buffer_type == STREAM_TYPE_H264) {
                while (rp + 3 < data_size) {
                    if (data[rp+0] == 0 &&
                        data[rp+1] == 0 &&
                        data[rp+2] == 1) {
                        nt = (data[rp+3] & 0x1f);
                        if (nt == 7 || nt == 8) {  // nt == 5
                            sync_frame = 1;
                            break;
                        }
                    }
                    rp++;
                }
            }*/
            if (buffer_type == STREAM_TYPE_HEVC) {
                while (rp + 4 < data_size) {
                    if (data[rp+0] == 0 &&
                        data[rp+1] == 0 &&
                        data[rp+2] == 1) {
                        nt = (data[rp+3] & 0x7f);
                        nt = nt >> 1;
                        if (nt == 34 || nt == 33 || nt == 32) {
                            sync_frame = 1;
                            break;
                        }
                    }
                    rp++;
                }
            }

            thumbnail_timer_delta = monotonic_ms() - thumbnail_timer_start;
            //if (sync_frame) {
            //    fprintf(stderr,"thumbnail_timer_delta=%ld, data_size=%d\n", thumbnail_timer_delta, data_size);
            //} else {
            //    fprintf(stderr,"thumbnail_timer_delta=%ld, no sync frame\n", thumbnail_timer_delta);
            //}
            if (sync_frame && thumbnail_timer_delta >= 1000) {
                while (data_size > 0) {
                    ret = av_parser_parse2(decode_parser, decode_avctx, &decode_pkt->data, &decode_pkt->size,
                                           data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                    if (ret < 0) {
                        memory_return(srtcore->videopool, buffer);
                        memory_return(srtcore->msgpool, msg);
                        buffer = NULL;
                        buffer_size = 0;
                        msg = NULL;
                        data_size = 0;
                        break;
                    }

                    data += ret;
                    data_size -= ret;

                    if (decode_pkt->size > 0) {
                        int dret;
                        dret = avcodec_send_packet(decode_avctx, decode_pkt);
                        if (dret < 0) {
                            memory_return(srtcore->videopool, buffer);
                            memory_return(srtcore->msgpool, msg);
                            buffer = NULL;
                            buffer_size = 0;
                            msg = NULL;
                            data_size = 0;
                            break;
                        }

                        while (dret >= 0) {
                            int is_frame_interlaced;
                            int is_frame_tff;
                            int frame_height;
                            int frame_height2;
                            int frame_width;
                            int frame_width2;
                            int row;
                            AVFrame *jpeg_frame;
                            AVRational frame_sample_aspect;
                            char display_aspect[32];
                            char sample_aspect[32];
                            double display_aspect_value = 0.0;
                            int afd_code = -1;
                            char frame_rate[32];
                            double frame_rate_value = 0.0;

                            dret = avcodec_receive_frame(decode_avctx, decode_av_frame);
                            if (dret == AVERROR(EAGAIN) || dret == AVERROR_EOF) {
                                break;
                            }
                            if (dret < 0) {
                                memory_return(srtcore->videopool, buffer);
                                memory_return(srtcore->msgpool, msg);
                                buffer = NULL;
                                buffer_size = 0;
                                msg = NULL;
                                data_size = 0;
                                break;
                            }

                            decoded_frame_count++;

                            is_frame_interlaced = decode_av_frame->interlaced_frame;
                            is_frame_tff = decode_av_frame->top_field_first;
                            source_format = decode_av_frame->format;
                            frame_height = decode_avctx->height;
                            frame_width = decode_avctx->width;

                            /* The frame carries the sample aspect ratio when
                             * the stream signalled one; fall back to the codec
                             * context, which keeps the last value the decoder
                             * saw, before assuming square pixels. */
                            frame_sample_aspect = decode_av_frame->sample_aspect_ratio;
                            if (frame_sample_aspect.num <= 0 || frame_sample_aspect.den <= 0) {
                                frame_sample_aspect = decode_avctx->sample_aspect_ratio;
                            }
                            format_aspect_ratio(display_aspect, sizeof(display_aspect),
                                                frame_width, frame_height,
                                                frame_sample_aspect, &display_aspect_value);
                            format_sample_aspect_ratio(sample_aspect, sizeof(sample_aspect),
                                                       frame_sample_aspect);

                            /* The coded frame rate, which both the MPEG-2 and
                             * the AVC/HEVC decoders put on the context. For an
                             * interlaced source this is the frame rate, not the
                             * field rate, so 1080i25 reports 25. */
                            format_frame_rate(frame_rate, sizeof(frame_rate),
                                              decode_avctx->framerate, &frame_rate_value);

                            /* The active format description, when the stream
                             * carries one. Both the MPEG-2 picture user data
                             * and the AVC/HEVC SEI route it to the same frame
                             * side data, a single byte holding active_format. */
                            {
                                AVFrameSideData *afd_side =
                                    av_frame_get_side_data(decode_av_frame, AV_FRAME_DATA_AFD);

                                if (afd_side && afd_side->data && afd_side->size >= 1) {
                                    afd_code = (int)(afd_side->data[0] & 0x0f);
                                }
                            }

                            /* Report mid-stream changes of the source format.
                             * The aspect ratio and the resolution are tracked
                             * separately because either can change without the
                             * other: an anamorphic switch leaves the
                             * resolution alone, and a 1080p to 720p change
                             * leaves the ratio alone. */
                            {
                                char resolution_text[MAX_FORMAT_VALUE_SIZE];
                                char change_message[MAX_SMALLBUF_SIZE];

                                snprintf(resolution_text, sizeof(resolution_text), "%dx%d",
                                         frame_width, frame_height);

                                if (format_change_update(&aspect_tracker, display_aspect, resolution_text,
                                                         change_message, sizeof(change_message))) {
                                    fprintf(stderr,"srthub_thumbnail_thread: source aspect ratio changed: %s\n",
                                            change_message);
                                    send_signal(srtcore, SIGNAL_VIDEO_ASPECT_CHANGE, change_message);
                                }

                                if (frame_width > 0 && frame_height > 0 &&
                                    format_change_update(&resolution_tracker, resolution_text, display_aspect,
                                                         change_message, sizeof(change_message))) {
                                    fprintf(stderr,"srthub_thumbnail_thread: source resolution changed: %s\n",
                                            change_message);
                                    send_signal(srtcore, SIGNAL_VIDEO_RESOLUTION_CHANGE, change_message);
                                }

                                /* The AFD code is the value being tracked, so
                                 * two reserved codes are not treated as the
                                 * same thing; its meaning and the picture it
                                 * applies to ride along as context. "none" is
                                 * fed when the frame carries no AFD, so a
                                 * stream that stops signalling it is reported
                                 * - and because a change still has to be
                                 * confirmed over two frames, a stream that
                                 * only signals AFD on some pictures does not
                                 * flap. */
                                {
                                    char afd_value[MAX_FORMAT_VALUE_SIZE];
                                    char afd_context[MAX_FORMAT_VALUE_SIZE];

                                    if (afd_code >= 0) {
                                        snprintf(afd_value, sizeof(afd_value), "%d", afd_code);
                                        snprintf(afd_context, sizeof(afd_context), "%s, %s %s",
                                                 afd_name(afd_code), resolution_text, display_aspect);
                                    } else {
                                        snprintf(afd_value, sizeof(afd_value), "%s", "none");
                                        snprintf(afd_context, sizeof(afd_context), "%s %s",
                                                 resolution_text, display_aspect);
                                    }

                                    if (format_change_update(&afd_tracker, afd_value, afd_context,
                                                             change_message, sizeof(change_message))) {
                                        fprintf(stderr,"srthub_thumbnail_thread: source AFD changed: %s\n",
                                                change_message);
                                        send_signal(srtcore, SIGNAL_VIDEO_AFD_CHANGE, change_message);
                                    }
                                }

                                if (format_change_update(&frame_rate_tracker, frame_rate, resolution_text,
                                                         change_message, sizeof(change_message))) {
                                    fprintf(stderr,"srthub_thumbnail_thread: source frame rate changed: %s\n",
                                            change_message);
                                    send_signal(srtcore, SIGNAL_VIDEO_FRAMERATE_CHANGE, change_message);
                                }
                            }

                            source_data[0] = decode_av_frame->data[0];
                            source_data[1] = decode_av_frame->data[1];
                            source_data[2] = decode_av_frame->data[2];
                            source_data[3] = decode_av_frame->data[3];
                            source_stride[0] = decode_av_frame->linesize[0];
                            source_stride[1] = decode_av_frame->linesize[1];
                            source_stride[2] = decode_av_frame->linesize[2];
                            source_stride[3] = decode_av_frame->linesize[3];

                            /* The scaler is bound to the source geometry it
                             * was built with, so it has to be rebuilt when the
                             * source changes. Without this, sws_scale() is
                             * handed a frame the context was not made for and
                             * refuses the slice ("Slice parameters 0, 1080 are
                             * invalid"), which left the preview frozen on the
                             * last good frame after a resolution change. The
                             * output buffer is a fixed thumbnail size, so it is
                             * allocated only once. */
                            if (!decode_converter ||
                                converter_width != frame_width ||
                                converter_height != frame_height ||
                                converter_format != source_format) {
                                if (decode_converter) {
                                    fprintf(stderr,"srthub_thumbnail_thread: rebuilding scaler for %dx%d %s\n",
                                            frame_width, frame_height, av_get_pix_fmt_name(source_format));
                                    sws_freeContext(decode_converter);
                                    decode_converter = NULL;
                                }
                                decode_converter = sws_getContext(frame_width, frame_height, source_format,
                                                                  THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, output_format,
                                                                  SWS_BICUBIC, NULL, NULL, NULL);
                                if (!output_allocated) {
                                    av_image_alloc(output_data, output_stride, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, output_format, 1);
                                    output_allocated = 1;
                                }
                                converter_width = frame_width;
                                converter_height = frame_height;
                                converter_format = source_format;
                            }
                            if (!decode_converter) {
                                /* cannot scale this frame; keep the service
                                 * running rather than losing the stream */
                                memory_return(srtcore->videopool, buffer);
                                memory_return(srtcore->msgpool, msg);
                                buffer = NULL;
                                buffer_size = 0;
                                msg = NULL;
                                data_size = 0;
                                break;
                            }

                            {
                                char codec[MAX_STRING_SIZE];
                                if (buffer_type == STREAM_TYPE_H264) {
                                    sprintf(codec,"h264");
                                } else if (buffer_type == STREAM_TYPE_MPEG2) {
                                    sprintf(codec,"mpeg2");
                                } else if (buffer_type == STREAM_TYPE_HEVC) {
                                    sprintf(codec,"hevc");
                                } else if (buffer_type == STREAM_TYPE_AV1) {
                                    sprintf(codec,"av1");
                                } else {
                                    sprintf(codec,"unknown");
                                }
                                FILE *statsfile = fopen(statsfilename,"wb");
                                if (statsfile) {
                                    fprintf(statsfile,"{\n");
                                    fprintf(statsfile,"    \"width\":%d,\n", frame_width);
                                    fprintf(statsfile,"    \"height\":%d,\n", frame_height);
                                    fprintf(statsfile,"    \"display-aspect-ratio\":\"%s\",\n", display_aspect);
                                    fprintf(statsfile,"    \"display-aspect-ratio-value\":%.4f,\n", display_aspect_value);
                                    fprintf(statsfile,"    \"sample-aspect-ratio\":\"%s\",\n", sample_aspect);
                                    fprintf(statsfile,"    \"frame-rate\":\"%s\",\n", frame_rate);
                                    fprintf(statsfile,"    \"frame-rate-value\":%.3f,\n", frame_rate_value);
                                    fprintf(statsfile,"    \"afd-present\":%d,\n", afd_code >= 0 ? 1 : 0);
                                    fprintf(statsfile,"    \"afd-code\":%d,\n", afd_code);
                                    fprintf(statsfile,"    \"afd\":\"%s\",\n",
                                            afd_code >= 0 ? afd_name(afd_code) : "");
                                    fprintf(statsfile,"    \"video-codec\":\"%s\",\n", codec);
                                    fprintf(statsfile,"    \"source-format\":\"%s\",\n", av_get_pix_fmt_name(source_format));
                                    fprintf(statsfile,"    \"total-streams\":%d,\n", muxstreams);
                                    fprintf(statsfile,"    \"current-stream\":%d,\n", stream_index+1);
                                    fprintf(statsfile,"    \"transport-source-errors\":%d,\n", corruption_count);
                                    fprintf(statsfile,"    \"last-source-error\":\"%s\",\n", corruptiontimedate);
                                    fprintf(statsfile,"    \"decode-errors\":0\n");
                                    fprintf(statsfile,"}\n");
                                    fclose(statsfile);
                                }

                                sws_scale(decode_converter,
                                          (const uint8_t * const*)source_data, source_stride, 0,
                                          frame_height, output_data, output_stride);

                                jpeg_frame = av_frame_alloc();
                                if (jpeg_frame) {
                                    thumbnail_timer_start = monotonic_ms();

                                    jpeg_frame->data[0] = output_data[0];
                                    jpeg_frame->data[1] = output_data[1];
                                    jpeg_frame->data[2] = output_data[2];
                                    jpeg_frame->data[3] = output_data[3];
                                    jpeg_frame->linesize[0] = output_stride[0];
                                    jpeg_frame->linesize[1] = output_stride[1];
                                    jpeg_frame->linesize[2] = output_stride[2];
                                    jpeg_frame->linesize[3] = output_stride[3];
                                    jpeg_frame->pts = AV_NOPTS_VALUE;
                                    jpeg_frame->pkt_dts = AV_NOPTS_VALUE;
                                    jpeg_frame->pkt_duration = 0;
                                    jpeg_frame->pkt_pos = -1;
                                    jpeg_frame->pkt_size = -1;
                                    jpeg_frame->key_frame = -1;
                                    jpeg_frame->sample_aspect_ratio = (AVRational){1,1};
                                    jpeg_frame->format = 0;
                                    jpeg_frame->extended_data = NULL;
                                    jpeg_frame->color_primaries = AVCOL_PRI_BT709;
                                    jpeg_frame->color_trc = AVCOL_TRC_BT709;
                                    jpeg_frame->colorspace = AVCOL_SPC_BT709;
                                    jpeg_frame->color_range = AVCOL_RANGE_JPEG;
                                    jpeg_frame->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
                                    jpeg_frame->flags = 0;
                                    jpeg_frame->channels = 0;
                                    jpeg_frame->channel_layout = 0;
                                    jpeg_frame->width = THUMBNAIL_WIDTH;
                                    jpeg_frame->height = THUMBNAIL_HEIGHT;
                                    jpeg_frame->interlaced_frame = 0;
                                    jpeg_frame->top_field_first = 0;

                                    fprintf(stderr,"saving jpeg frame\n");
                                    save_frame_as_jpeg(srtcore, jpeg_frame);

                                    av_frame_free(&jpeg_frame);
                                    //avcodec_close(decode_avctx);
                                    //avcodec_free_context(&decode_avctx);
                                    //av_parser_close(decode_parser);
                                    //decode_parser = NULL;
                                    //decode_avctx = NULL;
                                    //video_decoder_ready = 0;
                                    goto finish_parsing;
                                }
                            }
                        }
                    }
                }
            }
finish_parsing:
            if (buffer) {
                memory_return(srtcore->videopool, buffer);
            }
            if (msg) {
                memory_return(srtcore->msgpool, msg);
            }
            msg = NULL;
            buffer = NULL;
        }
    }

cleanup_thumbnail_thread:

    av_frame_free(&decode_av_frame);
    av_packet_free(&decode_pkt);
    avcodec_close(decode_avctx);
    avcodec_free_context(&decode_avctx);
    av_parser_close(decode_parser);
    if (decode_converter) {
        sws_freeContext(decode_converter);
        decode_converter = NULL;
    }
    if (output_allocated) {
        av_freep(&output_data[0]);
        output_allocated = 0;
    }

    msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->thumbnailqueue);
    while (msg) {
        uint8_t *buffer = (uint8_t*)msg->buffer;
        memory_return(srtcore->videopool, buffer);
        memory_return(srtcore->msgpool, msg);
        buffer = NULL;
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->thumbnailqueue);
    }

#endif
    return NULL;
}

/* Appends the push/pull role to a mode ("srt" + "push"), bounded by the
 * mode buffer. strncat's limit counts appended bytes, not the buffer, so the
 * old call overflowed a long mode into the fields after it. */
static void append_mode(char *mode, const char *role)
{
    size_t used = strlen(mode);
    if (used < MAX_STRING_SIZE - 1) {
        snprintf(mode + used, MAX_STRING_SIZE - used, "%s", role);
    }
}

/* A config value as text. The web app writes every value as a JSON string,
 * but a hand-written or older config can carry a number, and anything else
 * (null, an object, ...) has no valuestring at all: dereferencing that is
 * what crashed srthub on a config with "sourceport": 9000. */
static const char *config_string(const cJSON *item, char *scratch, int scratch_size)
{
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    if (cJSON_IsNumber(item)) {
        snprintf(scratch, scratch_size, "%.0f", item->valuedouble);
        return scratch;
    }
    return "";
}

int srthub_read_config(char *filename, srthub_configuration_struct *config)
{
    FILE *configfile;
    int br;
    char configbuffer[MAX_CONFIG_SIZE];
    char field_scratch[64];

    memset(config, 0, sizeof(srthub_configuration_struct));

    configfile = fopen(filename,"r");
    if (configfile) {
        br = fread(configbuffer, 1, MAX_CONFIG_SIZE-1, configfile);
        if (br > 0) {
            configbuffer[br] = 0;   /* cJSON_Parse needs the terminator fread does not write */
            cJSON *top = cJSON_Parse(configbuffer);
            if (top) {
                cJSON *sourcename_field;
                cJSON *streamid_field;
                cJSON *sourcemode_field;
                cJSON *sourceaddress_field;
                cJSON *sourceport_field;
                cJSON *sourceinterface_field;
                cJSON *outputmode_field;
                cJSON *outputaddress_field;
                cJSON *outputport_field;
                cJSON *outputinterface_field;
                cJSON *outputttl_field;
                cJSON *keysize_field;
                cJSON *passphrase_field;
                cJSON *servermode_field;
                cJSON *clientmode_field;
                cJSON *managementserverip_field;
                cJSON *latencyms_field;
                cJSON *whitelist_field;
                cJSON *overheadbw_field;

                sourcename_field = cJSON_GetObjectItem(top,"sourcename");
                sourcemode_field = cJSON_GetObjectItem(top,"sourcemode");
                sourceaddress_field = cJSON_GetObjectItem(top,"sourceaddress");
                sourceport_field = cJSON_GetObjectItem(top,"sourceport");
                sourceinterface_field = cJSON_GetObjectItem(top,"sourceinterface");

                outputmode_field = cJSON_GetObjectItem(top,"outputmode");
                outputaddress_field = cJSON_GetObjectItem(top,"outputaddress");
                outputport_field = cJSON_GetObjectItem(top,"outputport");
                outputinterface_field = cJSON_GetObjectItem(top,"outputinterface");
                outputttl_field = cJSON_GetObjectItem(top,"outputttl");

                keysize_field = cJSON_GetObjectItem(top,"keysize");
                passphrase_field = cJSON_GetObjectItem(top,"passphrase");
                streamid_field = cJSON_GetObjectItem(top,"streamid");
                servermode_field = cJSON_GetObjectItem(top,"servertype");
                clientmode_field = cJSON_GetObjectItem(top,"clienttype");
                whitelist_field = cJSON_GetObjectItem(top,"whitelist");

                managementserverip_field = cJSON_GetObjectItem(top,"managementserverip");
                overheadbw_field = cJSON_GetObjectItem(top,"overheadbw");
                latencyms_field = cJSON_GetObjectItem(top,"latency");

                fprintf(stderr,"-------------------- configuration options -----------------------\n");
                if (sourcename_field) {
                    snprintf(config->sourcename,MAX_STRING_SIZE-1,"%s",config_string(sourcename_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"sourcename:%s\n", config->sourcename);
                }
                if (streamid_field) {
                    snprintf(config->streamid,MAX_STRING_SIZE-1,"%s",config_string(streamid_field, field_scratch, sizeof(field_scratch)));
                    if (strlen(config->streamid) > 0) {
                        fprintf(stderr,"streamid:%s\n", config->streamid);
                    }
                }
                if (sourcemode_field) {
                    snprintf(config->sourcemode,MAX_STRING_SIZE-1,"%s",config_string(sourcemode_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"sourcemode:%s\n", config->sourcemode);
                }
                if (sourceaddress_field) {
                    snprintf(config->sourceaddress,MAX_STRING_SIZE-1,"%s",config_string(sourceaddress_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"sourceaddress:%s\n", config->sourceaddress);
                }
                if (sourceinterface_field) {
                    snprintf(config->sourceinterface,MAX_STRING_SIZE-1,"%s",config_string(sourceinterface_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"sourceinterface:%s\n", config->sourceinterface);
                }
                if (sourceport_field) {
                    config->sourceport = atoi(config_string(sourceport_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"sourceport:%d\n", config->sourceport);
                }
                if (outputmode_field) {
                    snprintf(config->outputmode,MAX_STRING_SIZE-1,"%s",config_string(outputmode_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"outputmode:%s\n", config->outputmode);
                }
                if (outputaddress_field) {
                    snprintf(config->outputaddress,MAX_STRING_SIZE-1,"%s",config_string(outputaddress_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"outputaddress:%s\n", config->outputaddress);
                }
                if (outputinterface_field) {
                    snprintf(config->outputinterface,MAX_STRING_SIZE-1,"%s",config_string(outputinterface_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"outputinterface:%s\n", config->outputinterface);
                }
                if (outputport_field) {
                    config->outputport = atoi(config_string(outputport_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"outputport:%d\n", config->outputport);
                }
                if (outputttl_field) {
                    config->outputttl = atoi(config_string(outputttl_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"outputttl:%d\n", config->outputttl);
                } else {
                    config->outputttl = 16;
                }
                if (keysize_field) {
                    config->keysize = atoi(config_string(keysize_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"keysize:%d\n", config->keysize);
                } else {
                    config->keysize = 0;
                }
                if (passphrase_field) {
                    snprintf(config->passphrase,MAX_STRING_SIZE-1,"%s",config_string(passphrase_field, field_scratch, sizeof(field_scratch)));
                    if (strlen(config->passphrase) > 0) {
                        fprintf(stderr,"passphrase:(set, %d characters)\n", (int)strlen(config->passphrase));
                    }
                }
                if (servermode_field) {
                    snprintf(config->servermode,MAX_STRING_SIZE-1,"%s",config_string(servermode_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"servermode:%s\n", config->servermode);
                }
                if (clientmode_field) {
                    snprintf(config->clientmode,MAX_STRING_SIZE-1,"%s",config_string(clientmode_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"clientmode:%s\n", config->clientmode);
                }
                if (managementserverip_field) {
                    snprintf(config->managementip,MAX_STRING_SIZE-1,"%s",config_string(managementserverip_field, field_scratch, sizeof(field_scratch)));
                    if (strlen(config->managementip) > 0) {
                        fprintf(stderr,"managementip:%s\n", config->managementip);
                    }
                }
                if (latencyms_field) {
                    config->latencyms = atoi(config_string(latencyms_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"latency:%d\n", config->latencyms);
                } else {
                    config->latencyms = 120;
                }
                if (overheadbw_field) {
                    config->overheadbw = atoi(config_string(overheadbw_field, field_scratch, sizeof(field_scratch)));
                    fprintf(stderr,"overheadbw:%d%%\n", config->overheadbw);
                } else {
                    config->overheadbw = 25;
                }
                if (whitelist_field) {
                    snprintf(config->whitelist,MAX_STRING_SIZE-1,"%s",config_string(whitelist_field, field_scratch, sizeof(field_scratch)));
                    if (strlen(config->whitelist) > 0) {
                        fprintf(stderr,"whitelist:%s\n", config->whitelist);
                    }
                }

                if (strncmp(config->sourcemode,"srt",3)==0) {
                    if (strncmp(config->clientmode,"push",4)==0) {
                        append_mode(config->sourcemode, config->clientmode);
                    }
                    if (strncmp(config->clientmode,"pull",4)==0) {
                        append_mode(config->sourcemode, config->clientmode);
                    }
                    fprintf(stderr,"updated sourcemode:%s\n", config->sourcemode);
                }
                if (strncmp(config->outputmode,"srt",3)==0) {
                    if (strncmp(config->servermode,"push",4)==0) {
                        append_mode(config->outputmode, config->servermode);
                    }
                    if (strncmp(config->servermode,"pull",4)==0) {
                        append_mode(config->outputmode, config->servermode);
                    }
                    fprintf(stderr,"updated outputmode:%s\n", config->outputmode);
                }
                fprintf(stderr,"------------------------------------------------------------------\n");
                cJSON_Delete(top);
            }
        }
        fclose(configfile);
    }

    return 0;
}

int main(int argc, char **argv)
{
    srthub_core_struct srtcore;
    int session_identifier = 1;
    char statsfilename[MAX_STRING_SIZE];
    char system_hostname[MAX_STRING_SIZE];
    int wait_count = 0;
    int64_t uptime = -1;
    struct timespec uptime_start;
    struct timespec uptime_check;
    int64_t diff;
    int thread = 0;

    fprintf(stderr,"srthub (C) Copyright 2023 John William\n");
    fprintf(stderr,"\n");
    fprintf(stderr,"srthub version is: %d.%d\n",
            SRTHUB_MAJOR, SRTHUB_MINOR);
    fprintf(stderr,"srt version is: %d.%d.%d\n",
            (srt_getversion() >> 16) & 0xff,
            (srt_getversion() >> 8) & 0xff,
            (srt_getversion() >> 0) & 0xff);

    sprintf(statsfilename,"/opt/srthub/srthub.json");
    FILE *srthubstatsfile = fopen(statsfilename,"wb");
    if (srthubstatsfile) {
        memset(system_hostname, 0, sizeof(system_hostname));
        gethostname(system_hostname, MAX_STRING_SIZE-1);
        fprintf(srthubstatsfile,"{\n");
        fprintf(srthubstatsfile,"    \"srt-version\":\"%d.%d.%d\",\n", (srt_getversion() >> 16) & 0xff, (srt_getversion() >> 8) & 0xff, (srt_getversion() >> 0) & 0xff);
        fprintf(srthubstatsfile,"    \"srthub-version\":\"%d.%d\",\n", SRTHUB_MAJOR, SRTHUB_MINOR);
        fprintf(srthubstatsfile,"    \"hostname\":\"%s\"\n", system_hostname);
        fprintf(srthubstatsfile,"}\n");
        fclose(srthubstatsfile);
    }

    if (argc < 2) {
        fprintf(stderr,"\n");
        fprintf(stderr,"usage: srthub sessionid\n");
        fprintf(stderr,"\n");
        fprintf(stderr,"    sessionid is a unique number that identifies the instance of the application (unsigned 32-bit) and it should correspond to a json config file in /opt/srthub/configs\n");
        fprintf(stderr,"\n");
        fprintf(stderr,"    srt to srt is an invalid mode\n");
        fprintf(stderr,"\n");
        return 0;
    }

    char configfilename[MAX_STRING_SIZE];
    srthub_configuration_struct config;
    session_identifier = atoi(argv[1]);
    snprintf(configfilename,MAX_STRING_SIZE-1,"/opt/srthub/configs/%d.json",session_identifier);
    fprintf(stderr,"reading configuration file %s\n", configfilename);
    srthub_read_config(configfilename,&config);

    srtcore.config = (srthub_configuration_struct*)&config;

    char *sourcemode = (char*)&config.sourcemode;
    char *server_address = (char*)&config.sourceaddress;
    int server_port = config.sourceport;
    char *sourceinterface = (char*)&config.sourceinterface;
    char *outputmode = (char*)&config.outputmode;
    char *output_address = (char*)&config.outputaddress;
    int output_port = config.outputport;
    char *outputinterface = (char*)&config.outputinterface;
    char *passphrase = (char*)&config.passphrase;
    int keysize = config.keysize;
    char *streamid = (char*)&config.streamid;

    if ((strncmp(sourcemode,"udp",3)==0) || (strncmp(sourcemode,"srt",3)==0)) {
        fprintf(stderr,"source mode is: %s\n", sourcemode);
    } else {
        fprintf(stderr,"\ninvalid source mode: %s    valid options are udp or srt\n\n", sourcemode);
        return -1;
    }
    fprintf(stderr,"sourceaddress is: %s\n", server_address);
    fprintf(stderr,"sourceport is: %d\n", server_port);

    socket_udp_global_init();

    srtcore.msgpool = memory_create(MAX_MSG_BUFFERS, sizeof(dataqueue_message_struct));
    srtcore.packetpool = memory_create(MAX_PACKET_BUFFERS, MAX_PACKET_BUFFER_SIZE);
    srtcore.videopool = NULL;
    srtcore.video_initialized = 0;
    srtcore.scte35_pid = 0;
    srtcore.pcr_pid = 0;
    srtcore.video_pid = 0;
    srtcore.video_stream_type = 0;
    srtcore.audio_pid_count = 0;
    memset(srtcore.audio_pid, 0, sizeof(srtcore.audio_pid));
    memset(srtcore.audio_stream_type, 0, sizeof(srtcore.audio_stream_type));
    memset(srtcore.audio_language, 0, sizeof(srtcore.audio_language));
    srtcore.scte35_cue_count = 0;
    srtcore.scte35_have_last_cue = 0;
    srtcore.scte35_last_event_id = 0;
    srtcore.scte35_last_cue_direction = SCTE35_CUE_UNKNOWN;
    srtcore.scte35_last_cue_cancel = 0;
    srtcore.scte35_last_cue_immediate = 0;
    srtcore.scte35_last_cue_duration = 0;
    srtcore.scte35_last_cue_command = 0;
    srtcore.scte35_last_segmentation_type = -1;
    srtcore.scte35_last_cue_time = 0;
    memset(srtcore.scte35_last_cue_name, 0, sizeof(srtcore.scte35_last_cue_name));
    memset(srtcore.scte35_recent, 0, sizeof(srtcore.scte35_recent));
    srtcore.video_init_lock = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(srtcore.video_init_lock, NULL);
    srtcore.audiopool = memory_create(MAX_AUDIO_BUFFERS, MAX_AUDIO_BUFFER_SIZE);

    srtcore.session_identifier = session_identifier;
    srtcore.msgqueue = dataqueue_create();
    srtcore.thumbnailqueue = dataqueue_create();
    srtcore.udpserverqueue = dataqueue_create();
    srtcore.signalqueue = dataqueue_create();
    for (thread = 0; thread < MAX_WORKER_THREADS; thread++) {
        srtcore.srtserverqueue[thread] = NULL;
        srtcore.audiodecodequeue[thread] = dataqueue_create();
    }
    srtcore.srtserverlock = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(srtcore.srtserverlock, NULL);

    start_signal_thread(&srtcore);

    if ((strncmp(sourcemode,"srt",3)==0) && (strncmp(outputmode,"udp",3)==0)) {
        send_signal(&srtcore, SIGNAL_START_SERVICE, "Started SRT Receiver to UDP Output");
    }
    if ((strncmp(sourcemode,"udp",3)==0) && (strncmp(outputmode,"srt",3)==0)) {
        send_signal(&srtcore, SIGNAL_START_SERVICE, "Started UDP Input to SRT Server");
    }

restart_srt:
    register_frame_callback(receive_frame, (void*)&srtcore);

    for (thread = 0; thread < MAX_WORKER_THREADS; thread++) {
        srt_audio_thread_struct *srtaudio;
        srtaudio = (srt_audio_thread_struct*)malloc(sizeof(srt_audio_thread_struct));
        srtaudio->core = (srthub_core_struct*)&srtcore;
        srtaudio->audio_stream = thread;
        srtcore.audio_decode_thread_running[thread] = 1;
        fprintf(stderr,"starting audio decode thread: %d\n", thread);
        pthread_create(&srtcore.audio_decode_thread_id[thread], NULL, srthub_audio_thread, (void*)srtaudio);
    }

    if (strncmp(outputmode,"udp",3)==0) {
        srtcore.udp_server_thread_running = 1;
        udp_server_thread_struct *udp_server_data = (udp_server_thread_struct*)malloc(sizeof(udp_server_thread_struct));
        sprintf(udp_server_data->interface_name, "%s", outputinterface);
        sprintf(udp_server_data->destination_address, "%s", output_address);
        udp_server_data->destination_port = output_port;
        udp_server_data->ttl = 8;
        udp_server_data->core = (srthub_core_struct*)&srtcore;
        fprintf(stderr,"starting udp server thread\n");
        pthread_create(&srtcore.udp_server_thread_id, NULL, udp_server_thread, (void*)udp_server_data);
    }

    if (strncmp(outputmode,"srtpull",7)==0) { // Listener mode
        srtcore.srt_server_thread_running = 1;
        srt_server_thread_struct *srt_server_data = (srt_server_thread_struct*)malloc(sizeof(srt_server_thread_struct));
        sprintf(srt_server_data->server_address, "%s", output_address);
        sprintf(srt_server_data->server_interface_name, "%s", outputinterface);
        srt_server_data->server_port = output_port;
        memset(&srt_server_data->streamid, 0, sizeof(srt_server_data->streamid));
        memset(&srt_server_data->passphrase, 0, sizeof(srt_server_data->passphrase));
        if (streamid) {
            sprintf(srt_server_data->streamid, "%s", streamid);
        }
        if (passphrase) {
            sprintf(srt_server_data->passphrase, "%s", passphrase);
            srt_server_data->keysize = keysize;
        } else {
            srt_server_data->keysize = 0;
        }
        srt_server_data->core = (srthub_core_struct*)&srtcore;
        fprintf(stderr,"starting srt server thread (we are the server and client will pull from us)\n");
        pthread_create(&srtcore.srt_server_thread_id, NULL, srt_server_thread_pull, (void*)srt_server_data);
    }

    if (strncmp(outputmode,"srtpush",7)==0) { // Caller mode
        srtcore.srt_server_thread_running = 1;

        srt_server_thread_struct *srt_server_data = (srt_server_thread_struct*)malloc(sizeof(srt_server_thread_struct));
        sprintf(srt_server_data->server_address, "%s", output_address);
        sprintf(srt_server_data->server_interface_name, "%s", outputinterface);
        srt_server_data->server_port = output_port;
        memset(&srt_server_data->streamid, 0, sizeof(srt_server_data->streamid));
        memset(&srt_server_data->passphrase, 0, sizeof(srt_server_data->passphrase));
        if (streamid) {
            sprintf(srt_server_data->streamid, "%s", streamid);
        }
        if (passphrase) {
            sprintf(srt_server_data->passphrase, "%s", passphrase);
            srt_server_data->keysize = keysize;
        } else {
            srt_server_data->keysize = 0;
        }
        srt_server_data->core = (srthub_core_struct*)&srtcore;
        fprintf(stderr,"starting srt server thread push (we are the client and we will push to server)\n");
        pthread_create(&srtcore.srt_server_thread_id, NULL, srt_server_thread_push, (void*)srt_server_data);
    }

    if (strncmp(sourcemode,"srtpull",7)==0) {  // Caller mode
        srtcore.srt_receiver_thread_running = 1;
        srt_receive_thread_caller_struct *srt_receive_data = (srt_receive_thread_caller_struct*)malloc(sizeof(srt_receive_thread_caller_struct));
        sprintf(srt_receive_data->server_address, "%s", server_address);
        sprintf(srt_receive_data->server_interface_name, "%s", sourceinterface);
        srt_receive_data->server_port = server_port;
        memset(&srt_receive_data->streamid, 0, sizeof(srt_receive_data->streamid));
        memset(&srt_receive_data->passphrase, 0, sizeof(srt_receive_data->passphrase));
        if (streamid) {
            sprintf(srt_receive_data->streamid, "%s", streamid);
        }
        if (passphrase) {
            sprintf(srt_receive_data->passphrase, "%s", passphrase);
            srt_receive_data->keysize = keysize;
        } else {
            srt_receive_data->keysize = 0;
        }
        srt_receive_data->core = (srthub_core_struct*)&srtcore;
        fprintf(stderr,"starting srt receiver thread (caller), address=%s, interface=%s, port=%d\n", server_address, sourceinterface, server_port);
        pthread_create(&srtcore.srt_receiver_thread_id, NULL, srt_receiver_thread_caller, (void*)srt_receive_data);
    }

    if (strncmp(sourcemode,"srtpush",7)==0) {  // Listener mode (Somebody will connect and push it to me)
        srtcore.srt_receiver_thread_running = 1;
        srt_receive_thread_listener_struct *srt_receive_data = (srt_receive_thread_listener_struct*)malloc(sizeof(srt_receive_thread_listener_struct));
        srt_receive_data->core = (srthub_core_struct*)&srtcore;
        sprintf(srt_receive_data->server_address, "%s", server_address);
        sprintf(srt_receive_data->server_interface_name, "%s", sourceinterface);
        srt_receive_data->server_port = server_port;
        memset(&srt_receive_data->streamid, 0, sizeof(srt_receive_data->streamid));
        memset(&srt_receive_data->passphrase, 0, sizeof(srt_receive_data->passphrase));
        if (streamid) {
            sprintf(srt_receive_data->streamid, "%s", streamid);
        }
        if (passphrase) {
            sprintf(srt_receive_data->passphrase, "%s", passphrase);
            srt_receive_data->keysize = keysize;
        } else {
            srt_receive_data->keysize = 0;
        }

        fprintf(stderr,"starting srt receiver thread (listener), address=%s, interface=%s, port=%d\n", server_address, sourceinterface, server_port);
        pthread_create(&srtcore.srt_receiver_thread_id, NULL, srt_receiver_thread_listener, (void*)srt_receive_data);
    }

    if (strncmp(sourcemode,"udp",3)==0) {
        srtcore.udp_receiver_thread_running = 1;
        udp_receiver_thread_struct *udp_receive_data = (udp_receiver_thread_struct*)malloc(sizeof(udp_receiver_thread_struct));
        sprintf(udp_receive_data->source_address, "%s", server_address);
        sprintf(udp_receive_data->interface_name, "%s", sourceinterface);
        udp_receive_data->source_port = server_port;
        udp_receive_data->core = (srthub_core_struct*)&srtcore;
        fprintf(stderr,"starting udp receiver thread\n");
        pthread_create(&srtcore.udp_receiver_thread_id, NULL, udp_receiver_thread, (void*)udp_receive_data);
    }

    fprintf(stderr,"\n\n\n\n\n\nstarting things back up....\n\n\n\n\n\n");

    sprintf(statsfilename,"/opt/srthub/status/corestatus_%d.json", srtcore.session_identifier);

    clock_gettime(CLOCK_MONOTONIC, &uptime_start);

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &uptime_check);
        diff = realtime_clock_difference(&uptime_check, &uptime_start) / 1000;
        if (wait_count >= 1000) {  // 1000 1ms is a second
            FILE *statsfile = fopen(statsfilename,"wb");
            if (statsfile) {
                fprintf(statsfile,"{\n");
                fprintf(statsfile,"    \"srt-version\":\"%d.%d.%d\",\n", (srt_getversion() >> 16) & 0xff, (srt_getversion() >> 8) & 0xff, (srt_getversion() >> 0) & 0xff);
                fprintf(statsfile,"    \"srthub-version\":\"%d.%d\",\n", SRTHUB_MAJOR, SRTHUB_MINOR);
                fprintf(statsfile,"    \"srthub-uptime\":%ld,\n", diff);
                fprintf(statsfile,"    \"session-identifier\":%d,\n", srtcore.session_identifier);
                fprintf(statsfile,"    \"thumbnail-queue\":%d,\n", dataqueue_get_size(srtcore.thumbnailqueue));
                fprintf(statsfile,"    \"udpserver-queue\":%d,\n", dataqueue_get_size(srtcore.udpserverqueue));
                fprintf(statsfile,"    \"pcr-pid\":%d,\n", srtcore.pcr_pid);
                fprintf(statsfile,"    \"video-pid\":%d,\n", srtcore.video_pid);
                fprintf(statsfile,"    \"video-type\":\"%s\",\n", stream_type_name(srtcore.video_stream_type));
                fprintf(statsfile,"    \"audio-pids\":[");
                {
                    int audio_entry;
                    int listed = 0;

                    for (audio_entry = 0; audio_entry < srtcore.audio_pid_count; audio_entry++) {
                        if (srtcore.audio_pid[audio_entry] == 0) {
                            continue;   /* gap in the audio indexes */
                        }
                        fprintf(statsfile,"%s{\"index\":%d,\"pid\":%d,\"type\":\"%s\",\"language\":\"%s\"}",
                                listed ? "," : "",
                                audio_entry,
                                srtcore.audio_pid[audio_entry],
                                stream_type_name(srtcore.audio_stream_type[audio_entry]),
                                srtcore.audio_language[audio_entry]);
                        listed++;
                    }
                }
                fprintf(statsfile,"],\n");
                fprintf(statsfile,"    \"scte35-pid\":%d,\n", srtcore.scte35_pid);
                fprintf(statsfile,"    \"scte35-present\":%d,\n", srtcore.scte35_pid != 0 ? 1 : 0);
                fprintf(statsfile,"    \"scte35-cue-count\":%ld,\n", (long)srtcore.scte35_cue_count);
                if (srtcore.scte35_have_last_cue) {
                    const char *last_cue_text;

                    if (srtcore.scte35_last_cue_cancel) {
                        last_cue_text = "none";
                    } else if (srtcore.scte35_last_cue_direction == SCTE35_CUE_OUT) {
                        last_cue_text = "out";
                    } else if (srtcore.scte35_last_cue_direction == SCTE35_CUE_IN) {
                        last_cue_text = "in";
                    } else {
                        last_cue_text = "none";
                    }

                    fprintf(statsfile,"    \"scte35-last-cue\":\"%s\",\n", last_cue_text);
                    fprintf(statsfile,"    \"scte35-last-cue-name\":\"%s\",\n", srtcore.scte35_last_cue_name);
                    fprintf(statsfile,"    \"scte35-last-cue-immediate\":%d,\n", srtcore.scte35_last_cue_immediate);
                    fprintf(statsfile,"    \"scte35-last-cue-duration\":%.3f,\n",
                            (double)srtcore.scte35_last_cue_duration / (double)90000.0);
                    fprintf(statsfile,"    \"scte35-last-cue-event-id\":%ld,\n", (long)srtcore.scte35_last_event_id);
                    fprintf(statsfile,"    \"scte35-last-cue-cancel\":%d,\n", srtcore.scte35_last_cue_cancel);
                    fprintf(statsfile,"    \"scte35-last-cue-time\":%ld\n", (long)srtcore.scte35_last_cue_time);
                } else {
                    fprintf(statsfile,"    \"scte35-last-cue\":\"\"\n");
                }
                fprintf(statsfile,"}\n");
                fclose(statsfile);
            }
            wait_count = 0;
        }
        dataqueue_message_struct *msg = dataqueue_take_back(srtcore.msgqueue);
        if (msg) {
            if (msg->flags == MESSAGE_TYPE_RESTART) {
                fprintf(stderr,"main: restart message received\n");
                if (strncmp(sourcemode,"srt",3)==0) {
                    srtcore.srt_receiver_thread_running = 0;
                    fprintf(stderr,"main: stopping srt_receiver_thread\n");
                    pthread_join(srtcore.srt_receiver_thread_id, NULL);
                    fprintf(stderr,"main: done stopping srt_receiver_thread\n");
                }
                if (strncmp(outputmode,"udp",3)==0) {
                    srtcore.udp_server_thread_running = 0;
                    fprintf(stderr,"main: stopping udp_server_thread\n");
                    pthread_join(srtcore.udp_server_thread_id, NULL);
                    fprintf(stderr,"main: done stopping udp_server_thread\n");
                }
                if (strncmp(sourcemode,"udp",3)==0) {
                    srtcore.udp_receiver_thread_running = 0;
                    fprintf(stderr,"main: stopping udp_receiver_thread\n");
                    pthread_join(srtcore.udp_receiver_thread_id, NULL);
                    fprintf(stderr,"main: done stopping udp_receiver_thread\n");
                }
                if (strncmp(outputmode,"srt",3)==0) {
                    srtcore.srt_server_thread_running = 0;
                    fprintf(stderr,"main: stopping srt_server_thread\n");
                    pthread_join(srtcore.srt_server_thread_id, NULL);
                    fprintf(stderr,"main: done stopping srt_server_thread\n");
                }
                if (srtcore.video_initialized) {
                    srtcore.thumbnail_thread_running = 0;
                    fprintf(stderr,"main: stopping thumbnail_thread\n");
                    pthread_join(srtcore.thumbnail_thread_id, NULL);
                    fprintf(stderr,"main: done stopping thumbnail thread\n");
                    memory_destroy(srtcore.videopool);
                    srtcore.videopool = NULL;
                    srtcore.video_initialized = 0;
                }

                for (thread = 0; thread < MAX_WORKER_THREADS; thread++) {
                    srtcore.audio_decode_thread_running[thread] = 0;
                    fprintf(stderr,"main: stopping audio decode thread (%d)\n", thread);
                    pthread_join(srtcore.audio_decode_thread_id[thread], NULL);
                    fprintf(stderr,"maun: done stopping audio decode thread (%d)\n", thread);
                }
            }
            memory_return(srtcore.msgpool, msg);
            msg = NULL;
            wait_count = 0;
            fprintf(stderr,"main: restarting\n");
            goto restart_srt;
        } else {
            usleep(1000);
            wait_count++;
        }
    }

    stop_signal_thread(&srtcore);

    dataqueue_destroy(srtcore.msgqueue);
    srtcore.msgqueue = NULL;
    dataqueue_destroy(srtcore.thumbnailqueue);
    srtcore.thumbnailqueue = NULL;
    dataqueue_destroy(srtcore.udpserverqueue);
    srtcore.udpserverqueue = NULL;
    dataqueue_destroy(srtcore.signalqueue);
    for (thread = 0; thread < MAX_WORKER_THREADS; thread++) {
        dataqueue_destroy(srtcore.srtserverqueue[thread]);
        srtcore.srtserverqueue[thread] = NULL;
        dataqueue_destroy(srtcore.audiodecodequeue[thread]);
        srtcore.audiodecodequeue[thread] = NULL;
    }
    pthread_mutex_destroy(srtcore.srtserverlock);

    memory_destroy(srtcore.msgpool);
    srtcore.msgpool = NULL;
    memory_destroy(srtcore.packetpool);
    srtcore.packetpool = NULL;
    if (srtcore.videopool) {
        memory_destroy(srtcore.videopool);
        srtcore.videopool = NULL;
    }
    pthread_mutex_destroy(srtcore.video_init_lock);
    free(srtcore.video_init_lock);
    srtcore.video_init_lock = NULL;
    memory_destroy(srtcore.audiopool);
    srtcore.audiopool = NULL;

    srt_cleanup();

    return 0;
}

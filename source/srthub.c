/*****************************************************************************
  Copyright (C) 2018-2023 John William

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
#include "srt.h"
#include "mempool.h"
#include "srthub.h"
#include "udpsource.h"
#include "tsdecode.h"
#include "dataqueue.h"
#include "esignal.h"
#include "cJSON.h"

#define SRTHUB_MAJOR 0
#define SRTHUB_MINOR 1

#define MAX_CONFIG_SIZE 16384
#define MAX_UDP_BUFFER_READ 2048

#define MESSAGE_TYPE_START 0x01
#define MESSAGE_TYPE_STOP 0x02
#define MESSAGE_TYPE_RESTART 0x99

#define MAX_MSG_BUFFERS            16384*8
#define MAX_PACKET_BUFFERS         16384*8
#define MAX_PACKET_BUFFER_SIZE     1536
#define MAX_THUMBNAIL_BUFFERS      384
#define MAX_THUMBNAIL_BUFFER_SIZE  1024*1024*4

#define ENABLE_THUMBNAIL

#if defined(ENABLE_THUMBNAIL)
#define THUMBNAIL_WIDTH   320
#define THUMBNAIL_HEIGHT  240
#define MAX_DECODE_WIDTH  3840
#define MAX_DECODE_HEIGHT 2160

#include "../cbffmpeg/libavcodec/avcodec.h"
#include "../cbffmpeg/libswscale/swscale.h"
#include "../cbffmpeg/libavutil/pixfmt.h"
#include "../cbffmpeg/libavutil/log.h"
#include "../cbffmpeg/libavutil/opt.h"
#include "../cbffmpeg/libavutil/imgutils.h"
#include "../cbffmpeg/libavformat/avformat.h"
#include "../cbffmpeg/libavfilter/buffersink.h"
#include "../cbffmpeg/libavfilter/buffersrc.h"
#endif

typedef struct _srt_server_worker_output_thread_struct_ {
    int          thread;
    srthub_core_struct *core;
    SRTSOCKET    client_sock;
    char         client_address[MAX_STRING_SIZE];
    int          client_port;
} srt_server_worker_output_thread_struct;

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

int save_frame_as_jpeg(srthub_core_struct *srtcore, AVFrame *pFrame)
{
    AVCodec *jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *jpegContext = avcodec_alloc_context3(jpegCodec);
    FILE *JPEG = NULL;
#define MAX_FILENAME_SIZE 256
    char temp_filename[MAX_FILENAME_SIZE];
    char actual_filename[MAX_FILENAME_SIZE];
    AVPacket packet = {.data = NULL, .size = 0};
    int encodedFrame = 0;

    jpegContext->bit_rate = 500000;
    jpegContext->width = THUMBNAIL_WIDTH;
    jpegContext->height = THUMBNAIL_HEIGHT;
    jpegContext->time_base = (AVRational){1,30};
    jpegContext->pix_fmt = AV_PIX_FMT_YUVJ420P;

    avcodec_open2(jpegContext, jpegCodec, NULL);
    av_init_packet(&packet);
    avcodec_encode_video2(jpegContext, &packet, pFrame, &encodedFrame);

    snprintf(temp_filename, MAX_FILENAME_SIZE-1, "/opt/srthub/thumbnail/%d.jpg.temp", srtcore->session_identifier);
    snprintf(actual_filename, MAX_FILENAME_SIZE-1, "/opt/srthub/thumbnail/%d.jpg", srtcore->session_identifier);
    JPEG = fopen(temp_filename, "wb");
    if (JPEG) {
        fwrite(packet.data, 1, packet.size, JPEG);
        fclose(JPEG);
        rename(temp_filename,actual_filename);
    }
    av_free_packet(&packet);
    avcodec_free_context(&jpegContext);
    return 0;
}

static int receive_frame(uint8_t *sample, int sample_size, int sample_type, uint32_t sample_flags,
                         int64_t pts, int64_t dts, int64_t last_pcr, int source,
                         int sub_source, char *lang_tag, int64_t corruption_count, int muxstreams, void *context)
{
    srthub_core_struct *srtcore = (srthub_core_struct*)context;
    dataqueue_message_struct *msg;
    int threadid = gettid();

    /*fprintf(stderr,"received frame (%d/%d): source=%d, sub_source=%d, type=0x%x, corruption_count=%ld, size=%d\n",
      source+1, muxstreams, source, sub_source, sample_type, corruption_count, sample_size);*/

    if (sample_type == STREAM_TYPE_H264 || sample_type == STREAM_TYPE_HEVC || sample_type == STREAM_TYPE_MPEG2) {
        //if (sample_flags == 1)
        {
            /*fprintf(stderr,"received frame (%d/%d): source=%d, sub_source=%d, type=0x%x, corruption_count=%ld, size=%d\n",
              source+1, muxstreams, source, sub_source, sample_type, corruption_count, sample_size);*/

            /*if (dataqueue_get_size(srtcore->thumbnailqueue) > 0) {
                fprintf(stderr,"received frame: thumbnail queue already has a sample... waiting\n");
                return 0;
            }*/

            int i;
            for (i = 0; i < sample_size-3; i++) {
                if (sample[i] == 0x00 && sample[i+1] == 0x00 && sample[i+2] == 0x01) {
                    //fprintf(stderr,"received frame: start code found pos %4d: type=0x%x\n", i, sample[i+3]);
                }
            }

            msg = (dataqueue_message_struct*)memory_take(srtcore->msgpool, threadid);
            if (msg) {
                uint8_t *buffer = (uint8_t*)memory_take(srtcore->videopool, threadid);
                if (buffer) {
                    memcpy(buffer, sample, sample_size);
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
                fprintf(stderr,"received frame: msg buffers exhausted, thumbnailqueue=%d\n", dataqueue_get_size(srtcore->msgpool));
            }
        }
    }

    return 0;
}

static int send_restart_message(srthub_core_struct *srtcore)
{
    dataqueue_message_struct *msg;
    int threadid = gettid();

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

static void *srt_receiver_thread_listener(void *context)
{
    srt_receive_thread_listener_struct *srtdata;
    srthub_core_struct *srtcore;
    transport_data_struct *decode = (transport_data_struct*)malloc(sizeof(transport_data_struct));
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET client_sock = SRT_INVALID_SOCK;
    struct sockaddr_in server_addr;
    struct in_addr local_address;
    char statsfilename[MAX_STRING_SIZE];
    int srt_connected = 0;
    int srterr;
    int no = 0;
    int recvbytes;
    int update_stats = 0;
    char *buffer = NULL;
    struct timeval connect_start;
    struct timeval connect_stop;
    SRT_TRACEBSTATS stats;
    int threadid = gettid();

    decode->pat_version_number = -1;

    fprintf(stderr,"srt_receiver_thread_listener: srt_startup() running, thread_id=%d\n", threadid);
    srt_startup();
    fprintf(stderr,"srt_receiver_thread_listener: srt_startup() done, thread_id=%d\n", threadid);

    srtdata = (srt_receive_thread_listener_struct*)context;
    srtcore = srtdata->core;

    sprintf(statsfilename,"/opt/srthub/status/srt_receiver_%d.json", srtcore->session_identifier);

    listener = srt_create_socket();
    if (listener == SRT_ERROR) {
        fprintf(stderr,"srt_receiver_thread_listener: srt_create_socket() failed\n");
        free(srtdata);
        srt_cleanup();
        return NULL;
    }

    inet_aton(srtdata->server_address, &local_address);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(srtdata->server_port);
    server_addr.sin_addr.s_addr = local_address.s_addr;

    srterr = srt_setsockflag(listener, SRTO_SNDSYN, &no, sizeof(no));
    if (srterr == SRT_ERROR) {
        fprintf(stderr,"srt_receiver_thread_listener: unable to proceed with srt_setsockflag()\n");
        goto cleanup_srt_receiver_thread_listener;
    }

    int passphrase_length = strlen(srtdata->passphrase);
    if (passphrase_length >= 10 && passphrase_length <= 79) {
        srterr = srt_setsockflag(listener, SRTO_PASSPHRASE, srtdata->passphrase, passphrase_length);
        if (srterr == SRT_ERROR) {
            fprintf(stderr,"srt_receiver_thread_listener: unable to proceed with srt_setsockflag()\n");
            goto cleanup_srt_receiver_thread_listener;
        }
    }

    int streamid_length = strlen(srtdata->streamid);
    if (streamid_length > 0) {
        srterr = srt_setsockflag(listener, SRTO_STREAMID, srtdata->streamid, streamid_length);
        if (srterr == SRT_ERROR) {
            fprintf(stderr,"srt_receiver_thread_listener: unable to proceed with srt_setsockflag()\n");
            goto cleanup_srt_receiver_thread_listener;
        }
    }

    srterr = srt_bind(listener, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (srterr == SRT_ERROR) {
        srt_close(listener);
        srt_cleanup();
        free(srtdata);
        free(decode);
        srtdata = NULL;
        decode = NULL;
        return NULL;
    }

    buffer = (char*)malloc(MAX_UDP_BUFFER_READ);

    fprintf(stderr,"srt_receiver_thread_listener: starting main thread loop\n");

    gettimeofday(&connect_start, NULL);
    while (srtcore->srt_receiver_thread_running) {
        srterr = srt_listen(listener, 1);  // only one
        if (srterr == SRT_ERROR) {
            // flag the error
            goto cleanup_srt_receiver_thread_listener;
        }

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

                    gettimeofday(&connect_stop, NULL);
                    delta_time_no_connection = (int64_t)get_time_difference(&connect_stop, &connect_start) / 1000;
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
                usleep(100);
            } else if (recvbytes == 0) {
                usleep(100);
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
                gettimeofday(&connect_start, NULL);
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
                            fprintf(statsfile,"    \"loss-percentage\":%.2f,\n", (double)stats.pktRcvLossTotal / (double)stats.pktRecvTotal * (double)100.0);
                            fprintf(statsfile,"    \"bitrate-kbps\":%.2f,\n", (double)stats.mbpsRecvRate * (double)1000.0);
                            fprintf(statsfile,"    \"rtt\":%.2f\n", (double)stats.msRTT);
                            fprintf(statsfile,"}\n");
                            fclose(statsfile);
                        }
                    }
                }
                update_stats++;
                tp = recvbytes / 188;
                //fprintf(stderr,"decode packets: tp=%d\n", tp);
                decode_packets((uint8_t*)buffer, tp, decode, 0);
                //fprintf(stderr,"done decode packets\n");

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
                            memcpy(obuffer, buffer, recvbytes);
                            msg->buffer = obuffer;
                            msg->buffer_size = recvbytes;
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
    free(decode);
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
    transport_data_struct *decode = (transport_data_struct*)malloc(sizeof(transport_data_struct));
    struct timeval connect_start;
    struct timeval connect_stop;
    int stats_size;
    uint32_t update_stats;
    char buffer[MAX_PACKET_BUFFER_SIZE];
    char statsfilename[MAX_STRING_SIZE];
    int srt_connected = 0;
    int threadid = gettid();

    decode->pat_version_number = -1;

    srt_startup();

    srtdata = (srt_receive_thread_caller_struct*)context;
    srtcore = srtdata->core;

    fprintf(stderr,"srt_receiver_thread_caller: starting srt receiver thread, %d\n", srtcore->session_identifier);

    serversock = srt_create_socket();
    if (serversock == SRT_ERROR) {
        fprintf(stderr,"srt_receiver_thread_caller: unable to srt_create_socket() successfully\n");
        free(decode);
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

    modes = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
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
    gettimeofday(&connect_start, NULL);

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

                gettimeofday(&connect_stop, NULL);
                delta_time_no_connection = (int64_t)get_time_difference(&connect_stop, &connect_start) / 1000;
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
            usleep(100);
        } else if (recvbytes == 0) {
            usleep(100);
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
            gettimeofday(&connect_start, NULL);
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
                        fprintf(statsfile,"    \"loss-percentage\":%.2f,\n", (double)stats.pktRcvLossTotal / (double)stats.pktRecvTotal * (double)100.0);
                        fprintf(statsfile,"    \"bitrate-kbps\":%.2f,\n", (double)stats.mbpsRecvRate * (double)1000.0);
                        fprintf(statsfile,"    \"rtt\":%.2f\n", (double)stats.msRTT);
                        fprintf(statsfile,"}\n");
                        fclose(statsfile);
                    }
                }
            }
            update_stats++;
            tp = recvbytes / 188;
            decode_packets((uint8_t*)buffer, tp, decode, 0);

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
                        memcpy(obuffer, buffer, recvbytes);
                        msg->buffer = obuffer;
                        msg->buffer_size = recvbytes;
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
    free(decode);
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
        pthread_mutex_lock(srtcore->srtserverlock);
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->srtserverqueue[thread]);
        pthread_mutex_unlock(srtcore->srtserverlock);

        while (!msg && srtcore->srt_server_worker_thread_running[thread]) {
            usleep(1000);
            pthread_mutex_lock(srtcore->srtserverlock);
            msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->srtserverqueue[thread]);
            pthread_mutex_unlock(srtcore->srtserverlock);
        }

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
            usleep(100);
        } else if (sent_bytes == 0) {
            usleep(100);
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
    return NULL;
}

static void *srt_server_thread_pull(void *context)
{
    srt_server_thread_struct *srtdata;
    srthub_core_struct *srtcore;
    dataqueue_message_struct *msg;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    struct sockaddr_in server_addr;
    struct in_addr local_address;
    int no = 0;
    int srterr;
    int thread = 0;
    int slots_available = 0;
    int64_t total_bytes_sent = 0;
    int64_t total_packets_sent = 0;

    srt_startup();

    srtdata = (srt_server_thread_struct*)context;
    srtcore = srtdata->core;

    listener = srt_create_socket();
    if (listener == SRT_ERROR) {
        // flag the error
        srt_cleanup();
        return NULL;
    }

    inet_aton(srtdata->server_address, &local_address);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(srtdata->server_port);
    server_addr.sin_addr.s_addr = local_address.s_addr;

    srterr = srt_setsockflag(listener, SRTO_SNDSYN, &no, sizeof(no));
    if (srterr == SRT_ERROR) {
        // srt_getlasterror_str();
        fprintf(stderr,"srt_server_thread: unable to proceed with srt_setsockflag()\n");
        goto cleanup_srt_server_thread_pull;
    }

    int passphrase_length = strlen(srtdata->passphrase);
    if (passphrase_length >= 10 && passphrase_length <= 79) {
        srterr = srt_setsockflag(listener, SRTO_PASSPHRASE, srtdata->passphrase, passphrase_length);
        if (srterr == SRT_ERROR) {
            // srt_getlasterror_str();
            fprintf(stderr,"srt_server_thread: unable to proceed with srt_setsockflag()\n");
            goto cleanup_srt_server_thread_pull;
        }
    }

    int streamid_length = strlen(srtdata->streamid);
    if (streamid_length > 0) {
        srterr = srt_setsockflag(listener, SRTO_STREAMID, srtdata->streamid, streamid_length);
        if (srterr == SRT_ERROR) {
            // srt_getlasterror_str();
            fprintf(stderr,"srt_server_thread: unable to proceed with srt_setsockflag()\n");
            goto cleanup_srt_server_thread_pull;
        }
    }

    srterr = srt_bind(listener, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (srterr == SRT_ERROR) {
        // flag the error
        free(srtdata);
        srt_close(listener);
        srt_cleanup();
        return NULL;
    }

    while (srtcore->srt_server_thread_running) {
        srterr = srt_listen(listener, MAX_WORKER_THREADS);
        if (srterr == SRT_ERROR) {
            // flag the error
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
            usleep(100);
        } else if (sent_bytes == 0) {
            usleep(100);
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
    transport_data_struct *decode = (transport_data_struct*)malloc(sizeof(transport_data_struct));
    int64_t total_bytes_received = 0;
    int64_t total_packets_received = 0;
    struct timespec receive_time_stop;
    struct timespec receive_time_start;
    struct timespec signal_check_stop;
    struct timespec signal_check_start;
    int threadid = gettid();

    decode->pat_version_number = -1;

    udpdata = (udp_receiver_thread_struct*)context;
    srtcore = udpdata->core;

    udp_buffer = (uint8_t*)malloc(MAX_UDP_BUFFER_READ);
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
            if (udp_socket > 0) {
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

        anysignal = socket_udp_ready(udp_socket, timeout_ms, &sockset);
        if (anysignal == 0) {
            no_signal_count++;
            source_interruptions++;
            input_signal = 0;

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
                decode_packets((uint8_t*)udp_buffer, tp, decode, 0);

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
                            if (bytes_read > MAX_PACKET_BUFFER_SIZE) {
                                bytes_read = MAX_PACKET_BUFFER_SIZE;
                            }
                            memcpy(outputbuffer, udp_buffer, bytes_read);
                            msg = (dataqueue_message_struct*)memory_take(srtcore->msgpool, threadid);
                            if (msg) {
                                memset(msg, 0, sizeof(dataqueue_message_struct));
                                msg->buffer = outputbuffer;
                                msg->buffer_size = bytes_read;
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
    if (udp_socket > 0) {
        socket_udp_close(udp_socket);
    }
    free(udp_buffer);
    free(decode);
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
    struct ifaddrs *ifaddr;
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
    setsockopt(output_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    inet_aton(udpdata->destination_address, &output_address);
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = output_address.s_addr;
    destination.sin_port = htons(udpdata->destination_port);

    getifaddrs(&ifaddr);
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
    freeifaddrs(ifaddr);
    inet_aton(host, &interface_address);
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
    while (srtcore->udp_server_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->udpserverqueue);

        if (!msg) {
            clock_gettime(CLOCK_MONOTONIC, &signal_check_start);
        }
        while (!msg && srtcore->udp_server_thread_running) {
            usleep(1000);
            msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->udpserverqueue);
            if (!msg) {
                clock_gettime(CLOCK_MONOTONIC, &signal_check_stop);
                diff = realtime_clock_difference(&signal_check_stop, &signal_check_start) / 1000;
                if (diff >= 2000) {  // 2 second timeout
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
                }
            }
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

static void *srthub_thumbnail_thread(void *context)
{
#if defined(ENABLE_THUMBNAIL)
    srthub_core_struct *srtcore = NULL;
    AVCodecContext *decode_avctx = NULL;
    AVCodec *decode_codec = NULL;
    AVPacket *decode_pkt = NULL;
    AVFrame *decode_av_frame = NULL;
    enum AVPixelFormat source_format = AV_PIX_FMT_YUV420P;
    enum AVPixelFormat output_format = AV_PIX_FMT_YUV420P;
    struct SwsContext *decode_converter = NULL;
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

    srtcore = (srthub_core_struct*)context;

    sprintf(statsfilename,"/opt/srthub/status/thumbnail_%d.json", srtcore->session_identifier);

    memset(corruptiontimedate, 0, sizeof(corruptiontimedate));
    while (srtcore->thumbnail_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->thumbnailqueue);
        while (!msg && srtcore->thumbnail_thread_running) {
            usleep(1000);
            msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->thumbnailqueue);
        }

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

            corruption_count = corruption_count - 1;  // hack for the startup condition that needs to be fixed (since we always report one at startup)
            if (corruption_count < 0) {
                corruption_count = 0;
            }

            if (corruption_count > 0 && corruption_count != srtcore->last_corruption_count) {
                int l;
                srtcore->last_corruption_time = time(NULL);
                srtcore->last_corruption_time = timegm(localtime(&srtcore->last_corruption_time));
                struct tm* local_time = localtime(&srtcore->last_corruption_time);
                sprintf(corruptiontimedate, "%s", asctime(local_time));
                l = strlen(corruptiontimedate);
                if (l > 0) {
                    corruptiontimedate[l-1] = '\0'; // remove the \n
                }
                srtcore->last_corruption_count = corruption_count;
            }

            if (!video_decoder_ready) {
                if (buffer_type == STREAM_TYPE_H264) {
                    decode_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
                } else if (buffer_type == STREAM_TYPE_MPEG2) {
                    decode_codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
                } else if (buffer_type == STREAM_TYPE_HEVC) {
                    decode_codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
                //} else if (buffer_type == STREAM_TYPE_AV1) {
                //decode_codec = avcodec_find_decoder(AV_CODEC_ID_AV1);
                } else {
                    // unknown codec
                }
                if (decode_codec) {
                    decode_avctx = avcodec_alloc_context3(decode_codec);
                    avcodec_open2(decode_avctx, decode_codec, NULL);
                    decode_av_frame = av_frame_alloc();
                    decode_pkt = av_packet_alloc();
                    video_decoder_ready = 1;
                }
            }
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

            decode_pkt->size = buffer_size;
            decode_pkt->data = buffer;
            decode_pkt->pts = 0;
            decode_pkt->dts = 0;

            ret = avcodec_send_packet(decode_avctx, decode_pkt);

            while (ret >= 0) {
                int is_frame_interlaced;
                int is_frame_tff;
                int frame_height;
                int frame_height2;
                int frame_width;
                int frame_width2;
                int row;
                AVFrame *jpeg_frame;

                ret = avcodec_receive_frame(decode_avctx, decode_av_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                if (ret < 0) {
                    break;
                }

                decoded_frame_count++;

                is_frame_interlaced = decode_av_frame->interlaced_frame;
                is_frame_tff = decode_av_frame->top_field_first;
                source_format = decode_av_frame->format;
                frame_height = decode_avctx->height;
                frame_width = decode_avctx->width;

                source_data[0] = decode_av_frame->data[0];
                source_data[1] = decode_av_frame->data[1];
                source_data[2] = decode_av_frame->data[2];
                source_data[3] = decode_av_frame->data[3];
                source_stride[0] = decode_av_frame->linesize[0];
                source_stride[1] = decode_av_frame->linesize[1];
                source_stride[2] = decode_av_frame->linesize[2];
                source_stride[3] = decode_av_frame->linesize[3];

                if (!decode_converter) {
                    decode_converter = sws_getContext(frame_width, frame_height, source_format,
                                                      THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, output_format,
                                                      SWS_BICUBIC, NULL, NULL, NULL);
                    av_image_alloc(output_data, output_stride, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, output_format, 1);
                }

                if ((decoded_frame_count % 120)==0) {
                    /*
                    fprintf(stderr,"srt_thumbnail_thread: decoded video frame, resolution is %d x %d\n",
                            frame_width, frame_height);
                    */
                    char codec[MAX_STRING_SIZE];
                    if (buffer_type == STREAM_TYPE_H264) {
                        sprintf(codec,"h264");
                    } else if (buffer_type == STREAM_TYPE_MPEG2) {
                        sprintf(codec,"mpeg2");
                    } else if (buffer_type == STREAM_TYPE_HEVC) {
                        sprintf(codec,"hevc");
                    } else {
                        sprintf(codec,"unknown");
                    }
                    FILE *statsfile = fopen(statsfilename,"wb");
                    if (statsfile) {
                        fprintf(statsfile,"{\n");
                        fprintf(statsfile,"    \"width\":%d,\n", frame_width);
                        fprintf(statsfile,"    \"height\":%d,\n", frame_height);
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
                    jpeg_frame->pkt_pts = AV_NOPTS_VALUE;
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

                    save_frame_as_jpeg(srtcore, jpeg_frame);

                    av_frame_free(&jpeg_frame);
                }
            }
            memory_return(srtcore->videopool, buffer);
            memory_return(srtcore->msgpool, msg);
            msg = NULL;
            buffer = NULL;
        }
    }

cleanup_thumbnail_thread:

    av_frame_free(&decode_av_frame);
    av_packet_free(&decode_pkt);
    avcodec_close(decode_avctx);
    avcodec_free_context(&decode_avctx);
    if (decode_converter) {
        sws_freeContext(decode_converter);
        av_freep(&output_data[0]);
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

int srthub_read_config(char *filename, srthub_configuration_struct *config)
{
    FILE *configfile;
    int br;
    char configbuffer[MAX_CONFIG_SIZE];

    memset(config, 0, sizeof(srthub_configuration_struct));

    configfile = fopen(filename,"r");
    if (configfile) {
        br = fread(configbuffer, 1, MAX_CONFIG_SIZE-1, configfile);
        if (br > 0) {
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
                latencyms_field = cJSON_GetObjectItem(top,"latencyms");

                fprintf(stderr,"-------------------- configuration options -----------------------\n");
                if (sourcename_field) {
                    snprintf(config->sourcename,MAX_STRING_SIZE-1,"%s",sourcename_field->valuestring);
                    fprintf(stderr,"sourcename:%s\n", config->sourcename);
                }
                if (streamid_field) {
                    snprintf(config->streamid,MAX_STRING_SIZE-1,"%s",streamid_field->valuestring);
                    if (strlen(config->streamid) > 0) {
                        fprintf(stderr,"streamid:%s\n", config->streamid);
                    }
                }
                if (sourcemode_field) {
                    snprintf(config->sourcemode,MAX_STRING_SIZE-1,"%s",sourcemode_field->valuestring);
                    fprintf(stderr,"sourcemode:%s\n", config->sourcemode);
                }
                if (sourceaddress_field) {
                    snprintf(config->sourceaddress,MAX_STRING_SIZE-1,"%s",sourceaddress_field->valuestring);
                    fprintf(stderr,"sourceaddress:%s\n", config->sourceaddress);
                }
                if (sourceinterface_field) {
                    snprintf(config->sourceinterface,MAX_STRING_SIZE-1,"%s",sourceinterface_field->valuestring);
                    fprintf(stderr,"sourceinterface:%s\n", config->sourceinterface);
                }
                if (sourceport_field) {
                    config->sourceport = atoi(sourceport_field->valuestring);
                    fprintf(stderr,"sourceport:%d\n", config->sourceport);
                }
                if (outputmode_field) {
                    snprintf(config->outputmode,MAX_STRING_SIZE-1,"%s",outputmode_field->valuestring);
                    fprintf(stderr,"outputmode:%s\n", config->outputmode);
                }
                if (outputaddress_field) {
                    snprintf(config->outputaddress,MAX_STRING_SIZE-1,"%s",outputaddress_field->valuestring);
                    fprintf(stderr,"outputaddress:%s\n", config->outputaddress);
                }
                if (outputinterface_field) {
                    snprintf(config->outputinterface,MAX_STRING_SIZE-1,"%s",outputinterface_field->valuestring);
                    fprintf(stderr,"outputinterface:%s\n", config->outputinterface);
                }
                if (outputport_field) {
                    config->outputport = atoi(outputport_field->valuestring);
                    fprintf(stderr,"outputport:%d\n", config->outputport);
                }
                if (outputttl_field) {
                    config->outputttl = atoi(outputttl_field->valuestring);
                    fprintf(stderr,"outputttl:%d\n", config->outputttl);
                } else {
                    config->outputttl = 16;
                }
                if (keysize_field) {
                    config->keysize = atoi(keysize_field->valuestring);
                    fprintf(stderr,"keysize:%d\n", config->keysize);
                } else {
                    config->keysize = 0;
                }
                if (passphrase_field) {
                    snprintf(config->passphrase,MAX_STRING_SIZE-1,"%s",passphrase_field->valuestring);
                    if (strlen(config->passphrase) > 0) {
                        fprintf(stderr,"passphrase:%s\n", config->passphrase);
                    }
                }
                if (servermode_field) {
                    snprintf(config->servermode,MAX_STRING_SIZE-1,"%s",servermode_field->valuestring);
                    fprintf(stderr,"servermode:%s\n", config->servermode);
                }
                if (clientmode_field) {
                    snprintf(config->clientmode,MAX_STRING_SIZE-1,"%s",clientmode_field->valuestring);
                    fprintf(stderr,"clientmode:%s\n", config->clientmode);
                }
                if (managementserverip_field) {
                    snprintf(config->managementip,MAX_STRING_SIZE-1,"%s",managementserverip_field->valuestring);
                    if (strlen(config->managementip) > 0) {
                        fprintf(stderr,"managementip:%s\n", config->managementip);
                    }
                }
                if (latencyms_field) {
                    config->latencyms = atoi(latencyms_field->valuestring);
                    fprintf(stderr,"latencyms:%d\n", config->latencyms);
                } else {
                    config->latencyms = 100;
                }
                if (overheadbw_field) {
                    config->overheadbw = atoi(overheadbw_field->valuestring);
                    fprintf(stderr,"overheadbw:%d%%\n", config->overheadbw);
                } else {
                    config->overheadbw = 25;
                }
                if (whitelist_field) {
                    snprintf(config->whitelist,MAX_STRING_SIZE-1,"%s",whitelist_field->valuestring);
                    if (strlen(config->whitelist) > 0) {
                        fprintf(stderr,"whitelist:%s\n", config->whitelist);
                    }
                }

                if (strncmp(config->sourcemode,"srt",3)==0) {
                    if (strncmp(config->clientmode,"push",4)==0) {
                        strncat(config->sourcemode, config->clientmode, MAX_STRING_SIZE-1);
                    }
                    if (strncmp(config->clientmode,"pull",4)==0) {
                        strncat(config->sourcemode, config->clientmode, MAX_STRING_SIZE-1);
                    }
                    fprintf(stderr,"updated sourcemode:%s\n", config->sourcemode);
                }
                if (strncmp(config->outputmode,"srt",3)==0) {
                    if (strncmp(config->servermode,"push",4)==0) {
                        strncat(config->outputmode, config->servermode, MAX_STRING_SIZE-1);
                    }
                    if (strncmp(config->servermode,"pull",4)==0) {
                        strncat(config->outputmode, config->servermode, MAX_STRING_SIZE-1);
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
        fprintf(stderr,"usage: srthub sourcemode sourceaddress sourceport outputmode outputaddress outputport passphrase\n");
        fprintf(stderr,"\n");
        fprintf(stderr,"    sourcemode is [udp, srtpush, srtpull] (where srtpull is Caller mode and srtpush is Listener mode)\n");
        fprintf(stderr,"    sourceaddress is IPv4 unicast IP address in the format of www.xxx.yyy.zzz (domain name not yet supported)\n");
        fprintf(stderr,"    sourceport is IPv4 source port\n");
        fprintf(stderr,"    sourceinterface is the Ethernet Interface\n");
        fprintf(stderr,"    outputmode is [udp, srtpush, srtpull] (where srtpush is Caller mode and srtpull is Listener mode)\n");
        fprintf(stderr,"    outputaddress is IPv4 unicast IP address in the format of www.xxx.yyy.zzz (domain name not yet supported)\n");
        fprintf(stderr,"    outputport is IPv4 output port\n");
        fprintf(stderr,"    outputinterface is the Ethernet Interface\n");
        fprintf(stderr,"    sessionid is a unique number that identifies the instance of the application (unsigned 32-bit)\n");
        fprintf(stderr,"    passphrase is the passphrase if you need to encrypt or decrypt, 10 to 79 characters long (no spaces)\n");
        fprintf(stderr,"    keysize is the size of the key if you need to encrypt or decrypt, 0=default, 16=AES-128, 24=AES-192 and 32=AES-256\n");
        fprintf(stderr,"    streamid is the streamid as defined by srt (no spaces)\n");
        fprintf(stderr,"    \n");
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
    srtcore.videopool = memory_create(MAX_THUMBNAIL_BUFFERS, MAX_THUMBNAIL_BUFFER_SIZE);

    srtcore.session_identifier = session_identifier;
    srtcore.msgqueue = dataqueue_create();
    srtcore.thumbnailqueue = dataqueue_create();
    srtcore.udpserverqueue = dataqueue_create();
    srtcore.signalqueue = dataqueue_create();
    for (thread = 0; thread < MAX_WORKER_THREADS; thread++) {
        srtcore.srtserverqueue[thread] = NULL;
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

    srtcore.thumbnail_thread_running = 1;
    pthread_create(&srtcore.thumbnail_thread_id, NULL, srthub_thumbnail_thread, &srtcore);

    if (strncmp(outputmode,"udp",3)==0) {
        srtcore.udp_server_thread_running = 1;
        udp_server_thread_struct *udp_server_data = (udp_server_thread_struct*)malloc(sizeof(udp_server_thread_struct));
        sprintf(udp_server_data->interface_name, "%s", outputinterface);
        sprintf(udp_server_data->destination_address, "%s", output_address);
        udp_server_data->destination_port = output_port;
        udp_server_data->ttl = 8;
        udp_server_data->core = (srthub_core_struct*)&srtcore;
        fprintf(stderr,"starting udp server thread\n");
        pthread_create(&srtcore.udp_server_thread_id, NULL, udp_server_thread, udp_server_data);
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
        pthread_create(&srtcore.srt_server_thread_id, NULL, srt_server_thread_pull, srt_server_data);
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
        pthread_create(&srtcore.srt_server_thread_id, NULL, srt_server_thread_push, srt_server_data);
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
        pthread_create(&srtcore.srt_receiver_thread_id, NULL, srt_receiver_thread_caller, srt_receive_data);
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
        pthread_create(&srtcore.srt_receiver_thread_id, NULL, srt_receiver_thread_listener, srt_receive_data);
    }

    if (strncmp(sourcemode,"udp",3)==0) {
        srtcore.udp_receiver_thread_running = 1;
        udp_receiver_thread_struct *udp_receive_data = (udp_receiver_thread_struct*)malloc(sizeof(udp_receiver_thread_struct));
        sprintf(udp_receive_data->source_address, "%s", server_address);
        sprintf(udp_receive_data->interface_name, "%s", sourceinterface);
        udp_receive_data->source_port = server_port;
        udp_receive_data->core = (srthub_core_struct*)&srtcore;
        fprintf(stderr,"starting udp receiver thread\n");
        pthread_create(&srtcore.udp_receiver_thread_id, NULL, udp_receiver_thread, udp_receive_data);
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
                fprintf(statsfile,"    \"udpserver-queue\":%d\n", dataqueue_get_size(srtcore.udpserverqueue));
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
                srtcore.thumbnail_thread_running = 0;
                fprintf(stderr,"main: stopping thumbnail_thread\n");
                pthread_join(srtcore.thumbnail_thread_id, NULL);
                fprintf(stderr,"main: done stopping thumbnail thread\n");
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
    }
    pthread_mutex_destroy(srtcore.srtserverlock);

    memory_destroy(srtcore.msgpool);
    srtcore.msgpool = NULL;
    memory_destroy(srtcore.packetpool);
    srtcore.packetpool = NULL;
    memory_destroy(srtcore.videopool);
    srtcore.videopool = NULL;

    srt_cleanup();

    return 0;
}

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include "srt.h"
#include "tsdecode.h"
#include "dataqueue.h"

#define SRTHUB_MAJOR 0
#define SRTHUB_MINOR 1

#define MAX_STRING_SIZE 512
#define MAX_PACKET_BUFFER_SIZE 1536
#define MAX_CONFIG_SIZE 16384

#define MESSAGE_TYPE_START 0x01
#define MESSAGE_TYPE_STOP 0x02
#define MESSAGE_TYPE_RESTART 0x99

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

typedef struct _input_statistics_struct_ {
    int          connected;
    int          signal;
    int64_t      bytes;
} input_statistics_struct;

typedef struct _output_statistics_struct_ {


} output_statistics_struct;

typedef struct _statistics_struct_ {
    input_statistics_struct inputinfo;
    output_statistics_struct outputinfo;
} statistics_struct;

typedef struct _srthub_core_struct_ {
    int session_identifier;
    pthread_t srt_server_thread_id;
    pthread_t udp_server_thread_id;
    pthread_t srt_receiver_thread_id;
    pthread_t udp_receiver_thread_id;
    pthread_t thumbnail_thread_id;
    pthread_t output_smoothing_thread_id;
    int srt_server_thread_running;
    int udp_server_thread_running;
    int srt_receiver_thread_running;
    int udp_receiver_thread_running;
    int thumbnail_thread_running;
    int output_smoothing_thread_running;
    void *msgqueue;
    void *thumbnailqueue;
    void *udpserverqueue;
    void *smoothingqueue;
} srthub_core_struct;

typedef struct _srt_receive_thread_struct_ {
    char         server_address[MAX_STRING_SIZE];
    int          server_port;
    srthub_core_struct *core;
} srt_receive_thread_struct;

typedef struct _udp_server_thread_struct_ {
    char         destination_address[MAX_STRING_SIZE];
    int          destination_port;
    char         interface_name[MAX_STRING_SIZE];
    int          ttl;
    srthub_core_struct *core;
} udp_server_thread_struct;

typedef struct _output_smoothing_thread_struct_ {
    int64_t      bitrate;
} output_smoothing_thread_struct;

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
                         int sub_source, char *lang_tag, void *context)
{
    srthub_core_struct *srtcore = (srthub_core_struct*)context;
    dataqueue_message_struct *msg;

    if (sample_type == STREAM_TYPE_H264 || sample_type == STREAM_TYPE_HEVC || sample_type == STREAM_TYPE_MPEG2) {
        //if (sample_flags == 1)
        {
            //fprintf(stderr,"received frame: type=0x%x size=%d\n", sample_type, sample_size);

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

            msg = (dataqueue_message_struct*)malloc(sizeof(dataqueue_message_struct));
            if (msg) {
                uint8_t *buffer = (uint8_t*)malloc(sample_size);
                if (buffer) {
                    memcpy(buffer, sample, sample_size);
                    msg->buffer = (void*)buffer;
                    msg->buffer_size = sample_size;
                    msg->buffer_type = sample_type;
                    dataqueue_put_front(srtcore->thumbnailqueue, msg);
                    msg = NULL;
                } else {
                    free(msg);
                }
            }
        }
    }

    return 0;
}

static int send_restart_message(srthub_core_struct *srtcore)
{
    dataqueue_message_struct *msg;

    msg = (dataqueue_message_struct*)malloc(sizeof(dataqueue_message_struct));
    if (msg) {
        msg->flags = MESSAGE_TYPE_RESTART;
        dataqueue_put_front(srtcore->msgqueue, msg);
        msg = NULL;
    }
    return 0;
}

static void *srt_receiver_thread(void *context)
{
    srt_receive_thread_struct *srtdata;
    srthub_core_struct *srtcore;
    int srterr;
    int epollid;
    struct sockaddr_in sa;
    int no = 0;
    int modes;
    int serversock = 0;
    int recvbytes;
    SRT_TRACEBSTATS stats;
    transport_data_struct *decode = (transport_data_struct*)malloc(sizeof(transport_data_struct));
    struct timeval connect_start;
    struct timeval connect_stop;
    int stats_size;
    int update_stats;
    char buffer[MAX_PACKET_BUFFER_SIZE];
    char statsfilename[MAX_STRING_SIZE];

    srt_startup();

    srtdata = (srt_receive_thread_struct*)context;
    srtcore = srtdata->core;

    serversock = srt_create_socket();
    if (serversock == SRT_ERROR) {
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

    fprintf(stderr,"srt_receiver_thread: attempting to connect to %s:%d\n",
            srtdata->server_address,
            srtdata->server_port);

    srterr = inet_pton(AF_INET, srtdata->server_address, &sa.sin_addr);
    if (srterr != 1) {
        goto cleanup_srt_receiver_thread;
    }

    epollid = srt_epoll_create();
    if (epollid == -1) {
        goto cleanup_srt_receiver_thread;
    }

    fprintf(stderr,"srt_receiver_thread: created epollid=%d\n", epollid);

    srterr = srt_setsockflag(serversock, SRTO_RCVSYN, &no, sizeof(no));
    if (srterr == SRT_ERROR) {
        // srt_getlasterror_str();
        goto cleanup_srt_receiver_thread;
    }

    srterr = srt_setsockflag(serversock, SRTO_SNDSYN, &no, sizeof(no));
    if (srterr == SRT_ERROR) {
        // srt_getlasterror_str();
        fprintf(stderr,"srt_receiver_thread: unable to proceed with srt_setsockflag()\n");
        goto cleanup_srt_receiver_thread;
    }

    // set the passphrase

    modes = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    srterr = srt_epoll_add_usock(epollid, serversock, &modes);
    if (srterr == SRT_ERROR) {
        // srt_getlasterror_str();
        fprintf(stderr,"srt_receiver_thread: unable to proceed with srt_epoll_add_usock()\n");
        goto cleanup_srt_receiver_thread;
    }

    fprintf(stderr,"srt_receiver_thread: attempting to proceed with srt_connect()\n");

    srterr = srt_connect(serversock, (struct sockaddr*)&sa, sizeof(sa));
    if (srterr == SRT_ERROR) {
        fprintf(stderr,"srt_receiver_thread: unable to proceed with srt_connect\n");
        // srt_getlasterror_str();
        goto cleanup_srt_receiver_thread;
    }

    fprintf(stderr,"srt_receiver_thread: finished with srt_connect(), serversock=%d\n", serversock);
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
                    fprintf(stderr,"srt_receiver_thread: SRT not connected, waiting...\n");
                    FILE *statsfile = fopen(statsfilename,"wb");
                    if (statsfile) {
                        fprintf(statsfile,"{\n");
                        fprintf(statsfile,"    \"srt-server-address\":\"%s\",\n", srtdata->server_address);
                        fprintf(statsfile,"    \"srt-server-port\":%d,\n", srtdata->server_port);
                        fprintf(statsfile,"    \"srt-connection\":0\n");
                        fprintf(statsfile,"}\n");
                        fclose(statsfile);
                    }
                }
                update_stats++;

                gettimeofday(&connect_stop, NULL);
                delta_time_no_connection = (int64_t)get_time_difference(&connect_stop, &connect_start) / 1000;
                if (delta_time_no_connection >= 3000) {
                    fprintf(stderr,"srt_receiver_thread: SRT waiting too long for connection, is the server up?\n");
                    send_restart_message(srtcore);
                    goto cleanup_srt_receiver_thread;
                }
            } else if (lasterr == SRT_ECONNLOST) {
                fprintf(stderr,"srt_receiver_thread: SRT connection has been lost!\n");
                send_restart_message(srtcore);
                goto cleanup_srt_receiver_thread;
            } else if (lasterr == SRT_EASYNCRCV) {

            } else {
                fprintf(stderr,"srt_receiver_thread: SRT unknown error: %s\n", srt_getlasterror_str());
            }
            usleep(100);
        } else if (recvbytes == 0) {
            usleep(100);
        } else {
            int cp;
            int tp;
            int current_state;
            int clear_it = 0;
            //current_state = srt_getsockstate(serversock, &stats, &stats_size);

            gettimeofday(&connect_start, NULL);
            if ((update_stats % 100)==0) {
                srterr = srt_bstats(serversock, &stats, clear_it);
                if (srterr != SRT_ERROR) {
                    int64_t now = srt_time_now();
                    fprintf(stderr,"srt_receiver_thread: received %d/%ld bytes (serversock=%d) r=%10ld l=%5d retrans=%5d ack=%d nack=%d d=%8d timestamp=%ld (now=%ld) diff=%ld\n",
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
                    //fprintf(stderr,"srt_receiver_thread: retransmissions detected = %d\n", stats.pktRetransTotal);
                    fprintf(stderr,"srt_receiver_thread: receive rate %.2f mbps @ %ld, %d\n", stats.mbpsRecvRate, stats.msTimeStamp, recvbytes);

                    FILE *statsfile = fopen(statsfilename,"wb");
                    if (statsfile) {
                        fprintf(statsfile,"{\n");
                        fprintf(statsfile,"    \"srt-server-address\":\"%s\",\n", srtdata->server_address);
                        fprintf(statsfile,"    \"srt-server-port\":%d,\n", srtdata->server_port);
                        fprintf(statsfile,"    \"srt-connection\":1,\n");
                        fprintf(statsfile,"    \"srt-time\":%ld,\n", now);
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

                msg = (dataqueue_message_struct*)malloc(sizeof(dataqueue_message_struct));
                if (msg) {
                    obuffer = (uint8_t*)malloc(recvbytes);
                    if (obuffer) {
                        memcpy(obuffer, buffer, recvbytes);
                        msg->buffer = obuffer;
                        msg->buffer_size = recvbytes;
                        msg->pts = srtcontrol.srctime;
                        dataqueue_put_front(srtcore->udpserverqueue, msg);
                    } else {
                        free(msg);
                    }
                }
                msg = NULL;
            }
        }
    }

cleanup_srt_receiver_thread:
    free(decode);
    decode = NULL;
    free(srtdata);
    srtdata = NULL;
    //srterr = srt_epoll_add_usock(epollid, serversock, &modes);
    //srt_epoll_remove_usock()
    srt_epoll_release(epollid);
    epollid = -1;
    srt_close(serversock);
    srt_cleanup();

    return NULL;
}

static void *srt_server_thread(void *context)
{
    srthub_core_struct *srtcore;
    dataqueue_message_struct *msg;
    return NULL;
}

static void *udp_receiver_thread(void *context)
{
    srthub_core_struct *srtcore;
    dataqueue_message_struct *msg;
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

                free(buffer);
                free(msg);
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
    struct in_addr bind_address;
    struct sockaddr_in destination;
    struct ifaddrs *ifaddr;
    struct ifaddrs *ifa;
    int output_socket = 0;
    int yes = 1;
    dataqueue_message_struct *msg;

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
    inet_aton(host, &bind_address);
    //    destination.sin_family = AF_INET;
    //    destination.sin_port = htons(INADDR_ANY);
    //    destination.sin_addr.s_addr = bind_address.s_addr;

    while (srtcore->udp_server_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->udpserverqueue);

        while (!msg && srtcore->udp_server_thread_running) {
            usleep(1000);
            msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->udpserverqueue);
        }

        if (!srtcore->udp_server_thread_running) {
            if (msg) {
                uint8_t *buffer = (uint8_t*)msg->buffer;
                free(buffer);
                free(msg);
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
            //fprintf(stderr,"udp_server_thread: sending %d bytes of data     now:%ld srctime:%ld diff:%ld\n", boutput, scheduled_now, srctime, scheduled_now-srctime);

            free(buffer);
            free(msg);
            msg = NULL;
        }
    }
cleanup_udp_server_thread:
    close(output_socket);

    return NULL;
}

static void *srthub_thumbnail_thread(void *context)
{
#if defined(ENABLE_THUMBNAIL)
    srthub_core_struct *srtcore;
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
    dataqueue_message_struct *msg;
    int video_decoder_ready = 0;
    uint8_t *output_video_frame;
    int64_t decoded_frame_count = 0;

    srtcore = (srthub_core_struct*)context;

    output_video_frame = (uint8_t*)malloc(MAX_DECODE_WIDTH*MAX_DECODE_HEIGHT*3);

    while (srtcore->thumbnail_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->thumbnailqueue);
        while (!msg && srtcore->thumbnail_thread_running) {
            usleep(1000);
            msg = (dataqueue_message_struct*)dataqueue_take_back(srtcore->thumbnailqueue);
        }

        if (!srtcore->thumbnail_thread_running) {
            if (msg) {
                uint8_t *buffer = (uint8_t*)msg->buffer;
                free(buffer);
                free(msg);
                msg = NULL;
            }
            goto cleanup_thumbnail_thread;
        }

        if (msg) {
            uint8_t *buffer = (uint8_t*)msg->buffer;
            int buffer_size = msg->buffer_size;
            int buffer_type = msg->buffer_type;
            int ret;

            if (!video_decoder_ready) {
                if (buffer_type == STREAM_TYPE_H264) {
                    decode_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
                } else if (buffer_type == STREAM_TYPE_MPEG2) {
                    decode_codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
                } else if (buffer_type == STREAM_TYPE_HEVC) {
                    decode_codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
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
                free(buffer);
                free(msg);
                msg = NULL;
                usleep(10000);
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
                uint8_t *output_video_frame;
                int video_frame_size;
                int frame_height;
                int frame_height2;
                int frame_width;
                int frame_width2;
                int row;
                uint8_t *y_output_video_frame;
                uint8_t *u_output_video_frame;
                uint8_t *v_output_video_frame;
                uint8_t *y_source_video_frame;
                uint8_t *u_source_video_frame;
                uint8_t *v_source_video_frame;
                int y_source_stride;
                int uv_source_stride;
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
            free(buffer);
            free(msg);
            msg = NULL;
        }
    }

cleanup_thumbnail_thread:

    free(output_video_frame);
    av_frame_free(&decode_av_frame);
    av_packet_free(&decode_pkt);
    avcodec_close(decode_avctx);
    avcodec_free_context(&decode_avctx);
    if (decode_converter) {
        sws_freeContext(decode_converter);
        av_freep(&output_data[0]);
    }
#endif
    return NULL;
}

/*
int srt_read_config(char *filename)
{
    FILE *configfile;
    int br;
    char configbuffer[MAX_CONFIG_SIZE];

    configfile = fopen(filename,"r");
    if (configfile) {
        br = fread(configbuffer, 1, MAX_CONFIG_SIZE-1, configfile);
        if (br > 0) {
            cJSON *top = cJSON_Parse(configbuffer);
            if (top) {
                cJSON *streamid_field;
                cJSON *sourcemode_field;
                cJSON *sourceaddress_field;
                cJSON *sourceport_field;
                cJSON *outputmode_field;
                cJSON *outputaddress_field;
                cJSON *outputport_field;
                streamid_field = cJSON_GetObjectItem(top,"streamid");

            }
        }
        fclose(configfile);
    }

    return 0;
}
*/

int main(int argc, char **argv)
{
    srthub_core_struct srtcore;
    int session_identifier;
    char statsfilename[MAX_STRING_SIZE];
    int wait_count = 0;
    int64_t uptime = -1;
    struct timespec uptime_start;
    struct timespec uptime_check;
    int64_t diff;

    fprintf(stderr,"srthub (C) Copyright 2023 John William\n");
    fprintf(stderr,"\n");
    fprintf(stderr,"srt version is: %d.%d.%d\n",
            (srt_getversion() >> 16) & 0xff,
            (srt_getversion() >> 8) & 0xff,
            (srt_getversion() >> 0) & 0xff);

    if (argc < 7) {
        fprintf(stderr,"\n");
        fprintf(stderr,"usage: srthub sourcemode sourceaddress sourceport outputmode outputaddress outputport\n");
        fprintf(stderr,"\n");
        fprintf(stderr,"    sourcemode is [udp, srt]\n");
        fprintf(stderr,"    sourceaddress is IPv4 unicast IP address in the format of www.xxx.yyy.zzz (or a domain name such as www.srtlivestream.com/sports)\n");
        fprintf(stderr,"    sourceport is IPv4 source port\n");
        fprintf(stderr,"    outputmode is [udp, srt]\n");
        fprintf(stderr,"    outputaddress is IPv4 unicast IP address in the format of www.xxx.yyy.zzz\n");
        fprintf(stderr,"    outputport is IPv4 output port\n");
        fprintf(stderr,"\n");
        return 0;
    }

    char *sourcemode = (char*)argv[1];
    char *server_address = (char*)argv[2];
    int server_port = atoi(argv[3]);
    char *outputmode = (char*)argv[4];
    char *output_address = (char*)argv[5];
    int output_port = atoi(argv[6]);

    if (argc == 8) {
        session_identifier = atoi(argv[7]);
    } else {
        session_identifier = 1;
    }

    if ((strncmp(sourcemode,"udp",3)==0) || (strncmp(sourcemode,"srt",3)==0)) {
        fprintf(stderr,"source mode is: %s\n", sourcemode);
    } else {
        fprintf(stderr,"\ninvalid source mode: %s    valid options are udp or srt\n\n", sourcemode);
        return -1;
    }
    fprintf(stderr,"sourceaddress is: %s\n", server_address);
    fprintf(stderr,"sourceport is: %d\n", server_port);

    srtcore.session_identifier = session_identifier;
    srtcore.msgqueue = dataqueue_create();
    srtcore.thumbnailqueue = dataqueue_create();
    srtcore.udpserverqueue = dataqueue_create();

restart_srt:
    register_frame_callback(receive_frame, (void*)&srtcore);

    srtcore.thumbnail_thread_running = 1;
    srtcore.srt_receiver_thread_running = 1;
    srtcore.udp_server_thread_running = 1;
    srt_receive_thread_struct *srt_receive_data = (srt_receive_thread_struct*)malloc(sizeof(srt_receive_thread_struct));
    sprintf(srt_receive_data->server_address, "%s", server_address);
    srt_receive_data->server_port = server_port;
    srt_receive_data->core = (srthub_core_struct*)&srtcore;

    udp_server_thread_struct *udp_server_data = (udp_server_thread_struct*)malloc(sizeof(udp_server_thread_struct));
    sprintf(udp_server_data->interface_name,"eno1");
    sprintf(udp_server_data->destination_address, "%s", output_address);
    udp_server_data->destination_port = output_port;
    udp_server_data->ttl = 8;
    udp_server_data->core = (srthub_core_struct*)&srtcore;

    fprintf(stderr,"\n\n\n\n\n\nstarting things back up....\n\n\n\n\n\n");
    pthread_create(&srtcore.thumbnail_thread_id, NULL, srthub_thumbnail_thread, &srtcore);
    pthread_create(&srtcore.udp_server_thread_id, NULL, udp_server_thread, udp_server_data);
    pthread_create(&srtcore.srt_receiver_thread_id, NULL, srt_receiver_thread, srt_receive_data);

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
                fprintf(stderr,"restart message received\n");
                srtcore.srt_receiver_thread_running = 0;
                pthread_join(srtcore.srt_receiver_thread_id, NULL);
                srtcore.thumbnail_thread_running = 0;
                pthread_join(srtcore.thumbnail_thread_id, NULL);
            }
            free(msg);
            msg = NULL;
            wait_count = 0;
            goto restart_srt;
        } else {
            usleep(1000);
            wait_count++;
        }
    }

    dataqueue_destroy(srtcore.msgqueue);
    srtcore.msgqueue = NULL;
    dataqueue_destroy(srtcore.thumbnailqueue);
    srtcore.thumbnailqueue = NULL;
    dataqueue_destroy(srtcore.udpserverqueue);
    srtcore.udpserverqueue = NULL;

    srt_cleanup();

    return 0;
}

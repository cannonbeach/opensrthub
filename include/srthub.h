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
#if !defined(SRTHUB_H)
#define SRTHUB_H

#include <sys/time.h>
#include <time.h>
#include <pthread.h>

#define MAX_STRING_SIZE 512
#define MAX_WORKER_THREADS 8

typedef struct _srthub_configuration_struct_ {
    char sourcename[MAX_STRING_SIZE];
    char streamid[MAX_STRING_SIZE];
    char sourcemode[MAX_STRING_SIZE];
    char sourceaddress[MAX_STRING_SIZE];
    char sourceinterface[MAX_STRING_SIZE];
    int sourceport;
    char outputmode[MAX_STRING_SIZE];
    char outputaddress[MAX_STRING_SIZE];
    char outputinterface[MAX_STRING_SIZE];
    int outputport;
    int outputttl;
    int keysize;
    char passphrase[MAX_STRING_SIZE];
    char servermode[MAX_STRING_SIZE];
    char clientmode[MAX_STRING_SIZE];
    char managementip[MAX_STRING_SIZE];
    int latencyms;
    char whitelist[MAX_STRING_SIZE];
    int overheadbw;
} srthub_configuration_struct;

typedef struct _srthub_core_struct_ {
    int session_identifier;
    pthread_t srt_server_thread_id;
    pthread_t udp_server_thread_id;
    pthread_t srt_receiver_thread_id;
    pthread_t udp_receiver_thread_id;
    pthread_t thumbnail_thread_id;
    pthread_t output_smoothing_thread_id;
    pthread_t audio_decode_thread_id[MAX_WORKER_THREADS];
    int srt_server_thread_running;
    int udp_server_thread_running;
    int srt_receiver_thread_running;
    int udp_receiver_thread_running;
    int thumbnail_thread_running;
    int output_smoothing_thread_running;
    int audio_decode_thread_running[MAX_WORKER_THREADS];
    void *msgqueue;
    void *thumbnailqueue;
    void *audiodecodequeue[MAX_WORKER_THREADS];
    void *udpserverqueue;
    void *smoothingqueue;
    void *signalqueue;
    int64_t last_corruption_count;
    time_t last_corruption_time;
    void *msgpool;
    void *packetpool;
    void *videopool;
    void *audiopool;
    pthread_mutex_t *srtserverlock;
    int srt_server_worker_thread_running[MAX_WORKER_THREADS];
    pthread_t srt_server_worker_thread_id[MAX_WORKER_THREADS];
    void *srtserverqueue[MAX_WORKER_THREADS];
    srthub_configuration_struct *config;
} srthub_core_struct;

#endif

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

/* Must be at least MAX_SCTE35_NAME_SIZE from tsdecode.h. Kept as its own
 * define so this header stays independent of the transport decoder. */
#define MAX_SCTE35_CUE_NAME 64

/* Must match MAX_SUMMARY_AUDIO_PIDS in tsdecode.h, for the same reason. */
#define MAX_SRTHUB_AUDIO_PIDS 8
#define MAX_SRTHUB_LANG_SIZE  4

/* A splice_info section is retransmitted several times a second for the same
 * event, so an identical cue is only reported once. This window re-arms the
 * check so that an encoder which reuses the same event id for every break
 * still produces one event per break rather than one ever. It is longer than
 * any realistic pre-roll repeat interval and shorter than the gap between
 * breaks. */
#define SCTE35_CUE_REPEAT_WINDOW_SECONDS 30

/* How many distinct cues are remembered for the repeat check above. More than
 * one is needed because a single section can carry several segmentation
 * descriptors: those arrive interleaved on every retransmission, so comparing
 * against only the previous cue would report every one of them again each time
 * the section repeats. */
#define SCTE35_RECENT_CUES 8

typedef struct _scte35_recent_cue_struct_ {
    int      valid;
    int64_t  event_id;
    int      cue_direction;
    int      cancel;
    int      segmentation_type;
    time_t   last_seen;
} scte35_recent_cue_struct;

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
    int64_t thumbnail_frame_counter;
    int video_initialized;
    pthread_mutex_t *video_init_lock;
    void *msgpool;
    void *packetpool;
    void *videopool;
    void *audiopool;
    pthread_mutex_t *srtserverlock;
    int srt_server_worker_thread_running[MAX_WORKER_THREADS];
    pthread_t srt_server_worker_thread_id[MAX_WORKER_THREADS];
    void *srtserverqueue[MAX_WORKER_THREADS];
    srthub_configuration_struct *config;

    /* Elementary stream PIDs from the PMT, refreshed by the receive thread at
     * each status tick and read once a second by the main loop's status
     * writer. Audio entries are indexed by the decoder's audio stream index,
     * so entry i lines up with the audio_<i> status file. */
    int      pcr_pid;
    int      video_pid;                      /* 0 when the PMT has no video */
    int      video_stream_type;              /* STREAM_TYPE_* from tsdecode.h */
    int      audio_pid_count;
    int      audio_pid[MAX_SRTHUB_AUDIO_PIDS];
    int      audio_stream_type[MAX_SRTHUB_AUDIO_PIDS];
    char     audio_language[MAX_SRTHUB_AUDIO_PIDS][MAX_SRTHUB_LANG_SIZE];

    /* SCTE-35 state. Written by the receive thread (which is the thread the
     * transport decoder's frame callback runs on) and read once a second by
     * the main loop's status writer. */
    int      scte35_pid;                     /* 0 when the PMT has no SCTE-35 stream */
    int64_t  scte35_cue_count;               /* distinct cues reported since start */
    int      scte35_have_last_cue;           /* 0 until the first cue is reported */
    int64_t  scte35_last_event_id;
    int      scte35_last_cue_direction;      /* SCTE35_CUE_* from tsdecode.h */
    int      scte35_last_cue_cancel;
    int      scte35_last_cue_immediate;
    int64_t  scte35_last_cue_duration;       /* 90kHz ticks, 0 when absent */
    int      scte35_last_cue_command;        /* splice_command_type */
    int      scte35_last_segmentation_type;  /* -1 when not from a time_signal */
    char     scte35_last_cue_name[MAX_SCTE35_CUE_NAME];
    time_t   scte35_last_cue_time;            /* when the last cue was reported */
    /* recently reported cues, for suppressing retransmissions */
    scte35_recent_cue_struct scte35_recent[SCTE35_RECENT_CUES];
} srthub_core_struct;

#endif

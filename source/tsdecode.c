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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <math.h>
#include <syslog.h>

#include "dataqueue.h"
#include "mempool.h"
#include "crc.h"
#include "tsdecode.h"

/* ------------------------------------------------------------------------
 * Defensive limits added during security hardening.
 *
 * These guard the parsers against malformed / hostile transport streams.
 * Each is #ifndef-guarded so that if tsdecode.h already defines an
 * equivalent limit, the header's value wins.
 *
 * IMPORTANT: the three TSHARDEN_* caps below MUST be set equal to (or less
 * than) the real array dimensions in tsdecode.h for the bounds checks to be
 * fully effective:
 *   - TSHARDEN_MAX_STREAMS      -> dimension of pmt_table_struct.stream_pid[],
 *                                  stream_type[], decoded_stream_type[],
 *                                  audio_stream_index[], first/last_pts/dts[],
 *                                  decoded_language_tag[], data_engine[]
 *   - TSHARDEN_MAX_DESCRIPTORS  -> dimension of pmt_table_struct.descriptor_id[]
 *                                  and descriptor_size[]
 *   - TSHARDEN_MAX_PMT_PID_IDX  -> dimension of transport_data_struct.pmt_pid_index[],
 *                                  pmt_version[], pmt_decoded[]
 * The defaults below are conservative; please verify them against your header.
 * ------------------------------------------------------------------------ */
#ifndef TS_PACKET_SIZE
#define TS_PACKET_SIZE  188
#endif
#ifndef TS_PAYLOAD_SIZE
#define TS_PAYLOAD_SIZE 184
#endif
#ifndef TSHARDEN_MAX_STREAMS
#define TSHARDEN_MAX_STREAMS 32
#endif
#ifndef TSHARDEN_MAX_DESCRIPTORS
#define TSHARDEN_MAX_DESCRIPTORS 64
#endif
#ifndef TSHARDEN_MAX_PMT_PID_IDX
#define TSHARDEN_MAX_PMT_PID_IDX MAX_PMT_PIDS
#endif

/* Per-packet/per-table stderr debug logging is a hot-path cost in production.
 * It is now compiled out by default; build with -DTSDECODE_DEBUG=1 to restore. */
#ifndef TSDECODE_DEBUG
#define TSDECODE_DEBUG 0
#endif
#define TSDECODE_DBG(...) do { if (TSDECODE_DEBUG) { fprintf(stderr, __VA_ARGS__); } } while (0)

/* Read a big-endian-stored 32-bit CRC from a byte buffer without an aligned
 * pointer cast (the old code cast to unsigned long*, which is 8 bytes on LP64,
 * over-read 4 bytes, and was undefined behaviour on strict-alignment targets). */
static inline uint32_t tsharden_read_u32(const uint8_t *p)
{
     uint32_t v;
     memcpy(&v, p, sizeof(v));
     return v;
}

static uint64_t total_input_packets = 0;

/* ------------------------------------------------------------------------
 * CONCURRENCY NOTE (review findings #13/#14 - NOT auto-fixed)
 *
 * The state below is process-global and mutable, and the PMT/PAT tables in
 * each transport_data_struct are read and written from decode_packets()
 * without holding pmt_lock, while decode_pmt_table() takes pmt_lock for the
 * same tables. That locking is therefore inconsistent.
 *
 *   - total_input_packets is incremented non-atomically (data race if more
 *     than one demux thread runs).
 *   - backup_caller/backup_context and send_frame_func/send_frame_context are
 *     global rather than per-stream, so two concurrently-decoded sources will
 *     stomp on each other's callbacks.
 *
 * These were intentionally left as-is because the correct fix depends on the
 * threading model (one demux thread per source vs. shared), which is not
 * visible from this file. Recommended remediation:
 *   * move these four callback pointers into transport_data_struct,
 *   * make total_input_packets per-stream (or _Atomic / GCC __atomic_*),
 *   * take pmt_lock around every read/modify of the PMT/PAT tables in
 *     decode_packets(), matching decode_pmt_table().
 * ------------------------------------------------------------------------ */

typedef int (*MYCALLBACK)(int p1, int64_t p2, int64_t p3, int64_t p4, int64_t p5, int source, void *context);
typedef int (*SAMPLE_CALLBACK)(uint8_t *sample, int sample_size, int sample_type, uint32_t sample_flags, int64_t pts, int64_t dts, int64_t last_pcr, int source, int sub_source, char *lang_tag, int64_t corruption_count, int muxstreams, void *context);

static MYCALLBACK backup_caller = NULL;
static void *backup_context = NULL;

static SAMPLE_CALLBACK send_frame_func = NULL;
static void *send_frame_context = NULL;

static pthread_mutex_t pmt_lock = PTHREAD_MUTEX_INITIALIZER;

void register_frame_callback(int (*cbfn)(uint8_t *sample, int sample_size, int sample_type, uint32_t sample_flags, int64_t pts, int64_t dts, int64_t last_pcr, int source, int sub_source, char *lang_tag, int64_t corruption_count, int muxstreams, void *context), void *context)
{
    send_frame_func = cbfn;
    send_frame_context = context;
}

void register_message_callback(int (*cbfn)(int p1,int64_t p2,int64_t p3,int64_t p4, int64_t p5, int source, void* context), void*context)
{
    backup_caller = cbfn;
    backup_context = context;
}

/* Returns the PID carrying SCTE-35 for the first decoded program, or 0 when
 * the PMT has no SCTE-35 stream (or has not been decoded yet). Takes pmt_lock,
 * so it must not be called from code already holding it. */
int get_scte35_pid(transport_data_struct *tsdata)
{
    int scte35_pid = 0;
    int pid_loop;

    if (!tsdata) {
        return 0;
    }

    pthread_mutex_lock(&pmt_lock);
    if (tsdata->pmt_pid_count > 0) {
        pmt_table_struct *current_pmt_table = (pmt_table_struct *)&tsdata->master_pmt_table[0];
        int stream_count = current_pmt_table->stream_count;
        if (stream_count > MAX_STREAMS) {
            stream_count = MAX_STREAMS;
        }
        for (pid_loop = 0; pid_loop < stream_count; pid_loop++) {
            if (current_pmt_table->decoded_stream_type[pid_loop] == STREAM_TYPE_SCTE35) {
                scte35_pid = current_pmt_table->stream_pid[pid_loop];
                break;
            }
        }
    }
    pthread_mutex_unlock(&pmt_lock);

    return scte35_pid;
}

/* segmentation_type_id names from SCTE-35 table 23. */
const char *scte35_segmentation_type_name(int segmentation_type_id)
{
    switch (segmentation_type_id) {
        case 0x00: return "Not Indicated";
        case 0x01: return "Content Identification";
        case 0x10: return "Program Start";
        case 0x11: return "Program End";
        case 0x12: return "Program Early Termination";
        case 0x13: return "Program Breakaway";
        case 0x14: return "Program Resumption";
        case 0x15: return "Program Runover Planned";
        case 0x16: return "Program Runover Unplanned";
        case 0x17: return "Program Overlap Start";
        case 0x18: return "Program Blackout Override";
        case 0x19: return "Program Join";
        case 0x20: return "Chapter Start";
        case 0x21: return "Chapter End";
        case 0x22: return "Break Start";
        case 0x23: return "Break End";
        case 0x24: return "Opening Credit Start";
        case 0x25: return "Opening Credit End";
        case 0x26: return "Closing Credit Start";
        case 0x27: return "Closing Credit End";
        case 0x30: return "Provider Advertisement Start";
        case 0x31: return "Provider Advertisement End";
        case 0x32: return "Distributor Advertisement Start";
        case 0x33: return "Distributor Advertisement End";
        case 0x34: return "Provider Placement Opportunity Start";
        case 0x35: return "Provider Placement Opportunity End";
        case 0x36: return "Distributor Placement Opportunity Start";
        case 0x37: return "Distributor Placement Opportunity End";
        case 0x38: return "Provider Overlay Placement Opportunity Start";
        case 0x39: return "Provider Overlay Placement Opportunity End";
        case 0x3a: return "Distributor Overlay Placement Opportunity Start";
        case 0x3b: return "Distributor Overlay Placement Opportunity End";
        case 0x3c: return "Provider Promo Announcement Start";
        case 0x3d: return "Provider Promo Announcement End";
        case 0x3e: return "Distributor Promo Announcement Start";
        case 0x3f: return "Distributor Promo Announcement End";
        case 0x40: return "Unscheduled Event Start";
        case 0x41: return "Unscheduled Event End";
        case 0x42: return "Alternate Content Opportunity Start";
        case 0x43: return "Alternate Content Opportunity End";
        case 0x44: return "Provider Ad Block Start";
        case 0x45: return "Provider Ad Block End";
        case 0x46: return "Distributor Ad Block Start";
        case 0x47: return "Distributor Ad Block End";
        case 0x50: return "Network Start";
        case 0x51: return "Network End";
        default: break;
    }
    return "";
}

/* Classifies a segmentation_type_id as a cue out or a cue in.
 *
 * Break Start/End (0x22/0x23) and the advertisement, placement opportunity,
 * overlay, promo, unscheduled event, alternate content and ad block ranges
 * (0x30 through 0x47) all alternate even=start, odd=end, so the low bit gives
 * the direction. Program and chapter boundaries carry no ad-break meaning and
 * report SCTE35_CUE_UNKNOWN rather than being misreported as cue out/in. */
int scte35_segmentation_cue_direction(int segmentation_type_id)
{
    if (segmentation_type_id == 0x22) {
        return SCTE35_CUE_OUT;
    }
    if (segmentation_type_id == 0x23) {
        return SCTE35_CUE_IN;
    }
    if (segmentation_type_id >= 0x30 && segmentation_type_id <= 0x47) {
        if ((segmentation_type_id & 1) == 0) {
            return SCTE35_CUE_OUT;
        }
        return SCTE35_CUE_IN;
    }
    return SCTE35_CUE_UNKNOWN;
}

/* Hands one decoded cue to the registered frame callback. The callback is
 * expected to copy what it needs; nothing here outlives the call. */
static void scte35_emit_cue(transport_data_struct *tsdata, scte35_data_struct *scte35_data)
{
    if (send_frame_func) {
        send_frame_func((uint8_t*)scte35_data, sizeof(scte35_data_struct), STREAM_TYPE_SCTE35, 1,
                        0, // pts
                        0, // dts
                        0, // PCR
                        tsdata->source,
                        0,
                        NULL,
                        0,  // cc errors
                        0,  // pmt table entries
                        send_frame_context);
    }
}

/* Parses splice_insert() starting at splice. Returns 1 when the whole command
 * was present within scte_end, 0 when it was truncated - in which case the
 * fields read before the truncation are still filled in. */
static int scte35_parse_splice_insert(const uint8_t *splice, const uint8_t *scte_end,
                                      scte35_data_struct *out)
{
    int program_splice_flag;
    int duration_flag;

    out->splice_command_type = SCTE35_CMD_SPLICE_INSERT;

    /* splice_event_id (4) + cancel_indicator byte (1) */
    if (splice + 5 > scte_end) {
        return 0;
    }
    out->splice_event_id = ((int64_t)splice[0] << 24) |
                           ((int64_t)splice[1] << 16) |
                           ((int64_t)splice[2] << 8) |
                            (int64_t)splice[3];
    out->cancel = !!(splice[4] & 0x80);   // bottom 7 bits reserved
    splice += 5;

    if (out->cancel) {
        /* A cancellation carries nothing past the event id. */
        return 1;
    }

    if (splice + 1 > scte_end) {
        return 0;
    }
    out->out_of_network_indicator = !!(splice[0] & 0x80);
    program_splice_flag           = !!(splice[0] & 0x40);
    duration_flag                 = !!(splice[0] & 0x20);
    out->splice_immediate         = !!(splice[0] & 0x10);
    // remaining 4 bits are reserved
    splice++;

    if (program_splice_flag == 1 && out->splice_immediate == 0) {
        // splice_time()
        int time_specified_flag;

        if (splice + 1 > scte_end) {
            return 0;
        }
        time_specified_flag = !!(splice[0] & 0x80);
        if (time_specified_flag) {
            if (splice + 5 > scte_end) {
                return 0;
            }
            out->pts_time = (((int64_t)(splice[0] & 0x01)) << 32) |
                             ((int64_t)splice[1] << 24) |
                             ((int64_t)splice[2] << 16) |
                             ((int64_t)splice[3] << 8) |
                              (int64_t)splice[4];
            splice += 5;
        } else {
            // next 7 bits are reserved
            splice++;
        }
    } else if (program_splice_flag == 0) {
        int component_count;
        int c;

        if (splice + 1 > scte_end) {
            return 0;
        }
        component_count = splice[0];
        splice++;
        for (c = 0; c < component_count; c++) {
            if (splice + 1 > scte_end) {
                return 0;
            }
            splice++;   // component_tag
            if (out->splice_immediate == 0) {
                int time_specified_flag;

                if (splice + 1 > scte_end) {
                    return 0;
                }
                time_specified_flag = !!(splice[0] & 0x80);
                if (time_specified_flag) {
                    if (splice + 5 > scte_end) {
                        return 0;
                    }
                    if (c == 0) {
                        /* report the first component's time as the splice point */
                        out->pts_time = (((int64_t)(splice[0] & 0x01)) << 32) |
                                         ((int64_t)splice[1] << 24) |
                                         ((int64_t)splice[2] << 16) |
                                         ((int64_t)splice[3] << 8) |
                                          (int64_t)splice[4];
                    }
                    splice += 5;
                } else {
                    // next 7 bits are reserved
                    splice++;
                }
            }
        }
    }

    if (duration_flag) {
        // break_duration()
        if (splice + 5 > scte_end) {
            return 0;
        }
        out->auto_return = !!(splice[0] & 0x80);
        out->pts_duration = (((int64_t)(splice[0] & 0x01)) << 32) |
                             ((int64_t)splice[1] << 24) |
                             ((int64_t)splice[2] << 16) |
                             ((int64_t)splice[3] << 8) |
                              (int64_t)splice[4];
        splice += 5;
    }

    if (splice + 2 > scte_end) {
        return 0;
    }
    out->program_id = ((int)splice[0] << 8) | (int)splice[1];
    /* avail_num and avails_expected follow but are not reported */

    return 1;
}

/* Parses one segmentation_descriptor body (everything after its tag and length
 * bytes) into cue. Returns 1 when the descriptor was complete, 0 otherwise. */
static int scte35_parse_segmentation_descriptor(const uint8_t *d, const uint8_t *d_end,
                                                scte35_data_struct *cue)
{
    int program_segmentation_flag;
    int segmentation_duration_flag;
    int upid_length;

    /* identifier - only CUEI-tagged descriptors are segmentation descriptors */
    if (d + 4 > d_end) {
        return 0;
    }
    if (d[0] != 'C' || d[1] != 'U' || d[2] != 'E' || d[3] != 'I') {
        return 0;
    }
    d += 4;

    if (d + 5 > d_end) {
        return 0;
    }
    cue->splice_event_id = ((int64_t)d[0] << 24) |
                           ((int64_t)d[1] << 16) |
                           ((int64_t)d[2] << 8) |
                            (int64_t)d[3];
    cue->cancel = !!(d[4] & 0x80);   // bottom 7 bits reserved
    d += 5;

    if (cue->cancel) {
        snprintf(cue->descriptor_name, MAX_SCTE35_NAME_SIZE, "%s", "Segmentation Event Canceled");
        return 1;
    }

    if (d + 1 > d_end) {
        return 0;
    }
    program_segmentation_flag  = !!(d[0] & 0x80);
    segmentation_duration_flag = !!(d[0] & 0x40);
    /* bit 0x20 is delivery_not_restricted_flag; the five bits below it are
     * either the delivery restriction flags or reserved, so either way the
     * whole group fits in this one byte. */
    d += 1;

    if (program_segmentation_flag == 0) {
        int component_count;

        if (d + 1 > d_end) {
            return 0;
        }
        component_count = d[0];
        d += 1;
        /* component_tag (1) + reserved and pts_offset (5) for each component */
        if (component_count > 0) {
            if ((d_end - d) < (6 * (long)component_count)) {
                return 0;
            }
            d += 6 * component_count;
        }
    }

    if (segmentation_duration_flag) {
        /* segmentation_duration is a full 40-bit 90kHz value */
        if (d + 5 > d_end) {
            return 0;
        }
        cue->pts_duration = ((int64_t)d[0] << 32) |
                            ((int64_t)d[1] << 24) |
                            ((int64_t)d[2] << 16) |
                            ((int64_t)d[3] << 8) |
                             (int64_t)d[4];
        d += 5;
    }

    if (d + 2 > d_end) {
        return 0;
    }
    cue->segmentation_upid_type = d[0];
    upid_length = d[1];
    d += 2;
    if (d + upid_length > d_end) {
        return 0;
    }
    d += upid_length;   /* the upid itself is not reported */

    if (d + 3 > d_end) {
        return 0;
    }
    cue->segmentation_type_id = d[0];
    cue->segment_num          = d[1];
    cue->segments_expected    = d[2];

    cue->cue_direction = scte35_segmentation_cue_direction(cue->segmentation_type_id);
    cue->out_of_network_indicator = (cue->cue_direction == SCTE35_CUE_OUT) ? 1 : 0;
    snprintf(cue->descriptor_name, MAX_SCTE35_NAME_SIZE, "%s",
             scte35_segmentation_type_name(cue->segmentation_type_id));

    return 1;
}

/* Parses time_signal() at cmd and walks the descriptor loop that follows it,
 * emitting one cue per segmentation_descriptor. Returns the number emitted. */
static int scte35_parse_time_signal(transport_data_struct *tsdata,
                                    const uint8_t *cmd, const uint8_t *scte_end,
                                    int splice_command_length,
                                    const scte35_data_struct *base)
{
    const uint8_t *p = cmd;
    const uint8_t *loop;
    const uint8_t *loop_end;
    int64_t pts_time = 0;
    int time_specified_flag;
    int descriptor_loop_length;
    int emitted = 0;

    /* time_signal() is a single splice_time(): one byte, or five with a PTS. */
    if (p + 1 > scte_end) {
        return 0;
    }
    time_specified_flag = !!(p[0] & 0x80);
    if (time_specified_flag) {
        if (p + 5 > scte_end) {
            return 0;
        }
        pts_time = (((int64_t)(p[0] & 0x01)) << 32) |
                    ((int64_t)p[1] << 24) |
                    ((int64_t)p[2] << 16) |
                    ((int64_t)p[3] << 8) |
                     (int64_t)p[4];
        p += 5;
    } else {
        // next 7 bits are reserved
        p += 1;
    }

    /* Trust the declared command length when it is a real value. 0x0fff means
     * the length is unknown, and then the position the parse above reached is
     * the only thing available. */
    if (splice_command_length != 0x0fff && (cmd + splice_command_length) <= scte_end) {
        p = cmd + splice_command_length;
    }

    if (p + 2 > scte_end) {
        return 0;
    }
    descriptor_loop_length = ((int)p[0] << 8) | (int)p[1];
    p += 2;

    loop_end = p + descriptor_loop_length;
    if (loop_end > scte_end) {
        loop_end = scte_end;     /* truncated loop - take what is present */
    }

    loop = p;
    while (loop + 2 <= loop_end) {
        int descriptor_tag = loop[0];
        int descriptor_length = loop[1];
        const uint8_t *d = loop + 2;
        const uint8_t *d_end = d + descriptor_length;

        if (d_end > loop_end) {
            break;              /* descriptor runs past the loop */
        }

        if (descriptor_tag == SCTE35_DESCRIPTOR_SEGMENTATION) {
            scte35_data_struct cue = *base;

            cue.splice_command_type = SCTE35_CMD_TIME_SIGNAL;
            cue.pts_time = pts_time;
            /* a time_signal with no splice_time applies right away */
            cue.splice_immediate = time_specified_flag ? 0 : 1;

            if (scte35_parse_segmentation_descriptor(d, d_end, &cue)) {
                cue.parse_complete = 1;
                scte35_emit_cue(tsdata, &cue);
                emitted++;
            }
        }

        loop = d_end;
    }

    return emitted;
}

/* Decodes one SCTE-35 splice_info_section from the start of a transport packet
 * payload. pdata[0] is the pointer_field, since this is only reached on a
 * packet with payload_unit_start_indicator set. */
static void scte35_decode_section(transport_data_struct *tsdata, const uint8_t *pdata,
                                  int payload_remaining, int current_pid)
{
    const uint8_t *scte_end;
    unsigned short section_size;
    unsigned char table_id;
    int splice_command_length;
    int splice_command_type;
    int64_t pts_adjustment;
    scte35_data_struct base;

    /* The fixed 15-byte splice_info header (pointer field through
     * splice_command_type) must be present before any of it is read. */
    if (payload_remaining < 15) {
        return;
    }

    table_id = pdata[1];
    if (table_id != 0xfc) {
        return;     /* not a splice_info_section */
    }

    section_size = ((pdata[2] << 8) + pdata[3]) & 0x0fff;
    pts_adjustment = (((int64_t)(pdata[5] & 0x01)) << 32) |
                      ((int64_t)pdata[6] << 24) |
                      ((int64_t)pdata[7] << 16) |
                      ((int64_t)pdata[8] << 8) |
                       (int64_t)pdata[9];
    splice_command_length = (((pdata[12] & 0x0f) << 8) + pdata[13]) & 0x0fff;
    splice_command_type = pdata[14];

    /* Parseable region ends at the smaller of the declared section length and
     * the bytes actually present in this packet payload. Every read below is
     * checked against scte_end. */
    scte_end = pdata + payload_remaining;
    if ((int)section_size + 4 < payload_remaining) {
        scte_end = pdata + 4 + (int)section_size;
    }

    memset(&base, 0, sizeof(base));
    base.pts_adjustment        = pts_adjustment;
    base.splice_pid            = current_pid;
    base.segmentation_type_id  = -1;
    base.segmentation_upid_type = -1;
    base.cue_direction         = SCTE35_CUE_UNKNOWN;

    if (splice_command_type == SCTE35_CMD_SPLICE_INSERT) {
        scte35_data_struct cue = base;

        cue.parse_complete = scte35_parse_splice_insert(pdata + 15, scte_end, &cue);
        if (cue.cancel) {
            cue.cue_direction = SCTE35_CUE_UNKNOWN;
            snprintf(cue.descriptor_name, MAX_SCTE35_NAME_SIZE, "%s", "Splice Event Canceled");
        } else {
            cue.cue_direction = cue.out_of_network_indicator ? SCTE35_CUE_OUT : SCTE35_CUE_IN;
            snprintf(cue.descriptor_name, MAX_SCTE35_NAME_SIZE, "%s",
                     cue.out_of_network_indicator ? "Splice Insert Out of Network"
                                                  : "Splice Insert Return to Network");
        }
        scte35_emit_cue(tsdata, &cue);
    } else if (splice_command_type == SCTE35_CMD_TIME_SIGNAL) {
        scte35_parse_time_signal(tsdata, pdata + 15, scte_end, splice_command_length, &base);
    }
}

int64_t get_time_difference(struct timeval *stoptime, struct timeval *starttime)
{
     int64_t delta_sec;
     int64_t delta_usec;
     int64_t temp_delta_sec;
     int64_t temp_delta_usec;
     int64_t final_time;

     delta_sec = stoptime->tv_sec - starttime->tv_sec;
     delta_usec = stoptime->tv_usec - starttime->tv_usec;
     if (delta_usec < 0) {
          temp_delta_sec = delta_sec - 1;
          temp_delta_usec = 1000000 + delta_usec;
     } else {
          temp_delta_sec = delta_sec;
          temp_delta_usec = delta_usec;
     }
     final_time = (temp_delta_sec * 1000000) + temp_delta_usec;

     return final_time;
}

static int decode_tvct_table(unsigned char *tvct_data, int tvct_data_size, int current_pid)
{
     return 0;
}

static int decode_pmt_table(pat_table_struct *master_pat_table, pmt_table_struct *master_pmt_table, unsigned char *pmt_data, int pmt_data_size, int current_pid)
{
     unsigned char *pdata = (unsigned char *)pmt_data;
     int pmt_program = (*(pdata+4) << 8) + *(pdata+5);
     int pmt_version = (*(pdata+6) & 0x1e) >> 1;
     int pmt_valid = *(pdata+6) & 0x01;
     int current_pmt_section = *(pdata+7);
     int previous_pmt_section = *(pdata+8);
     int pcr_pid = ((*(pdata+9) << 8) + *(pdata+10)) & 0x1fff;
     int program_info_length = ((*(pdata+11) << 8) + *(pdata+12)) & 0x0fff;
     int pmt_remaining = pmt_data_size;
     int descriptor_count = 0;
     int stream_count = 0;
     int pmt_count;
     int current_pmt_index = 0;
     int pmt_found = 0;
     pmt_table_struct *current_pmt_table = NULL;

     pthread_mutex_lock(&pmt_lock);
     for (pmt_count = 0; pmt_count < MAX_PMT_PIDS; pmt_count++) {
         if (master_pmt_table[pmt_count].pmt_pid == current_pid) {
             current_pmt_index = pmt_count;
             pmt_found = 1;
             break;
         }
         if (master_pmt_table[pmt_count].pmt_pid == 0) {
             break;
         }
     }
     if (!pmt_found) {
         /* The lookup loop exits with pmt_count == MAX_PMT_PIDS when the table
          * is full and nothing matched; using that index would write one past
          * the end of master_pmt_table[]. */
         if (pmt_count >= MAX_PMT_PIDS) {
             //backup_caller(2000, 505, 0, 0, 0, 0, backup_context);
             pthread_mutex_unlock(&pmt_lock);
             return -1;
         }
         current_pmt_index = pmt_count;
         master_pat_table->pmt_table_entries++;
     }

     current_pmt_table = (pmt_table_struct *)&master_pmt_table[current_pmt_index];

     current_pmt_table->pmt_pid = current_pid;
     current_pmt_table->pmt_data_size = pmt_data_size;
     current_pmt_table->pmt_program_number = pmt_program;
     current_pmt_table->pmt_valid = pmt_valid;
     current_pmt_table->current_pmt_section = current_pmt_section;
     current_pmt_table->previous_pmt_section = previous_pmt_section;
     current_pmt_table->pcr_pid = pcr_pid;
     current_pmt_table->program_info_length = program_info_length;
     current_pmt_table->pmt_version = pmt_version;
     current_pmt_table->audio_stream_count = 0;
     current_pmt_table->scte35_stream_count = 0;
     /* The local stream_count below indexes the per-PMT arrays from 0 on every
      * decode, so the published count has to restart with it. Without this it
      * accumulated across PMT version changes, leaving consumers to read stale
      * entries from the previous version (and eventually past the end of the
      * arrays once it passed MAX_STREAMS). */
     current_pmt_table->stream_count = 0;

     if (pmt_data_size <= MAX_TABLE_SIZE) {
         memcpy(current_pmt_table->pmt_data, pmt_data, pmt_data_size);
     }

     pdata += 13;
     pmt_remaining = pmt_data_size - 13;

     TSDECODE_DBG("decode_pmt_table: pmt_pid = %d, pmt_program =%d, pmt_data_size = %d\n",
             current_pid,
             pmt_program,
             pmt_data_size);

     if (program_info_length > pmt_remaining ||
         program_info_length < 0 ||
         program_info_length > MAX_TABLE_SIZE) {

         //syslog(LOG_ERR,"PMT TABLE ERROR: TABLE SIZE INVALID: %d\n", pmt_remaining);

         //backup_caller(2000, 503, 0, 0, 0, 0, backup_context);

         pthread_mutex_unlock(&pmt_lock);
         return -1;
     }

     while (program_info_length > 0) {
          int descriptor;
          int descriptor_size;

          descriptor = *(pdata+0);
          descriptor_size = *(pdata+1);

          if (descriptor_size < 0 ||
              descriptor_size > program_info_length) {
              //backup_caller(2000, 504, 0, 0, 0, 0, backup_context);

              pthread_mutex_unlock(&pmt_lock);
              return -1;
          }

          if (descriptor_count < TSHARDEN_MAX_DESCRIPTORS) {
              current_pmt_table->descriptor_id[descriptor_count] = descriptor;
              current_pmt_table->descriptor_size[descriptor_count] = descriptor_size;
              current_pmt_table->descriptor_count++;
              descriptor_count++;
          }

          if (descriptor == PMT_DESCRIPTOR_PRIVATE1) {
              //backup_caller(2000, 601, descriptor, current_pid, 0, 0, backup_context);
          } else if (descriptor == PMT_DESCRIPTOR_PRIVATE2) {
              //backup_caller(2000, 602, descriptor, current_pid, 0, 0, backup_context);
          } else if (descriptor == PMT_DESCRIPTOR_REGISTRATION) {
              //backup_caller(2000, 610, descriptor, current_pid, 0, 0, backup_context);
          } else if (descriptor == PMT_DESCRIPTOR_MAX_BITRATE) {
              int max_bitrate = ((int)pdata[3] << 8) + (int)pdata[4];
              //backup_caller(2000, 611, descriptor, max_bitrate, current_pid, 0, backup_context);
          } else if (descriptor == PMT_DESCRIPTOR_MUX_BUFFER) {
              //backup_caller(2000, 612, descriptor, current_pid, 0, 0, backup_context);
          } else {
              //backup_caller(2000, 600, descriptor, current_pid, 0, 0, backup_context);
          }

          pdata += (descriptor_size + 2);
          program_info_length -= (descriptor_size + 2);
          pmt_remaining -= (descriptor_size + 2);
     }

     if (pmt_remaining < 0 || program_info_length < 0) {
         //backup_caller(2000, 504, 0, 0, 0, 0, backup_context);
         pthread_mutex_unlock(&pmt_lock);
         return -1;
     }

     while (pmt_remaining >= 5) {
          int current_stream_type = *(pdata+0);
          int current_stream_pid = (int)(*(pdata+1) << 8) | (int)*(pdata+2);
          int pmt_info_length;
          int saved_position;
          unsigned char *saved_position_data;
          int saved_info_length;
          int local_count;
          int local_tag;
          int tag_index;
          int waiting_for_descriptor;

          /* Stop before overflowing the per-PMT stream arrays
           * (stream_pid[], stream_type[], data_engine[], ...). */
          if (stream_count >= TSHARDEN_MAX_STREAMS) {
              break;
          }

          current_stream_pid = current_stream_pid & 0x1fff;
          pmt_info_length = (((int)*(pdata+3) << 8) | (int)*(pdata+4)) & 0x0fff;

          /* The stream entry occupies 5 header bytes + pmt_info_length of
           * descriptors; if that runs past the remaining table bytes the
           * stream is malformed - stop rather than read/parse out of bounds. */
          if (pmt_info_length > pmt_remaining - 5) {
              break;
          }

          current_pmt_table->stream_pid[stream_count] = current_stream_pid;
          current_pmt_table->stream_type[stream_count] = current_stream_type;
          current_pmt_table->audio_stream_index[stream_count] = -1;
          current_pmt_table->first_pts[stream_count] = -1;
          current_pmt_table->first_dts[stream_count] = -1;
          current_pmt_table->last_pts[stream_count] = -1;
          current_pmt_table->last_dts[stream_count] = -1;
          waiting_for_descriptor = 0;

          current_pmt_table->decoded_stream_type[stream_count] = STREAM_TYPE_UNKNOWN_AUDIO; // default to unknown

          TSDECODE_DBG("decode_pmt_table: current_stream_type = 0x%x\n", current_stream_type);
          if (current_stream_type == 0x02) {
              //backup_caller(2000, 800, current_stream_pid, current_pid, 0, 0, backup_context);
              current_pmt_table->decoded_stream_type[stream_count] = STREAM_TYPE_MPEG2;
          } else if (current_stream_type == 0x1b) {
              //backup_caller(2000, 801, current_stream_pid, current_pid, 0, 0, backup_context);
              current_pmt_table->decoded_stream_type[stream_count] = STREAM_TYPE_H264;
          } else if (current_stream_type == 0x24) {
              //backup_caller(2000, 814, current_stream_pid, current_pid, 0, 0, backup_context);
              current_pmt_table->decoded_stream_type[stream_count] = STREAM_TYPE_HEVC;
          } else if (current_stream_type == 0x01) {
              //backup_caller(2000, 802, current_stream_pid, current_pid, 0, 0, backup_context);
              current_pmt_table->decoded_stream_type[stream_count] = STREAM_TYPE_MPEG;
              current_pmt_table->audio_stream_index[stream_count] = current_pmt_table->audio_stream_count;
              current_pmt_table->audio_stream_count++;
          } else if (current_stream_type == 0x03) {
              //backup_caller(2000, 803, current_stream_pid, current_pid, 0, 0, backup_context);
              current_pmt_table->decoded_stream_type[stream_count] = STREAM_TYPE_MPEG;
              current_pmt_table->audio_stream_index[stream_count] = current_pmt_table->audio_stream_count;
              current_pmt_table->audio_stream_count++;
          } else if (current_stream_type == 0x04) {
              //backup_caller(2000, 804, current_stream_pid, current_pid, 0, 0, backup_context);
              current_pmt_table->decoded_stream_type[stream_count] = STREAM_TYPE_MPEG;
              current_pmt_table->audio_stream_index[stream_count] = current_pmt_table->audio_stream_count;
              current_pmt_table->audio_stream_count++;
          } else if (current_stream_type == 0x0f) {
              //backup_caller(2000, 805, current_stream_pid, current_pid, 0, 0, backup_context);
              current_pmt_table->decoded_stream_type[stream_count] = STREAM_TYPE_AAC;
              current_pmt_table->audio_stream_index[stream_count] = current_pmt_table->audio_stream_count;
              current_pmt_table->audio_stream_count++;
          } else if (current_stream_type == 0x81) {
              //backup_caller(2000, 806, current_stream_pid, current_pid, 0, 0, backup_context);
              current_pmt_table->decoded_stream_type[stream_count] = STREAM_TYPE_AC3;
              current_pmt_table->audio_stream_index[stream_count] = current_pmt_table->audio_stream_count;
              current_pmt_table->audio_stream_count++;
          } else if (current_stream_type == 0x27) {
              //backup_caller(2000, 807, current_stream_pid, current_pid, 0, 0, backup_context);
          } else if (current_stream_type == 0x06) {
              waiting_for_descriptor = 0x06;
          } else if (current_stream_type == 0x05) {
              //backup_caller(2000, 809, current_stream_pid, current_pid, 0, 0, backup_context);
          } else if (current_stream_type == 0x0b) {
              //backup_caller(2000, 810, current_stream_pid, current_pid, 0, 0, backup_context);
          } else if (current_stream_type == 0x82) {
              //backup_caller(2000, 811, current_stream_pid, current_pid, 0, 0, backup_context);
          } else if (current_stream_type == 0x86) { // scte35
              //backup_caller(2000, 812, current_stream_pid, current_pid, 0, 0, backup_context);
              current_pmt_table->decoded_stream_type[stream_count] = STREAM_TYPE_SCTE35;
              current_pmt_table->scte35_stream_count++;
          } else if (current_stream_type == 0xC0) {
              //backup_caller(2000, 813, current_stream_pid, current_pid, 0, 0, backup_context);
          }

          current_pmt_table->stream_count++;
          stream_count++;

          saved_position = pmt_remaining;
          saved_position_data = pdata;
          saved_info_length = pmt_info_length;

          pmt_remaining -= 5;
          pdata += 5;

_redo_decode:
          local_count = pmt_info_length;
          tag_index = 0;
          while (local_count > 0) {
               int h;
               int local_tag_size;

               /* Need the 2-byte tag header within the descriptor region. */
               if (tag_index + 2 > pmt_info_length) {
                   break;
               }
               local_tag = *(pdata+tag_index);
               local_tag_size = *(pdata+tag_index+1);
               tag_index += 2;
               local_count -= local_tag_size;
               local_count -= 2;

               /* The tag payload must also stay inside the descriptor region. */
               if (local_tag_size < 0 || tag_index + local_tag_size > pmt_info_length) {
                   break;
               }

               TSDECODE_DBG("PMT TABLE LOCAL TAG: 0x%x  WAITING:%d\n", local_tag, waiting_for_descriptor);

               if (local_tag == STREAM_DESCRIPTOR_IDENTIFIER) {
                    for (h = 0; h < local_tag_size; h++) {
                         // do something with the data here
                         tag_index++;
                    }
               } else if (local_tag == STREAM_DESCRIPTOR_VIDEO) {
                    for (h = 0; h < local_tag_size; h++) {
                         // do something with the data here
                         tag_index++;
                    }
               } else if (local_tag == STREAM_DESCRIPTOR_ALIGNMENT) {
                    int alignment_type;
                    for (h = 0; h < local_tag_size; h++) {
                         // do something with the data here
                         tag_index++;
                    }
                    if (local_tag_size == 1) {
                         alignment_type = *(pdata+3);
                    } else {
                         alignment_type = 0;
                    }
                    //backup_caller(2000, 709, local_tag, alignment_type, 0, 0, backup_context);
               } else if (local_tag == STREAM_DESCRIPTOR_MAX_BITRATE) {
                    for (h = 0; h < local_tag_size; h++) {
                         // do something with the data here
                         tag_index++;
                    }
               } else if (local_tag == STREAM_DESCRIPTOR_AVC) {
                    for (h = 0; h < local_tag_size; h++) {
                         // do something with the data here
                         tag_index++;
                    }
                    //backup_caller(2000, 711, local_tag, 0, 0, 0, backup_context);
               } else if (local_tag == STREAM_DESCRIPTOR_SUBTITLE1 ||
                          local_tag == STREAM_DESCRIPTOR_SUBTITLE2) {
                    if (waiting_for_descriptor) {
                        //backup_caller(2000, 808, current_stream_pid, current_pid, 0, 0, backup_context);
                        waiting_for_descriptor = 0;
                        goto _redo_decode;
                    } else {
                        //backup_caller(2000, 712, local_tag, 0, 0, 0, backup_context);
                    }
                    tag_index += local_tag_size;
               } else if (local_tag == STREAM_DESCRIPTOR_EAC3) {
                    if (waiting_for_descriptor) {
                        //backup_caller(2000, 814, current_stream_pid, current_pid, 0, 0, backup_context);
                        waiting_for_descriptor = 0;
                        goto _redo_decode;
                    } else {
                        //backup_caller(2000, 713, local_tag, 0, 0, 0, backup_context);
                    }
                    tag_index += local_tag_size;
               } else if (local_tag == STREAM_DESCRIPTOR_STD) {
                   //backup_caller(2000, 714, local_tag, 0, 0, 0, backup_context);
                   tag_index += local_tag_size;
               } else if (local_tag == STREAM_DESCRIPTOR_SMOOTH) {
                   //backup_caller(2000, 715, local_tag, 0, 0, 0, backup_context);
                   tag_index += local_tag_size;
               } else if (local_tag == STREAM_DESCRIPTOR_CAPTION) {
                   //backup_caller(2000, 716, local_tag, 0, 0, 0, backup_context);
                   tag_index += local_tag_size;
               } else if (local_tag == STREAM_DESCRIPTOR_REGISTRATION) {
                   //backup_caller(2000, 717, local_tag, 0, 0, 0, backup_context);
                   if (waiting_for_descriptor) {
                       waiting_for_descriptor = 0;
                       current_pmt_table->decoded_stream_type[stream_count - 1] = STREAM_TYPE_UNKNOWN_AUDIO;  // DOLBY?
                       current_pmt_table->stream_type[stream_count - 1] = 0x06;
                       current_pmt_table->audio_stream_index[stream_count - 1] = current_pmt_table->audio_stream_count;
                       current_pmt_table->audio_stream_count++;
                       goto _redo_decode;
                   }

                   tag_index += local_tag_size;
               } else if (local_tag == STREAM_DESCRIPTOR_AC3) {
                   if (waiting_for_descriptor) {
                       waiting_for_descriptor = 0;
                       current_pmt_table->decoded_stream_type[stream_count - 1] = STREAM_TYPE_AC3;
                       current_pmt_table->stream_type[stream_count - 1] = 0x81;
                       current_pmt_table->audio_stream_index[stream_count - 1] = current_pmt_table->audio_stream_count;
                       current_pmt_table->audio_stream_count++;
                       goto _redo_decode;
                   }
                   //backup_caller(2000, 718, local_tag, 0, 0, 0, backup_context);
                   tag_index += local_tag_size;
               } else if (local_tag == STREAM_DESCRIPTOR_LANGUAGE) {
                   uint8_t l1 = *(pdata+tag_index+0);
                   uint8_t l2 = *(pdata+tag_index+1);
                   uint8_t l3 = *(pdata+tag_index+2);

                   current_pmt_table->decoded_language_tag[stream_count - 1].lang_tag[0] = (char)l1;
                   current_pmt_table->decoded_language_tag[stream_count - 1].lang_tag[1] = (char)l2;
                   current_pmt_table->decoded_language_tag[stream_count - 1].lang_tag[2] = (char)l3;
                   current_pmt_table->decoded_language_tag[stream_count - 1].lang_tag[3] = '\0';

                   //backup_caller(2000, 719, local_tag, l1, l2, l3, backup_context);
                   tag_index += local_tag_size;
               } else if (local_tag == STREAM_DESCRIPTOR_APPLICATION) {
                   //backup_caller(2000, 720, local_tag, 0, 0, 0, backup_context);
                   tag_index += local_tag_size;
               } else if (local_tag == STREAM_DESCRIPTOR_MPEGAUDIO) {
                   //backup_caller(2000, 721, local_tag, 0, 0, 0, backup_context);
                   tag_index += local_tag_size;
               } else if (local_tag == STREAM_DESCRIPTOR_AC3_2) {
                   if (waiting_for_descriptor) {
                       TSDECODE_DBG("status: setting decoded stream type to AC3 (stream_count:%d)\n",
                               stream_count);
                       waiting_for_descriptor = 0;
                       current_pmt_table->decoded_stream_type[stream_count - 1] = STREAM_TYPE_AC3;
                       current_pmt_table->stream_type[stream_count - 1] = 0x81;
                       current_pmt_table->audio_stream_index[stream_count - 1] = current_pmt_table->audio_stream_count;
                       current_pmt_table->audio_stream_count++;
                       goto _redo_decode;
                   }
                   //backup_caller(2000, 722, local_tag, 0, 0, 0, backup_context);
                   tag_index += local_tag_size;
               } else {
                   // the catch-all
                   tag_index += local_tag_size;
                   //backup_caller(2000, 799, local_tag, 0, 0, 0, backup_context);
               }
          }

          pmt_remaining = saved_position;
          pdata = saved_position_data;
          pmt_info_length = saved_info_length;

          pmt_remaining -= (pmt_info_length + 5);
          pdata += (pmt_info_length + 5);
     }
     pthread_mutex_unlock(&pmt_lock);
     return 0;
}

int decode_packets(uint8_t *transport_packet_data, int packet_count, transport_data_struct *tsdata, int stream_select)
{
     int packet_num;
     int each_pmt;

     for (packet_num = 0; packet_num < packet_count; packet_num++) {
          unsigned char *pdata = (unsigned char *)transport_packet_data + (packet_num * 188);
          unsigned char *pdata_initial = pdata + 4;

          if (*(pdata+0) == 0x47) {
               int pusi = (*(pdata+1) >> 6) & 0x1;
               int current_pid = (((int)*(pdata+1) << 8) + (int)*(pdata+2)) & 0x1fff;
               int afc = (*(pdata+3) >> 4) & 0x03;
               int cc = *(pdata+3) & 0x0F;
               int adaptation_size = 0;
               int pcr_flag = 0;

               int discontinuity_flag;
               int random_access_point = 0;
               int pid_counter;
               int64_t current_ext;
               int64_t current_pcr;
               int64_t received_pcr;
               int64_t offset_pcr;
               int pid_in_list = 0;
               int new_pid = -1;
               int64_t update_pid_time = 0;

               pdata += 4;
               tsdata->received_ts_packets++;
               pdata_initial = pdata;

               if (total_input_packets == 0) {
                   gettimeofday(&tsdata->pid_start_time, NULL);
               }
               gettimeofday(&tsdata->pid_stop_time, NULL);
               update_pid_time = (int64_t)get_time_difference(&tsdata->pid_stop_time, &tsdata->pid_start_time);

               for (pid_counter = 0; pid_counter < MAX_PIDS; pid_counter++) {
                   if (tsdata->master_packet_table[pid_counter].pid == current_pid &&
                       tsdata->master_packet_table[pid_counter].valid) {
                       tsdata->master_packet_table[pid_counter].input_packets++;
                       gettimeofday(&tsdata->master_packet_table[pid_counter].last_seen, NULL);
                       pid_in_list = 1;
                       break;
                   }
                   if (!tsdata->master_packet_table[pid_counter].valid) {

                       // SEND MESSAGE INDICATING A NEW PID WAS FOUND
                       // backup_caller();

                       new_pid = pid_counter;
                       break;
                   }
               }
               if (!pid_in_list && new_pid >= 0) {
                   tsdata->master_packet_table[new_pid].valid = 1;
                   tsdata->master_packet_table[new_pid].input_packets = 1;
                   tsdata->master_packet_table[new_pid].pid = current_pid;
                   gettimeofday(&tsdata->master_packet_table[new_pid].last_seen, NULL);
               }

               total_input_packets++;

               if (afc & 2) {
                    if (afc == 2) {
                         adaptation_size = 183;
                    } else {
                         adaptation_size = *(pdata+0);
                    }
                    if (adaptation_size > 0) {
                         discontinuity_flag = !!(*(pdata+1) & 0x80);
                         if (discontinuity_flag) {
                             //backup_caller(2000, 502, 0, 0, 0, 0, backup_context);
                         }
                         random_access_point = !!(*(pdata+1) & 0x40);
                         pcr_flag = !!(*(pdata+1) & 0x10);
                         if (pcr_flag) {
                              current_pcr = *(pdata+2);
                              current_pcr = (current_pcr << 8) | *(pdata+3);
                              current_pcr = (current_pcr << 8) | *(pdata+4);
                              current_pcr = (current_pcr << 8) | *(pdata+5);
                              current_pcr = current_pcr << 1;
                              if ((*(pdata+6) & 0x80) != 0) {
                                   current_pcr |= 1;
                              }
                              current_ext = (*(pdata+6) & 0x1) << 8;
                              current_ext = current_ext | *(pdata+7);

                              received_pcr = (current_pcr * 300) + current_ext;

                              if (tsdata->initial_pcr_base[current_pid] == -1) {
                                  tsdata->initial_pcr_base[current_pid] = received_pcr;
                                  tsdata->initial_pcr_ext = 0;
                                  /* Align the packet counter with the PCR window: the
                                   * mux-rate numerator must count only packets sent
                                   * after this baseline PCR, not since the start of
                                   * the stream. received_ts_packets is not read
                                   * anywhere else, so it is safe to repurpose as the
                                   * per-window count. (Assumes one PCR PID per
                                   * transport stream - the common single-program
                                   * case; see the multi-program note below.) */
                                  tsdata->received_ts_packets = 0;
                                  gettimeofday(&tsdata->pcr_start_time, NULL);
                                  gettimeofday(&tsdata->pcr_update_start_time, NULL);
                              } else {
                                  int64_t pcr_update_delta_time;
                                  int check_mux_rate = 0;

                                  gettimeofday(&tsdata->pcr_stop_time, NULL);
                                  offset_pcr = received_pcr - tsdata->initial_pcr_base[current_pid];
                                  pcr_update_delta_time = (int64_t)get_time_difference(&tsdata->pcr_stop_time, &tsdata->pcr_update_start_time);

                                  if (discontinuity_flag || offset_pcr <= 0) {
                                      /* PCR discontinuity, a 33-bit base wrap
                                       * (~26.5h), or a duplicate/zero interval:
                                       * restart the measurement window rather than
                                       * dividing by zero or by a negative interval. */
                                      tsdata->initial_pcr_base[current_pid] = received_pcr;
                                      tsdata->received_ts_packets = 0;
                                      gettimeofday(&tsdata->pcr_start_time, NULL);
                                      gettimeofday(&tsdata->pcr_update_start_time, NULL);
                                  } else if (offset_pcr >= 27000) {  /* >= ~1ms at 27MHz: ignore sub-ms windows */
                                      /* mux rate (bits/sec) = transmitted_bits / elapsed_seconds
                                       *   transmitted_bits = received_ts_packets * 188 * 8
                                       *   elapsed_seconds  = offset_pcr / 27000000  (PCR is a 27 MHz clock)
                                       * The old code added a spurious +10 bytes and had
                                       * no divide-by-zero or range guard. */
                                      double mux_rate = (27000000.0 * (double)tsdata->received_ts_packets * 188.0 * 8.0) / (double)offset_pcr;

                                      /* (int) of an out-of-range double is undefined
                                       * behaviour, so range-check before the cast. */
                                      if (mux_rate >= 0.0 && mux_rate < 2147483647.0) {
                                          check_mux_rate = (int)mux_rate;
                                      }

                                      /* NOTE: check_mux_rate is currently consumed only
                                       * by the commented-out backup_caller; route it to a
                                       * struct field or callback to actually use it. */
                                      //backup_caller(2000, 400, current_pid, check_mux_rate, 0, 0, backup_context);
                                      (void)check_mux_rate;
                                  }

                                  if (pcr_update_delta_time > 1000000) {
                                      gettimeofday(&tsdata->pcr_update_start_time, NULL);
                                  }
                              }
                         }
                         pdata += adaptation_size;
                         pdata++;
                         adaptation_size++;
                    } else {
                         pdata++;
                         adaptation_size = 1;
                    }
               }

               if (afc & 1) {
                   if (pusi) {
                       int pid_count = 0;
                       int scte35_pid = get_scte35_pid(tsdata);

                       if (scte35_pid != 0 && current_pid == scte35_pid) {
                           scte35_decode_section(tsdata, pdata,
                                                 TS_PAYLOAD_SIZE - (int)(pdata - pdata_initial),
                                                 current_pid);
                       }

                       for (pid_count = 0; pid_count < tsdata->pmt_pid_count; pid_count++) {
                           if (tsdata->pmt_pid_index[pid_count] == current_pid) {
                               int acquired_data_so_far = pdata - pdata_initial;
                               int unit_size = pdata[0];
                               int pmt_payload_remaining = TS_PAYLOAD_SIZE - acquired_data_so_far;
                               unsigned short section_size;
                               int pmt_version_input;
                               int table_id;

                               /* The pointer field plus the 7 header bytes we read
                                * below (offsets 0..6 after the jump) must stay in
                                * the packet payload. */
                               if (unit_size < 0 || pmt_payload_remaining < 0 ||
                                   (unit_size + 7) > pmt_payload_remaining) {
                                   goto continue_packet_processing;
                               }
                               pdata += unit_size;
                               table_id = pdata[1];

                               section_size = ((*(pdata+2) << 8) + *(pdata+3)) & 0x0fff;
                               pmt_version_input = (*(pdata+6) & 0x1e) >> 1;

                               if (table_id != 0x02) {
                                   goto continue_packet_processing;
                               }

                               for (each_pmt = 0; each_pmt < tsdata->master_pat_table.pmt_table_entries; each_pmt++)  {
                                   if (each_pmt == stream_select && stream_select != -1) {
                                       if (tsdata->master_pmt_table[each_pmt].pmt_pid == current_pid) {
                                           if (tsdata->master_pmt_table[each_pmt].max_pmt_time == 0) {
                                               tsdata->master_pmt_table[each_pmt].min_pmt_time = 999999999;
                                               gettimeofday(&tsdata->master_pmt_table[each_pmt].start_pmt_time, NULL);
                                               tsdata->master_pmt_table[each_pmt].max_pmt_time = 1;
                                           } else {
                                               int64_t delta_pmt_time;
                                               gettimeofday(&tsdata->master_pmt_table[each_pmt].end_pmt_time, NULL);
                                               delta_pmt_time = (int64_t)get_time_difference(&tsdata->master_pmt_table[each_pmt].end_pmt_time,
                                                                                             &tsdata->master_pmt_table[each_pmt].start_pmt_time);

                                               if (delta_pmt_time > tsdata->master_pmt_table[each_pmt].max_pmt_time) {
                                                   tsdata->master_pmt_table[each_pmt].max_pmt_time = delta_pmt_time;
                                                   // SIGNAL NEW MAX PMT TIME TO GUI
                                                   // backup_caller(2000, 505, delta_pmt_time, current_pid, 0, backup_context);
                                               }
                                               if (delta_pmt_time < tsdata->master_pmt_table[each_pmt].min_pmt_time) {
                                                   tsdata->master_pmt_table[each_pmt].min_pmt_time = delta_pmt_time;
                                                   // SIGNAL NEW MIN PMT TIME TO GUI
                                                   // backup_caller(2000, 506, delta_pmt_time, current_pid, 0, backup_context);
                                               }
                                               //backup_caller(2000, 505, delta_pmt_time / 1000, current_pid, 0, backup_context);
                                               tsdata->master_pmt_table[each_pmt].avg_pmt_time += delta_pmt_time;
                                               tsdata->master_pmt_table[each_pmt].avg_pmt_time /= 2;
                                               gettimeofday(&tsdata->master_pmt_table[each_pmt].start_pmt_time, NULL);
                                           }
                                       }
                                   }
                               }

                               if (pmt_version_input != tsdata->pmt_version[pid_count] ||
                                   tsdata->pmt_decoded[pid_count] == 0 ||
                                   tsdata->pmt_version[pid_count] == -1) {
                                   tsdata->pmt_table_acquired = 184 - acquired_data_so_far;
                                   tsdata->pmt_table_expected = section_size;
                                   /* The copy below reads from pdata (already
                                    * advanced past the pointer field), so the copy
                                    * length must not exceed the bytes remaining
                                    * after that field or it reads past the packet. */
                                   {
                                       int pmt_src_avail = pmt_payload_remaining - unit_size;
                                       if (pmt_src_avail < 0) {
                                           pmt_src_avail = 0;
                                       }
                                       if (tsdata->pmt_table_acquired > pmt_src_avail) {
                                           tsdata->pmt_table_acquired = pmt_src_avail;
                                       }
                                   }
                                   if (tsdata->pmt_position == 0) {
                                       tsdata->pmt_position = total_input_packets;
                                   }
                                   if ((section_size+4) > tsdata->pmt_table_acquired) {
                                       if (tsdata->pmt_table_acquired < 0 ||
                                           tsdata->pmt_table_acquired > MAX_TABLE_SIZE ||
                                           tsdata->pmt_table_expected > MAX_TABLE_SIZE ||
                                           tsdata->pmt_table_expected < 0) {
                                           tsdata->pmt_table_acquired = 0;
                                           tsdata->pmt_table_expected = 0;
                                       } else {
                                           memcpy(tsdata->pmt_data, pdata, tsdata->pmt_table_acquired);
                                       }
                                   } else {
                                       if (tsdata->pmt_table_acquired < 0 ||
                                           tsdata->pmt_table_acquired > MAX_TABLE_SIZE ||
                                           tsdata->pmt_table_expected > MAX_TABLE_SIZE ||
                                           tsdata->pmt_table_expected < 0) {
                                           tsdata->pmt_table_acquired = 0;
                                           tsdata->pmt_table_expected = 0;
                                       } else {
                                           unsigned short crc_position;
                                           unsigned long crc32_length;
                                           uint32_t calculated_crc;

                                           memcpy(tsdata->pmt_data, pdata, tsdata->pmt_table_acquired);
                                           tsdata->pmt_data_size = tsdata->pmt_table_expected;
                                           crc_position = ((int)(tsdata->pmt_data[2] << 8) + (int)tsdata->pmt_data[3]) & 0x0fff;
                                           crc32_length = crc_position - 1;
                                           if (crc_position > 4 && (int)crc_position + 4 <= MAX_TABLE_SIZE) {
                                               uint32_t pmt_crc2;

                                               calculated_crc = getcrc32(&tsdata->pmt_data[1], crc32_length);
                                               calculated_crc ^= 0xffffffff;
                                               calculated_crc = htonl(calculated_crc);

                                               pmt_crc2 = tsharden_read_u32((const uint8_t*)&tsdata->pmt_data[crc_position]);
                                               pmt_crc2 ^= 0xffffffff;

                                               if (pmt_crc2 == calculated_crc) {
                                                   tsdata->pmt_version[pid_count] = pmt_version_input;
                                                   decode_pmt_table(&tsdata->master_pat_table, tsdata->master_pmt_table, tsdata->pmt_data, tsdata->pmt_data_size, current_pid);
                                                   tsdata->pmt_decoded[pid_count] = 1;
                                               } else {
                                                   //backup_caller(2000, 201, calculated_crc, 0, 0, 0, backup_context);
                                               }
                                           }
                                           tsdata->pmt_table_acquired = 0;
                                           tsdata->pmt_table_expected = 0;
                                       }
                                   }
                               }
                           }
                       }

                       for (each_pmt = 0; each_pmt < tsdata->master_pat_table.pmt_table_entries; each_pmt++)  {
                           if (each_pmt == stream_select && stream_select != -1) {
                               int stream_count = tsdata->master_pmt_table[each_pmt].stream_count;
                               for (pid_count = 0; pid_count < stream_count; pid_count++) {
                                   if (tsdata->master_pmt_table[each_pmt].stream_pid[pid_count] == current_pid) {
                                       int last_cc;
                                       int pes_length;
                                       int check0 = *(pdata+6);
                                       int check1 = *(pdata+7);

                                       if (tsdata->master_pmt_table[each_pmt].data_engine[pid_count].data_index > 0) {
                                           unsigned char *video_frame;
                                           int is_intra = 0;
                                           int core_modified = 0;
                                           int video_frame_size = tsdata->master_pmt_table[each_pmt].data_engine[pid_count].data_index;
                                           int video_bitrate = 0;
                                           int video_framerate = 0;
                                           int stream_type = 0;
                                           int aspect_ratio = 0;
                                           int seqtype = 0;

                                           stream_type = tsdata->master_pmt_table[each_pmt].stream_type[pid_count];

                                           if (stream_type == 0x02 || stream_type == 0x80) {
                                               int64_t delta_data_time;

                                               video_frame = (unsigned char*)tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer;
                                               if (video_frame[0] == 0x00 && video_frame[1] == 0x00 &&
                                                   video_frame[2] == 0x01 && video_frame[3] == 0xb3) {
                                                   is_intra = 1;
                                               }
                                               if (tsdata->master_pmt_table[each_pmt].data_engine[pid_count].video_frame_count == 0) {
                                                   gettimeofday(&tsdata->master_pmt_table[each_pmt].data_engine[pid_count].start_data_time, NULL);
                                               }
                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].video_frame_count++;

                                               gettimeofday(&tsdata->master_pmt_table[each_pmt].data_engine[pid_count].end_data_time, NULL);
                                               delta_data_time = (int64_t)get_time_difference(&tsdata->master_pmt_table[each_pmt].data_engine[pid_count].end_data_time,
                                                                                              &tsdata->master_pmt_table[each_pmt].data_engine[pid_count].start_data_time);

                                               if (delta_data_time > 30000000) {
                                                   float measured_fps = (tsdata->master_pmt_table[each_pmt].data_engine[pid_count].video_frame_count * 1000000.0);
                                                   measured_fps = measured_fps / delta_data_time * 1000.0;
                                                   gettimeofday(&tsdata->master_pmt_table[each_pmt].data_engine[pid_count].start_data_time, NULL);
                                                   tsdata->master_pmt_table[each_pmt].data_engine[pid_count].video_frame_count = 0;
                                                   //backup_caller(2000, 1004, (long long)measured_fps, current_pid, 0, 0, backup_context);
                                               }

                                               if (core_modified & 32) {
                                                   //backup_caller(2000, 1000, video_framerate, current_pid, 0, 0, backup_context);
                                                   //backup_caller(2000, 1001, video_bitrate, current_pid, 0, 0, backup_context);
                                               }
                                               if (core_modified & 8) {
                                                   /*backup_caller(2000, 1002,
                                                                 tsdata->master_pmt_table[each_pmt].data_engine[pid_count].width,
                                                                 tsdata->master_pmt_table[each_pmt].data_engine[pid_count].height,
                                                                 current_pid, 0, backup_context);
                                                   */
                                               }
                                               if (core_modified & 2) {
                                                   /*backup_caller(2000, 1003,
                                                                 aspect_ratio,
                                                                 seqtype,
                                                                 current_pid,
                                                                 0,
                                                                 backup_context);
                                                   */
                                               }
                                               if (send_frame_func)
                                               send_frame_func(video_frame, video_frame_size, STREAM_TYPE_MPEG2, is_intra,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts,
                                                               0, // PCR
                                                               tsdata->source,
                                                               0, // sub-source is 0 for video
                                                               (char*)&tsdata->master_pmt_table[each_pmt].decoded_language_tag[pid_count].lang_tag[0],
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].corruption_count,
                                                               tsdata->master_pat_table.pmt_table_entries,
                                                               send_frame_context);
                                           } else if (stream_type == 0x0f) {
                                               uint8_t *audio_frame = (unsigned char*)tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer;
                                               if (send_frame_func)
                                               send_frame_func(audio_frame, video_frame_size, STREAM_TYPE_AAC, 1,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts,
                                                               0, // PCR
                                                               tsdata->source,
                                                               tsdata->master_pmt_table[each_pmt].audio_stream_index[pid_count],  //sub-source
                                                               (char*)&tsdata->master_pmt_table[each_pmt].decoded_language_tag[pid_count].lang_tag[0],
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].corruption_count,
                                                               tsdata->master_pat_table.pmt_table_entries,
                                                               send_frame_context);
                                           } else if (stream_type == 0x81) {
                                               uint8_t *audio_frame = (unsigned char*)tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer;
                                               if (send_frame_func)
                                               send_frame_func(audio_frame, video_frame_size, STREAM_TYPE_AC3, 1,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts,
                                                               0, // PCR
                                                               tsdata->source,
                                                               tsdata->master_pmt_table[each_pmt].audio_stream_index[pid_count], //sub-source
                                                               (char*)&tsdata->master_pmt_table[each_pmt].decoded_language_tag[pid_count].lang_tag[0],
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].corruption_count,
                                                               tsdata->master_pat_table.pmt_table_entries,
                                                               send_frame_context);
                                           } else if (stream_type == 0x06) {
                                               uint8_t *audio_frame = (unsigned char*)tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer;
                                               if (send_frame_func)
                                               send_frame_func(audio_frame, video_frame_size, STREAM_TYPE_UNKNOWN_AUDIO, 1,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts,
                                                               0, // PCR
                                                               tsdata->source,
                                                               tsdata->master_pmt_table[each_pmt].audio_stream_index[pid_count], //sub-source
                                                               (char*)&tsdata->master_pmt_table[each_pmt].decoded_language_tag[pid_count].lang_tag[0],
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].corruption_count,
                                                               tsdata->master_pat_table.pmt_table_entries,
                                                               send_frame_context);
                                           } else if (stream_type == 0x04 || stream_type == 0x03 || stream_type == 0x01) {
                                               uint8_t *audio_frame = (unsigned char*)tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer;
                                               if (send_frame_func)
                                               send_frame_func(audio_frame, video_frame_size, STREAM_TYPE_MPEG, 1,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts,
                                                               0, // PCR
                                                               tsdata->source,
                                                               tsdata->master_pmt_table[each_pmt].audio_stream_index[pid_count], //sub-source
                                                               (char*)&tsdata->master_pmt_table[each_pmt].decoded_language_tag[pid_count].lang_tag[0],
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].corruption_count,
                                                               tsdata->master_pat_table.pmt_table_entries,
                                                               send_frame_context);
                                           } else if (stream_type == 0x86) {
                                               // do nothing, scte35 handled elsewhere
                                           } else if (stream_type == 0x24) {
                                               int vf;
                                               int nal_type;
                                               int is_intra = 0;
                                               video_frame = (unsigned char*)tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer;
                                               for (vf = 0; vf < video_frame_size - 4; vf++) {
                                                   if (video_frame[vf] == 0x00 &&
                                                       video_frame[vf+1] == 0x00 &&
                                                       video_frame[vf+2] == 0x01) {
                                                       nal_type = (video_frame[vf+3] & 0x7f) >> 1;
                                                       if (nal_type == 20 || nal_type == 19) {
                                                           is_intra = 1;
                                                           if (tsdata->master_pmt_table[each_pmt].data_engine[pid_count].video_frame_count == 0) {
                                                               tsdata->first_frame_intra = 1;
                                                               is_intra = 1;
                                                           }
                                                           break;
                                                       }
                                                   }
                                               }

                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].video_frame_count++;

                                               if (send_frame_func)
                                               send_frame_func(video_frame, video_frame_size, STREAM_TYPE_HEVC, is_intra,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts,
                                                               0, // PCR
                                                               tsdata->source,
                                                               0, // sub-source is 0 for video
                                                               (char*)&tsdata->master_pmt_table[each_pmt].decoded_language_tag[pid_count].lang_tag[0],
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].corruption_count,
                                                               tsdata->master_pat_table.pmt_table_entries,
                                                               send_frame_context);
                                           } else if (stream_type == 0x1b) {
                                               int vf;
                                               int nal_type;
                                               int is_intra = 0;
                                               video_frame = (unsigned char*)tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer;
                                               for (vf = 0; vf < video_frame_size - 4; vf++) {
                                                   if (video_frame[vf] == 0x00 &&
                                                       video_frame[vf+1] == 0x00 &&
                                                       video_frame[vf+2] == 0x01) {
                                                       nal_type = video_frame[vf+3] & 0x1f;
                                                       //fprintf(stderr,"nal_type:0x%x\n", nal_type);
                                                       if (nal_type == 0x05 || nal_type == 0x07 || nal_type == 0x08) {
                                                           is_intra = 1;
                                                           if (tsdata->master_pmt_table[each_pmt].data_engine[pid_count].video_frame_count == 0) {
                                                               tsdata->first_frame_intra = 1;
                                                               is_intra = 1;
                                                           }
                                                           break;
                                                       }
                                                   }
                                               }

                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].video_frame_count++;

                                               if (send_frame_func)
                                               send_frame_func(video_frame, video_frame_size, STREAM_TYPE_H264, is_intra,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts,
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts,
                                                               0, // PCR
                                                               tsdata->source,
                                                               0, // sub-source is 0 for video
                                                               (char*)&tsdata->master_pmt_table[each_pmt].decoded_language_tag[pid_count].lang_tag[0],
                                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].corruption_count,
                                                               tsdata->master_pat_table.pmt_table_entries,
                                                               send_frame_context);
                                           }
                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].data_index = 0;
                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts = 0;
                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts = 0;
                                       }

                                       last_cc = tsdata->master_pmt_table[each_pmt].data_engine[pid_count].last_cc;
                                       if (last_cc == -1) {
                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].last_cc = cc;
                                       } else {
                                           int expected_continuity;

                                           expected_continuity = (last_cc + 1) % 16;
                                           if (expected_continuity != cc) {
                                               /*backup_caller(2000, 900+cc,
                                                             expected_continuity, current_pid,
                                                             total_input_packets, 0, backup_context);*/
                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].corruption_count++;
                                           }
                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].last_cc = cc;
                                       }
                                       pes_length = (*(pdata+4) << 8) + *(pdata+5);
                                       if (pes_length) {
                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].wanted_data_size = pes_length;
                                       }
                                       tsdata->master_pmt_table[each_pmt].data_engine[pid_count].actual_data_size = 0;
                                       tsdata->master_pmt_table[each_pmt].data_engine[pid_count].context = NULL;
                                       tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts = 0;
                                       tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts = 0;
                                       tsdata->master_pmt_table[each_pmt].data_engine[pid_count].data_index = 0;
                                       tsdata->master_pmt_table[each_pmt].data_engine[pid_count].flags = 0;
                                       tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pes_aligned = 0;

                                       int pes_header_size;
                                       int pes_aligned;
                                       int timestamp_present;
                                       int remaining_samples = 0;
                                       int pes_payload_remaining = TS_PAYLOAD_SIZE - (int)(pdata - pdata_initial);

                                       /* Need the 9-byte PES header prefix present. */
                                       if (pes_payload_remaining < 9) {
                                           goto continue_packet_processing;
                                       }
                                       pes_header_size = *(pdata+8);
                                       if (pes_header_size < 0 || pes_header_size > 184) {
                                           //backup_caller(2000, 916, current_pid, 0, 0, 0, backup_context);
                                           goto continue_packet_processing;
                                       }
                                       /* The declared PES header extension must fit
                                        * inside the remaining payload. */
                                       if (9 + pes_header_size > pes_payload_remaining) {
                                           goto continue_packet_processing;
                                       }
                                       pes_aligned = (check0 & 0x04) >> 2;
                                       tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pes_aligned = pes_aligned;
                                       pdata += 9;
                                       timestamp_present = (check1 & 0xc0) >> 6;
                                       if (timestamp_present == 1) {
                                           //backup_caller(2000, 917, current_pid, 0, 0, 0, backup_context);
                                           goto continue_packet_processing;
                                       } else if (timestamp_present == 2) {
                                           int64_t current_pts;
                                           int stream_count;
                                           int pid_index;

                                           /* PTS occupies 5 bytes after the 9-byte prefix. */
                                           if (pes_payload_remaining < 9 + 5) {
                                               goto continue_packet_processing;
                                           }
                                           current_pts = (*(pdata+0) >> 1) & 0x07;
                                           current_pts <<= 8;
                                           current_pts |= *(pdata+1);
                                           current_pts <<= 7;
                                           current_pts |= (*(pdata+2) >> 1) & 0x7f;
                                           current_pts <<= 8;
                                           current_pts |= *(pdata+3);
                                           current_pts <<= 7;
                                           current_pts |= (*(pdata+4) >> 1) & 0x7f;

                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts = current_pts;

                                           stream_count = tsdata->master_pmt_table[each_pmt].stream_count;
                                           for (pid_index = 0; pid_index < stream_count; pid_index++) {
                                               if (tsdata->master_pmt_table[each_pmt].stream_pid[pid_index] == current_pid) {
                                                   tsdata->master_pmt_table[each_pmt].last_pts[pid_index] = current_pts;
                                                   if (tsdata->master_pmt_table[each_pmt].first_pts[pid_index] == -1) {
                                                       tsdata->master_pmt_table[each_pmt].first_pts[pid_index] = current_pts;
                                                       break;
                                                   }
                                               }
                                           }
                                       } else if (timestamp_present == 3) {
                                           int64_t current_pts;
                                           int64_t current_dts;
                                           int stream_count;
                                           int pid_index;

                                           /* PTS+DTS occupy 10 bytes after the 9-byte prefix. */
                                           if (pes_payload_remaining < 9 + 10) {
                                               goto continue_packet_processing;
                                           }
                                           current_pts = (*(pdata+0) >> 1) & 0x07;
                                           current_pts <<= 8;
                                           current_pts |= *(pdata+1);
                                           current_pts <<= 7;
                                           current_pts |= (*(pdata+2) >> 1) & 0x7f;
                                           current_pts <<= 8;
                                           current_pts |= *(pdata+3);
                                           current_pts <<= 7;
                                           current_pts |= (*(pdata+4) >> 1) & 0x7f;

                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts = current_pts;

                                           stream_count = tsdata->master_pmt_table[each_pmt].stream_count;
                                           for (pid_index = 0; pid_index < stream_count; pid_index++) {
                                               if (tsdata->master_pmt_table[each_pmt].stream_pid[pid_index] == current_pid) {
                                                   tsdata->master_pmt_table[each_pmt].last_pts[pid_index] = current_pts;
                                                   if (tsdata->master_pmt_table[each_pmt].first_pts[pid_index] == -1) {
                                                       tsdata->master_pmt_table[each_pmt].first_pts[pid_index] = current_pts;
                                                       break;
                                                   }
                                               }
                                           }

                                           current_dts = (*(pdata+5) >> 1) & 0x07;
                                           current_dts <<= 8;
                                           current_dts |= *(pdata+6);
                                           current_dts <<= 7;
                                           current_dts |= (*(pdata+7) >> 1) & 0x7f;
                                           current_dts <<= 8;
                                           current_dts |= *(pdata+8);
                                           current_dts <<= 7;
                                           current_dts |= (*(pdata+9) >> 1) & 0x7f;

                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts = current_dts;
                                           for (pid_index = 0; pid_index < stream_count; pid_index++) {
                                               if (tsdata->master_pmt_table[each_pmt].stream_pid[pid_index] == current_pid) {
                                                   tsdata->master_pmt_table[each_pmt].last_dts[pid_index] = current_dts;
                                                   if (tsdata->master_pmt_table[each_pmt].first_dts[pid_index] == -1) {
                                                       tsdata->master_pmt_table[each_pmt].first_dts[pid_index] = current_dts;
                                                       break;
                                                   }
                                               }
                                           }
                                       } else {
                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].dts = 0;
                                           tsdata->master_pmt_table[each_pmt].data_engine[pid_count].pts = 0;
                                       }

                                       pdata += pes_header_size;
                                       remaining_samples = 184 - 9 - pes_header_size - adaptation_size;
                                       if (remaining_samples < 0) {
                                           goto continue_packet_processing;
                                       }
                                       if (remaining_samples > 0) {
                                           if (remaining_samples >= 184) {
                                               goto continue_packet_processing;
                                           }
                                           if (!tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer) {
                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer = (unsigned char *)malloc(MAX_BUFFER_SIZE);
                                           }
                                           if (tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer &&
                                               remaining_samples <= MAX_BUFFER_SIZE) {
                                               memcpy(tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer, pdata, remaining_samples);
                                               tsdata->master_pmt_table[each_pmt].data_engine[pid_count].data_index = remaining_samples;
                                           }
                                       }
                                       goto continue_packet_processing;
                                   }
                               }
                           }
                       }

                       if (current_pid == 0) {
                           int unit_size = *(pdata+0);
                           int pat_payload_remaining = TS_PAYLOAD_SIZE - (int)(pdata - pdata_initial);
                           int avail_from_pdata;
                           uint8_t table_id;
                           uint16_t section_size;
                           int version_number = 0;
                           int transport_stream_id;
                           int section_entries;
                           int entry_index;
                           uint16_t pmt_pid;
                           int is_valid;
                           int current_section;
                           int last_section;
                           int look_for_pmt_table;
                           int pat_program_number;

                           /* The pointer field plus the 9-byte fixed PAT header
                            * must fit inside the packet payload. */
                           if (unit_size < 0 || pat_payload_remaining < 0 ||
                               (unit_size + 9) > pat_payload_remaining) {
                               goto continue_packet_processing;
                           }
                           pdata += unit_size;
                           avail_from_pdata = pat_payload_remaining - unit_size;

                           table_id = *(pdata+1);
                           section_size = ((*(pdata+2) << 8) + *(pdata+3)) & 0x0fff;
                           transport_stream_id = (*(pdata+4) << 8) + *(pdata+5);

                           /* section_size counts the bytes that follow the 3-byte
                            * section header; it must cover at least the 9 fixed
                            * bytes before the program loop or the subtraction below
                            * underflows (it is unsigned). */
                           if (section_size < 9) {
                               goto continue_packet_processing;
                           }
                           section_size -= 9;
                           section_entries = section_size / 4;
                           entry_index = 9;

                           /* Clamp the program count so the entry loop cannot read
                            * past the packet payload, and cannot overrun
                            * pmt_pid_index[]/pmt_version[]/pmt_decoded[]. This code
                            * does not reassemble a PAT that spans multiple packets,
                            * so anything beyond this packet is intentionally dropped. */
                           {
                               int max_entries = (avail_from_pdata - 9) / 4;
                               if (max_entries < 0) {
                                   max_entries = 0;
                               }
                               if (section_entries > max_entries) {
                                   section_entries = max_entries;
                               }
                               if (section_entries > TSHARDEN_MAX_PMT_PID_IDX) {
                                   section_entries = TSHARDEN_MAX_PMT_PID_IDX;
                               }
                           }

                           version_number = (*(pdata+6) & 0x1E) >> 1;
                           is_valid = *(pdata+6) & 0x01;
                           current_section = *(pdata+7);
                           last_section = *(pdata+8);

                           if (tsdata->master_pat_table.max_pat_time == 0) {
                               tsdata->master_pat_table.min_pat_time = 999999999;
                               gettimeofday(&tsdata->master_pat_table.start_pat_time, NULL);
                               tsdata->master_pat_table.max_pat_time = 1;
                               //backup_caller(2000, 505, 1000, current_pid, 0, backup_context);
                           } else {
                               int64_t delta_pat_time;
                               gettimeofday(&tsdata->master_pat_table.end_pat_time, NULL);
                               delta_pat_time = (int64_t)get_time_difference(&tsdata->master_pat_table.end_pat_time,
                                                                             &tsdata->master_pat_table.start_pat_time);

                               if (delta_pat_time > tsdata->master_pat_table.max_pat_time) {
                                   tsdata->master_pat_table.max_pat_time = delta_pat_time;
                                   // SIGNAL NEW MAX PAT TIME TO GUI
                                   // backup_caller(2000, 507, delta_pat_time, 0, 0, backup_context);
                               }
                               if (delta_pat_time < tsdata->master_pat_table.min_pat_time) {
                                   tsdata->master_pat_table.min_pat_time = delta_pat_time;
                                   // SIGNAL NEW MIN PAT TIME TO GUI
                                   // backup_caller(2000, 508, delta_pat_time, 0, 0, backup_context);
                               }
                               //backup_caller(2000, 505, delta_pmt_time / 1000, current_pid, 0, backup_context);
                               tsdata->master_pat_table.avg_pat_time += delta_pat_time;
                               tsdata->master_pat_table.avg_pat_time /= 2;
                               gettimeofday(&tsdata->master_pat_table.start_pat_time, NULL);
                           }

                           look_for_pmt_table = 0;
                           TSDECODE_DBG("decoding PAT table, pat_version_number = %d, section_entries = %d, incoming_version_number = %d\n",
                                   tsdata->pat_version_number,
                                   section_entries,
                                   version_number);

                           if (tsdata->pat_transport_stream_id != transport_stream_id) {
                               tsdata->pat_version_number = -1;
                           }

                           if (tsdata->pat_version_number == -1) {
                               tsdata->pat_position = total_input_packets;
                               tsdata->pat_version_number = version_number;
                               tsdata->pat_program_count = section_entries;
                               tsdata->pat_transport_stream_id = transport_stream_id;
                               look_for_pmt_table = 1;
                               //backup_caller(2000, 100, tsdata->pat_program_count, 0, 0, 0, backup_context);
                           }

                           if (version_number != tsdata->pat_version_number) {
                               tsdata->pat_version_number = version_number;
                               tsdata->pat_program_count = section_entries;
                               tsdata->pat_transport_stream_id = transport_stream_id;
                               look_for_pmt_table = 1;
                               //backup_caller(2000, 100, tsdata->pat_program_count, 0, 0, 0, backup_context);
                           }

                           TSDECODE_DBG("decoding PAT table, transport_stream_id = %d, look_for_pmt_table = %d\n",
                                   transport_stream_id,
                                   look_for_pmt_table);

                           if (look_for_pmt_table == 1) {
                               int program_index;

                               tsdata->pmt_pid_count = 0;
                               memset(tsdata->pmt_decoded, 0, sizeof(tsdata->pmt_decoded));
                               for (program_index = 0; program_index < tsdata->pat_program_count; program_index++) {
                                   /* program_number is a 16-bit big-endian field:
                                    * high byte must be shifted left by 8 (the old
                                    * code shifted a single byte right by 8, which
                                    * is always 0). */
                                   pat_program_number = (int)((((unsigned)*(pdata+entry_index)) << 8) +
                                                              (unsigned)*(pdata+entry_index+1));

                                   if (pat_program_number == 0) {
                                       //backup_caller(2000, 300, 0, 0, 0, 0, backup_context);
                                   } else {
                                       pmt_pid = (unsigned short)((((unsigned long)*(pdata+entry_index+2) << 8)) |
                                                                  (unsigned long)(*(pdata+entry_index+3)));
                                       pmt_pid = pmt_pid & 0x1fff;

                                       //backup_caller(2000, 200, pmt_pid, 0, 0, 0, backup_context);
                                       if (tsdata->pmt_pid_count < TSHARDEN_MAX_PMT_PID_IDX) {
                                           tsdata->pmt_pid_index[tsdata->pmt_pid_count] = pmt_pid;
                                           tsdata->pmt_pid_count++;
                                       }

                                       TSDECODE_DBG("decoding PAT table, pmt_pid = %d\n", pmt_pid);
                                   }
                                   entry_index += 4;
                               }

                               if (!tsdata->pmt_pid_count) {
                                   //backup_caller(2000, 202, 0, 0, 0, 0, backup_context);
                               }
                           }
                       }
                   } else { // NOT THE START
                       int pid_count = 0;
                         int acquired_data_so_far = 0;
                         int tempval;

                         tempval = pdata - pdata_initial;

                         acquired_data_so_far = 184 - tempval;

                         for (pid_count = 0; pid_count < tsdata->pmt_pid_count; pid_count++) {
                              if (tsdata->pmt_pid_index[pid_count] == current_pid) {
                                   if (tsdata->pmt_table_acquired > 0 &&
                                       tsdata->pmt_table_acquired < MAX_TABLE_SIZE &&
                                       tsdata->pmt_table_expected > 0) {
                                       int pmt_bytes_remaining = tsdata->pmt_table_expected - tsdata->pmt_table_acquired;
                                       int dst_space = MAX_TABLE_SIZE - tsdata->pmt_table_acquired;

                                       /* acquired_data_so_far starts as the bytes
                                        * present in this packet (184 - offset). Only
                                        * ever shrink it: never copy more than the
                                        * table still needs, more than the
                                        * destination can hold, or (implicitly) more
                                        * than the source packet provides. The old
                                        * code set it to pmt_bytes_remaining, which
                                        * could grow it past the source buffer. */
                                       if (pmt_bytes_remaining < 0) {
                                           pmt_bytes_remaining = 0;
                                       }
                                       if (acquired_data_so_far > pmt_bytes_remaining) {
                                           acquired_data_so_far = pmt_bytes_remaining;
                                       }
                                       if (acquired_data_so_far > dst_space) {
                                           acquired_data_so_far = dst_space;
                                       }
                                       if (acquired_data_so_far < 0) {
                                           acquired_data_so_far = 0;
                                       }
                                       if (acquired_data_so_far > 0) {
                                           memcpy(&tsdata->pmt_data[tsdata->pmt_table_acquired], pdata, acquired_data_so_far);
                                           tsdata->pmt_table_acquired += acquired_data_so_far;
                                       }

                                       if (tsdata->pmt_table_acquired >= tsdata->pmt_table_expected) {
                                           unsigned short crc_position;
                                           unsigned long crc32_length;
                                           uint32_t calculated_crc;

                                           tsdata->pmt_data_size = tsdata->pmt_table_expected;
                                           crc_position = ((tsdata->pmt_data[2] << 8) + tsdata->pmt_data[3]) & 0x0fff;
                                           crc32_length = crc_position - 1;
                                           if (crc_position > 4 && (int)crc_position + 4 <= MAX_TABLE_SIZE) {
                                               uint32_t pmt_crc2;

                                               calculated_crc = getcrc32(&tsdata->pmt_data[1], crc32_length);
                                               calculated_crc ^= 0xffffffff;
                                               calculated_crc = htonl(calculated_crc);
                                               pmt_crc2 = tsharden_read_u32((const uint8_t*)&tsdata->pmt_data[crc_position]);
                                               pmt_crc2 ^= 0xffffffff;

                                               if (pmt_crc2 == calculated_crc) {
                                                   int pmt_version = ((tsdata->pmt_data[6]) & 0x1e) >> 1;
                                                   if (pmt_version != tsdata->pmt_version[pid_count] ||
                                                       tsdata->pmt_version[pid_count] == -1) {
                                                       tsdata->pmt_version[pid_count] = pmt_version;
                                                       decode_pmt_table(&tsdata->master_pat_table, tsdata->master_pmt_table, tsdata->pmt_data, tsdata->pmt_data_size, current_pid);
                                                   }
                                                   tsdata->pmt_decoded[pid_count] = 1;
                                               } else {
                                                   //backup_caller(2000, 201, calculated_crc, 0, 0, 0, backup_context);
                                               }
                                           }

                                           tsdata->pmt_table_acquired = 0;
                                           tsdata->pmt_table_expected = 0;
                                       }
                                       continue;
                                   }
                              }
                         }

                         for (each_pmt = 0; each_pmt < tsdata->master_pat_table.pmt_table_entries; each_pmt++)  {
                             if (each_pmt == stream_select && stream_select != -1) {
                                 int stream_count = tsdata->master_pmt_table[each_pmt].stream_count;
                                 for (pid_count = 0; pid_count < stream_count; pid_count++) {
                                     if (tsdata->master_pmt_table[each_pmt].stream_pid[pid_count] == current_pid) {
                                         int last_cc;

                                         last_cc = tsdata->master_pmt_table[each_pmt].data_engine[pid_count].last_cc;
                                         if (last_cc == -1) {
                                             tsdata->master_pmt_table[each_pmt].data_engine[pid_count].last_cc = cc;
                                         } else {
                                             int expected_continuity;
                                             expected_continuity = (last_cc + 1) % 16;
                                             if (expected_continuity != cc) {
                                                 /*backup_caller(2000, 900+cc,
                                                               expected_continuity, current_pid,
                                                               total_input_packets, 0, backup_context);*/
                                                 tsdata->master_pmt_table[each_pmt].data_engine[pid_count].corruption_count++;
                                             }
                                             tsdata->master_pmt_table[each_pmt].data_engine[pid_count].last_cc = cc;
                                         }

                                         if (tsdata->master_pmt_table[each_pmt].data_engine[pid_count].data_index > 0) {
                                             int remaining_samples = 184 - adaptation_size;
                                             if (remaining_samples < 0) {
                                                 continue;
                                             }
                                             if (tsdata->master_pmt_table[each_pmt].data_engine[pid_count].data_index + remaining_samples <= MAX_BUFFER_SIZE) {
                                                 memcpy(tsdata->master_pmt_table[each_pmt].data_engine[pid_count].buffer +
                                                        tsdata->master_pmt_table[each_pmt].data_engine[pid_count].data_index,
                                                        pdata,
                                                        remaining_samples);
                                                 tsdata->master_pmt_table[each_pmt].data_engine[pid_count].data_index += remaining_samples;
                                             }
                                         }
                                     }
                                 }
                             }  // stream select
                         } // each pmt
                   }// end of pusi
               }
          } // end of check for 0x47
continue_packet_processing:
          each_pmt = 0;
     } // end of for loop
     return 0;
}

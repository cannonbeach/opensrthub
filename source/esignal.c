/*****************************************************************************
  Copyright (C) 2018-2020 John William

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

#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include "srthub.h"
#include "dataqueue.h"
#include "esignal.h"
#include "curl.h"

#define MAX_SIGNAL_RESPONSE_SIZE 2048
#define MAX_FORMATTED_TIME 128
#define MAX_HOSTNAME_SIZE 128

static volatile int signal_thread_running = 0;
static pthread_t signal_thread_id;
static char *response_buffer = NULL;
static char *error_buffer = NULL;
static void *signal_thread(void *context);

int start_signal_thread(srthub_core_struct *core)
{
    signal_thread_running = 1;
    response_buffer = (char*)malloc(MAX_SIGNAL_RESPONSE_SIZE);
    error_buffer = (char*)malloc(MAX_SIGNAL_RESPONSE_SIZE);
    pthread_create(&signal_thread_id, NULL, signal_thread, (void*)core);
    return 0;
}

int stop_signal_thread(srthub_core_struct *core)
{
    signal_thread_running = 0;
    pthread_join(signal_thread_id, NULL);
    free(response_buffer);
    free(error_buffer);
    return 0;
}

int send_signal(srthub_core_struct *core, int signal_type, const char *message)
{
    dataqueue_message_struct *msg;

    msg = (dataqueue_message_struct*)malloc(sizeof(dataqueue_message_struct));
    if (msg) {
        memset(msg, 0, sizeof(dataqueue_message_struct));
        msg->buffer_type = signal_type;
        snprintf(msg->smallbuf, MAX_SMALLBUF_SIZE-1, "%s", message);
        dataqueue_put_front(core->signalqueue, msg);
    } else {
        fprintf(stderr,"fatal error: unable to generate signal!\n");
        exit(-1);
    }
    return 0;
}

/* Queues one SCTE-35 cue for reporting. The cue is copied, so the caller's
 * copy does not have to outlive the call. The signal thread frees both the
 * message and the attached cue. */
int send_signal_scte35(srthub_core_struct *core, const scte35_data_struct *cue)
{
    dataqueue_message_struct *msg;
    scte35_data_struct *payload;
    int signal_type;

    if (!cue) {
        return -1;
    }

    if (cue->cancel) {
        signal_type = SIGNAL_SCTE35_EVENT;
    } else if (cue->cue_direction == SCTE35_CUE_OUT) {
        signal_type = SIGNAL_SCTE35_START;
    } else if (cue->cue_direction == SCTE35_CUE_IN) {
        signal_type = SIGNAL_SCTE35_END;
    } else {
        signal_type = SIGNAL_SCTE35_EVENT;
    }

    payload = (scte35_data_struct*)malloc(sizeof(scte35_data_struct));
    if (!payload) {
        fprintf(stderr,"send_signal_scte35: unable to allocate cue payload\n");
        return -1;
    }
    memcpy(payload, cue, sizeof(scte35_data_struct));

    msg = (dataqueue_message_struct*)malloc(sizeof(dataqueue_message_struct));
    if (!msg) {
        fprintf(stderr,"send_signal_scte35: unable to allocate signal message\n");
        free(payload);
        return -1;
    }
    memset(msg, 0, sizeof(dataqueue_message_struct));
    msg->buffer_type = signal_type;
    msg->buffer = (void*)payload;
    msg->buffer_size = sizeof(scte35_data_struct);
    dataqueue_put_front(core->signalqueue, msg);

    return 0;
}

void signal_management_interface(srthub_core_struct *core, char *signal_buffer, int signal_buffer_length)
{
    CURLcode curlresponse;
    CURL *curl = NULL;
    long http_code = 200;
    char signal_url[MAX_STRING_SIZE];
    struct curl_slist *optional_data = NULL;
    int content_length = 0;
    int i;
    int signal_count = 1;

    /*if (strlen(core->cd->management_server) > 0) {
        signal_count++;
        }*/
    fprintf(stderr,"signal_management_inteface: sending signal\n");
    for (i = 0; i < signal_count; i++) {
        curl = curl_easy_init();
        optional_data = curl_slist_append(optional_data, "Content-Type: application/json");
        optional_data = curl_slist_append(optional_data, "Expect:");

        if (i == 0) {
            snprintf(signal_url,MAX_STRING_SIZE-1,"http://127.0.0.1:8080/api/v1/signal/%d",core->session_identifier);
        } else if (i == 1) {
            //send the signal to an additional destination as specified by the end-user
            //which could act as some sort of bridge to an snmp trap signal
            //we could also write a handler in the nodejs code to do bridging to another format
            //snprintf(signal_url,MAX_STRING_SIZE-1,"%s/%d",core->cd->management_server,core->session_identifier);
        }

        curl_easy_setopt(curl, CURLOPT_URL, signal_url);
        //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char*)signal_buffer);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)signal_buffer_length);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, optional_data);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
        //review these timeouts
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5);

        curlresponse = curl_easy_perform(curl);
        if (curlresponse != CURLE_OK) {
            //
        }
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, (long*)&http_code);

        curl_easy_cleanup(curl);
        curl_slist_free_all(optional_data);
        optional_data = NULL;
    }
}

int send_direct_error(srthub_core_struct *core, int signal_type, const char *message)
{
    time_t currenttime;
    struct tm currentUTC;
    char formattedtime[MAX_FORMATTED_TIME];
    int64_t id = (int64_t)core->session_identifier;
    char *sourcename = (char*)&core->config->sourcename;
    char node_hostname[MAX_HOSTNAME_SIZE];
    int nodeerr;

    memset(node_hostname,0,sizeof(node_hostname));

    nodeerr = gethostname((char*)&node_hostname[0], MAX_HOSTNAME_SIZE-1);
    if (nodeerr < 0) {
        snprintf(node_hostname,MAX_HOSTNAME_SIZE-1,"Unknown");
    }

    currenttime = time(NULL);
    gmtime_r(&currenttime, &currentUTC);
    strftime(formattedtime,MAX_FORMATTED_TIME-1,"%Y-%m-%dT%H:%M:%SZ",&currentUTC);

    snprintf(error_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
             "{\n"
             "    \"accesstime\": \"%s\",\n"
             "    \"host\": \"%s\",\n"
             "    \"id\": %ld,\n"
             "    \"status\": \"fatal error\",\n"
             "    \"message\": \"(%s)\"\n"
             "}\n",
             formattedtime,
             node_hostname,
             id,
             message);
    signal_management_interface(core, error_buffer, strlen(error_buffer));
    return 0;
}

void *signal_thread(void *context)
{
    srthub_core_struct *core = (srthub_core_struct*)context;
    dataqueue_message_struct *msg;
    int ret;
    char node_hostname[MAX_HOSTNAME_SIZE];
    int nodeerr;
    char *sourcename = (char*)&core->config->sourcename;

    memset(node_hostname,0,sizeof(node_hostname));

    nodeerr = gethostname((char*)&node_hostname[0], MAX_HOSTNAME_SIZE-1);
    if (nodeerr < 0) {
        snprintf(node_hostname,MAX_HOSTNAME_SIZE-1,"Unknown");
    }

    while (signal_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(core->signalqueue);
        while (!msg && signal_thread_running) {
            usleep(100000);
            msg = (dataqueue_message_struct*)dataqueue_take_back(core->signalqueue);
        }
        if (signal_thread_running) {
            time_t currenttime;
            struct tm currentUTC;
            char formattedtime[MAX_FORMATTED_TIME];
            int64_t id = (int64_t)core->session_identifier;

            currenttime = time(NULL);
            gmtime_r(&currenttime, &currentUTC);
            strftime(formattedtime,MAX_FORMATTED_TIME-1,"%Y-%m-%dT%H:%M:%SZ",&currentUTC);

            int buffer_type = msg->buffer_type;

            if (buffer_type == SIGNAL_SRT_CONNECTED) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            } else if (buffer_type == SIGNAL_SRT_UNABLE_TO_CONNECT) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            } else if (buffer_type == SIGNAL_NO_DATA) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            } else if (buffer_type == SIGNAL_SRT_CONNECTION_LOST) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            } else if (buffer_type == SIGNAL_START_SERVICE) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_STOP_SERVICE) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"Service Stopped\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_NO_INPUT_SIGNAL) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"No Input Signal Detected\",\n"
                         "    \"source\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_SCTE35_START ||
                buffer_type == SIGNAL_SCTE35_END ||
                buffer_type == SIGNAL_SCTE35_EVENT) {
                const scte35_data_struct *cue = (const scte35_data_struct*)msg->buffer;

                if (cue && msg->buffer_size == sizeof(scte35_data_struct)) {
                    char cue_message[MAX_SMALLBUF_SIZE];
                    const char *cue_text;
                    const char *command_text;
                    double duration_seconds = (double)cue->pts_duration / (double)90000.0;
                    int offset;

                    if (cue->cue_direction == SCTE35_CUE_OUT) {
                        cue_text = "out";
                    } else if (cue->cue_direction == SCTE35_CUE_IN) {
                        cue_text = "in";
                    } else {
                        cue_text = "none";
                    }

                    if (cue->splice_command_type == SCTE35_CMD_TIME_SIGNAL) {
                        command_text = "time_signal";
                    } else if (cue->splice_command_type == SCTE35_CMD_SPLICE_INSERT) {
                        command_text = "splice_insert";
                    } else {
                        command_text = "unknown";
                    }

                    /* Human-readable summary for the event log table. */
                    if (cue->cancel) {
                        offset = snprintf(cue_message, sizeof(cue_message),
                                          "SCTE-35 event canceled");
                    } else if (cue->cue_direction == SCTE35_CUE_OUT) {
                        offset = snprintf(cue_message, sizeof(cue_message),
                                          "SCTE-35 cue out");
                    } else if (cue->cue_direction == SCTE35_CUE_IN) {
                        offset = snprintf(cue_message, sizeof(cue_message),
                                          "SCTE-35 cue in");
                    } else {
                        offset = snprintf(cue_message, sizeof(cue_message),
                                          "SCTE-35 event");
                    }
                    if (offset < 0) {
                        offset = 0;
                    }
                    if (offset < (int)sizeof(cue_message) && cue->descriptor_name[0] != 0) {
                        int written = snprintf(cue_message + offset,
                                               sizeof(cue_message) - offset,
                                               " - %s", cue->descriptor_name);
                        if (written > 0) {
                            offset += written;
                            if (offset > (int)sizeof(cue_message)) {
                                offset = (int)sizeof(cue_message);
                            }
                        }
                    }
                    if (offset < (int)sizeof(cue_message)) {
                        if (cue->pts_duration > 0 && cue->splice_immediate) {
                            snprintf(cue_message + offset, sizeof(cue_message) - offset,
                                     " (event %ld, immediate, duration %.3fs)",
                                     (long)cue->splice_event_id, duration_seconds);
                        } else if (cue->pts_duration > 0) {
                            snprintf(cue_message + offset, sizeof(cue_message) - offset,
                                     " (event %ld, duration %.3fs)",
                                     (long)cue->splice_event_id, duration_seconds);
                        } else if (cue->splice_immediate) {
                            snprintf(cue_message + offset, sizeof(cue_message) - offset,
                                     " (event %ld, immediate)",
                                     (long)cue->splice_event_id);
                        } else {
                            snprintf(cue_message + offset, sizeof(cue_message) - offset,
                                     " (event %ld)",
                                     (long)cue->splice_event_id);
                        }
                    }

                    snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                             "{\n"
                             "    \"accesstime\": \"%s\",\n"
                             "    \"host\": \"%s\",\n"
                             "    \"sourcename\": \"%s\",\n"
                             "    \"id\": %ld,\n"
                             "    \"status\": \"success\",\n"
                             "    \"message\": \"%s\",\n"
                             "    \"scte35\": {\n"
                             "        \"cue\": \"%s\",\n"
                             "        \"immediate\": %s,\n"
                             "        \"duration\": %.3f,\n"
                             "        \"event-id\": %ld,\n"
                             "        \"program-id\": %d,\n"
                             "        \"auto-return\": %s,\n"
                             "        \"cancel\": %s,\n"
                             "        \"command\": \"%s\",\n"
                             "        \"segmentation-type-id\": %d,\n"
                             "        \"descriptor\": \"%s\",\n"
                             "        \"pts-time\": %ld,\n"
                             "        \"pts-adjustment\": %ld,\n"
                             "        \"pid\": %d\n"
                             "    }\n"
                             "}\n",
                             formattedtime,
                             node_hostname,
                             sourcename,
                             id,
                             cue_message,
                             cue_text,
                             cue->splice_immediate ? "true" : "false",
                             duration_seconds,
                             (long)cue->splice_event_id,
                             cue->program_id,
                             cue->auto_return ? "true" : "false",
                             cue->cancel ? "true" : "false",
                             command_text,
                             cue->segmentation_type_id,
                             cue->descriptor_name,
                             (long)cue->pts_time,
                             (long)cue->pts_adjustment,
                             cue->splice_pid);
                } else {
                    /* no cue attached - should not happen, but never report a
                     * cue that was not actually decoded */
                    snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                             "{\n"
                             "    \"accesstime\": \"%s\",\n"
                             "    \"host\": \"%s\",\n"
                             "    \"sourcename\": \"%s\",\n"
                             "    \"id\": %ld,\n"
                             "    \"status\": \"warning\",\n"
                             "    \"message\": \"SCTE-35 signal received with no cue data\"\n"
                             "}\n",
                             formattedtime,
                             node_hostname,
                             sourcename,
                             id);
                }
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_SEGMENT_PUBLISHED) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"segment successfully published\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_SEGMENT_FAILED) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"error\",\n"
                         "    \"message\": \"segment publish failed\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_HIGH_CPU) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"high cpu usage detected\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_LOW_DISK_SPACE) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"disk space is low\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_INPUT_SIGNAL_LOCKED) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_DECODE_ERROR) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"error\",\n"
                         "    \"message\": \"decode error (%s)\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_VIDEO_ASPECT_CHANGE) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"Source aspect ratio changed: %s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_PARSE_ERROR) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"error\",\n"
                         "    \"message\": \"parse error (%s)\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_MALFORMED_DATA) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"sourcename\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"error\",\n"
                         "    \"message\": \"malformed data (%s)\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         sourcename,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
        }
        if (msg) {
            if (msg->buffer) {
                free(msg->buffer);
                msg->buffer = NULL;
            }
            free(msg);
            msg = NULL;
        }
    }

    return NULL;
}

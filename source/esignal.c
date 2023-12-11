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

#define MAX_SIGNAL_RESPONSE_SIZE 1024
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
        msg->buffer_type = signal_type;
        snprintf(msg->smallbuf, MAX_SMALLBUF_SIZE-1, "%s", message);
        dataqueue_put_front(core->signalqueue, msg);
    } else {
        fprintf(stderr,"fatal error: unable to generate signal!\n");
        exit(-1);
    }
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
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            } else if (buffer_type == SIGNAL_SRT_UNABLE_TO_CONNECT) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            } else if (buffer_type == SIGNAL_NO_DATA) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            } else if (buffer_type == SIGNAL_SRT_CONNECTION_LOST) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            } else if (buffer_type == SIGNAL_START_SERVICE) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"Service Started\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_STOP_SERVICE) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"Service Stopped\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_NO_INPUT_SIGNAL) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"No Input Signal Detected\",\n"
                         "    \"source\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_SCTE35_START) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"scte35 out of network start\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_SCTE35_END) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"scte35 out of network done\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_SEGMENT_PUBLISHED) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"segment successfully published\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_SEGMENT_FAILED) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"error\",\n"
                         "    \"message\": \"segment publish failed\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_HIGH_CPU) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"high cpu usage detected\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_LOW_DISK_SPACE) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"warning\",\n"
                         "    \"message\": \"disk space is low\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_INPUT_SIGNAL_LOCKED) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"success\",\n"
                         "    \"message\": \"%s\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_DECODE_ERROR) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"error\",\n"
                         "    \"message\": \"decode error (%s)\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_PARSE_ERROR) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"error\",\n"
                         "    \"message\": \"parse error (%s)\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
            if (buffer_type == SIGNAL_MALFORMED_DATA) {
                snprintf(response_buffer, MAX_SIGNAL_RESPONSE_SIZE-1,
                         "{\n"
                         "    \"accesstime\": \"%s\",\n"
                         "    \"host\": \"%s\",\n"
                         "    \"id\": %ld,\n"
                         "    \"status\": \"error\",\n"
                         "    \"message\": \"malformed data (%s)\"\n"
                         "}\n",
                         formattedtime,
                         node_hostname,
                         id,
                         msg->smallbuf);
                signal_management_interface(core, response_buffer, strlen(response_buffer));
            }
        }
        free(msg);
        msg = NULL;
    }

    return NULL;
}

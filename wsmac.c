/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2021, Silicon Labs
 * Main authors:
 *     - Jérôme Pouiller <jerome.pouiller@silabs.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/select.h>

#include "wsmac.h"
#include "slist.h"
#include "log.h"
#include "os_timer.h"
#include "os_types.h"
#include "hal_interrupt.h"
#include "mbed-trace/mbed_trace.h"

#define TRACE_GROUP  "main"

// See warning in wsmac.h
struct wsmac_ctxt g_ctxt = { };
// See warning in os_types.h
struct os_ctxt g_os_ctxt = { };

void print_help(FILE *stream, int exit_code) {
    fprintf(stream, "Start Wi-SUN MAC emulation\n");
    fprintf(stream, "\n");
    fprintf(stream, "Usage:\n");
    fprintf(stream, "  wisun-mac [OPTIONS] UART_DEVICE\n");
    fprintf(stream, "\n");
    fprintf(stream, "Examples:\n");
    fprintf(stream, "  wisun-mac /dev/pts/15\n");
    exit(exit_code);
}

void configure(struct wsmac_ctxt *ctxt, int argc, char *argv[])
{
    static const struct option opt_list[] = {
        { "help", no_argument, 0, 'h' },
        { 0,      0,           0,  0  }
    };
    int opt;

    while ((opt = getopt_long(argc, argv, "h", opt_list, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_help(stdout, 0);
                break;
            case '?':
            default:
                print_help(stderr, 1);
                break;
        }
    }
    if (argc != optind + 1)
        print_help(stderr, 1);
}

int main(int argc, char *argv[])
{
    struct wsmac_ctxt *ctxt = &g_ctxt;
    struct callback_timer *timer;
    uint64_t timer_val;
    int maxfd, ret;
    fd_set rfds;

    ctxt->os_ctxt = &g_os_ctxt;
    platform_critical_init();
    mbed_trace_init();
    configure(ctxt, argc, argv);

    for (;;) {
        maxfd = 0;
        FD_ZERO(&rfds);
        SLIST_FOR_EACH_ENTRY(ctxt->os_ctxt->timers, timer, node) {
            FD_SET(timer->fd, &rfds);
            maxfd = max(maxfd, timer->fd);
        }
        ret = pselect(maxfd + 1, &rfds, NULL, NULL, NULL, NULL);
        if (ret < 0)
            FATAL(2, "pselect: %m");
        SLIST_FOR_EACH_ENTRY(ctxt->os_ctxt->timers, timer, node) {
            if (FD_ISSET(timer->fd, &rfds)) {
                read(timer->fd, &timer_val, sizeof(timer_val));
                WARN_ON(timer_val != 1);
                timer->fn(timer->fd, 0);
            }
        }
    }

    return 0;
}


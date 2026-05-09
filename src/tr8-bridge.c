/*
 * tr8-bridge — drain a class-compliant USB-MIDI device's rawmidi node and
 * re-publish the bidirectional stream as an ALSA sequencer port.
 *
 * Originally diagnosed against the Roland TR-8: when the TR-8 is connected
 * over USB but no host-side userspace client is reading its rawmidi output,
 * Linux's snd-usb-audio driver does not aggressively pull bytes off the
 * device's USB bulk-IN endpoint. The TR-8's internal USB-TX FIFO fills,
 * back-pressure stalls its firmware's MIDI send routine, and the sequencer
 * loop ends up gated on USB drain timing instead of its own quartz —
 * audibly, the pattern stutters, drifts, and occasionally stalls.
 *
 * This daemon opens the device's rawmidi node exclusively, drains it
 * continuously via poll(), and forwards events both ways onto an ALSA seq
 * port that PipeWire / JACK pick up as a normal MIDI device. DAWs subscribe
 * to the bridged seq port instead of fighting over the rawmidi node.
 *
 * Single-threaded by design: snd_seq_t is not thread-safe, and a single
 * poll() loop has lower latency than a thread-per-direction model anyway.
 *
 * Copyright (C) 2026 Kim McCann
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void log_alsa(const char *prefix, int err)
{
    fprintf(stderr, "tr8-bridge: %s: %s\n", prefix, snd_strerror(err));
}

/*
 * Accept "hw:X[,Y[,Z]]", "midiCXDY", or "/dev/snd/midiCXDY" and return a
 * malloc'd ALSA hw spec that snd_rawmidi_open accepts. NULL on parse failure.
 * Lets the systemd template unit pass %I (the kernel name) directly.
 */
static char *normalize_rawmidi_spec(const char *in)
{
    if (!in || !*in) return NULL;

    if (strncmp(in, "hw:", 3) == 0) {
        return strdup(in);
    }

    const char *base = in;
    if (strncmp(in, "/dev/snd/", 9) == 0) {
        base = in + 9;
    }

    int card = -1, dev = -1;
    if (sscanf(base, "midiC%dD%d", &card, &dev) == 2 && card >= 0 && dev >= 0) {
        char *out = NULL;
        if (asprintf(&out, "hw:%d,%d", card, dev) < 0) return NULL;
        return out;
    }

    return NULL;
}

static void try_realtime(int prio)
{
    struct sched_param sp = { .sched_priority = prio };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        fprintf(stderr,
                "tr8-bridge: warning: SCHED_FIFO/%d not granted (%s); "
                "running with default scheduling\n",
                prio, strerror(errno));
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr,
                "tr8-bridge: warning: mlockall failed (%s); "
                "page faults may add jitter\n",
                strerror(errno));
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s -d <rawmidi-spec> [-n <port-name>] [-p <rt-prio>]\n"
        "\n"
        "  -d <rawmidi-spec>   ALSA rawmidi device. Accepts:\n"
        "                        hw:X[,Y[,Z]]\n"
        "                        midiCXDY\n"
        "                        /dev/snd/midiCXDY\n"
        "  -n <port-name>      Visible name of the bridged seq port\n"
        "                      (default: \"TR-8 (bridged)\")\n"
        "  -p <rt-prio>        SCHED_FIFO priority (default: 80)\n"
        "  -h                  This help.\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *rawmidi_arg = NULL;
    const char *port_name = "TR-8 (bridged)";
    int rt_prio = 80;

    int opt;
    while ((opt = getopt(argc, argv, "d:n:p:h")) != -1) {
        switch (opt) {
        case 'd': rawmidi_arg = optarg; break;
        case 'n': port_name = optarg; break;
        case 'p': rt_prio = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    if (!rawmidi_arg) {
        fprintf(stderr, "tr8-bridge: -d <rawmidi-spec> is required\n");
        usage(argv[0]);
        return 2;
    }

    char *rawmidi_spec = normalize_rawmidi_spec(rawmidi_arg);
    if (!rawmidi_spec) {
        fprintf(stderr, "tr8-bridge: cannot parse rawmidi spec: %s\n", rawmidi_arg);
        return 2;
    }

    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    snd_rawmidi_t *raw_in = NULL;
    snd_rawmidi_t *raw_out = NULL;
    int err = snd_rawmidi_open(&raw_in, &raw_out, rawmidi_spec, SND_RAWMIDI_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "tr8-bridge: cannot open rawmidi %s: %s\n",
                rawmidi_spec, snd_strerror(err));
        free(rawmidi_spec);
        return 1;
    }

    snd_seq_t *seq = NULL;
    err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (err < 0) {
        log_alsa("snd_seq_open", err);
        return 1;
    }
    snd_seq_set_client_name(seq, "tr8-bridge");

    int port = snd_seq_create_simple_port(seq, port_name,
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
    if (port < 0) {
        log_alsa("snd_seq_create_simple_port", port);
        return 1;
    }

    snd_midi_event_t *encoder = NULL; /* rawmidi bytes -> seq events */
    snd_midi_event_t *decoder = NULL; /* seq events -> rawmidi bytes */
    if ((err = snd_midi_event_new(4096, &encoder)) < 0) {
        log_alsa("snd_midi_event_new(encoder)", err);
        return 1;
    }
    if ((err = snd_midi_event_new(4096, &decoder)) < 0) {
        log_alsa("snd_midi_event_new(decoder)", err);
        return 1;
    }
    /* Always write full status bytes back to the device — the TR-8 handles
     * running status fine but explicit is safer for a generic bridge. */
    snd_midi_event_no_status(decoder, 1);

    try_realtime(rt_prio);

    int npfds_raw = snd_rawmidi_poll_descriptors_count(raw_in);
    int npfds_seq = snd_seq_poll_descriptors_count(seq, POLLIN);
    int npfds = npfds_raw + npfds_seq;
    if (npfds <= 0) {
        fprintf(stderr, "tr8-bridge: no pollable descriptors\n");
        return 1;
    }
    struct pollfd *pfds = calloc((size_t)npfds, sizeof(*pfds));
    if (!pfds) {
        fprintf(stderr, "tr8-bridge: out of memory\n");
        return 1;
    }
    snd_rawmidi_poll_descriptors(raw_in, pfds, (unsigned int)npfds_raw);
    snd_seq_poll_descriptors(seq, pfds + npfds_raw, (unsigned int)npfds_seq, POLLIN);

    fprintf(stderr,
            "tr8-bridge: bridging %s <-> seq \"tr8-bridge:%s\" "
            "(client %d port %d)\n",
            rawmidi_spec, port_name, snd_seq_client_id(seq), port);

    unsigned char buf[1024];

    while (g_running) {
        int n = poll(pfds, (nfds_t)npfds, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "tr8-bridge: poll: %s\n", strerror(errno));
            break;
        }
        if (n == 0) continue;

        /* rawmidi -> seq */
        unsigned short revents = 0;
        snd_rawmidi_poll_descriptors_revents(raw_in, pfds,
                                             (unsigned int)npfds_raw, &revents);
        if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "tr8-bridge: rawmidi disconnected\n");
            break;
        }
        if (revents & POLLIN) {
            for (;;) {
                ssize_t r = snd_rawmidi_read(raw_in, buf, sizeof(buf));
                if (r == -EAGAIN) break;
                if (r < 0) {
                    log_alsa("snd_rawmidi_read", (int)r);
                    g_running = 0;
                    break;
                }
                if (r == 0) break;

                for (ssize_t i = 0; i < r; i++) {
                    snd_seq_event_t ev;
                    snd_seq_ev_clear(&ev);
                    long got = snd_midi_event_encode_byte(encoder, buf[i], &ev);
                    if (got == 1) {
                        snd_seq_ev_set_source(&ev, port);
                        snd_seq_ev_set_subs(&ev);
                        snd_seq_ev_set_direct(&ev);
                        int e = snd_seq_event_output_direct(seq, &ev);
                        if (e < 0 && e != -EAGAIN) {
                            log_alsa("snd_seq_event_output_direct", e);
                        }
                    }
                }
            }
        }

        /* seq -> rawmidi */
        revents = 0;
        snd_seq_poll_descriptors_revents(seq, pfds + npfds_raw,
                                         (unsigned int)npfds_seq, &revents);
        if (revents & POLLIN) {
            for (;;) {
                snd_seq_event_t *ev = NULL;
                int r = snd_seq_event_input(seq, &ev);
                if (r == -EAGAIN) break;
                if (r < 0) {
                    log_alsa("snd_seq_event_input", r);
                    break;
                }
                if (!ev) continue;

                long len = snd_midi_event_decode(decoder, buf, sizeof(buf), ev);
                if (len > 0) {
                    ssize_t w = snd_rawmidi_write(raw_out, buf, (size_t)len);
                    if (w < 0 && w != -EAGAIN) {
                        log_alsa("snd_rawmidi_write", (int)w);
                    } else if (w == -EAGAIN) {
                        fprintf(stderr,
                                "tr8-bridge: rawmidi TX full; dropped %ld bytes\n",
                                len);
                    }
                }
            }
        }
    }

    fprintf(stderr, "tr8-bridge: shutting down\n");
    snd_midi_event_free(encoder);
    snd_midi_event_free(decoder);
    free(pfds);
    snd_seq_close(seq);
    snd_rawmidi_close(raw_in);
    snd_rawmidi_close(raw_out);
    free(rawmidi_spec);
    return 0;
}

/*
 * tr8-bridge — drain a class-compliant USB-MIDI device's rawmidi node and
 * re-publish the bidirectional stream as ALSA sequencer ports.
 *
 * Originally diagnosed against the Roland TR-8: when the TR-8 is connected
 * over USB but no host-side userspace client is reading its rawmidi output,
 * Linux's snd-usb-audio driver does not aggressively pull bytes off the
 * device's USB bulk-IN endpoint. The TR-8's internal USB-TX FIFO fills,
 * back-pressure stalls its firmware's MIDI send routine, and the sequencer
 * loop ends up gated on USB drain timing instead of its own quartz —
 * audibly, the pattern stutters, drifts, and occasionally stalls.
 *
 * This daemon opens every substream of the device's rawmidi node
 * exclusively, drains them continuously via a single poll() loop, and
 * forwards events both ways onto matching ALSA seq ports that PipeWire /
 * JACK pick up as normal MIDI devices. DAWs subscribe to the bridged seq
 * ports instead of fighting over the rawmidi node.
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

#define MAX_STREAMS 16
#define BUF_SIZE    8192

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

typedef struct {
    int                  card;
    int                  device;
    int                  subdevice;
    snd_rawmidi_t       *raw_in;
    snd_rawmidi_t       *raw_out;
    int                  seq_port;
    snd_midi_event_t    *encoder;   /* rawmidi bytes -> seq events */
    snd_midi_event_t    *decoder;   /* seq events    -> rawmidi bytes */
    char                 hw_spec[32];
    char                 port_name[96];
    int                  n_pfds;
    int                  pfd_off;
} stream_t;

static stream_t g_streams[MAX_STREAMS];
static int      g_nstreams = 0;

static void rtrim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

/*
 * Parse a device spec into card/device, with an optional subdevice. If the
 * spec doesn't pin a subdevice (hw:X,Y or midiCXDY), *subdevice is set to
 * -1 to mean "bridge all substreams of this device". Returns 0 on success.
 */
static int parse_spec(const char *in, int *card, int *device, int *subdevice)
{
    if (!in || !*in) return -1;

    *subdevice = -1;

    if (strncmp(in, "hw:", 3) == 0) {
        int c, d, s;
        int n = sscanf(in + 3, "%d,%d,%d", &c, &d, &s);
        if (n >= 2 && c >= 0 && d >= 0) {
            *card = c;
            *device = d;
            if (n == 3 && s >= 0) *subdevice = s;
            return 0;
        }
        return -1;
    }

    const char *base = in;
    if (strncmp(in, "/dev/snd/", 9) == 0) base = in + 9;

    int c, d;
    if (sscanf(base, "midiC%dD%d", &c, &d) == 2 && c >= 0 && d >= 0) {
        *card = c;
        *device = d;
        return 0;
    }
    return -1;
}

/* Count input substreams of (card, device). Returns count or negative err. */
static int count_substreams(int card, int device)
{
    char hwname[16];
    snprintf(hwname, sizeof(hwname), "hw:%d", card);

    snd_ctl_t *ctl = NULL;
    int err = snd_ctl_open(&ctl, hwname, 0);
    if (err < 0) return err;

    snd_rawmidi_info_t *info;
    snd_rawmidi_info_alloca(&info);
    snd_rawmidi_info_set_device(info, (unsigned int)device);
    snd_rawmidi_info_set_subdevice(info, 0);
    snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);

    err = snd_ctl_rawmidi_info(ctl, info);
    int count = (err < 0) ? err : (int)snd_rawmidi_info_get_subdevices_count(info);

    snd_ctl_close(ctl);
    return count;
}

/* Derive a friendly seq port name from kernel rawmidi info. */
static void derive_port_name(int card, int device, int subdevice,
                             char *out, size_t outlen)
{
    char hwname[16];
    snprintf(hwname, sizeof(hwname), "hw:%d", card);

    snd_ctl_t *ctl = NULL;
    if (snd_ctl_open(&ctl, hwname, 0) < 0) {
        snprintf(out, outlen, "rawmidi hw:%d,%d,%d (bridged)",
                 card, device, subdevice);
        return;
    }

    snd_rawmidi_info_t *info;
    snd_rawmidi_info_alloca(&info);
    snd_rawmidi_info_set_device(info, (unsigned int)device);
    snd_rawmidi_info_set_subdevice(info, (unsigned int)subdevice);
    snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);

    if (snd_ctl_rawmidi_info(ctl, info) < 0) {
        snprintf(out, outlen, "rawmidi hw:%d,%d,%d (bridged)",
                 card, device, subdevice);
        snd_ctl_close(ctl);
        return;
    }

    const char *subname = snd_rawmidi_info_get_subdevice_name(info);
    const char *devname = snd_rawmidi_info_get_name(info);

    char tmp[80];
    if (subname && *subname) {
        snprintf(tmp, sizeof(tmp), "%s", subname);
        rtrim(tmp);
        snprintf(out, outlen, "%s (bridged)", tmp);
    } else if (devname && *devname) {
        snprintf(tmp, sizeof(tmp), "%s", devname);
        rtrim(tmp);
        snprintf(out, outlen, "%s MIDI %d (bridged)", tmp, subdevice + 1);
    } else {
        snprintf(out, outlen, "rawmidi hw:%d,%d,%d (bridged)",
                 card, device, subdevice);
    }

    snd_ctl_close(ctl);
}

static int open_stream(stream_t *s, int card, int device, int subdevice,
                       snd_seq_t *seq)
{
    s->card = card;
    s->device = device;
    s->subdevice = subdevice;
    snprintf(s->hw_spec, sizeof(s->hw_spec), "hw:%d,%d,%d",
             card, device, subdevice);

    int err = snd_rawmidi_open(&s->raw_in, &s->raw_out,
                               s->hw_spec, SND_RAWMIDI_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "tr8-bridge: cannot open rawmidi %s: %s\n",
                s->hw_spec, snd_strerror(err));
        return err;
    }

    derive_port_name(card, device, subdevice,
                     s->port_name, sizeof(s->port_name));

    s->seq_port = snd_seq_create_simple_port(seq, s->port_name,
        SND_SEQ_PORT_CAP_READ  | SND_SEQ_PORT_CAP_SUBS_READ |
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
    if (s->seq_port < 0) {
        log_alsa("snd_seq_create_simple_port", s->seq_port);
        return s->seq_port;
    }

    if ((err = snd_midi_event_new(BUF_SIZE, &s->encoder)) < 0) {
        log_alsa("snd_midi_event_new(encoder)", err);
        return err;
    }
    if ((err = snd_midi_event_new(BUF_SIZE, &s->decoder)) < 0) {
        log_alsa("snd_midi_event_new(decoder)", err);
        return err;
    }
    /* Always emit full status bytes back to the device — the parser side
     * leaves running status reconstruction enabled (default). */
    snd_midi_event_no_status(s->decoder, 1);

    return 0;
}

static void close_stream(stream_t *s)
{
    if (s->encoder) snd_midi_event_free(s->encoder);
    if (s->decoder) snd_midi_event_free(s->decoder);
    if (s->raw_in)  snd_rawmidi_close(s->raw_in);
    if (s->raw_out) snd_rawmidi_close(s->raw_out);
    memset(s, 0, sizeof(*s));
}

static stream_t *find_stream_by_seq_port(int port)
{
    for (int i = 0; i < g_nstreams; i++) {
        if (g_streams[i].seq_port == port) return &g_streams[i];
    }
    return NULL;
}

static void try_realtime(int prio)
{
    struct sched_param sp = { .sched_priority = prio };
    int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (err != 0) {
        fprintf(stderr,
                "tr8-bridge: warning: SCHED_FIFO/%d not granted (%s); "
                "running with default scheduling\n",
                prio, strerror(err));
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
        "Usage: %s -d <spec> [-p <rt-prio>]\n"
        "\n"
        "  -d <spec>     Rawmidi device. Accepts:\n"
        "                  hw:X,Y          (bridge all substreams of card X dev Y)\n"
        "                  hw:X,Y,Z        (bridge only substream Z)\n"
        "                  midiCXDY        (= hw:X,Y)\n"
        "                  /dev/snd/midiCXDY\n"
        "  -p <rt-prio>  SCHED_FIFO priority (default: 80)\n"
        "  -h            This help.\n"
        "\n"
        "Each substream is exposed as a seq port named after the kernel\n"
        "rawmidi subdevice with a \" (bridged)\" suffix.\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *spec = NULL;
    int rt_prio = 80;

    int opt;
    while ((opt = getopt(argc, argv, "d:p:h")) != -1) {
        switch (opt) {
        case 'd': spec = optarg; break;
        case 'p': rt_prio = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    if (!spec) {
        fprintf(stderr, "tr8-bridge: -d <spec> is required\n");
        usage(argv[0]);
        return 2;
    }

    int card, device, subdevice;
    if (parse_spec(spec, &card, &device, &subdevice) != 0) {
        fprintf(stderr, "tr8-bridge: cannot parse spec: %s\n", spec);
        return 2;
    }

    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    snd_seq_t *seq = NULL;
    int err = snd_seq_open(&seq, "default",
                           SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (err < 0) {
        log_alsa("snd_seq_open", err);
        return 1;
    }
    snd_seq_set_client_name(seq, "tr8-bridge");

    /* Decide which substreams to bridge. */
    int subs[MAX_STREAMS];
    int nsubs = 0;
    if (subdevice >= 0) {
        subs[0] = subdevice;
        nsubs = 1;
    } else {
        int n = count_substreams(card, device);
        if (n < 0) {
            log_alsa("count_substreams", n);
            snd_seq_close(seq);
            return 1;
        }
        if (n == 0) {
            fprintf(stderr,
                    "tr8-bridge: no substreams found for hw:%d,%d\n",
                    card, device);
            snd_seq_close(seq);
            return 1;
        }
        if (n > MAX_STREAMS) {
            fprintf(stderr,
                    "tr8-bridge: device exposes %d substreams, capping at %d\n",
                    n, MAX_STREAMS);
            n = MAX_STREAMS;
        }
        for (int i = 0; i < n; i++) subs[i] = i;
        nsubs = n;
    }

    /* Open each substream. */
    for (int i = 0; i < nsubs; i++) {
        if (open_stream(&g_streams[i], card, device, subs[i], seq) != 0) {
            fprintf(stderr,
                    "tr8-bridge: failed to open substream %d; aborting\n",
                    subs[i]);
            for (int j = 0; j < i; j++) close_stream(&g_streams[j]);
            snd_seq_close(seq);
            return 1;
        }
        g_nstreams++;
        fprintf(stderr,
                "tr8-bridge: bridged %s -> seq port %d \"%s\"\n",
                g_streams[i].hw_spec, g_streams[i].seq_port,
                g_streams[i].port_name);
    }

    try_realtime(rt_prio);

    /* Build pollfds: per-stream rawmidi input + a single seq input. */
    int total_pfds = 0;
    for (int i = 0; i < g_nstreams; i++) {
        g_streams[i].n_pfds = snd_rawmidi_poll_descriptors_count(g_streams[i].raw_in);
        total_pfds += g_streams[i].n_pfds;
    }
    int seq_n_pfds  = snd_seq_poll_descriptors_count(seq, POLLIN);
    int seq_pfd_off = total_pfds;
    total_pfds += seq_n_pfds;

    if (total_pfds <= 0) {
        fprintf(stderr, "tr8-bridge: no pollable descriptors\n");
        return 1;
    }

    struct pollfd *pfds = calloc((size_t)total_pfds, sizeof(*pfds));
    if (!pfds) {
        fprintf(stderr, "tr8-bridge: out of memory\n");
        return 1;
    }

    int idx = 0;
    for (int i = 0; i < g_nstreams; i++) {
        g_streams[i].pfd_off = idx;
        snd_rawmidi_poll_descriptors(g_streams[i].raw_in, &pfds[idx],
                                     (unsigned)g_streams[i].n_pfds);
        idx += g_streams[i].n_pfds;
    }
    snd_seq_poll_descriptors(seq, &pfds[seq_pfd_off],
                             (unsigned)seq_n_pfds, POLLIN);

    fprintf(stderr,
            "tr8-bridge: ready, %d substream(s) on seq client %d\n",
            g_nstreams, snd_seq_client_id(seq));

    unsigned char buf[BUF_SIZE];

    while (g_running) {
        int n = poll(pfds, (nfds_t)total_pfds, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "tr8-bridge: poll: %s\n", strerror(errno));
            break;
        }
        if (n == 0) continue;

        /* rawmidi -> seq, per stream */
        for (int i = 0; i < g_nstreams && g_running; i++) {
            stream_t *s = &g_streams[i];
            unsigned short revents = 0;
            snd_rawmidi_poll_descriptors_revents(s->raw_in, &pfds[s->pfd_off],
                                                 (unsigned)s->n_pfds, &revents);
            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "tr8-bridge: %s disconnected\n", s->hw_spec);
                g_running = 0;
                break;
            }
            if (!(revents & POLLIN)) continue;

            for (;;) {
                ssize_t r = snd_rawmidi_read(s->raw_in, buf, sizeof(buf));
                if (r == -EAGAIN) break;
                if (r < 0) {
                    log_alsa("snd_rawmidi_read", (int)r);
                    g_running = 0;
                    break;
                }
                if (r == 0) break;

                for (ssize_t k = 0; k < r; k++) {
                    snd_seq_event_t ev;
                    snd_seq_ev_clear(&ev);
                    long got = snd_midi_event_encode_byte(s->encoder, buf[k], &ev);
                    if (got == 1) {
                        snd_seq_ev_set_source(&ev, s->seq_port);
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

        /* seq -> rawmidi, dispatched by destination port */
        unsigned short revents = 0;
        snd_seq_poll_descriptors_revents(seq, &pfds[seq_pfd_off],
                                         (unsigned)seq_n_pfds, &revents);
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

                stream_t *target = find_stream_by_seq_port(ev->dest.port);
                if (!target) continue;

                long len = snd_midi_event_decode(target->decoder, buf,
                                                 sizeof(buf), ev);
                if (len > 0) {
                    ssize_t w = snd_rawmidi_write(target->raw_out, buf, (size_t)len);
                    if (w < 0 && w != -EAGAIN) {
                        log_alsa("snd_rawmidi_write", (int)w);
                    } else if (w == -EAGAIN) {
                        fprintf(stderr,
                                "tr8-bridge: rawmidi TX full on %s; dropped %ld bytes\n",
                                target->hw_spec, len);
                    }
                }
            }
        }
    }

    fprintf(stderr, "tr8-bridge: shutting down\n");
    free(pfds);
    for (int i = 0; i < g_nstreams; i++) close_stream(&g_streams[i]);
    snd_seq_close(seq);
    return 0;
}

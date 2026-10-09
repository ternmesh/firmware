/* A node on a pseudo-terminal: link.c and the core's framing, with a fake radio, for
 * tests/link_script.py to drive tools/companion.py against as it would a board's USB port.
 *
 * It prints the terminal's path, serves it until stdin closes or a minute passes, and writes a
 * line of console text now and then, as a board does, to show the script reads past it. A client
 * that asks nothing for LINK_LAPSE is taken for gone, as on a board; the first argument, in
 * milliseconds, shortens that, so a test can watch it happen. A message
 * a client sends goes "on the air" a moment later. On stdin, an "m" is a message from Bob
 * received, and an "r" restarts the node, as a SET restarts a board: the messages are lost, their
 * ids start again, and another from Bob arrives. */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _XOPEN_SOURCE 700
#endif

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "link.h"

static int master = -1;
static struct link node;
static uint32_t clock_s;

static const uint8_t self[TERN_ADDRESS_LEN] = {
    0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
    0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};

static tern_time now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (tern_time)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* Bytes to the port. With no program holding it open, macOS fails the write (EIO) where Linux
 * buffers it; either way they are lost, as a board's UART loses them, and the node carries on. */
static void to_port(const void *bytes, size_t n) {
    if (write(master, bytes, n) < 0) {
        return;
    }
}

static void out(void *ctx, unsigned conn, const uint8_t *frame, size_t len) {
    uint8_t wrapped[TERN_COMPANION_STREAM_MAX];
    size_t n = tern_companion_wrap(frame, len, wrapped);
    (void)ctx;
    (void)conn;
    to_port(wrapped, n);
}

static void view(void *ctx, struct link_view *v) {
    (void)ctx;
    memset(v, 0, sizeof *v);
    memcpy(v->address, self, TERN_ADDRESS_LEN);
    v->role = 1;
    v->region = "EU868";
    v->power = 14;
    v->time = clock_s;
    v->n_neighbours = 1;
    v->neighbours[0] = (struct link_neighbour){0x1D2E3F40, 1, -38, 4};
    v->period_s = 3600;
    v->allowed_ms = 360000;
    v->used_ms = 1234;
    v->percent = 255;
}

static uint8_t set(void *ctx, const struct tern_companion_msg *m) {
    (void)ctx;
    return m->setting == TERN_C_SET_POWER && m->power > 22 ? TERN_C_ERR_REFUSED : 0;
}

static void set_time(void *ctx, uint32_t t) {
    (void)ctx;
    clock_s = t;
}

static bool session(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    (void)ctx;
    (void)address;
    return true;
}

static uint8_t end_session(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    (void)ctx;
    (void)address;
    return 0;
}

static uint8_t why(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    (void)ctx;
    (void)address;
    return TERN_C_WAIT_RADIO;
}

static uint8_t contacts[sizeof(struct link_contact) * LINK_CONTACTS];
static bool have_contacts;

static bool load(void *ctx, void *buf, size_t len) {
    (void)ctx;
    if (!have_contacts || len != sizeof contacts) {
        return false;
    }
    memcpy(buf, contacts, len);
    return true;
}

static bool save(void *ctx, const void *buf, size_t len) {
    (void)ctx;
    if (len != sizeof contacts) {
        return false;
    }
    memcpy(contacts, buf, len);
    have_contacts = true;
    return true;
}

static void frame_in(void *ctx, const uint8_t *frame, size_t len) {
    (void)ctx;
    link_receive(&node, LINK_SERIAL, now_ns(), frame, len);
}

static void text_in(void *ctx, uint8_t byte) {
    (void)ctx;
    (void)byte;
}

/* This node holds no groups, and has nothing to make one with. */
static bool load_groups(void *ctx, void *buf, size_t len) {
    (void)ctx;
    (void)buf;
    (void)len;
    return false;
}

static bool save_groups(void *ctx, const void *buf, size_t len) {
    (void)ctx;
    (void)buf;
    (void)len;
    return true;
}

static bool no_random(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    (void)buf;
    (void)len;
    return false;
}

static bool load_ids(void *ctx, uint32_t *next) {
    (void)ctx;
    (void)next;
    return false;
}

static bool save_ids(void *ctx, uint32_t next) {
    (void)ctx;
    (void)next;
    return true;
}

static bool load_count(void *ctx, uint32_t *next) {
    (void)ctx;
    (void)next;
    return false;
}

static bool save_count(void *ctx, uint32_t next) {
    (void)ctx;
    (void)next;
    return true;
}

static size_t load_writers(void *ctx, uint8_t *buf, size_t cap) {
    (void)ctx;
    (void)buf;
    (void)cap;
    return 0;
}

static bool save_writers(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx;
    (void)buf;
    (void)len;
    return true;
}

/* This node saves no messages: it is the one whose flash had no room for them. */
static size_t load_message(void *ctx, size_t place, uint8_t *buf, size_t cap) {
    (void)ctx;
    (void)place;
    (void)buf;
    (void)cap;
    return 0;
}

static enum link_saved save_message(void *ctx, size_t place, const uint8_t *buf, size_t len) {
    (void)ctx;
    (void)place;
    (void)buf;
    return len == 0 ? LINK_SAVED : LINK_NOT_SAVED;
}

static bool load_state(void *ctx, size_t place, uint64_t *state) {
    (void)ctx;
    (void)place;
    (void)state;
    return false;
}

static bool save_state(void *ctx, size_t place, uint64_t state) {
    (void)ctx;
    (void)place;
    (void)state;
    return false;
}

/* An update is kept in memory, and run by saying so: the script sees the whole of one. */
static uint8_t image[16384];
static uint32_t image_size;

static bool update_begin(void *ctx, uint32_t size) {
    (void)ctx;
    image_size = size;
    return size <= sizeof image;
}

static bool update_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len) {
    (void)ctx;
    memcpy(image + offset, data, len);
    return true;
}

static uint8_t update_run(void *ctx) {
    (void)ctx;
    static const char line[] = "(the update is whole)\r\n";
    to_port(line, sizeof line - 1);
    return 0;
}

int main(int argc, char **argv) {
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        perror("pty");
        return 1;
    }
    struct termios t;
    tcgetattr(master, &t);
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_lflag = 0;
    tcsetattr(master, TCSANOW, &t);
    printf("%s\n", ptsname(master));
    fflush(stdout);

    static const uint8_t bob[TERN_ADDRESS_LEN] = {0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a,
                                                  0x92, 0xb7, 0x0a, 0xa7, 0x4d, 0x1b, 0x7e, 0xbc,
                                                  0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c,
                                                  0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c};
    tern_time lapse = argc > 1 ? TERN_MS(atoi(argv[1])) : LINK_LAPSE;
    struct link_host host = {.ctx = NULL,
                             .firmware = "tern host test",
                             .out = out,
                             .view = view,
                             .set = set,
                             .set_time = set_time,
                             .session = session,
                             .why = why,
                             .end_session = end_session,
                             .load = load,
                             .save = save,
                             .load_groups = load_groups,
                             .save_groups = save_groups,
                             .random = no_random,
                             .load_ids = load_ids,
                             .save_ids = save_ids,
                             .load_count = load_count,
                             .save_count = save_count,
                             .load_writers = load_writers,
                             .save_writers = save_writers,
                             .load_message = load_message,
                             .save_message = save_message,
                             .load_state = load_state,
                             .save_state = save_state,
                             .board = "host",
                             .release = "0.2.0",
                             .update_room = sizeof image,
                             .update_begin = update_begin,
                             .update_write = update_write,
                             .update_run = update_run};
    link_init(&node, &host);
    link_open(&node, LINK_SERIAL, lapse, 0);
    struct tern_companion_parser parser;
    tern_companion_parser_init(&parser);
    const struct tern_companion_sink sink = {NULL, frame_in, text_in};

    tern_time start = now_ns(), chatter = start, aired_at = 0;
    while (now_ns() - start < TERN_S(60)) {
        struct pollfd fds[2] = {{master, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        if (poll(fds, 2, 20) < 0) {
            return 1;
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) <= 0) {
                return 0;
            }
            static const char morning[] = "Morning", back[] = "Back after a restart";
            if (c == 'm') {
                link_add(&node, bob, clock_s, TERN_C_RECEIVED, 0, (const uint8_t *)morning,
                         sizeof morning - 1);
            }
            if (c == 'r') {
                link_init(&node, &host);
                link_open(&node, LINK_SERIAL, lapse, 0);
                link_add(&node, bob, clock_s, TERN_C_RECEIVED, 0, (const uint8_t *)back,
                         sizeof back - 1);
            }
        }
        if (fds[0].revents & POLLIN) {
            uint8_t buf[256];
            ssize_t n = read(master, buf, sizeof buf);
            for (ssize_t i = 0; i < n; i++) {
                tern_companion_push(&parser, now_ns(), buf[i], &sink);
            }
        }
        tern_time now = now_ns();
        tern_companion_idle(&parser, now, &sink);
        link_tick(&node, now);
        struct link_message *x = link_outgoing(&node);
        if (x != NULL && aired_at == 0) {
            aired_at = now + TERN_MS(200);
        }
        if (x != NULL && now >= aired_at) {
            link_taken(&node, x->id);
            aired_at = 0;
        }
        if (now - chatter > TERN_MS(300)) {
            static const char line[] = "(a 23-byte frame for someone else, at -97 dBm)\r\n";
            chatter = now;
            to_port(line, sizeof line - 1);
        }
    }
    return 0;
}

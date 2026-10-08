#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "display.h"
#include "qr.h"
#include "tern/share.h"

/* The Heltec V3 port's screen for the person carrying the board (ports/heltec-v3/main/ui.c),
 * which has no hardware in it. Each page is drawn into a picture and read back from it, a line at
 * a time, by matching the font's glyphs: so what is checked is what the panel would show.
 *
 * Given a directory, it also writes each picture it checks there as a PBM image, for a person to
 * look at: `test_ui pictures/`. */

static struct display d;
static const char *pictures; /* the directory to write pictures into, or NULL */

#define COLS 21

/* The text a page shows, read off its pixels: each six-pixel cell matched against every printable
 * character, light on dark or, on the title row, dark on light. Trailing spaces are dropped; a
 * cell that is no character reads as '#'. */
static const char *row(int page) {
    static char out[COLS + 1];
    struct display one;
    for (int k = 0; k < COLS; k++) {
        out[k] = '#';
        for (char c = DISPLAY_BLUETOOTH; c <= '~'; c = c == DISPLAY_BLUETOOTH ? ' ' : c + 1) {
            char s[2] = {c, '\0'};
            display_init(&one);
            display_text(&one, 0, s, page == 0);
            if (memcmp(&one.px[0][0], &d.px[page][k * 6], 6) == 0) {
                out[k] = c;
                break;
            }
        }
    }
    out[COLS] = '\0';
    for (int k = COLS - 1; k >= 0 && out[k] == ' '; k--) {
        out[k] = '\0';
    }
    return out;
}

/* Whether pages `page` and `page + 1` show `text` large. */
static bool big(int page, const char *text) {
    struct display want;
    display_init(&want);
    display_big(&want, page, text);
    return memcmp(want.px[page], d.px[page], DISPLAY_WIDTH) == 0 &&
           memcmp(want.px[page + 1], d.px[page + 1], DISPLAY_WIDTH) == 0;
}

#define CHECK_ROW(page, text)                                                                      \
    do {                                                                                           \
        const char *got_ = row(page);                                                              \
        if (strcmp(got_, text) != 0) {                                                             \
            fprintf(stderr, "%s:%d: row %d is \"%s\", expected \"%s\"\n", __FILE__, __LINE__,      \
                    page, got_, text);                                                             \
            check_failures++;                                                                      \
        }                                                                                          \
    } while (0)

static void picture(const char *name) {
    if (pictures == NULL) {
        return;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.pbm", pictures, name);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P1\n%d %d\n", DISPLAY_WIDTH, DISPLAY_PAGES * 8);
    for (int y = 0; y < DISPLAY_PAGES * 8; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            fputs(d.px[y / 8][x] >> (y % 8) & 1 ? "1 " : "0 ", f);
        }
        fputc('\n', f);
    }
    fclose(f);
}

static void draw(const struct ui_node *n, int page, const char *name) {
    display_init(&d);
    ui_draw(n, page, &d);
    picture(name);
}

/* A board just started on a bench in the US: nothing heard, nothing sent. */
static struct ui_node alone(void) {
    struct ui_node n = {
        .region = "US915",
        .version = "0.1.0-alpha.3",
        .relay = true,
        .dbm = 2,
        .battery = UI_BATTERY_UNKNOWN,
    };
    for (int i = 0; i < TERN_ADDRESS_LEN; i++) {
        n.address[i] = (uint8_t)(0x48 + i * 7);
    }
    return n;
}

/* A board in Europe a while later: two neighbours, a message in from Bob, one of its own on its
 * way, and a little of the hour's air used. */
static struct ui_node busy(void) {
    struct ui_node n = alone();
    n.region = "EU868";
    n.nearby = 2;
    n.reachable = 3;
    n.limited = true;
    n.air_used_ms = 12345;
    n.air_limit_ms = 360000;
    n.window_s = 3600;
    n.messages = 3;
    n.unread = 2;
    n.waiting = 1;
    snprintf(n.from, sizeof n.from, "Bob");
    n.message = (struct ui_message){
        .received = true,
        .unread = true,
        .aged = true,
        .ago_s = 125,
        .state = TERN_C_RECEIVED,
    };
    snprintf(n.message.who, sizeof n.message.who, "Bob");
    static const char text[] = "On the ridge by six. Bring the long antenna and a spare battery.";
    n.message.text_len = sizeof text - 1;
    memcpy(n.message.text, text, sizeof text - 1);
    return n;
}

static void home_says_it_is_listening(void) {
    struct ui_node n = alone();
    draw(&n, UI_HOME, "home-listening");
    CHECK_ROW(0, "Tern            US915");
    CHECK(big(2, "Listening"));
    CHECK_ROW(4, "");
    CHECK_ROW(5, "No nodes heard yet");
    CHECK_ROW(6, "No new messages");
    CHECK_ROW(7, "Air: no limit");

    n.nearby = 4;
    n.reachable = 9;
    draw(&n, UI_HOME, "home-nearby");
    CHECK(big(2, "4 nearby"));
    CHECK_ROW(5, "Reaches 9 nodes"); /* not "4 nearby" again */

    n.waiting = 1;
    draw(&n, UI_HOME, "home-waiting");
    CHECK_ROW(6, "1 awaiting delivery");

    n.bench = true;
    draw(&n, UI_HOME, "home-bench");
    CHECK(big(2, "Bench mode"));
}

static void home_puts_new_messages_first(void) {
    struct ui_node n = busy();
    draw(&n, UI_HOME, "home-news");
    CHECK_ROW(0, "Tern            EU868");
    CHECK(big(2, "2 new"));
    CHECK_ROW(5, "2 nearby, reaches 3");
    CHECK_ROW(6, "Last from Bob");
    CHECK_ROW(7, "Air: 96% left");
}

static void the_air_says_when_it_is_full(void) {
    struct ui_node n = busy();
    n.air_used_ms = n.air_limit_ms;
    n.wait_ms = 239001; /* rounded up: it never says the air is free sooner than it is */
    draw(&n, UI_HOME, "home-air-full");
    CHECK_ROW(7, "Air: full for 4m");
    draw(&n, UI_AIR, "air-full");
    CHECK_ROW(1, "Used 360.0 of 360 s");
    CHECK_ROW(6, "Next send in 4m");

    n = busy();
    draw(&n, UI_AIR, "air-limited");
    CHECK_ROW(0, "Air             EU868");
    CHECK_ROW(1, "Used 12.3 of 360 s");
    CHECK_ROW(2, "in any hour");
    CHECK_ROW(6, "Free to send now");
    CHECK_ROW(7, "EU868 allows 10%");

    n = alone();
    n.air_total_ms = 1234;
    draw(&n, UI_AIR, "air-unlimited");
    CHECK_ROW(2, "US915 sets no limit o");
    CHECK_ROW(5, "Sent for 1.2 s");
}

static void the_bar_shows_what_is_used(void) {
    display_init(&d);
    display_bar(&d, 4, 0, 100);
    CHECK_EQ_I64(d.px[4][2], 0x7E); /* the outline's ends */
    CHECK_EQ_I64(d.px[4][125], 0x7E);
    CHECK_EQ_I64(d.px[4][3], 0x42); /* nothing used: only its top and bottom */
    CHECK_EQ_I64(d.px[4][0], 0x00);
    display_bar(&d, 4, 1, 1000000); /* a little always shows */
    CHECK_EQ_I64(d.px[4][3], 0x7E);
    CHECK_EQ_I64(d.px[4][4], 0x42);
    display_bar(&d, 4, 50, 100);
    CHECK_EQ_I64(d.px[4][63], 0x7E);
    CHECK_EQ_I64(d.px[4][65], 0x42);
    display_bar(&d, 4, INT64_MAX, INT64_MAX - 1); /* past the whole: full, and no overflow */
    CHECK_EQ_I64(d.px[4][124], 0x7E);
    display_bar(&d, 4, INT64_MAX / 2, INT64_MAX);
    CHECK_EQ_I64(d.px[4][63], 0x7E);
    CHECK_EQ_I64(d.px[4][65], 0x42);
}

static void home_shows_a_phone_connected(void) {
    struct ui_node n = alone();
    n.phone = true;
    draw(&n, UI_HOME, "home-phone");
    CHECK_ROW(0, "Tern " DISPLAY_BLUETOOTH_S "          US915");
    n.battery = 100;
    n.charging = true;
    draw(&n, UI_HOME, "x"); /* the longest the right of it gets: the rune still fits */
    CHECK_ROW(0, "Tern " DISPLAY_BLUETOOTH_S " Chg 100% US915");
}

static void a_group_message_names_the_group_and_its_writer(void) {
    struct ui_node n = busy();
    n.from_group = true;
    snprintf(n.from, sizeof n.from, "Hikers");
    n.message.group = true;
    snprintf(n.message.who, sizeof n.message.who, "Hikers");
    snprintf(n.message.writer, sizeof n.message.writer, "Bob");
    draw(&n, UI_MESSAGES, "message-group");
    CHECK_ROW(1, "New in Hikers  2m ago");
    CHECK_ROW(2, "From Bob");
    CHECK_ROW(3, "On the ridge by six.");
    CHECK_ROW(4, "Bring the long");
    n.message.unread = false;
    snprintf(n.message.writer, sizeof n.message.writer, "node 1A2B3C4D");
    draw(&n, UI_MESSAGES, "x");
    CHECK_ROW(1, "In Hikers      2m ago");
    CHECK_ROW(2, "From node 1A2B3C4D");
    draw(&n, UI_HOME, "home-group");
    CHECK_ROW(6, "Last in Hikers");

    /* One this board wrote to the group: to it, with what became of it. */
    n.message.received = false;
    n.message.state = TERN_C_SENT;
    draw(&n, UI_MESSAGES, "x");
    CHECK_ROW(1, "To Hikers      2m ago");
    CHECK_ROW(2, "On the ridge by six.");
    CHECK_ROW(7, "Sent");
}

static void nearby_lists_who_was_heard(void) {
    struct ui_node n = busy();
    n.nearby_n = 2;
    n.neighbour[0] = (struct ui_neighbour){.snr_db = 7, .ago_s = 40};
    snprintf(n.neighbour[0].name, sizeof n.neighbour[0].name, "Bob");
    n.neighbour[1] = (struct ui_neighbour){.snr_db = -12, .ago_s = 750};
    snprintf(n.neighbour[1].name, sizeof n.neighbour[1].name, "1A2B3C4D");
    draw(&n, UI_NEARBY, "nearby");
    CHECK_ROW(0, "Nearby              2");
    CHECK_ROW(1, "Bob          +7dB 40s");
    CHECK_ROW(2, "1A2B3C4D    -12dB 12m");
    CHECK_ROW(3, "");

    /* More than fit: the heading says which these are, and a long name gives way to the signal. */
    n.nearby = 12;
    n.nearby_first = 7;
    n.nearby_n = 5;
    for (int i = 2; i < 5; i++) {
        n.neighbour[i] = (struct ui_neighbour){.snr_db = (int8_t)-i, .ago_s = 3600u * (unsigned)i};
        snprintf(n.neighbour[i].name, sizeof n.neighbour[i].name, "Camp %d", i - 1);
    }
    snprintf(n.neighbour[0].name, sizeof n.neighbour[0].name, "Bob on the ridge top");
    draw(&n, UI_NEARBY, "nearby-more");
    CHECK_ROW(0, "Nearby        8-12/12");
    CHECK_ROW(1, "Bob on the r +7dB 40s");
}

static void nearby_with_no_one_says_so(void) {
    struct ui_node n = alone();
    draw(&n, UI_NEARBY, "nearby-none");
    CHECK_ROW(0, "Nearby");
    CHECK_ROW(3, "No nodes heard yet");
    CHECK_ROW(5, "A node shows here");
}

static void a_message_is_shown_whole(void) {
    struct ui_node n = busy();
    draw(&n, UI_MESSAGES, "message-received");
    CHECK_ROW(0, "Messages          1/3");
    CHECK_ROW(1, "New from Bob   2m ago");
    CHECK_ROW(2, "On the ridge by six.");
    CHECK_ROW(3, "Bring the long");
    CHECK_ROW(4, "antenna and a spare");
    CHECK_ROW(5, "battery.");
    CHECK_ROW(6, "");

    n.message.unread = false;
    n.message.ago_s = 30;
    draw(&n, UI_MESSAGES, "message-read");
    CHECK_ROW(1, "From Bob          now");

    n.message.aged = false; /* the board's clock was never set */
    draw(&n, UI_MESSAGES, "message-no-clock");
    CHECK_ROW(1, "From Bob");
}

static void text_too_long_is_cut_where_it_says_so(void) {
    struct ui_node n = busy();
    memset(n.message.text, 'x', sizeof n.message.text);
    n.message.text_len = sizeof n.message.text;
    draw(&n, UI_MESSAGES, "message-long");
    CHECK_ROW(2, "xxxxxxxxxxxxxxxxxxxxx");
    CHECK_ROW(7, "xxxxxxxxxxxxxxxxxx...");

    /* Characters the font has not are one '?' each, however many bytes they take. */
    static const char text[] = "caf\xC3\xA9 \xF0\x9F\x93\xA1\nok";
    n.message.text_len = sizeof text - 1;
    memcpy(n.message.text, text, sizeof text - 1);
    snprintf(n.message.who, sizeof n.message.who, "Zo\xC3\xAB");
    draw(&n, UI_MESSAGES, "message-utf8");
    CHECK_ROW(1, "New from Zo?   2m ago");
    CHECK_ROW(2, "caf? ? ok");
}

static void a_message_sent_says_what_became_of_it(void) {
    static const struct {
        uint8_t state, reason;
        uint16_t wait_s;
        const char *says;
    } cases[] = {
        {TERN_C_WAITING, TERN_C_WAIT_UNNAMED, 0, "Waiting"},
        {TERN_C_WAITING, TERN_C_WAIT_ROUTE, 0, "Waiting: no route yet"},
        {TERN_C_WAITING, TERN_C_WAIT_SESSION, 0, "Making contact..."},
        {TERN_C_WAITING, TERN_C_WAIT_REGION, 0, "Waiting: air limit"},
        {TERN_C_WAITING, TERN_C_WAIT_REGION, 300, "Air limit, 5m wait"},
        {TERN_C_WAITING, TERN_C_WAIT_BUDGET, 0, "Waiting: air budget"},
        {TERN_C_WAITING, TERN_C_WAIT_RADIO, 0, "Waiting: radio busy"},
        {TERN_C_SENT, 0, 0, "Sent"},
        {TERN_C_DELIVERED, 0, 0, "Delivered"},
        {TERN_C_NOT_DELIVERED, 0, 0, "Not delivered"},
    };
    struct ui_node n = busy();
    n.message.received = false;
    n.message.unread = false;
    n.message.ago_s = 7200;
    snprintf(n.message.who, sizeof n.message.who, "Alice the climber");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        n.message.state = cases[i].state;
        n.message.reason = cases[i].reason;
        n.message.wait_s = cases[i].wait_s;
        draw(&n, UI_MESSAGES, i == 1 ? "message-no-route" : i == 8 ? "message-delivered" : "x");
        CHECK_ROW(1, "To Alice the c 2h ago"); /* a name gives way to the time */
        CHECK_ROW(7, cases[i].says);
    }
    /* Its text stops a row early, to leave room. */
    CHECK_ROW(6, "");
}

static void no_messages_says_where_they_come_from(void) {
    struct ui_node n = alone();
    draw(&n, UI_MESSAGES, "messages-none");
    CHECK_ROW(0, "Messages");
    CHECK_ROW(3, "No messages yet");
    CHECK_ROW(5, "Send one from your");
    CHECK_ROW(6, "phone or computer");
}

static void the_node_page_gives_its_address(void) {
    struct ui_node n = alone();
    for (int i = 0; i < TERN_ADDRESS_LEN; i++) {
        n.address[i] = (uint8_t)(i % 16 * 0x11);
    }
    n.address[31] = 0xAB;
    draw(&n, UI_NODE, "node");
    CHECK_ROW(0, "This node");
    CHECK_ROW(2, "  00112233 44556677");
    CHECK_ROW(3, "  8899AABB CCDDEEFF");
    CHECK_ROW(4, "  00112233 44556677");
    CHECK_ROW(5, "  8899AABB CCDDEEAB");
    CHECK_ROW(6, "Relay, US915, +2 dBm");
    CHECK_ROW(7, "Tern 0.1.0-alpha.3");
}

/* The first address in the specification's sharing vectors, and the short code they give it. */
static void the_node_page_gives_its_short_code(void) {
    static const uint8_t address[TERN_ADDRESS_LEN] = {
        0xd6, 0xd1, 0x5f, 0xab, 0xbc, 0x42, 0xce, 0x56, 0x17, 0x4a, 0x43,
        0x63, 0xe7, 0x57, 0x43, 0x7a, 0x4a, 0x7b, 0xaf, 0x42, 0x1b, 0x69,
        0x0c, 0xaa, 0x24, 0x67, 0x6f, 0x3f, 0x4f, 0x17, 0xc9, 0x96};
    struct ui_node n = alone();
    memcpy(n.address, address, sizeof address);
    draw(&n, UI_NODE, "node-code");
    CHECK_ROW(1, "Code 5358 3737 3382");
    CHECK_ROW(2, "  D6D15FAB BC42CE56");
    CHECK_ROW(5, "  24676F3F 4F17C996");
    draw(&n, UI_SHARE, "share-code");
}

static bool lit(int x, int y) { return (d.px[y / 8][x] >> (y % 8) & 1) != 0; }

/* The code is the address in the digits the next page shows, dark on light: each module two pixels
 * square, unlit where it is dark, in a lit block with a margin round it. */
static void the_share_page_is_the_address_as_a_code(void) {
    struct ui_node n = alone();
    struct qr want;
    draw(&n, UI_SHARE, "share");
    char text[TERN_ADDRESS_LINK_LEN + 1];
    tern_address_link(n.address,
                      text); /* src/share.c, checked against the specification's vectors */
    CHECK(qr_encode(&want, text, QR_MASK_BEST));
    int wrong = 0;
    for (int my = 0; my < QR_SIZE; my++) {
        for (int mx = 0; mx < QR_SIZE; mx++) {
            for (int k = 0; k < 4; k++) {
                int x = 6 + 2 * mx + k % 2, y = 3 + 2 * my + k / 2;
                wrong += lit(x, y) == qr_dark(&want, mx, my);
            }
        }
    }
    CHECK_EQ_I64(wrong, 0);
    for (int y = 0; y < 64; y++) { /* the margin: lit, left and right of the code */
        CHECK(lit(0, y) && lit(5, y) && lit(64, y) && lit(69, y));
        CHECK(!lit(70, y));
    }
    for (int x = 0; x < 70; x++) {
        CHECK(lit(x, 0) && lit(x, 2) && lit(x, 61) && lit(x, 63));
    }
}

static void the_battery_shows_once_it_is_known(void) {
    struct ui_node n = alone();
    n.battery = 87;
    draw(&n, UI_HOME, "home-battery");
    CHECK_ROW(0, "Tern        87% US915");
    n.battery = 100;
    draw(&n, UI_HOME, "x");
    CHECK_ROW(0, "Tern       100% US915");
    n.battery = 8;
    draw(&n, UI_HOME, "home-battery-low");
    CHECK_ROW(0, "Tern     Low 8% US915");
    n.charging = true; /* charging says so, not that it is low */
    draw(&n, UI_HOME, "home-charging");
    CHECK_ROW(0, "Tern     Chg 8% US915");
    n.battery = 100;
    draw(&n, UI_HOME, "x");
    CHECK_ROW(0, "Tern   Chg 100% US915");
}

static void holding_prg_counts_down_to_off(void) {
    display_init(&d);
    ui_turning_off(3, &d);
    picture("turning-off");
    CHECK_ROW(0, "Turning off");
    CHECK(big(2, "3"));
    CHECK_ROW(5, "Keep holding to turn");
    CHECK_ROW(6, "off. Let go: stays on");
    CHECK_ROW(7, "Its messages are lost");
}

static void off_says_how_to_turn_it_on(void) {
    display_init(&d);
    ui_off(UI_OFF_PRESSED, &d);
    picture("off");
    CHECK_ROW(0, "Turned off");
    CHECK_ROW(3, "Press PRG to turn");
    display_init(&d);
    ui_off(UI_OFF_EMPTY, &d);
    picture("off-empty");
    CHECK_ROW(0, "Battery empty");
    CHECK_ROW(5, "Charge it: it turns");
    CHECK_ROW(6, "on again by itself,");
    CHECK_ROW(7, "or press PRG.");
}

static void pairing_shows_the_passkey(void) {
    display_init(&d);
    ui_pairing(4213, &d);
    picture("pairing");
    CHECK_ROW(0, "Bluetooth pairing");
    CHECK_ROW(2, "Type this passkey");
    CHECK(big(5, "004213"));
}

static void the_boot_screen_fills_in_as_the_board_starts(void) {
    struct ui_node n = alone();
    struct ui_start s = {.version = "0.1.0-alpha.3"};
    display_init(&d);
    ui_boot(&s, &d);
    picture("boot-first");
    CHECK(big(0, "Tern"));
    CHECK_ROW(3, "Version 0.1.0-alpha.3");
    CHECK_ROW(5, "Starting...");
    CHECK_ROW(6, "");
    s.region = "US915";
    s.address = n.address;
    ui_boot(&s, &d);
    picture("boot");
    char sc[TERN_SHORT_CODE_LEN + 1], want[COLS + 1];
    tern_short_code(n.address, sc);
    snprintf(want, sizeof want, "Code %s", sc);
    CHECK_ROW(5, "US915, starting...");
    CHECK_ROW(6, want);
    CHECK_ROW(7, "");
    s.new_address = true;
    ui_boot(&s, &d);
    CHECK_ROW(7, "New address made");
}

static void a_fault_says_why_and_what_to_do(void) {
    struct ui_start s = {.version = "0.1.0-alpha.3"};
    display_init(&d);
    ui_fault(UI_FAULT_RADIO, &s, -5, &d);
    picture("fault-radio");
    CHECK_ROW(0, "Did not start");
    CHECK_ROW(1, "The radio did not");
    CHECK_ROW(4, "Error -5");
    CHECK_ROW(5, "Restart it. If this");
    CHECK_ROW(7, "Tern 0.1.0-alpha.3");
    ui_fault(UI_FAULT_STORAGE, &s, 0, &d);
    picture("fault-storage");
    CHECK_ROW(4, "");
    CHECK_ROW(6, "it gets a new address");
    /* Every fault's words fit their lines, a version as long as a release's included. */
    s.version = "1.20.30-alpha.40";
    for (int why = UI_FAULT_STORAGE; why <= UI_FAULT_RADIO; why++) {
        ui_fault((enum ui_fault)why, &s, -32768, &d);
        for (int r = 0; r < DISPLAY_PAGES; r++) {
            CHECK(strchr(row(r), '#') == NULL);
        }
        CHECK_ROW(7, "Tern 1.20.30-alpha.40");
        CHECK(strlen(row(1)) > 0 && strlen(row(5)) > 0);
    }
}

static void large_text_is_centred_and_cut(void) {
    display_init(&d);
    display_big(&d, 2, "A");
    /* One character, ten pixels wide, in the middle of 128: columns 59 to 68. */
    CHECK_EQ_I64(d.px[2][58], 0x00);
    CHECK(d.px[2][59] != 0 || d.px[3][59] != 0);
    CHECK_EQ_I64(d.px[2][69], 0x00);
    display_big(&d, 2, "0123456789ABC");
    CHECK(big(2, "0123456789"));
    display_big(&d, 7, "x"); /* no room for its lower half: not drawn */
    CHECK_EQ_I64(d.px[7][64], 0x00);
}

static void huge_numbers_stay_on_the_screen(void) {
    struct ui_node n = busy();
    n.nearby = n.reachable = UINT16_MAX;
    n.unread = n.waiting = n.messages = n.shown = UINT8_MAX;
    n.air_used_ms = n.air_limit_ms = n.air_total_ms = INT64_MAX;
    n.wait_ms = UINT32_MAX;
    n.window_s = UINT32_MAX;
    n.dbm = INT8_MIN;
    n.region = "A REGION WITH A VERY LONG NAME";
    n.message.ago_s = UINT32_MAX;
    n.message.wait_s = UINT16_MAX;
    n.message.text_len = UINT8_MAX; /* more than a message holds */
    n.nearby_first = UINT16_MAX;
    n.nearby_n = UINT8_MAX; /* more than the page has room for */
    for (int i = 0; i < UI_NEARBY_ROWS; i++) {
        n.neighbour[i] = (struct ui_neighbour){.snr_db = INT8_MIN, .ago_s = UINT32_MAX};
        memset(n.neighbour[i].name, 'W', UI_NAME); /* and no NUL */
    }
    for (int page = 0; page < UI_PAGES; page++) {
        draw(&n, page, "x");
        /* Every cell of every line is a character, so nothing ran off the edge or over another;
         * the rows the large line and the bar take are not text. */
        for (int r = 0; r < DISPLAY_PAGES; r++) {
            bool text = !(page == UI_HOME && (r == 2 || r == 3)) && !(page == UI_AIR && r == 4) &&
                        page != UI_SHARE;
            CHECK(!text || strchr(row(r), '#') == NULL);
        }
    }
    n.air_total_ms = -1;
    n.air_used_ms = -1;
    n.limited = false;
    draw(&n, UI_AIR, "x");
    CHECK_ROW(5, "Sent for 0.0 s");
}

static void an_unchanged_page_is_not_sent_again(void) {
    struct ui_node n = busy();
    for (int page = 0; page < UI_PAGES; page++) {
        display_init(&d);
        ui_draw(&n, page, &d);
        while (display_take(&d) >= 0) {
        }
        ui_draw(&n, page, &d);
        CHECK_EQ_I64(display_take(&d), -1);
    }
    ui_draw(&n, UI_HOME, &d);
    while (display_take(&d) >= 0) {
    }
    n.unread = 3;
    ui_draw(&n, UI_HOME, &d);
    CHECK_EQ_I64(display_take(&d), 2); /* the large line, and nothing else */
    CHECK_EQ_I64(display_take(&d), 3);
    CHECK_EQ_I64(display_take(&d), -1);
}

int main(int argc, char **argv) {
    pictures = argc > 1 ? argv[1] : NULL;
    RUN(home_says_it_is_listening);
    RUN(home_puts_new_messages_first);
    RUN(the_air_says_when_it_is_full);
    RUN(the_bar_shows_what_is_used);
    RUN(a_message_is_shown_whole);
    RUN(nearby_lists_who_was_heard);
    RUN(nearby_with_no_one_says_so);
    RUN(home_shows_a_phone_connected);
    RUN(a_group_message_names_the_group_and_its_writer);
    RUN(text_too_long_is_cut_where_it_says_so);
    RUN(a_message_sent_says_what_became_of_it);
    RUN(no_messages_says_where_they_come_from);
    RUN(the_node_page_gives_its_address);
    RUN(the_share_page_is_the_address_as_a_code);
    RUN(the_node_page_gives_its_short_code);
    RUN(the_battery_shows_once_it_is_known);
    RUN(pairing_shows_the_passkey);
    RUN(holding_prg_counts_down_to_off);
    RUN(off_says_how_to_turn_it_on);
    RUN(the_boot_screen_fills_in_as_the_board_starts);
    RUN(a_fault_says_why_and_what_to_do);
    RUN(large_text_is_centred_and_cut);
    RUN(huge_numbers_stay_on_the_screen);
    RUN(an_unchanged_page_is_not_sent_again);
    return CHECK_DONE();
}

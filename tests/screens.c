#include "colour.h"
#include "scene.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

/* Every page and screen of the node, drawn as a colour panel draws it (ports/node/colour.c) and
 * as a monochrome one does (scene.c), with the nodes tests/ui.c uses and a few more.
 *
 * Checked: that each draws, band by band, the same picture as when drawn whole, so a board that
 * sends a band at a time shows what was meant. Given a directory, it also writes each picture there
 * for a person to look at, `<name>.ppm` in colour and `<name>.pbm` in monochrome:
 * `test_screens pictures/`. */

#define W 240
#define H 135
#define BAND 15

static const char *pictures;
static uint16_t whole[W * H], banded[W * H];
static struct display d;
static struct scene sc;

static void write_ppm(const char *name, const uint16_t *px) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", pictures, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t c = px[i];
        unsigned r = c >> 11, g = (c >> 5) & 0x3F, b = c & 0x1F;
        fputc((int)(r << 3 | r >> 2), f);
        fputc((int)(g << 2 | g >> 4), f);
        fputc((int)(b << 3 | b >> 2), f);
    }
    fclose(f);
}

static void write_pbm(const char *name) {
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

/* Draws the scene `sc` both ways, checks that a band at a time is the same as all at once, and
 * keeps the pictures. */
static void show(const char *name) {
    struct canvas all = {.px = whole, .w = W, .h = H, .y0 = 0, .rows = H};
    colour_draw(&sc, &all);
    for (int y = 0; y < H; y += BAND) {
        struct canvas band = {.px = &banded[y * W], .w = W, .h = H, .y0 = y, .rows = BAND};
        colour_draw(&sc, &band);
    }
    if (memcmp(whole, banded, sizeof whole) != 0) {
        fprintf(stderr, "%s: drawn a band at a time, it is not the same picture\n", name);
        check_failures++;
    }
    display_init(&d);
    mono_draw(&sc, &d);
    if (pictures != NULL) {
        write_ppm(name, whole);
        write_pbm(name);
    }
    colour_forget();
}

static void page(const struct ui_node *n, int p, const char *name) {
    ui_scene(n, p, &sc);
    show(name);
}

static struct ui_node alone(void) {
    struct ui_node n = {
        .region = "US915",
        .version = "0.5.0-alpha.3",
        .relay = false,
        .dbm = 22,
        .battery = UI_BATTERY_UNKNOWN,
        .button = "USER",
        .bluetooth = true,
        .ble_name = "Tern 7KQ3",
        .menu = -1,
    };
    for (int i = 0; i < TERN_ADDRESS_LEN; i++) {
        n.address[i] = (uint8_t)(0x48 + i * 7);
    }
    return n;
}

static struct ui_node busy(void) {
    struct ui_node n = alone();
    n.battery = 76;
    n.phone = true;
    n.nearby = 5;
    n.reachable = 9;
    n.limited = false;
    n.air_total_ms = 4300;
    n.messages = 3;
    n.unread = 2;
    n.waiting = 1;
    n.paired = 2;
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
    static const char *const names[] = {"Bob", "Alice's truck", "1A2B3C4D", "Ridge relay",
                                        "9F00E1C2"};
    static const int8_t snr[] = {9, 2, -6, -12, -15};
    static const uint32_t ago[] = {40, 95, 720, 3000, 7200};
    n.nearby_n = UI_NEARBY_ROWS_COLOUR;
    for (int i = 0; i < n.nearby_n; i++) {
        snprintf(n.neighbour[i].name, sizeof n.neighbour[i].name, "%s", names[i]);
        n.neighbour[i].snr_db = snr[i];
        n.neighbour[i].ago_s = ago[i];
    }
    n.groups = 2;
    n.group_shown = 1;
    snprintf(n.group, sizeof n.group, "Ridge walkers");
    return n;
}

static void every_page(void) {
    struct ui_node n = alone();
    page(&n, UI_HOME, "home-alone");
    page(&n, UI_MESSAGES, "messages-none");
    page(&n, UI_NEARBY, "nearby-none");
    page(&n, UI_GROUPS, "groups-none");
    page(&n, UI_PHONES, "phones-none");

    n = busy();
    page(&n, UI_HOME, "home");
    page(&n, UI_MESSAGES, "messages");
    page(&n, UI_NEARBY, "nearby");
    page(&n, UI_SHARE, "share");
    page(&n, UI_MORE, "more");
    n.menu = 1;
    page(&n, UI_MORE, "more-open");
    n.menu = -1;
    page(&n, UI_AIR, "air-free");
    page(&n, UI_GROUPS, "groups");
    page(&n, UI_NODE, "node");
    page(&n, UI_PHONES, "phones");
    page(&n, UI_RESET, "reset");
    n.confirm_s = 8;
    page(&n, UI_RESET, "reset-confirm");
    page(&n, UI_PHONES, "phones-confirm");
    n.confirm_s = 0;

    uint8_t secret[TERN_GROUP_SECRET];
    for (int i = 0; i < TERN_GROUP_SECRET; i++) {
        secret[i] = (uint8_t)(i * 13 + 5);
    }
    tern_group_link(secret, (const uint8_t *)"Ridge walkers",
                    ui_join_name((const uint8_t *)"Ridge walkers", 13), n.join_link);
    page(&n, UI_GROUPS, "groups-code");
    n.join_link[0] = '\0';

    n.region = "EU868";
    n.limited = true;
    n.air_used_ms = 12345;
    n.air_limit_ms = 360000;
    n.window_s = 3600;
    page(&n, UI_AIR, "air");
    n.air_used_ms = 340000;
    n.wait_ms = 95000;
    page(&n, UI_AIR, "air-full");
    page(&n, UI_HOME, "home-air-full");

    n.unread = 0;
    n.message.received = false;
    n.message.unread = false;
    n.message.state = TERN_C_WAITING;
    n.message.reason = TERN_C_WAIT_ROUTE;
    snprintf(n.message.who, sizeof n.message.who, "Alice the climber");
    page(&n, UI_MESSAGES, "message-waiting");
    n.message.state = TERN_C_DELIVERED;
    page(&n, UI_MESSAGES, "message-delivered");
    n.message.state = TERN_C_NOT_DELIVERED;
    page(&n, UI_MESSAGES, "message-failed");
    n.message.received = true;
    n.message.group = true;
    n.message.state = TERN_C_RECEIVED;
    snprintf(n.message.who, sizeof n.message.who, "Ridge walkers");
    snprintf(n.message.writer, sizeof n.message.writer, "José");
    static const char short_text[] = "Café at the top \xE2\x80\x94 who's in?";
    n.message.text_len = sizeof short_text - 1;
    memcpy(n.message.text, short_text, sizeof short_text - 1);
    page(&n, UI_MESSAGES, "message-group");

    /* With a keyboard: the page says typing replies, and while a reply is written, shows it. */
    n.keyboard = true;
    page(&n, UI_MESSAGES, "message-keyboard");
    CHECK(strcmp(sc.hint, "Type: reply  Enter: new") == 0);
    n.composing = true;
    n.to_group = true;
    snprintf(n.to, sizeof n.to, "Ridge walkers");
    static const char reply[] = "On my way, ten minutes out";
    n.draft_len = sizeof reply - 1;
    memcpy(n.draft, reply, sizeof reply - 1);
    page(&n, UI_MESSAGES, "compose");
    CHECK(strcmp(sc.text, "On my way, ten minutes out_") == 0);
    CHECK(strcmp(sc.title_right, "26/128") == 0);
    CHECK(strcmp(sc.hint, "Enter: send") == 0);
    n.draft_len = TERN_COMPANION_TEXT_MAX;
    memset(n.draft, 'w', sizeof n.draft);
    page(&n, UI_MESSAGES, "compose-full");
    CHECK(strlen(sc.text) == TERN_COMPANION_TEXT_MAX + 1);
    n.draft_len = 0;
    n.to_group = false;
    snprintf(n.to, sizeof n.to, "Bob");
    n.draft_error = "no room";
    page(&n, UI_MESSAGES, "compose-not-sent");
    CHECK(strcmp(sc.text, "_") == 0);
    n.composing = false;
    n.draft_error = NULL;
    n.picking = true;
    page(&n, UI_MESSAGES, "pick-none");
    static const char *const whom[] = {"Alice the climber", "Bob", "Ridge walkers", "Carol"};
    n.pick_n = 6;
    n.pick_first = 2;
    n.pick_rows = UI_PICK_ROWS;
    n.pick_chosen = 4;
    for (int i = 0; i < UI_PICK_ROWS; i++) {
        snprintf(n.pick[i].name, sizeof n.pick[i].name, "%s", whom[i]);
        n.pick[i].group = i == 2;
    }
    page(&n, UI_MESSAGES, "pick");
    CHECK(strcmp(sc.title_right, "5/6") == 0);
    CHECK(sc.n == UI_PICK_ROWS && !sc.item[1].selected && sc.item[2].selected);
    n.picking = false;
    n.keyboard = false;

    n.battery = 8;
    n.wait_ms = 0;
    n.air_used_ms = 1000;
    page(&n, UI_HOME, "home-battery-low");
    n.battery = 54;
    n.charging = true;
    n.unread = 0;
    n.waiting = 0;
    page(&n, UI_HOME, "home-charging");
}

static void the_other_screens(void) {
    uint8_t address[TERN_ADDRESS_LEN];
    for (int i = 0; i < TERN_ADDRESS_LEN; i++) {
        address[i] = (uint8_t)(0x48 + i * 7);
    }
    struct ui_start st = {.version = "0.5.0-alpha.3"};
    ui_boot(&st, &sc);
    show("boot-first");
    st.region = "US915";
    st.address = address;
    st.new_address = true;
    ui_boot(&st, &sc);
    show("boot");
    ui_fault(UI_FAULT_RADIO, &st, -5, &sc);
    show("fault-radio");
    ui_pairing(123456, &sc);
    show("pairing");
    ui_updating(300000, 755200, &sc);
    show("updating");
    ui_bootloader(&sc);
    show("bootloader");
    ui_turning_off(3, &sc);
    show("turning-off");
    ui_off(UI_OFF_PRESSED, "USER", &sc);
    show("off");
    ui_erasing(&sc);
    show("erasing");
    static const char *const rows[] = {
        "Mesh 1/4",         "id 3af09d1b leaf",    "hears 2, routes 2", "sent 14 heard 22",
        "last 'ping 3' 2m", "-7 dB from 4d99b713", "air 2.3 s",         "SF9 500k 921.25"};
    ui_rows(rows, 8, &sc);
    show("bench");
}

int main(int argc, char **argv) {
    pictures = argc > 1 ? argv[1] : NULL;
    RUN(every_page);
    RUN(the_other_screens);
    return CHECK_DONE();
}

#include "boards.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

/* The ESP32 port's boards (ports/esp32/main/boards.c): that each is a board the build can choose,
 * that no pin does two jobs, and that a power asked for is never more at the antenna. */

static const char *port_dir; /* ports/esp32, from the command line */

static bool name_ok(const char *name) {
    if (name == NULL || name[0] == '\0' || strlen(name) > 15) {
        return false;
    }
    for (const char *c = name; *c != '\0'; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) {
            return false;
        }
    }
    return true;
}

static void names_are_file_names_and_unique(void) {
    for (size_t i = 0; board_defs[i] != NULL; i++) {
        CHECK(name_ok(board_defs[i]->name));
        CHECK(board_defs[i]->title != NULL);
        CHECK(board_def_named(board_defs[i]->name) == board_defs[i]);
        for (size_t j = 0; j < i; j++) {
            CHECK(strcmp(board_defs[i]->name, board_defs[j]->name) != 0);
        }
    }
    CHECK(board_def_named("heltec-v2") == NULL);
    CHECK(board_def_named(NULL) == NULL);
}

/* Every name Kconfig offers (TERN_BOARD_NAME) is a board here, and has its boards/<name>.defaults
 * choosing it: board.c finds its pins by that name, so a board missing here would not start. */
static void every_board_kconfig_offers_is_here(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/main/Kconfig.projbuild", port_dir);
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    char line[256];
    int offered = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char name[32], symbol[64];
        if (sscanf(line, " default \"%31[^\"]\" if TERN_BOARD_%63s", name, symbol) != 2) {
            continue;
        }
        offered++;
        const struct board_def *b = board_def_named(name);
        if (b == NULL) {
            fprintf(stderr, "Kconfig offers %s, which boards.c does not have\n", name);
        }
        CHECK(b != NULL);

        char defaults[512], want[96], got[256];
        snprintf(defaults, sizeof defaults, "%s/boards/%s.defaults", port_dir, name);
        snprintf(want, sizeof want, "CONFIG_TERN_BOARD_%s=y", symbol);
        FILE *d = fopen(defaults, "r");
        bool chooses = false;
        while (d != NULL && fgets(got, sizeof got, d) != NULL) {
            chooses = chooses || strncmp(got, want, strlen(want)) == 0;
        }
        if (!chooses) {
            fprintf(stderr, "%s does not say %s\n", defaults, want);
        }
        CHECK(chooses);
        if (d != NULL) {
            fclose(d);
        }
    }
    fclose(f);
    int known = 0;
    while (board_defs[known] != NULL) {
        known++;
    }
    CHECK_EQ_I64(offered, known); /* and no board here that a build cannot choose */
}

static void add_pin(int8_t *used, size_t *n, int8_t pin) {
    if (pin != BOARD_NO_PIN) {
        CHECK(pin >= 0 && pin <= 48); /* the ESP32-S3's GPIOs */
        used[(*n)++] = pin;
    }
}

static void no_pin_does_two_jobs(void) {
    for (size_t i = 0; board_defs[i] != NULL; i++) {
        const struct board_def *b = board_defs[i];
        int8_t used[32];
        size_t n = 0;
        add_pin(used, &n, b->lora.nss);
        add_pin(used, &n, b->lora.sck);
        add_pin(used, &n, b->lora.mosi);
        add_pin(used, &n, b->lora.miso);
        add_pin(used, &n, b->lora.reset);
        add_pin(used, &n, b->lora.busy);
        add_pin(used, &n, b->button);
        add_pin(used, &n, b->led);
        add_pin(used, &n, b->screen.sda);
        add_pin(used, &n, b->screen.scl);
        add_pin(used, &n, b->screen.reset);
        add_pin(used, &n, b->vext);
        add_pin(used, &n, b->battery.sense);
        add_pin(used, &n, b->battery.enable);
        add_pin(used, &n, b->amp.power);
        add_pin(used, &n, b->amp.enable);
        add_pin(used, &n, b->amp.tx[0]);
        add_pin(used, &n, b->amp.tx[1]);
        for (size_t x = 0; x < n; x++) {
            for (size_t y = 0; y < x; y++) {
                if (used[x] == used[y]) {
                    fprintf(stderr, "%s uses GPIO%d twice\n", b->name, used[x]);
                }
                CHECK(used[x] != used[y]);
            }
        }
        /* The radio is required; the button wakes the board, so is an RTC pin (GPIO0-21). */
        CHECK(b->lora.nss != BOARD_NO_PIN && b->lora.busy != BOARD_NO_PIN);
        CHECK(b->button >= 0 && b->button <= 21);
        CHECK(b->lora.tcxo_mv == 0 || (b->lora.tcxo_mv >= 1600 && b->lora.tcxo_mv <= 3300));
        CHECK(b->battery.sense == BOARD_NO_PIN || b->battery.bottom_k > 0);
    }
}

static void a_board_without_an_amplifier_is_its_chip(void) {
    const struct board_def *b = &board_heltec_v3;
    CHECK_EQ_I64(board_min_dbm(b), -9);
    CHECK_EQ_I64(board_max_dbm(b), 22);
    for (int dbm = -9; dbm <= 22; dbm++) {
        CHECK(board_gives(b, dbm));
        CHECK_EQ_I64(board_chip_dbm(b, (int8_t)dbm), dbm);
    }
    CHECK(!board_gives(b, -10));
    CHECK(!board_gives(b, 23));
}

/* With an amplifier, the chip is asked for the antenna's power less the gain taken, which is the
 * most the amplifier might give: so a power asked for is never exceeded. */
static void an_amplifier_is_taken_off_what_the_chip_is_asked(void) {
    const struct board_def *b = &board_heltec_v4;
    CHECK_EQ_I64(board_min_dbm(b), 1);
    CHECK_EQ_I64(board_max_dbm(b), 28);     /* Heltec's rating, not the chip's 22 and the gain */
    CHECK_EQ_I64(board_chip_dbm(b, 2), -8); /* the default, on a bench */
    CHECK_EQ_I64(board_chip_dbm(b, 28), 18);
    CHECK(!board_gives(b, 0));
    CHECK(!board_gives(b, 29));
    for (size_t i = 0; board_defs[i] != NULL; i++) {
        const struct board_def *d = board_defs[i];
        int gain = d->amp.power == BOARD_NO_PIN ? 0 : d->amp.gain_db;
        CHECK(board_min_dbm(d) <= board_max_dbm(d));
        for (int dbm = board_min_dbm(d); dbm <= board_max_dbm(d); dbm++) {
            int chip = board_chip_dbm(d, (int8_t)dbm);
            CHECK(chip >= -9 && chip <= 22);
            CHECK(chip + gain <= dbm);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_boards <ports/esp32>\n");
        return 2;
    }
    port_dir = argv[1];
    RUN(names_are_file_names_and_unique);
    RUN(every_board_kconfig_offers_is_here);
    RUN(no_pin_does_two_jobs);
    RUN(a_board_without_an_amplifier_is_its_chip);
    RUN(an_amplifier_is_taken_off_what_the_chip_is_asked);
    return CHECK_DONE();
}

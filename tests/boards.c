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
    CHECK(board_def_named("heltec-v1") == NULL);
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

        char defaults[512], want[96], got[256], target[32] = "esp32s3";
        snprintf(defaults, sizeof defaults, "%s/boards/%s.defaults", port_dir, name);
        snprintf(want, sizeof want, "CONFIG_TERN_BOARD_%s=y", symbol);
        FILE *d = fopen(defaults, "r");
        bool chooses = false;
        while (d != NULL && fgets(got, sizeof got, d) != NULL) {
            chooses = chooses || strncmp(got, want, strlen(want)) == 0;
            (void)sscanf(got, "CONFIG_IDF_TARGET=\"%31[^\"]\"", target);
        }
        if (!chooses) {
            fprintf(stderr, "%s does not say %s\n", defaults, want);
        }
        CHECK(chooses);
        /* The chip the build is for is the board's: build.sh and release.sh take it from there. */
        if (b != NULL) {
            const char *soc = b->soc == BOARD_ESP32 ? "esp32" : "esp32s3";
            if (strcmp(target, soc) != 0) {
                fprintf(stderr, "%s builds for %s, but %s is an %s\n", defaults, target, name, soc);
            }
            CHECK(strcmp(target, soc) == 0);
        }
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

/* The GPIOs each chip has, from its datasheet. The ESP32's 34 to 39 are inputs only, and its 6 to
 * 11 are its flash's. */
static bool gpio_ok(const struct board_def *b, int8_t pin) {
    if (b->soc == BOARD_ESP32) {
        return pin >= 0 && pin <= 39 && (pin < 6 || pin > 11) && pin != 20 && pin != 24 &&
               (pin < 28 || pin > 31);
    }
    return pin >= 0 && pin <= 48;
}

/* Whether a pin can drive: on the ESP32, not 34 to 39. */
static bool output_ok(const struct board_def *b, int8_t pin) {
    return pin == BOARD_NO_PIN || b->soc != BOARD_ESP32 || pin < 34;
}

/* Whether a pin can wake the chip from deep sleep: an RTC GPIO (each chip's datasheet). */
static bool rtc_ok(const struct board_def *b, int8_t pin) {
    if (b->soc == BOARD_ESP32) {
        static const int8_t rtc[] = {0,  2,  4,  12, 13, 14, 15, 25, 26,
                                     27, 32, 33, 34, 35, 36, 37, 38, 39};
        for (size_t i = 0; i < sizeof rtc; i++) {
            if (rtc[i] == pin) {
                return true;
            }
        }
        return false;
    }
    return pin >= 0 && pin <= 21;
}

static void add_pin(const struct board_def *b, int8_t *used, size_t *n, int8_t pin) {
    if (pin != BOARD_NO_PIN) {
        if (!gpio_ok(b, pin)) {
            fprintf(stderr, "%s has no GPIO%d to use\n", b->name, pin);
        }
        CHECK(gpio_ok(b, pin));
        used[(*n)++] = pin;
    }
}

static void no_pin_does_two_jobs(void) {
    for (size_t i = 0; board_defs[i] != NULL; i++) {
        const struct board_def *b = board_defs[i];
        int8_t used[32];
        size_t n = 0;
        add_pin(b, used, &n, b->lora.nss);
        add_pin(b, used, &n, b->lora.sck);
        add_pin(b, used, &n, b->lora.mosi);
        add_pin(b, used, &n, b->lora.miso);
        add_pin(b, used, &n, b->lora.reset);
        add_pin(b, used, &n, b->lora.busy);
        add_pin(b, used, &n, b->button);
        add_pin(b, used, &n, b->led);
        add_pin(b, used, &n, b->screen.sda);
        add_pin(b, used, &n, b->screen.scl);
        add_pin(b, used, &n, b->screen.reset);
        add_pin(b, used, &n, b->vext);
        add_pin(b, used, &n, b->battery.sense);
        add_pin(b, used, &n, b->battery.enable);
        add_pin(b, used, &n, b->amp.power);
        add_pin(b, used, &n, b->amp.enable);
        add_pin(b, used, &n, b->amp.tx[0]);
        add_pin(b, used, &n, b->amp.tx[1]);
        for (size_t x = 0; x < n; x++) {
            for (size_t y = 0; y < x; y++) {
                if (used[x] == used[y]) {
                    fprintf(stderr, "%s uses GPIO%d twice\n", b->name, used[x]);
                }
                CHECK(used[x] != used[y]);
            }
        }
        /* What the board drives can drive. */
        const int8_t outputs[] = {b->lora.nss,  b->lora.sck,   b->lora.mosi,    b->lora.reset,
                                  b->led,       b->vext,       b->screen.reset, b->battery.enable,
                                  b->amp.power, b->amp.enable, b->amp.tx[0],    b->amp.tx[1]};
        for (size_t o = 0; o < sizeof outputs; o++) {
            if (!output_ok(b, outputs[o])) {
                fprintf(stderr, "%s drives GPIO%d, which is an input\n", b->name, outputs[o]);
            }
            CHECK(output_ok(b, outputs[o]));
        }
        /* The radio is required, and BUSY is the SX1262's; the button wakes the board, so is an
         * RTC pin. */
        CHECK(b->lora.nss != BOARD_NO_PIN && b->lora.reset != BOARD_NO_PIN);
        if (b->lora.chip == BOARD_SX1262) {
            CHECK(b->lora.busy != BOARD_NO_PIN && !b->lora.pa_boost);
            CHECK(b->lora.tcxo_mv == 0 || (b->lora.tcxo_mv >= 1600 && b->lora.tcxo_mv <= 3300));
        } else {
            CHECK(b->lora.busy == BOARD_NO_PIN && !b->lora.dio2_rf_switch);
        }
        CHECK(b->button == BOARD_NO_PIN || rtc_ok(b, b->button));
        CHECK(!b->vext_always || b->vext != BOARD_NO_PIN);
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

/* An SX1276 on PA_BOOST gives +2 to +17 dBm, and this driver no more (tern/sx127x.h), whatever the
 * board is rated: the Heltec V2 is rated 19. */
static void an_sx1276_on_pa_boost_gives_2_to_17(void) {
    const struct board_def *b = &board_heltec_v2;
    CHECK_EQ_I64(board_min_dbm(b), 2);
    CHECK_EQ_I64(board_max_dbm(b), 17);
    CHECK(!board_gives(b, 1));
    CHECK(!board_gives(b, 18));
    CHECK_EQ_I64(board_chip_dbm(b, 10), 10);
    struct board_def rfo = board_heltec_v2;
    rfo.lora.pa_boost = false;
    CHECK_EQ_I64(board_min_dbm(&rfo), 0);
    CHECK_EQ_I64(board_max_dbm(&rfo), 14);
}

/* With an amplifier, the chip is asked for the antenna's power less the gain taken, which is the
 * most the amplifier might give: so a power asked for is never exceeded. */
static void an_amplifier_is_taken_off_what_the_chip_is_asked(void) {
    const struct board_def *b = &board_heltec_v4;
    CHECK_EQ_I64(board_min_dbm(b), 4);
    CHECK_EQ_I64(board_max_dbm(b), 28);     /* Heltec's rating, not the chip's 22 and the gain */
    CHECK_EQ_I64(board_chip_dbm(b, 4), -9); /* its least, the default on a bench */
    CHECK_EQ_I64(board_chip_dbm(b, 28), 15);
    CHECK(!board_gives(b, 3));
    CHECK(!board_gives(b, 29));
    for (size_t i = 0; board_defs[i] != NULL; i++) {
        const struct board_def *d = board_defs[i];
        int gain = d->amp.power == BOARD_NO_PIN ? 0 : d->amp.gain_db;
        CHECK(board_min_dbm(d) <= board_max_dbm(d));
        for (int dbm = board_min_dbm(d); dbm <= board_max_dbm(d); dbm++) {
            int chip = board_chip_dbm(d, (int8_t)dbm);
            CHECK(chip >= board_chip_min_dbm(d) && chip <= board_chip_max_dbm(d));
            CHECK(chip + gain <= dbm);
        }
    }
}

/* A full cell through each board's divider is within the widest range its chip's ADC reads, 3.1 V
 * on the ESP32-S3 and 2.45 V on the ESP32, with a tenth to spare (board.c chooses the narrowest
 * that holds it). */
static void a_full_battery_is_within_the_adcs_reach(void) {
    CHECK_EQ_I64(board_battery_pin_mv(&board_heltec_v3), 857); /* 390k over 100k */
    for (size_t i = 0; board_defs[i] != NULL; i++) {
        const struct board_def *b = board_defs[i];
        if (b->battery.sense != BOARD_NO_PIN) {
            CHECK(board_battery_pin_mv(b) * 11 / 10 <= (b->soc == BOARD_ESP32 ? 2450 : 3100));
        } else {
            CHECK_EQ_I64(board_battery_pin_mv(b), 0);
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
    RUN(an_sx1276_on_pa_boost_gives_2_to_17);
    RUN(a_full_battery_is_within_the_adcs_reach);
    return CHECK_DONE();
}

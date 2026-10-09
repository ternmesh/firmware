#include "qr.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

/* The node's QR code (ports/node/qr.c), against symbols made by other encoders: segno 1.6.6, a
 * Python implementation of ISO/IEC 18004, asked for version 3, level L, alphanumeric mode and each
 * mask in turn; and for a join code's link in three segments (draft/groups.md), the one with no
 * name from ternmesh/site's tests/vectors/qr.json, made with segno, and two with a name of 12
 * bytes, the most version 3 holds, from the site's encoder, which is held to those vectors. Every
 * symbol below was also read back by ZXing, and the address's by OpenCV's detector. They are a
 * check from outside, not something this code was written from. */

static const struct {
    const char *text;
    int mask;
    uint32_t rows[QR_SIZE];
} symbols[] = {
    {"482FA6143FA1B2C4C0FFEE0011223344556677889900AABBCCDDEEFF01234567",
     0,
     {0x1fd7c47f, 0x104c2241, 0x1744095d, 0x1754325d, 0x1745925d, 0x10487c41, 0x1fd5557f, 0x00bc900,
      0x47f51f7,  0x1320fb3f, 0x19f5dcda, 0x1a823706, 0x14714c54, 0x2ee2d14,  0xf6c4561,  0xea1eda2,
      0x159cd66e, 0xee91a3e,  0xdd7fd49,  0x1614d686, 0x7fc7cfd,  0x91a2f00,  0x115c717f, 0xb1d0941,
      0x13f5315d, 0x1eaca85d, 0x14d3195d, 0x192a8741, 0x11d5597f}},
    {"482FA6143FA1B2C4C0FFEE0011223344556677889900AABBCCDDEEFF01234567",
     1,
     {0x1fdd6f7f, 0x10468941, 0x174ea25d, 0x175e985d, 0x174f395d, 0x1042d741, 0x1fd5557f, 0x0016300,
      0x19f5fb67, 0x198a5195, 0x135f7670, 0x10289dac, 0x1edbe6fe, 0x84487be,  0x5c6efcb,  0x40b4708,
      0x1f367cc4, 0x443b094,  0x77d57e3,  0x1cbe7c2c, 0xdf6d657,  0x3108500,  0x1b56da7f, 0x117a341,
      0x19ff9a5d, 0x1406025d, 0x1e79b35d, 0x13802d41, 0x1b7ff37f}},
    {"482FA6143FA1B2C4C0FFEE0011223344556677889900AABBCCDDEEFF01234567",
     2,
     {0x1fc6027f, 0x10421b41, 0x1755ce5d, 0x175a0b5d, 0x1754545d, 0x10464541, 0x1fd5557f, 0x005f000,
      0xaae97df,  0x10aec39c, 0x5841bc6,  0x190c0fa5, 0x8008b48,  0x16015b7,  0x131d827d, 0xd2fd501,
      0x9ed1172,  0xd67229d,  0x11a63a55, 0x159aee25, 0x1bfdbbe1, 0xb141700,  0xd5db77f,  0x9133041,
      0xff4f75d,  0x1d22915d, 0x8a2df5d,  0x1aa4bf41, 0xda49f7f}},
    {"482FA6143FA1B2C4C0FFEE0011223344556677889900AABBCCDDEEFF01234567",
     3,
     {0x1fc6037f, 0x1054c041, 0x1758795d, 0x175a0b5d, 0x17428f5d, 0x104bf241,
      0x1fd5557f, 0x0132a00,  0x1723214f, 0x10aec39c, 0x832c0eb,  0x261b93e,
      0x8008b48,  0xcd6ce9a,  0x87034e6,  0xd2fd501,  0x45bca5f,  0x160a9406,
      0x11a63a55, 0x182c3508, 0x1f00d7a,  0xb141700,  0x15b6c7f,  0x131e8641,
      0xff4f65d,  0x10944b5d, 0x13cf695d, 0x1aa4bf41, 0x012457f}},
    {"482FA6143FA1B2C4C0FFEE0011223344556677889900AABBCCDDEEFF01234567",
     4,
     {0x1fdee17f, 0x105af941, 0x1752d35d, 0x175d175d, 0x174cb65d, 0x105ea741,
      0x1fd5557f, 0x002ec00,  0x1e967473, 0x1e962012, 0x144307f7, 0x8cb1394,
      0x63868c6,  0xf58f639,  0x2da9e4c,  0x1ce8c930, 0x7d5f2fc,  0x35fc113,
      0x0612664,  0x45df214,  0x15f5586f, 0x51cf500,  0x1d5aaa7f, 0x19142d41,
      0x1fc155d,  0x131a725d, 0x1965c25d, 0xb63a341,  0x39c7d7f}},
    {"482FA6143FA1B2C4C0FFEE0011223344556677889900AABBCCDDEEFF01234567",
     5,
     {0x1fdd6e7f, 0x10429841, 0x1755ce5d, 0x174bcd5d, 0x1754555d, 0x1046c641,
      0x1fd5557f, 0x0057200,  0x30e96e3,  0xcdf0480,  0x5841bc6,  0x112c8dad,
      0x1edbe6fe, 0x94097bf,  0x131d827d, 0x115e121d, 0x9ed1172,  0x547a095,
      0x77d57e3,  0x1dba6c2d, 0x1bfdbbe1, 0x1715d100, 0xd5db77f,  0x113b341,
      0x19ff9a5d, 0x1502125d, 0x8a2de5d,  0x6d57941,  0xda49f7f}},
    {"482FA6143FA1B2C4C0FFEE0011223344556677889900AABBCCDDEEFF01234567",
     6,
     {0x1fdd6f7f, 0x105af841, 0x175cea5d, 0x174bcc5d, 0x17461c5d, 0x1045ca41,
      0x1fd5557f, 0x01d1300,  0x1047b25b, 0xcdf0480,  0x11652e2,  0x1ef819d,
      0x1edbe6fe, 0xf58f639,  0x154a6ef,  0x115e121d, 0xd7f5856,  0x1584aca5,
      0x77d57e3,  0x1ba20dab, 0x9f49f73,  0x1715d100, 0x95ffe7f,  0x1110be41,
      0x19ff9b5d, 0x131a735d, 0x1aebfa5d, 0x6d57941,  0x936d77f}},
    {"482FA6143FA1B2C4C0FFEE0011223344556677889900AABBCCDDEEFF01234567",
     7,
     {0x1fd7c47f, 0x10450741, 0x1756415d, 0x1754325d, 0x174cb75d, 0x105a3541, 0x1fd5557f, 0x002ed00,
      0xdcd18cb,  0x1320fb3f, 0xbbcf848,  0x1e107e22, 0x14714c54, 0x10a70986, 0xbfe0c45,  0xea1eda2,
      0x7d5f2fc,  0xa7b531a,  0xdd7fd49,  0x45df214,  0x3fe35d9,  0x91a2f00,  0x355557f,  0xf1f4041,
      0x13f5305d, 0xce58d5d,  0x1041505d, 0x192a8741, 0x39c7d7f}},
    {"HELLO WORLD", 1, {0x1fd9997f, 0x10577741, 0x175ddc5d, 0x1744445d, 0x174eef5d, 0x10488941,
                        0x1fd5557f, 0x01ddd00,  0x19e66767, 0xa266796,  0x19088840, 0x84223af,
                        0x39bbbfa,  0x7911225,  0xcf77249,  0x1fbddd93, 0x17666753, 0x1a2662a6,
                        0xf088ac3,  0x6422490,  0x1dfbbc7f, 0x1b111300, 0xf5f767f,  0x91ddf41,
                        0x11f6645d, 0x276645d,  0x1e608b5d, 0x110a2141, 0x1e5bbb7f}},
    {"A", 0, {0x1fc4447f, 0x10422241, 0x1748895d, 0x1753325d, 0x1753325d, 0x105ddc41,
              0x1fd5557f, 0x0088900,  0x47111f7,  0x53bba2f,  0x105ddc6e, 0x5177607,
              0x15cccd72, 0xa8cce1e,  0xfa226d4,  0x1ae88dab, 0xf311463,  0xd3bbd30,
              0x105ddffd, 0xd177112,  0x1dfccb59, 0x314cf00,  0x752277f,  0xb188d41,
              0x17f1155d, 0xc23bc5d,  0x1175dd5d, 0x1c4f7141, 0x150cc97f}},
    {"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:0123456789ABCDEFGHIJKLMNOPQRSTUV",
     5,
     {0x1fc8a87f, 0x10481a41, 0x174d245d, 0x17458f5d, 0x174bab5d, 0x1049ce41,
      0x1fd5557f, 0x01c3a00,  0x307f4e3,  0x37bae1a,  0xc061dd1,  0x12d2778b,
      0xecf1654,  0x1c6a7080, 0x187b2170, 0x11048983, 0xd53dbfa,  0xdc035b1,
      0x1d41c663, 0x3c62909,  0xff7e2fd,  0x151eff00, 0x1b52397f, 0x1119ab41,
      0x11fc2c5d, 0x1420be5d, 0xa1d4c5d,  0x1232941,  0xc53977f}},
};

/* A join code's links: alphanumeric, then the `#` as a byte, then alphanumeric again. */
static const char no_name[] = "HTTPS://TERNMESH.ORG/G#YTCMJRGEYTCMJRGEYTCMJRGEYQUQU";
static const char twelve[] = /* the group "Ridge walker" */
    "HTTPS://TERNMESH.ORG/G#YTCMJRGEYTCMJRGEYTCMJRGEYSGX4UTJMRTWKIDXMFWGWZLS";

static const struct {
    const char *text;
    int mask;
    uint32_t rows[QR_SIZE];
} join_codes[] = {
    {no_name, 0, {0x1fd9527f, 0x105d7c41, 0x1750175d, 0x17510e5d, 0x1755e45d, 0x105f0241,
                  0x1fd5557f, 0x1df700,   0x46f4df7,  0x9fc2c32,  0x18906361, 0x20d49a5,
                  0x1a8c10c3, 0x1d023a15, 0x1e285e76, 0x1d44b7a3, 0x6b0ca5e,  0x186e888c,
                  0x67c26e1,  0x2dd8906,  0x11fc94f1, 0x111fbb00, 0x13575d7f, 0x171ef741,
                  0x7fac95d,  0xf4fce5d,  0x107ea35d, 0x1d2a0f41, 0x14d0917f}},
    {twelve, 2, {0x1fc8827f, 0x10534941, 0x1741e65d, 0x175f155d, 0x1744545d, 0x10516b41,
                 0x1fd5557f, 0x13e400,   0xabe99df,  0xa722ba9,  0x4e182e4,  0x1833a1d,
                 0x6fdcb75,  0x1e84482a, 0x241ba66,  0x1edaa205, 0x1ac12763, 0x1be0809f,
                 0x1a0ddc5d, 0x153e689,  0xdfd1445,  0x1311a900, 0xf56e77f,  0x1510f441,
                 0x1bfb6f5d, 0xcc19f5d,  0xc0f795d,  0x1ea42741, 0x8a10f7f}},
    {twelve, 5, {0x1fd3ee7f, 0x1053ca41, 0x1741e65d, 0x174ed35d, 0x1744555d, 0x1051e841,
                 0x1fd5557f, 0x136600,   0x31e98e3,  0x1603ecb5, 0x4e182e4,  0x9a3b815,
                 0x1026a6c3, 0x16a4ca22, 0x241ba66,  0x2ab6519,  0x1ac12763, 0x13c00297,
                 0xcd6b1eb,  0x9736481,  0xdfd1445,  0xf106f00,  0xf56e77f,  0x1d107741,
                 0xdf0025d,  0x4e11c5d,  0xc0f785d,  0x2d5e141,  0x8a10f7f}},
};

static void every_mask_matches_another_encoder(void) {
    for (size_t i = 0; i < sizeof symbols / sizeof symbols[0]; i++) {
        struct qr q;
        CHECK(qr_encode(&q, symbols[i].text, symbols[i].mask));
        for (int y = 0; y < QR_SIZE; y++) {
            if (q.rows[y] != symbols[i].rows[y]) {
                fprintf(stderr, "\"%.12s...\" mask %d, row %d: %07lx, expected %07lx\n",
                        symbols[i].text, symbols[i].mask, y, (unsigned long)q.rows[y],
                        (unsigned long)symbols[i].rows[y]);
                check_failures++;
                break;
            }
        }
    }
}

static void a_join_code_is_three_segments(void) {
    for (size_t i = 0; i < sizeof join_codes / sizeof join_codes[0]; i++) {
        struct qr q;
        CHECK(qr_encode(&q, join_codes[i].text, join_codes[i].mask));
        CHECK(memcmp(q.rows, join_codes[i].rows, sizeof q.rows) == 0);
    }
    /* One byte more of the name, and a version 3 code has no room for it. */
    struct qr q;
    char thirteen[sizeof twelve + 2];
    snprintf(thirteen, sizeof thirteen, "%sAB", twelve);
    CHECK(!qr_encode(&q, thirteen, QR_MASK_BEST));
}

/* The penalty rules choose mask 0 for this address, as the other encoder's own scores do. */
static void the_mask_chosen_scores_least(void) {
    struct qr best, zero;
    CHECK(qr_encode(&best, symbols[0].text, QR_MASK_BEST));
    CHECK(qr_encode(&zero, symbols[0].text, 0));
    CHECK(memcmp(&best, &zero, sizeof best) == 0);
}

static void the_finders_and_timing_are_where_they_belong(void) {
    struct qr q;
    CHECK(qr_encode(&q, "TERN", QR_MASK_BEST));
    /* Each finder's outer ring is dark and its separator light. */
    for (int i = 0; i < 7; i++) {
        CHECK(qr_dark(&q, i, 0) && qr_dark(&q, 0, i));
        CHECK(qr_dark(&q, QR_SIZE - 1 - i, 0) && qr_dark(&q, i, QR_SIZE - 1));
        CHECK(!qr_dark(&q, i, 7) && !qr_dark(&q, 7, i));
    }
    for (int i = 8; i < QR_SIZE - 8; i++) {
        CHECK(qr_dark(&q, i, 6) == (i % 2 == 0));
        CHECK(qr_dark(&q, 6, i) == (i % 2 == 0));
    }
    CHECK(qr_dark(&q, 8, QR_SIZE - 8));                                        /* the dark module */
    CHECK(qr_dark(&q, 22, 22) && !qr_dark(&q, 21, 22) && qr_dark(&q, 20, 22)); /* alignment */
}

static void what_does_not_fit_is_refused(void) {
    struct qr q;
    char text[QR_TEXT_MAX + 2];
    memset(text, 'A', sizeof text - 1);
    text[QR_TEXT_MAX + 1] = '\0';
    CHECK(!qr_encode(&q, text, QR_MASK_BEST)); /* 78 characters */
    text[QR_TEXT_MAX] = '\0';
    CHECK(qr_encode(&q, text, QR_MASK_BEST)); /* 77 */
    /* Anything outside the alphanumeric set goes as bytes: a byte apiece, and a segment's start. */
    CHECK(qr_encode(&q, "lower case", QR_MASK_BEST));
    CHECK(qr_encode(&q, "A#B", QR_MASK_BEST));
    memset(text, 'a', sizeof text - 1);
    text[53] = '\0';
    CHECK(qr_encode(&q, text, QR_MASK_BEST)); /* 4 + 8 + 53 * 8 bits: 436 of 440 */
    text[53] = 'a';
    text[54] = '\0';
    CHECK(!qr_encode(&q, text, QR_MASK_BEST));
    CHECK(!qr_encode(&q, "A", 8));
    CHECK(!qr_encode(&q, "A", -2));
    CHECK(qr_encode(&q, "", QR_MASK_BEST));
}

int main(void) {
    RUN(every_mask_matches_another_encoder);
    RUN(a_join_code_is_three_segments);
    RUN(the_mask_chosen_scores_least);
    RUN(the_finders_and_timing_are_where_they_belong);
    RUN(what_does_not_fit_is_refused);
    return CHECK_DONE();
}

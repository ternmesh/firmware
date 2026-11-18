#include "ui.h"

#include "tern/share.h"

#include <stdio.h>
#include <string.h>

/* More's choices, in the order the menu shows them. */
static const struct {
    int page;
    uint8_t icon;
    const char *name;
} menu[UI_MENU] = {
    {UI_AIR, ICON_AIR, "Air"},         {UI_GROUPS, ICON_GROUP, "Groups"},
    {UI_NODE, ICON_INFO, "This node"}, {UI_PHONES, ICON_PHONE, "Phones"},
    {UI_RESET, ICON_ERASE, "Erase"},   {UI_MORE, ICON_BACK, "Back"},
};

int ui_menu_page(int choice) {
    return choice >= 0 && choice < UI_MENU ? menu[choice].page : UI_MORE;
}

int ui_menu_choice(int page) {
    for (int i = 0; i < UI_MENU_BACK; i++) {
        if (menu[i].page == page) {
            return i;
        }
    }
    return -1;
}

/* What the board's button is marked, to name it in a line: a page that names it is reached only by
 * pressing it, so a board without one never shows it. */
static const char *button_of(const struct ui_node *n) {
    return n->button != NULL ? n->button : "PRG";
}

/* "45s", "12m", "3h", "9d": a span in two or three characters. */
static void span(char *buf, size_t size, uint32_t s) {
    if (s < 60) {
        snprintf(buf, size, "%lus", (unsigned long)s);
    } else if (s < 3600) {
        snprintf(buf, size, "%lum", (unsigned long)(s / 60));
    } else if (s < 86400) {
        snprintf(buf, size, "%luh", (unsigned long)(s / 3600));
    } else {
        snprintf(buf, size, "%lud", (unsigned long)(s / 86400));
    }
}

/* A name as the node keeps it, which ends at its NUL or at UI_NAME bytes, whichever comes first,
 * cut back to whole characters if it was cut short. */
static void name(char out[UI_NAME + 1], const char *in) {
    const char *end = memchr(in, '\0', UI_NAME + 1);
    size_t len = end != NULL ? (size_t)(end - in) : UI_NAME;
    if (end == NULL) {
        while (len > 0 && ((uint8_t)in[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    memcpy(out, in, len);
    out[len] = '\0';
}

/* The share of the air left, as a whole percentage, rounded down so that it never says more is
 * left than is. */
static unsigned left_percent(const struct ui_node *n) {
    if (n->air_limit_ms <= 0 || n->air_used_ms >= n->air_limit_ms) {
        return 0;
    }
    int64_t left = n->air_limit_ms - n->air_used_ms;
    if (n->air_limit_ms <= INT64_MAX / 100) {
        return (unsigned)(left * 100 / n->air_limit_ms);
    }
    return (unsigned)(left / (n->air_limit_ms / 100));
}

/* The heading of a main page: its name, the node's status, and which page it is. */
static void main_page(const struct ui_node *n, struct scene *s, int page, uint8_t icon,
                      const char *title) {
    snprintf(s->title, sizeof s->title, "%s", title);
    s->title_icon = icon;
    s->status = true;
    s->battery = n->battery;
    s->charging = n->charging;
    s->phone = n->phone;
    s->unread = n->unread;
    s->dot = (uint8_t)page;
    s->dots = UI_MAIN;
}

/* The heading of a page More opens: its name, and More's dot, since that is where it is. */
static void more_page(const struct ui_node *n, struct scene *s, uint8_t icon, const char *title) {
    main_page(n, s, UI_MORE, icon, title);
}

/* A page with nothing on it yet: what it would show, and where that comes from. */
static void empty(struct scene *s, uint8_t icon, const char *what, const char *how) {
    struct scene_item *it = scene_add(s, SCENE_LINE, TONE_TEXT, "%s", what);
    it->icon = icon;
    it->centre = true;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_COLOUR;
    snprintf(s->text, sizeof s->text, "%s", how);
    scene_add(s, SCENE_TEXT, TONE_DIM, NULL)->centre = true;
}

static void home(const struct ui_node *n, struct scene *s) {
    char v[16], wait[16];
    main_page(n, s, UI_HOME, ICON_HOME, "Tern");
    snprintf(s->title_right, sizeof s->title_right, "%s", "");

    scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
    if (n->bench) {
        scene_add(s, SCENE_HERO, TONE_WARN, "Bench mode");
    } else if (n->unread > 0) {
        scene_add(s, SCENE_HERO, TONE_ACCENT, "%u new", (unsigned)n->unread);
    } else if (n->nearby == 0) {
        scene_add(s, SCENE_HERO, TONE_DIM, "Listening");
    } else {
        scene_add(s, SCENE_HERO, TONE_GOOD, "%u nearby", (unsigned)n->nearby);
    }

    /* With room: three figures, each a tile. */
    bool says_nearby = !n->bench && n->unread == 0 && n->nearby > 0;
    if (says_nearby) {
        snprintf(v, sizeof v, "%u", (unsigned)n->messages); /* the large line counts them */
        scene_stat(s, n->messages > 0 ? TONE_TEXT : TONE_DIM, v,
                   n->messages == 1 ? "message" : "messages")
            ->on = ON_COLOUR;
    } else {
        snprintf(v, sizeof v, "%u", (unsigned)n->nearby);
        scene_stat(s, n->nearby > 0 ? TONE_TEXT : TONE_DIM, v, "nearby")->on = ON_COLOUR;
    }
    snprintf(v, sizeof v, "%u", (unsigned)n->reachable);
    scene_stat(s, n->reachable > 0 ? TONE_TEXT : TONE_DIM, v, "reachable");
    if (!n->limited) {
        scene_stat(s, TONE_GOOD, "Free", "air");
    } else if (n->wait_ms > 0) {
        span(wait, sizeof wait, (n->wait_ms + 999) / 1000);
        scene_stat(s, TONE_WARN, wait, "air full");
    } else {
        unsigned left = left_percent(n);
        snprintf(v, sizeof v, "%u%%", left);
        scene_stat(s, left <= 20 ? TONE_WARN : TONE_GOOD, v, "air left");
    }

    /* On a line of 21 characters: what the large line does not say, never the same thing twice. */
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
    struct scene_item *it;
    if (n->nearby == 0) {
        it = scene_add(s, SCENE_LINE, TONE_DIM, "No nodes heard yet");
    } else if (says_nearby) {
        it = scene_add(s, SCENE_LINE, TONE_TEXT, "Reaches %u nodes", (unsigned)n->reachable);
    } else {
        it = scene_add(s, SCENE_LINE, TONE_TEXT, "%u nearby, reaches %u", (unsigned)n->nearby,
                       (unsigned)n->reachable);
    }
    it->on = ON_MONO;

    char from[UI_NAME + 1];
    if (n->unread > 0) {
        name(from, n->from);
        it = scene_add(s, SCENE_LINE, TONE_ACCENT, "Last %s %s", n->from_group ? "in" : "from",
                       from);
        it->icon = ICON_CHAT;
    } else if (n->waiting > 0) {
        it = scene_add(s, SCENE_LINE, TONE_WARN, "%u awaiting delivery", (unsigned)n->waiting);
        it->icon = ICON_SCHEDULE;
    } else {
        it = scene_add(s, SCENE_LINE, TONE_DIM, "No new messages");
        it->icon = ICON_CHECK;
    }

    if (!n->limited) {
        it = scene_add(s, SCENE_LINE, TONE_TEXT, "Air: no limit");
    } else if (n->wait_ms > 0) {
        span(wait, sizeof wait, (n->wait_ms + 999) / 1000);
        it = scene_add(s, SCENE_LINE, TONE_WARN, "Air: full for %s", wait);
    } else {
        it = scene_add(s, SCENE_LINE, TONE_TEXT, "Air: %u%% left", left_percent(n));
    }
    it->on = ON_MONO;
}

/* What became of a message this node sent, in the words of draft/companion.md's states, with the
 * icon and tone a colour panel gives it. */
static void delivery(const struct ui_message *m, struct scene *s) {
    char wait[16] = "";
    if (m->wait_s > 0) {
        span(wait, sizeof wait, m->wait_s);
    }
    struct scene_item *it;
    switch (m->state) {
    case TERN_C_SENT:
        it = scene_add(s, SCENE_LINE, TONE_DIM, "Sent");
        it->icon = ICON_CHECK;
        return;
    case TERN_C_DELIVERED:
        it = scene_add(s, SCENE_LINE, TONE_GOOD, "Delivered");
        it->icon = ICON_DONE_ALL;
        return;
    case TERN_C_NOT_DELIVERED:
        it = scene_add(s, SCENE_LINE, TONE_BAD, "Not delivered");
        it->icon = ICON_ERROR;
        return;
    default:
        break;
    }
    switch (m->reason) {
    case TERN_C_WAIT_ROUTE:
        it = scene_add(s, SCENE_LINE, TONE_WARN, "Waiting: no route yet");
        break;
    case TERN_C_WAIT_SESSION:
        it = scene_add(s, SCENE_LINE, TONE_WARN, "Making contact...");
        break;
    case TERN_C_WAIT_REGION:
        if (wait[0] != '\0') {
            it = scene_add(s, SCENE_LINE, TONE_WARN, "Air limit, %s wait", wait);
        } else {
            it = scene_add(s, SCENE_LINE, TONE_WARN, "Waiting: air limit");
        }
        break;
    case TERN_C_WAIT_BUDGET:
        it = scene_add(s, SCENE_LINE, TONE_WARN, "Waiting: air budget");
        break;
    case TERN_C_WAIT_RADIO:
        it = scene_add(s, SCENE_LINE, TONE_WARN, "Waiting: radio busy");
        break;
    default:
        it = scene_add(s, SCENE_LINE, TONE_WARN, "Waiting");
        break;
    }
    it->icon = ICON_SCHEDULE;
}

static void messages(const struct ui_node *n, struct scene *s) {
    main_page(n, s, UI_MESSAGES, ICON_CHAT, "Messages");
    if (n->messages == 0) {
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        empty(s, ICON_CHAT, "No messages yet", "Send one from your phone or computer");
        return;
    }
    snprintf(s->title_right, sizeof s->title_right, "%u/%u", (unsigned)n->shown + 1,
             (unsigned)n->messages);

    const struct ui_message *m = &n->message;
    char who[UI_NAME + 1], ago[16] = "";
    name(who, m->who);
    const char *how = !m->received ? "To" : m->unread ? "New from" : "From";
    if (m->group && m->received) {
        how = m->unread ? "New in" : "In";
    }
    if (m->aged) {
        if (m->ago_s < 60) {
            snprintf(ago, sizeof ago, "now");
        } else {
            char t[8];
            span(t, sizeof t, m->ago_s);
            snprintf(ago, sizeof ago, "%s ago", t);
        }
    }
    struct scene_item *it =
        scene_add(s, SCENE_LINE, m->unread ? TONE_ACCENT : TONE_TEXT, "%s %s", how, who);
    it->icon = m->received ? (m->group ? ICON_GROUP : ICON_CHAT) : ICON_SEND;
    snprintf(it->right, sizeof it->right, "%s", ago);
    if (m->received && m->group) {
        /* The group on the line above; who wrote to it on its own. */
        char writer[UI_NAME + 1];
        name(writer, m->writer);
        scene_add(s, SCENE_SMALL, TONE_DIM, "Claims %s", writer); /* any member can say so */
    }
    size_t len = m->text_len < TERN_COMPANION_TEXT_MAX ? m->text_len : TERN_COMPANION_TEXT_MAX;
    if (len >= sizeof s->text) {
        len = sizeof s->text - 1;
    }
    memcpy(s->text, m->text, len);
    s->text[len] = '\0';
    it = scene_add(s, SCENE_TEXT, m->received ? TONE_TEXT : TONE_ACCENT, NULL);
    it->bubble = true;
    if (!m->received) {
        delivery(m, s);
    }
    if (n->messages > 1) {
        snprintf(s->hint, sizeof s->hint, "Hold: older");
    }
}

static void nearby(const struct ui_node *n, struct scene *s) {
    main_page(n, s, UI_NEARBY, ICON_PEOPLE, "Nearby");
    if (n->nearby == 0) {
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        empty(s, ICON_ANTENNA, "No nodes heard yet", "A node shows here once it is heard.");
        return;
    }
    if (n->nearby_n >= n->nearby) {
        snprintf(s->title_right, sizeof s->title_right, "%u", (unsigned)n->nearby);
    } else {
        /* More than fit: which of them these are, as Messages says which message it shows. */
        snprintf(s->title_right, sizeof s->title_right, "%u-%u/%u",
                 (unsigned)(n->nearby_first + 1) % 1000u,
                 (unsigned)(n->nearby_first + n->nearby_n) % 1000u, (unsigned)n->nearby % 1000u);
        snprintf(s->hint, sizeof s->hint, "Hold: more");
    }
    for (int i = 0; i < n->nearby_n && i < UI_NEARBY_ROWS; i++) {
        const struct ui_neighbour *g = &n->neighbour[i];
        char who[UI_NAME + 1];
        name(who, g->name);
        struct scene_item *it = scene_add(s, SCENE_LINE, TONE_TEXT, "%s", who);
        it->signal = true;
        it->snr_db = g->snr_db;
        span(it->right, sizeof it->right, g->ago_s);
    }
}

static void share(const struct ui_node *n, struct scene *s) {
    static char sc[TERN_SHORT_CODE_LEN + 1];
    main_page(n, s, UI_SHARE, ICON_QR, "My code");
    /* The link (draft/sharing.md): a web address a phone's camera opens, with the address in it in
     * base32, which the console's 'contact' also takes as it is. */
    tern_address_link(n->address, s->qr);
    tern_short_code(n->address, sc);
    /* Beside it, the short code, for whoever scanned it to check against their phone's. */
    scene_add(s, SCENE_SMALL, TONE_DIM, "Scan to add me");
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    scene_add(s, SCENE_SMALL, TONE_DIM, "Its code");
    scene_add(s, SCENE_HERO, TONE_ACCENT, "%.4s %.4s", sc, &sc[5]);
    scene_add(s, SCENE_HERO, TONE_ACCENT, "%.4s", &sc[10]);
    /* On a monochrome screen, where the code leads: a phone with no Tern app opens a page on the
     * site (draft/sharing.md). */
    static char first[10];
    snprintf(first, sizeof first, "%.9s", sc); /* "5358 3737" on one line, "3382" on the next */
    s->qr_words[1] = "Scan to";
    s->qr_words[2] = "open its";
    s->qr_words[3] = "web page";
    s->qr_words[5] = "Its code:";
    s->qr_words[6] = first;
    s->qr_words[7] = &sc[10];
}

static void more(const struct ui_node *n, struct scene *s) {
    main_page(n, s, UI_MORE, ICON_MORE, "More");
    for (int i = 0; i < UI_MENU; i++) {
        if (i == UI_MENU_BACK && n->menu < 0) {
            continue; /* closed: a press goes on to Home */
        }
        struct scene_item *it = scene_add(
            s, SCENE_TILE, i == UI_RESET - UI_AIR ? TONE_BAD : TONE_TEXT, "%s", menu[i].name);
        it->icon = menu[i].icon;
        it->selected = n->menu == i;
    }
    if (n->menu < 0) {
        snprintf(s->hint, sizeof s->hint, "Hold: open menu");
    } else {
        snprintf(s->hint, sizeof s->hint, "Hold: open");
    }
}

static void air(const struct ui_node *n, struct scene *s) {
    char used[24], wait[16];
    more_page(n, s, ICON_AIR, "Air");
    snprintf(s->title_right, sizeof s->title_right, "%s", n->region);
    snprintf(s->hint, sizeof s->hint, "Press: back");
    if (!n->limited) {
        int64_t ms = n->air_total_ms < 0 ? 0 : n->air_total_ms;
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
        scene_add(s, SCENE_HERO, TONE_GOOD, "No limit");
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        snprintf(s->text, sizeof s->text,
                 "%s sets no limit. Sent for %lld.%lld s since it started.", n->region,
                 (long long)(ms / 1000), (long long)(ms / 100 % 10));
        scene_add(s, SCENE_TEXT, TONE_DIM, NULL);
        return;
    }
    unsigned left = left_percent(n);
    int64_t ms = n->air_used_ms < 0 ? 0 : n->air_used_ms;
    snprintf(used, sizeof used, "%lld.%lld", (long long)(ms / 1000), (long long)(ms / 100 % 10));
    scene_add(s, SCENE_HERO, left <= 20 ? TONE_WARN : TONE_GOOD, "%u%% left", left)->on = ON_COLOUR;
    scene_add(s, SCENE_LINE, TONE_TEXT, "Used %s of %lld s", used,
              (long long)(n->air_limit_ms / 1000));
    struct scene_item *it;
    if (n->window_s == 3600) {
        it = scene_add(s, SCENE_SMALL, TONE_DIM, "in any hour");
    } else if (n->window_s % 60 == 0) {
        it = scene_add(s, SCENE_SMALL, TONE_DIM, "in any %lu minutes",
                       (unsigned long)(n->window_s / 60));
    } else {
        it = scene_add(s, SCENE_SMALL, TONE_DIM, "in any %lu s", (unsigned long)n->window_s);
    }
    it->on = ON_MONO;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
    it = scene_add(s, SCENE_METER, left <= 20 ? TONE_WARN : TONE_ACCENT, NULL);
    it->part = (int32_t)(n->air_used_ms < 0           ? 0
                         : n->air_used_ms > INT32_MAX ? INT32_MAX
                                                      : n->air_used_ms);
    it->whole = (int32_t)(n->air_limit_ms > INT32_MAX ? INT32_MAX : n->air_limit_ms);
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
    if (n->wait_ms > 0) {
        span(wait, sizeof wait, (n->wait_ms + 999) / 1000);
        it = scene_add(s, SCENE_LINE, TONE_WARN, "Next send in %s", wait);
        it->icon = ICON_SCHEDULE;
    } else {
        it = scene_add(s, SCENE_LINE, TONE_GOOD, "Free to send now");
        it->icon = ICON_CHECK;
    }
    if (n->window_s > 0 && n->air_limit_ms > 0) {
        scene_add(s, SCENE_SMALL, TONE_DIM, "%s allows %lld%%", n->region,
                  (long long)(n->air_limit_ms / 10 / (int64_t)n->window_s))
            ->on = ON_MONO;
    }
}

size_t ui_join_name(const uint8_t *name, size_t len) {
    if (len <= UI_JOIN_NAME) {
        return len;
    }
    size_t n = UI_JOIN_NAME;
    while (n > 0 && (name[n] & 0xC0) == 0x80) {
        n--; /* back to the start of the character the limit falls within */
    }
    return n;
}

/* A group's join code (draft/groups.md), once the user held PRG to see it, saying beside it what
 * anyone who sees it can do; and before that, which group a hold shows it for, and what it is. */
static void groups(const struct ui_node *n, struct scene *s) {
    static char shown[10];
    char group[UI_NAME + 1];
    name(group, n->group);
    more_page(n, s, ICON_GROUP, "Groups");
    if (n->groups > 0 && n->join_link[0] != '\0') {
        snprintf(s->title, sizeof s->title, "Join code");
        s->dots = 0; /* beside the code, the room is the hint's */
        snprintf(s->qr, sizeof s->qr, "%s", n->join_link);
        snprintf(shown, sizeof shown, "%.9s", group);
        scene_add(s, SCENE_LINE, TONE_TEXT, "%s", group);
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        snprintf(s->text, sizeof s->text, "Whoever scans it can read the group.");
        scene_add(s, SCENE_TEXT, TONE_WARN, NULL);
        snprintf(s->hint, sizeof s->hint, "Hold: next");
        s->qr_words[0] = "Join code";
        s->qr_words[1] = shown;
        s->qr_words[3] = "Whoever";
        s->qr_words[4] = "scans it";
        s->qr_words[5] = "can read";
        s->qr_words[6] = "the group";
        return;
    }
    if (n->groups == 0) {
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        empty(s, ICON_GROUP, "No groups yet", "Make or join one from your phone.");
        snprintf(s->hint, sizeof s->hint, "Press: back");
        return;
    }
    snprintf(s->title_right, sizeof s->title_right, "%u/%u", (unsigned)n->group_shown + 1,
             (unsigned)n->groups);
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
    scene_add(s, SCENE_LINE, TONE_TEXT, "%s", group[0] != '\0' ? group : "(no name)")->icon =
        ICON_GROUP;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    snprintf(s->text, sizeof s->text,
             "Hold %s to show its join code. Whoever scans it can read the group.", button_of(n));
    scene_add(s, SCENE_TEXT, TONE_DIM, NULL);
    snprintf(s->hint, sizeof s->hint, "Hold: join code");
}

static void node(const struct ui_node *n, struct scene *s) {
    char sc[TERN_SHORT_CODE_LEN + 1];
    tern_short_code(n->address, sc);
    more_page(n, s, ICON_INFO, "This node");
    snprintf(s->hint, sizeof s->hint, "Press: back");
    /* The short code, for two people to compare, and not the address: sixty-four hex digits are
     * no use read off a screen. My code has the address to scan, and the console's 'status' has
     * it to copy. */
    scene_add(s, SCENE_SMALL, TONE_DIM, "Code to compare:")->on = ON_MONO;
    scene_add(s, SCENE_LINE, TONE_TEXT, "  %s", sc)->on = ON_MONO;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
    struct scene_item *it = scene_add(s, SCENE_SMALL, TONE_TEXT, "%s, %s, %+d dBm",
                                      n->relay ? "Relay" : "Leaf", n->region, n->dbm);
    it->on = ON_MONO;
    if (n->ble_name != NULL) {
        scene_add(s, SCENE_SMALL, TONE_TEXT, "Seen as %s", n->ble_name)->on = ON_MONO;
    }
    scene_add(s, SCENE_SMALL, TONE_TEXT, "Tern %s", n->version)->on = ON_MONO;

    it = scene_add(s, SCENE_LINE, TONE_ACCENT, "%s", sc);
    it->icon = ICON_QR;
    it->on = ON_COLOUR;
    it = scene_add(s, SCENE_LINE, TONE_TEXT, "%s", n->relay ? "Relay" : "Leaf");
    snprintf(it->right, sizeof it->right, "%s", n->relay ? "helps the mesh" : "just me");
    it->icon = ICON_ANTENNA;
    it->on = ON_COLOUR;
    it = scene_add(s, SCENE_LINE, TONE_TEXT, "%s", n->region);
    snprintf(it->right, sizeof it->right, "%+d dBm", n->dbm);
    it->icon = ICON_AIR;
    it->on = ON_COLOUR;
    it = scene_add(s, SCENE_LINE, TONE_TEXT, "Tern %s", n->version);
    it->icon = ICON_INFO;
    it->on = ON_COLOUR;
}

/* The heading of a page that is asking to be sure, with the seconds it will go on asking. */
static void asking(struct scene *s, const char *question, unsigned seconds) {
    snprintf(s->title, sizeof s->title, "%s", question);
    snprintf(s->title_right, sizeof s->title_right, "%us", seconds);
    s->title_icon = ICON_WARNING;
    s->status = false; /* room for the question */
    s->dots = 0;
}

static void phones(const struct ui_node *n, struct scene *s) {
    more_page(n, s, ICON_PHONE, "Phones");
    snprintf(s->hint, sizeof s->hint, "Press: back");
    if (!n->bluetooth) {
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        scene_add(s, SCENE_LINE, TONE_BAD, "Bluetooth is off")->icon = ICON_BLUETOOTH;
        snprintf(s->text, sizeof s->text, "It did not start. Restart the board to try again.");
        scene_add(s, SCENE_TEXT, TONE_DIM, NULL);
        return;
    }
    if (n->confirm_s > 0 && n->paired > 0) {
        asking(s, "Forget phones?", n->confirm_s);
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        snprintf(s->text, sizeof s->text,
                 "Hold %s again to forget %u phone%s. Each must pair again.", button_of(n),
                 (unsigned)n->paired, n->paired == 1 ? "" : "s");
        scene_add(s, SCENE_TEXT, TONE_WARN, NULL);
        snprintf(s->hint, sizeof s->hint, "A press keeps them");
        return;
    }
    if (n->ble_name != NULL) {
        struct scene_item *it = scene_add(s, SCENE_LINE, TONE_TEXT, "Seen as %s", n->ble_name);
        it->icon = ICON_BLUETOOTH;
    }
    if (n->paired == 0) {
        scene_add(s, SCENE_LINE, TONE_DIM, "None paired")->on = ON_MONO;
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        snprintf(s->text, sizeof s->text,
                 "A phone pairs with the passkey shown here when it asks.");
        scene_add(s, SCENE_TEXT, TONE_DIM, NULL);
        return;
    }
    scene_add(s, SCENE_HERO, TONE_ACCENT, "%u paired", (unsigned)n->paired);
    struct scene_item *it = scene_add(s, SCENE_LINE, n->phone ? TONE_GOOD : TONE_DIM, "%s",
                                      n->phone ? "One is connected" : "None connected");
    it->icon = n->phone ? ICON_CHECK : ICON_NONE;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
    snprintf(s->text, sizeof s->text, "Hold %s to forget them all.", button_of(n));
    scene_add(s, SCENE_TEXT, TONE_DIM, NULL)->on = ON_MONO;
    snprintf(s->hint, sizeof s->hint, "Hold: forget all");
}

/* Everything the node keeps goes, not only what a client set: a new owner gets a node that
 * nothing can be read back from, with an address of its own. */
static void reset(const struct ui_node *n, struct scene *s) {
    more_page(n, s, ICON_ERASE, "Erase this node");
    if (n->confirm_s > 0) {
        asking(s, "Erase everything?", n->confirm_s);
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        snprintf(s->text, sizeof s->text,
                 "Hold %s again to erase it all. It starts again with a new address.",
                 button_of(n));
        scene_add(s, SCENE_TEXT, TONE_BAD, NULL);
        snprintf(s->hint, sizeof s->hint, "A press keeps it all");
        return;
    }
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    snprintf(s->text, sizeof s->text,
             "Erases its address, contacts, groups, messages, phones and settings: a new node.");
    scene_add(s, SCENE_TEXT, TONE_TEXT, NULL);
    snprintf(s->hint, sizeof s->hint, "Hold %s to erase", button_of(n));
}

void ui_scene(const struct ui_node *n, int page, struct scene *s) {
    scene_clear(s);
    switch (page) {
    case UI_MESSAGES:
        messages(n, s);
        break;
    case UI_NEARBY:
        nearby(n, s);
        break;
    case UI_SHARE:
        share(n, s);
        break;
    case UI_MORE:
        more(n, s);
        break;
    case UI_AIR:
        air(n, s);
        break;
    case UI_GROUPS:
        groups(n, s);
        break;
    case UI_NODE:
        node(n, s);
        break;
    case UI_PHONES:
        phones(n, s);
        break;
    case UI_RESET:
        reset(n, s);
        break;
    default:
        home(n, s);
        break;
    }
}

void ui_draw(const struct ui_node *n, int page, struct display *d) {
    static struct scene s;
    ui_scene(n, page, &s);
    mono_draw(&s, d);
    memset(s.qr, 0, sizeof s.qr); /* a join code's is the group's secret */
}

void ui_erasing(struct scene *s) {
    scene_clear(s);
    snprintf(s->title, sizeof s->title, "Erasing");
    s->title_icon = ICON_ERASE;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    snprintf(s->text, sizeof s->text, "Starting again as a new node...");
    scene_add(s, SCENE_TEXT, TONE_TEXT, NULL);
}

void ui_pairing(uint32_t passkey, struct scene *s) {
    scene_clear(s);
    snprintf(s->title, sizeof s->title, "Bluetooth pairing");
    s->title_icon = ICON_BLUETOOTH;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    scene_add(s, SCENE_LINE, TONE_TEXT, "Type this passkey")->on = ON_MONO;
    scene_add(s, SCENE_LINE, TONE_TEXT, "into your phone:")->on = ON_MONO;
    scene_add(s, SCENE_SMALL, TONE_DIM, "Type this passkey into your phone")->on = ON_COLOUR;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    scene_add(s, SCENE_HERO, TONE_ACCENT, "%06lu", (unsigned long)(passkey % 1000000));
}

void ui_updating(uint32_t held, uint32_t size, struct scene *s) {
    scene_clear(s);
    snprintf(s->title, sizeof s->title, "Updating");
    s->title_icon = ICON_DOWNLOAD;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    scene_add(s, SCENE_LINE, TONE_TEXT, "New firmware is")->on = ON_MONO;
    scene_add(s, SCENE_LINE, TONE_TEXT, "arriving:")->on = ON_MONO;
    scene_add(s, SCENE_SMALL, TONE_DIM, "New firmware is arriving")->on = ON_COLOUR;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    unsigned pc = (unsigned)(size == 0 ? 0 : (uint64_t)held * 100 / size);
    scene_add(s, SCENE_HERO, TONE_ACCENT, "%u%%", pc);
    struct scene_item *it = scene_add(s, SCENE_METER, TONE_ACCENT, NULL);
    it->part = (int32_t)(held > INT32_MAX ? INT32_MAX : held);
    it->whole = (int32_t)(size > INT32_MAX ? INT32_MAX : size);
    it->on = ON_COLOUR;
}

void ui_bootloader(struct scene *s) {
    scene_clear(s);
    snprintf(s->title, sizeof s->title, "Updating");
    s->title_icon = ICON_DOWNLOAD;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    snprintf(s->text, sizeof s->text,
             "Restarting to take new firmware over Bluetooth. Keep your phone near; nothing is "
             "lost.");
    scene_add(s, SCENE_TEXT, TONE_TEXT, NULL);
}

void ui_rows(const char *const *rows, int n, struct scene *s) {
    scene_clear(s);
    snprintf(s->title, sizeof s->title, "%s", n > 0 ? rows[0] : "");
    s->title_icon = ICON_ANTENNA;
    for (int i = 1; i < n; i++) {
        scene_add(s, SCENE_SMALL, TONE_TEXT, "%s", rows[i]);
    }
}

void ui_boot(const struct ui_start *st, struct scene *s) {
    scene_clear(s);
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_COLOUR;
    scene_add(s, SCENE_HERO, TONE_ACCENT, "Tern"); /* in place of a heading */
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
    scene_add(s, SCENE_SMALL, TONE_DIM, "Version %s", st->version);
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    if (st->region != NULL) {
        scene_add(s, SCENE_SMALL, TONE_TEXT, "%s, starting...", st->region);
    } else {
        scene_add(s, SCENE_SMALL, TONE_TEXT, "Starting...");
    }
    if (st->address != NULL) {
        char sc[TERN_SHORT_CODE_LEN + 1];
        tern_short_code(st->address, sc);
        scene_add(s, SCENE_SMALL, TONE_TEXT, "Code %s", sc);
        if (st->new_address) {
            scene_add(s, SCENE_SMALL, TONE_ACCENT, "New address made");
        }
    }
}

/* What went wrong, up to three lines, and what to do about it, up to two. */
struct fault_words {
    const char *what[3], *todo[2];
};

static const struct fault_words fault_words[] = {
    [UI_FAULT_STORAGE] = {{"Its saved data is", "from a layout this", "firmware can't read."},
                          {"Erase and flash it;", "it gets a new address"}},
    [UI_FAULT_IDENTITY] = {{"Could not make or", "save its identity", "to flash."},
                           {"Restart it. If this", "repeats, erase it."}},
    [UI_FAULT_POWER] = {{"Its power is more", "than the region", "allows."},
                        {"Lower the power and", "flash it again."}},
    [UI_FAULT_RANDOM] = {{"No random numbers", "to start the mesh", "with."},
                         {"Restart it.", NULL}},
    [UI_FAULT_RADIO] = {{"The radio did not", "answer.", NULL},
                        {"Restart it. If this", "repeats, ask for help"}},
};

void ui_fault(enum ui_fault why, const struct ui_start *st, int code, struct scene *s) {
    scene_clear(s);
    snprintf(s->title, sizeof s->title, "Did not start");
    s->title_icon = ICON_ERROR;
    const struct fault_words *w =
        (unsigned)why < sizeof fault_words / sizeof fault_words[0] ? &fault_words[why] : NULL;
    int lines = 0;
    for (int i = 0; w != NULL && i < 3; i++) {
        if (w->what[i] != NULL) {
            scene_add(s, SCENE_SMALL, TONE_TEXT, "%s", w->what[i]);
            lines++;
        }
    }
    while (lines++ < 3) {
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
    }
    /* For whoever is asked for help: the driver's own number for what failed, and which build. */
    if (code != 0) {
        scene_add(s, SCENE_SMALL, TONE_BAD, "Error %d", code);
    } else {
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    }
    for (int i = 0; i < 2; i++) {
        if (w != NULL && w->todo[i] != NULL) {
            scene_add(s, SCENE_SMALL, TONE_WARN, "%s", w->todo[i]);
        } else {
            scene_add(s, SCENE_GAP, TONE_TEXT, NULL)->on = ON_MONO;
        }
    }
    scene_add(s, SCENE_SMALL, TONE_DIM, "Tern %s", st->version);
}

void ui_turning_off(unsigned seconds, struct scene *s) {
    scene_clear(s);
    snprintf(s->title, sizeof s->title, "Turning off");
    s->title_icon = ICON_POWER;
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    scene_add(s, SCENE_HERO, TONE_WARN, "%u", seconds);
    scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
    scene_add(s, SCENE_SMALL, TONE_TEXT, "Keep holding to turn");
    scene_add(s, SCENE_SMALL, TONE_TEXT, "off. Let go: stays on");
    /* The link keeps messages in memory only (link.h), so they do not outlast the board's being
     * off: said while it can still be stopped. */
    scene_add(s, SCENE_SMALL, TONE_DIM, "Its messages are lost");
}

void ui_off(enum ui_off why, const char *button, struct scene *s) {
    scene_clear(s);
    s->title_icon = ICON_POWER;
    if (why == UI_OFF_EMPTY) {
        snprintf(s->title, sizeof s->title, "Battery empty");
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        scene_add(s, SCENE_SMALL, TONE_TEXT, "Turned off to save");
        scene_add(s, SCENE_SMALL, TONE_TEXT, "the battery.");
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        scene_add(s, SCENE_SMALL, TONE_DIM, "Charge it: it turns");
        scene_add(s, SCENE_SMALL, TONE_DIM, "on again by itself.");
    } else if (why == UI_OFF_ASKED) {
        snprintf(s->title, sizeof s->title, "Turned off");
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        scene_add(s, SCENE_SMALL, TONE_TEXT, "by a phone or");
        scene_add(s, SCENE_SMALL, TONE_TEXT, "computer.");
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        if (button != NULL) {
            scene_add(s, SCENE_SMALL, TONE_DIM, "Press %s or RESET", button);
        } else {
            scene_add(s, SCENE_SMALL, TONE_DIM, "Press RESET");
        }
        scene_add(s, SCENE_SMALL, TONE_DIM, "to turn it on again.");
    } else {
        snprintf(s->title, sizeof s->title, "Turned off");
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        scene_add(s, SCENE_GAP, TONE_TEXT, NULL);
        scene_add(s, SCENE_SMALL, TONE_TEXT, "Press %s to turn", button != NULL ? button : "RESET");
        scene_add(s, SCENE_SMALL, TONE_TEXT, "it on again.");
    }
}

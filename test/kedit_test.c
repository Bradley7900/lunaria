/* SPDX-License-Identifier: MPL-2.0
 * The keymap editor's model (luna_kedit_*): what it reads, what it changes,
 * and that what it writes is a file the loader accepts with everything it did
 * not model still in place. */
#include "luna_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}
static void spit(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    const char *path = "kedit_test.conf", *bak = "kedit_test.conf.bak";
    remove(path); remove(bak);
    spit(path,
         "# header comment\n"
         "stick 0.15625 0.764 0.18\n"
         "walk CTRL 0.04\n"
         "button SPACE 0.922 0.665\n"
         "button F 0.5 0.5 0.6 0.6\n"            /* two points: kept verbatim */
         "button E 0.731 0.858\n"
         "macro Q 0.1,0.1 @50 0.2,0.2\n");

    luna_kedit e;
    CHECK(luna_kedit_load(&e, path));
    CHECK(e.has_stick && e.radius > .179f && e.radius < .181f);
    CHECK(e.n == 2);                               /* SPACE and E */
    CHECK(e.kept_buttons == 1);
    CHECK(luna_kedit_find(&e, 32) == 0 && luna_kedit_find(&e, 'E') == 1);

    /* reserved / bad keys */
    CHECK(luna_kedit_set(&e, -1, 341, .3f, .3f) == LUNA_KEDIT_TAKEN);   /* CTRL is the walk key */
    CHECK(luna_kedit_set(&e, -1, 'Q', .3f, .3f) == LUNA_KEDIT_TAKEN);   /* macro key */
    CHECK(luna_kedit_set(&e, -1, 'F', .3f, .3f) == LUNA_KEDIT_TAKEN);   /* kept button */
    CHECK(luna_kedit_set(&e, -1, 'W', .3f, .3f) == LUNA_KEDIT_BADKEY);
    CHECK(luna_kedit_set(&e, -1, 290, .3f, .3f) == LUNA_KEDIT_BADKEY);  /* F1: no name */

    /* add, move by pressing the same key elsewhere, rebind, delete */
    int j = luna_kedit_set(&e, -1, 'J', .40f, .45f);
    CHECK(j == 2 && e.n == 3);
    CHECK(luna_kedit_set(&e, -1, 'J', .41f, .46f) == j && e.n == 3 && e.b[j].x > .405f);
    int k = luna_kedit_set(&e, 1, 'K', .7f, .8f);                    /* E becomes K */
    CHECK(k == 1 && e.b[1].key == 'K' && luna_kedit_find(&e, 'E') < 0);
    CHECK(luna_kedit_set(&e, 1, 'J', .7f, .8f) >= 0);                /* K -> J drops the other J */
    CHECK(e.n == 2 && luna_kedit_find(&e, 'J') >= 0 && luna_kedit_find(&e, 'K') < 0);
    CHECK(luna_kedit_set(&e, -1, 'R', 1.5f, -2.f) >= 0);             /* clamped into the screen */
    {
        int r = luna_kedit_find(&e, 'R');
        CHECK(r >= 0 && e.b[r].x == 1.f && e.b[r].y == 0.f);
    }
    luna_kedit_remove(&e, luna_kedit_find(&e, 32));                   /* SPACE gone */
    CHECK(luna_kedit_find(&e, 32) < 0);
    e.stick_x = .2f; e.radius = .2f;

    CHECK(luna_kedit_save(&e, path));
    luna_kedit_free(&e);

    const char *out = slurp(path);
    CHECK(out != NULL);
    if (out) {
        fprintf(stderr, "--- saved ---\n%s-------------\n", out);
        CHECK(!strncmp(out, "# header comment\n", 17));                 /* order kept */
        CHECK(strstr(out, "stick 0.2000 0.7640 0.2000\n") != NULL);
        CHECK(strstr(out, "walk CTRL 0.04\n") != NULL);
        CHECK(strstr(out, "button F 0.5 0.5 0.6 0.6\n") != NULL);
        CHECK(strstr(out, "macro Q 0.1,0.1 @50 0.2,0.2\n") != NULL);
        CHECK(strstr(out, "button SPACE") == NULL);
        CHECK(strstr(out, "button J 0.7000 0.8000\n") != NULL);          /* E's line, rebound and moved */
        CHECK(strstr(out, "button R 1.0000 0.0000\n") != NULL);
        CHECK(strstr(out, "button E") == NULL && strstr(out, "button K") == NULL);
    }
    const char *orig = slurp(bak);
    CHECK(orig && strstr(orig, "button SPACE 0.922 0.665\n"));          /* first save keeps the original */

    luna_keymap m;
    CHECK(luna_keymap_load(&m, path));
    CHECK(m.count == 3);                                                 /* F (kept), J, R */

    /* a stick smaller than the walk radius is refused and the file is left alone */
    CHECK(luna_kedit_load(&e, path));
    e.radius = .03f;
    CHECK(!luna_kedit_save(&e, path));
    luna_kedit_free(&e);
    CHECK(luna_keymap_load(&m, path));

    /* from nothing: default stick, 16 buttons fit, the 17th does not */
    CHECK(luna_kedit_load(&e, "off"));
    const char keys[] = "BCEFGHIJKLMNOPQR";
    for (int i = 0; i < 16; ++i) CHECK(luna_kedit_set(&e, -1, keys[i], i / 16.f, .5f) == i);
    CHECK(luna_kedit_set(&e, -1, 'T', .5f, .5f) == LUNA_KEDIT_FULL);
    CHECK(luna_kedit_save(&e, "kedit_new.conf"));
    luna_kedit_free(&e);
    CHECK(luna_keymap_load(&m, "kedit_new.conf") && m.count == 16);

    remove(path); remove(bak); remove("kedit_new.conf");
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    fprintf(stderr, "kedit: all checks passed\n");
    return 0;
}

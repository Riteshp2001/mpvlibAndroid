/* Execute playlist enumeration and the public API with an in-memory disc.
 * Check graphics before filtering, parse counts and legacy API compatibility. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLES_FILTER_DUP_TITLE 1
#define TITLES_FILTER_DUP_CLIP 2
#define DISC_PROPERTY_MAIN_FEATURE 1
#define DISC_PROPERTY_PLAYLISTS 2
#define DIR_SEP "/"
#define BD_DEBUG(...) ((void)0)
#define X_FREE(p) do { free(p); (p) = NULL; } while (0)
#define bd_mutex_lock(p) ((void)(p))
#define bd_mutex_unlock(p) ((void)(p))
#define DISC_EVENT_START 1

typedef struct {
    unsigned duration;
    int duplicate, repeated, invalid;
    unsigned ig[2];
} entry;
typedef struct {
    entry *entries;
    unsigned count, parsed, freed, events, cursor;
    int unavailable;
} BD_DISC;
typedef BD_DISC BD_DIR_H;
typedef struct { char d_name[256]; } BD_DIRENT;
typedef struct { struct { unsigned num_ig; } stn; } MPLS_PI;
typedef struct {
    entry data;
    unsigned list_count;
    MPLS_PI play_item[2];
} MPLS_PL;
typedef struct {
    char name[11];
    unsigned ref, mpls_id, duration;
} NAV_TITLE_INFO;
typedef struct {
    unsigned count;
    NAV_TITLE_INFO *title_info;
    unsigned main_title_idx;
    uint8_t has_interactive_graphics;
} NAV_TITLE_LIST;
typedef struct {
    BD_DISC *disc;
    int mutex;
    NAV_TITLE_LIST *title_list;
    struct { unsigned num_titles; } disc_info;
} BLURAY;

static BD_DISC *active_disc;

static BD_DIR_H *disc_open_dir(BD_DISC *disc, const char *path)
{
    assert(strcmp(path, "BDMV/PLAYLIST") == 0);
    disc->cursor = 0;
    active_disc = disc;
    return disc->unavailable ? NULL : disc;
}
static void dir_close(BD_DIR_H *dir) { (void)dir; }
static int dir_read(BD_DIR_H *dir, BD_DIRENT *ent)
{
    if (dir->cursor == dir->count)
        return 1;
    snprintf(ent->d_name, sizeof(ent->d_name), "%05u.mpls", dir->cursor++);
    return 0;
}
static char *disc_property_get(BD_DISC *disc, int property)
{
    (void)disc;
    (void)property;
    return NULL;
}
static MPLS_PL *mpls_get(BD_DISC *disc, const char *name)
{
    unsigned index = (unsigned)strtoul(name, NULL, 10);
    assert(index < disc->count);
    disc->parsed++;
    if (disc->entries[index].invalid)
        return NULL;
    MPLS_PL *pl = calloc(1, sizeof(*pl));
    assert(pl);
    pl->data = disc->entries[index];
    pl->list_count = 2;
    for (unsigned i = 0; i < 2; i++)
        pl->play_item[i].stn.num_ig = pl->data.ig[i];
    return pl;
}
static void mpls_free(MPLS_PL **pl)
{
    active_disc->freed++;
    X_FREE(*pl);
}
static int _filter_dup(MPLS_PL **list, unsigned count, MPLS_PL *pl)
{
    (void)list;
    (void)count;
    return !pl->data.duplicate;
}
static int _filter_repeats(MPLS_PL *pl, unsigned repeats)
{
    assert(repeats == 2);
    return !pl->data.repeated;
}
static unsigned _pl_duration(MPLS_PL *pl) { return pl->data.duration; }
static int _pl_guess_main_title(MPLS_PL *first, MPLS_PL *second,
                                const char *first_name, const char *second_name,
                                char *known)
{
    (void)first;
    (void)second;
    (void)first_name;
    (void)second_name;
    (void)known;
    return 0;
}
static void nav_free_title_list(NAV_TITLE_LIST **list)
{
    if (*list) {
        free((*list)->title_info);
        X_FREE(*list);
    }
}
static void disc_event(BD_DISC *disc, int event, unsigned titles)
{
    (void)titles;
    assert(event == DISC_EVENT_START);
    disc->events++;
}

#include "disc-streams-under-test.h"

static void check_list(entry *entries, unsigned count, unsigned flags,
                       unsigned min_length, unsigned expected_count, int expected_ig)
{
    BD_DISC disc = {.entries = entries, .count = count};
    BLURAY bd = {.disc = &disc};
    int graphics = -1;
    assert(bd_get_titles_with_menu_info(&bd, (uint8_t)flags, min_length, &graphics) == expected_count);
    assert(graphics == expected_ig);
    assert(bd.title_list && bd.title_list->count == expected_count);
    assert(disc.parsed == count && disc.freed == count && disc.events == 1);
    nav_free_title_list(&bd.title_list);
}

int main(void)
{
    entry entries[1000] = {0};
    for (unsigned i = 0; i < 1000; i++)
        entries[i].duration = 90000;
    check_list(entries, 1000, 3, 0, 1000, 0);
    entries[999].ig[1] = 1;
    check_list(entries, 1000, 3, 0, 1000, 1);

    entries[999].duplicate = 1;
    check_list(entries, 1000, TITLES_FILTER_DUP_TITLE, 0, 999, 1);
    entries[999].duplicate = 0;
    entries[999].repeated = 1;
    check_list(entries, 1000, TITLES_FILTER_DUP_CLIP, 0, 999, 1);
    entries[999].repeated = 0;
    entries[999].duration = 45000;
    check_list(entries, 1000, 0, 2, 999, 1);
    check_list(entries, 1000, 0, 3, 0, 1);
    check_list(entries, 0, 0, 0, 0, 0);

    BD_DISC disc = {.entries = entries, .count = 1000};
    BLURAY bd = {.disc = &disc};
    assert(bd_get_titles(&bd, 0, 2) == 999);
    assert(disc.parsed == 1000 && disc.events == 1);
    NAV_TITLE_LIST *old = bd.title_list;
    disc.unavailable = 1;
    int graphics = 1;
    assert(bd_get_titles_with_menu_info(&bd, 0, 0, &graphics) == 0);
    assert(graphics == 0 && bd.title_list == old && disc.events == 1);
    graphics = 1;
    assert(bd_get_titles_with_menu_info(NULL, 0, 0, &graphics) == 0 && graphics == 0);
    assert(bd_get_titles(NULL, 0, 0) == 0);
    nav_free_title_list(&bd.title_list);

    entries[999].invalid = 1;
    disc = (BD_DISC){.entries = entries, .count = 1000};
    bd = (BLURAY){.disc = &disc};
    graphics = 1;
    assert(bd_get_titles_with_menu_info(&bd, 0, 0, &graphics) == 999 && graphics == 0);
    assert(disc.parsed == 1000 && disc.freed == 999);
    nav_free_title_list(&bd.title_list);
    puts("libbluray menu info: passed (filters, 1000-playlist parse counts, API failures/compatibility)");
    return 0;
}

/* Fault injection for the real libdvdnav VM copy and menu probe functions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum failure {
    NONE, ALLOC, PGC_NUMBER, OPEN_VTSI, READ_VTSI, SET_PGC, NO_MENU, STOP_MENU,
};

typedef struct { int closed; } resource;
typedef resource dvd_reader_t;
typedef resource ifo_handle_t;
typedef int dvdnav_logger_cb;
typedef struct vm_s {
    void *priv;
    dvdnav_logger_cb logcb;
    resource *dvd, *vmgi, *vtsi;
    struct { int vtsN, pgN; void *pgc; } state;
    int stopped;
} vm_t;
typedef struct { vm_t *vm; int vm_lock; } dvdnav_t;
typedef int dvdnav_status_t;
typedef enum { DVD_MENU_Root, DVD_MENU_Title, DVD_MENU_Escape } DVDMenuID_t;
#define DVDNAV_STATUS_OK 1
#define DVDNAV_STATUS_ERR 0

static enum failure failure;
static resource live_dvd, vmgi, vtsi, copied_vtsi;
static vm_t *live_vm;
static int failures, jumps;
static const char *test_name;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", test_name, __LINE__, #c); failures++; \
} } while (0)

static void *test_calloc(size_t count, size_t size)
{
    return failure == ALLOC ? NULL : calloc(count, size);
}

static void ifoClose(resource *handle)
{
    handle->closed++;
}

static void DVDClose(resource *handle)
{
    handle->closed++;
}

static int get_PGCN(vm_t *vm)
{
    CHECK(vm == live_vm);
    return failure == PGC_NUMBER ? 0 : 1;
}

static ifo_handle_t *ifoOpenVTSI(dvd_reader_t *reader, int vtsN)
{
    CHECK(reader == &live_dvd && vtsN == 1);
    if (reader->closed || failure == OPEN_VTSI)
        return NULL;
    return &copied_vtsi;
}

static int ifoRead_VTS_PTT_SRPT(ifo_handle_t *ifo)
{
    CHECK(ifo == &copied_vtsi);
    return failure != READ_VTSI;
}

#define ifoRead_PGCIT ifoRead_VTS_PTT_SRPT
#define ifoRead_PGCI_UT ifoRead_VTS_PTT_SRPT
#define ifoRead_VOBU_ADMAP ifoRead_VTS_PTT_SRPT
#define ifoRead_TITLE_VOBU_ADMAP ifoRead_VTS_PTT_SRPT
#define Log0(...) ((void)0)

static int set_PGCN(vm_t *vm, int pgcN)
{
    CHECK(vm != live_vm && pgcN == 1);
    return failure != SET_PGC;
}

static int vm_jump_menu(vm_t *vm, DVDMenuID_t menu)
{
    CHECK(vm != live_vm);
    CHECK(menu == DVD_MENU_Root || menu == DVD_MENU_Title);
    CHECK(!live_dvd.closed && !vmgi.closed);
    jumps++;
    vm->state.pgN = 42;
    vm->stopped = failure == STOP_MENU;
    return failure != NO_MENU;
}

static void pthread_mutex_lock(int *lock)
{
    CHECK((*lock)++ == 0);
}

static void pthread_mutex_unlock(int *lock)
{
    CHECK(--*lock == 0);
}

static void vm_close(vm_t *vm);
void vm_free_copy(vm_t *vm);
#define calloc test_calloc
#include "dvdnav-vm-copy-under-test.h"
#undef calloc

static void check_live_vm(const vm_t *before)
{
    CHECK(memcmp(live_vm, before, sizeof(*live_vm)) == 0);
    CHECK(!live_dvd.closed);
    CHECK(!vmgi.closed);
    CHECK(!vtsi.closed);
}

static void test_probe(const char *name, enum failure fail, int vtsN,
                       DVDMenuID_t menu, int has_pgc)
{
    test_name = name;
    failure = fail;
    live_dvd = vmgi = vtsi = copied_vtsi = (resource){0};
    jumps = 0;
    vm_t source = {
        .dvd = &live_dvd, .vmgi = &vmgi, .vtsi = vtsN ? &vtsi : NULL,
        .state = {.vtsN = vtsN, .pgN = 3, .pgc = has_pgc ? &vmgi : NULL},
    };
    live_vm = &source;
    vm_t before = source;
    dvdnav_t nav = {.vm = &source};
    int expected = fail == NONE && has_pgc && menu != DVD_MENU_Escape;
    CHECK(dvdnav_menu_available(&nav, menu) == expected);
    CHECK(nav.vm_lock == 0);
    check_live_vm(&before);
    int copied = has_pgc && menu != DVD_MENU_Escape &&
                 fail != ALLOC && fail != PGC_NUMBER && fail != OPEN_VTSI;
    CHECK(copied_vtsi.closed == (copied && vtsN));
    CHECK(jumps == (copied && fail != READ_VTSI && fail != SET_PGC));

    /* A failed probe must leave the live VM usable for the next menu query. */
    failure = NONE;
    copied_vtsi = (resource){0};
    source.state.pgc = &vmgi;
    before = source;
    CHECK(dvdnav_menu_available(&nav, DVD_MENU_Title) == DVDNAV_STATUS_OK);
    check_live_vm(&before);
    CHECK(copied_vtsi.closed == !!vtsN);
    CHECK(nav.vm_lock == 0);

    /* Only the owner closes its DVD and VMGI, exactly once. */
    vm_close(&source);
    CHECK(live_dvd.closed == 1 && vmgi.closed == 1 && vtsi.closed == !!vtsN);
}

int main(void)
{
    test_probe("allocation failure", ALLOC, 1, DVD_MENU_Root, 1);
    test_probe("missing PGC number", PGC_NUMBER, 1, DVD_MENU_Root, 1);
    test_probe("VTSI open failure", OPEN_VTSI, 1, DVD_MENU_Root, 1);
    test_probe("VTSI read failure", READ_VTSI, 1, DVD_MENU_Root, 1);
    test_probe("PGC restore failure", SET_PGC, 1, DVD_MENU_Root, 1);
    test_probe("menu unavailable", NO_MENU, 1, DVD_MENU_Root, 1);
    test_probe("menu stops VM", STOP_MENU, 1, DVD_MENU_Root, 1);
    test_probe("root menu", NONE, 1, DVD_MENU_Root, 1);
    test_probe("VMGM menu", NONE, 0, DVD_MENU_Title, 1);
    test_probe("unsupported menu", NONE, 1, DVD_MENU_Escape, 1);
    test_probe("no active PGC", NONE, 1, DVD_MENU_Root, 0);
    printf("dvdnav VM copy: 11 cases, %d failures\n", failures);
    return failures != 0;
}

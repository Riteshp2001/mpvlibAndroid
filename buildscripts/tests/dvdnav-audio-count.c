/* Run libdvdnav's real count and route getters against domain/IFO fixtures. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef enum { DVD_SUBTITLE_STREAM, DVD_AUDIO_STREAM } dvdnav_stream_type_t;
enum { DVD_DOMAIN_FirstPlay, DVD_DOMAIN_VTSTitle, DVD_DOMAIN_VMGM,
       DVD_DOMAIN_VTSMenu };
typedef struct { uint16_t audio_control[8]; uint32_t subp_control[32]; } pgc_t;
typedef struct { uint8_t nr_of_vtsm_audio_streams; } vtsi_mat_t;
typedef struct { uint8_t nr_of_vmgm_audio_streams; } vmgi_mat_t;
typedef struct { vtsi_mat_t *vtsi_mat; vmgi_mat_t *vmgi_mat; } ifo_handle_t;
typedef struct {
    struct { int domain; pgc_t *pgc; } state;
    ifo_handle_t *vtsi, *vmgi;
} vm_t;
typedef struct { int started, vm_lock; vm_t *vm; } dvdnav_t;

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #c); failures++; \
} } while (0)
#define printerr(...) ((void)0)
static void pthread_mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void pthread_mutex_unlock(int *lock) { assert(*lock); *lock = 0; }

#ifdef _MSC_VER
#pragma warning(push)
/* The upstream route getter narrows its -1..7 result from int to int8_t. */
#pragma warning(disable: 4244)
#endif
#include "disc-streams-under-test.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

int main(void)
{
    pgc_t pgc = {0};
    vtsi_mat_t vtsi_mat = {0};
    vmgi_mat_t vmgi_mat = {0};
    ifo_handle_t vtsi = {.vtsi_mat = &vtsi_mat};
    ifo_handle_t vmgi = {.vmgi_mat = &vmgi_mat};
    vm_t vm = {.state = {.domain = DVD_DOMAIN_VTSTitle, .pgc = &pgc},
               .vtsi = &vtsi, .vmgi = &vmgi};
    dvdnav_t nav = {.started = 1, .vm = &vm};

    CHECK(dvdnav_get_number_of_streams(&nav, DVD_AUDIO_STREAM) == 0);
    pgc.audio_control[2] = 0x8500;
    pgc.audio_control[7] = 0x8000;
    CHECK(dvdnav_get_number_of_streams(&nav, DVD_AUDIO_STREAM) == 2);
    CHECK(dvdnav_get_audio_logical_stream(&nav, 2) == 5);
    CHECK(dvdnav_get_audio_logical_stream(&nav, 0) == -1);

    const int menus[] = {DVD_DOMAIN_VTSMenu, DVD_DOMAIN_VMGM, DVD_DOMAIN_FirstPlay};
    for (unsigned i = 0; i < sizeof(menus) / sizeof(menus[0]); i++) {
        vm.state.domain = menus[i];
        vtsi_mat.nr_of_vtsm_audio_streams = 0;
        vmgi_mat.nr_of_vmgm_audio_streams = 0;
        /* Menu routing synthesizes physical 0 even when no audio exists.
         * Stale/irrelevant PGC controls must not override the domain's IFO. */
        CHECK(dvdnav_get_audio_logical_stream(&nav, 7) == 0);
        CHECK(dvdnav_get_number_of_streams(&nav, DVD_AUDIO_STREAM) == 0);
        if (menus[i] == DVD_DOMAIN_VTSMenu)
            vtsi_mat.nr_of_vtsm_audio_streams = 1;
        else
            vmgi_mat.nr_of_vmgm_audio_streams = 1;
        /* The same fallback is also valid for an authored menu audio stream. */
        CHECK(dvdnav_get_audio_logical_stream(&nav, 0) == 0);
        CHECK(dvdnav_get_number_of_streams(&nav, DVD_AUDIO_STREAM) == 1);
        CHECK(nav.vm_lock == 0);
    }

    /* Subtitle counts and invalid-state behavior keep their existing contract. */
    vm.state.domain = DVD_DOMAIN_VTSTitle;
    pgc.subp_control[31] = UINT32_C(1) << 31;
    CHECK(dvdnav_get_number_of_streams(&nav, DVD_SUBTITLE_STREAM) == 1);
    vm.state.domain = DVD_DOMAIN_VTSMenu;
    CHECK(dvdnav_get_number_of_streams(&nav, DVD_SUBTITLE_STREAM) == 1);
    vm.state.domain = DVD_DOMAIN_VMGM;
    CHECK(dvdnav_get_number_of_streams(&nav, DVD_SUBTITLE_STREAM) == -1);
    vm.state.domain = 99;
    CHECK(dvdnav_get_number_of_streams(&nav, DVD_AUDIO_STREAM) == -1);
    CHECK(dvdnav_get_number_of_streams(&nav, (dvdnav_stream_type_t)99) == -1);
    vm.state.domain = DVD_DOMAIN_VTSTitle;
    nav.started = 0;
    CHECK(dvdnav_get_number_of_streams(&nav, DVD_AUDIO_STREAM) == -1);
    nav.started = 1;
    vm.state.pgc = NULL;
    CHECK(dvdnav_get_number_of_streams(&nav, DVD_AUDIO_STREAM) == -1);
    CHECK(nav.vm_lock == 0);
    printf("libdvdnav audio count: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

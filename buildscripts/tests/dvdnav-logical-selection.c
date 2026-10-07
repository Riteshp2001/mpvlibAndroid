#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define printerr(message) ((void)(message))
enum { DVD_AUDIO_STREAM, DVD_SUBTITLE_STREAM };
enum { DVD_DOMAIN_VTSTitle, DVD_DOMAIN_VTSMenu, DVD_DOMAIN_VMGM, DVD_DOMAIN_FirstPlay };
typedef int dvdnav_stream_type_t;
typedef struct { uint16_t audio_control[8]; uint32_t subp_control[32]; } pgc_t;
typedef struct { int nr_of_vts_audio_streams, nr_of_vts_subp_streams;
  int nr_of_vtsm_audio_streams, nr_of_vtsm_subp_streams; } vtsi_mat_t;
typedef struct { int nr_of_vmgm_audio_streams, nr_of_vmgm_subp_streams; } vmgi_mat_t;
typedef struct { vtsi_mat_t *vtsi_mat; } vtsi_t;
typedef struct { vmgi_mat_t *vmgi_mat; } vmgi_t;
typedef struct { struct { int domain, AST_REG, SPST_REG; pgc_t *pgc; } state;
  vtsi_t *vtsi; vmgi_t *vmgi; } vm_t;
typedef struct { int started, vm_lock; vm_t *vm; } dvdnav_t;
static int checks, failures, lock_depth;
#define CHECK(condition) do { checks++; if (!(condition)) { \
  fprintf(stderr, "line %d: %s\n", __LINE__, #condition); failures++; } } while (0)
static void pthread_mutex_lock(int *lock) { CHECK(*lock == 0); *lock = ++lock_depth; }
static void pthread_mutex_unlock(int *lock) { CHECK(*lock == 1); *lock = --lock_depth; }
static int vm_get_video_aspect(vm_t *vm) { (void)vm; return 3; }
#include "dvdnav-logical-selection-under-test.h"

int main(void) {
  pgc_t pgc = {0};
  vtsi_mat_t vtsi_mat = {8, 32, 1, 1};
  vmgi_mat_t vmgi_mat = {0, 1};
  vtsi_t vtsi = {&vtsi_mat}; vmgi_t vmgi = {&vmgi_mat};
  vm_t vm = {{DVD_DOMAIN_VTSTitle, 6, 31, &pgc}, &vtsi, &vmgi};
  dvdnav_t dvd = {1, 0, &vm};
  pgc.audio_control[2] = pgc.audio_control[6] = 0x8000;
  pgc.subp_control[7] = pgc.subp_control[31] = 0x80090000;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == 6);
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == 31);
  vm.state.AST_REG = 2; vm.state.SPST_REG = 7 | 0x40;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == 2);
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == 7);
  vm.state.AST_REG = 5; vm.state.SPST_REG = 4;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == 2);
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == 7);
  pgc.audio_control[2] = 0; pgc.subp_control[7] = 0;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == 6);
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == 31);
  pgc.audio_control[6] = 0; pgc.subp_control[31] = 0;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == -1);
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == -1);
  pgc.audio_control[2] = pgc.audio_control[6] = 0x8000;
  vm.state.AST_REG = 6; vtsi_mat.nr_of_vts_audio_streams = 3;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == -1);
  vtsi_mat.nr_of_vts_audio_streams = 9;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == -1);
  vtsi_mat.nr_of_vts_audio_streams = 8;
  vm.state.AST_REG = 65535; vm.state.SPST_REG = 65535;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == 2);
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == -1);
  vm.state.domain = DVD_DOMAIN_VTSMenu;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == 0);
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == 0);
  vtsi_mat.nr_of_vtsm_audio_streams = 0;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == -1);
  vm.state.domain = DVD_DOMAIN_VMGM;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_AUDIO_STREAM) == -1);
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == 0);
  vm.state.domain = DVD_DOMAIN_FirstPlay;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == 0);
  vm.state.pgc = NULL;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == -1);
  vm.state.pgc = &pgc; vm.vmgi = NULL;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == -1);
  vm.vmgi = &vmgi; dvd.started = 0;
  CHECK(dvdnav_get_active_logical_stream(&dvd, DVD_SUBTITLE_STREAM) == -1);
  CHECK(dvdnav_get_active_logical_stream(&dvd, 2) == -1);
  CHECK(dvd.vm_lock == 0 && lock_depth == 0);
  printf("%d checks, %d failures\n", checks, failures);
  return failures != 0;
}

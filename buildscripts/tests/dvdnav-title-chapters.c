/* Compile libdvdnav's actual chapter catalog and time conversion functions.
 * Authored PGC/PTT fixtures use the pinned libdvdread structures. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dvdread/ifo_read.h>
#include "dvdnav/dvdnav.h"
#include "vm/decoder.h"
#include "vm/vm.h"
#include "dvdnav_internal.h"

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", name, __LINE__, #c); failures++; } } while (0)
static int checks, failures, closes;
static const char *name;
static dvdnav_t nav;
static vm_t vm;
static ifo_handle_t vmgi, vtsi;
static tt_srpt_t titles;
static title_info_t fixture_title;
static vts_ptt_srpt_t ptts;
static ttu_t ttu;
static ptt_info_t fixture_parts[4];
static pgcit_t pgcit;
static pgci_srp_t pointers[2];
static pgc_t pgcs[2];
static uint8_t programs[2][4];
static cell_playback_t cells[2][256];

int vm_start(vm_t *value) { CHECK(value == &vm); return 1; }
ifo_handle_t *vm_get_title_ifo(vm_t *value, uint32_t number) {
  CHECK(value == &vm && number == 1);
  return &vtsi;
}
void vm_ifo_close(ifo_handle_t *value) { CHECK(value == &vtsi); closes++; }

#include "dvdnav-title-chapters-under-test.h"

static void duration(cell_playback_t *cell, unsigned seconds) {
  cell->playback_time.second = (uint8_t)((seconds / 10) * 16 + seconds % 10);
  cell->playback_time.frame_u = 0x40;
}

static void setup(const char *case_name) {
  name = case_name;
  memset(&vm, 0, sizeof(vm)); memset(&vmgi, 0, sizeof(vmgi));
  memset(&vtsi, 0, sizeof(vtsi)); memset(pgcs, 0, sizeof(pgcs));
  memset(cells, 0, sizeof(cells)); memset(fixture_parts, 0, sizeof(fixture_parts));
  memset(programs, 0, sizeof(programs)); closes = 0;
  nav.vm = &vm; nav.started = 1; vm.vmgi = &vmgi;
  vmgi.tt_srpt = &titles; titles.nr_of_srpts = 1; titles.title = &fixture_title;
  fixture_title.vts_ttn = 1;
  vtsi.vts_ptt_srpt = &ptts; ptts.nr_of_srpts = 1; ptts.title = &ttu;
  ttu.ptt = fixture_parts;
  vtsi.vts_pgcit = &pgcit; pgcit.nr_of_pgci_srp = 2;
  pgcit.last_byte = 2047; pgcit.pgci_srp = pointers;
  for (int i=0; i<2; i++) {
    pointers[i].pgc_start_byte = 16 + i*512; pointers[i].pgc = &pgcs[i];
    pgcs[i].program_map = programs[i]; pgcs[i].cell_playback = cells[i];
  }
}

static void chapter_count(int count) { fixture_title.nr_of_ptts = (uint16_t)count; ttu.nr_of_ptts = (uint16_t)count; }

static void check_catalog(const uint64_t *expected, uint32_t count) {
  uint64_t *times = NULL, total = 0;
  CHECK(dvdnav_describe_title_chapters(&nav, 1, &times, &total) == count);
  CHECK(times != NULL); CHECK(total == expected[count-1] * 90000);
  if (times) for (uint32_t i=0; i<count; i++) CHECK(times[i] == expected[i] * 90000);
  CHECK(closes == 1);
  free(times);
}

static void single_pgc(void) {
  setup("single PGC: last program has three cells");
  chapter_count(2); pgcs[0].nr_of_programs = 2; pgcs[0].nr_of_cells = 4;
  programs[0][0] = 1; programs[0][1] = 2;
  fixture_parts[0] = (ptt_info_t){1,1}; fixture_parts[1] = (ptt_info_t){1,2};
  duration(&cells[0][0],2); duration(&cells[0][1],3);
  duration(&cells[0][2],5); duration(&cells[0][3],7);
  const uint64_t expected[] = {2,17}; check_catalog(expected,2);
}

static void multi_pgc(int angle) {
  setup(angle ? "multi PGC: alternate angle not counted twice" : "multi PGC: last program has two cells");
  chapter_count(3);
  pgcs[0].nr_of_programs = 2; pgcs[0].nr_of_cells = 2;
  programs[0][0] = 1; programs[0][1] = 2;
  pgcs[1].nr_of_programs = 1; pgcs[1].nr_of_cells = angle ? 3 : 2;
  programs[1][0] = 1;
  fixture_parts[0] = (ptt_info_t){1,1}; fixture_parts[1] = (ptt_info_t){1,2}; fixture_parts[2] = (ptt_info_t){2,1};
  duration(&cells[0][0],2); duration(&cells[0][1],3); duration(&cells[1][0],5);
  duration(&cells[1][angle ? 2 : 1],7);
  if (angle) {
    cells[1][0].block_type = BLOCK_TYPE_ANGLE_BLOCK; cells[1][0].block_mode = BLOCK_MODE_FIRST_CELL;
    cells[1][1].block_type = BLOCK_TYPE_ANGLE_BLOCK; cells[1][1].block_mode = BLOCK_MODE_LAST_CELL;
    duration(&cells[1][1],5);
  }
  const uint64_t expected[] = {2,5,17}; check_catalog(expected,3);
}

static void maximum_cells(void) {
  setup("last program reaches cell 255"); chapter_count(1);
  pgcs[0].nr_of_programs = 1; pgcs[0].nr_of_cells = 255;
  programs[0][0] = 1; fixture_parts[0] = (ptt_info_t){1,1};
  for (int i=0; i<255; i++) duration(&cells[0][i],1);
  duration(&cells[0][255],59);
  const uint64_t expected[] = {255}; check_catalog(expected,1);
}

static void invalid_program(unsigned program) {
  setup("invalid PTT program is rejected and IFO released"); chapter_count(1);
  pgcs[0].nr_of_programs = 1; pgcs[0].nr_of_cells = 1;
  programs[0][0] = 1; fixture_parts[0] = (ptt_info_t){1,(uint16_t)program};
  duration(&cells[0][0],2);
  uint64_t *times = (uint64_t *)&nav, total = 99;
  CHECK(dvdnav_describe_title_chapters(&nav,1,&times,&total) == 0);
  CHECK(times == NULL); CHECK(total == 0); CHECK(closes == 1);
}

int main(void) {
  pthread_mutex_init(&nav.vm_lock,NULL);
  single_pgc(); multi_pgc(0); multi_pgc(1); maximum_cells();
  invalid_program(0); invalid_program(2);
  pthread_mutex_destroy(&nav.vm_lock);
  printf("libdvdnav title chapters: %d checks, %d failures\n",checks,failures);
  return failures ? 1 : 0;
}

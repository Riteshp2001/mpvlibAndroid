/* Real IFO/VM integration; no fake parser, navigation or clock implementation. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#ifndef _WIN32
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dvdread/ifo_read.h>
#include "dvdnav/title_layout.h"
#include "vm/decoder.h"
#include "vm/vm.h"
#include "dvdnav_internal.h"

static unsigned checks, failures;
static int fail_title_vob_reads;
static int fail_ifo_reads;
static unsigned rejected_ifo_reads;
static unsigned rejected_vob_reads;
static int64_t title_vob_offset;
#define CHECK(x) do { checks++; if (!(x)) { \
  fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define TICKS(s) ((uint64_t)(s) * 90000)

typedef struct {
  uint32_t title, title_set, pgcn, program;
  uint64_t pgc_time, title_time;
} mapped_position_t;

static dvdnav_status_t capture_layout_position(
    dvdnav_t *dvd, const dvdnav_title_layout_t *layout, mapped_position_t *output) {
  dvdnav_title_position_t raw;
  uint64_t title_time;
  if (!layout || dvdnav_get_current_title_position(dvd, &raw) != DVDNAV_STATUS_OK ||
      raw.title_set != layout->title_set ||
      dvdnav_title_layout_position(layout, raw.title, raw.pgcn, raw.pgc_time,
                                  &title_time) != DVDNAV_STATUS_OK)
    return DVDNAV_STATUS_ERR;
  output->title = raw.title;
  output->title_set = raw.title_set;
  output->pgcn = raw.pgcn;
  output->program = raw.program;
  output->pgc_time = raw.pgc_time;
  output->title_time = title_time;
  return DVDNAV_STATUS_OK;
}

static int seek_file(void *context, uint64_t offset) {
  return _fseeki64((FILE *)context, (int64_t)offset, SEEK_SET);
}

static int read_file(void *context, void *buffer, int bytes) {
  if (fail_ifo_reads) { rejected_ifo_reads++; return -1; }
  if (fail_title_vob_reads && _ftelli64(context) >= title_vob_offset) {
    rejected_vob_reads++;
    return -1;
  }
  size_t n = fread(buffer, 1, (size_t)bytes, context);
  return ferror(context) ? -1 : (int)n;
}

static dvdnav_t *open_fixture(const char *directory, const char *name, FILE **file) {
  char path[1024];
  dvdnav_t *dvd = NULL;
  dvdnav_stream_cb cb = {seek_file, read_file, NULL};
  snprintf(path, sizeof(path), "%s/%s.iso", directory, name);
  *file = fopen(path, "rb");
  CHECK(*file != NULL);
  if (!*file)
    exit(2);
  CHECK(dvdnav_open_stream(&dvd, *file, &cb) == DVDNAV_STATUS_OK);
  if (!dvd)
    exit(2);
  return dvd;
}

static void test_linear(const char *directory) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "linear", &file);
  dvdnav_title_layout_t *layout = NULL;
  uint32_t index = 99;
  uint64_t local = 99, global = 99, *chapters = NULL, duration;
  int32_t title, pgc, program;
  uint8_t buffer[2048];
  unsigned cells = 0;
  int stopped = 0;
  mapped_position_t position, captured;
  mapped_position_t first_pgc2_block = {0};
  int captured_pgc2_block = 0;
  const uint64_t expected[] = {TICKS(0), TICKS(2), TICKS(5), TICKS(10)};
  CHECK(!dvd->started);
  CHECK(dvdnav_get_title_layout(dvd, 0, &layout) == DVDNAV_TITLE_LAYOUT_ERROR);
  CHECK(layout == NULL);
  CHECK(dvdnav_get_title_layout(dvd, 2, &layout) == DVDNAV_TITLE_LAYOUT_ERROR);
  CHECK(layout == NULL);
  memset(&position, 0xa5, sizeof(position));
  captured = position;
  CHECK(capture_layout_position(dvd, layout, &position) == DVDNAV_STATUS_ERR);
  CHECK(memcmp(&position, &captured, sizeof(position)) == 0);
  CHECK(dvdnav_get_title_layout(dvd, 1, &layout) == DVDNAV_TITLE_LAYOUT_LINEAR);
  CHECK(!dvd->started);
  CHECK(layout != NULL);
  if (!layout)
    exit(2);
  CHECK(capture_layout_position(dvd, layout, &position) == DVDNAV_STATUS_ERR);
  CHECK(memcmp(&position, &captured, sizeof(position)) == 0);
  CHECK(layout->title == 1 && layout->title_set == 1);
  CHECK(layout->duration == TICKS(17) && layout->span_count == 2);
  CHECK(layout->spans[0].pgcn == 1 && layout->spans[0].entry_program == 1);
  CHECK(layout->spans[0].title_start == 0 && layout->spans[0].pgc_start == 0);
  CHECK(layout->spans[0].duration == TICKS(5));
  CHECK(layout->spans[1].pgcn == 2 && layout->spans[1].entry_program == 1);
  CHECK(layout->spans[1].title_start == TICKS(5) && layout->spans[1].pgc_start == 0);
  CHECK(layout->spans[1].duration == TICKS(12));
  CHECK(dvdnav_title_layout_seek(layout, TICKS(11), &index, &local) == DVDNAV_STATUS_OK);
  CHECK(index == 1 && local == TICKS(6));
  CHECK(dvdnav_title_layout_seek(layout, TICKS(3), &index, &local) == DVDNAV_STATUS_OK);
  CHECK(index == 0 && local == TICKS(3));
  CHECK(dvdnav_title_layout_seek(layout, TICKS(5), &index, &local) == DVDNAV_STATUS_OK);
  CHECK(index == 1 && local == 0);
  CHECK(dvdnav_title_layout_position(layout, 1, 2, TICKS(6), &global) == DVDNAV_STATUS_OK);
  CHECK(global == TICKS(11));
  CHECK(dvdnav_title_layout_position(layout, 1, 1, TICKS(5), &global) == DVDNAV_STATUS_OK);
  CHECK(global == TICKS(5));
  CHECK(dvdnav_title_layout_position(layout, 1, 2, 0, &global) == DVDNAV_STATUS_OK);
  CHECK(global == TICKS(5));
  CHECK(dvdnav_title_layout_position(layout, 1, 2, TICKS(12), &global) == DVDNAV_STATUS_OK);
  CHECK(global == TICKS(17));
  CHECK(dvdnav_title_layout_position(layout, 1, 2, TICKS(12)+1, &global) == DVDNAV_STATUS_ERR);
  CHECK(global == TICKS(17));
  CHECK(dvdnav_title_layout_position(layout, 2, 2, 0, &global) == DVDNAV_STATUS_ERR);
  CHECK(dvdnav_title_layout_position(layout, 1, 3, 0, &global) == DVDNAV_STATUS_ERR);
  index = 99; local = 99;
  CHECK(dvdnav_title_layout_seek(layout, TICKS(17), &index, &local) == DVDNAV_STATUS_ERR);
  CHECK(index == 99 && local == 99);
  CHECK(dvdnav_title_layout_seek(layout, UINT64_MAX, &index, &local) == DVDNAV_STATUS_ERR);
  CHECK(dvdnav_describe_title_chapters(dvd, 1, &chapters, &duration) == 4);
  CHECK(duration == TICKS(17));
  CHECK(chapters && chapters[0] == TICKS(2) && chapters[1] == TICKS(5) &&
        chapters[2] == TICKS(10) && chapters[3] == TICKS(17));
  free(chapters);
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  for (unsigned step = 0; step < 40000; step++) {
    int32_t event, length;
    if (dvdnav_get_next_block(dvd, buffer, &event, &length) != DVDNAV_STATUS_OK) {
      CHECK(0); break;
    }
    if (event == DVDNAV_CELL_CHANGE) {
      CHECK(dvdnav_current_title_program(dvd, &title, &pgc, &program) == DVDNAV_STATUS_OK);
      local = (uint64_t)dvdnav_get_current_time(dvd);
      CHECK(dvdnav_title_layout_position(layout, (uint32_t)title, (uint32_t)pgc,
                                        local, &global) == DVDNAV_STATUS_OK);
      CHECK(capture_layout_position(dvd, layout, &position) == DVDNAV_STATUS_OK);
      CHECK(position.title == (uint32_t)title && position.pgcn == (uint32_t)pgc &&
            position.program == (uint32_t)program && position.pgc_time == local &&
            position.title_time == global);
      if (cells == 2)
        captured = position;
      if (cells == 3) {
        CHECK(captured.pgcn == 2 && captured.pgc_time == 0 && captured.title_time == TICKS(5));
        CHECK(position.title_time == TICKS(10));
      }
      printf("VM title=%d pgc=%d program=%d local=%llu title=%llu ticks\n",
             title, pgc, program, (unsigned long long)local, (unsigned long long)global);
      CHECK(cells < 4 && global == expected[cells]);
      cells++;
    } else if (event == DVDNAV_NAV_PACKET) {
      /* This 17s metadata fixture retains 60s VOB bytes. Only the first block
       * in PGC2 has the deliberately matching zero cell elapsed time. */
      if (dvd->vm->state.pgcN == 2 && !captured_pgc2_block) {
        CHECK(capture_layout_position(dvd, layout, &position) == DVDNAV_STATUS_OK);
        first_pgc2_block = position;
        captured_pgc2_block = 1;
      }
    } else if (event == DVDNAV_WAIT) {
      CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
    } else if (event == DVDNAV_STILL_FRAME) {
      CHECK(dvdnav_still_skip(dvd) == DVDNAV_STATUS_OK);
    } else if (event == DVDNAV_STOP) {
      stopped = 1; break;
    }
  }
  CHECK(cells == 4 && stopped);
  CHECK(captured_pgc2_block);
  if (captured_pgc2_block) {
    CHECK(first_pgc2_block.pgcn == 2 && first_pgc2_block.pgc_time == 0);
    CHECK(first_pgc2_block.title_time == TICKS(5));
  }
  /* No accumulated segment offset survives a seek/restart at title zero. */
  CHECK(dvdnav_title_layout_position(layout, 1, 1, 0, &global) == DVDNAV_STATUS_OK);
  CHECK(global == 0);
  CHECK(dvdnav_title_layout_seek(layout, 0, &index, &local) == DVDNAV_STATUS_OK);
  CHECK(index == 0 && local == 0);
  dvdnav_close(dvd);
  fclose(file);
  /* Snapshot owns its storage; it remains usable after IFO and VM close. */
  CHECK(dvdnav_title_layout_position(layout, 1, 2, TICKS(6), &global) == DVDNAV_STATUS_OK);
  CHECK(global == TICKS(11));
  dvdnav_free_title_layout(layout);
}

static int next_cell(dvdnav_t *dvd) {
  uint8_t buffer[2048];
  for (unsigned i = 0; i < 40000; i++) {
    int32_t event, length;
    if (dvdnav_get_next_block(dvd, buffer, &event, &length) != DVDNAV_STATUS_OK)
      return 0;
    if (event == DVDNAV_CELL_CHANGE)
      return 1;
    if (event == DVDNAV_STOP)
      return 0;
    if (event == DVDNAV_WAIT)
      dvdnav_wait_skip(dvd);
    if (event == DVDNAV_STILL_FRAME)
      dvdnav_still_skip(dvd);
  }
  return 0;
}

static int next_nav(dvdnav_t *dvd, uint8_t buffer[2048]) {
  for (unsigned i = 0; i < 40000; i++) {
    int32_t event, length;
    if (dvdnav_get_next_block(dvd, buffer, &event, &length) != DVDNAV_STATUS_OK)
      return 0;
    if (event == DVDNAV_NAV_PACKET)
      return 1;
    if (event == DVDNAV_STOP)
      return 0;
    if (event == DVDNAV_WAIT)
      dvdnav_wait_skip(dvd);
    if (event == DVDNAV_STILL_FRAME)
      dvdnav_still_skip(dvd);
  }
  return 0;
}

static void test_atomic_seek(const char *directory) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "continuous-pts", &file);
  dvdnav_title_layout_t *layout = NULL;
  dvdnav_title_position_t raw;
  dvd_state_t old_state;
  vm_position_t old_position;
  int old_hop, old_verified_hop, old_verified_angle;
  int64_t old_cell_time;
  uint8_t buffer[2048];
  uint64_t global;
  CHECK(dvdnav_get_title_layout(dvd, 1, &layout) == DVDNAV_TITLE_LAYOUT_LINEAR);
  CHECK(dvdnav_set_PGC_positioning_flag(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_title_time_search_floor(dvd, layout, TICKS(36)) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
  CHECK(raw.pgcn == 2 && raw.pgc_time == TICKS(6));
  CHECK(dvdnav_title_layout_position(layout, raw.title, raw.pgcn, raw.pgc_time,
                                   &global) == DVDNAV_STATUS_OK);
  CHECK(global == TICKS(36));
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
  CHECK(raw.pgcn == 2 && raw.pgc_time == TICKS(6));
  CHECK(dvdnav_get_current_nav_dsi(dvd)->dsi_gi.c_eltm.second == 6);
  CHECK(dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm == TICKS(36) + 48600);
  printf("Atomic seek global36s: first encoded NAV pgc=%u local=%llu PTS=%u\n",
         raw.pgcn, (unsigned long long)raw.pgc_time,
         dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm);
  old_state = dvd->vm->state;
  old_position = dvd->position_current;
  old_hop = dvd->vm->hop_channel;
  old_cell_time = dvd->cur_cell_time;
  old_verified_hop = dvd->verified_seek_hop;
  old_verified_angle = dvd->verified_seek_angle;
  fail_title_vob_reads = 1;
  CHECK(dvdnav_title_time_search_floor(dvd, layout, TICKS(3)) == DVDNAV_STATUS_ERR);
  fail_title_vob_reads = 0;
  CHECK(rejected_vob_reads > 0);
  CHECK(memcmp(&old_state, &dvd->vm->state, sizeof(old_state)) == 0);
  CHECK(memcmp(&old_position, &dvd->position_current, sizeof(old_position)) == 0);
  CHECK(dvd->vm->hop_channel == old_hop && dvd->cur_cell_time == old_cell_time);
  CHECK(dvd->verified_seek_hop == old_verified_hop && dvd->verified_seek_angle == old_verified_angle);
  CHECK(dvdnav_title_time_search_floor(dvd, layout, TICKS(60)) == DVDNAV_STATUS_ERR);
  CHECK(memcmp(&old_state, &dvd->vm->state, sizeof(old_state)) == 0);
  CHECK(dvd->vm->hop_channel == old_hop && dvd->cur_cell_time == old_cell_time);
  CHECK(dvdnav_title_time_search_floor(dvd, layout, TICKS(3)) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
  CHECK(raw.pgcn == 1 && raw.pgc_time == TICKS(3));
  CHECK(dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm == TICKS(3) + 48600);
  printf("Atomic seek global3s: first encoded NAV pgc=%u local=%llu PTS=%u; rollback checks passed\n",
         raw.pgcn, (unsigned long long)raw.pgc_time,
         dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm);
  dvdnav_free_title_layout(layout);
  dvdnav_close(dvd);
  fclose(file);
}

static void check_pgc_seek_rejected(
    dvdnav_t *dvd, const dvdnav_title_position_t *scope, uint64_t time) {
  dvd_state_t state = dvd->vm->state;
  vm_position_t position = dvd->position_current;
  int hop = dvd->vm->hop_channel;
  int verified_hop = dvd->verified_seek_hop;
  int verified_angle = dvd->verified_seek_angle;
  int64_t cell_time = dvd->cur_cell_time;
  CHECK(dvdnav_pgc_time_search_floor(dvd, scope, time) == DVDNAV_STATUS_ERR);
  CHECK(memcmp(&state, &dvd->vm->state, sizeof(state)) == 0);
  CHECK(memcmp(&position, &dvd->position_current, sizeof(position)) == 0);
  CHECK(hop == dvd->vm->hop_channel && cell_time == dvd->cur_cell_time);
  CHECK(verified_hop == dvd->verified_seek_hop &&
        verified_angle == dvd->verified_seek_angle);
}

static void test_pgc_scope(const char *directory) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "continuous-pts", &file);
  dvdnav_title_layout_t *layout = NULL;
  dvdnav_title_position_t first, raw, altered;
  dvdnav_chapter_t *chapters = NULL;
  uint32_t count = 0;
  uint8_t buffer[2048];
  CHECK(dvdnav_get_title_layout(dvd, 1, &layout) == DVDNAV_TITLE_LAYOUT_LINEAR);
  CHECK(dvdnav_get_title_chapters(dvd, layout, &chapters, &count) == DVDNAV_STATUS_OK);
  CHECK(count == 4);
  for (uint32_t i = 0; i < count; i++)
    CHECK(chapters[i].chapter == i + 1 && chapters[i].time == TICKS(i * 15));
  dvdnav_free_chapters(chapters);
  CHECK(dvdnav_set_PGC_positioning_flag(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &first) == DVDNAV_STATUS_OK);
  CHECK(first.title == 1 && first.title_set == 1 && first.pgcn == 1);
  CHECK(first.pgc_duration == TICKS(30) && first.pgc_time == 0);
  CHECK(dvdnav_get_current_pgc_chapters(dvd, &first, &chapters, &count) == DVDNAV_STATUS_OK);
  CHECK(count == 2 && chapters[0].chapter == 1 && chapters[0].time == 0 &&
        chapters[1].chapter == 2 && chapters[1].time == TICKS(15));
  dvdnav_free_chapters(chapters);
  /* Cursor and recorded duration are not part of the scope identity. */
  altered = first; altered.program = 99; altered.pgc_time = UINT64_MAX;
  altered.pgc_duration = 0;
  CHECK(dvdnav_pgc_time_search_floor(dvd, &altered, TICKS(6)) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
  CHECK(raw.pgcn == 1 && raw.pgc_time == TICKS(6) && raw.pgc_duration == TICKS(30));
  CHECK(dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm == TICKS(6) + 48600);
  check_pgc_seek_rejected(dvd, &first, TICKS(30));
  check_pgc_seek_rejected(dvd, &first, UINT64_MAX);
  altered = first; altered.title = 2;
  check_pgc_seek_rejected(dvd, &altered, TICKS(3));
  altered = first; altered.title_set = 2;
  check_pgc_seek_rejected(dvd, &altered, TICKS(3));
  fail_title_vob_reads = 1;
  check_pgc_seek_rejected(dvd, &first, TICKS(3));
  fail_title_vob_reads = 0;
  CHECK(rejected_vob_reads > 0);
  CHECK(dvdnav_title_time_search_floor(dvd, layout, TICKS(36)) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
  CHECK(raw.pgcn == 2 && raw.pgc_time == TICKS(6) && raw.pgc_duration == TICKS(30));
  altered = first; altered.pgcn = UINT16_MAX;
  check_pgc_seek_rejected(dvd, &altered, TICKS(3));
  CHECK(dvdnav_get_current_pgc_chapters(dvd, &altered, &chapters, &count) == DVDNAV_STATUS_ERR);
  CHECK(chapters == NULL && count == 0);
  CHECK(dvdnav_get_current_pgc_chapters(dvd, &raw, &chapters, &count) == DVDNAV_STATUS_OK);
  CHECK(count == 2 && chapters[0].chapter == 3 && chapters[0].time == 0 &&
        chapters[1].chapter == 4 && chapters[1].time == TICKS(15));
  dvdnav_free_chapters(chapters);
  CHECK(dvdnav_pgc_time_search_floor(dvd, &raw, TICKS(3)) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm == TICKS(33) + 48600);
  CHECK(dvdnav_menu_call(dvd, DVD_MENU_Root) == DVDNAV_STATUS_OK);
  altered = raw;
  CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_ERR);
  CHECK(memcmp(&raw, &altered, sizeof(raw)) == 0);
  check_pgc_seek_rejected(dvd, &raw, 0);
  CHECK(dvdnav_get_current_pgc_chapters(dvd, &raw, &chapters, &count) == DVDNAV_STATUS_ERR);
  CHECK(chapters == NULL && count == 0);
  CHECK(dvdnav_menu_call(dvd, DVD_MENU_Escape) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
  CHECK(raw.pgcn == 2 && raw.pgc_duration == TICKS(30));
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
  CHECK(raw.pgcn == 1 && raw.pgc_time == 0 && raw.pgc_duration == TICKS(30));
  dvdnav_free_title_layout(layout);
  dvdnav_close(dvd); fclose(file);
}

static void test_unsupported_route_pgc(const char *directory, const char *name,
                                       uint32_t pgcn, uint64_t duration) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, name, &file);
  dvdnav_title_layout_t *layout = NULL;
  dvdnav_title_position_t raw;
  dvdnav_chapter_t *chapters = NULL;
  uint32_t count = 0;
  CHECK(dvdnav_get_title_layout(dvd, 1, &layout) == DVDNAV_TITLE_LAYOUT_UNSUPPORTED);
  CHECK(layout == NULL);
  CHECK(dvdnav_program_play(dvd, 1, (int)pgcn, 1) == DVDNAV_STATUS_OK);
  CHECK(next_cell(dvd));
  CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
  CHECK(raw.pgcn == pgcn && raw.pgc_duration == duration);
  CHECK(dvdnav_get_current_pgc_chapters(dvd, &raw, &chapters, &count) == DVDNAV_STATUS_OK);
  CHECK(count > 0 && chapters[0].chapter == (pgcn == 1 ? 1u : 3u) && chapters[0].time == 0);
  dvdnav_free_chapters(chapters);
  if (strcmp(name, "continuous-commands") == 0) {
    uint8_t buffer[2048];
    CHECK(next_nav(dvd, buffer));
    CHECK(dvdnav_set_PGC_positioning_flag(dvd, 1) == DVDNAV_STATUS_OK);
    CHECK(dvdnav_pgc_time_search_floor(dvd, &raw, TICKS(6)) == DVDNAV_STATUS_OK);
    CHECK(next_nav(dvd, buffer));
    CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
    CHECK(raw.pgcn == 1 && raw.pgc_time == TICKS(6));
    CHECK(dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm == TICKS(6) + 48600);
  }
  printf("PGC scope %s: pgc=%u duration=%llu chapters=%u; whole layout unsupported\n",
         name, raw.pgcn, (unsigned long long)raw.pgc_duration, count);
  dvdnav_close(dvd); fclose(file);
}

static void test_route_chapters(const char *directory, const char *name,
                                uint32_t count_expected, uint32_t first_chapter) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, name, &file);
  dvdnav_title_layout_t *layout = NULL;
  dvdnav_chapter_t *chapters = NULL;
  uint32_t count = 0;
  CHECK(dvdnav_get_title_layout(dvd, 1, &layout) == DVDNAV_TITLE_LAYOUT_LINEAR);
  CHECK(dvdnav_get_title_chapters(dvd, layout, &chapters, &count) == DVDNAV_STATUS_OK);
  CHECK(count == count_expected && chapters[0].chapter == first_chapter && chapters[0].time == 0);
  dvdnav_free_chapters(chapters);
  dvdnav_free_title_layout(layout);
  dvdnav_close(dvd); fclose(file);
}

static void test_pgc_natural(const char *directory) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "continuous-nop", &file);
  dvdnav_title_layout_t *layout = NULL;
  dvdnav_title_position_t raw;
  uint8_t buffer[2048];
  uint32_t cells = 0;
  int stopped = 0, saw_pgc2_nav = 0;
  CHECK(dvdnav_get_title_layout(dvd, 1, &layout) == DVDNAV_TITLE_LAYOUT_UNSUPPORTED);
  CHECK(layout == NULL);
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  for (unsigned step = 0; step < 40000; step++) {
    int32_t event, length;
    if (dvdnav_get_next_block(dvd, buffer, &event, &length) != DVDNAV_STATUS_OK) {
      CHECK(0); break;
    }
    if (event == DVDNAV_CELL_CHANGE) {
      dvdnav_chapter_t *chapters = NULL;
      uint32_t count = 0;
      CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
      CHECK(raw.pgcn == cells / 2 + 1 && raw.pgc_time == TICKS((cells % 2) * 15));
      CHECK(raw.pgc_duration == TICKS(30));
      CHECK(dvdnav_get_current_pgc_chapters(dvd, &raw, &chapters, &count) == DVDNAV_STATUS_OK);
      CHECK(count == 2 && chapters[0].chapter == (cells / 2) * 2 + 1 &&
            chapters[1].chapter == (cells / 2) * 2 + 2 &&
            chapters[0].time == 0 && chapters[1].time == TICKS(15));
      dvdnav_free_chapters(chapters);
      cells++;
    } else if (event == DVDNAV_NAV_PACKET) {
      CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
      if (raw.pgcn == 2 && !saw_pgc2_nav) {
        CHECK(raw.pgc_time == 0);
        CHECK(dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm == TICKS(30) + 48600);
        saw_pgc2_nav = 1;
      }
    } else if (event == DVDNAV_WAIT) {
      CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
    } else if (event == DVDNAV_STILL_FRAME) {
      CHECK(dvdnav_still_skip(dvd) == DVDNAV_STATUS_OK);
    } else if (event == DVDNAV_STOP) {
      stopped = 1; break;
    }
  }
  CHECK(cells == 4 && stopped && saw_pgc2_nav);
  printf("Natural current-PGC NOP route: four 15s cells, PGC1/2 each 30s, authored PTT1/2 then3/4\n");
  dvdnav_close(dvd); fclose(file);
}

static void test_selected_angle(const char *directory) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "unequal-angle-nav", &file);
  dvdnav_title_position_t first, second;
  uint8_t buffer[2048];
  CHECK(dvdnav_set_PGC_positioning_flag(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_program_play(dvd, 1, 2, 1) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &first) == DVDNAV_STATUS_OK);
  CHECK(first.pgc_duration == TICKS(5));
  CHECK(dvdnav_angle_change(dvd, 2) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_program_play(dvd, 1, 2, 1) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvd->vm->state.cellN == 2 && dvd->vm->state.AGL_REG == 2);
  CHECK(dvdnav_get_current_title_position(dvd, &second) == DVDNAV_STATUS_OK);
  CHECK(second.pgc_duration == TICKS(7));
  CHECK(dvdnav_pgc_time_search_floor(dvd, &second, 495000) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_get_current_title_position(dvd, &second) == DVDNAV_STATUS_OK);
  CHECK(second.pgc_time > TICKS(5) && second.pgc_time <= 495000);
  CHECK(second.pgc_duration == TICKS(7));
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &second) == DVDNAV_STATUS_OK);
  CHECK(second.pgc_time > TICKS(5) && second.pgc_time <= 495000);
  printf("Selected angle 2: PGC clock=%llu duration=%llu NAVelapsed=%lld\n",
         (unsigned long long)second.pgc_time,
         (unsigned long long)second.pgc_duration,
         (long long)dvdnav_convert_time(&dvdnav_get_current_nav_dsi(dvd)->dsi_gi.c_eltm));
  check_pgc_seek_rejected(dvd, &first, TICKS(3));
  check_pgc_seek_rejected(dvd, &second, TICKS(7));
  fail_title_vob_reads = 1;
  check_pgc_seek_rejected(dvd, &second, 495000);
  fail_title_vob_reads = 0;
  CHECK(dvdnav_angle_change(dvd, 1) == DVDNAV_STATUS_OK);
  check_pgc_seek_rejected(dvd, &second, TICKS(3));
  dvdnav_close(dvd); fclose(file);
}

static void test_selected_angle_following_cell(const char *directory) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "unequal-angle-followed", &file);
  dvdnav_title_position_t raw, old;
  dvdnav_chapter_t *chapters = NULL;
  uint32_t count = 0;
  CHECK(dvdnav_program_play(dvd, 1, 2, 1) == DVDNAV_STATUS_OK);
  CHECK(next_cell(dvd));
  CHECK(dvdnav_get_current_title_position(dvd, &old) == DVDNAV_STATUS_OK);
  CHECK(old.angle == 1 && old.pgc_duration == TICKS(7));
  CHECK(dvdnav_get_current_pgc_chapters(dvd, &old, &chapters, &count) == DVDNAV_STATUS_OK);
  CHECK(count == 2 && chapters[0].chapter == 3 && chapters[1].chapter == 4 &&
        chapters[0].time == 0 && chapters[1].time == TICKS(5));
  dvdnav_free_chapters(chapters);
  CHECK(dvdnav_angle_change(dvd, 2) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_get_current_pgc_chapters(dvd, &old, &chapters, &count) == DVDNAV_STATUS_ERR);
  CHECK(chapters == NULL && count == 0);
  CHECK(dvdnav_program_play(dvd, 1, 2, 2) == DVDNAV_STATUS_OK);
  CHECK(next_cell(dvd));
  CHECK(dvd->vm->state.cellN == 3);
  CHECK(dvdnav_get_current_title_position(dvd, &raw) == DVDNAV_STATUS_OK);
  CHECK(raw.angle == 2 && raw.pgc_duration == TICKS(9) && raw.pgc_time == TICKS(7));
  CHECK(dvdnav_get_current_time(dvd) == TICKS(5)); /* Existing API is unchanged. */
  CHECK(dvdnav_get_current_pgc_chapters(dvd, &raw, &chapters, &count) == DVDNAV_STATUS_OK);
  CHECK(count == 2 && chapters[0].chapter == 3 && chapters[1].chapter == 4 &&
        chapters[0].time == 0 && chapters[1].time == TICKS(7));
  dvdnav_free_chapters(chapters);
  dvdnav_close(dvd); fclose(file);
}

static void test_prefetched_pgc_seek(const char *directory) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "continuous-nop", &file);
  dvdnav_title_position_t presented, ahead, after;
  uint8_t buffer[2048];
  CHECK(dvdnav_set_PGC_positioning_flag(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &presented) == DVDNAV_STATUS_OK);
  CHECK(presented.pgcn == 1);
  ahead = presented;
  for (unsigned i = 0; i < 1000 && ahead.pgcn == 1; i++) {
    CHECK(next_nav(dvd, buffer));
    CHECK(dvdnav_get_current_title_position(dvd, &ahead) == DVDNAV_STATUS_OK);
  }
  CHECK(ahead.pgcn == 2);
  fail_title_vob_reads = 1;
  check_pgc_seek_rejected(dvd, &presented, TICKS(6));
  fail_title_vob_reads = 0;
  CHECK(dvdnav_pgc_time_search_floor(dvd, &presented, TICKS(6)) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_get_current_title_position(dvd, &after) == DVDNAV_STATUS_OK);
  CHECK(after.pgcn == 1 && after.pgc_time == TICKS(6));
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &after) == DVDNAV_STATUS_OK);
  CHECK(after.pgcn == 1 && after.pgc_time == TICKS(6));
  CHECK(dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm == TICKS(6) + 48600);
  printf("Prefetched PGC2 -> presented PGC1 seek: local=%llu NAVPTS=%u\n",
         (unsigned long long)after.pgc_time,
         dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm);
  dvdnav_close(dvd); fclose(file);
}

static void test_seek_after_read_ahead_stop(const char *directory, int title_scope) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, title_scope ? "continuous-pts" : "continuous-nop", &file);
  dvdnav_title_layout_t *layout = NULL;
  dvdnav_title_position_t presented, actual;
  dvd_state_t state;
  vm_position_t position;
  uint8_t buffer[2048];
  int stop = 0, hop, verified_hop, verified_angle;
  int64_t cell_time;
  CHECK(dvdnav_set_PGC_positioning_flag(dvd, 1) == DVDNAV_STATUS_OK);
  if (title_scope) {
    CHECK(dvdnav_get_title_layout(dvd, 1, &layout) == DVDNAV_TITLE_LAYOUT_LINEAR);
    CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
    CHECK(next_nav(dvd, buffer));
    CHECK(dvdnav_title_time_search_floor(dvd, layout, TICKS(36)) == DVDNAV_STATUS_OK);
  } else {
    CHECK(dvdnav_program_play(dvd, 1, 2, 1) == DVDNAV_STATUS_OK);
    CHECK(next_nav(dvd, buffer));
    CHECK(dvdnav_get_current_title_position(dvd, &presented) == DVDNAV_STATUS_OK);
    CHECK(dvdnav_pgc_time_search_floor(dvd, &presented, TICKS(6)) == DVDNAV_STATUS_OK);
  }
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &presented) == DVDNAV_STATUS_OK);
  CHECK(presented.pgcn == 2 && presented.pgc_time == TICKS(6));
  for (unsigned step = 0; step < 40000; step++) {
    int32_t event, length;
    CHECK(dvdnav_get_next_block(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_STOP) { stop = 1; break; }
    if (event == DVDNAV_WAIT) CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_STILL_FRAME) CHECK(dvdnav_still_skip(dvd) == DVDNAV_STATUS_OK);
  }
  CHECK(stop && !dvd->started && dvd->vm->stopped);
  state = dvd->vm->state; position = dvd->position_current;
  hop = dvd->vm->hop_channel; cell_time = dvd->cur_cell_time;
  verified_hop = dvd->verified_seek_hop; verified_angle = dvd->verified_seek_angle;
  for (int failure = 0; failure < 2; failure++) {
    dvdnav_status_t result;
    uint64_t time = failure ? (title_scope ? TICKS(60) : TICKS(30)) : TICKS(3);
    fail_title_vob_reads = !failure;
    result = title_scope ? dvdnav_title_time_search_floor(dvd, layout, time) :
        dvdnav_pgc_time_search_floor(dvd, &presented, time);
    fail_title_vob_reads = 0;
    CHECK(result == DVDNAV_STATUS_ERR);
    CHECK(!dvd->started && dvd->vm->stopped);
    CHECK(memcmp(&state, &dvd->vm->state, sizeof(state)) == 0);
    CHECK(memcmp(&position, &dvd->position_current, sizeof(position)) == 0);
    CHECK(hop == dvd->vm->hop_channel && cell_time == dvd->cur_cell_time);
    CHECK(verified_hop == dvd->verified_seek_hop && verified_angle == dvd->verified_seek_angle);
  }
  CHECK(rejected_vob_reads > 0);
  CHECK((title_scope ? dvdnav_title_time_search_floor(dvd, layout, TICKS(3)) :
      dvdnav_pgc_time_search_floor(dvd, &presented, TICKS(3))) == DVDNAV_STATUS_OK);
  CHECK(dvd->started && !dvd->vm->stopped);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &actual) == DVDNAV_STATUS_OK);
  CHECK(actual.pgcn == (title_scope ? 1u : 2u) && actual.pgc_time == TICKS(3));
  CHECK(dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm ==
        (title_scope ? TICKS(3) : TICKS(33)) + 48600);
  {
    int32_t event, length;
    CHECK(dvdnav_get_next_block(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    CHECK(event == DVDNAV_BLOCK_OK && length == 2048);
  }
  printf("%s scope after STOP: PGC=%u local=%llu actual NAVPTS=%u\n",
         title_scope ? "TITLE" : "PGC", actual.pgcn,
         (unsigned long long)actual.pgc_time,
         dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm);
  dvdnav_free_title_layout(layout); dvdnav_close(dvd); fclose(file);
}

/* The RED adapter is the former JNI chain, calling real production VM APIs. */
#ifdef CHAPTER_BASELINE
static dvdnav_status_t chapter_jump(dvdnav_t *dvd,
    const dvdnav_title_position_t *expected, int32_t part) {
  dvdnav_title_position_t current;
  if (dvdnav_get_current_title_position(dvd, &current) != DVDNAV_STATUS_OK ||
      current.title != expected->title || current.title_set != expected->title_set)
    return DVDNAV_STATUS_ERR;
  return dvdnav_part_play(dvd, (int32_t)expected->title, part);
}
#else
#define chapter_jump dvdnav_title_part_play
#endif

static void check_chapter_rejected(dvdnav_t *dvd,
    const dvdnav_title_position_t *expected, int32_t part) {
  dvd_state_t state = dvd->vm->state;
  vm_position_t position = dvd->position_current;
  dvd_file_t *file = dvd->file;
  int started = dvd->started, stopped = dvd->vm->stopped;
  int hop = dvd->vm->hop_channel, verified = dvd->verified_seek_hop;
  int angle = dvd->verified_seek_angle;
  int64_t time = dvd->cur_cell_time;
  CHECK(chapter_jump(dvd, expected, part) == DVDNAV_STATUS_ERR);
  CHECK(memcmp(&state, &dvd->vm->state, sizeof(state)) == 0);
  CHECK(memcmp(&position, &dvd->position_current, sizeof(position)) == 0);
  CHECK(dvd->file == file && dvd->started == started && dvd->vm->stopped == stopped);
  CHECK(dvd->vm->hop_channel == hop && dvd->cur_cell_time == time);
  CHECK(dvd->verified_seek_hop == verified && dvd->verified_seek_angle == angle);
}

static void test_chapter_transaction(const char *directory, int title_scope) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, title_scope ? "continuous-pts" : "continuous-nop", &file);
  dvdnav_title_layout_t *layout = NULL;
  dvdnav_title_position_t expected, altered, actual;
  uint8_t buffer[2048];
  CHECK(dvdnav_get_title_layout(dvd, 1, &layout) ==
        (title_scope ? DVDNAV_TITLE_LAYOUT_LINEAR : DVDNAV_TITLE_LAYOUT_UNSUPPORTED));
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &expected) == DVDNAV_STATUS_OK);
  /* Native PTT 4 means PGC 2, program 2, actual payload starts at global 45s. */
  CHECK(chapter_jump(dvd, &expected, 4) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &actual) == DVDNAV_STATUS_OK);
  CHECK(actual.pgcn == 2 && actual.program == 2 && actual.pgc_time == TICKS(15));
  CHECK(dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm == TICKS(45) + 48600);
  int stop = 0;
  for (unsigned step = 0; step < 40000; step++) {
    int32_t event, length;
    CHECK(dvdnav_get_next_block(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_STOP) { stop = 1; break; }
    if (event == DVDNAV_WAIT) CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_STILL_FRAME) CHECK(dvdnav_still_skip(dvd) == DVDNAV_STATUS_OK);
  }
  CHECK(stop && !dvd->started && dvd->vm->stopped);
  CHECK(dvdnav_get_current_title_position(dvd, &actual) == DVDNAV_STATUS_ERR);
  check_chapter_rejected(dvd, &expected, 0);
  check_chapter_rejected(dvd, &expected, 5);
  check_chapter_rejected(dvd, &expected, INT32_MAX);
  altered = expected; altered.title = 2; check_chapter_rejected(dvd, &altered, 1);
  altered = expected; altered.title_set = 2; check_chapter_rejected(dvd, &altered, 1);
  altered = expected; altered.angle = 2; check_chapter_rejected(dvd, &altered, 1);
  fail_ifo_reads = 1;
  check_chapter_rejected(dvd, &expected, 1);
  fail_ifo_reads = 0;
#ifndef CHAPTER_BASELINE
  CHECK(rejected_ifo_reads > 0);
#endif
  dvdnav_status_t result = chapter_jump(dvd, &expected, 1);
  CHECK(result == DVDNAV_STATUS_OK);
  if (result == DVDNAV_STATUS_OK) {
    CHECK(dvd->started && !dvd->vm->stopped);
    CHECK(next_nav(dvd, buffer));
    CHECK(dvdnav_get_current_title_position(dvd, &actual) == DVDNAV_STATUS_OK);
    CHECK(actual.pgcn == 1 && actual.program == 1 && actual.pgc_time == 0);
    CHECK(dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm == 48600);
    int32_t event, length;
    CHECK(dvdnav_get_next_block(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    CHECK(event == DVDNAV_BLOCK_OK && length == 2048);
    printf("Chapter after STOP %s: actual PGC=%u program=%u NAVPTS=%u payload=%d\n",
           title_scope ? "TITLE" : "CURRENT_PGC", actual.pgcn, actual.program,
           dvdnav_get_current_nav_pci(dvd)->pci_gi.vobu_s_ptm, length);
  }
  dvdnav_free_title_layout(layout); dvdnav_close(dvd); fclose(file);
}

static void test_chapter_non_ptt(const char *directory) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "missing-ptt-program", &file);
  dvdnav_title_position_t expected;
  CHECK(dvdnav_program_play(dvd, 1, 2, 2) == DVDNAV_STATUS_OK);
  CHECK(next_cell(dvd));
  CHECK(dvdnav_get_current_title_position(dvd, &expected) == DVDNAV_STATUS_OK);
  CHECK(expected.pgcn == 2 && expected.program == 2);
  int32_t title, part;
  CHECK(dvdnav_current_title_info(dvd, &title, &part) == DVDNAV_STATUS_ERR);
  CHECK(chapter_jump(dvd, &expected, 1) == DVDNAV_STATUS_OK);
  CHECK(dvd->vm->state.pgN == 1 && dvd->vm->state.pgcN == 1);
  dvdnav_close(dvd); fclose(file);
}


#ifdef PRECOMMAND_BASELINE
#define dvdnav_get_next_block_with_wait dvdnav_get_next_block
#endif

static int next_command_wait(dvdnav_t *dvd) {
  uint8_t buffer[2048];
  unsigned blocks = 0;
  for (unsigned step = 0; step < 40000; step++) {
    int32_t event, length;
    CHECK(dvdnav_get_next_block_with_wait(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_BLOCK_OK) blocks++;
    if (event == DVDNAV_WAIT) { CHECK(blocks > 500); return 1; }
    if (event == DVDNAV_STOP) return 0;
    if (event == DVDNAV_STILL_FRAME) CHECK(dvdnav_still_skip(dvd) == DVDNAV_STATUS_OK);
  }
  return 0;
}

static void test_presented_stream_commands(const char *directory, int audio_branch) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "stream-command-branch", &file);
  dvdnav_title_position_t expected, actual;
  uint8_t buffer[2048];
  CHECK(dvdnav_set_PGC_positioning_flag(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_set_active_stream(dvd, 0, DVD_AUDIO_STREAM) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_set_active_stream(dvd, 0, DVD_SUBTITLE_STREAM) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_toggle_spu_stream(dvd, 0) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_get_current_title_position(dvd, &expected) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_title_part_play(dvd, &expected, 4) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  int held = next_command_wait(dvd);
  CHECK(held);
  CHECK(dvd->vm->state.domain == DVD_DOMAIN_VTSTitle && dvd->vm->state.pgcN == 2);
  CHECK(dvd->vm->state.registers.GPRM[15] == 0);
  if (!held) { dvdnav_close(dvd); fclose(file); return; }
  CHECK(dvdnav_get_current_title_position(dvd, &expected) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_get_active_logical_stream(dvd, DVD_AUDIO_STREAM) == 0);
  CHECK(dvdnav_get_active_logical_stream(dvd, DVD_SUBTITLE_STREAM) == 0);
  CHECK(dvdnav_audio_stream_to_lang(dvd, 6) == 0x6a61);
  CHECK(dvdnav_spu_stream_to_lang(dvd, 1) == 0x6a61);
  CHECK(dvdnav_set_active_stream(dvd, 6, DVD_AUDIO_STREAM) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_set_active_stream(dvd, 1, DVD_SUBTITLE_STREAM) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_toggle_spu_stream(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_get_active_logical_stream(dvd, DVD_AUDIO_STREAM) == 6);
  CHECK(dvdnav_get_active_logical_stream(dvd, DVD_SUBTITLE_STREAM) == 1);
  CHECK(dvd->vm->state.SPST_REG == 0x41);
  for (int repeat = 0; repeat < 2; repeat++) {
    int32_t event, length;
    CHECK(dvdnav_get_next_block_with_wait(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    CHECK(event == DVDNAV_WAIT && dvd->vm->state.registers.GPRM[15] == 0);
  }
  dvd_state_t before = dvd->vm->state;
  CHECK(dvdnav_pgc_time_search_floor(dvd, &expected, TICKS(31)) == DVDNAV_STATUS_ERR);
  CHECK(memcmp(&before, &dvd->vm->state, sizeof(before)) == 0 && dvd->sync_wait);
  CHECK(dvdnav_pgc_time_search_floor(dvd, &expected, TICKS(6)) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &actual) == DVDNAV_STATUS_OK);
  CHECK(actual.pgcn == 2 && actual.pgc_time == TICKS(6));
  CHECK(dvdnav_get_active_logical_stream(dvd, DVD_AUDIO_STREAM) == 6);
  CHECK(dvdnav_get_active_logical_stream(dvd, DVD_SUBTITLE_STREAM) == 1);
  CHECK(dvd->vm->state.registers.GPRM[15] == 0);
  CHECK(next_command_wait(dvd));
  CHECK(dvdnav_title_part_play(dvd, &actual, 1) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &actual) == DVDNAV_STATUS_OK);
  CHECK(actual.pgcn == 1 && actual.program == 1 && actual.pgc_time == 0);
  CHECK(dvdnav_get_active_logical_stream(dvd, DVD_AUDIO_STREAM) == 6);
  CHECK(dvdnav_get_active_logical_stream(dvd, DVD_SUBTITLE_STREAM) == 1);
  CHECK(dvdnav_title_part_play(dvd, &actual, 4) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(next_command_wait(dvd));
  if (audio_branch == 1) {
    CHECK(dvdnav_toggle_spu_stream(dvd, 0) == DVDNAV_STATUS_OK);
    CHECK(dvdnav_get_active_logical_stream(dvd, DVD_SUBTITLE_STREAM) == 1);
    CHECK(dvd->vm->state.SPST_REG == 1);
  } else {
    CHECK(dvdnav_set_active_stream(dvd, 0, DVD_AUDIO_STREAM) == DVDNAV_STATUS_OK);
  }
  if (audio_branch == 2)
    CHECK(dvdnav_toggle_spu_stream(dvd, 0) == DVDNAV_STATUS_OK);
  CHECK(dvd->vm->state.registers.GPRM[15] == 0);
  CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
  if (audio_branch == 2) {
    int stopped = 0;
    for (int step = 0; step < 100; step++) {
      int32_t event, length;
      CHECK(dvdnav_get_next_block_with_wait(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
      if (event == DVDNAV_STOP) { stopped = 1; break; }
      CHECK(event != DVDNAV_BLOCK_OK && event != DVDNAV_WAIT);
    }
    CHECK(stopped && dvd->vm->state.registers.GPRM[15] == 1);
    dvdnav_close(dvd); fclose(file); return;
  }
  CHECK(next_nav(dvd, buffer));
  CHECK(dvdnav_get_current_title_position(dvd, &actual) == DVDNAV_STATUS_OK);
  CHECK(actual.pgcn == 1 && actual.program == 1 && actual.pgc_time == 0);
  CHECK(dvd->vm->state.registers.GPRM[15] == 1);
  int32_t event, length;
  CHECK(dvdnav_get_next_block_with_wait(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
  CHECK(event == DVDNAV_BLOCK_OK && length == 2048);
  CHECK(dvd->vm->state.registers.GPRM[15] == 1);
  printf("Presented %s branch: setter, readback, time/chapter, one ACK command => PGC1 and payload\n",
         audio_branch ? "audio" : "subtitle");
  dvdnav_close(dvd); fclose(file);
}

static void test_plain_cell_continuation(const char *directory) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "continuous-nop", &file);
  uint8_t buffer[2048];
  int second_cell = 0, waited = 0;
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  for (unsigned step = 0; step < 40000; step++) {
    int32_t event, length;
    CHECK(dvdnav_get_next_block_with_wait(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_CELL_CHANGE && dvd->vm->state.cellN == 2 && dvd->vm->state.pgcN == 1)
      second_cell = 1;
    if (event == DVDNAV_WAIT) { waited = 1; break; }
    if (event == DVDNAV_STOP) break;
  }
  CHECK(second_cell && waited);
  if (!waited) { dvdnav_close(dvd); fclose(file); return; }
  CHECK(dvd->vm->state.domain == DVD_DOMAIN_VTSTitle && dvd->vm->state.pgcN == 1 && dvd->vm->state.cellN == 2);
  CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvd->vm->state.pgcN == 2);
  dvdnav_close(dvd); fclose(file);
}


static void test_cell_command_wait(const char *directory) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "cell-command-nop", &file);
  uint8_t buffer[2048];
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  int held = next_command_wait(dvd);
  CHECK(held);
  if (!held) { dvdnav_close(dvd); fclose(file); return; }
  CHECK(dvd->vm->state.pgcN == 1 && dvd->vm->state.cellN == 1);
  CHECK(dvd->vm->state.registers.GPRM[14] == 0);
  CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
  CHECK(next_nav(dvd, buffer));
  CHECK(dvd->vm->state.pgcN == 1 && dvd->vm->state.cellN == 2);
  CHECK(dvd->vm->state.registers.GPRM[14] == 1);
  int32_t event, length;
  CHECK(dvdnav_get_next_block_with_wait(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
  CHECK(event == DVDNAV_BLOCK_OK && length == 2048);
  CHECK(dvd->vm->state.registers.GPRM[14] == 1);
  dvdnav_close(dvd); fclose(file);
}

static void test_selected_angle_command_wait(const char *directory, int angle) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, "unequal-angle-nav", &file);
  uint8_t buffer[2048];
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_angle_change(dvd, angle) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_program_play(dvd, 1, 2, 1) == DVDNAV_STATUS_OK);
  int held = 0;
  for (unsigned step = 0; step < 40000; step++) {
    int32_t event, length;
    CHECK(dvdnav_get_next_block_with_wait(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_WAIT) { held = 1; break; }
    if (event == DVDNAV_STOP) break;
  }
  CHECK(held && dvd->vm->state.pgcN == 2 && dvd->vm->state.cellN == angle);
  if (!held) { dvdnav_close(dvd); fclose(file); return; }
  CHECK(dvd->vm->state.AGL_REG == angle);
  CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
  int stopped = 0;
  for (unsigned step = 0; step < 100; step++) {
    int32_t event, length;
    CHECK(dvdnav_get_next_block_with_wait(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_STOP) { stopped = 1; break; }
    CHECK(event != DVDNAV_BLOCK_OK && event != DVDNAV_WAIT);
  }
  CHECK(stopped);
  dvdnav_close(dvd); fclose(file);
}

static void test_chapter_from_still(const char *directory, int still_duration) {
  FILE *file;
  dvdnav_t *dvd = open_fixture(directory, still_duration == 255 ?
      "continuous-infinite-still" : "continuous-finite-still", &file);
  uint8_t buffer[2048];
  dvdnav_title_position_t presented = {0};
  int held = 0;
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  for (unsigned step = 0; step < 40000; step++) {
    int32_t event, length;
    CHECK(dvdnav_get_next_block_with_wait(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_BLOCK_OK)
      CHECK(dvdnav_get_current_title_position(dvd, &presented) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_STILL_FRAME) { held = 1; break; }
    if (event == DVDNAV_WAIT) CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_STOP) break;
  }
  CHECK(held && dvd->position_current.still == still_duration);
  if (!held) { dvdnav_close(dvd); fclose(file); return; }
  dvd_state_t state = dvd->vm->state;
  dvdnav_vobu_t vobu = dvd->vobu;
  vm_position_t position = dvd->position_current;
  int hop = dvd->vm->hop_channel;
  int64_t cell_time = dvd->cur_cell_time;
  int waiting = dvd->sync_wait;
  CHECK(dvdnav_pgc_time_search_floor(dvd, &presented, 0) == DVDNAV_STATUS_ERR);
  CHECK(strcmp(dvdnav_err_to_string(dvd), "Cannot seek in a still frame.") == 0);
  CHECK(dvdnav_title_part_play(dvd, &presented, 999) == DVDNAV_STATUS_ERR);
  CHECK(memcmp(&state, &dvd->vm->state, sizeof(state)) == 0);
  CHECK(memcmp(&vobu, &dvd->vobu, sizeof(vobu)) == 0);
  CHECK(memcmp(&position, &dvd->position_current, sizeof(position)) == 0);
  CHECK(dvd->vm->hop_channel == hop && dvd->cur_cell_time == cell_time);
  CHECK(dvd->sync_wait == waiting);
  CHECK(dvdnav_title_part_play(dvd, &presented, 3) == DVDNAV_STATUS_OK);
  CHECK(dvd->position_current.still == 0 && dvd->cur_cell_time == 0);
  int payload = 0;
  for (unsigned step = 0; step < 1000; step++) {
    int32_t event, length;
    CHECK(dvdnav_get_next_block_with_wait(dvd, buffer, &event, &length) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_BLOCK_OK) {
      CHECK(length == 2048);
      CHECK(dvdnav_get_current_title_position(dvd, &presented) == DVDNAV_STATUS_OK);
      payload = 1;
      break;
    }
    if (event == DVDNAV_STILL_FRAME || event == DVDNAV_STOP) break;
    if (event == DVDNAV_WAIT) CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
  }
  CHECK(payload && presented.title == 1 && presented.pgcn == 2 && presented.program == 1);
  CHECK(dvd->position_current.still == 0 && presented.pgc_time < TICKS(1));
  printf("Authored still %d -> part 3: payload=%d PGC=%u program=%u still=%d\n",
         still_duration, payload, presented.pgcn, presented.program, dvd->position_current.still);
  dvdnav_close(dvd); fclose(file);
}

int main(int argc, char **argv) {
  char offset_path[2048];
  FILE *offset_file;
  long long offset;
  if (argc != 2) return 2;
  snprintf(offset_path, sizeof(offset_path), "%s/title-vob-offset.txt", argv[1]);
  offset_file = fopen(offset_path, "r");
  if (!offset_file || fscanf(offset_file, "%lld", &offset) != 1 || offset <= 0) return 2;
  fclose(offset_file);
  title_vob_offset = offset;
  test_chapter_from_still(argv[1], 3);
  test_chapter_from_still(argv[1], 255);
  test_presented_stream_commands(argv[1], 1);
  test_presented_stream_commands(argv[1], 0);
  test_presented_stream_commands(argv[1], 2);
  test_plain_cell_continuation(argv[1]);
  test_cell_command_wait(argv[1]);
  test_selected_angle_command_wait(argv[1], 1);
  test_selected_angle_command_wait(argv[1], 2);
  test_seek_after_read_ahead_stop(argv[1], 1);
  test_seek_after_read_ahead_stop(argv[1], 0);
  test_chapter_transaction(argv[1], 1);
  test_chapter_transaction(argv[1], 0);
  test_chapter_non_ptt(argv[1]);
  test_linear(argv[1]);
  test_atomic_seek(argv[1]);
  test_pgc_scope(argv[1]);
  test_unsupported_route_pgc(argv[1], "conditional-post", 1, TICKS(5));
  test_unsupported_route_pgc(argv[1], "loop", 1, TICKS(5));
  test_unsupported_route_pgc(argv[1], "infinite-still", 2, TICKS(12));
  test_unsupported_route_pgc(argv[1], "continuous-commands", 1, TICKS(30));
  test_route_chapters(argv[1], "entry-program-two", 4, 1);
  test_route_chapters(argv[1], "unreachable-ptt", 2, 1);
  test_pgc_natural(argv[1]);
  test_selected_angle(argv[1]);
  test_selected_angle_following_cell(argv[1]);
  test_prefetched_pgc_seek(argv[1]);
  printf("%u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}

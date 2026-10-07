/* Real native IFO/VM test: replay reads must preserve navigation and payload. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _fseeki64 fseeko
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <dvdnav/dvdnav.h>
#include <dvdnav/title_layout.h>
#include "vm/decoder.h"
#include "vm/vm.h"
#include "dvdnav_internal.h"

static unsigned checks, failures, callback_reads;
#define CHECK(x) do { checks++; if (!(x)) { \
  fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

typedef struct {
  dvdnav_block_position_t position;
  uint8_t bytes[2048];
} block_t;

static int seek_file(void *ctx, uint64_t offset) {
  return _fseeki64((FILE *)ctx, (int64_t)offset, SEEK_SET);
}

static int read_file(void *ctx, void *buffer, int length) {
  callback_reads++;
  size_t count = fread(buffer, 1, (size_t)length, (FILE *)ctx);
  return ferror((FILE *)ctx) ? -1 : (int)count;
}

static void replay(dvdnav_t *dvd, block_t *saved) {
  dvd_state_t state = dvd->vm->state;
  int hop = dvd->vm->hop_channel;
  int wait = dvd->sync_wait;
  dvd_file_t *current = dvd->file;
  uint8_t bytes[2048];
  dvd_file_t *file = dvdnav_open_block_reader(dvd, &saved->position);
  CHECK(file != NULL);
  if (file) {
    unsigned reads = callback_reads;
    CHECK(DVDReadBlocks(file, saved->position.sector, 1, bytes) == 1);
    CHECK(callback_reads > reads);
    CHECK(memcmp(bytes, saved->bytes, sizeof(bytes)) == 0);
    DVDCloseFile(file);
  }
  CHECK(memcmp(&state, &dvd->vm->state, sizeof(state)) == 0);
  CHECK(hop == dvd->vm->hop_channel);
  CHECK(wait == dvd->sync_wait);
  CHECK(current == dvd->file);
}

static void run_fixture(const char *directory, const char *name, int angle) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s.iso", directory, name);
  FILE *input = fopen(path, "rb");
  CHECK(input != NULL);
  if (!input) return;
  dvdnav_t *dvd = NULL;
  dvdnav_stream_cb callbacks = {seek_file, read_file, NULL};
  CHECK(dvdnav_open_stream(&dvd, input, &callbacks) == DVDNAV_STATUS_OK);
  if (!dvd) { fclose(input); return; }
  CHECK(dvdnav_set_PGC_positioning_flag(dvd, 1) == DVDNAV_STATUS_OK);
  CHECK(dvdnav_title_play(dvd, 1) == DVDNAV_STATUS_OK);
  if (angle > 1) CHECK(dvdnav_angle_change(dvd, angle) == DVDNAV_STATUS_OK);
  block_t *saved = calloc(256, sizeof(*saved));
  CHECK(saved != NULL);
  if (!saved) exit(2);
  int saved_count = 0;
  int blocks = 0;
  int waits = 0;
  int stopped = 0;
  for (int event_count = 0; event_count < 30000; event_count++) {
    uint8_t bytes[2048];
    int event, length;
    CHECK(dvdnav_get_next_block_with_wait(dvd, bytes, &event, &length) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_BLOCK_OK) {
      dvdnav_block_position_t position;
      CHECK(length == 2048);
      CHECK(dvdnav_get_current_block_position(dvd, &position) == DVDNAV_STATUS_OK);
      CHECK(position.sector == (uint32_t)(dvd->vobu.vobu_start + dvd->vobu.blockN));
      if ((blocks < 8 || blocks % 31 == 0) && saved_count < 256) {
        saved[saved_count].position = position;
        memcpy(saved[saved_count].bytes, bytes, sizeof(bytes));
        saved_count++;
      }
      blocks++;
    } else if (event == DVDNAV_WAIT) {
      waits++;
      if (saved_count) replay(dvd, &saved[0]);
      CHECK(dvdnav_wait_skip(dvd) == DVDNAV_STATUS_OK);
    } else if (event == DVDNAV_STILL_FRAME) {
      CHECK(dvdnav_still_skip(dvd) == DVDNAV_STATUS_OK);
    } else if (event == DVDNAV_STOP) {
      stopped = 1;
      break;
    }
  }
  CHECK(stopped);
  CHECK(saved_count > 0);
  CHECK(waits > 0);
  for (int i = saved_count - 1; i >= 0; i--) replay(dvd, &saved[i]);
  dvdnav_block_position_t bad = {0, DVD_READ_TITLE_VOBS, 0};
  CHECK(dvdnav_open_block_reader(dvd, &bad) == NULL);
  bad.title_set = 100;
  CHECK(dvdnav_open_block_reader(dvd, &bad) == NULL);
  bad.title_set = 1;
  bad.domain = DVD_READ_INFO_FILE;
  CHECK(dvdnav_open_block_reader(dvd, &bad) == NULL);
  bad.domain = DVD_READ_TITLE_VOBS;
  bad.sector = (uint32_t)INT32_MAX + 1;
  CHECK(dvdnav_open_block_reader(dvd, &bad) == NULL);
  CHECK(dvdnav_open_block_reader(dvd, NULL) == NULL);
  free(saved);
  dvdnav_close(dvd);
  fclose(input);
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  CHECK(dvdnav_open_block_reader(NULL, NULL) == NULL);
  CHECK(dvdnav_get_current_block_position(NULL, NULL) == DVDNAV_STATUS_ERR);
  run_fixture(argv[1], "linear", 1);
  run_fixture(argv[1], "two-pgc-dvd09", 1);
  run_fixture(argv[1], "finite-still", 1);
  run_fixture(argv[1], "equal-angle", 2);
  printf("%u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}

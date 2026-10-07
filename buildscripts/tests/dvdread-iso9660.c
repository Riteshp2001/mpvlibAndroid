/* Sector-authored ECMA-119 fixtures using production libdvdread file I/O. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dvd_input.h"
#include "dvdread/dvd_udf.h"
#include "reader-types.h"

#define BLOCKS 96
static unsigned char disc[BLOCKS][DVD_VIDEO_LB_LEN];
static FILE *image_file;
static int read_error = -1, short_read = -1, udf_calls, udf_lba, checks;
#define CHECK(x) do { checks++; if (!(x)) { \
  fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

void DVDReadLog(void *p, const dvd_logger_cb *l, dvd_logger_level_t level,
                const char *format, ...) {}
int InternalUDFReadBlocksRaw(const dvd_reader_t *ctx, uint32_t lba,
                            size_t count, unsigned char *data, int encrypted) {
  if (image_file) {
#ifdef _WIN32
    if (_fseeki64(image_file, (int64_t)lba * DVD_VIDEO_LB_LEN, SEEK_SET)) return -1;
#else
    if (fseek(image_file, (long)lba * DVD_VIDEO_LB_LEN, SEEK_SET)) return -1;
#endif
    size_t n = fread(data, DVD_VIDEO_LB_LEN, count, image_file);
    return ferror(image_file) ? -1 : (int)n;
  }
  if (lba >= BLOCKS || count > BLOCKS - lba ||
      (read_error >= 0 && lba <= (uint32_t)read_error &&
       (uint32_t)read_error - lba < count)) return -1;
  if ((int)lba == short_read) return 0;
  memcpy(data, disc[lba], count * DVD_VIDEO_LB_LEN);
  return (int)count;
}
uint32_t UDFFindFile(dvd_reader_t *ctx, const char *name, uint32_t *size) {
  udf_calls++;
  *size = udf_lba ? DVD_VIDEO_LB_LEN : 0;
  return udf_lba;
}
static int no_title(dvd_input_t d, int title) { return 0; }
static int no_close(dvd_input_t d) { return 0; }
int (*dvdinput_title)(dvd_input_t, int) = no_title;
int (*dvdinput_close)(dvd_input_t) = no_close;
static int initAllCSSKeys(dvd_reader_t *ctx) { return 0; }
static int DVDReadBlocksPath(const dvd_file_t *f, unsigned int offset,
                            size_t count, unsigned char *data, int encrypted) { return -1; }
static dvd_file_t *DVDOpenFilePath(dvd_reader_t *ctx, const char *filename) { return NULL; }
static dvd_file_t *DVDOpenVOBPath(dvd_reader_t *ctx, int title, int menu) { return NULL; }
static int DVDFileStatVOBPath(dvd_reader_t *ctx, int title, int menu, dvd_stat_t *s) { return -1; }
typedef struct { off_t st_size; } dvdstat_t;
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
static int findDVDFile(dvd_reader_t *ctx, const char *name, char *path) { return 0; }
static int dvdstat(const char *path, dvdstat_t *info) { return -1; }
#include "reader-functions.h"

static void le32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static void both32(unsigned char *p, uint32_t v) {
  le32(p, v);
  for (int i = 0; i < 4; i++) p[4 + i] = (unsigned char)(v >> (24 - 8 * i));
}
static void both16(unsigned char *p, unsigned v) {
  p[0] = p[3] = (unsigned char)v;
  p[1] = p[2] = (unsigned char)(v >> 8);
}
static int record(unsigned char *p, const char *name, uint32_t lba,
                  uint32_t bytes, int flags) {
  int n = (int)strlen(name), length = (33 + n + 1) & ~1;
  memset(p, 0, length);
  p[0] = (unsigned char)length;
  both32(p + 2, lba); both32(p + 10, bytes);
  p[25] = (unsigned char)flags; both16(p + 28, 1);
  p[32] = (unsigned char)n; memcpy(p + 33, name, n);
  return length;
}
static void descriptor(int sector, int type, const char *id) {
  disc[sector][0] = (unsigned char)type;
  memcpy(disc[sector] + 1, id, 5); disc[sector][6] = 1;
}
static dvd_reader_t *image(void) {
  memset(disc, 0, sizeof(disc)); read_error = short_read = -1; udf_calls = udf_lba = 0;
  descriptor(16, 1, "CD001"); descriptor(17, 255, "CD001");
  both32(disc[16] + 80, BLOCKS); both16(disc[16] + 120, 1);
  both16(disc[16] + 124, 1); both16(disc[16] + 128, DVD_VIDEO_LB_LEN);
  record(disc[16] + 156, "R", 20, 2 * DVD_VIDEO_LB_LEN, 2);
  disc[16][156 + 33] = 0;
  /* Directory padding requires advancing to the second sector. */
  record(disc[21], "VIDEO_TS", 22, DVD_VIDEO_LB_LEN, 2);
  dvd_reader_t *ctx = calloc(1, sizeof(*ctx));
  ctx->rd = calloc(1, sizeof(*ctx->rd)); ctx->rd->isImageFile = 1;
  ctx->rd->dev = (dvd_input_t)ctx; /* Borrowed handle for the fixture block input. */
  return ctx;
}
static void close_image(dvd_reader_t *ctx) {
#ifdef LIBDVDREAD_DVD_ISO9660_H
  free(ctx->rd->iso9660);
#endif
  free(ctx->rd); free(ctx);
}
static void simple_file(void) {
  dvd_reader_t *ctx = image();
  record(disc[22], "VIDEO_TS.IFO;1", 40, DVD_VIDEO_LB_LEN, 0);
  memset(disc[40], 0x4a, DVD_VIDEO_LB_LEN);
  dvd_file_t *f = DVDOpenFile(ctx, 0, DVD_READ_INFO_FILE);
  CHECK(f != NULL);
  unsigned char data[35]; CHECK(DVDFileSeek(f, 19) == 19);
  CHECK(DVDReadBytes(f, data, sizeof(data)) == sizeof(data));
  for (size_t i = 0; i < sizeof(data); i++) CHECK(data[i] == 0x4a);
  CHECK(udf_calls == 0);
  dvd_stat_t stat;
  CHECK(DVDFileStat(ctx, 0, DVD_READ_INFO_FILE, &stat) == 0);
  CHECK(stat.size == DVD_VIDEO_LB_LEN); CHECK(stat.nr_parts == 1);
  DVDCloseFile(f); close_image(ctx);
}

static void multi_extent_ifo(int cache) {
  dvd_reader_t *ctx = image();
  int n = record(disc[22], "VIDEO_TS.IFO;1", 40, DVD_VIDEO_LB_LEN, 0x80);
  record(disc[22] + n, "VIDEO_TS.IFO;1", 50, DVD_VIDEO_LB_LEN, 0);
  memset(disc[40], 0x40, DVD_VIDEO_LB_LEN);
  memset(disc[41], 0xee, DVD_VIDEO_LB_LEN); /* Not file data. */
  memset(disc[50], 0x50, DVD_VIDEO_LB_LEN);
  dvd_file_t *f = DVDOpenFileUDF(ctx, "/video_ts/video_ts.ifo", cache);
  CHECK(f != NULL); CHECK(f->filesize == 2);
  CHECK(DVDFileSeek(f, DVD_VIDEO_LB_LEN - 7) == DVD_VIDEO_LB_LEN - 7);
  unsigned char data[17];
  CHECK(DVDReadBytes(f, data, sizeof(data)) == sizeof(data));
  for (int i = 0; i < 17; i++) CHECK(data[i] == (i < 7 ? 0x40 : 0x50));
  if (cache) CHECK(DVDReadBlocksUDF(f, 1, SIZE_MAX, data, DVDINPUT_NOFLAGS) == 0);
  uint32_t position = f->seek_pos;
  CHECK(DVDReadBytes(f, data, SIZE_MAX) == -1);
#if SIZE_MAX > UINT32_MAX
  CHECK(DVDReadBytes(f, data, (size_t)1 << 43) == -1);
#endif
  CHECK(DVDReadBytes(f, data, 2 * DVD_VIDEO_LB_LEN) == -1);
  CHECK(f->seek_pos == position);
  CHECK(DVDFileSeekForce(f, 3 * DVD_VIDEO_LB_LEN, -1) == -1);
  CHECK(f->filesize == 2);
  CHECK(udf_calls == 0);
  DVDCloseFile(f); close_image(ctx);
}

static void multiple_vob_parts(void) {
  dvd_reader_t *ctx = image();
  int n = record(disc[22], "VTS_01_1.VOB;1", 40, DVD_VIDEO_LB_LEN, 0x80);
  n += record(disc[22] + n, "VTS_01_1.VOB;1", 50, DVD_VIDEO_LB_LEN, 0);
  record(disc[22] + n, "VTS_01_2.VOB;1", 70, DVD_VIDEO_LB_LEN, 0);
  memset(disc[40], 0x40, DVD_VIDEO_LB_LEN);
  memset(disc[50], 0x50, DVD_VIDEO_LB_LEN);
  memset(disc[70], 0x70, DVD_VIDEO_LB_LEN);
  dvd_file_t *f = DVDOpenFile(ctx, 1, DVD_READ_TITLE_VOBS);
  CHECK(f != NULL); CHECK(f->filesize == 3);
  unsigned char data[3 * DVD_VIDEO_LB_LEN];
  CHECK(DVDReadBlocks(f, 0, 3, data) == 3);
  for (size_t i = 0; i < sizeof(data); i++)
    CHECK(data[i] == (i < DVD_VIDEO_LB_LEN ? 0x40 : i < 2 * DVD_VIDEO_LB_LEN ? 0x50 : 0x70));
  CHECK(DVDReadBlocks(f, 3, 1, data) == -1);
  CHECK(DVDReadBlocks(f, 3, 0, data) == 0);
  CHECK(DVDReadBlocks(f, 0, SIZE_MAX, data) == -1);
  short_read = 50;
  CHECK(DVDReadBlocks(f, 0, 3, data) == 1);
  short_read = -1; read_error = 50;
  CHECK(DVDReadBlocks(f, 0, 3, data) == -1);
  read_error = -1;
  dvd_stat_t stat;
  CHECK(DVDFileStat(ctx, 1, DVD_READ_TITLE_VOBS, &stat) == 0);
  CHECK(stat.nr_parts == 2); CHECK(stat.size == 3 * DVD_VIDEO_LB_LEN);
  CHECK(stat.parts_size[0] == 2 * DVD_VIDEO_LB_LEN);
  CHECK(stat.parts_size[1] == DVD_VIDEO_LB_LEN);
  CHECK(udf_calls == 0);
  DVDCloseFile(f); close_image(ctx);
}

static void udf_priority(void) {
  dvd_reader_t *ctx = image();
  record(disc[22], "VIDEO_TS.IFO;1", 40, DVD_VIDEO_LB_LEN, 0);
  descriptor(18, 0, "BOOT2"); descriptor(19, 0, "CDW02");
  descriptor(20, 0, "BEA01"); descriptor(21, 0, "NSR03");
  /* UDF wins even when the unused ISO primary descriptor is malformed. */
  disc[16][6] = 0;
  udf_lba = 60; memset(disc[60], 0x60, DVD_VIDEO_LB_LEN);
  dvd_file_t *f = DVDOpenFileUDF(ctx, "/VIDEO_TS/VIDEO_TS.IFO", 0);
  CHECK(f != NULL); CHECK(f->lb_start == 60);
  unsigned char data[DVD_VIDEO_LB_LEN];
  CHECK(DVDReadBlocks(f, 0, 1, data) == 1); CHECK(data[0] == 0x60);
  CHECK(udf_calls == 1);
  DVDCloseFile(f); close_image(ctx);

  ctx = image(); memset(disc[16], 0, DVD_VIDEO_LB_LEN);
  udf_lba = 60;
  f = DVDOpenFileUDF(ctx, "/VIDEO_TS/VIDEO_TS.IFO", 0);
  CHECK(f != NULL); CHECK(udf_calls == 1);
  DVDCloseFile(f); close_image(ctx);
}

static void associated_file(void) {
  dvd_reader_t *ctx = image();
  int n = record(disc[22], "VIDEO_TS.IFO;1", 40, DVD_VIDEO_LB_LEN, 4);
  record(disc[22] + n, "VIDEO_TS.IFO;1", 50, DVD_VIDEO_LB_LEN, 0);
  dvd_file_t *f = DVDOpenFileUDF(ctx, "/VIDEO_TS/VIDEO_TS.IFO", 0);
  CHECK(f != NULL); CHECK(f->lb_start == 50); DVDCloseFile(f);
  CHECK(DVDOpenFileUDF(ctx, "/VIDEO_TS/VIDEO_TS.IF", 0) == NULL);
  close_image(ctx);
}

static void directory_boundary_and_attributes(void) {
  dvd_reader_t *ctx = image();
  /* A file section can continue after directory sector padding. */
  both32(disc[21] + 10, 2 * DVD_VIDEO_LB_LEN);
  record(disc[22], "VIDEO_TS.IFO;1", 39, DVD_VIDEO_LB_LEN, 0x80);
  disc[22][1] = 1; /* Extended-attribute block; data starts at 40. */
  record(disc[23], "VIDEO_TS.IFO;1", 50, DVD_VIDEO_LB_LEN, 0);
  memset(disc[39], 0xee, DVD_VIDEO_LB_LEN);
  memset(disc[40], 0x40, DVD_VIDEO_LB_LEN);
  memset(disc[50], 0x50, DVD_VIDEO_LB_LEN);
  dvd_file_t *f = DVDOpenFile(ctx, 0, DVD_READ_INFO_FILE);
  CHECK(f != NULL); CHECK(f->filesize == 2);
  unsigned char data[2 * DVD_VIDEO_LB_LEN];
  CHECK(DVDReadBlocks(f, 0, 2, data) == 2);
  CHECK(data[0] == 0x40); CHECK(data[DVD_VIDEO_LB_LEN] == 0x50);
  DVDCloseFile(f); close_image(ctx);
}

static void primary_after_boot_record(void) {
  dvd_reader_t *ctx = image();
  memcpy(disc[17], disc[16], DVD_VIDEO_LB_LEN);
  memset(disc[16], 0, DVD_VIDEO_LB_LEN);
  descriptor(16, 0, "CD001"); descriptor(18, 255, "CD001");
  memset(disc[17] + 40, ' ', 32);
  memcpy(disc[17] + 40, "PRIMARY_VOLUME", 14);
  memcpy(disc[17] + 190, "volume-set", 10);
  char volume[33] = {0}; unsigned char set[10] = {0};
  CHECK(DVDISOVolumeInfo(ctx, volume, sizeof(volume), set, sizeof(set)) == 0);
  CHECK(!strcmp(volume, "PRIMARY_VOLUME"));
  CHECK(!memcmp(set, "volume-set", sizeof(set)));
  CHECK(udf_calls == 0);
  close_image(ctx);
}

static void malformed_and_io(void) {
  for (int kind = 0; kind < 16; kind++) {
    dvd_reader_t *ctx = image();
    int n = record(disc[22], "VIDEO_TS.IFO;1", 40, DVD_VIDEO_LB_LEN, 0);
    switch (kind) {
      case 0: disc[16][84] ^= 1; break; /* Endian disagreement. */
      case 1: memset(disc[17], 0, DVD_VIDEO_LB_LEN); break;
      case 2: both32(disc[22] + 2, BLOCKS); break;
      case 3: disc[22][0] = 33; break;
      case 4: disc[22][25] = 0x80; break; /* Missing final section. */
      case 5:
        disc[22][25] = 0x80;
        record(disc[22] + n, "DIFFERENT.IFO;1", 50, DVD_VIDEO_LB_LEN, 0);
        break;
      case 6: disc[22][25] = 0x80; both32(disc[22] + 10, 17); break;
      case 7: read_error = 16; break;
      case 8: read_error = 22; break;
      case 9: short_read = 22; break;
      case 10: read_error = 40; break; /* Failed IFO cache fill. */
      case 11: disc[22][26] = 1; break; /* ISO file-unit interleaving. */
      case 12: both16(disc[22] + 28, 2); break;
      case 13: short_read = 16; break;
      case 14: disc[22][32] = 255; break; /* Identifier exceeds record. */
      case 15: both32(disc[16] + 80, UINT32_MAX); break;
    }
    CHECK(DVDOpenFileUDF(ctx, "/VIDEO_TS/VIDEO_TS.IFO", 1) == NULL);
    CHECK(udf_calls == 0); /* Never retry this image as UDF. */
    close_image(ctx);
  }
}

static void extract_image(const char *image_path, const char *name, const char *output_path) {
  dvd_reader_t *ctx = image();
  image_file = fopen(image_path, "rb"); CHECK(image_file != NULL);
  dvd_file_t *file = DVDOpenFileUDF(ctx, name, 0); CHECK(file != NULL);
  FILE *output = fopen(output_path, "wb"); CHECK(output != NULL);
  unsigned char buffer[DVD_VIDEO_LB_LEN];
  for (ssize_t i = 0; i < file->filesize; i++) {
    CHECK(DVDReadBlocks(file, (int)i, 1, buffer) == 1);
    CHECK(fwrite(buffer, sizeof(buffer), 1, output) == 1);
  }
  CHECK(fclose(output) == 0); DVDCloseFile(file);
  CHECK(fclose(image_file) == 0); image_file = NULL;
  close_image(ctx);
}

int main(int argc, char **argv) {
  if (argc == 4) {
    extract_image(argv[1], argv[2], argv[3]);
    return 0;
  }
  simple_file();
  multi_extent_ifo(0); multi_extent_ifo(1);
  multiple_vob_parts(); udf_priority(); associated_file(); malformed_and_io();
  directory_boundary_and_attributes(); primary_after_boot_record();
  printf("libdvdread ISO9660: %d checks passed\n", checks);
  return 0;
}

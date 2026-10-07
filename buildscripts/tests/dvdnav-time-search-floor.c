/* Real pinned search, VM angle selection, NAV/ILVU and event/read functions.
 * The disc reader and NAV field decoder supply authored in-memory fixtures. */
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dvdread/ifo_read.h>
#include <dvdread/nav_read.h>
#include "dvdnav/dvdnav.h"
#include "vm/decoder.h"
#include "vm/vm.h"
#include "vm/getset.h"
#include "vm/play.h"
#include "dvdnav_internal.h"
#include "read_cache.h"

#undef printerrf
#define printerrf(...) ((void)0)
#define Log0(...) ((void)0)
#define Log1(...) ((void)0)
#define Log2(...) ((void)0)
#define Log3(...) ((void)0)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", name, __LINE__, #c); failures++; } } while (0)
static int failures, cases, commits, copies, closes, fail_copy, fail_sector = -1;
static int missing_dsi;
static int angles, last_read, nav_count, nav_reads;
static uint64_t refusal_target;
static const char *name;
static dvdnav_t nav;
static vm_t live;
static ifo_handle_t vtsi;
static pgc_t pgc;
static cell_playback_t cells[4];
static cell_position_t positions[4];
static uint8_t programs[4] = {1, 2, 3, 4};
static uint32_t sectors[2048];
static vobu_admap_t admap;
static struct { int sector; dsi_t dsi; pci_t pci; } navs[2048];

vm_t *vm_new_copy(vm_t *vm) {
  CHECK(vm == &live);
  if (fail_copy) return NULL;
  vm_t *copy = malloc(sizeof(*copy));
  *copy = *vm;
  copies++;
  return copy;
}
void vm_free_copy(vm_t *vm) { CHECK(vm != &live); closes++; free(vm); }
void vm_merge(vm_t *dst, vm_t *src) { CHECK(dst == &live && src != dst); *dst = *src; commits++; }
int vm_start(vm_t *vm) { (void)vm; CHECK(0); return 0; }
void vm_stop(vm_t *vm) { vm->stopped = 1; }
void vm_get_next_cell(vm_t *vm) { (void)vm; CHECK(0); }
dvd_reader_t *vm_get_dvd_reader(vm_t *vm) { (void)vm; return NULL; }
void DVDCloseFile(dvd_file_t *f) { (void)f; CHECK(0); }
dvd_file_t *DVDOpenFile(dvd_reader_t *r, int title, dvd_read_domain_t d)
{ (void)r; (void)title; (void)d; CHECK(0); return NULL; }
void dvdnav_read_cache_clear(read_cache_t *c) { (void)c; CHECK(0); }
void dvdnav_pre_cache_blocks(read_cache_t *c, int sector, size_t count)
{ (void)c; (void)sector; (void)count; }
int vm_get_subp_active_stream(vm_t *vm, int mode) { (void)vm; (void)mode; return 0; }
int vm_get_audio_active_stream(vm_t *vm) { (void)vm; return 0; }
void set_HL_BTN(vm_t *vm, int button) { (void)vm; (void)button; CHECK(0); }
void vm_get_angle_info(vm_t *vm, int *angle, int *count)
{ *angle = vm->state.AGL_REG; *count = angles; }
int set_PGN(vm_t *vm) { vm->state.pgN = 1; return 1; }
link_t play_PGC_post(vm_t *vm) { (void)vm; CHECK(0); return (link_t){Exit, 0, 0, 0}; }
static int process_command(vm_t *vm, link_t cmd)
{ CHECK(vm != &live || !commits); CHECK(cmd.command == PlayThis); return 1; }
int ifoRead_VTS_TMAPT(ifo_handle_t *ifo) { (void)ifo; return 0; }

static int nav_index(int sector) {
  for (int i = 0; i < nav_count; i++)
    if (navs[i].sector == sector) return i;
  return -1;
}
ssize_t DVDReadBlocks(dvd_file_t *f, int sector, size_t count, unsigned char *buf) {
  CHECK(f == (dvd_file_t *)&vtsi && count == 1);
  last_read = sector;
  if (sector == fail_sector) return -1;
  memset(buf, 0, 2048);
  int index = nav_index(sector);
  if (index < 0) {
    buf[0] = 0x5a;
    memcpy(buf + 1, &sector, sizeof(sector));
    return 1;
  }
  nav_reads++;
  /* Two private_stream_2 packets; field decoders below expose fixture data. */
  buf[2] = 1; buf[3] = 0xbf; buf[5] = 5;
  memcpy(buf + 7, &index, 4);
  buf[13] = 1; buf[14] = 0xbf; buf[16] = 5; buf[17] = 1;
  memcpy(buf + 18, &index, 4);
  if (missing_dsi) buf[17] = 0;
  return 1;
}
int dvdnav_read_cache_block(read_cache_t *c, int sector, size_t count, uint8_t **buf)
{ (void)c; return (int)DVDReadBlocks(nav.file, sector, count, *buf); }
void navRead_PCI(pci_t *pci, unsigned char *bytes) {
  int index; memcpy(&index, bytes, 4); CHECK(index >= 0 && index < nav_count);
  *pci = navs[index].pci;
}
void navRead_DSI(dsi_t *dsi, unsigned char *bytes) {
  int index; memcpy(&index, bytes, 4); CHECK(index >= 0 && index < nav_count);
  *dsi = navs[index].dsi;
}

#include "dvdnav-time-search-floor-under-test.h"

static uint8_t bcd(int x) { return (uint8_t)((x / 10) * 16 + x % 10); }
static dvd_time_t tm(int frames) {
  int seconds = frames / 25;
  return (dvd_time_t){bcd(seconds / 3600), bcd(seconds / 60 % 60),
                      bcd(seconds % 60), (uint8_t)(0x40 | bcd(frames % 25))};
}
static void add_nav(int sector, int frames, int next) {
  int i = nav_count++;
  navs[i].sector = sector;
  navs[i].dsi.dsi_gi.nv_pck_lbn = sector;
  navs[i].dsi.dsi_gi.vobu_ea = 3;
  navs[i].dsi.dsi_gi.c_eltm = tm(frames);
  navs[i].dsi.dsi_gi.vobu_vob_idn = 1;
  navs[i].dsi.dsi_gi.vobu_c_idn = 1;
  navs[i].dsi.vobu_sri.next_vobu = next;
  navs[i].pci.pci_gi.nv_pck_lbn = sector;
  sectors[i] = sector;
}
static void init(const char *test) {
  name = test;
  cases++;
  memset(&nav, 0, sizeof(nav)); memset(&live, 0, sizeof(live));
  memset(&vtsi, 0, sizeof(vtsi)); memset(&pgc, 0, sizeof(pgc));
  memset(cells, 0, sizeof(cells)); memset(navs, 0, sizeof(navs));
  commits = copies = closes = fail_copy = nav_count = missing_dsi = nav_reads = 0;
  fail_sector = -1; angles = 1;
  refusal_target = 60 * 90000;
  pgc.nr_of_cells = pgc.nr_of_programs = 1;
  pgc.cell_playback = cells; pgc.program_map = programs;
  pgc.cell_position = positions;
  for (int i = 0; i < 4; i++) {
    positions[i].vob_id_nr = 1;
    positions[i].cell_nr = i + 1;
  }
  cells[0].playback_time = tm(120 * 25);
  cells[0].last_sector = 25599;
  cells[0].last_vobu_start_sector = 25472;
  pgc.playback_time = cells[0].playback_time;
  vtsi.vts_vobu_admap = &admap;
  admap.vobu_start_sectors = sectors;
  live.vtsi = &vtsi; live.state.pgc = &pgc; live.state.pgcN = 1;
  live.state.cellN = live.state.pgN = 1;
  live.state.AGL_REG = 1;
  live.state.vtsN = 1; live.state.domain = DVD_DOMAIN_VTSTitle;
  nav.vm = &live; nav.file = (dvd_file_t *)&vtsi;
  nav.pgc_based = nav.started = 1; nav.verified_seek_hop = -1;
  pthread_mutex_init(&nav.vm_lock, NULL);
  vm_position_get(&live, &nav.position_current);
}
static int compare_sector(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}
static void ready(void) {
  qsort(sectors, nav_count, sizeof(*sectors), compare_sector);
  admap.last_byte = 4 + nav_count * 4 - 1;
}
static void finish(void) {
  CHECK(copies == closes);
  pthread_mutex_destroy(&nav.vm_lock);
}
static void vbr(const char *test) {
  init(test);
  for (int i = 0; i < 200; i++)
    add_nav(i * 128, i < 100 ? i * 20 : 2000 + (i - 100) * 10,
            i == 199 ? SRI_END_OF_CELL : 128);
  /* The following cell provides the ADMAP upper bound for legacy search. */
  add_nav(25600, 0, SRI_END_OF_CELL);
  ready();
}
static void delivered(uint64_t target, int expected_sector, int64_t expected_time) {
  uint8_t bytes[2048], *buf = bytes;
  int32_t event = 0, len = 0;
  int hops = 0, nav_packets = 0;
  for (int i = 0; i < 12; i++) {
    CHECK(dvdnav_get_next_cache_block(&nav, &buf, &event, &len) == DVDNAV_STATUS_OK);
    if (event == DVDNAV_HOP_CHANNEL) hops++;
    if (event == DVDNAV_NAV_PACKET) {
      nav_packets++;
      CHECK(last_read == expected_sector);
      CHECK(dvdnav_get_current_time(&nav) == expected_time);
      CHECK((uint64_t)dvdnav_get_current_time(&nav) <= target);
    }
    if (event == DVDNAV_BLOCK_OK) break;
  }
  CHECK(hops == 1 && nav_packets == 1 && event == DVDNAV_BLOCK_OK);
  CHECK(len == 2048 && bytes[0] == 0x5a && last_read == expected_sector + 1);
  int payload; memcpy(&payload, bytes + 1, sizeof(payload));
  CHECK(payload == expected_sector + 1);
}
static void refusal(const char *test) {
  name = test;
  vm_t before_vm = live;
  dvdnav_t before = nav;
  CHECK(dvdnav_time_search_floor(&nav, refusal_target) == DVDNAV_STATUS_ERR);
  CHECK(commits == 0 && memcmp(&live, &before_vm, sizeof(live)) == 0);
  /* Error text and lock bookkeeping are not navigation/read state. */
  memcpy(before.err_str, nav.err_str, sizeof(nav.err_str));
  before.vm_lock = nav.vm_lock;
  CHECK(memcmp(&nav, &before, sizeof(nav)) == 0);
  finish();
}
int main(void) {
  vbr("legacy 2s preroll counterexample");
  CHECK(dvdnav_jump_to_sector_by_time(&nav, 58 * 90000, 0) == DVDNAV_STATUS_OK);
  CHECK(live.state.blockN == 12544);
  CHECK(dvdnav_convert_time(&navs[98].dsi.dsi_gi.c_eltm) == 78400 * 90);
  finish();
  vbr("legacy 10s preroll counterexample");
  CHECK(dvdnav_time_search(&nav, 50 * 90000) == DVDNAV_STATUS_OK);
  CHECK(live.state.blockN == 10624);
  CHECK(dvdnav_convert_time(&navs[83].dsi.dsi_gi.c_eltm) == 66400 * 90);
  finish();
  const uint64_t targets[] = {0, 1, 60 * 90000, 60100 * 90, 120 * 90000 - 1};
  const int expected[] = {0, 0, 75, 75, 199};
  for (int i = 0; i < 5; i++) {
    vbr("floor VBR and boundary");
    CHECK(dvdnav_time_search_floor(&nav, targets[i]) == DVDNAV_STATUS_OK);
    CHECK(commits == 1);
    CHECK(nav_reads <= 10);
    delivered(targets[i], expected[i] * 128,
              dvdnav_convert_time(&navs[expected[i]].dsi.dsi_gi.c_eltm));
    finish();
  }
  vbr("unreadable NAV"); fail_sector = 12800; refusal(name);
  vbr("allocation failure"); fail_copy = 1; refusal(name);
  vbr("missing ADMAP"); vtsi.vts_vobu_admap = NULL; refusal(name);
  vbr("malformed NAV"); navs[100].dsi.dsi_gi.nv_pck_lbn++; refusal(name);
  vbr("missing DSI at sector zero"); missing_dsi = 1; refusal(name);
  vbr("missing cell identity"); pgc.cell_position = NULL; refusal(name);
  vbr("unmatched cell identity"); positions[0].cell_nr = 3; refusal(name);
  vbr("nonmonotonic linked NAV");
  navs[76].dsi.dsi_gi.c_eltm = tm(59 * 25);
  refusal_target++; refusal(name);
  vbr("self-loop"); navs[75].dsi.vobu_sri.next_vobu = 0;
  refusal_target++; refusal(name);
  vbr("out-of-cell link"); navs[75].dsi.vobu_sri.next_vobu = 40000;
  refusal_target++; refusal(name);
  vbr("invalid ADMAP order"); sectors[50] = sectors[51] + 1; refusal(name);
  vbr("missing first ADMAP entry"); sectors[0] = 64; refusal(name);
  vbr("first NAV later than target"); navs[0].dsi.dsi_gi.c_eltm = tm(61 * 25); refusal(name);
  vbr("duplicate angle identity");
  pgc.nr_of_cells = 2; cells[1] = cells[0]; positions[1] = positions[0];
  cells[0].block_type = cells[1].block_type = BLOCK_TYPE_ANGLE_BLOCK;
  cells[0].block_mode = BLOCK_MODE_FIRST_CELL; cells[1].block_mode = BLOCK_MODE_LAST_CELL;
  refusal(name);
  vbr("invalid angle"); live.state.AGL_REG = 10; refusal(name);
  vbr("active still"); nav.position_current.still = 5; refusal(name);
  vbr("title end refusal");
  vm_t before = live;
  CHECK(dvdnav_time_search_floor(&nav, 120 * 90000) == DVDNAV_STATUS_ERR);
  CHECK(commits == 0 && memcmp(&live, &before, sizeof(live)) == 0);
  finish();
  for (int target = 0; target < 3; target++) {
    init("cross cell boundary");
    pgc.nr_of_cells = 2;
    cells[0].playback_time = tm(25);
    cells[0].last_sector = 127;
    cells[1].first_sector = 128; cells[1].last_sector = 511;
    cells[1].playback_time = tm(50);
    add_nav(0, 0, SRI_END_OF_CELL);
    add_nav(128, 0, 128); add_nav(256, 25, SRI_END_OF_CELL);
    navs[1].dsi.dsi_gi.vobu_c_idn = navs[2].dsi.dsi_gi.vobu_c_idn = 2;
    ready();
    const uint64_t ticks[] = {89999, 90000, 90001};
    CHECK(dvdnav_time_search_floor(&nav, ticks[target]) == DVDNAV_STATUS_OK);
    delivered(ticks[target], target ? 128 : 0, target ? 90000 : 0);
    finish();
  }
  for (int angle = 1; angle <= 2; angle++) {
    init("angle and ILVU first payload");
    angles = 2; live.state.AGL_REG = angle;
    pgc.nr_of_cells = 2;
    cells[0].block_type = cells[1].block_type = BLOCK_TYPE_ANGLE_BLOCK;
    cells[0].block_mode = BLOCK_MODE_FIRST_CELL;
    cells[1].block_mode = BLOCK_MODE_LAST_CELL;
    cells[0].last_sector = cells[1].last_sector = 511;
    cells[1].first_sector = 64;
    cells[1].playback_time = cells[0].playback_time;
    add_nav(0, 0, 64); add_nav(64, 0, 64);
    add_nav(128, 25, 64); add_nav(192, 25, 64);
    add_nav(256, 50, SRI_END_OF_CELL); add_nav(320, 50, SRI_END_OF_CELL);
    for (int i = 0; i < 6; i++) navs[i].dsi.dsi_gi.vobu_c_idn = i % 2 + 1;
    for (int i = 0; i < 4; i++) {
      navs[i].dsi.sml_pbi.category = DSI_ILVU_BLOCK | DSI_ILVU_LAST;
      navs[i].dsi.sml_pbi.ilvu_ea = 3;
      navs[i].dsi.sml_agli.data[angle - 1].address = 128;
    }
    ready();
    CHECK(dvdnav_time_search_floor(&nav, 0) == DVDNAV_STATUS_OK);
    delivered(0, (angle - 1) * 64, 0);
    CHECK(dvdnav_time_search_floor(&nav, 135000) == DVDNAV_STATUS_OK);
    delivered(135000, 128 + (angle - 1) * 64, 90000);
    finish();
  }
  init("negative ILVU link and pending earlier hop");
  angles = 2;
  cells[0].last_sector = 511;
  add_nav(0, 0, 256); add_nav(256, 25, 128); add_nav(128, 50, SRI_END_OF_CELL);
  navs[1].dsi.sml_pbi.category = DSI_ILVU_BLOCK | DSI_ILVU_LAST;
  navs[1].pci.nsml_agli.nsml_agl_dsta[0] = 0x80000080;
  /* A previous approximate seek has not delivered its HOP yet. */
  live.hop_channel += HOP_SEEK;
  ready();
  CHECK(dvdnav_time_search_floor(&nav, 225000) == DVDNAV_STATUS_OK);
  delivered(225000, 128, 180000);
  finish();
  init("cycle with equal NAV times");
  angles = 2;
  add_nav(0, 0, 128); add_nav(128, 0, 128);
  navs[1].dsi.sml_pbi.category = DSI_ILVU_BLOCK | DSI_ILVU_LAST;
  navs[1].pci.nsml_agli.nsml_agl_dsta[0] = 0x80000080;
  ready();
  refusal(name);
  init("NTSC fractional frame boundary");
  add_nav(0, 0, 128); add_nav(128, 0, 128); add_nav(256, 0, SRI_END_OF_CELL);
  for (int i = 0; i < 3; i++) navs[i].dsi.dsi_gi.c_eltm.frame_u = 0x80 | i;
  ready();
  CHECK(dvdnav_time_search_floor(&nav, 4499) == DVDNAV_STATUS_OK);
  delivered(4499, 128, 3000);
  finish();
  init("one-frame long still cell");
  cells[0].playback_time = tm(60 * 25);
  cells[0].last_sector = cells[0].last_vobu_start_sector = 0;
  add_nav(0, 0, SRI_END_OF_CELL); ready();
  CHECK(dvdnav_time_search_floor(&nav, 30 * 90000) == DVDNAV_STATUS_OK);
  delivered(30 * 90000, 0, 0);
  finish();
  init("64-bit cell elapsed clock");
  cells[0].playback_time = tm(8 * 3600 * 25);
  add_nav(0, 0, 128); add_nav(128, 7 * 3600 * 25, SRI_END_OF_CELL); ready();
  CHECK(dvdnav_time_search_floor(&nav, (uint64_t)7 * 3600 * 90000) == DVDNAV_STATUS_OK);
  delivered((uint64_t)7 * 3600 * 90000, 128, (int64_t)7 * 3600 * 90000);
  finish();
  printf("dvdnav floor: %d cases, %d failures\n", cases, failures);
  return failures != 0;
}

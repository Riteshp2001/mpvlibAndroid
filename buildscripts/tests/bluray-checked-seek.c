/* Production seek functions execute with scripted navigation and I/O results.
 * TEST_STREAM also executes clip opening, seek and buffered reads with mocked
 * file operations. Disc parsing and Android playback are outside this test. */
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BD_DEBUG(...) ((void)0)
#define BLURAY_PLAYER_SETTING_UO_RESTRICTION_SAFE 10
#define BLURAY_PLAYER_SETTING_UO_RESTRICTION_COMPLIANT 20
#define UO_MASK_TIME_SEARCH_MASK_INDEX 3
#define UO_MASK_CHAPTER_SEARCH_INDEX 8
#define BDJ_EVENT_UO_MASKED 4
#define BD_EVENT_SEEK 5
#define BDJ_EVENT_SEEK 6
#define GC_CTRL_PG_RESET 7
#define BD_EVENT_END_OF_TITLE 8
#define BD_EVENT_STILL_TIME 9
#define BD_EVENT_DISCONTINUITY 10
#define BLURAY_STILL_INFINITE 1
#define BLURAY_STILL_TIME 2
#define CONNECT_NON_SEAMLESS 1
#define GC_CTRL_INIT_MENU 11
#define GC_CTRL_PG_UPDATE 12
#define HDMV_PID_PCR 0x1001
#define SPN(x) ((uint32_t)((x) / 192))
#define BD_UNLIKELY(x) (x)
#define TS_PID(p) 0x2000

typedef int bd_uo_mask_index_e;
typedef struct {
    uint32_t duration;
    unsigned angle;
    struct {
        unsigned count;
    } chap_list;
} NAV_TITLE;
typedef struct {
    uint32_t in_time, title_time;
    unsigned ref;
    const char *name;
    uint32_t start_pkt, end_pkt, still_mode, still_time, connection, title_pkt;
} NAV_CLIP;
typedef struct bluray BLURAY;
typedef int BD_MUTEX;
typedef void BD_FILE_H;
typedef struct bd_read_source {
    BLURAY *owner;
    BD_FILE_H *file;
    BD_MUTEX mutex;
    unsigned references;
} BD_READ_SOURCE;
typedef void (*bd_read_origin_proc_f)(void *, BD_READ_SOURCE *, uint64_t,
                                     const uint8_t *, const uint8_t *, uint32_t);

typedef struct {
    void *fp;
    BD_READ_SOURCE *read_source;
    const NAV_CLIP *clip;
    void *m2ts_filter;
    uint64_t clip_pos, clip_block_pos;
    int int_buf_off, seek_flag, ig_pid, pg_pid, eof_hit, encrypted_block_cnt;
    uint64_t clip_size;
} BD_STREAM;
struct bluray {
    NAV_TITLE *title;
    BD_STREAM st0;
    int mutex, uo_restriction_level;
    uint64_t s_pos;
    void *graphics_controller;
    void *disc, *event_queue;
    int seamless_angle_change, end_of_playlist;
    uint32_t angle_change_pkt, angle_change_time;
    unsigned request_angle;
    BD_STREAM st_textst;
    unsigned char int_buf[6144];
    unsigned char original_buf[6144];
    void *read_origin_handle;
    bd_read_origin_proc_f read_origin_proc;
};

static NAV_CLIP sample_clip = {.in_time = 100, .title_time = 10, .name = "00001", .end_pkt = 64};
static NAV_TITLE title;
static BLURAY test_bd;
static int failures, cases, stream_calls, nav_calls, uo_checks, uo_notifications;
static int angle_calls, seek_events, bdj_seek_events, pg_resets, playitem_calls, mark_calls;
static int mask, open_result, open_calls, file_calls;
static int64_t stream_result, file_result;
static uint32_t destination_packet, last_search_tick;
static int expected_uo_index, nav_returns_null;
static unsigned last_chapter;
static int nav_angle_calls, filter_calls, init_calls, close_calls, end_events;
#if TEST_STREAM
typedef struct { int closed; int64_t position; } TestFile;
static TestFile files[4];
static int file_fail_call, block_reads;
#endif

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "line %d: %s\n", __LINE__, #condition);                                \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void bd_mutex_lock(int *mutex)
{
    CHECK(*mutex == 0);
    (*mutex)++;
}
static void bd_mutex_unlock(int *mutex)
{
    CHECK(*mutex == 1);
    (*mutex)--;
}
static int _is_uo_masked(BLURAY *b, int index)
{
    CHECK(b->mutex == 1 && index == expected_uo_index);
    uo_checks++;
    return mask;
}
static void _bdj_event(BLURAY *b, int event, unsigned param)
{
    CHECK(b->mutex == 1);
    if (event == BDJ_EVENT_UO_MASKED) {
        CHECK(param == (unsigned)expected_uo_index);
        uo_notifications++;
    } else {
        CHECK(event == BDJ_EVENT_SEEK);
        bdj_seek_events++;
    }
}
#if TEST_STREAM
static int bd_mutex_init(BD_MUTEX *mutex) {*mutex=0;return 0;}
static void bd_mutex_destroy(BD_MUTEX *mutex) {CHECK(*mutex==0);}
static void _change_angle(BLURAY *b)
{
    CHECK(b->mutex == 1);
    angle_calls++;
}
#endif
static void nav_set_angle(NAV_TITLE *t, unsigned angle)
{
    t->angle = angle;
    sample_clip.name = angle ? "00002" : "00001";
    nav_angle_calls++;
}
static const NAV_CLIP *nav_time_search(NAV_TITLE *t, uint32_t tick, uint32_t *clip_packet,
                                       uint32_t *out_packet)
{
    CHECK(test_bd.mutex == 1 && t == test_bd.title);
    last_search_tick = tick;
    nav_calls++;
    if (nav_returns_null)
        return NULL; /* Like an empty playlist, leave packet outputs untouched. */
    *clip_packet = 4;
    *out_packet = destination_packet;
    return &sample_clip;
}
static const NAV_CLIP *nav_chapter_search(NAV_TITLE *t, unsigned chapter, uint32_t *clip_packet,
                                          uint32_t *out_packet)
{
    CHECK(test_bd.mutex == 1 && t == test_bd.title && chapter < t->chap_list.count);
    last_chapter = chapter;
    nav_calls++;
    if (nav_returns_null)
        return NULL; /* Like an empty playlist, leave packet outputs untouched. */
    *clip_packet = 4;
    *out_packet = destination_packet;
    return &sample_clip;
}
#if TEST_STREAM
static void *disc_open_stream(void *disc, const char *name)
{
    (void)disc;
    (void)name;
    CHECK(test_bd.mutex == 1 && open_calls < 4);
    open_calls++;
    return open_result ? &files[open_calls - 1] : NULL;
}
static int64_t file_size(void *fp)
{
    CHECK(fp != NULL);
    return 12288;
}
static void file_close(void *fp)
{
    TestFile *file = fp;
    CHECK(!file->closed);
    file->closed = 1;
    close_calls++;
}
static void m2ts_filter_close(void **filter)
{
    *filter = NULL;
}
static void _init_main_clip(BLURAY *b, BD_STREAM *st)
{
    CHECK(b->mutex == 1 && st == &b->st0);
    st->m2ts_filter = b;
    init_calls++;
}
static void m2ts_filter_seek(void *filter, int64_t a, int64_t b)
{
    CHECK(filter && a == 0 && b == 200);
    filter_calls++;
}
static int64_t file_seek(void *fp, int64_t offset, int whence)
{
    TestFile *file = fp;
    CHECK(fp && !file->closed && whence == SEEK_SET && test_bd.mutex == 1);
    file_calls++;
    if (file_result < 0 || file_calls == file_fail_call)
        return -1;
    file->position = offset;
    return offset;
}
static const NAV_CLIP *nav_next_clip(NAV_TITLE *t, const NAV_CLIP *c)
{
    (void)t;
    (void)c;
    return NULL;
}
static void _clip_seek_time(BLURAY *b, uint32_t time)
{
    (void)b;
    (void)time;
}
static int _read_block(BLURAY *b, BD_STREAM *st, void *buf)
{
    (void)b;
    (void)st;
    (void)buf;
    block_reads++;
    return -1;
}
static int gc_decode_ts(void *gc, int pid, void *buf, int a, int64_t b)
{
    (void)gc;
    (void)pid;
    (void)buf;
    (void)a;
    (void)b;
    return 0;
}
static void _run_gc(BLURAY *b, int a, int c)
{
    (void)b;
    (void)a;
    (void)c;
}
static void _update_textst_timer(BLURAY *b)
{
    (void)b;
}
#else
static int64_t _seek_stream(BLURAY *b, BD_STREAM *st, const NAV_CLIP *c, uint32_t pkt)
{
    CHECK(b->mutex == 1 && st == &b->st0 && c == &sample_clip && pkt == 4);
    stream_calls++;
    return stream_result;
}
#endif
static uint32_t _update_time_psr_from_stream(BLURAY *b)
{
    CHECK(b->mutex == 1);
    return 120;
}
static void _queue_event(BLURAY *b, int event, uint32_t ticks)
{
    CHECK(b->mutex == 1);
    if (event == BD_EVENT_END_OF_TITLE) {
        end_events++;
        return;
    }
    CHECK(event == BD_EVENT_SEEK && ticks == 30);
    seek_events++;
}
static void _find_next_playmark(BLURAY *b)
{
    CHECK(b->mutex == 1);
}
static void gc_run(void *gc, int op, int arg, void *ptr)
{
    CHECK(gc && op == GC_CTRL_PG_RESET && !arg && !ptr);
    pg_resets++;
}
static void _init_textst_timer(BLURAY *b)
{
    CHECK(b->mutex == 1);
}
static int64_t bd_seek_playitem(BLURAY *b, int item)
{
    CHECK(b->mutex == 1 && item == 1);
    playitem_calls++;
    return 0;
}
static int64_t bd_seek_mark(BLURAY *b, int mark)
{
    CHECK(b->mutex == 1 && mark == 2);
    mark_calls++;
    return 0;
}

static void reset(void)
{
    sample_clip = (NAV_CLIP){.in_time = 100, .title_time = 10, .name = "00001", .end_pkt = 64};
    nav_angle_calls = filter_calls = init_calls = close_calls = end_events = 0;
#if TEST_STREAM
    memset(files, 0, sizeof(files));
    file_fail_call = block_reads = 0;
#endif
    title = (NAV_TITLE){.duration = 450000, .chap_list.count = 2};
    test_bd = (BLURAY){.title = &title,
                       .uo_restriction_level = BLURAY_PLAYER_SETTING_UO_RESTRICTION_SAFE,
                       .s_pos = 321};
    stream_calls = nav_calls = uo_checks = uo_notifications = angle_calls = 0;
    seek_events = bdj_seek_events = pg_resets = playitem_calls = mark_calls = 0;
    mask = 0;
    stream_result = 4;
    destination_packet = 9;
    last_search_tick = 0;
    open_result = 1;
    open_calls = file_calls = 0;
    file_result = 0;
    expected_uo_index = UO_MASK_TIME_SEARCH_MASK_INDEX;
    nav_returns_null = 0;
    last_chapter = UINT_MAX;
    cases++;
}
static void reset_chapter(void)
{
    reset();
    expected_uo_index = UO_MASK_CHAPTER_SEARCH_INDEX;
}
#if !TEST_STREAM
static void refused(void)
{
    CHECK(test_bd.s_pos == 321 && test_bd.mutex == 0 && stream_calls == 0);
    CHECK(seek_events == 0 && bdj_seek_events == 0 && angle_calls == 0);
}
#endif

#include "bluray-checked-seek-under-test.h"

#if TEST_STREAM
static void reading(void)
{
    test_bd.s_pos = 1152;
    test_bd.st0 = (BD_STREAM){.fp = &files[3], .clip = &sample_clip,
                             .m2ts_filter = &test_bd, .clip_pos = 192,
                             .clip_block_pos = 6144, .int_buf_off = 192};
    files[3].position = 6144;
    for (size_t i = 0; i < sizeof(test_bd.int_buf); i++)
        test_bd.int_buf[i] = (unsigned char)i;
}
static void read_after_failure(void)
{
    unsigned char output[384];
    CHECK(test_bd.s_pos == 1152 && test_bd.st0.fp == &files[3] && !files[3].closed);
    CHECK(test_bd.st0.clip && test_bd.st0.clip_pos == 192 &&
          test_bd.st0.clip_block_pos == 6144 && test_bd.st0.int_buf_off == 192);
    CHECK(test_bd.st0.m2ts_filter == &test_bd && filter_calls == 0 && init_calls == 0);
    CHECK(files[3].position == 6144 && seek_events == 0 && bdj_seek_events == 0);
    bd_mutex_lock(&test_bd.mutex);
    CHECK(_bd_read(&test_bd, output, sizeof(output)) == 384);
    CHECK(memcmp(output, test_bd.int_buf + 192, sizeof(output)) == 0);
    CHECK(_bd_read(&test_bd, output, sizeof(output)) == 384);
    CHECK(memcmp(output, test_bd.int_buf + 576, sizeof(output)) == 0);
    bd_mutex_unlock(&test_bd.mutex);
    CHECK(end_events == 0 && block_reads == 0);
}
#endif

#if TEST_STREAM
int main(void)
{

    reset();
    file_result = -1;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 0);
    CHECK(file_calls == 1 && test_bd.s_pos == 321 && seek_events == 0 && bdj_seek_events == 0 &&
          test_bd.mutex == 0);

    reset();
    file_result = -1;
    CHECK(bd_seek_time(&test_bd, 100) == 321);
    CHECK(file_calls == 1 && test_bd.s_pos == 321 && seek_events == 0 && bdj_seek_events == 0 &&
          test_bd.mutex == 0);

    reset();
    file_result = -1;
    mask = 1;
    CHECK(bd_bdj_seek(&test_bd, 0, -1, 100) == 1);
    CHECK(file_calls == 1 && uo_checks == 0 && test_bd.s_pos == 321 && seek_events == 0 &&
          bdj_seek_events == 0 && test_bd.mutex == 0);

    reset();
    open_result = 0;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 0);
    CHECK(open_calls == 1 && file_calls == 0 && test_bd.st0.clip == NULL &&
          test_bd.s_pos == 321 && seek_events == 0 && test_bd.mutex == 0);

    reset();
    CHECK(bd_seek_time_checked(&test_bd, 100) == 1);
    CHECK(open_calls == 1 && file_calls == 2 && test_bd.s_pos == 1728 && seek_events == 1 &&
          bdj_seek_events == 1 && test_bd.st0.seek_flag == 1 && test_bd.mutex == 0);

    reset();
    test_bd.st0.clip = &sample_clip;
    test_bd.st0.fp = &files[3];
    CHECK(bd_seek_time_checked(&test_bd, 100) == 1);
    CHECK(open_calls == 0 && file_calls == 1 && test_bd.s_pos == 1728 && seek_events == 1 &&
          test_bd.mutex == 0);

    /* Chapter APIs must also propagate actual file and clip-open failures. */
    reset_chapter();
    file_result = -1;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 0);
    CHECK(file_calls == 1 && test_bd.s_pos == 321 && seek_events == 0 && bdj_seek_events == 0 &&
          test_bd.mutex == 0);

    reset_chapter();
    file_result = -1;
    CHECK(bd_seek_chapter(&test_bd, 0) == 321);
    CHECK(file_calls == 1 && test_bd.s_pos == 321 && seek_events == 0 && bdj_seek_events == 0 &&
          test_bd.mutex == 0);

    reset_chapter();
    open_result = 0;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 0);
    CHECK(open_calls == 1 && file_calls == 0 && test_bd.s_pos == 321 && seek_events == 0 &&
          test_bd.mutex == 0);

    reset_chapter();
    CHECK(bd_seek_chapter_checked(&test_bd, 1) == 1);
    CHECK(open_calls == 1 && file_calls == 2 && test_bd.s_pos == 1728 && seek_events == 1 &&
          bdj_seek_events == 1 && test_bd.mutex == 0);
    /* A rejected seek must preserve buffered reads, including at clip end. */
    reset();
    reading();
    sample_clip.end_pkt = 6;
    file_result = -1;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 0);
    read_after_failure();

    reset_chapter();
    reading();
    file_result = -1;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 0);
    read_after_failure();

    /* A new clip is not published until open and target seek both succeed. */
    reset();
    reading();
    NAV_CLIP previous = sample_clip;
    previous.ref = 1;
    test_bd.st0.clip = &previous;
    open_result = 0;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 0);
    CHECK(test_bd.st0.clip == &previous && close_calls == 0 && !files[3].closed);
    read_after_failure();

    reset();
    reading();
    test_bd.st0.clip = &previous;
    file_fail_call = 1; /* The new file's initial seek fails. */
    CHECK(bd_seek_time_checked(&test_bd, 100) == 0);
    CHECK(test_bd.st0.clip == &previous && close_calls == 1 && files[0].closed);
    read_after_failure();

    reset_chapter();
    reading();
    test_bd.st0.clip = &previous;
    file_fail_call = 2; /* Opening succeeds, but the target seek fails. */
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 0);
    CHECK(test_bd.st0.clip == &previous && close_calls == 1 && files[0].closed);
    read_after_failure();

    reset();
    reading();
    test_bd.st0.clip = &previous;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 1);
    CHECK(close_calls == 1 && files[3].closed && !files[0].closed);
    CHECK(test_bd.st0.fp == &files[0] && test_bd.st0.clip == &sample_clip && init_calls == 1);
    CHECK(test_bd.st0.clip_pos == 768 && test_bd.st0.clip_block_pos == 0 &&
          test_bd.st0.int_buf_off == 6144 && test_bd.st0.seek_flag == 1);

    /* A queued angle change must not close the old stream on failed seek. */
    reset();
    reading();
    test_bd.seamless_angle_change = 1;
    test_bd.request_angle = 1;
    file_fail_call = 2;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 0);
    CHECK(test_bd.seamless_angle_change == 1 && title.angle == 0 && nav_angle_calls == 2);
    CHECK(strcmp(sample_clip.name, "00001") == 0 && !files[3].closed && files[0].closed);
    CHECK(test_bd.st0.clip_pos == 192 && test_bd.st0.int_buf_off == 192);

    reset_chapter();
    reading();
    test_bd.seamless_angle_change = 1;
    test_bd.request_angle = 1;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 1);
    CHECK(test_bd.seamless_angle_change == 0 && title.angle == 1 && nav_angle_calls == 1);
    CHECK(files[3].closed && test_bd.st0.fp == &files[0] && init_calls == 1);

    reset_chapter();
    reading();
    mask = 1;
    test_bd.uo_restriction_level = BLURAY_PLAYER_SETTING_UO_RESTRICTION_COMPLIANT;
    test_bd.seamless_angle_change = 1;
    test_bd.request_angle = 1;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 0);
    CHECK(title.angle == 0 && nav_angle_calls == 0 && open_calls == 0);
    CHECK(!files[3].closed && test_bd.st0.clip_pos == 192);

    reset();
    reading();
    test_bd.seamless_angle_change = 1;
    test_bd.request_angle = 1;
    nav_returns_null = 1;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 0);
    CHECK(title.angle == 0 && nav_angle_calls == 2 && test_bd.seamless_angle_change == 1);
    CHECK(open_calls == 0 && !files[3].closed);

    printf("libbluray checked seek stream: %d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
#else
int main(void)
{
    /* Checked invalid/no-title/out-of-duration paths must not attempt a seek. */

    reset();
    CHECK(bd_seek_time_checked(&test_bd, UINT64_C(1) << 33) == 0);
    refused();
    CHECK(uo_checks == 0);

    reset();
    CHECK(bd_seek_time_checked(&test_bd, UINT64_MAX) == 0);
    refused();
    CHECK(uo_checks == 0);

    reset();
    test_bd.title = NULL;
    CHECK(bd_seek_time_checked(&test_bd, 0) == 0);
    refused();

    reset();
    CHECK(bd_seek_time_checked(&test_bd, 900000) == 0);
    refused();

    reset();
    CHECK(bd_seek_time_checked(&test_bd, 900001) == 0);
    refused();

    reset();
    title.duration = 0;
    CHECK(bd_seek_time_checked(&test_bd, 0) == 0);
    refused();
    /* UO refusal keeps its original notification and ignore mode is honored. */

    reset();
    mask = 1;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 0);
    refused();
    CHECK(uo_checks == 1 && uo_notifications == 1);

    reset();
    mask = 1;
    test_bd.uo_restriction_level = 0;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 1);
    CHECK(uo_checks == 0 && stream_calls == 1 && test_bd.mutex == 0);
    /* Actual _seek_internal receives a simulated failing stream seek. */

    reset();
    stream_result = -7;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 0);
    CHECK(stream_calls == 1 && nav_calls == 1 && test_bd.s_pos == 321 && seek_events == 0 &&
          bdj_seek_events == 0 && test_bd.mutex == 0);

    reset();
    nav_returns_null = 1;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 0);
    CHECK(nav_calls == 1 && stream_calls == 0 && test_bd.s_pos == 321 && seek_events == 0 &&
          bdj_seek_events == 0 && test_bd.mutex == 0);
    /* Success at byte zero, no-position-change, odd tick and final valid tick. */

    reset();
    destination_packet = 0;
    CHECK(bd_seek_time_checked(&test_bd, 0) == 1);
    CHECK(test_bd.s_pos == 0 && stream_calls == 1 && seek_events == 1 && bdj_seek_events == 1 &&
          test_bd.mutex == 0);

    reset();
    test_bd.s_pos = 1728;
    CHECK(bd_seek_time_checked(&test_bd, 100) == 1);
    CHECK(test_bd.s_pos == 1728 && stream_calls == 1 && seek_events == 1 && test_bd.mutex == 0);

    reset();
    CHECK(bd_seek_time_checked(&test_bd, 101) == 1);
    CHECK(last_search_tick == 50 && test_bd.s_pos == 1728 && test_bd.mutex == 0);

    reset();
    test_bd.graphics_controller = &test_bd;
    CHECK(bd_seek_time_checked(&test_bd, 899999) == 1);
    CHECK(last_search_tick == 449999 && pg_resets == 1 && test_bd.s_pos == 1728 &&
          test_bd.mutex == 0);
    /* Preserve the legacy public return contract on the same outcomes. */

    reset();
    CHECK(bd_seek_time(&test_bd, UINT64_C(1) << 33) == 321);
    refused();

    reset();
    test_bd.title = NULL;
    CHECK(bd_seek_time(&test_bd, 0) == 321);
    refused();

    reset();
    nav_returns_null = 1;
    CHECK(bd_seek_time(&test_bd, 100) == 321);
    CHECK(nav_calls == 1 && stream_calls == 0 && seek_events == 0 && test_bd.mutex == 0);

    reset();
    CHECK(bd_seek_time(&test_bd, 900000) == 321);
    refused();

    reset();
    stream_result = -7;
    CHECK(bd_seek_time(&test_bd, 100) == 321);
    CHECK(stream_calls == 1 && test_bd.s_pos == 321 && test_bd.mutex == 0 && seek_events == 0);

    reset();
    mask = 1;
    CHECK(bd_seek_time(&test_bd, 100) == -1);
    refused();
    CHECK(uo_notifications == 1);

    reset();
    CHECK(bd_seek_time(&test_bd, 100) == 1728);
    CHECK(test_bd.s_pos == 1728 && seek_events == 1 && bdj_seek_events == 1 && test_bd.mutex == 0);
    /* Existing BD-J entry skips UO checks and retains its legacy status. */

    reset();
    mask = 1;
    CHECK(bd_bdj_seek(&test_bd, 0, -1, 100) == 1);
    CHECK(uo_checks == 0 && stream_calls == 1 && test_bd.s_pos == 1728 && test_bd.mutex == 0);

    reset();
    CHECK(bd_bdj_seek(&test_bd, 0, -1, INT64_C(1) << 33) == 1);
    refused();

    reset();
    CHECK(bd_bdj_seek(&test_bd, 1, 2, -1) == 1);
    CHECK(playitem_calls == 1 && mark_calls == 1 && stream_calls == 0 && test_bd.mutex == 0);

    /* Chapter requests are zero based and must stay below the chapter count. */
    reset_chapter();
    test_bd.title = NULL;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 0);
    refused();
    CHECK(nav_calls == 0 && uo_checks == 0);

    reset_chapter();
    title.chap_list.count = 0;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 0);
    refused();

    reset_chapter();
    CHECK(bd_seek_chapter_checked(&test_bd, title.chap_list.count) == 0);
    refused();

    reset_chapter();
    CHECK(bd_seek_chapter_checked(&test_bd, UINT_MAX) == 0);
    refused();

    /* Zero byte position and an unchanged position are successful seeks. */
    reset_chapter();
    destination_packet = 0;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 1);
    CHECK(test_bd.s_pos == 0 && last_chapter == 0 && nav_calls == 1 && seek_events == 1 &&
          bdj_seek_events == 1 && angle_calls == 0 && test_bd.mutex == 0);

    reset_chapter();
    test_bd.s_pos = 1728;
    CHECK(bd_seek_chapter_checked(&test_bd, title.chap_list.count - 1) == 1);
    CHECK(test_bd.s_pos == 1728 && last_chapter == 1 && seek_events == 1 && test_bd.mutex == 0);

    /* Chapter UO checks begin above SAFE, unlike time seek's inclusive gate. */
    reset_chapter();
    mask = 1;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 1);
    CHECK(uo_checks == 0 && uo_notifications == 0 && angle_calls == 0 && test_bd.mutex == 0);

    reset_chapter();
    mask = 1;
    test_bd.uo_restriction_level = 0;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 1);
    CHECK(uo_checks == 0 && stream_calls == 1 && test_bd.mutex == 0);

    reset_chapter();
    mask = 1;
    test_bd.uo_restriction_level = BLURAY_PLAYER_SETTING_UO_RESTRICTION_COMPLIANT;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 0);
    CHECK(uo_checks == 1 && uo_notifications == 1 && angle_calls == 0 && stream_calls == 0 &&
          nav_calls == 0 && test_bd.s_pos == 321 && test_bd.mutex == 0);

    reset_chapter();
    test_bd.uo_restriction_level = BLURAY_PLAYER_SETTING_UO_RESTRICTION_COMPLIANT;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 1);
    CHECK(uo_checks == 1 && uo_notifications == 0 && angle_calls == 0 && test_bd.mutex == 0);

    reset_chapter();
    nav_returns_null = 1;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 0);
    CHECK(nav_calls == 1 && stream_calls == 0 && test_bd.s_pos == 321 && seek_events == 0 &&
          bdj_seek_events == 0 && test_bd.mutex == 0);

    reset_chapter();
    stream_result = -7;
    CHECK(bd_seek_chapter_checked(&test_bd, 0) == 0);
    CHECK(stream_calls == 1 && test_bd.s_pos == 321 && seek_events == 0 && bdj_seek_events == 0 &&
          test_bd.mutex == 0);

    /* Legacy chapter returns current byte position on ordinary failure. */
    reset_chapter();
    test_bd.title = NULL;
    CHECK(bd_seek_chapter(&test_bd, 0) == 321);
    refused();

    reset_chapter();
    CHECK(bd_seek_chapter(&test_bd, title.chap_list.count) == 321);
    refused();

    reset_chapter();
    mask = 1;
    test_bd.uo_restriction_level = BLURAY_PLAYER_SETTING_UO_RESTRICTION_COMPLIANT;
    CHECK(bd_seek_chapter(&test_bd, 0) == -1);
    CHECK(uo_checks == 1 && uo_notifications == 1 && angle_calls == 0 && stream_calls == 0 &&
          test_bd.s_pos == 321 && test_bd.mutex == 0);

    reset_chapter();
    nav_returns_null = 1;
    CHECK(bd_seek_chapter(&test_bd, 0) == 321);
    CHECK(nav_calls == 1 && stream_calls == 0 && seek_events == 0 && test_bd.mutex == 0);

    reset_chapter();
    stream_result = -7;
    CHECK(bd_seek_chapter(&test_bd, 0) == 321);
    CHECK(stream_calls == 1 && seek_events == 0 && bdj_seek_events == 0 && test_bd.mutex == 0);

    reset_chapter();
    destination_packet = 0;
    CHECK(bd_seek_chapter(&test_bd, 0) == 0);
    CHECK(test_bd.s_pos == 0 && seek_events == 1 && test_bd.mutex == 0);

    reset_chapter();
    test_bd.s_pos = 1728;
    CHECK(bd_seek_chapter(&test_bd, 1) == 1728);
    CHECK(seek_events == 1 && bdj_seek_events == 1 && test_bd.mutex == 0);

    reset_chapter();
    mask = 1;
    CHECK(bd_seek_chapter(&test_bd, 0) == 1728);
    CHECK(uo_checks == 0 && angle_calls == 0 && seek_events == 1 && test_bd.mutex == 0);
    printf("libbluray checked seek API: %d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
#endif

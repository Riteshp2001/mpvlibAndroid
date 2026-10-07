/* Run the real file-origin API with scripted file cursors and ownership. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("line %d: %s\n", __LINE__, #x); } } while (0)
typedef int BD_MUTEX;
typedef struct bluray BLURAY;
typedef struct bd_read_source BD_READ_SOURCE;
typedef struct file BD_FILE_H;
struct file {
    int64_t (*read)(BD_FILE_H *, uint8_t *, uint32_t);
    int64_t position;
    int closes, reads, seeks, short_read, seek_failure, tell_failure;
};
typedef void (*bd_read_origin_proc_f)(void *, BD_READ_SOURCE *, uint64_t,
                                     const uint8_t *, const uint8_t *, uint32_t);
typedef struct { BD_FILE_H *fp; BD_READ_SOURCE *read_source; void *m2ts_filter; } BD_STREAM;
struct bluray {
    BD_MUTEX mutex;
    BD_STREAM st0;
    void *read_origin_handle;
    bd_read_origin_proc_f read_origin_proc;
};
static void bd_mutex_lock(BD_MUTEX *mutex) { CHECK(*mutex == 0); *mutex = 1; }
static void bd_mutex_unlock(BD_MUTEX *mutex) { CHECK(*mutex == 1); *mutex = 0; }
static void bd_mutex_destroy(BD_MUTEX *mutex) { CHECK(*mutex == 0); }
static void file_close(BD_FILE_H *file) { CHECK(file->closes == 0); file->closes++; }
static int64_t file_tell(BD_FILE_H *file) { return file->tell_failure ? -1 : file->position; }
static int64_t file_seek(BD_FILE_H *file, int64_t position, int whence) {
    CHECK(whence == SEEK_SET);
    if (++file->seeks == file->seek_failure) return -1;
    file->position = position;
    return position;
}
static void m2ts_filter_close(void **filter) { *filter = NULL; }
static BLURAY owner;
static int64_t read_file(BD_FILE_H *file, uint8_t *buffer, uint32_t size) {
    CHECK(owner.mutex == 1);
    file->reads++;
    for (uint32_t i = 0; i < size; i++) buffer[i] = (uint8_t)(file->position + i);
    file->position += size;
    return file->short_read ? (int64_t)size - 1 : size;
}
#include "bluray-read-origin-under-test.h"

int main(void) {
    BD_FILE_H file = { .read = read_file, .position = 6144 };
    uint8_t bytes[17] = {0};
    CHECK(bd_register_read_origin_proc(&owner, &file, NULL));
    CHECK(owner.read_origin_handle == &file && owner.mutex == 0);
    BD_READ_SOURCE *source = calloc(1, sizeof(*source));
    CHECK(source != NULL);
    source->owner = &owner;
    source->file = &file;
    source->references = 1;
    owner.st0.fp = &file;
    owner.st0.read_source = source;
    CHECK(!bd_register_read_origin_proc(&owner, NULL, NULL));
    CHECK(owner.read_origin_handle == &file);
    bd_retain_read_source(source);
    _close_m2ts(&owner.st0);
    CHECK(owner.st0.fp == NULL && owner.st0.read_source == NULL && file.closes == 0);
    CHECK(source->references == 1);
    CHECK(bd_read_source(&owner, source, 187, bytes, sizeof(bytes)) == sizeof(bytes));
    CHECK(file.position == 6144 && file.reads == 1 && file.seeks == 2);
    for (uint32_t i = 0; i < sizeof(bytes); i++) CHECK(bytes[i] == (uint8_t)(187 + i));
    file.short_read = 1;
    CHECK(bd_read_source(&owner, source, 1, bytes, sizeof(bytes)) == sizeof(bytes) - 1);
    CHECK(file.position == 6144);
    file.short_read = 0;
    file.seek_failure = file.seeks + 1;
    int reads = file.reads;
    CHECK(bd_read_source(&owner, source, 188, bytes, sizeof(bytes)) == -1);
    CHECK(file.position == 6144 && file.reads == reads);
    file.seek_failure = file.seeks + 2;
    CHECK(bd_read_source(&owner, source, 0, bytes, sizeof(bytes)) == -1);
    CHECK(file.position == sizeof(bytes)); /* Failed cursor restoration is reported. */
    file.seek_failure = 0;
    file.tell_failure = 1;
    int seeks = file.seeks;
    CHECK(bd_read_source(&owner, source, 0, bytes, sizeof(bytes)) == -1);
    CHECK(file.seeks == seeks);
    file.tell_failure = 0;
    BLURAY other = {0};
    CHECK(bd_read_source(&other, source, 0, bytes, sizeof(bytes)) == -1);
    CHECK(bd_read_source(&owner, source, UINT64_MAX, bytes, sizeof(bytes)) == -1);
    CHECK(file.seeks == seeks && owner.mutex == 0);
    bd_release_read_source(source);
    CHECK(file.closes == 1);
    owner.st0.fp = &file;
    file.closes = 0;
    _close_m2ts(&owner.st0);
    CHECK(file.closes == 1 && owner.st0.fp == NULL);
    printf("libbluray read origins: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

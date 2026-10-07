/* Exercise the complete production vm_reset with an existing callback reader. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef int (*callback)(void);
typedef struct { callback pf_seek, pf_read, pf_readv; } dvdnav_stream_cb;
typedef struct { struct { unsigned vmg_category; } *vmgi_mat; } ifo_t;
typedef struct {
    struct {
        struct { int SPRM[24], GPRM[16], GPRM_mode[16], GPRM_time[16]; } registers;
        int AST_REG, SPST_REG, AGL_REG, TTN_REG, VTS_TTN_REG, PTTN_REG, HL_BTNN_REG;
        int PTL_REG, pgN, cellN, cell_restart, domain, rsm_vtsN, rsm_cellN, rsm_blockN, vtsN;
    } state;
    int hop_channel;
    void *dvd, *priv;
    ifo_t *vmgi;
    dvdnav_stream_cb streamcb, dvdstreamcb;
    char dvd_name[64], dvd_serial[64];
} vm_t;
#define DVD_DOMAIN_FirstPlay 1
#define DVDREAD_VERSION 0
#define DVDREAD_VERSION_CODE(a,b,c) 1
#define Log0(...) ((void)0)
#define Log1(...) ((void)0)
#define Log2(...) ((void)0)
static int reads, closes, opens, failures;
static int original_read(void) { reads++; return 37; }
static int replacement_read(void) { reads++; return 42; }
static int dvd_reader_seek_handler(void) { return 1; }
static int dvd_reader_read_handler(void) { return 2; }
static int dvd_reader_readv_handler(void) { return 3; }
static void vm_close(vm_t *vm) { closes++; vm->dvd = NULL; }
static void *DVDOpen(const char *root) { (void)root; opens++; return &opens; }
static void *DVDOpenStream(vm_t *vm, dvdnav_stream_cb *cb)
{
    (void)vm; (void)cb; opens++; return &opens;
}
static ifo_t *ifoOpenVMGI(void *dvd) { (void)dvd; return NULL; }
static int ifoRead_FP_PGC(ifo_t *ifo) { (void)ifo; return 1; }
#define ifoRead_TT_SRPT ifoRead_FP_PGC
#define ifoRead_PGCI_UT ifoRead_FP_PGC
#define ifoRead_PTL_MAIT ifoRead_FP_PGC
#define ifoRead_VTS_ATRT ifoRead_FP_PGC
#define ifoRead_VOBU_ADMAP ifoRead_FP_PGC
static int dvd_read_name(vm_t *vm, char *name, char *serial, const char *root)
{ (void)vm; (void)name; (void)serial; (void)root; return 1; }
#include "dvdnav-stream-callbacks-under-test.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c); failures++; } } while (0)
int main(void)
{
    vm_t vm = {0};
    vm.dvd = &vm;
    vm.priv = &vm;
    vm.streamcb = (dvdnav_stream_cb){original_read, original_read, original_read};
    vm.dvdstreamcb = (dvdnav_stream_cb){dvd_reader_seek_handler, dvd_reader_read_handler, dvd_reader_readv_handler};
    CHECK(vm_reset(&vm, NULL, NULL, NULL) == 1);
    CHECK(vm.dvd == &vm && closes == 0 && opens == 0);
    CHECK(vm.streamcb.pf_seek == original_read && vm.streamcb.pf_read == original_read && vm.streamcb.pf_readv == original_read);
    CHECK(vm.dvdstreamcb.pf_seek == dvd_reader_seek_handler && vm.dvdstreamcb.pf_read == dvd_reader_read_handler && vm.dvdstreamcb.pf_readv == dvd_reader_readv_handler);
    if (vm.streamcb.pf_read) CHECK(vm.streamcb.pf_read() == 37);
    CHECK(reads == 1);
    dvdnav_stream_cb replacement = {NULL, replacement_read, NULL};
    /* New device: callback replacement must still happen before opening, even if IFO fails. */
    CHECK(vm_reset(&vm, NULL, &vm, &replacement) == 0);
    CHECK(closes == 1 && opens == 1);
    CHECK(vm.streamcb.pf_read == replacement_read && vm.streamcb.pf_seek == NULL && vm.streamcb.pf_readv == NULL);
    CHECK(vm.dvdstreamcb.pf_read == dvd_reader_read_handler && vm.dvdstreamcb.pf_seek == NULL && vm.dvdstreamcb.pf_readv == NULL);
    printf("stream callback reset: %d failures\n", failures);
    return failures != 0;
}

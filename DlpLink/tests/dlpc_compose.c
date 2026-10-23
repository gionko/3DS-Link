// dlpc_compose.c -- the composer of the Download Play child (AzaharLink/ConfigApp/dlpc/dlpc.c) run from the command line, on a PC, the way the app runs it on the console:
// the model (the resource the app carries), the app's CIA and the plugin go in, the finished child.cia comes out. DlpLink/tests/ship_check.sh uses it to prove that the model
// that SHIPS (the one in the app, made by make_child_model.sh WITHOUT the app) composes a child that verify_cia.py accepts, and that the model is what the child is without its RomFS
// whatever app is put in it.
//   dlpc_compose MODEL APP.cia PLUGIN.3gx VERSION_TEXT OUT.cia [--emu] [--tid HEX]
// VERSION_TEXT is "1.0.0"; the TMD version is worked out from it (major << 10 | minor << 4 | micro). Exit 0 = the child is written and verified by the composer itself.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlpc.h"

static uint8_t* slurp(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s (errno %d)\n", path, errno);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = n > 0 ? malloc((size_t) n) : NULL;
    if (!b || fread(b, 1, (size_t) n, f) != (size_t) n) {
        fprintf(stderr, "cannot read %s\n", path);
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t) n;
    return b;
}

static int read_file(void* user, uint64_t off, uint8_t* buf, size_t len) {
    FILE* f = user;
    return fseek(f, (long) off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len ? 0 : 1;
}

static uint64_t size_of(FILE* f) {
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    return n > 0 ? (uint64_t) n : 0;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: dlpc_compose MODEL APP.cia PLUGIN.3gx VERSION_TEXT OUT.cia [--emu] [--tid HEX]\n");
        return 2;
    }
    int emu = 0;
    uint64_t tid = 0x000400000F83D100ull;
    for (int i = 6; i < argc; ++i) {
        if (!strcmp(argv[i], "--emu")) {
            emu = 1;
        } else if (!strcmp(argv[i], "--tid") && i + 1 < argc) {
            tid = strtoull(argv[++i], NULL, 16);
        } else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    unsigned major = 0, minor = 0, micro = 0;
    if (sscanf(argv[4], "%u.%u.%u", &major, &minor, &micro) != 3 || major > 63 || minor > 63 || micro > 15) {
        fprintf(stderr, "VERSION_TEXT is not M.m.u: %s\n", argv[4]);
        return 2;
    }
    size_t model_size = 0;
    uint8_t* model = slurp(argv[1], &model_size);
    FILE* app = fopen(argv[2], "rb");
    FILE* plugin = fopen(argv[3], "rb");
    if (!model || !app || !plugin) {
        fprintf(stderr, "cannot open an input\n");
        return 1;
    }
    dlpc_job job;
    memset(&job, 0, sizeof(job));
    job.model = model;
    job.model_size = model_size;
    job.app_tid = tid;
    job.version16 = (uint16_t) ((major << 10) | (minor << 4) | micro);
    job.version_text = argv[4];
    job.read_app = read_file;
    job.app_user = app;
    job.app_size = size_of(app);
    job.read_plugin = read_file;
    job.plugin_user = plugin;
    job.plugin_size = size_of(plugin);
    job.out_path = argv[5];
    job.emu_variant = emu;
    char err[200] = "";
    dlpc_result rc = dlpc_compose(&job, err, sizeof(err));
    const unsigned long long out_size = dlpc_output_size(&job);
    fclose(app);
    fclose(plugin);
    free(model);
    if (rc != DLPC_OK) {
        fprintf(stderr, "the composer failed: %d (%s) %s\n", (int) rc, dlpc_result_text(rc), err);
        return 1;
    }
    printf("composed %s (%llu bytes, %s variant, version %u)\n", argv[5], out_size, emu ? "emulator" : "real", (unsigned) job.version16);
    return 0;
}

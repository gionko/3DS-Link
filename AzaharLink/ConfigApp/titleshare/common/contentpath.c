#include "contentpath.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void content_dirs_scan(content_dirs* d, const char* contentDir) {
    d->n = 0;
    d->last = -1;

    DIR* dir = opendir(contentDir);
    if(dir == NULL) {
        return;
    }

    struct dirent* e;
    while((e = readdir(dir)) != NULL && d->n < CONTENT_MAX_SUBDIRS) {
        if(e->d_type != DT_DIR || strcmp(e->d_name, "cmd") == 0 || strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || strlen(e->d_name) >= sizeof(d->sub[0])) {
            continue;
        }

        strcpy(d->sub[d->n++], e->d_name);
    }

    closedir(dir);
}

void content_paths(const content_dirs* d, const char* contentDir, const char* contentRel, uint32_t id, int subIdx, char* path,
                   size_t pathSize, char* rel, size_t relSize) {
    if(subIdx < 0) {
        snprintf(path, pathSize, "%s/%08" PRIx32 ".app", contentDir, id);
        snprintf(rel, relSize, "%s/%08" PRIx32 ".app", contentRel, id);
    } else {
        snprintf(path, pathSize, "%s/%s/%08" PRIx32 ".app", contentDir, d->sub[subIdx], id);
        snprintf(rel, relSize, "%s/%s/%08" PRIx32 ".app", contentRel, d->sub[subIdx], id);
    }
}

int content_locate(content_dirs* d, const char* contentDir, const char* contentRel, uint32_t id, char* path, size_t pathSize,
                   char* rel, size_t relSize, int* subIdx, uint64_t* fileSize) {
    struct stat st;

    // Prima la sottocartella dell'ultimo content trovato, poi content/ stessa, poi le altre.
    int order[CONTENT_MAX_SUBDIRS + 2];
    int cnt = 0;
    if(d->last >= 0) {
        order[cnt++] = d->last;
    }

    order[cnt++] = -1;
    for(int i = 0; i < d->n; i++) {
        if(i != d->last) {
            order[cnt++] = i;
        }
    }

    for(int k = 0; k < cnt; k++) {
        content_paths(d, contentDir, contentRel, id, order[k], path, pathSize, rel, relSize);
        if(stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            *subIdx = order[k];
            *fileSize = (uint64_t) st.st_size;
            if(order[k] >= 0) {
                d->last = order[k];
            }

            return 0;
        }
    }

    return -1;
}

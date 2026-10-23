#include "logrot.h"

#include <stdio.h>

int logrot_name(char* out, size_t size, const char* dir, int index) {
    if(dir == NULL || index < 0 || index > LOGROT_KEEP) {
        return -1;
    }

    int n;
    if(index == 0) {
        n = snprintf(out, size, "%s/3DSLink-TitleDelivery.log", dir);
    } else {
        n = snprintf(out, size, "%s/3DSLink-TitleDelivery.%d.log", dir, index);
    }

    return (n > 0 && (size_t) n < size) ? 0 : -1;
}

int logrot_due(unsigned long long bytes) {
    return bytes >= LOGROT_MAX_BYTES;
}

int logrot_plan(logrot_op* ops, int max) {
    if(max < LOGROT_MAX_OPS) {
        return 0;
    }

    int n = 0;
    ops[n].remove = 1;
    ops[n].from = LOGROT_KEEP;
    ops[n].to = 0;
    n++;
    for(int i = LOGROT_KEEP - 1; i >= 0; i--) {
        ops[n].remove = 0;
        ops[n].from = i;
        ops[n].to = i + 1;
        n++;
    }

    return n;
}

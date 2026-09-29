#include "screen_id.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int screen_id(char *out, size_t n) {
    static const unsigned char hdr[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    char *names[32];
    int count = 0, found = 0;
    if (n) out[0] = '\0';

    // Connectors in name order, so the choice does not depend on readdir.
    DIR *d = opendir("/sys/class/drm");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < 32) {
        // card0-HDMI-A-1 and the like: a connector is card<N>-<name>.
        if (strncmp(e->d_name, "card", 4) != 0 || !strchr(e->d_name, '-')) continue;
        if (strstr(e->d_name, "Writeback")) continue;
        names[count++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, count, sizeof(names[0]), name_cmp);

    for (int i = 0; i < count && !found; i++) {
        char path[256], status[32] = "";
        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", names[i]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(status, sizeof(status), f)) status[0] = '\0';
        fclose(f);
        if (strncmp(status, "connected", 9) != 0) continue;

        // The first connected display is the one: its ID, or none at all if
        // its EDID is missing - never a later connector's.
        found = -1;
        unsigned char edid[128];
        snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", names[i]);
        f = fopen(path, "rb");
        if (!f) continue;
        size_t got = fread(edid, 1, sizeof(edid), f);
        fclose(f);
        if (got < sizeof(edid) || memcmp(edid, hdr, sizeof(hdr)) != 0) continue;

        // Bytes 8-9: three 5-bit letters, 'A' = 1. Bytes 10-11: product, LE.
        const unsigned mfg = (edid[8] << 8) | edid[9];
        const char a = (char)('@' + ((mfg >> 10) & 31));
        const char b = (char)('@' + ((mfg >> 5) & 31));
        const char c = (char)('@' + (mfg & 31));
        if (a < 'A' || a > 'Z' || b < 'A' || b > 'Z' || c < 'A' || c > 'Z') continue;
        snprintf(out, n, "%c%c%c%04X", a, b, c, edid[10] | (edid[11] << 8));
        found = 1;
    }
    for (int i = 0; i < count; i++) free(names[i]);
    return found == 1;
}

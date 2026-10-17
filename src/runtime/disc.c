/*
 * CD image access (cue sheet + raw 2352-byte .bin tracks) and an ISO9660
 * reader used by the HLE BIOS (LoadExec) and tools.
 */
#include <stdlib.h>

#include "iso.h"
#include "psx.h"

#define MAX_TRACKS 99
#define MAX_FILES 99

typedef struct {
    FILE *fp;
    uint32_t start;   /* absolute LBA of first sector in this file */
    uint32_t sectors;
} BinFile;

typedef struct {
    int file;
    bool audio;
    uint32_t index0;  /* absolute LBA */
    uint32_t index1;  /* absolute LBA */
} Track;

static BinFile g_files[MAX_FILES];
static int g_nfiles;
static Track g_tracks[MAX_TRACKS + 1];
static int g_ntracks;
static uint32_t g_total;

static uint32_t msf_to_frames(int m, int s, int f) {
    return (uint32_t)((m * 60 + s) * 75 + f);
}

void disc_close(void) {
    for (int i = 0; i < g_nfiles; i++)
        if (g_files[i].fp)
            fclose(g_files[i].fp);
    memset(g_files, 0, sizeof g_files);
    memset(g_tracks, 0, sizeof g_tracks);
    g_nfiles = g_ntracks = 0;
    g_total = 0;
}

bool disc_open(const char *cue_path) {
    disc_close();
    FILE *cue = fopen(cue_path, "r");
    if (!cue) {
        LOGE("disc", "cannot open %s", cue_path);
        return false;
    }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", cue_path);
    char *slash = strrchr(dir, '/');
    if (slash)
        slash[1] = 0;
    else
        dir[0] = 0;

    char line[1024];
    int cur_file = -1;
    while (fgets(line, sizeof line, cue)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!strncmp(p, "FILE", 4)) {
            char *q1 = strchr(p, '"');
            char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2)
                continue;
            *q2 = 0;
            char path[2048];
            snprintf(path, sizeof path, "%s%s", dir, q1 + 1);
            FILE *fp = fopen(path, "rb");
            if (!fp) {
                LOGE("disc", "cannot open track file %s", path);
                fclose(cue);
                return false;
            }
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            cur_file = g_nfiles++;
            g_files[cur_file].fp = fp;
            g_files[cur_file].sectors = (uint32_t)(sz / 2352);
            g_files[cur_file].start = g_total;
            g_total += g_files[cur_file].sectors;
        } else if (!strncmp(p, "TRACK", 5)) {
            int n;
            char type[32];
            if (sscanf(p, "TRACK %d %31s", &n, type) == 2 && n <= MAX_TRACKS) {
                g_ntracks = n > g_ntracks ? n : g_ntracks;
                g_tracks[n].file = cur_file;
                g_tracks[n].audio = !strncmp(type, "AUDIO", 5);
                g_tracks[n].index0 = g_tracks[n].index1 =
                    g_files[cur_file].start;
            }
        } else if (!strncmp(p, "INDEX", 5)) {
            int idx, m, s, f;
            if (sscanf(p, "INDEX %d %d:%d:%d", &idx, &m, &s, &f) == 4) {
                Track *t = &g_tracks[g_ntracks];
                uint32_t a = g_files[t->file].start + msf_to_frames(m, s, f);
                if (idx == 0)
                    t->index0 = a;
                else if (idx == 1)
                    t->index1 = a;
            }
        }
    }
    fclose(cue);
    LOGI("disc", "opened %s: %d tracks, %u sectors", cue_path, g_ntracks,
         g_total);
    for (int i = 1; i <= g_ntracks; i++)
        LOGD("disc", "track %d %s start %u", i,
             g_tracks[i].audio ? "audio" : "data", g_tracks[i].index1);
    return g_ntracks > 0;
}

bool disc_read_sector(uint32_t lba, uint8_t *raw) {
    for (int i = 0; i < g_nfiles; i++) {
        BinFile *b = &g_files[i];
        if (lba >= b->start && lba < b->start + b->sectors) {
            fseek(b->fp, (long)(lba - b->start) * 2352, SEEK_SET);
            return fread(raw, 1, 2352, b->fp) == 2352;
        }
    }
    memset(raw, 0, 2352);
    return false;
}

uint32_t disc_track_count(void) { return (uint32_t)g_ntracks; }
uint32_t disc_track_start(int track) {
    if (track < 1 || track > g_ntracks)
        return 0;
    return g_tracks[track].index1;
}
bool disc_track_is_audio(int track) {
    return track >= 1 && track <= g_ntracks && g_tracks[track].audio;
}
uint32_t disc_total_sectors(void) { return g_total; }

/* ------------------------------------------------------------------ */
/* ISO9660                                                             */

bool iso_read(uint32_t lba, uint8_t *dst, uint32_t size) {
    uint8_t raw[2352];
    uint32_t done = 0;
    while (done < size) {
        if (!disc_read_sector(lba++, raw))
            return false;
        uint32_t n = size - done < 2048 ? size - done : 2048;
        memcpy(dst + done, raw + 24, n);
        done += n;
    }
    return true;
}

static bool dir_lookup(uint32_t dlba, uint32_t dsize, const char *name,
                       uint32_t *lba, uint32_t *size, bool *isdir) {
    uint8_t *buf = malloc(dsize + 2048);
    if (!iso_read(dlba, buf, dsize)) {
        free(buf);
        return false;
    }
    uint32_t pos = 0;
    size_t nl = strlen(name);
    while (pos < dsize) {
        uint8_t len = buf[pos];
        if (!len) {
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        uint8_t *r = buf + pos;
        uint8_t namelen = r[32];
        const char *nm = (const char *)r + 33;
        size_t cmp = namelen;
        for (size_t k = 0; k < namelen; k++)
            if (nm[k] == ';') { cmp = k; break; }
        if (cmp == nl && !strncasecmp(nm, name, nl)) {
            memcpy(lba, r + 2, 4);
            memcpy(size, r + 10, 4);
            *isdir = (r[25] & 2) != 0;
            free(buf);
            return true;
        }
        pos += len;
    }
    free(buf);
    return false;
}

bool iso_find(const char *path, uint32_t *lba, uint32_t *size) {
    uint8_t pvd[2048];
    if (!iso_read(16, pvd, 2048) || memcmp(pvd + 1, "CD001", 5))
        return false;
    uint32_t dlba, dsize;
    memcpy(&dlba, pvd + 156 + 2, 4);
    memcpy(&dsize, pvd + 156 + 10, 4);
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *semi = strchr(tmp, ';');
    if (semi)
        *semi = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "\\/", &save); tok;
         tok = strtok_r(NULL, "\\/", &save)) {
        bool isdir;
        if (!dir_lookup(dlba, dsize, tok, &dlba, &dsize, &isdir))
            return false;
    }
    *lba = dlba;
    *size = dsize;
    return true;
}

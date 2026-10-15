/*
 * PS5 TabShifter.elf v1.1.0 (FW 12.xx / 13.00 / 13.40 / 13.60 Edition)
 *
 * Changelog in v1.1.0 (Community Feedback Update):
 *  1. FIXED PADLOCKS: Preserves each game's native appType (1 vs 5), stops modifying game param.json
 *     files on disk, and removes sceAppInstUtilAppInstallAll(0) so fpkg/dumped games never get padlocked.
 *  2. FIXED 12.02 COMPATIBILITY: Safely skips missing Title IDs and prevents empty localConceptId matches.
 *  3. REBOOT-PERSISTENT BLOATWARE HIDING: Uses Sony's native hidden dispLocation (1) + visible=0/validFlag=0.
 *  4. PSGFX SYSTEM TILES: Exposes Media Gallery, Disc Player, Music Home, Discover, Welcome, and All Apps
 *     for individual hiding while keeping Welcome Hub untouched during 1-click PSN Bloatware sweeps.
 *  5. MORE TILES ON MAIN SCREEN: Added "Pin All Games to Main Screen" to set pinnedStatus=1 and boost
 *     lastAccessIndex so installed games stay on the main Home Screen bar instead of dropping into Library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ps5/kernel.h>
#include <sqlite3.h>

#define PORT 9095
#define TITLE_ID "TBSH00001"

#define DB_APP_PATH  "/system_data/priv/mms/app.db"
#define DB_INFO_PATH "/system_data/priv/mms/appinfo.db"
#define DB_APP_BAK   "/system_data/priv/mms/app.db.tabshifter.bak"
#define DB_INFO_BAK  "/system_data/priv/mms/appinfo.db.tabshifter.bak"

/* Display & Category Constants */
#define DISP_GAME   138
#define DISP_MEDIA  188
#define DISP_HIDDEN 1   /* Sony native hidden state (matches NPXS40106 Game Streaming) */

#define CONCEPT_DISP_GAME   1162
#define CONCEPT_DISP_MEDIA  2236
#define CONCEPT_DISP_HIDDEN 1

#define CAT_GAME  0
#define CAT_MEDIA 65536

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppUnInstall(const char*);

static volatile int g_running = 1;

static void ps5_notify(const char *fmt, ...) {
    notify_request_t req;
    va_list args;
    memset(&req, 0, sizeof(req));
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, args);
    va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* Finds SceShellUI via sysctl(KERN_PROC_PROC) and sends SIGKILL for instant 1.5s Home Screen reload */
static int restart_sceshellui(void) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t buf_size = 0;

    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) < 0 || buf_size == 0) return -1;
    uint8_t *buf = malloc(buf_size);
    if (!buf) return -1;

    if (sysctl(mib, 4, buf, &buf_size, NULL, 0) < 0) {
        free(buf);
        return -1;
    }

    int killed_pid = -1;
    uint8_t *ptr = buf;
    uint8_t *end = buf + buf_size;

    while (ptr < end) {
        int struct_size = *(int *)ptr;
        if (struct_size <= 0) break;

        if (struct_size >= 464) {
            pid_t pid = *(pid_t *)(ptr + 72);
            for (int off = 0x180; off < struct_size - 16; off++) {
                if (strcmp((char *)(ptr + off), "SceShellUI") == 0 && pid > 1) {
                    kill(pid, SIGKILL);
                    killed_pid = pid;
                    break;
                }
            }
        }
        if (killed_pid > 0) break;
        ptr += struct_size;
    }

    free(buf);
    return killed_pid;
}

static void remove_wal_locks(void) {
    unlink("/system_data/priv/mms/app.db-wal");
    unlink("/system_data/priv/mms/app.db-shm");
    unlink("/system_data/priv/mms/appinfo.db-wal");
    unlink("/system_data/priv/mms/appinfo.db-shm");
}

static void copy_file_if_missing(const char *src_path, const char *dst_path) {
    if (access(dst_path, F_OK) == 0) return;
    FILE *src = fopen(src_path, "rb");
    if (!src) return;
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { fclose(src); return; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);
}

static void force_copy_file(const char *src_path, const char *dst_path) {
    FILE *src = fopen(src_path, "rb");
    if (!src) return;
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { fclose(src); return; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);
}

static void backup_databases_once(void) {
    copy_file_if_missing(DB_APP_PATH, DB_APP_BAK);
    copy_file_if_missing(DB_INFO_PATH, DB_INFO_BAK);
}

/* Pure C CRC32 & Adler32 for dependency-free 512x512 PNG generation */
static uint32_t calc_crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

static uint32_t calc_adler32(const uint8_t *buf, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + buf[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void write_be32(FILE *f, uint32_t val) {
    uint8_t b[4] = { (val >> 24) & 0xff, (val >> 16) & 0xff, (val >> 8) & 0xff, val & 0xff };
    fwrite(b, 1, 4, f);
}

static void write_png_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len) {
    write_be32(f, len);
    uint8_t *crc_buf = malloc(4 + len);
    if (!crc_buf) return;
    memcpy(crc_buf, type, 4);
    if (len > 0 && data) memcpy(crc_buf + 4, data, len);
    fwrite(crc_buf, 1, 4 + len, f);
    uint32_t crc = calc_crc32(crc_buf, 4 + len);
    write_be32(f, crc);
    free(crc_buf);
}

static int generate_ts_icon_png(const char *filepath) {
    const int w = 512, h = 512;
    size_t raw_len = (size_t)h * (1 + w * 3);
    uint8_t *raw = malloc(raw_len);
    if (!raw) return -1;

    size_t idx = 0;
    for (int y = 0; y < h; y++) {
        raw[idx++] = 0;
        for (int x = 0; x < w; x++) {
            uint8_t r = 12, g = 16, b = 28;
            if (x < 18 || x >= w - 18 || y < 18 || y >= h - 18) {
                r = 37; g = 99; b = 235;
            }
            int t_top  = (x >= 95  && x <= 245 && y >= 150 && y <= 195);
            int t_stem = (x >= 148 && x <= 192 && y >= 150 && y <= 365);
            int s_top  = (x >= 275 && x <= 415 && y >= 150 && y <= 192);
            int s_mid  = (x >= 275 && x <= 415 && y >= 236 && y <= 278);
            int s_bot  = (x >= 275 && x <= 415 && y >= 323 && y <= 365);
            int s_ul   = (x >= 275 && x <= 318 && y >= 150 && y <= 278);
            int s_lr   = (x >= 372 && x <= 415 && y >= 236 && y <= 365);

            if (t_top || t_stem || s_top || s_mid || s_bot || s_ul || s_lr) {
                r = 255; g = 255; b = 255;
            }
            raw[idx++] = r;
            raw[idx++] = g;
            raw[idx++] = b;
        }
    }

    size_t num_blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + num_blocks * 5 + raw_len + 4;
    uint8_t *zbuf = malloc(z_len);
    if (!zbuf) { free(raw); return -1; }

    size_t zpos = 0;
    zbuf[zpos++] = 0x78;
    zbuf[zpos++] = 0x01;

    size_t rem = raw_len;
    size_t rpos = 0;
    while (rem > 0) {
        uint16_t blen = (rem > 65535) ? 65535 : (uint16_t)rem;
        uint8_t final_blk = (rem <= 65535) ? 1 : 0;
        uint16_t nlen = ~blen;
        zbuf[zpos++] = final_blk;
        zbuf[zpos++] = blen & 0xff;
        zbuf[zpos++] = (blen >> 8) & 0xff;
        zbuf[zpos++] = nlen & 0xff;
        zbuf[zpos++] = (nlen >> 8) & 0xff;
        memcpy(zbuf + zpos, raw + rpos, blen);
        zpos += blen;
        rpos += blen;
        rem -= blen;
    }

    uint32_t adler = calc_adler32(raw, raw_len);
    zbuf[zpos++] = (adler >> 24) & 0xff;
    zbuf[zpos++] = (adler >> 16) & 0xff;
    zbuf[zpos++] = (adler >> 8) & 0xff;
    zbuf[zpos++] = adler & 0xff;

    FILE *f = fopen(filepath, "wb");
    if (!f) { free(zbuf); free(raw); return -1; }

    const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        0, 0, 2, 0, /* width  = 512 */
        0, 0, 2, 0, /* height = 512 */
        8, 2, 0, 0, 0
    };
    write_png_chunk(f, "IHDR", ihdr, 13);
    write_png_chunk(f, "IDAT", zbuf, (uint32_t)zpos);
    write_png_chunk(f, "IEND", NULL, 0);
    fclose(f);

    free(zbuf);
    free(raw);
    return 0;
}

/*
 * Safe Home Screen Launcher Installer:
 * Uses ONLY sceAppInstUtilAppInstallTitleDir (NEVER calls sceAppInstUtilAppInstallAll,
 * which caused the OS license check padlocks on fpkg games).
 */
static int install_tabshifter_launcher(int force_reinstall) {
    if (!force_reinstall && access("/user/app/" TITLE_ID "/sce_sys/param.json", F_OK) == 0) {
        generate_ts_icon_png("/user/app/" TITLE_ID "/sce_sys/icon0.png");
        return 0;
    }

    if (sceAppInstUtilInitialize() != 0) {
        return -1;
    }

    mkdir("/user/app/" TITLE_ID, 0755);
    mkdir("/user/app/" TITLE_ID "/sce_sys", 0755);

    generate_ts_icon_png("/user/app/" TITLE_ID "/sce_sys/icon0.png");

    const char *pjson =
        "{\n"
        "  \"titleId\": \"" TITLE_ID "\",\n"
        "  \"applicationCategoryType\": 65536,\n"
        "  \"deeplinkUri\": \"http://127.0.0.1:9095/\",\n"
        "  \"localizedParameters\": {\n"
        "    \"defaultLanguage\": \"en-US\",\n"
        "    \"en-US\": {\n"
        "      \"titleName\": \"TabShifter\"\n"
        "    }\n"
        "  }\n"
        "}\n";

    FILE *pf = fopen("/user/app/" TITLE_ID "/sce_sys/param.json", "wb");
    if (pf) {
        fwrite(pjson, 1, strlen(pjson), pf);
        fclose(pf);
    }

    int (*sceAppInstUtilAppInstallTitleDir)(const char*, const char*, void*) = NULL;
    const char *nid = "Wudg3Xe3heE";
    uint32_t handle = 0;

    if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
        sceAppInstUtilAppInstallTitleDir = (void *)kernel_dynlib_resolve(-1, handle, nid);
    }

    int err = -1;
    if (sceAppInstUtilAppInstallTitleDir) {
        err = sceAppInstUtilAppInstallTitleDir(TITLE_ID, "/user/app/", 0);
    }
    /* Note: Never call sceAppInstUtilAppInstallAll(0) here, as it triggers fpkg license padlocks */

    sceAppInstUtilTerminate();
    return err;
}

static void uninstall_tabshifter_launcher(void) {
    if (sceAppInstUtilInitialize() == 0) {
        sceAppInstUtilAppUnInstall(TITLE_ID);
        sceAppInstUtilTerminate();
    }

    unlink("/user/app/" TITLE_ID "/sce_sys/icon0.png");
    unlink("/user/app/" TITLE_ID "/sce_sys/param.json");
    rmdir("/user/app/" TITLE_ID "/sce_sys");
    rmdir("/user/app/" TITLE_ID);
}

static void update_concept_info_json(sqlite3 *db_app, const char *local_cid, int disp_loc, const char *hub_uri) {
    if (!local_cid || !local_cid[0]) return;
    sqlite3_stmt *st = NULL;
    char *q = sqlite3_mprintf("SELECT ConceptInfoJson FROM tbl_conceptmetadata WHERE localConceptId = %Q;", local_cid);
    if (sqlite3_prepare_v2(db_app, q, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *raw = (const char *)sqlite3_column_text(st, 0);
            if (raw) {
                char *dl_pos = strstr(raw, "\"key\":\"display_location\"");
                if (dl_pos) {
                    char *data_pos = dl_pos;
                    while (data_pos > raw && strncmp(data_pos, "\"data\":", 7) != 0) data_pos--;
                    if (strncmp(data_pos, "\"data\":", 7) == 0) {
                        size_t prefix_len = (data_pos - raw) + 7;
                        const char *after_num = data_pos + 7;
                        while (*after_num == ' ' || (*after_num >= '0' && *after_num <= '9')) after_num++;
                        size_t new_len = prefix_len + 32 + strlen(after_num);
                        char *patched = malloc(new_len);
                        if (patched) {
                            memcpy(patched, raw, prefix_len);
                            int written = snprintf(patched + prefix_len, 32, "%d", disp_loc);
                            strcpy(patched + prefix_len + written, after_num);
                            char *u = sqlite3_mprintf("UPDATE tbl_conceptmetadata SET dispLocation = %d, pprHubAppUri = %Q, ConceptInfoJson = %Q WHERE localConceptId = %Q;",
                                                      disp_loc, hub_uri, patched, local_cid);
                            sqlite3_exec(db_app, u, NULL, NULL, NULL);
                            sqlite3_free(u);
                            free(patched);
                        }
                    }
                }
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_free(q);
}

/*
 * Core Tile Updater (v1.1.0):
 * - Preserves each game's original appType (1 for PS5 games, 5 for homebrew) when moving back to Games
 *   so games NEVER get padlocked!
 * - Skips missing Title IDs safely so 12.02 / 13.00 / 13.40 consoles never match empty localConceptIds.
 * - Does not touch game param.json files on disk (prevents "Ongoing" / corrupted param.json lockouts).
 */
static void update_1360_tile(const char *tid, int change_tab, int to_media, int change_vis, int visible) {
    sqlite3 *db_app = NULL;
    sqlite3 *db_info = NULL;

    if (sqlite3_open(DB_APP_PATH, &db_app) != SQLITE_OK) return;
    if (sqlite3_open(DB_INFO_PATH, &db_info) != SQLITE_OK) {
        sqlite3_close(db_app);
        return;
    }

    sqlite3_busy_timeout(db_app, 3000);
    sqlite3_busy_timeout(db_info, 3000);

    sqlite3_exec(db_app, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
    sqlite3_exec(db_info, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);

    char local_cid[128] = {0};
    int curr_disp = DISP_GAME;
    int curr_cat = CAT_GAME;
    int curr_video = 0;
    int curr_apptype = 1;
    int curr_concept_id = 0;
    int row_found = 0;

    sqlite3_stmt *st = NULL;
    char *q = sqlite3_mprintf("SELECT dispLocation, categoryType, is3rdPartyVideoApp, appType, conceptId, localConceptId FROM tbl_contentinfo WHERE titleId = %Q;", tid);
    if (sqlite3_prepare_v2(db_app, q, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            row_found = 1;
            curr_disp = sqlite3_column_int(st, 0);
            curr_cat = sqlite3_column_int(st, 1);
            curr_video = sqlite3_column_int(st, 2);
            curr_apptype = sqlite3_column_int(st, 3);
            curr_concept_id = sqlite3_column_int(st, 4);
            const char *cid_txt = (const char *)sqlite3_column_text(st, 5);
            if (cid_txt) strncpy(local_cid, cid_txt, sizeof(local_cid) - 1);
        }
        sqlite3_finalize(st);
    }
    sqlite3_free(q);

    /* If this titleId does not exist on this console/firmware, abort safely! */
    if (!row_found) {
        sqlite3_close(db_info);
        sqlite3_close(db_app);
        return;
    }

    int is_media = change_tab ? to_media : ((curr_disp == DISP_MEDIA) || (curr_cat == CAT_MEDIA) || (curr_video == 1));
    int is_vis   = change_vis ? visible : (curr_disp > 1);

    int disp_loc     = is_media ? DISP_MEDIA : DISP_GAME;
    int concept_disp = is_media ? CONCEPT_DISP_MEDIA : CONCEPT_DISP_GAME;
    int cat_type     = is_media ? CAT_MEDIA : CAT_GAME;
    int is_video     = is_media ? 1 : 0;
    int vis_flag     = is_vis ? 1 : 0;

    /* Preserve original appType! Only set 0 when moving to Media; restore 1 for retail/fpkg games or 5 for homebrew */
    int app_type_val = curr_apptype;
    if (change_tab) {
        if (is_media) {
            app_type_val = 0;
        } else {
            /* Native PS5 games have conceptId > 0 and start with PPSA/CUSA -> appType 1 */
            if (strncmp(tid, "PPSA", 4) == 0 && curr_concept_id > 0 && strncmp(tid, "PPSA99", 6) != 0) {
                app_type_val = 1;
            } else {
                app_type_val = (curr_apptype > 0) ? curr_apptype : 5;
            }
        }
    }

    char hub_uri[128];
    snprintf(hub_uri, sizeof(hub_uri), is_media ? "psmediahub:main?titleId=%s" : "pshome:gamehub?titleId=%s", tid);

    if (!is_vis) {
        disp_loc = DISP_HIDDEN;
        concept_disp = CONCEPT_DISP_HIDDEN;
    } else if (!change_tab && strncmp(tid, "NPXS", 4) == 0) {
        /* Restore native system dispLocations when unhiding system apps */
        if (strcmp(tid, "NPXS40037") == 0 || strcmp(tid, "NPXS40063") == 0) { disp_loc = 147; concept_disp = 2195; }
        else if (strcmp(tid, "NPXS40047") == 0 || strcmp(tid, "NPXS40071") == 0) { disp_loc = 146; concept_disp = 2194; }
        else if (strcmp(tid, "NPXS40074") == 0 || strcmp(tid, "NPXS40099") == 0) { disp_loc = 155; concept_disp = 2203; }
        else if (strcmp(tid, "NPXS40075") == 0) { disp_loc = 154; concept_disp = 2202; }
        else if (strcmp(tid, "NPXS40140") == 0) { disp_loc = 157; concept_disp = 2205; }
        else if (strcmp(tid, "NPXS40053") == 0 || strcmp(tid, "NPXS40054") == 0 || strcmp(tid, "NPXS40305") == 0) { disp_loc = 149; concept_disp = 2197; }
        else if (strcmp(tid, "NPXS40056") == 0 || strcmp(tid, "NPXS40139") == 0) { disp_loc = 148; concept_disp = 2196; }
    }

    char *sql_info = sqlite3_mprintf(
        "UPDATE tbl_appinfo SET val = %d WHERE titleId = %Q AND key = 'DISPLAYLOCATION';"
        "UPDATE tbl_appinfo SET val = %d WHERE titleId = %Q AND key = 'CATEGORY_TYPE';"
        "UPDATE tbl_appinfo SET val = %Q WHERE titleId = %Q AND key = 'HUBAPP_URI';"
        "UPDATE tbl_appinfo SET val = %d WHERE titleId = %Q AND key = 'APP_TYPE';"
        "UPDATE tbl_version SET status = status + 1 WHERE category LIKE '%%sync_index%%';",
        disp_loc, tid, cat_type, tid, hub_uri, tid, app_type_val, tid);
    sqlite3_exec(db_info, sql_info, NULL, NULL, NULL);
    sqlite3_free(sql_info);

    if (strlen(local_cid) > 0) {
        char *sql_cinfo = sqlite3_mprintf(
            "UPDATE tbl_conceptinfo SET val = %d WHERE local_concept_id = %Q AND key = 'display_location';"
            "UPDATE tbl_conceptinfo SET val = %Q WHERE local_concept_id = %Q AND key = 'hubapp_uri';",
            disp_loc, local_cid, hub_uri, local_cid);
        sqlite3_exec(db_info, sql_cinfo, NULL, NULL, NULL);
        sqlite3_free(sql_cinfo);
    }

    char *sql_app = sqlite3_mprintf(
        "UPDATE tbl_contentinfo SET dispLocation = %d, categoryType = %d, is3rdPartyVideoApp = %d, appType = %d, pprHubAppUri = %Q, "
        "AppInfoJson = json_set(AppInfoJson, '$.DISPLAYLOCATION', %d, '$.CATEGORY_TYPE', %d, '$.HUBAPP_URI', %Q, '$.APP_TYPE', %d) "
        "WHERE titleId = %Q;"
        "UPDATE tbl_version SET status = status + 1 WHERE category LIKE '%%sync_index%%';",
        disp_loc, cat_type, is_video, app_type_val, hub_uri,
        disp_loc, cat_type, hub_uri, app_type_val, tid);
    sqlite3_exec(db_app, sql_app, NULL, NULL, NULL);
    sqlite3_free(sql_app);

    if (strlen(local_cid) > 0) {
        update_concept_info_json(db_app, local_cid, disp_loc, hub_uri);
    }

    sqlite3_stmt *ust = NULL;
    char user_ids[16][32];
    int ucount = 0;
    if (sqlite3_prepare_v2(db_app, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%';", -1, &ust, NULL) == SQLITE_OK) {
        while (sqlite3_step(ust) == SQLITE_ROW && ucount < 16) {
            const char *tname = (const char *)sqlite3_column_text(ust, 0);
            if (tname && strlen(tname) > 13) strncpy(user_ids[ucount++], tname + 13, 31);
        }
        sqlite3_finalize(ust);
    }

    for (int i = 0; i < ucount; i++) {
        char *usql = NULL;
        if (strlen(local_cid) > 0) {
            usql = sqlite3_mprintf(
                "UPDATE tbl_iconinfo_%s SET dispLocation = %d, visible = %d, hubAppUri = %Q, emsDeletedFlag = %d WHERE titleId = %Q;"
                "UPDATE tbl_concepticoninfo_%s SET dispLocation = %d, validFlag = %d, hubAppUri = %Q WHERE primaryTitleId = %Q OR localConceptId = %Q;",
                user_ids[i], disp_loc, vis_flag, hub_uri, is_vis ? 0 : 1, tid,
                user_ids[i], concept_disp, vis_flag, hub_uri, tid, local_cid);
        } else {
            usql = sqlite3_mprintf(
                "UPDATE tbl_iconinfo_%s SET dispLocation = %d, visible = %d, hubAppUri = %Q, emsDeletedFlag = %d WHERE titleId = %Q;"
                "UPDATE tbl_concepticoninfo_%s SET dispLocation = %d, validFlag = %d, hubAppUri = %Q WHERE primaryTitleId = %Q;",
                user_ids[i], disp_loc, vis_flag, hub_uri, is_vis ? 0 : 1, tid,
                user_ids[i], concept_disp, vis_flag, hub_uri, tid);
        }
        sqlite3_exec(db_app, usql, NULL, NULL, NULL);
        sqlite3_free(usql);

        if (!is_vis) {
            char *dsql = sqlite3_mprintf(
                "DELETE FROM tbl_group_%s WHERE itemId = %Q;"
                "INSERT OR REPLACE INTO tbl_info_%s (key, value) VALUES ('nsx_content_flag_%s', '{\"deleted\":true,\"pushed\":true}');",
                user_ids[i], tid, user_ids[i], tid);
            sqlite3_exec(db_app, dsql, NULL, NULL, NULL);
            sqlite3_free(dsql);
        }
    }

    sqlite3_close(db_info);
    sqlite3_close(db_app);
    remove_wal_locks();
}

/*
 * NEW Feature: Pin All Installed Games to Main Home Screen Bar
 * Sets pinnedStatus = 1 and boosts lastAccessIndex across tbl_iconinfo_* & tbl_concepticoninfo_*
 * so games stay on the main PS5 Home Screen instead of getting pushed into the Game Library.
 */
static void pin_all_games_to_main_screen(void) {
    sqlite3 *db_app = NULL;
    if (sqlite3_open(DB_APP_PATH, &db_app) != SQLITE_OK) return;
    sqlite3_busy_timeout(db_app, 3000);
    sqlite3_exec(db_app, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);

    /* Boost lastAccessIndex & sortPriority on all visible non-system games */
    sqlite3_exec(db_app,
        "UPDATE tbl_contentinfo SET sortPriority = 100, lastAccessIndex = lastAccessIndex + 500 "
        "WHERE titleId NOT LIKE 'NPXS%' AND dispLocation > 1;",
        NULL, NULL, NULL);

    sqlite3_stmt *ust = NULL;
    char user_ids[16][32];
    int ucount = 0;
    if (sqlite3_prepare_v2(db_app, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%';", -1, &ust, NULL) == SQLITE_OK) {
        while (sqlite3_step(ust) == SQLITE_ROW && ucount < 16) {
            const char *tname = (const char *)sqlite3_column_text(ust, 0);
            if (tname && strlen(tname) > 13) strncpy(user_ids[ucount++], tname + 13, 31);
        }
        sqlite3_finalize(ust);
    }

    for (int i = 0; i < ucount; i++) {
        char *psql = sqlite3_mprintf(
            "UPDATE tbl_iconinfo_%s SET pinnedStatus = 1, visible = 1, lastAccessIndex = lastAccessIndex + 500 "
            "WHERE titleId NOT LIKE 'NPXS%%' AND dispLocation > 1;"
            "UPDATE tbl_concepticoninfo_%s SET validFlag = 1, lastAccessIndex = lastAccessIndex + 500 "
            "WHERE primaryTitleId NOT LIKE 'NPXS%%' AND dispLocation > 1;",
            user_ids[i], user_ids[i]);
        sqlite3_exec(db_app, psql, NULL, NULL, NULL);
        sqlite3_free(psql);
    }

    sqlite3_close(db_app);
    remove_wal_locks();
}

/* 1-Click Bloatware Toggle (Covers Both 13.xx and 4.xx-12.xx PSN Bloat IDs, Leaves Welcome Hub Untouched!) */
static void toggle_all_bloatware(int visible) {
    const char *bloat_ids[] = {
        "NPXS40037", "NPXS40047", "NPXS40074", "NPXS40099", "NPXS40305", /* 13.xx IDs */
        "NPXS40002", "NPXS40003", "NPXS40007", "NPXS40008"               /* 4.xx-12.xx IDs */
    };
    for (int i = 0; i < 9; i++) {
        update_1360_tile(bloat_ids[i], 0, 0, 1, visible);
    }
}

/* System Tiles Allowed in the Dashboard List (Includes PSGFX-style System Apps!) */
static int is_manageable_system_id(const char *tid) {
    const char *sys_ids[] = {
        "NPXS40037", /* PS Plus */
        "NPXS40047", /* PS Store */
        "NPXS40074", /* Remote Play */
        "NPXS40099", /* Share Play */
        "NPXS40063", /* Welcome Hub */
        "NPXS40305", /* Live TV */
        "NPXS40075", /* Media Gallery */
        "NPXS40140", /* Disc Player */
        "NPXS40054", /* Music Home */
        "NPXS40053", /* Discover */
        "NPXS40056", /* All Apps */
        "NPXS40002", "NPXS40003", "NPXS40007", "NPXS40008", "NPXS40087"
    };
    for (int i = 0; i < 16; i++) {
        if (strcmp(tid, sys_ids[i]) == 0) return 1;
    }
    return 0;
}

static void send_dashboard_html(int client_fd) {
    const char *header =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>PS5 TabShifter v1.1.0</title>"
        "<style>"
        "body{background:#080a10;color:#f0f4f8;font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;padding:24px;max-width:880px;margin:auto;}"
        "h1{font-size:24px;margin:0 0 6px 0;color:#fff;} .sub{color:#8b949e;font-size:14px;margin-bottom:18px;}"
        ".toolbar{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:20px;}"
        ".tbtn{flex:1;min-width:145px;text-align:center;padding:13px 10px;border-radius:12px;text-decoration:none;font-weight:700;font-size:13px;color:#fff;border:2px solid transparent;}"
        ".tbtn:focus,.tbtn:hover,.btn:focus,.btn:hover{border-color:#fff;transform:scale(1.02);}"
        ".b-hide{background:#7c3aed;} .b-show{background:#334155;} .b-pin{background:#0284c7;} .b-res{background:#b45309;} .b-uninst{background:#dc2626;}"
        ".card{background:#121622;border:1px solid #22293a;border-radius:14px;padding:14px 18px;margin-bottom:10px;display:flex;justify-content:space-between;align-items:center;gap:12px;}"
        ".title{font-weight:700;font-size:16px;color:#fff;} .meta{color:#8b949e;font-size:13px;margin-top:4px;}"
        ".badge-m{background:#1d4ed8;color:#fff;padding:2px 8px;border-radius:6px;font-size:11px;font-weight:700;}"
        ".badge-g{background:#059669;color:#fff;padding:2px 8px;border-radius:6px;font-size:11px;font-weight:700;}"
        ".actions{display:flex;gap:8px;}"
        ".btn{padding:10px 15px;border-radius:10px;text-decoration:none;font-weight:700;font-size:13px;border:2px solid transparent;}"
        ".to-media{background:#2563eb;color:#fff;} .to-game{background:#10b981;color:#fff;}"
        ".h-btn{background:#1e293b;color:#cbd5e1;border:1px solid #334155;}"
        "</style></head><body>"
        "<h1>PS5 TabShifter v1.1.0</h1>"
        "<div class='sub'>Move apps between <b>Games</b> &amp; <b>Media</b> tabs, pin games to main screen, or hide system bloatware.</div>"
        "<div class='toolbar'>"
        "<a class='tbtn b-hide' href='/bloat?v=0'>Hide PSN Bloatware</a>"
        "<a class='tbtn b-show' href='/bloat?v=1'>Unhide Bloatware</a>"
        "<a class='tbtn b-pin' href='/pin_all'>Pin All Games to Main Screen</a>"
        "<a class='tbtn b-res' href='/restore'>Restore DB Backup</a>"
        "<a class='tbtn b-uninst' href='/uninstall'>Uninstall Tile &amp; Exit</a>"
        "</div>";

    write(client_fd, header, strlen(header));

    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open(DB_APP_PATH, &db) == SQLITE_OK) {
        sqlite3_busy_timeout(db, 3000);
        const char *q = "SELECT titleId, titleName, dispLocation, categoryType, is3rdPartyVideoApp FROM tbl_contentinfo ORDER BY (dispLocation > 1) DESC, (titleId LIKE 'NPXS%') ASC, titleName ASC;";
        if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *tid = (const char *)sqlite3_column_text(stmt, 0);
                const char *tname = (const char *)sqlite3_column_text(stmt, 1);
                int disp = sqlite3_column_int(stmt, 2);
                int ctype = sqlite3_column_int(stmt, 3);
                int is_vid = sqlite3_column_int(stmt, 4);

                if (!tid || strcmp(tid, TITLE_ID) == 0) continue;
                if (strncmp(tid, "NPXS", 4) == 0 && !is_manageable_system_id(tid)) continue;

                int is_media = (disp == DISP_MEDIA) || (ctype == CAT_MEDIA) || (is_vid == 1);
                int is_vis = (disp > 1);

                char row[1400];
                snprintf(row, sizeof(row),
                    "<div class='card' style='opacity:%s'>"
                    "<div><div class='title'>%s %s</div>"
                    "<div class='meta'><span class='%s'>%s</span> &bull; %s</div></div>"
                    "<div class='actions'>"
                    "<a class='btn %s' href='/shift?tid=%s&media=%d'>Move to %s</a>"
                    "<a class='btn h-btn' href='/vis?tid=%s&v=%d'>%s</a>"
                    "</div></div>",
                    is_vis ? "1.0" : "0.45",
                    tname ? tname : tid,
                    is_vis ? "" : "(HIDDEN)",
                    is_media ? "badge-m" : "badge-g",
                    is_media ? "MEDIA TAB" : "GAMES TAB",
                    tid,
                    is_media ? "to-game" : "to-media", tid, is_media ? 0 : 1, is_media ? "Games" : "Media",
                    tid, is_vis ? 0 : 1, is_vis ? "Hide" : "Unhide");
                write(client_fd, row, strlen(row));
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    const char *footer = "</body></html>";
    write(client_fd, footer, strlen(footer));
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    syscall(SYS_thr_set_name, -1, "TabShifter.elf");

    backup_databases_once();
    install_tabshifter_launcher(0);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return 1;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        install_tabshifter_launcher(1);
        ps5_notify("TabShifter Tile Reinstalled on Media Tab!");
        close(server_fd);
        return 0;
    }

    listen(server_fd, 5);
    ps5_notify("PS5 TabShifter v1.1.0 Ready!\nOpen TabShifter from your Media Tab!");

    while (g_running) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;

        char req[1024] = {0};
        ssize_t n = read(client, req, sizeof(req) - 1);
        if (n <= 0) {
            close(client);
            continue;
        }

        if (strncmp(req, "GET /shift?tid=", 15) == 0) {
            char tid[32] = {0};
            int to_media = 0;
            sscanf(req + 15, "%31[^&]&media=%d", tid, &to_media);
            update_1360_tile(tid, 1, to_media, 0, 1);
            ps5_notify("Moved %s -> %s Tab!", tid, to_media ? "Media" : "Games");
            restart_sceshellui();
            const char *redir = "HTTP/1.1 302 Found\r\nLocation: /\r\nContent-Length: 0\r\n\r\n";
            write(client, redir, strlen(redir));
        } else if (strncmp(req, "GET /vis?tid=", 13) == 0) {
            char tid[32] = {0};
            int v = 0;
            sscanf(req + 13, "%31[^&]&v=%d", tid, &v);
            update_1360_tile(tid, 0, 0, 1, v);
            ps5_notify("%s %s!", v ? "Unhid" : "Hid", tid);
            restart_sceshellui();
            const char *redir = "HTTP/1.1 302 Found\r\nLocation: /\r\nContent-Length: 0\r\n\r\n";
            write(client, redir, strlen(redir));
        } else if (strncmp(req, "GET /bloat?v=", 13) == 0) {
            int v = atoi(req + 13);
            toggle_all_bloatware(v);
            ps5_notify("%s PSN Bloatware!", v ? "Restored" : "Hid");
            restart_sceshellui();
            const char *redir = "HTTP/1.1 302 Found\r\nLocation: /\r\nContent-Length: 0\r\n\r\n";
            write(client, redir, strlen(redir));
        } else if (strncmp(req, "GET /pin_all", 12) == 0) {
            pin_all_games_to_main_screen();
            ps5_notify("Pinned All Games to Main Home Screen!");
            restart_sceshellui();
            const char *redir = "HTTP/1.1 302 Found\r\nLocation: /\r\nContent-Length: 0\r\n\r\n";
            write(client, redir, strlen(redir));
        } else if (strncmp(req, "GET /restore", 12) == 0) {
            force_copy_file(DB_INFO_BAK, DB_INFO_PATH);
            force_copy_file(DB_APP_BAK, DB_APP_PATH);
            remove_wal_locks();
            ps5_notify("Restored Original Databases!");
            restart_sceshellui();
            const char *redir = "HTTP/1.1 302 Found\r\nLocation: /\r\nContent-Length: 0\r\n\r\n";
            write(client, redir, strlen(redir));
        } else if (strncmp(req, "GET /uninstall", 14) == 0) {
            const char *bye = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
                              "<body style='background:#080a10;color:#fff;font-family:sans-serif;text-align:center;padding-top:100px;'>"
                              "<h1>TabShifter Tile Uninstalled &amp; Payload Stopped.</h1>"
                              "<p>Press the PS Button to return to the Home Screen.</p></body>";
            write(client, bye, strlen(bye));
            close(client);
            uninstall_tabshifter_launcher();
            ps5_notify("TabShifter Tile Uninstalled & Stopped!");
            g_running = 0;
            break;
        } else {
            send_dashboard_html(client);
        }
        close(client);
    }

    close(server_fd);
    return 0;
}


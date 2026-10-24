#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sqlite3.h>

#define TABSHIFTER_VERSION      "1.3.0"
#define HTTP_PORT               9095
#define TBSH_TITLE_ID           "TBSH00001"
#define DEFAULT_ELF_FALLBACK    "/data/pldmgr/payloads/TabShifter.elf"

#define APP_DB_PATH             "/system_data/priv/mms/app.db"
#define APPINFO_DB_PATH         "/system_data/priv/mms/appinfo.db"

#define DISP_LOCATION_HIDDEN    1
#define DISP_LOCATION_GAMES     138
#define DISP_LOCATION_MEDIA     188

/* PROSPERO CONCEPT BITMASKS (CRITICAL: PREVENTS PADLOCKS) */
#define CONCEPT_LOC_GAMES       1162
#define CONCEPT_LOC_MEDIA       2236
#define CONCEPT_LOC_HIDDEN      1

#define SQLITE_BUSY_TIMEOUT_MS  10000
#define MAX_USER_PROFILES       16

/* ========================================================================= */
/*                     PS5 KERNEL NOTIFICATION & SHELL RELOAD                */
/* ========================================================================= */

typedef struct {
    int type;
    int req_id;
    int priority;
    int msg_id;
    int target_id;
    int user_id;
    int unk1;
    int unk2;
    int app_id;
    int error_num;
    int unk3;
    unsigned char use_icon_image_uri;
    char message[1024];
    char icon_uri[1024];
    char unk[1024];
} OrbisNotificationRequest;

extern int sceKernelSendNotificationRequest(int device, OrbisNotificationRequest *req, size_t size, int blocking);

static void notify_ps5(const char *fmt, ...) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.target_id = -1;
    req.use_icon_image_uri = 1;
    strncpy(req.icon_uri, "cxml://psnotification/tex_icon_system", sizeof(req.icon_uri) - 1);

    va_list args;
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message) - 1, fmt, args);
    va_end(args);

    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void reload_sceshellui(void) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t buf_size = 0;

    if (sysctl(mib, 3, NULL, &buf_size, NULL, 0) < 0 || buf_size == 0) return;

    uint8_t *buf = malloc(buf_size * 2);
    if (!buf) return;
    buf_size *= 2;

    if (sysctl(mib, 3, buf, &buf_size, NULL, 0) == 0) {
        uint8_t *ptr = buf;
        uint8_t *end = buf + buf_size;
        while (ptr + sizeof(int) < end) {
            int struct_size = *(int *)ptr;
            if (struct_size <= 0) break;
            for (int off = 0; off < struct_size - 12; off++) {
                if (memcmp(ptr + off, "SceShellUI", 10) == 0) {
                    pid_t pid = *(pid_t *)(ptr + 72);
                    if (pid <= 0 || pid > 99999) {
                        pid = *(pid_t *)(ptr + 40);
                    }
                    if (pid > 10 && pid < 99999) {
                        kill(pid, SIGKILL);
                        free(buf);
                        return;
                    }
                }
            }
            ptr += struct_size;
        }
    }
    free(buf);
}

/* ========================================================================= */
/*          PROCEDURAL "TS" LOGO PNG GENERATOR (ZERO EXTERNAL DEPS)          */
/* ========================================================================= */

static uint32_t crc32_calc(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
    return ~crc;
}

static uint32_t adler32_calc(const uint8_t *buf, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + buf[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;  p[3] = v & 0xFF;
}

static void write_png_chunk(FILE *fp, const char type[4], const uint8_t *data, uint32_t len) {
    uint8_t len_b[4], crc_b[4];
    write_be32(len_b, len);
    fwrite(len_b, 1, 4, fp);
    fwrite(type, 1, 4, fp);
    if (len > 0 && data) fwrite(data, 1, len, fp);

    uint8_t *crc_buf = malloc(4 + len);
    memcpy(crc_buf, type, 4);
    if (len > 0 && data) memcpy(crc_buf + 4, data, len);
    uint32_t crc = crc32_calc(crc_buf, 4 + len);
    free(crc_buf);

    write_be32(crc_b, crc);
    fwrite(crc_b, 1, 4, fp);
}

static void generate_ts_icon_png(const char *filepath) {
    const int W = 128, H = 128;
    size_t row_bytes = 1 + W * 3;
    size_t raw_size = H * row_bytes;
    uint8_t *raw = malloc(raw_size);
    if (!raw) return;

    const uint8_t glyph_T[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
    const uint8_t glyph_S[7] = { 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E };

    for (int y = 0; y < H; y++) {
        uint8_t *row = raw + y * row_bytes;
        row[0] = 0;
        for (int x = 0; x < W; x++) {
            uint8_t r = 10, g = 24 + (y / 3), b = 58 + (y / 2);
            if (x < 4 || x >= W - 4 || y < 4 || y >= H - 4) {
                r = 0; g = 112; b = 209;
            }
            int scale = 8;
            int start_y = (H - 7 * scale) / 2;
            int start_x_T = 16;
            int start_x_S = 68;

            if (y >= start_y && y < start_y + 7 * scale) {
                int gy = (y - start_y) / scale;
                if (x >= start_x_T && x < start_x_T + 5 * scale) {
                    int gx = 4 - ((x - start_x_T) / scale);
                    if ((glyph_T[gy] >> gx) & 1) { r = 255; g = 255; b = 255; }
                }
                if (x >= start_x_S && x < start_x_S + 5 * scale) {
                    int gx = 4 - ((x - start_x_S) / scale);
                    if ((glyph_S[gy] >> gx) & 1) { r = 56; g = 189; b = 248; }
                }
            }
            row[1 + x * 3 + 0] = r;
            row[1 + x * 3 + 1] = g;
            row[1 + x * 3 + 2] = b;
        }
    }

    size_t max_zlib = raw_size + (raw_size / 32000 + 2) * 5 + 6;
    uint8_t *zbuf = malloc(max_zlib);
    size_t zpos = 0;
    zbuf[zpos++] = 0x78; zbuf[zpos++] = 0x01;

    size_t rem = raw_size, off = 0;
    while (rem > 0) {
        uint16_t bsize = (rem > 32000) ? 32000 : (uint16_t)rem;
        rem -= bsize;
        zbuf[zpos++] = (rem == 0) ? 0x01 : 0x00;
        zbuf[zpos++] = bsize & 0xFF;
        zbuf[zpos++] = (bsize >> 8) & 0xFF;
        uint16_t nlen = ~bsize;
        zbuf[zpos++] = nlen & 0xFF;
        zbuf[zpos++] = (nlen >> 8) & 0xFF;
        memcpy(zbuf + zpos, raw + off, bsize);
        zpos += bsize;
        off += bsize;
    }

    uint32_t adler = adler32_calc(raw, raw_size);
    write_be32(zbuf + zpos, adler);
    zpos += 4;

    FILE *fp = fopen(filepath, "wb");
    if (fp) {
        const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        fwrite(sig, 1, 8, fp);
        uint8_t ihdr[13] = { 0 };
        write_be32(ihdr, W); write_be32(ihdr + 4, H);
        ihdr[8] = 8; ihdr[9] = 2;
        write_png_chunk(fp, "IHDR", ihdr, 13);
        write_png_chunk(fp, "IDAT", zbuf, (uint32_t)zpos);
        write_png_chunk(fp, "IEND", NULL, 0);
        fclose(fp);
        chmod(filepath, 0777);
    }
    free(zbuf);
    free(raw);
}

/* ========================================================================= */
/*                    SELF-UPDATER & INSTANCE PORT HANDOVER                  */
/* ========================================================================= */

static void kill_existing_tabshifter_instance(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct timeval tv = { 0, 300000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HTTP_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        const char *req = "POST /api/shutdown HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
        send(sock, req, strlen(req), 0);
        close(sock);
        usleep(400000);
    } else {
        close(sock);
    }
}

static void get_running_elf_path(char *out_path, size_t max_len) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t len = max_len;
    if (sysctl(mib, 4, out_path, &len, NULL, 0) == 0 && len > 1 && out_path[0] == '/') return;

    struct stat st;
    if (stat("/data/pldmgr/payloads", &st) == 0) {
        strncpy(out_path, "/data/pldmgr/payloads/TabShifter.elf", max_len - 1);
    } else {
        mkdir("/data/payloads", 0777);
        strncpy(out_path, "/data/payloads/TabShifter.elf", max_len - 1);
    }
    out_path[max_len - 1] = '\0';
}

static int apply_self_update(const uint8_t *binary_data, size_t binary_len) {
    if (!binary_data || binary_len < 1024) return -1;
    if (binary_data[0] != 0x7F || binary_data[1] != 'E' ||
        binary_data[2] != 'L'  || binary_data[3] != 'F') return -2;

    char target_path[512], temp_path[540];
    get_running_elf_path(target_path, sizeof(target_path));
    snprintf(temp_path, sizeof(temp_path), "%s.new", target_path);

    FILE *fp = fopen(temp_path, "wb");
    if (!fp) return -3;
    size_t written = fwrite(binary_data, 1, binary_len, fp);
    fflush(fp); fsync(fileno(fp)); fclose(fp);

    if (written != binary_len) { unlink(temp_path); return -4; }
    chmod(temp_path, 0777);
    if (rename(temp_path, target_path) != 0) { unlink(temp_path); return -5; }
    return 0;
}

/* ========================================================================= */
/*                   MULTI-USER PROFILE & TABLE DISCOVERY                    */
/* ========================================================================= */

static int get_all_user_ids(sqlite3 *db, char uids[MAX_USER_PROFILES][32]) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%';";
    int count = 0;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        const char *prefix = "tbl_iconinfo_";
        size_t prefix_len = strlen(prefix);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < MAX_USER_PROFILES) {
            const char *tname = (const char *)sqlite3_column_text(stmt, 0);
            if (tname && strncmp(tname, prefix, prefix_len) == 0) {
                strncpy(uids[count], tname + prefix_len, 31);
                uids[count][31] = '\0';
                count++;
            }
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

static bool table_exists(sqlite3 *db, const char *table_name) {
    sqlite3_stmt *stmt;
    bool exists = false;
    const char *sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) exists = true;
        sqlite3_finalize(stmt);
    }
    return exists;
}

static void create_initial_db_backups_once(void) {
    struct stat st;
    const char *app_bak = "/system_data/priv/mms/app.db.tabshifter.bak";
    if (stat(app_bak, &st) != 0) {
        sqlite3 *src = NULL, *dst = NULL;
        if (sqlite3_open(APP_DB_PATH, &src) == SQLITE_OK && sqlite3_open(app_bak, &dst) == SQLITE_OK) {
            sqlite3_backup *b = sqlite3_backup_init(dst, "main", src, "main");
            if (b) { sqlite3_backup_step(b, -1); sqlite3_backup_finish(b); }
        }
        if (dst) sqlite3_close(dst);
        if (src) sqlite3_close(src);
    }

    const char *info_bak = "/system_data/priv/mms/appinfo.db.tabshifter.bak";
    if (stat(info_bak, &st) != 0) {
        sqlite3 *src = NULL, *dst = NULL;
        if (sqlite3_open(APPINFO_DB_PATH, &src) == SQLITE_OK && sqlite3_open(info_bak, &dst) == SQLITE_OK) {
            sqlite3_backup *b = sqlite3_backup_init(dst, "main", src, "main");
            if (b) { sqlite3_backup_step(b, -1); sqlite3_backup_finish(b); }
        }
        if (dst) sqlite3_close(dst);
        if (src) sqlite3_close(src);
    }
}

/* ========================================================================= */
/*              TOKEN-SAFE UNIVERSAL DUAL-SCHEMA JSON PATCHER                */
/* ========================================================================= */

static char *patch_json_universal(const char *json_in, const char *title_id,
                                  int target_loc, const char *custom_deeplink,
                                  const char *new_title) {
    if (!json_in) return NULL;

    size_t in_len = strlen(json_in);
    size_t max_out = in_len + 4096;
    char *json_out = calloc(1, max_out);
    if (!json_out) return NULL;

    char hub_uri[128];
    if (strcmp(title_id, "NPXS40063") == 0) {
        strncpy(hub_uri, "pshome:explorehub", sizeof(hub_uri) - 1);
    } else if (target_loc == DISP_LOCATION_MEDIA) {
        snprintf(hub_uri, sizeof(hub_uri), "psmediahub:main?titleId=%s", title_id);
    } else {
        snprintf(hub_uri, sizeof(hub_uri), "pshome:gamehub?titleId=%s", title_id);
    }

    /* CONCEPT BITMASK MAPPING (1162 for Games, 2236 for Media) */
    int concept_loc = (target_loc == DISP_LOCATION_MEDIA) ? CONCEPT_LOC_MEDIA :
                      ((target_loc == DISP_LOCATION_HIDDEN) ? CONCEPT_LOC_HIDDEN : CONCEPT_LOC_GAMES);

    /* --- SCHEMA A: field_list Array Format --- */
    if (strstr(json_in, "\"field_list\"") != NULL) {
        const char *p = json_in;
        char *dst = json_out;
        const char *fl_start = strstr(json_in, "[");
        if (!fl_start) { strcpy(json_out, json_in); return json_out; }

        size_t prefix_len = (size_t)(fl_start - json_in) + 1;
        memcpy(dst, json_in, prefix_len);
        dst += prefix_len;
        p = fl_start + 1;

        while (*p) {
            if (*p == '{') {
                const char *obj_start = p;
                const char *scan = p + 1;
                bool in_quotes = false;
                while (*scan) {
                    if (*scan == '\"' && *(scan - 1) != '\\') in_quotes = !in_quotes;
                    else if (*scan == '}' && !in_quotes) break;
                    scan++;
                }

                if (*scan == '}') {
                    size_t obj_len = (size_t)(scan - obj_start) + 1;
                    char obj_buf[1024];
                    if (obj_len < sizeof(obj_buf)) {
                        memcpy(obj_buf, obj_start, obj_len);
                        obj_buf[obj_len] = '\0';

                        if (target_loc > 0 && strstr(obj_buf, "\"key\":\"display_location\"") != NULL) {
                            /* Concepts use 1162 / 2236 in ConceptInfoJson */
                            int loc_val = (strstr(json_in, "concept") != NULL || strstr(json_in, "primary_title_id") != NULL) ? concept_loc : target_loc;
                            dst += sprintf(dst, "{\"data\":%d,\"key\":\"display_location\",\"size\":8,\"type\":0}", loc_val);
                            p = scan + 1;
                            continue;
                        }
                        if (target_loc > 0 && strstr(obj_buf, "\"key\":\"hubapp_uri\"") != NULL && strncmp(title_id, "NPXS", 4) != 0) {
                            dst += sprintf(dst, "{\"data\":\"%s\",\"key\":\"hubapp_uri\",\"size\":%zu,\"type\":2}", hub_uri, strlen(hub_uri));
                            p = scan + 1;
                            continue;
                        }
                        if (custom_deeplink && strstr(obj_buf, "\"key\":\"deeplink_uri\"") != NULL) {
                            dst += sprintf(dst, "{\"data\":\"%s\",\"key\":\"deeplink_uri\",\"size\":%zu,\"type\":2}", custom_deeplink, strlen(custom_deeplink));
                            p = scan + 1;
                            continue;
                        }
                        if (new_title && new_title[0] != '\0' && strstr(obj_buf, "\"key\":\"concept_name\"") != NULL) {
                            dst += sprintf(dst, "{\"data\":\"%s\",\"key\":\"concept_name\",\"size\":%zu,\"type\":2}", new_title, strlen(new_title));
                            p = scan + 1;
                            continue;
                        }
                    }
                    memcpy(dst, obj_start, obj_len);
                    dst += obj_len;
                    p = scan + 1;
                    continue;
                }
            }
            *dst++ = *p++;
        }
        *dst = '\0';
        return json_out;
    }

    /* --- SCHEMA B: Flat Key-Value JSON --- */
    const char *src = json_in;
    char *dst = json_out;

    while (*src) {
        if (target_loc > 0 && strncmp(src, "\"DISPLAYLOCATION\":", 18) == 0) {
            dst += sprintf(dst, "\"DISPLAYLOCATION\":%d", target_loc);
            src += 18;
            while (*src == '-' || (*src >= '0' && *src <= '9')) src++;
            continue;
        }
        if (target_loc > 0 && strncmp(title_id, "NPXS", 4) != 0 && strncmp(src, "\"HUBAPP_URI\":\"", 14) == 0) {
            dst += sprintf(dst, "\"HUBAPP_URI\":\"%s\"", hub_uri);
            src += 14;
            while (*src && !(*src == '\"' && *(src - 1) != '\\')) src++;
            if (*src == '\"') src++;
            continue;
        }
        if (custom_deeplink && strncmp(src, "\"DEEPLINK_URI\":\"", 16) == 0) {
            dst += sprintf(dst, "\"DEEPLINK_URI\":\"%s\"", custom_deeplink);
            src += 16;
            while (*src && !(*src == '\"' && *(src - 1) != '\\')) src++;
            if (*src == '\"') src++;
            continue;
        }
        if (new_title && new_title[0] != '\0' && strncmp(src, "\"TITLE\":\"", 9) == 0) {
            dst += sprintf(dst, "\"TITLE\":\"%s\"", new_title);
            src += 9;
            while (*src && !(*src == '\"' && *(src - 1) != '\\')) src++;
            if (*src == '\"') src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    return json_out;
}

/* ========================================================================= */
/*          TBSH00001 DASHBOARD TILE & "TS" ICON INSTALLER (ON MEDIA)        */
/* ========================================================================= */

static void ensure_tabshifter_tile_installed(sqlite3 *app_db, char uids[MAX_USER_PROFILES][32], int uid_count) {
    mkdir("/user/app/TBSH00001", 0777);
    mkdir("/user/app/TBSH00001/sce_sys", 0777);
    mkdir("/user/appmeta/TBSH00001", 0777);

    const char *icon_app  = "/user/app/TBSH00001/sce_sys/icon0.png";
    const char *icon_meta = "/user/appmeta/TBSH00001/icon0.png";
    generate_ts_icon_png(icon_app);
    generate_ts_icon_png(icon_meta);

    long ts = (long)time(NULL);
    char icon_uri[256];
    snprintf(icon_uri, sizeof(icon_uri), "/user/app/TBSH00001/sce_sys/icon0.png?ts=%ld", ts);
    char deeplink[128];
    snprintf(deeplink, sizeof(deeplink), "http://127.0.0.1:%d/", HTTP_PORT);
    const char *hub_uri = "psmediahub:main?titleId=TBSH00001";

    sqlite3_exec(app_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);

    /* 1. Clone into tbl_contentinfo (Matches v1.2.1) */
    sqlite3_exec(app_db,
                 "DROP TABLE IF EXISTS t_c;"
                 "CREATE TEMP TABLE t_c AS SELECT * FROM tbl_contentinfo "
                 "ORDER BY (titleId='PLDM00001') DESC, (dispLocation=188) DESC LIMIT 1;"
                 "UPDATE t_c SET titleId='TBSH00001', titleName='TabShifter', "
                 "metaDataPath='/user/app/TBSH00001/sce_sys', dispLocation=188, uninstallable=1, "
                 "categoryType=65536, localConceptId='cid:local:TBSH00001', "
                 "pprDeeplinkUri='http://127.0.0.1:9095/', pprHubAppUri='psmediahub:main?titleId=TBSH00001', "
                 "appType=0, is3rdPartyVideoApp=1;"
                 "DELETE FROM tbl_contentinfo WHERE titleId='TBSH00001';"
                 "INSERT INTO tbl_contentinfo SELECT * FROM t_c;"
                 "DROP TABLE t_c;", NULL, NULL, NULL);

    /* Clean dummy concept rows (Prevents corrupting real game concepts) */
    sqlite3_exec(app_db, "DELETE FROM tbl_conceptmetadata WHERE localConceptId='cid:local:TBSH00001';", NULL, NULL, NULL);

    /* 2. Register in User Presentation Tables */
    for (int i = 0; i < uid_count; i++) {
        char sql[1024];
        snprintf(sql, sizeof(sql),
                 "DROP TABLE IF EXISTS t_i;"
                 "CREATE TEMP TABLE t_i AS SELECT * FROM tbl_iconinfo_%s "
                 "ORDER BY (titleId='PLDM00001') DESC, (dispLocation=188) DESC LIMIT 1;"
                 "UPDATE t_i SET titleId='TBSH00001', titleName='TabShifter', localConceptId='cid:local:TBSH00001', "
                 "pinnedStatus=1, dispLocation=188, visible=1, deeplinkUri='http://127.0.0.1:9095/', "
                 "hubAppUri='psmediahub:main?titleId=TBSH00001', emsDeletedFlag=0;"
                 "DELETE FROM tbl_iconinfo_%s WHERE titleId='TBSH00001';"
                 "INSERT INTO tbl_iconinfo_%s SELECT * FROM t_i;"
                 "DROP TABLE t_i;", uids[i], uids[i], uids[i]);
        sqlite3_exec(app_db, sql, NULL, NULL, NULL);

        snprintf(sql, sizeof(sql),
                 "DROP TABLE IF EXISTS t_ci;"
                 "CREATE TEMP TABLE t_ci AS SELECT * FROM tbl_concepticoninfo_%s "
                 "ORDER BY (primaryTitleId='PLDM00001') DESC, (dispLocation=2236) DESC LIMIT 1;"
                 "UPDATE t_ci SET localConceptId='cid:local:TBSH00001', validFlag=1, conceptName='TabShifter', "
                 "primaryTitleId='TBSH00001', primaryTitleName='TabShifter', dispLocation=2236, "
                 "deeplinkUri='http://127.0.0.1:9095/', hubAppUri='psmediahub:main?titleId=TBSH00001', "
                 "metaDataPath='/user/app/TBSH00001/sce_sys', icon0Info='%s', ucmConceptName='TabShifter';"
                 "DELETE FROM tbl_concepticoninfo_%s WHERE primaryTitleId='TBSH00001' OR localConceptId='cid:local:TBSH00001';"
                 "INSERT INTO tbl_concepticoninfo_%s SELECT * FROM t_ci;"
                 "DROP TABLE t_ci;", uids[i], icon_uri, uids[i], uids[i]);
        sqlite3_exec(app_db, sql, NULL, NULL, NULL);
    }

    sqlite3_exec(app_db, "COMMIT;", NULL, NULL, NULL);
}

/* ========================================================================= */
/*               CORE DATABASE OPERATIONS (SHIFT, HIDE, PIN, RENAME)         */
/* ========================================================================= */

static int internal_shift_single(sqlite3 *app_db, sqlite3 *appinfo_db,
                                 char uids[MAX_USER_PROFILES][32], int uid_count,
                                 const char *title_id, int target_loc) {
    if (!title_id || strcmp(title_id, TBSH_TITLE_ID) == 0) return SQLITE_OK;

    sqlite3_stmt *stmt;
    char sql[512];
    char local_cid[128] = {0};
    char *app_json = NULL;
    char *concept_json = NULL;

    if (sqlite3_prepare_v2(app_db, "SELECT localConceptId, AppInfoJson FROM tbl_contentinfo WHERE titleId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *cid = (const char *)sqlite3_column_text(stmt, 0);
            const char *aj  = (const char *)sqlite3_column_text(stmt, 1);
            if (cid) strncpy(local_cid, cid, sizeof(local_cid) - 1);
            if (aj)  app_json = strdup(aj);
        }
        sqlite3_finalize(stmt);
    }

    if (local_cid[0] == '\0') snprintf(local_cid, sizeof(local_cid), "cid:local:%s", title_id);

    if (sqlite3_prepare_v2(app_db, "SELECT ConceptInfoJson FROM tbl_conceptmetadata WHERE localConceptId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, local_cid, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *cj = (const char *)sqlite3_column_text(stmt, 0);
            if (cj) concept_json = strdup(cj);
        }
        sqlite3_finalize(stmt);
    }

    char *patched_app_json     = patch_json_universal(app_json, title_id, target_loc, NULL, NULL);
    char *patched_concept_json = patch_json_universal(concept_json, title_id, target_loc, NULL, NULL);
    free(app_json);
    free(concept_json);

    int is_video = (target_loc == DISP_LOCATION_MEDIA) ? 1 : 0;
    int concept_loc = (target_loc == DISP_LOCATION_MEDIA) ? CONCEPT_LOC_MEDIA :
                      ((target_loc == DISP_LOCATION_HIDDEN) ? CONCEPT_LOC_HIDDEN : CONCEPT_LOC_GAMES);

    char hub_uri[128];
    if (strcmp(title_id, "NPXS40063") == 0) {
        strncpy(hub_uri, "pshome:explorehub", sizeof(hub_uri) - 1);
    } else if (target_loc == DISP_LOCATION_MEDIA) {
        snprintf(hub_uri, sizeof(hub_uri), "psmediahub:main?titleId=%s", title_id);
    } else {
        snprintf(hub_uri, sizeof(hub_uri), "pshome:gamehub?titleId=%s", title_id);
    }

    /* 1. Update tbl_contentinfo (dispLocation 138/188) */
    const char *up_content =
        "UPDATE tbl_contentinfo SET dispLocation = ?, is3rdPartyVideoApp = ?, "
        "pprHubAppUri = CASE WHEN titleId LIKE 'NPXS%' AND titleId != 'NPXS40063' THEN pprHubAppUri ELSE ? END, "
        "AppInfoJson = coalesce(?, AppInfoJson) WHERE titleId = ?;";
    if (sqlite3_prepare_v2(app_db, up_content, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, target_loc);
        sqlite3_bind_int(stmt, 2, is_video);
        sqlite3_bind_text(stmt, 3, hub_uri, -1, SQLITE_STATIC);
        if (patched_app_json) sqlite3_bind_text(stmt, 4, patched_app_json, -1, SQLITE_STATIC);
        else sqlite3_bind_null(stmt, 4);
        sqlite3_bind_text(stmt, 5, title_id, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* 2. Update tbl_conceptmetadata with Concept Bitmask (1162/2236) */
    const char *up_concept =
        "UPDATE tbl_conceptmetadata SET dispLocation = ?, "
        "pprHubAppUri = CASE WHEN localConceptId LIKE 'cid:local:NPXS%' AND localConceptId != 'cid:local:NPXS40063' THEN pprHubAppUri ELSE ? END, "
        "ConceptInfoJson = coalesce(?, ConceptInfoJson) WHERE localConceptId = ?;";
    if (sqlite3_prepare_v2(app_db, up_concept, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, concept_loc);
        sqlite3_bind_text(stmt, 2, hub_uri, -1, SQLITE_STATIC);
        if (patched_concept_json) sqlite3_bind_text(stmt, 3, patched_concept_json, -1, SQLITE_STATIC);
        else sqlite3_bind_null(stmt, 3);
        sqlite3_bind_text(stmt, 4, local_cid, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    free(patched_app_json);
    free(patched_concept_json);

    /* 3. Update User Presentation Tables */
    for (int i = 0; i < uid_count; i++) {
        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_iconinfo_%s SET dispLocation = ?, "
                 "hubAppUri = CASE WHEN titleId LIKE 'NPXS%%' AND titleId != 'NPXS40063' THEN hubAppUri ELSE ? END "
                 "WHERE titleId = ?;", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, target_loc);
            sqlite3_bind_text(stmt, 2, hub_uri, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        /* tbl_concepticoninfo must use concept_loc (1162 / 2236) */
        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_concepticoninfo_%s SET dispLocation = ?, validFlag = 1, "
                 "hubAppUri = CASE WHEN primaryTitleId LIKE 'NPXS%%' AND primaryTitleId != 'NPXS40063' THEN hubAppUri ELSE ? END "
                 "WHERE localConceptId = ? OR primaryTitleId = ?;", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, concept_loc);
            sqlite3_bind_text(stmt, 2, hub_uri, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, local_cid, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    /* 4. Update appinfo.db */
    if (appinfo_db) {
        if (sqlite3_prepare_v2(appinfo_db, "UPDATE tbl_appinfo SET val = ? WHERE titleId = ? AND key = 'DISPLAYLOCATION';", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, target_loc);
            sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        if (sqlite3_prepare_v2(appinfo_db, "UPDATE tbl_conceptinfo SET val = ? WHERE local_concept_id = ? AND key = 'display_location';", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, concept_loc);
            sqlite3_bind_text(stmt, 2, local_cid, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return SQLITE_OK;
}

static int internal_set_visibility(sqlite3 *app_db, sqlite3 *appinfo_db,
                                   char uids[MAX_USER_PROFILES][32], int uid_count,
                                   const char *title_id, bool hide) {
    sqlite3_stmt *stmt;
    char sql[512];

    int restore_loc = DISP_LOCATION_GAMES;
    if (strcmp(title_id, "NPXS40053") == 0 || strcmp(title_id, "NPXS40054") == 0 ||
        strcmp(title_id, "NPXS40056") == 0 || strcmp(title_id, "NPXS40139") == 0) {
        restore_loc = DISP_LOCATION_MEDIA;
    } else if (strcmp(title_id, "NPXS40063") == 0) {
        restore_loc = 147;
    }
    int target_loc = hide ? DISP_LOCATION_HIDDEN : restore_loc;

    internal_shift_single(app_db, appinfo_db, uids, uid_count, title_id, target_loc);

    for (int i = 0; i < uid_count; i++) {
        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_iconinfo_%s SET visible = ?, dispLocation = ?, "
                 "emsDeletedFlag = ?, emsPushedFlag = ? WHERE titleId = ?;", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, hide ? 0 : 1);
            sqlite3_bind_int(stmt, 2, target_loc);
            sqlite3_bind_int(stmt, 3, hide ? 1 : 0);
            sqlite3_bind_int(stmt, 4, hide ? 0 : 1);
            sqlite3_bind_text(stmt, 5, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO tbl_info_%s (key, value) VALUES (?, ?);", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            char flag_key[128];
            snprintf(flag_key, sizeof(flag_key), "nsx_content_flag_%s", title_id);
            const char *flag_val = hide ? "{\"deleted\":true,\"pushed\":false}" : "{\"deleted\":false,\"pushed\":true}";
            sqlite3_bind_text(stmt, 1, flag_key, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, flag_val, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        char grp_table[64];
        snprintf(grp_table, sizeof(grp_table), "tbl_group_%s", uids[i]);
        if (hide && table_exists(app_db, grp_table)) {
            snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE itemId = ?;", grp_table);
            if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }
    return SQLITE_OK;
}

/* Safe Pinning: Only updates pinnedStatus in tbl_iconinfo (Matches v1.2.1) */
static int internal_set_pin(sqlite3 *app_db, char uids[MAX_USER_PROFILES][32], int uid_count,
                            const char *title_id, bool pin) {
    sqlite3_stmt *stmt;
    char sql[512];

    for (int i = 0; i < uid_count; i++) {
        snprintf(sql, sizeof(sql), "UPDATE tbl_iconinfo_%s SET pinnedStatus = ? WHERE titleId = ?;", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, pin ? 1 : 2);
            sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    return SQLITE_OK;
}

/* ========================================================================= */
/*              SYSTEM-WIDE PADLOCK REPAIR & BACKUP RESTORE                  */
/* ========================================================================= */

static int repair_all_padlocks(sqlite3 *app_db, sqlite3 *appinfo_db, char uids[MAX_USER_PROFILES][32], int uid_count) {
    sqlite3_exec(app_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);

    /* 1. Reset all Games concepts back to 1162 */
    sqlite3_exec(app_db,
                 "UPDATE tbl_conceptmetadata SET dispLocation = 1162 "
                 "WHERE localConceptId IN (SELECT localConceptId FROM tbl_contentinfo WHERE dispLocation = 138);",
                 NULL, NULL, NULL);

    /* 2. Reset all Media concepts back to 2236 */
    sqlite3_exec(app_db,
                 "UPDATE tbl_conceptmetadata SET dispLocation = 2236 "
                 "WHERE localConceptId IN (SELECT localConceptId FROM tbl_contentinfo WHERE dispLocation = 188);",
                 NULL, NULL, NULL);

    /* 3. Delete any corrupted TBSH concept entry */
    sqlite3_exec(app_db, "DELETE FROM tbl_conceptmetadata WHERE localConceptId = 'cid:local:TBSH00001';", NULL, NULL, NULL);

    /* 4. Fix User Presentation Concept Tables */
    for (int i = 0; i < uid_count; i++) {
        char sql[512];
        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_concepticoninfo_%s SET dispLocation = 1162, validFlag = 1 "
                 "WHERE primaryTitleId IN (SELECT titleId FROM tbl_contentinfo WHERE dispLocation = 138);", uids[i]);
        sqlite3_exec(app_db, sql, NULL, NULL, NULL);

        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_concepticoninfo_%s SET dispLocation = 2236, validFlag = 1 "
                 "WHERE primaryTitleId IN (SELECT titleId FROM tbl_contentinfo WHERE dispLocation = 188);", uids[i]);
        sqlite3_exec(app_db, sql, NULL, NULL, NULL);

        /* Clean artificial MyHome entries that cause license verification errors */
        char grp_table[64];
        snprintf(grp_table, sizeof(grp_table), "tbl_group_%s", uids[i]);
        if (table_exists(app_db, grp_table)) {
            snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE groupName = 'MyHome';", grp_table);
            sqlite3_exec(app_db, sql, NULL, NULL, NULL);
        }
    }

    if (appinfo_db) {
        sqlite3_exec(appinfo_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
        sqlite3_exec(appinfo_db,
                     "UPDATE tbl_conceptinfo SET val = 1162 WHERE key = 'display_location' "
                     "AND local_concept_id IN (SELECT local_concept_id FROM tbl_conceptinfo WHERE key = 'display_location' AND val = 138);",
                     NULL, NULL, NULL);
        sqlite3_exec(appinfo_db, "COMMIT;", NULL, NULL, NULL);
    }

    sqlite3_exec(app_db, "COMMIT;", NULL, NULL, NULL);
    return SQLITE_OK;
}

static int restore_clean_db_backup(void) {
    const char *app_bak = "/system_data/priv/mms/app.db.tabshifter.bak";
    struct stat st;
    if (stat(app_bak, &st) != 0) app_bak = "/system_data/priv/mms/app.db.bak";

    const char *info_bak = "/system_data/priv/mms/appinfo.db.tabshifter.bak";
    if (stat(info_bak, &st) != 0) info_bak = "/system_data/priv/mms/appinfo.db.bak";

    if (stat(app_bak, &st) != 0) return -1;

    /* Remove active WAL and SHM journal files */
    unlink("/system_data/priv/mms/app.db-wal");
    unlink("/system_data/priv/mms/app.db-shm");
    unlink("/system_data/priv/mms/appinfo.db-wal");
    unlink("/system_data/priv/mms/appinfo.db-shm");

    /* Copy pristine backup files directly */
    FILE *src = fopen(app_bak, "rb");
    FILE *dst = fopen(APP_DB_PATH, "wb");
    if (src && dst) {
        char buf[65536]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
        fflush(dst); fsync(fileno(dst));
    }
    if (src) fclose(src);
    if (dst) fclose(dst);

    src = fopen(info_bak, "rb");
    dst = fopen(APPINFO_DB_PATH, "wb");
    if (src && dst) {
        char buf[65536]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
        fflush(dst); fsync(fileno(dst));
    }
    if (src) fclose(src);
    if (dst) fclose(dst);

    return 0;
}

/* ========================================================================= */
/*          EMBEDDED WEB UI (BATCH QUEUE, NO SCROLL-JUMP, UPDATER)           */
/* ========================================================================= */

static const char *WEB_UI_HTML =
"<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
"<title>PS5-TabShifter v1.3.0</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0b0e14;color:#e6edf3;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;padding-bottom:90px}"
"#update-banner{display:none;background:linear-gradient(90deg,#11294d,#1a3d6e);border-bottom:1px solid #2e62a6;padding:12px 20px}"
".ub-row{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px;max-width:1100px;margin:0 auto}"
".ub-actions{display:flex;gap:8px}"
"#changelog-drawer{display:none;max-width:1100px;margin:10px auto 0;background:#081221;border:1px solid #234b82;border-radius:6px;padding:12px;font-size:13px;line-height:1.5;max-height:220px;overflow-y:auto;white-space:pre-wrap;color:#b8d0eb}"
".sticky-top{position:sticky;top:0;z-index:90;background:rgba(11,14,20,0.95);backdrop-filter:blur(8px);border-bottom:1px solid #21262d;padding:14px 20px}"
".top-inner{max-width:1100px;margin:0 auto;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:12px}"
"h1{font-size:21px;font-weight:700;color:#fff;display:flex;align-items:center;gap:10px}"
".badge{font-size:12px;background:#1f6feb;color:#fff;padding:3px 8px;border-radius:12px}"
".top-controls{display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
".btn{background:#21262d;color:#c9d1d9;border:1px solid #30363d;padding:8px 14px;border-radius:6px;cursor:pointer;font-size:13px;font-weight:600;transition:0.15s}"
".btn:hover{background:#30363d;color:#fff}"
".btn-primary{background:#0070d1;color:#fff;border-color:#0070d1}"
".btn-apply{background:#238636;color:#fff;border-color:#2ea043;font-size:14px;padding:9px 18px}"
".btn-warn{background:#b45309;color:#fff;border-color:#d97706}"
".btn-danger{background:#8b1e24;color:#fff;border-color:#a8242b}"
".container{max-width:1100px;margin:0 auto;padding:20px}"
".toolbar{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:16px;align-items:center}"
"#search-input{flex:1;min-width:220px;background:#161b22;border:1px solid #30363d;color:#fff;padding:9px 14px;border-radius:6px;font-size:14px}"
".pills{display:flex;gap:6px;flex-wrap:wrap}"
".pill{background:#161b22;border:1px solid #30363d;color:#8b949e;padding:6px 12px;border-radius:16px;cursor:pointer;font-size:12px;font-weight:600}"
".pill.active{background:#1f6feb;color:#fff;border-color:#1f6feb}"
".app-list{display:flex;flex-direction:column;gap:8px}"
".app-card{display:flex;align-items:center;justify-content:space-between;background:#161b22;border:1px solid #21262d;border-radius:8px;padding:12px 16px;gap:12px}"
".app-card.queued{border-color:#d29922;background:#1c1a17}"
".app-left{display:flex;align-items:center;gap:12px;min-width:0;flex:1}"
".app-info{min-width:0;flex:1}"
".app-title-row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}"
".app-title{font-weight:600;font-size:15px;color:#f0f6fc;word-break:break-word}"
".app-tid{font-family:monospace;font-size:12px;color:#8b949e}"
".loc-tag{font-size:11px;padding:2px 7px;border-radius:4px;font-weight:700}"
".loc-138{background:#133929;color:#3fb950}"
".loc-188{background:#3b234a;color:#d2a8ff}"
".loc-1{background:#3d1c20;color:#f85149}"
".pin-tag{font-size:11px;background:#3d2e0f;color:#e3b341;padding:2px 7px;border-radius:4px;font-weight:700}"
".q-tag{font-size:11px;background:#4b3508;color:#f2cc60;padding:2px 7px;border-radius:4px;font-weight:700}"
".app-actions{display:flex;gap:6px;flex-wrap:wrap}"
"#toast{position:fixed;bottom:25px;right:20px;background:#1f6feb;color:#fff;padding:10px 16px;border-radius:6px;font-size:13px;font-weight:600;display:none;z-index:200;box-shadow:0 4px 12px rgba(0,0,0,0.4)}"
"</style></head><body>"
"<div id='update-banner'>"
"  <div class='ub-row'>"
"    <div><strong>New Update Available:</strong> <span id='ub-ver'></span></div>"
"    <div class='ub-actions'>"
"      <button type='button' class='btn' onclick='toggleChangelog()'>What&#39;s New &#9662;</button>"
"      <button type='button' class='btn btn-primary' id='ub-install-btn' onclick='runSelfUpdate()'>Update Now</button>"
"    </div>"
"  </div>"
"  <div id='changelog-drawer'></div>"
"</div>"
"<div class='sticky-top'>"
"  <div class='top-inner'>"
"    <h1>PS5-TabShifter <span class='badge'>v1.3.0</span></h1>"
"    <div class='top-controls'>"
"      <button type='button' class='btn btn-apply' id='apply-btn' onclick='applyAndReload()'>Apply &amp; Reload PS5 UI (<span id='q-count'>0</span>)</button>"
"      <button type='button' class='btn btn-warn' onclick='fixPadlocks()'>Fix Padlocks</button>"
"      <button type='button' class='btn btn-danger' onclick='restoreOriginalDb()'>Restore DB Backup</button>"
"    </div>"
"  </div>"
"</div>"
"<div class='container'>"
"  <div class='toolbar'>"
"    <input type='text' id='search-input' placeholder='Search games, apps, or Title ID...' oninput='renderList()'>"
"    <div class='pills'>"
"      <button type='button' class='pill active' onclick='setFilter(\"all\",this)'>All</button>"
"      <button type='button' class='pill' onclick='setFilter(\"games\",this)'>Games Tab</button>"
"      <button type='button' class='pill' onclick='setFilter(\"media\",this)'>Media Tab</button>"
"      <button type='button' class='pill' onclick='setFilter(\"hidden\",this)'>Hidden / System</button>"
"      <button type='button' class='pill' onclick='setFilter(\"ps5\",this)'>PS5</button>"
"      <button type='button' class='pill' onclick='setFilter(\"ps4\",this)'>PS4</button>"
"      <button type='button' class='pill' onclick='setFilter(\"hb\",this)'>Homebrew</button>"
"    </div>"
"  </div>"
"  <div class='app-list' id='app-list'></div>"
"</div>"
"<div id='toast'></div>"
"<script>"
"let APPS=[];let FILTER='all';let QUEUE=[];let LATEST_ELF_URL='';"
"function showToast(m){const t=document.getElementById('toast');t.textContent=m;t.style.display='block';setTimeout(()=>t.style.display='none',2600);}"
"async function loadApps(){const res=await fetch('/api/apps');APPS=await res.json();renderList();}"
"function isHomebrew(tid){return /^(PLDM|PMNT|FMGR|WKAL|FAKE|ITEM|LAPY|PPSA99|TBSH)/.test(tid);}"
"function setFilter(f,el){FILTER=f;document.querySelectorAll('.pill').forEach(p=>p.classList.remove('active'));el.classList.add('active');renderList();}"
"function locLabel(loc){if(loc===138)return '<span class=\"loc-tag loc-138\">GAMES</span>';if(loc===188)return '<span class=\"loc-tag loc-188\">MEDIA</span>';return '<span class=\"loc-tag loc-1\">HIDDEN ('+loc+')</span>';}"
"function renderList(){"
"  const scrollY=window.scrollY;"
"  const q=document.getElementById('search-input').value.toLowerCase();"
"  document.getElementById('q-count').textContent=QUEUE.length;"
"  const container=document.getElementById('app-list');"
"  container.innerHTML=APPS.filter(a=>{"
"    if(q && !a.title.toLowerCase().includes(q) && !a.tid.toLowerCase().includes(q))return false;"
"    if(FILTER==='games')return a.loc===138 && a.visible!==0;"
"    if(FILTER==='media')return a.loc===188 && a.visible!==0;"
"    if(FILTER==='hidden')return (a.loc!==138 && a.loc!==188) || a.visible===0 || a.tid.startsWith('NPXS');"
"    if(FILTER==='ps5')return a.tid.startsWith('PPSA') && !isHomebrew(a.tid);"
"    if(FILTER==='ps4')return a.tid.startsWith('CUSA');"
"    if(FILTER==='hb')return isHomebrew(a.tid);"
"    return true;"
"  }).map(a=>{"
"    const pinBadge=a.pinned===1?'<span class=\"pin-tag\">PINNED</span>':'';"
"    const qBadge=a.queued?'<span class=\"q-tag\">QUEUED: '+a.queued.toUpperCase()+'</span>':'';"
"    const nextTab=a.loc===188?'games':'media';"
"    const nextTabTxt=a.loc===188?'Move to Games':'Move to Media';"
"    const hideAct=(a.loc===1||a.visible===0)?'unhide':'hide';"
"    const hideTxt=(a.loc===1||a.visible===0)?'Unhide':'Hide';"
"    const pinAct=a.pinned===1?'unpin':'pin';"
"    const pinTxt=a.pinned===1?'Unpin':'Pin';"
"    return `<div class='app-card ${a.queued?\"queued\":\"\"}' id='card-${a.tid}'>"
"      <div class='app-left'>"
"        <div class='app-info'>"
"          <div class='app-title-row'>"
"            <span class='app-title'>${a.title}</span>"
"            ${locLabel(a.loc)} ${pinBadge} ${qBadge}"
"          </div>"
"          <div class='app-tid'>${a.tid}</div>"
"        </div>"
"      </div>"
"      <div class='app-actions'>"
"        <button type='button' class='btn btn-primary' onclick='queueOp(\"${a.tid}\",\"${nextTab}\")'>${nextTabTxt}</button>"
"        <button type='button' class='btn' onclick='queueOp(\"${a.tid}\",\"${pinAct}\")'>${pinTxt}</button>"
"        <button type='button' class='btn' onclick='queueOp(\"${a.tid}\",\"${hideAct}\")'>${hideTxt}</button>"
"      </div>"
"    </div>`;"
"  }).join('');"
"  window.scrollTo(0,scrollY);"
"}"
"function queueOp(tid,action){"
"  QUEUE.push({tid,action});"
"  const item=APPS.find(x=>x.tid===tid);"
"  if(item){"
"    item.queued=action;"
"    if(action==='media'){item.loc=188;item.visible=1;}"
"    else if(action==='games'){item.loc=138;item.visible=1;}"
"    else if(action==='hide'){item.loc=1;item.visible=0;item.pinned=2;}"
"    else if(action==='unhide'){item.loc=138;item.visible=1;}"
"    else if(action==='pin'){item.pinned=1;}"
"    else if(action==='unpin'){item.pinned=2;}"
"  }"
"  renderList();"
"  showToast(`Queued ${tid} -> ${action.toUpperCase()}`);"
"}"
"async function applyAndReload(){"
"  const btn=document.getElementById('apply-btn');"
"  btn.textContent='Applying & Reloading UI...';btn.disabled=true;"
"  await fetch('/api/apply-queue',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ops:QUEUE})});"
"  QUEUE=[];"
"  await loadApps();"
"  btn.innerHTML='Apply &amp; Reload PS5 UI (<span id=\"q-count\">0</span>)';btn.disabled=false;"
"  showToast('Changes applied & UI reloaded!');"
"}"
"async function fixPadlocks(){"
"  if(confirm('Fix all locked games by restoring concept bitmasks (1162 Games / 2236 Media)?')){"
"    const res=await fetch('/api/fix-padlocks',{method:'POST'});"
"    if(res.ok){await loadApps();showToast('Padlocks fixed & PS5 UI reloaded!');}"
"  }"
"}"
"async function restoreOriginalDb(){"
"  if(confirm('Restore original untouched database backup?')){"
"    const res=await fetch('/api/restore-backup',{method:'POST'});"
"    if(res.ok){showToast('Original database restored! PS5 UI reloaded.');setTimeout(()=>location.reload(),2000);}"
"    else{alert('No backup file found on disk!');}"
"  }"
"}"
"function toggleChangelog(){const d=document.getElementById('changelog-drawer');d.style.display=d.style.display==='block'?'none':'block';}"
"async function checkGitHubUpdate(){"
"  try{"
"    const vRes=await fetch('/api/version').then(r=>r.json());"
"    const gh=await fetch('https://api.github.com/repos/xvvulcanvx/PS5-TabShifter/releases/latest').then(r=>r.json());"
"    if(!gh || !gh.tag_name)return;"
"    const latest=gh.tag_name.replace(/^v/,'');"
"    if(latest && latest!==vRes.version){"
"      const asset=(gh.assets||[]).find(a=>a.name.endsWith('.elf'));"
"      if(asset){"
"        LATEST_ELF_URL=asset.browser_download_url;"
"        document.getElementById('ub-ver').textContent=gh.tag_name;"
"        document.getElementById('changelog-drawer').textContent=gh.body||'New release available.';"
"        document.getElementById('update-banner').style.display='block';"
"      }"
"    }"
"  }catch(e){}"
"}"
"async function runSelfUpdate(){"
"  if(!LATEST_ELF_URL)return;"
"  const btn=document.getElementById('ub-install-btn');btn.textContent='Downloading...';btn.disabled=true;"
"  try{"
"    const blob=await fetch(LATEST_ELF_URL).then(r=>r.blob());"
"    btn.textContent='Installing...';"
"    const up=await fetch('/api/update-payload',{method:'POST',body:blob});"
"    if(up.ok){showToast('Update installed! Re-launch payload.');btn.textContent='Updated!';}"
"    else{btn.textContent='Update Failed';btn.disabled=false;}"
"  }catch(e){btn.textContent='Error';btn.disabled=false;}"
"}"
"loadApps();checkGitHubUpdate();"
"</script></body></html>";

/* ========================================================================= */
/*                    MINIMAL JSON & HTTP SERVER HANDLERS                    */
/* ========================================================================= */

static void json_escape_str(const char *in, char *out, size_t max_len) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] != '\0' && j + 2 < max_len; i++) {
        if (in[i] == '\"' || in[i] == '\\') out[j++] = '\\';
        if ((unsigned char)in[i] >= 0x20) out[j++] = in[i];
    }
    out[j] = '\0';
}

static char *build_apps_json(sqlite3 *app_db, const char *primary_uid) {
    size_t cap = 131072;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    size_t len = (size_t)sprintf(buf, "[");
    sqlite3_stmt *stmt;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT c.titleId, coalesce(c.titleName, i.titleName, c.titleId), "
             "coalesce(i.dispLocation, c.dispLocation, 138), coalesce(i.pinnedStatus, 2), coalesce(i.visible, 1) "
             "FROM tbl_contentinfo c LEFT JOIN tbl_iconinfo_%s i ON c.titleId = i.titleId "
             "WHERE c.titleId != 'TBSH00001' "
             "ORDER BY c.titleName COLLATE NOCASE ASC;", primary_uid);

    if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *tid   = (const char *)sqlite3_column_text(stmt, 0);
            const char *tname = (const char *)sqlite3_column_text(stmt, 1);
            int loc           = sqlite3_column_int(stmt, 2);
            int pinned        = sqlite3_column_int(stmt, 3);
            int vis           = sqlite3_column_int(stmt, 4);

            if (!tid) continue;
            char esc_name[256];
            json_escape_str(tname ? tname : tid, esc_name, sizeof(esc_name));

            if (len + 512 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            len += (size_t)snprintf(buf + len, cap - len,
                                    "%s{\"tid\":\"%s\",\"title\":\"%s\",\"loc\":%d,\"pinned\":%d,\"visible\":%d}",
                                    first ? "" : ",", tid, esc_name, loc, pinned, vis);
            first = false;
        }
        sqlite3_finalize(stmt);
    }
    snprintf(buf + len, cap - len, "]");
    return buf;
}

static void handle_apply_queue_json(sqlite3 *app_db, sqlite3 *appinfo_db,
                                    char uids[MAX_USER_PROFILES][32], int uid_count,
                                    const char *body) {
    if (!body) return;

    sqlite3_exec(app_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
    if (appinfo_db) sqlite3_exec(appinfo_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);

    const char *p = body;
    while ((p = strstr(p, "\"tid\":\"")) != NULL) {
        p += 7;
        char tid[32] = {0};
        size_t i = 0;
        while (p[i] && p[i] != '\"' && i < sizeof(tid) - 1) { tid[i] = p[i]; i++; }
        p += i;

        const char *act_p = strstr(p, "\"action\":\"");
        if (!act_p) break;
        act_p += 10;
        char action[32] = {0};
        size_t j = 0;
        while (act_p[j] && act_p[j] != '\"' && j < sizeof(action) - 1) { action[j] = act_p[j]; j++; }
        p = act_p + j;

        if (tid[0] != '\0' && action[0] != '\0') {
            if (strcmp(action, "media") == 0)       internal_shift_single(app_db, appinfo_db, uids, uid_count, tid, DISP_LOCATION_MEDIA);
            else if (strcmp(action, "games") == 0)  internal_shift_single(app_db, appinfo_db, uids, uid_count, tid, DISP_LOCATION_GAMES);
            else if (strcmp(action, "hide") == 0)   internal_set_visibility(app_db, appinfo_db, uids, uid_count, tid, true);
            else if (strcmp(action, "unhide") == 0) internal_set_visibility(app_db, appinfo_db, uids, uid_count, tid, false);
            else if (strcmp(action, "pin") == 0)    internal_set_pin(app_db, uids, uid_count, tid, true);
            else if (strcmp(action, "unpin") == 0)  internal_set_pin(app_db, uids, uid_count, tid, false);
        }
    }

    if (appinfo_db) sqlite3_exec(appinfo_db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_exec(app_db, "COMMIT;", NULL, NULL, NULL);

    reload_sceshellui();
}

static void send_http_response(int client_fd, int status, const char *content_type, const void *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d OK\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n\r\n",
                        status, content_type, body_len);
    send(client_fd, header, (size_t)hlen, 0);
    if (body && body_len > 0) send(client_fd, body, body_len, 0);
}

static bool handle_http_client(int client_fd, sqlite3 *app_db, sqlite3 *appinfo_db,
                               char uids[MAX_USER_PROFILES][32], int uid_count) {
    size_t cap = 65536;
    uint8_t *req = malloc(cap);
    if (!req) { close(client_fd); return true; }

    ssize_t received = recv(client_fd, req, cap - 1, 0);
    if (received <= 0) { free(req); close(client_fd); return true; }
    req[received] = '\0';

    size_t content_len = 0;
    const char *cl_hdr = strstr((char *)req, "Content-Length:");
    if (!cl_hdr) cl_hdr = strstr((char *)req, "content-length:");
    if (cl_hdr) content_len = (size_t)strtoul(cl_hdr + 15, NULL, 10);

    uint8_t *body_ptr = (uint8_t *)strstr((char *)req, "\r\n\r\n");
    if (body_ptr) {
        body_ptr += 4;
        size_t header_bytes = (size_t)(body_ptr - req);
        size_t total_needed = header_bytes + content_len;
        if (total_needed > cap && total_needed < 32 * 1024 * 1024) {
            req = realloc(req, total_needed + 1);
            body_ptr = req + header_bytes;
        }
        while ((size_t)received < total_needed) {
            ssize_t r = recv(client_fd, req + received, total_needed - (size_t)received, 0);
            if (r <= 0) break;
            received += r;
        }
        req[received] = '\0';
    }

    if (strncmp((char *)req, "POST /api/shutdown", 18) == 0) {
        send_http_response(client_fd, 200, "text/plain", "BYE", 3);
        free(req);
        close(client_fd);
        return false;
    } else if (strncmp((char *)req, "GET / ", 6) == 0) {
        send_http_response(client_fd, 200, "text/html; charset=utf-8", WEB_UI_HTML, strlen(WEB_UI_HTML));
    } else if (strncmp((char *)req, "GET /api/version", 16) == 0) {
        char elf_path[512], vjson[640];
        get_running_elf_path(elf_path, sizeof(elf_path));
        int vlen = snprintf(vjson, sizeof(vjson), "{\"version\":\"%s\",\"elf_path\":\"%s\"}", TABSHIFTER_VERSION, elf_path);
        send_http_response(client_fd, 200, "application/json", vjson, (size_t)vlen);
    } else if (strncmp((char *)req, "GET /api/apps", 13) == 0) {
        char *ajson = build_apps_json(app_db, uids[0]);
        if (ajson) {
            send_http_response(client_fd, 200, "application/json", ajson, strlen(ajson));
            free(ajson);
        }
    } else if (strncmp((char *)req, "POST /api/apply-queue", 21) == 0 && body_ptr) {
        handle_apply_queue_json(app_db, appinfo_db, uids, uid_count, (const char *)body_ptr);
        send_http_response(client_fd, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strncmp((char *)req, "POST /api/fix-padlocks", 22) == 0) {
        repair_all_padlocks(app_db, appinfo_db, uids, uid_count);
        reload_sceshellui();
        send_http_response(client_fd, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strncmp((char *)req, "POST /api/restore-backup", 24) == 0) {
        int rc = restore_clean_db_backup();
        if (rc == 0) {
            reload_sceshellui();
            send_http_response(client_fd, 200, "application/json", "{\"ok\":true}", 11);
        } else {
            send_http_response(client_fd, 500, "application/json", "{\"ok\":false}", 12);
        }
    } else if (strncmp((char *)req, "POST /api/update-payload", 24) == 0 && body_ptr && content_len > 0) {
        int rc = apply_self_update(body_ptr, content_len);
        if (rc == 0) {
            notify_ps5("TabShifter Updated!\nRe-launch payload to apply.");
            send_http_response(client_fd, 200, "application/json", "{\"ok\":true}", 11);
        } else {
            send_http_response(client_fd, 500, "application/json", "{\"ok\":false}", 12);
        }
    } else {
        send_http_response(client_fd, 404, "text/plain", "Not Found", 9);
    }

    free(req);
    close(client_fd);
    return true;
}

/* ========================================================================= */
/*                                MAIN ENTRY                                 */
/* ========================================================================= */

int main(void) {
    kill_existing_tabshifter_instance();
    create_initial_db_backups_once();

    sqlite3 *app_db = NULL;
    sqlite3 *appinfo_db = NULL;
    char uids[MAX_USER_PROFILES][32];

    if (sqlite3_open(APP_DB_PATH, &app_db) != SQLITE_OK) {
        notify_ps5("TabShifter Error: Failed to open app.db");
        return 1;
    }
    sqlite3_busy_timeout(app_db, SQLITE_BUSY_TIMEOUT_MS);
    sqlite3_exec(app_db, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);

    if (sqlite3_open(APPINFO_DB_PATH, &appinfo_db) == SQLITE_OK) {
        sqlite3_busy_timeout(appinfo_db, SQLITE_BUSY_TIMEOUT_MS);
        sqlite3_exec(appinfo_db, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);
    } else {
        appinfo_db = NULL;
    }

    int uid_count = get_all_user_ids(app_db, uids);
    if (uid_count <= 0) {
        notify_ps5("TabShifter Error: No user tables found");
        if (appinfo_db) sqlite3_close(appinfo_db);
        sqlite3_close(app_db);
        return 1;
    }

    ensure_tabshifter_tile_installed(app_db, uids, uid_count);
    reload_sceshellui();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return 1;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server_fd, 8) < 0) {
        notify_ps5("TabShifter: Port %d busy!\nReboot PS5 once to clear.", HTTP_PORT);
        close(server_fd);
        if (appinfo_db) sqlite3_close(appinfo_db);
        sqlite3_close(app_db);
        return 1;
    }

    notify_ps5("PS5-TabShifter v1.3.0 Ready!\nOpen 'TS' on Media Tab or Port %d", HTTP_PORT);

    bool running = true;
    while (running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            running = handle_http_client(client_fd, app_db, appinfo_db, uids, uid_count);
        }
    }

    close(server_fd);
    if (appinfo_db) sqlite3_close(appinfo_db);
    sqlite3_close(app_db);
    return 0;
}

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

#define CONCEPT_LOC_GAMES       1162
#define CONCEPT_LOC_MEDIA       2236
#define CONCEPT_LOC_HIDDEN      1

#define CATEGORY_PS5_GAME       0
#define CATEGORY_PS4_GAME       61440
#define CATEGORY_MEDIA_APP      65536

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

static void write_tbsh_param_json(const char *filepath) {
    const char *json =
        "{\n"
        "  \"titleId\": \"TBSH00001\",\n"
        "  \"applicationCategoryType\": 65536,\n"
        "  \"deeplinkUri\": \"http://127.0.0.1:9095/\",\n"
        "  \"localizedParameters\": {\n"
        "    \"defaultLanguage\": \"en-US\",\n"
        "    \"en-US\": {\n"
        "      \"titleName\": \"TabShifter\"\n"
        "    }\n"
        "  }\n"
        "}\n";
    FILE *fp = fopen(filepath, "wb");
    if (fp) {
        fwrite(json, 1, strlen(json), fp);
        fclose(fp);
        chmod(filepath, 0777);
    }
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

static void copy_file_if_missing(const char *src_path, const char *dst_path) {
    struct stat st;
    if (stat(dst_path, &st) == 0 && st.st_size > 0) return;
    FILE *src = fopen(src_path, "rb");
    if (!src) return;
    FILE *dst = fopen(dst_path, "wb");
    if (dst) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
        fflush(dst);
        fsync(fileno(dst));
        fclose(dst);
    }
    fclose(src);
}

static void create_initial_db_backups_once(void) {
    copy_file_if_missing(APP_DB_PATH, "/system_data/priv/mms/app.db.tabshifter.bak");
    copy_file_if_missing(APPINFO_DB_PATH, "/system_data/priv/mms/appinfo.db.tabshifter.bak");
}

/* ========================================================================= */
/*              V1.2.1 CONCEPT_INFO_JSON REWRITER (EXACT MATCH)              */
/* ========================================================================= */

static char *update_concept_info_json(const char *json_in, int new_disp_loc) {
    if (!json_in) return NULL;
    size_t len = strlen(json_in);
    char *out = malloc(len + 128);
    if (!out) return NULL;

    const char *key_pos = strstr(json_in, "\"key\":\"display_location\"");
    if (!key_pos) {
        strcpy(out, json_in);
        return out;
    }

    /* Walk backwards from "key":"display_location" to find "data": */
    const char *data_pos = key_pos;
    while (data_pos > json_in && strncmp(data_pos, "\"data\":", 7) != 0) {
        data_pos--;
    }

    if (strncmp(data_pos, "\"data\":", 7) == 0) {
        size_t prefix_len = (size_t)(data_pos - json_in) + 7;
        memcpy(out, json_in, prefix_len);
        const char *num_end = data_pos + 7;
        while (*num_end == ' ' || *num_end == '-' || (*num_end >= '0' && *num_end <= '9')) {
            num_end++;
        }
        sprintf(out + prefix_len, "%d%s", new_disp_loc, num_end);
        return out;
    }

    strcpy(out, json_in);
    return out;
}

/* ========================================================================= */
/*     TBSH00001 DASHBOARD TILE & "TS" ICON INSTALLER (EXACT V1.2.1 SQL)     */
/* ========================================================================= */

static bool is_tbsh_tile_already_installed(sqlite3 *app_db) {
    sqlite3_stmt *stmt;
    int loc = 0;
    if (sqlite3_prepare_v2(app_db, "SELECT dispLocation FROM tbl_contentinfo WHERE titleId='TBSH00001';", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            loc = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    struct stat st;
    bool icon_ok = (stat("/user/app/TBSH00001/sce_sys/icon0.png", &st) == 0 && st.st_size > 100);
    return (loc == 188 && icon_ok);
}

static void install_tabshifter_launcher(sqlite3 *app_db, sqlite3 *appinfo_db, char uids[MAX_USER_PROFILES][32], int uid_count) {
    mkdir("/user/app/TBSH00001", 0777);
    mkdir("/user/app/TBSH00001/sce_sys", 0777);
    mkdir("/user/appmeta/TBSH00001", 0777);
    mkdir("/system_data/priv/appmeta/TBSH00001", 0777);

    /* Generate TS Icon & param.json in all 3 PS5 metadata locations (Exact v1.2.1 behavior) */
    generate_ts_icon_png("/user/app/TBSH00001/sce_sys/icon0.png");
    generate_ts_icon_png("/user/appmeta/TBSH00001/icon0.png");
    generate_ts_icon_png("/system_data/priv/appmeta/TBSH00001/icon0.png");

    write_tbsh_param_json("/user/app/TBSH00001/sce_sys/param.json");
    write_tbsh_param_json("/user/appmeta/TBSH00001/param.json");
    write_tbsh_param_json("/system_data/priv/appmeta/TBSH00001/param.json");

    long ts = (long)time(NULL);
    char icon_uri[256];
    snprintf(icon_uri, sizeof(icon_uri), "/user/app/TBSH00001/sce_sys/icon0.png?ts=%ld", ts);

    sqlite3_exec(app_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);

    /* 1. Exact v1.2.1 tbl_contentinfo registration */
    char *sql_c = sqlite3_mprintf(
        "DROP TABLE IF EXISTS t_c;"
        "CREATE TEMP TABLE t_c AS SELECT * FROM tbl_contentinfo ORDER BY (titleId='PLDM00001') DESC, (dispLocation=188) DESC LIMIT 1;"
        "UPDATE t_c SET titleId='TBSH00001', titleName='TabShifter', metaDataPath='/user/app/TBSH00001/sce_sys', "
        "dispLocation=188, uninstallable=1, categoryType=65536, localConceptId='cid:local:TBSH00001', "
        "pprDeeplinkUri='http://127.0.0.1:9095/', pprHubAppUri='psmediahub:main?titleId=TBSH00001', "
        "icon0Info=%Q, appType=0, is3rdPartyVideoApp=1, "
        "AppInfoJson=CASE WHEN json_valid(AppInfoJson) AND instr(AppInfoJson, '\"field_list\"')=0 THEN "
        "json_set(AppInfoJson, '$.TITLE', 'TabShifter', '$.TITLE_01', 'TabShifter', '$.TITLE_ID', 'TBSH00001', "
        "'$.DISPLAYLOCATION', 188, '$.CATEGORY_TYPE', 65536, '$.DEEPLINK_URI', 'http://127.0.0.1:9095/', "
        "'$.HUBAPP_URI', 'psmediahub:main?titleId=TBSH00001', '$._local_concept_id', 'cid:local:TBSH00001', "
        "'$._metadata_path', '/user/app/TBSH00001/sce_sys', '$._org_path', '/user/app/TBSH00001', '$._uninstallable', 1) "
        "ELSE AppInfoJson END;"
        "DELETE FROM tbl_contentinfo WHERE titleId='TBSH00001';"
        "INSERT INTO tbl_contentinfo SELECT * FROM t_c;"
        "DROP TABLE t_c;", icon_uri);
    sqlite3_exec(app_db, sql_c, NULL, NULL, NULL);
    sqlite3_free(sql_c);

    sqlite3_exec(app_db, "DELETE FROM tbl_conceptmetadata WHERE localConceptId='cid:local:TBSH00001';", NULL, NULL, NULL);

    /* 2. Exact v1.2.1 tbl_iconinfo & tbl_concepticoninfo registration */
    for (int i = 0; i < uid_count; i++) {
        char *sql_u = sqlite3_mprintf(
            "DROP TABLE IF EXISTS t_i;"
            "CREATE TEMP TABLE t_i AS SELECT * FROM tbl_iconinfo_%s ORDER BY (titleId='PLDM00001') DESC, (dispLocation=188) DESC LIMIT 1;"
            "UPDATE t_i SET titleId='TBSH00001', titleName='TabShifter', localConceptId='cid:local:TBSH00001', "
            "pinnedStatus=1, dispLocation=188, visible=1, deeplinkUri='http://127.0.0.1:9095/', "
            "hubAppUri='psmediahub:main?titleId=TBSH00001', emsDeletedFlag=0;"
            "DELETE FROM tbl_iconinfo_%s WHERE titleId='TBSH00001';"
            "INSERT INTO tbl_iconinfo_%s SELECT * FROM t_i;"
            "DROP TABLE t_i;"
            "DROP TABLE IF EXISTS t_ci;"
            "CREATE TEMP TABLE t_ci AS SELECT * FROM tbl_concepticoninfo_%s ORDER BY (primaryTitleId='PLDM00001') DESC, (dispLocation=2236) DESC LIMIT 1;"
            "UPDATE t_ci SET localConceptId='cid:local:TBSH00001', validFlag=1, conceptName='TabShifter', "
            "primaryTitleId='TBSH00001', primaryTitleName='TabShifter', dispLocation=2236, "
            "deeplinkUri='http://127.0.0.1:9095/', hubAppUri='psmediahub:main?titleId=TBSH00001', "
            "metaDataPath='/user/app/TBSH00001/sce_sys', icon0Info=%Q, ucmConceptName='TabShifter';"
            "DELETE FROM tbl_concepticoninfo_%s WHERE primaryTitleId='TBSH00001' OR localConceptId='cid:local:TBSH00001';"
            "INSERT INTO tbl_concepticoninfo_%s SELECT * FROM t_ci;"
            "DROP TABLE t_ci;",
            uids[i], uids[i], uids[i], uids[i], icon_uri, uids[i], uids[i]);
        sqlite3_exec(app_db, sql_u, NULL, NULL, NULL);
        sqlite3_free(sql_u);
    }

    sqlite3_exec(app_db, "COMMIT;", NULL, NULL, NULL);

    /* 3. Exact v1.2.1 appinfo.db registration for TBSH00001 */
    if (appinfo_db) {
        sqlite3_exec(appinfo_db,
                     "BEGIN IMMEDIATE;"
                     "DELETE FROM tbl_appinfo WHERE titleId='TBSH00001';"
                     "INSERT INTO tbl_appinfo SELECT 'TBSH00001', metaDataId, key, "
                     "CASE WHEN key IN ('TITLE','TITLE_01') THEN 'TabShifter' "
                     "WHEN key='TITLE_ID' THEN 'TBSH00001' "
                     "WHEN key='DISPLAYLOCATION' THEN 188 "
                     "WHEN key='CATEGORY_TYPE' THEN 65536 "
                     "WHEN key='DEEPLINK_URI' THEN 'http://127.0.0.1:9095/' "
                     "WHEN key='HUBAPP_URI' THEN 'psmediahub:main?titleId=TBSH00001' "
                     "WHEN key='_local_concept_id' THEN 'cid:local:TBSH00001' "
                     "WHEN key='_metadata_path' THEN '/user/app/TBSH00001/sce_sys' "
                     "WHEN key='_org_path' THEN '/user/app/TBSH00001' "
                     "WHEN key='_uninstallable' THEN 1 ELSE val END "
                     "FROM tbl_appinfo WHERE titleId=(SELECT titleId FROM tbl_appinfo WHERE key='TITLE_ID' ORDER BY (titleId='PLDM00001') DESC LIMIT 1);"
                     "COMMIT;", NULL, NULL, NULL);
    }
}

/* ========================================================================= */
/*               CORE DATABASE OPERATIONS (EXACT V1.2.1 + UNIVERSAL)         */
/* ========================================================================= */

static int internal_shift_single(sqlite3 *app_db, sqlite3 *appinfo_db,
                                 char uids[MAX_USER_PROFILES][32], int uid_count,
                                 const char *title_id, int target_loc) {
    if (!title_id || strcmp(title_id, TBSH_TITLE_ID) == 0) return SQLITE_OK;

    sqlite3_stmt *stmt;
    char local_cid[128] = {0};
    char *app_json = NULL;

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

    int cat_type = (target_loc == DISP_LOCATION_MEDIA) ? CATEGORY_MEDIA_APP :
                   ((strncmp(title_id, "CUSA", 4) == 0) ? CATEGORY_PS4_GAME : CATEGORY_PS5_GAME);
    int is_video = (target_loc == DISP_LOCATION_MEDIA) ? 1 : 0;
    int concept_loc = (target_loc == DISP_LOCATION_MEDIA) ? CONCEPT_LOC_MEDIA : CONCEPT_LOC_GAMES;

    char hub_uri[128];
    if (target_loc == DISP_LOCATION_MEDIA) {
        snprintf(hub_uri, sizeof(hub_uri), "psmediahub:main?titleId=%s", title_id);
    } else {
        snprintf(hub_uri, sizeof(hub_uri), "pshome:gamehub?titleId=%s", title_id);
    }

    /* 1. Update tbl_contentinfo: Supports both Flat JSON (json_set) and Legacy field_list JSON */
    if (app_json && strstr(app_json, "\"field_list\"") != NULL) {
        char *patched_fl = update_concept_info_json(app_json, target_loc);
        char *q = sqlite3_mprintf(
            "UPDATE tbl_contentinfo SET dispLocation = %d, categoryType = %d, is3rdPartyVideoApp = %d, "
            "pprHubAppUri = %Q, AppInfoJson = %Q WHERE titleId = %Q;",
            target_loc, cat_type, is_video, hub_uri, patched_fl ? patched_fl : app_json, title_id);
        sqlite3_exec(app_db, q, NULL, NULL, NULL);
        sqlite3_free(q);
        free(patched_fl);
    } else {
        /* Exact v1.2.1 SQL statement using native SQLite json_set() */
        char *q = sqlite3_mprintf(
            "UPDATE tbl_contentinfo SET dispLocation = %d, categoryType = %d, is3rdPartyVideoApp = %d, "
            "pprHubAppUri = %Q, AppInfoJson = json_set(AppInfoJson, '$.DISPLAYLOCATION', %d, '$.CATEGORY_TYPE', %d, '$.HUBAPP_URI', %Q) "
            "WHERE titleId = %Q;",
            target_loc, cat_type, is_video, hub_uri, target_loc, cat_type, hub_uri, title_id);
        sqlite3_exec(app_db, q, NULL, NULL, NULL);
        sqlite3_free(q);
    }
    free(app_json);

    /* 2. Update tbl_conceptmetadata using Concept Bitmask (1162 / 2236) */
    char *cjson = NULL;
    if (sqlite3_prepare_v2(app_db, "SELECT ConceptInfoJson FROM tbl_conceptmetadata WHERE localConceptId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, local_cid, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *cj = (const char *)sqlite3_column_text(stmt, 0);
            if (cj) cjson = strdup(cj);
        }
        sqlite3_finalize(stmt);
    }
    char *patched_cjson = update_concept_info_json(cjson, concept_loc);
    if (patched_cjson) {
        char *q = sqlite3_mprintf("UPDATE tbl_conceptmetadata SET dispLocation = %d, ConceptInfoJson = %Q WHERE localConceptId = %Q;",
                                  concept_loc, patched_cjson, local_cid);
        sqlite3_exec(app_db, q, NULL, NULL, NULL);
        sqlite3_free(q);
        free(patched_cjson);
    }
    free(cjson);

    /* 3. Update User Tables (Exact v1.2.1 SQL with 1162/2236 for tbl_concepticoninfo) */
    for (int i = 0; i < uid_count; i++) {
        char *q = sqlite3_mprintf(
            "UPDATE tbl_iconinfo_%s SET dispLocation = %d, visible = 1, hubAppUri = %Q, emsDeletedFlag = 0 WHERE titleId = %Q;"
            "UPDATE tbl_concepticoninfo_%s SET dispLocation = %d, validFlag = 1, hubAppUri = %Q WHERE primaryTitleId = %Q OR localConceptId = %Q;",
            uids[i], target_loc, hub_uri, title_id,
            uids[i], concept_loc, hub_uri, title_id, local_cid);
        sqlite3_exec(app_db, q, NULL, NULL, NULL);
        sqlite3_free(q);
    }

    /* 4. Update appinfo.db (Exact v1.2.1 SQL) */
    if (appinfo_db) {
        char *qa = sqlite3_mprintf(
            "UPDATE tbl_appinfo SET val = %d WHERE titleId = %Q AND key = 'DISPLAYLOCATION';"
            "UPDATE tbl_appinfo SET val = %d WHERE titleId = %Q AND key = 'CATEGORY_TYPE';"
            "UPDATE tbl_appinfo SET val = %Q WHERE titleId = %Q AND key = 'HUBAPP_URI';"
            "UPDATE tbl_conceptinfo SET val = %d WHERE local_concept_id = %Q AND key = 'display_location';"
            "UPDATE tbl_conceptinfo SET val = %Q WHERE local_concept_id = %Q AND key = 'hubapp_uri';",
            target_loc, title_id, cat_type, title_id, hub_uri, title_id, concept_loc, local_cid, hub_uri, local_cid);
        sqlite3_exec(appinfo_db, qa, NULL, NULL, NULL);
        sqlite3_free(qa);
    }

    return SQLITE_OK;
}

static int internal_set_visibility(sqlite3 *app_db, sqlite3 *appinfo_db,
                                   char uids[MAX_USER_PROFILES][32], int uid_count,
                                   const char *title_id, bool hide) {
    char local_cid[128];
    snprintf(local_cid, sizeof(local_cid), "cid:local:%s", title_id);

    int restore_loc = DISP_LOCATION_GAMES;
    if (strcmp(title_id, "NPXS40053") == 0 || strcmp(title_id, "NPXS40054") == 0 ||
        strcmp(title_id, "NPXS40056") == 0 || strcmp(title_id, "NPXS40139") == 0) {
        restore_loc = DISP_LOCATION_MEDIA;
    } else if (strcmp(title_id, "NPXS40063") == 0) {
        restore_loc = 147;
    }

    int target_loc = hide ? 0 : restore_loc;
    int concept_loc = hide ? 0 : ((restore_loc == DISP_LOCATION_MEDIA) ? CONCEPT_LOC_MEDIA : CONCEPT_LOC_GAMES);
    int vis = hide ? 0 : 1;
    int del_flag = hide ? 1 : 0;

    /* 1. Update tbl_contentinfo */
    char *qc = sqlite3_mprintf(
        "UPDATE tbl_contentinfo SET dispLocation = %d, "
        "AppInfoJson = CASE WHEN json_valid(AppInfoJson) AND instr(AppInfoJson, '\"field_list\"')=0 "
        "THEN json_set(AppInfoJson, '$.DISPLAYLOCATION', %d) ELSE AppInfoJson END WHERE titleId = %Q;",
        target_loc, target_loc, title_id);
    sqlite3_exec(app_db, qc, NULL, NULL, NULL);
    sqlite3_free(qc);

    /* 2. Update tbl_conceptmetadata */
    sqlite3_stmt *stmt;
    char *cjson = NULL;
    if (sqlite3_prepare_v2(app_db, "SELECT ConceptInfoJson FROM tbl_conceptmetadata WHERE localConceptId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, local_cid, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *cj = (const char *)sqlite3_column_text(stmt, 0);
            if (cj) cjson = strdup(cj);
        }
        sqlite3_finalize(stmt);
    }
    char *patched_cjson = update_concept_info_json(cjson, concept_loc);
    if (patched_cjson) {
        char *qm = sqlite3_mprintf("UPDATE tbl_conceptmetadata SET dispLocation = %d, ConceptInfoJson = %Q WHERE localConceptId = %Q;",
                                   concept_loc, patched_cjson, local_cid);
        sqlite3_exec(app_db, qm, NULL, NULL, NULL);
        sqlite3_free(qm);
        free(patched_cjson);
    }
    free(cjson);

    /* 3. Update User Tables + Sleep Mode Persistence (Issues #16 & #17) */
    for (int i = 0; i < uid_count; i++) {
        char *qu = sqlite3_mprintf(
            "UPDATE tbl_iconinfo_%s SET dispLocation = %d, visible = %d, emsDeletedFlag = %d, emsPushedFlag = %d WHERE titleId = %Q;"
            "UPDATE tbl_concepticoninfo_%s SET dispLocation = %d, validFlag = %d WHERE primaryTitleId = %Q OR localConceptId = %Q;"
            "INSERT OR REPLACE INTO tbl_info_%s (key, value) VALUES ('nsx_content_flag_%s', '{\"deleted\":%s,\"pushed\":%s}');",
            uids[i], target_loc, vis, del_flag, hide ? 0 : 1, title_id,
            uids[i], concept_loc, vis, title_id, local_cid,
            uids[i], title_id, hide ? "true" : "false", hide ? "false" : "true");
        sqlite3_exec(app_db, qu, NULL, NULL, NULL);
        sqlite3_free(qu);

        char grp_table[64];
        snprintf(grp_table, sizeof(grp_table), "tbl_group_%s", uids[i]);
        if (hide && table_exists(app_db, grp_table)) {
            char *qg = sqlite3_mprintf("DELETE FROM %s WHERE itemId = %Q;", grp_table, title_id);
            sqlite3_exec(app_db, qg, NULL, NULL, NULL);
            sqlite3_free(qg);
        }
    }

    if (appinfo_db) {
        char *qa = sqlite3_mprintf(
            "UPDATE tbl_appinfo SET val = %d WHERE titleId = %Q AND key = 'DISPLAYLOCATION';"
            "UPDATE tbl_conceptinfo SET val = %d WHERE local_concept_id = %Q AND key = 'display_location';",
            target_loc, title_id, concept_loc, local_cid);
        sqlite3_exec(appinfo_db, qa, NULL, NULL, NULL);
        sqlite3_free(qa);
    }

    return SQLITE_OK;
}

static int internal_set_pin(sqlite3 *app_db, char uids[MAX_USER_PROFILES][32], int uid_count,
                            const char *title_id, bool pin) {
    for (int i = 0; i < uid_count; i++) {
        char *q = sqlite3_mprintf("UPDATE tbl_iconinfo_%s SET pinnedStatus = %d WHERE titleId = %Q;",
                                  uids[i], pin ? 1 : 2, title_id);
        sqlite3_exec(app_db, q, NULL, NULL, NULL);
        sqlite3_free(q);
    }
    return SQLITE_OK;
}

/* ========================================================================= */
/*          COMPLETE PADLOCK REPAIR (RESTORES JSON, CAT_TYPE & BITMASKS)     */
/* ========================================================================= */

static int repair_all_padlocks(sqlite3 *app_db, sqlite3 *appinfo_db, char uids[MAX_USER_PROFILES][32], int uid_count) {
    sqlite3_stmt *stmt;
    char tids[512][16];
    int locs[512];
    int count = 0;

    if (sqlite3_prepare_v2(app_db, "SELECT titleId, dispLocation FROM tbl_contentinfo WHERE titleId NOT LIKE 'NPXS%' AND titleId != 'TBSH00001';", -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && count < 512) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            int l = sqlite3_column_int(stmt, 1);
            if (t) {
                strncpy(tids[count], t, 15);
                tids[count][15] = '\0';
                locs[count] = (l == DISP_LOCATION_MEDIA) ? DISP_LOCATION_MEDIA : DISP_LOCATION_GAMES;
                count++;
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_exec(app_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (appinfo_db) sqlite3_exec(appinfo_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);

    /* Re-apply exact v1.2.1 shift logic to every game to restore categoryType, ConceptInfoJson (1162), and validFlag */
    for (int i = 0; i < count; i++) {
        internal_shift_single(app_db, appinfo_db, uids, uid_count, tids[i], locs[i]);
    }

    if (appinfo_db) sqlite3_exec(appinfo_db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_exec(app_db, "COMMIT;", NULL, NULL, NULL);

    /* Re-install TBSH00001 cleanly so its icon and metadata are 100% intact */
    install_tabshifter_launcher(app_db, appinfo_db, uids, uid_count);

    return SQLITE_OK;
}

static int restore_clean_db_backup(sqlite3 **app_db_ptr, sqlite3 **appinfo_db_ptr, char uids[MAX_USER_PROFILES][32], int uid_count) {
    const char *app_bak = "/system_data/priv/mms/app.db.tabshifter.bak";
    struct stat st;
    if (stat(app_bak, &st) != 0) app_bak = "/system_data/priv/mms/app.db.bak";

    const char *info_bak = "/system_data/priv/mms/appinfo.db.tabshifter.bak";
    if (stat(info_bak, &st) != 0) info_bak = "/system_data/priv/mms/appinfo.db.bak";

    if (stat(app_bak, &st) != 0) return -1;

    /* Close active SQLite handles before replacing files on disk */
    if (*appinfo_db_ptr) { sqlite3_close(*appinfo_db_ptr); *appinfo_db_ptr = NULL; }
    if (*app_db_ptr)     { sqlite3_close(*app_db_ptr);     *app_db_ptr = NULL; }

    unlink("/system_data/priv/mms/app.db-wal");
    unlink("/system_data/priv/mms/app.db-shm");
    unlink("/system_data/priv/mms/appinfo.db-wal");
    unlink("/system_data/priv/mms/appinfo.db-shm");

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

    /* Reopen databases & re-register TabShifter tile */
    if (sqlite3_open(APP_DB_PATH, app_db_ptr) == SQLITE_OK) {
        sqlite3_busy_timeout(*app_db_ptr, SQLITE_BUSY_TIMEOUT_MS);
        sqlite3_exec(*app_db_ptr, "PRAGMA temp_store=MEMORY; PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
    }
    if (sqlite3_open(APPINFO_DB_PATH, appinfo_db_ptr) == SQLITE_OK) {
        sqlite3_busy_timeout(*appinfo_db_ptr, SQLITE_BUSY_TIMEOUT_MS);
        sqlite3_exec(*appinfo_db_ptr, "PRAGMA temp_store=MEMORY; PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
    }

    if (*app_db_ptr) {
        install_tabshifter_launcher(*app_db_ptr, *appinfo_db_ptr, uids, uid_count);
    }

    return 0;
}

/* ========================================================================= */
/*          EMBEDDED WEB UI (SEMVER CHECK, ASCII ARROWS, NO SCROLL-JUMP)     */
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
"      <button type='button' class='btn' id='ub-chg-btn' onclick='toggleChangelog()'>What&#39;s New (v)</button>"
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
"function locLabel(loc){if(loc===138)return '<span class=\"loc-tag loc-138\">GAMES</span>';if(loc===188)return '<span class=\"loc-tag loc-188\">MEDIA</span>';return '<span class=\"loc-tag loc-1\">HIDDEN</span>';}"
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
"    const isHid=(a.loc<=1||a.visible===0);"
"    const hideAct=isHid?'unhide':'hide';"
"    const hideTxt=isHid?'Unhide':'Hide';"
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
"    else if(action==='hide'){item.loc=0;item.visible=0;item.pinned=2;}"
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
"  if(confirm('Repair all game padlocks and restore TabShifter icon?')){"
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
"function toggleChangelog(){"
"  const d=document.getElementById('changelog-drawer');"
"  const b=document.getElementById('ub-chg-btn');"
"  const open=(d.style.display==='block');"
"  d.style.display=open?'none':'block';"
"  b.textContent=open?\"What's New (v)\":\"What's New (^)\";"
"}"
"function isNewerVersion(remoteVer,localVer){"
"  const r=remoteVer.replace(/^v/,'').split('.').map(n=>parseInt(n,10)||0);"
"  const l=localVer.replace(/^v/,'').split('.').map(n=>parseInt(n,10)||0);"
"  for(let i=0;i<3;i++){if((r[i]||0)>(l[i]||0))return true;if((r[i]||0)<(l[i]||0))return false;}"
"  return false;"
"}"
"async function checkGitHubUpdate(){"
"  try{"
"    const vRes=await fetch('/api/version').then(r=>r.json());"
"    const gh=await fetch('https://api.github.com/repos/xvvulcanvx/PS5-TabShifter/releases/latest').then(r=>r.json());"
"    if(!gh || !gh.tag_name)return;"
"    if(isNewerVersion(gh.tag_name,vRes.version)){"
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
             "ORDER BY (c.dispLocation > 1) DESC, (c.titleId LIKE 'NPXS%%') ASC, c.titleName COLLATE NOCASE ASC;",
             primary_uid);

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

    sqlite3_exec(app_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (appinfo_db) sqlite3_exec(appinfo_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);

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
                        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n\r\n",
                        status, content_type, body_len);
    send(client_fd, header, (size_t)hlen, 0);
    if (body && body_len > 0) send(client_fd, body, body_len, 0);
}

static bool handle_http_client(int client_fd, sqlite3 **app_db_ptr, sqlite3 **appinfo_db_ptr,
                               char uids[MAX_USER_PROFILES][32], int uid_count) {
    sqlite3 *app_db = *app_db_ptr;
    sqlite3 *appinfo_db = *appinfo_db_ptr;

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
    } else if (strncmp((char *)req, "GET / ", 6) == 0 || strncmp((char *)req, "GET /?", 6) == 0) {
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
        int rc = restore_clean_db_backup(app_db_ptr, appinfo_db_ptr, uids, uid_count);
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
    signal(SIGPIPE, SIG_IGN);
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
    /* Exact v1.2.1 PRAGMA settings: prevents WAL lock desyncs */
    sqlite3_exec(app_db, "PRAGMA temp_store=MEMORY; PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);

    if (sqlite3_open(APPINFO_DB_PATH, &appinfo_db) == SQLITE_OK) {
        sqlite3_busy_timeout(appinfo_db, SQLITE_BUSY_TIMEOUT_MS);
        sqlite3_exec(appinfo_db, "PRAGMA temp_store=MEMORY; PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
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

    /* Check if tile is already installed before reloading SceShellUI (Prevents black screen on launch!) */
    bool already_installed = is_tbsh_tile_already_installed(app_db);
    install_tabshifter_launcher(app_db, appinfo_db, uids, uid_count);
    if (!already_installed) {
        reload_sceshellui();
    }

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

    notify_ps5("PS5 TabShifter v1.3.0 Ready!\nOpen TabShifter from your Media Tab!");

    bool running = true;
    while (running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            running = handle_http_client(client_fd, &app_db, &appinfo_db, uids, uid_count);
        }
    }

    close(server_fd);
    if (appinfo_db) sqlite3_close(appinfo_db);
    sqlite3_close(app_db);
    return 0;
}

/*
 * cs-snipe.c — CS2 准星切换器（零 VAC 风险方案）
 *
 * 原理：本程序只做文本文件读写，完全不接触游戏进程（无 OpenProcess / 注入 /
 * 钩子 / 模拟输入 / 网络）。切换由游戏引擎自身的 exec 命令完成，与手动编辑
 * autoexec.cfg 完全等价，是 Valve 明确允许的机制。
 *
 * 用法：
 *   cs-snipe.exe                          双击 / 无参数 → 图形界面
 *   cs-snipe.exe -cli                     命令行模式：生成预设+绑定 (默认 F5 F6 F7)
 *   cs-snipe.exe F8 F9 F10                自定义 3 个切换按键
 *   cs-snipe.exe -dir "D:\...\cfg" ...    手动指定 CS2 cfg 目录
 *   cs-snipe.exe -open                    一键打开侦测到的 cfg 目录（资源管理器）
 *   cs-snipe.exe -grab 2                  抓取当前游戏准星配置，覆盖存储槽 2
 *
 * 抓取实时值：先在游戏控制台执行 host_writeconfig（引擎合法命令，即时落盘），
 * 再运行抓取。程序读取的是游戏自己保存的 userdata vcfg 文件。
 *
 * 存储：准星槽主存储 = 程序目录\cfg\cs-snipe1-3.cfg（文件夹不存在自动创建），
 * 应用/抓取时同步副本到游戏 cfg 目录供 exec 使用；设置(按键/目录)持久化在
 * 程序目录\cs-snipe.ini。
 *
 * 编译：gcc -O2 -static -o cs-snipe.exe cs-snipe.c -lshell32 -ladvapi32 -lgdi32
 */
#define _WIN32_WINNT 0x0601
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>
#include <sys/stat.h>
#include <time.h>
#include <windows.h>

#define MAXP 1024
#define NSLOTS 3
#define MARK_B "// cs-snipe-switcher begin"
#define MARK_E "// cs-snipe-switcher end"
#define CS2_SUB "steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo\\cfg"

static const char *steam_roots[] = {
    "C:\\SteamLibrary", "D:\\SteamLibrary", "E:\\SteamLibrary",
    "C:\\Program Files (x86)\\Steam", "C:\\Program Files\\Steam",
    "D:\\Steam", "E:\\Steam", "B:\\HauntZone\\softWare\\Steam",
};

/* 内置预设：1=小绿十字(静态) 2=青色小点(静态) 3=白色经典动态 */
static const char *presets[NSLOTS] = {
    "cl_crosshairstyle 4\ncl_crosshairsize 2\ncl_crosshairthickness 0.5\n"
    "cl_crosshairgap -3\ncl_crosshairdot 0\ncl_crosshair_t 0\n"
    "cl_crosshair_drawoutline 1\ncl_crosshair_outlinethickness 0.5\n"
    "cl_crosshaircolor 1\ncl_crosshairalpha 255\ncl_crosshairusealpha 1\n"
    "cl_crosshair_recoil 0\ncl_crosshairgap_useweaponvalue 0\n",

    "cl_crosshairstyle 4\ncl_crosshairsize 0\ncl_crosshairthickness 1\n"
    "cl_crosshairgap -4\ncl_crosshairdot 1\ncl_crosshair_t 0\n"
    "cl_crosshair_drawoutline 1\ncl_crosshair_outlinethickness 0.5\n"
    "cl_crosshaircolor 5\ncl_crosshaircolor_r 0\ncl_crosshaircolor_g 255\n"
    "cl_crosshaircolor_b 255\ncl_crosshairalpha 255\ncl_crosshairusealpha 1\n"
    "cl_crosshair_recoil 0\ncl_crosshairgap_useweaponvalue 0\n",

    "cl_crosshairstyle 2\ncl_crosshairsize 3\ncl_crosshairthickness 0.6\n"
    "cl_crosshairgap -2\ncl_crosshairdot 0\ncl_crosshair_t 0\n"
    "cl_crosshair_drawoutline 0\ncl_crosshaircolor 5\ncl_crosshaircolor_r 255\n"
    "cl_crosshaircolor_g 255\ncl_crosshaircolor_b 255\ncl_crosshairalpha 255\n"
    "cl_crosshairusealpha 1\ncl_crosshair_recoil 0\ncl_crosshairgap_useweaponvalue 0\n",
};

static const char *preset_names[NSLOTS] = {
    "预设: 小绿十字 (静态)", "预设: 青色小点 (静态)", "预设: 白色经典动态",
};

static char g_progcfg[MAXP];  /* 程序目录\cfg：准星槽主存储（不存在自动创建） */
static char g_ini[MAXP];      /* 程序目录\cs-snipe.ini：按键/目录设置持久化 */

static void norm_path(char *s) { for (; *s; s++) if (*s == '/') *s = '\\'; }

static int dir_exists(const char *p) { return _access(p, 0) == 0; }

/* 追加格式化日志（CLI 打印 / GUI 显示共用） */
static void logadd(char *log, size_t cap, const char *fmt, ...) {
    size_t len = strlen(log);
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log + len, cap - len, fmt, ap);
    va_end(ap);
}

static void usage(void) {
    fprintf(stderr,
        "用法:\n"
        "  cs-snipe.exe                        图形界面 (双击)\n"
        "  cs-snipe.exe -cli                   命令行: 生成预设+绑定 (默认 F5 F6 F7)\n"
        "  cs-snipe.exe F8 F9 F10              自定义 3 个切换按键 (none = 该槽设空)\n"
        "  cs-snipe.exe -dir <cfg目录> [...]   手动指定 CS2 cfg 目录\n"
        "  cs-snipe.exe -open                  打开侦测到的 cfg 目录\n"
        "  cs-snipe.exe -grab <1-3>            抓取当前游戏准星覆盖指定存储槽\n");
}

/* 读注册表 Steam 客户端路径（规范化斜杠） */
static int reg_steam_path(char *out, size_t n) {
    DWORD cb = (DWORD)n;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath",
                     RRF_RT_REG_SZ, NULL, out, &cb) != ERROR_SUCCESS) return 0;
    out[n - 1] = 0;
    norm_path(out);
    return out[0] != 0;
}

/* 三级优先定位 cfg 目录：-dir > 候选扫描 > 交互式手动输入 */
static int detect_cfgdir(char *out, size_t n) {
    char steam[MAXP];
    if (reg_steam_path(steam, sizeof steam)) {
        snprintf(out, n, "%.900s\\%s", steam, CS2_SUB);
        if (dir_exists(out)) return 1;
    }
    for (size_t i = 0; i < sizeof steam_roots / sizeof steam_roots[0]; i++) {
        snprintf(out, n, "%s\\%s", steam_roots[i], CS2_SUB);
        if (dir_exists(out)) return 1;
    }
    return 0;
}

static int prompt_cfgdir(char *out, size_t n) {
    char buf[MAXP];
    printf("自动侦测失败，请输入 CS2 cfg 目录完整路径: ");
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) return 0;
    buf[strcspn(buf, "\r\n")] = 0;
    char *s = buf;
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l-1] == ' ' || s[l-1] == '\t')) s[--l] = 0;
    if (l >= 2 && s[0] == '"' && s[l-1] == '"') { s[l-1] = 0; s++; }
    snprintf(out, n, "%s", s);
    norm_path(out);
    return dir_exists(out);
}

/* 读整个文件（调用者 free） */
static char *read_file(const char *path, long *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = 0;
    fclose(f);
    if (len_out) *len_out = (long)rd;
    return buf;
}

/* 程序目录初始化：cfg 子文件夹不存在则自动创建 */
static void init_paths(void) {
    char p[MAXP];
    GetModuleFileNameA(NULL, p, sizeof p);
    char *s = strrchr(p, '\\');
    if (s) *s = 0;
    snprintf(g_progcfg, sizeof g_progcfg, "%.990s\\cfg", p);
    snprintf(g_ini, sizeof g_ini, "%.990s\\cs-snipe.ini", p);
    CreateDirectoryA(g_progcfg, NULL);   /* 已存在时失败忽略 */
}

static int copy_file(const char *src, const char *dst) {
    long n = 0;
    char *b = read_file(src, &n);
    if (!b) return 0;
    FILE *f = fopen(dst, "wb");
    if (!f) { free(b); return 0; }
    fwrite(b, 1, (size_t)n, f);
    fclose(f);
    free(b);
    return 1;
}

/* 主存储(程序cfg) → 游戏 cfg 目录同步（exec 只能读游戏目录） */
static void sync_slots(const char *gamedir, char *log, size_t cap) {
    for (int i = 0; i < NSLOTS; i++) {
        char a[MAXP], b[MAXP];
        snprintf(a, sizeof a, "%.990s\\cs-snipe%d.cfg", g_progcfg, i + 1);
        snprintf(b, sizeof b, "%.990s\\cs-snipe%d.cfg", gamedir, i + 1);
        if (!copy_file(a, b)) logadd(log, cap, "同步失败: %s\n", b);
    }
}

/* 设置持久化：keys=K1,K2,K3（空串=该槽解绑） cs2dir=... */
static void settings_load(char keys[3][64], char *cs2dir, size_t dn) {
    FILE *f = fopen(g_ini, "rb");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "keys=", 5)) {
            char *p = line + 5;
            for (int i = 0; i < 3; i++) {
                char *c = strchr(p, ',');
                size_t l = c ? (size_t)(c - p) : strlen(p);
                if (l > 63) l = 63;
                memcpy(keys[i], p, l);
                keys[i][l] = 0;
                p = c ? c + 1 : p + strlen(p);
            }
        } else if (!strncmp(line, "cs2dir=", 7)) {
            snprintf(cs2dir, dn, "%.1000s", line + 7);
        }
    }
    fclose(f);
}

static void settings_save(const char *keys[3], const char *cs2dir) {
    FILE *f = fopen(g_ini, "wb");
    if (!f) return;
    fprintf(f, "keys=%s,%s,%s\ncs2dir=%s\n", keys[0], keys[1], keys[2], cs2dir);
    fclose(f);
}

/* 生成预设槽文件（写入主存储）；已抓取(captured)的槽不被预设覆盖 */
static void write_presets(char *log, size_t cap) {
    for (int i = 0; i < NSLOTS; i++) {
        char path[MAXP];
        snprintf(path, sizeof path, "%.990s\\cs-snipe%d.cfg", g_progcfg, i + 1);
        long len = 0;
        char *old = read_file(path, &len);
        int captured = old && strstr(old, "(captured)") != NULL;
        free(old);
        if (captured) {
            logadd(log, cap, "  跳过 %s (已抓取内容，保留)\n", path);
            continue;
        }
        FILE *f = fopen(path, "wb");
        if (!f) { logadd(log, cap, "无法写入 %s\n", path); continue; }
        fprintf(f, "// cs-snipe slot %d (preset)\n%s"
                   "echo \"[cs-snipe] Preset %d applied\"\n", i + 1, presets[i], i + 1);
        fclose(f);
        logadd(log, cap, "  生成 %s\n", path);
    }
}

/* 更新 autoexec.cfg 中的托管绑定区块（区块外内容原样保留）
 * 空按键 = 解绑：对旧区块中该槽的按键写 unbind */
static int update_autoexec(const char *cfgdir, const char *keys[3], char *log, size_t cap) {
    char path[MAXP];
    snprintf(path, sizeof path, "%.990s\\autoexec.cfg", cfgdir);
    long len = 0;
    char *old = read_file(path, &len);
    char *b = old ? strstr(old, MARK_B) : NULL;
    char *e = b ? strstr(b, MARK_E) : NULL;
    char oldkeys[3][64] = {{0}, {0}, {0}};
    if (b && e) {
        char *p = b;
        while (p < e) {
            char *nl = strchr(p, '\n');
            size_t ll = nl ? (size_t)(nl - p) : strlen(p);
            if (ll > 255) ll = 255;
            char line[256];
            memcpy(line, p, ll);
            line[ll] = 0;
            char k[64];
            int n = 0;
            if (sscanf(line, "bind \"%63[^\"]\" \"exec cs-snipe%d\"", k, &n) == 2 && n >= 1 && n <= 3)
                snprintf(oldkeys[n - 1], sizeof oldkeys[0], "%s", k);
            if (!nl) break;
            p = nl + 1;
        }
    }
    char block[1024];
    int off = snprintf(block, sizeof block, MARK_B "\n");
    for (int i = 0; i < 3; i++) {
        if (keys[i][0])
            off += snprintf(block + off, sizeof block - off,
                            "bind \"%s\" \"exec cs-snipe%d\"\n", keys[i], i + 1);
        else if (oldkeys[i][0])
            off += snprintf(block + off, sizeof block - off,
                            "unbind \"%s\"\n", oldkeys[i]);
    }
    snprintf(block + off, sizeof block - off, MARK_E "\n");
    FILE *f = fopen(path, "wb");
    if (!f) { logadd(log, cap, "无法写入 %s\n", path); free(old); return 0; }
    if (b) {
        fwrite(old, 1, (size_t)(b - old), f);
        fputs(block, f);
        if (e) {
            char *nl = strchr(e, '\n');
            if (nl) fputs(nl + 1, f);
        }
    } else {
        if (old && len > 0) {
            fwrite(old, 1, (size_t)len, f);
            if (old[len - 1] != '\n') fputc('\n', f);
        }
        fputs(block, f);
    }
    fclose(f);
    free(old);
    return 1;
}

/* 从一行中提取两个引号 token */
static int extract_quoted(const char *line, char *t1, size_t n1, char *t2, size_t n2) {
    const char *p = strchr(line, '"');
    if (!p) return 0;
    const char *e = strchr(p + 1, '"');
    if (!e || (size_t)(e - p - 1) >= n1) return 0;
    memcpy(t1, p + 1, (size_t)(e - p - 1)); t1[e - p - 1] = 0;
    p = strchr(e + 1, '"');
    if (!p) return 0;
    e = strchr(p + 1, '"');
    if (!e || (size_t)(e - p - 1) >= n2) return 0;
    memcpy(t2, p + 1, (size_t)(e - p - 1)); t2[e - p - 1] = 0;
    return 1;
}

/* 回退：扫描各 Steam 根目录下所有账号的 convars 文件，取修改时间最新者 */
static int find_convars_scan(char *out, size_t n) {
    char roots[16][MAXP];
    int nr = 0;
    if (reg_steam_path(roots[nr], MAXP)) nr++;
    for (size_t i = 0; i < sizeof steam_roots / sizeof steam_roots[0] && nr < 16; i++)
        snprintf(roots[nr++], MAXP, "%s", steam_roots[i]);
    const char *variants[] = {
        "local\\cfg\\cs2_user_convars_0_slot0.vcfg",
        "remote\\cs2_user_convars.vcfg",
    };
    FILETIME best = {0, 0};
    int found = 0;
    for (int r = 0; r < nr; r++) {
        char pat[MAXP];
        snprintf(pat, sizeof pat, "%.990s\\userdata\\*", roots[r]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            for (int v = 0; v < 2; v++) {
                char p[MAXP];
                snprintf(p, sizeof p, "%.700s\\userdata\\%.250s\\730\\%s",
                         roots[r], fd.cFileName, variants[v]);
                WIN32_FILE_ATTRIBUTE_DATA fa;
                if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa) &&
                    CompareFileTime(&fa.ftLastWriteTime, &best) > 0) {
                    best = fa.ftLastWriteTime;
                    snprintf(out, n, "%s", p);
                    found = 1;
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return found;
}

/* 抓取当前游戏准星 → 覆盖存储槽 slot */
static int do_grab(const char *cfgdir, int slot, char *log, size_t cap) {
    char src[MAXP] = "";
    char steam[MAXP];
    DWORD id = 0, cb = sizeof id;
    if (reg_steam_path(steam, sizeof steam) &&
        RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess",
                     "ActiveUser", RRF_RT_REG_DWORD, NULL, &id, &cb) == ERROR_SUCCESS && id) {
        snprintf(src, sizeof src, "%.900s\\userdata\\%lu\\730\\local\\cfg\\cs2_user_convars_0_slot0.vcfg",
                 steam, (unsigned long)id);
        if (!dir_exists(src))
            snprintf(src, sizeof src, "%.900s\\userdata\\%lu\\730\\remote\\cs2_user_convars.vcfg",
                     steam, (unsigned long)id);
        if (!dir_exists(src)) src[0] = 0;
    }
    if (!src[0] && !find_convars_scan(src, sizeof src)) {
        logadd(log, cap, "找不到游戏准星落盘文件 (userdata\\<账号>\\730\\...\\cs2_user_convars*.vcfg)\n");
        return 1;
    }
    FILE *f = fopen(src, "rb");
    if (!f) { logadd(log, cap, "无法读取 %s\n", src); return 1; }
    char dst[MAXP];
    snprintf(dst, sizeof dst, "%.990s\\cs-snipe%d.cfg", g_progcfg, slot);
    FILE *o = fopen(dst, "wb");
    if (!o) { logadd(log, cap, "无法写入 %s\n", dst); fclose(f); return 1; }
    fprintf(o, "// cs-snipe slot %d (captured)\n", slot);
    int count = 0;
    char line[2048], name[256], val[256];
    while (fgets(line, sizeof line, f)) {
        if (!extract_quoted(line, name, sizeof name, val, sizeof val)) continue;
        if (strncmp(name, "cl_crosshair", 12) != 0) continue;
        if (!strcmp(val, "true")) strcpy(val, "1");
        else if (!strcmp(val, "false")) strcpy(val, "0");
        fprintf(o, "%s %s\n", name, val);
        count++;
    }
    fprintf(o, "echo \"[cs-snipe] Slot %d loaded (captured)\"\n", slot);
    fclose(f);
    fclose(o);
    if (count == 0) { logadd(log, cap, "源文件中未找到 cl_crosshair* 变量: %s\n", src); return 1; }
    char gdst[MAXP];
    snprintf(gdst, sizeof gdst, "%.990s\\cs-snipe%d.cfg", cfgdir, slot);
    if (!copy_file(dst, gdst)) logadd(log, cap, "同步到游戏目录失败: %s\n", gdst);
    struct _stat st;
    logadd(log, cap, "已抓取 %d 个准星变量 -> %s (已同步游戏目录)\n", count, dst);
    logadd(log, cap, "源文件: %s\n", src);
    if (_stat(src, &st) == 0) logadd(log, cap, "源文件修改时间: %s", ctime(&st.st_mtime));
    logadd(log, cap, "提示: 对局内抓实时值，先在游戏控制台执行 host_writeconfig 再抓取\n");
    return 0;
}

/* ============================ GUI ============================ */

#define IDC_DIR   101
#define IDC_OPEN  102
#define IDC_KEY1  103
#define IDC_APPLY 106
#define IDC_SLOT1 107
#define IDC_GRAB1 110
#define IDC_LOG   113

static HINSTANCE g_hi;
static HFONT g_font;
static HWND g_dir, g_key[3], g_slot[3], g_log;
static char g_logbuf[16384];
static char g_keys[3][64] = {"F5", "F6", "F7"};
static int g_listening = -1;   /* 正在监听按键的槽，-1 = 无 */

static void u2w(const char *s, wchar_t *w, int wn) { MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wn); }
static void a2w(const char *s, wchar_t *w, int wn) { MultiByteToWideChar(CP_ACP, 0, s, -1, w, wn); }
static void w2a(const wchar_t *w, char *s, int sn) { WideCharToMultiByte(CP_ACP, 0, w, -1, s, sn, NULL, NULL); }

static void set_u(HWND h, const char *utf8) {
    wchar_t w[2048];
    u2w(utf8, w, 2048);
    SetWindowTextW(h, w);
}

static void get_a(HWND h, char *s, int sn) {
    wchar_t w[1024];
    GetWindowTextW(h, w, 1024);
    w2a(w, s, sn);
}

static void gui_log(const char *utf8) {
    logadd(g_logbuf, sizeof g_logbuf, "%s", utf8);
    set_u(g_log, g_logbuf);
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void refresh_keybtn(int i) {
    wchar_t w[80];
    if (g_listening == i) u2w("按键...", w, 80);
    else if (g_keys[i][0]) u2w(g_keys[i], w, 80);
    else u2w("(空)", w, 80);
    SetWindowTextW(g_key[i], w);
}

/* 虚拟键码 → CS2 键名；不支持的键返回 NULL */
static const char *vk_to_cs2(WPARAM vk) {
    static char buf[16];
    if (vk >= 'A' && vk <= 'Z') { buf[0] = (char)(vk - 'A' + 'a'); buf[1] = 0; return buf; }
    if (vk >= '0' && vk <= '9') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, sizeof buf, "F%d", (int)(vk - VK_F1 + 1));
        return buf;
    }
    switch (vk) {
    case VK_SPACE:  return "space";
    case VK_RETURN: return "enter";
    case VK_TAB:    return "tab";
    case VK_DELETE: return "delete";
    case VK_INSERT: return "insert";
    case VK_HOME:   return "home";
    case VK_END:    return "end";
    case VK_PRIOR:  return "pgup";
    case VK_NEXT:   return "pgdn";
    case VK_UP:     return "uparrow";
    case VK_DOWN:   return "downarrow";
    case VK_LEFT:   return "leftarrow";
    case VK_RIGHT:  return "rightarrow";
    case VK_SHIFT:  return "shift";
    case VK_CONTROL:return "ctrl";
    case VK_MENU:   return "alt";
    case VK_CAPITAL:return "capslock";
    case VK_NUMLOCK:return "numlock";
    case VK_SCROLL: return "scrolllock";
    case VK_LWIN:
    case VK_RWIN:   return "win";
    }
    return NULL;
}

static void refresh_slots(void) {
    for (int i = 0; i < NSLOTS; i++) {
        char path[MAXP];
        snprintf(path, sizeof path, "%.990s\\cs-snipe%d.cfg", g_progcfg, i + 1);
        long len = 0;
        char *c = read_file(path, &len);
        char lbl[128];
        const char *st;
        if (!c) st = "未生成";
        else if (strstr(c, "(captured)")) {
            struct _stat fs;
            if (_stat(path, &fs) == 0) {
                struct tm *tm = localtime(&fs.st_mtime);
                snprintf(lbl, sizeof lbl, "已抓取: 游戏当前准星 (%02d:%02d:%02d)",
                         tm->tm_hour, tm->tm_min, tm->tm_sec);
                st = lbl;
            } else st = "已抓取: 游戏当前准星";
        } else st = preset_names[i];
        free(c);
        set_u(g_slot[i], st);
    }
}

/* 从目录输入框取有效路径；无效则记日志返回 0 */
static int gui_dir(char *out, size_t n) {
    get_a(g_dir, out, (int)n);
    norm_path(out);
    if (!dir_exists(out)) {
        gui_log("目录不存在，请检查 cfg 目录路径 (可用 -open 侦测或手动填写)\n");
        return 0;
    }
    return 1;
}

static HWND mk(HWND parent, const wchar_t *cls, const wchar_t *text,
               DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static LRESULT CALLBACK gui_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_listening >= 0) {
            int i = g_listening;
            char msgbuf[256];
            msgbuf[0] = 0;
            if (wp == VK_ESCAPE) {
                g_listening = -1;
                refresh_keybtn(i);
                logadd(msgbuf, sizeof msgbuf, "已取消槽 %d 的按键修改\n", i + 1);
            } else if (wp == VK_BACK) {
                g_keys[i][0] = 0;
                g_listening = -1;
                refresh_keybtn(i);
                logadd(msgbuf, sizeof msgbuf, "槽 %d 设为空 (应用绑定时解绑旧键)\n", i + 1);
            } else {
                const char *name = vk_to_cs2(wp);
                if (name) {
                    snprintf(g_keys[i], sizeof g_keys[i], "%s", name);
                    g_listening = -1;
                    refresh_keybtn(i);
                    logadd(msgbuf, sizeof msgbuf, "槽 %d 绑定按键: %s (点「应用绑定」生效)\n", i + 1, name);
                } else {
                    logadd(msgbuf, sizeof msgbuf, "该键不支持，请按其他键 (Esc 取消)\n");
                }
            }
            gui_log(msgbuf);
            return 0;
        }
        break;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        char dir[MAXP], oplog[8192];
        if (id >= IDC_KEY1 && id < IDC_KEY1 + NSLOTS) {
            if (g_listening >= 0) refresh_keybtn(g_listening);
            g_listening = id - IDC_KEY1;
            refresh_keybtn(g_listening);
            SetFocus(hwnd);   /* 键盘焦点收到主窗口以捕获按键 */
            oplog[0] = 0;
            logadd(oplog, sizeof oplog,
                   "监听槽 %d: 按下任意键绑定, Esc 取消, Backspace 设空\n", g_listening + 1);
            gui_log(oplog);
        } else if (id == IDC_OPEN) {
            if (!gui_dir(dir, sizeof dir)) return 0;
            wchar_t wdir[MAXP];
            a2w(dir, wdir, MAXP);
            ShellExecuteW(NULL, L"open", wdir, NULL, NULL, SW_SHOWNORMAL);
            oplog[0] = 0;
            logadd(oplog, sizeof oplog, "已打开: %s\n", dir);
            gui_log(oplog);
        } else if (id == IDC_APPLY) {
            if (!gui_dir(dir, sizeof dir)) return 0;
            if (g_listening >= 0) {   /* 先结束监听，避免半截状态 */
                int i = g_listening;
                g_listening = -1;
                refresh_keybtn(i);
            }
            const char *keys[3];
            for (int i = 0; i < 3; i++) keys[i] = g_keys[i];
            oplog[0] = 0;
            write_presets(oplog, sizeof oplog);
            sync_slots(dir, oplog, sizeof oplog);
            settings_save(keys, dir);
            if (update_autoexec(dir, keys, oplog, sizeof oplog))
                logadd(oplog, sizeof oplog, "绑定: %s -> cs-snipe1 | %s -> cs-snipe2 | %s -> cs-snipe3\n"
                     "进游戏控制台执行一次 exec autoexec 生效\n",
                     keys[0][0] ? keys[0] : "(空)",
                     keys[1][0] ? keys[1] : "(空)",
                     keys[2][0] ? keys[2] : "(空)");
            gui_log(oplog);
            refresh_slots();
        } else if (id >= IDC_GRAB1 && id < IDC_GRAB1 + NSLOTS) {
            if (!gui_dir(dir, sizeof dir)) return 0;
            oplog[0] = 0;
            do_grab(dir, id - IDC_GRAB1 + 1, oplog, sizeof oplog);
            gui_log(oplog);
            refresh_slots();
        } else if (id == IDC_DIR && HIWORD(wp) == EN_CHANGE) {
            get_a(g_dir, dir, sizeof dir);
            norm_path(dir);
            if (dir_exists(dir)) refresh_slots();
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int gui_main(void) {
    /* 双击启动(仅本进程挂在控制台上)时释放控制台，避免黑窗口 */
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) <= 1) FreeConsole();

    g_hi = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = gui_proc;
    wc.hInstance = g_hi;
    wc.lpszClassName = L"cs-snipe_gui";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    RegisterClassW(&wc);

    g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Microsoft YaHei UI");

    RECT rc = {0, 0, 560, 400};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND hwnd = CreateWindowExW(0, L"cs-snipe_gui", L"CS2 准星切换器 (零 VAC 风险)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, g_hi, NULL);
    if (!hwnd) return 1;

    mk(hwnd, L"STATIC", L"cfg 目录", 0, 12, 16, 60, 20, 0);
    g_dir = mk(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 76, 12, 372, 24, IDC_DIR);
    mk(hwnd, L"BUTTON", L"打开目录", BS_PUSHBUTTON, 456, 11, 92, 26, IDC_OPEN);

    mk(hwnd, L"STATIC", L"切换按键", 0, 12, 52, 60, 20, 0);
    for (int i = 0; i < 3; i++) {
        g_key[i] = mk(hwnd, L"BUTTON", L"", BS_PUSHBUTTON,
                      76 + i * 88, 47, 80, 26, IDC_KEY1 + i);
        refresh_keybtn(i);
    }
    mk(hwnd, L"BUTTON", L"生成预设 + 应用绑定", BS_PUSHBUTTON, 344, 47, 172, 26, IDC_APPLY);

    for (int i = 0; i < NSLOTS; i++) {
        wchar_t label[16];
        swprintf(label, 16, L"槽 %d", i + 1);
        int y = 90 + i * 40;
        mk(hwnd, L"STATIC", label, 0, 12, y + 4, 44, 20, 0);
        g_slot[i] = mk(hwnd, L"STATIC", L"", 0, 60, y + 4, 300, 20, IDC_SLOT1 + i);
        mk(hwnd, L"BUTTON", L"抓取当前准星", BS_PUSHBUTTON, 380, y, 168, 28, IDC_GRAB1 + i);
    }

    g_log = mk(hwnd, L"EDIT", L"",
               WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
               12, 212, 536, 150, IDC_LOG);
    mk(hwnd, L"STATIC",
       L"对局内抓实时值: 先在游戏控制台执行 host_writeconfig，再点抓取",
       0, 12, 372, 536, 20, 0);

    /* 初始化：加载持久化设置 → 侦测目录 → 刷新槽状态 */
    char dir[MAXP] = "";
    g_logbuf[0] = 0;
    settings_load(g_keys, dir, sizeof dir);
    for (int i = 0; i < 3; i++) refresh_keybtn(i);
    if ((!dir[0] || !dir_exists(dir)) && !detect_cfgdir(dir, sizeof dir)) dir[0] = 0;
    if (dir[0]) {
        wchar_t wdir[MAXP];
        a2w(dir, wdir, MAXP);
        SetWindowTextW(g_dir, wdir);
        refresh_slots();
        gui_log("已就绪 (主存储: 程序目录\\cfg, 设置: cs-snipe.ini)。修改按键后点「生成预设 + 应用绑定」，"
                "进游戏控制台执行一次 exec autoexec 即可对局内切换。\n");
    } else {
        gui_log("自动侦测失败，请手动填写 CS2 cfg 目录完整路径。\n");
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

/* ============================ CLI ============================ */

int main(int argc, char **argv) {
    init_paths();
    if (argc == 1) return gui_main();

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    const char *dir_arg = NULL;
    int open_flag = 0, grab_slot = 0;
    char skeys[3][64] = {"F5", "F6", "F7"};
    char sdir[MAXP] = "";
    settings_load(skeys, sdir, sizeof sdir);
    const char *keys[3] = {skeys[0], skeys[1], skeys[2]};
    int nkeys = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-dir")) {
            if (++i >= argc) { usage(); return 1; }
            dir_arg = argv[i];
        } else if (!strcmp(argv[i], "-open")) {
            open_flag = 1;
        } else if (!strcmp(argv[i], "-cli")) {
            /* 命令行默认生成（无参数时改为 GUI，此开关保留旧行为） */
        } else if (!strcmp(argv[i], "-grab")) {
            if (++i >= argc) { usage(); return 1; }
            grab_slot = atoi(argv[i]);
            if (grab_slot < 1 || grab_slot > NSLOTS) {
                fprintf(stderr, "槽号必须是 1-%d\n", NSLOTS);
                return 1;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            usage();
            return 1;
        } else {
            if (nkeys >= 3) { usage(); return 1; }
            keys[nkeys++] = !strcmp(argv[i], "none") ? "" : argv[i];
        }
    }
    if (nkeys != 0 && nkeys != 3) { usage(); return 1; }
    for (int i = 0; i < nkeys; i++) {
        if (keys[i][0] && strpbrk(keys[i], " \t\"'")) {
            fprintf(stderr, "非法按键名: %s (不能含空格/引号)\n", keys[i]);
            return 1;
        }
    }

    char cfgdir[MAXP];
    if (dir_arg) {
        snprintf(cfgdir, sizeof cfgdir, "%s", dir_arg);
        norm_path(cfgdir);
        if (!dir_exists(cfgdir)) {
            fprintf(stderr, "目录不存在: %s\n", cfgdir);
            return 1;
        }
    } else if (sdir[0] && dir_exists(sdir)) {
        snprintf(cfgdir, sizeof cfgdir, "%s", sdir);
    } else if (!detect_cfgdir(cfgdir, sizeof cfgdir)) {
        if (!prompt_cfgdir(cfgdir, sizeof cfgdir)) {
            fprintf(stderr, "目录无效，退出。可用 -dir 手动指定。\n");
            return 1;
        }
    }

    if (open_flag) {
        ShellExecuteA(NULL, "open", cfgdir, NULL, NULL, SW_SHOWNORMAL);
        printf("已打开: %s\n", cfgdir);
        return 0;
    }

    char log[8192];
    log[0] = 0;
    if (grab_slot) {
        int r = do_grab(cfgdir, grab_slot, log, sizeof log);
        if (r == 0) settings_save(keys, cfgdir);
        printf("%s", log);
        return r;
    }

    printf("CS2 cfg 目录: %s\n", cfgdir);
    printf("主存储: %s\n", g_progcfg);
    write_presets(log, sizeof log);
    sync_slots(cfgdir, log, sizeof log);
    if (!update_autoexec(cfgdir, keys, log, sizeof log)) { printf("%s", log); return 1; }
    settings_save(keys, cfgdir);
    printf("%s", log);
    printf("绑定: %s -> cs-snipe1 | %s -> cs-snipe2 | %s -> cs-snipe3\n",
           keys[0][0] ? keys[0] : "(空)",
           keys[1][0] ? keys[1] : "(空)",
           keys[2][0] ? keys[2] : "(空)");
    printf("\n使用说明:\n");
    printf("  1. 进游戏后控制台执行一次 exec autoexec (或重启游戏)\n");
    printf("  2. 对局内按 %s / %s / %s 随时切换准星\n", keys[0], keys[1], keys[2]);
    printf("  换键: cs-snipe.exe F8 F9 F10 重跑后再 exec autoexec\n");
    printf("  抓取当前准星: 游戏控制台 host_writeconfig 后运行 cs-snipe.exe -grab <1-3>\n");
    printf("  打开 cfg 目录: cs-snipe.exe -open\n");
    return 0;
}
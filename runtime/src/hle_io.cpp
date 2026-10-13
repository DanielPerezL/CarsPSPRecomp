// IoFileMgrForUser y sceUmdUser: el UMD se lee de la ISO (iso.cpp); ms0: es la carpeta de partidas.
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <map>

#include "iso.h"
#include "threads.h"

std::string g_disc_root = "extracted";
std::string g_save_root = "saves";
static std::string g_cwd = "disc0:/PSP_GAME/USRDIR";

// Resuelve un componente sin distinguir mayúsculas (el UMD es ISO9660 en mayúsculas).
static std::string find_ci(const std::string &dir, const std::string &name) {
    std::string direct = dir + "/" + name;
    struct stat st;
    if (stat(direct.c_str(), &st) == 0) return direct;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            if (strcasecmp(e->d_name, name.c_str()) == 0) {
                std::string r = dir + "/" + e->d_name;
                closedir(d);
                return r;
            }
        }
        closedir(d);
    }
    return direct;
}

// Divide "dev:/ruta" -> (dispositivo en minúsculas, ruta); sin dispositivo usa el directorio actual.
static std::pair<std::string, std::string> split_path(const std::string &psp_path_in) {
    std::string p = psp_path_in;
    if (p.find(':') == std::string::npos) p = g_cwd + "/" + p;
    size_t colon = p.find(':');
    std::string dev = p.substr(0, colon);
    std::transform(dev.begin(), dev.end(), dev.begin(), ::tolower);
    while (!dev.empty() && isdigit((unsigned char)dev.back())) dev.pop_back();
    return {dev, p.substr(colon + 1)};
}

static bool is_disc(const std::string &dev) { return dev == "disc" || dev == "umd"; }

static std::string host_path(const std::string &psp_path_in) {
    auto [dev, rest] = split_path(psp_path_in);
    std::string out = is_disc(dev) ? g_disc_root : g_save_root;
    size_t i = 0;
    while (i < rest.size()) {
        size_t j = rest.find('/', i);
        if (j == std::string::npos) j = rest.size();
        std::string comp = rest.substr(i, j - i);
        if (!comp.empty() && comp != ".") out = find_ci(out, comp);
        i = j + 1;
    }
    return out;
}

struct OpenFile {
    FILE *f = nullptr;    // fichero del host (ms0:)
    bool iso = false;     // fichero del UMD
    IsoEntry entry;
    uint64_t pos = 0;
    std::string path;
    int64_t async_result = 0;
    bool closed = false;
};
static std::map<int, OpenFile> g_files;
static int g_next_fd = 3;

static void mkdirs(const std::string &path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

static int io_open(const char *psp, uint32_t flags) {
    auto [dev, rest] = split_path(psp);
    OpenFile of;
    of.path = psp;
    if (is_disc(dev) && iso_loaded()) {
        if (!iso_lookup(rest, &of.entry) || of.entry.dir) {
            LOGD("io", "open '%s' NO ENCONTRADO en la ISO", psp);
            return (int)SCE_ERROR_FILE_NOT_FOUND;
        }
        of.iso = true;
    } else {
        std::string hp = host_path(psp);
        const char *mode = "rb";
        if (flags & 0x0002) {  // escritura
            mode = (flags & 0x0200) ? "w+b" : "r+b";  // PSP_O_CREAT
            if (flags & 0x0400) mode = "w+b";         // PSP_O_TRUNC
            if (flags & 0x0100) mode = "a+b";         // PSP_O_APPEND
            mkdirs(hp);
        }
        of.f = fopen(hp.c_str(), mode);
        if (!of.f) {
            LOGD("io", "open '%s' -> '%s' NO ENCONTRADO", psp, hp.c_str());
            return (int)SCE_ERROR_FILE_NOT_FOUND;
        }
    }
    int fd = g_next_fd++;
    g_files[fd] = of;
    LOGD("io", "open '%s' -> fd %d", psp, fd);
    return fd;
}

static OpenFile *get_file(int fd) {
    auto it = g_files.find(fd);
    return it == g_files.end() || it->second.closed ? nullptr : &it->second;
}

HLE(sceIoOpen) { RET(io_open(mem_str(A0), A1)); }
static void close_file(OpenFile *f) {
    if (f->f) fclose(f->f);
    f->f = nullptr;
    f->closed = true;
}
HLE(sceIoClose) {
    OpenFile *f = get_file((int)A0);
    if (!f) RET(SCE_ERROR_BAD_FD);
    close_file(f);
    g_files.erase((int)A0);
    RET(0);
}
HLE(sceIoCloseAsync) {
    OpenFile *f = get_file((int)A0);
    if (!f) RET(SCE_ERROR_BAD_FD);
    close_file(f);
    f->async_result = 0;
    RET(0);
}

static int64_t io_read(int fd, uint32_t buf, uint32_t size) {
    OpenFile *f = get_file(fd);
    if (!f) return (int32_t)SCE_ERROR_BAD_FD;
    if (f->iso) {
        uint64_t left = f->pos < f->entry.size ? f->entry.size - f->pos : 0;
        size_t n = iso_read(f->entry.lbn, f->pos, MEMP(buf), size < left ? size : left);
        f->pos += n;
        return (int64_t)n;
    }
    return (int64_t)fread(MEMP(buf), 1, size, f->f);
}
HLE(sceIoRead) { RET(io_read((int)A0, A1, A2)); }
HLE(sceIoReadAsync) {
    OpenFile *f = get_file((int)A0);
    if (!f) RET(SCE_ERROR_BAD_FD);
    f->async_result = io_read((int)A0, A1, A2);
    RET(0);
}
HLE(sceIoWrite) {
    if (A0 == 1 || A0 == 2) {  // stdout/stderr
        LOGI("stdout", "%.*s", (int)A2, mem_str(A1));
        RET(A2);
    }
    OpenFile *f = get_file((int)A0);
    if (!f || !f->f) RET(SCE_ERROR_BAD_FD);
    RET(fwrite(MEMP(A1), 1, A2, f->f));
}
HLE(sceIoLseek) {
    // int64 offset en a2:a3 (alineado a par de registros), whence en t0
    OpenFile *f = get_file((int)A0);
    if (!f) { c->r[2] = c->r[3] = SCE_ERROR_BAD_FD; return; }
    int64_t off = (int64_t)(((uint64_t)A3 << 32) | A2);
    int64_t pos;
    if (f->iso) {
        int64_t base = T0 == 0 ? 0 : T0 == 1 ? (int64_t)f->pos : (int64_t)f->entry.size;
        f->pos = (uint64_t)(base + off < 0 ? 0 : base + off);
        pos = (int64_t)f->pos;
    } else {
        fseeko(f->f, off, T0 == 0 ? SEEK_SET : T0 == 1 ? SEEK_CUR : SEEK_END);
        pos = ftello(f->f);
    }
    c->r[2] = (uint32_t)pos;
    c->r[3] = (uint32_t)(pos >> 32);
}

static void async_wait(Ctx *c) {
    auto it = g_files.find((int)A0);
    if (it == g_files.end()) RET(SCE_ERROR_BAD_FD);
    if (A1) {
        W32(A1, (uint32_t)it->second.async_result);
        W32(A1 + 4, (uint32_t)(it->second.async_result >> 32));
    }
    if (it->second.closed) g_files.erase(it);
    RET(0);
}
HLE(sceIoWaitAsync) { async_wait(c); }
HLE(sceIoPollAsync) { async_wait(c); }
HLE(sceIoChangeAsyncPriority) { RET(0); }

static void fill_stat(uint8_t *s, bool dir, int64_t size) {
    memset(s, 0, 0x58);
    uint32_t mode = dir ? 0x11FF : 0x21FF;
    uint32_t attr = dir ? 0x10 : 0x20;
    memcpy(s + 0, &mode, 4);
    memcpy(s + 4, &attr, 4);
    memcpy(s + 8, &size, 8);
}

HLE(sceIoGetstat) {
    auto [dev, rest] = split_path(mem_str(A0));
    if (is_disc(dev) && iso_loaded()) {
        IsoEntry e;
        if (!iso_lookup(rest, &e)) RET(SCE_ERROR_FILE_NOT_FOUND);
        fill_stat(MEMP(A1), e.dir, e.size);
        RET(0);
    }
    struct stat st;
    if (stat(host_path(mem_str(A0)).c_str(), &st) != 0) RET(SCE_ERROR_FILE_NOT_FOUND);
    fill_stat(MEMP(A1), S_ISDIR(st.st_mode), st.st_size);
    RET(0);
}

HLE(sceIoMkdir) {
    std::string hp = host_path(mem_str(A0));
    mkdirs(hp + "/");
    RET(0);
}
HLE(sceIoRename) {
    std::string a = host_path(mem_str(A0)), b = host_path(mem_str(A1));
    RET(rename(a.c_str(), b.c_str()) == 0 ? 0 : SCE_ERROR_FILE_NOT_FOUND);
}

struct OpenDir {
    std::vector<std::pair<std::string, std::pair<bool, int64_t>>> entries;  // nombre, (dir, tamaño)
    size_t pos = 0;
};
static std::map<int, OpenDir> g_dirs;

HLE(sceIoDopen) {
    auto [dev, rest] = split_path(mem_str(A0));
    OpenDir od;
    if (is_disc(dev) && iso_loaded()) {
        IsoEntry e;
        if (!iso_lookup(rest, &e) || !e.dir) RET(SCE_ERROR_FILE_NOT_FOUND);
        for (const IsoEntry &x : iso_list(e)) od.entries.push_back({x.name, {x.dir, x.size}});
    } else {
        std::string hp = host_path(mem_str(A0));
        DIR *d = opendir(hp.c_str());
        if (!d) RET(SCE_ERROR_FILE_NOT_FOUND);
        while (dirent *e = readdir(d)) {
            struct stat st;
            stat((hp + "/" + e->d_name).c_str(), &st);
            od.entries.push_back({e->d_name, {S_ISDIR(st.st_mode), st.st_size}});
        }
        closedir(d);
    }
    int fd = g_next_fd++;
    g_dirs[fd] = std::move(od);
    RET(fd);
}
HLE(sceIoDread) {
    auto it = g_dirs.find((int)A0);
    if (it == g_dirs.end()) RET(SCE_ERROR_BAD_FD);
    OpenDir &d = it->second;
    if (d.pos >= d.entries.size()) RET(0);
    auto &[name, info] = d.entries[d.pos++];
    uint8_t *e = MEMP(A1);
    memset(e, 0, 0x58 + 256);
    fill_stat(e, info.first, info.second);
    strncpy((char *)e + 0x58, name.c_str(), 255);
    RET(1);
}
HLE(sceIoDclose) {
    g_dirs.erase((int)A0);
    RET(0);
}

HLE(sceIoDevctl) {
    std::string dev = mem_str(A0);
    LOGD("io", "devctl '%s' cmd=%08x", dev.c_str(), A1);
    if (A1 == 0x02425818 && T0) {  // espacio libre del memory stick
        uint32_t info = R32(T0);
        W32(info + 0, 0x10000);  // clusters máximos
        W32(info + 4, 0x10000);  // clusters libres
        W32(info + 8, 0x10000);
        W32(info + 12, 512);     // bytes por sector
        W32(info + 16, 64);      // sectores por cluster
    }
    if (A1 == 0x02025806 && T0) W32(T0, 1);  // memory stick insertado
    if (A1 == 0x02415821 && A2) callback_notify((int)R32(A2), 1);  // callback de inserción: ya insertado
    if (A1 == 0x02425823 && T0) W32(T0, 1);  // estado: insertado
    RET(0);
}
// ioctl de ficheros del UMD (fd, cmd, indata, inlen, outdata, outlen), como en PPSSPP
HLE(sceIoIoctl) {
    OpenFile *f = get_file((int)A0);
    if (!f) RET(SCE_ERROR_BAD_FD);
    uint32_t cmd = A1, in = A2, out = T0;
    uint64_t size = f->iso ? f->entry.size : 0;
    switch (cmd) {
    case 0x01020001: if (out) W32(out, 2048); RET(0);              // tamaño de sector
    case 0x01020004:                                               // posición actual
    case 0x01D20001: if (out) W32(out, (uint32_t)f->pos); RET(0);
    case 0x01020006: if (out) W32(out, f->iso ? f->entry.lbn : 0); RET(0);  // sector inicial
    case 0x01020007:                                               // tamaño del fichero (u64)
        if (out) { W32(out, (uint32_t)size); W32(out + 4, (uint32_t)(size >> 32)); }
        RET(0);
    case 0x01010005: {                                             // seek {u64 offset, u32, u32 whence}
        int64_t off = (int64_t)(((uint64_t)R32(in + 4) << 32) | R32(in));
        uint32_t whence = R32(in + 12);
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->pos : (int64_t)size;
        if (base + off < 0 || (uint64_t)(base + off) > size) RET(0x80010005);
        f->pos = (uint64_t)(base + off);
        RET(0);
    }
    case 0x01030008: RET(io_read((int)A0, out, R32(in)));         // lectura
    default:
        LOGW("io", "ioctl %08x sin implementar (fd %d)", cmd, (int)A0);
        RET(0);
    }
}

// ---------------------------------------------------------------- UMD
static int g_umd_cb = 0;
HLE(sceUmdCheckMedium) { RET(1); }
HLE(sceUmdActivate) {
    if (g_umd_cb) callback_notify(g_umd_cb, 0x32);
    RET(0);
}
HLE(sceUmdDeactivate) { RET(0); }
HLE(sceUmdGetDriveStat) { RET(0x32); }  // presente | listo | activo
HLE(sceUmdGetErrorStat) { RET(0); }
HLE(sceUmdWaitDriveStat) { RET(0); }
HLE(sceUmdWaitDriveStatCB) { RET(0); }
HLE(sceUmdRegisterUMDCallBack) {
    g_umd_cb = (int)A0;
    RET(0);
}
HLE(sceUmdUnRegisterUMDCallBack) {
    g_umd_cb = 0;
    RET(0);
}

// Despacho de imports: NID (stub generado) -> nombre (tabla generada) -> manejador registrado con HLE().
#include <set>
#include <string>
#include <unordered_map>

#include "threads.h"

static std::unordered_map<std::string, HleFunc> &by_name() {
    static std::unordered_map<std::string, HleFunc> m;
    return m;
}
static std::unordered_map<uint32_t, std::pair<const char *, HleFunc>> g_by_nid;

void hle_register(const char *name, HleFunc fn) { by_name()[name] = fn; }

void hle_init() {
    int missing = 0;
    for (uint32_t i = 0; i < g_recomp_import_count; i++) {
        const RecompImport &imp = g_recomp_imports[i];
        auto it = by_name().find(imp.name);
        g_by_nid[imp.nid] = {imp.name, it == by_name().end() ? nullptr : it->second};
        if (it == by_name().end()) {
            LOGW("hle", "import sin implementar: %s (%08x)", imp.name, imp.nid);
            missing++;
        }
    }
    LOGI("hle", "%u imports, %d sin implementar", g_recomp_import_count, missing);
}

extern "C" void hle_call(Ctx *c, uint32_t nid) {
    auto it = g_by_nid.find(nid);
    if (it == g_by_nid.end() || !it->second.second) {
        static std::set<uint32_t> warned;
        if (warned.insert(nid).second)
            LOGW("hle", "llamada sin implementar: %s", it == g_by_nid.end() ? "?" : it->second.first);
        c->r[2] = 0;
        return;
    }
    LOGT("hle", "[%d] %s(%08x, %08x, %08x, %08x) ra=%08x", thread_current() ? thread_current()->id : 0,
         it->second.first, c->r[4], c->r[5], c->r[6], c->r[7], c->r[31]);
    uint32_t sp = c->r[29];
    it->second.second(c);
    if (c->r[29] != sp) fatal("%s ha cambiado sp: %08x -> %08x", it->second.first, sp, c->r[29]);
    if ((sp & 0x1FFFFFFF) < 0x08800000 || (sp & 0x1FFFFFFF) >= 0x0A000000)
        fatal("sp fuera de la memoria de usuario (%08x) al llamar a %s", sp, it->second.first);
}

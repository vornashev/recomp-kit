// Symbol loading and lazy indexes with a synthetic image; no game required.
#include "../mods_internal.h"
#include "../../platform/os.h"
#include <cstdio>
#include <string>
#include <cstring>

static int failures = 0;
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #e);                                   \
        }                                                                                          \
    } while (0)
static std::string resource;
std::string host_resource(const char *) {
    return resource;
}
const char *loader_exe_sha256() {
    return "synthetic-image";
}

static void put(const char *text) {
    FILE *f = fopen(resource.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

int main() {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/recomp-symbols-XXXXXX", os_temp_dir());
    CHECK(os_mkdtemp(dir) == 0);
    resource = std::string(dir) + "/symbols.json";
    put(R"({"exe_sha256":"synthetic-image","functions":[
        {"addr":"00401020","name":"later","kind":"continuation","hookable":false},
        {"addr":"00401000","name":"first","kind":"entry","hookable":true,
         "aliases":["alias","later","global"]}],
        "globals":[{"name":"global","addr":"00402000","size":4,"stride":8,"count":2}],
        "events":{"on_frame":"00401000"}})");
    CHECK(mods_symbols_load(nullptr));
    CHECK(mods_symbols_count() == 2);
    // Address lookup must work before the name index has ever been built.
    CHECK(mods_symbol_hookable(0x00401000));
    CHECK(!mods_symbol_hookable(0x00401020));
    CHECK(!mods_symbol_hookable(0x00401001));
    CHECK(strcmp(mods_symbol_kind(0x00401020), "continuation") == 0);
    CHECK(strcmp(mods_symbol_kind(0), "") == 0);
    CHECK(mods_symbol_global("global") == 0x00402000);
    CHECK(mods_symbol_global_stride("global") == 8);
    CHECK(mods_symbol_global_count("global") == 2);
    CHECK(mods_symbol_event("on_frame") == 0x00401000);
    uint32_t addr = 0, count = 0, matches[3]{};
    CHECK(mods_symbol("later", &addr) == POP_OK && addr == 0x00401020);
    CHECK(mods_symbol("alias", &addr) == POP_OK && addr == 0x00401000);
    CHECK(mods_symbol("global", &addr) == POP_OK && addr == 0x00401000);
    CHECK(mods_symbols_matching("", matches, 3, &count) == POP_OK);
    CHECK(count == 2 && matches[0] == 0x00401000 && matches[1] == 0x00401020);
    put(R"({"exe_sha256":"wrong","functions":[{"addr":"00403000","name":"bad"}]})");
    CHECK(!mods_symbols_load(nullptr));
    CHECK(mods_symbol("alias", &addr) == POP_OK); // rejected load preserves old namespace
    put(R"({"exe_sha256":"synthetic-image","functions":[
        {"addr":"00403000","name":"new","kind":"entry","hookable":true}]})");
    CHECK(mods_symbols_load(nullptr));
    CHECK(!mods_symbol_hookable(0x00401000));
    CHECK(mods_symbol("alias", &addr) == POP_E_NOSYMBOL);
    CHECK(mods_symbol("new", &addr) == POP_OK && addr == 0x00403000);
    CHECK(mods_symbol_event("on_frame") == 0);
    remove(resource.c_str());
    printf("symbols tests: %d failures\n", failures);
    return failures ? 1 : 0;
}

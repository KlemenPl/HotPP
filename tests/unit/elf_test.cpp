#include "host/compile_db.h"
#include "host/image/elf_image.h"
#include "host/link/symbol_index.h"
#include "host/object/elf_object.h"
#include "unit/test.h"

#include <algorithm>

using namespace hotpp;

static volatile int local_marker = 7;
int hotpp_elf_test_marker() { return local_marker; }

HOTPP_TEST(elf_image_reads_own_symbols_and_units) {
    const auto img = image::ElfImage::load("/proc/self/exe");
    const auto& syms = img->symbols();
    const auto marker = std::find_if(syms.begin(), syms.end(),
                                     [](const image::ImageSymbol& s) { return s.name == "_Z21hotpp_elf_test_markerv"; });
    CHECK(marker != syms.end());
    CHECK(marker != syms.end() && marker->type == elf::STT_FUNC);
    const auto local = std::find_if(syms.begin(), syms.end(),
                                    [](const image::ImageSymbol& s) { return s.name == "_ZL12local_marker"; });
    CHECK(local != syms.end());
    CHECK(local != syms.end() && local->file == "elf_test.cpp");
    CHECK(!img->build_id().empty());

    // DWARF compile units name this file (only in builds with debug info).
    if (!img->compile_units().empty()) {
        const auto self = normalize_path(HOTPP_TEST_SOURCE_DIR "/unit/elf_test.cpp");
        bool found = false;
        for (const auto& cu : img->compile_units()) found |= normalize_path(cu) == self;
        CHECK(found);
    }
}

HOTPP_TEST(elf_object_loads_patch_object) {
    const auto obj = object::ElfObject::load(HOTPP_TEST_LINK_OBJECT);
    CHECK(obj->source_name == "link_sample.cpp");
    bool update_section = false, ticks_section = false, group_free_text = true;
    for (const auto& s : obj->sections) {
        if (s.name == ".text._Z6updatev") update_section = !s.relocs.empty();
        if (s.name == ".bss._ZL5ticks") ticks_section = s.type == elf::SHT_NOBITS;
        if (s.name.rfind(".text.", 0) == 0 && s.group >= 0) group_free_text = false;
    }
    CHECK(update_section);
    CHECK(ticks_section);
    CHECK(group_free_text); // no inline functions in the sample, so no COMDATs
}

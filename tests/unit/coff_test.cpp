// COFF objects, PE images and PDBs (Windows builds): the fixture coff_sample.cpp is
// built like a hot-reloadable program, and the test reads its object, image and PDB.
#include "host/image/codeview.h"
#include "host/image/pe_image.h"
#include "host/object/elf_object.h"
#include "unit/test.h"

#include <algorithm>

using namespace hotpp;

namespace {
const image::ImageSymbol* image_symbol(const image::PeImage& img, const std::string& name) {
    for (const image::ImageSymbol& s : img.symbols())
        if (s.name == name) return &s;
    return nullptr;
}
} // namespace

HOTPP_TEST(coff_object_and_pdb_agree_on_statics) {
    const auto obj = object::load_object(HOTPP_TEST_COFF_OBJECT);
    CHECK(obj->format == object::Format::Coff);
    CHECK(obj->source_name == "coff_sample.cpp");
    const auto img = image::PeImage::load(HOTPP_TEST_COFF_IMAGE);
    CHECK(img->debug_warning().empty());
    CHECK(img->build_id().size() == 20);

    // Every static of the object (renamed to its bridge key) is found in the PDB,
    // the two overloads of over() as two different symbols.
    size_t statics = 0, overloads = 0;
    std::vector<uint64_t> over_rvas;
    for (const object::ObjSymbol& s : obj->symbols) {
        if (!codeview::is_bridge_key(s.name)) continue;
        ++statics;
        const image::ImageSymbol* found = image_symbol(*img, s.name);
        CHECK(found != nullptr);
        if (!found) continue;
        CHECK(found->file == "coff_sample.cpp");
        if (codeview::bridge_display(s.name) == "over()") {
            ++overloads;
            over_rvas.push_back(found->value);
        }
    }
    CHECK(statics >= 7); // counter, hidden, helper, over x2, origin, calls
    CHECK(overloads == 2);
    CHECK(over_rvas.size() == 2 && over_rvas[0] != over_rvas[1]);

    // Externals under their decorated names.
    const image::ImageSymbol* exported = image_symbol(*img, "?exported@@YAHH@Z");
    CHECK(exported && exported->type == elf::STT_FUNC && exported->size > 0);
    const image::ImageSymbol* global = image_symbol(*img, "?global_value@@3HA");
    CHECK(global && global->type == elf::STT_OBJECT && global->size == 4);
    CHECK(img->section_writable(global ? global->shndx : 0));

    // Functions have /FUNCTIONPADMIN padding before them.
    uint8_t pad[5] = {};
    CHECK(exported && img->read(exported->value - 5, 5, pad));
    CHECK(std::all_of(pad, pad + 5, [](uint8_t b) { return b == 0xCC || b == 0x90; }));

    // The compile unit and the struct layouts.
    CHECK(std::any_of(img->compile_units().begin(), img->compile_units().end(),
                      [](const std::filesystem::path& p) { return p.filename() == "coff_sample.cpp"; }));
    const auto layouts = img->type_layouts();
    CHECK(layouts.count("Point") && layouts.at("Point").size == 16 && layouts.at("Point").members.size() == 3);
    CHECK(layouts.count("Derived") && layouts.at("Derived").members.size() == 2);
}

HOTPP_TEST(coff_object_comdats_and_relocations) {
    const auto obj = object::load_object(HOTPP_TEST_COFF_OBJECT);
    // Each function (/Gy) is a COMDAT with its unwind data as associative sections.
    bool exported_group = false;
    for (const object::ObjGroup& g : obj->groups)
        if (g.signature == "?exported@@YAHH@Z") {
            exported_group = g.members.size() >= 2;
            for (uint32_t m : g.members) CHECK(obj->sections[m].group >= 0);
        }
    CHECK(exported_group);
    // Relocations were translated and point at real symbols.
    size_t relocs = 0;
    for (const object::ObjSection& s : obj->sections)
        for (const elf::Rela& r : s.relocs) {
            ++relocs;
            CHECK(r.sym() < obj->symbols.size());
        }
    CHECK(relocs > 10);
}

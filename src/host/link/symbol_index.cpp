#include "host/link/symbol_index.h"

#include "host/image/elf_image.h"

namespace hotpp::link {

using namespace hotpp::elf;

void SymbolIndex::add_image(const image::ElfImage& image, uint64_t base) {
    for (const image::ImageSymbol& s : image.symbols()) {
        IndexEntry e;
        switch (s.type) {
        case STT_FUNC: case STT_GNU_IFUNC: e.kind = SymbolKind::Function; break;
        case STT_OBJECT: case STT_COMMON: e.kind = SymbolKind::Data; break;
        case STT_TLS: e.kind = SymbolKind::Tls; break;
        default: e.kind = SymbolKind::Other; break;
        }
        // TLS symbol values are offsets into the TLS block, not addresses.
        e.address = s.shndx == SHN_ABS || e.kind == SymbolKind::Tls ? s.value : base + s.value;
        e.size = s.size;
        e.writable = image.section_writable(s.shndx);
        if (e.kind == SymbolKind::Function) {
            uint8_t probe[kSiteProbeBytes];
            FunctionVersion v;
            v.entry = e.address;
            if (s.value >= kPadBytes && image.read(s.value - kPadBytes, sizeof probe, probe))
                v.site = analyze_patch_site(probe, sizeof probe);
            e.versions.push_back(v);
        }

        add({s.bind == STB_LOCAL, s.bind == STB_LOCAL ? s.file : std::string(), s.name}, e);
    }
}

void SymbolIndex::add(const SymbolKey& key, const IndexEntry& entry) {
    auto [it, inserted] = entries_.emplace(key.str(), entry);
    if (!inserted && key.local) it->second.ambiguous = true;
}

const IndexEntry* SymbolIndex::find(const SymbolKey& key) const {
    auto it = entries_.find(key.str());
    return it == entries_.end() ? nullptr : &it->second;
}

void SymbolIndex::apply(const IndexDelta& delta) {
    for (const IndexDelta::NewVersion& v : delta.versions) {
        auto it = entries_.find(v.key.str());
        if (it != entries_.end()) it->second.versions.push_back(v.version);
    }
    for (const IndexDelta::NewSymbol& s : delta.symbols) entries_.emplace(s.key.str(), s.entry);
}

} // namespace hotpp::link

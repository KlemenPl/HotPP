// CodeView debug records, as found in object files (.debug$S, .debug$T) and in PDBs
// (module symbol streams, TPI and IPI): type records, their sizes and layouts, and
// the procedure and data symbols HotPP matches between a patch object and the
// running program (docs/DESIGN.md §6, Windows). Malformed input throws CvError.
#pragma once

#include "host/image/dwarf_types.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace hotpp::codeview {

struct CvError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// clang-format off
constexpr uint16_t S_END = 0x0006, S_BLOCK32 = 0x1103, S_THUNK32 = 0x1102, S_SEPCODE = 0x1132,
                   S_LDATA32 = 0x110C, S_GDATA32 = 0x110D, S_PUB32 = 0x110E, S_LPROC32 = 0x110F,
                   S_GPROC32 = 0x1110, S_LTHREAD32 = 0x1112, S_GTHREAD32 = 0x1113,
                   S_LPROC32_ID = 0x1146, S_GPROC32_ID = 0x1147, S_PROC_ID_END = 0x114F,
                   S_BUILDINFO = 0x114C, S_INLINESITE = 0x114D, S_INLINESITE_END = 0x114E,
                   S_LPROC32_DPC = 0x1155, S_LPROC32_DPC_ID = 0x1156;
constexpr uint16_t LF_MODIFIER = 0x1001, LF_POINTER = 0x1002, LF_PROCEDURE = 0x1008,
                   LF_MFUNCTION = 0x1009, LF_ARGLIST = 0x1201, LF_FIELDLIST = 0x1203,
                   LF_BITFIELD = 0x1205, LF_BCLASS = 0x1400, LF_VBCLASS = 0x1401, LF_IVBCLASS = 0x1402,
                   LF_INDEX = 0x1404, LF_VFUNCTAB = 0x1409, LF_ENUMERATE = 0x1502, LF_ARRAY = 0x1503,
                   LF_CLASS = 0x1504, LF_STRUCTURE = 0x1505, LF_UNION = 0x1506, LF_ENUM = 0x1507,
                   LF_MEMBER = 0x150D, LF_STMEMBER = 0x150E, LF_METHOD = 0x150F, LF_NESTTYPE = 0x1510,
                   LF_ONEMETHOD = 0x1511, LF_INTERFACE = 0x1519,
                   LF_FUNC_ID = 0x1601, LF_MFUNC_ID = 0x1602, LF_BUILDINFO = 0x1603,
                   LF_SUBSTR_LIST = 0x1604, LF_STRING_ID = 0x1605;
constexpr uint32_t kDebugSSymbols = 0xF1;  // .debug$S subsection holding symbol records
constexpr uint32_t kSignatureC13 = 4;      // first word of .debug$S, .debug$T and module streams
// clang-format on

// Type (or id) records indexed from 0x1000, pointing into data that must outlive it.
class TypeTable {
public:
    TypeTable() = default;
    // `data` is a sequence of records {u16 length, u16 kind, ...}.
    static TypeTable parse(const uint8_t* data, size_t size, uint32_t first_index = 0x1000);

    bool has(uint32_t index) const { return index >= first_ && index - first_ < records_.size(); }
    // Kind and contents (after the kind) of a record; throws if `index` is unknown.
    uint16_t kind(uint32_t index) const;
    const uint8_t* data(uint32_t index, size_t& size) const;
    size_t size() const { return records_.size(); }
    uint32_t first() const { return first_; }

private:
    struct Rec {
        const uint8_t* p;
        uint16_t kind;
        uint16_t size; // after the kind
    };
    uint32_t first_ = 0x1000;
    std::vector<Rec> records_;
};

// Identifies a type by its structure: two type graphs (an object's .debug$T and a
// PDB's TPI) give the same fingerprint for the same type. Named records (classes,
// enums) count by unique name. `ids` resolves LF_FUNC_ID / LF_MFUNC_ID indices.
std::string fingerprint(const TypeTable& types, uint32_t index, const TypeTable* ids = nullptr);
// Size in bytes of a type (0 if unknown).
uint64_t type_size(const TypeTable& types, uint32_t index);
// Struct, class and union layouts by qualified name (as dwarf_type_layouts).
std::unordered_map<std::string, image::TypeLayout> type_layouts(const TypeTable& types);

// A procedure or data symbol, with the key that identifies it across an object file
// and the PDB: name, enclosing function and type fingerprint (statics are
// undecorated in PDBs, decorated in objects).
struct NamedSymbol {
    uint16_t kind = 0;
    bool procedure = false, local = false, tls = false;
    std::string name;      // as recorded ("ns::f", "`anonymous namespace'::helper")
    std::string key;       // bridge key (see bridge_key)
    uint32_t type = 0;     // type index (procedures: of the function type)
    uint32_t offset = 0;   // section offset (PDB: in its image section)
    uint16_t segment = 0;  // image section number (PDB)
    uint32_t length = 0;   // procedures: code size
    size_t offset_field = 0; // position of the offset field within the records (objects: its relocation)
};
// The procedure and data symbols of a run of symbol records. `base` is added to
// offset_field. Nested statics get their function in their key.
void collect_symbols(const uint8_t* records, size_t size, size_t base, const TypeTable& types, const TypeTable* ids,
                     std::vector<NamedSymbol>& out);

// Marks names that are bridge keys rather than linker names.
constexpr char kBridgeMark = '\x1e';
bool is_bridge_key(const std::string& name);
// The readable part of a bridge key (the name, with its function).
std::string bridge_display(const std::string& key);

// LF_BUILDINFO: the working directory and source file of a compiland; empty if
// absent. `ids` is the id table (IPI, or an object's .debug$T).
struct BuildInfo {
    std::string directory, source;
};
BuildInfo build_info(const TypeTable& ids, uint32_t index);

} // namespace hotpp::codeview

#include "host/image/pe_image.h"

#include "host/elf/elf_file.h"
#include "host/image/codeview.h"

#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

namespace hotpp::image {

namespace fs = std::filesystem;
using namespace hotpp::elf;

namespace {

std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Bytes {
    const uint8_t* p;
    size_t n;
    std::string what;
    template <class T>
    T at(size_t off) const {
        if (off > n || sizeof(T) > n - off) throw std::runtime_error(what + ": truncated");
        T v;
        std::memcpy(&v, p + off, sizeof v);
        return v;
    }
    std::string cstr(size_t off) const {
        if (off >= n) throw std::runtime_error(what + ": truncated");
        const size_t len = strnlen(reinterpret_cast<const char*>(p + off), n - off);
        return std::string(reinterpret_cast<const char*>(p + off), len);
    }
};

// A PDB: an MSF container of numbered streams.
class Msf {
public:
    explicit Msf(std::vector<uint8_t> data, std::string name) : d_(std::move(data)), name_(std::move(name)) {
        static const char kMagic[] = "Microsoft C/C++ MSF 7.00\r\n\x1a" "DS\0\0\0";
        if (d_.size() < 56 || std::memcmp(d_.data(), kMagic, 32) != 0) throw std::runtime_error(name_ + ": not a PDB (MSF 7.00)");
        const Bytes b{d_.data(), d_.size(), name_};
        block_ = b.at<uint32_t>(32);
        const uint32_t nblocks = b.at<uint32_t>(40), dir_bytes = b.at<uint32_t>(44), map_block = b.at<uint32_t>(52);
        if (block_ < 512 || block_ > 65536 || (block_ & (block_ - 1)) || uint64_t(nblocks) * block_ > d_.size() + block_)
            throw std::runtime_error(name_ + ": bad MSF header");
        const uint32_t dir_blocks = (dir_bytes + block_ - 1) / block_;
        std::vector<uint32_t> dir_list(dir_blocks);
        for (uint32_t i = 0; i < dir_blocks; ++i) dir_list[i] = b.at<uint32_t>(size_t(map_block) * block_ + i * 4);
        const std::vector<uint8_t> dir = gather(dir_list, dir_bytes);
        const Bytes db{dir.data(), dir.size(), name_ + " (stream directory)"};
        const uint32_t nstreams = db.at<uint32_t>(0);
        if (nstreams > 1u << 20) throw std::runtime_error(name_ + ": too many streams");
        size_t pos = 4 + size_t(nstreams) * 4;
        streams_.resize(nstreams);
        for (uint32_t s = 0; s < nstreams; ++s) {
            const uint32_t size = db.at<uint32_t>(4 + size_t(s) * 4);
            Stream& st = streams_[s];
            if (size == 0xFFFFFFFF) continue;
            st.size = size;
            const uint32_t count = (size + block_ - 1) / block_;
            for (uint32_t k = 0; k < count; ++k, pos += 4) st.blocks.push_back(db.at<uint32_t>(pos));
        }
    }

    size_t stream_count() const { return streams_.size(); }
    std::vector<uint8_t> stream(uint32_t index) const {
        if (index >= streams_.size()) throw std::runtime_error(name_ + ": no stream " + std::to_string(index));
        return gather(streams_[index].blocks, streams_[index].size);
    }

private:
    struct Stream {
        uint32_t size = 0;
        std::vector<uint32_t> blocks;
    };
    std::vector<uint8_t> gather(const std::vector<uint32_t>& blocks, uint32_t size) const {
        std::vector<uint8_t> out;
        out.reserve(size);
        for (uint32_t blk : blocks) {
            const uint64_t at = uint64_t(blk) * block_;
            if (at >= d_.size()) throw std::runtime_error(name_ + ": block outside the file");
            const size_t take = std::min<size_t>(block_, size - out.size());
            if (take > d_.size() - at) throw std::runtime_error(name_ + ": block outside the file");
            out.insert(out.end(), d_.begin() + ptrdiff_t(at), d_.begin() + ptrdiff_t(at + take));
            if (out.size() == size) break;
        }
        if (out.size() != size) throw std::runtime_error(name_ + ": short stream");
        return out;
    }

    std::vector<uint8_t> d_;
    std::string name_;
    uint32_t block_ = 4096;
    std::vector<Stream> streams_;
};

// The type records of a TPI or IPI stream (they start after its header).
codeview::TypeTable type_stream(const std::vector<uint8_t>& s, const std::string& what) {
    if (s.empty()) return {};
    const Bytes b{s.data(), s.size(), what};
    const uint32_t header = b.at<uint32_t>(4), first = b.at<uint32_t>(8), bytes = b.at<uint32_t>(16);
    if (header > s.size() || bytes > s.size() - header) throw std::runtime_error(what + ": bad header");
    return codeview::TypeTable::parse(s.data() + header, bytes, first);
}

std::string basename(const std::string& p) {
    const size_t sep = p.find_last_of("/\\");
    return sep == std::string::npos ? p : p.substr(sep + 1);
}

constexpr uint32_t kScnWrite = 0x80000000;

} // namespace

std::unique_ptr<PeImage> PeImage::load(const fs::path& path, const fs::path& pdb) {
    auto img = std::unique_ptr<PeImage>(new PeImage);
    img->file_ = read_file(path);
    const Bytes b{img->file_.data(), img->file_.size(), path.string()};
    if (b.at<uint16_t>(0) != 0x5A4D) throw std::runtime_error(path.string() + ": not a PE image");
    const uint32_t nt = b.at<uint32_t>(0x3C);
    if (b.at<uint32_t>(nt) != 0x00004550) throw std::runtime_error(path.string() + ": not a PE image");
    if (b.at<uint16_t>(nt + 4) != 0x8664) throw std::runtime_error(path.string() + ": not an x86-64 image");
    const uint16_t nsections = b.at<uint16_t>(nt + 6);
    const uint16_t opt_size = b.at<uint16_t>(nt + 20);
    const size_t opt = nt + 24;
    if (b.at<uint16_t>(opt) != 0x20B) throw std::runtime_error(path.string() + ": not a PE32+ image");
    img->image_base_ = b.at<uint64_t>(opt + 24);
    const uint32_t ndirs = b.at<uint32_t>(opt + 108);
    auto dir = [&](uint32_t i, uint32_t& rva, uint32_t& size) {
        rva = size = 0;
        if (i < ndirs) {
            rva = b.at<uint32_t>(opt + 112 + i * 8);
            size = b.at<uint32_t>(opt + 116 + i * 8);
        }
    };
    const size_t sec_table = opt + opt_size;
    for (uint16_t i = 0; i < nsections; ++i) {
        const size_t h = sec_table + size_t(i) * 40;
        Section s;
        s.name = std::string(reinterpret_cast<const char*>(b.p + h), strnlen(reinterpret_cast<const char*>(b.p + h), 8));
        s.virtual_size = b.at<uint32_t>(h + 8);
        s.rva = b.at<uint32_t>(h + 12);
        s.raw_size = b.at<uint32_t>(h + 16);
        s.raw_offset = b.at<uint32_t>(h + 20);
        s.characteristics = b.at<uint32_t>(h + 36);
        img->sections_.push_back(s);
    }

    // CodeView record: the build's identity and where its PDB was written.
    uint32_t dbg_rva, dbg_size;
    dir(6, dbg_rva, dbg_size);
    for (uint32_t off = 0; off + 28 <= dbg_size; off += 28) {
        uint8_t entry[28];
        if (!img->read(dbg_rva + off, sizeof entry, entry)) break;
        uint32_t type, size, data_rva;
        std::memcpy(&type, entry + 12, 4);
        std::memcpy(&size, entry + 16, 4);
        std::memcpy(&data_rva, entry + 20, 4);
        if (type != 2 || size < 24 || size > 4096) continue; // IMAGE_DEBUG_TYPE_CODEVIEW
        std::vector<uint8_t> cv(size);
        if (!img->read(data_rva, size, cv.data()) || std::memcmp(cv.data(), "RSDS", 4) != 0) continue;
        img->build_id_.assign(cv.begin() + 4, cv.begin() + 24);
        img->pdb_from_image_ = std::string(reinterpret_cast<const char*>(cv.data() + 24), strnlen(reinterpret_cast<const char*>(cv.data() + 24), size - 24));
        break;
    }
    // TLS: variables' offsets are from the start of the template.
    uint32_t tls_rva, tls_size;
    dir(9, tls_rva, tls_size);
    if (tls_rva && tls_size >= 40) {
        uint8_t t[40];
        if (img->read(tls_rva, sizeof t, t)) {
            uint64_t start_va, end_va;
            uint32_t zero_fill;
            std::memcpy(&start_va, t, 8);
            std::memcpy(&end_va, t + 8, 8);
            std::memcpy(&zero_fill, t + 32, 4);
            img->tls_start_rva_ = uint32_t(start_va - img->image_base_);
            img->tls_end_rva_ = uint32_t(end_va - img->image_base_) + zero_fill;
        }
    }

    fs::path pdb_path = pdb;
    if (pdb_path.empty()) {
        std::error_code ec;
        const fs::path recorded = img->pdb_from_image_;
        if (!img->pdb_from_image_.empty() && fs::exists(recorded, ec)) {
            pdb_path = recorded;
        } else {
            pdb_path = path.parent_path() / (img->pdb_from_image_.empty() ? path.stem().string() + ".pdb"
                                                                         : basename(img->pdb_from_image_));
        }
    }
    try {
        img->read_pdb(pdb_path);
    } catch (const std::exception& e) {
        img->warning_ = e.what();
        img->symbols_.clear();
        img->compile_units_.clear();
    }
    return img;
}

uint64_t PeImage::rva_of(uint16_t segment, uint32_t offset) const {
    if (segment == 0 || segment > sections_.size()) return 0;
    return uint64_t(sections_[segment - 1].rva) + offset;
}

void PeImage::read_pdb(const fs::path& path) {
    pdb_path_ = path;
    const Msf msf(read_file(path), path.string());

    // Info stream: the identity must match the image's.
    const std::vector<uint8_t> info = msf.stream(1);
    const Bytes ib{info.data(), info.size(), path.string()};
    if (!build_id_.empty()) {
        std::vector<uint8_t> id(info.begin() + 12, info.begin() + 28);
        const uint32_t age = ib.at<uint32_t>(8);
        // The image records the age the PDB had when it was linked; a PDB can only
        // have been updated since (age at least as high).
        uint32_t image_age;
        std::memcpy(&image_age, build_id_.data() + 16, 4);
        if (!std::equal(id.begin(), id.end(), build_id_.begin()) || age < image_age)
            throw std::runtime_error(path.string() + " doesn't match the image (rebuilt separately?)");
    }

    const std::vector<uint8_t> tpi_s = msf.stream(2);
    const codeview::TypeTable tpi = type_stream(tpi_s, path.string() + " (TPI)");
    std::vector<uint8_t> ipi_s;
    if (msf.stream_count() > 4) ipi_s = msf.stream(4);
    const codeview::TypeTable ipi = type_stream(ipi_s, path.string() + " (IPI)");
    try {
        layouts_ = codeview::type_layouts(tpi);
    } catch (const std::exception&) {
        layouts_.clear();
    }

    const std::vector<uint8_t> dbi = msf.stream(3);
    const Bytes db{dbi.data(), dbi.size(), path.string() + " (DBI)"};
    const uint16_t symrec_stream = db.at<uint16_t>(20);
    const int32_t modinfo_size = db.at<int32_t>(24);
    if (modinfo_size < 0 || size_t(modinfo_size) > dbi.size() - 64) throw std::runtime_error(path.string() + ": bad DBI header");

    // Module streams: statics (local symbols under bridge keys), function sizes,
    // source files.
    std::map<uint64_t, uint32_t> proc_length; // RVA -> length, for publics
    std::map<uint64_t, uint64_t> data_size;   // RVA -> size, for publics
    for (size_t pos = 64; pos + 64 <= size_t(64 + modinfo_size);) {
        const uint16_t stream = db.at<uint16_t>(pos + 34);
        const uint32_t sym_bytes = db.at<uint32_t>(pos + 36);
        const std::string module_name = db.cstr(pos + 64);
        const std::string obj_name = db.cstr(pos + 64 + module_name.size() + 1);
        pos = (pos + 64 + module_name.size() + 1 + obj_name.size() + 1 + 3) & ~size_t(3);
        if (stream == 0xFFFF || sym_bytes < 4) continue;
        const std::vector<uint8_t> mod = msf.stream(stream);
        if (sym_bytes > mod.size()) continue;
        const Bytes mb{mod.data(), sym_bytes, path.string() + " (module " + module_name + ")"};

        // The compiland's source, from its S_BUILDINFO record.
        std::string source;
        for (size_t r = 4; r + 4 <= sym_bytes;) {
            const uint16_t len = mb.at<uint16_t>(r), kind = mb.at<uint16_t>(r + 2);
            if (len < 2) break;
            if (kind == codeview::S_BUILDINFO && len >= 6) {
                const codeview::BuildInfo bi = codeview::build_info(ipi, mb.at<uint32_t>(r + 4));
                if (!bi.source.empty()) {
                    fs::path src = bi.source;
                    // A path from another system (a cross build) isn't absolute here.
                    const bool rooted = !bi.source.empty() && (bi.source[0] == '/' || bi.source[0] == '\\');
                    if (!src.is_absolute() && !rooted && !bi.directory.empty()) src = fs::path(bi.directory) / src;
                    source = src.string();
                    compile_units_.push_back(src);
                }
                break;
            }
            r += 2 + size_t(len);
        }

        std::vector<codeview::NamedSymbol> named;
        try {
            codeview::collect_symbols(mod.data() + 4, sym_bytes - 4, 4, tpi, &ipi, named);
        } catch (const codeview::CvError&) {
            continue;
        }
        const std::string file = basename(source.empty() ? obj_name : source);
        for (const codeview::NamedSymbol& n : named) {
            const uint64_t rva = rva_of(n.segment, n.offset);
            if (!rva) continue;
            if (n.procedure) proc_length[rva] = n.length;
            else data_size[rva] = codeview::type_size(tpi, n.type);
            if (!n.local) continue;
            ImageSymbol s;
            s.name = n.key;
            s.file = file;
            s.bind = STB_LOCAL;
            s.shndx = n.segment;
            if (n.procedure) {
                s.type = STT_FUNC;
                s.value = rva;
                s.size = n.length;
            } else if (n.tls) {
                s.type = STT_TLS;
                // Within the TLS template: the offset from its start. Otherwise (MSVC's
                // records of static thread_locals) the record's offset already is one.
                s.value = tls_start_rva_ && rva >= tls_start_rva_ && rva < tls_end_rva_ ? rva - tls_start_rva_ : n.offset;
                s.size = codeview::type_size(tpi, n.type);
            } else {
                s.type = STT_OBJECT;
                s.value = rva;
                s.size = codeview::type_size(tpi, n.type);
            }
            symbols_.push_back(std::move(s));
        }
    }

    // Publics: external symbols under their decorated names. Global data records
    // in the same stream give their sizes.
    const std::vector<uint8_t> rec = msf.stream(symrec_stream);
    const Bytes rb{rec.data(), rec.size(), path.string() + " (symbol records)"};
    for (size_t r = 0; r + 4 <= rec.size();) {
        const uint16_t len = rb.at<uint16_t>(r), kind = rb.at<uint16_t>(r + 2);
        if (len < 2) break;
        if ((kind == codeview::S_GDATA32 || kind == codeview::S_GTHREAD32) && len >= 12) {
            const uint64_t rva = rva_of(rb.at<uint16_t>(r + 12), rb.at<uint32_t>(r + 8));
            if (rva && !data_size.count(rva)) data_size[rva] = codeview::type_size(tpi, rb.at<uint32_t>(r + 4));
        }
        r += 2 + size_t(len);
    }
    for (size_t r = 0; r + 4 <= rec.size();) {
        const uint16_t len = rb.at<uint16_t>(r), kind = rb.at<uint16_t>(r + 2);
        if (len < 2) break;
        if (kind == codeview::S_PUB32 && len >= 12) {
            const uint32_t flags = rb.at<uint32_t>(r + 4), offset = rb.at<uint32_t>(r + 8);
            const uint16_t segment = rb.at<uint16_t>(r + 12);
            const uint64_t rva = rva_of(segment, offset);
            if (rva) {
                ImageSymbol s;
                s.name = rb.cstr(r + 14);
                s.bind = STB_GLOBAL;
                s.shndx = segment;
                // thread_locals live in the TLS template (the TLS directory's range).
                const bool tls = tls_start_rva_ && rva >= tls_start_rva_ && rva < tls_end_rva_;
                if (flags & 2) { // cvpsfFunction
                    s.type = STT_FUNC;
                    s.value = rva;
                    auto it = proc_length.find(rva);
                    if (it != proc_length.end()) s.size = it->second;
                } else if (tls) {
                    s.type = STT_TLS;
                    s.value = tls_start_rva_ && rva >= tls_start_rva_ ? rva - tls_start_rva_ : rva;
                    auto it = data_size.find(rva);
                    if (it != data_size.end()) s.size = it->second;
                } else {
                    s.type = (flags & 1) ? STT_FUNC : STT_OBJECT; // cvpsfCode: code labels (thunks)
                    s.value = rva;
                    auto it = data_size.find(rva);
                    if (it != data_size.end()) s.size = it->second;
                }
                symbols_.push_back(std::move(s));
            }
        }
        r += 2 + size_t(len);
    }
    // What MSVC code refers to as the image base (jump tables, RVAs).
    ImageSymbol base;
    base.name = "__ImageBase";
    base.bind = STB_GLOBAL;
    base.type = STT_OBJECT;
    base.shndx = 1;
    symbols_.push_back(base);
}

bool PeImage::section_writable(uint32_t shndx) const {
    return shndx >= 1 && shndx <= sections_.size() && (sections_[shndx - 1].characteristics & kScnWrite);
}

bool PeImage::read(uint64_t rva, size_t n, uint8_t* out) const {
    for (const Section& s : sections_) {
        if (rva < s.rva || n > s.raw_size || rva - s.rva > s.raw_size - n) continue;
        const uint64_t at = uint64_t(s.raw_offset) + (rva - s.rva);
        if (at > file_.size() || n > file_.size() - at) return false;
        std::memcpy(out, file_.data() + at, n);
        return true;
    }
    return false;
}

} // namespace hotpp::image

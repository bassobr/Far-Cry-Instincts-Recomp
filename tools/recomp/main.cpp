// fci-recomp: translates the x86 code of Far Cry Instincts' default.xbe into C++.
//   fci-recomp <default.xbe> <output dir> [config dir]
// The config dir holds symbols.txt (XDK symbols, tools/symbols.py), symbols_fci.txt (found by hand) and
// hle.txt (the symbols host code replaces, one name per line).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "analysis.h"
#include "codegen.h"
#include "hooks.h"
#include "semtest.h"
#include "xbe/xbe.h"

namespace {

// number of generated source files; must match FCI_PARTITIONS in CMakeLists.txt
constexpr int kPartitions = 128;

// sections of default.xbe holding code; the rest is data
const std::vector<std::string> kCodeSections = {
    ".text", "D3D", "XNET", "XONLINE", "D3DX", "XGRPH", "DSOUND", "WMADEC", "XPP",
    "BINK", "BINK32", "BINK32A", "BINK16", "BINK4444", "BINK5551", "BINKYUY2",
    "BINK16MX", "BINK16X2", "BINK16M", "BINK32MX", "BINK32X2", "BINK32M",
};

// "fun Name = 0x1234" / "var Name = 0x1234", # comments
bool LoadSymbols(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string kind, name, eq, value;
        if (!(ls >> kind >> name >> eq >> value) || eq != "=") continue;
        uint32_t address = uint32_t(std::stoul(value, nullptr, 16));
        recomp::Symbols()[name] = {address, kind == "fun"};
    }
    return true;
}

bool LoadHooks(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    bool ok = true;
    while (std::getline(in, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string name;
        if (!(ls >> name)) continue;
        auto it = recomp::Symbols().find(name);
        if (it == recomp::Symbols().end() || !it->second.function || !it->second.address) {
            std::fprintf(stderr, "error: hle.txt names %s, which is no known function\n", name.c_str());
            ok = false;
            continue;
        }
        recomp::Hooks()[it->second.address] = name;
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: fci-recomp <default.xbe> <output dir>\n");
        return 1;
    }
    xbe::Image image;
    std::string error;
    if (!image.Load(argv[1], &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s (%08X), entry %08X, %zu sections, %zu kernel imports\n", image.title.c_str(), image.title_id,
                image.entry, image.sections.size(), image.kernel_imports.size());

    std::filesystem::path out = argv[2];
    std::filesystem::create_directories(out);

    if (argc > 3) {
        std::filesystem::path config = argv[3];
        if (!LoadSymbols(config / "symbols.txt")) std::fprintf(stderr, "warning: no %s\n", (config / "symbols.txt").string().c_str());
        LoadSymbols(config / "symbols_fci.txt");
        if (std::filesystem::exists(config / "hle.txt") && !LoadHooks(config / "hle.txt")) return 1;
        std::printf("%zu symbols, %zu host replacements\n", recomp::Symbols().size(), recomp::Hooks().size());
    }

    recomp::Analysis analysis(image, kCodeSections);
    for (const auto& [name, sym] : recomp::Symbols())
        if (sym.function && sym.address) analysis.AddEntry(sym.address);
    analysis.Run();
    analysis.WriteReport((out / "analysis.txt").string());
    analysis.WriteShapes((out / "shapes.txt").string());
    std::printf("%zu functions, %zu instructions, report in %s\n", analysis.functions.size(), analysis.insns.size(),
                (out / "analysis.txt").string().c_str());

    recomp::Codegen codegen(image, analysis);
    codegen.Write(out, kPartitions);
    size_t traps = 0;
    for (const auto& [mn, n] : codegen.unimplemented) traps += n;
    std::printf("C++ written to %s (%d files), %zu instructions emitted as traps:\n", out.string().c_str(), kPartitions,
                traps);
    for (const auto& [mn, n] : codegen.unimplemented) std::printf("  %-12s %zu\n", mn.c_str(), n);

    recomp::WriteSemanticsTest(codegen, (out / "semtest_cases.cpp").string());
    return 0;
}

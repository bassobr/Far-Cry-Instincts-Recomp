// fci-recomp: translates the x86 code of Far Cry Instincts' default.xbe into C++.
//   fci-recomp <default.xbe> <output dir> [config dir]
//   fci-recomp --semtest <file.cpp>   only the instruction semantics test (tools/semtest), no XBE needed
//   fci-recomp --host-headers <config dir> <output dir>   only what the executable compiles against, no XBE
// The config dir holds symbols.txt (XDK symbols, tools/symbols.py), symbols_fci.txt (found by hand) and
// hle.txt (the symbols host code replaces, one name per line).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
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
        if (it == recomp::Symbols().end() || !it->second.function) {
            std::fprintf(stderr, "error: hle.txt names %s, which is no known function\n", name.c_str());
            ok = false;
            continue;
        }
        if (!it->second.address) {  // a symbol at 0: not found in this XBE, the replacement is off
            recomp::OffHooks().insert(name);
            continue;
        }
        recomp::Hooks()[it->second.address] = name;
    }
    return ok;
}

}  // namespace

bool LoadConfig(const std::filesystem::path& config) {
    if (!LoadSymbols(config / "symbols.txt")) std::fprintf(stderr, "warning: no %s\n", (config / "symbols.txt").string().c_str());
    LoadSymbols(config / "symbols_fci.txt");
    if (std::filesystem::exists(config / "hle.txt") && !LoadHooks(config / "hle.txt")) return false;
    std::printf("%zu symbols, %zu host replacements", recomp::Symbols().size(), recomp::Hooks().size());
    if (!recomp::OffHooks().empty()) {
        std::printf(", %zu off (address 0):", recomp::OffHooks().size());
        for (const std::string& name : recomp::OffHooks()) std::printf(" %s", name.c_str());
    }
    std::printf("\n");
    return true;
}

int main(int argc, char** argv) {
    // --partitions N anywhere: the number of code_NNN.cpp files (default 128, CMakeLists.txt's FCI_PARTITIONS)
    int partitions = kPartitions;
    for (int i = 1; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--partitions") {
            partitions = std::max(1, std::atoi(argv[i + 1]));
            for (int k = i; k + 2 < argc; k++) argv[k] = argv[k + 2];
            argc -= 2;
            break;
        }
    if (argc < 3) {
        std::fprintf(stderr, "usage: fci-recomp <default.xbe> <output dir> [config dir] [--partitions N]\n"
                             "       fci-recomp --semtest <file.cpp>\n"
                             "       fci-recomp --host-headers <config dir> <output dir>\n");
        return 1;
    }
    // the test cases are fixed instructions (semtest.cpp), translated without a program around them
    if (std::string(argv[1]) == "--semtest") {
        xbe::Image image;
        recomp::Analysis analysis(image, {});
        recomp::Codegen codegen(image, analysis);
        return recomp::WriteSemanticsTest(codegen, argv[2]) > 0 ? 0 : 1;
    }
    // what the executable is compiled against, from the configuration alone (CI builds it without game files)
    if (std::string(argv[1]) == "--host-headers") {
        if (argc < 4 || !LoadConfig(argv[2])) return 1;
        recomp::WriteHostHeaders(argv[3]);
        return 0;
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

    if (argc > 3 && !LoadConfig(argv[3])) return 1;

    recomp::Analysis analysis(image, kCodeSections);
    for (const auto& [name, sym] : recomp::Symbols())
        if (sym.function && sym.address) analysis.AddEntry(sym.address);
    analysis.Run();
    analysis.WriteReport((out / "analysis.txt").string());
    analysis.WriteShapes((out / "shapes.txt").string());
    std::printf("%zu functions, %zu instructions, report in %s\n", analysis.functions.size(), analysis.insns.size(),
                (out / "analysis.txt").string().c_str());

    recomp::Codegen codegen(image, analysis);
    codegen.Write(out, partitions, image.content_hash);
    size_t traps = 0;
    for (const auto& [mn, n] : codegen.unimplemented) traps += n;
    std::printf("C++ written to %s (%d files), %zu instructions emitted as traps:\n", out.string().c_str(), partitions,
                traps);
    for (const auto& [mn, n] : codegen.unimplemented) std::printf("  %-12s %zu\n", mn.c_str(), n);

    recomp::WriteSemanticsTest(codegen, (out / "semtest_cases.cpp").string());
    return 0;
}

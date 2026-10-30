#include <cassert>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <file.h>
#include <disasm.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>
#include "function.h"

// Jump tables are found by tracing the value moved into the count register backwards from every
// "mtctr rX; bctr" pair. Tracing data flow instead of matching fixed instruction sequences makes the
// detection independent of how the compiler scheduled the instructions, which differs between games.
//
// The supported shapes are:
//   absolute:  ctr = *(u32*)(table + (index << 2))
//   offset:    ctr = labelBase + *(u8*)(table + index)
//   computed:  ctr = labelBase + (*(u8*)(table + index) << shift)
//   short:     ctr = labelBase + *(u16*)(table + (index << 1))   (optionally shifted as well)

constexpr size_t MAX_WINDOW = 64;

struct SwitchTable
{
    std::vector<size_t> labels{};
    size_t base{};
    size_t defaultLabel{};
    uint32_t r{};
};

// Instructions preceding a "mtctr", in reverse order: window[0] is the mtctr itself.
struct Window
{
    std::vector<ppc_insn> insns;
    size_t address{};

    size_t AddressOf(size_t i) const
    {
        return address - i * 4;
    }
};

static bool StartsWith(const char* name, const char* prefix)
{
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

static bool EndsBlock(const ppc_insn& insn)
{
    switch (insn.opcode->id)
    {
    case PPC_INST_B:
    case PPC_INST_BL:
    case PPC_INST_BLR:
    case PPC_INST_BCTR:
    case PPC_INST_BCTRL:
        return true;
    }

    return false;
}

static bool WritesGpr(const ppc_insn& insn, uint32_t reg)
{
    switch (insn.opcode->id)
    {
    case PPC_INST_NOP:
        return false;

    case PPC_INST_LIS:
    case PPC_INST_LI:
    case PPC_INST_ADDI:
    case PPC_INST_ORI:
    case PPC_INST_RLWINM:
    case PPC_INST_CLRLWI:
    case PPC_INST_LWZX:
    case PPC_INST_LHZX:
    case PPC_INST_LBZX:
    case PPC_INST_ADD:
    case PPC_INST_MR:
        return insn.operands[0] == reg;
    }

    // Instructions whose first operand is not a destination GPR.
    static const char* nonWriters[] = { "st", "mt", "cmp", "b", "lf", "lv", "f", "v", "dcb", "icb", "tw", "td", "sync", "lwsync", "eieio", "isync", "db" };
    for (const char* prefix : nonWriters)
    {
        if (StartsWith(insn.opcode->name, prefix))
            return false;
    }

    // Anything else is conservatively assumed to write its first operand.
    return insn.operands[0] == reg;
}

// Finds the closest instruction before "from" that writes "reg". Returns 0 when there is none.
static size_t FindDef(const Window& window, uint32_t reg, size_t from)
{
    for (size_t i = from + 1; i < window.insns.size(); i++)
    {
        if (WritesGpr(window.insns[i], reg))
            return i;
    }

    return 0;
}

static std::optional<uint32_t> ResolveConstant(const Window& window, uint32_t reg, size_t from, size_t& earliest)
{
    size_t def = FindDef(window, reg, from);
    if (def == 0)
        return std::nullopt;

    earliest = (std::max)(earliest, def);
    const auto& insn = window.insns[def];

    switch (insn.opcode->id)
    {
    case PPC_INST_LIS:
        return insn.operands[1] << 16;

    case PPC_INST_LI:
        return insn.operands[1];

    case PPC_INST_ADDI:
        if (auto value = ResolveConstant(window, insn.operands[1], def, earliest))
            return *value + insn.operands[2];
        break;

    case PPC_INST_ORI:
        if (auto value = ResolveConstant(window, insn.operands[1], def, earliest))
            return *value | insn.operands[2];
        break;

    case PPC_INST_MR:
        return ResolveConstant(window, insn.operands[1], def, earliest);
    }

    return std::nullopt;
}

// Resolves "reg" to "source << shift" with a plain left shift. A register with no shifting is a shift of 0.
static void ResolveShift(const Window& window, uint32_t reg, size_t from, uint32_t& source, size_t& sourcePos, uint32_t& shift)
{
    source = reg;
    sourcePos = from;
    shift = 0;

    size_t def = FindDef(window, reg, from);
    if (def == 0)
        return;

    const auto& insn = window.insns[def];
    if (insn.opcode->id == PPC_INST_RLWINM && insn.operands[3] == 0 && insn.operands[4] == 31 - insn.operands[2])
    {
        source = insn.operands[1];
        sourcePos = def;
        shift = insn.operands[2];
    }
}

struct TableLoad
{
    uint32_t table{};
    uint32_t index{};
    size_t indexPos{};
    size_t earliest{};
};

// Matches "load rD, rA, rB" where one operand is a constant table address and the other the index,
// scaled by the size of the loaded element.
static std::optional<TableLoad> MatchTableLoad(const Window& window, size_t def, uint32_t elementShift, bool& constantTable)
{
    const auto& insn = window.insns[def];
    for (int i = 0; i < 2; i++)
    {
        uint32_t tableReg = insn.operands[1 + i];
        uint32_t indexReg = insn.operands[2 - i];
        if (tableReg == 0)
            continue;

        TableLoad load{};
        load.earliest = def;
        auto table = ResolveConstant(window, tableReg, def, load.earliest);
        if (!table.has_value())
            continue;

        constantTable = true;

        uint32_t shift;
        ResolveShift(window, indexReg, def, load.index, load.indexPos, shift);
        if (shift != elementShift)
            continue;

        load.table = *table;
        load.earliest = (std::max)(load.earliest, load.indexPos);
        return load;
    }

    return std::nullopt;
}

enum class EntryType
{
    Absolute,
    Byte,
    Short
};

struct Candidate
{
    EntryType type{};
    TableLoad load{};
    uint32_t labelBase{};
    uint32_t entryShift{};
    bool looksLikeTable{};
};

static std::optional<Candidate> MatchTarget(const Window& window, Candidate& candidate)
{
    uint32_t target = window.insns[0].operands[0];
    size_t def = FindDef(window, target, 0);
    if (def == 0)
        return std::nullopt;

    const auto& insn = window.insns[def];
    if (insn.opcode->id == PPC_INST_LWZX)
    {
        if (auto load = MatchTableLoad(window, def, 2, candidate.looksLikeTable))
        {
            candidate.type = EntryType::Absolute;
            candidate.load = *load;
            return candidate;
        }
    }
    else if (insn.opcode->id == PPC_INST_ADD)
    {
        for (int i = 0; i < 2; i++)
        {
            size_t earliest = def;
            auto labelBase = ResolveConstant(window, insn.operands[1 + i], def, earliest);
            if (!labelBase.has_value())
                continue;

            candidate.looksLikeTable = true;

            uint32_t offsetReg;
            size_t offsetPos;
            ResolveShift(window, insn.operands[2 - i], def, offsetReg, offsetPos, candidate.entryShift);

            size_t loadDef = FindDef(window, offsetReg, offsetPos);
            if (loadDef == 0)
                continue;

            std::optional<TableLoad> load;
            uint32_t opcode = window.insns[loadDef].opcode->id;
            if (opcode == PPC_INST_LBZX)
            {
                candidate.type = EntryType::Byte;
                load = MatchTableLoad(window, loadDef, 0, candidate.looksLikeTable);
            }
            else if (opcode == PPC_INST_LHZX)
            {
                candidate.type = EntryType::Short;
                load = MatchTableLoad(window, loadDef, 1, candidate.looksLikeTable);
            }

            if (load.has_value())
            {
                candidate.load = *load;
                candidate.load.earliest = (std::max)(candidate.load.earliest, earliest);
                candidate.labelBase = *labelBase;
                return candidate;
            }
        }
    }

    return std::nullopt;
}

// Walks backwards from the index use to find the bounds check ("cmplwi crN, rIndex, N" followed by a
// conditional branch to the default case), following register copies made with "mr" or "rotlwi rD, rS, 0" and,
// in unoptimized code, an index reloaded from the stack slot whose value was compared.
static bool FindBounds(const Window& window, uint32_t index, size_t indexPos, SwitchTable& table)
{
    std::vector<uint32_t> aliases{ index };
    std::optional<size_t> maskCount;

    // Conditional branches seen so far that have not been matched with a compare yet, per CR field.
    std::optional<size_t> pendingBranch[8];
    bool crClaimed[8]{};

    // Compares on a register that may turn out to be a copy of the index, e.g. "mr r30, r4; cmplwi r4, 9"
    // followed by indexing with r30. They match once that copy is found, as long as the register is not
    // written in between.
    struct DeferredCompare
    {
        size_t compare;
        size_t branch;
        uint32_t reg;
        bool valid;
    };
    std::vector<DeferredCompare> deferred;

    // Offset from r1 of the stack slot the index was reloaded from, until a store to that slot is seen.
    std::optional<uint32_t> slot;

    auto accept = [&](size_t compare, size_t branchPos)
        {
            const auto& branch = window.insns[branchPos];
            if (branch.opcode->id == PPC_INST_BGT && branch.opcode->operands[1] != 0)
                table.defaultLabel = branch.operands[1];

            table.labels.resize(window.insns[compare].operands[2] + 1);
            return true;
        };

    for (size_t i = 1; i < window.insns.size(); i++)
    {
        const auto& insn = window.insns[i];
        uint32_t id = insn.opcode->id;

        if (id == PPC_INST_BGT || id == PPC_INST_BGTLR || id == PPC_INST_BLE || id == PPC_INST_BLELR)
        {
            uint32_t cr = insn.operands[0] & 7;
            if (!crClaimed[cr] && !pendingBranch[cr].has_value())
                pendingBranch[cr] = i;
            continue;
        }

        if (StartsWith(insn.opcode->name, "cmp"))
        {
            uint32_t cr = insn.operands[0] & 7;
            if (id == PPC_INST_CMPLWI && !crClaimed[cr] && pendingBranch[cr].has_value())
            {
                if (std::find(aliases.begin(), aliases.end(), insn.operands[1]) != aliases.end())
                    return accept(i, *pendingBranch[cr]);

                deferred.push_back({ i, *pendingBranch[cr], insn.operands[1], true });
            }

            crClaimed[cr] = true;
            continue;
        }

        if (slot.has_value() && insn.operands[2] == 1 && insn.operands[1] == *slot)
        {
            if (id == PPC_INST_LWZ)
            {
                for (const auto& compare : deferred)
                {
                    if (compare.valid && compare.reg == insn.operands[0])
                        return accept(compare.compare, compare.branch);
                }
            }
            else if (StartsWith(insn.opcode->name, "st"))
            {
                slot.reset();
            }
        }

        for (auto& compare : deferred)
            compare.valid &= !WritesGpr(insn, compare.reg);

        // Only definitions before the index is used affect which registers hold it.
        if (i <= indexPos)
            continue;

        for (size_t a = 0; a < aliases.size(); a++)
        {
            if (!WritesGpr(insn, aliases[a]))
                continue;

            if (id == PPC_INST_MR || (id == PPC_INST_ROTLWI && insn.operands[2] == 0))
            {
                aliases[a] = insn.operands[1];

                for (const auto& compare : deferred)
                {
                    if (compare.valid && compare.reg == aliases[a])
                        return accept(compare.compare, compare.branch);
                }
            }
            else
            {
                if (id == PPC_INST_CLRLWI && !maskCount.has_value() && insn.operands[2] >= 24)
                    maskCount = size_t(1) << (32 - insn.operands[2]);

                if (id == PPC_INST_LWZ && insn.operands[2] == 1 && !slot.has_value())
                    slot = insn.operands[1];

                aliases.erase(aliases.begin() + a);
            }
            break;
        }

        if (aliases.empty() && !slot.has_value())
            break;
    }

    // An index masked with clrlwi needs no bounds check.
    if (maskCount.has_value())
    {
        table.labels.resize(*maskCount);
        return true;
    }

    return false;
}

static std::optional<uint32_t> ReadEntry(const Image& image, const Candidate& candidate, size_t i)
{
    static const size_t entrySizes[] = { 4, 1, 2 };
    size_t entrySize = entrySizes[size_t(candidate.type)];
    size_t address = candidate.load.table + i * entrySize;
    if (address < image.base || address + entrySize > image.base + image.size)
        return std::nullopt;

    const auto* data = (const uint8_t*)image.Find(address);
    switch (candidate.type)
    {
    case EntryType::Absolute:
        return *(const be<uint32_t>*)data;
    case EntryType::Byte:
        return candidate.labelBase + (uint32_t(*data) << candidate.entryShift);
    case EntryType::Short:
        return candidate.labelBase + (uint32_t(*(const be<uint16_t>*)data) << candidate.entryShift);
    }

    return std::nullopt;
}

static std::string out;

template<class... Args>
static void println(fmt::format_string<Args...> fmt, Args&&... args)
{
    fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    out += '\n';
};

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("Usage: XenonAnalyse [input XEX file path] [output jump table TOML file path]");
        return EXIT_SUCCESS;
    }

    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

    println("# Generated by XenonAnalyse");

    // Function boundaries from .pdata, used to tell switch targets apart from function pointers.
    std::map<size_t, size_t> functions;
    if (const auto* pdata = image.Find(".pdata"))
    {
        const auto* entries = (const IMAGE_CE_RUNTIME_FUNCTION*)pdata->data;
        for (size_t i = 0; i < pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION); i++)
        {
            auto fn = entries[i];
            fn.BeginAddress = ByteSwap(fn.BeginAddress);
            fn.Data = ByteSwap(fn.Data);
            if (fn.BeginAddress != 0)
                functions.emplace(fn.BeginAddress, fn.BeginAddress + fn.FunctionLength * 4);
        }
    }

    static const char* typeNames[] = { "absolute", "byte offset", "short offset" };
    size_t found = 0;
    size_t inferred = 0;
    size_t functionTables = 0;
    std::vector<std::pair<size_t, const char*>> unresolved;

    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;

        const auto* code = (const uint32_t*)section.data;
        size_t count = section.size / 4;

        for (size_t i = 1; i < count; i++)
        {
            ppc_insn insn;
            ppc::Disassemble(&code[i], section.base + i * 4, insn);
            if (insn.opcode == nullptr || insn.opcode->id != PPC_INST_BCTR)
                continue;

            // Older compilers can schedule a nop between mtctr and bctr.
            size_t start = i - 1;
            while (start > 0)
            {
                ppc_insn nop;
                ppc::Disassemble(&code[start], section.base + start * 4, nop);
                if (nop.opcode == nullptr || nop.opcode->id != PPC_INST_NOP)
                    break;
                start--;
            }

            Window window;
            window.address = section.base + start * 4;
            for (size_t k = start; window.insns.size() < MAX_WINDOW; k--)
            {
                ppc_insn& prev = window.insns.emplace_back();
                ppc::Disassemble(&code[k], section.base + k * 4, prev);
                if (prev.opcode == nullptr || (window.insns.size() > 1 && EndsBlock(prev)))
                {
                    window.insns.pop_back();
                    break;
                }

                if (k == 0)
                    break;
            }

            if (window.insns.empty() || window.insns[0].opcode->id != PPC_INST_MTCTR)
                continue;

            size_t bctrAddress = section.base + i * 4;
            Candidate candidate{};
            auto match = MatchTarget(window, candidate);
            if (!match.has_value())
            {
                if (candidate.looksLikeTable)
                    unresolved.emplace_back(bctrAddress, "unrecognized table shape");

                continue;
            }

            // Switch targets must stay inside the function containing the bctr. Leaf functions have no .pdata
            // entry, but they lie between the neighboring .pdata functions, which bounds them.
            size_t fnBegin = section.base;
            size_t fnEnd = section.base + section.size;
            bool fnKnown = false;
            auto fn = functions.upper_bound(bctrAddress);
            if (fn != functions.begin() && bctrAddress < std::prev(fn)->second)
            {
                fnBegin = std::prev(fn)->first;
                fnEnd = std::prev(fn)->second;
                fnKnown = true;
            }
            else
            {
                if (fn != functions.end())
                    fnEnd = (std::min)(fnEnd, fn->first);
                if (fn != functions.begin())
                    fnBegin = (std::max)(fnBegin, std::prev(fn)->second);
            }

            auto isSwitchLabel = [&](size_t label)
                {
                    return label > fnBegin && label < fnEnd && (label & 3) == 0 && functions.find(label) == functions.end();
                };

            SwitchTable table{};
            table.r = match->load.index;

            bool sizeInferred = false;
            if (!FindBounds(window, match->load.index, match->load.indexPos, table))
            {
                // Switch tables are constant, so the compiler places them in code or read-only data. A table
                // anywhere else holds function pointers, which the recompiler calls indirectly.
                bool constantTable = false;
                for (const auto& tableSection : image.sections)
                {
                    if (match->load.table >= tableSection.base && match->load.table < tableSection.base + tableSection.size)
                        constantTable = (tableSection.flags & SectionFlags_Code) || tableSection.name == ".rdata";
                }

                if (!constantTable)
                {
                    functionTables++;
                    continue;
                }

                // Without a bounds check, count the leading entries that are valid targets. A table whose first
                // entry is not a valid target holds function pointers. Tables placed right after the bctr are
                // followed by their case labels, so the table cannot extend past its lowest label.
                bool inlineTable = match->load.table == bctrAddress + 4;
                size_t entries = 0;
                size_t lowestLabel = SIZE_MAX;
                while (entries < 0x1000)
                {
                    auto entry = ReadEntry(image, *match, entries);
                    if (!entry.has_value() || !isSwitchLabel(*entry))
                        break;

                    if (inlineTable && match->load.table + (entries + 1) * 4 > (std::min)(lowestLabel, size_t(*entry)))
                        break;

                    lowestLabel = (std::min)(lowestLabel, size_t(*entry));
                    entries++;
                }

                if (entries == 0)
                {
                    functionTables++;
                    continue;
                }

                // Offset tables are only sized this way inside a known function, which bounds their labels. Such a
                // switch is usually nested in a case of another switch on the same index, which checked the bounds.
                if (!(fnKnown || inlineTable) || (match->type != EntryType::Absolute && !fnKnown))
                {
                    unresolved.emplace_back(bctrAddress, "bounds check not found");
                    continue;
                }

                table.labels.resize(entries);
                sizeInferred = true;
            }

            bool labelsValid = true;
            bool anyLabelValid = false;
            for (size_t k = 0; k < table.labels.size(); k++)
            {
                auto entry = ReadEntry(image, *match, k);
                bool valid = entry.has_value() && isSwitchLabel(*entry);
                labelsValid &= valid;
                anyLabelValid |= valid;
                table.labels[k] = entry.value_or(0);
            }

            if (!anyLabelValid)
            {
                functionTables++;
                continue;
            }

            if (!labelsValid)
            {
                unresolved.emplace_back(bctrAddress, "labels point outside the function");
                continue;
            }

            // The recompiler switches on the index register at the bctr, so it must still hold the index there.
            bool redefined = false;
            for (size_t k = 1; k <= match->load.indexPos; k++)
                redefined |= WritesGpr(window.insns[k], table.r);

            if (redefined)
            {
                unresolved.emplace_back(bctrAddress, "index register is overwritten before bctr");
                continue;
            }

            table.base = window.AddressOf(match->load.earliest);

            println("[[switch]]");
            println("# bctr = 0x{:X}, {}{}", bctrAddress, typeNames[size_t(match->type)],
                sizeInferred ? ", no bounds check: size inferred from table contents, verify manually" : "");
            println("base = 0x{:X}", table.base);
            println("r = {}", table.r);
            println("default = 0x{:X}", table.defaultLabel);
            println("labels = [");
            for (const auto& label : table.labels)
                println("    0x{:X},", label);
            println("]");
            println("");

            found++;
            inferred += sizeInferred;
        }
    }

    for (const auto& [address, reason] : unresolved)
        println("# UNRESOLVED: 0x{:X} ({})", address, reason);

    std::ofstream f(argv[2]);
    f.write(out.data(), out.size());

    fmt::println("Found {} jump tables ({} with inferred sizes), skipped {} function pointer tables, {} unresolved",
        found, inferred, functionTables, unresolved.size());
    for (const auto& [address, reason] : unresolved)
        fmt::println("  unresolved jump table at 0x{:X}: {}", address, reason);

    return EXIT_SUCCESS;
}

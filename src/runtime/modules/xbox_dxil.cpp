#include "runtime/modules/xbox_dxil.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <objbase.h>
#include <dxcapi.h>
#else
#include <dxc/dxcapi.h>
#endif

namespace xv::runtime::modules {
namespace {
// --- the text of a DXIL -------------------------------------------------------------------
//
// The lowering works on the text of the DXIL, as `dxc -dumpbin` prints it. It is a port of
// americas-recomp's `tools/xbox/platform/xbox_dxil_lower.py`, which lowers the console's own
// intrinsics (@xbox.*) to the standard ones the host knows.

std::string trim(const std::string &text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// The pieces of a text, split at the separators that are not inside brackets.
std::vector<std::string> split_top(const std::string &text, char separator) {
    std::vector<std::string> pieces;
    std::string current;
    int depth = 0;
    for (const char c : text) {
        if (c == '{' || c == '(' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ')' || c == ']') {
            --depth;
        }
        if (c == separator && depth == 0) {
            pieces.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    pieces.push_back(current);
    return pieces;
}

// The lines of a text.
std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    lines.push_back(current);
    return lines;
}

// The lines of a text, one after another.
std::string join_lines(const std::vector<std::string> &lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i)
            out += "\n";
        out += lines[i];
    }
    return out;
}

// The lines of a text, with what ends them.
std::vector<std::string> split_lines_kept(const std::string &text) {
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto end = text.find('\n', at);
        if (end == std::string::npos) {
            lines.push_back(text.substr(at));
            break;
        }
        lines.push_back(text.substr(at, end - at + 1));
        at = end + 1;
    }
    return lines;
}

// The words of a text.
std::vector<std::string> split_words(const std::string &text) {
    std::vector<std::string> words;
    std::string current;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        current += c;
    }
    if (!current.empty())
        words.push_back(current);
    return words;
}

// Where the bracket that opens here closes.
std::size_t bracket_end(const std::string &text, std::size_t at) {
    int depth = 0;
    for (std::size_t i = at; i < text.size(); ++i) {
        if (text[i] == '{' || text[i] == '(' || text[i] == '[') {
            ++depth;
        } else if (text[i] == '}' || text[i] == ')' || text[i] == ']') {
            if (--depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

// Whether the text is one of the pairs the console's shaders use.
bool pair_type(const std::string &text) {
    return text == "{ half, half }" || text == "{ i32, i32 }" || text == "{ i32, i32, i32 }" ||
           text == "{ i32, i32, i32, i32 }";
}

// Whether the text is a value's name.
bool value_name(const std::string &text) {
    if (text.size() < 2 || text.front() != '%')
        return false;
    return std::all_of(text.begin() + 1, text.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
    });
}

// Whether the text is a number.
bool number(const std::string &text) {
    if (text.empty())
        return false;
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

// The name of the value a text holds, if it holds one.
std::string name_of(const std::string &text) {
    const auto trimmed = trim(text);
    if (value_name(trimmed))
        return trimmed;
    const auto space = trimmed.find(' ');
    if (space == std::string::npos)
        return {};
    const auto rest = trim(trimmed.substr(space + 1));
    return value_name(rest) ? rest : std::string{};
}

// The value a text holds, without its type: "i32 %3" is "%3", "i32 -1" is "-1".
std::string scalar_of(const std::string &text) {
    const auto trimmed = trim(text);
    const auto space = trimmed.find(' ');
    if (space == std::string::npos)
        return trimmed;
    return trim(trimmed.substr(space + 1));
}

// The values of a literal pair: "{ i32 1, i32 2 }" is "1" and "2".
std::vector<std::string> literal_parts(const std::string &text) {
    std::vector<std::string> parts;
    const auto words = split_words(text);
    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        if (words[i] != "half" && words[i] != "i32")
            continue;
        auto value = words[i + 1];
        const auto end = value.find_first_of(",}");
        if (end != std::string::npos)
            value = value.substr(0, end);
        parts.push_back(value);
    }
    return parts;
}

// What an argument holds: a pair's components, or one value.
struct Operand {
    bool is_pair = false;
    std::vector<std::string> parts;
    std::string one;
};

// The state of the lowering.
struct Lowering {
    std::vector<std::string> lines;
    // The components of the pairs that were made.
    std::map<std::string, std::vector<std::string>> pairs;
    // The values that are another one.
    std::map<std::string, std::string> alias;
    // The byte a load returned.
    std::map<std::string, std::string> bytes;
    // The declarations that are needed.
    std::set<std::string> used;
    int counter = 0;
    int calls = 0;

    std::string fresh() { return "%xl." + std::to_string(++counter); }
    void emit(const std::string &line) { lines.push_back(line); }
    // The value of an argument: the components of a pair, or the value itself.
    Operand operand(const std::string &argument) const {
        Operand result;
        const auto trimmed = trim(argument);
        if (!trimmed.empty() && trimmed.front() == '{') {
            const auto close = bracket_end(trimmed, 0);
            if (close == std::string::npos)
                return result;
            const auto type = trimmed.substr(0, close + 1);
            if (!pair_type(type))
                return result;
            result.is_pair = true;
            const auto source = trim(trimmed.substr(close + 1));
            if (!source.empty() && source.front() == '{') {
                // a literal pair
                result.parts = literal_parts(source);
                return result;
            }
            const auto found = pairs.find(source);
            if (found != pairs.end())
                result.parts = found->second;
            return result;
        }
        result.one = scalar_of(trimmed);
        return result;
    }
};

// The values a call made are replaced by the ones they alias.
std::string substitute(const Lowering &lowering, const std::string &line) {
    if (lowering.alias.empty())
        return line;
    std::string out;
    for (std::size_t i = 0; i < line.size();) {
        if (line[i] != '%') {
            out += line[i++];
            continue;
        }
        std::size_t end = i + 1;
        while (end < line.size() &&
               (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_' ||
                line[end] == '.'))
            ++end;
        const auto name = line.substr(i, end - i);
        const auto found = lowering.alias.find(name);
        out += found != lowering.alias.end() ? found->second : name;
        i = end;
    }
    return out;
}

// A call in the text.
struct Call {
    std::string indent;
    std::string destination; // empty when the call returns nothing
    std::string type;
    std::string name;
    std::string opcode; // the number that says what the call is
    std::vector<std::string> arguments;
};

// A call to a function, in the text: "  %12 = call float @xbox.Med3_F32(i32 10, float %3)".
bool parse_call(const std::string &line, const std::string &function, Call &out) {
    const auto trimmed = trim(line);
    std::string rest;
    const auto at = trimmed.find(" = call ");
    if (at != std::string::npos) {
        out.destination = trim(trimmed.substr(0, at));
        rest = trimmed.substr(at + 8);
    } else if (trimmed.rfind("call ", 0) == 0) {
        rest = trimmed.substr(5);
    } else {
        return false;
    }
    const auto name_at = rest.find(function);
    if (name_at == std::string::npos)
        return false;
    out.type = trim(rest.substr(0, name_at));
    const auto open = rest.find('(', name_at);
    if (open == std::string::npos)
        return false;
    const auto close = bracket_end(rest, open);
    if (close == std::string::npos)
        return false;
    out.name = rest.substr(name_at + function.size(), open - name_at - function.size());
    const auto inner = rest.substr(open + 1, close - open - 1);
    for (auto &piece : split_top(inner, ','))
        out.arguments.push_back(trim(piece));
    out.indent = line.substr(0, line.find_first_not_of(" \t"));
    // the first argument of a call is the number that says what it is
    if (out.arguments.empty())
        return false;
    const auto first = trim(out.arguments.front());
    if (first.rfind("i32 ", 0) != 0 || !number(trim(first.substr(4))))
        return false;
    out.opcode = first;
    out.arguments.erase(out.arguments.begin());
    return true;
}

// The instruction that makes a pair's component.
struct InsertValue {
    std::string destination;
    std::string type;
    std::string source;
    std::string value;
    int index = 0;
};
bool parse_insertvalue(const std::string &line, InsertValue &out) {
    static const std::string pattern = " = insertvalue ";
    const auto at = line.find(pattern);
    if (at == std::string::npos)
        return false;
    const auto rest = trim(line.substr(at + pattern.size()));
    const auto pieces = split_top(rest, ',');
    if (pieces.size() != 3)
        return false;
    auto head = trim(pieces[0]);
    if (head.empty() || head.front() != '{')
        return false;
    const auto close = bracket_end(head, 0);
    if (close == std::string::npos)
        return false;
    const auto type = head.substr(0, close + 1);
    if (!pair_type(type))
        return false;
    out.destination = trim(line.substr(0, at));
    out.type = type;
    out.source = trim(head.substr(close + 1));
    out.value = scalar_of(pieces[1]);
    out.index = std::atoi(trim(pieces[2]).c_str());
    return true;
}

// The instruction that takes a pair's component.
struct ExtractValue {
    std::string destination;
    std::string type;
    std::string source;
    int index = 0;
};
bool parse_extractvalue(const std::string &line, ExtractValue &out) {
    static const std::string pattern = " = extractvalue ";
    const auto at = line.find(pattern);
    if (at == std::string::npos)
        return false;
    const auto rest = trim(line.substr(at + pattern.size()));
    const auto pieces = split_top(rest, ',');
    if (pieces.size() != 2)
        return false;
    auto head = trim(pieces[0]);
    if (head.empty() || head.front() != '{')
        return false;
    const auto close = bracket_end(head, 0);
    if (close == std::string::npos)
        return false;
    const auto type = head.substr(0, close + 1);
    if (!pair_type(type))
        return false;
    out.destination = trim(line.substr(0, at));
    out.type = type;
    out.source = trim(head.substr(close + 1));
    out.index = std::atoi(trim(pieces[1]).c_str());
    return true;
}

// The instruction that takes a component of what a load returned.
struct ResRetValue {
    std::string destination;
    std::string source;
    int index = 0;
};
bool parse_resret(const std::string &line, ResRetValue &out) {
    static const std::string pattern = " = extractvalue %dx.types.ResRet.i32 ";
    const auto at = line.find(pattern);
    if (at == std::string::npos)
        return false;
    const auto rest = trim(line.substr(at + pattern.size()));
    const auto pieces = split_top(rest, ',');
    if (pieces.size() != 2)
        return false;
    out.destination = trim(line.substr(0, at));
    out.source = trim(pieces[0]);
    out.index = std::atoi(trim(pieces[1]).c_str());
    return true;
}

// The call to llvm.copysign.f32, which the console's compiler leaves in the DXIL.
struct CopySign {
    std::string destination;
    std::string first;
    std::string second;
};
bool parse_copysign(const std::string &line, CopySign &out) {
    static const std::string pattern = " = call float @llvm.copysign.f32(float ";
    const auto at = line.find(pattern);
    if (at == std::string::npos)
        return false;
    const auto rest = line.substr(at + pattern.size());
    const auto close = rest.find(')');
    if (close == std::string::npos)
        return false;
    const auto arguments = split_top(rest.substr(0, close), ',');
    if (arguments.size() != 2)
        return false;
    out.destination = trim(line.substr(0, at));
    out.first = scalar_of(arguments[0]);
    out.second = scalar_of(arguments[1]);
    return true;
}

// The declarations the lowering needs.
const std::map<std::string, std::string> &declarations() {
    static const std::map<std::string, std::string> known{
        {"@dx.op.binary.f32(", "declare float @dx.op.binary.f32(i32, float, float) #0"},
        {"@dx.op.binary.f16(", "declare half @dx.op.binary.f16(i32, half, half) #0"},
        {"@dx.op.waveGetLaneIndex(", "declare i32 @dx.op.waveGetLaneIndex(i32) #0"},
        {"@dx.op.waveReadLaneFirst.i32(",
         "declare i32 @dx.op.waveReadLaneFirst.i32(i32, i32) #1"},
        {"@dx.op.waveReadLaneAt.i32(", "declare i32 @dx.op.waveReadLaneAt.i32(i32, i32, i32) #1"},
        {"@dx.op.waveActiveBallot(",
         "declare %dx.types.fouri32 @dx.op.waveActiveBallot(i32, i1) #1"},
        {"@dx.op.unaryBits.i32(", "declare i32 @dx.op.unaryBits.i32(i32, i32) #0"},
        {"@dx.op.binary.i32(", "declare i32 @dx.op.binary.i32(i32, i32, i32) #0"},
        {"@dx.op.waveActiveOp.i32(", "declare i32 @dx.op.waveActiveOp.i32(i32, i32, i8, i8) #1"},
        {"@dx.op.waveReadLaneFirst.i64(",
         "declare i64 @dx.op.waveReadLaneFirst.i64(i32, i64) #1"},
        {"@dx.op.rawBufferLoad.i32(",
         "declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, "
         "i8, i32) #1"},
        {"@dx.op.rawBufferStore.i32(",
         "declare void @dx.op.rawBufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, "
         "i32, i8, i32)"},
        {"@dx.op.atomicBinOp.i32(",
         "declare i32 @dx.op.atomicBinOp.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32)"},
        {"@dx.op.createHandleFromHeap(",
         "declare %dx.types.Handle @dx.op.createHandleFromHeap(i32, i32, i1, i1)"},
        {"@dx.op.annotateHandle(",
         "declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, "
         "%dx.types.ResourceProperties)"},
    };
    return known;
}

// The address table of the console: the descriptors of the heaps, by address.
constexpr int address_table = 0;
constexpr int address_table_log2 = 12;

// The lowering itself.
class Lowerer {
  public:
    explicit Lowerer(Lowering &state) : state_(state) {}

    // The call in the text, lowered. Whether it was one of the console's.
    bool lower(const std::string &line, const Call &call);

  private:
    Lowering &state_;

  public:

    // A handle of a descriptor heap.
    std::string heap(const std::string &indent, const std::string &index, bool nonuniform) {
        const auto raw = state_.fresh(), handle = state_.fresh();
        state_.emit(indent + raw + " = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, "
                                    "i32 " +
                     index + ", i1 false, i1 " + (nonuniform ? "true" : "false") + ")");
        state_.emit(indent + handle +
                    " = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle " +
                    raw + ", %dx.types.ResourceProperties { i32 4107, i32 0 })");
        state_.used.insert("@dx.op.createHandleFromHeap(");
        state_.used.insert("@dx.op.annotateHandle(");
        state_.used.insert("@dx.op.rawBufferLoad.i32(");
        return handle;
    }
    // An integer operation.
    std::string integer(const std::string &indent, const char *operation, const std::string &first,
                        const std::string &second) {
        const auto result = state_.fresh();
        state_.emit(indent + result + " = " + operation + " i32 " + first + ", " + second);
        return result;
    }
    // One entry of the address table: the address it holds and its heap.
    std::string table_entry(const std::string &indent, const std::string &index, int mask,
                            std::vector<std::string> &components) {
        const auto loaded = state_.fresh();
        const auto offset = integer(indent, "shl", index, "4");
        const auto place = integer(indent, "add", offset, "16");
        state_.emit(indent + loaded + " = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 "
                                    "139, %dx.types.Handle " +
                     table_ + ", i32 " + place + ", i32 undef, i8 " + std::to_string(mask) +
                     ", i32 4)");
        state_.used.insert("@dx.op.rawBufferLoad.i32(");
        components.clear();
        for (int k = 0; k < (mask == 15 ? 4 : 2); ++k) {
            const auto component = state_.fresh();
            state_.emit(indent + component + " = extractvalue %dx.types.ResRet.i32 " + loaded +
                        ", " + std::to_string(k));
            components.push_back(component);
        }
        const auto low = state_.fresh(), high = state_.fresh(), wide = state_.fresh(),
                   base = state_.fresh();
        state_.emit(indent + low + " = zext i32 " + components[0] + " to i64");
        state_.emit(indent + high + " = zext i32 " + components[1] + " to i64");
        state_.emit(indent + wide + " = shl i64 " + high + ", 32");
        state_.emit(indent + base + " = or i64 " + wide + ", " + low);
        return base;
    }
    // The heap an address is in, and the offset in it.
    std::string address(const std::string &indent, const std::string &value, bool nonuniform,
                        std::string &offset) {
        table_ = heap(indent, std::to_string(address_table), false);
        std::string current = "0";
        for (int step = 1 << (address_table_log2 - 1); step > 0; step >>= 1) {
            const auto probe = integer(indent, "add", current, std::to_string(step));
            std::vector<std::string> components;
            const auto base = table_entry(indent, probe, 3, components);
            const auto below = state_.fresh(), next = state_.fresh();
            state_.emit(indent + below + " = icmp ule i64 " + base + ", " + value);
            state_.emit(indent + next + " = select i1 " + below + ", i32 " + probe + ", i32 " +
                        current);
            current = next;
        }
        std::vector<std::string> components;
        const auto base = table_entry(indent, current, 15, components);
        const auto relative = state_.fresh();
        offset = state_.fresh();
        state_.emit(indent + relative + " = sub i64 " + value + ", " + base);
        state_.emit(indent + offset + " = trunc i64 " + relative + " to i32");
        return heap(indent, components[3], nonuniform);
    }
    // A binary operation of a kind.
    std::string binary(const std::string &indent, const char *kind, const char *type,
                       const std::string &first, const std::string &second, int operation) {
        const auto result = state_.fresh();
        state_.emit(indent + result + " = call " + type + " @dx.op.binary." + kind + "(i32 " +
                    std::to_string(operation) + ", " + type + " " + first + ", " + type + " " +
                    second + ")");
        state_.used.insert(std::string("@dx.op.binary.") + kind + "(");
        return result;
    }
    // How many bits a value has.
    std::string popcount(const std::string &indent, const std::string &value) {
        const auto result = state_.fresh();
        state_.emit(indent + result + " = call i32 @dx.op.unaryBits.i32(i32 31, i32 " + value +
                    ")");
        state_.used.insert("@dx.op.unaryBits.i32(");
        return result;
    }
    std::string table_;
};

// The lowering of the text.
std::string lower_text(const std::string &text, int &calls, std::vector<std::string> &left) {
    Lowering state;
    Lowerer lowerer(state);
    for (auto &raw : split_lines(text)) {
        std::string line = substitute(state, raw);
        // a pair's component is put in
        InsertValue insert;
        if (parse_insertvalue(line, insert)) {
            std::vector<std::string> components;
            if (!insert.source.empty() && insert.source.front() == '{') {
                components = literal_parts(insert.source);
            } else {
                const auto found = state.pairs.find(insert.source);
                if (found != state.pairs.end())
                    components = found->second;
            }
            const auto count = insert.type == "{ half, half }"       ? 2
                               : insert.type == "{ i32, i32 }"       ? 2
                               : insert.type == "{ i32, i32, i32 }"   ? 3
                                                                       : 4;
            components.resize(count, "undef");
            if (insert.index >= 0 && insert.index < static_cast<int>(components.size()))
                components[insert.index] = insert.value;
            state.pairs[insert.destination] = components;
            continue;
        }
        // a pair's component is taken out
        ExtractValue extract;
        if (parse_extractvalue(line, extract)) {
            const auto found = state.pairs.find(extract.source);
            if (found != state.pairs.end() && extract.index >= 0 &&
                extract.index < static_cast<int>(found->second.size())) {
                state.alias[extract.destination] = found->second[extract.index];
                continue;
            }
            state.lines.push_back(line);
            continue;
        }
        // a byte that was loaded is taken out
        ResRetValue resret;
        if (parse_resret(line, resret)) {
            const auto found = state.bytes.find(resret.source);
            if (found != state.bytes.end()) {
                if (resret.index != 0) {
                    state.lines.push_back(line);
                    continue;
                }
                const auto indent = line.substr(0, line.find_first_not_of(" \t"));
                state.lines.push_back(indent + resret.destination + " = and i32 " + found->second +
                                      ", 255");
                continue;
            }
            state.lines.push_back(line);
            continue;
        }
        // the console's compiler leaves llvm.copysign.f32 in
        CopySign copysign;
        if (parse_copysign(line, copysign)) {
            const auto indent = line.substr(0, line.find_first_not_of(" \t"));
            const auto first_bits = state.fresh(), second_bits = state.fresh();
            const auto first_mask = state.fresh(), second_mask = state.fresh();
            const auto joined = state.fresh();
            state.emit(indent + first_bits + " = bitcast float " + copysign.first + " to i32");
            state.emit(indent + second_bits + " = bitcast float " + copysign.second + " to i32");
            state.emit(indent + first_mask + " = and i32 " + first_bits + ", 2147483647");
            state.emit(indent + second_mask + " = and i32 " + second_bits + ", -2147483648");
            state.emit(indent + joined + " = or i32 " + first_mask + ", " + second_mask);
            state.emit(indent + copysign.destination + " = bitcast i32 " + joined + " to float");
            ++state.calls;
            continue;
        }
        // a store to an address
        Call store;
        if (parse_call(line, "@xbox.", store) &&
            (store.name == "global_store_dword_with_cache" ||
             store.name == "global_store_dwordx3_with_cache" ||
             store.name == "global_store_dwordx4_with_cache") &&
            !store.arguments.empty()) {
            const auto &indent = store.indent;
            std::string offset;
            const auto handle = lowerer.address(indent, state.operand(store.arguments[0]).one, true,
                                                offset);
            auto values = state.operand(store.arguments.size() > 1 ? store.arguments[1] : "");
            if (!values.is_pair)
                values.parts = {values.one};
            const int count = static_cast<int>(values.parts.size());
            values.parts.resize(4, "undef");
            std::string arguments;
            for (std::size_t k = 0; k < 4; ++k) {
                if (k)
                    arguments += ", ";
                arguments += "i32 " + values.parts[k];
            }
            state.emit(indent + "call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle " +
                         handle + ", i32 " + offset + ", i32 undef, " + arguments + ", i8 " +
                         std::to_string((1 << count) - 1) + ", i32 4)");
            state.used.insert("@dx.op.rawBufferStore.i32(");
            ++state.calls;
            continue;
        }
        // a call of one of the console's own intrinsics
        Call call;
        if (!parse_call(line, "@xbox.", call)) {
            state.lines.push_back(line);
            continue;
        }
        const auto &indent = call.indent;
        const auto &name = call.name;
        // a load
        const bool load = name == "global_load_dword_with_cache" ||
                          name == "global_load_dwordx2_with_cache" ||
                          name == "global_load_dwordx3_with_cache" ||
                          name == "global_load_dwordx4_with_cache" ||
                          name == "s_load_dword_with_cache";
        if (load) {
            const int count = name == "global_load_dword_with_cache" ||
                                      name == "s_load_dword_with_cache"
                                  ? 1
                                  : name == "global_load_dwordx2_with_cache" ? 2
                                  : name == "global_load_dwordx3_with_cache" ? 3
                                                                               : 4;
            std::string offset;
            const auto handle =
                lowerer.address(indent, state.operand(call.arguments[0]).one,
                                name.rfind("s_", 0) != 0, offset);
            const auto loaded = state.fresh();
            state.emit(indent + loaded + " = call %dx.types.ResRet.i32 "
                                      "@dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle " +
                         handle + ", i32 " + offset + ", i32 undef, i8 " +
                         std::to_string((1 << count) - 1) + ", i32 4)");
            state.used.insert("@dx.op.rawBufferLoad.i32(");
            if (count == 1) {
                state.emit(indent + call.destination + " = extractvalue %dx.types.ResRet.i32 " +
                           loaded + ", 0");
            } else {
                std::vector<std::string> components;
                for (int k = 0; k < count; ++k) {
                    const auto component = state.fresh();
                    state.emit(indent + component + " = extractvalue %dx.types.ResRet.i32 " +
                               loaded + ", " + std::to_string(k));
                    components.push_back(component);
                }
                state.pairs[call.destination] = components;
            }
            ++state.calls;
            continue;
        }
        // an atomic
        if (name == "global_atomic_add" || name == "global_atomic_sub" ||
            name == "global_atomic_fmin" || name == "global_atomic_fmax") {
            std::string offset;
            const auto handle =
                lowerer.address(indent, state.operand(call.arguments[0]).one, true, offset);
            const auto value = state.operand(call.arguments[1]).one;
            const auto atomic = [&](int operation, const std::string &amount,
                                    const std::string &into) {
                const auto result = into.empty() ? state.fresh() : into;
                state.emit(indent + result + " = call i32 @dx.op.atomicBinOp.i32(i32 78, "
                                          "%dx.types.Handle " +
                             handle + ", i32 " + std::to_string(operation) + ", i32 " + offset +
                             ", i32 undef, i32 undef, i32 " + amount + ")");
                return result;
            };
            state.used.insert("@dx.op.atomicBinOp.i32(");
            if (name == "global_atomic_add") {
                atomic(0, value, call.destination);
            } else if (name == "global_atomic_sub") {
                const auto difference = lowerer.integer(indent, "sub", "0", value);
                atomic(0, difference, call.destination);
            } else {
                const auto bits = state.fresh(), negative = state.fresh();
                state.emit(indent + bits + " = bitcast float " + value + " to i32");
                state.emit(indent + negative + " = icmp slt i32 " + bits + ", 0");
                const auto low = state.fresh(), high = state.fresh();
                if (name == "global_atomic_fmin") {
                    state.emit(indent + low + " = select i1 " + negative +
                               ", i32 2147483647, i32 " + bits);
                    state.emit(indent + high + " = select i1 " + negative + ", i32 " + bits +
                               ", i32 0");
                    const auto first = atomic(4, low, "");
                    atomic(7, high, "");
                    state.emit(indent + call.destination + " = bitcast i32 " + first + " to float");
                } else {
                    state.emit(indent + low + " = select i1 " + negative +
                               ", i32 -2147483648, i32 " + bits);
                    state.emit(indent + high + " = select i1 " + negative + ", i32 " + bits +
                               ", i32 -1");
                    const auto first = atomic(5, low, "");
                    atomic(6, high, "");
                    state.emit(indent + call.destination + " = bitcast i32 " + first + " to float");
                }
            }
            ++state.calls;
            continue;
        }
        // the median of three floats
        if (name == "Med3_F32") {
            const auto a = state.operand(call.arguments[0]).one;
            const auto b = state.operand(call.arguments[1]).one;
            const auto c = state.operand(call.arguments[2]).one;
            const auto low = lowerer.binary(indent, "f32", "float", a, b, 36);
            const auto high = lowerer.binary(indent, "f32", "float", a, b, 35);
            const auto middle = lowerer.binary(indent, "f32", "float", high, c, 36);
            state.emit(indent + call.destination + " = call float @dx.op.binary.f32(i32 35, float " +
                       low + ", float " + middle + ")");
            ++state.calls;
            continue;
        }
        // the minimum and the maximum of two half pairs
        if (name == "V_PK_MIN_F16" || name == "V_PK_MAX_F16") {
            const auto a = state.operand(call.arguments[0]);
            const auto b = state.operand(call.arguments[1]);
            const int operation = name == "V_PK_MIN_F16" ? 36 : 35;
            std::vector<std::string> components;
            for (int k = 0; k < 2; ++k) {
                const auto first = k < static_cast<int>(a.parts.size()) ? a.parts[k] : "undef";
                const auto second = k < static_cast<int>(b.parts.size()) ? b.parts[k] : "undef";
                components.push_back(lowerer.binary(indent, "f16", "half", first, second, operation));
            }
            state.pairs[call.destination] = components;
            ++state.calls;
            continue;
        }
        // the lane of the wave
        if (name == "GetLaneID") {
            state.emit(indent + call.destination + " = call i32 @dx.op.waveGetLaneIndex(i32 111)");
            state.used.insert("@dx.op.waveGetLaneIndex(");
            ++state.calls;
            continue;
        }
        // a value that is the same in the whole wave
        if (name == "MakeUniform.i32") {
            state.emit(indent + call.destination +
                       " = call i32 @dx.op.waveReadLaneFirst.i32(i32 118, i32 " +
                       state.operand(call.arguments[0]).one + ")");
            state.used.insert("@dx.op.waveReadLaneFirst.i32(");
            ++state.calls;
            continue;
        }
        if (name == "MakeUniform.i64") {
            state.emit(indent + call.destination +
                       " = call i64 @dx.op.waveReadLaneFirst.i64(i32 118, i64 " +
                       state.operand(call.arguments[0]).one + ")");
            state.used.insert("@dx.op.waveReadLaneFirst.i64(");
            ++state.calls;
            continue;
        }
        // the value of another lane
        if (name == "ReadLane.i32") {
            state.emit(indent + call.destination +
                       " = call i32 @dx.op.waveReadLaneAt.i32(i32 117, i32 " +
                       state.operand(call.arguments[0]).one + ", i32 " +
                       state.operand(call.arguments[1]).one + ")");
            state.used.insert("@dx.op.waveReadLaneAt.i32(");
            ++state.calls;
            continue;
        }
        // the value of another lane, chosen by a swizzle
        if (name == "LaneSwizzle.i32") {
            const auto value = state.operand(call.arguments[0]).one;
            const auto offset = std::atoi(state.operand(call.arguments[1]).one.c_str());
            const auto lane = state.fresh();
            state.emit(indent + lane + " = call i32 @dx.op.waveGetLaneIndex(i32 111)");
            state.used.insert("@dx.op.waveGetLaneIndex(");
            std::string target;
            if (offset & 0x8000) {
                // the quad permute: lane' = (lane & ~3) | ((off >> 2 * (lane & 3)) & 3)
                const auto in_quad = lowerer.integer(indent, "and", lane, "3");
                const auto shift = lowerer.integer(indent, "shl", in_quad, "1");
                const auto moved = lowerer.integer(indent, "lshr", std::to_string(offset & 0xFF),
                                                   shift);
                const auto picked = lowerer.integer(indent, "and", moved, "3");
                const auto base = lowerer.integer(indent, "and", lane, "-4");
                target = lowerer.integer(indent, "or", base, picked);
            } else {
                // the bit mask: ((lane & and) | or) ^ xor, inside a group of 32 lanes
                const auto first = lowerer.integer(indent, "and", lane, std::to_string(offset & 31));
                const auto second =
                    lowerer.integer(indent, "or", first, std::to_string((offset >> 5) & 31));
                const auto third =
                    lowerer.integer(indent, "xor", second, std::to_string((offset >> 10) & 31));
                const auto base = lowerer.integer(indent, "and", lane, "-32");
                target = lowerer.integer(indent, "or", base, third);
            }
            state.emit(indent + call.destination +
                       " = call i32 @dx.op.waveReadLaneAt.i32(i32 117, i32 " + value + ", i32 " +
                       target + ")");
            state.used.insert("@dx.op.waveReadLaneAt.i32(");
            ++state.calls;
            continue;
        }
        // the mask of the lanes that are active
        if (name == "Ballot64") {
            const auto condition = state.fresh();
            state.emit(indent + condition + " = icmp ne i32 " +
                       state.operand(call.arguments[0]).one + ", 0");
            const auto ballot = state.fresh();
            state.emit(indent + ballot + " = call %dx.types.fouri32 @dx.op.waveActiveBallot(i32 "
                                      "116, i1 " +
                       condition + ")");
            state.used.insert("@dx.op.waveActiveBallot(");
            const auto low = state.fresh(), high = state.fresh();
            state.emit(indent + low + " = extractvalue %dx.types.fouri32 " + ballot + ", 0");
            state.emit(indent + high + " = extractvalue %dx.types.fouri32 " + ballot + ", 1");
            state.pairs[call.destination] = {low, high};
            ++state.calls;
            continue;
        }
        // the value of one lane or the other, by the mask
        if (name == "V_CNDMASK_B32") {
            const auto mask = state.operand(call.arguments[0]);
            const auto values = state.operand(call.arguments[1]);
            const auto lane = state.fresh();
            state.emit(indent + lane + " = call i32 @dx.op.waveGetLaneIndex(i32 111)");
            state.used.insert("@dx.op.waveGetLaneIndex(");
            const auto high = state.fresh();
            state.emit(indent + high + " = icmp uge i32 " + lane + ", 32");
            const auto shift = lowerer.integer(indent, "and", lane, "31");
            const auto low = mask.parts.size() > 0 ? mask.parts[0] : "0";
            const auto upper = mask.parts.size() > 1 ? mask.parts[1] : "0";
            const auto low_word = lowerer.integer(indent, "lshr", low, shift);
            const auto high_word = lowerer.integer(indent, "lshr", upper, shift);
            const auto word = state.fresh();
            state.emit(indent + word + " = select i1 " + high + ", i32 " + high_word + ", i32 " +
                       low_word);
            const auto bit = lowerer.integer(indent, "and", word, "1");
            const auto taken = state.fresh();
            state.emit(indent + taken + " = icmp ne i32 " + bit + ", 0");
            const auto first = values.parts.size() > 0 ? values.parts[0] : "0";
            const auto second = values.parts.size() > 1 ? values.parts[1] : "0";
            state.emit(indent + call.destination + " = select i1 " + taken + ", i32 " + second +
                       ", i32 " + first);
            ++state.calls;
            continue;
        }
        // how many lanes below this one are set
        if (name == "MBCNT64") {
            const auto mask = state.operand(call.arguments[0]);
            const auto lane = state.fresh();
            state.emit(indent + lane + " = call i32 @dx.op.waveGetLaneIndex(i32 111)");
            state.used.insert("@dx.op.waveGetLaneIndex(");
            const auto high = state.fresh();
            state.emit(indent + high + " = icmp uge i32 " + lane + ", 32");
            const auto shift = lowerer.integer(indent, "and", lane, "31");
            const auto one = lowerer.integer(indent, "shl", "1", shift);
            const auto below = lowerer.integer(indent, "add", one, "-1");
            const auto low_mask = state.fresh(), high_mask = state.fresh();
            state.emit(indent + low_mask + " = select i1 " + high + ", i32 -1, i32 " + below);
            state.emit(indent + high_mask + " = select i1 " + high + ", i32 " + below + ", i32 0");
            const auto low = mask.parts.size() > 0 ? mask.parts[0] : "0";
            const auto upper = mask.parts.size() > 1 ? mask.parts[1] : "0";
            const auto low_bits = lowerer.integer(indent, "and", low, low_mask);
            const auto low_count = lowerer.popcount(indent, low_bits);
            const auto high_bits = lowerer.integer(indent, "and", upper, high_mask);
            const auto high_count = lowerer.popcount(indent, high_bits);
            state.emit(indent + call.destination + " = add i32 " + low_count + ", " + high_count);
            ++state.calls;
            continue;
        }
        // how many bits of the mask are set
        if (name == "S_BCNT1_U64") {
            const auto mask = state.operand(call.arguments[0]);
            const auto low = mask.parts.size() > 0 ? mask.parts[0] : "0";
            const auto upper = mask.parts.size() > 1 ? mask.parts[1] : "0";
            const auto low_count = lowerer.popcount(indent, low);
            const auto high_count = lowerer.popcount(indent, upper);
            state.emit(indent + call.destination + " = add i32 " + low_count + ", " + high_count);
            ++state.calls;
            continue;
        }
        // the minimum and the maximum of three
        if (name == "Min3_F32" || name == "Max3_F32") {
            const auto a = state.operand(call.arguments[0]).one;
            const auto b = state.operand(call.arguments[1]).one;
            const auto c = state.operand(call.arguments[2]).one;
            const int operation = name == "Min3_F32" ? 36 : 35;
            const auto first = lowerer.binary(indent, "f32", "float", a, b, operation);
            state.emit(indent + call.destination + " = call float @dx.op.binary.f32(i32 " +
                       std::to_string(operation) + ", float " + first + ", float " + c + ")");
            state.used.insert("@dx.op.binary.f32(");
            ++state.calls;
            continue;
        }
        if (name == "Min3_U32" || name == "Max3_U32" || name == "Med3_U32") {
            const auto a = state.operand(call.arguments[0]).one;
            const auto b = state.operand(call.arguments[1]).one;
            const auto c = state.operand(call.arguments[2]).one;
            state.used.insert("@dx.op.binary.i32(");
            const auto integer = [&](int operation, const std::string &first,
                                     const std::string &second, const std::string &into) {
                const auto result = into.empty() ? state.fresh() : into;
                state.emit(indent + result + " = call i32 @dx.op.binary.i32(i32 " +
                           std::to_string(operation) + ", i32 " + first + ", i32 " + second + ")");
                return result;
            };
            if (name == "Min3_U32") {
                integer(40, integer(40, a, b, ""), c, call.destination);
            } else if (name == "Max3_U32") {
                integer(39, integer(39, a, b, ""), c, call.destination);
            } else {
                // the median: max(min(a,b), min(max(a,b), c))
                const auto low = integer(40, a, b, "");
                const auto high = integer(39, a, b, "");
                integer(39, low, integer(40, high, c, ""), call.destination);
            }
            ++state.calls;
            continue;
        }
        // the bits of a mask, taken from two values
        if (name == "BFI") {
            const auto mask = state.operand(call.arguments[0]).one;
            const auto first = state.operand(call.arguments[1]).one;
            const auto second = state.operand(call.arguments[2]).one;
            const auto inverted = lowerer.integer(indent, "xor", mask, "-1");
            const auto kept = lowerer.integer(indent, "and", mask, first);
            const auto others = lowerer.integer(indent, "and", inverted, second);
            state.emit(indent + call.destination + " = or i32 " + kept + ", " + others);
            ++state.calls;
            continue;
        }
        // two values, joined by a byte count
        if (name == "V_ALIGNBYTE_B32") {
            const auto high = state.operand(call.arguments[0]).one;
            const auto low = state.operand(call.arguments[1]).one;
            const auto shift = state.operand(call.arguments[2]).one;
            const auto bytes = lowerer.integer(indent, "and", shift, "3");
            const auto amount = lowerer.integer(indent, "shl", bytes, "3");
            const auto right = lowerer.integer(indent, "lshr", low, amount);
            const auto not_zero = state.fresh();
            state.emit(indent + not_zero + " = icmp ne i32 " + amount + ", 0");
            const auto rest = lowerer.integer(indent, "sub", "32", amount);
            const auto place = lowerer.integer(indent, "and", rest, "31");
            const auto left = lowerer.integer(indent, "shl", high, place);
            const auto chosen = state.fresh();
            state.emit(indent + chosen + " = select i1 " + not_zero + ", i32 " + left + ", i32 0");
            state.emit(indent + call.destination + " = or i32 " + right + ", " + chosen);
            ++state.calls;
            continue;
        }
        // the minimum of the wave
        if (name == "WaveMin_U32") {
            state.emit(indent + call.destination +
                       " = call i32 @dx.op.waveActiveOp.i32(i32 119, i32 " +
                       state.operand(call.arguments[0]).one + ", i8 2, i8 1)");
            state.used.insert("@dx.op.waveActiveOp.i32(");
            ++state.calls;
            continue;
        }
        // a byte of a buffer
        if (name == "TypedLoad.i32" && call.arguments.size() == 3 &&
            state.operand(call.arguments[2]).one == "65") {
            const auto handle = state.operand(call.arguments[0]).one;
            const auto offset = state.operand(call.arguments[1]).one;
            const auto loaded = state.fresh();
            const auto place = lowerer.integer(indent, "and", offset, "-4");
            state.emit(indent + loaded + " = call %dx.types.ResRet.i32 "
                                      "@dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle " +
                         handle + ", i32 " + place + ", i32 undef, i8 1, i32 4)");
            state.used.insert("@dx.op.rawBufferLoad.i32(");
            const auto word = state.fresh();
            state.emit(indent + word + " = extractvalue %dx.types.ResRet.i32 " + loaded + ", 0");
            const auto byte = lowerer.integer(indent, "and", offset, "3");
            const auto times = lowerer.integer(indent, "shl", byte, "3");
            const auto shifted = lowerer.integer(indent, "lshr", word, times);
            state.bytes[call.destination] = shifted;
            ++state.calls;
            continue;
        }
        // an intrinsic this lowering does not know
        state.lines.push_back(line);
        left.push_back(name);
        continue;
    }
    calls = state.calls;
    std::string result = join_lines(state.lines);

    // the console's target and its compiler's options are not the host's
    const auto triple = result.find("target triple = \"dxil-ms-xb");
    if (triple != std::string::npos) {
        const auto end = result.find('"', triple + 17);
        result.replace(triple, end == std::string::npos ? result.size() - triple : end - triple + 1,
                       "target triple = \"dxil-ms-dx\"");
    }
    const auto options = result.find("!xbox.compiler.options = ");
    if (options != std::string::npos) {
        const auto end = result.find('\n', options);
        result.erase(options, end == std::string::npos ? result.size() - options : end - options + 1);
    }
    // the declarations of the intrinsics that are gone
    const auto called = [&result](const std::string &name) {
        for (const auto &line : split_lines(result)) {
            if (line.find(name) == std::string::npos)
                continue;
            if (line.find("call ") != std::string::npos)
                return true;
        }
        return false;
    };
    const auto lines = split_lines(result);
    std::vector<std::string> kept_lines;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto &line = lines[i];
        if (line.rfind("declare ", 0) == 0) {
            const auto at = line.find('@');
            const auto until = at == std::string::npos ? std::string::npos : line.find('(', at);
            if (at != std::string::npos && until != std::string::npos) {
                const auto name = line.substr(at, until - at);
                // the console's own intrinsics, and the one its compiler leaves in
                const bool console = name.rfind("@xbox.", 0) == 0 || name == "@llvm.copysign.f32";
                if (console && !called(name)) {
                    // the comment in front of it goes with it
                    if (!kept_lines.empty() &&
                        kept_lines.back().rfind("; Function Attrs:", 0) == 0)
                        kept_lines.pop_back();
                    // and the empty line behind it
                    if (i + 1 < lines.size() && trim(lines[i + 1]).empty())
                        ++i;
                    continue;
                }
            }
        }
        kept_lines.push_back(line);
    }
    const auto kept = join_lines(kept_lines);
    result = kept;
    // the declarations that are needed now
    std::string added;
    for (const auto &[name, declaration] : declarations()) {
        if (!state.used.count(name))
            continue;
        bool there = false;
        for (const auto &line : split_lines(result))
            if (line.rfind("declare ", 0) == 0 && line.find(name) != std::string::npos)
                there = true;
        if (there)
            continue;
        added += declaration + "\n";
    }
    if (!added.empty()) {
        auto at = result.find("declare ");
        if (at == std::string::npos)
            at = result.find("define ");
        if (at == std::string::npos)
            result += "\n" + added + "\n";
        else
            result.insert(at, added + "\n");
    }
    // the types the lowered text needs
    const auto add_type = [&result](const std::string &name, const std::string &type) {
        if (result.find(name) == std::string::npos)
            return;
        if (result.find(name + " = type") != std::string::npos)
            return;
        const auto at = result.find("%dx.types.Handle = type { i8* }\n");
        if (at == std::string::npos)
            return;
        result.insert(at + std::string("%dx.types.Handle = type { i8* }\n").size(),
                      name + " = type { " + type + " }\n");
    };
    add_type("%dx.types.fouri32", "i32, i32, i32, i32");
    add_type("%dx.types.ResRet.i32", "i32, i32, i32, i32, i32");
    add_type("%dx.types.ResourceProperties", "i32, i32");
    // the descriptor heaps are indexed
    if (state.used.count("@dx.op.createHandleFromHeap(")) {
        std::string out;
        bool done = false;
        for (auto &line : split_lines(result)) {
            if (!done && line.rfind('!', 0) == 0) {
                const auto at = line.find("!{i32 0, i64 ");
                if (at != std::string::npos) {
                    const auto start = at + 13;
                    auto end = start;
                    while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])))
                        ++end;
                    if (end > start) {
                        const auto flags = std::strtoull(line.substr(start, end - start).c_str(),
                                                          nullptr, 10) |
                                           (1ull << 30);
                        line = line.substr(0, start) + std::to_string(flags) + line.substr(end);
                        done = true;
                    }
                }
            }
            out += line + "\n";
        }
        result = out;
    }
    return result;
}

// The values of a function are numbered one after another; taking instructions out leaves
// holes, which the host's parser does not take. The numbers are given again, in order.
std::string renumber_function(const std::string &header, const std::string &body) {
    std::map<std::string, std::string> mapping;
    int next = 0, original = 0;
    const auto open = header.find('(');
    if (open != std::string::npos) {
        const auto close = bracket_end(header, open);
        if (close != std::string::npos) {
            const auto parameters = header.substr(open + 1, close - open - 1);
            for (auto &parameter : split_top(parameters, ',')) {
                if (trim(parameter).empty())
                    continue;
                const auto words = split_words(parameter);
                std::string name;
                for (const auto &word : words)
                    if (!word.empty() && word.front() == '%')
                        name = word;
                if (name.empty()) {
                    mapping[std::to_string(original)] = std::to_string(next++);
                    ++original;
                } else if (number(name.substr(1))) {
                    mapping[name.substr(1)] = std::to_string(next++);
                    original = std::max(original, std::atoi(name.c_str() + 1) + 1);
                } else if (words.size() < 2 || words.back() != name) {
                    mapping[std::to_string(original)] = std::to_string(next++);
                    ++original;
                }
            }
        }
    }
    // the lines of the function, without the comments that are not labels
    std::vector<std::string> lines;
    for (auto &line : split_lines(body)) {
        const auto trimmed = trim(line);
        if (trimmed.empty())
            continue;
        if (trimmed.front() == ';' && trimmed.rfind("; <label>:", 0) != 0)
            continue;
        lines.push_back(trimmed);
    }
    // the entry block has no label of its own when the first line is an instruction
    if (!lines.empty()) {
        const auto &first = lines.front();
        const bool label = first.rfind("; <label>:", 0) == 0 ||
                           (first.back() == ':' && number(first.substr(0, first.size() - 1))) ||
                           (first.back() == ':' && first.find(' ') == std::string::npos);
        if (!label) {
            mapping[std::to_string(original)] = std::to_string(next++);
            ++original;
        }
    }
    for (const auto &line : lines) {
        std::string key;
        if (line.rfind("; <label>:", 0) == 0) {
            auto rest = line.substr(10);
            std::size_t length = 0;
            while (length < rest.size() && std::isdigit(static_cast<unsigned char>(rest[length])))
                ++length;
            key = rest.substr(0, length);
        } else if (line.size() > 2 && line.back() == ':' && number(line.substr(0, line.size() - 1))) {
            key = line.substr(0, line.size() - 1);
        } else if (line.rfind("%", 0) == 0 && line.find(" = ") != std::string::npos) {
            const auto end = line.find(" = ");
            const auto name = line.substr(1, end - 1);
            if (number(name))
                key = name;
        }
        if (key.empty())
            continue;
        mapping[key] = std::to_string(next++);
    }
    // the numbers are given again
    const auto replace = [&mapping](const std::string &text, bool parameters_only) {
        std::string out;
        for (std::size_t i = 0; i < text.size();) {
            if (text[i] != '%') {
                out += text[i++];
                continue;
            }
            std::size_t end = i + 1;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
                ++end;
            if (end == i + 1) {
                out += text[i++];
                continue;
            }
            const auto key = text.substr(i + 1, end - i - 1);
            if (parameters_only) {
                // only the parameters of the header
                auto after = end;
                while (after < text.size() && std::isspace(static_cast<unsigned char>(text[after])))
                    ++after;
                const bool is_parameter = after < text.size() &&
                                          (text[after] == ',' || text[after] == ')') &&
                                          i > 0 && std::isspace(static_cast<unsigned char>(text[i - 1]));
                if (!is_parameter) {
                    out += text.substr(i, end - i);
                    i = end;
                    continue;
                }
            }
            const auto found = mapping.find(key);
            out += found != mapping.end() ? "%" + found->second : text.substr(i, end - i);
            i = end;
        }
        return out;
    };
    // the labels: "; <label>:48" and a line that is only "48:"
    const auto fix_labels = [&mapping](const std::string &text) {
        std::string out;
        for (auto &line : split_lines_kept(text)) {
            std::string changed = line;
            if (changed.rfind("; <label>:", 0) == 0) {
                auto rest = changed.substr(10);
                std::size_t length = 0;
                while (length < rest.size() && std::isdigit(static_cast<unsigned char>(rest[length])))
                    ++length;
                const auto found = mapping.find(rest.substr(0, length));
                if (found != mapping.end())
                    changed = "; <label>:" + found->second + rest.substr(length);
            } else {
                std::size_t end = 0;
                while (end < changed.size() && std::isdigit(static_cast<unsigned char>(changed[end])))
                    ++end;
                if (end > 0 && end + 1 == changed.size() && changed[end] == ':') {
                    const auto found = mapping.find(changed.substr(0, end));
                    if (found != mapping.end())
                        changed = found->second + ":";
                }
            }
            out += changed;
        }
        return out;
    };
    return replace(header, true) + fix_labels(replace(body, false));
}

std::string renumber(const std::string &text) {
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto begin = text.find("define ", at);
        if (begin == std::string::npos) {
            out += text.substr(at);
            break;
        }
        const auto brace = text.find('{', begin);
        if (brace == std::string::npos) {
            out += text.substr(at);
            break;
        }
        const auto end = text.find("\n}\n", brace);
        if (end == std::string::npos) {
            out += text.substr(at);
            break;
        }
        out += text.substr(at, begin - at);
        out += renumber_function(text.substr(begin, brace - begin + 1),
                                 text.substr(brace + 1, end - brace));
        at = end + 1;
    }
    return out;
}

// --- the compiler and the signer ------------------------------------------------------------
#if defined(_WIN32)
using LibraryHandle = HMODULE;
#else
using LibraryHandle = void *;
#endif

LibraryHandle open_library(const std::string &path) {
#if defined(_WIN32)
    const int size = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (size <= 1)
        return nullptr;
    std::wstring wide(static_cast<std::size_t>(size), 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), size);
    return LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

DxcCreateInstanceProc library_entry(LibraryHandle library, const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(library, name));
#else
    return reinterpret_cast<DxcCreateInstanceProc>(dlsym(library, name));
#endif
}

// The folder the program is in.
std::string program_folder() {
#if defined(_WIN32)
    wchar_t path[MAX_PATH]{};
    const auto size = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!size)
        return {};
    const std::wstring text(path, size);
    const auto at = text.find_last_of(L"\\/");
    if (at == std::wstring::npos)
        return {};
    return std::string(text.substr(0, at).begin(), text.substr(0, at).end());
#else
    char path[4096]{};
    const auto size = readlink("/proc/self/exe", path, sizeof path - 1);
    if (size <= 0)
        return {};
    const std::string text(path, static_cast<std::size_t>(size));
    const auto at = text.find_last_of('/');
    if (at == std::string::npos)
        return {};
    return text.substr(0, at);
#endif
}

// The folders the compiler and the signer may be in.
std::vector<std::string> library_folders(const char *variable) {
    std::vector<std::string> folders;
    if (const char *const given = std::getenv(variable))
        folders.emplace_back(given);
    if (const auto folder = program_folder(); !folder.empty()) {
        folders.push_back(folder);
#if !defined(_WIN32)
        folders.push_back(folder + "/../lib");
#endif
    }
    return folders;
}

// The shader compiler, as the host has it: dxcompiler.dll (libdxcompiler.so here).
DxcCreateInstanceProc compiler_factory() {
    static const auto create = []() -> DxcCreateInstanceProc {
#if defined(_WIN32)
        static const char *const name = "dxcompiler.dll";
#else
        static const char *const name = "libdxcompiler.so";
#endif
        for (const auto &folder : library_folders("XV_DXC_DIR")) {
            const auto handle = open_library(folder + "/" + name);
            if (!handle)
                continue;
            if (const auto entry = library_entry(handle, "DxcCreateInstance"))
                return entry;
        }
        if (const auto handle = open_library(name)) {
            if (const auto entry = library_entry(handle, "DxcCreateInstance"))
                return entry;
        }
        return nullptr;
    }();
    return create;
}

// The signer: dxil.dll (libdxil.so here).
DxcCreateInstanceProc signer_factory() {
    static const auto create = []() -> DxcCreateInstanceProc {
#if defined(_WIN32)
        static const char *const name = "dxil.dll";
#else
        static const char *const name = "libdxil.so";
#endif
        for (const auto &folder : library_folders("XV_DXIL_DIR")) {
            const auto handle = open_library(folder + "/" + name);
            if (!handle)
                continue;
            if (const auto entry = library_entry(handle, "DxcCreateInstance"))
                return entry;
        }
        if (const auto handle = open_library(name)) {
            if (const auto entry = library_entry(handle, "DxcCreateInstance"))
                return entry;
        }
        return nullptr;
    }();
    return create;
}

// A word of a buffer.
std::uint32_t word_of(std::span<const std::uint8_t> bytes, std::size_t at) {
    if (at + 4 > bytes.size())
        return 0;
    return std::uint32_t(bytes[at]) | std::uint32_t(bytes[at + 1]) << 8 |
           std::uint32_t(bytes[at + 2]) << 16 | std::uint32_t(bytes[at + 3]) << 24;
}

// The bitcode of a console shader: the DXIL part of its container holds it.
std::optional<std::vector<std::uint8_t>> container_bitcode(
    std::span<const std::uint8_t> container) {
    if (container.size() < 32 || std::memcmp(container.data(), "DXBC", 4))
        return std::nullopt;
    const auto count = word_of(container, 28);
    for (std::uint32_t i = 0; i < count && 36 + 4 * i <= container.size(); ++i) {
        const auto offset = word_of(container, 32 + 4 * i);
        if (offset + 8 > container.size() || std::memcmp(container.data() + offset, "DXIL", 4))
            continue;
        const auto size = word_of(container, offset + 4);
        if (size > container.size() - offset - 8)
            return std::nullopt;
        const std::size_t part = offset + 8;
        if (part + 24 > container.size())
            return std::nullopt;
        // the console's header: the bitcode is at eight bytes past what it says
        const auto bitcode = part + 8 + word_of(container, part + 16);
        const auto length = word_of(container, part + 20);
        if (length == 0 || bitcode + length > container.size())
            return std::nullopt;
        static constexpr std::uint8_t magic[] = {0x42, 0x43, 0xc0, 0xde};
        if (std::memcmp(container.data() + bitcode, magic, 4))
            return std::nullopt;
        return std::vector<std::uint8_t>(container.begin() + bitcode,
                                         container.begin() + bitcode + length);
    }
    return std::nullopt;
}

// The text of a bitcode.
std::optional<std::string> disassemble(const std::vector<std::uint8_t> &bitcode,
                                       std::string *why) {
    const auto create = compiler_factory();
    if (!create) {
        if (why)
            *why = "the shader compiler is not there";
        return std::nullopt;
    }
    IDxcCompiler *compiler = nullptr;
    if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || !compiler) {
        if (why)
            *why = "the shader compiler could not be made";
        return std::nullopt;
    }
    IDxcLibrary *library = nullptr;
    if (FAILED(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) || !library) {
        compiler->Release();
        if (why)
            *why = "the compiler's library could not be made";
        return std::nullopt;
    }
    IDxcBlobEncoding *blob = nullptr, *text = nullptr;
    std::string result;
    if (FAILED(library->CreateBlobWithEncodingFromPinned(bitcode.data(),
                                                         static_cast<UINT32>(bitcode.size()),
                                                         65001, &blob)) ||
        !blob) {
        if (why)
            *why = "the bitcode could not be read";
    } else if (FAILED(compiler->Disassemble(blob, &text)) || !text) {
        if (why)
            *why = "the bitcode could not be taken apart";
    } else {
        result.assign(static_cast<const char *>(text->GetBufferPointer()), text->GetBufferSize());
    }
    if (text)
        text->Release();
    if (blob)
        blob->Release();
    library->Release();
    compiler->Release();
    if (result.empty())
        return std::nullopt;
    return result;
}

// The container of a lowered DXIL.
std::optional<std::vector<std::uint8_t>> assemble(const std::string &text, std::string *why) {
    const auto create = compiler_factory();
    if (!create) {
        if (why)
            *why = "the shader compiler is not there";
        return std::nullopt;
    }
    IDxcLibrary *library = nullptr;
    IDxcAssembler *assembler = nullptr;
    if (FAILED(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) || !library ||
        FAILED(create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler))) || !assembler) {
        if (library)
            library->Release();
        if (why)
            *why = "the assembler could not be made";
        return std::nullopt;
    }
    IDxcBlobEncoding *source = nullptr;
    IDxcOperationResult *result = nullptr;
    std::vector<std::uint8_t> container;
    if (FAILED(library->CreateBlobWithEncodingFromPinned(text.data(),
                                                         static_cast<UINT32>(text.size()), 65001,
                                                         &source)) ||
        !source) {
        if (why)
            *why = "the text could not be read";
    } else if (FAILED(assembler->AssembleToContainer(source, &result)) || !result) {
        if (why)
            *why = "the text could not be made into a shader";
    } else {
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        IDxcBlob *blob = nullptr;
        result->GetResult(&blob);
        if (FAILED(status) || !blob) {
            IDxcBlobEncoding *error = nullptr, *text8 = nullptr;
            result->GetErrorBuffer(&error);
            if (error && library->GetBlobAsUtf8(error, &text8) && text8 && why)
                *why = std::string(static_cast<const char *>(text8->GetBufferPointer()),
                                   text8->GetBufferSize());
            if (why && why->empty())
                *why = "the text could not be made into a shader";
        } else {
            const auto *const bytes = static_cast<const std::uint8_t *>(blob->GetBufferPointer());
            container.assign(bytes, bytes + blob->GetBufferSize());
            blob->Release();
        }
    }
    if (result)
        result->Release();
    if (source)
        source->Release();
    assembler->Release();
    library->Release();
    if (container.empty())
        return std::nullopt;
    return container;
}

// Whether the signer takes the container, and what it says when it does not.
bool sign(std::vector<std::uint8_t> &container, std::string *why) {
    const auto create = signer_factory();
    if (!create) {
        if (why)
            *why = "the signer is not there";
        return false;
    }
    IDxcValidator *validator = nullptr;
    if (FAILED(create(CLSID_DxcValidator, IID_PPV_ARGS(&validator))) || !validator) {
        if (why)
            *why = "the signer could not be made";
        return false;
    }
    // the blobs come from the compiler's library; the validator is the signer's
    const auto compiler = compiler_factory();
    IDxcLibrary *library = nullptr;
    IDxcBlobEncoding *blob = nullptr;
    IDxcOperationResult *result = nullptr;
    bool signed_ok = false;
    if (!compiler || FAILED(compiler(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) || !library) {
        if (why)
            *why = "the signer's library could not be made";
    } else if (FAILED(library->CreateBlobWithEncodingFromPinned(container.data(),
                                                                static_cast<UINT32>(container.size()),
                                                                65001, &blob)) ||
               !blob) {
        if (why)
            *why = "the shader could not be read";
    } else {
        HRESULT status = E_FAIL;
        if (FAILED(validator->Validate(blob, DxcValidatorFlags_InPlaceEdit, &result)) || !result) {
            if (why)
                *why = "the signer did not answer";
        } else {
            result->GetStatus(&status);
            if (FAILED(status)) {
                IDxcBlobEncoding *error = nullptr, *text8 = nullptr;
                result->GetErrorBuffer(&error);
                if (error && library->GetBlobAsUtf8(error, &text8) && text8 && why)
                    *why = std::string(static_cast<const char *>(text8->GetBufferPointer()),
                                       text8->GetBufferSize());
                if (why && why->empty())
                    *why = "the signer turned the shader down";
            } else {
                signed_ok = true;
            }
        }
    }
    if (result)
        result->Release();
    if (blob)
        blob->Release();
    if (library)
        library->Release();
    validator->Release();
    return signed_ok;
}
} // namespace

// --- the shader model 6.6 ------------------------------------------------------------------
//
// The lowering makes handles of the descriptor heaps, and the host's compiler only takes them
// in shader model 6.6, where a handle is made of a binding instead of of a resource. This is
// the part of americas-recomp's `tools/xbox/platform/dxil_sm66_wave.py` that has to do with
// that (the wave size is not pinned: the host's waves are 32 lanes wide).

// One resource a shader binds.
struct Resource {
    int space = 0;
    int lower = 0;
    int count = 1;
    int kind = 0;
    int stride = 0;
};

// The metadata of a text: what every "!N" holds.
std::map<std::string, std::string> metadata(const std::string &text) {
    std::map<std::string, std::string> nodes;
    for (const auto &line : split_lines(text)) {
        if (line.empty() || line.front() != '!')
            continue;
        const auto at = line.find(" = ");
        if (at == std::string::npos)
            continue;
        nodes[line.substr(0, at)] = trim(line.substr(at + 3));
    }
    return nodes;
}

// The contents of a metadata node: what is between its brackets.
std::string node_content(const std::string &value) {
    if (value.size() < 2 || value.front() != '{')
        return {};
    const auto close = bracket_end(value, 0);
    if (close == std::string::npos)
        return {};
    return value.substr(1, close - 1);
}

// The number a field of a resource holds.
int field_number(const std::string &field) {
    const auto words = split_words(field);
    if (words.size() < 2)
        return 0;
    return std::atoi(words[1].c_str());
}

// The resources a shader binds, by class and range, and the size of its constant buffer.
std::map<std::pair<int, int>, Resource> resources_of(const std::string &text, int &cbuffer_size) {
    const auto nodes = metadata(text);
    std::map<std::pair<int, int>, Resource> resources;
    cbuffer_size = 0;
    auto top = nodes.find("!dx.resources");
    if (top == nodes.end())
        return resources;
    const auto lists = split_top(node_content(top->second), ',');
    for (std::size_t cls = 0; cls < lists.size(); ++cls) {
        const auto list = trim(lists[cls]);
        if (list.empty() || list.front() != '!')
            continue;
        const auto found = nodes.find(list);
        if (found == nodes.end())
            continue;
        for (auto &entry : split_top(node_content(found->second), ',')) {
            const auto name = trim(entry);
            if (name.empty() || name.front() != '!')
                continue;
            const auto one = nodes.find(name);
            if (one == nodes.end())
                continue;
            const auto fields = split_top(node_content(one->second), ',');
            if (fields.size() < 7)
                continue;
            Resource resource;
            const auto range = field_number(fields[0]);
            resource.space = field_number(fields[3]);
            resource.lower = field_number(fields[4]);
            resource.count = field_number(fields[5]);
            resource.kind = field_number(fields[6]);
            // the stride is in the node the last field names
            const auto last = trim(fields.back());
            if (!last.empty() && last.front() == '!') {
                const auto stride = nodes.find(last);
                if (stride != nodes.end()) {
                    const auto values = split_top(node_content(stride->second), ',');
                    if (values.size() >= 2)
                        resource.stride = field_number(values[1]);
                }
            }
            if (resource.kind == 4 && cbuffer_size == 0)
                cbuffer_size = field_number(fields.back());
            resources[{static_cast<int>(cls), range}] = resource;
        }
    }
    return resources;
}

// The text as shader model 6.6: the handles are made of the bindings.
std::string to_shader_model_66(const std::string &text) {
    int cbuffer_size = 16;
    const auto resources = resources_of(text, cbuffer_size);
    std::string out;
    bool has_res_bind = false, has_properties = false, has_annotate = false;
    for (const auto &line : split_lines(text)) {
        // the version and the shader model
        auto changed = line;
        const auto version = changed.find("!{i32 1, i32 5}");
        if (version != std::string::npos && changed.rfind("!dx.version", 0) == 0)
            changed.replace(version, 13, "!{i32 1, i32 6}");
        const auto model = changed.find("!{!\"cs\", i32 6, i32 5}");
        if (model != std::string::npos && changed.rfind("!dx.shaderModel", 0) == 0)
            changed.replace(model, 21, "!{!\"cs\", i32 6, i32 6}");
        // a handle of a resource
        static const std::string pattern = "call %dx.types.Handle @dx.op.createHandle(i32 57, i8 ";
        const auto at = changed.find(pattern);
        if (at == std::string::npos) {
            out += changed + "\n";
            continue;
        }
        const auto rest = changed.substr(at + pattern.size());
        const auto parts = split_top(rest, ',');
        if (parts.size() < 5) {
            out += changed + "\n";
            continue;
        }
        const auto cls = std::atoi(trim(parts[0]).c_str());
        const auto range = std::atoi(trim(parts[1]).substr(3).c_str());
        const auto index = trim(parts[2]).substr(3);
        const auto non_uniform = trim(parts[3]);
        const auto found = resources.find({cls, range});
        const int space = found != resources.end() ? found->second.space : 0;
        const int lower = found != resources.end() ? found->second.lower : range;
        const int count = found != resources.end() ? found->second.count : 1;
        const int kind = found != resources.end() ? found->second.kind : 0;
        const int stride = found != resources.end() ? found->second.stride : 0;
        const int upper = count > 0 ? lower + count - 1 : -1;
        const auto name = changed.substr(0, at);
        const auto name_at = name.find_last_of('%');
        const auto number = name_at == std::string::npos ? std::string()
                                                         : name.substr(name_at + 1);
        const auto indent = changed.substr(0, changed.find_first_not_of(" \t"));
        const auto handle = "%hraw." + number;
        std::string properties;
        if (cls == 2) {
            properties = "{ i32 13, i32 " + std::to_string(cbuffer_size) + " }";
        } else {
            const int value = kind | (cls == 1 ? 0x1000 : 0);
            properties = "{ i32 " + std::to_string(value) + ", i32 " +
                         std::to_string((kind & 0xFF) == 12 ? stride : 0) + " }";
        }
        out += indent + handle + " = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 "
               "217, %dx.types.ResBind { i32 " + std::to_string(lower) + ", i32 " +
               std::to_string(upper) + ", i32 " + std::to_string(space) + ", i8 " +
               std::to_string(cls) + " }, i32 " + index + ", i1 " + non_uniform + ")\n";
        out += indent + number + " = call %dx.types.Handle @dx.op.annotateHandle(i32 216, "
               "%dx.types.Handle " + handle + ", %dx.types.ResourceProperties " + properties +
               ")";
        has_res_bind = true;
        has_properties = true;
        has_annotate = true;
    }
    // the declarations that go with it
    std::string declarations;
    if (has_res_bind)
        declarations +=
            "; Function Attrs: nounwind readnone\n"
            "declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, "
            "i32, i1) #0\n\n";
    if (has_annotate &&
        out.find("declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, "
                 "%dx.types.ResourceProperties)") == std::string::npos)
        declarations +=
            "; Function Attrs: nounwind readnone\n"
            "declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, "
            "%dx.types.ResourceProperties) #0\n\n";
    if (!declarations.empty()) {
        auto at = out.find("declare ");
        if (at == std::string::npos)
            at = out.find("define ");
        if (at == std::string::npos)
            out += "\n" + declarations + "\n";
        else
            out.insert(at, declarations);
    }
    // the types that go with it
    if (has_res_bind && out.find("%dx.types.ResBind = type") == std::string::npos) {
        const auto at = out.find("%dx.types.Handle = type { i8* }\n");
        if (at != std::string::npos)
            out.insert(at + std::string("%dx.types.Handle = type { i8* }\n").size(),
                       "%dx.types.ResBind = type { i32, i32, i32, i8 }\n");
    }
    if (has_properties && out.find("%dx.types.ResourceProperties = type") == std::string::npos) {
        const auto at = out.find("%dx.types.Handle = type { i8* }\n");
        if (at != std::string::npos)
            out.insert(at + std::string("%dx.types.Handle = type { i8* }\n").size(),
                       "%dx.types.ResourceProperties = type { i32, i32 }\n");
    }
    // the declaration of the handles of resources is not wanted any more
    const auto declare = out.find("declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, "
                                  "i32, i1)");
    if (declare != std::string::npos) {
        const auto end = out.find('\n', declare);
        out.erase(declare, end == std::string::npos ? out.size() - declare : end - declare + 1);
    }
    return out;
}

std::optional<XboxDxilText> lower_xbox_dxil_text(const std::string &text) {
    XboxDxilText result;
    std::vector<std::string> left;
    result.lowered = to_shader_model_66(renumber(lower_text(text, result.calls, left)));
    std::set<std::string> unique(left.begin(), left.end());
    result.left.assign(unique.begin(), unique.end());
    return result;
}

std::optional<std::string> xbox_dxil_text(std::span<const std::uint8_t> container,
                                          std::string *why) {
    const auto bitcode = container_bitcode(container);
    if (!bitcode) {
        if (why)
            *why = "the container holds no DXIL";
        return std::nullopt;
    }
    return disassemble(*bitcode, why);
}

bool xbox_dxil_has_intrinsics(std::span<const std::uint8_t> container) {
    const auto bitcode = container_bitcode(container);
    if (!bitcode)
        return false;
    std::string why;
    const auto text = disassemble(*bitcode, &why);
    if (!text)
        return false;
    return text->find("@xbox.") != std::string::npos;
}

std::optional<XboxDxilLowering> lower_xbox_dxil(std::span<const std::uint8_t> container,
                                                std::string *why) {
    XboxDxilLowering result;
    const auto bitcode = container_bitcode(container);
    if (!bitcode) {
        if (why)
            *why = "the container holds no DXIL";
        return std::nullopt;
    }
    std::string said;
    const auto text = disassemble(*bitcode, &said);
    if (!text) {
        if (why)
            *why = said.empty() ? "the DXIL could not be taken apart" : said;
        return std::nullopt;
    }
    result.text = *text;
    if (result.text.find("@xbox.") == std::string::npos) {
        if (why)
            *why = "the DXIL calls none of the console's intrinsics";
        return std::nullopt;
    }
    const auto lowered = lower_xbox_dxil_text(result.text);
    if (!lowered) {
        if (why)
            *why = "the DXIL could not be lowered";
        return std::nullopt;
    }
    result.lowered = lowered->lowered;
    result.calls = lowered->calls;
    result.left = lowered->left;
    if (!result.left.empty()) {
        if (why)
            *why = "the console's intrinsics that are still called:";
        for (const auto &name : result.left)
            if (why)
                *why += " " + name;
        return std::nullopt;
    }
    auto container_bytes = assemble(result.lowered, &said);
    if (!container_bytes) {
        if (why)
            *why = said.empty() ? "the lowered DXIL could not be made into a shader" : said;
        return std::nullopt;
    }
    result.signed_ok = sign(*container_bytes, &said);
    if (!result.signed_ok) {
        if (why)
            *why = said.empty() ? "the signer turned the shader down" : said;
        return std::nullopt;
    }
    result.dxil = std::move(*container_bytes);
    return result;
}
} // namespace xv::runtime::modules

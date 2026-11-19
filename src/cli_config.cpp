#include "cli_config.hpp"
#include <cwctype>
#include <sstream>

namespace studio {
namespace {
enum class Kind { text, number, flag, array, object };
struct Field { const char* key; Kind kind; };
using enum Kind;
constexpr Field fields[]{
    {"command", text}, {"mode", text}, {"source", text}, {"sources", array}, {"character", text},
    {"output", text}, {"styles", array}, {"instruction", text}, {"count", number}, {"crop", object},
    {"region", object}, {"stride", number}, {"max_frames", number}, {"consistency", flag},
    {"preserve_text", flag}, {"resume", flag}, {"base_url", text}, {"api_key_env", text}, {"model", text},
    {"quality", text}, {"size", text}, {"complexity", text}, {"output_format", text},
    {"output_compression", number}, {"stream", flag}, {"partial_images", number}, {"image_field", text},
    {"timeout_seconds", number}, {"extra", object}, {"plugins", array}, {"plugin_directory", text},
    {"preview_file", text}, {"progress_file", text}, {"routing_report", text}, {"api_routing", object},
    {"upscale", object}, {"plugin_id", text}, {"plugin_command", text}, {"context", object},
    {"probe_run", flag}, {"text_only", flag}, {"cases", array}, {"channel", number}
};
const Field& field(const std::string& key) {
    for (const auto& item : fields) if (key == item.key) return item;
    throw Error("Unknown option or configuration field: " + key);
}
bool matches(const Json& value, Kind kind) {
    switch (kind) {
    case text: return value.is_string();
    case number: return value.is_number_integer() && (value.is_number_unsigned() ?
        value.get<uint64_t>() <= INT_MAX : value.get<int64_t>() >= INT_MIN && value.get<int64_t>() <= INT_MAX);
    case flag: return value.is_boolean();
    case array: return value.is_array();
    case object: return value.is_object();
    }
    return false;
}
std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
bool identifier(const std::string& value) {
    return !value.empty() && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
        std::string::npos;
}
Json typedValue(const std::string& value, Kind kind, const std::string& key, bool ini = false) {
    if (kind == text && (!ini || !value.starts_with('"'))) return value;
    auto result = Json::parse(value, nullptr, false);
    if (!matches(result, kind)) throw Error("Invalid value for " + key);
    return result;
}
Json readIni(std::string data) {
    if (data.starts_with("\xef\xbb\xbf")) data.erase(0, 3);
    Json result = Json::object();
    std::string section;
    std::istringstream input(std::move(data));
    int lineNo = 0;
    for (std::string raw; std::getline(input, raw);) {
        ++lineNo;
        auto line = trim(raw);
        if (line.empty() || line.starts_with('#') || line.starts_with(';')) continue;
        try {
            if (line.starts_with('[') && line.ends_with(']')) {
                section = trim(line.substr(1, line.size() - 2));
                if (section == "job") section.clear();
                else if (field(section).kind != object) throw Error("Section must name an object field");
                continue;
            }
            auto equals = line.find('=');
            if (equals == std::string::npos) throw Error("Expected key=value");
            auto key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
            if (!identifier(key)) throw Error("Invalid key");
            if (section.empty()) {
                if (result.contains(key)) throw Error("Duplicate key: " + key);
                result[key] = typedValue(value, field(key).kind, key, true);
                continue;
            }
            auto& target = result[section];
            if (target.is_null()) target = Json::object();
            if (target.contains(key)) throw Error("Duplicate key: " + section + "." + key);
            auto parsed = Json::parse(value, nullptr, false);
            if (parsed.is_discarded() && (value.starts_with('"') || value.starts_with('[') || value.starts_with('{')))
                throw Error("Invalid JSON value for " + key);
            target[key] = parsed.is_discarded() ? Json(value) : std::move(parsed);
        } catch (const std::exception& error) {
            throw Error("INI line " + std::to_string(lineNo) + ": " + error.what());
        }
    }
    return result;
}
void validate(const Json& config) {
    if (!config.is_object()) throw Error("Task configuration must be an object");
    for (const auto& [key, value] : config.items())
        if (!matches(value, field(key).kind)) throw Error("Invalid configuration value for " + key);
    for (auto key : {"plugins", "styles", "sources"}) if (config.contains(key))
        for (const auto& value : config.at(key)) if (!value.is_string() || value.get_ref<const std::string&>().empty())
            throw Error(std::string("Expected non-empty string entries in ") + key);
    if (config.contains("cases")) for (const auto& value : config.at("cases"))
        if (!matches(value, number)) throw Error("Probe case indices must be integers");
}
Json readConfig(const fs::path& path) {
    auto bytes = readFile(path, 1024 * 1024);
    auto extension = path.extension().wstring();
    for (auto& c : extension) c = wchar_t(towlower(c));
    Json result;
    if (extension == L".json" || extension == L".jsonc") {
        bool relaxed = extension == L".jsonc";
        result = Json::parse(bytes.begin(), bytes.end(), nullptr, false, relaxed);
        if (result.is_discarded()) throw Error("Invalid JSON or JSONC task configuration");
    } else if (extension == L".ini") result = readIni(std::string(bytes.begin(), bytes.end()));
    else throw Error("Supported task file extensions: .json, .jsonc, .ini");
    validate(result);
    const auto pathValue = [&](Json& value) {
        auto name = value.get<std::string>();
        if (name.empty()) return;
        auto candidate = fs::path(wide(name));
        if (candidate.is_relative()) value = utf8((path.parent_path() / candidate).lexically_normal().wstring());
    };
    for (auto key : {"source", "character", "output", "plugin_directory", "preview_file", "progress_file", "routing_report"})
        if (result.contains(key)) pathValue(result[key]);
    for (auto key : {"styles", "sources"}) if (result.contains(key))
        for (auto& value : result[key]) pathValue(value);
    if (result.contains("upscale")) for (auto key : {"executable", "models"})
        if (result["upscale"].contains(key)) pathValue(result["upscale"][key]);
    return result;
}
}
CliOptions cliOptions(int argc, wchar_t** argv) {
    CliOptions result;
    int i = 1;
    auto argument = [&] {
        if (i >= argc) throw Error("Missing command or option value");
        return utf8(argv[i++]);
    };
    if (i == argc) { result.command = "help"; return result; }
    if (std::wstring_view(argv[i]) == L"--help" || std::wstring_view(argv[i]) == L"-h") {
        result.command = "help"; return result;
    }
    result.command = std::wstring_view(argv[i]).starts_with(L"--") ? "run" : argument();
    if (result.command == "plugin" && i < argc && !std::wstring_view(argv[i]).starts_with(L"--")) {
        result.plugin = argument();
        result.action = argument();
    }
    Json overrides = Json::object(), addedPlugins = Json::array();
    while (i < argc) {
        auto option = argument();
        if (option == "--help" || option == "-h") { result.command = "help"; return result; }
        if (!option.starts_with("--")) throw Error("Unexpected positional argument");
        auto equal = option.find('=');
        auto value = [&] {
            if (equal != std::string::npos) return option.substr(equal + 1);
            if (i == argc || std::wstring_view(argv[i]).starts_with(L"--"))
                throw Error("Missing option value for " + option);
            return argument();
        };
        auto key = option.substr(2, equal == std::string::npos ? equal : equal - 2);
        if (key == "config") {
            if (!result.configFile.empty()) throw Error("Use only one --config file per command");
            result.configFile = fs::absolute(fs::path(wide(value())));
        } else if (key == "quiet" && equal == std::string::npos) result.quiet = true;
        else if (key == "load") addedPlugins.push_back(value());
        else if (key == "style" || key == "input") {
            auto name = key == "style" ? "styles" : "sources";
            overrides[name].push_back(value());
        } else {
            if (key == "prompt") key = "instruction";
            if (key == "format") key = "output-format";
            if (key == "compression") key = "output-compression";
            bool disabled = key.starts_with("no-");
            if (disabled) key.erase(0, 3);
            for (auto& c : key) if (c == '-') c = '_';
            auto kind = field(key).kind;
            if (disabled && (kind != flag || equal != std::string::npos))
                throw Error("Invalid negative flag: " + option);
            if (kind == flag && equal == std::string::npos) overrides[key] = !disabled;
            else overrides[key] = typedValue(value(), kind, key);
        }
    }
    if (!result.configFile.empty()) result.config = readConfig(result.configFile);
    result.config.update(overrides);
    for (const auto& id : addedPlugins) result.config["plugins"].push_back(id);
    validate(result.config);
    if (result.command == "run") {
        result.command = result.config.value("command", result.config.contains("sources") ? "batch" :
            result.config.value("mode", "replace"));
    }
    if (result.command == "plugin") {
        if (result.plugin.empty()) result.plugin = result.config.value("plugin_id", "");
        if (result.action.empty()) result.action = result.config.value("plugin_command", "");
        if (result.plugin.empty() || result.action.empty()) throw Error("Specify plugin ID and command ID");
    }
    return result;
}
}

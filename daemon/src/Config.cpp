#include "kitty_a2a/Config.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;

namespace kitty_a2a {

std::optional<std::string> Config::default_agent_for(const std::string& cwd) const {
    // Longest matching path wins (DESIGN.md §13). A project path P matches cwd
    // if cwd == P or cwd starts with P + "/". Among matches, the longest P is
    // chosen.
    const std::string c = fs::weakly_canonical(cwd).string();
    std::string best_path;
    std::string best_agent;
    for (const auto& rule : projects) {
        const std::string p = fs::weakly_canonical(rule.path).string();
        const bool exact = (c == p);
        const bool under = c.size() > p.size() && c.compare(0, p.size() + 1, p + "/") == 0;
        if (exact || under) {
            if (p.size() > best_path.size()) {
                best_path = p;
                best_agent = rule.default_agent;
            }
        }
    }
    if (best_agent.empty()) return std::nullopt;
    return best_agent;
}

static std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    if (v && *v) return v;
    return fallback;
}

// Expand "${VAR:-default}" (and plain "${VAR}") substitutions. Any other
// "${...}" is left untouched. This is deliberately minimal — it only supports
// the single form used by the shipped example config, not a full shell.
static std::string expand_env_substitution(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
            size_t close = in.find('}', i + 2);
            if (close != std::string::npos) {
                std::string inner = in.substr(i + 2, close - (i + 2));
                std::string var = inner;
                std::string def;
                auto colon = inner.find(":-");
                if (colon != std::string::npos) {
                    var = inner.substr(0, colon);
                    def = inner.substr(colon + 2);
                }
                const char* ev = std::getenv(var.c_str());
                out += (ev && *ev) ? ev : def;
                i = close + 1;
                continue;
            }
        }
        out += in[i];
        ++i;
    }
    return out;
}

Paths Paths::resolve() {
    Paths p;
    std::string home = env_or("HOME", "/tmp");

    const std::string cfg = env_or("XDG_CONFIG_HOME", home + "/.config");
    const std::string state = env_or("XDG_STATE_HOME", home + "/.local/state");
    const std::string runtime = env_or("XDG_RUNTIME_DIR", "");

    p.config_dir = fs::path(cfg) / "kitty-a2a";
    p.state_dir = fs::path(state) / "kitty-a2a";

    // XDG_RUNTIME_DIR is per-session and guaranteed 0700 owned by the user.
    // When it is absent we fall back to a per-user, strictly-permissioned temp
    // dir so we never expose the socket on a shared world-writable path
    // (DESIGN.md §5: "Fall back safely when XDG_RUNTIME_DIR is unavailable").
    if (!runtime.empty()) {
        p.runtime_dir = fs::path(runtime) / "kitty-a2a";
    } else {
        p.runtime_dir = fs::temp_directory_path() / ("kitty-a2a-" + std::to_string(::getuid()));
    }

    p.agents_yaml = (fs::path(p.config_dir) / "agents.yaml").string();
    p.db = (fs::path(p.state_dir) / "a2ad.db").string();
    p.socket = (fs::path(p.runtime_dir) / "a2ad.sock").string();
    return p;
}

Config load_config(const std::string& path, bool require, std::string* error) {
    Config cfg;
    fs::path p(path);
    if (!fs::exists(p)) {
        if (require) {
            if (error) *error = "config file not found: " + path;
            return cfg;
        }
        return cfg;  // no agents; not an error
    }

    try {
        YAML::Node root = YAML::LoadFile(path);

        if (root["agents"]) {
            for (const auto& kv : root["agents"]) {
                if (!kv.second || !kv.second["endpoint"]) continue;
                AgentConfig ac;
                ac.endpoint = kv.second["endpoint"].as<std::string>();
                if (kv.second["host_label"]) ac.host_label = kv.second["host_label"].as<std::string>();
                if (kv.second["description"]) ac.description = kv.second["description"].as<std::string>();
                if (kv.second["auth"]) {
                    const auto& a = kv.second["auth"];
                    if (a["type"]) ac.auth_type = a["type"].as<std::string>();
                    if (a["name"]) ac.auth_name = a["name"].as<std::string>();
                }
                cfg.agents[kv.first.as<std::string>()] = ac;
            }
        }

        if (root["projects"]) {
            for (const auto& kv : root["projects"]) {
                if (!kv.second || !kv.second["default_agent"]) continue;
                ProjectRule pr;
                pr.path = kv.first.as<std::string>();
                pr.default_agent = kv.second["default_agent"].as<std::string>();
                cfg.projects.push_back(pr);
            }
        }

        if (root["ipc"]) {
            const auto& ipc = root["ipc"];
            if (ipc["socket"]) {
                // Support the "${VAR:-default}" form used in the example config.
                cfg.ipc.socket = expand_env_substitution(ipc["socket"].as<std::string>());
            }
            if (ipc["db"]) {
                cfg.ipc.db = expand_env_substitution(ipc["db"].as<std::string>());
            }
            if (ipc["reconcile_interval_sec"]) {
                cfg.ipc.reconcile_interval_sec = ipc["reconcile_interval_sec"].as<int>();
            }
        }
    } catch (const std::exception& e) {
        if (error) *error = std::string("failed to parse config: ") + e.what();
        return Config{};
    }
    return cfg;
}

}  // namespace kitty_a2a

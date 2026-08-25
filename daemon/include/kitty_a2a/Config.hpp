#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kitty_a2a {

// Configuration loaded from YAML (DESIGN.md §6, §13, §20).
//
// agents.yaml:
//   agents:
//     cpp-specialist:
//       endpoint: https://dev01.internal/a2a
//   projects:                 # optional, longest-path-wins (DESIGN.md §13)
//     /data/projects/proto-time:
//       default_agent: emulator-agent
//
// Credential values are NEVER expected in this file. `auth` entries name a
// credential source (env var or provider) that the CredentialProvider resolves
// at request time; the daemon never stores the secret in config (DESIGN.md §20).
struct AgentConfig {
    std::string endpoint;              // required
    std::string host_label;            // optional; short label for the picker
    // Optional auth reference. `type` is one of: "none" (default), "env",
    // "credential-file". `name` is the env var name or the file path. The
    // daemon reads the value via CredentialProvider — it is not cached here.
    std::string auth_type = "none";
    std::string auth_name;

    // Optional static metadata used only until the Agent Card is discovered.
    std::string description;
};

struct ProjectRule {
    std::string path;                  // absolute directory prefix
    std::string default_agent;
};

struct IpcConfig {
    // Explicit socket path override. If empty, the XDG default (Paths::resolve)
    // is used. Useful for tests and multi-daemon setups. Supports the
    // "${VAR:-default}" env substitution form used in the example config.
    std::string socket;
    // Explicit SQLite database path override. If empty, the XDG default is used.
    // Useful for tests and multi-daemon setups (isolated state).
    std::string db;
    // Poll non-terminal tasks every N seconds and broadcast state changes.
    // 0 disables the reconcile loop (one-shot reconciliation at startup still runs).
    int reconcile_interval_sec = 0;
};

struct Config {
    std::map<std::string, AgentConfig> agents;
    std::vector<ProjectRule> projects; // kept as-is; matched at routing time
    IpcConfig ipc;

    // Longest matching project path for cwd; returns the default_agent id, or
    // std::nullopt. Ties broken by longer (more specific) path first.
    //   (DESIGN.md §13: "Longest matching path should win.")
    std::optional<std::string> default_agent_for(const std::string& cwd) const;
};

// Load agents.yaml. Returns ok=false with `error` set on any parse failure.
// A missing file is a soft failure (no agents) unless `require` is true.
Config load_config(const std::string& path, bool require, std::string* error);

// Where the daemon looks for its config/state/socket by default (XDG-aware,
// DESIGN.md §5, §19). Returned as concrete resolved paths.
struct Paths {
    std::string config_dir;    // $XDG_CONFIG_HOME/kitty-a2a (else ~/.config/kitty-a2a)
    std::string state_dir;     // $XDG_STATE_HOME/kitty-a2a (else ~/.local/state/kitty-a2a)
    std::string runtime_dir;   // $XDG_RUNTIME_DIR/kitty-a2a (else a per-user temp fallback)
    std::string agents_yaml;   // config_dir + "/agents.yaml"
    std::string db;            // state_dir + "/a2ad.db"
    std::string socket;        // runtime_dir + "/a2ad.sock"

    static Paths resolve();
};

}  // namespace kitty_a2a

#pragma once

#include <optional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "kitty_a2a/types.hpp"

namespace kitty_a2a {

// A single advertised protocol endpoint + binding for an agent (A2A 1.0
// `AgentInterface`). In 1.0 the protocol version moved from the card to each
// interface, so we carry it here.
struct AgentInterface {
    std::string url;               // full endpoint URL
    std::string protocol_binding;  // "jsonrpc" | "grpc" | "rest"
    std::string protocol_version;  // e.g. "1.0.0"
    std::string tenant;
};

struct SecurityScheme {
    std::string name;
    std::string type;      // apiKey | http | oauth2 | openIdConnect | mutualTLS
    std::string scheme;    // bearer/basic/etc. for HTTP auth
    std::string location;  // header | query | cookie for apiKey
    std::string parameter;
};

struct SecurityRequirement {
    std::map<std::string, std::vector<std::string>> schemes;
};

// A skill the agent advertises (A2A `AgentSkill`).
struct AgentSkill {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> tags;
};

// Capabilities block of an AgentCard (A2A 1.0 `AgentCapabilities`).
struct AgentCapabilities {
    bool streaming = false;
    bool push_notifications = false;
    bool extended_agent_card = false;
};

struct AgentExtension {
    std::string uri;
    std::string description;
    bool required = false;
    nlohmann::json params = nlohmann::json::object();
};

// The daemon's parsed view of an A2A Agent Card.
//
// DESIGN.md §6 is explicit that the config file must *not* duplicate card
// information; the card is the source of truth. We only persist the parts a
// client needs to interact: identity, capabilities, skills, supported
// interfaces (endpoint + binding + version), and auth requirement descriptors.
struct AgentCard {
    // Identity
    std::string name;
    std::string description;
    std::string version;

    // Interaction
    AgentCapabilities capabilities;
    std::vector<AgentExtension> extensions;
    std::vector<AgentSkill> skills;
    std::vector<AgentInterface> interfaces;

    std::vector<std::string> input_modes;   // e.g. "text/plain"
    std::vector<std::string> output_modes;

    // Authentication. We record only *descriptors* of what the card demands
    // (scheme name/type + a hint like the header or env var to read), never a
    // credential value. Values live behind the CredentialProvider (DESIGN.md §20).
    std::vector<std::string> auth_schemes;   // e.g. "apiKey", "http", "oauth2"
    std::map<std::string, SecurityScheme> security_schemes;
    std::vector<SecurityRequirement> security_requirements;
    std::vector<std::string> warnings;
    nlohmann::json raw = nlohmann::json::object();

    bool valid = false;          // parsed successfully and minimally coherent
    std::string parse_error;     // why parsing/validation failed, if any

    // True if the card declares at least one non-anonymous security scheme
    // (i.e. the endpoint will require an auth header).
    bool requires_auth() const { return !auth_schemes.empty(); }
};

// The daemon's knowledge about a configured agent: its registry id + endpoint
// (from config) merged with whatever the Agent Card tells us (once discovered).
struct Agent {
    AgentId id;                  // registry name, e.g. "cpp-specialist"
    std::string endpoint;        // configured base endpoint, e.g. "https://dev01.internal/a2a"
    std::string host_label;      // short host label for the picker, e.g. "dev01"

    std::optional<AgentCard> card;      // populated after discovery
    bool available = false;            // last discovery health check
    std::string last_error;            // why unavailable, if any

    // The endpoint actually used for A2A calls (the first supported interface
    // URL from the card, else the configured endpoint). A remote card cannot
    // replace a non-local configured endpoint with a loopback/unspecified URL.
    std::string effective_endpoint() const;
    std::optional<AgentInterface> effective_interface() const;
    bool supports_streaming() const;
    // Whether the (discovered) card declares at least one non-anonymous
    // security scheme — i.e. the endpoint will require an auth header.
    bool requires_auth() const {
        return card.has_value() && !card->auth_schemes.empty();
    }
};

}  // namespace kitty_a2a

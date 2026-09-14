#include "kitty_a2a/AgentCard.hpp"

#include <algorithm>
#include <cctype>

namespace kitty_a2a {

namespace {
bool supported(const AgentInterface& iface) {
    std::string binding = iface.protocol_binding;
    for (char& c : binding) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const bool known = binding == "JSONRPC" || binding == "GRPC" || binding == "HTTP+JSON" ||
                       binding == "REST" || binding == "HTTP_JSON";
    const bool v1 = iface.protocol_version.empty() || iface.protocol_version == "1" ||
                    iface.protocol_version.rfind("1.", 0) == 0;
    return known && v1 && !iface.url.empty();
}

std::string url_host(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const auto authority_start = scheme + 3;
    const auto authority_end = url.find_first_of("/?#", authority_start);
    std::string authority = url.substr(authority_start, authority_end - authority_start);
    if (const auto userinfo = authority.rfind('@'); userinfo != std::string::npos)
        authority.erase(0, userinfo + 1);
    if (!authority.empty() && authority.front() == '[') {
        const auto closing = authority.find(']');
        if (closing == std::string::npos) return {};
        return authority.substr(1, closing - 1);
    }
    if (const auto port = authority.find(':'); port != std::string::npos)
        authority.resize(port);
    std::transform(authority.begin(), authority.end(), authority.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return authority;
}

bool is_local_only_url(const std::string& url) {
    const std::string host = url_host(url);
    if (host == "localhost" || host == "::" || host == "::1" || host == "0.0.0.0") return true;
    if (host.size() > 10 && host.ends_with(".localhost")) return true;
    if (host.rfind("127.", 0) == 0) return true;
    return false;
}
}

std::optional<AgentInterface> Agent::effective_interface() const {
    if (card) {
        for (const auto& iface : card->interfaces) {
            if (!supported(iface)) continue;
            if (!endpoint.empty() && !is_local_only_url(endpoint) && is_local_only_url(iface.url)) {
                AgentInterface safe = iface;
                safe.url = endpoint;
                return safe;
            }
            return iface;
        }
    }
    if (endpoint.empty()) return std::nullopt;
    return AgentInterface{endpoint, "JSONRPC", "1.0", ""};
}

std::string Agent::effective_endpoint() const {
    // The Agent Card orders interfaces by preference. All three standard v1
    // bindings are implemented, so honor that ordering.
    auto iface = effective_interface();
    return iface ? iface->url : endpoint;
}

bool Agent::supports_streaming() const {
    if (!card.has_value()) return false;
    return card->capabilities.streaming;
}

}  // namespace kitty_a2a

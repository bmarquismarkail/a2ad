#include "kitty_a2a/AgentCard.hpp"

namespace kitty_a2a {

std::string Agent::effective_endpoint() const {
    // The Agent Card orders interfaces by preference. All three standard v1
    // bindings are implemented, so honor that ordering.
    if (card.has_value()) {
        for (const auto& iface : card->interfaces) if (!iface.url.empty()) return iface.url;
    }
    return endpoint;
}

bool Agent::supports_streaming() const {
    if (!card.has_value()) return false;
    return card->capabilities.streaming;
}

}  // namespace kitty_a2a

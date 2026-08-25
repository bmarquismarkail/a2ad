#include "kitty_a2a/AgentCard.hpp"

namespace kitty_a2a {

std::string Agent::effective_endpoint() const {
    // Prefer the first supported interface URL from the discovered card (the
    // card is the source of truth for the actual endpoint), else fall back to
    // the configured endpoint.
    if (card.has_value()) {
        for (const auto& iface : card->interfaces) {
            if (!iface.url.empty()) return iface.url;
        }
    }
    return endpoint;
}

bool Agent::supports_streaming() const {
    if (!card.has_value()) return false;
    return card->capabilities.streaming;
}

}  // namespace kitty_a2a

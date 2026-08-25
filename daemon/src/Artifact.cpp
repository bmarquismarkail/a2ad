#include "kitty_a2a/Artifact.hpp"

#include "kitty_a2a/Task.hpp"

#include <sstream>

namespace kitty_a2a {

std::string Part::preview() const {
    if (!text.empty()) {
        const size_t n = text.size() < 240 ? text.size() : 240;
        std::string out = text.substr(0, n);
        // Collapse newlines for a compact one-line preview.
        for (auto& c : out) if (c == '\n' || c == '\r') c = ' ';
        if (text.size() > 240) out += " …";
        return out;
    }
    if (!url.empty()) return "url: " + url;
    if (!raw_b64.empty()) return "<binary " + std::to_string(raw_b64.size()) + " b64 chars>";
    if (!data.empty()) {
        const size_t n = data.size() < 240 ? data.size() : 240;
        return data.substr(0, n) + (data.size() > 240 ? " …" : "");
    }
    return "(empty part)";
}

std::string Artifact::content_summary() const {
    if (parts.empty()) return "(no parts)";
    for (const auto& p : parts) {
        if (!p.text.empty()) {
            // Prefer the first text part.
            return p.preview();
        }
    }
    for (const auto& p : parts) {
        if (!p.data.empty()) return p.preview();
    }
    for (const auto& p : parts) {
        if (!p.url.empty()) return p.preview();
    }
    return parts.front().preview();
}

std::string Message::text() const {
    std::string out;
    for (const auto& p : parts) {
        if (!p.text.empty()) {
            if (!out.empty()) out += "\n";
            out += p.text;
        } else if (!p.data.empty()) {
            if (!out.empty()) out += "\n";
            out += p.data;
        }
    }
    return out;
}

}  // namespace kitty_a2a

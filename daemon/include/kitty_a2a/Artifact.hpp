#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "kitty_a2a/types.hpp"

namespace kitty_a2a {

// A single unit of content inside a message or artifact, mirroring the A2A
// `Part` oneof. We keep exactly one of the fields populated, matching the
// protocol's "exactly one content field" rule.
struct Part {
    std::string text;              // content.kind == "text"
    std::string raw_b64;           // content.kind == "raw" (base64 on the wire)
    std::string url;               // content.kind == "url"
    std::string data;              // content.kind == "data" (JSON text)

    std::string media_type;        // e.g. "text/plain", "application/json"
    std::string filename;          // optional file name
    nlohmann::json metadata = nlohmann::json::object();

    bool empty() const {
        return text.empty() && raw_b64.empty() && url.empty() && data.empty();
    }
    std::string preview() const;   // a short, human-readable snippet for UIs
};

// An A2A `Artifact`: a tangible output produced by a task (patch file, report,
// benchmark data, image, ...). Per DESIGN.md §17 these are first-class and must
// be surfaced separately from conversational messages — never scraped from
// terminal output.
struct Artifact {
    std::string artifact_id;
    std::string name;              // e.g. "vdp-optimization.patch"
    std::string description;
    std::vector<Part> parts;
    nlohmann::json metadata = nlohmann::json::object();
    std::vector<std::string> extensions;

    // The best single human-readable representation of the artifact's content,
    // preferring text/data, then URL, then a raw-size note. Used by the task
    // view. Does not change the structured fields above.
    std::string content_summary() const;
};

}  // namespace kitty_a2a

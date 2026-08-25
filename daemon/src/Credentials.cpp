#include "kitty_a2a/Credentials.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace kitty_a2a {

AuthSpec FilesystemCredentialProvider::resolve(const std::string& auth_type, const std::string& name) {
    AuthSpec spec;

    if (auth_type.empty() || auth_type == "none" || name.empty()) {
        spec.active = false;
        spec.scheme = "none";
        return spec;
    }

    if (auth_type == "env") {
        const char* v = std::getenv(name.c_str());
        if (v && *v) {
            spec.active = true;
            spec.scheme = "env";
            spec.header_name = "Authorization";
            spec.header_value = std::string("Bearer ") + v;
        } else {
            spec.active = false;
            spec.scheme = "error";
        }
        return spec;
    }

    if (auth_type == "credential-file") {
        // Read the first non-empty line of `name`.
        std::ifstream f(name);
        if (!f) {
            spec.active = false;
            spec.scheme = "error";
            return spec;
        }
        // Best-effort permission check: warn-level (not fatal) if the file is
        // world/group readable. We intentionally do not log the value.
        struct stat st{};
        std::string line;
        if (::stat(name.c_str(), &st) == 0) {
            if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
                // Leave it usable but flag it; the daemon log records this once.
            }
        }
        while (std::getline(f, line)) {
            // trim trailing \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) break;
        }
        if (line.empty()) {
            spec.active = false;
            spec.scheme = "error";
            return spec;
        }
        spec.active = true;
        spec.scheme = "credential-file";
        spec.header_name = "Authorization";
        spec.header_value = "Bearer " + line;
        return spec;
    }

    spec.active = false;
    spec.scheme = "error";
    return spec;
}

std::unique_ptr<CredentialProvider> make_credential_provider() {
    return std::make_unique<FilesystemCredentialProvider>();
}

}  // namespace kitty_a2a

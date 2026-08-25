#pragma once

#include <memory>
#include <optional>
#include <string>

namespace kitty_a2a {

// How an HTTP request should be authenticated. Populated by a CredentialProvider
// from the (non-secret) reference in the agent config plus runtime context.
struct AuthSpec {
    bool active = false;

    // For HTTP header auth (api-key, basic, bearer): set header_name + header_value.
    std::string header_name;   // e.g. "Authorization", "X-Api-Key"
    std::string header_value;  // e.g. "Bearer <token>", "<key>"

    // The kind of auth, for logging/diagnostics (never the secret itself).
    std::string scheme;        // "env" | "credential-file" | "none" | "error"
};

// DESIGN.md §20: "Design authentication behind an interface." Credentials are
// resolved from external, non-config sources at request time. The initial
// implementation supports:
//   * environment variables, and
//   * a filesystem credential file with strict permissions (0600).
// Future sources (keyring, client certs, Vault, systemd credentials) plug in by
// adding a subclass — the daemon core does not change.
class CredentialProvider {
public:
    virtual ~CredentialProvider() = default;

    // Resolve the auth spec for an agent from its (non-secret) config reference.
    //   auth_type "none"           -> AuthSpec{active=false}
    //   auth_type "env" + name     -> read env var NAME
    //   auth_type "credential-file" + name -> read first line of file `name`
    // Returns active=false (with scheme=="error" and a reason) if the source is
    // missing/unreadable. The caller must treat an inactive spec as "no auth".
    virtual AuthSpec resolve(const std::string& auth_type, const std::string& name) = 0;
};

// Concrete provider backed by the process environment + credential files.
class FilesystemCredentialProvider : public CredentialProvider {
public:
    AuthSpec resolve(const std::string& auth_type, const std::string& name) override;
};

std::unique_ptr<CredentialProvider> make_credential_provider();

}  // namespace kitty_a2a

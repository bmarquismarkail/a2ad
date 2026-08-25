#pragma once

#include <memory>
#include <string>
#include <vector>

#include "kitty_a2a/A2AClient.hpp"

namespace kitty_a2a {

// A libcurl-backed HTTP transport. Performs a single blocking request per
// call with a connect+total timeout. Suitable for the daemon's worker thread.
class CurlTransport : public HttpTransport {
public:
    explicit CurlTransport(int timeout_ms = 30000, std::string user_agent = "a2ad/0.1");
    ~CurlTransport() override;

    HttpResponse request(const std::string& endpoint, const std::string& method,
                         const std::string& path, const std::string& body,
                         const std::vector<std::pair<std::string, std::string>>& headers) override;
    HttpResponse stream(const std::string& endpoint, const std::string& method,
                        const std::string& path, const std::string& body,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        const std::function<bool(std::string_view)>& on_chunk) override;

private:
    int timeout_ms_;
    std::string user_agent_;
};

std::shared_ptr<HttpTransport> make_curl_transport(int timeout_ms = 30000);

}  // namespace kitty_a2a

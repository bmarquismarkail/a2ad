#include "kitty_a2a/HttpTransport.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace kitty_a2a {

namespace {
// libcurl's global init is not thread-safe to call repeatedly. Use a
// process-lifetime static.
struct CurlGlobalInit {
    CurlGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobalInit() { curl_global_cleanup(); }
};
CurlGlobalInit& curlGlobal() {
    static CurlGlobalInit g;
    return g;
}

size_t writeCb(void* data, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(data), size * nmemb);
    return size * nmemb;
}
}  // namespace

CurlTransport::CurlTransport(int timeout_ms, std::string user_agent)
    : timeout_ms_(timeout_ms), user_agent_(std::move(user_agent)) {
    (void)curlGlobal();
}

CurlTransport::~CurlTransport() = default;

HttpResponse CurlTransport::request(const std::string& endpoint, const std::string& method,
                                    const std::string& path, const std::string& body,
                                    const std::vector<std::pair<std::string, std::string>>& headers) {
    HttpResponse r;
    std::string url = endpoint;
    std::string p = path;
    if (!p.empty()) {
        if (url.empty() || url.back() != '/') url += '/';
        if (p[0] == '/') p.erase(0, 1);
        url += p;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        r.transport_error = true;
        r.transport_error_detail = "curl_easy_init failed";
        return r;
    }

    std::string resp_body;
    curl_slist* hdrs = nullptr;
    for (const auto& [k, v] : headers) {
        hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
    }
    // Ensure Accept is present for GETs (agent card discovery).
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent_.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout_ms_ / 2));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms_);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    // Disable SSL verification only if explicitly requested via env (a2ad
    // trusts the system CA store by default).
    if (const char* noverify = std::getenv("A2AD_INSECURE_TLS"); noverify && *noverify == '1') {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {  // POST (default for A2A JSON-RPC)
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        }
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        r.transport_error = true;
        r.transport_error_detail = curl_easy_strerror(res);
        return r;
    }
    r.status = (int)http_code;
    r.body = std::move(resp_body);
    return r;
}

std::shared_ptr<HttpTransport> make_curl_transport(int timeout_ms) {
    return std::make_shared<CurlTransport>(timeout_ms);
}

}  // namespace kitty_a2a

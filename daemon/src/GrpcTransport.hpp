#pragma once

#include <functional>
#include <stop_token>
#include <string>

#include "kitty_a2a/A2AClient.hpp"

namespace kitty_a2a {

// Native A2A v1 gRPC adapter. Results are converted to ProtoJSON so the
// binding-independent model parser in A2AClient remains the single source of
// truth for Task/Message decoding.
HttpResponse grpcCall(const std::string& endpoint, const AuthSpec& auth,
                      const std::string& method, const nlohmann::json& params);

HttpResponse grpcSubscribe(const std::string& endpoint, const AuthSpec& auth,
                           const std::string& task_id,
                           const std::function<bool(const nlohmann::json&)>& on_event,
                           std::stop_token stop);

}  // namespace kitty_a2a

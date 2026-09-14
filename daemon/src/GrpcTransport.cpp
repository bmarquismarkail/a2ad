#include "GrpcTransport.hpp"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <memory>
#include <fstream>
#include <sstream>

#include <grpcpp/grpcpp.h>
#include <google/protobuf/util/json_util.h>

#include "a2a.grpc.pb.h"

namespace kitty_a2a {
namespace {
namespace pb = lf::a2a::v1;

std::string targetFor(std::string endpoint) {
    if (endpoint.rfind("https://", 0) == 0) endpoint.erase(0, 8);
    else if (endpoint.rfind("http://", 0) == 0) endpoint.erase(0, 7);
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    return endpoint;
}

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary); std::ostringstream out; out << input.rdbuf(); return out.str();
}

std::shared_ptr<grpc::Channel> channelFor(const std::string& endpoint, const AuthSpec& auth) {
    // Canonical gRPC targets (host:port) are TLS by default. Only an explicit
    // http:// scheme opts into plaintext, which is suitable for local tunnels.
    const bool secure = endpoint.rfind("http://", 0) != 0;
    grpc::SslCredentialsOptions options;
    if (!auth.client_cert_file.empty()) options.pem_cert_chain = readFile(auth.client_cert_file);
    if (!auth.client_key_file.empty()) options.pem_private_key = readFile(auth.client_key_file);
    std::shared_ptr<grpc::ChannelCredentials> credentials = secure ? grpc::SslCredentials(options)
                                                                  : grpc::InsecureChannelCredentials();
    return grpc::CreateChannel(targetFor(endpoint), std::move(credentials));
}

void configureContext(grpc::ClientContext& context, const AuthSpec& auth, bool streaming,
                      std::string_view version = "1.0") {
    if (auth.active && !auth.header_value.empty()) {
        std::string key = auth.header_name;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        context.AddMetadata(key, auth.header_value);
    }
    for (const auto& [header, value] : auth.extra_headers) {
        std::string key = header;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        context.AddMetadata(key, value);
    }
    if (auth.active && !auth.query_name.empty()) context.AddMetadata(auth.query_name, auth.query_value);
    for (const auto& [name, value] : auth.extra_query) context.AddMetadata(name, value);
    context.AddMetadata("a2a-version", std::string(version));
    if (!streaming) context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
}

HttpResponse fromStatus(const grpc::Status& status) {
    HttpResponse response;
    if (status.ok()) { response.status = 200; return response; }
    response.body = status.error_message();
    switch (status.error_code()) {
        case grpc::StatusCode::UNAUTHENTICATED: response.status = 401; break;
        case grpc::StatusCode::PERMISSION_DENIED: response.status = 403; break;
        case grpc::StatusCode::NOT_FOUND: response.status = 404; break;
        case grpc::StatusCode::INVALID_ARGUMENT:
        case grpc::StatusCode::FAILED_PRECONDITION: response.status = 400; break;
        case grpc::StatusCode::UNAVAILABLE:
        case grpc::StatusCode::DEADLINE_EXCEEDED:
            response.transport_error = true;
            response.transport_error_detail = status.error_message();
            break;
        default: response.status = 500; break;
    }
    return response;
}

template <class Message>
bool fromJson(const nlohmann::json& json, Message* message, HttpResponse* error) {
    auto status = google::protobuf::util::JsonStringToMessage(json.dump(), message);
    if (status.ok()) return true;
    error->status = 400;
    error->body = std::string("cannot encode gRPC request: ") + status.ToString();
    return false;
}

template <class Message>
void setJson(const Message& message, HttpResponse* response) {
    google::protobuf::util::JsonPrintOptions options;
    options.preserve_proto_field_names = false;
    auto status = google::protobuf::util::MessageToJsonString(message, &response->body, options);
    if (!status.ok()) {
        response->status = 500;
        response->body = std::string("cannot decode gRPC response: ") + status.ToString();
    }
}
}  // namespace

HttpResponse grpcCall(const std::string& endpoint, const AuthSpec& auth,
                      const std::string& method, const nlohmann::json& params,
                      const std::string& protocol_version) {
    auto stub = pb::A2AService::NewStub(channelFor(endpoint, auth));
    grpc::ClientContext context;
    configureContext(context, auth, false, protocol_version);
    HttpResponse response;
    grpc::Status status;

    if (method == "SendMessage") {
        pb::SendMessageRequest request; pb::SendMessageResponse result;
        if (!fromJson(params, &request, &response)) return response;
        status = stub->SendMessage(&context, request, &result);
        response = fromStatus(status); if (status.ok()) setJson(result, &response);
    } else if (method == "GetTask") {
        pb::GetTaskRequest request; pb::Task result;
        if (!fromJson(params, &request, &response)) return response;
        status = stub->GetTask(&context, request, &result);
        response = fromStatus(status); if (status.ok()) setJson(result, &response);
    } else if (method == "ListTasks") {
        pb::ListTasksRequest request; pb::ListTasksResponse result;
        if (!fromJson(params, &request, &response)) return response;
        status = stub->ListTasks(&context, request, &result);
        response = fromStatus(status); if (status.ok()) setJson(result, &response);
    } else if (method == "CancelTask") {
        pb::CancelTaskRequest request; pb::Task result;
        if (!fromJson(params, &request, &response)) return response;
        status = stub->CancelTask(&context, request, &result);
        response = fromStatus(status); if (status.ok()) setJson(result, &response);
    } else if (method == "CreateTaskPushNotificationConfig") {
        pb::TaskPushNotificationConfig request, result;
        if (!fromJson(params, &request, &response)) return response;
        status = stub->CreateTaskPushNotificationConfig(&context, request, &result);
        response = fromStatus(status); if (status.ok()) setJson(result, &response);
    } else if (method == "GetTaskPushNotificationConfig") {
        pb::GetTaskPushNotificationConfigRequest request; pb::TaskPushNotificationConfig result;
        if (!fromJson(params, &request, &response)) return response;
        status = stub->GetTaskPushNotificationConfig(&context, request, &result);
        response = fromStatus(status); if (status.ok()) setJson(result, &response);
    } else if (method == "ListTaskPushNotificationConfigs") {
        pb::ListTaskPushNotificationConfigsRequest request; pb::ListTaskPushNotificationConfigsResponse result;
        if (!fromJson(params, &request, &response)) return response;
        status = stub->ListTaskPushNotificationConfigs(&context, request, &result);
        response = fromStatus(status); if (status.ok()) setJson(result, &response);
    } else if (method == "DeleteTaskPushNotificationConfig") {
        pb::DeleteTaskPushNotificationConfigRequest request; google::protobuf::Empty result;
        if (!fromJson(params, &request, &response)) return response;
        status = stub->DeleteTaskPushNotificationConfig(&context, request, &result);
        response = fromStatus(status); if (status.ok()) setJson(result, &response);
    } else if (method == "GetExtendedAgentCard") {
        pb::GetExtendedAgentCardRequest request; pb::AgentCard result;
        if (!fromJson(params, &request, &response)) return response;
        status = stub->GetExtendedAgentCard(&context, request, &result);
        response = fromStatus(status); if (status.ok()) setJson(result, &response);
    } else {
        response.status = 400;
        response.body = "unsupported gRPC operation: " + method;
    }
    return response;
}

HttpResponse grpcStream(const std::string& endpoint, const AuthSpec& auth,
                        const std::string& method, const nlohmann::json& params,
                        const std::function<bool(const nlohmann::json&)>& on_event,
                        std::stop_token stop, const std::string& protocol_version) {
    auto stub = pb::A2AService::NewStub(channelFor(endpoint, auth));
    grpc::ClientContext context;
    configureContext(context, auth, true, protocol_version);
    std::stop_callback cancel(stop, [&context] { context.TryCancel(); });
    pb::StreamResponse event;
    auto consume = [&](auto& reader) {
        while (!stop.stop_requested() && reader->Read(&event)) {
            std::string json;
            if (!google::protobuf::util::MessageToJsonString(event, &json).ok()) continue;
            try { if (!on_event(nlohmann::json::parse(json))) { context.TryCancel(); break; } }
            catch (...) { /* ignore one malformed conversion */ }
        }
        return fromStatus(reader->Finish());
    };
    HttpResponse error;
    if (method == "SubscribeToTask") {
        pb::SubscribeToTaskRequest request;
        if (!fromJson(params, &request, &error)) return error;
        auto reader = stub->SubscribeToTask(&context, request);
        return consume(reader);
    }
    if (method == "SendStreamingMessage") {
        pb::SendMessageRequest request;
        if (!fromJson(params, &request, &error)) return error;
        auto reader = stub->SendStreamingMessage(&context, request);
        return consume(reader);
    }
    error.status = 400; error.body = "unsupported gRPC stream operation: " + method;
    return error;
}

}  // namespace kitty_a2a

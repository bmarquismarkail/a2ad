#include "GrpcTransport.hpp"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <memory>

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

std::shared_ptr<grpc::Channel> channelFor(const std::string& endpoint) {
    const bool secure = endpoint.rfind("https://", 0) == 0;
    std::shared_ptr<grpc::ChannelCredentials> credentials = secure
        ? grpc::SslCredentials(grpc::SslCredentialsOptions{})
        : grpc::InsecureChannelCredentials();
    return grpc::CreateChannel(targetFor(endpoint), std::move(credentials));
}

void configureContext(grpc::ClientContext& context, const AuthSpec& auth, bool streaming) {
    if (auth.active && !auth.header_value.empty()) {
        std::string key = auth.header_name;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        context.AddMetadata(key, auth.header_value);
    }
    context.AddMetadata("a2a-version", "1.0");
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
                      const std::string& method, const nlohmann::json& params) {
    auto stub = pb::A2AService::NewStub(channelFor(endpoint));
    grpc::ClientContext context;
    configureContext(context, auth, false);
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
    } else {
        response.status = 400;
        response.body = "unsupported gRPC operation: " + method;
    }
    return response;
}

HttpResponse grpcSubscribe(const std::string& endpoint, const AuthSpec& auth,
                           const std::string& task_id,
                           const std::function<bool(const nlohmann::json&)>& on_event,
                           std::stop_token stop) {
    auto stub = pb::A2AService::NewStub(channelFor(endpoint));
    grpc::ClientContext context;
    configureContext(context, auth, true);
    std::stop_callback cancel(stop, [&context] { context.TryCancel(); });
    pb::SubscribeToTaskRequest request; request.set_id(task_id);
    auto reader = stub->SubscribeToTask(&context, request);
    pb::StreamResponse event;
    while (!stop.stop_requested() && reader->Read(&event)) {
        std::string json;
        if (!google::protobuf::util::MessageToJsonString(event, &json).ok()) continue;
        try { if (!on_event(nlohmann::json::parse(json))) { context.TryCancel(); break; } }
        catch (...) { /* ignore one malformed conversion */ }
    }
    return fromStatus(reader->Finish());
}

}  // namespace kitty_a2a

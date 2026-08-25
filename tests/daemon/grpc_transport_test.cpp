#include "test_framework.hpp"
#include "kitty_a2a/A2AClient.hpp"
#include "kitty_a2a/Credentials.hpp"

#include <grpcpp/grpcpp.h>
#include "a2a.grpc.pb.h"

using namespace kitty_a2a;
namespace a2apb = lf::a2a::v1;

namespace {
class CardTransport final : public HttpTransport {
public:
    explicit CardTransport(int port) : port_(port) {}
    HttpResponse request(const std::string&, const std::string&, const std::string&,
                         const std::string&,
                         const std::vector<std::pair<std::string, std::string>>&) override {
        return {200, "{\"name\":\"grpc-test\",\"version\":\"1.0\","
                     "\"capabilities\":{\"streaming\":true},\"supportedInterfaces\":[{"
                     "\"url\":\"127.0.0.1:" + std::to_string(port_) +
                     "\",\"protocolBinding\":\"GRPC\",\"protocolVersion\":\"1.0\"}]}", false, ""};
    }
private:
    int port_;
};

class TestA2AService final : public a2apb::A2AService::Service {
public:
    grpc::Status SendMessage(grpc::ServerContext*, const a2apb::SendMessageRequest* request,
                             a2apb::SendMessageResponse* response) override {
        auto* task = response->mutable_task();
        task->set_id("grpc-task"); task->set_context_id(request->message().context_id());
        task->mutable_status()->set_state(a2apb::TASK_STATE_WORKING);
        return grpc::Status::OK;
    }
    grpc::Status GetTask(grpc::ServerContext*, const a2apb::GetTaskRequest* request,
                         a2apb::Task* task) override {
        task->set_id(request->id()); task->set_context_id("grpc-context");
        task->mutable_status()->set_state(a2apb::TASK_STATE_COMPLETED);
        return grpc::Status::OK;
    }
    grpc::Status ListTasks(grpc::ServerContext*, const a2apb::ListTasksRequest*,
                           a2apb::ListTasksResponse* response) override {
        auto* task = response->add_tasks(); task->set_id("listed-grpc-task");
        task->mutable_status()->set_state(a2apb::TASK_STATE_WORKING);
        return grpc::Status::OK;
    }
    grpc::Status CancelTask(grpc::ServerContext*, const a2apb::CancelTaskRequest* request,
                            a2apb::Task* task) override {
        task->set_id(request->id()); task->mutable_status()->set_state(a2apb::TASK_STATE_CANCELED);
        return grpc::Status::OK;
    }
    grpc::Status SubscribeToTask(grpc::ServerContext*, const a2apb::SubscribeToTaskRequest* request,
                                 grpc::ServerWriter<a2apb::StreamResponse>* writer) override {
        a2apb::StreamResponse response; auto* task = response.mutable_task();
        task->set_id(request->id()); task->mutable_status()->set_state(a2apb::TASK_STATE_COMPLETED);
        writer->Write(response); return grpc::Status::OK;
    }
};
}  // namespace

ADD_TEST(native_grpc_round_trip) {
    TestA2AService service;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    CHECK(server != nullptr);
    CHECK(port > 0);
    if (!server) return;

    auto transport = std::make_shared<CardTransport>(port);
    auto credentials = make_credential_provider();
    A2AClient client(transport, std::shared_ptr<CredentialProvider>(std::move(credentials)));
    auto card = client.discover("http://card.invalid/a2a");
    CHECK(card.valid);
    const std::string endpoint = "127.0.0.1:" + std::to_string(port);

    auto sent = client.sendMessage(endpoint, AuthSpec{}, "hello", TaskId{}, ContextId("grpc-context"));
    CHECK(sent.ok); CHECK(sent.task.has_value());
    if (sent.task) CHECK_EQ(sent.task->state, TaskState::Working);

    auto listed = client.listTasks(endpoint, AuthSpec{});
    CHECK(listed.ok); CHECK_EQ(listed.tasks.size(), size_t{1});

    auto fetched = client.getTask(endpoint, AuthSpec{}, TaskId("grpc-task"));
    CHECK(fetched.ok); CHECK(fetched.task.has_value());
    if (fetched.task) CHECK_EQ(fetched.task->state, TaskState::Completed);

    auto canceled = client.cancelTask(endpoint, AuthSpec{}, TaskId("grpc-task"));
    CHECK(canceled.ok); CHECK(canceled.task.has_value());
    if (canceled.task) CHECK_EQ(canceled.task->state, TaskState::Canceled);

    std::optional<Task> streamed;
    auto subscription = client.subscribeToTask(endpoint, AuthSpec{}, TaskId("grpc-task"),
                                                [&](const Task& task) { streamed = task; });
    CHECK(subscription.ok); CHECK(streamed.has_value());
    if (streamed) CHECK_EQ(streamed->state, TaskState::Completed);
    server->Shutdown();
}

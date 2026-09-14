#include "test_framework.hpp"
#include "kitty_a2a/Config.hpp"
#include <cstdio>
#include <fstream>
#include <filesystem>

using namespace kitty_a2a;
namespace fs = std::filesystem;

static std::string write_tmp(const char* name, const char* body) {
    auto p = fs::temp_directory_path() / name;
    std::ofstream f(p);
    f << body;
    f.close();
    return p.string();
}

ADD_TEST(longest_path_wins) {
    Config c;
    c.projects.push_back({"/data", "general-agent"});
    c.projects.push_back({"/data/projects/proto-time", "emulator-agent"});
    c.projects.push_back({"/data/projects/proto-time/tools", "tools-agent"});

    auto r1 = c.default_agent_for("/data/projects/proto-time");
    CHECK(r1.has_value());
    if (r1) CHECK_EQ(*r1, std::string("emulator-agent"));

    auto r2 = c.default_agent_for("/data/projects/proto-time/tools");
    CHECK(r2.has_value());
    if (r2) CHECK_EQ(*r2, std::string("tools-agent"));

    auto r3 = c.default_agent_for("/data/other");
    CHECK(r3.has_value());
    if (r3) CHECK_EQ(*r3, std::string("general-agent"));

    auto r4 = c.default_agent_for("/unrelated");
    CHECK(!r4.has_value());
}

ADD_TEST(parse_agents_yaml) {
    auto p = write_tmp("a2ad_test_agents.yaml", R"(
agents:
  cpp-specialist:
    endpoint: https://dev01.internal/a2a
    host_label: dev01
    auth:
      type: env
      name: CPP_AGENT_TOKEN
      schemes:
        api-key:
          type: credential-file
          name: /run/credentials/api-key
          in: header
          parameter: X-Api-Key
  general:
    endpoint: https://general.local/a2a
projects:
  /data/projects/proto-time:
    default_agent: cpp-specialist
)");
    std::string err;
    Config c = load_config(p, false, &err);
    CHECK(err.empty());
    CHECK(c.agents.count("cpp-specialist") == 1);
    CHECK(c.agents.count("general") == 1);
    if (c.agents.count("cpp-specialist")) {
        const auto& a = c.agents.at("cpp-specialist");
        CHECK_EQ(a.endpoint, std::string("https://dev01.internal/a2a"));
        CHECK_EQ(a.host_label, std::string("dev01"));
        CHECK_EQ(a.auth_type, std::string("env"));
        CHECK_EQ(a.auth_name, std::string("CPP_AGENT_TOKEN"));
        CHECK_EQ(a.auth_schemes.size(), size_t{1});
        if (a.auth_schemes.contains("api-key")) {
            CHECK_EQ(a.auth_schemes.at("api-key").parameter, std::string("X-Api-Key"));
        }
    }
    CHECK_EQ(c.projects.size(), (size_t)1);
    fs::remove(p);
}

ADD_TEST(missing_config_soft) {
    std::string err;
    Config c = load_config("/nonexistent/agents.yaml", false, &err);
    CHECK(err.empty());
    CHECK(c.agents.empty());
    Config req = load_config("/nonexistent/agents.yaml", true, &err);
    CHECK(!err.empty());
}

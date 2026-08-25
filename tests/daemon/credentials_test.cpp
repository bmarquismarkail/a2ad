#include "test_framework.hpp"
#include "kitty_a2a/Credentials.hpp"
#include <cstdlib>
#include <string>

using namespace kitty_a2a;

ADD_TEST(env_credential) {
    setenv("A2AD_TEST_TOKEN", "secret-value", 1);
    auto prov = make_credential_provider();
    AuthSpec s = prov->resolve("env", "A2AD_TEST_TOKEN");
    CHECK(s.active);
    CHECK_EQ(s.header_name, std::string("Authorization"));
    CHECK_EQ(s.header_value, std::string("Bearer secret-value"));
    unsetenv("A2AD_TEST_TOKEN");
}

ADD_TEST(env_credential_missing) {
    unsetenv("A2AD_TEST_MISSING_TOKEN");
    auto prov = make_credential_provider();
    AuthSpec s = prov->resolve("env", "A2AD_TEST_MISSING_TOKEN");
    CHECK(!s.active);
}

ADD_TEST(none_credential) {
    auto prov = make_credential_provider();
    AuthSpec s = prov->resolve("none", "");
    CHECK(!s.active);
}

ADD_TEST(unsupported_type) {
    auto prov = make_credential_provider();
    AuthSpec s = prov->resolve("bearer-inline", "");
    CHECK(!s.active);
    // A bogus type should not accidentally activate auth.
    (void)s;
}

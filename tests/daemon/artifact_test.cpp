#include "test_framework.hpp"
#include "kitty_a2a/Artifact.hpp"
#include "kitty_a2a/Task.hpp"
#include <string>

using namespace kitty_a2a;

ADD_TEST(part_preview) {
    Part p; p.text = "hello\nworld";
    std::string prev = p.preview();
    CHECK(prev.find("hello") != std::string::npos);
    CHECK(prev.find("world") != std::string::npos);
    CHECK(prev.find('\n') == std::string::npos);

    Part url_part; url_part.url = "https://x/p.pdf";
    CHECK(url_part.preview().find("url:") != std::string::npos);

    Part empty;
    CHECK(empty.empty());
}

ADD_TEST(artifact_summary) {
    Artifact a; a.name = "report.patch";
    Part p2; p2.url = "https://x/p";
    Part p1; p1.text = "diff --git a/x b/y";
    a.parts.push_back(p2);
    a.parts.push_back(p1);
    CHECK(a.content_summary().find("diff --git") != std::string::npos);

    Artifact empty; empty.name = "empty";
    CHECK_EQ(empty.content_summary(), std::string("(no parts)"));
}

ADD_TEST(message_text) {
    Message m; m.role = "agent";
    Part a; a.text = "line1";
    Part b; b.data = "{\"x\":1}";
    Part c; c.url = "https://ignored";
    m.parts = {a, b, c};
    std::string txt = m.text();
    CHECK(txt.find("line1") != std::string::npos);
    CHECK(txt.find("{\"x\":1}") != std::string::npos);
    CHECK(txt.find("https://ignored") == std::string::npos);
}

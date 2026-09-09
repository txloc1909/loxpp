// Unit tests for the loxpp-lsp building blocks that are awkward to cover from
// the Python smoke test: JSON-RPC framing, CRLF handling, the debounce, and
// the stdlib doc table's coverage of src/tooling/stdlib_names.h.

#include "lsp/document_store.h"
#include "lsp/json_rpc.h"
#include "lsp/protocol.h"
#include "lsp/stdlib_docs.h"
#include "tooling/stdlib_names.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

using loxpp::lsp::Diagnostic;
using loxpp::lsp::DocumentStore;
using loxpp::lsp::JsonRpc;
using json = nlohmann::json;

std::string frame(const json& message) {
    const std::string body = message.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// Splits a raw LSP output stream into its JSON message bodies.
std::vector<json> parseFrames(const std::string& raw) {
    std::vector<json> out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t header = raw.find("Content-Length: ", pos);
        if (header == std::string::npos) {
            break;
        }
        const std::size_t numStart =
            header + std::string("Content-Length: ").size();
        const std::size_t numEnd = raw.find("\r\n", numStart);
        const auto len = static_cast<std::size_t>(
            std::stoul(raw.substr(numStart, numEnd - numStart)));
        const std::size_t bodyStart = raw.find("\r\n\r\n", numEnd) + 4;
        out.push_back(json::parse(raw.substr(bodyStart, len)));
        pos = bodyStart + len;
    }
    return out;
}

TEST(LspJsonRpc, FramingAndDispatch) {
    std::istringstream in(frame({{"jsonrpc", "2.0"},
                                 {"id", 1},
                                 {"method", "ping"},
                                 {"params", {{"n", 41}}}}));
    std::ostringstream out;
    JsonRpc rpc(in, out);
    rpc.onRequest("ping", [](const json& p) {
        return json{{"pong", p.at("n").get<int>() + 1}};
    });
    EXPECT_EQ(rpc.run(), 0);

    const auto frames = parseFrames(out.str());
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].at("id"), 1);
    EXPECT_EQ(frames[0].at("result").at("pong"), 42);
}

TEST(LspJsonRpc, MalformedJsonGivesParseErrorAndKeepsRunning) {
    std::string input = "Content-Length: 7\r\n\r\n{ bad :";
    input += frame({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}});
    std::istringstream in(input);
    std::ostringstream out;
    JsonRpc rpc(in, out);
    rpc.onRequest("ping", [](const json&) { return json{{"ok", true}}; });
    rpc.run();

    const auto frames = parseFrames(out.str());
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(frames[0].at("error").at("code"), -32700); // ParseError
    EXPECT_EQ(frames[1].at("result").at("ok"), true);
}

TEST(LspJsonRpc, UnknownMethodGivesMethodNotFound) {
    std::istringstream in(
        frame({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "nope"}}));
    std::ostringstream out;
    JsonRpc rpc(in, out);
    rpc.run();
    const auto frames = parseFrames(out.str());
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].at("error").at("code"), -32601);
}

// A publish sink that lets a test wait for the next publish.
class PublishSink {
  public:
    DocumentStore::PublishFn fn() {
        return [this](const std::string& uri, std::int64_t version,
                      const std::vector<Diagnostic>& diags) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_last = {uri, version, diags};
            ++m_count;
            m_cv.notify_all();
        };
    }

    struct Event {
        std::string uri;
        std::int64_t version = 0;
        std::vector<Diagnostic> diags;
    };

    Event waitForCount(int n) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::seconds(5),
                      [&] { return m_count >= n; });
        return m_last;
    }
    int count() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_count;
    }

  private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    Event m_last;
    int m_count = 0;
};

TEST(LspDocumentStore, OpenPublishesDiagnosticsSynchronously) {
    PublishSink sink;
    DocumentStore store(sink.fn());

    store.didOpen("file:///clean.lox", 1, "var x = 1;\nprint x + 2;\n");
    EXPECT_EQ(sink.count(), 1);
    EXPECT_TRUE(sink.waitForCount(1).diags.empty());

    // '1 +;' is the LSP diagnostics fixture. analyze() reports "Expect
    // expression." anchored at end of input (same as the compiler's
    // 'print 1 +;' fixture).
    store.didOpen("file:///bad.lox", 1, "1 +;\n");
    auto ev = sink.waitForCount(2);
    ASSERT_EQ(ev.diags.size(), 1U);
    EXPECT_EQ(ev.diags[0].severity, loxpp::lsp::DiagnosticSeverity::Error);

    // A syntax error inside a line lands on that line, not at end of input.
    store.didOpen("file:///inline.lox", 1, "var a = 1;\nvar b = @;\n");
    ev = sink.waitForCount(3);
    ASSERT_EQ(ev.diags.size(), 1U);
    EXPECT_EQ(ev.diags[0].range.start.line, 1U);
    EXPECT_GE(ev.diags[0].range.start.character, 1U);
    EXPECT_LE(ev.diags[0].range.start.character, 10U);
}

TEST(LspDocumentStore, CrlfFileAnalysesLikeItsLfTwin) {
    PublishSink lfSink;
    DocumentStore lfStore(lfSink.fn());
    lfStore.didOpen("file:///lf.lox", 1, "var a = 1;\nvar b = @;\n");
    const auto lf = lfSink.waitForCount(1);

    PublishSink crlfSink;
    DocumentStore crlfStore(crlfSink.fn());
    crlfStore.didOpen("file:///crlf.lox", 1, "var a = 1;\r\nvar b = @;\r\n");
    const auto crlf = crlfSink.waitForCount(1);

    ASSERT_EQ(lf.diags.size(), 1U);
    ASSERT_EQ(crlf.diags.size(), 1U);
    EXPECT_EQ(crlf.diags[0].range.start.line, lf.diags[0].range.start.line);
    EXPECT_EQ(crlf.diags[0].range.start.character,
              lf.diags[0].range.start.character);
}

TEST(LspDocumentStore, LoneCrFileAnalysesLikeItsLfTwin) {
    PublishSink lfSink;
    DocumentStore lfStore(lfSink.fn());
    lfStore.didOpen("file:///lf.lox", 1,
                    "var a = 1;\nvar b = 2;\nvar c = @;\n");
    const auto lf = lfSink.waitForCount(1);

    // Classic-Mac line endings: a lone CR terminates each line.
    PublishSink crSink;
    DocumentStore crStore(crSink.fn());
    crStore.didOpen("file:///cr.lox", 1,
                    "var a = 1;\rvar b = 2;\rvar c = @;\r");
    const auto cr = crSink.waitForCount(1);

    ASSERT_EQ(lf.diags.size(), 1U);
    ASSERT_EQ(cr.diags.size(), 1U);
    EXPECT_EQ(cr.diags[0].range.start.line, lf.diags[0].range.start.line);
    EXPECT_EQ(cr.diags[0].range.start.character,
              lf.diags[0].range.start.character);
    // The error is on the third line; a dropped CR would report it earlier.
    EXPECT_EQ(cr.diags[0].range.start.line, 2U);
}

TEST(LspDocumentStore, DebounceCollapsesBurstAndKeepsLastChange) {
    PublishSink sink;
    DocumentStore store(sink.fn());
    store.didOpen("file:///d.lox", 1, "var x = 1;\n");
    ASSERT_TRUE(sink.waitForCount(1).diags.empty());

    // A rapid burst. Every change but the last is clean; the last has a
    // syntax error. The final state's diagnostic must appear.
    for (int i = 2; i <= 6; ++i) {
        store.didChange("file:///d.lox", i, "var x = 1;\n// edit\n");
    }
    store.didChange("file:///d.lox", 7, "var x = 1;\nvar y = @;\n");

    const auto ev = sink.waitForCount(2);
    EXPECT_EQ(ev.version, 7);
    ASSERT_EQ(ev.diags.size(), 1U);
    EXPECT_EQ(ev.diags[0].range.start.line, 1U);
    // The whole burst collapsed into a single rebuild.
    EXPECT_LE(sink.count(), 2);
}

TEST(LspStdlibDocs, EveryStdlibNameHasADoc) {
    EXPECT_TRUE(loxpp::lsp::stdlibDocsMissingNames().empty());
    for (std::string_view name : loxpp::tooling::stdlibGlobals()) {
        EXPECT_NE(loxpp::lsp::stdlibGlobalDoc(name), nullptr) << name;
    }
    for (std::string_view name : loxpp::tooling::mathMembers()) {
        const auto* e = loxpp::lsp::mathMemberDoc(name);
        ASSERT_NE(e, nullptr) << name;
        EXPECT_FALSE(loxpp::lsp::renderHover(*e).empty());
    }
}

} // namespace

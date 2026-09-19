// Unit tests for the loxpp-lsp building blocks that are awkward to cover from
// the Python smoke test: JSON-RPC framing, CRLF handling, the debounce, the
// stdlib doc table's coverage of src/tooling/stdlib_names.h, and the
// textDocument/codeAction quick fixes.

#include "lsp/code_action.h"
#include "lsp/document_store.h"
#include "lsp/json_rpc.h"
#include "lsp/protocol.h"
#include "lsp/signature_help.h"
#include "lsp/stdlib_docs.h"
#include "tooling/document_model.h"
#include "tooling/stdlib_names.h"
#include "analyze.h"

#include <gtest/gtest.h>

#include <algorithm>
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

TEST(LspJsonRpc, MissingFieldGivesInvalidParams) {
    // textDocument/rename without newName: the from_json throw must not
    // escape as InternalError with a raw json.exception message.
    std::istringstream in(
        frame({{"jsonrpc", "2.0"},
               {"id", 4},
               {"method", "rename"},
               {"params",
                {{"textDocument", {{"uri", "file:///a.lox"}}},
                 {"position", {{"line", 0}, {"character", 1}}}}}}));
    std::ostringstream out;
    JsonRpc rpc(in, out);
    rpc.onRequest("rename", [](const json& p) {
        return json{{"echo", p.at("newName").get<std::string>()}};
    });
    rpc.run();

    const auto frames = parseFrames(out.str());
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].at("id"), 4);
    EXPECT_EQ(frames[0].at("error").at("code"), -32602); // InvalidParams
    const std::string message = frames[0].at("error").at("message");
    EXPECT_NE(message.find("newName"), std::string::npos) << message;
    EXPECT_NE(message.find("missing field"), std::string::npos) << message;
    EXPECT_EQ(message.find("json.exception"), std::string::npos) << message;
}

TEST(LspJsonRpc, WrongTypeGivesInvalidParams) {
    std::istringstream in(frame({{"jsonrpc", "2.0"},
                                 {"id", 5},
                                 {"method", "hover"},
                                 {"params",
                                  {{"textDocument", {{"uri", "file:///a.lox"}}},
                                   {"position", "here"}}}}));
    std::ostringstream out;
    JsonRpc rpc(in, out);
    rpc.onRequest("hover", [](const json& p) {
        loxpp::lsp::TextDocumentPositionParams parsed = p;
        return json{{"uri", parsed.uri}};
    });
    rpc.run();

    const auto frames = parseFrames(out.str());
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].at("id"), 5);
    EXPECT_EQ(frames[0].at("error").at("code"), -32602); // InvalidParams
    const std::string message = frames[0].at("error").at("message");
    EXPECT_EQ(message.find("json.exception"), std::string::npos) << message;
}

TEST(LspJsonRpc, RpcErrorKeepsItsOwnCodeAndMessage) {
    std::istringstream in(frame({{"jsonrpc", "2.0"},
                                 {"id", 6},
                                 {"method", "rename"},
                                 {"params", json::object()}}));
    std::ostringstream out;
    JsonRpc rpc(in, out);
    rpc.onRequest("rename", [](const json&) -> json {
        throw loxpp::lsp::RpcError{loxpp::lsp::RpcErrorCode::InvalidParams,
                                   "new name is not a valid identifier"};
    });
    rpc.run();

    const auto frames = parseFrames(out.str());
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].at("error").at("code"), -32602);
    EXPECT_EQ(frames[0].at("error").at("message"),
              "new name is not a valid identifier");
}

TEST(LspJsonRpc, NonJsonFailureStaysInternalError) {
    std::istringstream in(frame({{"jsonrpc", "2.0"},
                                 {"id", 7},
                                 {"method", "boom"},
                                 {"params", json::object()}}));
    std::ostringstream out;
    JsonRpc rpc(in, out);
    rpc.onRequest(
        "boom", [](const json&) -> json { throw std::runtime_error("boom"); });
    rpc.run();

    const auto frames = parseFrames(out.str());
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].at("error").at("code"), -32603); // InternalError
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

// Applies zero-length SourceEdits (descending by offset) to a copy of text.
std::string applySourceEdits(std::string text,
                             const std::vector<loxpp::lsp::SourceEdit>& edits) {
    std::vector<loxpp::lsp::SourceEdit> sorted = edits;
    std::ranges::sort(sorted, [](const auto& a, const auto& b) {
        return a.offset > b.offset;
    });
    for (const auto& e : sorted) {
        EXPECT_LE(e.offset + e.length, text.size());
        text.replace(e.offset, e.length, e.newText);
    }
    return text;
}

bool hasExhaustivenessError(const std::string& source) {
    for (const ::Diagnostic& d : analyze(source)) {
        if (d.message.find("Non-exhaustive match") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// No line holds only spaces or tabs. The match fix folds the brace indent
// into its edit, so it must never leave an indent-only line behind.
bool hasWhitespaceOnlyLine(const std::string& text) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line =
            text.substr(start, end == std::string::npos ? end : end - start);
        if (!line.empty() &&
            line.find_first_not_of(" \t") == std::string::npos) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

TEST(LspCodeAction, ParsesExhaustivenessMessage) {
    auto one = loxpp::lsp::parseNonExhaustiveMatch(
        "Non-exhaustive match on enum 'Result': missing arms for: Err");
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->enumName, "Result");
    EXPECT_EQ(one->missing, std::vector<std::string>{"Err"});

    auto many = loxpp::lsp::parseNonExhaustiveMatch(
        "Non-exhaustive match on enum 'Op': missing arms for: Add, Sub");
    ASSERT_TRUE(many.has_value());
    EXPECT_EQ(many->enumName, "Op");
    EXPECT_EQ(many->missing, (std::vector<std::string>{"Add", "Sub"}));

    EXPECT_FALSE(
        loxpp::lsp::parseNonExhaustiveMatch("Expect expression.").has_value());
    EXPECT_FALSE(loxpp::lsp::parseNonExhaustiveMatch("").has_value());
    // An empty arm list or a non-identifier name must not reach source text.
    EXPECT_FALSE(loxpp::lsp::parseNonExhaustiveMatch(
                     "Non-exhaustive match on enum 'E': missing arms for: ")
                     .has_value());
    EXPECT_FALSE(
        loxpp::lsp::parseNonExhaustiveMatch(
            "Non-exhaustive match on enum 'E': missing arms for: A; rm")
            .has_value());
}

TEST(LspCodeAction, MatchFixInsertsMissingArm) {
    const std::string source = "enum Result { Ok(value) Err(msg) }\n"
                               "\n"
                               "fun label(r) {\n"
                               "    return match r {\n"
                               "        case Ok(v) => \"ok\"\n"
                               "    };\n"
                               "}\n";
    ASSERT_TRUE(hasExhaustivenessError(source));

    loxpp::tooling::DocumentModel model(source);
    // The context diagnostic is the compiler's own report, mapped through
    // the model's line index the way publishDiagnostics maps it.
    json context = json::array();
    for (const ::Diagnostic& d : analyze(source)) {
        if (d.message.find("Non-exhaustive match") == std::string::npos) {
            continue;
        }
        const auto a = model.offsetToPosition(d.offset);
        const auto b = model.offsetToPosition(d.offset + d.length);
        context.push_back(
            {{"message", d.message},
             {"range",
              {{"start", {{"line", a.line}, {"character", a.character}}},
               {"end", {{"line", b.line}, {"character", b.character}}}}}});
    }
    ASSERT_EQ(context.size(), 1U);

    const std::vector<loxpp::lsp::QuickFix> fixes =
        loxpp::lsp::matchExhaustivenessFixes(model, 0, context);
    ASSERT_EQ(fixes.size(), 1U);
    EXPECT_EQ(fixes[0].title, "Add missing match arm: Err");
    ASSERT_TRUE(fixes[0].diagnosticIndex.has_value());
    EXPECT_EQ(*fixes[0].diagnosticIndex, 0U);
    ASSERT_EQ(fixes[0].edits.size(), 1U);
    // The edit replaces the brace indent, so its range holds only spaces.
    const auto& edit = fixes[0].edits[0];
    EXPECT_NE(edit.length, 0U);
    EXPECT_EQ(source.substr(edit.offset, edit.length),
              std::string(edit.length, ' '));

    const std::string fixed = applySourceEdits(source, fixes[0].edits);
    EXPECT_FALSE(hasExhaustivenessError(fixed)) << "fixed source:\n" << fixed;
    EXPECT_TRUE(fixed.find("case Err => nil") != std::string::npos);
    EXPECT_FALSE(hasWhitespaceOnlyLine(fixed)) << "fixed source:\n" << fixed;
}

TEST(LspCodeAction, MatchFixDedupesRepeatedDiagnostic) {
    const std::string source = "enum Result { Ok(value) Err(msg) }\n"
                               "\n"
                               "fun label(r) {\n"
                               "    return match r {\n"
                               "        case Ok(v) => \"ok\"\n"
                               "    };\n"
                               "}\n";
    loxpp::tooling::DocumentModel model(source);
    json one = json::array();
    for (const ::Diagnostic& d : analyze(source)) {
        if (d.message.find("Non-exhaustive match") == std::string::npos) {
            continue;
        }
        const auto a = model.offsetToPosition(d.offset);
        const auto b = model.offsetToPosition(d.offset + d.length);
        one.push_back(
            {{"message", d.message},
             {"range",
              {{"start", {{"line", a.line}, {"character", a.character}}},
               {"end", {{"line", b.line}, {"character", b.character}}}}}});
    }
    ASSERT_EQ(one.size(), 1U);

    // The same diagnostic twice still yields one fix: applying two
    // identical fixes would duplicate the arm.
    json repeated = json::array({one[0], one[0]});
    const std::vector<loxpp::lsp::QuickFix> fixes =
        loxpp::lsp::matchExhaustivenessFixes(model, 0, repeated);
    ASSERT_EQ(fixes.size(), 1U);
    EXPECT_EQ(fixes[0].title, "Add missing match arm: Err");
}

TEST(LspCodeAction, MatchFixInsertsSeveralArms) {
    const std::string source = "enum Light { Red Amber Green }\n"
                               "fun go(l) {\n"
                               "    return match l {\n"
                               "        case Red => 1\n"
                               "    };\n"
                               "}\n";
    ASSERT_TRUE(hasExhaustivenessError(source));

    loxpp::tooling::DocumentModel model(source);
    json context = json::array();
    for (const ::Diagnostic& d : analyze(source)) {
        if (d.message.find("Non-exhaustive match") == std::string::npos) {
            continue;
        }
        const auto a = model.offsetToPosition(d.offset);
        const auto b = model.offsetToPosition(d.offset + d.length);
        context.push_back(
            {{"message", d.message},
             {"range",
              {{"start", {{"line", a.line}, {"character", a.character}}},
               {"end", {{"line", b.line}, {"character", b.character}}}}}});
    }
    ASSERT_EQ(context.size(), 1U);

    const std::vector<loxpp::lsp::QuickFix> fixes =
        loxpp::lsp::matchExhaustivenessFixes(model, 0, context);
    ASSERT_EQ(fixes.size(), 1U);
    EXPECT_EQ(fixes[0].title, "Add missing match arms: Amber, Green");

    const std::string fixed = applySourceEdits(source, fixes[0].edits);
    EXPECT_FALSE(hasExhaustivenessError(fixed)) << "fixed source:\n" << fixed;
    EXPECT_FALSE(hasWhitespaceOnlyLine(fixed)) << "fixed source:\n" << fixed;
}

TEST(LspCodeAction, MatchFixNeedsADiagnostic) {
    loxpp::tooling::DocumentModel model("var x = 1;\n");
    EXPECT_TRUE(
        loxpp::lsp::matchExhaustivenessFixes(model, 0, json::array()).empty());
    // An unrelated diagnostic inside a match-free file offers no fix either.
    const json context =
        json::array({{{"message", "Expect expression."},
                      {"range",
                       {{{"start", {{{"line", 0}, {"character", 0}}}},
                         {"end", {{{"line", 0}, {"character", 0}}}}}}}}});
    EXPECT_TRUE(
        loxpp::lsp::matchExhaustivenessFixes(model, 0, context).empty());
}

TEST(LspCodeAction, StrWrapOffersBothOperands) {
    const std::string source = "print \"n=\" + n;\n";
    loxpp::tooling::DocumentModel model(source);
    const std::size_t plus = source.find('+');
    ASSERT_NE(plus, std::string::npos);

    const std::vector<loxpp::lsp::QuickFix> fixes =
        loxpp::lsp::strWrapFixes(model, plus);
    ASSERT_EQ(fixes.size(), 2U);
    EXPECT_EQ(fixes[0].title, "Wrap left operand in str()");
    EXPECT_EQ(fixes[1].title, "Wrap right operand in str()");
    EXPECT_FALSE(fixes[0].diagnosticIndex.has_value());

    EXPECT_EQ(applySourceEdits(source, fixes[0].edits),
              "print str(\"n=\") + n;\n");
    EXPECT_EQ(applySourceEdits(source, fixes[1].edits),
              "print \"n=\" + str(n);\n");
}

TEST(LspCodeAction, StrWrapSkipsOperandsAlreadyInStr) {
    {
        loxpp::tooling::DocumentModel model("print str(a) + b;\n");
        const auto fixes =
            loxpp::lsp::strWrapFixes(model, model.text().find('+'));
        ASSERT_EQ(fixes.size(), 1U);
        EXPECT_EQ(fixes[0].title, "Wrap right operand in str()");
    }
    {
        loxpp::tooling::DocumentModel model("print a + str(b);\n");
        const auto fixes =
            loxpp::lsp::strWrapFixes(model, model.text().find('+'));
        ASSERT_EQ(fixes.size(), 1U);
        EXPECT_EQ(fixes[0].title, "Wrap left operand in str()");
    }
}

TEST(LspCodeAction, StrWrapNeedsAPlus) {
    loxpp::tooling::DocumentModel model("print 1;\n");
    EXPECT_TRUE(loxpp::lsp::strWrapFixes(model, 0).empty());
    EXPECT_TRUE(loxpp::lsp::strWrapFixes(model, model.text().size()).empty());
}

TEST(LspSignatureHelp, StdlibGlobalTracksActiveParameter) {
    loxpp::tooling::DocumentModel model("print str(1, 2);\n");
    const std::string& text = model.text();
    // Inside the first and second argument of str(.
    json first = loxpp::lsp::signatureHelpFor(model, text.find("str(") + 4);
    ASSERT_TRUE(first.is_object());
    EXPECT_EQ(first.at("signatures")[0].at("label"), "str(value) -> String");
    EXPECT_EQ(first.at("activeParameter"), 0);
    json second = loxpp::lsp::signatureHelpFor(model, text.find(", 2") + 1);
    EXPECT_EQ(second.at("activeParameter"), 0); // one param, clamp
}

TEST(LspSignatureHelp, MathMemberAndVariadicClamp) {
    loxpp::tooling::DocumentModel model("print math.pow(2, 3);\n");
    const std::string& text = model.text();
    json help = loxpp::lsp::signatureHelpFor(model, text.find("math.pow(") + 9);
    ASSERT_TRUE(help.is_object());
    EXPECT_EQ(help.at("signatures")[0].at("label"), "math.pow(x, y) -> Number");
    EXPECT_EQ(help.at("activeParameter"), 0);
    json second = loxpp::lsp::signatureHelpFor(model, text.find(", 3") + 1);
    EXPECT_EQ(second.at("activeParameter"), 1);

    // Variadic callMethod stays on the last parameter for extra args.
    loxpp::tooling::DocumentModel v("callMethod(o, \"m\", 1, 2);\n");
    json vhelp = loxpp::lsp::signatureHelpFor(
        v, v.text().find(", 2", v.text().find(", 1")) + 1);
    ASSERT_TRUE(vhelp.is_object());
    EXPECT_EQ(vhelp.at("activeParameter"), 2);
}

TEST(LspSignatureHelp, UserFunctionAndNullCases) {
    loxpp::tooling::DocumentModel model(
        "fun greet(name, day) { return name; }\nprint greet(\"a\", \"b\");\n");
    const std::string& text = model.text();
    json help =
        loxpp::lsp::signatureHelpFor(model, text.find("greet(\"a\"") + 6);
    ASSERT_TRUE(help.is_object());
    EXPECT_EQ(help.at("signatures")[0].at("label"), "fun greet(name, day)");
    EXPECT_EQ(help.at("signatures")[0].at("documentation"),
              "fun greet(name, day)");
    EXPECT_EQ(help.at("activeParameter"), 0);
    json second = loxpp::lsp::signatureHelpFor(model, text.find(", \"b\"") + 1);
    EXPECT_EQ(second.at("activeParameter"), 1);

    // Grouping parens are not a call.
    loxpp::tooling::DocumentModel g("var x = (1 + 2);\n");
    EXPECT_TRUE(
        loxpp::lsp::signatureHelpFor(g, g.text().find("1 +")).is_null());
    // Cursor after the closed call returns null.
    EXPECT_TRUE(
        loxpp::lsp::signatureHelpFor(model, text.find(");") + 1).is_null());
    // Cursor in a complete string returns null.
    loxpp::tooling::DocumentModel s("print str(\"a,b\");\n");
    EXPECT_TRUE(
        loxpp::lsp::signatureHelpFor(s, s.text().find("a,b") + 1).is_null());
}

TEST(LspSignatureHelp, UserDefinitionShadowsStdlib) {
    loxpp::tooling::DocumentModel model(
        "fun str(x, y) { return x; }\nprint str(1, 2);\n");
    const std::string& text = model.text();
    json help = loxpp::lsp::signatureHelpFor(model, text.find("str(1,") + 6);
    ASSERT_TRUE(help.is_object());
    EXPECT_EQ(help.at("signatures")[0].at("label"), "fun str(x, y)");
    EXPECT_EQ(help.at("activeParameter"), 1);
}

TEST(LspSignatureHelp, MethodHeuristicAndLimits) {
    // Map/File methods match by unique name for any receiver.
    loxpp::tooling::DocumentModel m("var who = \"world\";\nprint who.keys(");
    json help = loxpp::lsp::signatureHelpFor(m, m.text().size());
    ASSERT_TRUE(help.is_object());
    EXPECT_EQ(help.at("signatures")[0].at("label"), "map.keys() -> List");

    // Constants have no call signature.
    loxpp::tooling::DocumentModel c("print math.pi(");
    EXPECT_TRUE(loxpp::lsp::signatureHelpFor(c, c.text().size()).is_null());
    loxpp::tooling::DocumentModel o("print math(");
    EXPECT_TRUE(loxpp::lsp::signatureHelpFor(o, o.text().size()).is_null());

    // User-defined methods through a receiver do not resolve: the receiver
    // type is unknown.
    loxpp::tooling::DocumentModel u(
        "class A {\n  foo(a) { return a; }\n}\nvar o = A();\nprint o.foo(1,");
    EXPECT_TRUE(loxpp::lsp::signatureHelpFor(u, u.text().size()).is_null());
}

TEST(LspSignatureHelp, TypingStatesKeepHelp) {
    // An open string with no closing quote is still being typed.
    loxpp::tooling::DocumentModel s("print str(\"abc");
    json shelp = loxpp::lsp::signatureHelpFor(s, s.text().size());
    ASSERT_TRUE(shelp.is_object());
    EXPECT_EQ(shelp.at("signatures")[0].at("label"), "str(value) -> String");

    // A trailing comment to end of file keeps the call.
    loxpp::tooling::DocumentModel c("print math.pow(2, // foo");
    json chelp = loxpp::lsp::signatureHelpFor(c, c.text().size());
    ASSERT_TRUE(chelp.is_object());
    EXPECT_EQ(chelp.at("activeParameter"), 1);

    // Nested calls: on the inner close the inner help shows; between the
    // closes the outer help shows; past both there is no call.
    loxpp::tooling::DocumentModel n("print str(len(\"ab\"))");
    const std::string& t = n.text();
    const std::size_t inner = t.find("))");
    json innerHelp = loxpp::lsp::signatureHelpFor(n, inner);
    ASSERT_TRUE(innerHelp.is_object());
    EXPECT_EQ(innerHelp.at("signatures")[0].at("label"), "len(seq) -> Number");
    json outerHelp = loxpp::lsp::signatureHelpFor(n, inner + 1);
    ASSERT_TRUE(outerHelp.is_object());
    EXPECT_EQ(outerHelp.at("signatures")[0].at("label"),
              "str(value) -> String");
    EXPECT_TRUE(loxpp::lsp::signatureHelpFor(n, inner + 2).is_null());
}

} // namespace

// The property that a text formatter rests on (issue #213).
//
// A formatter prints the token stream that it parsed. That is safe only if a
// second scan of the printed text gives the same tokens. This test proves the
// property over every .lox program in the repository, for the one change a
// formatter makes to a program: it writes one space between two adjacent
// tokens.
//
// The renderer takes each token's source span (Token::offset and
// Token::length), never Token::lexeme. A STRING lexeme has no quotes and an
// ERROR lexeme is a message, so printing either one would change the program.
//
// Three limits, each with its own owner:
//
// 1. Scope. The property is proven for the native scanner (src/scanner.cpp).
//    The tooling parser and the language server scan with the same code, so
//    `loxpp fmt` and the LSP share this proof. The bootstrap interpreters
//    (bootstrap/lox_interpreter.lox, bootstrap/loxpp_interpreter.lox) have
//    their own scanner, and this test does not cover them.
// 2. An error source. A file whose scan reports an ERROR token is not proven
//    here. That token has a real span but no reparsable text, so keeping its
//    span moves the error and can swallow the tokens after it: the case
//    AnErrorSpanCannotBeRenderedBySpacing pins one such file. A formatter must
//    refuse a source like that, or leave it byte for byte.
// 3. Comments. A comment is not a token, so a formatter cannot restore one
//    from the stream. The case CommentsAreNotInTheTokenStream proves that
//    premise. Keeping every comment is a separate property that the formatter
//    owes: issue #847.

#include "corpus.h"
#include "scanner.h"
#include "token.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using loxpp_test::hasStaticErrorMarker;
using loxpp_test::projectRoot;
using loxpp_test::readFile;

// One tree the sweep reads, and the smallest number of .lox files it may hold.
// Raise the floor when you add a program. A floor that fails means programs
// left the tree, which is what a rename does when it moves them out.
struct SweepRoot {
    const char* path;
    std::size_t floor;
};

const std::vector<SweepRoot>& sweepRoots() {
    static const std::vector<SweepRoot> roots = {
        {"benchmarks", 35},
        {"bootstrap", 2},
        {"editors/tree-sitter-loxpp/test/highlight", 3},
        {"examples", 150},
        {"test/compile-errors", 129},
        {"test/coroutine-probes", 11},
        {"test/print-lines", 1},
        {"test/translation-probes", 333},
    };
    return roots;
}

// Whether a root may hold a program that scans to an ERROR token.
enum class ErrorPolicy {
    // Every program in this root must compile, so an ERROR token is a defect.
    Forbidden,
    // The root is a corpus of programs that a compiler must reject, so a
    // lexical error is one of the ways to reject one.
    Allowed,
    // A probe is a program that a compiler must reject only when it says so
    // with `// STATIC-ERROR` (test/corpus.h). Any other probe must compile.
    AllowedForStaticErrorProbe
};

ErrorPolicy errorPolicyFor(std::string_view root) {
    if (root == "test/compile-errors") {
        return ErrorPolicy::Allowed;
    }
    if (root == "test/translation-probes") {
        return ErrorPolicy::AllowedForStaticErrorProbe;
    }
    return ErrorPolicy::Forbidden;
}

// Every .lox file under a root, subdirectories included: the corpus holds
// programs in `benchmarks/core/`, `test/translation-probes/jvm-only/`, and four
// other subdirectories, and a formatter will see them.
std::vector<std::filesystem::path> sweepFiles(std::string_view root) {
    std::vector<std::filesystem::path> files;
    const std::filesystem::path base = projectRoot() / root;
    if (!std::filesystem::is_directory(base)) {
        return files;
    }
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(base)) {
        if (entry.is_regular_file() && entry.path().extension() == ".lox") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string displayPath(const std::filesystem::path& path) {
    return std::filesystem::relative(path, projectRoot()).string();
}

struct Scanned {
    // Every token in order, with EOF_ last.
    std::vector<Token> tokens;
    // True when the scanner reported an error token. The token has a real
    // span, but its lexeme is a message rather than source text, so a rendered
    // program cannot reproduce it (see the header).
    bool hasError{false};
};

// The caller owns `source`: a Token's lexeme is a view into it.
Scanned scanAll(const std::string& source) {
    Scanner scanner(source);
    Scanned out;
    for (;;) {
        Token token = scanner.scanOneToken();
        const bool atEnd = token.type == TokenType::EOF_;
        if (token.type == TokenType::ERROR) {
            out.hasError = true;
        }
        out.tokens.push_back(token);
        if (atEnd) {
            return out;
        }
    }
}

// The source text of the program with `separator` between two tokens, and a
// newline at the end.
std::string render(const std::string& source, const std::vector<Token>& tokens,
                   std::string_view separator) {
    std::string out;
    for (const Token& token : tokens) {
        if (token.type == TokenType::EOF_) {
            break;
        }
        if (!out.empty()) {
            out.append(separator);
        }
        out.append(source, token.offset, token.length);
    }
    out.push_back('\n');
    return out;
}

// The same source with every line comment removed and everything else kept.
// A `//` inside a string literal starts no comment.
std::string stripLineComments(const std::string& source) {
    std::string out;
    out.reserve(source.size());
    bool inString = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        if (inString) {
            out.push_back(c);
            if (c == '\\' && i + 1 < source.size()) {
                out.push_back(source[++i]);
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            out.push_back(c);
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i + 1 < source.size() && source[i + 1] != '\n') {
                ++i;
            }
            continue; // the loop's ++i steps onto the newline, or past the end
        }
        out.push_back(c);
    }
    return out;
}

// Compares two token streams by the two fields that a second scan must
// reproduce. The offsets differ, because the separator moved the text.
// Returns an empty string when the streams are the same, else the first
// difference, so a failure names the token it stopped at.
std::string firstDifference(const std::vector<Token>& a,
                            const std::vector<Token>& b) {
    if (a.size() != b.size()) {
        return "token count " + std::to_string(a.size()) + " then " +
               std::to_string(b.size());
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].type != b[i].type || a[i].lexeme != b[i].lexeme) {
            return "token " + std::to_string(i) + " is '" +
                   std::string(a[i].lexeme) + "' then '" +
                   std::string(b[i].lexeme) + "'";
        }
    }
    return {};
}

} // namespace

// A renamed, deleted, or emptied directory must not turn a sweep into a silent
// no-op. The floor of each root is the count it holds today.
TEST(TokenRoundtrip, EverySweepRootHoldsItsPrograms) {
    for (const SweepRoot& root : sweepRoots()) {
        const std::size_t count = sweepFiles(root.path).size();
        EXPECT_GE(count, root.floor)
            << root.path
            << ": raise the floor when you add a program; a count below it "
               "means programs left this root";
    }
}

// The property: one space between two tokens does not change the token stream.
TEST(TokenRoundtrip, OneSpaceBetweenTokensKeepsTheTokenStream) {
    std::size_t checked = 0;
    std::size_t skipped = 0;
    for (const SweepRoot& root : sweepRoots()) {
        const ErrorPolicy policy = errorPolicyFor(root.path);
        for (const auto& path : sweepFiles(root.path)) {
            SCOPED_TRACE(displayPath(path));
            const std::string source = readFile(path);
            const Scanned first = scanAll(source);
            if (first.hasError) {
                switch (policy) {
                case ErrorPolicy::Forbidden:
                    ADD_FAILURE() << "a program that must compile scans to an "
                                     "error token";
                    break;
                case ErrorPolicy::AllowedForStaticErrorProbe:
                    if (!hasStaticErrorMarker(path)) {
                        ADD_FAILURE() << "an error token in a probe that does "
                                         "not carry // STATIC-ERROR";
                    }
                    break;
                case ErrorPolicy::Allowed:
                    break;
                }
                ++skipped;
                continue;
            }
            // The rendered text must outlive its tokens: every lexeme is a
            // view into it.
            const std::string rendered = render(source, first.tokens, " ");
            const Scanned second = scanAll(rendered);
            ASSERT_FALSE(second.hasError)
                << "the rendered program no longer scans";
            const std::string difference =
                firstDifference(first.tokens, second.tokens);
            EXPECT_TRUE(difference.empty())
                << difference
                << ": the rendered program has a different token stream";
            ++checked;
        }
    }
    RecordProperty("checked", static_cast<int>(checked));
    RecordProperty("skipped", static_cast<int>(skipped));
    // A sweep that quietly skipped everything would pass without a check, so
    // the counts are part of the test.
    EXPECT_GT(checked, 600u);
}

// The mutation on record for the test above: with no separator at all, the
// corpus does not round-trip. Without this, a sweep that compared nothing
// would still pass, and no reader could tell whether one space is necessary.
TEST(TokenRoundtrip, RemovingEverySeparatorChangesTheCorpus) {
    std::size_t diverged = 0;
    for (const SweepRoot& root : sweepRoots()) {
        for (const auto& path : sweepFiles(root.path)) {
            const std::string source = readFile(path);
            const Scanned first = scanAll(source);
            if (first.hasError) {
                continue;
            }
            const std::string joinedText = render(source, first.tokens, "");
            const Scanned joined = scanAll(joinedText);
            if (!firstDifference(first.tokens, joined.tokens).empty()) {
                ++diverged;
            }
        }
    }
    RecordProperty("diverged", static_cast<int>(diverged));
    EXPECT_GT(diverged, 0u)
        << "no program changed when every separator was removed, so the sweep "
           "above does not prove that a separator is needed";
}

// The reason an error source is out of the sweep, pinned on one file. An ERROR
// token has a real span, but that span does not scan back to the same token, so
// a formatter that keeps it moves the error and swallows the tokens after it.
// `print "a\qb";` renders as `print "a\q ;`, where the second scan reads one
// unterminated string and loses the semicolon.
TEST(TokenRoundtrip, AnErrorSpanCannotBeRenderedBySpacing) {
    const std::filesystem::path path =
        projectRoot() / "test/compile-errors/unknown_escape.lox";
    const std::string source = readFile(path);
    const Scanned first = scanAll(source);
    ASSERT_TRUE(first.hasError)
        << displayPath(path) << " no longer reports one";
    const std::string rendered = render(source, first.tokens, " ");
    const Scanned second = scanAll(rendered);
    const std::string difference = firstDifference(first.tokens, second.tokens);
    EXPECT_FALSE(difference.empty())
        << "the rendered text keeps the token stream, so the skip in the sweep "
           "above needs no rule of its own";
    RecordProperty("difference", difference);
}

// The premise of issue #847: a comment is not a token, so no token stream can
// restore one. A program and the same program without its line comments have
// one token stream.
TEST(TokenRoundtrip, CommentsAreNotInTheTokenStream) {
    std::size_t withComments = 0;
    for (const SweepRoot& root : sweepRoots()) {
        for (const auto& path : sweepFiles(root.path)) {
            const std::string source = readFile(path);
            const std::string stripped = stripLineComments(source);
            if (stripped.size() == source.size()) {
                continue; // no comment on any line
            }
            ++withComments;
            SCOPED_TRACE(displayPath(path));
            const Scanned before = scanAll(source);
            const Scanned after = scanAll(stripped);
            if (before.hasError || after.hasError) {
                continue; // out of scope, as in the sweep above
            }
            const std::string difference =
                firstDifference(before.tokens, after.tokens);
            EXPECT_TRUE(difference.empty())
                << difference << ": a comment is part of the token stream";
        }
    }
    RecordProperty("filesWithComments", static_cast<int>(withComments));
    EXPECT_GT(withComments, 100u);
}

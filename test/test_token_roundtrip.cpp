// The property that a text formatter rests on (issue #213).
//
// A formatter prints the token stream it parsed. That is safe only if a second
// scan of the printed text gives the same tokens. This test proves the
// property over the corpus for the one change a formatter makes to a program:
// it writes one space between two adjacent tokens.
//
// The renderer takes each token's source span (Token::offset and
// Token::length), never Token::lexeme. A STRING lexeme has no quotes and an
// ERROR lexeme is a message, so printing either one would change the program.
// A formatter has the same constraint.

#include "corpus.h"
#include "scanner.h"
#include "token.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

using loxpp_test::listLoxFiles;
using loxpp_test::projectRoot;
using loxpp_test::readFile;

// The directories the sweep reads. It is wider than test/corpus.h's
// corpusFiles(): a formatter must be safe on every program in the repository,
// including the compile-error corpus and the coroutine probes.
const std::vector<std::string>& sweepDirectories() {
    static const std::vector<std::string> dirs = {"benchmarks",
                                                  "bootstrap",
                                                  "examples",
                                                  "test/compile-errors",
                                                  "test/coroutine-probes",
                                                  "test/translation-probes"};
    return dirs;
}

struct Scanned {
    // Every token in order, with EOF_ last.
    std::vector<Token> tokens;
    // True when the scanner reported an error token. Such a file has no
    // source text behind one token, so it is out of the sweep.
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

// A renamed or deleted directory must not turn the sweep below into a silent
// no-op.
TEST(TokenRoundtrip, EverySweepDirectoryHasPrograms) {
    for (const std::string& dir : sweepDirectories()) {
        EXPECT_FALSE(listLoxFiles(projectRoot() / dir).empty()) << dir;
    }
}

// The property: one space between two tokens does not change the token stream.
TEST(TokenRoundtrip, OneSpaceBetweenTokensKeepsTheTokenStream) {
    std::size_t checked = 0;
    std::size_t skipped = 0;
    for (const std::string& dir : sweepDirectories()) {
        for (const auto& path : listLoxFiles(projectRoot() / dir)) {
            SCOPED_TRACE(path.string());
            const std::string source = readFile(path);
            const Scanned first = scanAll(source);
            if (first.hasError) {
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
    // A sweep that quietly skipped everything would pass without a check, so
    // the counts are part of the test.
    EXPECT_GT(checked, 400u);
    EXPECT_LT(skipped, 100u);
    RecordProperty("checked", static_cast<int>(checked));
    RecordProperty("skipped", static_cast<int>(skipped));
}

// The mutation on record for the test above: with no separator at all, the
// corpus does not round-trip. Without this, a sweep that compared nothing
// would still pass, and no reader could tell whether one space is necessary.
TEST(TokenRoundtrip, RemovingEverySeparatorChangesTheCorpus) {
    std::size_t diverged = 0;
    for (const std::string& dir : sweepDirectories()) {
        for (const auto& path : listLoxFiles(projectRoot() / dir)) {
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

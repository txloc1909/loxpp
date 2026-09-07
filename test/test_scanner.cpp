#include "scanner.h"
#include <gtest/gtest.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

static std::vector<Token> scanTokens(const char* source) {
    auto scanner = Scanner(source);
    std::vector<Token> tokens;
    for (;;) {
        Token token = scanner.scanOneToken();
        tokens.push_back(token);
        if (token.type == TokenType::EOF_) {
            break;
        }
    }
    return tokens;
}

class ScannerTest : public ::testing::Test {
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ScannerTest, EmptySource) {
    const char* source = "";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::EOF_);
}

TEST_F(ScannerTest, SingleCharacterTokens) {
    const char* source = "(){},.-+;*%";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 12); // 11 tokens + EOF
    EXPECT_EQ(tokens[0].type, TokenType::LEFT_PAREN);
    EXPECT_EQ(tokens[1].type, TokenType::RIGHT_PAREN);
    EXPECT_EQ(tokens[2].type, TokenType::LEFT_BRACE);
    EXPECT_EQ(tokens[3].type, TokenType::RIGHT_BRACE);
    EXPECT_EQ(tokens[4].type, TokenType::COMMA);
    EXPECT_EQ(tokens[5].type, TokenType::DOT);
    EXPECT_EQ(tokens[6].type, TokenType::MINUS);
    EXPECT_EQ(tokens[7].type, TokenType::PLUS);
    EXPECT_EQ(tokens[8].type, TokenType::SEMICOLON);
    EXPECT_EQ(tokens[9].type, TokenType::STAR);
    EXPECT_EQ(tokens[10].type, TokenType::PERCENT);
}

TEST_F(ScannerTest, OneOrTwoCharacterTokens) {
    const char* source = "! != = == > >= < <=";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 9); // 8 tokens + EOF
    EXPECT_EQ(tokens[0].type, TokenType::BANG);
    EXPECT_EQ(tokens[1].type, TokenType::BANG_EQUAL);
    EXPECT_EQ(tokens[2].type, TokenType::EQUAL);
    EXPECT_EQ(tokens[3].type, TokenType::EQUAL_EQUAL);
    EXPECT_EQ(tokens[4].type, TokenType::GREATER);
    EXPECT_EQ(tokens[5].type, TokenType::GREATER_EQUAL);
    EXPECT_EQ(tokens[6].type, TokenType::LESS);
    EXPECT_EQ(tokens[7].type, TokenType::LESS_EQUAL);
    EXPECT_EQ(tokens[8].type, TokenType::EOF_);
}

TEST_F(ScannerTest, FatArrow) {
    // => is FAT_ARROW; = alone is EQUAL; == is EQUAL_EQUAL.
    const char* source = "=> = ==";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 4); // 3 tokens + EOF
    EXPECT_EQ(tokens[0].type, TokenType::FAT_ARROW);
    EXPECT_EQ(tokens[0].lexeme, "=>");
    EXPECT_EQ(tokens[1].type, TokenType::EQUAL);
    EXPECT_EQ(tokens[2].type, TokenType::EQUAL_EQUAL);
}

TEST_F(ScannerTest, MatchKeyword) {
    const char* source = "match";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 2); // match + EOF
    EXPECT_EQ(tokens[0].type, TokenType::MATCH);
    EXPECT_EQ(tokens[0].lexeme, "match");
}

TEST_F(ScannerTest, StringLiteral) {
    const char* source = "\"hello world\"";
    auto tokens = scanTokens(source);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme, "hello world");
}

TEST_F(ScannerTest, StringWithEscapedQuote) {
    // Source: "say \"hi\""  — embedded double quotes via escape
    auto tokens = scanTokens(R"("say \"hi\"")");
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme,
              R"(say \"hi\")"); // raw lexeme keeps backslashes
}

TEST_F(ScannerTest, StringWithEscapeSequences) {
    auto tokens = scanTokens(R"("a\nb\tc\\d")");
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme, R"(a\nb\tc\\d)");
}

TEST_F(ScannerTest, StringUnknownEscape) {
    auto tokens = scanTokens(R"("\q")");
    EXPECT_EQ(tokens[0].type, TokenType::ERROR);
}

TEST_F(ScannerTest, NumberLiteral) {
    const char* source = "123 123.456";
    auto tokens = scanTokens(source);
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[1].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].lexeme, "123");
    EXPECT_EQ(tokens[1].lexeme, "123.456");
}

TEST_F(ScannerTest, Keywords) {
    const char* source = "and class else false fun for if match nil or print "
                         "return super this true var while";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 18); // 17 tokens + EOF
    EXPECT_EQ(tokens[0].type, TokenType::AND);
    EXPECT_EQ(tokens[1].type, TokenType::CLASS);
    EXPECT_EQ(tokens[2].type, TokenType::ELSE);
    EXPECT_EQ(tokens[3].type, TokenType::FALSE);
    EXPECT_EQ(tokens[4].type, TokenType::FUN);
    EXPECT_EQ(tokens[5].type, TokenType::FOR);
    EXPECT_EQ(tokens[6].type, TokenType::IF);
    EXPECT_EQ(tokens[7].type, TokenType::MATCH);
    EXPECT_EQ(tokens[8].type, TokenType::NIL);
    EXPECT_EQ(tokens[9].type, TokenType::OR);
    EXPECT_EQ(tokens[10].type, TokenType::PRINT);
    EXPECT_EQ(tokens[11].type, TokenType::RETURN);
    EXPECT_EQ(tokens[12].type, TokenType::SUPER);
    EXPECT_EQ(tokens[13].type, TokenType::THIS);
    EXPECT_EQ(tokens[14].type, TokenType::TRUE);
    EXPECT_EQ(tokens[15].type, TokenType::VAR);
    EXPECT_EQ(tokens[16].type, TokenType::WHILE);
    EXPECT_EQ(tokens[17].type, TokenType::EOF_);
}

TEST_F(ScannerTest, ElipsisToken) {
    // "..." scans as ELIPSIS; "." alone scans as DOT
    const char* source = "... .";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 3); // ELIPSIS, DOT, EOF
    EXPECT_EQ(tokens[0].type, TokenType::ELIPSIS);
    EXPECT_EQ(tokens[0].lexeme, "...");
    EXPECT_EQ(tokens[1].type, TokenType::DOT);
    EXPECT_EQ(tokens[1].lexeme, ".");
}

TEST_F(ScannerTest, ElipsisInListPattern) {
    // ...rest inside a match arm bracket
    const char* source = "[head, ...tail]";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 7); // [ head , ... tail ] EOF
    EXPECT_EQ(tokens[0].type, TokenType::LEFT_BRACKET);
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[2].type, TokenType::COMMA);
    EXPECT_EQ(tokens[3].type, TokenType::ELIPSIS);
    EXPECT_EQ(tokens[4].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[4].lexeme, "tail");
    EXPECT_EQ(tokens[5].type, TokenType::RIGHT_BRACKET);
}

TEST_F(ScannerTest, WhitespaceHandling) {
    const char* source = "   \t\r 123   \n  456  ";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 3); // two numbers + EOF
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].lexeme, "123");
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_EQ(tokens[1].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[1].lexeme, "456");
    EXPECT_EQ(tokens[1].line, 2);
    EXPECT_EQ(tokens[2].type, TokenType::EOF_);
}

TEST_F(ScannerTest, CommentHandling) {
    const char* source = "// this is a comment\n"
                         "123 // comment after number\n"
                         "// another comment\n"
                         "456";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 3); // two numbers + EOF
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].lexeme, "123");
    EXPECT_EQ(tokens[1].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[1].lexeme, "456");
    EXPECT_EQ(tokens[2].type, TokenType::EOF_);
}

TEST_F(ScannerTest, TokenOffsetFirstToken) {
    const char* source = "foo + bar";
    auto tokens = scanTokens(source);
    EXPECT_EQ(tokens[0].offset, 0);
    EXPECT_EQ(tokens[0].length, 3);
    EXPECT_EQ(tokens[1].offset, 4); // "+"
    EXPECT_EQ(tokens[1].length, 1);
    EXPECT_EQ(tokens[2].offset, 6); // "bar"
    EXPECT_EQ(tokens[2].length, 3);
}

TEST_F(ScannerTest, TokenOffsetLeadingWhitespace) {
    const char* source = "   123";
    auto tokens = scanTokens(source);
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].offset, 3);
}

TEST_F(ScannerTest, TokenOffsetSameLexemeAcrossLines) {
    // "foo" appears on line 1 and line 3; the offsets must differ.
    const char* source = "foo bar\n"
                         "baz\n"
                         "foo qux";
    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 6); // foo bar baz foo qux EOF
    EXPECT_EQ(tokens[0].lexeme, "foo");
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_EQ(tokens[0].offset, 0);
    EXPECT_EQ(tokens[3].lexeme, "foo");
    EXPECT_EQ(tokens[3].line, 3);
    EXPECT_EQ(tokens[3].offset, 12);
    EXPECT_NE(tokens[0].offset, tokens[3].offset);
}

TEST_F(ScannerTest, TokenOffsetPastLineComment) {
    const char* source = "// header comment\n"
                         "value";
    auto tokens = scanTokens(source);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "value");
    EXPECT_EQ(tokens[0].offset, 18); // length of "// header comment\n"
}

TEST_F(ScannerTest, TokenOffsetPastTrailingComment) {
    const char* source = "a // trailing\n"
                         "b";
    auto tokens = scanTokens(source);
    EXPECT_EQ(tokens[0].offset, 0);
    EXPECT_EQ(tokens[1].lexeme, "b");
    EXPECT_EQ(tokens[1].offset, 14); // length of "a // trailing\n"
}

TEST_F(ScannerTest, TokenOffsetStringLexeme) {
    // The STRING lexeme drops the quotes, but offset/length still describe the
    // full source span, both quotes included.
    const char* source = "  \"hi\"";
    auto tokens = scanTokens(source);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme, "hi");
    EXPECT_EQ(tokens[0].offset, 2); // the opening quote
    EXPECT_EQ(tokens[0].length, 4); // "hi" with both quotes
    // The contract in token.h: source.substr(offset, length) is the token text.
    EXPECT_EQ(std::string(source).substr(tokens[0].offset, tokens[0].length),
              "\"hi\"");
}

TEST_F(ScannerTest, ErrorTokenOffset) {
    const char* source = "abc \x01";
    auto tokens = scanTokens(source);
    EXPECT_EQ(tokens[1].type, TokenType::ERROR);
    // offset points AT the offending character, and length covers it.
    EXPECT_EQ(tokens[1].offset, 4);
    EXPECT_EQ(tokens[1].length, 1);
}

TEST_F(ScannerTest, ErrorTokenOffsetBadEscape) {
    const char* source = "x = \"a\\qb\";";
    auto tokens = scanTokens(source);
    EXPECT_EQ(tokens[2].type, TokenType::ERROR);
    // offset is the opening quote, the start of the problem token.
    EXPECT_EQ(tokens[2].offset, 4);
}

TEST_F(ScannerTest, ErrorTokenOffsetUnterminatedString) {
    const char* source = "x = \"abc";
    auto tokens = scanTokens(source);
    EXPECT_EQ(tokens[2].type, TokenType::ERROR);
    // offset is the opening quote; the span stays inside the buffer, unlike a
    // value equal to the source length.
    EXPECT_EQ(tokens[2].offset, 4);
    EXPECT_EQ(tokens[2].offset + tokens[2].length, std::strlen(source));
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#include "scanner.h"
#include <gtest/gtest.h>

#include <cassert>
#include <iostream>
#include <vector>

static std::vector<Token> scanTokens(const char* source) {
    auto scanner = Scanner(source);
    std::vector<Token> tokens;
    for (;;) {
        Token token = scanner.scanOneToken();
        tokens.push_back(token);
        if (token.type == TokenType::EOF_)
            break;
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

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
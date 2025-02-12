#include "scanner.h"

#include <cassert>
#include <iostream>
#include <vector>

static std::vector<Token> scanTokens(const std::string& source) {
    Scanner scanner(source);
    std::vector<Token> tokens;
    for (;;) {
        Token token = scanner.scanOneToken();
        tokens.push_back(token);
        if (token.type == TokenType::EOF_)
            break;
    }
    return tokens;
}

void test_empty_source() {
    auto tokens = scanTokens("");
    assert(tokens.size() == 1);
    assert(tokens[0].type == TokenType::EOF_);
    std::cout << "Empty source test passed!\n";
}

void test_single_character_tokens() {
    auto tokens = scanTokens("(){},.-+;*");
    assert(tokens.size() == 11); // 10 tokens + EOF
    assert(tokens[0].type == TokenType::LEFT_PAREN);
    assert(tokens[1].type == TokenType::RIGHT_PAREN);
    assert(tokens[2].type == TokenType::LEFT_BRACE);
    assert(tokens[3].type == TokenType::RIGHT_BRACE);
    assert(tokens[4].type == TokenType::COMMA);
    assert(tokens[5].type == TokenType::DOT);
    assert(tokens[6].type == TokenType::MINUS);
    assert(tokens[7].type == TokenType::PLUS);
    assert(tokens[8].type == TokenType::SEMICOLON);
    assert(tokens[9].type == TokenType::STAR);
    std::cout << "Single character tokens test passed!\n";
}

void test_one_or_two_character_tokens() {
    auto tokens = scanTokens("! != = == > >= < <=");
    assert(tokens.size() == 9); // 8 tokens + EOF
    assert(tokens[0].type == TokenType::BANG);
    assert(tokens[1].type == TokenType::BANG_EQUAL);
    assert(tokens[2].type == TokenType::EQUAL);
    assert(tokens[3].type == TokenType::EQUAL_EQUAL);
    assert(tokens[4].type == TokenType::GREATER);
    assert(tokens[5].type == TokenType::GREATER_EQUAL);
    assert(tokens[6].type == TokenType::LESS);
    assert(tokens[7].type == TokenType::LESS_EQUAL);
    assert(tokens[8].type == TokenType::EOF_);
    std::cout << "One or two character tokens test passed!\n";
}

void test_string_literal() {
    auto tokens = scanTokens("\"hello world\"");
    assert(tokens[0].type == TokenType::STRING);
    assert(tokens[0].lexeme == "\"hello world\"");
    std::cout << "String literal test passed!\n";
}

void test_number_literal() {
    auto tokens = scanTokens("123 123.456");
    assert(tokens[0].type == TokenType::NUMBER);
    assert(tokens[1].type == TokenType::NUMBER);
    assert(tokens[0].lexeme == "123");
    assert(tokens[1].lexeme == "123.456");
    std::cout << "Number literal test passed!\n";
}

void test_keywords() {
    auto tokens = scanTokens("and class else false fun for if nil or print "
                             "return super this true var while");
    assert(tokens.size() == 17); // 16 tokens + EOF
    assert(tokens[0].type == TokenType::AND);
    assert(tokens[1].type == TokenType::CLASS);
    assert(tokens[2].type == TokenType::ELSE);
    assert(tokens[3].type == TokenType::FALSE);
    assert(tokens[4].type == TokenType::FUN);
    assert(tokens[5].type == TokenType::FOR);
    assert(tokens[6].type == TokenType::IF);
    assert(tokens[7].type == TokenType::NIL);
    assert(tokens[8].type == TokenType::OR);
    assert(tokens[9].type == TokenType::PRINT);
    assert(tokens[10].type == TokenType::RETURN);
    assert(tokens[11].type == TokenType::SUPER);
    assert(tokens[12].type == TokenType::THIS);
    assert(tokens[13].type == TokenType::TRUE);
    assert(tokens[14].type == TokenType::VAR);
    assert(tokens[15].type == TokenType::WHILE);
    assert(tokens[16].type == TokenType::EOF_);
    std::cout << "Keywords test passed!\n";
}

void test_whitespace_handling() {
    auto tokens = scanTokens("   \t\r\n  123   \n  456  ");
    assert(tokens.size() == 3); // two numbers + EOF
    assert(tokens[0].type == TokenType::NUMBER);
    assert(tokens[0].lexeme == "123");
    assert(tokens[0].line == 1);
    assert(tokens[1].type == TokenType::NUMBER);
    assert(tokens[1].lexeme == "456");
    assert(tokens[1].line == 2);
    assert(tokens[2].type == TokenType::EOF_);
    std::cout << "Whitespace handling test passed!\n";
}

void test_comment_handling() {
    auto tokens = scanTokens("// this is a comment\n"
                             "123 // comment after number\n"
                             "// another comment\n"
                             "456");
    assert(tokens.size() == 3); // two numbers + EOF
    assert(tokens[0].type == TokenType::NUMBER);
    assert(tokens[0].lexeme == "123");
    assert(tokens[1].type == TokenType::NUMBER);
    assert(tokens[1].lexeme == "456");
    assert(tokens[2].type == TokenType::EOF_);
    std::cout << "Comment handling test passed!\n";
}

int main() {
    test_empty_source();
    test_single_character_tokens();
    test_one_or_two_character_tokens();
    test_string_literal();
    test_number_literal();
    test_keywords();
    test_whitespace_handling();
    test_comment_handling();

    std::cout << "All scanner tests passed!\n";
    return 0;
}
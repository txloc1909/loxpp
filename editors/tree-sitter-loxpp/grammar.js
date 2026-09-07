/**
 * @file Lox++ grammar for tree-sitter
 * @author Lox++ contributors
 * @license MIT
 *
 * Transcribed from spec/01-lexical.md and spec/02-syntax.md.
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

const PREC = {
  assign: 1,
  or: 2,
  and: 3,
  equality: 4,
  comparison: 5,
  term: 6,
  factor: 7,
  unary: 8,
  call: 9,
};

module.exports = grammar({
  name: "loxpp",

  word: $ => $.identifier,

  extras: $ => [/\s/, $.comment],

  conflicts: $ => [
    [$.block, $.map],
  ],

  supertypes: $ => [
    $._declaration,
    $._statement,
    $._expression,
    $._primary,
  ],

  rules: {
    source_file: $ => repeat($._declaration),

    // -------------------------------------------------------------------
    // Declarations
    // -------------------------------------------------------------------
    _declaration: $ => choice(
      $.class_declaration,
      $.function_declaration,
      $.variable_declaration,
      $.enum_declaration,
      $._statement,
    ),

    class_declaration: $ => seq(
      "class",
      field("name", $.identifier),
      optional(seq("<", field("superclass", $.identifier))),
      field("body", $.class_body),
    ),

    class_body: $ => seq("{", repeat($.method_definition), "}"),

    method_definition: $ => seq(
      field("name", $.identifier),
      field("parameters", $.parameters),
      field("body", $.block),
    ),

    function_declaration: $ => seq(
      "fun",
      field("name", $.identifier),
      field("parameters", $.parameters),
      field("body", $.block),
    ),

    parameters: $ => seq(
      "(",
      optional(seq($.identifier, repeat(seq(",", $.identifier)))),
      ")",
    ),

    variable_declaration: $ => seq(
      "var",
      choice(
        seq(
          field("name", $.identifier),
          optional(seq("=", field("value", $._expression))),
        ),
        seq(field("pattern", $.object_pattern), "=", field("value", $._expression)),
        seq(field("pattern", $.list_pattern), "=", field("value", $._expression)),
      ),
      ";",
    ),

    object_pattern: $ => seq(
      "{",
      $.identifier,
      repeat(seq(",", $.identifier)),
      optional(","),
      "}",
    ),

    list_pattern: $ => seq(
      "[",
      $.identifier,
      repeat(seq(",", $.identifier)),
      optional(","),
      "]",
    ),

    enum_declaration: $ => seq(
      "enum",
      field("name", $.identifier),
      "{",
      repeat($.enum_variant),
      "}",
    ),

    enum_variant: $ => seq(
      field("name", $.identifier),
      optional(seq(
        "(",
        field("field", $.identifier),
        repeat(seq(",", field("field", $.identifier))),
        ")",
      )),
    ),

    // -------------------------------------------------------------------
    // Statements
    // -------------------------------------------------------------------
    _statement: $ => choice(
      $.expression_statement,
      $.for_statement,
      $.if_statement,
      $.print_statement,
      $.return_statement,
      $.while_statement,
      $.break_statement,
      $.continue_statement,
      $.block,
    ),

    block: $ => seq("{", repeat($._declaration), "}"),

    expression_statement: $ => seq($._expression, ";"),

    if_statement: $ => prec.right(seq(
      "if", "(", field("condition", $._expression), ")",
      field("consequence", $._statement),
      optional(seq("else", field("alternative", $._statement))),
    )),

    while_statement: $ => seq(
      "while", "(", field("condition", $._expression), ")",
      field("body", $._statement),
    ),

    for_statement: $ => choice(
      seq(
        "for", "(",
        field("initializer", choice($.variable_declaration, $.expression_statement, ";")),
        field("condition", optional($._expression)), ";",
        field("update", optional($._expression)),
        ")",
        field("body", $._statement),
      ),
      seq(
        "for", "(", "var",
        field("name", $.identifier), "in", field("iterable", $._expression),
        ")",
        field("body", $._statement),
      ),
    ),

    print_statement: $ => seq("print", $._expression, ";"),

    return_statement: $ => seq("return", optional($._expression), ";"),

    break_statement: _ => seq("break", ";"),

    continue_statement: _ => seq("continue", ";"),

    // -------------------------------------------------------------------
    // Expressions
    // -------------------------------------------------------------------
    _expression: $ => choice(
      $.assignment_expression,
      $.binary_expression,
      $.unary_expression,
      $._primary,
    ),

    assignment_expression: $ => prec.right(PREC.assign, seq(
      field("left", choice($.identifier, $.field_expression, $.subscript_expression)),
      "=",
      field("right", $._expression),
    )),

    binary_expression: $ => {
      const table = [
        ["or", PREC.or],
        ["and", PREC.and],
        ["==", PREC.equality],
        ["!=", PREC.equality],
        ["<", PREC.comparison],
        ["<=", PREC.comparison],
        [">", PREC.comparison],
        [">=", PREC.comparison],
        ["in", PREC.comparison],
        ["+", PREC.term],
        ["-", PREC.term],
        ["*", PREC.factor],
        ["/", PREC.factor],
        ["%", PREC.factor],
      ];
      return choice(...table.map(([operator, precedence]) => prec.left(precedence, seq(
        field("left", $._expression),
        field("operator", operator),
        field("right", $._expression),
      ))));
    },

    unary_expression: $ => prec.right(PREC.unary, seq(
      field("operator", choice("!", "-")),
      field("operand", $._expression),
    )),

    _primary: $ => choice(
      $.call_expression,
      $.field_expression,
      $.subscript_expression,
      $.slice_expression,
      $.parenthesized_expression,
      $.list,
      $.map,
      $.match_expression,
      $.this_expression,
      $.super_expression,
      $.identifier,
      $.number,
      $.string,
      $.true,
      $.false,
      $.nil,
    ),

    call_expression: $ => prec(PREC.call, seq(
      field("function", $._primary),
      field("arguments", $.arguments),
    )),

    arguments: $ => seq(
      "(",
      optional(seq($._expression, repeat(seq(",", $._expression)))),
      ")",
    ),

    field_expression: $ => prec(PREC.call, seq(
      field("object", $._primary),
      ".",
      field("property", $.identifier),
    )),

    subscript_expression: $ => prec(PREC.call, seq(
      field("object", $._primary),
      "[",
      field("index", $._expression),
      "]",
    )),

    slice_expression: $ => prec(PREC.call, seq(
      field("object", $._primary),
      "[",
      field("start", $._expression),
      ":",
      field("end", $._expression),
      "]",
    )),

    parenthesized_expression: $ => seq("(", $._expression, ")"),

    list: $ => seq(
      "[",
      optional(seq($._expression, repeat(seq(",", $._expression)))),
      "]",
    ),

    map: $ => seq(
      "{",
      optional(seq($.pair, repeat(seq(",", $.pair)))),
      "}",
    ),

    pair: $ => seq(
      field("key", $._expression),
      ":",
      field("value", $._expression),
    ),

    this_expression: _ => "this",

    super_expression: $ => seq("super", ".", field("property", $.identifier)),

    // -------------------------------------------------------------------
    // match expression
    // -------------------------------------------------------------------
    match_expression: $ => seq(
      "match",
      field("value", $._expression),
      "{",
      repeat($.match_arm),
      "}",
    ),

    match_arm: $ => seq(
      "case",
      field("pattern", $._arm_patterns),
      optional(field("guard", $.guard)),
      "=>",
      field("body", $._arm_body),
    ),

    guard: $ => seq("if", $._expression),

    _arm_patterns: $ => choice(
      seq($.literal_pattern, repeat(seq(",", $.literal_pattern))),
      seq($._arm_pattern, repeat(seq("or", $._arm_pattern))),
    ),

    literal_pattern: $ => choice(
      $.number,
      $.string,
      $.true,
      $.false,
      $.nil,
    ),

    _arm_pattern: $ => choice(
      $.binding_pattern,
      $.constructor_pattern,
      $.record_pattern,
      $.sequence_pattern,
      $._arm_pattern_ident,
    ),

    // A bare identifier pattern: the match wildcard `_`, a value binding, or a
    // zero-field constructor tag check. The three roles are a resolver
    // concern, not a syntactic one.
    _arm_pattern_ident: $ => alias($.identifier, $.identifier_pattern),

    binding_pattern: $ => seq(
      field("name", $.identifier),
      "@",
      field("subpattern", $._sub_pattern),
    ),

    constructor_pattern: $ => seq(
      field("name", $.identifier),
      "(",
      field("binding", $.identifier),
      repeat(seq(",", field("binding", $.identifier))),
      ")",
    ),

    record_pattern: $ => seq(
      field("name", $.identifier),
      "{",
      field("binding", $.identifier),
      repeat(seq(",", field("binding", $.identifier))),
      "}",
    ),

    sequence_pattern: $ => seq(
      "[",
      optional(seq($._sequence_element, repeat(seq(",", $._sequence_element)))),
      "]",
    ),

    _sequence_element: $ => choice(
      $.rest_element,
      alias($.identifier, $.identifier_pattern),
    ),

    rest_element: $ => seq("...", field("name", $.identifier)),

    _sub_pattern: $ => choice(
      $.constructor_pattern,
      $.record_pattern,
      $.sequence_pattern,
      alias($.identifier, $.identifier_pattern),
    ),

    _arm_body: $ => choice(
      $.block_arm_body,
      $._expression,
    ),

    // `{ declaration* expression }` — an arm body that runs statements before
    // its result expression. Named to keep it distinct from `block` and `map`.
    block_arm_body: $ => seq(
      "{",
      repeat($._declaration),
      $._expression,
      "}",
    ),

    // -------------------------------------------------------------------
    // Terminals
    // -------------------------------------------------------------------
    identifier: _ => /[a-zA-Z_][a-zA-Z0-9_]*/,

    number: _ => token(/\d+(\.\d+)?/),

    string: $ => seq(
      '"',
      repeat(choice($.escape_sequence, $.string_content)),
      '"',
    ),

    string_content: _ => token.immediate(prec(1, /[^"\\]+/)),

    escape_sequence: _ => token.immediate(/\\["\\ntr0]/),

    true: _ => "true",
    false: _ => "false",
    nil: _ => "nil",

    comment: _ => token(seq("//", /[^\n]*/)),
  },
});

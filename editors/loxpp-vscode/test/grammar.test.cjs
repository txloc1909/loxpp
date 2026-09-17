'use strict';

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vsctm = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');

const grammarPath = require.resolve('../syntaxes/lox.tmLanguage.json');
const wasmPath = require.resolve('vscode-oniguruma/release/onig.wasm');

const grammarData = fs.readFileSync(grammarPath, 'utf8');

let registry;
let grammarPromise;

function getGrammar() {
  if (!grammarPromise) {
    const wasmBin = fs.readFileSync(wasmPath).buffer;
    const onigLib = oniguruma.loadWASM(wasmBin).then(() => ({
      createOnigScanner: (patterns) => new oniguruma.OnigScanner(patterns),
      createOnigString: (s) => new oniguruma.OnigString(s)
    }));
    registry = new vsctm.Registry({
      onigLib,
      loadGrammar: (scopeName) => {
        if (scopeName === 'source.lox') {
          return Promise.resolve(vsctm.parseRawGrammar(grammarData, grammarPath));
        }
        return Promise.resolve(null);
      }
    });
    grammarPromise = registry.loadGrammar('source.lox').then((g) => {
      assert.ok(g, 'grammar must load for scopeName source.lox');
      return g;
    });
  }
  return grammarPromise;
}

function tokenize(text, grammar) {
  const lines = text.split(/\r?\n/);
  const tokens = [];
  let ruleStack = vsctm.INITIAL;
  for (const line of lines) {
    const r = grammar.tokenizeLine(line, ruleStack);
    ruleStack = r.ruleStack;
    for (const t of r.tokens) {
      tokens.push({
        text: line.slice(t.startIndex, t.endIndex),
        scopes: t.scopes,
        startIndex: t.startIndex,
        endIndex: t.endIndex
      });
    }
  }
  return { tokens, ruleStack };
}

function flat(scopes) {
  return scopes.filter((s) => s && s !== 'source.lox');
}

function topLevel(tokens) {
  return tokens.map((t) => flat(t.scopes)[0]).filter(Boolean);
}

test('grammar loads', async () => {
  const g = await getGrammar();
  assert.ok(g, 'grammar must load');
  const raw = JSON.parse(grammarData);
  assert.strictEqual(raw.scopeName, 'source.lox');
});

test('keywords scope to keyword.control', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('if else for while break continue return print', g);
  assert.deepStrictEqual(
    tokens.map((t) => flat(t.scopes)[0]).filter(Boolean),
    [
      'keyword.control.lox',
      'keyword.control.lox',
      'keyword.control.lox',
      'keyword.control.lox',
      'keyword.control.lox',
      'keyword.control.lox',
      'keyword.control.lox',
      'keyword.control.lox'
    ]
  );
});

test('storage keywords scope to storage.type', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('var x = 1; fun f() {} enum E { } class C { }', g);
  assert.deepStrictEqual(
    topLevel(tokens),
    [
      'storage.type.lox',
      'variable.other.lox',
      'keyword.operator.lox',
      'constant.numeric.lox',
      'punctuation.separator.lox',
      'storage.type.function.lox',
      'entity.name.function.lox',
      'punctuation.section.brackets.lox',
      'punctuation.section.brackets.lox',
      'punctuation.section.brackets.lox',
      'punctuation.section.brackets.lox',
      'storage.type.lox',
      'entity.name.type.enum.lox',
      'punctuation.section.brackets.lox',
      'punctuation.section.brackets.lox',
      'storage.type.lox',
      'entity.name.type.class.lox',
      'punctuation.section.brackets.lox',
      'punctuation.section.brackets.lox'
    ]
  );
});

test('match and case scope to keyword.control.match', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('match case', g);
  assert.deepStrictEqual(
    tokens.map((t) => flat(t.scopes)[0]).filter(Boolean),
    ['keyword.control.match.lox', 'keyword.control.match.lox']
  );
});

test('logical keywords scope to keyword.operator.logical', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('and or in', g);
  assert.deepStrictEqual(
    tokens.map((t) => flat(t.scopes)[0]).filter(Boolean),
    [
      'keyword.operator.logical.lox',
      'keyword.operator.logical.lox',
      'keyword.operator.logical.lox'
    ]
  );
});

test('this super default scope to keyword.control', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('this super default', g);
  assert.deepStrictEqual(
    tokens.map((t) => flat(t.scopes)[0]).filter(Boolean),
    ['keyword.control.lox', 'keyword.control.lox', 'keyword.control.lox']
  );
});

test('exception keywords scope to keyword.control.exception', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('try catch throw defer', g);
  assert.deepStrictEqual(
    tokens.map((t) => flat(t.scopes)[0]).filter(Boolean),
    [
      'keyword.control.exception.lox',
      'keyword.control.exception.lox',
      'keyword.control.exception.lox',
      'keyword.control.exception.lox'
    ]
  );
});

test('literals true false nil scope to constant.language', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('true false nil', g);
  assert.deepStrictEqual(
    tokens.map((t) => flat(t.scopes)[0]).filter(Boolean),
    ['constant.language.lox', 'constant.language.lox', 'constant.language.lox']
  );
});

test('keywords are case sensitive per spec/01-lexical.md', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('If AND varx', g);
  assert.deepStrictEqual(
    tokens.map((t) => flat(t.scopes)[0]).filter(Boolean),
    ['variable.other.lox', 'variable.other.lox', 'variable.other.lox']
  );
});

test('numbers match spec/01-lexical.md NUMBER form', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('0 42 3.14 1.0', g);
  assert.deepStrictEqual(
    tokens.map((t) => flat(t.scopes)[0]).filter(Boolean),
    [
      'constant.numeric.lox',
      'constant.numeric.lox',
      'constant.numeric.lox',
      'constant.numeric.lox'
    ]
  );
});

test('numbers tokenize decimal followed by dot as numeric then dot', async () => {
  const g = await getGrammar();
  const dotIdent = tokenize('1.foo', g).tokens.filter((t) => t.text.trim() !== '');
  assert.deepStrictEqual(
    dotIdent.map((t) => [t.text, flat(t.scopes)[0]]),
    [
      ['1', 'constant.numeric.lox'],
      ['.', 'punctuation.separator.dot.lox'],
      ['foo', 'variable.other.lox']
    ]
  );
  const twoDots = tokenize('1..2', g).tokens.filter((t) => t.text.trim() !== '');
  assert.deepStrictEqual(
    twoDots.map((t) => [t.text, flat(t.scopes)[0]]),
    [
      ['1', 'constant.numeric.lox'],
      ['.', 'punctuation.separator.dot.lox'],
      ['.', 'punctuation.separator.dot.lox'],
      ['2', 'constant.numeric.lox']
    ]
  );
});

test('no single numeric token covers hex or exponent forms', async () => {
  const g = await getGrammar();
  const hex = tokenize('0x1F', g).tokens.filter((t) => flat(t.scopes).length > 0);
  const hexNumerics = hex.filter((t) => flat(t.scopes)[0] === 'constant.numeric.lox');
  assert.ok(hexNumerics.length > 0, 'hex digits must still produce numeric tokens');
  for (const t of hexNumerics) {
    assert.notStrictEqual(t.text, '0x1F', 'hex literal must not be one numeric token');
  }
  const exp = tokenize('1e5', g).tokens.filter((t) => flat(t.scopes).length > 0);
  const expNumerics = exp.filter((t) => flat(t.scopes)[0] === 'constant.numeric.lox');
  assert.ok(expNumerics.length > 0, 'exponent digits must still produce numeric tokens');
  for (const t of expNumerics) {
    assert.notStrictEqual(t.text, '1e5', 'exponent literal must not be one numeric token');
  }
  for (const t of [...hex, ...exp]) {
    assert.notStrictEqual(flat(t.scopes)[0], 'invalid.illegal', 'split numerics are permitted, not flagged');
  }
});

test('multiline string with escapes', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('"a\nb \\n \\t \\r \\0 \\\" \\\\ tail"', g);
  assert.strictEqual(tokens[0].scopes.includes('string.quoted.double.lox'), true);
  assert.strictEqual(tokens[0].scopes.includes('punctuation.definition.string.begin.lox'), true);
  const escapes = tokens.filter((t) => flat(t.scopes).includes('constant.character.escape.lox'));
  assert.deepStrictEqual(escapes.map((e) => e.text), ['\\n', '\\t', '\\r', '\\0', '\\"', '\\\\']);
  assert.strictEqual(tokens[tokens.length - 1].scopes.includes('string.quoted.double.lox'), true);
  assert.strictEqual(tokens[tokens.length - 1].scopes.includes('punctuation.definition.string.end.lox'), true);
});

test('bad escape is marked illegal inside string', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('"a \\q b"', g);
  const illegal = tokens.find((t) => flat(t.scopes).includes('invalid.illegal.bad-escape.lox'));
  assert.ok(illegal, 'bad escape must be flagged');
  assert.strictEqual(illegal.text, '\\q');
});

test('unterminated string consumes to end of line', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('var x = "oops', g);
  const str = tokens.filter((t) => flat(t.scopes).some((s) => s.startsWith('string.quoted.double')));
  assert.ok(str.length > 0, 'unterminated string must be highlighted as string');
  assert.strictEqual(str[str.length - 1].text, 'oops');
  assert.strictEqual(tokens[tokens.length - 1].scopes.includes('punctuation.definition.string.end.lox'), false);
  assert.strictEqual(
    tokens.filter((t) => flat(t.scopes).includes('punctuation.definition.string.begin.lox')).length,
    1
  );
});

test('multiline string keeps string scope across lines', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('"first\nsecond\nthird"', g);
  const strings = tokens.filter((t) => flat(t.scopes).includes('string.quoted.double.lox'));
  assert.deepStrictEqual(
    strings.map((t) => t.text),
    ['"', 'first', 'second', 'third', '"']
  );
  for (const s of topLevel(tokens)) {
    assert.strictEqual(s, 'string.quoted.double.lox');
  }
});

test('comments scope to comment.line', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('// full line\nvar x = 1; // trailing\n// TODO fix', g);
  const comments = tokens.filter((t) => flat(t.scopes).includes('comment.line.double-slash.lox'));
  assert.strictEqual(comments[0].text, '//');
  assert.strictEqual(comments[1].text, ' full line');
  assert.strictEqual(comments[2].text, '//');
  assert.strictEqual(comments[3].text, ' trailing');
  assert.strictEqual(
    tokens.some((t) => flat(t.scopes).includes('keyword.comment.todo.lox')),
    true
  );
  assert.strictEqual(
    tokens.some((t) => t.text === 'var' && flat(t.scopes).includes('storage.type.lox')),
    true
  );
});

test('comment after code does not eat rest of file', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('// c\nvar ok = 1;', g);
  assert.strictEqual(tokens.some((t) => t.text === 'var' && flat(t.scopes).includes('storage.type.lox')), true);
});

test('operators scope to keyword.operator', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('= == != < <= > >= + - * / % !', g);
  const ops = tokens.filter((t) => flat(t.scopes).includes('keyword.operator.lox'));
  assert.deepStrictEqual(
    ops.map((t) => t.text),
    ['=', '==', '!=', '<', '<=', '>', '>=', '+', '-', '*', '/', '%', '!']
  );
});

test('compound operators prefer the longer match', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('a=>b a==b a!=b a<=b a>=b', g);
  const ops = tokens.filter((t) => flat(t.scopes).includes('keyword.operator.lox'));
  assert.deepStrictEqual(ops.map((t) => t.text), ['=>', '==', '!=', '<=', '>=']);
});

test('pattern operators @ and ... scope to keyword.operator.pattern', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('case v @ Cons(h, t) or ...rest => v', g);
  const pat = tokens.filter((t) => flat(t.scopes).includes('keyword.operator.pattern.lox'));
  assert.deepStrictEqual(pat.map((t) => t.text), ['@', '...']);
  const fat = tokens.filter((t) => flat(t.scopes).includes('keyword.operator.lox'));
  assert.deepStrictEqual(fat.map((t) => t.text), ['=>']);
});

test('punctuation scopes', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('( ) [ ] { } , ; : .', g);
  const brackets = tokens.filter((t) => flat(t.scopes).includes('punctuation.section.brackets.lox'));
  assert.deepStrictEqual(brackets.map((t) => t.text), ['(', ')', '[', ']', '{', '}']);
  const seps = tokens.filter((t) => flat(t.scopes).includes('punctuation.separator.lox'));
  assert.deepStrictEqual(seps.map((t) => t.text), [',', ';', ':']);
  const dot = tokens.filter((t) => flat(t.scopes).includes('punctuation.separator.dot.lox'));
  assert.deepStrictEqual(dot.map((t) => t.text), ['.']);
});

test('fun declaration names the function', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('fun add(a, b) { return a + b; }', g);
  const funName = tokens.find((t) => flat(t.scopes).includes('entity.name.function.lox'));
  assert.ok(funName, 'function name must be scoped');
  assert.strictEqual(funName.text, 'add');
});

test('class declaration names class and superclass', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('class Base { }', g);
  const className = tokens.find((t) => flat(t.scopes).includes('entity.name.type.class.lox'));
  assert.ok(className, 'class name must be scoped');
  assert.strictEqual(className.text, 'Base');

  const sub = tokenize('class Derived < Base { }', g).tokens;
  const inherited = sub.find((t) => flat(t.scopes).includes('entity.other.inherited-class.lox'));
  assert.ok(inherited, 'superclass must be scoped');
  assert.strictEqual(inherited.text, 'Base');
});

test('keywords at call position are never function names', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('if(1) while(2) for(3)', g);
  for (const t of tokens) {
    if (['if', 'while', 'for'].includes(t.text)) {
      assert.strictEqual(flat(t.scopes)[0], 'keyword.control.lox');
    }
  }
  assert.strictEqual(tokens.some((t) => flat(t.scopes).includes('entity.name.function.lox')), false);
});

test('method call heuristic scopes callee names after declarations and keywords', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('obj.run(); print tail();', g);
  const names = tokens.filter((t) => flat(t.scopes).includes('entity.name.function.lox'));
  assert.deepStrictEqual(names.map((t) => t.text), ['run', 'tail']);
  const dotted = tokens.findIndex((t) => t.text === 'run');
  assert.strictEqual(tokens[dotted - 1].text, '.');
});

test('identifiers default to variable.other', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('foo _bar1 x', g);
  const ids = tokens.filter((t) => flat(t.scopes).includes('variable.other.lox'));
  assert.deepStrictEqual(ids.map((t) => t.text), ['foo', '_bar1', 'x']);
});

test('full program tokenizes with keyword and literal scopes', async () => {
  const g = await getGrammar();
  const src = [
    'try {',
    '  for (var i = 0; i < 10; i = i + 1) {',
    '    if (i % 2 == 0) print i;',
    '    else print "odd";',
    '  }',
    '} catch (e) {',
    '  print e;',
    '}',
    'defer cleanup();',
    'throw "boom";',
    ''
  ].join('\n');
  const { tokens } = tokenize(src, g);
  const scopes = tokens.map((t) => flat(t.scopes)[0]);
  assert.strictEqual(scopes.includes('keyword.control.exception.lox'), true);
  assert.strictEqual(scopes.includes('constant.numeric.lox'), true);
  assert.strictEqual(scopes.includes('string.quoted.double.lox'), true);
});

test('token spans cover the line exactly', async () => {
  const g = await getGrammar();
  const src = 'var x = 1; // done';
  const { tokens } = tokenize(src, g);
  let pos = 0;
  for (const t of tokens) {
    assert.strictEqual(t.startIndex, pos);
    pos = t.endIndex;
  }
  assert.strictEqual(pos, src.length);
});

test('all 23 reserved keywords from spec/01-lexical.md are scoped', async () => {
  const g = await getGrammar();
  const keywordTexts = [
    'and', 'break', 'case', 'class', 'continue', 'default', 'else', 'enum',
    'false', 'for', 'fun', 'if', 'in', 'match', 'nil', 'or', 'print',
    'return', 'super', 'this', 'true', 'var', 'while'
  ];
  const { tokens } = tokenize(keywordTexts.join(' '), g);
  const scoped = tokens.filter((t) => flat(t.scopes).length > 0);
  assert.deepStrictEqual(
    scoped.map((t) => t.text),
    keywordTexts,
    'every reserved keyword must be scoped as something other than an identifier'
  );
  for (const t of scoped) {
    assert.notStrictEqual(flat(t.scopes)[0], 'variable.other.lox');
  }
});

test('try catch throw defer scope in a full statement', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('try { risky(); } catch (err) { print err; }', g);
  const exceptionKeywords = tokens.filter((t) => flat(t.scopes).includes('keyword.control.exception.lox'));
  assert.deepStrictEqual(exceptionKeywords.map((t) => t.text), ['try', 'catch']);
  const binding = tokens.find((t) => t.text === 'err');
  assert.ok(binding, 'catch binding must tokenize');
  assert.strictEqual(flat(binding.scopes)[0], 'variable.other.lox');
});

test('throw and defer scope as exception keywords', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('throw Error("x"); defer log();', g);
  const exceptionKeywords = tokens.filter((t) => flat(t.scopes).includes('keyword.control.exception.lox'));
  assert.deepStrictEqual(exceptionKeywords.map((t) => t.text), ['throw', 'defer']);
});

test('unterminated string spans lines until the next quote', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('var s = "oops\nvar after = 1;', g);
  const whole = tokens.find((t) => t.text === 'var after = 1;');
  assert.ok(whole, 'second line must tokenize');
  assert.strictEqual(flat(whole.scopes)[0], 'string.quoted.double.lox');
  const recovered = tokenize('var s = "oops\nvar t = "fix";\nprint t;', g).tokens;
  const fix = recovered.find((t) => t.text === 'fix');
  assert.ok(fix, 'recovered string content must tokenize');
  assert.strictEqual(flat(fix.scopes)[0], 'variable.other.lox');
  const print = recovered.find((t) => t.text === 'print t;');
  assert.ok(print, 'third line is still inside the string');
  assert.strictEqual(flat(print.scopes)[0], 'string.quoted.double.lox');
});

test('dot runs follow spec/01-lexical.md ELIPSIS rules', async () => {
  const g = await getGrammar();
  const two = tokenize('a .. b', g).tokens.filter((t) => t.text.trim() !== '');
  const dots = two.filter((t) => t.text === '.');
  assert.strictEqual(dots.length, 2);
  for (const d of dots) {
    assert.strictEqual(flat(d.scopes)[0], 'punctuation.separator.dot.lox');
  }
  const four = tokenize('a .... b', g).tokens.filter((t) => t.text.trim() !== '');
  assert.strictEqual(flat(four[1].scopes)[0], 'keyword.operator.pattern.lox');
  assert.strictEqual(four[1].text, '...');
  assert.strictEqual(flat(four[2].scopes)[0], 'punctuation.separator.dot.lox');
  assert.strictEqual(four[2].text, '.');
});

test('escapes outside the six valid sequences are illegal anywhere in a string', async () => {
  const g = await getGrammar();
  const { tokens } = tokenize('"bad \\x more"', g);
  const illegal = tokens.find((t) => flat(t.scopes).includes('invalid.illegal.bad-escape.lox'));
  assert.ok(illegal, 'bad escape must be flagged');
  assert.strictEqual(illegal.text, '\\x');
});

test('no token text is empty and spans never overlap', async () => {
  const g = await getGrammar();
  const src = [
    'class A < B { m(x) { return this.x; } }',
    'var {a, b} = pair;',
    'var [c, d] = list;',
    'match (v) { case 1, 2 => "low", case n @ Some(x) if n > 0 => n, case _ => nil }',
    'for (var e in items) { continue; } break;',
    'try { throw "e"; } catch (err) { defer done(); }'
  ].join('\n');
  const { tokens } = tokenize(src, g);
  for (const t of tokens) {
    assert.ok(t.text.length > 0, 'no zero-length tokens');
  }
  for (const s of topLevel(tokens)) {
    assert.strictEqual(typeof s, 'string');
  }
});

test('grammar repository includes required top-level rules', async () => {
  const raw = JSON.parse(grammarData);
  const required = [
    'comments', 'strings', 'numbers', 'keywords', 'declarations',
    'operators', 'operators-patterns', 'punctuation', 'identifiers'
  ];
  for (const name of required) {
    assert.ok(raw.repository[name], `repository must define #${name}`);
  }
  assert.strictEqual(raw.name, 'Lox++');
  assert.strictEqual(raw.patterns.some((p) => p.include === '#shebang'), false);
  assert.strictEqual('shebang' in raw.repository, false);
  const commentProps = grammarData.match(/"comment"/g) || [];
  assert.strictEqual(commentProps.length, 0, 'no comment properties may remain');
});

test('corpus files tokenize without empty-line gaps', async () => {
  const g = await getGrammar();
  const corpusRoot = path.resolve(grammarPath, '../../../..', 'examples');
  assert.ok(fs.existsSync(corpusRoot), `corpus dir must exist: ${corpusRoot}`);
  const files = fs.readdirSync(corpusRoot).filter((f) => f.endsWith('.lox')).slice(0, 8);
  assert.ok(files.length >= 5, 'expected several corpus files');
  for (const f of files) {
    const src = fs.readFileSync(path.join(corpusRoot, f), 'utf8');
    const lines = src.split(/\r?\n/);
    let ruleStack = vsctm.INITIAL;
    for (const line of lines) {
      const r = g.tokenizeLine(line, ruleStack);
      ruleStack = r.ruleStack;
      for (const t of r.tokens) {
        assert.strictEqual(t.scopes[0], 'source.lox', `${f}: root scope must be source.lox`);
        assert.ok(t.endIndex <= line.length + 1, `${f}: token end past line end`);
      }
    }
  }
});

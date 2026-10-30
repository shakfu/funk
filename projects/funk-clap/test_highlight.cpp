// The editor's syntax colours (highlight.cpp): the class of a word in each line of funk.
// Prints each check; exits 1 if any fails.

#include "editor_logic.hpp"
#include "highlight.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

const char* const kNames[] = {"plain", "comment", "keyword", "string", "number", "type", "constructor",
                              "function", "operator", "module", "punctuation", "field"};

struct Check {
	const char* code;
	const char* word; // its first occurrence in code
	Token want;
};

const Check kChecks[] = {
	{"-- a note", "note", kComment},
	{"{- a block -}", "block", kComment},
	{"import graph as g", "import", kKeyword},
	{"import graph as g", "graph", kModule},
	{"foreign library \"m\"", "foreign", kKeyword},
	{"foreign library \"m\"", "library", kKeyword},
	{"foreign struct T { x :: CInt }", "struct", kKeyword},
	{"foreign struct T { x :: CInt }", "T", kType},
	{"foreign struct T { x :: CInt }", "x", kField},
	{"foreign struct T { x :: CInt }", "CInt", kType},
	{"f :: Int ->> ()", "f", kFunction},
	{"f :: Int ->> ()", "->>", kOperator},
	{"f :: Int ->> ()", "Int", kType},
	{"s = \"hi\"", "\"hi\"", kString},
	{"n = 42", "42", kNumber},
	{"a = g.play p", "play", kField},
	{"a = g.Node \"k\" [] []", "g", kModule},
	{"a = g.Node \"k\" [] []", "Node", kConstructor},
	{"Shape.area (Circle _ r) = r", "area", kFunction},
	{"Shape.area (Circle _ r) = r", "Circle", kConstructor},
	{"g = case x of\n  Just y -> y", "case", kKeyword},
};

} // namespace

int main()
{
	int failed = 0;
	std::vector<uint8_t> tokens;
	for (const Check& c : kChecks) {
		const std::string code = c.code;
		FunkHighlight(code, tokens);
		const size_t at = code.find(c.word);
		const int got = at < tokens.size() ? tokens[at] : -1;
		const bool ok = got == c.want;
		failed += !ok;
		printf("%s: %s in `%s`: %s%s%s\n", ok ? "PASS" : "FAIL", c.word, c.code, kNames[c.want], ok ? "" : ", got ",
		       ok ? "" : got >= 0 ? kNames[got] : "nothing");
	}
	return failed ? 1 : 0;
}

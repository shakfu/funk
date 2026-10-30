// Syntax colours for the plugin's editor: projects/tree-sitter-funk's parser and its
// queries/highlights.scm, embedded as kHighlightsQuery (funk_highlights.h, made by CMake).
// As in Neovim, a later pattern's capture wins over an earlier one's on the same bytes.

#include "highlight.hpp"

#include "editor_logic.hpp"
#include "funk_highlights.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string_view>

extern "C" const TSLanguage* tree_sitter_funk(void);

namespace {

// A capture name's class: its first component, or the whole name where that decides;
// kTokenCount for a name that is no colour, such as Neovim's @spell.
Token ClassOf(std::string_view name)
{
	auto is = [&](const char* prefix) {
		const size_t n = strlen(prefix);
		return name.compare(0, n, prefix) == 0 && (name.size() == n || name[n] == '.');
	};
	if (name == "variable.member") return kField;
	if (is("comment")) return kComment;
	if (is("keyword")) return kKeyword;
	if (is("string") || is("character")) return kString;
	if (is("number") || is("boolean") || is("constant")) return kNumber;
	if (is("type")) return kType;
	if (is("constructor")) return kConstructor;
	if (is("function")) return kFunction;
	if (is("operator")) return kOperator;
	if (is("module")) return kModule;
	if (is("punctuation")) return kPunctuation;
	if (is("variable")) return kPlain;
	return kTokenCount;
}

// A pattern's #eq? or #any-of?: capture a against capture b, or against strings.
struct Predicate {
	bool anyOf;
	uint32_t a;
	int64_t b = -1; // a capture, or -1
	std::vector<std::string> strings;
};

struct Highlighter {
	TSParser* parser = ts_parser_new();
	TSQuery* query = nullptr;
	std::vector<uint8_t> classes;                 // per capture; kTokenCount: not painted
	std::vector<std::vector<Predicate>> predicates; // per pattern
	std::mutex mutex;

	Highlighter()
	{
		ts_parser_set_language(parser, tree_sitter_funk());
		uint32_t offset;
		TSQueryError error;
		query = ts_query_new(tree_sitter_funk(), kHighlightsQuery, (uint32_t)strlen(kHighlightsQuery), &offset, &error);
		if (!query) return;
		for (uint32_t i = 0, n = ts_query_capture_count(query); i < n; i++) {
			uint32_t len;
			const char* name = ts_query_capture_name_for_id(query, i, &len);
			classes.push_back(ClassOf({name, len})); // kTokenCount for @_name, a predicate's
		}
		for (uint32_t p = 0, n = ts_query_pattern_count(query); p < n; p++) {
			uint32_t steps;
			const TSQueryPredicateStep* s = ts_query_predicates_for_pattern(query, p, &steps);
			std::vector<Predicate> preds;
			for (uint32_t i = 0; i < steps;) {
				uint32_t j = i;
				while (j < steps && s[j].type != TSQueryPredicateStepTypeDone) j++;
				uint32_t len;
				const char* op = ts_query_string_value_for_id(query, s[i].value_id, &len);
				const std::string_view name(op, len);
				if ((name == "eq?" || name == "any-of?") && j - i >= 3 && s[i + 1].type == TSQueryPredicateStepTypeCapture) {
					Predicate pr{name == "any-of?", s[i + 1].value_id};
					for (uint32_t k = i + 2; k < j; k++) {
						if (s[k].type == TSQueryPredicateStepTypeCapture) pr.b = s[k].value_id;
						else pr.strings.emplace_back(ts_query_string_value_for_id(query, s[k].value_id, &len), len);
					}
					preds.push_back(std::move(pr));
				}
				i = j + 1; // other predicates hold
			}
			predicates.push_back(std::move(preds));
		}
	}

	static std::string_view Text(const std::string& text, TSNode node)
	{
		const uint32_t a = ts_node_start_byte(node), b = ts_node_end_byte(node);
		return std::string_view(text).substr(a, b - a);
	}

	bool holds(const TSQueryMatch& m, const std::string& text) const
	{
		auto first = [&](uint32_t capture, std::string_view* out) {
			for (uint16_t i = 0; i < m.capture_count; i++)
				if (m.captures[i].index == capture) return *out = Text(text, m.captures[i].node), true;
			return false;
		};
		for (const Predicate& p : predicates[m.pattern_index]) {
			std::string_view a, b;
			if (!first(p.a, &a)) return false;
			if (p.b >= 0) {
				if (!first((uint32_t)p.b, &b) || a != b) return false;
				continue;
			}
			bool any = false;
			for (const std::string& s : p.strings) any = any || a == s;
			if (!any) return false;
		}
		return true;
	}

	void run(const std::string& text, std::vector<uint8_t>& tokens)
	{
		tokens.assign(text.size(), kPlain);
		if (!query || text.empty()) return;
		std::lock_guard<std::mutex> lock(mutex);
		TSTree* tree = ts_parser_parse_string(parser, nullptr, text.data(), (uint32_t)text.size());
		if (!tree) return;
		TSQueryCursor* cursor = ts_query_cursor_new();
		ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));
		std::vector<int32_t> by(text.size(), -1); // the pattern that painted each byte
		TSQueryMatch m;
		while (ts_query_cursor_next_match(cursor, &m)) {
			if (!holds(m, text)) continue;
			for (uint16_t i = 0; i < m.capture_count; i++) {
				const uint8_t c = classes[m.captures[i].index];
				if (c == kTokenCount) continue;
				const uint32_t end = std::min<uint32_t>(ts_node_end_byte(m.captures[i].node), (uint32_t)text.size());
				for (uint32_t b = ts_node_start_byte(m.captures[i].node); b < end; b++)
					if ((int32_t)m.pattern_index >= by[b]) by[b] = m.pattern_index, tokens[b] = c;
			}
		}
		ts_query_cursor_delete(cursor);
		ts_tree_delete(tree);
	}
};

} // namespace

void FunkHighlight(const std::string& text, std::vector<uint8_t>& tokens)
{
	static Highlighter highlighter;
	highlighter.run(text, tokens);
}

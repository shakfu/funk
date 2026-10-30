#pragma once

#include <cstdint>
#include <string>
#include <vector>

// LanguageInfo::highlight for funk: a Token (editor_logic.hpp) per byte of text, from
// projects/tree-sitter-funk's parser and its queries/highlights.scm.
void FunkHighlight(const std::string& text, std::vector<uint8_t>& tokens);

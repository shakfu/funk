# tree-sitter-funk (README.md). Needs the tree-sitter CLI and node.
#   make            the parser, src/parser.c, from grammar.js and grammar/
#   make check      every .fk file in the repository parses without errors
#   make helix      helix/queries: queries/ with Helix's capture names

all: src/parser.c

src/parser.c: grammar.js grammar/*.js
	tree-sitter generate

check: src/parser.c
	sh check.sh

# Helix names a few captures differently; the rest are shared.
helix: queries/*.scm
	mkdir -p helix/queries
	perl -pe 's/\@boolean\b/\@constant.builtin.boolean/g; s/\@module\b/\@namespace/g;' \
	    -e 's/\@number\.float\b/\@constant.numeric.float/g; s/\@number\b/\@constant.numeric.integer/g;' \
	    -e 's/\@character\b/\@constant.character/g; s/ *\@spell\b//g' \
	    queries/highlights.scm > helix/queries/highlights.scm
	cp queries/locals.scm queries/injections.scm helix/queries/

.PHONY: all check helix

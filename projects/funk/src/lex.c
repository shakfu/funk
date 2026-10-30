/* lex.c -- tokenizer and layout rule.

   Layout follows Haskell 2010 section 10.3: `let`, `where`, `of` and `do` open an
   implicit block at the column of the next token; a line starting at that
   column inserts `;`, a line left of it closes the block. The standard's
   parse-error(t) rule is approximated by closing implicit blocks on:
     - `in`        the innermost open `let` block
     - `)` `]`     blocks opened inside the bracket being closed
     - `,`         blocks opened inside the current bracket
     - then/else   blocks opened after the matching `if`
     - `where`     a block at whose column it starts a line */
#include <ctype.h>
#include <errno.h>

#include "funk.h"

typedef struct {
  const char *src;
  size_t pos, len;
  int line, col;
  bool bol;
  VEC(Token) out;
} Lexer;

const char *tok_name(TokKind k) {
  static const char *const names[] = {
      "end of input", "identifier", "constructor", "operator", "operator", "integer", "float",
      "character", "string", "'('", "')'", "'['", "']'", "','", "';' or new line", "'`'",
      "'{'", "'}'", "start of block", "end of block (check indentation)", "'='", "'|'", "'->'",
      "'\\'", "'::'", "'..'", "'=>'", "'_'", "'case'", "'of'", "'let'", "'in'", "'where'",
      "'if'", "'then'", "'else'", "'data'", "'deriving'", "'infixl'", "'infixr'", "'infix'", "'class'", "'instance'", "'do'", "'<-'", "field access"};
  return names[k];
}

static int peekc(Lexer *L, size_t off) {
  return L->pos + off < L->len ? (unsigned char)L->src[L->pos + off] : -1;
}

static void adv(Lexer *L) {
  if (L->src[L->pos] == '\n') {
    L->line++;
    L->col = 1;
    L->bol = true;
  } else if (L->src[L->pos] == '\t') {
    L->col = ((L->col - 1) / 8 + 1) * 8 + 1;
  } else {
    L->col++;
  }
  L->pos++;
}

static bool is_sym(int c) { return c > 0 && strchr("!#$%&*+./<=>?@\\^|-~:", c); }
static bool is_idc(int c) { return c > 0 && (isalnum(c) || c == '_' || c == '\''); }

static int lex_escape(Lexer *L) {
  int line = L->line, col = L->col;
  adv(L); /* backslash */
  int c = peekc(L, 0);
  if (isdigit(c)) {
    int v = 0;
    while (isdigit(peekc(L, 0))) { v = v * 10 + peekc(L, 0) - '0'; adv(L); }
    if (v > 255) cerror(line, col, "character escape out of range");
    return v;
  }
  adv(L);
  switch (c) {
  case 'n': return '\n';
  case 't': return '\t';
  case 'r': return '\r';
  case '0': return 0;
  case 'a': return 7;
  case 'b': return 8;
  case '\\': case '\'': case '"': return c;
  }
  cerror(line, col, "unknown escape sequence");
}

static void skip_space(Lexer *L) {
  for (;;) {
    int c = peekc(L, 0);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { adv(L); continue; }
    if (c == '-' && peekc(L, 1) == '-') {
      size_t k = 2;
      while (peekc(L, k) == '-') k++;
      if (!is_sym(peekc(L, k))) {
        while (L->pos < L->len && L->src[L->pos] != '\n') adv(L);
        continue;
      }
    }
    if (c == '{' && peekc(L, 1) == '-') {
      int line = L->line, col = L->col, depth = 0;
      do {
        if (L->pos >= L->len) cerror(line, col, "unterminated block comment");
        if (peekc(L, 0) == '{' && peekc(L, 1) == '-') { depth++; adv(L); }
        else if (peekc(L, 0) == '-' && peekc(L, 1) == '}') { depth--; adv(L); }
        adv(L);
      } while (depth > 0);
      continue;
    }
    return;
  }
}

static void raw_lex(Lexer *L) {
  static struct { const char *s; TokKind k; } kws[] = {
      {"case", T_CASE}, {"of", T_OF},       {"let", T_LET},   {"in", T_IN},
      {"where", T_WHERE}, {"if", T_IF},     {"then", T_THEN}, {"else", T_ELSE},
      {"data", T_DATA}, {"deriving", T_DERIVING}, {"infixl", T_INFIXL}, {"infixr", T_INFIXR},
      {"infix", T_INFIX}, {"class", T_CLASS}, {"instance", T_INSTANCE},
      {"do", T_DO}};
  static struct { const char *s; TokKind k; } rops[] = {
      {"=", T_EQ},     {"|", T_BAR},     {"->", T_ARROW},  {"\\", T_LAMBDA},
      {"::", T_DCOLON}, {"..", T_DOTDOT}, {"=>", T_DARROW}, {"<-", T_LARROW}};
  for (;;) {
    skip_space(L);
    Token t = {0};
    t.line = L->line;
    t.col = L->col;
    t.bol = L->bol;
    L->bol = false;
    if (L->pos >= L->len) {
      t.k = T_EOF;
      VPUSH(L->out, t);
      return;
    }
    int c = peekc(L, 0);
    size_t start = L->pos;
    if (isalpha(c) || c == '_') {
      while (is_idc(peekc(L, 0))) adv(L);
      t.s = intern_n(L->src + start, L->pos - start);
      t.k = isupper(c) ? T_CONID : T_VARID;
      if (strcmp(t.s, "_") == 0) t.k = T_UNDERSCORE;
      for (size_t i = 0; i < sizeof kws / sizeof kws[0]; i++)
        if (strcmp(t.s, kws[i].s) == 0) t.k = kws[i].k;
    } else if (isdigit(c)) {
      bool flt = false;
      while (isdigit(peekc(L, 0))) adv(L);
      if (peekc(L, 0) == '.' && isdigit(peekc(L, 1))) {
        flt = true;
        adv(L);
        while (isdigit(peekc(L, 0))) adv(L);
      }
      if ((peekc(L, 0) == 'e' || peekc(L, 0) == 'E') &&
          (isdigit(peekc(L, 1)) || ((peekc(L, 1) == '-' || peekc(L, 1) == '+') && isdigit(peekc(L, 2))))) {
        flt = true;
        adv(L);
        adv(L);
        while (isdigit(peekc(L, 0))) adv(L);
      }
      char *num = xmalloc(L->pos - start + 1);
      memcpy(num, L->src + start, L->pos - start);
      num[L->pos - start] = 0;
      if (flt) {
        t.k = T_FLOAT;
        t.fval = strtod(num, NULL);
      } else {
        t.k = T_INT;
        errno = 0;
        t.ival = strtoll(num, NULL, 10);
        if (errno) cerror(t.line, t.col, "integer literal too large");
      }
      free(num);
    } else if (c == '\'') {
      adv(L);
      int ch = peekc(L, 0);
      if (ch == '\\') ch = lex_escape(L);
      else if (ch < 0 || ch == '\n') cerror(t.line, t.col, "bad character literal");
      else adv(L);
      if (peekc(L, 0) != '\'') cerror(t.line, t.col, "unterminated character literal");
      adv(L);
      t.k = T_CHAR;
      t.ival = ch;
    } else if (c == '"') {
      SB b = {0};
      sb_puts(&b, "");
      adv(L);
      size_t n = 0;
      for (;;) {
        int ch = peekc(L, 0);
        if (ch < 0 || ch == '\n') cerror(t.line, t.col, "unterminated string literal");
        if (ch == '"') { adv(L); break; }
        if (ch == '\\') ch = lex_escape(L);
        else adv(L);
        char cc[2] = {(char)ch, 0};
        if (ch == 0) { /* keep embedded NULs: SB is NUL-terminated */
          if (b.n + 2 > b.cap) { b.cap = (b.n + 2) * 2; b.s = xrealloc(b.s, b.cap); }
          b.s[b.n++] = 0;
          b.s[b.n] = 0;
        } else {
          sb_puts(&b, cc);
        }
        n++;
      }
      t.k = T_STRING;
      t.s = b.s;
      t.len = (int)n;
    } else if (c == '.' && (islower(peekc(L, 1)) || peekc(L, 1) == '_') && start > 0 &&
               (is_idc(L->src[start - 1]) || strchr(")](", L->src[start - 1]))) {
      adv(L); /* p.x, (f p).x, Shape.area, (.x): no space before or after '.' */
      size_t s0 = L->pos;
      while (is_idc(peekc(L, 0))) adv(L);
      t.k = T_FIELD;
      t.s = intern_n(L->src + s0, L->pos - s0);
    } else if (is_sym(c)) {
      while (is_sym(peekc(L, 0))) adv(L);
      t.s = intern_n(L->src + start, L->pos - start);
      t.k = c == ':' ? T_CONSYM : T_VARSYM;
      for (size_t i = 0; i < sizeof rops / sizeof rops[0]; i++)
        if (strcmp(t.s, rops[i].s) == 0) t.k = rops[i].k;
    } else {
      adv(L);
      switch (c) {
      case '(': t.k = T_LPAREN; break;
      case ')': t.k = T_RPAREN; break;
      case '[': t.k = T_LBRACKET; break;
      case ']': t.k = T_RBRACKET; break;
      case ',': t.k = T_COMMA; break;
      case ';': t.k = T_SEMI; break;
      case '`': t.k = T_BACKTICK; break;
      case '{': t.k = T_LBRACE; break;
      case '}': t.k = T_RBRACE; break;
      default: cerror(t.line, t.col, "unexpected character '%c'", c);
      }
    }
    VPUSH(L->out, t);
  }
}

/* ---------------------------------------------------------------- layout */

enum { CX_TOP, CX_IMPLICIT, CX_EXPLICIT };
typedef struct {
  int kind, col, depth;
  bool is_let;
  int ifs;         /* `if`s seen in this block awaiting `else` */
  int closed_lets; /* let blocks closed here before their `in` arrived */
} Ctx;

typedef struct {
  VEC(Ctx) st;
  VEC(Token) out;
  int depth;
  VEC(bool) braces; /* per open '{': true if a record brace, which nests like '(' */
} Layout;

static Ctx *top(Layout *Y) { return &Y->st.a[Y->st.n - 1]; }

static void emit(Layout *Y, TokKind k, Token *at) {
  Token t = *at;
  t.k = k;
  t.bol = false;
  VPUSH(Y->out, t);
}

static void pop(Layout *Y, Token *at) {
  Ctx c = Y->st.a[--Y->st.n];
  emit(Y, c.kind == CX_EXPLICIT ? T_RBRACE : T_VRBRACE, at);
  if (c.is_let) top(Y)->closed_lets++;
}

static bool implicit(Layout *Y) { return top(Y)->kind == CX_IMPLICIT; }

Token *lex(const char *src, size_t len, int *ntok) {
  Lexer L = {src, 0, len, 1, 1, true, {0}};
  raw_lex(&L);
  Layout Y = {0};
  Token *in = L.out.a;
  VPUSH(Y.st, ((Ctx){CX_TOP, in[0].k == T_EOF ? 1 : in[0].col, 0, false, 0, 0}));
  bool open = false, open_let = false;
  for (int i = 0; i < L.out.n; i++) {
    Token *t = &in[i];
    bool fresh_block = false;
    if (open) {
      open = false;
      if (t->k != T_LBRACE) {
        int enclosing = top(&Y)->kind == CX_EXPLICIT ? 0 : top(&Y)->col;
        if (t->k != T_EOF && t->col > enclosing) {
          VPUSH(Y.st, ((Ctx){CX_IMPLICIT, t->col, Y.depth, open_let, 0, 0}));
          emit(&Y, T_VLBRACE, t);
          fresh_block = true;
        } else {
          emit(&Y, T_VLBRACE, t);
          emit(&Y, T_VRBRACE, t);
          if (open_let) top(&Y)->closed_lets++;
        }
      } else {
        VPUSH(Y.st, ((Ctx){CX_EXPLICIT, 0, Y.depth, open_let, 0, 0}));
        VPUSH(Y.braces, false);
        VPUSH(Y.out, *t);
        continue;
      }
    }
    if (t->bol && t->k != T_EOF && !fresh_block) {
      /* No block item starts with `where`, so one at a block's column ends it. */
      while (implicit(&Y) && (t->col < top(&Y)->col || (t->k == T_WHERE && t->col == top(&Y)->col))) pop(&Y, t);
      if (top(&Y)->kind != CX_EXPLICIT && t->col == top(&Y)->col && Y.out.n > 0) {
        emit(&Y, T_SEMI, t);
        top(&Y)->closed_lets = 0; /* a new statement: a `let` statement in `do` has no `in` */
      }
    }
    switch (t->k) {
    case T_IN:
      while (top(&Y)->closed_lets == 0 && implicit(&Y)) pop(&Y, t);
      if (top(&Y)->closed_lets > 0) top(&Y)->closed_lets--;
      break;
    case T_RPAREN:
    case T_RBRACKET:
      while (implicit(&Y) && top(&Y)->depth == Y.depth) pop(&Y, t);
      Y.depth--;
      break;
    case T_COMMA:
      while (implicit(&Y) && top(&Y)->depth == Y.depth && Y.depth > 0) pop(&Y, t);
      break;
    case T_THEN:
    case T_ELSE:
      while (implicit(&Y) && top(&Y)->ifs == 0) pop(&Y, t);
      if (t->k == T_ELSE && top(&Y)->ifs > 0) top(&Y)->ifs--;
      break;
    case T_IF: top(&Y)->ifs++; break;
    case T_LPAREN:
    case T_LBRACKET: Y.depth++; break;
    case T_LBRACE: /* not after let/where/of: a record */
      VPUSH(Y.braces, true);
      Y.depth++;
      break;
    case T_RBRACE:
      if (Y.braces.n && Y.braces.a[--Y.braces.n]) {
        while (implicit(&Y) && top(&Y)->depth == Y.depth) pop(&Y, t);
        Y.depth--;
        break;
      }
      while (implicit(&Y)) pop(&Y, t);
      if (top(&Y)->kind != CX_EXPLICIT) cerror(t->line, t->col, "unmatched '}'");
      Y.st.n--;
      if (Y.st.a[Y.st.n].is_let) top(&Y)->closed_lets++;
      break;
    case T_EOF:
      while (implicit(&Y)) pop(&Y, t);
      break;
    default: break;
    }
    VPUSH(Y.out, *t);
    if (t->k == T_LET || t->k == T_WHERE || t->k == T_OF || t->k == T_DO) {
      open = true;
      open_let = t->k == T_LET;
    }
  }
  free(L.out.a);
  free(Y.st.a);
  free(Y.braces.a);
  *ntok = Y.out.n;
  return Y.out.a;
}

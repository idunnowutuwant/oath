#include "oath/ingest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char* src;
    size_t cursor;
    size_t len;
} IngestScanner;

static char scanner_peek(const IngestScanner* s) {
    if (s->cursor >= s->len) return '\0';
    return s->src[s->cursor];
}

static char scanner_advance(IngestScanner* s) {
    if (s->cursor >= s->len) return '\0';
    return s->src[s->cursor++];
}

static void scanner_skip_ws(IngestScanner* s) {
    while (s->cursor < s->len) {
        char c = scanner_peek(s);
        if (isspace((unsigned char)c)) {
            scanner_advance(s);
        } else if (c == '/' && s->cursor + 1 < s->len && s->src[s->cursor + 1] == '/') {
            while (s->cursor < s->len && scanner_peek(s) != '\n') scanner_advance(s);
        } else {
            break;
        }
    }
}

static bool scanner_match_token(IngestScanner* s, const char* word) {
    scanner_skip_ws(s);
    size_t wlen = strlen(word);
    if (s->cursor + wlen <= s->len && strncmp(&s->src[s->cursor], word, wlen) == 0) {
        s->cursor += wlen;
        return true;
    }
    return false;
}

static bool scanner_read_ident(IngestScanner* s, char* out, size_t cap) {
    scanner_skip_ws(s);
    size_t i = 0;
    char c = scanner_peek(s);
    if (!isalpha((unsigned char)c) && c != '_') return false;

    while (s->cursor < s->len) {
        c = scanner_peek(s);
        if (isalnum((unsigned char)c) || c == '_') {
            if (i + 1 < cap) out[i++] = scanner_advance(s);
            else scanner_advance(s);
        } else {
            break;
        }
    }
    out[i] = '\0';
    return i > 0;
}

static OathInterval scanner_parse_bounds_comment(IngestScanner* s) {
    scanner_skip_ws(s);
    int64_t lo = 0;
    int64_t hi = 0;

    if (scanner_match_token(s, "/*")) {
        while (s->cursor < s->len && scanner_peek(s) != '[' && scanner_peek(s) != '*') {
            scanner_advance(s);
        }
        if (scanner_match_token(s, "[")) {
            char num_buf[32];
            size_t i = 0;
            scanner_skip_ws(s);
            if (scanner_peek(s) == '-') num_buf[i++] = scanner_advance(s);
            while (isdigit((unsigned char)scanner_peek(s))) num_buf[i++] = scanner_advance(s);
            num_buf[i] = '\0';
            lo = (int64_t)strtoll(num_buf, NULL, 10);

            scanner_match_token(s, ",");
            i = 0;
            scanner_skip_ws(s);
            if (scanner_peek(s) == '-') num_buf[i++] = scanner_advance(s);
            while (isdigit((unsigned char)scanner_peek(s))) num_buf[i++] = scanner_advance(s);
            num_buf[i] = '\0';
            hi = (int64_t)strtoll(num_buf, NULL, 10);

            while (s->cursor < s->len && scanner_peek(s) != ']') scanner_advance(s);
            scanner_match_token(s, "]");
        }
        while (s->cursor < s->len && !(s->src[s->cursor] == '*' && s->cursor + 1 < s->len && s->src[s->cursor + 1] == '/')) {
            scanner_advance(s);
        }
        scanner_match_token(s, "*/");
    }
    return oath_interval_create(lo, hi);
}

static bool parse_c_function_decl(IngestScanner* s, OirModule* mod) {
    bool is_const_ret = scanner_match_token(s, "const");
    (void)is_const_ret;

    char type_name[64];
    if (!scanner_read_ident(s, type_name, sizeof(type_name))) return false;

    char func_name[64];
    if (!scanner_read_ident(s, func_name, sizeof(func_name))) return false;

    if (!scanner_match_token(s, "(")) return false;

    OirFunction* fn = oir_module_add_function(mod, func_name);
    fn->is_extern = true;
    fn->postcondition = oath_interval_create(0, INT64_MAX);

    while (s->cursor < s->len && scanner_peek(s) != ')') {
        bool is_const_param = scanner_match_token(s, "const");
        char param_type[64];
        if (!scanner_read_ident(s, param_type, sizeof(param_type))) break;

        bool is_ptr = false;
        scanner_skip_ws(s);
        if (scanner_peek(s) == '*') {
            is_ptr = true;
            scanner_advance(s);
        }

        bool is_restrict = scanner_match_token(s, "restrict");

        char param_name[64];
        if (!scanner_read_ident(s, param_name, sizeof(param_name))) break;

        OathInterval bounds = scanner_parse_bounds_comment(s);

        size_t p_idx = fn->param_count++;
        snprintf(fn->params[p_idx].name, sizeof(fn->params[p_idx].name), "%s", param_name);
        if (is_ptr) {
            if (is_const_param) {
                fn->params[p_idx].kind = PARAM_BORROW_IMMUT;
            } else if (is_restrict) {
                fn->params[p_idx].kind = PARAM_BORROW_MUT;
            } else {
                fn->params[p_idx].kind = PARAM_SLICE;
            }
            fn->params[p_idx].slice.elem_bounds = bounds;
        } else {
            fn->params[p_idx].kind = PARAM_SCALAR;
            fn->params[p_idx].scalar_bounds = bounds;
        }

        scanner_match_token(s, ",");
    }

    scanner_match_token(s, ")");

    OathInterval ret_bounds = scanner_parse_bounds_comment(s);
    if (ret_bounds.lo != 0 || ret_bounds.hi != 0) {
        fn->postcondition = ret_bounds;
    }

    scanner_match_token(s, ";");
    return true;
}

static bool parse_rust_function_decl(IngestScanner* s, OirModule* mod) {
    scanner_match_token(s, "pub");
    scanner_match_token(s, "extern");
    if (scanner_match_token(s, "\"C\"")) {}

    if (!scanner_match_token(s, "fn")) return false;

    char func_name[64];
    if (!scanner_read_ident(s, func_name, sizeof(func_name))) return false;

    if (!scanner_match_token(s, "(")) return false;

    OirFunction* fn = oir_module_add_function(mod, func_name);
    fn->is_extern = true;
    fn->postcondition = oath_interval_create(0, INT64_MAX);

    while (s->cursor < s->len && scanner_peek(s) != ')') {
        char param_name[64];
        if (!scanner_read_ident(s, param_name, sizeof(param_name))) break;

        if (!scanner_match_token(s, ":")) break;

        bool is_mut_borrow = false;
        bool is_immut_borrow = false;
        bool is_raw_ptr = false;

        scanner_skip_ws(s);
        if (scanner_match_token(s, "&mut")) {
            is_mut_borrow = true;
        } else if (scanner_match_token(s, "&")) {
            is_immut_borrow = true;
        } else if (scanner_match_token(s, "*mut") || scanner_match_token(s, "*const")) {
            is_raw_ptr = true;
        }

        char param_type[64];
        if (!scanner_read_ident(s, param_type, sizeof(param_type))) break;

        OathInterval bounds = scanner_parse_bounds_comment(s);

        size_t p_idx = fn->param_count++;
        snprintf(fn->params[p_idx].name, sizeof(fn->params[p_idx].name), "%s", param_name);
        if (is_mut_borrow) {
            fn->params[p_idx].kind = PARAM_BORROW_MUT;
            fn->params[p_idx].slice.elem_bounds = bounds;
        } else if (is_immut_borrow) {
            fn->params[p_idx].kind = PARAM_BORROW_IMMUT;
            fn->params[p_idx].slice.elem_bounds = bounds;
        } else if (is_raw_ptr) {
            fn->params[p_idx].kind = PARAM_SLICE;
            fn->params[p_idx].slice.elem_bounds = bounds;
        } else {
            fn->params[p_idx].kind = PARAM_SCALAR;
            fn->params[p_idx].scalar_bounds = bounds;
        }

        scanner_match_token(s, ",");
    }

    scanner_match_token(s, ")");

    if (scanner_match_token(s, "->")) {
        char ret_type[64];
        scanner_read_ident(s, ret_type, sizeof(ret_type));
    }

    OathInterval ret_bounds = scanner_parse_bounds_comment(s);
    if (ret_bounds.lo != 0 || ret_bounds.hi != 0) {
        fn->postcondition = ret_bounds;
    }

    scanner_match_token(s, ";");
    return true;
}

bool oath_detect_and_ingest(const char* filepath, const char* src, OirModule* out_mod, OathArena* arena) {
    (void)arena;
    *out_mod = oir_module_create(32);

    IngestLang lang = LANG_OATH;
    size_t flen = strlen(filepath);
    if (flen >= 2 && strcmp(&filepath[flen - 2], ".h") == 0) lang = LANG_C;
    else if (flen >= 2 && strcmp(&filepath[flen - 2], ".c") == 0) lang = LANG_C;
    else if (flen >= 3 && strcmp(&filepath[flen - 3], ".rs") == 0) lang = LANG_RUST;

    if (lang == LANG_OATH) return false;

    IngestScanner s = { .src = src, .cursor = 0, .len = strlen(src) };

    while (s.cursor < s.len) {
        scanner_skip_ws(&s);
        if (s.cursor >= s.len) break;

        if (lang == LANG_C) {
            if (!parse_c_function_decl(&s, out_mod)) {
                scanner_advance(&s);
            }
        } else if (lang == LANG_RUST) {
            if (!parse_rust_function_decl(&s, out_mod)) {
                scanner_advance(&s);
            }
        }
    }

    return out_mod->function_count > 0;
}
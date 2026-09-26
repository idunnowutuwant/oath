#include "oath/diagnostic.h"
#include <stdarg.h>
#include <string.h>

void oath_diag_init(OathDiagContext* ctx, const char* filepath, const char* src) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->filepath = filepath;
    ctx->src_text = src;
}

void oath_diag_report(OathDiagContext* ctx, OathDiagId id, size_t line, size_t col, const char* fmt, ...) {
    if (ctx->count >= 32) return;
    OathDiagnostic* d = &ctx->items[ctx->count++];
    d->id = id;
    d->line = line;
    d->col = col;

    va_list args;
    va_start(args, fmt);
    vsnprintf(d->message, sizeof(d->message), fmt, args);
    va_end(args);

    ctx->error_count++;
}

static void extract_source_line(const char* src, size_t target_line, char* out, size_t cap) {
    size_t cur = 1;
    const char* p = src;
    while (*p && cur < target_line) {
        if (*p == '\n') cur++;
        p++;
    }
    size_t i = 0;
    while (*p && *p != '\n' && *p != '\r' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

void oath_diag_render(FILE* stream, const OathDiagContext* ctx) {
    for (size_t i = 0; i < ctx->count; ++i) {
        const OathDiagnostic* d = &ctx->items[i];
        char line_buf[256];
        extract_source_line(ctx->src_text, d->line, line_buf, sizeof(line_buf));

        fprintf(stream, "error[E%d]: %s\n", d->id, d->message);
        fprintf(stream, "  --> %s:%zu:%zu\n", ctx->filepath, d->line, d->col);
        fprintf(stream, "   |\n");
        fprintf(stream, "%4zu | %s\n", d->line, line_buf);
        fprintf(stream, "   | ");
        for (size_t c = 1; c < d->col; ++c) {
            fputc(' ', stream);
        }
        fprintf(stream, "^\n");
        fprintf(stream, "   |\n\n");
    }
}
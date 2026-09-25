#ifndef OATH_BACKEND_H
#define OATH_BACKEND_H

#include <stdio.h>
#include "oath/sepe.h"

void oath_emit_c99_module(FILE* stream, const OathModule* mod);
void oath_emit_c_header(FILE* stream, const OathModule* mod, const char* prefix);
void oath_emit_b2b_certificate(FILE* stream, const OathFunction* fn, const SepeReport* report);

#endif
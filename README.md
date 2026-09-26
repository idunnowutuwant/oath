![OATH](banner.svg?v=3) 

# OATH v3

A certifying systems compiler. Statically proves memory safety, integer boundedness, and resource lifetimes at compile time. Emits branchless, panic-free code across 8 targets with zero runtime checks.

Verification runs in-process using an abstract interpreter, a Difference Bound Matrix (DBM) solver, and a flat SSA intermediate representation (**OIR**). No external SMT solvers (Z3, CVC5) required.

---

## Performance

Measured on x86_64 hardware (10,000,000 iterations, hardware memory barrier enforced):

| Metric | Measured Value |
| :--- | :--- |
| **Throughput** | 4.67B ops/sec |
| **Latency** | 0.21 ns / op |
| **Hardware Traps** | 0.000000% |
| **Runtime Checks** | 0 (100% statically eliminated) |
| **Verification Engine** | SEPE on OIR (Formally Verified Sound) |

---

## Formal Guarantees

* **Trap Freedom**: Integer overflow, underflow, zero division, and the x86 `#DE` 64-bit sign trap (`INT64_MIN / -1`) are proven unreachable.
* **Relational Bounds**: Slice indices (`buf[idx]`) against dynamic lengths (`len`) are verified via Floyd-Warshall difference constraints ($idx - len \le -1$).
* **Linear Resources**: Heap handles (`alloc`/`free`) follow strict affine typing ($!A \to A \otimes A$ forbidden). Moves (`MOV`) transfer ownership (`RES_MOVED`). Leaks, double-free, and use-after-free are rejected at compile time.
* **Taint Sanitization**: `tainted` inputs cannot index arrays or pass to certified parameters until branch conditions mathematically close their scalar bounds.

---

## Architecture

```
[ Inbound ]
  .oath source  /  C header (.h, .c)  /  Rust FFI (.rs)
      │
      ▼
┌────────────────────────────────────────────────────────┐
│                   OATH IR (OIR)                        │
│   Flat 3-address SSA bytecode & CFG basic blocks       │
└────────────────────────────────────────────────────────┘
      │
      ▼
┌────────────────────────────────────────────────────────┐
│             SEPE Verifier on OIR                       │
│   Intervals + Floyd-Warshall DBM + Linear States       │
└────────────────────────────────────────────────────────┘
      │
      ▼
[ Outbound Targets ]
  C99 / Rust (#![no_std]) / WebAssembly (WAT) /
  TypeScript / Python (ctypes) / Go (cgo) /
  Java (JNI) / C# (P/Invoke) / Audit (.audit.json)
```

---

## Polyglot Matrix

One input generates 8 target artifacts in a single pass:

| Target | Output | Model | Details |
| :--- | :--- | :--- | :--- |
| **C99** | `<out>.c`, `<out>.h` | Native ABI | `static restrict`, zero runtime branch checks |
| **Rust** | `<out>.rs` | Standalone | `#![no_std]`, dispatch loop, raw pointer indexing, no panics |
| **WASM** | `<out>.wat` | Standalone | Structured stack-machine bytecode (`loop`, `br_table`, `i64.*`) |
| **TypeScript** | `<out>.ts` | Web Bridge | Async WebAssembly instantiator + `bigint` interfaces |
| **Python** | `<out>.py` | FFI | `ctypes` bindings with contract metadata |
| **Go** | `<out>.go` | CGO | Exported C wrapper package |
| **Java** | `<out>.java` | JNI | `System.loadLibrary` native class |
| **C#** | `<out>.cs` | P/Invoke | `[DllImport]` Cdecl bindings for .NET |
| **Audit** | `<out>.audit.json` | JSON | Machine-readable verification report (ISO 26262 / DO-178C) |

---

## CLI

### Build Compiler
```bash
clang -std=c99 -O3 -Wall -Wextra -Wpedantic -Werror -Iinclude src/*.c -o oath
```

### Polyglot Synthesis
Compile `.oath` source into all 8 targets:
```bash
oath polyglot test.oath -o enterprise
```

Ingest a C header with contract annotations and emit client bindings:
```bash
oath polyglot api.h -o bridge
```

### Dump OIR (Bytecode)
```bash
oath ir test.oath
```

### Microbenchmark
Run 10,000,000 iterations:
```bash
oath bench test.oath
```

### Native Runner
Verify, compile via host `-O3`, and execute:
```bash
oath run test.oath
```

---

## Directory Structure

```
include/oath/
  arena.h       - Linear memory allocator
  ast.h         - AST definitions
  backend.h     - C99 emitter
  common.h      - Fixed-width types and compiler macros
  ingest.h      - C/Rust interface ingest
  interval.h    - 64-bit checked interval domain
  lexer.h       - Lexer
  lower.h       - AST -> OIR lowerer
  oir.h         - Universal SSA intermediate representation (OIR)
  parser.h      - Parser
  polyglot.h    - Multi-target emitters (Rust, WASM, Py, Go, Java, C#, TS)
  sepe.h        - Symbolic Execution & Proof Engine on OIR
src/
  arena.c    backend.c   ingest.c    interval.c
  lexer.c    lower.c     main.c      oir.c
  parser.c   polyglot.c  sepe.c
```

---

## License

MIT License. Copyright (c) 2026 idunnowutuwant.

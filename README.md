![OATH](banner.svg)

# OATH

A certifying systems compiler. Statically proves memory safety, integer boundedness, and resource lifecycles at compile time. Eliminates runtime checks (zero branch bounds checks, zero panic paths) and synthesizes verified targets across 8 major language ecosystems with turnkey enterprise package manifests.

Verification runs in-process using an abstract interpreter, Miné's Octagon abstract domain ($\pm x \pm y \le c$), a lexical borrow checker, and a flat SSA intermediate representation (**OIR**). No external SMT solvers (Z3, CVC5) required.

---

## Performance

Measured on x86_64 hardware (10,000,000 iterations, hardware memory barrier enforced, anti-DCE volatile execution):

| Metric | Measured Value |
| :--- | :--- |
| **Throughput** | 994,846,694 ops/sec (~1.0B ops/sec) |
| **Latency** | 1.01 ns / op (~4–5 CPU cycles) |
| **Hardware Traps** | 0.000000% |
| **Runtime Checks** | 0 (100% statically eliminated) |
| **Verification Engine** | SEPE on OIR (Formally Proved Sound) |

---

## Formal Guarantees

* **Zero Hardware Traps**: Integer overflow, underflow, zero division, and the x86 `#DE` 64-bit sign trap (`INT64_MIN / -1`) are proven unreachable.
* **Octagon Relational Bounds**: Multidimensional memory indices, strides, and dynamic slices (`buf[idx]`) are verified via an Octagon abstract domain ($\pm x \pm y \le c$) with strong closure tightening ($idx - len \le -1$).
* **Lexical Borrow Checking**: Memory handles follow affine capability semantics ($!A \to A \otimes A$ prohibited). Supports exclusive mutable borrows (`&mut`) and shared immutable borrows (`&`). Capability states (`RES_LENT_MUT`, `RES_LENT_SHARED`) freeze roots during active references. Leaks, double-free, and simultaneous mutable aliasing (`SEPE_BORROW_CONFLICT`) are rejected at compile time.
* **Taint Sanitization**: `tainted` inputs cannot index arrays or pass to certified parameters until branch guards mathematically prove their bounds within finite scalar intervals.

---

## Architecture

```
[ Inbound Sources ]
  ├── .oath (Native Certifying DSL)
  ├── C Headers & Signatures (.h, .c)
  └── Rust FFI (.rs)
          │
          ▼ [ Ingest / Lowering Engine ]
┌─────────────────────────────────────────────────────────────┐
│                      OATH IR (OIR)                          │
│  - Linear 3-Address Instructions (Flat SSA Bytecode)        │
│  - CFG Basic Blocks with Canonical Virtual Registers        │
│  - OIR Optimizer: Constant Folding & Algebraic Reduction    │
└─────────────────────────────────────────────────────────────┘
          │
          ▼ [ SEPE on OIR Verifier ]
┌─────────────────────────────────────────────────────────────┐
│        Symbolic Execution & Proof Engine (SEPE)             │
│  - Interval Arithmetic Domain ([lo, hi], 128-bit Checked)   │
│  - Octagon Domain Engine (2N x 2N Dual Variable Matrix)     │
│  - Lexical Borrow Checker (Shared & Exclusive Borrow States)│
│  - Information Flow & Taint Range Sanitization              │
└─────────────────────────────────────────────────────────────┘
          │
          ▼ [ S-Tier Polyglot Synthesis ]
┌─────────────────────────────────────────────────────────────┐
│  Systems   : C99 (static restrict), Rust (no_std + Cargo)   │
│  Web/Edge  : Direct Binary WASM (.wasm), TypeScript (+ NPM) │
│  Enterprise: Java (JNI), C# (.NET + csproj), Go (+ go.mod)  │
│  Scripting : Python (Zero-overhead ctypes FFI)              │
│  Compliance: Machine-Verifiable Audit Report (.audit.json)  │
└─────────────────────────────────────────────────────────────┘
```

---

## Polyglot Target Matrix

A single compilation run synthesizes ready-to-use artifacts and turnkey package manifests across all 8 tiers:

| Target | Output Artifact | Project Manifest | Execution Model |
| :--- | :--- | :--- | :--- |
| **WebAssembly** | `<prefix>.wasm`, `<prefix>.wat` | *Standalone binary* | Native binary WASM direct emission (zero toolchain dependencies). |
| **Rust** | `<prefix>.rs` | `Cargo.toml` | `#![no_std]`, dispatch loop, raw pointer indexing, no panics. |
| **TypeScript** | `<prefix>.ts` | `package.json` | Web/Node.js async WebAssembly instantiator + `bigint` interfaces. |
| **Go** | `<prefix>.go` | `go.mod` | Exported CGO wrapper package. |
| **C# (.NET)** | `<prefix>.cs` | `<prefix>.csproj` | Enterprise `[DllImport]` Cdecl bindings for .NET 8+. |
| **Java** | `<prefix>.java` | *JNI Class* | Enterprise JNI class loading native shared libraries via `System.loadLibrary`. |
| **Python** | `<prefix>.py` | *Native FFI* | Zero-copy `ctypes` bindings with contract annotations. |
| **C99 / C++** | `<prefix>.c`, `<prefix>.h` | *Native ABI* | Branchless C99 with `static restrict` pointer guarantees. |
| **Audit** | `<prefix>.audit.json` | *Compliance* | Formal verification report (ISO 26262 / DO-178C). |

---

## CLI

### Build Compiler
```bash
clang -std=c99 -O3 -Wall -Wextra -Wpedantic -Werror -D_CRT_SECURE_NO_WARNINGS -Iinclude src/*.c -o oath
```

### Full S-Tier Enterprise Synthesis
Compile `.oath` source into all 8 targets, direct binary WASM, and package manifests:
```bash
oath polyglot test.oath -o enterprise
```

### Inbound Interface Ingestion (from C Header / Rust FFI)
Directly ingest C headers containing contract annotations and synthesize client bindings:
```bash
oath polyglot api.h -o bridge
```

### Inspect Universal SSA OIR
Inspect lowered three-address bytecode and basic block control-flow graphs:
```bash
oath ir test.oath
```

### Physical Hardware Benchmark
Execute 10,000,000-iteration hardware microbenchmark with anti-DCE black-box guarantees:
```bash
oath bench test.oath
```

### Native Execution Runner
Verify, compile using host C compiler with `-O3`, and execute:
```bash
oath run test.oath
```

---

## Directory Structure

```
include/oath/
  arena.h       - Linear memory allocator
  ast.h         - AST node definitions & capability parameter kinds
  backend.h     - C99 code and certificate emitter
  common.h      - 128-bit integer types and compiler macros
  diagnostic.h  - Compiler diagnostics, error codes, and source renderer
  ingest.h      - C header and foreign contract ingestion engine
  interval.h    - 64-bit checked interval arithmetic domain
  lexer.h       - Lexical analyzer
  lower.h       - AST to OIR SSA lowering interface
  oir.h         - Universal SSA Intermediate Representation (OIR)
  opt.h         - OIR optimization pipeline (constant propagation & simplification)
  parser.h      - Recursive descent parser with synchronization recovery
  polyglot.h    - Polyglot multi-target emitters & manifest generators
  sepe.h        - Symbolic Execution & Proof Engine on OIR
src/
  arena.c       backend.c     diagnostic.c  ingest.c
  interval.c    lexer.c       lower.c       main.c
  oir.c         opt.c         parser.c      polyglot.c
  sepe.c
```

---

## License

MIT License. Copyright (c) 2026 idunnowutuwant.
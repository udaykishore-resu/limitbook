# C++ feature policy

"Modern C++" here means **compile-time abstraction, runtime austerity**. Features that move work to compile time are encouraged everywhere. Features that hide allocation, indirection or unwinding are banned on the hot path (decode → book update → match). Every rule below has a reason you should be able to defend in an interview.

## Used, and why

| Feature | Where | Why |
|---|---|---|
| Concepts / `requires` | `FillSink`, `IntrusiveList<T> requires std::derived_from<T, ListHook>` | Readable constraint errors; static polymorphism without CRTP boilerplate |
| `if constexpr` + `std::is_invocable_v` | `itch::detail::dispatch` | Handlers opt into message types at compile time; unused types cost nothing |
| `std::span<const std::byte>` | decoder and framing | Bounds and lifetime are explicit; no `(ptr, len)` pairs |
| `std::byte` | all wire buffers | Bytes are not characters or integers; prevents accidental arithmetic |
| `std::endian`, `__cpp_lib_byteswap` | `endian.hpp` | Standard byte order detection; feature-test for C++23 `std::byteswap`, builtin fallback |
| `std::countl_zero`, `std::bit_ceil` | histogram, index sizing | Portable `clz` and power-of-two rounding |
| `std::construct_at` / `std::destroy_at` | `ObjectPool` | Explicit object lifetime in raw storage |
| `[[likely]]` / `[[unlikely]]` | exhaustion and error branches | Keeps the cold path out of the hot instruction stream |
| `[[nodiscard]]` | every fallible operation | A dropped `nullptr` from `add()` is a silent book corruption |
| Defaulted `operator==` | `Quote`, `Fill` | Value semantics for tests without hand-written comparisons |
| `constexpr` everything possible | histogram bucket math, message sizes | `static_assert` checks layout against the ITCH spec at build time |

## Banned on the hot path, and why

| Feature | Why it's banned | What replaces it |
|---|---|---|
| Exceptions | Unwinding tables, and the compiler must assume any call can throw. `limitbook-replay` builds with `-fno-exceptions` to prove the headers comply | `nullptr` / `bool` / status enum plus `[[nodiscard]]` |
| RTTI (`typeid`, `dynamic_cast`) | Not needed; costs binary size. Built with `-fno-rtti` | Static dispatch |
| `virtual` per message | Indirect call per message; blocks inlining | Templates and concepts |
| `std::function` | Type erasure; may heap-allocate captures | Template parameter (`FillSink`, handler) |
| `std::shared_ptr` | Atomic refcount traffic on every copy | Pool-owned raw pointers with one clear owner |
| `std::map` / `std::list` / `std::unordered_map` | Node allocation per element; pointer chasing | Sorted vector, intrusive list, open addressing |
| `std::string` | Allocation past SSO | `std::string_view` over wire bytes |
| `iostream` | Locale machinery, static init, slow | `printf` in tools only, never in the library |
| Floating-point prices | Equality and ordering must be exact | `int64_t` ticks (ITCH 1e-4 units) |

## Allowed off the hot path

Tests, the synthetic generator and the `std::map` oracle use the standard containers freely. They are oracles: obviously correct beats fast.

## Warnings are part of the policy

`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align -Wdouble-promotion -Werror`, plus `-Wnull-dereference` on Clang. Two GCC warnings are deliberately disabled, each with a reason in `cmake/ProjectOptions.cmake`:

- `-Wuseless-cast`: `size_t` vs `uint64_t` differ on macOS, so the cast is required there.
- `-Wstringop-overflow`: GCC 12/13 false positives inside `std::vector::insert` at `-O3`.

## Version policy

C++20 is required: GCC 12+, Clang 16+, AppleClang 15+. C++23 library features are used only behind feature-test macros, because Apple's libc++ ships them later than libstdc++.

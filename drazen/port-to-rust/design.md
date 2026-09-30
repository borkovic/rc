# Porting rc to Rust: Obstacle Analysis and Design Notes

Status: draft analysis, not an implementation plan. Goal is to catalog every
structural obstacle to a Rust port, expand on the three already identified in
`obstacles.txt`, and sketch viable Rust-idiomatic replacements for each.

Codebase snapshot: ~14.3k lines of C across ~50 `.c`/`.h` files, two yacc
grammars (`parse.y` for rc syntax, `calc.y` for `$((...))` arithmetic), a
custom small-object arena allocator, a custom varargs formatting engine, and
setjmp/longjmp-based control flow for break/continue/return/error unwinding.

## 1. Parsing (grammar + lexer)

Already identified. Additional detail:

- Two independent grammars/parsers coexist: `parse.y` (rc statements) and
  `calc.y` (arithmetic, generated via byacc/bison as a *reentrant*,
  differently-prefixed parser — see the `calcparse`/`calclex` macro
  indirection in `calc.y`). A Rust port needs two parsers or one grammar
  engine driving both, and the calc grammar is currently vendored/generated
  separately from the rc grammar (`calc.tab.h`, `calc_decl.h`).
- The rc lexer (`lex.c`) is hand-written and *stateful*: it tracks quoting,
  heredoc bodies (`heredoc.c`, `qdoc`/`hq` global list), nested backquotes,
  and interacts with the *input* stack (`input.c`) character-by-character,
  including mid-lex prompting (`nextline()` prints `$prompt2` while still
  inside `yylex`). This is not a clean tokenize-then-parse pipeline; the
  lexer calls back into I/O and history-logging as it goes. Porting this to
  `logos`/hand-written Rust lexer is fine, but reproducing the interleaving
  with prompting and heredoc collection needs care — it's not purely
  finite-state, it's coupled to shell semantics (e.g. a heredoc's body is
  swallowed by the lexer only once `qdoc()` has registered how many are
  pending, and multiple heredocs on one line queue up on `hq`).
- Error recovery is yacc-specific: `yyerror`/`scanerror` interact with
  `rc_raise(eError)`, which prints, resets shell state (`cond`, `redirq`,
  `interactive`), and does a stack unwind (see §3). Any parser generator
  swap must reproduce "one syntax error aborts the *current top-level
  statement*, not the whole shell" semantics, and interactive mode must
  resync at the next input line.
- `grmtools`/`lrpar` (LR) vs the original yacc (LALR with conflict
  resolution via precedence/associativity declarations, and mid-rule
  actions) — `parse.y` and `calc.y` both use mid-rule actions and
  precedence declarations that may not map 1:1 to lrpar's conflict model;
  this was already flagged as causing lrpar errors. A plausible fallback is
  a hand-written recursive-descent/Pratt parser for both grammars instead
  of a parser-generator port — rc's grammar is small enough that this may
  actually be *less* work than fighting a different parser generator's
  conflict resolution, and it sidesteps needing two generated parsers.
- Confirmed from `rc.1`'s `GRAMMAR` section (the shipped skeletal grammar,
  actions stripped): the empty-producible nonterminals are `cmd` (empty
  command, `%prec WHILE`), `else` (`%prec ELSE`), `epilog`, `optcaret`,
  `words`, `nlwords`, and `optnl` — seven rules, all resolved via
  precedence declarations rather than restructuring the grammar to avoid
  the ambiguity. lrpar (via its `yacc_kind: Original` mode) does support
  `%left`/`%right`/`%nonassoc` precedence declarations including a
  fictitious terminal used purely for precedence (rc's grammar already
  does this itself — `%nonassoc PREDIR /* fictitious */` — so the pattern
  isn't unprecedented for lrpar to need to reproduce), but each of these
  seven empty productions is a concrete site to check individually against
  lrpar's conflict reporting rather than assuming precedence declarations
  alone will resolve them the same way LALR did.
- `rc.1`'s `BUGS` section documents "a compile-time limit on the number of
  `;`-separated commands in a line: usually 500" — this is a yacc/bison
  parser-stack-depth artifact (`YYMAXDEPTH`-style), not a deliberate
  design constraint. Worth explicitly deciding to drop it rather than
  reproduce it: a hand-written or lrpar-generated parser has no reason to
  inherit a fixed statement-count ceiling, and silently reproducing it
  would be a case of porting an implementation accident as if it were a
  requirement. (Full-parity `trip.rc` testing should not depend on this
  limit's exact value if it's intentionally dropped — worth checking
  `trip.rc` doesn't test for it.)

## 2. Arena allocator (`nalloc.c`)

Already identified. Additional detail specific to *how* it's used, which
matters for what a Rust replacement needs to support, not just what
allocates fast:

- It's not one arena for the process lifetime: `newblock()`/`restoreblock()`
  implement *nested* arena scopes. `doit()` in `input.c` calls
  `newblock()`/pushes an `eArena` exception frame before parsing+executing
  each top-level statement, and `restoreblock()` on completion *or on
  unwind* (via `unexcept`/`rc_raise`, see §3) frees everything allocated
  during that statement — including the parse tree itself. So arena
  lifetime is tied to control flow, not lexical scope: a `bumpalo::Bump`
  per statement would need to be paired with the same unwind-safe
  restore-on-error semantics rc gets almost for free from `longjmp`
  unwinding past the `restoreblock` call in `unexcept`.
- Some arena-allocated data *escapes* its arena deliberately:
  `fnassign()`/`fnlookup()` explicitly `treecpy(def, ealloc)` function
  bodies out of arena space into `malloc`'d ("permanent") space so a
  function definition survives past the statement that defined it. This
  dual-allocator pattern (fast scoped arena + permanent heap, with manual
  "promote out of the arena" copies) has to be replicated, not just the
  arena mechanics. Anything reachable from a `Variable`/`rc_Function` in
  the hash table must never live in the transient arena.
- The arena is a global (`static Block *ul`), not a value threaded through
  call chains — `nalloc()` is called from >30 call sites (parser actions,
  `glom.c`, `list.c`, `tree.c`, `word()`) with no allocator argument. A
  faithful Rust port either keeps a thread-local/global arena (fine, rc is
  single-threaded) or threads an allocator handle through every
  tree/list-building function signature, which is a large mechanical
  change across the parser and glom/walk code.
- Freed blocks are recycled onto a free list (`fl`) rather than returned to
  the OS immediately, capped at `MAXMEM`. `bumpalo` supports reset-and-reuse
  of a `Bump`, which covers this, but the "keep up to MAXMEM bytes of spare
  capacity, free the rest" policy is bespoke and would need reimplementing
  if that memory ceiling behavior matters.

## 3. Control flow via setjmp/longjmp (`except.c`, `jbwrap.h`)

Already identified as "exception handling." It's worth separating two
distinct things this mechanism does, because they need different Rust
strategies.

**Recommended design: compile to a flat opcode array and interpret with an
explicit VM loop, instead of tree-walking.** This is a better fit than
trying to map `except.c` onto Rust's own unwinding machinery (an earlier
draft of this doc sketched an RAII-guard-per-frame-type design; the
bytecode approach below supersedes it — it solves the same problem with
plain data instead of relying on `Drop`/panics as control flow). The shape:

- Compile the parse tree (or compile directly from parser actions,
  see the note on `Node` in §4) into a flat `Vec<Instr>` per compiled unit
  (function body, top-level statement, loop body). `break`/`continue`
  become `Jump`/`JumpIfFalse` to a resolved offset — this is exactly what
  a bytecode compiler does with loop targets, and it eliminates
  break/continue as a control-flow *problem* entirely: there is no
  non-local jump, just `ip = target` in the dispatch loop. This is
  strictly easier than even the "just use a Rust `enum ControlFlow`
  threaded through recursive `walk()` calls" approach floated for this
  case in an earlier draft — with a flat instruction array you don't need
  to thread anything back up through recursive call frames at all.
- `return` becomes a jump back through an explicit **call-frame stack**
  you maintain as VM state (`Vec<Frame>` with return addresses/instruction
  pointers), not the Rust call stack. Function calls push a frame, `return`
  pops it and jumps to the saved return address.
- The genuinely hard part of `except.c` — `eError` propagating through
  arbitrary nesting while running cleanup at each level it passes through
  (arena restore, var-stack pop, fd close, fifo unlink) — reduces to
  exactly the same algorithm `rc_raise()` already implements, just
  operating over **VM-owned data instead of the C call stack**: maintain
  an explicit `Vec<Frame>` (mirroring `Estack`, tagged by frame kind:
  loop, call, arena-checkpoint, var-push, fd, fifo), and on error, pop
  frames running each one's cleanup until a frame of the target kind is
  found — or none exists, matching `rc_raise`'s fallback to
  `rc_exit`/reporting an unhandled error. This is plain `Vec` manipulation:
  no `unsafe`, no reliance on Rust's `panic!`/`catch_unwind` (which is
  slow and explicitly discouraged as a control-flow mechanism), and no
  fighting Rust's unwinding model to make it "selective" the way
  `rc_raise` is. It is a direct, mechanical port of `rc_raise`'s existing
  logic (including the `"break outside of loop"` / `"return outside of
  function"` nesting-rule checks) onto an explicit data structure instead
  of a linked list threaded through native stack frames.
- Concretely, an arena checkpoint (`newblock()`/`restoreblock()`) becomes
  just one more `Frame::ArenaCheckpoint(usize)` variant popped/cleaned the
  same way as any other frame, which also resolves obstacle #2's
  arena-lifetime-tied-to-unwind coupling for free — no separate RAII-guard
  design is needed for that either.
- `eError` unwinding is also **triggered from a signal handler**
  (`sigint()` in `except.c`, plus arbitrary user-defined `fn sighandler`
  trap functions invoked via `catcher()`/`sigchk()` in `signal.c`/`fn.c`)
  and can jump out of a `read()`/`wait()` syscall via
  `HAVE_RESTARTABLE_SYSCALLS`'s `slowbuf`/`siglongjmp` in `system-bsd.c`.
  Signal-handler-driven longjmp out of libc calls has no safe Rust
  equivalent at all (Rust unwinding through a signal handler across libc
  frames is UB). This has to be redesigned around self-pipe/signalfd
  style deferred signal delivery (set a flag in the handler, check it
  at a syscall retry point — actually the codebase's `sigchk()` +
  `caught[]` flag array in `signal.c` already separates "mark as caught"
  from "act on it," except for the `slow`/`slowbuf` fast-path bypass for
  restartable syscalls, which is precisely the part that can't be kept).
  This is a real obstacle beyond the generic "no longjmp" one.
- `rc_error`/exception state also touches *shell-visible* global mutable
  state as part of unwinding (`redirq = NULL; cond = FALSE;`), so the
  Rust replacement's "signal" type needs to carry/trigger the same
  side effects, not just transfer control.

## 4. The `Node` tree representation: tagged union with variable arity, built via C varargs

`rc.h`'s `Node`:
```c
struct Node {
    nodetype type;
    union { char *s; int i; Node *p; } u[4];
};
```
and `tree.c`'s `mk()` allocates **less memory than `sizeof(Node)`** for most
node kinds — `nalloc(offsetof(Node, u[N]))` for the specific `N` (1, 2, 3,
or 4) fields that node type actually uses, then fills only those slots via
a `va_arg` walk keyed on a `switch (t)`. This is a legal-but-sharp C idiom
(flexible-array-adjacent trick relying on the allocator never reading past
`u[N]`), and every consumer (`walk.c`, `glom.c`, `print.c`'s `%T`
formatter, `treecpy`/`treefree` in `tree.c`) has an equivalent hand-written
`switch (type)` that must agree, by convention only, on which union arm and
how many slots are valid for each `nodetype`. There is no compiler-enforced
correspondence between `nodetype` and which `u[i]` are populated/typed.

This has no direct Rust translation and is worth calling out as its own
obstacle distinct from "no unions" — the fix is straightforward (a proper
Rust `enum Node { Word(String, String, i32), Pipe(i32, i32, Box<Node>,
Box<Node>), ... }` with each variant's fields as real typed fields), but
*every* site that currently pattern-matches on `type` and reaches into
`u[i]` (there are four: `tree.c` mk/treecpy/treefree, `walk.c`'s
interpreter, `glom.c`'s glom/assign, and `print.c`'s tree-printing `%T`
conversion) must be rewritten in lockstep, and the variable-arity
allocation trick (saving 1-3 pointers per node vs. a fixed `[Node; 4]`)
goes away for free once it's a real enum — Rust enums are already
sized/tagged by their largest variant, this isn't a regression, just not a
"port the struct" job.

**With the §3 bytecode-VM design, this enum may not need to outlive
compilation at all.** If the parser compiles straight to opcodes (parser
action → emit `Instr`, no persistent tree built), the only long-lived
`Node`-shaped data left is whatever the compiler needs as its own input
representation — which can be as ephemeral as the compiler pass wants,
since nothing downstream (`walk.c`, `treecpy`, `%T` printing) touches it
after compilation. The one wrinkle: rc currently *reconstructs source* from
a `Node` tree for two purposes — `fn`/`whatis` pretty-printing (`%T` in
`print.c`, `prettyprint_fn`) and exporting function definitions through the
environment as `fn_name={...}` text (`fnlookup_string` in `fn.c`, consumed
by child rc processes via `fnassign_string`). Either purpose needs *some*
persistent source-like representation to survive past compilation — the
existing code already leans this way (`rc_Function.extdef` stores the
external string form and `fnlookup` lazily reparses it, `mk()`'s tree is
not what's kept around long-term for exported functions). The cleanest
option is likely to keep the **original source text** as the persistent
form (already true for env-imported functions) and treat "pretty-print" as
"print the source," making a persistent `Node` enum optional infrastructure
rather than a load-bearing part of the runtime — it would only need to
exist as a short-lived compiler-internal type if `%T`-equivalent behavior
can be reduced to "return the stored source slice."

## 5. Custom extensible `printf`-family engine (`print.c`)

rc has its own `fmtprint`/`fprint`/`mprint`/`nprint` implementing a
`printf`-like mini-language on top of C varargs (`va_list`), with **runtime-
installable custom conversion specifiers** via `fmtinstall(int c, Conv f)` —
e.g. `%T` (print a `Node*` tree), `%A` (print an argv-style `char**`), `%F`/
`%S` (rc-specific string quoting). This is used pervasively for error
messages, `fn`/`whatis` pretty-printing, and building strings from trees
(`mprint("fn_%F={%T}", name, look->def)` in `fn.c`). Rust has no varargs and
no runtime-pluggable format machinery (`format!`/`Display` are compile-time
and monomorphic per type, not per-character-code). Porting this means either:
- replacing each `%X` call site with an explicit typed formatting function
  (a `fmt_tree(&mut String, &Node)`, `fmt_argv(&mut String, &[String])`,
  etc.), losing the single-mini-language-string convenience but gaining
  type safety — the realistic choice; or
- building a small internal templating engine with `Box<dyn Fn>` handlers
  to keep the "install a conversion, then use `%c` in a template string"
  pattern, which is more faithful to the C code's shape but is extra
  machinery to port for little benefit.
This is a non-trivial, cross-cutting rewrite (print.c is one of the more
subtle files — buffer growth callbacks, per-conversion flag parsing) that
isn't captured by "allocator" or "parsing" or "exceptions."

## 6. Global mutable state everywhere

The whole interpreter is built on file-scope `static`/global mutable state
with no synchronization (fine in a single-threaded C program, but Rust's
ownership model actively resists this shape): the input stack (`istack`/
`itop`/`inbuf` in `input.c`), the exception stack (`estack` in `except.c`),
the function/variable hash tables (`fp`/`vp` in `hash.c`), the arena
(`ul`/`fl` in `nalloc.c`), the child-process list (`plist` in `wait.c`),
per-signal handler tables (`sighandlers`/`caught` in `signal.c`,
`handlers`/`runexit` in `fn.c`), and shell option flags (`dashdee` etc. in
`main.c`). None of this is behind a struct/module boundary — files reach
into each other's globals directly via `extern`. A Rust port needs to
decide early whether to:
- centralize this into one `struct Shell { .. }` threaded explicitly through
  every function (idiomatic, but touches nearly every function signature in
  the codebase — this is a bigger mechanical migration than it sounds,
  since e.g. `nalloc()`/`ealloc()` and `varlookup()` are called from
  parser actions, builtins, glom, and the printf engine alike), or
- use `thread_local!`/`static` with `RefCell`/`Cell` to minimize call-site
  churn at the cost of losing most of the borrow-checker's benefit (a
  "C but in Rust syntax" outcome many idiomatic-Rust ports specifically
  want to avoid).
This decision affects essentially every other item in this document (the
arena, the exception stack, hash tables), so it's worth resolving as a
first design decision rather than deciding it file-by-file.

## 7. Process/job control and signal-handler-driven interpreter re-entrancy

- `wait.c`/`exec.c`/`redir.c` do fork/exec/dup2/waitpid style job control,
  which maps onto Rust fairly directly via `nix`/raw libc (there's no
  portable safe Rust process-group/job-control API, so this stays `unsafe`
  either way — not a blocker, just a note that this isn't a "safe Rust"
  win).
- `fn.c`'s user-definable signal handlers (`fn sigint {...}`) call back
  into the **tree-walking interpreter from inside a signal handler path**
  (`fn_handler` → `funcall` → `walk()`), which is only safe in the current
  code because the real work happens via `rc_signal`'s deferred
  `sigchk()`/flag-check design, not directly inside the OS signal handler.
  Any Rust rewrite must preserve "the actual interpreter re-entry happens
  at a safe point after the signal, not inside the handler," which is
  easy to accidentally violate while restructuring control flow per §3.
- Restartable-syscall handling (`HAVE_RESTARTABLE_SYSCALLS`,
  `system-bsd.c`'s `rc_read`/`rc_wait`, `slowbuf`) is a portability shim
  that itself relies on siglongjmp out of a blocking syscall — called out
  in §3(b) but worth flagging separately as a wait.c/system.c-level
  obstacle too, since it affects the blocking-read and waitpid loops, not
  just error unwinding.

## 8. Pluggable line-editing backends selected at compile time

`edit.h` + six interchangeable backends (`edit-null.c`, `edit-edit.c`,
`edit-editline.c`, `edit-readline.c`, `edit-vrl.c`, `edit-bestline.c`),
chosen via a `Makefile` variable (`EDIT=`) linking against a different C
library (readline, editline, vrl, bestline) or none. A Rust port needs
either FFI bindings to whichever of these libraries stay in scope, or a
decision to standardize on one Rust-native line-editing crate (e.g.
`rustyline`) and drop backend choice as a feature. This isn't hard, but
it's scope: it's real, non-generated code (each backend file wraps a
different C API: readline's `rl_callback_*`, its own history file format
in `history.c`, terminal `termchange()` hooks into `input.c`).

## 9. Build-time code generation (`mksignal.c`, `mkstatval.c`)

Two host-compiled-and-run C programs generate `sigmsgs.h` (signal
name/number/message table, built from whatever `<signal.h>` macros the
*build* platform defines) and a status-value table, at build time, so the
signal table is portable across platforms without `#ifdef`-per-signal
sprawl in the main source. A Rust port needs an equivalent `build.rs`
step (Rust has no portable way to enumerate which `SIGxxx` constants a
given libc defines either, so this still ends up as a small C helper or
`bindgen`/`libc` crate feature detection run from `build.rs`) — not hard,
but it's an actual build-system obstacle, not just a source-translation
one, and easy to overlook since it's invisible at the `.c` file level.

## 10. Autoconf-style feature-detection `config.h`

`config.def.h`/`proto.h` gate ~30 platform capability macros (`HAVE_SIGACTION`,
`HAVE_RESTARTABLE_SYSCALLS`, `HAVE_DEV_FD`, `SETPGRP_VOID`, `RLIM_T_IS_QUAD_T`,
etc.) reflecting real historical portability needs (BSD vs SysV signal
semantics, presence of `/dev/fd`, `quad_t` vs `long`, void vs non-void
`setpgrp()`...). A subset of these no longer matter on the platforms rc
realistically targets today (Linux/macOS/BSD with POSIX signals), and can
simply be dropped rather than ported; but the ones still relevant to
Linux/macOS binary compatibility (job control differences, `/dev/fd`
presence, restartable syscalls not existing anymore as a portable concept)
need a real decision, not a mechanical `cfg!` translation, since some of
these `#define`s encode assumptions (e.g. "SysV vs. BSD signal semantics")
that don't have a 1:1 modern equivalent. Worth an explicit prune-and-decide
pass rather than assuming every macro needs a Rust `cfg`.

## 11. Historical note: this analysis supersedes/duplicates two smaller notes already in `drazen/`

`drazen/volatile/sigsetjmp-jbwrap-except.txt` documents an easy-to-miss
ordering dependency in the current C code: `sigsetjmp(j.j, 1)` must be
called *before* `except(e, data, &estack_frame)` at every call site (in
`input.c`, `walk.c`, `builtins.c`) because the `Edata.jb` pointer copied
into the `Estack` frame must point at an already-`sigsetjmp`'d buffer, and
in `input.c`'s `doit()` specifically, `sigsetjmp` must precede `except`
because a returning `longjmp` re-enters at the `sigsetjmp` without
re-running `except`, so the stack frame installed by the *first* pass must
still be the one `unexcept` pops on every subsequent pass through the loop.
This is exactly the kind of invariant that has no representation in C's
type system and is trivially violated by future edits — it's a concrete
illustration of why §3's bytecode-VM design is worth doing even though it's
the hardest item here: with an explicit `Vec<Frame>` pushed by the VM's own
`call`/`loop-enter`/`arena-checkpoint` opcodes rather than a `sigsetjmp`
call paired by convention with a separate `except()` call at each of a
dozen call sites, this ordering constraint becomes structurally impossible
to violate — pushing a frame and recording its resume point are one atomic
step (one opcode, one `Vec::push`), not two separately-written statements
that must stay in a fragile, undocumented order.

## 12. Existing regression test suite (`trip.rc`) as the port's acceptance criterion

The project already has a regression suite invoked as `rc -p < trip.rc`
(`trip.rc` is ~34k lines; `-p` runs rc in "protected mode," skipping
initialization of shell functions from the environment — see `rc.1` — so
the suite starts from a clean function table rather than needing to
control the parent environment). **The Rust port's acceptance bar should
be: the Rust binary passes the same `trip.rc` suite, run the same way,
producing the same observable results as the C implementation.** That
reframes every obstacle above from "does this compile" to "does this
still make `trip.rc` pass," which is a much sharper, mechanically checkable
target than "port looks equivalent." Concretely:
- Wire `rc -p < trip.rc` (both binaries, diffed) into CI from the very
  first milestone, not just at the end — it turns each of the redesigns
  above (bytecode VM, frame-stack unwinding, `Node` enum, printf-engine
  replacement) into something falsifiable as soon as enough of the
  pipeline exists to run any of the suite, rather than something only
  checked once the whole port is "done."
- `trip.rc` exercises rc-*language* behavior end-to-end, so it's a strong
  check on outcomes but a weak localizer of *which* internal redesign
  broke something — pair it with targeted unit tests for the riskiest
  pieces specifically (break/continue/return nesting rules across function
  and loop boundaries, arena-checkpoint restore on error mid-statement,
  signal-triggered `fn sig* {}` handlers firing during a blocking read)
  so a `trip.rc` regression can be traced back to §3/§6/§7 quickly rather
  than requiring a bisection through the whole interpreter.
- Worth auditing `trip.rc`'s existing coverage of exactly those nesting
  edge cases before porting starts, since gaps there are gaps in the
  port's safety net, not just gaps in documentation.

## 13. Full-parity semantic details easy to silently drop

Since the port's scope is full feature parity (confirmed), the following
are behaviors documented in `rc.1` that are easy to get "close enough" on
during a rewrite but that `trip.rc` and/or careful reading would need to
catch as exact-behavior regressions, not just missing features:

- **Nonlinear pipe redirection (`<{cmd}`, `>{cmd}`) has three fallback
  implementations, tiered by platform capability**: `/dev/fd`, then
  `/proc/self/fd`, then a named FIFO in `/tmp` as a last resort (`rc.1`'s
  `BUGS` section: "on modern systems... implemented that way [via
  `/dev/fd`/`/proc/self/fd`]... on older systems it is implemented with
  named pipes"). This ties directly into §3's `Frame::Fifo` cleanup entry
  and §10's `HAVE_DEV_FD`/`HAVE_PROC_SELF_FD`/`HAVE_FIFO` config macros —
  full parity means keeping (or deliberately dropping, if the named-pipe
  fallback is judged not worth it for a Rust port's target platforms) all
  three tiers, not just picking whichever is easiest to write in Rust.
- **`-e`'s "exits on any failing command, but not inside a conditional"
  rule** depends on the interpreter tracking "am I currently evaluating
  the test of an `if`/`while`, or the left side of `&&`/`||`" as dynamic
  state (the `cond` global, set/cleared around those specific constructs
  in `walk.c`/`glom.c`). Under the §3 bytecode-VM redesign, this has to be
  either an explicit VM flag toggled by dedicated opcodes bracketing
  conditional-test bytecode, or encoded directly in which instruction
  variant tests exit status (e.g. a `TestCond` opcode that never triggers
  `-e` vs. a plain statement-boundary check that does) — it's state that
  needs a deliberate home in the new design, not something that falls out
  automatically from "translate the C control flow."
- **Variable aliasing**: `$cdpath`/`$CDPATH` (and implicitly `$path`/`$PATH`)
  are two names backed by one underlying value, kept in sync automatically,
  with a format conversion at the sync boundary (rc list ↔
  colon-separated string) — not just "the same variable importable under
  two names." Whatever replaces `hash.c`'s `Htab` needs to preserve this
  as an explicit bidirectional-sync rule, plus the `neverexport`/
  `maybeexport` exportability tables (`hash.c`) that decide which
  variables round-trip into a child's environment at all — getting the
  export set wrong is an easy way to silently break `$path`-search or
  subshell behavior without `trip.rc` necessarily catching it unless it
  specifically tests environment inheritance.
- **`whatis` output must be re-sourceable** (`rc.1`: "it should be possible
  to recreate the state of rc by sourcing this file with a `.` command").
  This is a hard constraint on whatever replaces `print.c`'s `%T`/`%F`
  tree-to-source pretty-printing (§5): it's not just "print something
  readable," it's "print something that re-parses to the same
  definition," which argues for the "keep original source text, print
  that" approach floated in §4 over any pretty-printer that reformats from
  a compiled/decompiled representation and might not round-trip exactly
  (e.g. through whitespace-sensitive constructs, quoting edge cases).
  Exception explicitly called out in the man page: `whatis -s` output for
  signal handlers is deliberately *not* capturable this way in a subshell
  redirect, because signal handlers are always reset to default after
  `fork()` (`setsigdefaults` in `fn.c`) — this is intentional behavior to
  preserve, not a bug to fix in the port.
- **Field-splitting on backquote substitution collapses consecutive
  separators** (documented under `BUGS`: `` `{echo -n a!!b} `` with
  `ifs=!` yields `(a b)`, not `(a '' b)`) — i.e. `$ifs` splitting behaves
  like whitespace-collapsing `awk`/shell word-splitting, not like a naive
  `str.split(sep)` that would preserve empty fields. Any Rust
  reimplementation of the splitting logic (`glom.c`'s list-building, likely
  wherever `varsub`/backquote output gets tokenized) needs this exact
  collapsing behavior, since it's called out as a known, permanent
  characteristic rather than an incidental bug.
- **SIGCLD is untrappable "on System V-based Unix systems"** (`rc.1`;
  matches `hash.c`'s `HAVE_SYSV_SIGCLD`-gated behavior and `fn.c`'s
  explicit `rc_error("can't trap SIGCHLD")` for `sigchld`/`sigcld`). Worth
  deciding explicitly whether this SysV-specific carve-out still matters
  for the Rust port's target platforms (see §10) rather than either
  silently keeping or silently dropping it.

## Summary table

| # | Obstacle | Novel vs. already-known (1-3)? | Recommended direction |
|---|----------|-------------------------------|------------------------|
| 1 | Dual yacc grammars + stateful lexer/heredoc coupling | Expands #1 | `grmtools`/`lrpar`; grammar must lose mid-rule actions/empty productions regardless of tool |
| 2 | Nested/scoped arena tied to unwind, dual arena+permanent alloc | Expands #2 | Arena checkpoints become one `Frame` variant in the §3 VM frame stack; permanent storage is the compiled-function cache, not the arena |
| 3 | setjmp/longjmp control flow: break/continue/return + selective error unwinding with per-frame cleanup | Expands #3 | Compile to a flat opcode array; VM loop with explicit `ip`/call-frame stack for break/continue/return; explicit `Vec<Frame>` mirroring `Estack`, popped-with-cleanup on error — no reliance on Rust panics/unwinding |
| 3c| Signal-handler-driven longjmp out of blocking syscalls | New, adjacent to #3 | Self-pipe/flag-and-poll at syscall retry points; drop the `slowbuf` fast-path bypass, keep `sigchk()`-style deferred handling |
| 4 | Untagged variable-arity `Node` union built via varargs | New | Likely doesn't need to persist past compilation at all if §3's compile-to-opcodes design is used; source text remains the form used for `fn`/env pretty-printing, as it partly already is via `extdef` |
| 5 | Runtime-extensible custom printf engine | New | Replace `%X` call sites with explicit typed formatting functions |
| 6 | Pervasive unsynchronized global mutable state | New (architectural) | Decide early: single threaded `struct Shell` vs. global/thread-local + `Cell`/`RefCell` |
| 7 | Signal-safe re-entrant interpreter invocation from `fn sig* {}` | New, adjacent to #3 | Preserve "handler sets a flag, real work happens at a safe re-entry point" — don't let VM re-entry move inside the OS handler |
| 8 | Six compile-time-selected C line-editing library backends | New (scope/FFI) | Standardize on one Rust-native crate (e.g. `rustyline`), or FFI only the backend(s) still wanted |
| 9 | Build-time C code generators for signal/status tables | New (build system) | `build.rs` equivalent, likely still shelling out to a small C probe or using `libc`-crate constants |
| 10| Autoconf-style portability `#define`s needing a prune pass | New (build system) | Explicit prune-and-decide pass, not a mechanical `cfg!` translation |
| 12| `trip.rc` as acceptance criterion | New (process) | Wire `rc -p < trip.rc` into CI from milestone 1; audit its coverage of break/continue/return nesting before porting starts |

## Suggested order of attack

1. Resolve #6 (state ownership model) first — it constrains every other
   answer.
2. Design #3's bytecode VM (opcode set, explicit frame stack) together with
   #4 (whether `Node` needs to persist past compilation), since `walk.c`
   sits at the intersection of both and the compile-vs-tree-walk decision
   determines what the compiler's own input/output types need to look like.
3. Pick a parsing strategy for #1 (parser-generator vs. hand-written) once
   #3/#4's compiler-target shape is settled, so grammar actions can emit
   opcodes (or build whatever short-lived intermediate the compiler needs)
   directly, rather than building a persistent tree first.
4. Treat #5, #8, #9, #10 as independently schedulable, lower-risk, mostly
   mechanical work streams once the above architecture is fixed.
5. From milestone 1 onward, run `rc -p < trip.rc` against both
   implementations in CI (#12) — don't defer this until the port is
   "feature complete."

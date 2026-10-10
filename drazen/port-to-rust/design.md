# Porting rc to Rust: Obstacle Analysis and Design Notes

Status: draft analysis, not an implementation plan. Goal is to catalog every
structural obstacle to a Rust port, expand on the three already identified in
`obstacles.txt`, and sketch viable Rust-idiomatic replacements for each.

Codebase snapshot: ~14.3k lines of C across ~50 `.c`/`.h` files, two yacc
grammars (`parse.y` for rc syntax, `calc.y` for `$((...))` arithmetic), a
custom small-object arena allocator, a custom varargs formatting engine, and
setjmp/longjmp-based control flow for break/continue/return/error unwinding.

**Repository layout decision:** the Rust port lives in its own sibling
repository, `rc-rs`, alongside this repo (i.e. `../rc-rs` relative to this
repo's root) rather than as a subdirectory of this C codebase — a
from-scratch rewrite with its own git history, not a fork.

**Implementation status (updated as `rc-rs` progresses — check its own
commit history for the authoritative, current state; this is a snapshot,
not a live view):** scaffolded and underway. Working end-to-end for a real
(intentionally narrow) subset of the language: hand-written lexer
(§1, `lexer.rs`) → lrpar-generated parser with typed actions producing a
real `Ast` (§1.1/§1.2/§4.1, `parse.y`/`ast.rs`/`parse_glue.rs`) →
`compile()` into the `Instr` bytecode (§3.1/§3.2, `instr.rs`/`compile.rs`)
→ a real `Shell`/VM executing it (§3.2, `shell.rs`). Control flow is for
real, not just structurally asserted: `break`/`continue` inside `while`
and `for`, `switch`/`case`, and real (recursive) function calls with
`return` unwinding through a `Call` frame — including the rule that
`break`/`continue` cannot cross a function-call boundary, which turned
out to be load-bearing for correctness, not just parity (§3.2 explains
why). Real external commands run too now (`$path` search + `fork`/
`execv`/`waitpid`, `proc.rs`), inheriting this process's actual fds/
environment directly rather than the in-memory capture the handful of
shell builtins (`echo`/`true`/`false`) still use. Subshells (`@{}`),
background (`&`), and pipelines (real `pipe`/`fork`/`dup2`/`waitpid`
chains, arbitrary stage counts) all work too, including the correct
"strip Loop/Iter/Call frames after fork" rule generalized from the
function-call case. Redirections touch real file descriptors now too,
via save/`dup2`/restore rather than forking (§13's deliberate divergence
from `exec.c`, now confirmed working end-to-end: `break >/dev/null`
inside a loop correctly breaks instead of erroring). Variable
subscripting (`$var(n)`, `$var(m-n)`, `$var(m-)`, ported from `glom.c`)
works too, including its exact clamping/skip-vs-error edge cases,
confirmed against the real binary. `Ast::Pre` (local-assignment and
redirection prefixes, `a=foo cmd` / `>file cmd`, `walk.c`'s `nPre`) is
implemented via two new always-signal-transparent frame kinds,
`Frame::VarStack`/`Frame::Redir` (§13 adds detail on the redir-prefix
divergence this needed, parallel to the postfix-redirect one). Backquote
substitution (`` `cmd ``, `` `{brace} ``, `` ``ifs cmd ``, `glom.c`'s
`backq`/`bqinput`) works too — a real fork with the child's stdout
captured through a pipe (not just inherited, unlike `Fork`), split on
`$ifs` with the documented separator-run-collapsing behavior preserved.
Writing its tests surfaced (and fixed) two real gaps in `$status`
handling that predate `Ast::Backq` itself: `$status` wasn't wired to
`var_lookup` at all, and `Exec`'s empty-argv path incorrectly reset
`$status` to `0` instead of leaving it untouched (§13 has the detail).
Still missing: `calc.y`'s actions, `Nmpipe`, and the remaining `Frame`/
`RcSignal` variants (`Error`/`Arena`/`Fifo`) — nothing compiled yet needs
them.

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
  resolution via precedence/associativity declarations) — this was already
  flagged as causing lrpar errors. **Correction after reading the actual
  `parse.y`/`calc.y` source (an earlier draft of this doc, following
  `obstacles.txt`, guessed the cause was mid-rule actions — that's wrong):
  neither grammar has a single mid-rule (embedded) action.** Every action
  in both files sits at the end of its production, which is exactly the
  form lrpar/grmtools expects — this is good news, not a blocker. The
  actual risk is narrower and is analyzed concretely in §1.1 below.
- Confirmed from `rc.1`'s `GRAMMAR` section (the shipped skeletal grammar,
  actions stripped) and cross-checked directly against `parse.y`: the
  empty-producible nonterminals are `cmd` (empty command, `%prec WHILE`),
  `else` (`%prec ELSE`), `epilog`, `optcaret`, `words`, `nlwords`, and
  `optnl` — seven rules, all resolved via precedence declarations rather
  than restructuring the grammar to avoid the ambiguity (classic
  dangling-else-style resolution: `cmd: /* empty */` competes with
  shifting more tokens, resolved by giving the empty reduction low
  precedence via `%prec`). See §1.1 for the concrete plan to validate this
  against lrpar.
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

### 1.1 Empirical check: byacc reports zero conflicts on both grammars

Ran the project's own `byacc` (`byacc -t -v -d -b parse parse.y` /
`-b calc calc.y`, `.output` inspected for the conflict summary byacc always
emits when any shift/reduce or reduce/reduce conflict exists) against the
actual grammar files as they exist in this repo today: **zero conflicts,
in both `parse.y` and `calc.y`.** The seven empty productions in §1 and
every `%prec`-tagged rule (`redir cmd %prec PREDIR`, `assign cmd %prec
BANG`, `FN words %prec ELSE`, `simple: first/first args %prec ELSE`, the
dangling-else `else` production, `optcaret`) fully resolve under the
declared `%left`/`%right`/`%nonassoc` table — none of it is relying on
byacc's default reduce-conflicts-silently-with-a-warning behavior. This is
a materially different starting point than `obstacles.txt`'s "current
grammar causes lrpar errors" note assumed, and **resolves the
lrpar-vs-hand-written question in favor of lrpar**: since LR(1) (what
lrpar/grmtools builds) is strictly more discriminating than LALR(1) (what
byacc builds) — an LALR(1)-conflict-free grammar cannot pick up *new*
conflicts by moving to LR(1), only potentially resolve borderline ones
that LALR(1) merging would have caused — a verbatim transcription of this
grammar and precedence table into lrpar's `YaccKind::Original` mode should
also produce zero reported conflicts. The concrete next implementation
step is exactly that: transcribe both grammars into `.y` files lrpar can
consume, build with `CTLexerBuilder`/`CTParserBuilder`, and confirm the
conflict count is zero — a fast, cheap, falsifiable check to run before
committing further design effort to the grammar, and if it does surface a
conflict lrpar/LR(1) genuinely can't resolve the way byacc/LALR(1) did,
that's the actual, now-narrowed, decision point for falling back to
hand-written recursive descent — not a default expectation.

**Given this evidence, lrpar is the right call over a hand-written
parser** (superseding the "plausible fallback" framing in the bullet
above) — it gets you real error-recovery (CPCT+) essentially for free,
and there's no longer a concrete reason on the table to expect grammar
conflicts to force a rewrite. Worth still keeping recursive-descent as a
documented contingency (see the item below on `YYABORT`, which is a real,
separate gap lrpar doesn't paper over), but it's no longer the
recommended primary path.

### 1.2 Confirmed empirically: transcribed both grammars into real lrpar 0.15.0, zero conflicts

Went further than the byacc check and actually built both grammars against
real `lrpar`/`lrlex`/`cfgrammar` 0.15.0 (`CTParserBuilder`, `YaccKind::
Original(YaccOriginalActionKind::NoAction)`, `error_on_conflicts(true)` —
the strict default, left on) in a throwaway scratch crate. **Both `parse.y`
and `calc.y`, transcribed with their exact token lists and precedence
tables, build with zero reported conflicts.** This upgrades §1.1's
prediction from "should also produce zero conflicts" to confirmed.

To make sure this was a meaningful result and not a check that trivially
passes, I sanity-tested the methodology by deliberately removing the
`%prec BANG` annotation from `assign cmd %prec BANG` — this immediately
and correctly produced three real shift/reduce conflicts (`cmd: /* empty
*/` vs. shifting `ANDAND`/`OROR`/`PIPE`), reported with precise, readable
diagnostics (better than byacc's, in fact — it names the exact competing
shift and reduce, with a source pointer). Restoring `%prec BANG` returned
the build to zero conflicts. So the zero-conflict result for the real
grammar is trustworthy, not a false negative from a check that can't fail.

Two genuine, previously-unflagged gaps surfaced while doing this — both
are things a *verbatim* transcription cannot survive as-is, independent of
the conflict question:

- **grmtools has no equivalent of yacc's `error` token/rule at all** —
  not "a differently-shaped one," none. Including `parse.y`'s literal `rc:
  error end` production verbatim fails to build with `Unknown reference to
  rule 'error'`: `error` isn't a reserved symbol lrpar recognizes as
  meaning "the token stream the recoverer resynchronizes on." This isn't
  a new problem for the `YYABORT` item above — it's a separate, more basic
  one: the *production itself* has no home in the ported grammar; the
  error-recovery intent it expressed (`yyerrok; parsetree = NULL;
  YYABORT;` — treat a syntax error as "this statement produced nothing,
  keep going") has to be reconstructed as caller-side logic around
  whatever lrpar's CPCT+ recovery hands back (a parse result plus a list
  of recovered-from errors), not as a grammar production to drop. Simply
  delete this alternative when transcribing the grammar — it plays no role
  in the accepted language, only in recovery mechanics, which now live at
  the driver level instead.
- **grmtools treats a "fictitious," precedence-only token (declared only
  to be named in a `%prec`, never appearing in any production's body) as
  an `Unused token`, and escalates that to a hard build error by default**
  (`warnings_are_errors` defaults to `true`). Both of rc's fictitious
  tokens — `PREDIR` in `parse.y`, `CALC_UNARY_PLUSMINUS` in `calc.y` — hit
  this. Yacc/byacc allow the fictitious-token pattern unconditionally (it's
  a well-known, intentional yacc idiom for injecting a precedence level
  with no lexical representation); grmtools does not, by default.
  **Resolved with a real grammar fix, not `warnings_are_errors(false)`:**
  declare the token with `%token` in addition to its `%nonassoc`/`%left`/
  `%right` precedence entry (grmtools, unlike yacc, doesn't treat a bare
  precedence declaration as sufficient to make a symbol referenceable in a
  production body — confirmed empirically: referencing the bare
  `%nonassoc`-only token in a production fails with `Unknown reference to
  rule 'PREDIR'` until `%token PREDIR` is added too), then add it as one
  extra, never-actually-reachable-at-runtime alternative on an existing,
  thematically-adjacent, already-reachable nonterminal — concretely,
  `redir : DUP | REDIR word | SREDIR word | PREDIR ;` (analogously
  `expr : ... | CALC_UNARY_PLUSMINUS ;` for `calc.y`). Confirmed empirically:
  with this change, both grammars build with **zero warnings and zero
  conflicts** under the strict defaults (`error_on_conflicts(true)`,
  `warnings_are_errors(true)` — neither relaxed). This is sound because
  `PREDIR`/`CALC_UNARY_PLUSMINUS` are never actually emitted by the lexer
  in the first place (that's the entire point of a fictitious token) —
  making them grammatically reachable-but-lexically-impossible doesn't
  change which real input strings parse or how, it only satisfies
  grmtools' static "does every declared terminal appear somewhere in a
  live production" check. First attempt (adding the token to an
  *unreachable* dummy nonterminal, e.g. `marker: PREDIR ;` with `marker`
  referenced by nothing) does **not** work — grmtools' reachability
  analysis flags the unreachable nonterminal itself (`Unused rule`) and
  *still* reports the token as unused, since it isn't reachable from
  `%start` either; the fix requires wiring it into a nonterminal that's
  actually reachable, not just any production mentioning it.

**Decision: the grammar is not required to be a verbatim transcription.**
The constraint is the accepted language and semantics, not the shape of
`parse.y`/`calc.y` as yacc rules — so if lrpar's LR(1) construction (or
just ordinary grammar hygiene) is better served by restructuring a
production, that's in scope, as long as the same set of programs parses to
equivalent behavior and `trip.rc` (§12) still passes. This matters
concretely for the `YYABORT` gap right below: rather than contorting the
grammar to fake yacc's mid-action abort inside lrpar, the heredoc-pending
check in `end`/`cmdsan` (and the division-by-zero/negative-power checks in
`calc.y`) can instead be restructured as an explicit post-reduce
validation step outside the grammar's own production shape, since nothing
about rc's *language* depends on that check happening to live inside a
yacc action as opposed to right after the parser returns a value for that
production — it's an implementation accident of how yacc actions
happened to be the convenient place to put it, not accepted-language
behavior worth preserving structurally.

**Remaining real gap: `YYABORT`/`YYACCEPT` mid-action control flow.**
Both grammars use yacc's ability to abort or accept the parse from inside
an action, which has no lrpar equivalent (lrpar actions return typed
values built bottom-up; they don't reach back into the parser's control
state). Concrete sites: `parse.y`'s `end: END {...if (!heredoc(1))
YYABORT;}` / `'\n' {...if (!heredoc(0)) YYABORT;}` and `cmdsan: cmd '\n'
{...if (!heredoc(0)) YYABORT;}` (an unterminated heredoc at end-of-input
or end-of-line is a *semantic*, not syntactic, error — discovered only
once the heredoc-body collector realizes there's nothing left to read),
`rc: line end {parsetree = $1; YYACCEPT;} | error end {yyerrok; parsetree
= NULL; YYABORT;}` (the top-level error-recovery rule), and `calc.y`'s
division-by-zero and negative-power `YYABORT`s. The idiomatic lrpar
replacement — used in grmtools' own example grammars — is: give
error-prone productions a `Result<T, ()>`-shaped semantic value (or thread
a `&mut Vec<Error>` through the action via the user-supplied parser
context), have the action push a diagnostic and return an error/sentinel
value on failure, and let that sentinel propagate bottom-up through parent
productions (which check for it and also short-circuit) rather than
unwinding the parser itself; the top-level caller checks the accumulated
error list once parsing finishes instead of relying on `YYABORT` to jump
there directly. This is a small, well-trodden pattern, but it's a real
rewrite at exactly five call sites, not a mechanical translation — worth
prototyping early alongside the conflict-count check above, since it
touches the heredoc-collection interaction from §1's lexer-statefulness
point too (the heredoc collector needs a way to signal "still waiting for
more input" vs. "reached EOF/EOL with an unterminated heredoc" back into
whichever of these two representations the grammar action reads).

**Dead code found while checking, not to port:** `calc.y`'s `CALC_EQEQ`
action (`expr CALC_EQEQ expr`, i.e. `==`) unconditionally opens/writes/
flushes a hardcoded `eqeq.txt` debug log file and duplicates the same
message to stdout via `fprint` on every single `==` comparison a script
evaluates — this is leftover debugging instrumentation, not shell
behavior to replicate (it would also be a surprising, undocumented
side-effecting file write in a from-scratch implementation if carried
over by rote translation).

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
  (function body, top-level statement, loop body). The loop constructs'
  **own** test/body/back-edge control flow — `while`'s test-then-loop,
  `for`'s iteration, `if`/`else` branching, `&&`/`||` short-circuiting,
  `switch`/`case` dispatch — compiles straight to `Jump`/`JumpIfFalse` at
  fixed, statically-known offsets, exactly like any bytecode compiler's
  loop/branch codegen. That part really is a non-problem once you're not
  tree-walking.
- **Correction to an easy mistake here: `break`/`continue`/`return` are
  *not* syntax rc's grammar can resolve to a jump target at compile time —
  they're ordinary builtin *commands*, dispatched dynamically, same as any
  other command word.** Confirmed from three places: `rc.1`'s shipped
  grammar (`GRAMMAR` section) doesn't list `break`/`continue`/`return` in
  its `keyword` production at all — they're plain `WORD` tokens; `exec.c`
  resolves a command name against the *function* table before the builtin
  table (`!saw_builtin && fnlookup(*av) != NULL` wins over
  `isbuiltin(*av)`), so a user can shadow `break` with `fn break {...}` and
  the shadowed version always wins — the compiler cannot know at compile
  time whether a given `break` token will resolve to the real builtin,
  since `fn` definitions are fully dynamic; and `except.c`'s `rc_raise`
  applies *dynamic call-stack* nesting rules when the real `break` builtin
  does run (`rc_raise(eBreak)` errors with `"break outside of loop"` if it
  hits an `eReturn` frame — a function-call boundary — before finding an
  `eBreak` frame, even though the call may be lexically inside a loop one
  frame up). `trip.rc` (`trip.rc:518`) tests exactly the case that exposes
  this dynamism: `for(i in 1 2){echo $i;break >/dev/null}` reports
  `"break outside of loop"` for *both* iterations — the redirection on
  `break` forces a fork (`dofork()` in `walk.c`'s `nPre` case), and the
  forked child's copy of the exception stack has had its
  `eBreak`/`eContinue`/`eReturn` frames stripped by `clearflow()` (called
  from `rc_fork()`'s child branch in `wait.c`) precisely so control-flow
  signals never appear to cross a real process fork. `trip.rc:517`
  (`` fn f{@{return;echo xxx}};f;echo yyy ``) is the same phenomenon for
  `return` via a subshell fork. **Conclusion: `break`/`continue`/`return`
  must stay dynamic signals in the Rust VM too** — resolved via the normal
  command-dispatch path (function table, then builtin table) like every
  other command, with the builtin implementations returning
  `Err(RcSignal::Break)` / `Err(RcSignal::Continue)` /
  `Err(RcSignal::Return(status))` that then gets walked through the
  `Vec<Frame>` stack below, exactly mirroring `rc_raise`'s nesting-rule
  checks — not lowered to a static `Jump` at compile time. This is
  functionally identical to how `eError` already had to be handled; the
  earlier draft of this doc treated break/continue as "the easy part" that
  reduces to plain jumps, which is only true for the loop's own internal
  control flow, not for the `break`/`continue`/`return` *commands*
  themselves.
- `return`, similarly, is a dynamic signal walked back through an explicit
  **call-frame stack** maintained as VM state (`Vec<Frame>` with return
  addresses/instruction pointers, pushed by a `Call` frame kind), not
  resolved by the Rust call stack or a precomputed jump target. A function
  call pushes a `Frame::Call`; `return`'s `Err(RcSignal::Return(status))`
  walks up to the nearest `Frame::Call`, popping/cleaning any intervening
  loop/arena/var-stack frames along the way (this is exactly what lets
  `return` legally exit a `while`/`for` loop nested inside the same
  function, per `rc.1`: "you can return from a loop inside a function").
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

### 3.1 Concrete `Instr` set and `Frame` enum

A first-pass sketch, enough detail to start implementing against. Values on
the VM's operand stack are `List`s (rc's native value: a linked/vec'd
sequence of `Word { w: Rc<str>, m: Option<Rc<str>>, q: bool }`, mirroring
`struct Word` in `rc.h` — `w` the literal text, `m` an optional glob mask,
`q` whether it was quoted).

```rust
enum Instr {
    // --- value construction ---
    PushWord(WordId),          // literal word -> single-element List
    Concat,                    // pop 2 Lists, push pairwise/distributive ^-concat (rc.1 "List Concatenation")
    VarRef(StrId),             // $var -> push List (empty list if unset)
    VarSubscript(StrId),       // $var(n / m-n / m-) -> push List, pops subscript list first
    VarCount(StrId),           // $#var -> push 1-element List
    Flatten,                   // $^var: pop List, push single space-joined Word
    Glob,                      // pop List, expand filename metacharacters, push List
    Backquote { ifs: Option<StrId> }, // run compiled command, capture stdout, split, push List
    MakeList(u32),             // pop N Lists, build literal (a b c) list

    // --- side-effecting ops ---
    Exec(u32),                 // pop N-word List as argv; resolve fn table, then builtin table, then $path; may fork
    Pipeline(u32),             // N pipeline stages, each a compiled sub-unit; forks+pipes+waits, sets $status
    Redirect(RedirOp),         // queue/apply a redirection (open/dup2/close), mirrors qredir/doredirs
    Assign,                    // pop value List + name, assign (glom.c's assign())
    LocalAssignPush(StrId),    // push Frame::VarStack(name) for "a=foo cmd" local-assignment scoping
    LocalAssignPop,
    FnDef(StrId, CompiledId), FnRm(StrId),
    Match,                     // `~` command: pop pattern list + subject list, push bool-as-status

    // --- static control flow (resolved at compile time) ---
    Jump(IP), JumpIfFalse(IP), JumpIfTrue(IP),
    Not,
    SetCond(bool),             // toggle the `cond` flag around test evaluation, for `-e` semantics (§13)

    // --- loop/arena scaffolding (paired push/pop, not signal-based) ---
    PushLoopFrame(IP),         // Frame::Loop{break_target}; ~ except(eBreak, ...)
    PopLoopFrame,
    PushIterFrame(IP),         // Frame::Iter{continue_target}; ~ except(eContinue, ...) + a fresh Frame::Arena per iteration
    PopIterFrame,
    ArenaCheckpoint, ArenaRestore, // Frame::Arena; ~ newblock()/restoreblock()

    // --- function calls / forking ---
    Call(CompiledId),          // pushes Frame::Call{return_ip, saved $0/$*}, runs body
    Fork(ForkKind),            // subshell/background/pipeline-stage/redirected-builtin;
                               // child branch runs Frame-stripping (~ clearflow()) before continuing — see below
}

enum Frame {
    Error   { resume: IP, was_interactive: bool },     // ~ eError
    Loop    { break_target: IP },                       // ~ eBreak
    Iter    { continue_target: IP },                    // ~ eContinue
    Call    { return_target: IP, saved_star: SavedStar }, // ~ eReturn
    VarStack{ name: StrId },                             // ~ eVarstack
    Arena   { checkpoint: ArenaMark },                   // ~ eArena
    Fifo    { path: PathBuf },                           // ~ eFifo
    Fd      { fd: RawFd },                               // ~ eFd
}

enum RcSignal {
    Error,
    Break,
    Continue,
    Return(Status),
}
```

The `break`/`continue`/`return` *builtins* (dispatched dynamically via
`Instr::Exec`, per §3's correction above, not via any dedicated opcode)
return `Err(RcSignal::...)`. A single `Shell::raise(&mut self, sig:
RcSignal) -> RaiseOutcome` function is the direct port of `rc_raise`: pop
`self.frames` from the top; for each frame that doesn't match `sig`'s
target kind, apply `sig`'s nesting-rule check (the exact table from
`rc_raise` — e.g. raising `Break` past anything other than `Arena`/
`VarStack`/`Iter` is a hard error, matching `"break outside of loop"`) and
run that frame's cleanup (`VarStack` → `varrm`-equivalent, `Arena` →
restore, `Fd`/`Fifo` → close/unlink); on a matching frame, stop and jump to
its stored resume point. No Rust-level unwinding, panics, or `?`-propagation
across arbitrary call depth is involved — `raise` is one bounded loop over
`Vec<Frame>`, callable from anywhere in the VM dispatch loop.

**Fork must strip signal-reachable frames in the child, mirroring
`clearflow()`.** Every `Instr::Fork` (subshells `@{}`, background `&`,
pipeline stages, and the redirected-builtin case that forces a fork before
running a builtin like `break >/dev/null`) duplicates the OS process; in
Rust this means the child branch inherits a full copy of `self.frames`
(same as C's `fork()` copying the exception stack), and — just like
`wait.c`'s `rc_fork()` calling `clearflow()` on the child branch — the
child must immediately strip every `Frame::Loop`/`Frame::Iter`/`Frame::Call`
entry before executing anything. Skipping this step is exactly the bug
class `trip.rc:518`/`:517` guard against: without it, `break`/`return` in
a forked child would silently (and incorrectly) resolve against frame
markers that belong to a loop/function in a *different process*.

### 3.2 Implemented in `rc-rs`: real `Shell`/VM, one bug found via running code

The design above is no longer just a sketch: `rc-rs`'s `src/instr.rs`,
`src/compile.rs`, and `src/shell.rs` implement it end-to-end for a real
(if intentionally narrow) subset — simple commands, pipes, `&&`/`||`,
`if`/`else`, `while`, assignment, brace/body/bang/subshell/background —
with `break`/`continue` genuinely executing through the frame stack inside
a running `while` loop, not just structurally asserted. Deliberately not
yet implemented: real process exec (`$path` search, `execve`, `wait4` —
obstacle #7's territory), `Instr::Pipeline`/`Fork` (need real `fork()`),
redirections actually touching file descriptors, and the `Error`/
`Return`/`VarStack`/`Arena`/`Fd`/`Fifo` frame kinds (nothing compiled yet
needs them — no functions, no arena, no error unwinding).

**One correction the original sketch above got wrong, found only by
running real `break`/`continue` and watching it panic, not by review:**
`PushIterFrame` needed a `continue_target` field it didn't have above —
now added (`PushIterFrame(IP)`, matching `PushLoopFrame(IP)`'s
`break_target`). More importantly, that target must point *after* the
loop body's `PopIterFrame` instruction, not *at* it. The reason mirrors a
real asymmetry in `walk.c`'s `loop_body()`:

```c
if (sigsetjmp(cont_jb.j, 1) == 0) {
    cont_data.jb = &cont_jb;
    except(eContinue, cont_data, &cont_stack);
    walk(n, TRUE);
    unexcept(eContinue);   /* only reached on the NORMAL path */
}
```

`unexcept(eContinue)` sits *inside* the `if (sigsetjmp(...) == 0)` body —
it only runs when `walk()` returns normally. When `continue` actually
fires, `siglongjmp` jumps back to the `sigsetjmp` call directly (returning
nonzero), skipping the `if` body — including `unexcept` — entirely,
because `rc_raise()`'s own unwind already popped the `eContinue` frame as
part of finding it. A first implementation of `Shell::raise` (§3.1's
`raise`) that pops the matching frame during its search, paired with a
compiled `PopIterFrame` instruction that *unconditionally* pops on the
normal path, double-pops when `continue` fires and jumps straight to that
`PopIterFrame` instruction — popping the *next* frame down (the enclosing
`Loop` frame) instead, since the `Iter` frame is already gone. Confirmed
experimentally: this produced exactly that panic (`PushLoopFrame` found
where an `Iter` frame was expected) before the fix. The fix is for
`continue`'s target to land one instruction *past* `PopIterFrame`,
skipping the redundant pop — mirroring `unexcept(eContinue)`'s own
conditional-on-the-normal-path placement exactly, just expressed as "don't
compile a pop instruction into the path `continue` actually takes"
instead of C's runtime `if`.

This is a concrete instance of a general lesson worth stating explicitly:
**the frame-stack design's correctness depends on exactly matching which
cleanup steps the original C code runs on the *raised* path versus the
*normal* path for every frame kind — a distinction easy to get backwards
by analogy/review alone, since both paths often "do the same cleanup" in
the common case and only diverge in a specific reentry-timing detail like
this one.** Worth deliberately auditing this same normal-vs-raised
asymmetry for each remaining frame kind (`Arena`, `VarStack`, `Call`,
`Fd`, `Fifo`) against its C counterpart when implementing them, rather
than assuming the pattern established for `Loop`/`Iter` generalizes
automatically.

Also found while writing tests for this: two of the tests exercising
`continue` were themselves genuine infinite loops (`while (true)` with no
way to terminate), one explicitly commented as relying on "the test
harness's own timeout" as its failure signal — confirmed by a real hung
test run, not caught by inspection. Fixed by using `while ($x)` (execs
whatever `$x` currently holds as a command name — `Ast::Var` already
compiles through the same "atomic value, then `Exec`" path as any bare
word command) to get a mutable, boundable loop condition without needing
arithmetic or comparison operators, neither of which are compiled yet.
Worth remembering as a general testing pattern for this VM until `calc.y`
and `~` are wired up: loop termination in a test needs an actual state
change the compiled subset can observe, not just "eventually something
makes this false" reasoning that turns out to depend on unimplemented
features.

**Since this section was written, `rc-rs` has also implemented `switch`/
`case`, `for`, and real function calls (`fn`/`return`), each adding a
real, tested instance of the same normal-vs-raised-path auditing §3.2
already called for — worth recording concretely rather than just noting
the pattern held:**

- **`for`'s `break` needed the identical fix as `while`'s `continue`, for
  a different reason.** `walk.c`'s `nForin` installs its `eBreak` frame
  *unconditionally*, before ever checking whether the list is empty —
  unlike `nWhile`, which only installs it once the first test has already
  passed. That removes the "never entered, no frame exists to pop" case
  `while` needed a bypass for, but introduces the same two-exit-paths
  shape for the opposite reason: the natural "list exhausted, no signal
  raised" fallthrough must explicitly `PopLoopFrame` (the frame is still
  there), while `break`'s own target must land *after* that pop, since
  `raise()` already removed the frame by the time it jumps there. Same
  underlying principle as §3.2's `continue` fix, confirmed to generalize
  to a second frame kind and a different triggering condition (loop-body
  exhaustion vs. mid-body `continue`) rather than being a one-off.
- **Function calls forced an architectural fork the original §3.1 sketch
  didn't anticipate.** `Loop`/`Iter`'s `break_target`/`continue_target`
  are jump offsets *within the currently-executing `Program`* — that's
  fine as long as every frame's resume point lives in the same flat
  instruction array. A function call breaks that assumption: it runs a
  wholly different, unrelated `Program` (the callee's body), so `return`
  finding its `Call` frame can't resume by jumping to an `Ip`, the way
  `break`/`continue` do — there is no meaningful offset to jump to in a
  *different* array. `raise()`'s return type had to split into
  `RaiseOutcome::{Jump(Ip), ReturnFromCall}`: `Jump` resumes the current
  `Program` (the `Loop`/`Iter` case), `ReturnFromCall` instead unwinds the
  *entire current recursive `run()` invocation* back out to whichever
  `call_function` call started it — real Rust call-stack recursion, not
  an instruction-pointer jump. `Call` frames are correspondingly pushed
  and popped by `call_function` itself, not by any compiled `Instr` the
  way `PushLoopFrame`/`PopLoopFrame` are — a function's boundary is
  `call_function`'s own Rust stack frame, not an offset in its callee.
- **This is also *why* `break`/`continue` crossing a `Call` frame must be
  a hard error, not just a nicety worth adding for parity.** If `raise()`
  let `Break` pop through a `Call` frame to reach an outer `Loop` frame
  belonging to the *caller's* `Program`, the resulting `Jump(break_target)`
  would be applied to the *callee's* currently-executing `Program` — an
  offset meaningful only in a different, unrelated instruction array.
  This is silent memory-safety-adjacent corruption of control flow, not
  "jumps to a slightly wrong place": confirmed both by re-reading
  `except.c`'s `rc_raise` (its nesting-rule table already disallows
  `eBreak`/`eContinue` from passing an `eReturn` frame) and empirically
  against the real `rc` binary (`fn b { break }; for (i in (1 2)) { b }`
  → `rc: line 0: break outside of loop`) before implementing the check,
  and covered by a test that fails loudly if the check is ever removed.
- **`$1`/`$2`/... are not variables literally named `"1"`/`"2"`.**
  `var.c`'s `varlookup()` treats any purely-numeric name as a 1-based
  index into `$*` — a runtime-lookup-time special case, not a grammar or
  lexer distinction (`$1` parses through the exact same `'$' sword`
  production as `$x`). Missed on the first pass (a real function-argument
  test caught it: `fn greet { echo hi $1 }; greet world` produced `hi`
  with no `world`), fixed, and verified against the real binary
  (`echo $1 $2 $3` with `one two three` as arguments) rather than assumed
  from reading `var.c` alone, since the loop structure there is easy to
  mis-trace by eye (whether it's 0- or 1-indexed isn't obvious from the
  code's own shape without running it).
- **Run tests using `cargo nextest run`.**
  1. File Descriptors are Process-Wide (Not Thread-Local)
  Standard cargo test runs all unit tests in parallel using multiple threads within a single OS process.
  In Unix/Linux, file descriptor 1 (stdout) is shared by every thread in the process.
  2. In-Place dup2(file_fd, 1) Redirection
  When append_redirect_appends() ran /bin/echo one > {path}, rc-rs opened {path} and called:
  ```c
  dup2(file_fd, 1); // Point process-wide stdout (fd 1) to {path}
  ```
  `cargo nextest run` runs each unit test in a separate process with the debug executable.
  `cargo nextest run --release` runs each unit test in a separate process with the release executable.

Empirical verification against the real `rc` binary (available locally)
has been a running theme worth calling out as a general practice, not
just a one-off for this section: several of the behaviors above (`lmatch`
cross-product semantics, `switch`'s no-fallthrough shape, `return`'s
status-setting, `$1`'s indexing) were confirmed by *running* real rc
scripts before committing to an implementation, not solely by reading the
C source — the source and a plausible reading of it are not the same
thing, and this session repeatedly found the gap between them.

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

### 4.1 Concrete resolution: transient `Ast`, source-backed functions

- **A real `Ast` enum exists, but only as compiler input — nothing stores
  it past one `compile(&Ast) -> Vec<Instr>` call.** Grammar actions (§1)
  build `Ast` nodes (owned, `Box`ed, ordinary Rust — no arena needed for
  this, since it's freed the moment compilation finishes); a separate
  `compile` pass walks it and emits `Instr`s from §3.1, doing the jump
  backpatching structured control flow needs (straightforward
  recursive-descent codegen — no forward-declared cross-function labels
  are needed, since `if`/`while`/`for`/`&&`/`||`/`switch` are all
  structured constructs where the jump targets are known as soon as the
  sub-`Ast` they bracket has finished compiling). Building the `Ast`
  directly inside lrpar's per-production action code (rather than through
  an intermediate/decoupled pass) is fine — it's a normal typed value, not
  the C `Node` union — but doing backpatching-style codegen would be
  awkward inside individual grammar actions, so `compile` is kept as its
  own pass over a finished `Ast` rather than fused into parsing.
- **Every `Ast` node built for a `fn` definition carries a source-text
  span** (byte offsets into the original definition text, captured while
  lexing/parsing). This is what makes "pretty-print = print the source"
  concrete rather than aspirational: `whatis`/`%T`-equivalent output for a
  function is a direct slice of stored source text, not a
  decompiled-from-`Ast` reconstruction — which is also the only way to
  honestly satisfy §13's "`whatis` output must be re-sourceable" bar,
  since reformatting from a structured representation risks not
  round-tripping through quoting/whitespace edge cases that the original
  text trivially preserves.
- **`rc_Function` becomes source-backed and lazily compiled**, directly
  mirroring the existing C laziness pattern (`fnlookup`'s
  `parse_fn(look->extdef)` + `treecpy` on first use) rather than
  inventing a new one:
  ```rust
  struct RcFunction {
      source: Rc<str>,                    // the `{ ... }` body text, for whatis/env export
      compiled: OnceCell<Rc<[Instr]>>,     // lazily compiled on first call; invalidated on redefinition
  }
  ```
  A function imported from the environment (`fn_name=...`) just sets
  `source` and leaves `compiled` empty until first invocation — the same
  "don't pay parse cost for functions that are never called" property
  `fnlookup_string`/`fnassign_string` already have in C. Redefining a
  function (`fn name {...}` again) replaces the whole `RcFunction`, so
  there's no cache-invalidation subtlety beyond "assignment replaces the
  value," matching `fnassign`'s current behavior of building a fresh
  `rc_Function` unconditionally.
- Top-level script statements (not inside a `fn`) don't need source spans
  retained at all past the single `doit()`-equivalent iteration that
  parses, compiles, and runs them (`-n`/`-x`'s "print each command as it's
  parsed" flags, §"OPTIONS" `-n`, are the one exception — those need
  *some* printable form at parse time, but only transiently, immediately
  after parsing that one statement, not stored for later).

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

**Decision: threaded `Shell` struct.** Rough shape, mapping each C global
inventoried above to a field:

```rust
struct Shell {
    arena: Arena,           // nalloc.c: ul/fl block lists
    vars: VarTable,         // hash.c: vp (+ cdpath/CDPATH-style alias sync)
    funcs: FnTable,         // hash.c: fp
    input: InputStack,      // input.c: istack/itop/inbuf/chars_in/out/lineno
    frames: Vec<Frame>,     // replaces except.c's estack — see §3
    jobs: JobTable,         // wait.c: plist
    signal_handlers: SignalHandlerTable, // fn.c: handlers[]/runexit (NOT signal.c's OS-level state, see below)
    options: ShellOptions,  // main.c: dashdee/dashee/.../interactive/rc_pid/rc_ppid
    exec: ExecState,        // redirq, cond, $status, lineno-adjacent exec-time state
}
```

Interpreter functions become methods on `Shell` (or free functions taking
`&mut Shell` as the first argument) — e.g. `fn varlookup(&self, name: &str)
-> Option<&List>`, `fn nalloc(&mut self, ...) -> ArenaHandle`, `fn
run(&mut self, code: &[Instr]) -> Result<Value, RcSignal>` for the VM loop
from §3. One `Shell` value is created in `main`, and everything — parser
actions, builtins, the VM loop, `fn`/`whatis` pretty-printing — operates
through `&Shell`/`&mut Shell`, replacing C's implicit "reach into whatever
global you need."

Two things worth flagging now, before any code is written, because they're
the likely friction points with this design:

- **The arena can't hand out ordinary Rust references stored across `Shell`
  method calls.** If `VarTable`/`FnTable` values (`List`s built via
  `nalloc`) are represented as `&'arena [Word]` borrowed from
  `Shell.arena`, then any method that needs `&mut self.arena` (to allocate
  more) while a caller still holds a `&List` borrowed from an earlier
  lookup will not compile — this is exactly the shape of borrow-checker
  fight that "just thread a struct through" doesn't automatically avoid.
  The practical fix is to *not* store borrowed arena references as the
  value type in `VarTable`/`FnTable`/anywhere long-lived: use owned,
  cheaply-cloneable values instead (`Rc<[Rc<str>]>` or a small
  `Vec<Rc<str>>` per `List`, or an index/handle into a `slotmap`-style
  arena rather than a lifetime-carrying reference). This does give up some
  of the original allocator's raw speed advantage, but it sidesteps
  fighting the borrow checker over arena-lifetime references threaded
  through a mutable struct — a straight `&'arena` reference design and a
  `&mut Shell`-per-call design are in tension with each other, and the
  owned/`Rc`-based value representation is the way to have both.
- **Not everything can move into `Shell`.** `signal.c`'s `sigcount`/
  `caught[]` (`static volatile sig_atomic_t`) are touched directly by the
  real OS signal handler (`catcher()`), which cannot safely dereference
  into heap-allocated, non-`'static`, non-atomic application state — this
  is a hard constraint of async-signal-safety, not a Rust-specific
  limitation (the same is true in C; it's *why* the C code keeps these as
  bare file-scope statics rather than, say, fields of some context struct
  passed around). These stay as genuine Rust `static` atomics (`static
  SIGCOUNT: AtomicUsize`, `static CAUGHT: [AtomicBool; NUMOFSIGNALS]`)
  outside `Shell`, and `Shell::sigchk(&mut self)` reads them and does the
  real (non-signal-context) dispatch — mirroring the split the C code
  already has between `catcher()` (signal-context, statics only) and
  `sigchk()` (normal context, touches everything else) rather than trying
  to unify all state into one struct.

## 7. Process management and signal-handler-driven interpreter re-entrancy

**Correction: rc has no interactive job control** (no `fg`/`bg`/`jobs`
builtins anywhere in `builtins.c` — confirmed by grep, not just absence
from `rc.1`'s builtin list). What `RC_JOB` (`config.def.h`) actually gates
is narrower: putting a *background* (`&`) child into its own process
group and having it ignore `SIGTTOU`/`SIGTTIN`/`SIGTSTP`, purely so a
background job doesn't get stopped by the terminal the way a naive
backgrounded child could — process-group hygiene for one specific case,
not a job-control subsystem with suspend/resume semantics. `newpgrp`
(`rc.1`) is a related but separate, coarser tool: it puts the *whole
shell* in a new process group, for a specific job-control-hostile-terminal
workaround (the NeXT Terminal case `rc.1` mentions), not per-job tracking.
Worth having gotten this right up front, since "job control" is a
misleading label for what this section actually needs to port — it's
smaller in scope than that name implies.

- `wait.c`/`exec.c`/`redir.c` do fork/exec/dup2/waitpid process management,
  which maps onto Rust fairly directly via `nix`/raw libc (there's no
  portable safe Rust process-group API, so this stays `unsafe` either way
  — not a blocker, just a note that this isn't a "safe Rust" win).
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
`setpgrp()`...).

**Target platform decision: macOS, Ubuntu, and CentOS (i.e. modern
Linux/glibc plus macOS/Darwin) — not the full historical Unix portability
spectrum the C implementation supports.** This resolves the
prune-and-decide call this section used to leave open into an actual
answer: nearly every one of these ~30 macros exists to handle a Unix
variant or vintage outside that target list (SysV-vs-BSD signal
semantics, systems without `/dev/fd`, non-POSIX `getgroups`/`setpgrp`,
`quad_t`-only platforms, non-restartable syscalls). Concretely, given the
target list:
- `HAVE_SIGACTION`, `HAVE_DEV_FD`, `HAVE_POSIX_GETGROUPS`, `HAVE_SETRLIMIT`,
  `SETPGRP_VOID`-style POSIX behavior: always true on all three targets —
  don't gate these behind a `cfg` at all, just assume POSIX.
- `HAVE_SYSV_SIGCLD` and the whole BSD-vs-SysV signal-semantics split: SysV
  semantics don't apply to any of the three targets — drop entirely,
  including the `fn.c` "can't trap SIGCHLD on SysV" carve-out §13 flagged
  as needing a decision (decided: drop it, don't replicate it).
- `HAVE_RESTARTABLE_SYSCALLS`/`slowbuf`: modern Linux and macOS both
  restart syscalls by default (`SA_RESTART` semantics) — the whole
  `rc_read`/`rc_wait`/`slowbuf` portability shim (already flagged in §3 as
  needing a signal-safety redesign regardless) can drop the "syscalls
  don't restart" branch entirely, not just redesign it.
- `RLIM_T_IS_QUAD_T`/`HAVE_QUAD_T`: irrelevant on any modern 64-bit target
  — `rlim_t` is a real type everywhere that matters now.
- The one real remaining split: macOS/Darwin vs. Linux/glibc differences
  that *do* still exist today (e.g. some `getrlimit`/resource-constant
  values, `/proc/self/fd` existing on Linux but not macOS — macOS has
  `/dev/fd` though, so §1.2's `<{cmd}` tiering still needs at least a
  Linux/macOS `cfg`, just not the historical three-tier
  `/dev/fd`/`/proc/self/fd`/named-pipe fallback the C code supports for
  older systems — two tiers, or even just one (`/dev/fd`, present on both
  targets), may suffice).

Net effect: obstacle #10 shrinks substantially under this target list —
most of these macros are just gone, not translated, and the ones that
remain are ordinary `#[cfg(target_os = "...")]` splits between exactly two
OS families, not an open-ended portability matrix.

**Priority ordering, not a scope limit:** the goal is `trip.rc` (§12)
passing on macOS, Ubuntu, and CentOS *first* — this target-platform
decision exists to unblock that goal quickly by not spending effort on
portability the acceptance criterion doesn't need yet, not to declare
that broader portability will never matter. If it matters later, revisit
then; don't let "we're not chasing the full historical Unix matrix right
now" drift into "portability beyond these three is out of scope
permanently" — that's a separate decision nobody's made.

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

**Update: `rc-rs` now has a minimal CLI driver (previously none existed
at all -- `fn main()` just printed `"Hello, world!"`), `trip.rc` is
copied into `rc-rs` (`./trip.rc`), and running it surfaced the next real
blocker directly rather than hypothetically.** It fails to parse at line
~410 -- the first heredoc (`` <<eof ``). Heredoc/herestring body
collection was already a known, explicitly-flagged gap (§1's lexer scope
note, repeated at execution time in `shell.rs`'s `open_redirect`), but
this pins it down as the *first* one `trip.rc` actually hits, ahead of
`calc`'s arithmetic actions (`trip.rc` uses `calc` starting around line
17, textually earlier, but that code never gets a chance to run) and
`$0`/`$version` (also referenced early). Worth calling out why those
earlier-in-the-file features don't matter yet even though they appear
first: this port's `rc` grammar rule (§1.2) was made left-recursive
specifically so one `lrpar` `parse()` call consumes an entire script at
once (lrpar has no `YYACCEPT` for real rc's own per-statement,
interactive-loop-driven parsing). That means **a single unimplemented
construct anywhere in the file blocks parsing the *entire* file** --
there's no partial credit the way real rc's interactive loop would give
by executing everything up to the unsupported line before failing.
That's a real, practical difference specifically for `trip.rc`-style
whole-file acceptance testing (as opposed to hand-written unit tests,
which only ever exercise one construct at a time and never hit this): it
means the *textually first* unimplemented construct in the file is what
gates all `trip.rc` progress, not necessarily the *most commonly used*
one or the one highest on this document's own priority list. Two ways to
address this, neither done yet: implement enough constructs to get a full
parse (heredocs, then whatever's next), or change the driver to parse and
run one top-level statement at a time (matching real rc's own model),
so a script makes partial progress up to wherever it actually breaks --
the latter is arguably the more valuable fix, since it would make every
future `trip.rc` run informative instead of all-or-nothing, but wasn't
attempted this session (it's a real restructuring of the parse driver
built specifically around the single-`parse()`-call decision above, not
a small change).

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

**Deliberate divergence decision (not a gap — a considered improvement):
redirected builtins should not fork in the Rust port.** `exec.c` forks
before running a *redirected* builtin (`b != NULL && redirq != NULL`)
purely to keep the redirection's fd-table changes from leaking into the
parent shell once the command finishes — forking is C's *mechanism* for
fd isolation here, not a deliberate statement that control flow should
stop working. Its side effect, though, is real and tested:
`trip.rc:518`'s `for(i in 1 2){echo $i;break >/dev/null}` gets
`"break outside of loop"` specifically *because* the fork's `clearflow()`
strips the `break`/`continue`/`return` frames before the redirected
`break` ever runs (§3.2 traces this exactly). In the Rust VM, a
redirection is just an `Instr::Redirect` manipulating an fd table — the
same "don't leak the fd change" isolation is achievable by saving the
target fd, applying the redirect, running the builtin, and restoring the
saved fd, all *without* forking, since there's no process boundary
involved in the first place. That means `break`/`continue`/`return`
inside a redirected builtin would correctly reach the frame stack instead
of being severed by a fork that only ever existed to protect an unrelated
fd-table concern. **Adopting this is a deliberate, intentional behavioral
divergence from the C implementation** — worth being explicit about
precisely because `trip.rc` is the stated acceptance bar (§12):
`trip.rc:518`'s specific assertion would need to be treated as a
documented, deliberately-superseded test case when this VM reaches
redirection support, not chased as a silent regression to "fix" back to
matching C's fork-induced error. Record the decision here now, ahead of
implementing `Instr::Redirect` for real, so it isn't rediscovered as a
surprising trip.rc mismatch later without the reasoning behind it.

**Update: implemented, and the headline scenario is confirmed working
end-to-end, not just designed.** `rc-rs` now applies redirections via
save/`dup2`/restore around each command (`PushRedirScope`, matching
`redir.c`'s `rc_open`/`mvfd` for the actual open+dup2, plus a save/restore
step `redir.c` has no equivalent of, since C redirections normally run in
a predictable freshly-forked child rather than in-place in a long-lived
process). `for (i in (1 2)) {echo $i; break >/dev/null}` now prints `1`
and stops, exactly the divergence from `trip.rc:518` predicted above,
verified against the real `rc` binary for both implementations before
trusting either result.

Getting there surfaced the **same normal-path-vs-raised-path bug class
found for `continue`/`PopIterFrame` (§3.2) a second time, in a new
place** — worth treating as confirmation this is a recurring hazard of
this VM design generally, not a one-off: the first implementation
restored a command's redirects via a separate `PopRedirScope` instruction
placed *after* `Exec` in the compiled stream. `break`/`continue`/`return`
raised from inside that `Exec` jump straight to their target, skipping
every instruction sequentially after `Exec` — including `PopRedirScope`,
leaking the redirect permanently (`break >/dev/null` left fd 1 pointed at
`/dev/null` for the rest of the process, confirmed by a corrupted test
run, not by review). The fix generalizes the same principle §3.2 already
stated: **anything that must clean up after an operation, where that
operation might trigger a signal, must do its own cleanup unconditionally
inside that operation's own instruction handler — never rely on a
separate, sequentially-later instruction that a signal's jump can skip
past.** `Exec` now restores its own redirect scope directly, every time,
before deciding whether to propagate a signal outward. Two data points
now support this as a general rule for every future frame/scope kind
(`Arena`, `VarStack`, `Fd`, `Fifo`), not just the two already found it in.

**A second, unrelated lesson from the same implementation pass, worth
keeping for whoever writes VM tests going forward:** `cargo test`'s
default multi-threaded runner puts every test in one process sharing one
real fd table — `Redirect`/fork-based tests running concurrently reliably
corrupted each other's fd 1 mid-redirect, since OS-level fd state is
process-wide, not per-Rust-thread. Fixed with one shared
`std::sync::Mutex` every fd/fork-touching test acquires first,
serializing that portion of the suite. Any future test exercising real
process/fd state needs to go through this lock too — it's easy to forget
since most of this VM's tests (parsing, compiling, pure in-memory
execution) have no such constraint, and the failure mode (an
intermittent, hard-to-reproduce corrupted test) doesn't look like an
obvious "you forgot to synchronize" symptom at first glance.

**Update: the shared-lock fix above narrows but doesn't eliminate this
class of flakiness.** Reproduced independently of `Ast::Pre`: even on the
commit *before* `Ast::Pre` existed, a default multi-threaded `cargo test`
run failed intermittently (~1 in 8 runs observed) with real `rc`-shell
stdout output showing up inside a test's redirected-file assertion. The
shared `Mutex` only serializes *fd-mutating* tests against each other; it
does nothing to stop the test harness's own `println!`-based progress
output — printed from other, non-locked worker threads — from landing in
whichever file fd 1 happens to be redirected to at that instant. Confirmed
this is a pre-existing gap, not something `Ast::Pre` introduced: `--test-
threads=1` runs are consistently clean (5/5 observed). Not fixed yet —
would need either running fd-touching tests under a forced single thread
(a `#[test]`-level annotation Rust doesn't have natively) or redirecting a
fd the harness itself doesn't write progress through. Worth fixing before
leaning harder on real-fd-redirection tests (`Nmpipe`, heredocs will add
more of them), but out of scope for the `Ast::Pre` milestone itself.

**Extending the postfix-redirect no-fork divergence to prefix redirects
too (`Ast::Pre`'s `Ast::Redir`/`Ast::Dup` branch, `walk.c`'s `nPre`
case).** Confirmed against the real `rc` binary that C's behavior here is
actually *more* fork-happy than the postfix case, not just the same: a
redir/dup *prefix* (`>file cmd`) always forks in `walk.c`, unconditionally
— unlike the assign-prefix branch of the same `nPre` case, which checks
`isallpre()` and only ever uses a real `eVarstack` exception frame (no
fork at all). Empirically, this means `break`/`continue` inside a
`>file cmd`-prefixed command inside a loop reports `"break outside of
loop"` in real `rc`, for the exact same reason as `trip.rc:518`'s postfix
case: the fork's `clearflow()`-equivalent strips the loop frame before the
prefixed `break` runs. `rc-rs` applies the same reasoning as the postfix
divergence uniformly here: a new `Frame::Redir` (parallel to
`Frame::VarStack` for the assign-prefix case) saves/restores the fd
without forking, so `break`/`continue`/`return` inside a redir/dup-
prefixed command now correctly reach the outer loop/function frame — a
second, deliberate, documented divergence from `trip.rc`-observable C
behavior, verified end-to-end (`for (i in 1 2 3) { >/dev/null break; ...}`
now actually breaks the loop, where real `rc` prints the error three times
and never does). `Frame::Redir`/`Frame::VarStack` are both unconditionally
transparent to every signal in `raise()` (matching `eVarstack`/`eFd`'s
always-passable status in `except.c`'s nesting rules), which is also what
makes the assign-prefix scoping case (`a=foo cmd`) work correctly through
a `return` unwind without any special-casing beyond the frame-stack walk
already built for `Break`/`Continue`/`Call`.

**Backquote substitution implemented; two pre-existing `$status` gaps
found and fixed along the way, both confirmed against the real binary
before changing anything.** `Instr::Backq` forks, captures the child's
stdout through a real pipe instead of inheriting fds (`glom.c`'s
`backq`), and splits on `$ifs` via a direct port of `bqinput`'s
collapsing-separator-runs FSA. Writing real tests for it (rather than
just structural ones) exposed two things that had nothing to do with
backquotes specifically, just hadn't been exercised yet:

1. `$status` wasn't wired to anything — `var_lookup` only ever consulted
   `self.vars`, so `$status` silently read as empty. This didn't matter
   until something needed to *read back* `$status` as a variable rather
   than just observe `Shell::status` directly from test code — a
   backquote followed by `echo $bqstatus status=$status` was the first
   real case. Fixed by special-casing `"status"` in `var_lookup` to
   return `self.status` live, the same way `$1`/`$2`-style numeric
   shorthand is already special-cased there. (Still not `$status`'s real
   shape — real rc's `$status` is a *list*, one element per pipeline
   stage, `status.c` — that stays an open gap, just no longer a total one.)
2. `Exec`'s empty-`av` path (reached when a value-position construct
   evaluates to zero words, e.g. an unset variable or — now — a
   backquote that captured no output, used as a bare command) was
   setting `$status = 0`, on the theory that an empty command is
   "vacuously true" like `walk()`'s `n == NULL` case. That theory
   conflated two different things: `walk()`'s `n == NULL` (a literally
   *absent* `cmd` node, e.g. an empty `else`) really does `set(TRUE)`,
   but that's `compile_cmd`'s `Ast::Empty` arm here, which already
   correctly no-ops without touching `$status` at all — it never reaches
   `exec`. A *non-null* node that merely *evaluates* to an empty word
   list is a different code path in `exec.c` entirely (`*av == NULL`
   inside `exec()` itself), and it does **not** reset status — confirmed
   against the real binary (`false; $nosuchvar; echo $status` prints
   `1`, not `0`; real rc gets this by forking anyway just to still apply
   any queued redirect, then exiting with whatever `getstatus()` already
   was). Fixed by leaving `self.status` untouched in this VM's
   equivalent path — no fork needed here either, for the same reason
   postfix/prefix redirects don't need one.

**Test-infrastructure hardening, prompted by a real hang mid-session, not
by a language-semantics question.** A test genuinely wedged the whole
suite: every fd-touching test reported "running for over 60 seconds"
simultaneously, which is the signature of one test holding
`PROCESS_TEST_LOCK` and never releasing it (a real bug — a pipe read
that never saw EOF, in code that turned out to still be mid-edit at the
time) rather than N independently slow tests. A plain `.lock()` has no
way to distinguish "the lock-holder is just doing real, slightly slow
I/O" from "the lock-holder is permanently stuck" — both look identical
from every other thread's perspective, and cargo's own progress output
can't tell you which specific test is the one actually holding the lock
versus the ones merely queued behind it. Replaced with a bounded
acquire (`crate::process_test_lock` in `main.rs`, `PROCESS_TEST_LOCK`
itself unchanged): a 10-second `try_lock` polling loop that panics with
an explicit "still held after 10s" message instead of blocking forever.
This doesn't fix a hang — it turns an indefinite, hard-to-attribute wedge
of the entire suite into one clearly-labeled failing test, which is the
actual, durable improvement. The real fix — giving every test its own
process (`cargo-nextest`) so there's no shared fd table to protect in the
first place, which would let this whole `PROCESS_TEST_LOCK` mechanism be
deleted outright — isn't set up in this environment; worth adopting
before `Nmpipe`/heredocs add more real-fd tests to the pile.

## Current Rust implementation notes

The shell VM dispatches builtins after checking for a user-defined
function of the same name, preserving rc's function-over-builtin
precedence. Implemented builtins include `break`, `continue`, `return`,
`true`, `false`, `echo`, `cd`, `shift`, `umask`, `whatis`, and `exit`. `exit` is
represented as shell state so it unwinds nested function execution and
stops the top-level input loop without terminating the process from inside
the VM; this also keeps the VM usable in tests and embedding code.

The C variable table confirms three environment alias pairs:
`home`/`HOME` share the same list value; `path`/`PATH` and
`cdpath`/`CDPATH` convert between rc lists and colon-separated strings.
The Rust implementation currently imports `PATH` into `path` for command
lookup and lets `cd` consult `home`/`HOME` and `cdpath`. Full bidirectional
alias synchronization on assignments and export of updated values to
child processes remains a variable-table task, as described in §13.

`whatis` currently prints variable definitions, builtin names, and
resolved executable paths. Full parity remains open: compiled functions
do not retain their source text, and signal handlers are not modeled, so
function/signal output cannot yet meet rc's re-sourceability guarantee.
Next builtin additions should prioritize `eval`, `.`, and `exec`, with
behavior checked against C rc. `eval` must preserve control-flow signals
such as `return`; it is not safe to approximate as an ordinary nested
program run without explicit signal propagation.

**Interactive SIGINT must interrupt the current input/command without
terminating the shell.** The C implementation's `sigint()` prints a
newline when appropriate, clears pending redirects and conditional state,
then raises `eError`; its interactive exception frame catches that error
and returns to the prompt (`except.c`, `input.c`). The Rust driver
currently installs no SIGINT handler, so the default OS action terminates
it. Add a deferred, signal-safe flag and check it at safe parser/VM
boundaries; do not perform allocation, formatting, or VM unwinding inside
the OS signal handler. Also restore default signal dispositions in forked
children before running external programs, as `setsigdefaults()` does.

## Summary table

| # | Obstacle | Novel vs. already-known (1-3)? | Recommended direction |
|---|----------|-------------------------------|------------------------|
| 1 | Dual yacc grammars + stateful lexer/heredoc coupling | Expands #1 | `grmtools`/`lrpar`, confirmed via real 0.15.0 build: both grammars transcribe with zero conflicts; drop the `error`-token production (no lrpar equivalent) and rework `YYABORT` sites as `Result`-sentinel propagation |
| 2 | Nested/scoped arena tied to unwind, dual arena+permanent alloc | Expands #2 | Arena checkpoints become one `Frame` variant in the §3 VM frame stack; permanent storage is the compiled-function cache, not the arena |
| 3 | setjmp/longjmp control flow: selective error/break/continue/return unwinding with per-frame cleanup | Expands #3 | Compile loop/branch structure itself to static `Jump`/`JumpIfFalse` (§3.1); keep `break`/`continue`/`return` as *dynamic signals* dispatched like any other command (they're shadowable builtins, not grammar keywords — confirmed via `rc.1`'s grammar + `exec.c`'s fn-before-builtin lookup + `trip.rc:517`/`:518`), walked through an explicit `Vec<Frame>` mirroring `Estack`'s nesting rules; forked children must strip Loop/Iter/Call frames (~`clearflow()`) |
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

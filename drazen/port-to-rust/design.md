# Porting rc to Rust: Obstacle Analysis and Design Notes

Status: draft analysis, not an implementation plan. Goal is to catalog every
structural obstacle to a Rust port, expand on the three already identified in
`obstacles.txt`, and sketch viable Rust-idiomatic replacements for each.

Codebase snapshot: ~14.3k lines of C across ~50 `.c`/`.h` files, two yacc
grammars (`parse.y` for rc syntax, `calc.y` for `$((...))` arithmetic), a
custom small-object arena allocator, a custom varargs formatting engine, and
setjmp/longjmp-based control flow for break/continue/return/error unwinding.

**Repository layout decision:** the Rust port will live in its own sibling
repository, `rc-rs`, alongside this repo (i.e. `../rc-rs` relative to this
repo's root) rather than as a subdirectory of this C codebase — a
from-scratch rewrite with its own git history, not a fork. Not yet
scaffolded; this is a decision recorded ahead of implementation.

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
  with no lexical representation); grmtools does not, by default. Workaround
  confirmed: `CTParserBuilder::warnings_are_errors(false)` downgrades it to
  a harmless warning and the build succeeds — a one-line builder-config
  fix, not a grammar change, but worth recording explicitly so it isn't
  rediscovered as a mystery build failure during real implementation. (If
  `warnings_are_errors(true)` needs to stay on for other diagnostics, the
  alternative is threading the fictitious token through one otherwise-dead
  production instead.)

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
    PushIterFrame,             // Frame::Iter; ~ except(eContinue, ...) + a fresh Frame::Arena per iteration
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

# Interpreter execution-state API for stackful coroutines

Definition and rationale for a supported core API that creates, swaps and tears
down a *green-thread execution state* — the mutable per-thread interpreter state
(the stacks and the execution position) that a stackful coroutine library must
switch between. First consumer: Coro. Companion to the savestack suspend/resume
API (`Porting/savestack_suspend_api.md`): that one *serializes* part of the
state for the stackless model (Future::AsyncAwait); this one *swaps* the whole
state for the stackful model (Coro).

## Rationale

Coro implements green threads by giving each coroutine its own set of interpreter
stacks and its own C stack, and swapping both on a context switch. The C-stack
swap is architecture code (libcoro / the hand-written asm backend) and is no
business of perl core. The *interpreter*-stack swap, however, requires Coro to
know the exact set of mutable execution registers the interpreter keeps — and it
hard-codes that list in `Coro/state.h`: **56 `VAR(...)` entries**, festooned with
`#if PERL_VERSION_ATLEAST` guards (retstack removed in 5.10, `scopestack_name`
gated on DEBUGGING, `sortcxix` gone, the `localizing`/`in_eval` width change, …).

That duplicated list is the dependency. Whenever core adds, removes, renames or
reorders an interpreter execution register, Coro's `state.h` must track it or
break — and getting it subtly wrong corrupts memory rather than failing cleanly.
Two further pieces reach into internals the same way:

- **Setup** (`Coro/State.xs:coro_setup` → `init_perl`): allocating a *fresh*,
  correctly-initialised set of stacks for a new coroutine. This re-implements,
  by hand, what `Perl_init_stacks()` does for the main interpreter.
- **Teardown** (`Coro/State.xs:coro_unwind_stacks`): the delicate
  `dounwind(-1)` → `LEAVE_SCOPE(0)` → `FREETMPS` *ordering* needed to unwind a
  dying coroutine's dynamic scope. This is version-sensitive and semantically
  subtle: doing `LEAVE_SCOPE(0)` before `dounwind` processes inner-frame save
  entries (`SAVEt_CLEARSV`, a `local $h{k}` `SAVEt_DELETE`) while the pad is at
  the wrong depth, leaving an outer pad slot `PADSTALE` and asserting/segfaulting
  inside `Perl_leave_scope` — the recently-fixed `->safe_cancel`-of-a-blocked-
  thread corruption. Coro has had to fix this class of bug more than once.

Moving this into core lets core evolve the interpreter freely, gives every
green-thread library (Coro today; others later) one correct implementation, and
puts the delicate unwind-ordering knowledge in the one place that owns
`leave_scope`.

## What the execution state is

The per-thread mutable execution state — grounded in `Coro/state.h` and in what
`Perl_init_stacks()` (perl.c) already allocates as one unit:

- **The value stack + context stack**, as the `PERL_SI` chain:
  `PL_curstackinfo` (whose `si_stack` is the value stack `AV` and whose
  `si_cxstack`/`si_cxix` are the context/`cx` stack), `PL_curstack`,
  `PL_mainstack`, `PL_stack_base`, `PL_stack_sp`, `PL_stack_max`.
- **The mark stack**: `PL_markstack`, `PL_markstack_ptr`, `PL_markstack_max`.
- **The scope stack**: `PL_scopestack`, `PL_scopestack_ix`, `PL_scopestack_max`,
  `PL_scopestack_name` (DEBUGGING).
- **The save stack**: `PL_savestack`, `PL_savestack_ix`, `PL_savestack_max`.
- **The temporaries stack**: `PL_tmps_stack`, `PL_tmps_ix`, `PL_tmps_floor`,
  `PL_tmps_max`.
- **Execution position**: `PL_op`, `PL_comppad`, `PL_curpad`, `PL_curcop`,
  `PL_curpm`, `PL_sortcop`/`PL_sortstash`.
- **Execution flags**: `PL_localizing`, `PL_in_eval`, `PL_tainted`.

Core already owns this set: it is precisely what `Perl_init_stacks()` sets up and
what `Perl_nuke_stacks()` frees. The API's job is to let that set be created,
snapshotted, installed and destroyed as a *unit that is not necessarily the live
one* — so the field list lives once, in core, instead of being transcribed into
`state.h`.

Explicitly **out** of the execution state (interpreter-global, must NOT be
swapped): the SV heap and pad names, `%ENV`/stashes, `PL_sig_pending` and async
signal state, `PL_runops`, allocator state. Coro does not swap these today.

## Public API

The state is carried by a `PerlExecState`. It is **transparent**, not opaque: a
consumer such as Coro overlays it on its own memory, and the single register
list lives in `execstate.h` as one X-macro from which the struct is generated.
The lifecycle acts on the *live* interpreter rather than returning a handle.

The surface is a **capability ladder**, so core can implement as much of the
mechanism as it wants and a consumer backfills any level below that, testing
`PERL_EXECSTATE_LEVEL` (the legacy boolean `PERL_EXECSTATE` counts as level 1).
Core currently ships all four levels (`execstate.h`: `PERL_EXECSTATE_LEVEL 4`).

```c
typedef struct PerlExecState PerlExecState;   /* transparent; see execstate.h */

/* level 1 — register snapshot */
void      execstate_save(pTHX_ PerlExecState *into);
void      execstate_load(pTHX_ PerlExecState *from);

/* level 2 — fresh-stack lifecycle */
void      execstate_init(pTHX_ int cxextra);
void      execstate_unwind(pTHX);
void      execstate_destroy(pTHX);

/* level 3 — pad */
PADLIST  *execstate_derive_padlist(pTHX_ CV *cv);
void      execstate_free_padlist(pTHX_ PADLIST *padlist);

/* level 4 — transfer (JMPENV) registers: lvalue aliases + helper */
JMPENV   *execstate_topenv_root(pTHX);
#define execstate_topenv          PL_top_env
#define execstate_restartop       PL_restartop
#define execstate_topenv_reset()  (PL_top_env = &PL_start_env)
```

- **level 1 — register snapshot.** `execstate_save` copies the live `PL_*`
  execution registers into `into` (pointer/cursor copy; no allocation);
  `execstate_load` installs `from`'s registers into the live `PL_*`.
  `save(a); load(b)` is a context switch; the caller does its own C-stack switch
  between them.
- **level 2 — fresh-stack lifecycle.** `execstate_init(cxextra)` sets up a fresh
  set of stacks as the live ones — a new `PERL_SI` with an empty value+cx stack
  and empty mark/scope/save/tmps stacks, with the same initial invariants
  `Perl_init_stacks()` establishes (floors set, an initial `ENTER`,
  `PL_tmps_floor == -1`), reserving `cxextra` context-stack slots for the
  consumer's overlay; this is what Coro's `coro_setup` did by hand.
  `execstate_unwind()` runs the correct `dounwind` → `LEAVE_SCOPE` → `FREETMPS`
  teardown for a state that will never run again (the ordering
  `coro_unwind_stacks` had to get right); `execstate_destroy()` frees the stacks.
- **level 3 — pad.** `execstate_derive_padlist(cv)` /
  `execstate_free_padlist(pl)` for re-entering a sub on an independent context.
- **level 4 — transfer registers.** The `JMPENV` interpreter state, exposed as
  the lvalue aliases `execstate_topenv` / `execstate_restartop` plus
  `execstate_topenv_reset()` and `execstate_topenv_root()` (which walks to the
  base handler). The machine-level C-stack switch (`coro_transfer` and the cctx
  C stacks) is **not** part of the API — it is a pluggable, platform-specific
  mechanism and stays the consumer's own.

SV enumeration for GC/introspection is deliberately not a core entry: a suspended
state's SVs (value-stack temporaries, tmps, save-stack-retained SVs, the
localized pad) stay reachable through the consumer's overlay, which is where Coro
does its global-destruction marking and Devel::MAT integration.

## Division of labour

**Core owns** (moves out of Coro):
- the execution-state field list (above) — via save/load and the shared
  `execstate.h` X-macro that also feeds `init_stacks`;
- fresh-stack initialisation (`execstate_init`, generalising `init_stacks`);
- correct teardown ordering (`execstate_unwind`/`execstate_destroy`, folding in
  the `dounwind` → `LEAVE_SCOPE` → `FREETMPS` sequence);
- pad derivation for re-entering a sub on an independent context.

**Coro keeps** (genuinely Coro's, not internals):
- the **C-stack switch** (`cctx` / libcoro / the arm64 asm backend) — machine
  code, unrelated to perl internals;
- the **policy set of per-thread dynamic globals** it swaps around the core
  switch — `$_`, `@_`, `$@`, `$/`, `select`, `%^H`, `$SIG{__DIE__/__WARN__}`
  (the `defsv/defav/errsv/rs/defoutgv/hinthv/diehook/warnhook` + `SWAP_SVS`
  entries in `state.h`). *Which* globals are per-green-thread is a Coro design
  choice, not an interpreter fact. (Core could later offer a generic
  "swap this set of GV/`PL_` slots" helper, but the selection stays Coro's.)
- scheduling, the coro/`HV` object model, event-loop integration.

With the API in use, `Coro::State`'s transfer path is roughly:

```
save_perl(current):  execstate_save(current->exec); swap policy SVs off
cctx_switch(current, next)
load_perl(next):     swap policy SVs on; execstate_load(next->exec)
```

with `coro_setup` → `execstate_init` and `destroy_perl`'s stack handling →
`execstate_destroy`. `state.h` disappears.

## Relationship to parkapi and to ithreads

- **parkapi** (`savestack_freeze/thaw`) serializes *part* of one execution state
  (the save stack) for the stackless model, where there is a single C stack and
  the savestack must be lifted off and re-applied by hand. **execstate** swaps
  *whole* execution states for the stackful model. parkapi is thus a subset of
  what execstate touches, which is exactly why it does not help Coro directly —
  Coro swaps the savestack wholesale (O(1)) rather than freezing it (O(n)). The
  two share the underlying save-stack invariants and can reuse helpers.
- **ithreads** (`perl_clone`) is the opposite trade-off: a *separate whole
  interpreter* with its own globals and heap — heavyweight, and not what a green
  thread wants (shared globals, shared heap, separate stacks). execstate is the
  missing lightweight middle: many execution states within one interpreter.

## API/ABI stability

The API is **purely additive**: it introduces new symbols only and changes no
existing API or ABI.

- **No existing exported function changes** — no signature, calling convention,
  `embed.fnc` flag, symbol name or return type of any current function is
  altered; new `embed.fnc` entries are additions only.
- **No existing struct layout changes** — `PerlInterpreter` (`intrpvar.h`,
  `thrdvar.h`), `PERL_SI`, `ANY`, `PERL_CONTEXT` and every other public struct
  keep their exact field order, sizes and offsets. `PerlExecState` is a
  standalone struct with its own storage, generated from the `execstate.h`
  register list; it is never embedded in, aliased over, or used to re-declare
  any part of `PerlInterpreter`. `save`/`load` *copy* register values between the
  live `PL_*` slots and a `PerlExecState`; they never repoint or reorder the
  interpreter's own fields.
- **No existing macro or typedef is redefined or removed** — `ENTER`/`LEAVE`,
  `SAVETMPS`, the `SS_*`/`SP` macros, etc. are untouched; `PERL_EXECSTATE` /
  `PERL_EXECSTATE_LEVEL` are new feature macros.
- **No behavioural change to existing code paths** — the live interpreter runs
  exactly as before when the API is not used.

The register list is shared with `Perl_init_stacks`/`Perl_nuke_stacks` via the
`execstate.h` X-macro, so the API and the live interpreter can never disagree.
The X-macro references the existing `PL_*` members by name and does not redeclare
them, so `intrpvar.h`/`thrdvar.h` are unchanged and the interpreter struct layout
is untouched.

## Attribution

The technique this API standardises — and, concretely, the list of interpreter
registers that constitute a switchable execution context, the fresh-stack setup,
and the correct unwinding teardown — is the work of **Marc A. Lehmann**
E<lt>schmorp@schmorp.deE<gt>, the author of Coro
(http://software.schmorp.de/pkg/Coro.html). `execstate_init`/`_destroy` derive
from Coro's `coro_init_stacks`/`coro_destruct_stacks`/`coro_unwind_stacks`, and
the register list from `Coro/state.h`. Any core implementation MUST retain this
credit (a header comment and a perldelta/AUTHORS acknowledgement). Coro is
licensed under the same terms as perl, so there is no licensing obstacle to
folding the derived logic into core.

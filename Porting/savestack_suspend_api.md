# Savestack suspend/resume API ("parkapi")

Specification of a supported core API that freezes, unwinds and later re-applies
a region of the save stack. First consumer: Future::AsyncAwait (FAA); also of
interest to Syntax::Keyword::Dynamically.

Branch: `savestackAPI`. Companion to the strerror/locale containment already on
this branch — the `ENTER`/`LEAVE` bracketing the `my_strerror()` scratch in
`Perl_sv_string_from_errnum()` (`mg.c`), which stops that scratch from lingering
in the caller's dynamic scope on threaded builds (see §1.5). That change
also fixes the FAA `await` panic when a non-zero `$!`/`$^E` has been stringified
before the suspend (rt.cpan.org #178421).

---

## Specification

### 1.1 Problem

Suspending an `async sub` mid-execution requires lifting the sub's *dynamic
scope* off the interpreter and reinstating it later. A large part of that scope
is the save stack (`PL_savestack`): every `local`, `SAVETMPS`,
`SAVEDESTRUCTOR_X`, pad-clear, etc.

Today FAA does this by re-implementing `leave_scope` backwards in a hand-written
`switch (SAVEt_*)` that duplicates core's private knowledge of savestack layout.
This is fragile (it breaks whenever core adds or changes a save type — FAA then
`panic`s), it must rediscover core's per-type refcount conventions by trial and
error (e.g. `save_scalar` inc's the old value but `save_hash` does not — a
difference only observable as a dangling free on the cancel path), and it cannot
handle `SAVEt_ALLOC` scratch referenced by absolute savestack offset.

Any stackless suspend-a-scope consumer (FAA, S::K::Dynamically) needs this same
save/restore semantics. Moving it into core lets core evolve the save stack
freely and gives every consumer one correct implementation instead of N fragile
re-implementations. (Stackful engines take a different route entirely — they
switch whole C stacks and carry their dynamic scope with them, via the separate
execstate interface — so this API is for the stackless case.)

### 1.2 Public API

Opaque handle + five entry points (names provisional):

```c
typedef struct PerlSavestackFrozen PerlSavestackFrozen;

PerlSavestackFrozen *Perl_savestack_freeze(pTHX_ I32 base_ix);
void  Perl_savestack_thaw(pTHX_ PerlSavestackFrozen *frozen);
void  Perl_savestack_frozen_free(pTHX_ PerlSavestackFrozen *frozen);
void  Perl_savestack_frozen_run_deferred(pTHX_ PerlSavestackFrozen *frozen);
void  Perl_savestack_frozen_foreach_sv(pTHX_ PerlSavestackFrozen *frozen,
          void (*cb)(pTHX_ SV *sv, const char *desc, void *ud), void *ud);
```

- **freeze(base_ix)**: serialize `PL_savestack[base_ix .. PL_savestack_ix)` into
  a heap blob and unwind it off the live stack, applying each entry's *park*
  transition (localized target reverted to its outer value, with set-magic for
  magical containers). Afterwards `PL_savestack_ix == base_ix`. Croaks on a
  non-relocatable entry (§1.5).
- **thaw**: re-push equivalent live entries from the blob (so a later ordinary
  `leave_scope` restores/frees correctly) and re-apply the localized values
  (replaying set-magic for magical containers, the mirror of freeze's park).
  Consumes the blob.
- **frozen_free**: discard a blob that will never be thawed. For value
  localizations this drops owned references and leaves the already-parked outer
  values in place; for *memory-reclaiming* deferred entries (`SAVEt_FREESV`,
  `SAVEt_FREEPV`, `SAVEt_MORTALIZESV`, `SAVEt_SET_SVFLAGS`, …) it performs the
  reclaim. It does **not** run user-visible finalizers (`SAVEt_DESTRUCTOR`/`_X` —
  `defer` / `try`/`finally` blocks): a blob that is merely garbage-collected must
  not fire them (see the split below). Idempotent-safe to call after
  `run_deferred`; frees the blob.
- **frozen_run_deferred**: run only the deferred *user callbacks* parked in the
  blob (`SAVEt_DESTRUCTOR` / `SAVEt_DESTRUCTOR_X`), in `leave_scope` order
  (most-recently-entered first), **without** freeing. This is the cancellation
  hook: a consumer tearing down a suspended scope calls `run_deferred` (to fire
  `finally` / `defer` as a normal scope exit would — FAA's on-cancel semantics,
  rt.cpan.org #135351) and then `frozen_free` to reclaim. The split exists
  precisely because "cancelled" and "garbage-collected" must differ: only the
  former runs finalizers. (`SAVEt_FREESV` etc. are *not* user callbacks, so they
  stay with `frozen_free`.)
- **frozen_foreach_sv**: enumerate retained SVs for Devel::MAT-style tooling.

### 1.3 Per-type relocation vtable (core-internal)

`freeze`/`thaw` are driven by a table keyed by `SAVEt_*`, parallel to the
`SAVEt_*` enum in `scope_types.h`:

```c
typedef struct {
  bool relocatable;                              /* false => freeze() croaks   */
  I32  (*freeze) (pTHX_ SavedEntry *out);        /* pop+serialize+park; ret slots */
  void (*thaw)   (pTHX_ const SavedEntry *in);   /* re-push + re-apply value   */
  void (*discard)(pTHX_ SavedEntry *in);         /* drop owned refs            */
  void (*walk)   (pTHX_ const SavedEntry *in, SV_walk_cb, void *ud);
} savetype_reloc;
```

Because the handlers live in core they share `leave_scope`'s primitives and
conventions, so refcount ownership (the inc-vs-no-inc asymmetry, tied-elem
mortalizing, `PL_localizing = 2` set-magic on restore) is correct by
construction — the central advantage over the downstream re-implementation.

### 1.4 Type classification (the ~60 `SAVEt_*` of scope_types.h)

- **Value localizations — full freeze/thaw/park:** `SAVEt_SV`, `SAVEt_SVREF`,
  `SAVEt_GVSV`, `SAVEt_GVSLOT`, `SAVEt_GENERIC_SVREF`, `SAVEt_AV`, `SAVEt_HV`,
  `SAVEt_ITEM`, `SAVEt_SPTR`, `SAVEt_PADSV_AND_MORTALIZE`, `SAVEt_HELEM`,
  `SAVEt_AELEM`, `SAVEt_DELETE`, `SAVEt_ADELETE`.
- **Bookkeeping — freeze/thaw, no value to park:** `SAVEt_CLEARSV`,
  `SAVEt_CLEARPADRANGE`, `SAVEt_COMPPAD`, `SAVEt_FREEPV`, `SAVEt_FREESV`,
  `SAVEt_FREEOP`, `SAVEt_FREEPADNAME`, `SAVEt_FREECOPHH`, `SAVEt_FREERCPV`,
  `SAVEt_MORTALIZESV`, `SAVEt_SET_SVFLAGS`, `SAVEt_HINTS`/`_HH`,
  `SAVEt_COMPILE_WARNINGS`, `SAVEt_CURCOP_WARNINGS`, and the scalar-C-type saves
  (`SAVEt_INT`/`_IV`/`_BOOL`/`_I32`/`_STRLEN`/… and their `*_SMALL` tight-packed
  forms). On cancellation (`frozen_free`) the memory-reclaiming members of this
  group perform their reclaim; none of them is a user-visible finalizer.
- **Deferred user callbacks — freeze/thaw, run only on explicit cancel:**
  `SAVEt_DESTRUCTOR`, `SAVEt_DESTRUCTOR_X` (`defer` / `try`/`finally`). Unlike the
  bookkeeping group these are *not* run by `frozen_free`; the consumer fires them
  via `frozen_run_deferred` when it is cancelling (not merely GC-ing) the scope.
- **Non-relocatable — `relocatable = false`, freeze croaks:** `SAVEt_ALLOC`
  (§1.5), `SAVEt_REGCONTEXT`, `SAVEt_FREE_REXC_STATE`, `SAVEt_STACK_POS`, and
  interpreter bookkeeping that only makes sense at its original stack position
  (e.g. a `PL_tmps_floor` save — the consumer re-establishes `SAVETMPS` itself).

`SAVEt_DESTRUCTOR_X` is mechanically relocatable (func + data) but its semantics
are opaque (used by S::K::Try `finally`, S::K::Dynamically). Relocate it as today
and document the position-independence assumption; consider a public registration
hook (§1.6).

Version note: this base (5.42.x line) adds `SAVEt_FREERCPV` (a ref-counted-PV
free, classified as bookkeeping above) and `SAVEt_FREE_REXC_STATE` (regex
compile state, classified non-relocatable with `SAVEt_REGCONTEXT`) relative to
the 5.38 audit. Every reloc-table row defaults non-relocatable, so any type not
yet classified is caught by a croak rather than mishandled.

**Magical vs tied targets.** A localized target that carries get/set *magic* but
is not tied — the important cases being `%ENV` and `%SIG`, whole and per-element
— is supported: freeze and thaw each replay the target's set-magic
(`PL_localizing = 2; mg_set`), exactly as `leave_scope` does on a normal exit, so
the magic's side effect re-syncs to whatever value is currently installed (e.g.
`%ENV` re-runs `setenv` when the outer value is parked back while suspended, and
again when the localized value is restored on thaw). The exception is *tied*
elements: `SAVEt_HELEM`/`SAVEt_AELEM`/`SAVEt_DELETE`/`SAVEt_ADELETE` on a tied
container live behind `FETCH`/`STORE` rather than in the HE/AV slot the handlers
manipulate, so they are refused (deterministic croak; `S_can_freeze_elem` /
`_delete` test `mg_find(..., PERL_MAGIC_tied)`). Caveat: set-magic can in
principle `die` (a `STORE` that throws); if that happens mid-freeze the
partially-built blob leaks and the save stack is left partly unwound — the same
exposure a hand-rolled `leave_scope`-in-reverse has. Refusing tied bounds this to
well-behaved core magic.

### 1.5 The absolute-offset problem

`save_alloc` scratch located by a destructor via absolute `PL_savestack[ix]`
cannot be relocated (freezing shifts the base). freeze must *refuse* it
(deterministic croak), never corrupt. This couples the API to core hygiene: no
`save_alloc` scratch may escape into a caller's dynamic scope. Audit result: the
only offender was the locale/strerror path reached by stringifying `$!`/`$^E` on
**threaded** builds — contained on this branch by the `ENTER`/`LEAVE` around
`my_strerror()` in `Perl_sv_string_from_errnum()` (`mg.c`). tie (`magic_methcall`),
`%SIG` (`magic_setsig`), `%ENV`, `%^H` are already `ENTER`/`LEAVE`-balanced.

### 1.6 Open questions

1. Error handling: croak vs. return NULL + set errsv (lets a consumer fall back
   gracefully). FAA currently panics.
2. Public relocation-handler registration so modules can support their own
   custom `SAVEt_*`/destructors (would also give S::K::Dynamically a supported
   hook instead of FAA-specific `post_suspend`/`pre_resume`).
3. Header placement (`scope.h` vs a new `savestack.h`) and final naming.
4. Devel::MAT integration shape (callback iterator vs typed accessors).
5. Scope boundary: this API is savestack-only; pad/mark/value-stack/`cx`
   relocation stays with the consumer (FAA already does it).

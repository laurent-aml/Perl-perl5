# Porting an XS module to multicore — a worked example

A concrete, side-by-side illustration of the two ways an XS module can hand a
blocking / CPU-bound C section off the interpreter thread, using the core
multicore hook (`perlmulticore.h`). It shows the *same* trivial XSUB in three
forms:

1. the original, unported code;
2. the same, upgraded with the **release/acquire bracket**;
3. the same, restructured for the **offload** primitive.

For the rationale — when each is worth it, which models (stackful Coro /
stackless Future::AsyncAwait) and platforms (notably Windows) each supports, and
why offload is the portable but more intrusive path — see
`Porting/green_threads_and_execstate.md`. This file is just the code.

The toy operation, `scramble`, is a pure-C, O(n) byte transform (XOR every byte
with `0x5A`): a meaningful SV in, SV out, and CPU-bound enough for large buffers
that releasing the interpreter is worthwhile.

## Common preamble (all three)

```c
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include "perlmulticore.h"   /* the bracket; offload comes via perl.h */

MODULE = Toy::Scramble   PACKAGE = Toy::Scramble

BOOT:
#ifdef perlmulticore_support   /* see below: the bundled CPAN header wants this */
    perlmulticore_support ();
#endif
```

That `BOOT:` line is for the *deployed CPAN* `perlmulticore.h`, where
`perlmulticore_support` is a macro a module calls to announce itself. Core's
header has no such entry point and nothing to register — the hook lives in the
interpreter — so calling it unguarded against core's header compiles with a
warning and then dies at load time on an unresolved symbol. Guard it, or drop it
if you only ever build against core.

Which header that `#include` finds does not decide what a module can use. Either
one gives it `perlinterp_release`/`acquire`; offload comes from neither, but from
`perl.h` — the callback types out of the types section it includes unconditionally,
and the functions out of `embed.h` — so all three forms below build the same way
whether a module bundles the CPAN header or takes core's. The caveats at the end
say what that means for guarding.

## 1. Unported — SV in, SV out, no multicore

```c
SV *
scramble(SV *in)
  CODE:
    STRLEN len;
    const unsigned char *src = (const unsigned char *)SvPVbyte(in, len);
    SV *out = newSVpvn((const char *)src, len);       /* copy in -> out */
    unsigned char *dst = (unsigned char *)SvPVX(out);
    for (STRLEN i = 0; i < len; i++)
        dst[i] ^= 0x5A;                                  /* the pure-C work */
    RETVAL = out;
  OUTPUT:
    RETVAL
```

## 2. release/acquire — bracket the pure-C loop, everything else unchanged

```c
SV *
scramble(SV *in)
  CODE:
    STRLEN len;
    const unsigned char *src = (const unsigned char *)SvPVbyte(in, len);
    SV *out = newSVpvn((const char *)src, len);       /* perl: holds the interp */
    unsigned char *dst = (unsigned char *)SvPVX(out);

    if (len > 8000) perlinterp_release ();      /* --- interp to a worker --- */
    for (STRLEN i = 0; i < len; i++)            /* pure C: touch NO perl here */
        dst[i] ^= 0x5A;
    if (len > 8000) perlinterp_acquire ();      /* --- interp back ---------- */

    RETVAL = out;
  OUTPUT:
    RETVAL
```

The change from (1) is **two lines**. Same signature, same synchronous return.
Notes:

- The SV access (`SvPVbyte`, `newSVpvn`) stays *outside* the bracket; only the
  raw-pointer loop is inside. Touch no perl data between release and acquire.
- `dst` points into `out`, a fresh SV private to this call that no other coro can
  reach, so it is safe to keep transforming while the interpreter runs elsewhere.
- Gated on `len > 8000`: below the threshold the handoff costs more than the work,
  so it runs inline.
- This is the near-transparent upgrade — but it works by *migrating the
  interpreter* between OS threads, so it needs a perl that is not *using*
  ithreads: anything that pins interpreter state to one OS thread breaks it. That
  is a build property, not an operating system. On Windows it rules out the
  ithreads-based `fork` emulation, so the bracket wants a non-ithread build there;
  Coro's own stackful switch does work on Windows (the asm backend covers x64, and
  Coro + Coro::Multicore have been confirmed working on it). See the portability
  discussion in the main doc.

## 3. offload — restructure into work + done, and add an async twin

The two callbacks split the old body cleanly: `work` is the pure-C part (runs on
a worker, no interpreter), and `done` marshals the C result into an SV and
**returns it** (runs holding the interpreter, when the work finishes). Neither
knows anything about Coro, FAA, rouse, or Future.

`multicore_offload` hands back a **handle**, not the value: the work may still be
running. The two entry points below differ only in what they do with it — the
synchronous one waits for it and returns the value, the asynchronous one returns
the handle for the caller to `await` or `get`. That is the colouring boundary, and
it is the only place in the module where it appears.

Both callbacks take a context from the backend, which is where the cancellation
flag lives; check `size` before reading a field, so a module built against an older
header keeps working. (In a real `.xs` these plain C functions go *before* the
`MODULE =` line; they are shown here in reading order.)

```c
/* --- plain C: work() touches no SVs and gets no aTHX --- */
typedef struct {
    SV            *out;   /* output SV, made on the interpreter thread */
    unsigned char *buf;   /* its buffer, transformed in place by the worker */
    STRLEN         len;
    int            heap;  /* job allocated by the async form: done () frees it */
    int            completed;       /* work () got to the end.  See done () */
} scramble_job;

static void
scramble_work (void *arg, const perl_multicore_work_ctx *ctx)   /* worker thread */
{
    scramble_job *j = (scramble_job *)arg;
    STRLEN i;

    for (i = 0; i < j->len; i++) {
        /* Advisory cancellation: poll between chunks of work, cheaply enough that
         * a caller which stops waiting is not left waiting anyway. */
        if (!(i & 0xffff)
            && ctx && ctx->size >= sizeof (*ctx) && ctx->cancel && *ctx->cancel)
            return;                       /* `completed` stays 0: see done () */

        j->buf[i] ^= 0x5A;                /* the same pure-C work */
    }

    j->completed = 1;                     /* got to the end: see done () */
}

/* Runs holding the interpreter once the work is over; marshals the C result and
 * RETURNS the SV, which becomes the handle's value.  Not necessarily on the thread
 * or in the green thread that made the call - see "who owns what" below. */
static SV *
scramble_done (pTHX_ void *arg, const perl_multicore_done_ctx *ctx)
{
    scramble_job *j = (scramble_job *)arg;
    SV *out       = j->out;
    int heap      = j->heap;
    int done_all  = j->completed;       /* read before the job can be freed */
    int have_ctx  = ctx && ctx->size >= sizeof (*ctx);

    if (heap)
        Safefree (j);                     /* the frame it came from is long gone */

    /* The handle was dropped: this call exists only so that the heap job above
     * gets freed.  Nobody is waiting, so release and go - do not croak. */
    if (have_ctx && ctx->dropped) {
        if (heap)
            SvREFCNT_dec (out);

        return &PL_sv_undef;
    }

    /* Asked to stop, and we did not get to the end: raise, rather than hand back a
     * buffer the caller could not tell apart from a whole one.
     *
     * Note which way round the flag goes.  `completed` is set by `work` as its
     * last act, so anything that stopped it - a poll that saw the flag, or never
     * having been started at all, which is what happens when a cancellation beats
     * the worker to the job - leaves it clear.  A flag meaning "stopped early"
     * would have missed the second case, and reported an untouched buffer as an
     * answer.
     *
     * And note the first test: a cancellation that arrived during the last chunk
     * is simply too late, and a whole result is not an error. */
    if (have_ctx && ctx->cancelled && !done_all) {
        if (heap)
            SvREFCNT_dec (out);           /* nothing else owns it */

        /* what was scrambled so far is in the caller's own buffer, so there is
         * nothing to attach; a module that marshals its result only here would
         * pass it as the first argument */
        croak_sv (multicore_offload_cancelled (NULL, "scramble: cancelled"));
    }

    /* HANDS OVER a reference either way: the heap job's `out` already is one, the
     * frame job's is a mortal the frame owns.  See below. */
    return heap ? out : SvREFCNT_inc (out);
}
```

The synchronous entry point — the one form (1) already had, and the one that keeps
working for every existing caller:

```c
SV *
scramble(SV *in)
  CODE:
    STRLEN len;
    const unsigned char *src = (const unsigned char *)SvPVbyte(in, len);
    scramble_job j;                              /* on THIS frame: we wait below */

    /* Everything that needs perl happens here, on the interpreter thread: make the
     * output SV, and hand the worker nothing but a raw pointer into it. */
    j.out  = sv_2mortal (newSVpvn ((const char *)src, len));
    j.buf  = (unsigned char *)SvPVX (j.out);
    j.len  = len;
    j.heap = 0;
    j.completed = 0;

    /* Offload, wait for the handle, return the value.  Under a green-thread
     * backend the wait suspends only the calling thread, so this is the
     * transparent form: same signature, same return, and the interpreter is free
     * meanwhile. */
    RETVAL = multicore_offload_sync (scramble_work, &j, scramble_done, &j);
  OUTPUT:
    RETVAL
```

And the asynchronous twin, which returns the handle:

```c
SV *
scramble_async(SV *in)
  CODE:
    STRLEN len;
    const unsigned char *src = (const unsigned char *)SvPVbyte(in, len);
    scramble_job *j;

    /* This one returns BEFORE the work is over, so the job may not live on this
     * frame.  It goes on the heap and `done` frees it - by then the work has
     * finished and nothing else refers to it.  For the same reason `out` is a
     * counted reference the job owns, not a mortal. */
    Newx (j, 1, scramble_job);

    j->out  = newSVpvn ((const char *)src, len);
    j->buf  = (unsigned char *)SvPVX (j->out);
    j->len  = len;
    j->heap = 1;
    j->completed = 0;

    RETVAL = multicore_offload (scramble_work, j, scramble_done, j);
    /* The backend's handle, returned unchanged: the caller awaits it, or calls
     * get on it.  This module never names its class. */
  OUTPUT:
    RETVAL
```

The change from (1) is **almost everything**:

- The work is lifted into a standalone
  `void (*)(void *, const perl_multicore_work_ctx *)` that touches no perl and
  gets no `aTHX`.
- The result is marshalled back in `done()`, which *returns* the SV. The XSUB does
  not produce it, and cannot: by then the frame may be gone. The module never
  mentions Coro/FAA/rouse/Future.
- There are now **two** entry points. `scramble` keeps its signature and its return
  value, so existing callers are unaffected; `scramble_async` returns the handle,
  and is the one a stackless caller needs, because a stackless caller cannot get a
  value back synchronously. A generic XS module adds such a twin per offloadable
  method — the **function-colouring** boundary, made visible in the API. What it
  does not have to do is change what the existing method returns, which is the
  point of the handle being the backend's rather than the module's.
- A layer in the middle — a library wrapping this one — should **return the
  handle** rather than consume it. Consuming it is what forces everything above to
  be asynchronous.

### What `work` may not do

The worker thread has **no interpreter**. That is more restrictive than it first
looks, and all but the first of these was a real bug found porting a real module
(PDL):

- **No SVs, no `aTHX`** — the obvious one. Marshal what the work needs into plain
  C before the call, as above.
- **No perl allocator.** `Newx`, `Safefree`, `SvGROW`, `newSV` are `PerlMem_*`
  macros that go through the interpreter's memory API; on a threaded perl that is
  a per-interpreter vtable. So no allocation from `work`: size and allocate
  everything on the interpreter thread first, as `scramble_async` does with `out`.
- **No `warn` or `croak`.** Both call into perl. Report failure by writing it into
  your own struct and raising it in `done`, which does hold the interpreter. (A
  library that already defers its own diagnostics — as PDL does for its worker
  pthreads — must make sure the *replay* also happens on the interpreter thread.)
- **Poll `ctx->cancel`.** A `work` that never polls cannot be interrupted, and an
  interrupted call then waits for it to finish anyway — the backend may not let
  the caller's frame go while the worker is still using it.

### Who owns what, and for how long

`work_arg` and everything `work` touches must stay valid **until the handle
resolves** — which is not the same as until the call returns. Since a portable
consumer cannot tell whether the handle it got is pending, the rule is a
biconditional:

> keep the job on your frame **if and only if** you wait on the handle before
> returning.

Both halves are above. `scramble` waits, so its job is an automatic on the calling
frame and its output SV is a mortal the frame owns — cleaned up on every path,
including the one where `done` croaks. `scramble_async` does not wait, so its job
is heap-allocated, it owns a counted reference to the output SV, and `done`
releases both. Getting this wrong the other way round is a use-after-free the
compiler cannot see: the worker writes into a frame that has returned.

`done` runs **exactly once** for every offload, including when the handle is
dropped while the work is still going — because otherwise nothing could free that
heap job. On that path `done_ctx.dropped` is set, the value is discarded, and the
frame the job may belong to is mid-unwind: release, do not compute, and do not
croak. `scramble_done` above needs nothing extra for it, since it frees the job
before it looks at anything.

And `done` **hands over** a reference: return a new SV with refcount 1, or an
immortal such as `&PL_sv_undef` — never a mortal one. The handle owns it until the
caller takes it out, and the caller owns it after that, which for an XSUB returning
it is what the `SV *` RETVAL typemap arranges (xsubpp emits `sv_2mortal`).
Returning an already-mortal SV therefore frees it twice. That is why
`scramble_done` returns `SvREFCNT_inc (j->out)` for the frame-owned job, and the
reference it already holds for the heap one.

Nor may `done` assume *where* it runs. It holds the interpreter — that is the whole
promise — but not necessarily on the thread, or in the green thread, that made the
call: a backend resolves its handle wherever the completion reaches it. So no
thread-local state established before the call, and no OS handle only the calling
thread may touch.

Optimisation aside: the copy into `out` above is the simple form. You can instead
pin the *input* SV (`SvREFCNT_inc`) and lend its buffer to the worker, avoiding the
copy — provided the SV stays alive, which pinning guarantees. Drop that reference
where you release the rest of the job: on the frame for `scramble`, which waits,
and in `done` for `scramble_async`, which does not. Dropping it when the
asynchronous call *returns* would free the buffer the worker is still reading.

## Contrast

| | release/acquire (2) | offload (3) |
|---|---|---|
| What moves | the **interpreter** | the **work** |
| XS change vs. (1) | two macros | `work`/`done` + an async twin |
| Call shape | synchronous | a handle |
| Models | stackful (Coro) | stackful **and** stackless |
| Needs of the perl | not *using* ithreads | any perl with the hook |
| Cancellable | no | advisory, if `work` polls |

What each row leaves out. In (2) the *work* stays where it was, on a thread that
no longer holds an interpreter; in (3) the *interpreter* stays where it was. (2)
leaves the module's code and its API alone, which is the whole of its appeal; (3)
rewrites the body and adds a method, and the synchronous twin still returns a
value, so the colouring is visible in the API rather than sprung on a caller.
"Not *using* ithreads" is a property of the build rather than of the operating
system — a perl with ithreads compiled in but unused is fine — and it is what
rules the bracket out on a stock Windows perl, where the `fork` emulation uses
them; offload asks nothing of the build at all.

Two caveats for both:

- With **no backend installed**, both are safe no-ops in the good sense: the
  bracket runs inline; `multicore_offload` runs `work` then `done` inline and hands
  back an already-resolved handle, so `scramble` and `scramble_async` both keep
  their shape. Correct either way, and nothing in the module has to test for it.
- `perlinterp_release`/`acquire` exist in both the deployed CPAN
  `perlmulticore.h` and the core one, so form (2) builds anywhere.
  `perlmulticore_support` is the CPAN header's alone, which is what the `BOOT:`
  guard in the preamble is for.

  **Offload is new, and needs a perl that ships the hook** — but not core's
  header: on such a perl the callback types arrive from the types section of
  `perlmulticore.h` that `perl.h` includes unconditionally, and `multicore_offload`
  itself from `embed.h`. So a module that bundles the CPAN header for the bracket
  can still reach offload; what is missing on an unpatched perl is all of it at
  once, which is why guarding on the callback ABI version works. Guard on that
  rather than on the function name, and keep a synchronous fallback:

  ```c
  #if defined PERL_MULTICORE_OFFLOAD_ABI && PERL_MULTICORE_OFFLOAD_ABI >= 1
    /* the work/done form above */
  #else
    /* form (1) or (2) */
  #endif
  ```

  The version is bumped whenever the callback signatures change, so a comparison
  keeps compiling where a bare `#ifdef` would not.

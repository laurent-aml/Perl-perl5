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
#include "perlmulticore.h"   /* release/acquire; offload is core-header only */

MODULE = Toy::Scramble   PACKAGE = Toy::Scramble

BOOT:
    perlmulticore_support ();
```

## 1. Unported — SV in, SV out, no multicore

```c
SV *
scramble(SV *in)
  CODE:
    STRLEN len;
    const unsigned char *src = (const unsigned char *)SvPVbyte(in, len);
    SV *out = newSVpvn((const char *)src, len);          /* copy input -> output SV */
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
    SV *out = newSVpvn((const char *)src, len);          /* perl work: holds the interp */
    unsigned char *dst = (unsigned char *)SvPVX(out);

    if (len > 8000) perlinterp_release ();               /* --- hand interp to a worker --- */
    for (STRLEN i = 0; i < len; i++)                     /* pure C only: touch NO perl here */
        dst[i] ^= 0x5A;
    if (len > 8000) perlinterp_acquire ();               /* --- take the interp back ------ */

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
  interpreter* between OS threads, so it is Unix + Coro only (see the portability
  discussion in the main doc).

## 3. offload — restructure into work + done + async return

The two callbacks split the old body cleanly: `work` is the pure-C part (runs on
a worker, no interpreter), and `done` marshals the C result into an SV and
**returns it** (runs on the interpreter thread). Neither knows anything about
Coro, FAA, rouse, or Future.

```c
/* --- plain C: work() touches no SVs and gets no aTHX --- */
typedef struct {
    unsigned char *buf;   /* owned copy of the input bytes; transformed in place */
    STRLEN         len;
} scramble_job;

static void
scramble_work (void *arg)                 /* runs on a worker thread */
{
    scramble_job *j = (scramble_job *)arg;
    for (STRLEN i = 0; i < j->len; i++)
        j->buf[i] ^= 0x5A;                /* the same pure-C work */
}

/* runs back on the interpreter thread; marshals the C result and RETURNS the SV */
static SV *
scramble_done (pTHX_ void *arg)
{
    scramble_job *j = (scramble_job *)arg;
    SV *out = newSVpvn ((const char *)j->buf, j->len);   /* marshal result into an SV */
    Safefree (j->buf);
    Safefree (j);
    return sv_2mortal (out);              /* the backend delivers this (value / Future) */
}
```

```c
SV *
scramble_async(SV *in)
  CODE:
    STRLEN len;
    const unsigned char *src = (const unsigned char *)SvPVbyte(in, len);
    scramble_job *j;
    Newx (j, 1, scramble_job);
    j->len = len;
    Newx (j->buf, len ? len : 1, unsigned char);
    Copy (src, j->buf, len, unsigned char);      /* marshal input OUT of the SV first */

    RETVAL = multicore_offload (scramble_work, j,   /* work -> worker thread            */
                                scramble_done, j);  /* done -> interp thread, returns SV */
    /* RETVAL is whatever the backend produced: the value itself (Coro, transparent)
     * or a Future (FAA, to be awaited). This XSUB just hands it back. */
  OUTPUT:
    RETVAL

The change from (1) is **almost everything**:

- The work is lifted into a standalone `void(*)(void*)` that touches no perl and
  gets no `aTHX`.
- Inputs are **marshalled out** of the SV before offloading (the worker cannot
  read SVs); the job is heap-allocated so it outlives this call, and freed in
  `done()`.
- The result is **marshalled back** in `done()`, which *returns* the SV. The XSUB
  does not deliver it — `multicore_offload` hands back whatever the backend made
  of it, and the XSUB just returns that. The module never mentions Coro/FAA/
  rouse/Future.
- The entry point is an explicitly **asynchronous** method (`scramble_async`),
  distinct from a plain synchronous `scramble`. Its return is the value under a
  Coro backend (transparent) or a Future under a FAA backend (`await` it) — the
  **function-colouring** boundary, made visible in the API. A generic XS module
  must add such an async twin per offloadable method; there is no way to hide the
  colour, because a stackless caller cannot get a value back synchronously.

Optimisation aside: the input copy above is the simple form, not mandatory. You
can instead pin the input SV (`SvREFCNT_inc`, released in `done()`) and lend its
buffer to the worker, avoiding the copy — provided the SV stays alive, which
pinning guarantees.

## Contrast

| | release/acquire (2) | offload (3) |
|---|---|---|
| What moves to another thread | the **interpreter** (work stays put) | the **work** (interpreter stays put) |
| XS change vs. unported | two macros; code & API unchanged | restructure into `work`/`done`; async twin method |
| Call shape | synchronous | value (Coro) or Future (FAA) — colour is explicit |
| Models | stackful (Coro) | stackful **and** stackless (FAA) |
| Platforms | Unix (needs interpreter migration) | all, **including Windows** |

Two caveats for both:

- With **no backend installed**, both are safe no-ops in the good sense: the
  bracket runs inline; `multicore_offload` runs `work` then `done` inline and
  returns `done`'s SV (so `scramble_async` returns the value directly). Correct
  either way.
- `perlinterp_release`/`acquire` (+ `perlmulticore_support`) exist in both the
  deployed CPAN `perlmulticore.h` and the core one, so form (2) builds anywhere.
  **`multicore_offload` is new — it is in core's `perlmulticore.h` only** — so
  form (3) requires a perl that ships the core hook (or an `#ifdef` on a feature
  macro with a fallback).

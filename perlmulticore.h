/*    perlmulticore.h
 *
 *    A core, backend-neutral hook for releasing the interpreter around a
 *    blocking or CPU-bound C section - the Perl analogue of CPython's
 *    Py_BEGIN_ALLOW_THREADS / Py_END_ALLOW_THREADS.
 *
 *    This is the "Perl Multicore Specification" by Marc A. Lehmann
 *    (http://perlmulticore.schmorp.de/), promoted into core.  Crucially it keeps
 *    the SAME wire ABI as the widely deployed CPAN perlmulticore.h so it is
 *    backward compatible: the two backend hooks live in a shared
 *    C<struct perl_multicore_api> stored in C<PL_modglobal> under the key
 *    "perl_multicore_api", with fields C<pmapi_release> / C<pmapi_acquire>.
 *    Existing perlmulticore-enabled modules (which bundle their own copy of this
 *    header) and existing backends (Coro::Multicore) meet at that struct, and
 *    this core copy meets there too - so a module built against core's header is
 *    driven by an already-installed Coro::Multicore, and a core-registered
 *    backend drives existing bundled-header modules, both without changes.
 *
 *    An XS module brackets a pure-C section that must not touch the interpreter:
 *
 *        perlinterp_release ();
 *        do_the_blocking_C_thing ();     // no perl API here!
 *        perlinterp_acquire ();
 *
 *    With no backend installed the hooks are nops, so the section runs inline and
 *    blocking, exactly as today.  A cooperative-scheduling backend registers
 *    hooks (with multicore_register(), or, like the CPAN backends, by writing the
 *    shared struct directly) that migrate the current thread of execution onto a
 *    worker OS thread for the duration, freeing the interpreter thread.
 *
 *    Rules (see the specification): touch NO perl data between release and
 *    acquire; call them in pairs, non-nested, release first; the bracketed C
 *    code must be thread-safe, as it may run in parallel with the interpreter.
 *
 *    ABI (struct layout, key, field names) derived from Marc A. Lehmann's
 *    perlmulticore.h, used under its public-domain / CC0 terms.
 */

/* ---- types needed early ---------------------------------------------------
 * perlvars.h declares PL_multicore_api (a C<struct perl_multicore_api *>) and
 * PL_multicore_offload (a C<perl_multicore_offload_t>), and proto.h declares
 * the Perl_multicore_* functions in terms of the hook typedefs, so these TYPES
 * must be visible before those headers.  perl.h pulls in ONLY this section (it
 * #defines PERL_MULTICORE_TYPES_ONLY around the include): it does NOT get the
 * struct body or the perlinterp_release/acquire macros.  That is what keeps an
 * XS module which bundles the widely deployed upstream perlmulticore.h from
 * seeing a second struct body and a second set of macros in the same
 * translation unit - the two headers use different include guards, so without
 * this split they collide.  scope.c and any module that opts in include the
 * whole header (no PERL_MULTICORE_TYPES_ONLY) to get the body and macros too. */
#ifndef PERL_MULTICORE_TYPES_H_
#define PERL_MULTICORE_TYPES_H_

/* Forward declaration only; the body is defined in the main section below (or
 * by a module's own bundled perlmulticore.h).  A pointer to it - the cached
 * PL_multicore_api - needs no body here. */
struct perl_multicore_api;

/* a backend hook; no interpreter context because acquire() runs on the worker
 * thread that does not (yet) hold the interpreter. */
typedef void (*perl_multicore_hook_t)(void);

/* Advisory cancellation flag, reached through the work context below.  A plain
 * volatile int rather than sig_atomic_t because this section is pulled into
 * perl.h ahead of any <signal.h>, and perl.h does not include one.  It is a
 * single word, set by whoever requests the abort and polled by the worker with
 * no further synchronisation: the worker may notice late, which is all an
 * advisory flag needs to promise. */
typedef volatile int perl_multicore_cancel_t;

/* Backend -> module contexts, handed to `work` and `done`.  Extensible: `size`
 * is sizeof the struct as the backend built it, so a module must check it before
 * reading any field appended after the ones below.  Backend-to-module only, so
 * the backend owns the layout. */
typedef struct {
    U16 size;
    perl_multicore_cancel_t *cancel; /* NULL, or nonzero once abort is requested */
} perl_multicore_work_ctx;

typedef struct {
    U16 size;
    int cancelled;                   /* work returned early on request */
} perl_multicore_done_ctx;

/* Bumped whenever the offload callback signatures below change, so a module can
 * guard its offload path:
 *     #if defined PERL_MULTICORE_OFFLOAD_ABI && PERL_MULTICORE_OFFLOAD_ABI >= 2
 * Lives in the TYPES section deliberately: a module that takes the offload types
 * from perl.h (PERL_MULTICORE_TYPES_ONLY) while bundling its own header for the
 * release/acquire bracket must still be able to test it. */
#define PERL_MULTICORE_OFFLOAD_ABI 1

/* offload work / completion callbacks (see the offload section below).  `work`
 * is pure C run on the worker thread (no interpreter); `done` runs on the
 * interpreter thread once work finishes and marshals the C result into an SV,
 * which becomes the offloaded call's return value.  `done` HANDS OVER a reference:
 * return a new SV with refcount 1 (or an immortal such as &PL_sv_undef), NOT a
 * mortal one.  The consumer that receives multicore_offload's value owns it and
 * must release it - which is what the `SV *` RETVAL typemap does for you, since
 * xsubpp emits sv_2mortal() there.  Returning an already-mortal SV from `done`
 * therefore mortalises it twice and frees it twice. */
typedef void (*perl_multicore_work_t)(void *arg, const perl_multicore_work_ctx *ctx);
typedef SV * (*perl_multicore_done_t)(pTHX_ void *arg, const perl_multicore_done_ctx *ctx);
typedef SV * (*perl_multicore_offload_t)(perl_multicore_work_t work, void *work_arg,
                                         perl_multicore_done_t done, void *done_arg);

#endif /* PERL_MULTICORE_TYPES_H_ */

#ifndef PERL_MULTICORE_TYPES_ONLY
#ifndef PERL_MULTICORE_H_
#define PERL_MULTICORE_H_

/* Feature macro: an XS module can C<#ifdef PERL_MULTICORE> to detect that this
 * perl ships the core multicore hook (this header, installed into CORE/) before
 * relying on it - then simply C<#include "perlmulticore.h"> and bracket with
 * perlinterp_release/acquire, carrying no bundled copy of its own. */
#define PERL_MULTICORE 1

/* The shared rendezvous struct - identical layout to the CPAN perlmulticore.h,
 * kept in PL_modglobal["perl_multicore_api"].  Do not reorder or extend the two
 * pointers: existing modules and backends depend on this exact layout. */
struct perl_multicore_api {
  void (*pmapi_release)(void);
  void (*pmapi_acquire)(void);
};

/* PL_multicore_api (perlvars.h) is a process-global cached pointer to the shared
 * struct, so acquire() is reachable from the worker thread without a context. */

#define perlinterp_release()   Perl_multicore_release (aTHX)
#define perlinterp_acquire()   Perl_multicore_acquire ()
#define perlmulticore_active() Perl_multicore_active ()

/* --- offload: a separate hook from the release/acquire bracket ------------
 *
 * Where the bracket migrates the interpreter to a worker while the blocking call
 * stays on the caller's thread, offload keeps the interpreter PINNED and moves
 * the *work* to a worker: it runs a pure-C `work` on a worker thread and, when
 * that finishes, runs `done` on the interpreter thread to marshal the C result
 * into an SV.  Because the interpreter never migrates, this works where the
 * bracket cannot - notably Windows.
 *
 * The call's shape is decided entirely by the registered backend, which is what
 * `multicore_offload` returns: a stackful (Coro) backend suspends the calling
 * green thread and resumes it to run `done`, returning the value directly (the
 * offloaded call looks synchronous); a stackless (Future::AsyncAwait) backend
 * returns a Future and resolves it from `done` on the loop (the caller awaits).
 * The core primitive is neutral to that choice - it only forwards work/done and
 * returns whatever SV the backend produced - so a consumer that hands off its C
 * call this way needs no knowledge of Coro, FAA, rouse, or Future.
 *
 * Unlike release/acquire there is no deployed ABI to be compatible with (this
 * is new), so it is a plain interpreter hook rather than a PL_modglobal struct.
 * With no backend installed multicore_offload() runs `work` then `done` inline
 * and returns done()'s SV (blocking, but correct), just as the bracket is a nop
 * without a backend.
 */

/* A backend MUST tolerate `done` croaking: that is how a module reports failure,
 * since it is the only failure channel a backend can recognise without knowing
 * the module's result encoding.  A stackful backend gets this free - the croak
 * unwinds the caller's frame - but it must still release its own per-call
 * resources on that path (a scope guard, not statement ordering).  A stackless
 * backend must trap the croak and FAIL its deferred; resolving it instead yields
 * a Future that succeeds with an error, which is the worst available outcome. */

/* true if an offload backend is installed (else multicore_offload runs inline) */
#define perlmulticore_offload_active() (PL_multicore_offload != NULL)

/* multicore_offload (work, work_arg, done, done_arg) and
 * multicore_register_offload (fn) are generated from embed.fnc. */

#endif /* PERL_MULTICORE_H_ */
#endif /* !PERL_MULTICORE_TYPES_ONLY */

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
 * perlvars.h declares PL_multicore_api (a C<struct perl_multicore_api *>), and
 * proto.h declares the Perl_multicore_* functions in terms of the hook typedef,
 * so these TYPES must be visible before those headers.  perl.h pulls in ONLY
 * this section (it #defines PERL_MULTICORE_TYPES_ONLY around the include): it
 * does NOT get the struct body or the perlinterp_release/acquire macros.  That
 * is what keeps an XS module which bundles the widely deployed upstream
 * perlmulticore.h from seeing a second struct body and a second set of macros
 * in the same translation unit - the two headers use different include guards,
 * so without this split they collide.  scope.c and any module that opts in
 * include the whole header (no PERL_MULTICORE_TYPES_ONLY) to get the body and
 * macros too. */
#ifndef PERL_MULTICORE_TYPES_H_
#define PERL_MULTICORE_TYPES_H_

/* Forward declaration only; the body is defined in the main section below (or
 * by a module's own bundled perlmulticore.h).  A pointer to it - the cached
 * PL_multicore_api - needs no body here. */
struct perl_multicore_api;

/* a backend hook; no interpreter context because acquire() runs on the worker
 * thread that does not (yet) hold the interpreter. */
typedef void (*perl_multicore_hook_t)(void);

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
/* True if a backend is installed.  The interpreter context is what makes that
 * answer trustworthy: the struct being asked about lives in PL_modglobal, which is
 * per-interpreter, so the probe needs a context to look there - and PL_multicore_api
 * is only a cache of that lookup, filled in lazily by multicore_register() or by
 * the first release through core.  Reporting from the cache alone meant a backend
 * that installed itself the CPAN way, writing the struct directly, stayed invisible
 * until something else happened to touch the bracket; the answer depended on call
 * order, and the same process could see false and then true.  With the context it
 * resolves the struct itself, so every backend is seen however it registered, and
 * whenever it is asked.
 *
 * Two consequences worth knowing.  This one is NOT callable without an interpreter,
 * unlike perlinterp_acquire() - which is no loss, since "is a backend installed?"
 * is a question asked on the interpreter thread before releasing, not from a
 * worker.  And, like a release, asking creates the rendezvous struct if it is not
 * there yet (with nop hooks), which is idempotent and has the useful property that
 * a backend installing later, by writing that struct, is seen at once.
 *
 * The spelling is unchanged for a module: the macro passes aTHX itself.  And note
 * the probe is an optimisation, not a requirement - bracketing unconditionally is
 * always correct, since with no backend the hooks are nops. */
#define perlmulticore_active() Perl_multicore_active (aTHX)

#endif /* PERL_MULTICORE_H_ */
#endif /* !PERL_MULTICORE_TYPES_ONLY */

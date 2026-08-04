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
    int dropped;                     /* nobody is going to receive the value */
} perl_multicore_done_ctx;

/* Bumped whenever the offload callback signatures below change, so a module can
 * guard its offload path:
 *     #if defined PERL_MULTICORE_OFFLOAD_ABI && PERL_MULTICORE_OFFLOAD_ABI >= 1
 * 1 is the first, and passes a perl_multicore_work_ctx / perl_multicore_done_ctx
 * to the callbacks; testing that it is merely defined is equivalent today, and
 * the comparison is what keeps working if this is ever bumped.
 *
 * 1 was redefined rather than superseded when the primitive went from returning
 * `done`'s value to returning a handle, because nothing had shipped: offload is
 * unreleased, so there is no build to stay compatible with, and a version could
 * not have enforced anything anyway - PL_multicore_offload is a function pointer,
 * and a backend cannot tell which contract its caller compiled against.  Every
 * consumer must be rebuilt against this header; there is deliberately no way to
 * write one source that satisfies both.
 * Lives in the TYPES section deliberately: a module that takes the offload types
 * from perl.h (PERL_MULTICORE_TYPES_ONLY) while bundling its own header for the
 * release/acquire bracket must still be able to test it. */
#define PERL_MULTICORE_OFFLOAD_ABI 1

/* offload work / completion callbacks (see the offload section below).  `work`
 * is pure C run on the worker thread (no interpreter); `done` runs once work
 * finishes, holding the interpreter, and marshals the C result into an SV, which
 * becomes the value of the handle multicore_offload returned.  Holding the
 * interpreter is the whole promise: `done` runs on whichever OS thread owns it
 * then, which need NOT be the thread that made the offloaded call, and not
 * necessarily in the green thread that made it either - a backend resolves its
 * handle wherever completion reaches it.  So `done` may use the perl API freely,
 * but must not assume identity: no thread-local state established before the
 * call, and no OS handle only the calling thread may touch.  `done` HANDS OVER a
 * reference: return a new SV with refcount 1 (or an immortal such as
 * &PL_sv_undef), NOT a mortal one.  The handle owns it from then until the caller
 * takes it out, and the caller owns it after that - which for an XSUB returning
 * it is what the `SV *` RETVAL typemap arranges, since xsubpp emits sv_2mortal()
 * there.  Returning an already-mortal SV from `done` therefore frees it twice. */
/* "no interpreter" is more restrictive than it first looks.  `work` may not touch
 * SVs, and it also may not allocate through perl (Newx / Safefree / SvGROW are
 * PerlMem_* macros reaching a per-interpreter vtable) or warn or croak (both call
 * in).  Size and allocate on the interpreter thread beforehand, and report failure
 * by leaving it in your own struct for `done` to raise. */
/* The synchronous form of the offload primitive: offload, wait, hand back the value
 * (the contract is in the offload section below).  The function lives in core, so
 * that one implementation, rather than a copy in every consumer, gets the delicate
 * part right: the handle is released through a scope guard, so the job is given up
 * properly even when `get` raises what `done` croaked with, or when the green thread
 * waiting in it is cancelled.  The spelling lives HERE, in the types section,
 * because that is the part perl.h pulls in unconditionally - a module that bundles
 * the deployed CPAN perlmulticore.h shadows the rest of this file and would
 * otherwise be unable to reach it.  It is deliberately not in the public API list:
 * it is a convenience over multicore_offload, not a second primitive. */
#ifndef PERL_CORE      /* embed.h defines the same thing, but only for core */
#  define multicore_offload_sync(work, work_arg, done, done_arg) \
     Perl_multicore_offload_sync (aTHX_ (work), (work_arg), (done), (done_arg))
#endif

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

/* --- offload: a separate hook from the release/acquire bracket ------------
 *
 * Where the bracket migrates the interpreter to a worker while the blocking call
 * stays on the caller's thread, offload keeps the interpreter PINNED and moves
 * the *work* to a worker: it runs a pure-C `work` on a worker thread and, when
 * that finishes, runs `done` holding the interpreter to marshal the C result into
 * an SV ("pinned" describes the interpreter, not `done`'s thread - see the note
 * where the callbacks are typedef'd above).  Because the interpreter never
 * migrates, this works where the bracket cannot.  What the bracket needs is a perl
 * that is not *using* ithreads, since it hands the interpreter to another OS
 * thread - a property of the build rather than of the operating system.  On Windows
 * that means a non-ithread build, because the threaded build there turns on
 * PERL_IMPLICIT_SYS, which routes even malloc and setjmp through a per-interpreter
 * vtable.  As Windows' `fork` emulation is built on ithreads as well, a non-ithread
 * Windows perl has no `fork` at all, which is what choosing this build costs.  The
 * bracket additionally needs a coroutine backend whose saved context another thread
 * may resume, which is a property of the backend, not of perl - Coro::Multicore
 * declines to release on one that cannot.  Offload needs none of that and works
 * either way, ithreads compiled in or not.
 *
 * Unlike release/acquire there is no deployed ABI to be compatible with (this
 * is new), so it is a plain interpreter hook rather than a PL_modglobal struct.
 * With no backend installed multicore_offload() runs `work` then `done` inline
 * (blocking, but correct) and hands back a resolved handle, just as the bracket is
 * a nop without a backend.
 */

/* --- what multicore_offload returns: the handle --------------------------
 *
 * `multicore_offload` returns a HANDLE OBJECT, supplied by the backend, and the
 * offload may still be running when it returns.  The value is taken out of the
 * handle afterwards.
 *
 * One shape, whatever the backend, is the point.  A backend decides how the work
 * is delivered - a stackful one (Coro) suspends the calling green thread until the
 * work is over, a stackless one (Future::AsyncAwait) resolves on its loop - and if
 * that choice reached the module's return type, then offloading an existing
 * synchronous method would change what that method returns as soon as an FAA
 * backend was loaded, invisibly and at a distance.  With one shape, a module
 * returns the handle from an asynchronous entry point without naming its class,
 * and the CALLER decides:
 *
 *     my $handle = $obj->scramble_async ($buf);
 *     await $handle;              # a stackless caller
 *     my $r = $handle->get;       # a blocking one; transparent under Coro
 *     $handle->cancel;            # stop the work, blocking until it has
 *     await $handle->safe_cancel; # stop it without blocking (optional)
 *
 * The handle implements the AWAIT_* protocol of Future::AsyncAwait::Awaitable,
 * which is duck-typed: the method set is the whole requirement, and neither core
 * nor a backend has to depend on Future - a plain Future satisfies it as it
 * stands.  Three groups, and it is worth knowing which is which:
 *
 *   get, cancel                     what CORE itself calls (multicore_offload_sync
 *                                   uses `get`), and what a blocking consumer uses.
 *                                   Not part of the AWAIT_* protocol: additions.
 *   AWAIT_IS_READY, AWAIT_GET,      the observers, for a consumer that wants to
 *   AWAIT_IS_CANCELLED,             ask rather than block.
 *   AWAIT_ON_READY, AWAIT_WAIT
 *   AWAIT_CLONE, AWAIT_NEW_DONE,    for Future::AsyncAwait, which builds the future
 *   AWAIT_NEW_FAIL, AWAIT_DONE,     an async sub returns by CLONEing the awaited
 *   AWAIT_FAIL, AWAIT_ON_CANCEL,    object and resolving the clone.  A backend must
 *   AWAIT_CHAIN_CANCEL              supply them for `await $handle` to work with no
 *                                   glue, which is the whole point - but they make
 *                                   no sense on a handle over a running job, so a
 *                                   handle that HAS one must refuse to be resolved
 *                                   through them rather than let a waiter return
 *                                   while the worker is still writing.
 *
 * A backend that cannot supply the set is refused at multicore_register_offload()
 * time rather than at the call.
 *
 * `get` must wait on the job's own completion primitive, and must NOT drive the
 * backend's event loop by hand: a backend that delivers completion through its
 * loop deadlocks if the thread it blocks is the one running that loop.
 *
 * The corollary for the caller, and it is a backend-dependent one: where a backend
 * delivers completion through an event loop, an offloaded call finishes only once
 * that loop gets a turn.  The work itself is done on the worker regardless, but
 * `done` cannot run, and `get` cannot return, until the loop is polled.  A caller
 * that occupies the interpreter without ever yielding to the loop therefore waits
 * for a value that has already been computed.  This is not special to offload -
 * such a caller stalls that loop's timers and I/O too - but offload is a place
 * where it shows up as one call never returning rather than as general
 * unresponsiveness, which is easy to misread.  A backend resolving completion
 * without a loop (a condvar, a semaphore) has no such requirement, so a portable
 * consumer should neither rely on nor defeat either behaviour: just do not busy-
 * wait while a handle is outstanding.
 *
 * A backend may hand back a handle that is already resolved - the two inline paths
 * below do exactly that - and a consumer must not care: `get` on a resolved handle
 * returns at once.
 *
 * Only the module's own entry point knows whether the handle should travel: a
 * middleware layer that has no use for the value should RETURN the handle rather
 * than consume it, since consuming it is what forces its callers to be
 * asynchronous too.
 */

/* Lifetime: `work_arg`, and everything `work` writes into, must stay valid until
 * the handle RESOLVES, not merely until multicore_offload returns.  A portable
 * consumer cannot tell whether the handle it got is pending, so the rule is a
 * biconditional:
 *
 *     keep the job on your frame IF AND ONLY IF you wait on the handle
 *     before returning
 *
 * The synchronous form below is that first case, and needs nothing further.  A
 * module that returns the handle upward must heap-allocate its job instead and let
 * `done` release it - by then the work has finished and nothing else refers to it.
 *
 * A backend must honour the rule even when the call is interrupted or the handle
 * is dropped while still pending: it may not let the frame go while the worker is
 * still writing into it, so it asks the work to stop (the cancel flag is there to
 * make that quick) and waits for it.
 *
 * `done` runs EXACTLY ONCE for every offload the backend accepted, including when
 * the handle is dropped while still pending.  It has to: it is the only place a
 * module can release what it built on the interpreter thread, and the
 * heap-allocated job above would otherwise leak with nothing left able to free it.
 * On that path `done_ctx.dropped` is set, the value it returns is discarded, and it
 * runs while the handle is being destroyed - so a frame-owned job is still there,
 * but the frame may be mid-unwind.  A `done` that sees `dropped` should therefore
 * release and return, not compute, and should not croak: there is nobody to raise
 * to, and the exception becomes a warning at best. */

/* Cancellation comes in two kinds, and what separates them is what happens to the
 * interpreter while the work is stopping.  Both are advisory in the same way: they
 * raise the flag `work` polls, and a `work` that never polls is simply waited out.
 *
 *   cancel       PROMPT.  Blocks until the work has actually stopped, so that when
 *                it returns the offload is over.  That is what makes it comparable
 *                to a Future's cancel, where the instance is cancelled at once -
 *                here it cannot be, because the worker is still writing.
 *
 *   safe_cancel  ASYNCHRONOUS, and optional.  Raises the flag and hands back an
 *                awaitable that completes when the work has stopped; the handle
 *                becomes cancelled at that point and not before.  Nothing is
 *                blocked meanwhile, which is what a stackless caller needs, since
 *                blocking its thread would stop the loop the completion arrives on.
 *                A backend that does not offer it is driven through the prompt path
 *                instead.
 *
 * A backend that offers safe_cancel should also offer AWAIT_CHAIN_SAFE_CANCEL, the
 * chaining companion, whose ordering requirement is that a parent orchestrating the
 * teardown is marked cancelled BEFORE its children - so that a frame resumed by a
 * child becoming ready sees its own future already cancelled.
 *
 * Reporting cancellation, on the other hand, is `done`'s job and not the
 * backend's.
 *
 * `done_ctx.cancelled` says cancellation was REQUESTED.  It does not say the work
 * stopped early: `work` returns void and has no channel back to the backend, so a
 * `work` that was on its last chunk when the flag went up may well have finished
 * complete.  Only the module can tell the two apart, by polling the flag itself.
 *
 * So, when `done` knows its result is incomplete, it must RAISE rather than return
 * it.  A truncated result the caller cannot tell apart from a whole one is the worst
 * outcome available, and the caller that awaits a handle is frequently not the one
 * that cancelled it, so it has no reason to go looking for a flag:
 *
 *     if (ctx->cancelled && !j->completed)
 *         croak_sv (multicore_offload_cancelled (partial, NULL));
 *
 * Note which way round that flag goes.  Have `work` set `completed` as its last act,
 * rather than setting a "stopped early" flag where it gives up: a cancellation can
 * beat the worker to the job, in which case `work` NEVER RAN, and a stopped-early
 * flag would then be clear and the module would report untouched output as an
 * answer.  Whether the work ran at all is not something the backend reports - it has
 * no channel for it - so this is the module's to get right, and getting it wrong
 * shows up only in the rarest timing.
 *
 * A `work` that does not return at the point it gives up - one whose inner loop
 * abandons the job but whose outer function returns normally, as PDL's does - needs
 * both facts recorded, since "the function returned" no longer implies "the work was
 * finished".
 *
 * multicore_offload_cancelled builds a PerlMulticore::Cancelled, so that every
 * module raises the same exception and a caller has one way - `$err->partial` - to
 * reach whatever was salvaged.  Whether anything was is the module's judgement:
 * PDL attaches nothing, because a cancelled transformation leaves its outputs
 * part-written and it marks them for recomputation instead.
 *
 * Returning a value that is indistinguishable from a complete one is a bug in the
 * module, and not one the backend can catch for it. */

/* A backend MUST tolerate `done` croaking: that is how a module reports failure,
 * since it is the only failure channel a backend can recognise without knowing
 * the module's result encoding.  The croak becomes the handle's FAILURE, raised
 * again when the value is asked for - which is the only place it can be raised,
 * `done` having run when the work finished rather than at a point in the program
 * that has anything to do with the call.  Resolving successfully with an error
 * inside instead is the worst available outcome.  A backend must also release its
 * own per-call resources on that path with a scope guard rather than by statement
 * ordering. */

/* true if an offload backend is installed (else multicore_offload runs inline) */
#define perlmulticore_offload_active() (PL_multicore_offload != NULL)

/* multicore_offload (work, work_arg, done, done_arg),
 * multicore_register_offload (fn), multicore_offload_ready (value) and
 * multicore_offload_cancelled (partial, message) are generated from embed.fnc.
 *
 * multicore_offload_ready wraps a value that is already known in a resolved
 * handle.  A module needs it wherever its asynchronous entry point has the answer
 * without offloading - a cached result, or an input too small to be worth a
 * worker - since returning the value there instead would put back exactly the
 * shape variance the handle exists to remove.  It consumes the reference it is
 * given and hands back one, like `done`. */

/* multicore_offload_sync (work, work_arg, done, done_arg) is the synchronous
 * form: offload, wait, and hand back the value.  That is the whole of what an
 * offload was before it returned a handle, and it is what a module whose entry
 * point must keep returning a value - or that has no asynchronous entry point at
 * all - wants.  The caller keeps its job on its frame, and the wrapper satisfies
 * the lifetime rule above by waiting before it returns.
 *
 * Like `done`, it HANDS OVER a reference: the value comes back with refcount 1
 * (the `SV *` RETVAL typemap in an XSUB mortalises it for you).  A croak from
 * `done`, or an exception aimed at the calling green thread while it waits, is
 * raised from there, once the work has stopped.
 *
 * It is generated from embed.fnc rather than defined here, so that it is reachable
 * from perl.h alone - a module that bundles the CPAN release/acquire header (which
 * shadows this file) still gets the whole offload interface. */

#endif /* PERL_MULTICORE_H_ */
#endif /* !PERL_MULTICORE_TYPES_ONLY */

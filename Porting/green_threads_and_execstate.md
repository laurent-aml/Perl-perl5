# Threads, green threads, and a core execution-state API for Perl

## Purpose of this document

This document studies the state of *threading* in Perl — both senses of the
word: the several models by which Perl programs run more than one thing at a
time, and the interpreter *execution state* that a thread of Perl execution is
made of. It is written to motivate and frame a specific proposal: that perl
core grow a small, additive, **public** API describing a thread's execution
state, so that the green-thread libraries that today reach into private
interpreter internals can instead stand on a supported contract.

It is the rationale companion to `Porting/execstate_api.md`, which specifies the
API itself: the "execstate" ladder, which publishes the interpreter state a
green-thread switch has to carry. This document argues *why*; that one
defines *what*. The reader is assumed to know Perl and C but not the internals
of Coro or of the perl run loop.

The argument runs: define what a thread is → separate concurrency from
multi-core parallelism → survey how Perl does each today (ithreads, process
pools, green threads) → observe where performance actually lives (XS) and what
that implies for the model people want → look at how other languages resolve the
same tensions → identify why the current Perl green-thread libraries are fragile
→ propose the core API that removes the fragility.

## What a thread is

Following the ordinary computer-science definition (as summarised on Wikipedia):
a **thread of execution** is the smallest sequence of programmed instructions
that can be managed independently by a scheduler. Multiple threads within a
single process run *concurrently* and *fully share the process's memory* and
resources (address space, file descriptors, globals), in contrast to separate
processes, which do not share those resources. A thread is therefore
characterised by three things:

1. an independent **flow of control** — its own instruction pointer and call
   stack;
2. **shared memory** with the other threads of the process; and
3. **lightweight lifecycle management** — cheap to create, and cheap for a
   scheduler to start, suspend, resume, switch between, and join.

Nothing in that definition says *how* the scheduler runs the threads. It may
interleave them on one CPU (time-slicing or cooperative yielding), or it may run
them literally at the same instant on several CPUs. Both are threads.

## Concurrency is the requirement; multi-core parallelism is a bonus

It is worth stating plainly, because much confusion in the Perl community (and
elsewhere) comes from conflating the two: **simultaneous execution on multiple
cores is not part of the definition of a thread.** A thread requires
*concurrency* — the ability to make progress on several flows of control whose
lifetimes overlap — which is a *structuring* property. *Parallelism* — two
instructions genuinely retiring in the same cycle on two cores — is a
*performance* property, and an additional capability layered on top.

The two are routinely provided separately. A great deal of software is threaded
for concurrency alone and never runs two threads at once: a GUI keeps its event
loop responsive while a "worker thread" waits on I/O; a network server keeps ten
thousand connections in flight though only one is ever computing. Many runtimes
deliberately *withhold* parallelism from their threads (Python's GIL, Node's
single loop, cooperative green threads) precisely because withholding it removes
whole classes of data race and lets threads share mutable memory safely.

Parallelism is nonetheless the strongest possible bonus: it is the only way to
make a CPU-bound program faster on modern hardware. So the interesting design
question for any language is not "threads or not" but "which axis — concurrency,
parallelism — do I get from which mechanism, and at what cost to shared state."
Perl answers that question with several mechanisms, each landing at a different
point on those axes.

## Current pure-Perl solutions

### Interpreter threads (`ithreads`)

Perl's built-in `use threads` (interpreter threads, since 5.8) provides real OS
threads. But it deliberately does *not* satisfy point (2) of the thread
definition — it does not share memory. Creating an ithread **clones the entire
interpreter**: every variable, pad, and package is deep-copied into the new
thread, and thereafter the two are independent unless a variable is explicitly
declared `:shared` (which routes every access through a lock and a serialised
backing store).

- **Pros.** They are genuine OS threads, so CPU-bound pure-Perl code *can* run on
  multiple cores — the one Perl mechanism that gives shared-nothing parallelism
  without a second process. No GIL, because there is nothing shared to lock.
- **Cons.** The clone is expensive in both time and memory (a fresh interpreter
  per thread), so thread creation is heavy and thread counts stay small. The
  "shared memory" that makes threads pleasant is gone: `:shared` data is slow,
  limited in type, and easy to get wrong; most CPAN modules are not thread-safe
  across a clone; and the model is error-prone enough that `perldoc threads`
  officially discourages its use for new code. It is, in effect, "processes that
  look like threads," with most of the cost of processes and little of the
  convenience of threads.

Note the contrast with Python, drawn out below: Python threads *share* memory but
*serialise* execution (the GIL); Perl ithreads *do not share* memory but *do*
run in parallel. They are opposite trade-offs, and neither is the shared-memory,
parallel thread people often imagine they are asking for.

### Process pools with IPC

The pragmatic mainstream answer to *parallelism* in Perl is not threads at all
but **processes**: `fork` a pool of workers and communicate over pipes, sockets,
or shared files. A rich CPAN layer packages this:

- `Parallel::ForkManager` — the classic fork-a-worker-per-job pool.
- `MCE` (Many-Core Engine) — chunked map/grep-style parallelism over a worker
  pool, with shared queues.
- `AnyEvent::Fork` / `AnyEvent::Fork::RPC` — fork a *template* process once,
  cheaply, then spawn workers from it and drive them from an event loop.
- `IO::Async::Function` — a pool of worker processes exposed as an
  asynchronous function returning a `Future`, integrated with `IO::Async`.

- **Pros.** True parallelism across all cores, with no interpreter-thread
  fragility. Hard fault isolation: a worker that segfaults or leaks does not take
  the parent down. No shared-mutable-state races — the isolation is enforced by
  the OS. Scales to and beyond the machine (the same pattern extends to remote
  workers).
- **Cons.** No shared memory: every datum crossing a process boundary must be
  *serialised*, sent, and deserialised, which dominates the cost for
  fine-grained work or large working sets. `fork` itself is not free, copy-on-
  write notwithstanding, and is problematic on Windows and inside threaded
  parents. Coordinating many workers, back-pressure, and partial failure is real
  work that the CPAN modules ameliorate but do not erase.

Processes are the right tool for coarse-grained, CPU-bound, embarrassingly
parallel work. They are a poor tool for keeping ten thousand mostly-idle I/O
flows alive, or for sharing a big in-memory index.

## What "green" threads bring

**Green threads** are threads scheduled in user space, inside one OS thread of
one process, by a library rather than the kernel. They *do* satisfy all three
points of the definition — independent control flow, fully shared memory
(they are all the same interpreter), and very lightweight management — while
deliberately declining multi-core parallelism: at most one green thread runs at
any instant, and control passes between them cooperatively.

That combination is exactly what the process-pool approach lacks. Because every
green thread lives in the same interpreter, they share all memory for free — no
serialisation, no marshalling, a coro can hand another a reference to a
multi-gigabyte structure at no cost. Because switching is a user-space register
swap rather than a kernel context switch or an interpreter clone, they are cheap
enough to have tens of thousands of them. The price is that they are
*cooperative* and *single-core*: a green thread that neither yields nor blocks on
a scheduler-aware operation starves all the others, and no green thread ever
speeds up a CPU-bound computation, because they never run at the same time.

Perl has two mature green-thread libraries, built on opposite mechanisms:

- **Coro** — *stackful* coroutines. Each coro has its own real C stack and its
  own copy of the interpreter's execution stacks; a switch saves one set of
  those and installs another, and (when needed) swaps the C stack too. Because a
  coro owns a full stack, it can suspend **anywhere** — deep inside nested
  function calls, inside a `map` block, even inside an XS call that calls back
  into Perl — simply by yielding. `$coro->cede`, `Coro::Semaphore`, channels,
  etc. are built on this.
- **Future::AsyncAwait (FAA)** — *stackless* async/await. `async sub`/`await`
  transforms a subroutine into a resumable state machine; suspension is only
  possible at an explicit `await` on a `Future`, and only within an `async sub`,
  not across an ordinary Perl or C frame. It integrates with the `Future` /
  `IO::Async` ecosystem and reads like modern async code in other languages.

## Stackful vs stackless: the technical difference

The two libraries answer one question differently: *what is a suspended thread,
concretely, and what must be preserved to resume it?*

- **Coro (stackful)** treats a suspended thread as an entire live execution
  context frozen in place. To resume it you restore the interpreter's execution
  registers — the value/mark/scope/save/temporaries stacks, the current op and
  COP, the pad, the compile-time cursors, the flags — and the C stack the coro
  was running on, and jump back in. Suspension is transparent to the code being
  suspended: it does not know or care that it is a coro. This is powerful
  (suspend anywhere, including under XS) and it is O(1) in the amount of live
  state — you swap pointers to whole stacks, you do not walk them.

- **FAA (stackless)** treats a suspended thread as a captured *continuation of an
  async sub*: enough saved state to re-enter that sub at its `await` point. It
  does not own a C stack, so it cannot suspend through a frame that is not itself
  an `async sub` — a plain sub, or an XS function, on the call chain is a barrier.
  Capturing the state is proportional to the depth being suspended (it must
  freeze the relevant save-stack region), i.e. O(n) rather than O(1), but the
  per-thread footprint is much smaller because there is no reserved C stack.

Several differences follow from that — some bearing on how much core support
each model needs, some on what the programmer may actually write:

- **Creation.** Coro spawns *explicitly* — `async { ... }` (or a constructor)
  puts a new coro on the scheduler. FAA has no spawn primitive at all: calling an
  `async sub` runs it synchronously to its first `await`, and if the awaited
  Future is not yet ready the frame suspends and hands a *pending Future* back to
  its caller. A concurrent thread therefore exists precisely when such a pending
  Future is left in flight rather than awaited inline — concurrency arises from
  the event loop driving several outstanding Futures, not from any create call.
- **The caller stack.** A stackful coro keeps its real call stack, so the
  Perl-level `caller()` chain and argument frames are simply *there* on resume. A
  stackless FAA thread has unwound the real stack at each suspension, so it must
  *reconstruct* — simulate — that caller chain when it resumes, so that code in a
  resumed `async sub` still sees a coherent call history. That simulation is
  extra machinery a stackful design does not need at all.
- **Cancellation.** Cancelling a suspended coro means unwinding a real, live
  dynamic scope, and Coro offers two flavours. *Prompt* cancel (`cancel`) tears
  the coro down at once from the canceller's context; *safe* cancel
  (`safe_cancel`) unwinds the coro in its **own** context at a cancellable point,
  so its `local` restorations, `DESTROY`s and `finally`/guard blocks run in the
  correct order **and may themselves block** — a guard can cede and wait while
  cleaning up. That cleanup is *allowed to block* is the whole point of
  `safe_cancel` over prompt cancel (and it is the delicate teardown this document
  keeps returning to).

  A stackless FAA thread has no such stack to unwind. Synchronous cleanup — a
  `defer` block, or a guard object's `DESTROY` — still runs, but it cannot
  `await`, so *asynchronous* cleanup on cancellation (cleanup that must itself
  `await`) has no shipped mechanism today: it is an open proposal, the
  `Future::With` approach — not (yet) a CPAN module; see RT #171969
  (https://rt.cpan.org/Public/Bug/Display.html?id=171969). So the contrast is
  sharper than a difference of syntax: Coro already runs blocking cleanup on
  cancellation, via `safe_cancel`, whereas for FAA asynchronous cancellation
  cleanup is still unsolved.
- **Dynamic scope and implicit variables.** Because a stackful coro carries its
  *entire* dynamic scope with it, `local`, `$_`, `@_`, `wantarray`, and
  `await`-ing inside a `map`/`grep`/`sort`/`eval` block behave exactly as they do
  in ordinary code — the coro resumes with the same dynamic state it left. A
  stackless FAA thread does not own that scope, and so carries a set of
  documented limitations. The sharpest is that a plain `local` does not compose
  with `await` — its effect would remain in force for whatever runs during the
  suspension — which is why the await-aware `dynamically` keyword
  (`Syntax::Keyword::Dynamically`) had to be introduced as its replacement.
  `$_` and other implicit/global variables are likewise not reliably preserved
  across an `await`; call context is subtle, because an `async sub` hands a
  `Future` back in its caller's context and only produces its real value later,
  so `wantarray` inside it does not mean what it does in an ordinary sub; and
  there are further restrictions on where an `await` may appear. The
  Future::AsyncAwait documentation carries the current list. These
  are less defects than the standing price of *not* carrying the full execution
  state — precisely the state a stackful coro swaps wholesale, and precisely what
  a core execution-state API would let it swap through a supported surface.
- **Adoption cost — function "coloring."** For a sub to `await`, it must be an
  `async sub`, and its callers must `await` it (or be `async` themselves), so the
  annotation propagates up the *entire* call graph — the well-known "what colour
  is your function" problem — and, as above, `local` must be rewritten as
  `dynamically`. Adopting FAA in an existing code base is therefore an invasive,
  transitive source rewrite. Coro is *colorless*: unchanged, ordinary subs can
  cede or block, because suspension need not appear in the signature of every
  function on the stack — the same property Go's goroutines and Java's virtual
  threads are prized for.
- **Explicit vs implicit suspension — knowing where you can block.** The mirror
  image of coloring, and here the advantage is FAA's — though narrower than it
  first appears. Coro is *cooperative*, not preemptive: a coro yields only where
  it (directly or transitively) calls something that cedes, so the programmer
  still controls where suspension may happen, and between those points code runs
  to completion with nothing else interleaving. That controlled atomicity is
  itself a feature for avoiding concurrency bugs. What Coro lacks is not control
  but *visibility at the call site*: a plain method or sub call does not advertise
  whether it might cede somewhere inside, so in code one did not write it is hard
  to tell which calls are suspension points. FAA makes that explicit — every
  suspending call is an `await` — and, more strongly, *structurally forbids*
  suspension where `await` is not allowed: inside a `DESTROY`, a tie or overload
  method, a signal handler, cleanup and magic are guaranteed synchronous. Coro
  cannot make that guarantee — a `DESTROY` run at an arbitrary refcount drop or
  during global destruction *may* cede and switch coros at an unsafe moment, with
  no parse-time way to detect or forbid it. So the honest gap is call-site
  visibility, and the un-checkable no-suspend contexts, not any loss of control
  over suspension itself.

  The second half of that gap can be closed from Coro's side, and an
  experimental (as-yet-unpublished) `Coro::Atomic` does so: an `atomic { ... }`
  section — equivalently a scope guard, or an `:Atomic` sub attribute — marks a
  region the running coro must not yield in. It is enforced *deterministically*
  at Coro's scheduler entry points (so it catches C-level blocking such as a
  semaphore or condvar wait too, not just an explicit `cede`), before any
  ready-queue state is touched, so a stray suspension inside it — including
  inside a `DESTROY` — is a fatal error rather than a silent switch. This does
  not make cede-ability *visible* at every call site (that would need FAA-style
  coloring), but it lets cleanup and critical sections *guarantee* no
  suspension, which is the property that most mattered — the complement of "flag
  what may block" is "forbid blocking here," and it composes with dynamic
  dispatch where a static scheme cannot.
- **Scheduling.** Coro is a *scheduler*, not only a suspension mechanism: it has
  a ready queue, thread priorities, explicit `cede` / `schedule`, and time-sliced
  yielding (such as the clock-based `cede_slice` — as-yet-unpublished work — which
  lets a long computation yield cooperatively with no external timer). FAA has no
  scheduler of its own —
  it yields suspendable tasks and leaves *when* they run to the driving event
  loop, with no built-in cooperative-yield or time-slice primitive. FAA is the
  suspension syntax; Coro is suspension *plus* a scheduler.
- **Native tooling — and here the debit is Coro's, but a modest one.** It is
  tempting to say C-level debuggers "cannot follow" a coro; that overstates it.
  C-level debugging of a *Perl* program is mostly about one's own XS, not about
  stepping perl's run loop (rarely useful), and that still works: a breakpoint in
  an XS function fires normally under Coro, and while stopped you can inspect that
  frame and walk the current coro's C stack up to where it was entered, because a
  *running* coro's C stack is an ordinary contiguous stack. What the private
  stacks and the hand-written switch actually cost is the *cross-coro* picture: a
  C backtrace does not continue coherently past a coro's entry (the frames beneath
  it are on another stack, and the switch carries no unwind information), the C
  stacks of *suspended* coros are not walkable at all, and sampling profilers get
  confused at switch boundaries. So the loss is whole-program C backtraces and
  clean cross-coro profiling — not the everyday "break in my XS and look around."
  The *Perl* debugger fares well too. Tested (perl 5.40, Coro 6.57): it runs Coro
  programs, breakpoints fire in every coro, and — since a running coro's stack
  *is* the live interpreter stack — variable inspection and `T` backtraces are
  correct for whichever coro is stopped; what it cannot do is confine control flow
  to one coro, since its step/continue state is global, so `c`/`n`/`s` follow the
  interpreter across every `cede` and hop between coros. (Coro ships its own
  `Coro::Debug` — a coro-listing, backtrace and remote-shell introspector — for
  exactly this reason.) FAA, on the ordinary C stack, is friendlier still. A real
  cost of the stackful model, but a narrower one than "debuggers cannot follow
  Coro" would suggest.

For all those differences, Coro and FAA share the one property that matters most
here: each must reach into interpreter state that Perl does not expose as API.
Coro copies the `PL_*` execution registers and manipulates the context-stack and
padlists directly; FAA freezes and thaws a region of the save stack. The two
reach into the same private machinery from opposite ends — Coro swapping the
*whole* state, FAA freezing a *slice* of it.

### Is FAA the simpler, "solved" answer? An honest reckoning

One reading colours much of this discussion and should be named directly: that
Future::AsyncAwait is the clean, modern successor whose async/await makes Coro's
stackful "hackery" unnecessary, so core need only bless FAA and let Coro fade.
On the axes this document is about, that reading does not hold up, and it is more
honest to say so plainly.

FAA does simplify one real thing: being stackless it never touches the C stack or
the context stack, so it needs no per-platform assembly switch backend, is
correspondingly more portable to build, has a smaller per-suspension footprint,
and integrates natively with the `Future`/event-loop world. Those are genuine
merits, and they are why many reach for it first.

But "simpler in that dimension" is not "simpler," and it is certainly not
"internals-free." FAA depends on the *same class* of unpublished interpreter
state as Coro — it freezes and thaws the save stack — so it does not escape the
problem that motivates a core API; it reaches into it from the other end. And a
*complete* FAA needs machinery Coro does not: simulating the `caller()` stack
across suspension, and a dedicated construct for asynchronous cancellation. The
net is that the core surface needed to underwrite FAA is *larger* than the one
needed to underwrite Coro — the opposite of the intuition. Both surfaces are
specified in *The proposal: publish the execution state as a core API* below, and
named throughout: **execstate** swaps a whole execution state, which is what a
stackful library needs, and **parkapi** freezes and thaws a region of the save
stack, which is what a stackless one needs. FAA needs parkapi, plus the `caller()`
simulation and the cancellation construct just described.

Nor does FAA solve any *technical* problem that Coro has. Both are single-core and
cooperative; neither adds parallelism. Both depend on interpreter internals;
neither is inherently stable across releases. And FAA is not even a superset: it
is the suspension *syntax*, not a scheduler, so it has no `cede`/`cede_slice`
equivalent of its own and leans on the event loop for *when* things run. What FAA
changes is the *trade*, not the ledger: it buys visible, explicit suspension
points and portability, at the cost of Coro's transparency. It cannot suspend
through a non-`async` or XS frame; it gives up dynamic-scope fidelity (`local`,
`$_` and friends need `dynamically`, or do not survive an `await`); and it
*colors* the call graph, so adopting it means an `async`/`await` rewrite up the
whole chain — where Coro is colorless. The trade runs the other way in two places.
One is tooling: Coro's private C stacks cost the cross-coro view — whole-program C
backtraces and clean profiling across coros — though a breakpoint in your own XS
still fires, and the Perl debugger, while usable, is not coro-aware; FAA sits on a
normal C stack. The other is explicitness: FAA's visible `await` points make every
suspension site visible at the call and *structurally* bar suspension where
`await` is not allowed (a `DESTROY`, say). Coro is still cooperative — the
programmer controls where yielding happens — but cede-ability is not visible at a
call site, and suspension in an un-checkable context like `DESTROY` cannot be
forbidden at parse time. None of this reads as "one is simpler"; it reads as two
different bargains.

So the two are not "hacky old" versus "clean new." They are stackful-transparent
versus stackless-explicit — each capable, each buying one property with another —
and neither makes the execution-state problem disappear. If anything the smaller,
more self-contained core commitment is the one the stackful model needs, and it
happens to enable the more capable (suspend-anywhere, scope-faithful) style. The
serious answer is to support both styles on one published foundation — execstate
for the stackful half, parkapi for the stackless — not to bet that one style's
syntax dissolves the other's requirements.

## Where the performance is: "Pure Perl orchestrates, XS executes"

To see what model Perl users actually want, look at where time is spent. The
Perl interpreter is, for tight numeric or byte-processing loops, one to several
orders of magnitude slower than C. The community learned long ago not to fight
this but to *arrange around it*: performance-critical work is pushed into **XS**
(or into C reached through `Inline::C`, `FFI::Platypus`, or an existing XS
module), while Perl itself does the gluing, control flow, configuration, and
I/O. PDL, `JSON::XS`, `Text::CSV_XS`, DBD drivers, the regex engine, and the
whole "\_XS" tail of CPAN exist for this reason.

The consequence for parallelism is decisive. The place where running on multiple
cores actually pays is the **XS/C** computation, not the pure-Perl orchestration.
Pure Perl spends most of its wall-clock either waiting on I/O (a concurrency
problem, not a parallelism one) or delegating to C. So the model most Perl
programs would benefit from is:

- **Pure Perl orchestrates** — one interpreter, single-threaded, using **green
  threads** to keep thousands of I/O flows and delegated computations in flight
  concurrently, and using **process pools** where coarse pure-Perl parallelism is
  genuinely needed;
- **critical-performance XS executes** — and, when a piece of XS is CPU-bound or
  makes a blocking syscall, it does so on a **real OS thread**, off the single
  interpreter thread, so it can either run on another core or simply not stall
  the scheduler.

This is precisely the pattern that works so well elsewhere. In Python, the
numeric stack (NumPy, and friends) *releases the GIL* around its C kernels: Python
stays single-threaded and safe, while the heavy C runs on OS threads and uses all
the cores. The Perl analogue is: keep one interpreter, let green threads schedule
the Perl side, and let blocking or CPU-bound XS run on OS threads. Threading *the
XS part* — not the Perl part — is what most people are really after.

### PDL: the pattern already shipped — and where it stops

PDL is the closest thing Perl has to NumPy, and it is worth looking at because it
demonstrates both halves of this split *and* the seam between them.

It demonstrates the orchestration half so thoroughly that it is packaged that way:
alongside its plain 32- and 64-bit editions, Strawberry Perl publishes a dedicated
**PDL edition** — an entire Perl distribution assembled around "Perl orchestrates,
XS executes", with the numeric stack and its supporting libraries prebuilt. That
is about as direct an endorsement of the model as an ecosystem can give.

It also already does the *parallel* half, without any help from perl. Built with
POSIX threads, `set_autopthread_targ` / `set_autopthread_size` split an
operation's implicit-loop dimensions across a pool of pthreads that execute only
generated C and never touch the interpreter. (Confusingly, PDL historically called
that implicit looping "threading"; it is now called *broadcasting*, precisely
because it is a loop-shape feature and not a CPU-count one. The pthread support is
the separate, genuinely parallel thing.)

What PDL does **not** do is release the interpreter. During a pthreaded operation
the calling perl thread sits inside the XS call holding the interpreter, so
nothing else in the program advances: a program that is otherwise happily running
thousands of green threads stops dead for the duration of a large PDL op. That is
exactly the gap the two *multicore* modes fill, both specified in *A companion: a
core multicore hook* below. In the **release/acquire bracket** the XS module hands
the interpreter to another OS thread for the duration of a pure-C section and takes
it back at the end; in **offload** the interpreter stays where it is and the work
moves to a worker thread instead. PDL is close to the ideal candidate for either:
for the ops that matter its parallel loop is already known not to touch perl data,
which is the hard precondition both need — a per-op property rather than a blanket
one.

The two mechanisms are orthogonal and compose: PDL's pthreads parallelise *within*
one operation, while multicore overlaps that operation with everything else the
interpreter has to do. Together they are the whole model.

Which of the two multicore modes to use is decided by the platform, and for PDL
the answer on Windows is the interesting one. The release/acquire bracket there
needs the non-ithreads build (see *Windows: the ithreads build is the obstacle*),
which the PDL edition — a Strawberry build — is not. **Offload does not**: it
never migrates the interpreter, so none of that applies. It is therefore the route
that leaves the shipped Windows distribution, and its whole prebuilt module set,
exactly as it is.

The distinction worth being precise about is between ithreads being *built* and
being *used*, and it is worth being careful about which sense of "thread" is
meant. Offload is indifferent to the build flags — an ithreads-enabled perl is
fine — and it is equally indifferent to *OS* threads, because the interpreter
stays on its own thread by construction; that is the whole point of the primitive.
Nothing here asks the program to stay on a particular thread.

What the existing backend cannot serve is a second **live interpreter** — a
limitation of that implementation rather than of the hook, as *Not inherently
single-interpreter* below sets out. Coro imposes the same restriction
independently of multicore, for its own reasons: use it from the first interpreter
only.

So the run-time constraint is not "recompile perl without ithreads", and not "keep
off other OS threads" — it is "do not start a second interpreter", i.e. leave
`threads->create` alone. That costs a PDL workload nothing, since its parallelism
comes from pthreads inside the C loop rather than from perl-level threads.

Offload also decouples PDL from Coro specifically. The release/acquire bracket
needs a suspended C frame to return into, so it serves the stackful model only;
offload serves the stackless one as well, and PDL is close to the best case for it
— for exactly the reason that made `done()` cheap above. Where a marshalling
module has to rebuild its result from the caller's argument stack (see the
FFI::Platypus appendix, where that is fatal under Future::AsyncAwait), a PDL
operation writes into an output ndarray that already exists as an SV before the
computation starts. Nothing has to be *produced* at resolution time, so nothing is
lost when the caller's frame is gone: `done()` needs only the transformation and
the output ndarray, both captured in the job at call time.

Getting there is real work, and of a different shape from the Coro case:

- **Retained lifetimes.** A suspended coro holds its ndarrays alive on its own
  stack; an awaiting FAA caller does not. The job has to retain every
  participating ndarray and release them in `done()`.
- **PDL is eager, so this is surgery, not a bracket.** `pdl_make_trans_mutual`
  runs the transformation inline unless dataflow is enabled. An async entry point
  needs a third path — set the transformation up, do *not* run it, return the
  deferred, run on a worker, resolve — and that path must compose with the existing
  dataflow branch, which is already a second deferral mechanism on the same object.
- **A mutation window the stackful path does not have.** While the worker reads the
  input buffers, a suspended coro cannot touch them; an FAA caller has not stopped
  and can modify its own inputs, in the same flow of control. That is a data race
  Coro structurally cannot produce, and it wants a mechanism rather than a
  documentation note — an in-flight flag on participating ndarrays that makes
  mutation fail loudly, in the spirit of the existing untouchable-data and
  transient-busy flags.

None of that is exotic: refcount discipline, one restructured decision, one new
guard. The point is that the *hard* part — delivering a result without the
caller's stack — is already how PDL works, so a well-implemented offload gets
both concurrency models rather than only Coro.

**What the Coro half took.** It has been done, on a PDL branch. Under a stackful
backend the eager path is not an obstacle: PDL takes the synchronous form of the
primitive, which waits for the handle before returning, so the calling green thread
is suspended for the duration and the transformation still returns with its data
computed. No third evaluation path is needed, and the job may stay on the frame.
The whole hook is one site — the forward call in `pdl__ensure_trans`, which becomes
work-plus-done around `readdata` — behind an eligibility check (declines an op that
declares itself unsafe to parallelise, and anything under a size threshold, since
the handshake costs a mutex, a condvar and an event-loop turn).

What took the work was everything the worker is not allowed to do:

- **Auto-pthreading composes rather than competes**, but only once the scratch
  allocation is hoisted out. PDL's fan-out sizes and allocates the per-pthread
  temporaries from inside the loop; doing that on a worker means allocating through
  perl. Sized and made physical before the hand-over, the in-loop pass finds them
  ready and allocates nothing — and then the op gets its cores *and* the
  interpreter stays free, which is the whole point of the model.
- **Deferred diagnostics have to travel home.** A pthread buffers its warnings for
  the thread that spawned it to replay; when that thread is the worker, the replay
  itself would call perl. They now travel back with the offload and are replayed
  once the caller is resumed. This path turned out never to have been exercised —
  neither by PDL's own suite nor by any shipping op — and carried three defects,
  including one that made an offloaded op with no fan-out hang.
- **Cancellation reaches the fan-out.** The backend's advisory flag is carried in
  thread-local state that the cast copies into the pthreads it spawns, so every
  thread of the fan-out polls the same word, and the poll sits in the broadcast
  loop. A stopped transformation reports it as an error and is marked as such,
  because PDL otherwise *re-runs* a pending transformation when it is destroyed —
  which quietly recomputed the whole thing while the exception was unwinding.

Measured on a 200-million-element reduction: the interpreter runs other green
threads throughout, four pthreads work under the offload, results match the inline
path exactly, and a cancellation 60ms in stops the work within 50ms. The hook costs
about 9 microseconds a call, of which the thread handshake — a mutex, a condvar,
the wakeup pipe and an event-loop turn — is about 7, and the handle the remaining
1.4: a hash with a few keys, two or three references, two method calls into perl
and a destructor. Against the smallest op the eligibility check lets through, a
1-million-element reduction at some 545 microseconds, the handle is a quarter of
one percent, and nothing smaller is offloaded at all.

None of this waits on core, in the sense that the mechanism does not need it.
Offload could reach PDL the way the release/acquire bracket already reaches CPAN
today — a vendored header and a rendezvous in `PL_modglobal` — and because the
header runs `work` then `done` inline when no backend is installed, a PDL so
patched behaves identically for everyone who has no backend loaded. What that
route gives up is the single authority for a contract still under change; see the
core-hook section for what that costs. What a PDL edition actually needs is a
backend and the patch; the core hook is the same neutrality argument made for the
bracket, not a precondition.

Which does place a caveat on the prototype described below: it puts the offload
hook in core proper, so *as prototyped* it does need a patched perl. Note which
fallback that gives and which it does not. A missing *backend* is handled for you —
the call runs inline, blocking but correct — but a missing *hook* is not: on an
unpatched perl the declarations are simply absent, so a consumer has to guard its
offload path at compile time, much as Coro::Multicore already guards its own. That
is an implementation choice about where the rendezvous lives, not a limit on what
is reachable from CPAN.

### Coro::Multicore

`Coro::Multicore` implements exactly this: it is the release/acquire backend for
the hook, coupling Coro's cooperative scheduler to a pool of real OS threads so
that **a blocking C section and the rest of the program run at the same time**.

A multicore-enabled XS module calls `perlinterp_release` before its blocking
section, and Coro::Multicore hands the interpreter to a pool thread, which resumes
the other coros there. The C section itself does not move: it carries on, on the
thread it was already running on, which is now a thread without an interpreter.
When it finishes, the module's matching `perlinterp_acquire` takes the interpreter
back and the coro continues. So the Perl level stays single-threaded and
cooperative — no shared-interpreter races — while the C level gains both I/O
concurrency and, for CPU-bound C kernels, real multi-core execution.

For this to be useful, individual XS modules opt in — marking the points at which
they may safely let the interpreter go while their C runs. Work
in this vein includes multicore-aware modes for database drivers (e.g. a
`DBD::SQLite` option), XML parsing (`XML::LibXML`), and generic foreign calls
(`FFI::Platypus`), each letting its blocking C run off the interpreter thread
under Coro::Multicore. The picture that results — Perl orchestrating, green
threads scheduling, and OS threads carrying the heavy XS — is the whole model in
one place.

What makes this approach attractive is *how little* it asks of the module.
Running a blocking XS routine on a separate OS thread is always possible in
principle, but the general route is invasive: the module has to be rewritten to
dispatch its C work to a thread pool and expose an asynchronous, callback- or
`Future`-returning interface — its synchronous API, and usually its internals,
must change. Coro::Multicore's release/acquire instead hands the interpreter to a
worker for the duration of the module's existing blocking call, so an ordinary
synchronous XS module can be upgraded with only a thin opt-in at its blocking
entry points — its API and its C code stay as they are. That is why the multicore
variants above are small adapters over unchanged modules rather than rewrites,
where offload — multicore's other mechanism, specified below — would demand that
same redesign.

There is a natural generalization worth flagging, in the same spirit as the rest
of this document: that opt-in need not be Coro-specific. What an XS module
declares — *"I am about to run a blocking or CPU-bound C section that does not
touch the interpreter; offer to run it off the scheduler thread"* — is precisely
Perl's analogue of CPython's `Py_BEGIN_ALLOW_THREADS` / `Py_END_ALLOW_THREADS`
bracket, which every C extension uses to release the interpreter around blocking
work, independent of any one concurrency library. Perl core could expose the same
neutral bracket as a public hook: the module marks its blocking C section against
the *core* API, and whatever **backend** is loaded — Coro::Multicore today, a
future core scheduler, or none — decides what to do: hand the interpreter to a
worker OS thread so the other green threads run there, or, absent any backend, run
the section inline and blocking exactly as today. Core would supply the convention
and the hook point, not the thread pool or the migration machinery, which stay in
the backend. Two constraints are inherent and worth stating: the bracketed section
must be **pure C that does not call into the interpreter** (the same discipline
`Py_BEGIN_ALLOW_THREADS` demands, since the rest of the program makes progress
while it runs); and Perl's backend is intrinsically harder than Python's — Python
releases the GIL and its other threads, which already exist with their own stacks,
proceed while the C carries on where it is; Perl has one interpreter and no such
threads waiting, so the backend has to move the interpreter itself and coordinate
handing it back. Done this way an XS module would be written once to be
multicore-friendly without depending on Coro at all — the same decoupling
execstate performs for the stackful context switch, applied to the blocking-C
boundary. It is a distinct, complementary proposal, not part of execstate,
developed as *A companion: a core multicore hook* below.

## How other languages resolve the same tensions

Every managed language faces the same three-way tension between shared memory,
parallelism, and safety, and the designs cluster into a few recognisable
answers. Perl's mechanisms are not exotic; they are points in this shared space.

- **JavaScript / Node.js.** A single-threaded event loop with stackless
  `async`/`await` (like FAA) for concurrency, and *no* shared-memory threads in
  the language: parallelism comes from Web Workers / `worker_threads`, which are
  separate heaps communicating by message passing (like Perl's process pools),
  with only `SharedArrayBuffer` as a narrow shared-memory escape hatch.
  Concurrency in-heap, parallelism out-of-heap.

- **Python.** The richest menu, and the closest parallel to Perl's situation.
  `threading` gives real OS threads that share memory but are serialised by the
  **GIL**, so they help I/O-bound code and not CPU-bound code — hence the
  release-the-GIL-in-C pattern described above. `multiprocessing` gives
  process-pool parallelism (Perl's fork pools). `asyncio` is stackless
  async/await (FAA/Node). `greenlet`/`gevent` are stackful green threads (Coro).
  And Python is now dismantling the central constraint: per-interpreter GILs
  (3.12) and an experimental free-threading, no-GIL build (3.13+) aim at genuine
  shared-memory parallel threads.

- **Go.** The design many hold up as the goal: **goroutines** are stackful green
  threads with small growable stacks, multiplexed **M:N** onto a pool of OS
  threads by the runtime's work-stealing scheduler. They give shared memory,
  cheap creation *and* real multi-core parallelism at once, with communication by
  channels (CSP). The cost is that this only works because the whole runtime and
  memory model were co-designed for it — the GC, the scheduler, and the stack
  layout all cooperate.

- **Erlang / BEAM.** Massive numbers of green *processes* that share *nothing* and
  communicate only by message; the scheduler is preemptive and multi-core. Safety
  by isolation rather than by locking — the opposite end from Go.

- **Java.** Long the canonical heavy-OS-thread, shared-memory, locked model;
  Project Loom's **virtual threads** now add Go-style M:N green threads on top of
  the same shared heap, letting old blocking code scale like async code.

Read against this map, Perl has strong instances of the *concurrency* answers
(Coro ≈ gevent/goroutine-shaped stackful; FAA ≈ asyncio/Node stackless) and of
*out-of-heap parallelism* (fork pools ≈ multiprocessing/workers), but its one
*in-heap parallel* mechanism, ithreads, took the shared-nothing clone route and
is discouraged. The realistic near-term target is therefore the Python/Node
consensus — single safe interpreter, green threads for concurrency, OS threads
under the XS for parallelism — rather than a Go-style rewrite of the runtime.

It is worth being explicit about why the *other* Python route — 3.13's
free-threading, no-GIL build — is not an option for Perl, because it is the
obvious "why not just remove the lock?" question and the answer is instructive.
Perl has **no GIL to remove**: it never had shared-memory interpreter threads in
the first place (ithreads *clone*), so the task is not to delete a lock but to
*build* a thread-safe shared interpreter from scratch — strictly harder than
CPython's job, not easier. That means making all of Perl's shared mutable
internals safe for concurrent access: the reference counts on every SV, the
magic/tie machinery, the pad system, the pervasive `PL_*` globals, and the
dynamic-scope (`local`/save-stack) model that is per-thread-of-execution but acts
on shared variables. PEP 703 needed exactly this class of work for CPython —
biased and deferred reference counting, per-object locks, thread-safe containers,
immortal objects, a reworked cycle collector — delivered over years by a large,
funded team, with a single-thread performance cost and a multi-release migration.
Perl has a fraction of that engineering capacity, arguably more entangled
internals, and — decisively — a vast, old, largely unmaintained XS ecosystem that
manipulates SV internals and refcounts with no notion of thread-safety; making
the *core* safe would not make that code safe, and there is no one to port it the
way CPython's top C extensions are being ported. And the payoff would be
misaligned even if it were free: a free-threaded interpreter parallelises
*pure-Perl* execution — the slow half one would rather delegate — whereas the
parallelism that actually pays, the XS/C kernels, is already reachable by running
XS on OS threads (the orchestrate/execute model above), the same
release-the-lock-in-C pattern Python itself leans on. So the cost is far higher
and the benefit far lower for Perl than for Python; the leverage is in threading
the XS, not in un-threading the interpreter.

## The stability problem with Coro and FAA

Both green-thread libraries work, and work well, but both are perennially at risk,
and for the same underlying reason: **they depend on interpreter state and
behaviour that core neither exposes as API nor promises to keep stable.**

Coro is the sharper case because it swaps the *whole* execution state. It reads
and writes the private `PL_*` execution registers directly; it overlays its
saved state on the context stack, which requires exact knowledge of `PERL_SI` and
`PERL_CONTEXT` layout; it clones padlists by hand, which the internal padlist
representation has broken more than once; and it must tear a coroutine down in an
exact order dictated by how perl's scope, context, and pad machinery interlock.
Every one of these is a place where a perl release can — and historically does —
change something private out from under it:

- the save/scope/pad **teardown order** is a hard contract that *tightened*
  between perl versions: unwinding in the wrong order leaves an outer frame's pad
  slot stale and corrupts refcounts, asserting or segfaulting in `leave_scope` on
  modern perls (a real, debugged corruption when a coro is cancelled while
  blocked with a `local` in scope);
- the save-stack `SS_MAXPUSH` accounting moved from header macros into
  `PL_savestack_max` in 5.24;
- the padlist representation changed across the `NEWPADAPI` and 5.15.3 reworks;
- and the low-level C-stack switch needs a hand-written assembly backend on some
  platforms (it is required on arm64), while the portable `setjmp`/`longjmp`
  backend is fragile enough that `Coro::Multicore` deadlocks on it.

FAA is more robust in scope — stackless, so it never touches the C stack or the
context stack — but it is fragile in kind for the same reason: it freezes and
thaws a region of the **save stack**, and the save stack's per-type layout is an
internal detail. Both libraries are, in effect, privately maintaining a fork of
perl's understanding of its own execution state, and re-validating it against
each release.

The result is a recurring tax: green threading in Perl is powerful but has a
reputation for breaking on new perls, needs per-version patching, and leans on a
single expert's continued attention. The fragility is not in the *idea* of green
threads; it is in the *coupling* to unpublished internals.

## The proposal: publish the execution state as a core API

The fix is to move the interpreter-internals knowledge out of the libraries and
into perl core, behind a small, additive, **public** API — so that a green-thread
library calls a supported function instead of reaching into `PL_*`, and core owns
(and keeps stable, and maintains next to the machinery it depends on) the
contract those libraries need. Concretely, two complementary APIs, matching the
two mechanisms:

- **execstate** — the *stackful* half, for Coro. A small ladder of operations on
  a thread's whole execution state, specified in `Porting/execstate_api.md`:
  1. **register snapshot** — a transparent `PerlExecState` plus `execstate_save`
     / `execstate_load` to copy the generic execution registers to and from the
     live interpreter;
  2. **fresh-stack lifecycle** — `execstate_init` / `execstate_unwind` /
     `execstate_destroy`, which own the delicate teardown order once and for all;
  3. **pad** — `execstate_derive_padlist` / `execstate_free_padlist` for
     re-entering a sub on an independent context;
  4. **transfer registers** — the `JMPENV` exception-handler state that must
     follow a C-stack switch (`execstate_topenv` and friends).

  Because it is a **capability ladder** gated by `PERL_EXECSTATE_LEVEL`, core can
  adopt it incrementally: it advertises how far up it implements, and a consumer
  backfills only the levels core lacks. Deliberately *out* of the API is the
  machine-level C-stack switch itself — that is a pluggable, platform-specific
  mechanism (the assembly/`ucontext`/`setjmp` backends) and rightly stays the
  library's own; only the interpreter *state* that a switch must carry is
  core's concern.

- **parkapi** — the *stackless* half, for FAA and similar: a supported way to
  freeze and thaw a save-stack region, so that async/await libraries stop
  depending on the internal per-type save-stack layout. This half is now
  **prototyped**: the freeze/thaw/frozen-free/run-deferred/foreach-sv entry
  points cover the value-localization, pad-scope, deferred-action *and* magical
  (`%ENV`/`%SIG`, whole and per-element) save types, and a prototype migration of
  Future::AsyncAwait onto it — replacing its hand-written per-`SAVEt_` switch —
  passes FAA's own test suite, including `local %ENV`/`%SIG` across an `await`.
  See `Porting/savestack_suspend_api.md` for the specification.

An honest note on their relative cost, since it cuts against the usual
intuition. The stackful model, often dismissed as the hacky one, needs the
*smaller* core API; the stackless model, usually held up as the clean modern
design, needs the *larger* one. execstate is compact — a fixed register list, a
mechanical save/load, a short lifecycle, all copies and pointer swaps.
parkapi must understand the save stack *per save type*, freezing and thawing each
`SAVEt_*` correctly, which is inherently more code and more coupling to internal
semantics; and a *complete* stackless implementation needs still more that has no
stackful analogue — it must **simulate the `caller()` stack** across suspension
(a stackful coro keeps it for free), and **asynchronous cancellation cleanup** is
not even solved yet: no shipped FAA mechanism provides it, only the proposed
`Future::With` approach (not a CPAN module; RT #171969), where a stackful coro's
`safe_cancel` gets blocking cleanup for free by unwinding its real dynamic scope.
So underwriting FAA is the heavier commitment of the two — a larger, more
internals-coupled core piece (parkapi) plus machinery Coro simply does not need —
even though execstate is the one attached to the more contentious library.

Both are designed to be **purely additive and ABI-safe**: `PerlExecState` is a
standalone struct a consumer overlays on its own memory; the operations copy or
walk existing state rather than changing any representation; no existing
interpreter variable or struct is altered. Adopting them removes nothing and
risks nothing for code that does not use them, while turning the green-thread
libraries' most dangerous dependency — private, drifting internals — into a
stable surface that core tests and maintains. The historically version-sensitive
pieces (the teardown order, the `SS_MAXPUSH` accounting, the padlist handling)
become core's responsibility, living beside the very code whose changes used to
break Coro, so that the next such change updates the API in lock-step instead of
silently breaking a CPAN module.

## A companion: a core multicore hook

Separate from execstate, but in the same spirit, is a second, much smaller core
addition: promoting the *Perl Multicore Specification*'s release/acquire bracket
(see the Coro::Multicore discussion above) from a bundled CPAN header into core.
It is included here because it is prototyped alongside execstate and rounds out
the "thread the XS, not the interpreter" model — but it stands entirely on its
own and could land independently.

**The API.** A core `perlmulticore.h` gives an XS module the same three tokens
it uses today, now core-provided:

```c
perlinterp_release ();      /* offer to run the following pure-C section off the */
do_the_blocking_C_thing (); /* interpreter thread; touch no perl data here       */
perlinterp_acquire ();
```

plus `perlmulticore_active()` and a registration entry point,
`multicore_register(release, acquire)`, for a backend. With no backend installed
the bracket is a nop and the section runs inline and blocking, exactly as today.

Alongside the bracket the API carries its dual, the **offload** primitive. Where
the bracket migrates the interpreter to a worker while the blocking call stays on
the caller's thread, offload keeps the interpreter *pinned* and moves the **work**
to a worker:

```c
SV *handle = multicore_offload (work, work_arg,  /* pure C, on a worker   */
                                done, done_arg); /* holds the interpreter */
```

`work` is pure C and touches no interpreter; `done` runs holding the interpreter
when the work finishes and marshals the C result into an SV.

What comes back is a **handle** supplied by the backend, and the work may still be
running when it does. One shape, whatever the backend, is the point: a backend
decides how completion is delivered — a stackful one (Coro) suspends the green
thread that asks for the value, a stackless one (Future::AsyncAwait) resolves on
its loop — and if that choice reached the module's return type, then offloading an
existing synchronous method would change what that method returns as soon as an
FAA backend was loaded, invisibly and at a distance. With one shape the module
returns the handle without naming its class, and the caller decides: `await
$handle` from an `async sub`, or `$handle->get`, which blocks — transparently
under Coro, since it suspends only the calling green thread. The handle implements
the `AWAIT_*` protocol of `Future::AsyncAwait::Awaitable`. That protocol asks only
that the object answer to a set of method names — nothing has to inherit from
anything, and neither core nor a backend has to depend on `Future`, which is why a
plain `Future` and a backend's own class are interchangeable here.

A module that wants the value rather than the handle — which is what a synchronous
method needs — calls `multicore_offload_sync` instead, and the wait happens inside
it. That is the whole of what an offload was before the handle existed, and it is
what PDL uses.

The core primitive is neutral about delivery: it forwards work/done and hands back
the backend's handle, so a consumer needs *no* knowledge of Coro, FAA, Coro's
rouse callbacks, or Future. Registered by a backend with
`multicore_register_offload(fn)`; with none, `work` then `done` run inline and the
value comes back in an already-resolved handle core supplies
(`PerlMulticore::Handle`), so the shape holds even with nothing installed. A
module that already has an answer — a cached result, an input too small to be
worth a worker — wraps it with `multicore_offload_ready` for the same reason.

**Lifetime.** `work_arg`, and whatever `work` writes into, must stay valid until
the handle *resolves*, which is not the same as until the call returns. Since a
portable consumer cannot tell whether the handle it got is pending, the rule is a
biconditional: keep the job on your frame if and only if you wait on the handle
before returning. `multicore_offload_sync` is that first case. A module that
returns the handle upward heap-allocates its job and lets `done` release it — which
is why `done` runs exactly once for every offload, including when the handle is
dropped mid-flight (`done_ctx.dropped` says so, and the value is discarded). That
last point was not in the design until PDL's asynchronous entry point was written
against it, and leaked its job on precisely that path.

Both callbacks are also handed a small context by the backend, which is what
carries **advisory cancellation**: `work` polls a flag and may return early, and
`done` is told that cancellation was *requested*. Only requested — the backend
cannot see whether the work acted on it, since `work` returns nothing, and a
request that arrives while the last chunk runs produces a whole result. So it is
the module that knows, and the contract asks it to **raise** rather than return a
truncated result its caller could not tell apart from a complete one:
`multicore_offload_cancelled` builds the exception (`PerlMulticore::Cancelled`) so
that every module raises the same one. Advisory is the most that can be offered,
since C code cannot be interrupted from outside.

The handle offers cancellation in two forms, differing in what happens to the
interpreter while the work stops: a prompt one that blocks until it has, so that
the offload is over when the call returns, and an asynchronous one
(`safe_cancel`, optional) that hands back an awaitable completing at that point —
which is what a stackless caller needs, since blocking its thread would stop the
loop the completion arrives on.

A backend that abandons a call in flight — an interrupted wait, or a pending handle
dropped by a caller that wants neither the value nor the work — must wait for the
worker before letting the caller's frame go; the flag is what keeps that wait
short. A `work` that never polls is therefore not merely uninterruptible: it makes
an interrupted caller wait out the whole operation.

Offload is a *separate* hook, not an extension of the release/acquire struct
(whose two-pointer layout is a frozen ABI); a pool-owning backend can register
both.

**What that costs, and what it need not have cost.** Because the bracket's
rendezvous is a `PL_modglobal` struct of two `void (*)(void)` pointers, a module
and a backend find each other there with no help from the interpreter, which is
why the bracket reaches unpatched perls today. Offload as prototyped rendezvouses
*inside* the interpreter instead, so it exists only on a perl built with it: a
consumer must guard its offload path at compile time and keep an inline fallback,
and a backend compiles its offload half out — which is what Coro::Multicore and
PDL both do. A missing *backend* is still handled for the consumer (the work runs
inline), but a missing *hook* is not.

That is a choice about where the rendezvous lives, and it could be made the other
way: a `PL_modglobal` struct with a function pointer, a vendored header, an inline
fallback — the bracket's design exactly, needing nothing from the interpreter.

What core supplies, and that route gives up, is one authority for the contract.
The bracket needs no authority. Its contract is two `void (*)(void)` pointers,
frozen, so vendoring the file verbatim is the whole of the agreement, and it cannot
drift. Offload's contract is callback contexts with `size` fields, a handle
protocol, cancellation rules and an ABI number, and it is still changing. Two
vendored copies that disagree about the layout or the key do not fail to link.
They misread each other's memory.

Two lesser costs come with that route. Each consumer would carry its own class for
the handle a backendless call hands back. That is harmless: the contract asks for a
set of methods, never for a class name, so a caller cannot tell one consumer's
class from another's. The second cost is that the synchronous wrapper's most
delicate part would be copied into every consumer rather than fixed in one place —
it releases the handle through a scope guard, so that the job is given up properly
even when asking the handle for the value raises an exception, or when the green
thread waiting on it is cancelled outright.

One apparent way out does not work: shipping the shared pieces as a CPAN
distribution. The backendless path exists for the user who has installed nothing,
so it cannot depend on an installation.

*PDL: the pattern already shipped* shows the same trade from a consumer's side.

Offload's reach is broader than the bracket's, at a higher cost to the module.
Because the worker never touches the interpreter and the interpreter never moves,
offload serves **both** the stackful (Coro) and stackless (Future::AsyncAwait)
models, and it works on **every platform, Windows included** — none of the
interpreter-migration constraints that limit the bracket apply (see *Portability:
on Windows* below). The price is **deeper changes to the XS module**, plus a
**function-colouring** boundary the module must own. It restructures the blocking
operation into a pure-C `work()` (inputs marshalled out of SVs beforehand) and a
`done()` that marshals the result back — and, because a *stackless* caller cannot
receive a value synchronously, a module that wants to serve one must expose the
offloaded form as an explicitly **asynchronous** entry point (e.g. a `..._async`
method returning the handle), distinct from the plain synchronous call. The
synchronous one is not lost: it calls `multicore_offload_sync` and keeps returning
a value, which under Coro is the transparent behaviour. So a generic XS module has
to *add* an async twin for each offloadable method — the price of colouring, paid
once per method rather than by changing what the existing one returns. The bracket
remains the near-transparent upgrade for existing synchronous Unix+Coro modules,
and offload is the portable, model-agnostic path for code written — or rewritten —
for it.

**Not inherently single-interpreter.** Nothing in the offload design ties it to one
interpreter. The worker never touches perl, so it has no interpreter to be wrong
about, and the registration side is already per-interpreter by construction: the
hook struct lives in `PL_modglobal`, which ithreads clones along with everything
else. Several live interpreters could therefore each register a backend and drive
their own pool, or agree to share one.

The existing backend does not, however. Coro::Multicore holds its job pool,
wake-up pipe, enable flags and cached `Coro::API` pointer in process-wide C
statics, so a second live interpreter finds either no backend registered at all
or — if the module is loaded there too — one pool and one pipe shared between two
interpreters. For practical purposes this costs nothing: ithreads is discouraged
for the reasons given earlier, and Coro independently requires the first
interpreter, so this is a corner nobody should be trying to occupy. It is worth
recording only because the limitation belongs to the *backend* rather than to the
hook — which would matter if core ever shipped a default backend of its own.

The whole core surface is a short header, two interpreter-global hooks, and a
handful of tiny functions — the hard machinery (the worker-thread pool, the
handing of the interpreter between threads) stays entirely in the backend.

**Backward compatible by construction.** The prototype keeps the *exact wire ABI*
of the deployed CPAN `perlmulticore.h`: the two hooks live in a struct in
`PL_modglobal["perl_multicore_api"]` with the field names `pmapi_release` /
`pmapi_acquire`, and core caches a pointer to that same struct. So everyone meets
at one rendezvous, and nothing downstream needs changing: an already-installed
**Coro::Multicore drives a module built against core's header** (it writes the
struct core reads), and a core-`multicore_register` backend drives existing
bundled-header modules (it writes the struct they read). In particular
Coro::Multicore itself needs **no update** — indeed it should keep its bundled
header so it still works on perls that predate the core one. (A test installs a
backend the CPAN way — writing the struct directly, as Coro::Multicore's BOOT
does — and confirms core's bracket drives it.)

**What the gain is — and is not, for the bracket.** Everything in this subsection
is about the release/acquire half; offload is assessed separately below, and comes
out differently. For the bracket, honestly, this is a modest, mostly
ecosystem-level win, and it is worth being clear about its shape:

- It adds **no new capability, no performance, and no behaviour** — the bracket
  already works via the bundled header, and it still does nothing without a
  backend installed.
- The tangible benefit is that a new module uses a **core-provided, canonical,
  neutral header** instead of vendoring schmorp's copy — and that the convention
  becomes a *documented Perl capability* rather than "a CPAN header you copy in,"
  decoupled from Coro. That legitimacy/neutrality is the real point, the same one
  execstate makes.
- Even the "stop vendoring" benefit is **deferred**: a module supporting perls
  older than the one that first ships the header must keep a bundled fallback
  until that perl is its minimum, so the practical saving arrives years later.
- A genuine *future* upside it unlocks: because the hook is core's, core could one
  day ship a **default backend** (even a minimal thread pool), so the bracket does
  something useful out of the box without any CPAN backend at all.

So for the bracket the stakes are far smaller than execstate's — a small vendored
header versus the fragile, version-sensitive interpreter internals — but the move
is the same: put the neutral convention in core so modules can depend on it
without depending on Coro, and let perl own the contract.

**For offload the gain is not modest, because there is nothing to canonicalise.**
The bracket existed on CPAN before core had an opinion; offload does not exist
anywhere else. As prototyped the hook *is* the capability: without it a module has
no way to hand a C section to a worker thread and get an answer back in a shape
that works under either concurrency model, and the stackless world has no route to
multi-core blocking XS that does not go through Coro. What is modest here is the
*core surface* — one function pointer, four small functions, an extensible context
struct and a short perl class — not what it buys. The debit is the one named
above: a consumer needs a perl that carries it, where a bracket consumer needs
nothing. If offload were ever moved out to a `PL_modglobal` rendezvous like the
bracket's, this paragraph would come to read like the one before it.

**Portability: on Windows, offload — not the bracket.** The transparent
release/acquire bracket works by *migrating the interpreter across OS threads* —
the blocking call stays put while a worker thread picks up the interpreter and
resumes other coros. On Unix that is fine. On Windows what stands in the way is
the *standard build* rather than the platform: the obstacles are properties of the
ithreads-enabled perl that everyone ships there, and a non-ithreads build removes
the documented ones — see *Windows: the ithreads build is the obstacle* below,
which reports what is measured there, including Win64 exception unwinding.

The **offload** primitive is the way out that needs none of that, for exactly the
reason given where it is introduced above: the interpreter never migrates, so no
Windows constraint touches it, and the deeper XS restructuring it asks for is the
price paid once for code that then runs on every platform and under both models.
One Windows-specific point to add: since `fork` under a live worker pool is
hazardous anyway, such a build should make Perl's emulated `fork` fail fast while
multicore is enabled rather than let it clone into the pool. This is why the core
hook is deliberately two-pronged — the transparent bracket where the platform and
build allow interpreter migration, offload where they do not.

## Windows: the ithreads build is the obstacle

It is tempting to read "Coro::Multicore does not work on Windows" as a statement
about Windows. It is mostly a statement about the perl that Windows ships. Worth
separating, because one of the two obstacles is removable and the other is not
what it is usually taken to be.

**What ithreads costs here.** Every mainstream Windows perl — Strawberry,
ActivePerl — is built `useithreads=define usemultiplicity=define`, and that build
also enables `PERL_IMPLICIT_SYS`, which routes even ISO-C calls such as `malloc`
and `setjmp` through the `PerlHost` layer and so requires a `dTHX` to be in scope
for them. That is the concrete referent of Marc Lehmann's "win32 perls are beyond
fixing" comment at the top of `Multicore.xs`: not the operating system, but the
implicit-sys indirection that the threaded build turns on. Separately, ithreads
is *also* what supplies Windows' emulated `fork` — an interpreter cloned onto
another thread of the same address space — which collides with a worker pool.
Both are properties of the build.

**The no-ithreads alternative.** `win32/GNUmakefile` exposes `USE_MULTI`,
`USE_ITHREADS` and `USE_IMP_SYS` as three independent switches (each defaulting
to `undef` when commented out, with `USE_MULTI` auto-enabled by either of the
others). With all three off the picture inverts: `PERL_SET_CONTEXT` degenerates
to assigning `PL_curinterp`, `dTHXa` expands to nothing, and the implicit-sys
objection disappears entirely. The interpreter becomes one set of plain globals —
the *simplest possible* substrate for handing it between OS threads, simpler than
Unix-with-ithreads. The emulated `fork` disappears too, which removes the pool
collision rather than merely mitigating it.

All three off is the cleanest form, but only `USE_IMP_SYS` has to go for the
implicit-sys objection specifically: the build actually measured below keeps
`USE_MULTI=define` and turns off just `USE_ITHREADS` and `USE_IMP_SYS`, which is
enough to drop `PERL_IMPLICIT_SYS` from `ccflags` and the emulated `fork` with it,
while retaining `MULTIPLICITY`. Worth stating because it is the weaker and hence
easier variant to package.

The consequence is a packaging one, not a technical one. Dropping the flags
changes `archname` — with all three off the `-multi-thread` suffix goes away
entirely; in the `USE_MULTI`-retaining variant above it becomes
`MSWin32-x64-multi`, losing only `-thread` — so such a perl cannot
load XS modules compiled for the threaded one. That sounds like a wall only if one
imagines each user rebuilding CPAN by hand. The natural answer is the one
Strawberry already applies to every other archname-affecting flag: ship it as an
**edition**. Strawberry's 32-bit, 64-bit, 64-bit-integer and long-double builds are
already mutually binary-incompatible, each with its own prebuilt module set, and
the PDL edition shows the project is willing to assemble a distribution around a
particular use case. A non-ithreads edition is the same kind of variant, and the
rebuild is done once by whoever assembles it rather than by anyone who installs
it.

That reframes the cost honestly. It is not "you lose the binary ecosystem"; it is
"somebody has to build a second one, and users then pick the edition matching what
they need" — a real but bounded, one-off distribution effort, of exactly the kind
this ecosystem already absorbs routinely.

**What still has to hold.** With the build objection gone, the remaining
requirements are not about perl at all:

- **The coroutine backend must survive thread migration.** The bracket parks a
  worker thread's machine context in the released coro's saved-context slot and
  resumes it later from whichever thread next runs the scheduler. Coro switches
  stacks through libcoro, which offers a backend per mechanism; the two that
  matter on Windows are `CORO_ASM`, handcoded assembler, and `CORO_FIBER`, built
  on the Win32 fiber API. A `CORO_ASM` context is a bare stack pointer, so the
  slot can be reused that way, and its Windows path maintains the TIB stack bounds
  (`StackBase`/`StackLimit`) on every switch — on whichever thread performs the
  switch, which is exactly the property needed. `CORO_FIBER` cannot: its transfer
  records the current fiber only when the slot is empty, so a worker's identity is
  never stored, and a later switch would target a fiber another thread is still
  executing, which Windows forbids outright. Built with `CORO_FIBER`,
  `Coro::Multicore` faults inside `SwitchToFiber` → `TlsGetValue` the first time a
  pool thread tries to resume a coro. (That crash establishes the outcome; the
  slot-empty mechanism above is not separately verified.) Since Windows' *default*
  libcoro backend is not `CORO_ASM`, this is a deliberate build choice rather than
  something obtained for free.
- **Small portability gaps in the backend.** `Coro::Multicore` calls
  `pthread_atfork` directly and unguarded (`Multicore.xs`), which is meaningless on
  a fork-less perl; on mingw-w64 winpthreads *provides* it, so it links and runs.
  Its `pthread_sigmask` calls compile to nothing on Win32 (`xthread.h` defines them
  away) — silently dropping the "worker threads never handle perl signals"
  discipline the surrounding code relies on.
- **Win64 exception handling.** A croak raised while a worker runs perl on a coro
  stack has to unwind to that worker's own `JMPENV`, and on Win64 `longjmp`
  participates in SEH-based unwinding. Unwinding to a `jmp_buf` living on a
  different stack from the one executing is not something the ABI contemplates.
  Measured behaviour is under *Status* below.

**Status.** Non-ithreads plus `CORO_ASM` is the only combination in which the
transparent bracket is possible on Windows; `CORO_FIBER` cannot support it.

The `CORO_ASM` amd64 path needs work before it runs on Windows at all — both to
compile under perl's win32 `ccflags` and to survive the first transfer into a
freshly created context. Two further repairs were needed in `Coro::Multicore`
itself. All of them are Coro-side and carried in that branch; the detail belongs
with it rather than here.

Measured on the **non-ithreads** build — perl from this branch with
`USE_ITHREADS=undef` `USE_IMP_SYS=undef` (`archname` `MSWin32-x64-multi`), built
with Strawberry's winlibs GCC 13.2.0 / mingw-w64 UCRT toolchain — Coro's suite
under `CORO_ASM` runs 238 tests across 24 files with no subtest failures, matching
the `CORO_FIBER` baseline. The single failing file is `t/22_preempt.t`, which
cannot pass on Win32 for an unrelated reason (`Time::HiRes::alarm` is unimplemented
there). `Coro::Multicore` loads and passes its shipped tests. The ithreads build is
unverified; nothing here speaks to it.

The bracket's effect is measurable. Four coros each holding a 0.40s released sleep
complete in 0.49s where serial execution would take 1.60s; with release suppressed
the same work takes 1.66s. That 3.38× ratio isolates the transparent bracket as the
source of the concurrency.

Win64 SEH unwinding holds. `Win32::GetCurrentThreadId` shows the scenario is
genuinely reachable: coros run *perl* on distinct pool threads (main 7732; coros on
264, 1684, 10256), each staying on its thread across the release. Raising
exceptions there — a die caught on the coro stack, an `eval` straddling the
release, three of six concurrent coros throwing, `local()` correctly restored after
a die across a release, and nested `eval`s both unwinding — passes six for six with
no crash.

One behaviour of the offload backend, not Windows-specific:

- **`Coro::Multicore` runs `done` wherever the completion is noticed**, which is
  neither a fixed OS thread nor the green thread that issued the offload. There are
  three call sites: the event-loop callback that drains the wakeup pipe, on the
  normal path; the handle's destructor, when a pending handle is dropped; and
  `cancel`, when a caller stops the work promptly. The issuing green thread is not
  one of them — it may not be waiting, or may not exist any more.

  Which OS thread that turns out to be depends on the *other* mechanism. Driving
  `ext/XS-APItest`'s work/done pair through this backend, `work_ran_off_thread`
  is 1 either way, while `done_ran_on_main` is 1 with offload alone — the
  interpreter never leaves the calling thread, so the loop runs there — and 0 once
  a release/acquire bracket is in flight, because then another OS thread holds the
  interpreter and runs the loop. Core's own test backend gives 1, resolving before
  it returns.

  `perlmulticore.h` states the guarantee accordingly: `done` runs *holding the
  interpreter*, on whichever OS thread owns it at the time and in whichever green
  thread noticed. A module may therefore use the perl API freely in `done`, but
  neither thread nor green-thread identity - no thread-local state established
  before the call, no OS handle only the calling thread may touch, and nothing that
  assumes the caller's coroutine is still there.

Windows ARM64 is half-answered. The backend side is ready — libcoro has the arm64
Windows path, written but unexercised — while the toolchain side is not:
`win32/GNUmakefile` has no arm64 case, so ARM64 means MSVC, which has no GCC-style
inline assembly, which forces the fiber backend. A Strawberry ARM64 edition would
change that, since it implies both a mingw-style arm64 toolchain and the makefile
support to use it.

So the recommendation, with a better reason than "Windows is hopeless":
**offload for portability**, because it asks nothing of the coroutine backend and
nothing of the packaging; **non-ithreads plus `CORO_ASM`** if the transparent
bracket on Windows is worth a second edition, because it is the only road there.
The bracket is measured, so the cost of that second road is packaging.

One qualification: offload asks *almost* nothing of the build. It did ask one
thing. Core's own offload tests were compiled out on Windows, because
`win32/config_H.gc` left `I_PTHREAD` undefined and `ext/XS-APItest` gates its
thread-pool test backend on that symbol. `<pthread.h>` is present on mingw-w64 and
links without help, and every other core use of `I_PTHREAD` is gated on
`USE_ITHREADS`, so the win32 config now defines it: inert for a non-ithreads
build, and enough to make the offload path testable there.

## An honest assessment: the social obstacle

The hardest part of this proposal is not technical. A clean, additive API is
straightforward to specify and, as the accompanying implementation shows, to
build. The real obstacle is social and historical, and it would be dishonest to
present the plan without saying so.

Green threading in Perl has long sat inside a documented, years-long friction
between the author of Coro (and of EV, AnyEvent, and much of the surrounding
high-performance stack) and perl5-porters. The recurring substance was backward
compatibility: internal changes in core repeatedly broke downstream modules that
relied on them, and the two sides drew opposite lessons — that core should treat
such breakage as a regression to be avoided, versus that modules reaching into
unpublished internals had taken on a risk core never promised to underwrite.
The tone of the exchanges, on both sides, hardened the disagreement into lasting
distrust. The practical residue is a de facto position that core would not go
out of its way to accommodate Coro, and a matching reluctance on the Coro side to
route its needs through the core process at all.

The honest reading of the incentives *today* is that **neither party is actually
asking for this integration**:

- The Coro author, by his own public statements, does not see a core API as the
  remedy: his position is that core should simply stop breaking things, and he
  maintains an independent stack (his own coroutine backend, event loop, and
  glue) precisely so as not to depend on core's follow-through. He has also
  largely stepped back from tracking new perl releases. A plan whose success
  depends on his sustained engagement with p5p is unlikely to find it.
- perl5-porters, for their part, have little appetite to add APIs that expose
  execution-state internals in order to support a design a number of porters
  consider unsupportable in principle. There is no standing champion for it, and
  its association with the old conflict makes it easy to decline.

Both positions are understandable, and neither is wholly wrong. The porters'
concern is legitimate: unpublished internals *are* a real maintenance hazard, and
blessing one consumer's needs as API carries genuine long-term cost. The Coro
author's frustration is also legitimate: he shipped working, widely used software
and watched it broken by changes he did not initiate. The deadlock is as much
about trust and misaligned incentives as about any line of code — which is why it
has outlasted every purely technical rebuttal from either direction.

What follows from this is not that the proposal is hopeless, but that it cannot
lean on either party's advocacy and must be framed and carried accordingly:

- **As a general capability, not "Coro support."** The API is worth having on its
  own terms: it is not Coro-specific code but the *stackful*-switch primitive
  itself — what constitutes a switchable execution state, and how to tear one
  down correctly — which any stackful green-thread or continuation runtime could
  consume, Coro being merely the one that exists today. It encodes a piece of
  interpreter knowledge core arguably ought to own regardless of who consumes it.
  (Its stackless counterpart is parkapi, which makes the same case on the
  async/await side: the two are siblings, not one facility. FAA does *not* switch
  stacks and does not use execstate — so the honest general claim here is
  "any stackful runtime," not "async/await too.") Presented as "a stable
  execution-state API" it stands on its merits; presented as "make core support
  Coro" it almost certainly does not.
- **At zero cost to the uninvolved.** Being purely additive and ABI-safe (see
  above) is not only good engineering but the political precondition: it lets
  porters say yes without taking on any obligation to a consumer, any change to
  existing behaviour, or any new breakage surface.
- **Carried by a neutral party.** The core-side work, and its upkeep across
  releases, needs an owner who is neither re-litigating the old dispute nor
  waiting on the other side to change posture. The knowledge encoded is Marc
  Lehmann's and is credited as such; *carrying* it into core and keeping it
  correct has to be someone else's standing commitment.

In short: the technical case is strong and the code is small, but the proposal
should be advanced with clear eyes — as a modest, self-justifying core facility
that happens to rescue an important use case, owned by someone prepared to
maintain it without waiting for a reconciliation that may never come.

## Coexisting with Futures and IO::Async

Publishing the state does not pick a winner between the stackful and stackless
styles; it strengthens both, and they interoperate. A stackful coro and a
`Future`-based, event-loop world are complementary, not rival:

- Coro integrates with event loops through `Coro::AnyEvent`, so a coro that
  "blocks" (on a semaphore, a channel, a socket) actually cedes to the underlying
  loop and lets other work proceed; through AnyEvent this reaches any supported
  loop, `IO::Async` included, and there is also direct Coro↔`IO::Async` bridging
  for programs that want the loop without the AnyEvent layer.
- Because a coro can suspend anywhere, it can **await a `Future`** by ceding until
  the future is ready and then returning its value — giving synchronous-looking,
  suspend-anywhere Perl code on top of an asynchronous, `Future`-returning API.
  Conversely FAA *is* `Future`-native: `await` consumes a `Future` directly.

So the same program can use `IO::Async` (or any AnyEvent loop) as its I/O engine,
`Future`s as the currency of pending results, FAA's `async`/`await` where a
stackless state machine is the natural shape, and Coro where suspend-anywhere
stackful threads (or unmodified blocking-style code, or `Coro::Multicore`'s
OS-thread offload) are wanted — all in one interpreter, all sharing memory. A
stable core execution-state API is what lets the stackful side of that picture
stop being a maintenance hazard.

### Worked example: blocking XS as a Future, off the interpreter thread

The three pieces — FAA, Coro::Multicore, and the multicore hook — compose into
something genuinely useful: an `async sub` can consume a blocking XS call
*asynchronously and off the interpreter thread*, without that module ever growing
a Future-based API. A generic wrapper does it:

```perl
sub on_worker (&) {
    my ($code) = @_;
    my $f = $loop->new_future;          # a Future tied to the shared loop
    Coro::async {
        my @r = eval { $code->() };     # blocking, multicore-enabled XS runs here
        $@ ? $f->fail ($@) : $f->done (@r);
    };
    return $f;
}

# in an async sub:
my $rows = await on_worker { $sth->execute (@args); $sth->fetchall_arrayref };
```

The spawned coro runs the blocking call; because a multicore-enabled module
brackets its blocking section with `perlinterp_release`/`perlinterp_acquire`,
Coro::Multicore hands the interpreter to a worker OS thread for the duration, so
the loop and the awaiting `async sub` keep running there while the blocking call
carries on where it was. When it returns, the module takes the interpreter back,
the coro resolves the Future and the loop resumes the awaiter.

Why this is attractive, and the one real constraint:

- It avoids the *large* change (rewriting the XS to a callback/Future API) and
  needs only the *small* one (the release/acquire bracket), which the
  multicore-enabled modules already carry — so the whole set of them becomes
  async-consumable from FAA through this single wrapper. - Unlike the process-pool
  route (`IO::Async::Function`), the hand-off is *shared-memory*: large arguments
  and results pass by reference, not serialised across a process boundary. - The
  constraint: the code run this way must be genuinely thread-safe (the multicore
  hard rule), and the program pulls in the whole Coro + Coro::Multicore stack —
  the stackless world leaning on the stackful one to get thread offload. It is,
  notably, the *safe* way to mix the two: the coro is only a thread that lets the
  interpreter go while it runs a plain blocking call, and FAA meets it at the
  Future — no deep
  interleaving of the two suspension models.

### Offload to a Future without Coro

That wrapper uses a coro only as something Coro::Multicore can release the
interpreter around.
The clean end state removes even that, and the core hook already provides the
primitive it needs: `multicore_offload(work, work_arg, done, done_arg)` (above)
runs a pure-C `work` on a worker thread and, when it finishes, runs `done` holding
the interpreter to marshal the result into an SV — **with no caller
suspension**. A thin FAA backend registers a hook that creates its handle,
resolves it from `done`'s SV, and hands it back for the caller to `await`:

```c
/* an FAA offload backend, in outline */
static SV *faa_offload (work, work_arg, done, done_arg) {
    SV *f = new_future ();       /* a Future is already an awaitable          */
    /* enqueue work on the pool; on completion, on the interp thread:
     *     SV *r = done (aTHX_ done_arg, &ctx);   $f->done(r);               */
    return f;                    /* the handle multicore_offload returns      */
}
```

That the handle it returns is a `Future` and Coro::Multicore's is a class of its
own does not reach the module: the contract fixes the `AWAIT_*` protocol, and a
`Future` satisfies it already.

With an offload backend installed (a thread pool), FAA runs blocking,
multicore-enabled XS off the interpreter thread with **no coroutine library in
sight**: the program stays pure stackless `async`/`await` on its event loop, the
hook carries the work to a worker and the completion resolves the Future. The
price is the one offload always asks: the C section has to be exposed as the
`work`/`done` pair the primitive takes, not merely bracketed. (With no backend,
`multicore_offload` runs `work` and `done` inline and hands back a handle that is
already resolved — correct, blocking — just like the release/acquire bracket.)

That is what the neutral multicore hook ultimately points at: the *stackless*
world getting multi-core blocking-XS on its own terms, with a stackful runtime
being an implementation detail of *one possible* backend rather than a required
dependency. The core side of this exists: `multicore_offload` /
`multicore_register_offload` alongside the bracket, with two test backends in
`ext/XS-APItest` — one that joins its worker before returning, confirming `work`
runs on another OS thread and `done` holding the interpreter, and one that defers,
which is the shape a stackless backend has and which exercises the pending path.
No FAA adapter is needed: the handle contract is the `AWAIT_*` protocol, and a
plain `Future` satisfies it as it stands, which that second backend demonstrates by
handing back `Future` instances and resolving them. What remains outside core is a
real stackless backend — a thread pool that resolves on the loop. Coro::Multicore
is the stackful one, and works today; the offload primitive is what would let FAA
use multicore without Coro at all.

Not every consumer can take this path, though. See the appendix on FFI::Platypus
for a concrete module where offload → Future dead-ends — its result delivery is
welded to the synchronous XSUB stack — even as offload still earns its keep for
that module by giving it Windows multicore under Coro.

## Summary

- A thread is an independently scheduled flow of control that shares its process's
  memory and is cheap to manage; concurrency is intrinsic to the idea, multi-core
  parallelism is a separate, valuable bonus.
- Perl gets parallelism today mainly from **processes** (fork pools, and CPAN
  frameworks over them) and, awkwardly, from **ithreads** (clone-per-thread, no
  real sharing, discouraged); it gets cheap in-heap **concurrency** from green
  threads — **Coro** (stackful, suspend-anywhere) and **FAA** (stackless
  async/await).
- FAA is not the "simpler" answer that retires Coro: it depends on the same class
  of internals, needs a *larger* core surface (parkapi + `caller()` simulation),
  still has no async cancellation (only the proposed `Future::With`, RT #171969,
  which Coro's `safe_cancel` already covers), is not itself a scheduler (no
  `cede`/`cede_slice`), forces function "coloring" (`async`/`await` propagates up
  the whole call graph, and `local` becomes `dynamically`), and gives up Coro's
  suspend-anywhere transparency and dynamic-scope fidelity (`local`, `$_`,
  `wantarray`). Coro's honest debits in return are tooling (no cross-coro C
  backtraces or clean C profiling — though XS breakpoints still fire, and the Perl
  debugger is usable if not coro-aware) and call-site visibility (Coro is
  cooperative — the programmer controls where yielding happens — but a call does
  not advertise whether it may `cede`, and suspension cannot be forbidden in an
  un-checkable context such as `DESTROY`). The two are a trade —
  stackful-transparent vs stackless-explicit — not an upgrade; core should support
  both on one foundation, not bet that async/await syntax dissolves the problem.
- Because performance-critical Perl work lives in **XS**, the model most programs
  want is *Pure Perl orchestrates, critical XS executes*: one interpreter, green
  threads for I/O concurrency, process pools for coarse parallelism, and OS
  threads carrying the heavy XS — which is exactly what `Coro::Multicore` and
  opt-in multicore XS modules provide, and exactly the release-the-lock-in-C
  pattern other languages already rely on.
- The green-thread libraries are fragile only because they depend on private,
  drifting interpreter internals. The proposal is to publish those internals as a
  small, additive, ABI-safe core API — **execstate** (stackful) and **parkapi**
  (stackless) — so that the fragility becomes core's well-tested responsibility
  and green threading in Perl becomes something one can rely on.
- The chief obstacle is social, not technical: a long history of friction has left
  neither the Coro author nor perl5-porters advocating for this. The plan must
  therefore stand as a general, self-justifying, zero-cost core facility — not as
  "Coro support" — and be carried by a neutral owner willing to maintain it
  without waiting on a reconciliation.

See `Porting/execstate_api.md` for the execstate API specification and its
implementation plan, and `Porting/savestack_suspend_api.md` for the parkapi
specification.

## Appendix: the FFI::Platypus + FAA dead end (and why offload still pays off)

The *Offload to a Future without Coro* section above sketches the clean end
state: a generic blocking-XS module hands its C work to `multicore_offload`, an
FAA backend resolves a Future from `done`'s SV, and the stackless world gets
multi-core blocking-XS with no coroutine in sight. Trying to realise this for
**FFI::Platypus** — the obvious canonical consumer, since it turns *any* C
function into a Perl sub — mapped out exactly where that path holds and where it
dead-ends. The short version: it works for purpose-written async XS, but a
generic *marshalling* module like FFI cannot get *transparent* FAA support from
it — while the same offload hook still buys FFI a real win under Coro, notably on
Windows.

### Where it breaks: result delivery is bound to the synchronous XSUB frame

FFI::Platypus does not so much *return* a value as *push* one. Its result
marshalling is a large return-type switch built on `XSRETURN_UV` / `XSRETURN_IV`
/ `XSRETURN_NV` / `XSRETURN_EMPTY` and direct `ST(0)` assignment — macros that
set the XSUB's mortal `TARG`, write the argument stack, and `return` out of the C
function. All three ingredients — the argument stack (`ST(n)`), the XSUB's
`TARG`, and the C frame you return from — exist only for the duration of the
synchronous call.

That is fatal to the offload → Future model, whose whole point is that `done()`
runs **later**, on the event loop, after the XSUB has already returned a Future.
By then there is no stack to push onto and no frame to return from: `XSRETURN`
cannot be used from `done()` at all. The result has to be *produced* as a plain
`SV *` that `done()` returns, not *returned* off the stack.

The scalar/void return is, on its own, fixable: factor the switch into a producer
`SV *ffi_pl_result_to_sv(pTHX_ self, result)` that `newSV`s the value instead of
pushing it, and have `done()` return that SV. Simple by-value-in, scalar-out
functions would then work under FAA.

### Why the general case does not recover

The return value is only half of FFI's delivery. It also **writes back output
arguments** — `int *`, arrays, in-out records — into the *caller's* `@_` SVs
after the call, and **re-enters Perl** for custom types (type coderefs run during
conversion). Both are bound to the caller's synchronous frame. Under Coro that
frame is merely suspended and still present, so the write-back and re-entry just
work when the coro resumes. Under FAA the caller's `@_` is *gone* by the time
`done()` runs. Making it work means marshalling every caller-side SV out of the
transient stack into a heap-allocated job with retained refcounts, performing the
write-back and re-entry in `done()` against those retained SVs, and freeing them
there — a deep rewrite of the call path that must get lifetime and refcounting
exactly right, for a payoff that is *still* a **coloured** API (a distinct
`call_async` returning a Future; `await` required).

So the dead end is specific. It is not "offload cannot serve FAA"; it is that **a
generic marshalling module whose result delivery is entangled with the
synchronous XSUB stack cannot get *transparent* FAA support from offload.** The
transparency the clean-end-state sketch implies never materialises for
FFI::Platypus. What is achievable is a scoped, explicitly-async subset
(scalar/void return, no out-params) — useful, but not the drop-in the sketch
suggested.

### What still pays off: FFI + offload under Coro, including Windows

The offload hook is not wasted on FFI. Under **Coro** the same primitive gives
FFI the *transparent* form for free, through the synchronous wrapper:
`multicore_offload_sync` queues the work, suspends the calling coro until it is
over, marshals the result inline (on the still-present frame — no `XSRETURN`
problem, and the frame is still there precisely because the wrapper waited), and
returns the value. `$sub->(...)` stays an ordinary synchronous call while other
coros keep running.

Crucially, offload does this **where the bracket does not reach: Windows as
shipped.** The release/acquire bracket works by *migrating the interpreter* between
OS threads, which Coro's default Windows backend cannot do and which the standard
ithreads build obstructs besides (see *Windows: the ithreads build is the
obstacle*); offload needs none of that and keeps the interpreter
pinned and moves only the C work, so a multicore FFI call runs off the
interpreter thread on Windows too. For FFI, that portability — not the FAA story
— is the concrete win of the offload hook.

### The general lesson

Offload → Future is clean for XS modules that already produce their *entire*
result inside a `done`-style callback: pure C work in, one SV out, no dependence
on the caller's stack (the `scramble_async` worked example is exactly this
shape). It breaks for modules whose result delivery is welded to the synchronous
XSUB stack — `XSRETURN`, `TARG`, output-argument write-back — of which
FFI::Platypus is the archetype. The offload primitive therefore delivers "FAA
multi-core without Coro" for the well-behaved case and for purpose-written async
XS, but not universally: stack-bound result delivery, layered on top of function
colouring, is the boundary.

There is a second qualifying shape, and for numeric work it matters more than the
first: modules that deliver their result **in place**, into a buffer that already
exists before the computation starts. Then there is nothing to produce at
resolution time at all, and independence from the caller's stack comes by
construction rather than by careful factoring. PDL is the archetype — see *PDL:
the pattern already shipped — and where it stops* — which is why the
Future::AsyncAwait story there is considerably better than this appendix's own
subject would suggest. The boundary is stack-bound *delivery*, not offload, and
not FAA.

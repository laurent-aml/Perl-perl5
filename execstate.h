/*    execstate.h
 *
 *    Interpreter execution-state API ("execstate") - a proposal; see
 *    Porting/execstate_api.md.
 *
 *    A green-thread execution state is the mutable per-thread interpreter
 *    execution registers - the value / mark / scope / save / temporaries
 *    stacks, the execution position, the compile-time cursors and the execution
 *    flags - that a stackful coroutine library must swap between.  This header
 *    is the single list of those registers, as one X-macro, from which the
 *    (transparent) PerlExecState struct is generated; Perl_execstate_save /
 *    Perl_execstate_load (scope.c) copy them to/from the live interpreter.  It
 *    is transparent (not opaque) because a consumer such as Coro overlays it on
 *    its own memory.
 *
 *    It deliberately EXCLUDES the policy set of per-thread dynamic globals a
 *    green-thread library may also swap ($_, @_, $@, $/, select, %^H, the %SIG
 *    hooks) and PL_mainstack: which of those are per-green-thread is the
 *    consumer's design choice, not an interpreter fact.
 *
 *    This register list is derived from Coro/state.h by Marc A. Lehmann
 *    <schmorp@schmorp.de> (http://software.schmorp.de/pkg/Coro.html) and is used
 *    under the same terms as perl.  Knowing exactly which interpreter registers
 *    make up a switchable execution context is his contribution.
 */

#ifndef PERL_EXECSTATE_H_
#define PERL_EXECSTATE_H_

#ifdef DEBUGGING
#  define PERL_EXECSTATE_SLOT_SSNAME(SLOT) \
     SLOT(scopestack_name, PL_scopestack_name, const char **)
#else
#  define PERL_EXECSTATE_SLOT_SSNAME(SLOT)
#endif

/* The generic execution registers, as SLOT(member, live_lvalue, ctype). */
#define PERL_EXECSTATE_SLOTS(SLOT)                                             \
  SLOT(stack_sp,            PL_stack_sp,            SV **)                      \
  SLOT(op,                  PL_op,                  OP *)                       \
  SLOT(curpad,              PL_curpad,              SV **)                      \
  SLOT(stack_base,          PL_stack_base,          SV **)                      \
  SLOT(stack_max,           PL_stack_max,           SV **)                      \
  SLOT(scopestack,          PL_scopestack,          I32 *)                      \
  SLOT(scopestack_ix,       PL_scopestack_ix,       I32)                        \
  SLOT(scopestack_max,      PL_scopestack_max,      I32)                        \
  PERL_EXECSTATE_SLOT_SSNAME(SLOT)                                             \
  SLOT(savestack,           PL_savestack,           ANY *)                     \
  SLOT(savestack_ix,        PL_savestack_ix,        I32)                        \
  SLOT(savestack_max,       PL_savestack_max,       I32)                        \
  SLOT(tmps_stack,          PL_tmps_stack,          SV **)                      \
  SLOT(tmps_ix,             PL_tmps_ix,             SSize_t)                    \
  SLOT(tmps_floor,          PL_tmps_floor,          SSize_t)                    \
  SLOT(tmps_max,            PL_tmps_max,            SSize_t)                     \
  SLOT(markstack,           PL_markstack,           Stack_off_t *)             \
  SLOT(markstack_ptr,       PL_markstack_ptr,       Stack_off_t *)             \
  SLOT(markstack_max,       PL_markstack_max,       Stack_off_t *)             \
  SLOT(curpm,               PL_curpm,               PMOP *)                     \
  SLOT(curcop,              PL_curcop,              COP *)                      \
  SLOT(curstack,            PL_curstack,            AV *)                       \
  SLOT(curstackinfo,        PL_curstackinfo,        PERL_SI *)                  \
  SLOT(sortcop,             PL_sortcop,             OP *)                       \
  SLOT(sortstash,           PL_sortstash,           HV *)                       \
  SLOT(localizing,          PL_localizing,          U8)                         \
  SLOT(in_eval,             PL_in_eval,             U8)                         \
  SLOT(tainted,             PL_tainted,             bool)                       \
  SLOT(compcv,              PL_compcv,              CV *)                       \
  SLOT(comppad,             PL_comppad,             PAD *)                      \
  SLOT(comppad_name,        PL_comppad_name,        PADNAMELIST *)             \
  SLOT(comppad_name_fill,   PL_comppad_name_fill,   PADOFFSET)                  \
  SLOT(comppad_name_floor,  PL_comppad_name_floor,  PADOFFSET)                  \
  SLOT(runops,              PL_runops,              runops_proc_t)              \
  SLOT(hints,               PL_hints,               U32)                        \
  SLOT(parser,              PL_parser,              yy_parser *)

struct PerlExecState {
#define PERL_EXECSTATE_MEMBER(name, lval, type) type name;
  PERL_EXECSTATE_SLOTS(PERL_EXECSTATE_MEMBER)
#undef PERL_EXECSTATE_MEMBER
};
typedef struct PerlExecState PerlExecState;

/* Capability ladder (see Porting/execstate_api.md): core implements the API up
 * to PERL_EXECSTATE_LEVEL and a consumer backfills any level below that -
 *   1  register snapshot     (PerlExecState, execstate_save / execstate_load)
 *   2  fresh-stack lifecycle (execstate_init / execstate_unwind / execstate_destroy)
 *   3  pad derive / free     (execstate_derive_padlist / execstate_free_padlist)
 *   4  transfer registers    (execstate_topenv / _restartop / _topenv_reset / _topenv_root)
 * The machine-level C-stack switch itself is deliberately NOT part of the API:
 * it is a pluggable, platform-specific mechanism and thus the consumer's own. */
#define PERL_EXECSTATE_LEVEL 4

/* Legacy boolean, for consumers predating the level scheme (counts as level 1). */
#define PERL_EXECSTATE 1

/* Level 4: the JMPENV "registers" of a context transfer are plain interpreter
 * state, exposed as lvalue aliases (the function macros for the other levels are
 * generated from embed.fnc).  execstate_topenv_root() walks to the base handler. */
#define execstate_topenv          PL_top_env
#define execstate_restartop       PL_restartop
#define execstate_topenv_reset()  (PL_top_env = &PL_start_env)

#endif /* PERL_EXECSTATE_H_ */

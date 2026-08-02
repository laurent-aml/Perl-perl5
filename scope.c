/*    scope.c
 *
 *    Copyright (C) 1991, 1992, 1993, 1994, 1995, 1996, 1997, 1998, 1999, 2000,
 *    2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008 by Larry Wall and others
 *
 *    You may distribute under the terms of either the GNU General Public
 *    License or the Artistic License, as specified in the README file.
 *
 */

/*
 * For the fashion of Minas Tirith was such that it was built on seven
 * levels...
 *
 *     [p.751 of _The Lord of the Rings_, V/i: "Minas Tirith"]
 */

/* This file contains functions to manipulate several of Perl's stacks;
 * in particular it contains code to push various types of things onto
 * the savestack, then to pop them off and perform the correct restorative
 * action for each one. This corresponds to the cleanup Perl does at
 * each scope exit.
 */

#include "EXTERN.h"
#define PERL_IN_SCOPE_C
#include "perl.h"
#include "feature.h"
#include "perlmulticore.h"     /* full hook API: struct body + Perl_multicore_* */

SV**
Perl_stack_grow(pTHX_ SV **sp, SV **p, SSize_t n)
{
    SSize_t extra;
    SSize_t current = (p - PL_stack_base);

    PERL_ARGS_ASSERT_STACK_GROW;

    if (UNLIKELY(n < 0))
        croak(
            "panic: stack_grow() negative count (%" IVdf ")", (IV)n);

    PL_stack_sp = sp;
    extra =
#ifdef STRESS_REALLOC
        1;
#else
        128;
#endif
    /* If the total might wrap, panic instead. This is really testing
     * that (current + n + extra < Stack_off_t_MAX), but done in a way that
     * can't wrap */
    if (UNLIKELY(   current         > Stack_off_t_MAX - extra
                 || current + extra > Stack_off_t_MAX - n
    ))
        /* diag_listed_as: Out of memory during %s extend */
        croak("Out of memory during stack extend");

    av_extend(PL_curstack, current + n + extra);
#ifdef PERL_USE_HWM
        PL_curstackinfo->si_stack_hwm = current + n + extra;
#endif

    return PL_stack_sp;
}

#ifdef STRESS_REALLOC
#define GROW(old) ((old) + 1)
#else
#define GROW(old) ((old) * 3 / 2)
#endif

/* for backcomp */
PERL_SI *
Perl_new_stackinfo(pTHX_ I32 stitems, I32 cxitems)
{
    return new_stackinfo_flags(stitems, cxitems, 0);
}

/* current flag meanings:
 *   1 make the new arg stack AvREAL
 */

PERL_SI *
Perl_new_stackinfo_flags(pTHX_ I32 stitems, I32 cxitems, UV flags)
{
    PERL_SI *si;
    Newx(si, 1, PERL_SI);
    si->si_stack = newAV();
    if (!(flags & 1))
        AvREAL_off(si->si_stack);
    av_extend(si->si_stack, stitems > 0 ? stitems-1 : 0);
    AvALLOC(si->si_stack)[0] = &PL_sv_undef;
    AvFILLp(si->si_stack) = 0;
#ifdef PERL_RC_STACK
    si->si_stack_nonrc_base = 0;
#endif
    si->si_prev = 0;
    si->si_next = 0;
    si->si_cxmax = cxitems - 1;
    si->si_cxix = -1;
    si->si_cxsubix = -1;
    si->si_type = PERLSI_UNDEF;
    Newx(si->si_cxstack, cxitems, PERL_CONTEXT);
    /* Without any kind of initialising CX_PUSHSUBST()
     * in pp_subst() will read uninitialised heap. */
    PoisonNew(si->si_cxstack, cxitems, PERL_CONTEXT);
    return si;
}

I32
Perl_cxinc(pTHX)
{
    const IV old_max = cxstack_max;
    const IV new_max = GROW(cxstack_max);
    Renew(cxstack, new_max + 1, PERL_CONTEXT);
    cxstack_max = new_max;
    /* Without any kind of initialising deep enough recursion
     * will end up reading uninitialised PERL_CONTEXTs. */
    PoisonNew(cxstack + old_max + 1, new_max - old_max, PERL_CONTEXT);
    return cxstack_ix + 1;
}

/*
=for apidoc_section $callback
=for apidoc push_scope

Implements L<perlapi/C<ENTER>>

=cut
*/

void
Perl_push_scope(pTHX)
{
    if (UNLIKELY(PL_scopestack_ix == PL_scopestack_max)) {
        const IV new_max = GROW(PL_scopestack_max);
        Renew(PL_scopestack, new_max, I32);
#ifdef DEBUGGING
        Renew(PL_scopestack_name, new_max, const char*);
#endif
        PL_scopestack_max = new_max;
    }
#ifdef DEBUGGING
    PL_scopestack_name[PL_scopestack_ix] = "unknown";
#endif
    PL_scopestack[PL_scopestack_ix++] = PL_savestack_ix;

}

/*
=for apidoc_section $callback
=for apidoc pop_scope

Implements L<perlapi/C<LEAVE>>

=cut
*/

void
Perl_pop_scope(pTHX)
{
    const I32 oldsave = PL_scopestack[--PL_scopestack_ix];
    LEAVE_SCOPE(oldsave);
}

Stack_off_t *
Perl_markstack_grow(pTHX)
{
    const I32 oldmax = PL_markstack_max - PL_markstack;
    const I32 newmax = GROW(oldmax);

    Renew(PL_markstack, newmax, Stack_off_t);
    PL_markstack_max = PL_markstack + newmax;
    PL_markstack_ptr = PL_markstack + oldmax;
    DEBUG_s(DEBUG_v(PerlIO_printf(Perl_debug_log,
            "MARK grow %p %" IVdf " by %" IVdf "\n",
            PL_markstack_ptr, (IV)*PL_markstack_ptr, (IV)oldmax)));
    return PL_markstack_ptr;
}

void
Perl_savestack_grow(pTHX)
{
    const I32 by = PL_savestack_max - PL_savestack_ix;
    Perl_savestack_grow_cnt(aTHX_ by);
}

void
Perl_savestack_grow_cnt(pTHX_ I32 need)
{
    /* NOTE: PL_savestack_max and PL_savestack_ix are I32.
     *
     * This makes sense when you consider that having I32_MAX items on
     * the stack would be quite large.
     *
     * However, we use IV here so that we can detect if the new requested
     * amount is larger than I32_MAX.
     */
    const IV new_floor = PL_savestack_max + need; /* what we need */
    /* the GROW() macro normally does scales by 1.5 but under
     * STRESS_REALLOC it simply adds 1 */
    IV new_max         = GROW(new_floor); /* and some extra */

    /* the new_max < PL_savestack_max is for cases where IV is I32
     * and we have rolled over from I32_MAX to a small value */
    if (new_max > I32_MAX || new_max < PL_savestack_max) {
        if (new_floor > I32_MAX || new_floor < PL_savestack_max) {
            croak("panic: savestack overflows I32_MAX");
        }
        new_max = new_floor;
    }

    /* Note that we add an additional SS_MAXPUSH slots on top of
     * PL_savestack_max so that SS_ADD_END(), SSGROW() etc can do
     * a simper check and if necessary realloc *after* apparently
     * overwriting the current PL_savestack_max. See scope.h.
     *
     * The +1 is because new_max/PL_savestack_max is the highest
     * index, by Renew needs the number of items, which is one
     * larger than the highest index. */
    Renew(PL_savestack, new_max + SS_MAXPUSH + 1, ANY);
    PL_savestack_max = new_max;
}

#undef GROW

/*  The original function was called Perl_tmps_grow and was removed from public
    API, Perl_tmps_grow_p is the replacement and it used in public macros but
    isn't public itself.

    Perl_tmps_grow_p takes a proposed ix. A proposed ix is PL_tmps_ix + extend_by,
    where the result of (PL_tmps_ix + extend_by) is >= PL_tmps_max
    Upon return, PL_tmps_stack[ix] will be a valid address. For machine code
    optimization and register usage reasons, the proposed ix passed into
    tmps_grow is returned to the caller which the caller can then use to write
    an SV * to PL_tmps_stack[ix]. If the caller was using tmps_grow in
    pre-extend mode (EXTEND_MORTAL macro), then it ignores the return value of
    tmps_grow. Note, tmps_grow DOES NOT write ix to PL_tmps_ix, the caller
    must assign ix or ret val of tmps_grow to PL_temps_ix themselves if that is
    appropriate. The assignment to PL_temps_ix can happen before or after
    tmps_grow call since tmps_grow doesn't look at PL_tmps_ix.
 */

SSize_t
Perl_tmps_grow_p(pTHX_ SSize_t ix)
{
    SSize_t extend_to = ix;
#ifndef STRESS_REALLOC
    SSize_t grow_size = PL_tmps_max < 512 ? 128 : PL_tmps_max / 2;
    if (extend_to > SSize_t_MAX - grow_size - 1)
        /* trigger memwrap message or fail allocation */
        extend_to = SSize_t_MAX-1;
    else
        extend_to += grow_size;
#endif
    Renew(PL_tmps_stack, extend_to + 1, SV*);
    PL_tmps_max = extend_to + 1;
    return ix;
}


void
Perl_free_tmps(pTHX)
{
    /* XXX should tmps_floor live in cxstack? */
    const SSize_t myfloor = PL_tmps_floor;
    while (PL_tmps_ix > myfloor) {      /* clean up after last statement */
        SV* const sv = PL_tmps_stack[PL_tmps_ix--];
#ifdef PERL_POISON
        PoisonWith(PL_tmps_stack + PL_tmps_ix + 1, 1, SV *, 0xAB);
#endif
        if (LIKELY(sv)) {
            SvTEMP_off(sv);
            SvREFCNT_dec_NN(sv);		/* note, can modify tmps_ix!!! */
        }
    }
}

/*
=for apidoc save_scalar_at

A helper function for localizing the SV referenced by C<*sptr>.

If C<SAVEf_KEEPOLDELEM> is set in in C<flags>, the function returns the input
scalar untouched.

Otherwise it replaces C<*sptr> with a new C<undef> scalar, and returns that.
The new scalar will have the old one's magic (if any) copied to it.
If there is such magic, and C<SAVEf_SETMAGIC> is set in in C<flags>, 'set'
magic will be processed on the new scalar.  If unset, 'set' magic will be
skipped.  The latter typically means that assignment will soon follow (I<e.g.>,
S<C<'local $x = $y'>>), and that will handle the magic.

=for apidoc Amnh ||SAVEf_KEEPOLDELEM
=for apidoc Amnh ||SAVEf_SETMAGIC

=cut
*/

STATIC SV *
S_save_scalar_at(pTHX_ SV **sptr, const U32 flags)
{
    SV * osv;
    SV *sv;

    PERL_ARGS_ASSERT_SAVE_SCALAR_AT;

    osv = *sptr;
    if (flags & SAVEf_KEEPOLDELEM)
        sv = osv;
    else {
        sv  = (*sptr = newSV_type(SVt_NULL));
        if (SvTYPE(osv) >= SVt_PVMG && SvMAGIC(osv))
            mg_localize(osv, sv, cBOOL(flags & SAVEf_SETMAGIC));
    }

    return sv;
}

void
Perl_save_pushptrptr(pTHX_ void *const ptr1, void *const ptr2, const int type)
{
    dSS_ADD;
    SS_ADD_PTR(ptr1);
    SS_ADD_PTR(ptr2);
    SS_ADD_UV(type);
    SS_ADD_END(3);
}

SV *
Perl_save_scalar(pTHX_ GV *gv)
{
    SV ** const sptr = &GvSVn(gv);

    PERL_ARGS_ASSERT_SAVE_SCALAR;

    if (UNLIKELY(SvGMAGICAL(*sptr))) {
        PL_localizing = 1;
        (void)mg_get(*sptr);
        PL_localizing = 0;
    }
    save_pushptrptr(SvREFCNT_inc_simple(gv), SvREFCNT_inc(*sptr), SAVEt_SV);
    return save_scalar_at(sptr, SAVEf_SETMAGIC); /* XXX - FIXME - see #60360 */
}

/*
=for apidoc save_generic_svref

Implements C<SAVEGENERICSV>.

Like save_sptr(), but also SvREFCNT_dec()s the new value.  Can be used to
restore a global SV to its prior contents, freeing new value.

=cut
 */

void
Perl_save_generic_svref(pTHX_ SV **sptr)
{
    PERL_ARGS_ASSERT_SAVE_GENERIC_SVREF;

    save_pushptrptr(sptr, SvREFCNT_inc(*sptr), SAVEt_GENERIC_SVREF);
}


/*
=for apidoc save_rcpv

Implements C<SAVERCPV>.

Saves and restores a refcounted string, similar to what
save_generic_svref would do for a SV*. Can be used to restore
a refcounted string to its previous state. Performs the 
appropriate refcount counting so that nothing should leak
or be prematurely freed.

=cut
 */
void
Perl_save_rcpv(pTHX_ char **prcpv) {
    PERL_ARGS_ASSERT_SAVE_RCPV;
    save_pushptrptr(prcpv, rcpv_copy(*prcpv), SAVEt_RCPV);
}

/*
=for apidoc save_freercpv

Implements C<SAVEFREERCPV>.

Saves and frees a refcounted string. Calls rcpv_free()
on the argument when the current pseudo block is finished.

=cut
 */
void
Perl_save_freercpv(pTHX_ char *rcpv) {
    PERL_ARGS_ASSERT_SAVE_FREERCPV;
    save_pushptr(rcpv, SAVEt_FREERCPV);
}


/*
=for apidoc_section $callback
=for apidoc save_generic_pvref

Implements C<SAVEGENERICPV>.

Like save_pptr(), but also Safefree()s the new value if it is different
from the old one.  Can be used to restore a global char* to its prior
contents, freeing new value.

=cut
 */

void
Perl_save_generic_pvref(pTHX_ char **str)
{
    PERL_ARGS_ASSERT_SAVE_GENERIC_PVREF;

    save_pushptrptr(*str, str, SAVEt_GENERIC_PVREF);
}

/*
=for apidoc_section $callback
=for apidoc save_shared_pvref

Implements C<SAVESHAREDPV>.

Like save_generic_pvref(), but uses PerlMemShared_free() rather than Safefree().
Can be used to restore a shared global char* to its prior
contents, freeing new value.

=cut
 */

void
Perl_save_shared_pvref(pTHX_ char **str)
{
    PERL_ARGS_ASSERT_SAVE_SHARED_PVREF;

    save_pushptrptr(str, *str, SAVEt_SHARED_PVREF);
}


/*
=for apidoc_section $callback
=for apidoc save_set_svflags

Implements C<SAVESETSVFLAGS>.

Set the SvFLAGS specified by mask to the values in val

=cut
 */

void
Perl_save_set_svflags(pTHX_ SV* sv, U32 mask, U32 val)
{
    dSS_ADD;

    PERL_ARGS_ASSERT_SAVE_SET_SVFLAGS;

    SS_ADD_PTR(sv);
    SS_ADD_INT(mask);
    SS_ADD_INT(val);
    SS_ADD_UV(SAVEt_SET_SVFLAGS);
    SS_ADD_END(4);
}

/*

=for apidoc_section $GV

=for apidoc save_gp

Saves the current GP of gv on the save stack to be restored on scope exit.

If C<empty> is true, replace the GP with a new GP.

If C<empty> is false, mark C<gv> with C<GVf_INTRO> so the next reference
assigned is localized, which is how S<C< local *foo = $someref; >> works.

=cut
*/

void
Perl_save_gp(pTHX_ GV *gv, I32 empty)
{
    PERL_ARGS_ASSERT_SAVE_GP;

    /* XXX For now, we just upgrade any coderef in the stash to a full GV
           during localisation.  Maybe at some point we could make localis-
           ation work without needing the upgrade.  (In which case our
           callers should probably call a different function, not save_gp.)
     */
    if (!isGV(gv)) {
        assert(isGV_or_RVCV(gv));
        (void)CvGV(SvRV((SV *)gv)); /* CvGV does the upgrade */
        assert(isGV(gv));
    }

    save_pushptrptr(SvREFCNT_inc(gv), GvGP(gv), SAVEt_GP);

    if (empty) {
        GP *gp = Perl_newGP(aTHX_ gv);
        HV * const stash = GvSTASH(gv);
        bool isa_changed = 0;

        if (stash && HvHasENAME(stash)) {
            if (memEQs(GvNAME(gv), GvNAMELEN(gv), "ISA"))
                isa_changed = TRUE;
            else if (GvCVu(gv))
                /* taking a method out of circulation ("local")*/
                mro_method_changed_in(stash);
        }
        if (GvIOp(gv) && (IoFLAGS(GvIOp(gv)) & IOf_ARGV)) {
            gp->gp_io = newIO();
            IoFLAGS(gp->gp_io) |= IOf_ARGV|IOf_START;
        }
        GvGP_set(gv,gp);
        if (isa_changed) mro_isa_changed_in(stash);
    }
    else {
        gp_ref(GvGP(gv));
        GvINTRO_on(gv);
    }
}

AV *
Perl_save_ary(pTHX_ GV *gv)
{
    AV * const oav = GvAVn(gv);
    AV *av;

    PERL_ARGS_ASSERT_SAVE_ARY;

    if (UNLIKELY(!AvREAL(oav) && AvREIFY(oav)))
        av_reify(oav);
    save_pushptrptr(SvREFCNT_inc_simple_NN(gv), oav, SAVEt_AV);

    GvAV(gv) = NULL;
    av = GvAVn(gv);
    if (UNLIKELY(SvMAGIC(oav)))
        mg_localize(MUTABLE_SV(oav), MUTABLE_SV(av), TRUE);
    return av;
}

HV *
Perl_save_hash(pTHX_ GV *gv)
{
    HV *ohv, *hv;

    PERL_ARGS_ASSERT_SAVE_HASH;

    save_pushptrptr(
        SvREFCNT_inc_simple_NN(gv), (ohv = GvHVn(gv)), SAVEt_HV
    );

    GvHV(gv) = NULL;
    hv = GvHVn(gv);
    if (UNLIKELY(SvMAGIC(ohv)))
        mg_localize(MUTABLE_SV(ohv), MUTABLE_SV(hv), TRUE);
    return hv;
}

void
Perl_save_item(pTHX_ SV *item)
{
    SV * const sv = newSVsv(item);

    PERL_ARGS_ASSERT_SAVE_ITEM;

    save_pushptrptr(item, /* remember the pointer */
                    sv,   /* remember the value */
                    SAVEt_ITEM);
}

void
Perl_save_bool(pTHX_ bool *boolp)
{
    dSS_ADD;

    PERL_ARGS_ASSERT_SAVE_BOOL;

    SS_ADD_PTR(boolp);
    SS_ADD_UV(SAVEt_BOOL | (*boolp << 8));
    SS_ADD_END(2);
}

void
Perl_save_pushi32ptr(pTHX_ const I32 i, void *const ptr, const int type)
{
    dSS_ADD;

    SS_ADD_INT(i);
    SS_ADD_PTR(ptr);
    SS_ADD_UV(type);
    SS_ADD_END(3);
}

void
Perl_save_int(pTHX_ int *intp)
{
    const int i = *intp;
    UV type = ((UV)((UV)i << SAVE_TIGHT_SHIFT) | SAVEt_INT_SMALL);
    int size = 2;
    dSS_ADD;

    PERL_ARGS_ASSERT_SAVE_INT;

    if (UNLIKELY((int)(type >> SAVE_TIGHT_SHIFT) != i)) {
        SS_ADD_INT(i);
        type = SAVEt_INT;
        size++;
    }
    SS_ADD_PTR(intp);
    SS_ADD_UV(type);
    SS_ADD_END(size);
}

void
Perl_save_I8(pTHX_ I8 *bytep)
{
    dSS_ADD;

    PERL_ARGS_ASSERT_SAVE_I8;

    SS_ADD_PTR(bytep);
    SS_ADD_UV(SAVEt_I8 | ((UV)*bytep << 8));
    SS_ADD_END(2);
}

void
Perl_save_I16(pTHX_ I16 *intp)
{
    dSS_ADD;

    PERL_ARGS_ASSERT_SAVE_I16;

    SS_ADD_PTR(intp);
    SS_ADD_UV(SAVEt_I16 | ((UV)*intp << 8));
    SS_ADD_END(2);
}

void
Perl_save_I32(pTHX_ I32 *intp)
{
    const I32 i = *intp;
    UV type = ((I32)((U32)i << SAVE_TIGHT_SHIFT) | SAVEt_I32_SMALL);
    int size = 2;
    dSS_ADD;

    PERL_ARGS_ASSERT_SAVE_I32;

    if (UNLIKELY((I32)(type >> SAVE_TIGHT_SHIFT) != i)) {
        SS_ADD_INT(i);
        type = SAVEt_I32;
        size++;
    }
    SS_ADD_PTR(intp);
    SS_ADD_UV(type);
    SS_ADD_END(size);
}

void
Perl_save_strlen(pTHX_ STRLEN *ptr)
{
    const IV i = *ptr;
    UV type = ((I32)((U32)i << SAVE_TIGHT_SHIFT) | SAVEt_STRLEN_SMALL);
    int size = 2;
    dSS_ADD;

    PERL_ARGS_ASSERT_SAVE_STRLEN;

    if (UNLIKELY((I32)(type >> SAVE_TIGHT_SHIFT) != i)) {
        SS_ADD_IV(*ptr);
        type = SAVEt_STRLEN;
        size++;
    }

    SS_ADD_PTR(ptr);
    SS_ADD_UV(type);
    SS_ADD_END(size);
}

void
Perl_save_iv(pTHX_ IV *ivp)
{
    PERL_ARGS_ASSERT_SAVE_IV;

    SSGROW(3);
    SSPUSHIV(*ivp);
    SSPUSHPTR(ivp);
    SSPUSHUV(SAVEt_IV);
}

/* Cannot use save_sptr() to store a char* since the SV** cast will
 * force word-alignment and we'll miss the pointer.
 */
void
Perl_save_pptr(pTHX_ char **pptr)
{
    PERL_ARGS_ASSERT_SAVE_PPTR;

    save_pushptrptr(*pptr, pptr, SAVEt_PPTR);
}

/*
=for apidoc_section $callback
=for apidoc save_vptr

Implements C<SAVEVPTR>.

=cut
 */

void
Perl_save_vptr(pTHX_ void *ptr)
{
    PERL_ARGS_ASSERT_SAVE_VPTR;

    save_pushptrptr(*(char**)ptr, ptr, SAVEt_VPTR);
}

void
Perl_save_sptr(pTHX_ SV **sptr)
{
    PERL_ARGS_ASSERT_SAVE_SPTR;

    save_pushptrptr(*sptr, sptr, SAVEt_SPTR);
}

/*
=for apidoc_section $callback
=for apidoc save_padsv_and_mortalize

Implements C<SAVEPADSVANDMORTALIZE>.

=cut
 */

void
Perl_save_padsv_and_mortalize(pTHX_ PADOFFSET off)
{
    dSS_ADD;

    ASSERT_CURPAD_ACTIVE("save_padsv");
    SS_ADD_PTR(SvREFCNT_inc_simple_NN(PL_curpad[off]));
    SS_ADD_PTR(PL_comppad);
    SS_ADD_UV((UV)off);
    SS_ADD_UV(SAVEt_PADSV_AND_MORTALIZE);
    SS_ADD_END(4);
}

void
Perl_save_hptr(pTHX_ HV **hptr)
{
    PERL_ARGS_ASSERT_SAVE_HPTR;

    save_pushptrptr(*hptr, hptr, SAVEt_HPTR);
}

void
Perl_save_aptr(pTHX_ AV **aptr)
{
    PERL_ARGS_ASSERT_SAVE_APTR;

    save_pushptrptr(*aptr, aptr, SAVEt_APTR);
}

/*
=for apidoc_section $callback
=for apidoc save_pushptr

The refcnt of object C<ptr> will be decremented at the end of the current
I<pseudo-block>.  C<type> gives the type of C<ptr>, expressed as one of the
constants in F<scope.h> whose name begins with C<SAVEt_>.

This is the underlying implementation of several macros, like
C<SAVEFREESV>.

=cut
*/

void
Perl_save_pushptr(pTHX_ void *const ptr, const int type)
{
    dSS_ADD;
    SS_ADD_PTR(ptr);
    SS_ADD_UV(type);
    SS_ADD_END(2);
}

void
Perl_save_clearsv(pTHX_ SV **svp)
{
    const UV offset = svp - PL_curpad;
    const UV offset_shifted = offset << SAVE_TIGHT_SHIFT;

    PERL_ARGS_ASSERT_SAVE_CLEARSV;

    ASSERT_CURPAD_ACTIVE("save_clearsv");
    assert(*svp);
    SvPADSTALE_off(*svp); /* mark lexical as active */
    if (UNLIKELY((offset_shifted >> SAVE_TIGHT_SHIFT) != offset)) {
        croak("panic: pad offset %" UVuf " out of range (%p-%p)",
                   offset, svp, PL_curpad);
    }

    {
        dSS_ADD;
        SS_ADD_UV(offset_shifted | SAVEt_CLEARSV);
        SS_ADD_END(1);
    }
}

void
Perl_save_delete(pTHX_ HV *hv, char *key, I32 klen)
{
    PERL_ARGS_ASSERT_SAVE_DELETE;

    save_pushptri32ptr(key, klen, SvREFCNT_inc_simple(hv), SAVEt_DELETE);
}

/*
=for apidoc_section $callback
=for apidoc save_hdelete

Implements C<SAVEHDELETE>.

=cut
*/

void
Perl_save_hdelete(pTHX_ HV *hv, SV *keysv)
{
    STRLEN len;
    I32 klen;
    const char *key;

    PERL_ARGS_ASSERT_SAVE_HDELETE;

    key  = SvPV_const(keysv, len);
    klen = SvUTF8(keysv) ? -(I32)len : (I32)len;
    SvREFCNT_inc_simple_void_NN(hv);
    save_pushptri32ptr(savepvn(key, len), klen, hv, SAVEt_DELETE);
}

/*
=for apidoc_section $callback
=for apidoc save_adelete

Implements C<SAVEADELETE>.

=cut
*/

void
Perl_save_adelete(pTHX_ AV *av, SSize_t key)
{
    dSS_ADD;

    PERL_ARGS_ASSERT_SAVE_ADELETE;

    SvREFCNT_inc_void(av);
    SS_ADD_UV(key);
    SS_ADD_PTR(av);
    SS_ADD_IV(SAVEt_ADELETE);
    SS_ADD_END(3);
}

void
Perl_save_destructor(pTHX_ DESTRUCTORFUNC_NOCONTEXT_t f, void* p)
{
    dSS_ADD;
    PERL_ARGS_ASSERT_SAVE_DESTRUCTOR;

    SS_ADD_DPTR(f);
    SS_ADD_PTR(p);
    SS_ADD_UV(SAVEt_DESTRUCTOR);
    SS_ADD_END(3);
}

void
Perl_save_destructor_x(pTHX_ DESTRUCTORFUNC_t f, void* p)
{
    dSS_ADD;

    SS_ADD_DXPTR(f);
    SS_ADD_PTR(p);
    SS_ADD_UV(SAVEt_DESTRUCTOR_X);
    SS_ADD_END(3);
}

/*
=for apidoc_section $callback
=for apidoc save_hints

Implements C<SAVEHINTS>.

=cut
 */

void
Perl_save_hints(pTHX)
{
    COPHH *save_cophh = cophh_copy(CopHINTHASH_get(&PL_compiling));
    if (PL_hints & HINT_LOCALIZE_HH) {
        HV *oldhh = GvHV(PL_hintgv);
        {
            dSS_ADD;
            SS_ADD_INT(PL_hints);
            SS_ADD_PTR(save_cophh);
            SS_ADD_PTR(oldhh);
            SS_ADD_UV(SAVEt_HINTS_HH | (PL_prevailing_version << 8));
            SS_ADD_END(4);
        }
        GvHV(PL_hintgv) = NULL; /* in case copying dies */
        GvHV(PL_hintgv) = hv_copy_hints_hv(oldhh);
        SAVEFEATUREBITS();
    } else {
        save_pushi32ptr(PL_hints, save_cophh, SAVEt_HINTS | (PL_prevailing_version << 8));
    }
}

static void
S_save_pushptri32ptr(pTHX_ void *const ptr1, const I32 i, void *const ptr2,
                        const int type)
{
    dSS_ADD;
    SS_ADD_PTR(ptr1);
    SS_ADD_INT(i);
    SS_ADD_PTR(ptr2);
    SS_ADD_UV(type);
    SS_ADD_END(4);
}

/*
=for apidoc_section $callback
=for apidoc      save_aelem
=for apidoc_item save_aelem_flags

These each arrange for the value of the array element C<av[idx]> to be restored
at the end of the enclosing I<pseudo-block>.

In C<save_aelem>, the SV at C**sptr> will be replaced by a new C<undef>
scalar.  That scalar will inherit any magic from the original C<**sptr>,
and any 'set' magic will be processed.

In C<save_aelem_flags>, C<SAVEf_KEEPOLDELEM> being set in C<flags> causes
the function to forgo all that:  the scalar at C<**sptr> is untouched.
If C<SAVEf_KEEPOLDELEM> is not set, the SV at C**sptr> will be replaced by a
new C<undef> scalar.  That scalar will inherit any magic from the original
C<**sptr>.  Any 'set' magic will be processed if and only if C<SAVEf_SETMAGIC>
is set in in C<flags>.

=cut
*/

void
Perl_save_aelem_flags(pTHX_ AV *av, SSize_t idx, SV **sptr,
                            const U32 flags)
{
    dSS_ADD;
    SV *sv;

    PERL_ARGS_ASSERT_SAVE_AELEM_FLAGS;

    SvGETMAGIC(*sptr);
    SS_ADD_PTR(SvREFCNT_inc_simple(av));
    SS_ADD_IV(idx);
    SS_ADD_PTR(SvREFCNT_inc(*sptr));
    SS_ADD_UV(SAVEt_AELEM);
    SS_ADD_END(4);
    /* The array needs to hold a reference count on its new element, so it
       must be AvREAL. */
    if (UNLIKELY(!AvREAL(av) && AvREIFY(av)))
        av_reify(av);
    save_scalar_at(sptr, flags); /* XXX - FIXME - see #60360 */
    if (flags & SAVEf_KEEPOLDELEM)
        return;
    sv = *sptr;
    /* If we're localizing a tied array element, this new sv
     * won't actually be stored in the array - so it won't get
     * reaped when the localize ends. Ensure it gets reaped by
     * mortifying it instead. DAPM */
    if (UNLIKELY(SvTIED_mg((const SV *)av, PERL_MAGIC_tied)))
        sv_2mortal(sv);
}

/*
=for apidoc_section $callback
=for apidoc      save_helem
=for apidoc_item save_helem_flags

These each arrange for the value of the hash element (in Perlish terms)
C<$hv{key}]> to be restored at the end of the enclosing I<pseudo-block>.

In C<save_helem>, the SV at C**sptr> will be replaced by a new C<undef>
scalar.  That scalar will inherit any magic from the original C<**sptr>,
and any 'set' magic will be processed.

In C<save_helem_flags>, C<SAVEf_KEEPOLDELEM> being set in C<flags> causes
the function to forgo all that:  the scalar at C<**sptr> is untouched.
If C<SAVEf_KEEPOLDELEM> is not set, the SV at C**sptr> will be replaced by a
new C<undef> scalar.  That scalar will inherit any magic from the original
C<**sptr>.  Any 'set' magic will be processed if and only if C<SAVEf_SETMAGIC>
is set in in C<flags>.

=cut
*/

void
Perl_save_helem_flags(pTHX_ HV *hv, SV *key, SV **sptr, const U32 flags)
{
    SV *sv;

    PERL_ARGS_ASSERT_SAVE_HELEM_FLAGS;

    SvGETMAGIC(*sptr);
    {
        dSS_ADD;
        SS_ADD_PTR(SvREFCNT_inc_simple(hv));
        SS_ADD_PTR(newSVsv(key));
        SS_ADD_PTR(SvREFCNT_inc(*sptr));
        SS_ADD_UV(SAVEt_HELEM);
        SS_ADD_END(4);
    }
    save_scalar_at(sptr, flags);
    if (flags & SAVEf_KEEPOLDELEM)
        return;
    sv = *sptr;
    /* If we're localizing a tied hash element, this new sv
     * won't actually be stored in the hash - so it won't get
     * reaped when the localize ends. Ensure it gets reaped by
     * mortifying it instead. DAPM */
    if (UNLIKELY(SvTIED_mg((const SV *)hv, PERL_MAGIC_tied)))
        sv_2mortal(sv);
}

SV*
Perl_save_svref(pTHX_ SV **sptr)
{
    PERL_ARGS_ASSERT_SAVE_SVREF;

    SvGETMAGIC(*sptr);
    save_pushptrptr(sptr, SvREFCNT_inc(*sptr), SAVEt_SVREF);
    return save_scalar_at(sptr, SAVEf_SETMAGIC); /* XXX - FIXME - see #60360 */
}


void
Perl_savetmps(pTHX)
{
    dSS_ADD;
    SS_ADD_IV(PL_tmps_floor);
    PL_tmps_floor = PL_tmps_ix;
    SS_ADD_UV(SAVEt_TMPSFLOOR);
    SS_ADD_END(2);
}

/*
=for apidoc_section $stack
=for apidoc save_alloc

Implements L<perlapi/C<SSNEW>> and kin, which should be used instead of this
function.

=cut
*/

SSize_t
Perl_save_alloc(pTHX_ SSize_t size, I32 pad)
{
    const SSize_t start = pad + ((char*)&PL_savestack[PL_savestack_ix]
                          - (char*)PL_savestack);
    const UV elems = 1 + ((size + pad - 1) / sizeof(*PL_savestack));
    const UV elems_shifted = elems << SAVE_TIGHT_SHIFT;

    if (UNLIKELY((elems_shifted >> SAVE_TIGHT_SHIFT) != elems))
        croak(
            "panic: save_alloc elems %" UVuf " out of range (%" IVdf "-%" IVdf ")",
                   elems, (IV)size, (IV)pad);

    SSGROW(elems + 1);

    PL_savestack_ix += elems;
    SSPUSHUV(SAVEt_ALLOC | elems_shifted);
    return start;
}



/*
=for apidoc_section $callback
=for apidoc leave_scope

Implements C<LEAVE_SCOPE> which you should use instead.

=cut
 */

void
Perl_leave_scope(pTHX_ I32 base)
{
    /* Localise the effects of the TAINT_NOT inside the loop.  */
    bool was = TAINT_get;

    if (UNLIKELY(base < -1))
        croak("panic: corrupt saved stack index %ld", (long) base);
    DEBUG_l(Perl_deb(aTHX_ "savestack: releasing items %ld -> %ld\n",
                        (long)PL_savestack_ix, (long)base));
    while (PL_savestack_ix > base) {
        UV uv;
        U8 type;
        ANY *ap; /* arg pointer */
        ANY a0, a1, a2; /* up to 3 args */

        TAINT_NOT;

        {
            U8  argcount;
            I32 ix = PL_savestack_ix - 1;

            ap = &PL_savestack[ix];
            uv = ap->any_uv;
            type = (U8)uv & SAVE_MASK;
            argcount = leave_scope_arg_counts[type];
            PL_savestack_ix = ix - argcount;
            ap -= argcount;
        }

        switch (type) {
        case SAVEt_ITEM:			/* normal string */
            a0 = ap[0]; a1 = ap[1];
            sv_replace(a0.any_sv, a1.any_sv);
            if (UNLIKELY(SvSMAGICAL(a0.any_sv))) {
                PL_localizing = 2;
                mg_set(a0.any_sv);
                PL_localizing = 0;
            }
            break;

            /* This would be a mathom, but Perl_save_svref() calls a static
               function, S_save_scalar_at(), so has to stay in this file.  */
        case SAVEt_SVREF:			/* scalar reference */
            a0 = ap[0]; a1 = ap[1];
            a2.any_svp = a0.any_svp;
            a0.any_sv = NULL; /* what to refcnt_dec */
            goto restore_sv;

        case SAVEt_SV:				/* scalar reference */
            a0 = ap[0]; a1 = ap[1];
            a2.any_svp = &GvSV(a0.any_gv);
        restore_sv:
        {
            /* do *a2.any_svp = a1 and free a0 */
            SV * const sv = *a2.any_svp;
            *a2.any_svp = a1.any_sv;
            SvREFCNT_dec(sv);
            if (UNLIKELY(SvSMAGICAL(a1.any_sv))) {
                /* mg_set could die, skipping the freeing of a0 and
                 * a1; Ensure that they're always freed in that case */
                dSS_ADD;
                SS_ADD_PTR(a1.any_sv);
                SS_ADD_UV(SAVEt_FREESV);
                SS_ADD_PTR(a0.any_sv);
                SS_ADD_UV(SAVEt_FREESV);
                SS_ADD_END(4);
                PL_localizing = 2;
                mg_set(a1.any_sv);
                PL_localizing = 0;
                break;
            }
            SvREFCNT_dec_NN(a1.any_sv);
            SvREFCNT_dec(a0.any_sv);
            break;
        }

        case SAVEt_GENERIC_PVREF:		/* generic pv */
            a0 = ap[0]; a1 = ap[1];
            if (*a1.any_pvp != a0.any_pv) {
                Safefree(*a1.any_pvp);
                *a1.any_pvp = a0.any_pv;
            }
            break;

        case SAVEt_SHARED_PVREF:		/* shared pv */
            a0 = ap[0]; a1 = ap[1];
            if (*a0.any_pvp != a1.any_pv) {
                PerlMemShared_free(*a0.any_pvp);
                *a0.any_pvp = a1.any_pv;
            }
            break;

        case SAVEt_GVSV:			/* scalar slot in GV */
            a0 = ap[0]; a1 = ap[1];
            a0.any_svp = &GvSV(a0.any_gv);
            goto restore_svp;


        case SAVEt_GENERIC_SVREF:		/* generic sv */
            a0 = ap[0]; a1 = ap[1];
        restore_svp:
        {
            /* do *a0.any_svp = a1 */
            SV * const sv = *a0.any_svp;
            *a0.any_svp = a1.any_sv;
            SvREFCNT_dec(sv);
            SvREFCNT_dec(a1.any_sv);
            break;
        }

        case SAVEt_RCPV:           /* like generic sv, but for struct rcpv */
        {
            a0 = ap[0]; a1 = ap[1];
            char *old = *a0.any_pvp;
            *a0.any_pvp = a1.any_pv;
            (void)rcpv_free(old);
            (void)rcpv_free(a1.any_pv);
            break;
        }

        case SAVEt_FREERCPV:           /* like SAVEt_FREEPV but for a RCPV */
        {
            a0 = ap[0];
            char *rcpv = a0.any_pv;
            (void)rcpv_free(rcpv);
            break;
        }

        case SAVEt_GVSLOT:			/* any slot in GV */
        {
            HV * hv;
            a0 = ap[0]; a1 = ap[1]; a2 = ap[2];
            hv = GvSTASH(a0.any_gv);
            if (hv && HvHasENAME(hv) && (
                    (a2.any_sv && SvTYPE(a2.any_sv) == SVt_PVCV)
                 || (*a1.any_svp && SvTYPE(*a1.any_svp) == SVt_PVCV)
               ))
            {
                if ((char *)a1.any_svp < (char *)GvGP(a0.any_gv)
                 || (char *)a1.any_svp > (char *)GvGP(a0.any_gv) + sizeof(struct gp)
                 || GvREFCNT(a0.any_gv) > 2) /* "> 2" to ignore savestack's ref */
                    PL_sub_generation++;
                else mro_method_changed_in(hv);
            }
            a0.any_svp = a1.any_svp;
            a1.any_sv  = a2.any_sv;
            goto restore_svp;
        }

        case SAVEt_AV:				/* array reference */
            a0 = ap[0]; a1 = ap[1];
            SvREFCNT_dec(GvAV(a0.any_gv));
            GvAV(a0.any_gv) = a1.any_av;
          avhv_common:
            if (UNLIKELY(SvSMAGICAL(a1.any_sv))) {
                /* mg_set might die, so make sure a0 isn't leaked */
                dSS_ADD;
                SS_ADD_PTR(a0.any_sv);
                SS_ADD_UV(SAVEt_FREESV);
                SS_ADD_END(2);
                PL_localizing = 2;
                mg_set(a1.any_sv);
                PL_localizing = 0;
                break;
            }
            SvREFCNT_dec_NN(a0.any_sv);
            break;

        case SAVEt_HV:				/* hash reference */
            a0 = ap[0]; a1 = ap[1];
            SvREFCNT_dec(GvHV(a0.any_gv));
            GvHV(a0.any_gv) = a1.any_hv;
            goto avhv_common;

        case SAVEt_INT_SMALL:
            a0 = ap[0];
            *(int*)a0.any_ptr = (int)(uv >> SAVE_TIGHT_SHIFT);
            break;

        case SAVEt_INT:				/* int reference */
            a0 = ap[0]; a1 = ap[1];
            *(int*)a1.any_ptr = (int)a0.any_i32;
            break;

        case SAVEt_STRLEN_SMALL:
            a0 = ap[0];
            *(STRLEN*)a0.any_ptr = (STRLEN)(uv >> SAVE_TIGHT_SHIFT);
            break;

        case SAVEt_STRLEN:			/* STRLEN/size_t ref */
            a0 = ap[0]; a1 = ap[1];
            *(STRLEN*)a1.any_ptr = (STRLEN)a0.any_iv;
            break;

        case SAVEt_TMPSFLOOR:			/* restore PL_tmps_floor */
            a0 = ap[0];
            PL_tmps_floor = (SSize_t)a0.any_iv;
            break;

        case SAVEt_BOOL:			/* bool reference */
            a0 = ap[0];
            *(bool*)a0.any_ptr = cBOOL(uv >> 8);
#ifdef NO_TAINT_SUPPORT
            PERL_UNUSED_VAR(was);
#else
            if (UNLIKELY(a0.any_ptr == &(PL_tainted))) {
                /* If we don't update <was>, to reflect what was saved on the
                 * stack for PL_tainted, then we will overwrite this attempt to
                 * restore it when we exit this routine.  Note that this won't
                 * work if this value was saved in a wider-than necessary type,
                 * such as I32 */
                was = *(bool*)a0.any_ptr;
            }
#endif
            break;

        case SAVEt_I32_SMALL:
            a0 = ap[0];
            *(I32*)a0.any_ptr = (I32)(uv >> SAVE_TIGHT_SHIFT);
            break;

        case SAVEt_I32:				/* I32 reference */
            a0 = ap[0]; a1 = ap[1];
#ifdef PERL_DEBUG_READONLY_OPS
            if (*(I32*)a1.any_ptr != a0.any_i32)
#endif
                *(I32*)a1.any_ptr = a0.any_i32;
            break;

        case SAVEt_SPTR:			/* SV* reference */
        case SAVEt_VPTR:			/* random* reference */
        case SAVEt_PPTR:			/* char* reference */
        case SAVEt_HPTR:			/* HV* reference */
        case SAVEt_APTR:			/* AV* reference */
            a0 = ap[0]; a1 = ap[1];
            *a1.any_svp= a0.any_sv;
            break;

        case SAVEt_GP:				/* scalar reference */
        {
            HV *hv;
            bool had_method;

            a0 = ap[0]; a1 = ap[1];
            /* possibly taking a method out of circulation */	
            had_method = cBOOL(GvCVu(a0.any_gv));
            gp_free(a0.any_gv);
            GvGP_set(a0.any_gv, (GP*)a1.any_ptr);
            if ((hv=GvSTASH(a0.any_gv)) && HvHasENAME(hv)) {
                if (memEQs(GvNAME(a0.any_gv), GvNAMELEN(a0.any_gv), "ISA"))
                    mro_isa_changed_in(hv);
                else if (had_method || GvCVu(a0.any_gv))
                    /* putting a method back into circulation ("local")*/	
                    gv_method_changed(a0.any_gv);
            }
            SvREFCNT_dec_NN(a0.any_gv);
            break;
        }

        case SAVEt_FREESV:
            a0 = ap[0];
            SvREFCNT_dec(a0.any_sv);
            break;

        case SAVEt_FREEPADNAME:
            a0 = ap[0];
            PadnameREFCNT_dec((PADNAME *)a0.any_ptr);
            break;

        case SAVEt_FREECOPHH:
            a0 = ap[0];
            cophh_free((COPHH *)a0.any_ptr);
            break;

        case SAVEt_MORTALIZESV:
            a0 = ap[0];
            sv_2mortal(a0.any_sv);
            break;

        case SAVEt_FREEOP:
            a0 = ap[0];
            ASSERT_CURPAD_LEGAL("SAVEt_FREEOP");
            op_free(a0.any_op);
            break;

        case SAVEt_FREEPV:
            a0 = ap[0];
            Safefree(a0.any_ptr);
            break;

        case SAVEt_FREE_REXC_STATE:
            a0 = ap[0];
            if (a0.any_ptr)
                release_RExC_state(a0.any_ptr);
            break;

        case SAVEt_CLEARPADRANGE:
        {
            I32 i;
            SV **svp;
            i = (I32)((uv >> SAVE_TIGHT_SHIFT) & OPpPADRANGE_COUNTMASK);
            svp = &PL_curpad[uv >>
                    (OPpPADRANGE_COUNTSHIFT + SAVE_TIGHT_SHIFT)] + i - 1;
            goto clearsv;
        case SAVEt_CLEARSV:
            svp = &PL_curpad[uv >> SAVE_TIGHT_SHIFT];
            i = 1;
          clearsv:
            for (; i; i--, svp--) {
                SV *sv = *svp;

                DEBUG_Xv(PerlIO_printf(Perl_debug_log,
             "Pad 0x%" UVxf "[0x%" UVxf "] clearsv: %ld sv=0x%" UVxf "<%" IVdf "> %s\n",
                    PTR2UV(PL_comppad), PTR2UV(PL_curpad),
                    (long)(svp-PL_curpad), PTR2UV(sv), (IV)SvREFCNT(sv),
                    (SvREFCNT(sv) <= 1 && !SvOBJECT(sv)) ? "clear" : "abandon"
                ));

                /* Can clear pad variable in place? */
                if (SvREFCNT(sv) == 1 && !SvOBJECT(sv)) {

                    /* these flags are the union of all the relevant flags
                     * in the individual conditions within */
                    if (UNLIKELY(SvFLAGS(sv) & (
                            SVf_READONLY|SVf_PROTECT /*for SvREADONLY_off*/
                          | (SVs_GMG|SVs_SMG|SVs_RMG) /* SvMAGICAL() */
                          | SVf_OOK
                          | SVf_THINKFIRST)))
                    {
                        /* if a my variable that was made readonly is
                         * going out of scope, we want to remove the
                         * readonlyness so that it can go out of scope
                         * quietly
                         */
                        if (SvREADONLY(sv))
                            SvREADONLY_off(sv);

                        if (SvTYPE(sv) == SVt_PVHV && HvHasAUX(sv))
                            Perl_hv_kill_backrefs(aTHX_ MUTABLE_HV(sv));
                        else if(SvOOK(sv))
                            sv_backoff(sv);

                        if (SvMAGICAL(sv)) {
                            /* note that backrefs (either in HvAUX or magic)
                             * must be removed before other magic */
                            sv_unmagic(sv, PERL_MAGIC_backref);
                            if (SvTYPE(sv) != SVt_PVCV)
                                mg_free(sv);
                        }
                        if (SvTHINKFIRST(sv))
                            sv_force_normal_flags(sv, SV_IMMEDIATE_UNREF
                                                     |SV_COW_DROP_PV);

                    }
                    switch (SvTYPE(sv)) {
                    case SVt_NULL:
                        break;
                    case SVt_PVAV:
                        av_clear(MUTABLE_AV(sv));
                        break;
                    case SVt_PVHV:
                        hv_clear(MUTABLE_HV(sv));
                        break;
                    case SVt_PVCV:
                    {
                        HEK *hek = CvGvNAME_HEK(sv);
                        assert(hek);
                        (void)share_hek_hek(hek);
                        cv_undef((CV *)sv);
                        CvNAME_HEK_set(sv, hek);
                        CvLEXICAL_on(sv);
                        break;
                    }
                    default:
                        /* This looks odd, but these two macros are for use in
                           expressions and finish with a trailing comma, so
                           adding a ; after them would be wrong. */
                        assert_not_ROK(sv)
                        assert_not_glob(sv)
                        SvFLAGS(sv) &=~ (SVf_OK|SVf_IVisUV|SVf_UTF8);
                        break;
                    }
                    SvPADTMP_off(sv);
                    SvPADSTALE_on(sv); /* mark as no longer live */
                }
                else {	/* Someone has a claim on this, so abandon it. */
                    switch (SvTYPE(sv)) {	/* Console ourselves with a new value */
                    case SVt_PVAV:	*svp = MUTABLE_SV(newAV());	break;
                    case SVt_PVHV:	*svp = MUTABLE_SV(newHV());	break;
                    case SVt_PVCV:
                    {
                        HEK * const hek = CvGvNAME_HEK(sv);

                        /* Create a stub */
                        *svp = newSV_type(SVt_PVCV);

                        /* Share name */
                        CvNAME_HEK_set(*svp,
                                       share_hek_hek(hek));
                        CvLEXICAL_on(*svp);
                        break;
                    }
                    default:	*svp = newSV_type(SVt_NULL);		break;
                    }
                    SvREFCNT_dec_NN(sv); /* Cast current value to the winds. */
                    /* preserve pad nature, but also mark as not live
                     * for any closure capturing */
                    SvFLAGS(*svp) |= SVs_PADSTALE;
                }
            }
            break;
        }

        case SAVEt_DELETE:
            a0 = ap[0]; a1 = ap[1]; a2 = ap[2];
            /* hv_delete could die, so free the key and SvREFCNT_dec the
             * hv by pushing new save actions
             */
            /* ap[0] is the key */
            ap[1].any_uv = SAVEt_FREEPV; /* was len */
            /* ap[2] is the hv */
            ap[3].any_uv = SAVEt_FREESV; /* was SAVEt_DELETE */
            PL_savestack_ix += 4;
            (void)hv_delete(a2.any_hv, a0.any_pv, a1.any_i32, G_DISCARD);
            break;

        case SAVEt_ADELETE:
            a0 = ap[0]; a1 = ap[1];
            /* av_delete could die, so SvREFCNT_dec the av by pushing a
             * new save action
             */
            ap[0].any_av = a1.any_av;
            ap[1].any_uv = SAVEt_FREESV;
            PL_savestack_ix += 2;
            (void)av_delete(a1.any_av, a0.any_iv, G_DISCARD);
            break;

        case SAVEt_DESTRUCTOR_X:
            a0 = ap[0]; a1 = ap[1];
            (*a0.any_dxptr)(aTHX_ a1.any_ptr);
            break;

        case SAVEt_REGCONTEXT:
            /* regexp must have croaked */
        case SAVEt_ALLOC:
            PL_savestack_ix -= uv >> SAVE_TIGHT_SHIFT;
            break;

        case SAVEt_STACK_POS:		/* Position on Perl stack */
#ifdef PERL_RC_STACK
            /* DAPM Jan 2023. I don't think this save type is used any
             * more, but if some XS code uses it, fail it for now, as
             * it's not clear to me what perl should be doing to stack ref
             * counts when arbitrarily resetting the stack pointer.
             */
            assert(0);
#endif
            a0 = ap[0];
            PL_stack_sp = PL_stack_base + a0.any_i32;
            break;

        case SAVEt_AELEM:		/* array element */
        {
            SV **svp;
            a0 = ap[0]; a1 = ap[1]; a2 = ap[2];
            svp = av_fetch(a0.any_av, a1.any_iv, 1);
            if (UNLIKELY(!AvREAL(a0.any_av) && AvREIFY(a0.any_av))) /* undo reify guard */
                SvREFCNT_dec(a2.any_sv);
            if (LIKELY(svp)) {
                SV * const sv = *svp;
                if (LIKELY(sv && sv != &PL_sv_undef)) {
                    if (UNLIKELY(SvTIED_mg((const SV *)a0.any_av, PERL_MAGIC_tied)))
                        SvREFCNT_inc_void_NN(sv);
                    a1.any_sv  = a2.any_sv;
                    a2.any_svp = svp;
                    goto restore_sv;
                }
            }
            SvREFCNT_dec(a0.any_av);
            SvREFCNT_dec(a2.any_sv);
            break;
        }

        case SAVEt_HELEM:		/* hash element */
        {
            HE *he;

            a0 = ap[0]; a1 = ap[1]; a2 = ap[2];
            he = hv_fetch_ent(a0.any_hv, a1.any_sv, 1, 0);
            SvREFCNT_dec(a1.any_sv);
            if (LIKELY(he)) {
                const SV * const oval = HeVAL(he);
                if (LIKELY(oval && oval != &PL_sv_undef)) {
                    SV **svp = &HeVAL(he);
                    if (UNLIKELY(SvTIED_mg((const SV *)a0.any_hv, PERL_MAGIC_tied)))
                        SvREFCNT_inc_void(*svp);
                    a1.any_sv  = a2.any_sv;
                    a2.any_svp = svp;
                    goto restore_sv;
                }
            }
            SvREFCNT_dec(a0.any_hv);
            SvREFCNT_dec(a2.any_sv);
            break;
        }

        case SAVEt_OP:
            a0 = ap[0];
            PL_op = (OP*)a0.any_ptr;
            break;

        case SAVEt_HINTS_HH:
            a2 = ap[2];
            /* FALLTHROUGH */
        case SAVEt_HINTS:
            a0 = ap[0]; a1 = ap[1];
            if ((PL_hints & HINT_LOCALIZE_HH)) {
              while (GvHV(PL_hintgv)) {
                HV *hv = GvHV(PL_hintgv);
                GvHV(PL_hintgv) = NULL;
                SvREFCNT_dec(MUTABLE_SV(hv));
              }
            }
            cophh_free(CopHINTHASH_get(&PL_compiling));
            CopHINTHASH_set(&PL_compiling, (COPHH*)a1.any_ptr);
            *(I32*)&PL_hints = a0.any_i32;
            PL_prevailing_version = (U16)(uv >> 8);
            if (type == SAVEt_HINTS_HH) {
                SvREFCNT_dec(MUTABLE_SV(GvHV(PL_hintgv)));
                GvHV(PL_hintgv) = MUTABLE_HV(a2.any_ptr);
            }
            if (!GvHV(PL_hintgv)) {
                /* Need to add a new one manually, else rv2hv can
                   add one via GvHVn and it won't have the magic set.  */
                HV *const hv = newHV();
                hv_magic(hv, NULL, PERL_MAGIC_hints);
                GvHV(PL_hintgv) = hv;
            }
            assert(GvHV(PL_hintgv));
            break;

        case SAVEt_COMPPAD:
            a0 = ap[0];
            PL_comppad = (PAD*)a0.any_ptr;
            if (LIKELY(PL_comppad))
                PL_curpad = AvARRAY(PL_comppad);
            else
                PL_curpad = NULL;
            break;

        case SAVEt_PADSV_AND_MORTALIZE:
            {
                SV **svp;

                a0 = ap[0]; a1 = ap[1]; a2 = ap[2];
                assert (a1.any_ptr);
                svp = AvARRAY((PAD*)a1.any_ptr) + (PADOFFSET)a2.any_uv;
                /* This mortalizing used to be done by CX_POOPLOOP() via
                   itersave.  But as we have all the information here, we
                   can do it here, save even having to have itersave in
                   the struct.
                   */
                sv_2mortal(*svp);
                *svp = a0.any_sv;
            }
            break;

        case SAVEt_SAVESWITCHSTACK:
            {
                dSP;

                a0 = ap[0]; a1 = ap[1];
                SWITCHSTACK(a1.any_av, a0.any_av);
                PL_curstackinfo->si_stack = a0.any_av;
            }
            break;

        case SAVEt_SET_SVFLAGS:
            a0 = ap[0]; a1 = ap[1]; a2 = ap[2];
            SvFLAGS(a0.any_sv) &= ~(a1.any_u32);
            SvFLAGS(a0.any_sv) |= a2.any_u32;
            break;

            /* These are only saved in mathoms.c */
        case SAVEt_NSTAB:
            a0 = ap[0];
            (void)sv_clear(a0.any_sv);
            break;

        case SAVEt_IV:				/* IV reference */
            a0 = ap[0]; a1 = ap[1];
            *(IV*)a1.any_ptr = a0.any_iv;
            break;

        case SAVEt_I16:				/* I16 reference */
            a0 = ap[0];
            *(I16*)a0.any_ptr = (I16)(uv >> 8);
            break;

        case SAVEt_I8:				/* I8 reference */
            a0 = ap[0];
            *(I8*)a0.any_ptr = (I8)(uv >> 8);
            break;

        case SAVEt_DESTRUCTOR:
            a0 = ap[0]; a1 = ap[1];
            (*a0.any_dptr)(a1.any_ptr);
            break;

        case SAVEt_COMPILE_WARNINGS:
            /* NOTE: we can't put &PL_compiling or PL_curcop on the save
             *       stack directly, as we currently cannot translate
             *       them to the correct addresses after a thread start
             *       or win32 fork start. - Yves
             */
            a0 = ap[0];
            free_and_set_cop_warnings(&PL_compiling, a0.any_pv);
            break;

        case SAVEt_CURCOP_WARNINGS:
            /* NOTE: see comment above about SAVEt_COMPILE_WARNINGS */
            a0 = ap[0];
            free_and_set_cop_warnings(PL_curcop, a0.any_pv);
            break;

        case SAVEt_PARSER:
            a0 = ap[0];
            parser_free((yy_parser *)a0.any_ptr);
            break;

        case SAVEt_READONLY_OFF:
            a0 = ap[0];
            SvREADONLY_off(a0.any_sv);
            break;

        default:
            croak("panic: leave_scope inconsistency %u",
                    (U8)uv & SAVE_MASK);
        }
    }

    TAINT_set(was);
}

/* ------------------------------------------------------------------------- *
 * Interpreter execution-state API ("execstate") - see execstate.h and
 * Porting/execstate_api.md.  A green-thread execution state is the generic
 * mutable execution registers (the value/mark/scope/save/tmps stacks, the
 * execution position, the compile cursors and the flags), listed once as the
 * PERL_EXECSTATE_SLOTS X-macro in execstate.h.  save/load copy them to/from the
 * live interpreter; the caller does its own C-stack switch between a save and a
 * load, and handles any per-thread policy globals itself.
 *
 * Register list derived from Coro/state.h by Marc A. Lehmann; see execstate.h.
 * ------------------------------------------------------------------------- */

/*
=for apidoc_section $callback
=for apidoc execstate_save

Snapshot the live interpreter's generic execution registers (the value / mark /
scope / save / temporaries stacks, the execution position, the compile-time
cursors and the execution flags) into C<into>.  With
L</C<execstate_load>> this lets a stackful coroutine library swap execution
states: S<C<execstate_save(a); ... ; execstate_load(b)>>, doing its own C-stack
switch in between.  It does not touch per-thread policy globals (C<$_>, C<$@>,
C<$/>, ...) - those are the caller's to manage.  B<Experimental.>

=cut
*/

void
Perl_execstate_save(pTHX_ PerlExecState *into)
{
    PERL_ARGS_ASSERT_EXECSTATE_SAVE;
#define PERL_EXECSTATE_SAVE(name, lval, type) into->name = lval;
    PERL_EXECSTATE_SLOTS(PERL_EXECSTATE_SAVE)
#undef PERL_EXECSTATE_SAVE
}

/*
=for apidoc execstate_load

Install a state previously captured by L</C<execstate_save>> as the live
interpreter's generic execution registers.  B<Experimental.>

=cut
*/

void
Perl_execstate_load(pTHX_ PerlExecState *from)
{
    PERL_ARGS_ASSERT_EXECSTATE_LOAD;
#define PERL_EXECSTATE_LOAD(name, lval, type) lval = from->name;
    PERL_EXECSTATE_SLOTS(PERL_EXECSTATE_LOAD)
#undef PERL_EXECSTATE_LOAD
}

/*
=for apidoc execstate_init

Allocate a fresh set of interpreter stacks for a new execution context and make
them live, reserving C<cxextra> extra context-stack entries for the caller to
overlay its own per-thread storage on.  Undone by L</C<execstate_destroy>>.  The
initial sizes are modest, on the assumption that a green thread does not usually
need much stack.  B<Experimental.>

=cut
*/

void
Perl_execstate_init(pTHX_ int cxextra)
{
    PL_curstackinfo = new_stackinfo(32, 4 + cxextra);
    PL_curstackinfo->si_type = PERLSI_MAIN;
    PL_curstack = PL_curstackinfo->si_stack;
    PL_mainstack = PL_curstack;		/* remember in case we switch stacks */

    PL_stack_base = AvARRAY(PL_curstack);
    PL_stack_sp = PL_stack_base;
    PL_stack_max = PL_stack_base + AvMAX(PL_curstack);

    Newx(PL_tmps_stack, 32, SV*);
    PL_tmps_floor = -1;
    PL_tmps_ix = -1;
    PL_tmps_max = 32;

    Newx(PL_markstack, 16, Stack_off_t);
    PL_markstack_ptr = PL_markstack;
    PL_markstack_max = PL_markstack + 16;

    SET_MARK_OFFSET;

    Newx(PL_scopestack, 8, I32);
    PL_scopestack_ix = 0;
    PL_scopestack_max = 8;
#ifdef DEBUGGING
    Newx(PL_scopestack_name, 8, const char*);
#endif

    Newx(PL_savestack, 24, ANY);
    PL_savestack_ix = 0;
    /* PL_savestack_max always carries SS_MAXPUSH of slack over what it claims */
    PL_savestack_max = 24 - SS_MAXPUSH;
}

/*
=for apidoc execstate_unwind

Unwind the live execution context - pop the context stack, leave all remaining
scopes and free the temporaries - as when a green thread is discarded.  The
order matters: C<dounwind> must run before the blanket C<LEAVE_SCOPE(0)>, so each
frame's save-stack entries are processed against that frame's pad; doing
C<LEAVE_SCOPE> first leaves an outer frame's pad slot stale and corrupts
refcounts in C<leave_scope>.  A no-op during global destruction.
B<Experimental.>

=cut
*/

void
Perl_execstate_unwind(pTHX)
{
    if (PL_phase != PERL_PHASE_DESTRUCT) {
        POPSTACK_TO(PL_mainstack);
        dounwind(-1);
        LEAVE_SCOPE(0);
        assert(PL_tmps_floor == -1);
        FREETMPS;
        assert(PL_tmps_ix == -1);
    }
}

/*
=for apidoc execstate_destroy

Free the interpreter stacks and the context-stack chain of an execution context
allocated by L</C<execstate_init>>.  B<Experimental.>

=cut
*/

void
Perl_execstate_destroy(pTHX)
{
    while (PL_curstackinfo->si_next)
        PL_curstackinfo = PL_curstackinfo->si_next;

    while (PL_curstackinfo) {
        PERL_SI *p = PL_curstackinfo->si_prev;

        if (PL_phase != PERL_PHASE_DESTRUCT)
            SvREFCNT_dec(PL_curstackinfo->si_stack);

        Safefree(PL_curstackinfo->si_cxstack);
        Safefree(PL_curstackinfo);
        PL_curstackinfo = p;
    }

    Safefree(PL_tmps_stack);
    Safefree(PL_markstack);
    Safefree(PL_scopestack);
#ifdef DEBUGGING
    Safefree(PL_scopestack_name);
#endif
    Safefree(PL_savestack);
}

/*
=for apidoc execstate_derive_padlist

Derive a fresh padlist for C<cv> so it may be re-entered on an independent
execution context (recursion across green threads).  Free the result with
L</C<execstate_free_padlist>>.  B<Experimental.>

=cut
*/

PADLIST *
Perl_execstate_derive_padlist(pTHX_ CV *cv)
{
    PADLIST *padlist = CvPADLIST(cv);
    PADLIST *newpadlist;
    PADNAMELIST *padnames;
    PAD *newpad;
    PADOFFSET off = PadlistMAX(padlist) + 1;

    PERL_ARGS_ASSERT_EXECSTATE_DERIVE_PADLIST;

    /* steal the deepest live pad slot */
    while (!PadlistARRAY(padlist)[off - 1])
        --off;

    pad_push(padlist, off);
    newpad = PadlistARRAY(padlist)[off];
    PadlistARRAY(padlist)[off] = NULL;

    Newxz(newpadlist, 1, PADLIST);
    Newx(PadlistARRAY(newpadlist), 2, PAD *);
    PadlistMAX(newpadlist) = 1;
    padnames = PadlistNAMES(padlist);
    ++PadnamelistREFCNT(padnames);
    PadlistNAMES(newpadlist) = padnames;
    PadlistARRAY(newpadlist)[1] = newpad;

    return newpadlist;
}

/*
=for apidoc execstate_free_padlist

Free a padlist created by L</C<execstate_derive_padlist>>.  A no-op during
global destruction.  B<Experimental.>

=cut
*/

void
Perl_execstate_free_padlist(pTHX_ PADLIST *padlist)
{
    PERL_ARGS_ASSERT_EXECSTATE_FREE_PADLIST;

    if (PL_phase != PERL_PHASE_DESTRUCT) {
        I32 i = PadlistMAX(padlist);

        while (i > 0) {   /* index 0 is the shared names, freed below */
            PAD *pad = PadlistARRAY(padlist)[i--];

            if (pad) {
                I32 j = PadMAX(pad);

                while (j >= 0)
                    SvREFCNT_dec(PadARRAY(pad)[j--]);

                PadMAX(pad) = -1;
                SvREFCNT_dec(pad);
            }
        }

        PadnamelistREFCNT_dec(PadlistNAMES(padlist));
        Safefree(PadlistARRAY(padlist));
        Safefree(padlist);
    }
}

/*
=for apidoc execstate_topenv_root

Return the base (outermost) handler of the live C<JMPENV> exception-handler
chain - the root reached by following C<je_prev>.  A green-thread library
captures this once to recognise the interpreter's own top level.
B<Experimental.>

=cut
*/

JMPENV *
Perl_execstate_topenv_root(pTHX)
{
    JMPENV *te = PL_top_env;

    while (te->je_prev)
        te = te->je_prev;

    return te;
}

/* ------------------------------------------------------------------------- *
 * multicore hook - see perlmulticore.h.  Wire-compatible with the deployed
 * CPAN perlmulticore.h: the two hooks live in a struct in PL_modglobal under
 * "perl_multicore_api", and PL_multicore_api caches a pointer to it.
 * ------------------------------------------------------------------------- */

static void S_multicore_nop(void) { }

#define PERL_MULTICORE_API_KEY "perl_multicore_api"

/* fetch (or create, with nop hooks) the shared struct and cache it */
static void
S_multicore_init(pTHX)
{
    SV **svp = hv_fetch(PL_modglobal, PERL_MULTICORE_API_KEY,
                        sizeof(PERL_MULTICORE_API_KEY) - 1, 1);

    if (SvPOKp(*svp))
        PL_multicore_api = (struct perl_multicore_api *)SvPVX(*svp);
    else {
        SV *sv = newSV(sizeof(struct perl_multicore_api));
        SvCUR_set(sv, sizeof(struct perl_multicore_api));
        SvPOK_only(sv);
        PL_multicore_api = (struct perl_multicore_api *)SvPVX(sv);
        PL_multicore_api->pmapi_release = S_multicore_nop;
        PL_multicore_api->pmapi_acquire = S_multicore_nop;
        *svp = sv;
    }
}

/*
=for apidoc multicore_release

Release the interpreter around a blocking or CPU-bound C section, as the
C<perlinterp_release> half of the multicore bracket (see F<perlmulticore.h>).
A no-op unless a backend is installed.  B<Experimental.>

=cut
*/

void
Perl_multicore_release(pTHX)
{
    if (!PL_multicore_api)
        S_multicore_init(aTHX);

    PL_multicore_api->pmapi_release();
}

/*
=for apidoc multicore_acquire

Re-acquire the interpreter, the C<perlinterp_acquire> half of the multicore
bracket.  Context-free, as it may run on a worker thread; must follow a
C<perlinterp_release>.  B<Experimental.>

=cut
*/

void
Perl_multicore_acquire(void)
{
    if (PL_multicore_api)
        PL_multicore_api->pmapi_acquire();
}

/*
=for apidoc multicore_active

True if a multicore backend (other than the built-in no-op) is installed.  Resolves
the shared struct on first use, exactly as C<perlinterp_release> does, so it
answers the same whether or not anything has driven the bracket yet - a backend
that installed itself by writing C<PL_modglobal> directly, as a module built
against a bundled CPAN F<perlmulticore.h> does, is seen immediately.
B<Experimental.>

=cut
*/

bool
Perl_multicore_active(pTHX)
{
    if (!PL_multicore_api)
        S_multicore_init(aTHX);

    return PL_multicore_api->pmapi_release != S_multicore_nop;
}

/*
=for apidoc multicore_register

Install (or, with C<NULL> arguments, remove) the multicore backend hooks that
C<perlinterp_release> / C<perlinterp_acquire> call.  Writes the shared struct in
C<PL_modglobal>, so this drives modules built against either core's header or a
bundled CPAN F<perlmulticore.h>.  A cooperative-scheduling backend registers
hooks that move the running thread onto a worker OS thread for the duration.
B<Experimental.>

=cut
*/

void
Perl_multicore_register(pTHX_ perl_multicore_hook_t release,
                              perl_multicore_hook_t acquire)
{
    if (!PL_multicore_api)
        S_multicore_init(aTHX);

    PL_multicore_api->pmapi_release = release ? release : S_multicore_nop;
    PL_multicore_api->pmapi_acquire = acquire ? acquire : S_multicore_nop;
}

/*
=for apidoc multicore_offload_cancelled

Build the exception a module's C<done> raises when it was asked to stop early and
the result it would have produced is incomplete:

    if (ctx->cancelled && j->stopped_early)
        croak_sv(multicore_offload_cancelled(partial, NULL));

Returns a mortal C<PerlMulticore::Cancelled> (see L<PerlMulticore>), to hand straight to
C<croak_sv>.  C<partial> is whatever the module salvaged, or C<NULL> if it kept
nothing; it is consumed.  C<message> may be C<NULL> for the default.

Raising rather than returning matters because a truncated result the caller cannot
tell apart from a whole one is the worst outcome available, and the caller that
awaits an offload is often not the one that cancelled it.  It has to be C<done> that
decides: C<done_ctx.cancelled> says cancellation was I<requested>, and a C<work> on
its last chunk may have finished complete anyway - which only the module can know.

If the class cannot be loaded, a plain string exception SV is returned instead, so
the failure still reaches the caller.  B<Experimental.>

=cut
*/

SV *
Perl_multicore_offload_cancelled(pTHX_ SV *partial, const char *message)
{
    SV *err;
    SSize_t count;
    dSP;

    if (!message)
        message = "offload cancelled";

    if (!get_cvs("PerlMulticore::Cancelled::new", 0))
        require_pv("PerlMulticore.pm");

    if (!get_cvs("PerlMulticore::Cancelled::new", 0)) {
        SvREFCNT_dec(partial);
        return sv_2mortal(newSVpvf("%s\n", message));
    }

    ENTER;
    SAVETMPS;

    SPAGAIN;
    PUSHMARK(SP);
    EXTEND(SP, 5);
    PUSHs(newSVpvs_flags("PerlMulticore::Cancelled", SVs_TEMP));
    PUSHs(newSVpvs_flags("message", SVs_TEMP));
    PUSHs(sv_2mortal(newSVpv(message, 0)));
    PUSHs(newSVpvs_flags("partial", SVs_TEMP));
    PUSHs(partial ? sv_2mortal(partial) : &PL_sv_undef);
    PUTBACK;

    count = call_method("new", G_SCALAR | G_EVAL);

    SPAGAIN;
    err = count >= 1 && !SvTRUE(ERRSV) ? SvREFCNT_inc_NN(*SP)
                                       : newSVpvf("%s\n", message);
    SP -= count;
    PUTBACK;

    FREETMPS;
    LEAVE;

    return sv_2mortal(err);
}

/*
=for apidoc multicore_offload_ready

Wrap a value that is already known in an already-resolved offload handle, so that
it can be returned where L</C<multicore_offload>>'s handle is expected.  Two
callers need this: an offload backend that finishes the work before it returns,
and a module whose asynchronous entry point sometimes has the answer without
offloading at all - a cached result, or an input too small to be worth a worker.
Both must hand back the same shape as an offload that really was deferred.

C<value> is consumed: pass a reference with refcount 1, exactly as C<done> hands
one over.  The handle comes back with refcount 1 in turn.

The class is C<PerlMulticore::Handle> (see L<PerlMulticore>), loaded on demand.  If it cannot be loaded -
a partial installation, or an embedded perl without the library - C<value> is
returned bare rather than the call failing, since losing the inline path to a
missing module would be worse than the shape being wrong on a perl that is
already broken.  B<Experimental.>

=cut
*/

SV *
Perl_multicore_offload_ready(pTHX_ SV *value)
{
    SV *handle;
    dSP;

    PERL_ARGS_ASSERT_MULTICORE_OFFLOAD_READY;

    ENTER;
    SAVETMPS;

    if (!get_cvs("PerlMulticore::Handle::AWAIT_NEW_DONE", 0))
        require_pv("PerlMulticore.pm");

    SPAGAIN;
    PUSHMARK(SP);
    EXTEND(SP, 2);
    PUSHs(newSVpvs_flags("PerlMulticore::Handle", SVs_TEMP));
    PUSHs(sv_2mortal(value));
    PUTBACK;

    {
        SSize_t count = call_method("AWAIT_NEW_DONE", G_SCALAR | G_EVAL);

        SPAGAIN;

        /* on a die with G_SCALAR an undef is pushed, so the count alone does not
         * say whether the call worked */
        handle = count >= 1 && !SvTRUE(ERRSV) ? SvREFCNT_inc_NN(*SP)
                                              : SvREFCNT_inc_NN(value);

        SP -= count;
        PUTBACK;
    }

    FREETMPS;
    LEAVE;

    return handle;
}

/*
=for apidoc multicore_offload

Hand a blocking/CPU-bound C section to a worker thread while the interpreter
stays on its thread.  C<work(work_arg)> is pure C and must not touch the
interpreter; when it finishes, C<done(done_arg)> runs holding the interpreter and
returns the result marshalled into an SV.

The return value is a B<handle> object supplied by the registered backend.  The
offload may still be in flight when this returns, and the caller takes the result
out of the handle - with C<await> from a stackless caller, or C<get>, which
blocks, from a stackful one.  Handing back one shape whichever backend is
installed is what lets an XS module offer an asynchronous entry point without
knowing which one the application chose; F<perlmulticore.h> gives the methods the
handle implements, and the rule for how long C<work_arg> must stay valid.  A C
consumer that only wants the value can use the C<multicore_offload_sync> wrapper
there, which is the whole of what an offload used to be.

This is the dual of the C<perlinterp_release>/C<perlinterp_acquire> bracket - it
never migrates the interpreter, so it works where the bracket cannot: the bracket
hands the interpreter to another OS thread and so needs a perl that is not
I<using> ithreads, which on Windows is what the C<fork> emulation is built on.
With no offload backend installed it runs C<work> then C<done> inline and hands
back an already-resolved C<PerlMulticore::Handle> (blocking, but correct).
B<Experimental.>

=cut
*/

SV *
Perl_multicore_offload(pTHX_ perl_multicore_work_t work, void *work_arg,
                             perl_multicore_done_t done, void *done_arg)
{
    PERL_ARGS_ASSERT_MULTICORE_OFFLOAD;

    if (PL_multicore_offload)
        return PL_multicore_offload(work, work_arg, done, done_arg);

    /* no backend: run inline (blocking) and marshal the result here.  The
     * contexts are still supplied, so a module never has to special-case the
     * backendless path: no cancellation is possible here, hence a NULL flag. */
    {
        perl_multicore_work_ctx work_ctx;
        perl_multicore_done_ctx done_ctx;

        work_ctx.size   = sizeof(perl_multicore_work_ctx);
        work_ctx.cancel = NULL;

        work(work_arg, &work_ctx);

        done_ctx.size      = sizeof(perl_multicore_done_ctx);
        done_ctx.cancelled = 0;
        done_ctx.dropped   = 0;

        return Perl_multicore_offload_ready(aTHX_ done(aTHX_ done_arg, &done_ctx));
    }
}

/*
=for apidoc multicore_offload_sync

The synchronous form of C<multicore_offload> (see L<perlapi>): offload, wait for
the handle,
and return the value C<done> produced - which is the whole of what an offload was
before it returned a handle.  This is what a module whose entry point must keep
returning a value, or that offers no asynchronous entry point at all, wants: the
call blocks (transparently, under a green-thread backend, which runs its other
threads meanwhile), so the caller may keep its job on its own frame.

Hands over a reference, as C<done> does.  A croak from C<done>, or an exception
aimed at the calling green thread while it waits, is raised from here - after the
work has stopped, never while the worker is still writing into the caller's frame.

With no backend installed there is nothing to wait for and no handle worth
building, so C<work> and C<done> run right here.  B<Experimental.>

=cut
*/

SV *
Perl_multicore_offload_sync(pTHX_ perl_multicore_work_t work, void *work_arg,
                                  perl_multicore_done_t done, void *done_arg)
{
    SV *value;
    dSP;

    PERL_ARGS_ASSERT_MULTICORE_OFFLOAD_SYNC;

    if (!PL_multicore_offload) {
        perl_multicore_work_ctx work_ctx;
        perl_multicore_done_ctx done_ctx;

        work_ctx.size   = sizeof(perl_multicore_work_ctx);
        work_ctx.cancel = NULL;

        work(work_arg, &work_ctx);

        done_ctx.size      = sizeof(perl_multicore_done_ctx);
        done_ctx.cancelled = 0;
        done_ctx.dropped   = 0;

        return done(aTHX_ done_arg, &done_ctx);
    }

    ENTER;
    SAVETMPS;

    {
        SV *handle = PL_multicore_offload(work, work_arg, done, done_arg);
        SSize_t count;

        /* The scope owns the handle from here on, so it is released however this
         * leaves - including when `get` raises what `done` croaked with, and when
         * the green thread waiting in it is cancelled outright, which is how the
         * backend learns the job was dropped. */
        SAVEFREESV(handle);

        PUSHMARK(SP);
        XPUSHs(handle);
        PUTBACK;

        count = call_method("get", G_SCALAR);

        SPAGAIN;
        value = count >= 1 ? SvREFCNT_inc_NN(*SP) : &PL_sv_undef;
        SP -= count;
        PUTBACK;
    }

    FREETMPS;
    LEAVE;

    return value;
}

/*
=for apidoc multicore_register_offload

Install (or, with C<NULL>, remove) the offload backend that L</C<multicore_offload>>
dispatches to.  A backend that owns a worker-thread pool - such as one that also
registers the release/acquire bracket - can provide both.  B<Experimental.>

=cut
*/

void
Perl_multicore_register_offload(pTHX_ perl_multicore_offload_t offload)
{
    PERL_UNUSED_CONTEXT;
    PL_multicore_offload = offload;
}

/* ------------------------------------------------------------------------- *
 * Savestack suspend/resume API ("parkapi") - Phase 3: freeze/thaw/free/foreach_sv.
 *
 * A supported way to freeze (serialize + unwind) a region of the save stack
 * and later thaw (re-apply) it, so that suspend-a-scope consumers such as
 * Future::AsyncAwait and Coro need not re-implement leave_scope in reverse.
 * See Porting/savestack_suspend_api.md for the full design and plan.
 *
 * Each per-SAVEt handler is derived directly from the matching case in
 * Perl_leave_scope() above, so its refcount ownership is correct by
 * construction relative to core (the whole point of moving this into core).
 * A save type with no handler is refused (croak) - safe by default.
 *
 * Landed so far, each with freeze + thaw + discard + walk:
 *     SAVEt_SV     ("local $pkg_scalar";  Perl_save_scalar)   \ share
 *     SAVEt_SVREF  ("local $$ref" et al.; Perl_save_svref)    / restore_sv
 *     SAVEt_AV     ("local @pkg_array";   Perl_save_ary)      \ share
 *     SAVEt_HV     ("local %pkg_hash";    Perl_save_hash)     / avhv_common
 *     SAVEt_HELEM  ("local $h{k}";        Perl_save_helem_flags) \ share, re-
 *     SAVEt_AELEM  ("local $a[i]";        Perl_save_aelem_flags) / fetch slot
 *     SAVEt_DELETE ("local $h{new_key}";  Perl_save_hdelete)     \ delete now,
 *     SAVEt_ADELETE("local $a[new_idx]";  Perl_save_adelete)     / re-create
 *     SAVEt_GVSV          (GV scalar slot; pp_refgen etc.)       \ share
 *     SAVEt_GENERIC_SVREF (SAVEGENERICSV: PL_curstash, ...)      / restore_svp
 *     SAVEt_INT/IV/I32/I16/I8/BOOL/STRLEN (+ tight _SMALL forms) - localized C
 *                         scalars (S_*_cint); no SVs, no refcounts
 *     SAVEt_SPTR/VPTR/PPTR/HPTR/APTR - non-refcounted pointer slots (S_*_ptr)
 *     SAVEt_ITEM          (save_item; sv_replace-based localization)
 *   memory-reclaiming discards (run by frozen_free): SAVEt_FREESV/FREEPV/FREEOP/
 *     FREEPADNAME/FREECOPHH/FREERCPV/MORTALIZESV/READONLY_OFF, SAVEt_SET_SVFLAGS
 *   deferred user callbacks (run by savestack_frozen_run_deferred, NOT by
 *     frozen_free): SAVEt_DESTRUCTOR / SAVEt_DESTRUCTOR_X
 *   pad-scope bookkeeping (a suspended async sub's lexicals leave these):
 *     SAVEt_CLEARSV/CLEARPADRANGE (S_*_padclear), SAVEt_COMPPAD (S_*_comppad),
 *     SAVEt_PADSV_AND_MORTALIZE (S_*_padsv)
 * Still non-relocatable (croak): interpreter/compile state (ALLOC, REGCONTEXT,
 * HINTS, GP, GVSLOT, ...).  Tied/magical (SvSMAGICAL) targets are refused for
 * now - see S_can_freeze_*().
 * ------------------------------------------------------------------------- */

/* One serialized save-stack entry.  Per-type handlers interpret arg[] the same
 * way leave_scope's switch interprets the raw save-stack slots.  Layout of arg[]
 * is documented next to each handler. */
typedef struct {
    U8  type;                   /* SAVEt_* (already masked with SAVE_MASK) */
    U8  nargs;                  /* == leave_scope_arg_counts[type], 0..3   */
    ANY arg[3];                 /* the retained / serialized argument slots */
} SavedEntry;

struct PerlSavestackFrozen {
    U32         len;            /* number of entries in use                 */
    U32         max;            /* allocated capacity                       */
    SavedEntry *entries;        /* captured top-first (unwind order)        */
};

/* Per-SAVEt handler vtable, keyed by SAVEt_*.  A row with relocatable == FALSE
 * (and hence NULL handlers) makes freeze() croak on that type.  `can_freeze`
 * (optional) rejects individual instances a relocatable type cannot yet handle;
 * `freeze` parks + serializes one entry; `thaw` re-pushes a live entry and
 * re-applies the localized value; `discard` drops the refs an unthawed entry
 * still owns; `walk` enumerates the SVs an entry retains (introspection). */
typedef struct {
    bool relocatable;
    bool (*can_freeze)(pTHX_ const ANY *ap, UV uv);
    void (*freeze)    (pTHX_ const ANY *ap, UV uv, SavedEntry *out);
    void (*thaw)      (pTHX_ const SavedEntry *in);
    void (*discard)   (pTHX_ SavedEntry *entry);
    void (*walk)      (pTHX_ const SavedEntry *in,
                       PerlSavestackFrozenSVCb cb, void *ud);
} savetype_reloc;

/* --- SAVEt_SV / SAVEt_SVREF (scalar localization) ------------------------- *
 *
 * Save-stack slots captured (ap points at the first arg, as in leave_scope):
 *   ap[0] = target locator: SAVEt_SV -> GV* (holds a ref); SAVEt_SVREF -> SV**
 *   ap[1] = outer value (holds the ref save_scalar/save_svref took on it)
 * The slot itself (&GvSV(gv) or *sptr) currently holds the localized value,
 * which owns the slot's reference.
 *
 * Frozen entry layout:
 *   arg[0] = ap[0]        (GV* with ref retained | SV** locator, no ref)
 *   arg[1] = localized SV (ref retained, transferred out of the slot)
 */

/* Re-sync a magical target after its value slot has been changed, mirroring the
 * PL_localizing=2; mg_set() that leave_scope runs when restoring a localized
 * value (scope.c restore_sv / avhv_common).  Called on BOTH transitions so a
 * magical global tracks whatever is currently installed:
 *   - freeze (park): after reverting the slot to the OUTER value, so e.g. %ENV
 *     re-syncs the real environment to the outer value while suspended;
 *   - thaw (restore): after re-installing the LOCALIZED value.
 * Caveat: mg_set can in principle die (a tied STORE that throws); if that
 * happens during freeze the partially-built blob leaks and the save stack is
 * left partly unwound.  This is the same exposure the hand-rolled FAA suspend
 * has; tied containers are refused up front (S_can_freeze_elem/_delete) so this
 * is bounded to non-tied magic such as %ENV / %SIG. */
static void
S_resync_magic(pTHX_ SV *sv)
{
    if (SvSMAGICAL(sv)) {
        PL_localizing = 2;
        mg_set(sv);
        PL_localizing = 0;
    }
}

static bool
S_can_freeze_sv(pTHX_ const ANY *ap, UV uv)
{
    PERL_UNUSED_ARG(uv);
    PERL_UNUSED_ARG(ap);
    /* A magical outer value is fine: freeze/thaw replay its set-magic via
     * S_resync_magic (mirroring leave_scope's restore_sv). */
    return TRUE;
}

static void
S_freeze_sv(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    const U8 type = (U8)uv & SAVE_MASK;
    SV **svp      = (type == SAVEt_SV) ? &GvSV(ap[0].any_gv) : ap[0].any_svp;
    SV *localized = *svp;           /* current value: owns the slot's ref  */
    SV *outer     = ap[1].any_sv;   /* pre-local value: owns save_*'s inc   */

    /* Mirror leave_scope's non-magical restore_sv: put the outer value back in
     * the slot and drop the extra ref save_scalar/save_svref took on it, so the
     * slot again solely owns it.  Unlike leave_scope we do NOT free the
     * localized value or (for SAVEt_SV) the GV: those refs are transferred into
     * the frozen entry for thaw()/discard(). */
    *svp = outer;
    SvREFCNT_dec_NN(outer);
    S_resync_magic(aTHX_ outer);      /* magical (e.g. tied) target: re-sync outer */

    out->arg[0]        = ap[0];       /* GV* (ref kept) | SV** locator (no ref) */
    out->arg[1].any_sv = localized;   /* retained; slot's ref now owned by blob */
}

static void
S_thaw_sv(pTHX_ const SavedEntry *in)
{
    const U8 type = in->type;
    SV *localized = in->arg[1].any_sv;
    SV **svp;
    void *locator;

    if (type == SAVEt_SV) {
        GV *gv  = in->arg[0].any_gv;
        svp     = &GvSV(gv);
        locator = (void *)gv;      /* transfer the retained GV ref to the entry */
    }
    else { /* SAVEt_SVREF */
        svp     = in->arg[0].any_svp;
        locator = (void *)svp;     /* raw locator; save_svref held no ref on it */
    }

    /* Re-push the save entry that freeze() parked away, then re-install the
     * localized value - mirroring save_scalar/save_svref + save_scalar_at.  The
     * slot currently holds the outer value: SvREFCNT_inc it for the new entry's
     * second slot (as save_*() did) before overwriting the slot.  The blob's
     * refs on the locator (SAVEt_SV) and on the localized value are transferred
     * onto the live stack / into the slot, so thaw() must not run discard. */
    save_pushptrptr(locator, SvREFCNT_inc(*svp), (int)type);
    *svp = localized;
    S_resync_magic(aTHX_ localized);  /* magical target: re-sync to localized */
}

static void
S_discard_sv(pTHX_ SavedEntry *entry)
{
    if (entry->type == SAVEt_SV)
        SvREFCNT_dec(entry->arg[0].any_gv);   /* retained GV ref            */
    SvREFCNT_dec(entry->arg[1].any_sv);       /* retained localized value   */
}

static void
S_walk_sv(pTHX_ const SavedEntry *in, PerlSavestackFrozenSVCb cb, void *ud)
{
    if (in->type == SAVEt_SV)
        cb(aTHX_ (SV *)in->arg[0].any_gv, "SAVEt_SV localized GV", ud);
    cb(aTHX_ in->arg[1].any_sv,
       in->type == SAVEt_SV ? "SAVEt_SV localized value"
                            : "SAVEt_SVREF localized value", ud);
}

/* --- SAVEt_AV / SAVEt_HV (array / hash localization) ---------------------- *
 *
 * Save-stack slots captured (ap points at the first arg):
 *   ap[0] = GV* (holds a ref, from save_ary/save_hash's SvREFCNT_inc)
 *   ap[1] = outer AV or HV -- NOTE: NOT reference-counted by save_ary/save_hash;
 *           the slot's own ref was transferred onto the stack.
 * GvAV(gv) / GvHV(gv) currently holds the localized (new) container.
 *
 * This is the inverse refcount convention to the scalar family: leave_scope's
 * avhv_common frees the localized container and the GV but does NOT touch the
 * outer container (it just re-adopts the transferred ref).  So freeze does no
 * SvREFCNT_dec at all - it reverts the slot to the outer container and retains
 * the GV and the localized container in the frozen entry.
 *
 * Frozen entry layout:
 *   arg[0] = GV* (ref retained)
 *   arg[1] = localized AV or HV (ref retained, transferred out of the slot)
 */

static bool
S_can_freeze_avhv(pTHX_ const ANY *ap, UV uv)
{
    PERL_UNUSED_ARG(uv);
    PERL_UNUSED_ARG(ap);
    /* A magical outer container (e.g. local %ENV / %SIG) is fine: freeze/thaw
     * replay avhv_common's set-magic via S_resync_magic. */
    return TRUE;
}

static void
S_freeze_avhv(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    const U8 type = (U8)uv & SAVE_MASK;
    GV *gv        = ap[0].any_gv;
    SV *localized;

    if (type == SAVEt_AV) {
        localized = (SV *)GvAV(gv);
        GvAV(gv)  = ap[1].any_av;   /* revert to outer; no inc (as leave_scope) */
    }
    else { /* SAVEt_HV */
        localized = (SV *)GvHV(gv);
        GvHV(gv)  = ap[1].any_hv;
    }
    S_resync_magic(aTHX_ ap[1].any_sv);  /* magical (%ENV/%SIG): re-sync to outer */

    out->arg[0].any_gv = gv;         /* retained (leave_scope would dec)        */
    out->arg[1].any_sv = localized;  /* retained (leave_scope would dec)        */
}

static void
S_thaw_avhv(pTHX_ const SavedEntry *in)
{
    const U8 type = in->type;
    GV *gv        = in->arg[0].any_gv;
    SV *localized = in->arg[1].any_sv;

    /* Re-push the parked entry, transferring the blob's GV ref back onto it (as
     * save_ary/save_hash held one) and the current outer container without a
     * fresh inc (as they did), then re-install the localized container. */
    if (type == SAVEt_AV) {
        save_pushptrptr(gv, GvAV(gv), SAVEt_AV);
        GvAV(gv) = (AV *)localized;
    }
    else { /* SAVEt_HV */
        save_pushptrptr(gv, GvHV(gv), SAVEt_HV);
        GvHV(gv) = (HV *)localized;
    }
    S_resync_magic(aTHX_ localized);  /* magical (%ENV/%SIG): re-sync to localized */
}

static void
S_discard_avhv(pTHX_ SavedEntry *entry)
{
    SvREFCNT_dec(entry->arg[0].any_gv);   /* retained GV ref                  */
    SvREFCNT_dec(entry->arg[1].any_sv);   /* retained localized AV/HV         */
}

static void
S_walk_avhv(pTHX_ const SavedEntry *in, PerlSavestackFrozenSVCb cb, void *ud)
{
    cb(aTHX_ (SV *)in->arg[0].any_gv,
       in->type == SAVEt_AV ? "SAVEt_AV localized GV"
                            : "SAVEt_HV localized GV", ud);
    cb(aTHX_ in->arg[1].any_sv,
       in->type == SAVEt_AV ? "SAVEt_AV localized value"
                            : "SAVEt_HV localized value", ud);
}

/* --- SAVEt_HELEM / SAVEt_AELEM (hash / array element localization) -------- *
 *
 * Save-stack slots captured (ap points at the first arg):
 *   ap[0] = container: HV (HELEM) or AV (AELEM), holds a ref
 *   ap[1] = key: HELEM -> SV* copy (owned); AELEM -> IV index (no ref)
 *   ap[2] = outer value (holds a ref, from save_helem/save_aelem's inc)
 * The element slot currently holds the localized value.
 *
 * An element's address is NOT stable across a suspension (the hash may rehash,
 * the array may be reallocated), so - exactly like leave_scope - both freeze
 * and thaw locate the element afresh from (container, key/idx).  Tied/magical
 * containers, non-real arrays and vanished elements are refused for now
 * (S_can_freeze_elem).  Otherwise this mirrors the scalar family's restore_sv.
 *
 * Frozen entry layout:
 *   arg[0] = container (ref retained)
 *   arg[1] = key SV (HELEM, ref retained) | IV index (AELEM)
 *   arg[2] = localized value (ref retained)
 */

static SV **
S_elem_slot(pTHX_ U8 type, SV *container, const ANY *key, I32 lval)
{
    if (type == SAVEt_HELEM) {
        HE *he = hv_fetch_ent((HV *)container, key->any_sv, lval, 0);
        return he ? &HeVAL(he) : NULL;
    }
    return av_fetch((AV *)container, key->any_iv, lval);
}

static bool
S_can_freeze_elem(pTHX_ const ANY *ap, UV uv)
{
    const U8 type = (U8)uv & SAVE_MASK;
    SV *container = ap[0].any_sv;
    SV **svp;

    /* Tied elements route through mg_copy/FETCH/STORE rather than the HE/AV slot
     * we manipulate directly; refuse them (as the hand-rolled FAA suspend does).
     * Other container magic - %ENV / %SIG (envelem/sigelem on the values) - is
     * fine: its set-magic is replayed by S_resync_magic on freeze and thaw. */
    if (mg_find(container, PERL_MAGIC_tied))
        return FALSE;
    if (type == SAVEt_AELEM && !AvREAL((AV *)container))    /* reify-guard path */
        return FALSE;

    svp = S_elem_slot(aTHX_ type, container, &ap[1], 0);    /* non-creating */
    return svp && *svp && *svp != &PL_sv_undef;
}

static void
S_freeze_elem(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    const U8 type = (U8)uv & SAVE_MASK;
    SV *container = ap[0].any_sv;
    SV *outer     = ap[2].any_sv;
    SV **svp      = S_elem_slot(aTHX_ type, container, &ap[1], 1);
    SV *localized;

    assert(svp);                     /* pass 1 (can_freeze) verified existence */
    localized = *svp;

    *svp = outer;                    /* revert element to outer value */
    SvREFCNT_dec_NN(outer);          /* drop save_*elem's inc (as leave_scope) */
    S_resync_magic(aTHX_ outer);     /* magical element (%ENV/%SIG): re-sync outer */

    out->arg[0] = ap[0];             /* container (ref retained)               */
    out->arg[1] = ap[1];             /* key SV retained (HELEM) | idx (AELEM)  */
    out->arg[2].any_sv = localized;  /* retained                               */
}

static void
S_thaw_elem(pTHX_ const SavedEntry *in)
{
    const U8 type = in->type;
    SV *container = in->arg[0].any_sv;
    SV *localized = in->arg[2].any_sv;
    SV **svp      = S_elem_slot(aTHX_ type, container, &in->arg[1], 1);
    dSS_ADD;

    assert(svp);

    /* Re-push the parked entry mirroring save_helem_flags/save_aelem_flags,
     * transferring the blob's container ref (and, for HELEM, the key copy) back
     * onto it and taking a fresh ref on the current outer value, then re-install
     * the localized value. */
    SS_ADD_PTR(container);
    if (type == SAVEt_HELEM)
        SS_ADD_PTR(in->arg[1].any_sv);
    else
        SS_ADD_IV(in->arg[1].any_iv);
    SS_ADD_PTR(SvREFCNT_inc(*svp));
    SS_ADD_UV(type);
    SS_ADD_END(4);

    *svp = localized;
    S_resync_magic(aTHX_ localized); /* magical element (%ENV/%SIG): re-sync */
}

static void
S_discard_elem(pTHX_ SavedEntry *entry)
{
    SvREFCNT_dec(entry->arg[0].any_sv);        /* container                   */
    if (entry->type == SAVEt_HELEM)
        SvREFCNT_dec(entry->arg[1].any_sv);    /* key copy                    */
    SvREFCNT_dec(entry->arg[2].any_sv);        /* localized value             */
}

static void
S_walk_elem(pTHX_ const SavedEntry *in, PerlSavestackFrozenSVCb cb, void *ud)
{
    const bool helem = (in->type == SAVEt_HELEM);
    cb(aTHX_ in->arg[0].any_sv,
       helem ? "SAVEt_HELEM container" : "SAVEt_AELEM container", ud);
    if (helem)
        cb(aTHX_ in->arg[1].any_sv, "SAVEt_HELEM key", ud);
    cb(aTHX_ in->arg[2].any_sv,
       helem ? "SAVEt_HELEM localized value" : "SAVEt_AELEM localized value", ud);
}

/* --- SAVEt_DELETE / SAVEt_ADELETE (localized new element) ----------------- *
 *
 * Pushed (instead of SAVEt_HELEM/SAVEt_AELEM) when the localized key/index did
 * NOT exist beforehand (pp_helem/pp_aelem: !preeminent).  Restoring the outer
 * state therefore means DELETING the element again, which is what leave_scope
 * does (hv_delete / av_delete, G_DISCARD).
 *
 * Save-stack slots captured (ap points at the first arg):
 *   SAVEt_DELETE : ap[0] = key char* (owned), ap[1] = I32 klen, ap[2] = HV* (+1)
 *   SAVEt_ADELETE: ap[0] = IV index,          ap[1] = AV* (+1)
 *
 * Park: perform the deletion now (so the outer world sees the element absent),
 * but keep the deleted value - and, for HELEM, the key as an SV - so thaw can
 * re-create the element and re-arm the delete.  The owned char* key is consumed
 * into an SV to free a slot for the retained value.
 *
 * Frozen entry layout:
 *   SAVEt_DELETE : arg[0] = key SV (owned), arg[1] = HV* (ref), arg[2] = value
 *   SAVEt_ADELETE: arg[0] = IV index,       arg[1] = AV* (ref), arg[2] = value
 */

static bool
S_can_freeze_delete(pTHX_ const ANY *ap, UV uv)
{
    const U8 type = (U8)uv & SAVE_MASK;
    /* Refuse tied containers (their delete/store route through DELETE/STORE, not
     * the slot); allow other magic (%ENV/%SIG) - hv_delete/av_delete run the
     * container's delete-magic on freeze, and thaw replays set-magic. */
    if (type == SAVEt_DELETE) {
        HV *hv = ap[2].any_hv;
        if (mg_find((SV *)hv, PERL_MAGIC_tied))
            return FALSE;
        return cBOOL(hv_exists(hv, ap[0].any_pv, ap[1].any_i32));
    }
    else { /* SAVEt_ADELETE */
        AV *av = ap[1].any_av;
        if (mg_find((SV *)av, PERL_MAGIC_tied))
            return FALSE;
        return cBOOL(av_exists(av, ap[0].any_iv));
    }
}

static void
S_freeze_delete(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    const U8 type = (U8)uv & SAVE_MASK;

    if (type == SAVEt_DELETE) {
        char  *keypv = ap[0].any_pv;
        I32    klen  = ap[1].any_i32;
        HV    *hv    = ap[2].any_hv;
        STRLEN len   = (klen < 0) ? (STRLEN)(-klen) : (STRLEN)klen;
        SV    *value = hv_delete(hv, keypv, klen, 0);   /* remove; mortal value */
        SV    *keysv;

        assert(value);
        SvREFCNT_inc_simple_void_NN(value);             /* retain past FREETMPS */
        keysv = newSVpvn(keypv, len);
        if (klen < 0)
            SvUTF8_on(keysv);
        Safefree(keypv);                                /* consume owned key pv */

        out->arg[0].any_sv = keysv;    /* owned key SV                          */
        out->arg[1].any_hv = hv;       /* retained (leave_scope would FREESV)   */
        out->arg[2].any_sv = value;    /* retained deleted value                */
    }
    else { /* SAVEt_ADELETE */
        SSize_t idx  = ap[0].any_iv;
        AV     *av   = ap[1].any_av;
        SV     *value = av_delete(av, idx, 0);

        assert(value);
        SvREFCNT_inc_simple_void_NN(value);

        out->arg[0].any_iv = idx;
        out->arg[1].any_av = av;
        out->arg[2].any_sv = value;
    }
}

static void
S_thaw_delete(pTHX_ const SavedEntry *in)
{
    const U8 type = in->type;
    SV *value = in->arg[2].any_sv;

    if (type == SAVEt_DELETE) {
        SV         *keysv = in->arg[0].any_sv;
        HV         *hv    = in->arg[1].any_hv;
        STRLEN      len;
        const char *kp    = SvPV_const(keysv, len);
        I32         klen  = SvUTF8(keysv) ? -(I32)len : (I32)len;
        char       *newpv = savepvn(kp, len);
        dSS_ADD;

        (void)hv_store_ent(hv, keysv, value, 0);   /* value ref -> hash         */
        SS_ADD_PTR(newpv);                         /* fresh owned key (as save) */
        SS_ADD_INT(klen);
        SS_ADD_PTR(hv);                            /* transfer blob's HV ref    */
        SS_ADD_UV(SAVEt_DELETE);
        SS_ADD_END(4);
        SvREFCNT_dec(keysv);                       /* blob's key SV consumed    */
        S_resync_magic(aTHX_ value);   /* magical element (%ENV/%SIG): re-sync  */
    }
    else { /* SAVEt_ADELETE */
        SSize_t idx = in->arg[0].any_iv;
        AV     *av  = in->arg[1].any_av;
        dSS_ADD;

        (void)av_store(av, idx, value);            /* value ref -> array        */
        SS_ADD_UV((UV)idx);
        SS_ADD_PTR(av);                            /* transfer blob's AV ref    */
        SS_ADD_IV(SAVEt_ADELETE);
        SS_ADD_END(3);
        S_resync_magic(aTHX_ value);   /* magical element: re-sync              */
    }
}

static void
S_discard_delete(pTHX_ SavedEntry *entry)
{
    if (entry->type == SAVEt_DELETE)
        SvREFCNT_dec(entry->arg[0].any_sv);   /* retained key SV                */
    SvREFCNT_dec(entry->arg[1].any_sv);       /* retained container (HV/AV)     */
    SvREFCNT_dec(entry->arg[2].any_sv);       /* retained deleted value         */
}

static void
S_walk_delete(pTHX_ const SavedEntry *in, PerlSavestackFrozenSVCb cb, void *ud)
{
    const bool hd = (in->type == SAVEt_DELETE);
    if (hd)
        cb(aTHX_ in->arg[0].any_sv, "SAVEt_DELETE key", ud);
    cb(aTHX_ in->arg[1].any_sv,
       hd ? "SAVEt_DELETE hash" : "SAVEt_ADELETE array", ud);
    cb(aTHX_ in->arg[2].any_sv,
       hd ? "SAVEt_DELETE value" : "SAVEt_ADELETE value", ud);
}

/* --- SAVEt_GVSV / SAVEt_GENERIC_SVREF (restore_svp scalar cousins) -------- *
 *
 * Save-stack slots captured (ap points at the first arg):
 *   SAVEt_GVSV        : ap[0] = GV*  (no ref)  -> slot is &GvSV(gv)
 *   SAVEt_GENERIC_SVREF: ap[0] = SV** (no ref) -> slot is that SV**
 *   ap[1] = outer value (holds the ref save took on it)
 * The slot currently holds the localized value (owns the slot's ref).
 *
 * Both use leave_scope's restore_svp, which - unlike restore_sv - holds no ref
 * on the locator and runs NO set-magic, so these are magic-safe and need no
 * can_freeze gate.  freeze reverts the slot to the outer value and drops the
 * saved ref on it (mirroring restore_svp's dec(a1)), retaining only the
 * localized value.
 *
 * Frozen entry layout:  arg[0] = locator (GV* or SV**, no ref)
 *                       arg[1] = localized value (ref retained)
 */

static SV **
S_svp_slot(pTHX_ U8 type, const ANY *locator)
{
    return (type == SAVEt_GVSV) ? &GvSV(locator->any_gv) : locator->any_svp;
}

static void
S_freeze_svp(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    const U8 type = (U8)uv & SAVE_MASK;
    SV **svp      = S_svp_slot(aTHX_ type, &ap[0]);
    SV *localized = *svp;
    SV *outer     = ap[1].any_sv;

    *svp = outer;
    SvREFCNT_dec_NN(outer);          /* release save's ref (restore_svp dec) */

    out->arg[0]        = ap[0];       /* locator (no ref)                    */
    out->arg[1].any_sv = localized;   /* retained                            */
}

static void
S_thaw_svp(pTHX_ const SavedEntry *in)
{
    const U8 type = in->type;
    SV **svp      = S_svp_slot(aTHX_ type, &in->arg[0]);
    SV *localized = in->arg[1].any_sv;

    /* Re-push mirroring pp's SAVEt_GVSV / save_generic_svref: locator (no ref)
     * plus a fresh ref on the current outer value, then re-install localized. */
    save_pushptrptr(in->arg[0].any_ptr, SvREFCNT_inc(*svp), (int)type);
    *svp = localized;
}

static void
S_discard_svp(pTHX_ SavedEntry *entry)
{
    SvREFCNT_dec(entry->arg[1].any_sv);   /* only the localized value is owned */
}

static void
S_walk_svp(pTHX_ const SavedEntry *in, PerlSavestackFrozenSVCb cb, void *ud)
{
    cb(aTHX_ in->arg[1].any_sv,
       in->type == SAVEt_GVSV ? "SAVEt_GVSV localized value"
                              : "SAVEt_GENERIC_SVREF localized value", ud);
}

/* --- localized C scalars: SAVEt_INT{,_SMALL}, IV, I32{,_SMALL}, I16, I8,
 *     BOOL, STRLEN{,_SMALL} ---------------------------------------------------
 *
 * These localize a C variable (int / IV / I32 / ... / STRLEN) rather than an
 * SV, so there are no references to manage and nothing for walk/discard to do
 * (their vtable rows leave can_freeze/discard/walk NULL).  The outer value is
 * held either in a separate slot (the wide INT/IV/I32/STRLEN forms) or packed
 * into the type word (the tight _SMALL / I16 / I8 / BOOL forms); the pointer to
 * the C variable is the remaining slot.  freeze reads the current (localized)
 * value, writes the outer value back, and keeps the localized value as an IV;
 * thaw re-arms the save with the matching save_*() helper - which re-encodes
 * the (now outer) value, choosing the tight form when it fits - then writes the
 * localized value back.
 *
 * Frozen entry layout:  arg[0] = pointer to the C variable
 *                       arg[1] = localized value (as IV)
 */

static void
S_freeze_cint(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    const U8 type = (U8)uv & SAVE_MASK;
    void *ptr;
    IV    localized;

    switch (type) {
    case SAVEt_INT:
        ptr = ap[1].any_ptr; localized = (IV)*(int*)ptr;
        *(int*)ptr = (int)ap[0].any_i32;                    break;
    case SAVEt_INT_SMALL:
        ptr = ap[0].any_ptr; localized = (IV)*(int*)ptr;
        *(int*)ptr = (int)(uv >> SAVE_TIGHT_SHIFT);         break;
    case SAVEt_IV:
        ptr = ap[1].any_ptr; localized = *(IV*)ptr;
        *(IV*)ptr = ap[0].any_iv;                           break;
    case SAVEt_I32:
        ptr = ap[1].any_ptr; localized = (IV)*(I32*)ptr;
        *(I32*)ptr = ap[0].any_i32;                         break;
    case SAVEt_I32_SMALL:
        ptr = ap[0].any_ptr; localized = (IV)*(I32*)ptr;
        *(I32*)ptr = (I32)(uv >> SAVE_TIGHT_SHIFT);         break;
    case SAVEt_I16:
        ptr = ap[0].any_ptr; localized = (IV)*(I16*)ptr;
        *(I16*)ptr = (I16)(uv >> 8);                        break;
    case SAVEt_I8:
        ptr = ap[0].any_ptr; localized = (IV)*(I8*)ptr;
        *(I8*)ptr = (I8)(uv >> 8);                          break;
    case SAVEt_BOOL:
        ptr = ap[0].any_ptr; localized = (IV)*(bool*)ptr;
        *(bool*)ptr = cBOOL(uv >> 8);                       break;
    case SAVEt_STRLEN:
        ptr = ap[1].any_ptr; localized = (IV)*(STRLEN*)ptr;
        *(STRLEN*)ptr = (STRLEN)ap[0].any_iv;               break;
    case SAVEt_STRLEN_SMALL:
        ptr = ap[0].any_ptr; localized = (IV)*(STRLEN*)ptr;
        *(STRLEN*)ptr = (STRLEN)(uv >> SAVE_TIGHT_SHIFT);   break;
    default:
        NOT_REACHED; /* NOTREACHED */
    }

    out->arg[0].any_ptr = ptr;
    out->arg[1].any_iv  = localized;
}

static void
S_thaw_cint(pTHX_ const SavedEntry *in)
{
    void    *ptr = in->arg[0].any_ptr;
    const IV loc = in->arg[1].any_iv;

    switch (in->type) {
    case SAVEt_INT: case SAVEt_INT_SMALL:
        save_int((int*)ptr);        *(int*)ptr    = (int)loc;       break;
    case SAVEt_IV:
        save_iv((IV*)ptr);          *(IV*)ptr     = (IV)loc;        break;
    case SAVEt_I32: case SAVEt_I32_SMALL:
        save_I32((I32*)ptr);        *(I32*)ptr    = (I32)loc;       break;
    case SAVEt_I16:
        save_I16((I16*)ptr);        *(I16*)ptr    = (I16)loc;       break;
    case SAVEt_I8:
        save_I8((I8*)ptr);          *(I8*)ptr     = (I8)loc;        break;
    case SAVEt_BOOL:
        save_bool((bool*)ptr);      *(bool*)ptr   = cBOOL(loc);     break;
    case SAVEt_STRLEN: case SAVEt_STRLEN_SMALL:
        save_strlen((STRLEN*)ptr);  *(STRLEN*)ptr = (STRLEN)loc;    break;
    default:
        NOT_REACHED; /* NOTREACHED */
    }
}

/* --- SAVEt_SPTR / VPTR / PPTR / HPTR / APTR (non-refcounted pointer slots) - *
 *
 * leave_scope restores *slot = outer with no reference counting - these slots
 * do not own their pointee (save_sptr and friends push (*slot, slot) with no
 * inc).  ap[0] = outer value, ap[1] = the slot.  freeze saves the current
 * pointer, writes the outer value back, and keeps the localized pointer (again
 * without a ref); thaw re-pushes (current outer, slot) and reinstalls the
 * localized pointer.  Nothing is owned, so discard/walk stay NULL.
 *
 * Frozen entry layout:  arg[0] = slot, arg[1] = localized pointer (no ref)
 */

static void
S_freeze_ptr(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    SV **slot     = ap[1].any_svp;
    SV *localized = *slot;

    PERL_UNUSED_ARG(uv);
    *slot = ap[0].any_sv;            /* restore outer (no refcounting) */

    out->arg[0].any_svp = slot;
    out->arg[1].any_sv  = localized; /* raw pointer, not owned         */
}

static void
S_thaw_ptr(pTHX_ const SavedEntry *in)
{
    SV **slot = in->arg[0].any_svp;

    save_pushptrptr(*slot, slot, (int)in->type);   /* (current outer, slot) */
    *slot = in->arg[1].any_sv;                      /* reinstall localized   */
}

/* --- SAVEt_ITEM (localized SV value via sv_replace) ----------------------- *
 *
 * save_item snapshots an SV's value (newSVsv) and leave_scope sv_replace()s the
 * snapshot back into it.  ap[0] = the target SV (persistent identity),
 * ap[1] = the outer-value snapshot (consumed by sv_replace).  freeze snapshots
 * the current (localized) value, replaces the outer snapshot back into the
 * target, and retains the localized snapshot; thaw re-snapshots the (now outer)
 * value via save_item and sv_replace()s the localized value back in.  Magical
 * targets are refused (leave_scope runs set-magic on them).
 *
 * Frozen entry layout:  arg[0] = target SV (borrowed)
 *                       arg[1] = localized value snapshot (ref retained)
 */

static bool
S_can_freeze_item(pTHX_ const ANY *ap, UV uv)
{
    PERL_UNUSED_ARG(uv);
    return !SvSMAGICAL(ap[0].any_sv);
}

static void
S_freeze_item(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    SV *item = ap[0].any_sv;

    PERL_UNUSED_ARG(uv);
    out->arg[1].any_sv = newSVsv(item);   /* snapshot the localized value    */
    sv_replace(item, ap[1].any_sv);        /* item := outer (consumes ap[1])  */
    out->arg[0].any_sv = item;             /* borrowed target                 */
}

static void
S_thaw_item(pTHX_ const SavedEntry *in)
{
    SV *item = in->arg[0].any_sv;

    save_item(item);                       /* re-snapshot outer, push SAVEt_ITEM */
    sv_replace(item, in->arg[1].any_sv);   /* item := localized (consumes)       */
}

static void
S_discard_item(pTHX_ SavedEntry *entry)
{
    SvREFCNT_dec(entry->arg[1].any_sv);    /* localized snapshot */
}

static void
S_walk_item(pTHX_ const SavedEntry *in, PerlSavestackFrozenSVCb cb, void *ud)
{
    cb(aTHX_ in->arg[1].any_sv, "SAVEt_ITEM localized value", ud);
}

/* --- Deferred-action / bookkeeping save types ---------------------------- *
 *
 * Unlike the value-localization types above, these do not restore an outer
 * value - they register an ACTION to run when the scope exits: free an SV / PV
 * / op, mortalize an SV, call a destructor or "finally" block, restore SvFLAGS,
 * etc.
 *
 * Cancel semantics (decided): savestack_frozen_free() RUNS the action, because
 * cancelling a suspended scope tears it down as if it had been left normally -
 * RAII-style cleanups (destructors, finally blocks, frees) must execute,
 * exactly as leave_scope would run them (and as Future::AsyncAwait already runs
 * finally blocks on cancel).  So: freeze() lifts the action off the live save
 * stack, taking ownership of any resource it holds; thaw() re-registers it so
 * it runs at the eventual real scope exit; discard() executes it.  freeze()
 * itself never runs the action.
 *
 * Covered: the 1-arg pointer actions SAVEt_FREESV / FREEPV / FREEOP /
 * FREEPADNAME / FREECOPHH / FREERCPV / MORTALIZESV / READONLY_OFF
 * (S_thaw_action1 / S_discard_action1); SAVEt_DESTRUCTOR / DESTRUCTOR_X
 * (S_*_destructor); SAVEt_SET_SVFLAGS (S_*_setflags).  All share S_freeze_action
 * (a straight capture of the arg slots - ownership of any held resource simply
 * transfers from the live stack into the blob).
 *
 * Resources these hold (an SV ref, a malloc'd PV, a destructor's data) are
 * borrowed by / owned by the blob until it is thawed or freed; a blob that is
 * neither thawed nor freed leaks them, exactly as leaking a live scope would.
 */

static void
S_freeze_action(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    U8 i;
    PERL_UNUSED_ARG(uv);
    for (i = 0; i < out->nargs; i++)
        out->arg[i] = ap[i];
}

static void
S_thaw_action1(pTHX_ const SavedEntry *in)
{
    save_pushptr(in->arg[0].any_ptr, (int)in->type);
}

static void
S_discard_action1(pTHX_ SavedEntry *entry)
{
    void *p = entry->arg[0].any_ptr;
    switch (entry->type) {
    case SAVEt_FREESV:       SvREFCNT_dec((SV *)p);              break;
    case SAVEt_MORTALIZESV:  sv_2mortal((SV *)p);                break;
    case SAVEt_FREEPV:       Safefree(p);                        break;
    case SAVEt_FREEOP:       op_free((OP *)p);                   break;
    case SAVEt_FREEPADNAME:  PadnameREFCNT_dec((PADNAME *)p);    break;
    case SAVEt_FREECOPHH:    cophh_free((COPHH *)p);             break;
    case SAVEt_FREERCPV:     (void)rcpv_free((char *)p);         break;
    case SAVEt_READONLY_OFF: SvREADONLY_off((SV *)p);            break;
    default:                 NOT_REACHED; /* NOTREACHED */
    }
}

static void
S_walk_action1(pTHX_ const SavedEntry *in, PerlSavestackFrozenSVCb cb, void *ud)
{
    switch (in->type) {
    case SAVEt_FREESV:       cb(aTHX_ in->arg[0].any_sv, "SAVEt_FREESV",       ud); break;
    case SAVEt_MORTALIZESV:  cb(aTHX_ in->arg[0].any_sv, "SAVEt_MORTALIZESV",  ud); break;
    case SAVEt_READONLY_OFF: cb(aTHX_ in->arg[0].any_sv, "SAVEt_READONLY_OFF", ud); break;
    default: break;         /* opaque resources (PV / OP / PADNAME / COPHH / RCPV) */
    }
}

static void
S_thaw_destructor(pTHX_ const SavedEntry *in)
{
    dSS_ADD;
    if (in->type == SAVEt_DESTRUCTOR)
        SS_ADD_DPTR(in->arg[0].any_dptr);
    else
        SS_ADD_DXPTR(in->arg[0].any_dxptr);
    SS_ADD_PTR(in->arg[1].any_ptr);
    SS_ADD_UV((UV)in->type);
    SS_ADD_END(3);
}

/* Run a parked SAVEDESTRUCTOR / SAVEDESTRUCTOR_X - a deferred *user* callback
 * (e.g. a Syntax::Keyword defer / try-finally block).  Driven only by
 * savestack_frozen_run_deferred, NOT by frozen_free, so a consumer that merely
 * discards a suspended scope does not fire user-visible finalizers. */
static void
S_run_destructor(pTHX_ SavedEntry *entry)
{
    if (entry->type == SAVEt_DESTRUCTOR)
        (*entry->arg[0].any_dptr)(entry->arg[1].any_ptr);
    else
        (*entry->arg[0].any_dxptr)(aTHX_ entry->arg[1].any_ptr);
}

static void
S_thaw_setflags(pTHX_ const SavedEntry *in)
{
    dSS_ADD;
    SS_ADD_PTR(in->arg[0].any_sv);
    SS_ADD_INT(in->arg[1].any_i32);
    SS_ADD_INT(in->arg[2].any_i32);
    SS_ADD_UV((UV)SAVEt_SET_SVFLAGS);
    SS_ADD_END(4);
}

static void
S_discard_setflags(pTHX_ SavedEntry *entry)
{
    SV *sv = entry->arg[0].any_sv;
    SvFLAGS(sv) &= ~(entry->arg[1].any_u32);
    SvFLAGS(sv) |=   entry->arg[2].any_u32;
}

/* --- SAVEt_GENERIC_PVREF / SHARED_PVREF / RCPV (localized char* slots) ---- *
 *
 * char* analogues of GENERIC_SVREF: a pointer slot whose value is restored, and
 * whose localized value is freed, at scope exit.  The slot/outer arg order and
 * the free function differ:
 *   GENERIC_PVREF: ap[0]=outer pv, ap[1]=char** slot   -> Safefree
 *   SHARED_PVREF : ap[0]=char** slot, ap[1]=outer pv   -> PerlMemShared_free
 *   RCPV         : ap[0]=char** slot, ap[1]=+1 rcpv copy of outer (rc-counted)
 *
 * For GENERIC/SHARED, leave_scope only frees/sets when the value changed, so
 * freeze reverts the slot (when changed) and retains the localized pv; discard
 * frees it (guarded the same way).  RCPV is ref-counted: freeze installs the
 * outer copy and releases its extra ref, retaining the localized reference for
 * discard.  No SVs are owned, so walk stays NULL.
 *
 * Frozen entry: arg[0]=slot (char**), arg[1]=localized pv,
 *               arg[2]=outer pv (GENERIC/SHARED only).
 */

static void
S_freeze_pvref(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    const U8 type    = (U8)uv & SAVE_MASK;
    char   **slot    = (type == SAVEt_GENERIC_PVREF) ? ap[1].any_pvp : ap[0].any_pvp;
    char    *outer   = (type == SAVEt_GENERIC_PVREF) ? ap[0].any_pv  : ap[1].any_pv;
    char    *localized = *slot;

    if (localized != outer)
        *slot = outer;                 /* revert (leave_scope guards on !=) */

    out->arg[0].any_pvp = slot;
    out->arg[1].any_pv  = localized;
    out->arg[2].any_pv  = outer;
}

static void
S_thaw_pvref(pTHX_ const SavedEntry *in)
{
    char **slot = in->arg[0].any_pvp;

    if (in->type == SAVEt_GENERIC_PVREF)
        save_pushptrptr(*slot, slot, SAVEt_GENERIC_PVREF);   /* (outer, slot) */
    else
        save_pushptrptr(slot, *slot, SAVEt_SHARED_PVREF);    /* (slot, outer) */
    *slot = in->arg[1].any_pv;
}

static void
S_discard_pvref(pTHX_ SavedEntry *entry)
{
    char *localized = entry->arg[1].any_pv;

    if (localized != entry->arg[2].any_pv) {                 /* value did change */
        if (entry->type == SAVEt_GENERIC_PVREF)
            Safefree(localized);
        else
            PerlMemShared_free(localized);
    }
}

static void
S_freeze_rcpv(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    char **slot      = ap[0].any_pvp;
    char  *localized = *slot;

    PERL_UNUSED_ARG(uv);
    *slot = ap[1].any_pv;              /* outer copy (+1) */
    (void)rcpv_free(ap[1].any_pv);     /* release the +1; slot holds outer     */

    out->arg[0].any_pvp = slot;
    out->arg[1].any_pv  = localized;   /* retained rcpv reference               */
}

static void
S_thaw_rcpv(pTHX_ const SavedEntry *in)
{
    char **slot = in->arg[0].any_pvp;

    save_pushptrptr(slot, rcpv_copy(*slot), SAVEt_RCPV);     /* (slot, +1 copy) */
    *slot = in->arg[1].any_pv;
}

static void
S_discard_rcpv(pTHX_ SavedEntry *entry)
{
    (void)rcpv_free(entry->arg[1].any_pv);                   /* retained ref */
}

/* --- SAVEt_CLEARSV / SAVEt_CLEARPADRANGE (pad-slot clear bookkeeping) ------ *
 *
 * A deferred "clear this pad slot (or range) at scope exit" action.  The whole
 * entry is the single type-tagged UV pushed by save_clearsv / the CLEARPADRANGE
 * op (the pad offset, and for a range the count, packed above SAVE_MASK); there
 * is no separate arg slot and no value to park.  freeze must NOT run the clear
 * (the lexical has to survive the suspension) and in any case the clear is
 * relative to PL_curpad, which is only the intended pad while the suspended
 * frame is live - so it can only be re-run by a real leave_scope once the
 * consumer has re-established that pad.  We therefore just carry the UV across
 * and re-push it verbatim.  Nothing is owned (the pad slot's SV belongs to the
 * pad and is freed with it), so discard and walk are NULL and cancel drops it.
 *
 * Frozen entry: arg[0].any_uv = the packed type-word.
 */

static void
S_freeze_padclear(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    PERL_UNUSED_ARG(ap);
    out->arg[0].any_uv = uv;            /* offset[/count] | SAVEt_* : re-push as-is */
}

static void
S_thaw_padclear(pTHX_ const SavedEntry *in)
{
    dSS_ADD;
    SS_ADD_UV(in->arg[0].any_uv);
    SS_ADD_END(1);
}

/* --- SAVEt_COMPPAD (PL_comppad / PL_curpad bookkeeping) -------------------- *
 *
 * A non-refcounted save of the compiling pad pointer (save_pushptr; leave_scope
 * restores it with a plain PL_comppad/PL_curpad assignment).  freeze reverts the
 * globals to the outer pad exactly as leave_scope would and keeps the localized
 * pad pointer so thaw can reinstall it; thaw re-pushes a SAVEt_COMPPAD saving the
 * outer and restores the localized globals.  The pad AV is owned by its CV, not
 * by this entry, so nothing is refcounted and discard/walk are NULL.  (Pad
 * *contents* are the consumer's responsibility - see
 * F<Porting/savestack_suspend_api.md> Sec 1.6.)
 *
 * Frozen entry: arg[0].any_ptr = outer pad, arg[1].any_ptr = localized pad.
 */

static void
S_freeze_comppad(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    PERL_UNUSED_ARG(uv);
    out->arg[0].any_ptr = ap[0].any_ptr;        /* outer (saved) pad    */
    out->arg[1].any_ptr = PL_comppad;           /* localized (current)  */

    PL_comppad = (PAD *)ap[0].any_ptr;          /* revert (leave_scope) */
    PL_curpad  = PL_comppad ? AvARRAY(PL_comppad) : NULL;
}

static void
S_thaw_comppad(pTHX_ const SavedEntry *in)
{
    PL_comppad = (PAD *)in->arg[0].any_ptr;               /* outer               */
    save_pushptr(MUTABLE_SV(PL_comppad), SAVEt_COMPPAD);  /* re-save outer       */
    PL_comppad = (PAD *)in->arg[1].any_ptr;               /* reinstall localized */
    PL_curpad  = PL_comppad ? AvARRAY(PL_comppad) : NULL;
}

/* --- SAVEt_PADSV_AND_MORTALIZE (localized pad slot, mortalize-on-restore) -- *
 *
 * save_padsv_and_mortalize pushed (SvREFCNT_inc'd outer SV, comppad, offset);
 * leave_scope mortalizes the current slot SV and restores the outer into the
 * slot.  The pad and offset in the entry locate the slot independently of the
 * live PL_curpad (unlike CLEARSV), so this is fully relocatable.  freeze mirrors
 * leave_scope but *parks* the localized SV - taking over the slot's reference -
 * instead of mortalizing it, and moves the saved outer's reference into the
 * slot.  thaw moves the slot's outer reference onto a fresh save entry and
 * reinstalls the localized SV.  On cancel the outer stays live in the pad slot
 * (owned by the pad), so discard drops only the parked localized reference.
 *
 * Frozen entry: arg[0].any_ptr = pad (PAD*), arg[1].any_uv = pad offset,
 *               arg[2].any_sv = parked localized SV (one owned reference).
 */

static void
S_freeze_padsv(pTHX_ const ANY *ap, UV uv, SavedEntry *out)
{
    SV **svp = AvARRAY((PAD *)ap[1].any_ptr) + (PADOFFSET)ap[2].any_uv;

    PERL_UNUSED_ARG(uv);
    out->arg[0].any_ptr = ap[1].any_ptr;   /* pad (borrowed; relocates the slot) */
    out->arg[1].any_uv  = ap[2].any_uv;    /* pad offset                          */
    out->arg[2].any_sv  = *svp;            /* park localized: take the slot's ref */

    *svp = ap[0].any_sv;                   /* slot := outer (save entry's ref)    */
}

static void
S_thaw_padsv(pTHX_ const SavedEntry *in)
{
    SV **svp = AvARRAY((PAD *)in->arg[0].any_ptr) + (PADOFFSET)in->arg[1].any_uv;
    dSS_ADD;

    SS_ADD_PTR(*svp);                      /* outer: move the slot's ref onto SS  */
    SS_ADD_PTR(in->arg[0].any_ptr);        /* comppad                             */
    SS_ADD_UV(in->arg[1].any_uv);          /* offset                              */
    SS_ADD_UV(SAVEt_PADSV_AND_MORTALIZE);
    SS_ADD_END(4);

    *svp = in->arg[2].any_sv;              /* reinstall localized (parked ref)    */
}

static void
S_discard_padsv(pTHX_ SavedEntry *entry)
{
    SvREFCNT_dec(entry->arg[2].any_sv);    /* parked localized; outer stays in pad */
}

static void
S_walk_padsv(pTHX_ const SavedEntry *in, PerlSavestackFrozenSVCb cb, void *ud)
{
    cb(aTHX_ in->arg[2].any_sv, "SAVEt_PADSV_AND_MORTALIZE localized value", ud);
}

/* Cover the whole SAVE_MASK range so a corrupt/out-of-range type still indexes
 * a defined (non-relocatable) row rather than reading past the array. */
#define SAVETYPE_RELOC_MAX (SAVE_MASK + 1)      /* 64 */

/* Rows not listed default to { relocatable = FALSE, NULL... } => freeze croaks.
 * Later increments add rows as their handlers land. */
static const savetype_reloc S_savetype_reloc[SAVETYPE_RELOC_MAX] = {
    [SAVEt_SV]    = { TRUE, S_can_freeze_sv, S_freeze_sv, S_thaw_sv,
                      S_discard_sv, S_walk_sv },
    [SAVEt_SVREF] = { TRUE, S_can_freeze_sv, S_freeze_sv, S_thaw_sv,
                      S_discard_sv, S_walk_sv },
    [SAVEt_AV]    = { TRUE, S_can_freeze_avhv, S_freeze_avhv, S_thaw_avhv,
                      S_discard_avhv, S_walk_avhv },
    [SAVEt_HV]    = { TRUE, S_can_freeze_avhv, S_freeze_avhv, S_thaw_avhv,
                      S_discard_avhv, S_walk_avhv },
    [SAVEt_HELEM] = { TRUE, S_can_freeze_elem, S_freeze_elem, S_thaw_elem,
                      S_discard_elem, S_walk_elem },
    [SAVEt_AELEM] = { TRUE, S_can_freeze_elem, S_freeze_elem, S_thaw_elem,
                      S_discard_elem, S_walk_elem },
    [SAVEt_DELETE]  = { TRUE, S_can_freeze_delete, S_freeze_delete, S_thaw_delete,
                        S_discard_delete, S_walk_delete },
    [SAVEt_ADELETE] = { TRUE, S_can_freeze_delete, S_freeze_delete, S_thaw_delete,
                        S_discard_delete, S_walk_delete },
    [SAVEt_GVSV]         = { TRUE, NULL, S_freeze_svp, S_thaw_svp,
                             S_discard_svp, S_walk_svp },
    [SAVEt_GENERIC_SVREF] = { TRUE, NULL, S_freeze_svp, S_thaw_svp,
                             S_discard_svp, S_walk_svp },
    [SAVEt_INT]         = { TRUE, NULL, S_freeze_cint, S_thaw_cint, NULL, NULL },
    [SAVEt_INT_SMALL]   = { TRUE, NULL, S_freeze_cint, S_thaw_cint, NULL, NULL },
    [SAVEt_IV]          = { TRUE, NULL, S_freeze_cint, S_thaw_cint, NULL, NULL },
    [SAVEt_I32]         = { TRUE, NULL, S_freeze_cint, S_thaw_cint, NULL, NULL },
    [SAVEt_I32_SMALL]   = { TRUE, NULL, S_freeze_cint, S_thaw_cint, NULL, NULL },
    [SAVEt_I16]         = { TRUE, NULL, S_freeze_cint, S_thaw_cint, NULL, NULL },
    [SAVEt_I8]          = { TRUE, NULL, S_freeze_cint, S_thaw_cint, NULL, NULL },
    [SAVEt_BOOL]        = { TRUE, NULL, S_freeze_cint, S_thaw_cint, NULL, NULL },
    [SAVEt_STRLEN]      = { TRUE, NULL, S_freeze_cint, S_thaw_cint, NULL, NULL },
    [SAVEt_STRLEN_SMALL]= { TRUE, NULL, S_freeze_cint, S_thaw_cint, NULL, NULL },
    [SAVEt_SPTR]        = { TRUE, NULL, S_freeze_ptr, S_thaw_ptr, NULL, NULL },
    [SAVEt_VPTR]        = { TRUE, NULL, S_freeze_ptr, S_thaw_ptr, NULL, NULL },
    [SAVEt_PPTR]        = { TRUE, NULL, S_freeze_ptr, S_thaw_ptr, NULL, NULL },
    [SAVEt_HPTR]        = { TRUE, NULL, S_freeze_ptr, S_thaw_ptr, NULL, NULL },
    [SAVEt_APTR]        = { TRUE, NULL, S_freeze_ptr, S_thaw_ptr, NULL, NULL },
    [SAVEt_ITEM]        = { TRUE, S_can_freeze_item, S_freeze_item, S_thaw_item,
                            S_discard_item, S_walk_item },
    [SAVEt_FREESV]      = { TRUE, NULL, S_freeze_action, S_thaw_action1,
                            S_discard_action1, S_walk_action1 },
    [SAVEt_MORTALIZESV] = { TRUE, NULL, S_freeze_action, S_thaw_action1,
                            S_discard_action1, S_walk_action1 },
    [SAVEt_FREEPV]      = { TRUE, NULL, S_freeze_action, S_thaw_action1,
                            S_discard_action1, S_walk_action1 },
    [SAVEt_FREEOP]      = { TRUE, NULL, S_freeze_action, S_thaw_action1,
                            S_discard_action1, S_walk_action1 },
    [SAVEt_FREEPADNAME] = { TRUE, NULL, S_freeze_action, S_thaw_action1,
                            S_discard_action1, S_walk_action1 },
    [SAVEt_FREECOPHH]   = { TRUE, NULL, S_freeze_action, S_thaw_action1,
                            S_discard_action1, S_walk_action1 },
    [SAVEt_FREERCPV]    = { TRUE, NULL, S_freeze_action, S_thaw_action1,
                            S_discard_action1, S_walk_action1 },
    [SAVEt_READONLY_OFF]= { TRUE, NULL, S_freeze_action, S_thaw_action1,
                            S_discard_action1, S_walk_action1 },
    [SAVEt_DESTRUCTOR]  = { TRUE, NULL, S_freeze_action, S_thaw_destructor,
                            NULL, NULL },   /* run via savestack_frozen_run_deferred */
    [SAVEt_DESTRUCTOR_X]= { TRUE, NULL, S_freeze_action, S_thaw_destructor,
                            NULL, NULL },   /* run via savestack_frozen_run_deferred */
    [SAVEt_SET_SVFLAGS] = { TRUE, NULL, S_freeze_action, S_thaw_setflags,
                            S_discard_setflags, NULL },
    [SAVEt_GENERIC_PVREF]={ TRUE, NULL, S_freeze_pvref, S_thaw_pvref,
                            S_discard_pvref, NULL },
    [SAVEt_SHARED_PVREF] = { TRUE, NULL, S_freeze_pvref, S_thaw_pvref,
                            S_discard_pvref, NULL },
    [SAVEt_RCPV]        = { TRUE, NULL, S_freeze_rcpv, S_thaw_rcpv,
                            S_discard_rcpv, NULL },
    [SAVEt_CLEARSV]      = { TRUE, NULL, S_freeze_padclear, S_thaw_padclear,
                             NULL, NULL },
    [SAVEt_CLEARPADRANGE]= { TRUE, NULL, S_freeze_padclear, S_thaw_padclear,
                             NULL, NULL },
    [SAVEt_COMPPAD]      = { TRUE, NULL, S_freeze_comppad, S_thaw_comppad,
                             NULL, NULL },
    [SAVEt_PADSV_AND_MORTALIZE] = { TRUE, NULL, S_freeze_padsv, S_thaw_padsv,
                             S_discard_padsv, S_walk_padsv },
};

PERL_STATIC_INLINE const savetype_reloc *
S_savetype_reloc_for(U8 type)
{
    assert(type < SAVETYPE_RELOC_MAX);
    return &S_savetype_reloc[type];
}

#define SAVESTACK_FROZEN_MIN 8

static void
S_frozen_grow(pTHX_ PerlSavestackFrozen *frozen)
{
    const U32 newmax = frozen->max ? (frozen->max << 1) : SAVESTACK_FROZEN_MIN;
    Renew(frozen->entries, newmax, SavedEntry);
    frozen->max = newmax;
}

/*
=for apidoc_section $callback
=for apidoc savestack_freeze

Freeze the region of the save stack from C<base_ix> up to the current
C<PL_savestack_ix>: serialize each entry into a newly allocated opaque
C<PerlSavestackFrozen> blob and unwind it off the live save stack, reverting
each localized target to its outer value.  Afterwards C<PL_savestack_ix> equals
C<base_ix>.  The returned blob may later be re-applied with
L</C<savestack_thaw>> or discarded with L</C<savestack_frozen_free>>.

This lets a suspend-a-scope consumer such as Future::AsyncAwait or Coro lift a
region of dynamic scope (the effects of C<local> and friends) off the
interpreter across a suspension point and re-apply it later, instead of
re-implementing C<leave_scope> in reverse.  Freezing an empty region succeeds
and returns an empty blob.

Croaks, leaving the save stack unchanged, if the region contains a save type
that cannot be suspended (for example C<local> on a tied or magical target,
which is not yet supported).  See F<Porting/savestack_suspend_api.md>.

B<Note:> this API is new and considered experimental.

=cut
*/

PerlSavestackFrozen *
Perl_savestack_freeze(pTHX_ I32 base_ix)
{
    PerlSavestackFrozen *frozen;
    I32 ix;

    if (UNLIKELY(base_ix < 0 || base_ix > PL_savestack_ix))
        croak("panic: savestack_freeze: bad base index %ld (ix=%ld)",
              (long)base_ix, (long)PL_savestack_ix);

    /* Pass 1: verify the whole region can be suspended, mutating nothing, so a
     * refusal leaves the save stack intact (never corrupt - Sec 1.5).  The walk
     * uses leave_scope_arg_counts[]; a non-relocatable type is refused the
     * moment it is reached, before its (possibly variable) width is trusted. */
    ix = PL_savestack_ix;
    while (ix > base_ix) {
        const UV  uv    = PL_savestack[ix - 1].any_uv;
        const U8  type  = (U8)uv & SAVE_MASK;
        const U8  nargs = leave_scope_arg_counts[type];
        const savetype_reloc *r = S_savetype_reloc_for(type);

        if (!r->relocatable)
            croak("savestack_freeze: cannot suspend save type %d "
                  "(not relocatable); see Porting/savestack_suspend_api.md",
                  (int)type);
        if (UNLIKELY(ix - 1 - (I32)nargs < base_ix))
            croak("panic: savestack_freeze: save type %d straddles base %ld",
                  (int)type, (long)base_ix);
        if (r->can_freeze
            && !r->can_freeze(aTHX_ &PL_savestack[ix - 1 - nargs], uv))
            croak("savestack_freeze: cannot suspend this instance of save "
                  "type %d; see Porting/savestack_suspend_api.md", (int)type);

        ix -= (I32)nargs + 1;
    }
    if (UNLIKELY(ix != base_ix))
        croak("panic: savestack_freeze: region does not end on a save "
              "boundary (ix=%ld base=%ld)", (long)ix, (long)base_ix);

    /* Pass 2: park + capture, unwinding to base_ix.  Every entry passed pass 1,
     * so no handler here croaks and the blob is always fully built. */
    Newxz(frozen, 1, PerlSavestackFrozen);
    while (PL_savestack_ix > base_ix) {
        const I32 top   = PL_savestack_ix - 1;
        const UV  uv    = PL_savestack[top].any_uv;
        const U8  type  = (U8)uv & SAVE_MASK;
        const U8  nargs = leave_scope_arg_counts[type];
        const savetype_reloc *r = S_savetype_reloc_for(type);
        SavedEntry *out;

        assert(r->relocatable && r->freeze);
        if (frozen->len == frozen->max)
            S_frozen_grow(aTHX_ frozen);
        out = &frozen->entries[frozen->len];
        out->type  = type;
        out->nargs = nargs;
        r->freeze(aTHX_ &PL_savestack[top - nargs], uv, out);
        frozen->len++;
        PL_savestack_ix = top - nargs;
    }

    return frozen;
}

/*
=for apidoc savestack_thaw

Re-apply a blob previously produced by L</C<savestack_freeze>> onto the live
save stack, re-pushing an equivalent save entry for each frozen entry (so a
later ordinary C<leave_scope> restores and frees correctly) and re-installing
each localized value.  The blob is consumed (freed).  The caller is responsible
for C<PL_savestack_ix> being at the base the entries should be re-applied from.
A NULL blob is a no-op.

B<Note:> this API is new and considered experimental.

=cut
*/

void
Perl_savestack_thaw(pTHX_ PerlSavestackFrozen *frozen)
{
    U32 i;

    if (!frozen)
        return;

    /* Replay in reverse capture order: entry[len-1] sat closest to the base and
     * was pushed first originally, so it must be re-pushed first. */
    i = frozen->len;
    while (i-- > 0) {
        const SavedEntry *e = &frozen->entries[i];
        const savetype_reloc *r = S_savetype_reloc_for(e->type);
        assert(r->thaw);
        r->thaw(aTHX_ e);
    }

    /* Every retained ref has been transferred back onto the live save stack /
     * into its slot, so free the blob shell WITHOUT running discard. */
    Safefree(frozen->entries);
    Safefree(frozen);
}

/*
=for apidoc savestack_frozen_free

Discard a blob from L</C<savestack_freeze>> that will never be thawed (for
example a cancelled suspension), dropping any references it owns and freeing any
owned resources.  The parked outer values are already live in their slots and
are left untouched.  Deferred B<user callbacks> registered with C<SAVEDESTRUCTOR>
/ C<SAVEDESTRUCTOR_X> (e.g. C<defer> and C<try>/C<finally> blocks) are B<not> run
here; a consumer that wants them to fire on cancellation must call
L</C<savestack_frozen_run_deferred>> first.  A NULL blob is a no-op.

B<Note:> this API is new and considered experimental.

=cut
*/

void
Perl_savestack_frozen_free(pTHX_ PerlSavestackFrozen *frozen)
{
    U32 i;

    if (!frozen)
        return;

    for (i = 0; i < frozen->len; i++) {
        SavedEntry *e = &frozen->entries[i];
        const savetype_reloc *r = S_savetype_reloc_for(e->type);
        if (r->discard)
            r->discard(aTHX_ e);
    }
    Safefree(frozen->entries);
    Safefree(frozen);
}

/*
=for apidoc savestack_frozen_run_deferred

Run the deferred B<user callbacks> parked in a frozen blob - those registered
with C<SAVEDESTRUCTOR> / C<SAVEDESTRUCTOR_X> (e.g. C<defer> and C<try>/C<finally>
blocks) - in the order a real C<leave_scope> would (most-recently-entered
first).  Intended for a consumer that is cancelling a suspended scope and wants
those finalizers to fire as they would on an ordinary scope exit.  This does
B<not> free the blob or drop any references - call L</C<savestack_frozen_free>>
afterwards to reclaim it.  Calling it more than once runs the callbacks more
than once.  A NULL blob is a no-op.

B<Note:> this API is new and considered experimental.

=cut
*/

void
Perl_savestack_frozen_run_deferred(pTHX_ PerlSavestackFrozen *frozen)
{
    U32 i;

    if (!frozen)
        return;

    /* entries[0] is the top-most (last-entered) scope, so forward iteration
     * fires finalizers most-recently-entered first, matching leave_scope. */
    for (i = 0; i < frozen->len; i++) {
        SavedEntry *e = &frozen->entries[i];
        if (e->type == SAVEt_DESTRUCTOR || e->type == SAVEt_DESTRUCTOR_X)
            S_run_destructor(aTHX_ e);
    }
}

/*
=for apidoc savestack_frozen_foreach_sv

Enumerate every SV a frozen blob retains, invoking C<cb> once per SV as
S<C<cb(aTHX_ sv, desc, ud)>>, where C<desc> is a static human-readable label
and C<ud> is the opaque data passed through unchanged.  Intended for
Devel::MAT-style tooling that needs to see into a suspended scope.  The SVs are
borrowed (not reference-counted for the callback); C<cb> must not free them.  A
NULL blob is a no-op.

B<Note:> this API is new and considered experimental.

=cut
*/

void
Perl_savestack_frozen_foreach_sv(pTHX_ PerlSavestackFrozen *frozen,
                                 PerlSavestackFrozenSVCb cb, void *ud)
{
    U32 i;

    PERL_ARGS_ASSERT_SAVESTACK_FROZEN_FOREACH_SV;

    if (!frozen)
        return;

    for (i = 0; i < frozen->len; i++) {
        const SavedEntry *e = &frozen->entries[i];
        const savetype_reloc *r = S_savetype_reloc_for(e->type);
        if (r->walk)
            r->walk(aTHX_ e, cb, ud);
    }
}

void
Perl_cx_dump(pTHX_ PERL_CONTEXT *cx)
{
    PERL_ARGS_ASSERT_CX_DUMP;

#ifdef DEBUGGING
    PerlIO_printf(Perl_debug_log, "CX %ld = %s\n", (long)(cx - cxstack), PL_block_type[CxTYPE(cx)]);
    if (CxTYPE(cx) != CXt_SUBST) {
        const char *gimme_text;
        PerlIO_printf(Perl_debug_log, "BLK_OLDSP = %ld\n", (long)cx->blk_oldsp);
        PerlIO_printf(Perl_debug_log, "BLK_OLDCOP = 0x%" UVxf "\n",
                      PTR2UV(cx->blk_oldcop));
        PerlIO_printf(Perl_debug_log, "BLK_OLDMARKSP = %ld\n", (long)cx->blk_oldmarksp);
        PerlIO_printf(Perl_debug_log, "BLK_OLDSCOPESP = %ld\n", (long)cx->blk_oldscopesp);
        PerlIO_printf(Perl_debug_log, "BLK_OLDSAVEIX = %ld\n", (long)cx->blk_oldsaveix);
        PerlIO_printf(Perl_debug_log, "BLK_OLDPM = 0x%" UVxf "\n",
                      PTR2UV(cx->blk_oldpm));
        switch (cx->blk_gimme) {
            case G_VOID:
                gimme_text = "VOID";
                break;
            case G_SCALAR:
                gimme_text = "SCALAR";
                break;
            case G_LIST:
                gimme_text = "LIST";
                break;
            default:
                gimme_text = "UNKNOWN";
                break;
        }
        PerlIO_printf(Perl_debug_log, "BLK_GIMME = %s\n", gimme_text);
    }
    switch (CxTYPE(cx)) {
    case CXt_NULL:
    case CXt_BLOCK:
    case CXt_DEFER:
        break;
    case CXt_FORMAT:
        PerlIO_printf(Perl_debug_log, "BLK_FORMAT.CV = 0x%" UVxf "\n",
                PTR2UV(cx->blk_format.cv));
        PerlIO_printf(Perl_debug_log, "BLK_FORMAT.GV = 0x%" UVxf "\n",
                PTR2UV(cx->blk_format.gv));
        PerlIO_printf(Perl_debug_log, "BLK_FORMAT.DFOUTGV = 0x%" UVxf "\n",
                PTR2UV(cx->blk_format.dfoutgv));
        PerlIO_printf(Perl_debug_log, "BLK_FORMAT.HASARGS = %d\n",
                      (int)CxHASARGS(cx));
        PerlIO_printf(Perl_debug_log, "BLK_FORMAT.RETOP = 0x%" UVxf "\n",
                PTR2UV(cx->blk_format.retop));
        break;
    case CXt_SUB:
        PerlIO_printf(Perl_debug_log, "BLK_SUB.CV = 0x%" UVxf "\n",
                PTR2UV(cx->blk_sub.cv));
        PerlIO_printf(Perl_debug_log, "BLK_SUB.OLDDEPTH = %ld\n",
                (long)cx->blk_sub.olddepth);
        PerlIO_printf(Perl_debug_log, "BLK_SUB.HASARGS = %d\n",
                (int)CxHASARGS(cx));
        PerlIO_printf(Perl_debug_log, "BLK_SUB.LVAL = %d\n", (int)CxLVAL(cx));
        PerlIO_printf(Perl_debug_log, "BLK_SUB.RETOP = 0x%" UVxf "\n",
                PTR2UV(cx->blk_sub.retop));
        break;
    case CXt_EVAL:
        PerlIO_printf(Perl_debug_log, "BLK_EVAL.OLD_IN_EVAL = %ld\n",
                (long)CxOLD_IN_EVAL(cx));
        PerlIO_printf(Perl_debug_log, "BLK_EVAL.OLD_OP_TYPE = %s (%s)\n",
                PL_op_name[CxOLD_OP_TYPE(cx)],
                PL_op_desc[CxOLD_OP_TYPE(cx)]);
        if (cx->blk_eval.old_namesv)
            PerlIO_printf(Perl_debug_log, "BLK_EVAL.OLD_NAME = %s\n",
                          SvPVX_const(cx->blk_eval.old_namesv));
        PerlIO_printf(Perl_debug_log, "BLK_EVAL.OLD_EVAL_ROOT = 0x%" UVxf "\n",
                PTR2UV(cx->blk_eval.old_eval_root));
        PerlIO_printf(Perl_debug_log, "BLK_EVAL.RETOP = 0x%" UVxf "\n",
                PTR2UV(cx->blk_eval.retop));
        break;

    case CXt_LOOP_PLAIN:
    case CXt_LOOP_LAZYIV:
    case CXt_LOOP_LAZYSV:
    case CXt_LOOP_LIST:
    case CXt_LOOP_ARY:
        PerlIO_printf(Perl_debug_log, "BLK_LOOP.LABEL = %s\n", CxLABEL(cx));
        PerlIO_printf(Perl_debug_log, "BLK_LOOP.MY_OP = 0x%" UVxf "\n",
                PTR2UV(cx->blk_loop.my_op));
        if (CxTYPE(cx) != CXt_LOOP_PLAIN) {
            PerlIO_printf(Perl_debug_log, "BLK_LOOP.ITERVAR = 0x%" UVxf "\n",
                    PTR2UV(CxITERVAR(cx)));
            PerlIO_printf(Perl_debug_log, "BLK_LOOP.ITERSAVE = 0x%" UVxf "\n",
                    PTR2UV(cx->blk_loop.itersave));
        }
        if (CxTYPE(cx) == CXt_LOOP_ARY) {
            PerlIO_printf(Perl_debug_log, "BLK_LOOP.ITERARY = 0x%" UVxf "\n",
                    PTR2UV(cx->blk_loop.state_u.ary.ary));
            PerlIO_printf(Perl_debug_log, "BLK_LOOP.ITERIX = %ld\n",
                    (long)cx->blk_loop.state_u.ary.ix);
        }
        break;

    case CXt_SUBST:
        PerlIO_printf(Perl_debug_log, "SB_ITERS = %ld\n",
                (long)cx->sb_iters);
        PerlIO_printf(Perl_debug_log, "SB_MAXITERS = %ld\n",
                (long)cx->sb_maxiters);
        PerlIO_printf(Perl_debug_log, "SB_RFLAGS = %ld\n",
                (long)cx->sb_rflags);
        PerlIO_printf(Perl_debug_log, "SB_ONCE = %ld\n",
                (long)CxONCE(cx));
        PerlIO_printf(Perl_debug_log, "SB_ORIG = %s\n",
                cx->sb_orig);
        PerlIO_printf(Perl_debug_log, "SB_DSTR = 0x%" UVxf "\n",
                PTR2UV(cx->sb_dstr));
        PerlIO_printf(Perl_debug_log, "SB_TARG = 0x%" UVxf "\n",
                PTR2UV(cx->sb_targ));
        PerlIO_printf(Perl_debug_log, "SB_S = 0x%" UVxf "\n",
                PTR2UV(cx->sb_s));
        PerlIO_printf(Perl_debug_log, "SB_M = 0x%" UVxf "\n",
                PTR2UV(cx->sb_m));
        PerlIO_printf(Perl_debug_log, "SB_STREND = 0x%" UVxf "\n",
                PTR2UV(cx->sb_strend));
        PerlIO_printf(Perl_debug_log, "SB_RXRES = 0x%" UVxf "\n",
                PTR2UV(cx->sb_rxres));
        break;
    }
#else
    PERL_UNUSED_CONTEXT;
    PERL_UNUSED_ARG(cx);
#endif	/* DEBUGGING */
}

/*
=for apidoc_section $callback
=for apidoc mortal_destructor_sv

This function arranges for either a Perl code reference, or a C function
reference to be called at the B<end of the current statement>.

The C<coderef> argument determines the type of function that will be
called. If it is C<SvROK()> it is assumed to be a reference to a CV and
will arrange for the coderef to be called. If it is not SvROK() then it
is assumed to be a C<SvIV()> which is C<SvIOK()> whose value is a pointer
to a C function of type C<DESTRUCTORFUNC_t> created using C<PTR2INT()>.
Either way the C<args> parameter will be provided to the callback as a
parameter, although the rules for doing so differ between the Perl and
C mode. Normally this function is only used directly for the Perl case
and the wrapper C<mortal_destructor_x()> is used for the C function case.

When operating in Perl callback mode the C<args> parameter may be NULL
in which case the code reference is called with no arguments, otherwise
if it is an AV (SvTYPE(args) == SVt_PVAV) then the contents of the AV
will be used as the arguments to the code reference, and if it is any
other type then the C<args> SV will be provided as a single argument to
the code reference.

When operating in a C callback mode the C<args> parameter will be passed
directly to the C function as a C<void *> pointer. No additional
processing of the argument will be performed, and it is the callers
responsibility to free the C<args> parameter if necessary.

Be aware that there is a significant difference in timing between the
I<end of the current statement> and the I<end of the current pseudo
block>. If you are looking for a mechanism to trigger a function at the
end of the B<current pseudo block> you should look at
L<perlapi/C<SAVEDESTRUCTOR_X>> instead of this function.

=for apidoc mortal_svfunc_x

This function arranges for a C function reference to be called at the
B<end of the current statement> with the arguments provided. It is a
wrapper around C<mortal_destructor_sv()> which ensures that the latter
function is called appropriately.

Be aware that there is a significant difference in timing between the
I<end of the current statement> and the I<end of the current pseudo
block>. If you are looking for a mechanism to trigger a function at the
end of the B<current pseudo block> you should look at
L<perlapi/C<SAVEDESTRUCTOR_X>> instead of this function.

=for apidoc magic_freedestruct

This function is called via magic to implement the
C<mortal_destructor_sv()> and C<mortal_destructor_x()> functions. It
should not be called directly and has no user serviceable parts.

=cut
*/

void
Perl_mortal_destructor_sv(pTHX_ SV *coderef, SV *args) {
    PERL_ARGS_ASSERT_MORTAL_DESTRUCTOR_SV;
    assert(
        (SvROK(coderef) && SvTYPE(SvRV(coderef)) == SVt_PVCV) /* perl coderef */
         ||
        (SvIOK(coderef) && !SvROK(coderef))                  /* C function ref */
    );
    SV *variable = newSV_type_mortal(SVt_IV);
    (void)sv_magicext(variable, coderef, PERL_MAGIC_destruct,
                      &PL_vtbl_destruct, (char *)args, args ? HEf_SVKEY : 0);
}


void
Perl_mortal_svfunc_x(pTHX_ SVFUNC_t f, SV *sv) {
    PERL_ARGS_ASSERT_MORTAL_SVFUNC_X;
    SV *sviv = newSViv(PTR2IV(f));
    mortal_destructor_sv(sviv,sv);
}


int
Perl_magic_freedestruct(pTHX_ SV* sv, MAGIC* mg) {
    PERL_ARGS_ASSERT_MAGIC_FREEDESTRUCT;
    dSP;
    union {
        SV   *sv;
        AV   *av;
        char *pv;
    } args_any;
    SV *coderef;

    IV nargs = 0;
    if (PL_phase == PERL_PHASE_DESTRUCT) {
        warn("Can't call destructor for 0x%p in global destruction\n", sv);
        return 1;
    }

    args_any.pv = mg->mg_ptr;
    coderef = mg->mg_obj;

    /* Deal with C function destructor */
    if (SvTYPE(coderef) == SVt_IV && !SvROK(coderef)) {
        SVFUNC_t f = INT2PTR(SVFUNC_t, SvIV(coderef));
        (f)(aTHX_ args_any.sv);
        return 0;
    }

    if (args_any.sv) {
        if (SvTYPE(args_any.sv) == SVt_PVAV) {
            nargs = av_len(args_any.av) + 1;
        } else {
            nargs = 1;
        }
    }
    PUSHSTACKi(PERLSI_MAGIC);
    ENTER_with_name("call_freedestruct");
    SAVETMPS;
    EXTEND(SP, nargs);
    PUSHMARK(SP);
    if (args_any.sv) {
        if (SvTYPE(args_any.sv) == SVt_PVAV) {
            IV n;
            for (n = 0 ; n < nargs ; n++ ) {
                SV **argp = av_fetch(args_any.av, n, 0);
                if (argp && *argp)
                    PUSHs(*argp);
            }
        } else {
            PUSHs(args_any.sv);
        }
    }
    PUTBACK;
    (void)call_sv(coderef, G_VOID | G_EVAL | G_KEEPERR);
    FREETMPS;
    LEAVE_with_name("call_freedestruct");
    POPSTACK;
    return 0;
}


/*
 * ex: set ts=8 sts=4 sw=4 et:
 */

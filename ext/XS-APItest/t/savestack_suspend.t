#!perl -w
# Tests for the savestack suspend/resume API ("parkapi"):
#   Perl_savestack_freeze / _thaw / _frozen_free / _frozen_foreach_sv
# See Porting/savestack_suspend_api.md.
#
# The round-trip is driven from a self-contained XSUB
# (XS::APItest::savestack::test_suspend_scalar) so the save-stack contents
# between the base and the freeze are known to be exactly one SAVEt_SV.

use strict;
use warnings;
use Test::More;

use XS::APItest qw(sv_count);

# ($during, $after_freeze, $freeze_delta, $after_op, $after_leave, $nsv)
sub run { XS::APItest::savestack::test_suspend_scalar(@_) }

# op/svleak.t-style leak helper: run $code $n times; the live SV count must not
# grow by more than ($n-1)*$delta.  delta 0 => not a single SV may leak.
sub leak {
    my ($n, $delta, $code, $name) = @_;
    my ($sv0, $sv1) = (0, 0);
    for my $i (1 .. $n) {
        $code->();
        $sv1 = sv_count();
        $sv0 = $sv1 if $i == 1;
    }
    cmp_ok($sv1 - $sv0, '<=', ($n - 1) * $delta, $name);
}

# --- freeze -> thaw : full round-trip ------------------------------------
{
    my ($during, $after_freeze, $delta, $after_thaw, $after_leave, $nsv)
        = run("OUTER", "INNER", 0);

    is $during,      "INNER", "during scope: probe holds the localized value";
    is $after_freeze,"OUTER", "after freeze: probe parked back to outer value";
    is $delta,       0,       "after freeze: save stack unwound to base";
    is $after_thaw,  "INNER", "after thaw: localized value re-installed";
    is $after_leave, "OUTER", "after leave_scope: outer value restored";
    is $nsv,         2,       "frozen_foreach_sv saw 2 SVs (GV + localized)";
}

# --- freeze -> frozen_free : cancellation --------------------------------
{
    my ($during, $after_freeze, $delta, $after_free, $after_leave, $nsv)
        = run("OUTER", "INNER", 1);

    is $during,      "INNER", "cancel: during scope holds localized value";
    is $after_freeze,"OUTER", "cancel: after freeze parked to outer";
    is $delta,       0,       "cancel: save stack unwound to base";
    is $after_free,  "OUTER", "cancel: after frozen_free outer still in place";
    is $after_leave, "OUTER", "cancel: after leave_scope outer intact";
    is $nsv,         2,       "cancel: frozen_foreach_sv saw 2 SVs";
}

# --- stability: repeated round-trips do not corrupt the probe ------------
{
    my $ok = 1;
    for my $i (1 .. 1000) {
        my (undef, $af, $d, $at, $al) = run("out$i", "in$i", $i % 2);
        $ok &&= ($af eq "out$i" && $d == 0 && $al eq "out$i"
                 && ($i % 2 ? 1 : $at eq "in$i"));
    }
    ok $ok, "1000 freeze/thaw and freeze/cancel cycles stay consistent";
}

# --- local @array (SAVEt_AV) and local %hash (SAVEt_HV) ------------------
# save_ary/save_hash use the opposite refcount convention to save_scalar
# (the outer container is not incremented), so these exercise a distinct code
# path in the freeze/thaw handlers.
for my $case (
    [ "array", \&XS::APItest::savestack::test_suspend_av, ["OUTER"], ["INNER"] ],
    [ "hash",  \&XS::APItest::savestack::test_suspend_hv,
               { k => "OUTER" }, { k => "INNER" } ],
) {
    my ($what, $fn, $outer, $inner) = @$case;

    my ($during, $af, $d, $at, $al, $nsv) = $fn->($outer, $inner, 0);
    is $during, "INNER", "$what during: localized container in place";
    is $af,     "OUTER", "$what after freeze: parked to outer container";
    is $d,      0,       "$what after freeze: save stack unwound";
    is $at,     "INNER", "$what after thaw: localized container re-installed";
    is $al,     "OUTER", "$what after leave: outer container restored";
    is $nsv,    2,       "$what foreach_sv: 2 SVs (GV + container)";

    my (undef, $caf, $cd, $cfree, $cal) = $fn->($outer, $inner, 1);
    is $caf,   "OUTER", "$what cancel: parked to outer";
    is $cfree, "OUTER", "$what cancel: outer intact after frozen_free";
    is $cal,   "OUTER", "$what cancel: outer intact after leave";

    leak 50, 0, sub { my @r = $fn->($outer, $inner, 0) },
        "$what freeze -> thaw leaks no SVs";
    leak 50, 0, sub { my @r = $fn->($outer, $inner, 1) },
        "$what freeze -> frozen_free leaks no SVs";
}

# --- local $h{k} (SAVEt_HELEM) and local $a[i] (SAVEt_AELEM) -------------
# The element address is not stable across a suspension, so freeze and thaw
# both re-locate the element from (container, key/idx).
for my $case (
    [ "helem", \&XS::APItest::savestack::test_suspend_helem, 3 ],
    [ "aelem", \&XS::APItest::savestack::test_suspend_aelem, 2 ],
) {
    my ($what, $fn, $want_nsv) = @$case;

    my ($during, $af, $d, $at, $al, $nsv) = $fn->("OUTER", "INNER", 0);
    is $during, "INNER", "$what during: localized element in place";
    is $af,     "OUTER", "$what after freeze: element parked to outer";
    is $d,      0,       "$what after freeze: save stack unwound";
    is $at,     "INNER", "$what after thaw: localized element re-installed";
    is $al,     "OUTER", "$what after leave: outer element restored";
    is $nsv,    $want_nsv, "$what foreach_sv: $want_nsv SVs";

    my (undef, $caf, $cd, $cfree, $cal) = $fn->("OUTER", "INNER", 1);
    is $caf,   "OUTER", "$what cancel: parked to outer";
    is $cfree, "OUTER", "$what cancel: outer intact after frozen_free";
    is $cal,   "OUTER", "$what cancel: outer intact after leave";

    leak 50, 0, sub { my @r = $fn->("OUTER", "INNER", 0) },
        "$what freeze -> thaw leaks no SVs";
    leak 50, 0, sub { my @r = $fn->("OUTER", "INNER", 1) },
        "$what freeze -> frozen_free leaks no SVs";
}

# --- local $h{new_key} / $a[new_idx] (SAVEt_DELETE / SAVEt_ADELETE) ------
# The key/index did not exist before, so the outer state is ABSENT: freeze's
# park deletes the element, and a real leave_scope deletes it too.
for my $case (
    [ "hdelete", \&XS::APItest::savestack::test_suspend_hdelete, 3 ],
    [ "adelete", \&XS::APItest::savestack::test_suspend_adelete, 2 ],
) {
    my ($what, $fn, $want_nsv) = @$case;

    my ($dval, $af_ex, $d, $at_val, $al_ex, $nsv) = $fn->("INNER", 0);
    is $dval,  "INNER", "$what during: localized element present";
    is $af_ex, 0,       "$what after freeze: element parked to absent";
    is $d,     0,       "$what after freeze: save stack unwound";
    is $at_val,"INNER", "$what after thaw: element re-created with value";
    is $al_ex, 0,       "$what after leave: element deleted (outer absent)";
    is $nsv,   $want_nsv, "$what foreach_sv: $want_nsv SVs";

    my ($cdval, $caf_ex, $cd, $cop_val, $cal_ex) = $fn->("INNER", 1);
    is $caf_ex, 0,     "$what cancel: parked to absent";
    is $cop_val, undef,"$what cancel: element still absent after frozen_free";
    is $cal_ex, 0,     "$what cancel: element absent after leave";

    leak 50, 0, sub { my @r = $fn->("INNER", 0) },
        "$what freeze -> thaw leaks no SVs";
    leak 50, 0, sub { my @r = $fn->("INNER", 1) },
        "$what freeze -> frozen_free leaks no SVs";
}

# --- SAVEt_GVSV / SAVEt_GENERIC_SVREF (restore_svp scalar cousins) -------
# Both localize a GV scalar slot; magic-safe (restore_svp does no set-magic).
for my $case (
    [ "gvsv",    0 ],
    [ "generic", 1 ],
) {
    my ($what, $is_generic) = @$case;
    my $fn = sub { XS::APItest::savestack::test_suspend_svp($is_generic, @_) };

    my ($during, $af, $d, $at, $al, $nsv) = $fn->("OUTER", "INNER", 0);
    is $during, "INNER", "$what during: localized value in slot";
    is $af,     "OUTER", "$what after freeze: parked to outer";
    is $d,      0,       "$what after freeze: save stack unwound";
    is $at,     "INNER", "$what after thaw: localized value re-installed";
    is $al,     "OUTER", "$what after leave: outer restored";
    is $nsv,    1,       "$what foreach_sv: 1 SV (localized value)";

    my (undef, $caf, $cd, $cfree, $cal) = $fn->("OUTER", "INNER", 1);
    is $caf,   "OUTER", "$what cancel: parked to outer";
    is $cfree, "OUTER", "$what cancel: outer intact after frozen_free";
    is $cal,   "OUTER", "$what cancel: outer intact after leave";

    leak 50, 0, sub { my @r = $fn->("OUTER", "INNER", 0) },
        "$what freeze -> thaw leaks no SVs";
    leak 50, 0, sub { my @r = $fn->("OUTER", "INNER", 1) },
        "$what freeze -> frozen_free leaks no SVs";
}

# --- localized C scalars: INT/IV/I32/I16/I8/BOOL/STRLEN ------------------
# No SVs / refcounts; verify the C value round-trips through freeze/thaw.
for my $case (
    [ "int",    0, 11, 22 ], [ "iv",  1, 11, 22 ], [ "i32", 2, 11, 22 ],
    [ "i16",    3, 11, 22 ], [ "i8",  4, 11, 22 ], [ "bool",5,  0,  1 ],
    [ "strlen", 6, 11, 22 ],
) {
    my ($what, $kind, $o, $i) = @$case;
    my $fn = sub { XS::APItest::savestack::test_suspend_cint($kind, $o, $i, shift) };

    my ($during, $af, $d, $at, $al, $nsv) = $fn->(0);
    is $during, $i, "$what during: localized C value";
    is $af,     $o, "$what after freeze: parked to outer";
    is $d,      0,  "$what after freeze: save stack unwound";
    is $at,     $i, "$what after thaw: localized value re-applied";
    is $al,     $o, "$what after leave: outer restored";
    is $nsv,    0,  "$what foreach_sv: 0 SVs";

    my (undef, $caf, undef, $cfree, $cal) = $fn->(1);
    is $caf,   $o, "$what cancel: parked to outer";
    is $cfree, $o, "$what cancel: outer after frozen_free";
    is $cal,   $o, "$what cancel: outer after leave";
}

# --- SAVEt_SPTR (non-refcounted pointer) and SAVEt_ITEM (sv_replace) -----
for my $case (
    [ "sptr", \&XS::APItest::savestack::test_suspend_sptr, 0 ],
    [ "item", \&XS::APItest::savestack::test_suspend_item, 1 ],
) {
    my ($what, $fn, $want_nsv) = @$case;

    my ($during, $af, $d, $at, $al, $nsv) = $fn->("OUTER", "INNER", 0);
    is $during, "INNER", "$what during: localized value";
    is $af,     "OUTER", "$what after freeze: parked to outer";
    is $d,      0,       "$what after freeze: save stack unwound";
    is $at,     "INNER", "$what after thaw: localized value re-applied";
    is $al,     "OUTER", "$what after leave: outer restored";
    is $nsv,    $want_nsv, "$what foreach_sv: $want_nsv SVs";

    my (undef, $caf, $cd, $cfree, $cal) = $fn->("OUTER", "INNER", 1);
    is $caf,   "OUTER", "$what cancel: parked to outer";
    is $cfree, "OUTER", "$what cancel: outer after frozen_free";
    is $cal,   "OUTER", "$what cancel: outer after leave";

    leak 50, 0, sub { my @r = $fn->("OUTER", "INNER", 0) },
        "$what freeze -> thaw leaks no SVs";
    leak 50, 0, sub { my @r = $fn->("OUTER", "INNER", 1) },
        "$what freeze -> frozen_free leaks no SVs";
}

# --- deferred user callbacks: frozen_free must NOT run them; run_deferred does
# SAVEt_DESTRUCTOR_X is a user callback (defer/finally). freeze never runs it; a
# plain discard (frozen_free) must NOT run it; cancellation runs it explicitly
# via savestack_frozen_run_deferred. (thaw still defers it to leave_scope.)
{
    my ($delta, $at_freeze, $at_mid, $at_end, $nsv)
        = XS::APItest::savestack::test_suspend_destructor_x(0);       # thaw path
    is $delta,     0, "destructor_x: save stack unwound by freeze";
    is $at_freeze, 0, "destructor_x: freeze does NOT run the destructor";
    is $at_mid,    0, "destructor_x thaw: re-registered, not yet run";
    is $at_end,    1, "destructor_x thaw: runs once at leave_scope";
    is $nsv,       0, "destructor_x: foreach_sv 0 SVs";

    my (undef, $af1, $mid1, $end1)
        = XS::APItest::savestack::test_suspend_destructor_x(1);       # cancel path
    is $af1,  0, "destructor_x cancel: freeze does NOT run it";
    is $mid1, 1, "destructor_x cancel: run_deferred RUNS it";
    is $end1, 1, "destructor_x cancel: frozen_free does NOT run it again";

    my (undef, $af2, $mid2, $end2)
        = XS::APItest::savestack::test_suspend_destructor_x(2);       # plain discard
    is $af2,  0, "destructor_x discard: freeze does NOT run it";
    is $mid2, 0, "destructor_x discard: no run_deferred call";
    is $end2, 0, "destructor_x discard: frozen_free alone does NOT run it";
}

# SAVEt_SET_SVFLAGS: deferred clear of SVf_IOK.
{
    my ($delta, $if, $io, $il, $nsv)
        = XS::APItest::savestack::test_suspend_setflags(0);           # thaw path
    is $if, 1, "set_svflags: freeze does not apply the flag change";
    is $io, 1, "set_svflags: thaw re-registers (IOK still set)";
    is $il, 0, "set_svflags: applied at leave_scope after thaw (IOK cleared)";

    my (undef, $if2, $io2, $il2)
        = XS::APItest::savestack::test_suspend_setflags(1);           # cancel path
    is $if2, 1, "set_svflags cancel: not applied by freeze";
    is $io2, 0, "set_svflags cancel: frozen_free applies it (IOK cleared)";
    is $il2, 0, "set_svflags cancel: stays cleared";
}

# SAVEt_FREESV: freeze retains, thaw+leave / frozen_free free exactly once.
{
    my ($delta, $rc, $nsv) = XS::APItest::savestack::test_suspend_freesv(0);
    is $delta, 0, "freesv: save stack unwound by freeze";
    is $rc,    1, "freesv: freeze retains the SV (not freed)";
    is $nsv,   1, "freesv: foreach_sv reports the 1 retained SV";

    leak 50, 0, sub { XS::APItest::savestack::test_suspend_freesv(0) },
        "freesv freeze -> thaw frees exactly once (no leak)";
    leak 50, 0, sub { XS::APItest::savestack::test_suspend_freesv(1) },
        "freesv freeze -> frozen_free frees exactly once (no leak)";
}

# --- char* slot restores: GENERIC_PVREF / SHARED_PVREF / RCPV -----------
for my $case (
    [ "generic_pvref", 0 ],
    [ "shared_pvref",  1 ],
    [ "rcpv",          2 ],
) {
    my ($what, $kind) = @$case;
    my $fn = sub { XS::APItest::savestack::test_suspend_pvref($kind, shift) };

    my ($during, $af, $d, $at, $al, $nsv) = $fn->(0);
    is $during, "INNER", "$what during: localized pv";
    is $af,     "OUTER", "$what after freeze: parked to outer";
    is $d,      0,       "$what after freeze: save stack unwound";
    is $at,     "INNER", "$what after thaw: localized pv re-installed";
    is $al,     "OUTER", "$what after leave: outer restored";
    is $nsv,    0,       "$what foreach_sv: 0 SVs";

    my (undef, $caf, undef, $cfree, $cal) = $fn->(1);
    is $caf,   "OUTER", "$what cancel: parked to outer";
    is $cfree, "OUTER", "$what cancel: outer after frozen_free";
    is $cal,   "OUTER", "$what cancel: outer after leave";

    # stress: many round-trips must not crash / double-free (DEBUGGING poisons)
    $fn->($_ % 2) for 1 .. 200;
    pass "$what: 200 freeze/thaw+cancel cycles stable";
}

# --- refcount correctness: neither path may leak an SV -------------------
leak 50, 0, sub { my @r = run("OUTER", "INNER", 0) },
    "freeze -> thaw round-trip leaks no SVs";
leak 50, 0, sub { my @r = run("OUTER", "INNER", 1) },
    "freeze -> frozen_free (cancel) leaks no SVs";

# --- pad-scope save types (the parkapi extension) ------------------------
# These are what a real async sub with lexicals leaves on the save stack.
# Each XSUB drives a self-contained freeze/thaw (or freeze/frozen_free) against
# a self-owned pad, returning a 6-tuple of checkpoints.

# SAVEt_COMPPAD: PL_comppad reverts to outer on freeze, is re-installed on thaw.
{
    my ($during, $af, $delta, $aop, $al, $nsv)
        = XS::APItest::savestack::test_suspend_comppad(0);
    ok $during, "comppad during: localized pad active";
    ok $af,     "comppad after freeze: reverted to outer pad";
    is $delta, 0, "comppad after freeze: save stack unwound";
    ok $aop,    "comppad after thaw: localized pad re-installed";
    ok $al,     "comppad after leave: outer pad restored";
    is $nsv, 0, "comppad foreach_sv: 0 SVs";

    my (undef, undef, undef, $caop, $cal)
        = XS::APItest::savestack::test_suspend_comppad(1);
    ok $caop, "comppad cancel: outer pad after frozen_free";
    ok $cal,  "comppad cancel: outer pad after leave";

    XS::APItest::savestack::test_suspend_comppad($_ % 2) for 1 .. 200;
    pass "comppad: 200 freeze/thaw+cancel cycles stable";
    leak 50, 0, sub { XS::APItest::savestack::test_suspend_comppad($_) for 0,1 },
        "comppad: freeze/thaw + cancel leak no SVs";
}

# SAVEt_CLEARSV: freeze must not clear the lexical; leave_scope (after thaw)
# clears it; a cancelled suspension drops the clear.
{
    my ($ad, $af, $delta, $aop, $cleared, $nsv)
        = XS::APItest::savestack::test_suspend_clearsv(0);
    ok $ad,       "clearsv during: lexical alive";
    ok $af,       "clearsv after freeze: lexical NOT cleared";
    is $delta, 0, "clearsv after freeze: save stack unwound";
    ok $aop,      "clearsv after thaw: still alive";
    ok $cleared,  "clearsv after leave: cleared";
    is $nsv, 0,   "clearsv foreach_sv: 0 SVs";

    my (undef, undef, undef, undef, $ccleared)
        = XS::APItest::savestack::test_suspend_clearsv(1);
    ok !$ccleared, "clearsv cancel: clear dropped, lexical still alive at leave";

    leak 50, 0, sub { XS::APItest::savestack::test_suspend_clearsv($_) for 0,1 },
        "clearsv: freeze/thaw + cancel leak no SVs";
}

# SAVEt_CLEARPADRANGE: same handler, count>1; both slots cleared after thaw.
{
    my ($ad, $af, $delta, $aop, $both, $nsv)
        = XS::APItest::savestack::test_suspend_clearpadrange(0);
    ok $ad,       "clearpadrange during: both lexicals alive";
    ok $af,       "clearpadrange after freeze: not cleared";
    is $delta, 0, "clearpadrange after freeze: save stack unwound";
    ok $both,     "clearpadrange after leave: both cleared";
    is $nsv, 0,   "clearpadrange foreach_sv: 0 SVs";

    my (undef, undef, undef, undef, $cboth)
        = XS::APItest::savestack::test_suspend_clearpadrange(1);
    ok !$cboth, "clearpadrange cancel: clear dropped";

    leak 50, 0, sub { XS::APItest::savestack::test_suspend_clearpadrange($_) for 0,1 },
        "clearpadrange: freeze/thaw + cancel leak no SVs";
}

# SAVEt_PADSV_AND_MORTALIZE: freeze parks the localized SV and reverts the slot
# to outer; thaw re-installs localized; leave_scope restores outer.
{
    my ($during, $af, $delta, $aop, $al, $nsv)
        = XS::APItest::savestack::test_suspend_padsv(0);
    is $during, "LOCAL", "padsv during: slot holds localized";
    is $af,     "OUTER", "padsv after freeze: slot parked to outer";
    is $delta,  0,       "padsv after freeze: save stack unwound";
    is $aop,    "LOCAL", "padsv after thaw: localized re-installed";
    is $al,     "OUTER", "padsv after leave: outer restored";
    is $nsv,    1,       "padsv foreach_sv: 1 SV (the localized value)";

    my (undef, $caf, undef, $caop, $cal)
        = XS::APItest::savestack::test_suspend_padsv(1);
    is $caf,  "OUTER", "padsv cancel: parked to outer";
    is $caop, "OUTER", "padsv cancel: outer after frozen_free";
    is $cal,  "OUTER", "padsv cancel: outer after leave";

    leak 50, 0, sub { XS::APItest::savestack::test_suspend_padsv($_) for 0,1 },
        "padsv: freeze/thaw + cancel leak no SVs";
}

# NB: magical-container support (local %ENV/%SIG, local $ENV{...}) is validated
# end-to-end by Future::AsyncAwait's own test suite (t/26local-hash.t et al.),
# which drives real op-level `local` on magical hashes across an await -- the
# machinery a bare XSUB here cannot faithfully reconstruct. The freeze/thaw
# handlers replay set-magic via S_resync_magic (scope.c).

done_testing;

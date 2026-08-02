#!perl -w
# Tests for the interpreter execution-state API - the four-level ladder
# (see execstate.h and Porting/execstate_api.md).

use strict;
use warnings;
use Test::More;

use XS::APItest;

# ---- level 1: register snapshot (execstate_save / _load) ------------------
is XS::APItest::execstate::roundtrip_ok(), 1,
    "L1: execstate_save/load round-trips the live execution registers exactly";

# also with a non-trivial dynamic scope live (a local pushes a save entry)
{
    local $XS::APItest::execstate::probe = 1;
    is XS::APItest::execstate::roundtrip_ok(), 1,
        "L1: round-trip is exact with a live local() on the save stack";
}

# ---- level 2: fresh-stack lifecycle (init / unwind / destroy) -------------
# Stand up and tear down a fresh set of stacks under the running interpreter,
# then keep using it - if the swap were not exact, the ops below would crash.
is XS::APItest::execstate::lifecycle_ok(), 1,
    "L2: execstate_init/unwind/destroy stands up and frees a fresh context";

{
    local $XS::APItest::execstate::probe = 2;   # a live local() across the lifecycle
    is XS::APItest::execstate::lifecycle_ok(), 1,
        "L2: lifecycle is clean with a live dynamic scope in the caller";
}

# the interpreter is unharmed afterwards
{
    my @a = map { $_ * $_ } 1 .. 10;
    is "@a", "1 4 9 16 25 36 49 64 81 100",
        "L2: interpreter computes correctly after the lifecycle round-trip";
}

# ---- level 3: pad derive / free -------------------------------------------
is XS::APItest::execstate::padlist_ok(sub { my ($x, $y); $x + $y }), 1,
    "L3: execstate_derive_padlist yields a distinct padlist sharing the name pad; free releases it";

# ---- level 4: JMPENV transfer registers -----------------------------------
is XS::APItest::execstate::jmpenv_ok(), 1,
    "L4: execstate_topenv/_restartop aliases, _topenv_root and _topenv_reset behave";

# ...even from inside a live eval frame (a deeper JMPENV chain)
{
    my $r = eval { XS::APItest::execstate::jmpenv_ok() };
    is $r, 1, "L4: topenv_root finds the base handler from within an eval";
}

done_testing;

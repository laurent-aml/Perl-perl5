#!perl
# A backend that installs itself the CPAN way - writing the shared PL_modglobal
# struct from its own BOOT block, without core's multicore_register() - must be
# visible to perlmulticore_active() straight away.
#
# This needs its own file.  Core's pointer to the shared struct is resolved lazily,
# so anything that drives the bracket first would resolve it and hide the question:
# what a module sees when it asks "is a backend installed?" before anything else in
# the process has touched the hook.  That is the order a real backend arrives in,
# and the answer decides whether the module releases the interpreter at all.

use strict;
use warnings;
use Test::More;
use XS::APItest;

plan tests => 4;

XS::APItest::multicore::install_via_modglobal();

is XS::APItest::multicore::active(), 1,
    "CPAN-style backend seen as active as the first core touch";
is XS::APItest::multicore::run_blocking(), 499500,
    "the bracket computes with it";
is XS::APItest::multicore::released(), 1,
    "core perlinterp_release drove its release hook";
is XS::APItest::multicore::acquired(), 1,
    "and its acquire hook, paired";

XS::APItest::multicore::uninstall();

#!perl -w
# The core multicore hook (perlmulticore.h): perlinterp_release/acquire, the
# multicore_register backend, the no-backend nop default, and - crucially -
# backward compatibility with the deployed CPAN ABI (a backend that writes the
# PL_modglobal "perl_multicore_api" struct directly must drive core's bracket).

use strict;
use warnings;
use Test::More;

use XS::APItest;

my $M = 'XS::APItest::multicore';

# --- no backend: the bracket is a nop, the C section runs inline -----------
is XS::APItest::multicore::active(), 0, "no backend -> perlmulticore_active() false";
is XS::APItest::multicore::run_blocking(), 499500, "bracketed C section runs inline with no backend";
is XS::APItest::multicore::released(), 0, "release hook not called when no backend";

# --- backend installed via the core multicore_register() API ---------------
XS::APItest::multicore::install();
is XS::APItest::multicore::active(), 1, "register() -> active";
is XS::APItest::multicore::run_blocking(), 499500, "bracket still computes with a backend";
is XS::APItest::multicore::released(), 1, "release hook called once";
is XS::APItest::multicore::acquired(), 1, "acquire hook called once (paired)";
XS::APItest::multicore::uninstall();
is XS::APItest::multicore::active(), 0, "unregister() -> inactive";

# --- BACKWARD COMPAT: a backend installed the CPAN way (writing the shared
#     PL_modglobal struct directly, as Coro::Multicore does) must also drive
#     core's perlinterp_release/acquire, with no core-API call involved --------
XS::APItest::multicore::install_via_modglobal();
is XS::APItest::multicore::active(), 1,
    "CPAN-style backend (direct PL_modglobal struct) seen as active by core";
is XS::APItest::multicore::run_blocking(), 499500,
    "core bracket still computes with a CPAN-style backend";
is XS::APItest::multicore::released(), 1,
    "core perlinterp_release drove the CPAN-style backend's release hook";
is XS::APItest::multicore::acquired(), 1,
    "core perlinterp_acquire drove the CPAN-style backend's acquire hook (paired)";

XS::APItest::multicore::uninstall();
is XS::APItest::multicore::active(), 0, "uninstalled -> inactive again";

# --- offload: multicore_offload / multicore_register_offload ---------------
# no backend: work then done run inline, on the calling thread
is XS::APItest::multicore::offload_active(), 0, "no offload backend -> inactive";
XS::APItest::multicore::run_offload();
is XS::APItest::multicore::off_workran(), 1, "inline offload ran work";
is XS::APItest::multicore::off_doneran(), 1, "inline offload ran done";
is XS::APItest::multicore::off_work_ctx_ok(), 1,
    "inline offload handed work a well-formed context, with no cancel flag";
is XS::APItest::multicore::off_done_ctx_ok(), 1,
    "inline offload handed done a well-formed context, not cancelled";

SKIP: {
    skip "no pthreads in this build", 6
        unless XS::APItest::multicore::have_pthread();

    XS::APItest::multicore::install_offload();
    is XS::APItest::multicore::offload_active(), 1, "offload backend registered -> active";

    XS::APItest::multicore::run_offload();
    ok XS::APItest::multicore::work_ran_off_thread(),
        "offload: work ran on a different OS thread";
    ok XS::APItest::multicore::done_ran_on_main(),
        "offload: done ran on the calling (interpreter) thread";
    is XS::APItest::multicore::off_work_ctx_ok(), 1,
        "offload backend handed work a well-formed context";
    is XS::APItest::multicore::off_done_ctx_ok(), 1,
        "offload backend handed done a well-formed context";

    XS::APItest::multicore::uninstall_offload();
    is XS::APItest::multicore::offload_active(), 0, "offload backend unregistered";
}

done_testing;

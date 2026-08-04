#!perl -w
# What multicore_offload () hands back: a handle object, whichever backend is
# installed - or none, where core supplies a resolved PerlMulticore::Handle so
# that a module's asynchronous entry point returns one shape everywhere.
#
# multicore.t covers the primitive itself (that work and done run, and where).
# This covers the shape: the handle's protocol, the synchronous wrapper that
# waits for it, and multicore_offload_ready () for a value that never went near
# a worker.

use strict;
use warnings;
use Test::More;

use XS::APItest;

my $M = 'XS::APItest::multicore';

# --- no backend: an inline offload still hands back a resolved handle ------

my $handle = XS::APItest::multicore::run_offload_async();

ok ref $handle, "offload with no backend returns an object, not a value";
isa_ok $handle, 'PerlMulticore::Handle';

ok $handle->AWAIT_IS_READY, "the handle core supplies is already resolved";
ok !$handle->AWAIT_IS_CANCELLED, "and not cancelled";
is scalar $handle->AWAIT_GET, undef, "AWAIT_GET yields what done () returned";
is scalar $handle->get, undef, "get () agrees, and does not block on a resolved handle";

# every method the contract in perlmulticore.h names must be there, since a
# module may use any of them without knowing which backend answered
can_ok $handle, qw(
   AWAIT_IS_READY AWAIT_IS_CANCELLED AWAIT_GET AWAIT_WAIT AWAIT_ON_READY
   AWAIT_DONE AWAIT_FAIL AWAIT_NEW_DONE AWAIT_NEW_FAIL AWAIT_CLONE get cancel
);

my $called = 0;
$handle->AWAIT_ON_READY (sub { ++$called });
is $called, 1, "AWAIT_ON_READY on a resolved handle fires at once";

# --- the synchronous wrapper -----------------------------------------------

is XS::APItest::multicore::run_offload(), undef,
   "multicore_offload_sync () hands back done ()'s value, not the handle";

# --- a value that never went near a worker --------------------------------

my $ready = XS::APItest::multicore::offload_ready ("cached");

isa_ok $ready, 'PerlMulticore::Handle';
ok $ready->AWAIT_IS_READY, "multicore_offload_ready () yields a resolved handle";
is scalar $ready->get, "cached", "carrying the value it was given";
is_deeply [ $ready->AWAIT_GET ], [ "cached" ], "AWAIT_GET in list context too";

# --- the protocol's own constructors --------------------------------------

my $failed = PerlMulticore::Handle->AWAIT_NEW_FAIL ("nope");
ok !eval { $failed->AWAIT_GET; 1 }, "a failed handle raises from AWAIT_GET";
like $@, qr/^nope at \Q$0\E line /, "raised where the value was asked for";

my $pending = PerlMulticore::Handle->new->AWAIT_CLONE;
ok !$pending->AWAIT_IS_READY, "AWAIT_CLONE yields a pending handle";
$pending->AWAIT_DONE ("late");
is scalar $pending->AWAIT_GET, "late", "which AWAIT_DONE resolves";

# with no backend there is no event system to turn, so a pending handle cannot
# be waited for - which is the one thing core's handle refuses to pretend
my $stuck = PerlMulticore::Handle->new;
ok !eval { $stuck->get; 1 }, "get () on a pending handle with no backend dies";
like $@, qr/without an offload backend/, "saying why";

# --- with the thread-pool backend ------------------------------------------

SKIP: {
    skip "no pthreads in this build", 4
        unless XS::APItest::multicore::have_pthread();

    XS::APItest::multicore::install_offload();

    my $h = XS::APItest::multicore::run_offload_async();

    ok ref $h, "a backend's offload returns an object too";
    ok $h->AWAIT_IS_READY,
       "this backend resolves before returning, which the contract permits";
    is scalar $h->get, undef, "and get () returns at once";

    is XS::APItest::multicore::run_offload(), undef,
       "the synchronous wrapper works through a backend as well";

    XS::APItest::multicore::uninstall_offload();
}

# --- a deferred backend: the handle is really pending ----------------------
# The backend above resolves before returning, which hides the whole point of a
# handle.  This one does not wait at all - the shape a stackless (Future) backend
# has - so the handle comes back pending and stays that way until something
# resolves it, here an explicit tick standing in for an event loop.

# The same backend is run over two unrelated handle classes: core's own, and
# Future, which implements the same AWAIT_* protocol.  That the contract is
# duck-typed rather than tied to a class is the claim.
my @classes = ('PerlMulticore::Handle');
push @classes, 'Future' if eval { require Future; 1 };

SKIP: {
    skip "no pthreads in this build", 13 * @classes
        unless XS::APItest::multicore::have_pthread();

    for my $class (@classes) {
        XS::APItest::multicore::install_offload_deferred ($class);

        my $h = XS::APItest::multicore::run_offload_async();

        isa_ok $h, $class, "$class: the deferred backend's handle";
        ok !$h->AWAIT_IS_READY, "$class: it comes back pending - nothing waited";

        my $ready = 0;
        $h->AWAIT_ON_READY (sub { $ready++ });
        is $ready, 0, "$class: AWAIT_ON_READY has not fired yet";

        ok XS::APItest::multicore::offload_tick(), "$class: the tick resolves it";

        ok $h->AWAIT_IS_READY, "$class: now ready";
        is $ready, 1, "$class: and AWAIT_ON_READY fired";
        is scalar $h->get, undef, "$class: get () yields done ()'s value";

        # a croak from done () is the module's failure channel, and for a deferred
        # backend it can only be delivered through the handle
        my $f = XS::APItest::multicore::run_offload_croaking_async();
        XS::APItest::multicore::offload_tick();

        ok $f->AWAIT_IS_READY, "$class: a croaking done () still resolves the handle";
        ok !eval { $f->get; 1 }, "$class: as a failure";
        like $@, qr/deliberate croak from done/, "$class: carrying what it croaked with";

        # A done () that stopped early must raise rather than hand back a
        # truncated result the caller could not tell apart from a whole one - and
        # what it salvaged travels in the exception.
        my $p = XS::APItest::multicore::run_offload_partial_async();
        XS::APItest::multicore::offload_tick();

        ok !eval { $p->get; 1 }, "$class: a done () that stopped early fails the handle";
        isa_ok $@, 'PerlMulticore::Cancelled', "$class: with the exception core supplies";
        is $@->get, "chunks=3", "$class: from which what it salvaged is reachable";
        like "$@", qr/stopped early/, "$class: and stringifying to why";

        XS::APItest::multicore::uninstall_offload();
    }
}

done_testing;

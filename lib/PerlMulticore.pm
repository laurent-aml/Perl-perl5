package PerlMulticore;

use strict;
use warnings;

our $VERSION = '0.01';

# The two classes the offload interface needs on the perl side (see
# perlmulticore.h): the handle multicore_offload () hands back when no backend is
# installed, and the exception a module raises when it was asked to stop early and
# what it had was incomplete.  One file, because they are one interface and a
# backendless perl must be able to load it without anything installed.

package PerlMulticore::Handle;

our $VERSION = '0.01';

sub new            { bless { ready => 0 }, ref $_[0] || $_[0] }
sub AWAIT_CLONE    { bless { ready => 0 }, ref $_[0] || $_[0] }
sub AWAIT_NEW_DONE { my $class = shift; bless { ready => 1, values  => [ @_ ] }, ref $class || $class }
sub AWAIT_NEW_FAIL { my $class = shift; bless { ready => 1, failure => $_[0]  }, ref $class || $class }

sub AWAIT_IS_READY     { $_[0]{ready} ? 1 : 0 }
sub AWAIT_IS_CANCELLED { $_[0]{cancelled} ? 1 : 0 }

# A failure is raised at the point that asked for the value, $level frames up,
# rather than from inside this file.
sub _result {
    my ($self, $level) = @_;

    if (defined (my $failure = $self->{failure})) {
        die $failure if ref $failure;

        my (undef, $file, $line) = caller $level;
        $failure .= " at $file line $line.\n" unless $failure =~ /\n\z/;

        die $failure;
    }

    my $values = $self->{values} || [];

    wantarray ? @$values : $values->[0]
}

sub AWAIT_GET { $_[0]->_result (1) }

# Nothing here can be pending unless the program made it so itself, and with no
# backend there is no event system to run, so waiting can only report that.
sub AWAIT_WAIT {
    my $self = shift;

    die "PerlMulticore::Handle: cannot wait for a pending handle without an offload backend\n"
        unless $self->{ready};

    $self->_result (1)
}

*get = \&AWAIT_WAIT;

sub AWAIT_ON_READY {
    my ($self, $cb) = @_;

    if ($self->{ready}) {
        $cb->($self);
    } else {
        push @{$self->{on_ready}}, $cb;
    }
}

sub AWAIT_DONE {
    my $self = shift;

    $self->{values} = [ @_ ];
    $self->_resolved
}

sub AWAIT_FAIL {
    my ($self, $failure) = @_;

    $self->{failure} = $failure;
    $self->_resolved
}

sub _resolved {
    my $self = shift;

    $self->{ready} = 1;

    if (my $on_ready = delete $self->{on_ready}) {
        $_->($self) for @$on_ready;
    }

    ()
}

sub AWAIT_ON_CANCEL    { push @{$_[0]{on_cancel}}, $_[1] }
sub AWAIT_CHAIN_CANCEL { push @{$_[0]{on_cancel}}, $_[1] }

sub cancel {
    my $self = shift;

    return if $self->{ready};

    $self->{cancelled} = 1;

    if (my $on_cancel = delete $self->{on_cancel}) {
        for my $c (@$on_cancel) {
            ref $c eq 'CODE' ? $c->($self) : $c->cancel;
        }
    }

    $self->{failure} = PerlMulticore::Cancelled->new;
    $self->_resolved;

    ()
}

# Nothing core hands back is ever still running, so there is no asynchronous
# cleanup to wait for: safe-cancelling is a prompt cancel, and the cleanup this
# reports is already finished.  A backend whose work IS still running does more -
# see Coro::Multicore.
sub safe_cancel {
    my $self = shift;

    $self->cancel;

    (ref $self || $self)->AWAIT_NEW_DONE
}

sub AWAIT_CHAIN_SAFE_CANCEL { push @{$_[0]{safe_children}}, $_[1] }

package PerlMulticore::Cancelled;

our $VERSION = '0.01';

# Raised by a module's `done` when it was asked to stop early and what it had was
# incomplete.  Not raised when the work finished anyway - a cancellation that
# arrives during the last chunk is simply too late, and a whole result is a whole
# result.
sub new {
    my ($class, %args) = @_;

    bless {
        message => $args{message} || "offload cancelled",
        partial => $args{partial},
    }, $class
}

sub message { $_[0]{message} }

# Whatever the module salvaged, if it had anywhere to put it - usually it does
# not, because the partial output is already in the buffers the caller owns.
sub get { $_[0]{partial} }

sub throw { my $class = shift; die $class->new (@_) }

use overload
    '""'     => sub { $_[0]{message} . "\n" },
    fallback => 1;

1;

__END__

=head1 NAME

PerlMulticore - the perl side of the offload interface

=head1 SYNOPSIS

    # neither class is named by a module that offloads: they arrive from
    # multicore_offload (see perlmulticore.h)

    my $handle = $obj->scramble_async ($buf);
    my $result = eval { $handle->get };

    if (my $err = $@) {
        ref $err && $err->isa ("PerlMulticore::Cancelled")
            or die $err;

        # it stopped early: what is in $buf is not the whole answer
    }

=head1 DESCRIPTION

C<multicore_offload> (see F<perlmulticore.h>) hands an XS module back a handle
rather than a value, so that a module offering an asynchronous entry point returns
the same shape whichever offload backend the application installed - or none.
This module is the perl side core itself provides: the handle for when there is no
backend, and the exception for a cancelled offload, which every backend shares.

=head2 PerlMulticore::Handle

The handle core hands back when no backend is installed. C<work> and C<done> have
already run inline by then, so it is always resolved before the caller sees it.

The protocol is the C<AWAIT_*> method set of Future::AsyncAwait::Awaitable, which
is duck-typed - implementing the methods is all that is required, and nothing here
needs Future installed - plus C<get>, the blocking adapter, and C<cancel>.

Because there is no backend, and therefore no event system to turn, C<get> and
C<AWAIT_WAIT> on an instance that is somehow still pending can only die. Every
handle core produces is resolved. Likewise C<safe_cancel> has nothing to wait for
and is simply a prompt C<cancel>.

This is not a general-purpose future implementation and it is not meant to be
subclassed by a backend: a backend supplies its own handle over its own job.

=head2 PerlMulticore::Cancelled

The exception a module's C<done> raises when it was asked to stop early and the
result it would have produced is incomplete. Raising rather than returning is the
point: a truncated result that the caller cannot tell apart from a whole one is the
worst outcome available, and the caller that awaits an offload is frequently not
the one that cancelled it, so it has no reason to go looking for a flag.

It is B<not> raised when the work finished anyway. C<done_ctx.cancelled> means
cancellation was I<requested>; a C<work> that was on its last chunk when the flag
went up produces a whole result, and a whole result is not an error.

=over 4

=item message

    $str = $err->message;

The description, and what the exception stringifies to.

=item get

    $data = $err->get;

Whatever the module salvaged, or C<undef> - which is the usual answer, and not a
gap in the interface: a partial result almost always sits in buffers the caller
already owns (the ndarray it passed, the string it handed over), so there is
nothing for the exception to carry. It exists for a module that marshals its result
only at C<done> time and therefore has nowhere else to put a truncated one.

Note what this does I<not> promise: that the data is usable. PDL, for one,
salvages nothing and marks its part-written outputs so that reading one computes it
again.

=item new, throw

    die PerlMulticore::Cancelled->new (
        message => $str, partial => $data);

    PerlMulticore::Cancelled->throw;

Both arguments are optional. From C, C<multicore_offload_cancelled> builds one.

=back

=head1 EXPERIMENTAL

The offload interface, and this module with it, are experimental and may change
or be removed.

=cut

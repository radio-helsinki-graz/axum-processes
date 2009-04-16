#!/usr/bin/perl

# Requirements:
#   Standard Perl install
#   JSON::XS
#    (to install, run `cpan install JSON::XS`)

use strict;
use warnings;
use IO::Socket::UNIX;
use Term::ReadLine;
use JSON::XS;

my $s = IO::Socket::UNIX->new("/tmp/axum-address");
die "Couldn't connect: $!\n" if !$s || !$s->connected;

my $t = Term::ReadLine->new("MambaNet Address Client");
my $O = $t->OUT || \*STDOUT;

printf $O "Connected to %s\n", $s->peerpath;

while(defined ($_ = $t->readline("addr > "))) {
  exit if /^(q|quit|exit)$/;
  if(/^(?:get|list)( .+)?$/) {
    my %o = $1 ? eval $1 : ();
    $o{limit} = 50 if !defined $o{limit};
    printf $s "GET %s\n", encode_json(\%o);
    my $re = <$s>;
    if($re !~ /^NODES (.+)$/) {
      print $O $re;
      next;
    }
    $re = decode_json($1);
    if(!$re || !$re->{result} || !@{$re->{result}}) {
      print $O "No result.\n";
      next;
    }
    print " Address   UniqueID        Parent          S   Engine    Name\n";      
    printf $O " %s  %s  %s  %02X  %s  %s\n", @{$_}{qw| MambaNetAddr UniqueID Parent Services EngineAddr Name|}
      for (@{$re->{result}});
  } else {
    print $O "Uknown command\n";
  }
}


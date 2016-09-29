
# vmss

Tooling for VMware's VM Suspended State (VMSS) files -- which at the moment
consists of only a single program.  (As/if this expands, this should be
refactored into a library.)  The VMSS format appears to be entirely
undocumented; it was inferred by looking at the VMSS support that VMware
added for Red Hat's [crash utility](https://github.com/crash-utility/crash).

## vmss-nmi

Drops an NMI on a suspended VM by setting `pendingNMI` for a specified CPU
to be 1.  Upon resume, NMI will be received, the behavior of which will
be specific to the guest operating system.  (On SmartOS and other illumos
derivatives, an NMI will induce a panic; this can be used to force a
dump of an otherwise hung VM.)



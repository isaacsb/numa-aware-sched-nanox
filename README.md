NUMA-aware schedule policies for Nanos++
========================================

This is a set of plugins for the [Nanos++][nanox] runtime system and
has been developed at
the [Runtime-aware architectures][runtime-aware BSC] team
from the [Barcelona Supercomputing Center (BSC)][BSC].

This work work is part of the ICS ’18 article “Reducing Data Movement
on Large Shared Memory Systems by Exploiting Computation
Dependencies”. See [how to cite](#how-to-cite) for further details.


## Installation instructions
### Requirements

1. [Nanos++][nanox] runtime library
2. [hwloc][hwloc] library
3. [libnuma][numactl] as a shared library
    * Needed for page migration support
4. [SCOTCH][scotch] graph partitioning library v6.0.x
5. (optional) [Metapart][metapart] library with SCOTCH support inside
    * Good for the RIP-MW method (see [how to cite](#how-to-cite))


### Install Nanos++
You first need
to [install the Nanos++ runtime system][nanox-install]. Apply this
patch before compiling the code to ensure better executions:

```diff
diff --git a/src/core/schedule.cpp b/src/core/schedule.cpp
index e41ec45..7e6f1a7 100644
--- a/src/core/schedule.cpp
+++ b/src/core/schedule.cpp
@@ -465,7 +465,7 @@ void Scheduler::waitOnCondition (GenericSyncCond *condition)
                //! Second calling scheduler policy at block
                if ( !next ) {
                   memoryFence();
-                  if ( sys.getSchedulerStats()._readyTasks > 0 ) {
+                  if ( sys.getSchedulerStats()._readyTasks > 0 or thread->getTeam()->getSchedulePolicy().testDequeue() ) {
                      if ( sys.getSchedulerConf().getSchedulerEnabled() )
                         next = thread->getTeam()->getSchedulePolicy().atBlock( thread, current );
             if ( next != NULL ) {
```

This patch is needed for some corner-case executions and not adding it
might not affect you. However, it is recommended to apply the patch.


### Install the NUMA-aware schedule policies for Nanos++
These plugins need to be installed in the same path as Nanos++, so you need to have write permissions in that path. Considering the sources are in `$NANOX_NUMA_AWARE_SOURCES`, and you are building in `$NANOX_NUMA_AWARE_BUILD`

```
cd $NANOX_NUMA_AWARE_SOURCES
autoreconf -fiv
cd $NANOX_NUMA_AWARE_BUILD
$NANOX_NUMA_AWARE_SOURCES/configure  \
    --with-nanox=${NANOX_HOME}       \
    --prefix=${NANOX_HOME}           \
    --with-scotch=${SCOTCH_HOME}     \
    --with-metapart=${METAPART_HOME}
```


## Usage instructions
### Application requirements
The application can be written using either OmpSs (version 1) or
OpenMP 4.0 as supported by Nanos++. You cannot use nested tasks (i.e.,
tasks created recursively by other tasks), data must be initialised in
parallel using tasks, with no barrier between initialisation and
computation. It will work best if data dependencies use complete
detailed regions and not just the address of the first element of the
input/ouput data blocks.

If you use OpenMP, make sure that everything in `main` (mostly
initialisation and computation) is enclosed in a single `#pragma omp
parallel` block and also `#pragma omp single` or `#pragma omp
master`. For Fortran applications use the corresponding `!$OMP`
directives.


### Execution environment
In order for the schedulers to work, you need to use
the [contiguous regions][cregions] (`cregions`) dependency manager in
Nanos++. For that, either set the `--deps=cregions` flag to the
`NX_ARGS` environment variable or set the `NX_DEPS` environment
variable to `cregions`.

For executing with DEP, set `--schedule=dep` in `NX_ARGS` or set
`NX_SCHEDULE=dep`.

For executing with RIP, set `--schedule=rip` in `NX_ARGS` or set
`NX_SCHEDULE=rip` (use `ripm` instead of `rip` you want to use
Metapart). You can choose between RIP-DEP and RIP-MW by setting
`NX_RIP_MODE` to `dep` and `mw` respectively (or set flag `--rip-mode`
in `NX_ARGS`).


## How to cite
Sánchez Barrera, Isaac; Moretó, Miquel; Ayguadé, Eduard; Labarta,
Jesús; Valero, Mateo, and Casas, Marc. “Reducing Data Movement on
Large Shared Memory Systems by Exploiting Computation
Dependencies”. In _ICS ’18: 2018 International Conference on
Supercomputing, June 12–15, 2018, Beijing, China_. ACM, New York, NY,
USA. <https://doi.org/10.1145/3205289.3205310>.

© [Barcelona Supercomputing Center (BSC)][BSC].

[nanox]: https://pm.bsc.es/nanox
[runtime-aware BSC]: https://www.bsc.es/research-development/research-areas/computer-architecture-and-codesign/runtime-aware-architectures
[BSC]: https://www.bsc.es/
[hwloc]: https://www.open-mpi.org/projects/hwloc/
[numactl]: https://github.com/numactl/numactl
[scotch]: https://www.labri.fr/perso/pelegrin/scotch/
[metapart]: http://metapart.gforge.inria.fr/
[nanox-install]: https://pm.bsc.es/ompss-docs/user-guide/installation.html#installation-of-nanos
[cregions]: https://pm.bsc.es/ompss-docs/user-guide/run-programs-plugin-dependence.html#contiguous-regions

Basic M-Bus Master
==================

This project provides a very basic M-Bus master for a wired network.  It
builds on [libmbus][1] and provides enough functionality to:

 - Initiate primary address scanning
 - Initiate secondary address scanning
 - Set primary address from secondary address
 - Request data from primary address


TODO
----

 - Add scripting support, for running a test program/file
 - Read single record


Build & Install
---------------

First install [libmbus][1], using the defaults it gets installed into
`/usr/local`.  Debian/Ubuntu systems support that path out of the box,
but Fedora/RedHat systems may need some special incantations.  The
installed library provides a `libmbus.pc` file, which this project use
to figure out the path to the library and include files.

Issuing

    make

builds `mbus-master` in the current directory.  Install it anywhere on
your system, and make sure to bundle the library .so file if you move
the binary to another system.


Origin & References
-------------------

Made by Addiva Elektronik AB, Sweden.  Available as Open Source under
the MIT license.  Please note, libus has a 3-clause BSD license which
contains the advertising clause.

[1]: https://github.com/rscada/libmbus

# SECD_Scheme8
Based on M. Hiroi's Common Lisp implementation of micro schemes, I rewrote it in C++, resulting in a reasonably usable micro scheme implementation. I never thought I'd get this far, but with the help of AI, I made it! I'm incredibly grateful.

I have built and tested this on clang++ on Ubuntu-24.04 and g++ on Windows 11.

Compiler on Ubuntu:
$ clang++ -v
Ubuntu clang version 18.1.3 (1ubuntu1)
Target: x86_64-pc-linux-gnu
Thread model: posix
InstalledDir: /usr/bin
Found candidate GCC installation: /usr/bin/../lib/gcc/x86_64-linux-gnu/13
Found candidate GCC installation: /usr/bin/../lib/gcc/x86_64-linux-gnu/14
Selected GCC installation: /usr/bin/../lib/gcc/x86_64-linux-gnu/14
Candidate multilib: .;@m64
Selected multilib: .;@m64
Compiler on Windows 11:
> g++ -v
Using built-in specs.
COLLECT_GCC=C:\w64devkit\bin\g++.exe
COLLECT_LTO_WRAPPER=C:/w64devkit/bin/../libexec/gcc/x86_64-w64-mingw32/15.2.0/lto-wrapper.exe
Target: x86_64-w64-mingw32
Configured with: /dl/gcc/configure --prefix=/w64devkit --with-sysroot=/w64devkit --with-native-system-header-dir=/include --target=x86_64-w64-mingw32 --host=x86_64-w64-mingw32 --enable-static --disable-shared --with-pic --with-gmp=/deps --with-mpc=/deps --with-mpfr=/deps --enable-languages=c,c++,fortran --enable-libgomp --enable-threads=posix --enable-version-specific-runtime-libs --disable-libstdcxx-verbose --disable-dependency-tracking --disable-lto --disable-multilib --disable-nls --disable-win32-registry --enable-mingw-wildcard CFLAGS_FOR_TARGET=-O2 CXXFLAGS_FOR_TARGET=-O2 LDFLAGS_FOR_TARGET=-s CFLAGS=-O2 CXXFLAGS=-O2 LDFLAGS=-s
Thread model: posix
Supported LTO compression algorithms: zlib
gcc version 15.2.0 (GCC)
>
> 

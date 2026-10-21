#!/usr/bin/env bash
#
# Builds the CAS engine for cas.so: Giac (from KhiCAS, the calculator port by
# its author), GMP, MPFR, MPFI, libstdc++, openlibm and the quad-float part
# of libgcc, all as position-independent code for the ESP32-P4, merged into
# one archive:
#
#     apps/cas/engine/lib/libcasengine.a    the engine and its runtime
#     apps/cas/engine/lib/libcasend.a       the end marker of .init_array
#
#   apps/cas/engine/build_engine.sh            build what is missing
#   CLEAN=1 apps/cas/engine/build_engine.sh    start over
#
# Why all of it is built here and nothing comes from the toolchain: a .so
# takes only -fPIC code (ld refuses R_RISCV_HI20 in a shared object), and the
# libstdc++, libgcc and libm the toolchain ships are not. The firmware does
# not export the C++ runtime or the double-precision maths either.
#
# The sources are downloaded once into $CAS_WORK (default ~/.cache/p4os-cas,
# outside Dropbox: the build tree is ~600 MB) and checked against the hashes
# below. Nothing of it goes into git; only lib/ is left next to this script,
# and lib/ is ignored too.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${CAS_WORK:-$HOME/.cache/p4os-cas}
OUT=$HERE/lib
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}

# what the build takes from the machine, besides the toolchain: on the Mac
# it is all there; in the CI's IDF container (Ubuntu) some has to be
# installed: apt-get install m4 bzip2 xz-utils curl make gcc g++ perl
missing=""
for c in curl tar bzip2 xz m4 make cc sed awk; do command -v $c >/dev/null || missing="$missing $c"; done
command -v sha256sum >/dev/null || command -v shasum >/dev/null || missing="$missing sha256sum"
[ -z "$missing" ] || { echo "missing on this machine:$missing"; exit 1; }

if [ "${CLEAN:-0}" = 1 ]; then rm -rf "$WORK/build" "$WORK/src" "$OUT"; fi
mkdir -p "$WORK/dl" "$WORK/src" "$WORK/build" "$OUT"

# --- toolchain -------------------------------------------------------------
if ! command -v riscv32-esp-elf-gcc >/dev/null; then
    TC=$(ls -d "$HOME"/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin 2>/dev/null | tail -1)
    [ -n "$TC" ] || { echo "riscv32-esp-elf-gcc not found: export ESP-IDF first"; exit 1; }
    export PATH=$TC:$PATH
fi
T=riscv32-esp-elf
# The firmware's ABI (ilp32f); the .so is refused otherwise. The xesppie
# extension of the firmware's own -march is left out: nothing here uses it.
ARCH="-march=rv32imafc_zicsr_zifencei -mabi=ilp32f"
# No path of this machine inside the .so (build_apps.sh checks it): MPFR's
# assertions carry __FILE__.
MAP="-ffile-prefix-map=$WORK/="
CF="-Os -fPIC $ARCH -ffunction-sections -fdata-sections -DNDEBUG $MAP"

# --- sources ---------------------------------------------------------------
# KhiCAS's tree for the NumWorks: Giac 1.9 already set up for a machine with
# no OS, plus the GMP/MPFR/MPFI it is built against.
KHI_URL=https://www-fourier.univ-grenoble-alpes.fr/~parisse/numworks/khiext.tar.bz2
KHI_SHA=0d269aa2e0b69097e4bf0399ad69fa45340a9cb992c9abd08084101dc6106c34
GCC_URL=https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz
GCC_SHA=a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9
OLM_URL=https://github.com/JuliaMath/openlibm/archive/refs/tags/v0.8.8.tar.gz
OLM_SHA=721bab18ff37160fd9e903eb211ff26fdee38c3d0041d556b07d76089c435d17

fetch() {  # url sha file
    local f=$WORK/dl/$3
    if [ ! -f "$f" ]; then
        echo "downloading $3"
        curl --fail --location --retry 2 --connect-timeout 20 --max-time 600 -sS -o "$f.part" "$1"
        mv "$f.part" "$f"
    fi
    local sum
    if command -v sha256sum >/dev/null; then sum=$(sha256sum "$f"); else sum=$(shasum -a 256 "$f"); fi
    [ "${sum%% *}" = "$2" ] || { echo "$3: wrong checksum"; exit 1; }
}
fetch $KHI_URL $KHI_SHA khiext.tar.bz2
fetch $GCC_URL $GCC_SHA gcc-14.2.0.tar.xz
fetch $OLM_URL $OLM_SHA openlibm-0.8.8.tar.gz

S=$WORK/src
if [ ! -f $S/khiext/.p4os-extracted ]; then
    echo "unpacking KhiCAS"
    # Discard generated links and unused autotools links to the author's host.
    # GMP configure recreates gmp-mparam.h for the selected target.
    mkdir -p $S/khiext && tar xjf $WORK/dl/khiext.tar.bz2 -C $S/khiext --strip-components=1 \
        --exclude='*/.libs/*' --exclude='*/mpfi-1.5.4/m4/*' \
        --exclude='*/mpfi-1.5.4/build-aux/*' --exclude='*/gmp-6.2.1/gmp-mparam.h' \
        khiext/apps/KhiCAS/src/giac-1.6.0/src khiext/apps/KhiCAS/src/gmp-6.2.1 khiext/apps/KhiCAS/src/mpfr-4.1.0 \
        khiext/apps/KhiCAS/src/mpfi-1.5.4
    # the tarball carries ARM objects and Linux host tools from its own build
    find $S/khiext \( -name '*.o' -o -name '*.lo' -o -name '*.a' -o -name '*.la' \) -delete
    for f in $S/khiext/apps/KhiCAS/src/gmp-6.2.1/gen-*; do
        case $f in *.c|*.h) ;; *) rm -f "$f" ;; esac
    done
    touch $S/khiext/.p4os-extracted
fi
if [ ! -d $S/gcc-14.2.0 ]; then
    echo "unpacking GCC's libstdc++ and libgcc"
    tar xJf $WORK/dl/gcc-14.2.0.tar.xz -C $S gcc-14.2.0/libstdc++-v3 gcc-14.2.0/libgcc \
        gcc-14.2.0/config gcc-14.2.0/include gcc-14.2.0/config.sub gcc-14.2.0/config.guess \
        gcc-14.2.0/install-sh gcc-14.2.0/ltmain.sh gcc-14.2.0/libtool-ldflags gcc-14.2.0/config-ml.in gcc-14.2.0/missing \
        gcc-14.2.0/mkinstalldirs gcc-14.2.0/config.rpath gcc-14.2.0/gcc/BASE-VER \
        gcc-14.2.0/gcc/DATESTAMP gcc-14.2.0/gcc/DEV-PHASE gcc-14.2.0/libiberty
    # std::chrono's time-zone database wants threads and a filesystem; the
    # CAS needs neither
    echo "// not built for P4OS" > $S/gcc-14.2.0/libstdc++-v3/src/c++20/tzdb.cc
fi
# Older cached extractions omitted this helper used by libstdc++'s link rules.
if [ ! -f "$S/gcc-14.2.0/libtool-ldflags" ]; then
    tar xJf "$WORK/dl/gcc-14.2.0.tar.xz" -C "$S" gcc-14.2.0/libtool-ldflags
fi
if [ ! -d $S/openlibm ]; then
    mkdir -p $S/openlibm && tar xzf $WORK/dl/openlibm-0.8.8.tar.gz -C $S/openlibm --strip-components=1
    # FreeBSD's RISC-V layout of a 128-bit long double assumes a 64-bit long
    sed -i.orig 's/unsigned long\tman/unsigned long long\tman/' $S/openlibm/src/riscv_fpmath.h
fi

P=$WORK/build/prefix
mkdir -p $P/lib $P/include

# --- GMP, MPFR, MPFI -------------------------------------------------------
GMP=$S/khiext/apps/KhiCAS/src/gmp-6.2.1
if [ ! -f $P/lib/libgmp.a ]; then
    echo "building GMP"
    (cd $GMP && ./configure --host=$T --disable-assembly --disable-shared --prefix=$P \
        CFLAGS="$CF --specs=nosys.specs" > $WORK/build/gmp.log 2>&1 &&
     make -j$JOBS >> $WORK/build/gmp.log 2>&1 && make install >> $WORK/build/gmp.log 2>&1) ||
        { echo "GMP failed: $WORK/build/gmp.log"; exit 1; }
fi
MPFR=$S/khiext/apps/KhiCAS/src/mpfr-4.1.0
if [ ! -f $P/lib/libmpfr.a ]; then
    echo "building MPFR"
    (cd $MPFR && ./configure --host=$T --with-gmp=$P --disable-shared --disable-thread-safe \
        --prefix=$P CFLAGS="$CF --specs=nosys.specs" > $WORK/build/mpfr.log 2>&1 &&
     make -j$JOBS -C src >> $WORK/build/mpfr.log 2>&1 && make -C src install >> $WORK/build/mpfr.log 2>&1) ||
        { echo "MPFR failed: $WORK/build/mpfr.log"; exit 1; }
fi
MPFI=$S/khiext/apps/KhiCAS/src/mpfi-1.5.4
if [ ! -f $P/lib/libmpfi.a ]; then
    echo "building MPFI"
    # its autotools tree links to /usr/share files of the machine it came
    # from; the sources are plain C and its config header is already there
    O=$WORK/build/obj/mpfi; rm -rf $O; mkdir -p $O
    for f in $MPFI/src/*.c; do
        case $(basename $f) in set_sj.c|set_uj.c) continue ;; esac   # intmax_t, unused
        $T-gcc $CF -Wno-error=incompatible-pointer-types -DHAVE_CONFIG_H -I$MPFI -I$MPFI/src \
            -I$P/include -c $f -o $O/$(basename ${f%.c}).o
    done
    $T-ar rcs $P/lib/libmpfi.a $O/*.o
    cp $MPFI/src/mpfi.h $MPFI/src/mpfi_io.h $P/include/
fi

# --- libstdc++ (and libsupc++) ---------------------------------------------
G=$S/gcc-14.2.0
if [ ! -f $P/lib/libstdc++.a ]; then
    echo "building libstdc++ (a few minutes)"
    B=$WORK/build/libstdc++; rm -rf $B; mkdir -p $B
    # threads off: Giac runs on one thread, and gthreads would want pthreads
    # the firmware does not export
    (cd $B && CC=$T-gcc CXX=$T-g++ AR=$T-ar RANLIB=$T-ranlib NM=$T-nm \
        CFLAGS="$CF" CXXFLAGS="$CF" LDFLAGS="--specs=nosys.specs" \
        $G/libstdc++-v3/configure --host=$T --build=$($G/config.guess) --target=$T --prefix=$P \
        --disable-shared --enable-static --disable-multilib --with-newlib --disable-libstdcxx-verbose \
        --disable-libstdcxx-pch --disable-nls --disable-libstdcxx-filesystem-ts \
        --disable-libstdcxx-backtrace --disable-threads > $WORK/build/libstdc++.log 2>&1 &&
     make -j$JOBS >> $WORK/build/libstdc++.log 2>&1 && make install >> $WORK/build/libstdc++.log 2>&1) ||
        { echo "libstdc++ failed: $WORK/build/libstdc++.log"; exit 1; }
fi
CXXINC="-nostdinc++ -isystem $P/include/c++/14.2.0 -isystem $P/include/c++/14.2.0/$T"

# --- the quad-precision float routines of libgcc ---------------------------
# Giac uses long double (128-bit on RV32) here and there: powl, logl...
if [ ! -f $P/lib/libsfp.a ]; then
    echo "building libgcc's soft-fp (long double)"
    O=$WORK/build/obj/sfp; rm -rf $O; mkdir -p $O
    for f in addtf3 subtf3 multf3 divtf3 negtf2 eqtf2 getf2 letf2 unordtf2 extenddftf2 extendsftf2 \
             trunctfdf2 trunctfsf2 fixtfsi fixtfdi fixunstfsi fixunstfdi floatsitf floatunsitf \
             floatditf floatunditf; do
        $T-gcc $CF -I$G/libgcc/soft-fp -I$G/libgcc/config/riscv -I$G/libgcc -I$G/include \
            -c $G/libgcc/soft-fp/$f.c -o $O/$f.o
    done
    $T-ar rcs $P/lib/libsfp.a $O/*.o
fi

# --- openlibm: sin, pow, the complex functions... in double ----------------
if [ ! -f $P/lib/libopenlibm.a ]; then
    echo "building openlibm"
    O=$WORK/build/obj/openlibm; rm -rf $O; mkdir -p $O
    L=$S/openlibm
    # the ld128 versions first: a src/ file of the same name is the 80-bit one
    for f in $L/ld128/*.c $L/bsdsrc/*.c $L/src/*.c $L/riscv64/fenv.c; do
        n=$(basename $(dirname $f))_$(basename ${f%.c})
        case $n in ld128_s_nextafterl|ld128_s_remquol|ld128_s_truncl|src_common) continue ;; esac
        $T-gcc $CF -fno-builtin -D__BSD_VISIBLE=1 -I$L -I$L/include -I$L/src -I$L/ld128 -I$L/riscv64 \
            -c $f -o $O/$n.o 2>/dev/null || echo "  (openlibm: $n skipped)"
    done
    $T-ar rcs $P/lib/libopenlibm.a $O/ld128_*.o $O/bsdsrc_*.o $O/src_*.o $O/riscv64_*.o
fi

# --- Giac ------------------------------------------------------------------
# (khiext/src is a link to this; the folder's name is older than the code,
# which config.h says is 1.9.0)
GS=$S/khiext/apps/KhiCAS/src/giac-1.6.0/src
# what KhiCAS's Makefile.numworks.b sets, minus TAR90400000 (its file store
# in the NumWorks' flash). TLSF and MICROPY_LIB pick the code paths that do
# not touch the NumWorks' heap symbols; the hooks they need are in cas_rt.c.
GDEFS="-DDEVICE -DNUMWORKS -DNUMWORKS_DELTA -DHAVE_CONFIG_H -DIN_GIAC -DTIMEOUT -DNO_STDEXCEPT \
 -DGIAC_BINARY_ARCHIVE -DNO_UNARY_FUNCTION_COMPOSE -DNO_TEMPLATE_MULTGCD -DGIAC_NO_OPTIMIZATIONS \
 -DSTATIC_BUILTIN_LEXER_FUNCTIONS -DTLSF -DMICROPY_LIB"
GCXX="$CF $CXXINC -std=gnu++14 -fno-exceptions -fno-threadsafe-statics -fno-strict-aliasing \
 -fpermissive -w -I$GS -I$P/include $GDEFS"
GOBJS="sym2poly gausspol threaded maple ti89 mathml moyal misc permu desolve input_parser symbolic
 index modpoly modfactor ezgcd derive solve intg intgab risch lin series subst vecteur sparse csturm
 tex global ifactor alg_ext gauss isom help plot plot3d rpn prog pari cocoa unary usual identificateur
 gen input_lexer tinymt32 first quater kadd k_csdk qrcodegen"
O=$WORK/build/obj/giac; mkdir -p $O
todo=""
for n in $GOBJS; do
    f=$GS/$n.cc; [ -f $f ] || f=$GS/$n.c
    [ $O/$n.o -nt $f ] || todo="$todo $f"
done
# one compile per call, so xargs can run them side by side
cat > $WORK/build/giac_cc.sh <<EOS
#!/bin/sh
n=\$(basename "\$1"); n=\${n%.cc}; n=\${n%.c}
$T-g++ $GCXX -c "\$1" -o $O/\$n.o 2> $O/\$n.log || echo "FAILED \$1 (\$(head -3 $O/\$n.log))"
EOS
chmod +x $WORK/build/giac_cc.sh
if [ -n "$todo" ]; then
    echo "building Giac ($(echo $todo | wc -w | tr -d ' ') files)"
    fails=$(echo $todo | tr ' ' '\n' | xargs -P$JOBS -n1 $WORK/build/giac_cc.sh)
    [ -z "$fails" ] || { echo "$fails"; exit 1; }
fi
# KhiCAS's interface (kdisplay.cc) is left out: cas_kshim.cc answers what
# Giac asks of it. Its turtle commands are kept, cut out of it as they are,
# between the two lines below (checked: a new KhiCAS that moves them fails
# here, not at run time).
KD=$GS/kdisplay.cc
t0=$(grep -n '^  int turtle_speed=0;' $KD | cut -d: -f1)
t1=$(grep -n 'define_unary_function_ptr5( at_dessine_tortue' $KD | cut -d: -f1)
[ -n "$t0" ] && [ -n "$t1" ] || { echo "kdisplay.cc: the turtle's block was not found"; exit 1; }
{ printf '// cut out of KhiCAS kdisplay.cc (GPL-3.0) by build_engine.sh\n'
  printf '#include "config.h"\n#include "giacPCH.h"\n#include "kdisplay.h"\n'
  printf 'using namespace std;\nusing namespace giac;\nnamespace giac {\n'
  sed -n "${t0},$((t1 + 1))p" $KD
  printf '}\n'; } > $WORK/build/cas_turtle.cc
$T-g++ $GCXX -c $WORK/build/cas_turtle.cc -o $O/cas_turtle.o
$T-g++ $GCXX -c $HERE/cas_kshim.cc -o $O/cas_kshim.o
rm -f $O/kdisplay.o
# the bridge between the app's C and Giac's C++
$T-g++ $GCXX -c $HERE/cas_engine.cc -o $O/cas_engine.o

# --- one archive -----------------------------------------------------------
# Every allocation of the engine goes to the app's arena (cas_rt.c): the
# arena is freed whole when the app closes, so nothing Giac keeps in its
# globals can leak. Renaming the symbols here is what -Wl,--wrap would do,
# but project_so() builds its link line by hand and takes no flags.
echo "merging libcasengine.a"
M=$WORK/build/merge; rm -rf $M; mkdir -p $M
cp $O/*.o $M/
for a in libgmp libmpfr libmpfi libstdc++ libsupc++ libopenlibm libsfp; do
    mkdir -p $M/$a && (cd $M/$a && $T-ar x $P/lib/$a.a)
    for f in $M/$a/*.o; do mv "$f" "$M/${a}_$(basename "$f")"; done
    rmdir $M/$a
done
rm -f $OUT/libcasengine.a
# A native Windows process cannot accept the 1,500-object command line.
# Relative names also avoid MSYS path conversion inside the response file.
(cd "$M" && printf '"%s"\n' *.o > objects.rsp &&
 $T-ar rcs "$OUT/libcasengine.a" @objects.rsp)
$T-objcopy \
    --redefine-sym malloc=cas_malloc --redefine-sym free=cas_free \
    --redefine-sym calloc=cas_calloc --redefine-sym realloc=cas_realloc \
    --redefine-sym memalign=cas_memalign --redefine-sym aligned_alloc=cas_aligned_alloc \
    --redefine-sym posix_memalign=cas_posix_memalign \
    $OUT/libcasengine.a

# The end of .init_array: the last object of the link (cas_rt.c has the
# start). Its own archive, so it lands after everything else.
$T-gcc $CF -c $HERE/cas_end.c -o $WORK/build/cas_end.o
rm -f $OUT/libcasend.a
$T-ar rcs $OUT/libcasend.a $WORK/build/cas_end.o

# the export list, as an input file the linker reads as a script
cp $HERE/cas_exports.ld $OUT/cas_exports.o

ls -la $OUT/*.a

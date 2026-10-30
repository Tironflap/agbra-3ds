#!/bin/sh
# Re-exec without CR if Windows saved this file as CRLF (breaks line continuations).
if grep -q "$(printf '\r')" "$0" 2>/dev/null; then
	sed 's/\r$//' "$0" > "$0.unix"
	chmod +x "$0.unix"
	exec /bin/sh "$0.unix" "$@"
fi

# Build libmgba for Nintendo 3DS → third_party/prefix
# Windows: MSYS2 only (C:\devkitPro\msys2\msys2.exe).
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
PREFIX="$ROOT/third_party/prefix"
MGBA_DIR="$ROOT/third_party/mgba"

if [ -z "${DEVKITPRO:-}" ]; then
	if [ -d /opt/devkitpro ]; then
		DEVKITPRO=/opt/devkitpro
	elif [ -d /c/devkitPro ]; then
		DEVKITPRO=/c/devkitPro
	fi
	export DEVKITPRO
fi
if [ -z "${DEVKITARM:-}" ] && [ -n "${DEVKITPRO:-}" ]; then
	export DEVKITARM="$DEVKITPRO/devkitARM"
fi

if [ -z "${DEVKITARM:-}" ] || [ -z "${DEVKITPRO:-}" ]; then
	echo "DEVKITPRO / DEVKITARM must be set."
	echo "  export DEVKITPRO=/opt/devkitpro"
	echo "  export DEVKITARM=\$DEVKITPRO/devkitARM"
	exit 1
fi

export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"

echo "DEVKITPRO=$DEVKITPRO"
echo "DEVKITARM=$DEVKITARM"
command -v arm-none-eabi-gcc >/dev/null 2>&1 || { echo "arm-none-eabi-gcc missing. pacman -S 3ds-dev"; exit 1; }
command -v git >/dev/null 2>&1 || { echo "git missing. pacman -S git"; exit 1; }

# devkitPro's /usr/bin/cmake often exits with no output (missing msys-jsoncpp DLL).
# Use the first cmake that actually prints a version. Kitware's Windows cmake is fine.
CMAKE_BIN=""
for cand in \
	"${CMAKE:-}" \
	"/c/Program Files/CMake/bin/cmake.exe" \
	"/c/Program Files (x86)/CMake/bin/cmake.exe" \
	"cmake"
do
	[ -n "$cand" ] || continue
	if "$cand" --version >/tmp/agbra-cmake-ver.txt 2>/tmp/agbra-cmake-err.txt; then
		CMAKE_BIN="$cand"
		break
	fi
done
if [ -z "$CMAKE_BIN" ]; then
	echo "No working cmake."
	echo "The MSYS one printed:"
	cat /tmp/agbra-cmake-err.txt 2>/dev/null || true
	echo
	echo "Install the normal Windows CMake from https://cmake.org/download/"
	echo "(Windows x64 Installer). Leave the default path. Then re-run this script."
	exit 1
fi
echo "compiler: $(arm-none-eabi-gcc --version | head -n 1)"
echo "cmake: $CMAKE_BIN"
head -n 1 /tmp/agbra-cmake-ver.txt

JOBS=$(nproc 2>/dev/null || echo 4)
GEN="Unix Makefiles"
case "$(uname -s 2>/dev/null || echo unknown)" in
	MINGW*|MSYS*|CYGWIN*) GEN="MSYS Makefiles" ;;
esac

mkdir -p "$ROOT/third_party"
if [ ! -d "$MGBA_DIR/.git" ]; then
	echo "==> Cloning mGBA"
	git clone --depth 1 https://github.com/mgba-emu/mgba.git "$MGBA_DIR"
fi

TOOLCHAIN="$MGBA_DIR/src/platform/3ds/CMakeToolchain.txt"
[ -f "$TOOLCHAIN" ] || { echo "missing $TOOLCHAIN"; exit 1; }

# The stock 3DS rules open the SD archive from userAppInit and mmap files.
# Both crash the fs module when this core is linked into another homebrew.
python3 - "$MGBA_DIR/src/platform/3ds/CMakeLists.txt" << 'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
t = p.read_text()
t = t.replace(
    "set(OS_DEFINES _GNU_SOURCE COLOR_16_BIT COLOR_5_6_5 FIXED_ROM_BUFFER IOAPI_NO_64)",
    "set(OS_DEFINES _GNU_SOURCE COLOR_16_BIT COLOR_5_6_5 IOAPI_NO_64 ENABLE_VFS_FILE)",
)
t = t.replace(
"""set(OS_SRC
	${PROJECT_SOURCE_DIR}/src/util/memory.c
	${CMAKE_CURRENT_SOURCE_DIR}/3ds-vfs.c
	${CMAKE_CURRENT_SOURCE_DIR}/ctru-heap.c
	${CMAKE_CURRENT_SOURCE_DIR}/socket.c
	PARENT_SCOPE)""",
"""set(OS_SRC
	${PROJECT_SOURCE_DIR}/src/util/memory.c
	${CMAKE_CURRENT_SOURCE_DIR}/socket.c
	PARENT_SCOPE)""",
)
old = """if(ENABLE_VFS_3DS)
	list(APPEND OS_DEFINES ENABLE_VFS_3DS)
else()
	list(APPEND OS_DEFINES ENABLE_VFS_FD)
	list(APPEND VFS_SRC ${PROJECT_SOURCE_DIR}/src/util/vfs/vfs-fd.c ${PROJECT_SOURCE_DIR}/src/util/vfs/vfs-dirent.c)
endif()"""
new = """list(APPEND VFS_SRC ${PROJECT_SOURCE_DIR}/src/util/vfs/vfs-file.c ${PROJECT_SOURCE_DIR}/src/util/vfs/vfs-dirent.c)"""
if "ENABLE_VFS_FILE" in t and "ctru-heap.c" not in t and "vfs-fd.c" not in t:
    print("already patched", p)
    raise SystemExit(0)
if old not in t:
    raise SystemExit("3DS CMakeLists.txt layout changed; not patching")
t = t.replace(old, new)
p.write_text(t)
print("patched", p)
PY

BUILD="$MGBA_DIR/build-3ds"
rm -rf "$BUILD"
mkdir -p "$BUILD"

# One option per line so a CRLF editor cannot break backslash continuations.
cat > "$BUILD/agbra-flags.cmake" << 'EOF'
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_QT OFF CACHE BOOL "" FORCE)
set(BUILD_SDL OFF CACHE BOOL "" FORCE)
set(BUILD_GL OFF CACHE BOOL "" FORCE)
set(BUILD_GLES2 OFF CACHE BOOL "" FORCE)
set(BUILD_GLES3 OFF CACHE BOOL "" FORCE)
set(USE_DEBUGGERS OFF CACHE BOOL "" FORCE)
set(USE_EDITLINE OFF CACHE BOOL "" FORCE)
set(USE_FFMPEG OFF CACHE BOOL "" FORCE)
set(USE_PNG OFF CACHE BOOL "" FORCE)
set(USE_LIBZIP OFF CACHE BOOL "" FORCE)
set(USE_MINIZIP OFF CACHE BOOL "" FORCE)
set(USE_SQLITE3 OFF CACHE BOOL "" FORCE)
set(USE_ELF OFF CACHE BOOL "" FORCE)
set(USE_LUA OFF CACHE BOOL "" FORCE)
set(USE_GDB_STUB OFF CACHE BOOL "" FORCE)
set(USE_DISCORD_RPC OFF CACHE BOOL "" FORCE)
set(USE_JSON_C OFF CACHE BOOL "" FORCE)
set(USE_FREETYPE OFF CACHE BOOL "" FORCE)
set(USE_LZMA OFF CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(ENABLE_DEBUGGERS OFF CACHE BOOL "" FORCE)
set(ENABLE_GDB_STUB OFF CACHE BOOL "" FORCE)
set(M_CORE_GB ON CACHE BOOL "" FORCE)
set(M_CORE_GBA ON CACHE BOOL "" FORCE)
set(USE_ZLIB ON CACHE BOOL "" FORCE)
EOF

echo "==> Configuring libmgba (generator: $GEN)"
"$CMAKE_BIN" -S "$MGBA_DIR" -B "$BUILD" -C "$BUILD/agbra-flags.cmake" -G "$GEN" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=/usr/bin/make
echo "==> Building libmgba (-j$JOBS)"
if ! "$CMAKE_BIN" --build "$BUILD" --target mgba -j "$JOBS"; then
	echo "==> retry target mgba-static"
	"$CMAKE_BIN" --build "$BUILD" --target mgba-static -j "$JOBS"
fi

LIB=""
for cand in "$BUILD/libmgba.a" "$BUILD/libmgba-static.a"; do
	if [ -f "$cand" ]; then LIB="$cand"; break; fi
done
if [ -z "$LIB" ]; then
	LIB=$(find "$BUILD" -name 'libmgba*.a' | head -n 1 || true)
fi
if [ -z "$LIB" ] || [ ! -f "$LIB" ]; then
	echo "ERROR: libmgba.a was not produced."
	exit 1
fi

echo "==> Installing to $PREFIX"
STAGE="$ROOT/third_party/prefix.new"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib" "$STAGE/include"
cp "$LIB" "$STAGE/lib/libmgba.a"
cp -R "$MGBA_DIR/include/mgba" "$STAGE/include/"
cp -R "$MGBA_DIR/include/mgba-util" "$STAGE/include/"
if [ -d "$BUILD/include" ]; then
	cp -R "$BUILD/include/." "$STAGE/include/"
fi
[ -f "$STAGE/include/mgba/core/core.h" ] || { echo "ERROR: core.h missing"; exit 1; }
rm -rf "$PREFIX"
mv "$STAGE" "$PREFIX"
echo "libmgba ready: $PREFIX/lib/libmgba.a"
echo "Now: make -j$JOBS"

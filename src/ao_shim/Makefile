# ao_shim Makefile — local build only, Debian cross-gcc, -nostdlib.
#
# The camera runs uClibc, not glibc, so the shipped .so must NOT link
# any libc. Build with -nostdlib and raw syscalls only (see ao_shim.c).
# `make check` verifies: ARM 32-bit shared object, exports
# ioctl/open/open64/openat/close, NO NEEDED entries, exactly ONE
# undefined symbol (__errno_location — H1 fix, see ao_shim.c).
#
# NOTE: the qemu-arm mock test (mock_test.c, run_qemu_test.sh) is NOT part
# of this repo; it lives in the dev workdir where the shim was first built.
# A clean rebuild here must reproduce the verified sha256
# a13150434b65f8561f3230458199d6fea3fdb244365c93289bb87d5e22d330ef
# (logging-shim build + review fixes, 2026-11-17; previous hash beb11047… was first logging build).

CC      := arm-linux-gnueabi-gcc
READELF := arm-linux-gnueabi-readelf
NM      := arm-linux-gnueabi-nm

CFLAGS := -nostdlib -shared -fPIC -ffreestanding -fno-builtin \
          -fno-stack-protector -Os -march=armv7-a -marm -mfloat-abi=soft \
          -Wall -Wextra

.PHONY: all check clean

all: libao_shim.so

libao_shim.so: ao_shim.c
	$(CC) $(CFLAGS) -o $@ $<

check: libao_shim.so
	@echo "== readelf -h (must say ARM, DYN shared object) =="
	$(READELF) -h libao_shim.so | grep -E 'Class|Type|Machine'
	@echo "== readelf -d (must show NO NEEDED entries) =="
	$(READELF) -d libao_shim.so
	@echo "== dynamic symbols: must export ioctl/open/open64/openat/close =="
	$(READELF) --dyn-syms -W libao_shim.so | grep -E ' (ioctl|open|open64|openat|close)$$' || true
	@echo "== undefined symbols (must be exactly __errno_location) =="
	$(NM) -u libao_shim.so
	@test "$$( $(NM) -u libao_shim.so | awk '{print $$2}' | tr '\n' ' ' )" = "__errno_location "
	@echo "== check OK =="

clean:
	rm -f libao_shim.so

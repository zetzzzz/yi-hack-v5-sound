OBJECTS = pcmvol.o
all: pcmvol

pcmvol.o: pcmvol.c $(HEADERS)
	$(CC) -c $< -fPIC -Os -o $@

pcmvol: $(OBJECTS)
	$(CC) $(OBJECTS) -lm -fPIC -Os -static -o $@
	$(STRIP) $@
# NOTE (yi-hack-v5 port): -static added: the Debian glibc toolchain is
# used (hisiv300 is unobtainable) and the camera runs uClibc, so a
# dynamically linked binary would not run on the camera.

.PHONY: clean

clean:
	rm -f pcmvol
	rm -f $(OBJECTS)

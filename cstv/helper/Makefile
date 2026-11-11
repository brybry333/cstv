# cstvd — CS:TV host helper (runs on the host, outside Steam's container)
# x86-64-v3 (AVX2, any CPU since ~2015) lets the compiler vectorise the DXT1 encoder: about 2x faster
CFLAGS := -O3 -march=x86-64-v3 -g -Wall -Wextra -Wno-unused-parameter -I../common -I../third_party/stb -ffile-prefix-map=$(abspath ..)=.
cstvd: cstvd.c cstv_os.h dxt1_fast.h ../common/cstv_proto.h glibc_compat.o
	$(CC) $(CFLAGS) -o $@ cstvd.c glibc_compat.o $$(pkg-config --cflags --libs mpv) -lpthread -lm
# plain C99 on purpose: see the file
glibc_compat.o: glibc_compat.c
	$(CC) -std=c99 -O2 -c -o $@ glibc_compat.c
clean:
	rm -f cstvd glibc_compat.o

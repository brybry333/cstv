# cstvd — CS:TV host helper (runs on the host, outside Steam's container)
# x86-64-v3 (AVX2, any CPU since ~2015) lets the compiler vectorise the DXT1 encoder: about 2x faster
CFLAGS := -O3 -march=x86-64-v3 -g -Wall -Wextra -Wno-unused-parameter -I../common -I../third_party/stb
cstvd: cstvd.c cstv_os.h dxt1_fast.h ../common/cstv_proto.h
	$(CC) $(CFLAGS) -o $@ cstvd.c $$(pkg-config --cflags --libs mpv) -lpthread -lm
clean:
	rm -f cstvd

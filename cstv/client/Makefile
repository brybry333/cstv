# libcstv.so — CS:TV client preload. 64-bit, loaded into cstrike_linux64 via LD_PRELOAD.
# Static libstdc++/libgcc (Steam runtime has an older one) and every symbol hidden, so our C++
# runtime never interposes on the game's.
#   make                       build libcstv.so
#   make install GAME=<dir>    copy the materials into <dir>/cstrike/custom/cstv (default: Steam's CS:S)
SDK      := ../third_party/source-sdk-2013/src
GAME     ?= $(HOME)/.local/share/Steam/steamapps/common/Counter-Strike Source
CXX      ?= g++
CXXFLAGS := -O2 -g -Wall -Wno-unused-parameter -Wno-unknown-pragmas -std=c++17 -fno-strict-aliasing -fPIC -m64 \
            -DPOSIX -DLINUX -D_LINUX -DGNUC -DCOMPILER_GCC -DPLATFORM_64BITS -DX64BITS -DNO_MALLOC_OVERRIDE -DCLIENT_DLL \
            -I$(SDK)/public -I$(SDK)/public/tier0 -I$(SDK)/public/tier1 -I$(SDK)/common -I../third_party/stb -I../common \
            -fvisibility=hidden -fvisibility-inlines-hidden -fno-stack-protector
LDFLAGS  := -shared -static-libstdc++ -static-libgcc -Wl,--no-undefined -Wl,--exclude-libs,ALL \
            -Wl,--version-script=cstv.ver -ldl -lpthread
SRCS     := cstv_client.cpp cstv_gui.cpp cstv_capture.cpp

libcstv.so: $(SRCS) cstv_gui.h cstv_capture.h cstv_place.h cstv_platform.h cstv.ver ../common/cstv_proto.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LDFLAGS)

install:
	install -d "$(GAME)/cstrike/custom/cstv/materials/cstv"
	install -m644 ../assets/materials/cstv/*.vmt ../assets/materials/cstv/*.vtf "$(GAME)/cstrike/custom/cstv/materials/cstv/"

clean:
	rm -f libcstv.so

.PHONY: install clean

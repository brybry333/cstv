// Keeps libcstv.so loadable on glibc 2.31 (Steam's runtime) when built against a newer one.
// Adapted from bhoptools (GPL-3.0, github.com/ddyfad/bhoptools).
#define HIDDEN extern "C" __attribute__((visibility("hidden")))

typedef __builtin_va_list va_list_t;

extern "C" {
long strtol(const char*, char**, int);
unsigned long strtoul(const char*, char**, int);
long long strtoll(const char*, char**, int);
unsigned long long strtoull(const char*, char**, int);
int vsscanf(const char*, const char*, va_list_t);
int getentropy(void*, unsigned long);

void* compat_dlopen(const char*, int);
void* compat_dlsym(void*, const char*);
int   compat_dlclose(void*);
int   compat_dladdr(const void*, void*);
int   compat_pthread_create(unsigned long*, const void*, void* (*)(void*), void*);
int   compat_pthread_once(int*, void (*)(void));
int   compat_pthread_key_create(unsigned int*, void (*)(void*));
int   compat_xstat(int, const char*, void*);
int   compat_fxstat(int, int, void*);
int   compat_lxstat(int, const char*, void*);
}

__asm__(".symver compat_dlopen,dlopen@GLIBC_2.2.5");
__asm__(".symver compat_dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver compat_dlclose,dlclose@GLIBC_2.2.5");
__asm__(".symver compat_dladdr,dladdr@GLIBC_2.2.5");
__asm__(".symver compat_pthread_create,pthread_create@GLIBC_2.2.5");
__asm__(".symver compat_pthread_once,pthread_once@GLIBC_2.2.5");
__asm__(".symver compat_pthread_key_create,pthread_key_create@GLIBC_2.2.5");
__asm__(".symver compat_xstat,__xstat@GLIBC_2.2.5");
__asm__(".symver compat_fxstat,__fxstat@GLIBC_2.2.5");
__asm__(".symver compat_lxstat,__lxstat@GLIBC_2.2.5");

// C23 only adds "0b" prefixes and the like; nothing here parses those.
HIDDEN long __isoc23_strtol(const char* s, char** e, int b) { return strtol(s, e, b); }
HIDDEN unsigned long __isoc23_strtoul(const char* s, char** e, int b) { return strtoul(s, e, b); }
HIDDEN long long __isoc23_strtoll(const char* s, char** e, int b) { return strtoll(s, e, b); }
HIDDEN unsigned long long __isoc23_strtoull(const char* s, char** e, int b) { return strtoull(s, e, b); }
HIDDEN int __isoc23_vsscanf(const char* s, const char* f, va_list_t ap) { return vsscanf(s, f, ap); }
HIDDEN int __isoc23_sscanf(const char* s, const char* f, ...)
{
	va_list_t ap;
	__builtin_va_start(ap, f);
	const int n = vsscanf(s, f, ap);
	__builtin_va_end(ap);
	return n;
}

HIDDEN void* dlopen(const char* f, int m) { return compat_dlopen(f, m); }
HIDDEN void* dlsym(void* h, const char* s) { return compat_dlsym(h, s); }
HIDDEN int dlclose(void* h) { return compat_dlclose(h); }
HIDDEN int dladdr(const void* a, void* i) { return compat_dladdr(a, i); }
HIDDEN int pthread_create(unsigned long* t, const void* a, void* (*f)(void*), void* p) { return compat_pthread_create(t, a, f, p); }
HIDDEN int pthread_once(int* o, void (*f)(void)) { return compat_pthread_once(o, f); }
// The static libstdc++'s weak reference that tells it threads are in use.
HIDDEN int __pthread_key_create(unsigned int* k, void (*d)(void*)) { return compat_pthread_key_create(k, d); }

// _STAT_VER is 1 on x86_64.
HIDDEN int stat(const char* p, void* st) { return compat_xstat(1, p, st); }
HIDDEN int fstat(int fd, void* st) { return compat_fxstat(1, fd, st); }
HIDDEN int lstat(const char* p, void* st) { return compat_lxstat(1, p, st); }

// 0 means "maybe threaded": libstdc++ then always uses atomic refcounts.
extern "C" { __attribute__((visibility("hidden"))) char __libc_single_threaded = 0; }

HIDDEN unsigned int arc4random(void)
{
	unsigned int v = 0;
	getentropy(&v, sizeof(v));
	return v;
}

// glibc 2.35's lookup, rebuilt on dl_iterate_phdr; libgcc only reads dlfo_eh_frame and the map range.
struct DlFindObject {
	unsigned long long flags;
	void* map_start;
	void* map_end;
	void* link_map;
	void* eh_frame;
	unsigned long long reserved[7];
};

// <link.h> would pull in <dlfcn.h>; these match its x86_64 layouts.
struct Elf64Phdr {
	unsigned int p_type, p_flags;
	unsigned long p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
struct DlPhdrInfo {
	unsigned long dlpi_addr;
	const char* dlpi_name;
	const Elf64Phdr* dlpi_phdr;
	unsigned short dlpi_phnum;
};
extern "C" int dl_iterate_phdr(int (*)(DlPhdrInfo*, unsigned long, void*), void*);
static const unsigned int PT_LOAD_ = 1, PT_GNU_EH_FRAME_ = 0x6474e550;

struct FindCtx { unsigned long pc; DlFindObject* out; bool found; };

static int FindObjectCb(DlPhdrInfo* info, unsigned long, void* data)
{
	FindCtx* ctx = (FindCtx*)data;
	unsigned long lo = ~0ul, hi = 0;
	bool inside = false;
	const void* eh = nullptr;
	for (int i = 0; i < info->dlpi_phnum; ++i) {
		const Elf64Phdr& ph = info->dlpi_phdr[i];
		const unsigned long start = info->dlpi_addr + ph.p_vaddr;
		if (ph.p_type == PT_LOAD_) {
			if (start < lo) lo = start;
			if (start + ph.p_memsz > hi) hi = start + ph.p_memsz;
			if (ctx->pc >= start && ctx->pc < start + ph.p_memsz) inside = true;
		} else if (ph.p_type == PT_GNU_EH_FRAME_) {
			eh = (const void*)start;
		}
	}
	if (!inside) return 0;
	ctx->out->flags = 0;
	ctx->out->map_start = (void*)lo;
	ctx->out->map_end = (void*)hi;
	ctx->out->link_map = nullptr;
	ctx->out->eh_frame = (void*)eh;
	ctx->found = true;
	return 1;
}

HIDDEN int _dl_find_object(void* pc, DlFindObject* out)
{
	FindCtx ctx = { (unsigned long)pc, out, false };
	dl_iterate_phdr(FindObjectCb, &ctx);
	return ctx.found ? 0 : -1;
}

// ---------------------------------------------------------------------------
// FishCrack :: native il2cpp driver
//
// 目标：实现「免登陆进游戏」。
//
// 原理（全部结论来自对 Logic.dll 的反编译，见 work/README.md 第 7 节）：
//   热更层 UI 类 KunpoUserSystem_FGUI.UserSystemWindow 有一个静态委托属性
//
//       public static Action Complete { get; set; }
//
//   登录流程 UserSystemWindow.ShowLoginOption() 的第一步就是
//
//       Complete = delegate { UniTaskExtensions.Forget<int>(Login()); };
//
//   之后它 await AutoLoginAsync()（用本地缓存的微信 token 自动登录），
//   失败才显示「微信登录」按钮等用户点。也就是说：
//   **只要我们在它等待用户点按钮的那段时间里把这个 Complete 委托取出来调用一次，
//     游戏就会直接走 Login() —— 完全跳过微信授权。**
//
//   问题在于：UserSystemWindow / Login() 都在热更 DLL 里，是 HybridCLR 解释执行的
//   C# 方法，纯 Java 的 Xposed hook 摸不到它（libil2cpp 里没有对应的 JNI 方法）。
//
//   解法：libil2cpp.so **导出了完整的 il2cpp C API**（实测 237 个 il2cpp_* 符号，
//   il2cpp_domain_get / il2cpp_domain_get_assemblies / il2cpp_class_from_name /
//   il2cpp_runtime_invoke / il2cpp_thread_attach ... 全在）。于是本 .so 通过
//   dlopen+dlsym 拿到这套 API，在运行时：
//     1. 遍历 il2cpp 的所有 image，找到热更类 UserSystemWindow；
//     2. 读它的静态属性 Complete（get_Complete / <Complete>k__BackingField）；
//     3. 取 Action.Invoke 并 il2cpp_runtime_invoke 它 → Login() 被调用。
//
//   调用必须发生在 Unity 主线程（Login() 内部会构造 UnityWebRequest 等 Unity 对象），
//   所以本 .so 只提供「无状态的一次尝试」函数，由 Java 层用
//   Handler(Looper.getMainLooper()) 轮询调用 —— Java 主线程在 Unity Android 上
//   就是 Unity 主线程。
// ---------------------------------------------------------------------------

// 本模块刻意不依赖 C++ 标准库（build.gradle 传 -DANDROID_STL=none，NDK 会加
// -nostdinc++ 屏蔽 C++ 头，好处是最终 .so 不链接 libc++_static）：
// 所以这里只能用 C 头，不能写 <cstddef>/<cstdint>/<cstdio>/<cstring>，
// 对应的 C 写法是 <stddef.h>/<stdint.h>/<stdio.h>/<string.h>，
// 且所有符号都是全局命名空间（snprintf / memcpy / strcmp ...，不能带 std::）。
#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   // malloc / free（nativePrefsCall 里给返回串分配缓冲）
#include <elf.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define LOGTAG "FishCrack-Native"
#define NLOGI(...) __android_log_print(ANDROID_LOG_INFO, LOGTAG, __VA_ARGS__)
#define NLOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOGTAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// il2cpp C API 的最小前置声明（只需要不透明指针 + 用到的函数签名）
// ---------------------------------------------------------------------------
struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct Il2CppObject;
struct MethodInfo;
struct FieldInfo;

typedef Il2CppDomain* (*pfn_domain_get)();
typedef const Il2CppAssembly** (*pfn_domain_get_assemblies)(const Il2CppDomain*, size_t*);
typedef Il2CppImage* (*pfn_assembly_get_image)(const Il2CppAssembly*);
typedef const char* (*pfn_image_get_name)(const Il2CppImage*);
typedef Il2CppClass* (*pfn_class_from_name)(const Il2CppImage*, const char*, const char*);
// 注意：返回的是 Il2CppImage*（不是 Il2CppClass*）—— 写错会导致
// “incompatible pointer types assigning to 'Il2CppImage *' from 'Il2CppClass *'”。
typedef Il2CppImage* (*pfn_class_get_image)(Il2CppClass*);
typedef const char* (*pfn_class_get_name)(Il2CppClass*);
typedef const char* (*pfn_class_get_namespace)(Il2CppClass*);
typedef FieldInfo* (*pfn_class_get_field_from_name)(Il2CppClass*, const char*);
typedef size_t (*pfn_field_get_offset)(FieldInfo*);
typedef void* (*pfn_class_get_static_field_data)(Il2CppClass*);
typedef Il2CppClass* (*pfn_object_get_class)(Il2CppObject*);
// il2cpp 字符串 API：让模块能直接调游戏自己的加解密函数
// （存档的 AES key 已换，旧 key 解不开，只能借用游戏自身的 DecryptString）
typedef Il2CppObject* (*pfn_string_new)(const char*);
typedef uint16_t* (*pfn_string_chars)(Il2CppObject*);
typedef int32_t (*pfn_string_length)(Il2CppObject*);
typedef const MethodInfo* (*pfn_class_get_method_from_name)(Il2CppClass*, const char*, int);
typedef Il2CppObject* (*pfn_runtime_invoke)(const MethodInfo*, void*, void**, Il2CppObject**);
typedef void* (*pfn_thread_attach)(const Il2CppDomain*);
typedef void (*pfn_gc_foreach_heap)(void (*)(void*, void*), void*);
typedef Il2CppClass* (*pfn_class_get_parent)(Il2CppClass*);
// void il2cpp_class_for_each(void (*klassCallback)(Il2CppClass*, void*), void* userData)
typedef void (*pfn_class_for_each)(void (*)(Il2CppClass*, void*), void*);
// 字段枚举。il2cpp_class_get_field_from_name 在新版 il2cpp 已被移除（真机
// dlsym 拿不到），所以只能 class_get_fields + field_get_name 手工匹配名字。
typedef FieldInfo* (*pfn_class_get_fields)(Il2CppClass*, void**);
typedef const char* (*pfn_field_get_name)(FieldInfo*);

struct Il2CppApi {
    void* handle;
    pfn_domain_get domain_get;
    pfn_domain_get_assemblies domain_get_assemblies;
    pfn_assembly_get_image assembly_get_image;
    pfn_image_get_name image_get_name;
    pfn_class_from_name class_from_name;
    pfn_class_get_image class_get_image;
    pfn_class_get_name class_get_name;
    pfn_class_get_namespace class_get_namespace;
    pfn_class_get_field_from_name class_get_field_from_name;
    pfn_field_get_offset field_get_offset;
    pfn_class_get_static_field_data class_get_static_field_data;
    pfn_object_get_class object_get_class;
    pfn_string_new string_new;
    pfn_string_chars string_chars;
    pfn_string_length string_length;
    pfn_class_get_method_from_name class_get_method_from_name;
    pfn_runtime_invoke runtime_invoke;
    pfn_thread_attach thread_attach;
    pfn_gc_foreach_heap gc_foreach_heap;
    pfn_class_get_parent class_get_parent;
    pfn_class_for_each class_for_each;
    pfn_class_get_fields class_get_fields;
    pfn_field_get_name field_get_name;
};

static Il2CppApi g_api;
static bool g_resolved = false;

// 命中后的目标类缓存：GC 堆扫描很贵（20 万对象 × parent 链字符串比较），
// 一次找到就记住，避免每 400 ms 重扫把游戏主线程拖卡。
static Il2CppClass* g_cached_target = nullptr;
static int g_scan_ticks = 0;

// ---------------------------------------------------------------------------
// namespace 隔离的兼底方案
//
// 问题（真机日志坐实）：LSPosed 会把模块的 .so 装进一个隔离 namespace（日志
// 里的 `using isolated ns clns-10`），而游戏的 libil2cpp.so 在应用自己的
// namespace 里。于是 `dlopen("libil2cpp.so")` 必然失败 —— 哪怕
// /proc/self/maps 里明明映射了 6 段。
//
// 解法：完全绕开 dlopen/dlsym。从 /proc/self/maps 拿到 libil2cpp.so 的加载
// 基址，再自己读 ELF 的 .dynsym/.dynstr 算符号地址（ET_DYN 的 st_value 就是
// 相对基址的偏移）。namespace 再怎么隔离都拦不住直接读内存。
// ---------------------------------------------------------------------------

struct SoImage {
    void* base;
    char path[512];
};

static SoImage g_il2cpp_img;
static void* g_elf_map = nullptr;      // mmap 缓存（libil2cpp.so 很大，只映射一次）
static size_t g_elf_map_size = 0;

static bool find_so_in_maps(const char* needle, SoImage* out) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        return false;
    }
    char line[1024];
    bool got = false;
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, needle)) {
            continue;
        }
        unsigned long start = 0, off = 0;
        char perms[8] = {0};
        char path[512] = {0};
        if (sscanf(line, "%lx-%*lx %7s %lx %*s %*s %511s", &start, perms, &off, path) != 4) {
            continue;
        }
        if (path[0] != '/') {
            continue;
        }
        // 只认可执行段。
        // Unity 启动早期会先把 libil2cpp.so 映射成 r--p，稍后才 mprotect 成
        // r-xp。如果在这个窗口里就去调函数，取指会直接 SIGSEGV
        // (SEGV_ACCERR) 把游戏打崩（真机已复现：pc == fault addr，栈顶就在
        // il2cpp_domain_get 入口）。拿不到可执行段就当作“还没就绪”，
        // 交回上层 400ms 轮询继续等。
        if (perms[2] != 'x') {
            continue;
        }
        // 由可执行段反推加载基址。ELF 要求 p_vaddr % p_align == p_offset % p_align，
        // ARM64 上 libil2cpp.so 的 vaddr 与 offset 相同，start - off 就是基址。
        out->base = (void*)(start - off);
        strncpy(out->path, path, sizeof(out->path) - 1);
        got = true;
        break;
    }
    fclose(fp);
    return got;
}

static void* elf_lookup(const SoImage* img, const char* name) {
    if (!img->base || img->path[0] == '\0') {
        return nullptr;
    }
    if (!g_elf_map) {
        int fd = open(img->path, O_RDONLY);
        if (fd < 0) {
            NLOGE("open(%s) failed", img->path);
            return nullptr;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
            close(fd);
            return nullptr;
        }
        void* m = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (m == MAP_FAILED) {
            NLOGE("mmap(%s) failed", img->path);
            return nullptr;
        }
        g_elf_map = m;
        g_elf_map_size = (size_t)st.st_size;
    }

    const unsigned char* m = (const unsigned char*)g_elf_map;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)m;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_shoff == 0 || eh->e_shnum == 0) {
        return nullptr;
    }
    if ((size_t)eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) > g_elf_map_size) {
        return nullptr;
    }
    const Elf64_Shdr* sh = (const Elf64_Shdr*)(m + eh->e_shoff);
    const char* shstr = (const char*)(m + sh[eh->e_shstrndx].sh_offset);

    const Elf64_Shdr* dynsym = nullptr;
    const char* dynstr = nullptr;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_DYNSYM) {
            dynsym = &sh[i];
        } else if (sh[i].sh_name && strcmp(shstr + sh[i].sh_name, ".dynstr") == 0) {
            dynstr = (const char*)(m + sh[i].sh_offset);
        }
    }
    if (!dynsym || !dynstr) {
        NLOGE("elf_lookup: no .dynsym/.dynstr in %s", img->path);
        return nullptr;
    }

    const Elf64_Sym* syms = (const Elf64_Sym*)(m + dynsym->sh_offset);
    size_t count = (size_t)(dynsym->sh_size / sizeof(Elf64_Sym));
    for (size_t i = 0; i < count; i++) {
        const char* sn = dynstr + syms[i].st_name;
        if (strcmp(sn, name) == 0 && syms[i].st_value != 0) {
            return (char*)img->base + syms[i].st_value;
        }
    }
    return nullptr;
}

// 校验某个地址当前是否落在一个可执行映射里。
// 用于在真正调用 JIT/AOT 函数前确认代码段已经 mprotect 成可执行。
static bool is_executable_addr(const void* addr) {
    if (!addr) {
        return false;
    }
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        return false;
    }
    const unsigned long target = (unsigned long)(uintptr_t)addr;
    bool ok = false;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long s = 0, e = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) != 3) {
            continue;
        }
        if (target >= s && target < e) {
            ok = (perms[2] == 'x');
            break;
        }
    }
    fclose(fp);
    return ok;
}

static bool ensure_lib() {
    if (g_api.handle) {
        return true;
    }
    void* h = dlopen("libil2cpp.so", RTLD_NOW);
    if (!h) {
        h = dlopen("libil2cpp.so", RTLD_LAZY);
    }
    if (h) {
        g_api.handle = h;
        NLOGI("libil2cpp.so loaded via dlopen, handle=%p", h);
        return true;
    }
    NLOGI("dlopen(libil2cpp.so) failed (%s) -> 走 /proc/self/maps + ELF 手工解析",
          dlerror());
    if (find_so_in_maps("libil2cpp.so", &g_il2cpp_img)) {
        NLOGI("maps: libil2cpp.so base=%p path=%s", g_il2cpp_img.base, g_il2cpp_img.path);
        return true;
    }
    NLOGE("maps 里也找不到 libil2cpp.so");
    return false;
}

// 可选符号解析：handle → 手工 ELF 解析 → 全局默认，三级兜底。
//
// 坑（真机坐实）：走 maps/ELF 路径时 g_api.handle 恒为 NULL，而那些“只用于
// 诊断”的符号当初只写了 dlsym(g_api.handle, ...)，于是它们全部静默失败。
// 症状：why 恒为空（路径 B 根本进不去）、class_get_image 拿不到 image 名，
// 于是 AOT 空壳类与 HybridCLR 热更真类无法区分。
static void* resolve_opt(const char* name) {
    void* p = g_api.handle ? dlsym(g_api.handle, name) : nullptr;
    if (!p) {
        p = elf_lookup(&g_il2cpp_img, name);
    }
    if (!p) {
        p = dlsym(RTLD_DEFAULT, name);
    }
    return p;
}

static bool bind_one(void* slot, const char* name) {
    void* p = nullptr;
    if (g_api.handle) {
        p = dlsym(g_api.handle, name);
    }
    if (!p) {
        p = elf_lookup(&g_il2cpp_img, name);
    }
    if (!p) {
        p = dlsym(RTLD_DEFAULT, name);
    }
    if (!p) {
        NLOGE("resolve(%s) failed", name);
        return false;
    }
    // 函数指针与数据指针在 AArch64/ARM 上都是指针宽度，用 memcpy 避免
    // (void**)(fnptr*) 这种退化转换带来的 UB / 警告。
    memcpy(slot, &p, sizeof(void*));
    return true;
}

// 目标类（热更 DLL 里的登录 UI 类）
static const char* kTargetNs = "KunpoUserSystem_FGUI";
static const char* kTargetClass = "UserSystemWindow";
static const char* kCompleteGetter = "get_Complete";
static const char* kCompleteField = "<Complete>k__BackingField";

// UserSystemWindow.cs:460  protected static void CompleteInvoke() {
//                              Complete?.Invoke(); Complete = null; }
// 游戏自己用来宣告“登录成功”的入口，语义完整（含置空），优先用它。
static const char* kCompleteInvoke = "CompleteInvoke";

// ---------------------------------------------------------------------------
// 绑定（ensure_lib / bind_one 见文件上方：已改为 dlopen + maps/ELF 双路径）
// ---------------------------------------------------------------------------
#define BIND(field, sym)                                       \
    do {                                                       \
        if (!bind_one(&g_api.field, sym)) return false;         \
    } while (0)

static bool resolve_all() {
    if (g_resolved) {
        return true;
    }
    // 这几个是“硬需求”，少一个就干不了活
    BIND(domain_get, "il2cpp_domain_get");
    BIND(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    BIND(assembly_get_image, "il2cpp_assembly_get_image");
    BIND(image_get_name, "il2cpp_image_get_name");
    BIND(class_from_name, "il2cpp_class_from_name");
    BIND(class_get_method_from_name, "il2cpp_class_get_method_from_name");
    BIND(runtime_invoke, "il2cpp_runtime_invoke");
    BIND(object_get_class, "il2cpp_object_get_class");
    BIND(thread_attach, "il2cpp_thread_attach");
    BIND(field_get_offset, "il2cpp_field_get_offset");
    BIND(class_get_static_field_data, "il2cpp_class_get_static_field_data");

    // 下面几个只用于诊断 / 兜底，缺了也能跑。
    // 必须走 resolve_opt：maps/ELF 路径下 g_api.handle 是 NULL。
    g_api.class_get_image = (pfn_class_get_image)resolve_opt("il2cpp_class_get_image");
    g_api.class_get_name = (pfn_class_get_name)resolve_opt("il2cpp_class_get_name");
    g_api.class_get_namespace =
        (pfn_class_get_namespace)resolve_opt("il2cpp_class_get_namespace");
    g_api.class_get_field_from_name =
        (pfn_class_get_field_from_name)resolve_opt("il2cpp_class_get_field_from_name");
    g_api.gc_foreach_heap = (pfn_gc_foreach_heap)resolve_opt("il2cpp_gc_foreach_heap");
    g_api.class_get_parent = (pfn_class_get_parent)resolve_opt("il2cpp_class_get_parent");
    g_api.class_for_each = (pfn_class_for_each)resolve_opt("il2cpp_class_for_each");
    g_api.class_get_fields = (pfn_class_get_fields)resolve_opt("il2cpp_class_get_fields");
    g_api.field_get_name = (pfn_field_get_name)resolve_opt("il2cpp_field_get_name");
    // 字符串 API：DecryptString / EncryptString 需要它们构造入参、读返回值
    g_api.string_new = (pfn_string_new)resolve_opt("il2cpp_string_new");
    g_api.string_chars = (pfn_string_chars)resolve_opt("il2cpp_string_chars");
    g_api.string_length = (pfn_string_length)resolve_opt("il2cpp_string_length");
    NLOGI("optional symbols: image=%p name=%p fields=%p field_name=%p field_by_name=%p",
          (void*)g_api.class_get_image, (void*)g_api.class_get_name,
          (void*)g_api.class_get_fields, (void*)g_api.field_get_name,
          (void*)g_api.class_get_field_from_name);

    // 最后一道保险：确认关键函数所在页确实可执行。
    // 地址算对但页还是 r--p 时直接调用 = SIGSEGV，游戏当场闪退。
    void* dg = nullptr;
    memcpy(&dg, &g_api.domain_get, sizeof(void*));
    if (!is_executable_addr(dg)) {
        NLOGI("il2cpp 代码段尚未就绪（domain_get 不可执行），等下一次轮询");
        return false;
    }

    g_resolved = true;
    NLOGI("il2cpp api resolved");
    return true;
}

#undef BIND

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
static jstring mk(JNIEnv* env, const char* s) {
    return env->NewStringUTF(s ? s : "(null)");
}

// ---------------------------------------------------------------------------
// 调用游戏自身的 SimplePlayerPrefs.EncryptString / DecryptString
//
// 为什么要绕这一圈：1.1.8 的存档 AES 密钥已经不是旧的那把
// (:{j%6j?E:t#}G10mM%9hp5S=%}2,Y26C)，用旧 key 硬解只会得到乱码。最稳的办法
// 是直接让游戏的托管代码替我们加解密 —— 它手里一定是最新的 key。
// ---------------------------------------------------------------------------
static const char* kPrefsNs = "Arvin.PlayerPrefsUtilities";
static const char* kPrefsClass = "SimplePlayerPrefs";

// Il2CppString 布局（64 位）：klass(8) + monitor(8) + length(4) + chars[]
// 导出符号缺失时按内存布局兜底读取。
static int32_t str_len(Il2CppObject* s) {
    if (g_api.string_length) return g_api.string_length(s);
    return *(int32_t*)((char*)s + 16);
}
static const uint16_t* str_chars(Il2CppObject* s) {
    if (g_api.string_chars) return g_api.string_chars(s);
    return (const uint16_t*)((char*)s + 20);
}

// UTF-16LE -> UTF-8（含代理对）
static void utf16_to_utf8(const uint16_t* s, int32_t len, char* out, size_t outSize) {
    size_t o = 0;
    for (int32_t i = 0; i < len && o + 5 < outSize; ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len) {
            const uint32_t lo = s[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = '\0';
}

// 按命名空间 + 类名遍历所有 assembly 的 image 找类（不限于 UserSystemWindow）
static Il2CppClass* find_class_ns_name(Il2CppDomain* dom, const char* ns, const char* name) {
    if (!dom || !g_api.domain_get_assemblies || !g_api.assembly_get_image ||
        !g_api.class_from_name) {
        return nullptr;
    }
    size_t n = 0;
    const Il2CppAssembly** asms = g_api.domain_get_assemblies(dom, &n);
    if (!asms) return nullptr;
    for (size_t i = 0; i < n; ++i) {
        // il2cpp_assembly_get_image 接受的是 Il2CppAssembly*（不是 domain+index）
        Il2CppImage* img = g_api.assembly_get_image(asms[i]);
        if (!img) continue;
        Il2CppClass* k = g_api.class_from_name(img, ns, name);
        if (k) {
            NLOGI("found %s.%s in image %s", ns, name,
                  g_api.image_get_name ? g_api.image_get_name(img) : "?");
            return k;
        }
    }
    return nullptr;
}

/**
 * decrypt=true  -> DecryptString(arg)  返回明文
 * decrypt=false -> EncryptString(arg)  返回密文
 * 成功返回托管字符串对象，失败返回 nullptr 并写 err。
 */
static Il2CppObject* prefs_call(bool decrypt, const char* argUtf8, char* err, size_t errSize) {
    if (err && errSize) err[0] = '\0';
    if (!g_api.string_new || !g_api.class_from_name || !g_api.class_get_method_from_name ||
        !g_api.runtime_invoke || !g_api.domain_get) {
        snprintf(err, errSize, "核心符号缺失（string_new=%p）", (void*)g_api.string_new);
        return nullptr;
    }
    Il2CppDomain* dom = g_api.domain_get();
    if (!dom) {
        snprintf(err, errSize, "il2cpp domain 未就绪");
        return nullptr;
    }
    g_api.thread_attach(dom);

    Il2CppClass* klass = find_class_ns_name(dom, kPrefsNs, kPrefsClass);
    if (!klass) {
        snprintf(err, errSize, "找不到类 %s.%s", kPrefsNs, kPrefsClass);
        return nullptr;
    }
    const char* mname = decrypt ? "DecryptString" : "EncryptString";
    const MethodInfo* m = g_api.class_get_method_from_name(klass, mname, 1);
    if (!m) {
        snprintf(err, errSize, "找不到方法 %s(string)", mname);
        return nullptr;
    }

    Il2CppObject* arg = g_api.string_new(argUtf8);
    if (!arg) {
        snprintf(err, errSize, "构造入参字符串失败");
        return nullptr;
    }

    void* params[1] = {&arg};
    Il2CppObject* exc = nullptr;
    Il2CppObject* ret = g_api.runtime_invoke(m, nullptr, params, &exc);
    if (exc) {
        snprintf(err, errSize, "%s 抛了托管异常", mname);
        return nullptr;
    }
    return ret;
}

/** 调试/改造用入口：把密文解密成明文，或把明文加密成密文。 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_kunpo_crack_NativeBridge_nativePrefsCall(JNIEnv* env, jclass clazz, jboolean decrypt,
                                                 jstring arg) {
    if (!ensure_lib()) return mk(env, "ERR libil2cpp.so 未加载");
    if (!resolve_all()) return mk(env, "ERR il2cpp 符号解析失败");
    if (!arg) return mk(env, "ERR 入参为空");

    const char* in = env->GetStringUTFChars(arg, nullptr);
    if (!in) return mk(env, "ERR 读取入参失败");

    char why[256];
    Il2CppObject* ret = prefs_call(decrypt != 0, in, why, sizeof(why));
    env->ReleaseStringUTFChars(arg, in);

    if (!ret) {
        char buf[512];
        snprintf(buf, sizeof(buf), "ERR %s", why);
        NLOGE("nativePrefsCall failed: %s", why);
        return mk(env, buf);
    }

    const int32_t n = str_len(ret);
    const size_t cap = (size_t)n * 4 + 8;
    char* out = (char*)malloc(cap);
    if (!out) return mk(env, "ERR 内存不足");
    utf16_to_utf8(str_chars(ret), n, out, cap);
    jstring js = env->NewStringUTF(out);
    free(out);
    return js;
}

// 遍历所有 image 找类；顺带把 image 名列进 diag（最多留 700 字符）
// 把类的字段名全打出来。用途：判断 native 拿到的 klass 到底是 AOT 空壳
// （Assembly-CSharp.dll，HybridCLR 的 DummyDll，方法体全是 trap 桩）还是热更后
// 的真类（Logic.dll）——两者的静态字段是完全独立的两份内存。
static void dump_class_fields(Il2CppClass* klass) {
    if (!g_api.class_get_fields || !g_api.field_get_name) {
        NLOGI("  [dump] class_get_fields 未绑定，跳过字段枚举");
        return;
    }
    void* iter = nullptr;
    int i = 0;
    for (;;) {
        FieldInfo* f = g_api.class_get_fields(klass, &iter);
        if (!f) {
            break;
        }
        const char* nm = g_api.field_get_name(f);
        NLOGI("  [dump] field[%d] %s", i, nm ? nm : "?");
        if (++i >= 60) {
            break;
        }
    }
    NLOGI("  [dump] %d fields on %s", i, kTargetClass);
}

static Il2CppClass* find_class_in_images(Il2CppDomain* dom, size_t* outCount, char* diag, size_t diagSize) {
    size_t n = 0;
    const Il2CppAssembly** asms = g_api.domain_get_assemblies(dom, &n);
    if (outCount) {
        *outCount = n;
    }
    if (!asms) {
        return nullptr;
    }
    size_t used = 0;
    Il2CppClass* found = nullptr;
    for (size_t i = 0; i < n; i++) {
        if (!asms[i]) {
            continue;
        }
        Il2CppImage* img = g_api.assembly_get_image(asms[i]);
        if (!img) {
            continue;
        }
        if (diag && used + 80 < diagSize) {
            const char* nm = g_api.image_get_name ? g_api.image_get_name(img) : nullptr;
            if (nm) {
                int w = snprintf(diag + used, diagSize - used, "%s|", nm);
                if (w > 0) {
                    used += (size_t)w;
                }
            }
        }
        if (!found) {
            Il2CppClass* k = g_api.class_from_name(img, kTargetNs, kTargetClass);
            if (k) {
                found = k;
                const char* inm = g_api.image_get_name ? g_api.image_get_name(img) : nullptr;
                NLOGI("target class %s.%s hit in image=%s klass=%p", kTargetNs, kTargetClass,
                      inm ? inm : "?", k);
                dump_class_fields(k);
            }
        }
        if (found && used + 80 >= diagSize) {
            break;
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// 找目标类
// ---------------------------------------------------------------------------

// k 是否就是目标类（namespace + name 精确匹配）。
// 注意：HybridCLR 的解释器类拿到的 namespace 可能为空，此时只按名字算命中。
static bool is_target_class(Il2CppClass* k) {
    if (!k || !g_api.class_get_name) {
        return false;
    }
    const char* nm = g_api.class_get_name(k);
    if (!nm || strcmp(nm, kTargetClass) != 0) {
        return false;
    }
    if (g_api.class_get_namespace) {
        const char* ns = g_api.class_get_namespace(k);
        if (ns && ns[0] != '\0' && strcmp(ns, kTargetNs) != 0) {
            return false;
        }
    }
    return true;
}

// 沿 parent 链向上找目标类。
// 关键：UserSystemWindow 是 abstract，堆上不会有它自己的实例；
// 但它的 UI 子类一定有实例。用 il2cpp_class_get_parent 往上走，
// 任一层的名字对上了，那一层的 klass 就是我们要的（静态字段在父类上）。
static Il2CppClass* match_hierarchy(Il2CppClass* k) {
    if (!g_api.class_get_parent) {
        return is_target_class(k) ? k : nullptr;
    }
    for (int depth = 0; k && depth < 32; depth++) {
        if (is_target_class(k)) {
            return k;
        }
        k = g_api.class_get_parent(k);
    }
    return nullptr;
}

// il2cpp_class_for_each 全类遍历（HybridCLR 的解释器类通常能被它枚举到）
struct ClassScan {
    Il2CppClass* klass;
    int seen;
    int limit;
    int kunpoSeen;
    bool stop;
};

static void class_for_each_cb(Il2CppClass* k, void* userData) {
    ClassScan* c = (ClassScan*)userData;
    if (c->stop || !k) {
        return;
    }
    if (++c->seen > c->limit) {
        c->stop = true;
        return;
    }
    if (g_api.class_get_namespace) {
        const char* ns = g_api.class_get_namespace(k);
        if (ns && strncmp(ns, "Kunpo", 5) == 0) {
            c->kunpoSeen++;
        }
    }
    Il2CppClass* hit = match_hierarchy(k);
    if (hit) {
        c->klass = hit;
        c->stop = true;
    }
}

// 兜底：从 GC 堆的实例反推（热更 image 既不在 assemblies 列表里、
// 也不被 for_each 枚举时，只能顺着实例 -> klass -> parent 链找）
struct HeapScan {
    Il2CppClass* klass;
    Il2CppImage* image;    // 任取一个热更 image
    int seen;
    int limit;
    bool stop;
};

static void heap_scan_cb(void* data, void* userData) {
    HeapScan* ctx = (HeapScan*)userData;
    if (ctx->stop || !data) {
        return;
    }
    if (++ctx->seen > ctx->limit) {
        ctx->stop = true;
        return;
    }
    Il2CppClass* k = g_api.object_get_class((Il2CppObject*)data);
    if (!k) {
        return;
    }
    Il2CppClass* hit = match_hierarchy(k);
    if (hit) {
        ctx->klass = hit;
        ctx->stop = true;
        return;
    }
    if (!ctx->image && g_api.class_get_image && g_api.class_get_namespace) {
        const char* ns = g_api.class_get_namespace(k);
        if (ns && strncmp(ns, "Kunpo", 5) == 0) {
            ctx->image = g_api.class_get_image(k);
        }
    }
}

// 读静态属性 Complete
static Il2CppObject* read_complete_delegate(Il2CppClass* klass, char* why, size_t whySize) {
    Il2CppObject* exc = nullptr;

    // 路径 A：调用属性 getter（最“正当”，无副作用）
    //
    // ⚠️ 安全开关：HybridCLR 的 AOT 空壳（Assembly-CSharp.dll 的 DummyDll）
    // 里方法体是 trap 桩，一旦 klass 命中的是空壳，runtime_invoke 会当场把游戏
    // 打成 SIGTRAP（真机已复现：signal 5 SI_QUEUE）。所以默认只走纯内存读
    // （读字段/静态块不可能 trap），待 image 名与字段清单确认后再打开。
    static const bool kAllowInvokeGetter = false;
    const MethodInfo* getter = (!kAllowInvokeGetter || !g_api.class_get_method_from_name)
                                  ? nullptr
                                  : g_api.class_get_method_from_name(klass, kCompleteGetter, 0);
    if (getter) {
        Il2CppObject* del = g_api.runtime_invoke(getter, nullptr, nullptr, &exc);
        if (exc) {
            NLOGE("get_Complete threw a managed exception");
            del = nullptr;
        }
        if (del) {
            return del;
        }
    } else if (why && whySize) {
        snprintf(why, whySize, "getter get_Complete not found; ");
    }

    // 路径 B：直接读静态 backing field（HybridCLR 的静态字段同样落在
    //         Il2CppClass::static_fields 里）。
    // 优先用 class_get_fields 手工匹配：il2cpp_class_get_field_from_name
    // 在新版 il2cpp 已被移除，真机 dlsym 拿不到。
    FieldInfo* f = nullptr;
    if (g_api.class_get_fields && g_api.field_get_name) {
        void* iter = nullptr;
        for (;;) {
            FieldInfo* cand = g_api.class_get_fields(klass, &iter);
            if (!cand) {
                break;
            }
            const char* nm = g_api.field_get_name(cand);
            if (nm && strcmp(nm, kCompleteField) == 0) {
                f = cand;
                break;
            }
        }
    } else if (g_api.class_get_field_from_name) {
        f = g_api.class_get_field_from_name(klass, kCompleteField);
    }
    if (!f) {
        if (why && whySize) {
            strncat(why, "backing field not found", whySize - strlen(why) - 1);
        }
        return nullptr;
    }
    void* staticData = g_api.class_get_static_field_data(klass);
    if (!staticData) {
        if (why && whySize) {
            strncat(why, "class static data is null", whySize - strlen(why) - 1);
        }
        return nullptr;
    }
    size_t off = g_api.field_get_offset(f);
    Il2CppObject* del = *(Il2CppObject**)((char*)staticData + (ptrdiff_t)off);
    NLOGI("backing field off=%zu staticData=%p value=%p", off, staticData, (void*)del);
    if (!del && why && whySize) {
        strncat(why, "field value is null", whySize - strlen(why) - 1);
    }
    return del;
}

// ---------------------------------------------------------------------------
// JNI 入口
// ---------------------------------------------------------------------------

/**
 * 尝试触发一次 Login()。
 * 返回字符串以状态码开头：
 *   "OK"   —— 已成功调用 Complete 委托（等价于点了登录按钮）
 *   "WAIT" —— 时机未到（so 没加载 / il2cpp 没初始化 / 还没走到登录界面），继续轮询
 *   "ERR"  —— 硬错误（符号缺失等），继续轮询也不会有用
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_kunpo_crack_NativeBridge_nativeTryTriggerLogin(JNIEnv* env, jclass clazz, jboolean verbose) {
    char out[1024];
    out[0] = '\0';

    if (!ensure_lib()) {
        return mk(env, "WAIT libil2cpp.so not loaded yet");
    }
    if (!resolve_all()) {
        return mk(env, "ERR il2cpp symbol resolve failed (see logcat)");
    }

    Il2CppDomain* dom = g_api.domain_get();
    if (!dom) {
        return mk(env, "WAIT il2cpp domain not ready");
    }
    // 幂等：主线程本来就已经 attach 到 il2cpp，这里只是兜底
    g_api.thread_attach(dom);

    size_t imageCount = 0;
    char diag[768];
    diag[0] = '\0';

    // L1：标准路径（AOT image 里按名字查）。命中过就直接用缓存。
    Il2CppClass* target = g_cached_target;
    if (!target) {
        target = find_class_in_images(dom, &imageCount, diag, sizeof(diag));
    }

    // L2：il2cpp_class_for_each 全类遍历。HybridCLR 的解释器类不在 AOT
    //     metadata 里，但通常能被这个 API 枚举到 —— 这是主要修复点。
    int forEachSeen = 0;
    if (!target && g_api.class_for_each) {
        ClassScan cs;
        memset(&cs, 0, sizeof(cs));
        cs.limit = 200000;
        g_api.class_for_each(class_for_each_cb, &cs);
        forEachSeen = cs.seen;
        if (cs.klass) {
            target = cs.klass;
        }
    }

    // L3：GC 堆实例反推（贵，降频到每 3 次轮询一次）
    int heapSeen = -1;
    bool wantHeap = ((g_scan_ticks++ % 3) == 0);
    if (!target && wantHeap && g_api.gc_foreach_heap
            && (g_api.class_get_image || g_api.class_get_parent)) {
        HeapScan ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.limit = 200000;
        g_api.gc_foreach_heap(heap_scan_cb, &ctx);
        heapSeen = ctx.seen;
        if (ctx.klass) {
            target = ctx.klass;
        } else if (ctx.image) {
            // L4：拿到热更 image 后再按名字查
            target = g_api.class_from_name(ctx.image, kTargetNs, kTargetClass);
            if (target) {
                NLOGI("found %s via GC heap scan (image=%s)", kTargetClass,
                      g_api.image_get_name ? g_api.image_get_name(ctx.image) : "?");
            }
        }
    }

    if (target && !g_cached_target) {
        g_cached_target = target;
        NLOGI("target class %s.%s cached (klass=%p)", kTargetNs, kTargetClass,
              (void*)target);
    }

    if (!target) {
        if (verbose) {
            snprintf(out, sizeof(out),
                     "WAIT class not found; assemblies=%zu for_each=%d heap=%d images=%s",
                     imageCount, forEachSeen, heapSeen, diag);
        } else {
            snprintf(out, sizeof(out), "WAIT class not found (assemblies=%zu)", imageCount);
        }
        return mk(env, out);
    }

    char why[256];
    why[0] = '\0';
    Il2CppObject* del = read_complete_delegate(target, why, sizeof(why));
    if (!del) {
        snprintf(out, sizeof(out), "WAIT Complete delegate is null (%s)", why);
        return mk(env, out);
    }

    // ⚠️ 免登陆触发开关（默认关）。
    //
    // 实测到的完整链（见 work/README.md 第 8 节）：
    //   Complete → KunpoAndroidLauncher.Login() → NetHelper.Login(deviceId)
    //   → 服务端下发 token → SetNetConfigByLogin() 才能填 NetworkConfig.*
    // 也就是说 Complete 的 lambda 本身又走一次网络验证，绕不过服务端；
    // 而且在这个 runtime_invoke 栈里启动 UniTask 状态机，真机会直接
    // SIGTRAP (signal 5, SI_QUEUE，栈顶就是 libfishcrack)。
    // 所以默认只读取 + 报告，不触发。
    static const bool kAutoTriggerComplete = false;
    if (!kAutoTriggerComplete) {
        snprintf(out, sizeof(out),
                 "SKIP Complete delegate present but auto-trigger disabled (%s)", why);
        return mk(env, out);
    }

    // 首选：调游戏自己的 CompleteInvoke()（protected static void, 0 参数）。
    //   il2cpp_runtime_invoke 不做访问检查，protected 可直接调。
    //   它做的是 Complete?.Invoke() + Complete=null，比我们手动 Invoke 更贴合
    //   原生语义（空字段后不会因游戏后续再调 CompleteInvoke 而重复触发）。
    //   注意：必须先确认 del != null 才调，否则 CompleteInvoke 只是空操作，
    //   会让我们误判为“已触发”。
    const MethodInfo* ci = g_api.class_get_method_from_name(target, kCompleteInvoke, 0);
    if (ci) {
        Il2CppObject* exc0 = nullptr;
        g_api.runtime_invoke(ci, nullptr, nullptr, &exc0);
        if (!exc0) {
            NLOGI("Login() triggered through UserSystemWindow.CompleteInvoke()");
            return mk(env, "OK Login() invoked via UserSystemWindow.CompleteInvoke()");
        }
        NLOGE("CompleteInvoke() threw a managed exception; falling back to delegate Invoke");
    } else {
        NLOGI("CompleteInvoke not found; falling back to delegate Invoke");
    }

    Il2CppClass* delClass = g_api.object_get_class(del);
    if (!delClass) {
        return mk(env, "ERR delegate class is null");
    }
    const MethodInfo* invoke = g_api.class_get_method_from_name(delClass, "Invoke", 0);
    if (!invoke) {
        return mk(env, "ERR System.Action::Invoke not found");
    }

    Il2CppObject* exc = nullptr;
    g_api.runtime_invoke(invoke, del, nullptr, &exc);
    if (exc) {
        return mk(env, "ERR Complete.Invoke raised a managed exception");
    }
    NLOGI("Login() triggered through UserSystemWindow.Complete");
    return mk(env, "OK Login() invoked via UserSystemWindow.Complete");
}

/** 诊断用：打印 il2cpp 运行时里能看到的所有 image，以及目标类是否可见。 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_kunpo_crack_NativeBridge_nativeProbe(JNIEnv* env, jclass clazz) {
    char out[4096];
    out[0] = '\0';

    if (!ensure_lib()) {
        return mk(env, "probe: libil2cpp.so not loaded");
    }
    if (!resolve_all()) {
        return mk(env, "probe: symbol resolve failed");
    }
    Il2CppDomain* dom = g_api.domain_get();
    if (!dom) {
        return mk(env, "probe: domain not ready");
    }
    size_t imageCount = 0;
    char diag[3000];
    diag[0] = '\0';
    Il2CppClass* target = find_class_in_images(dom, &imageCount, diag, sizeof(diag));

    int forEachSeen = 0;
    int kunpoClasses = 0;
    if (!target && g_api.class_for_each) {
        ClassScan cs;
        memset(&cs, 0, sizeof(cs));
        cs.limit = 500000;
        g_api.class_for_each(class_for_each_cb, &cs);
        forEachSeen = cs.seen;
        kunpoClasses = cs.kunpoSeen;
        if (cs.klass) {
            target = cs.klass;
        }
    }

    int heapSeen = -1;
    if (!target && g_api.gc_foreach_heap
            && (g_api.class_get_image || g_api.class_get_parent)) {
        HeapScan ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.limit = 500000;
        g_api.gc_foreach_heap(heap_scan_cb, &ctx);
        heapSeen = ctx.seen;
        if (ctx.klass) {
            target = ctx.klass;
        } else if (ctx.image) {
            target = g_api.class_from_name(ctx.image, kTargetNs, kTargetClass);
        }
    }

    snprintf(out, sizeof(out),
             "probe: assemblies=%zu for_each=%d kunpo=%d heap=%d target=%s images=%s",
             imageCount, forEachSeen, kunpoClasses, heapSeen,
             target ? "FOUND" : "missing", diag);
    return mk(env, out);
}

// ---------------------------------------------------------------------------
// 读 SimplePlayerPrefs.key 静态字段（存档 AES 密钥）
// ---------------------------------------------------------------------------
// 为什么要这么干：
//   SimplePlayerPrefs 的 key 默认是硬编码的 ":{j%6j?E:t#}G10mM%9hp5S=%}2,Y26C"，
//   但它有 `public static string Msg { set { key = value; } }`，而
//   NetHelper.SetNetConfigByLogin() 最后几行就是
//   `SimplePlayerPrefs.Msg = NetworkConfig.UId;`
//   —— 登录后 key 可能被改成 UId。到底当前用的是哪个，取决于 provider 的创建时机。
//
//   读静态字段只是内存读取，不进入解释器，不会像 runtime_invoke 那样把游戏打崩。

/** 读一个静态引用类型字段的当前值。 */
static Il2CppObject* read_static_field_obj(Il2CppClass* k, const char* name,
                                           char* why, size_t whySize) {
    if (!g_api.class_get_field_from_name || !g_api.class_get_static_field_data ||
        !g_api.field_get_offset) {
        if (why && whySize) snprintf(why, whySize, "api missing");
        return nullptr;
    }
    FieldInfo* f = g_api.class_get_field_from_name(k, name);
    if (!f) {
        if (why && whySize) snprintf(why, whySize, "field '%s' not found", name);
        return nullptr;
    }
    void* sd = g_api.class_get_static_field_data(k);
    if (!sd) {
        if (why && whySize) snprintf(why, whySize, "static data null");
        return nullptr;
    }
    size_t off = g_api.field_get_offset(f);
    Il2CppObject* v = *(Il2CppObject**)((char*)sd + (ptrdiff_t)off);
    if (!v && why && whySize) snprintf(why, whySize, "value null");
    return v;
}

/**
 * 返回 SimplePlayerPrefs 当前的 AES 密钥字符串。
 * 返回以 "ERR" 开头表示失败。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_kunpo_crack_NativeBridge_nativePrefsKey(JNIEnv* env, jclass clazz) {
    if (!ensure_lib()) {
        return mk(env, "ERR libil2cpp.so not loaded");
    }
    if (!resolve_all()) {
        return mk(env, "ERR il2cpp symbol resolve failed");
    }
    Il2CppDomain* dom = g_api.domain_get();
    if (!dom) {
        return mk(env, "WAIT il2cpp domain not ready");
    }
    g_api.thread_attach(dom);

    Il2CppClass* k = find_class_ns_name(dom, "Arvin.PlayerPrefsUtilities", "SimplePlayerPrefs");
    if (!k) {
        return mk(env, "ERR class Arvin.PlayerPrefsUtilities.SimplePlayerPrefs not found");
    }

    char why[128];
    why[0] = '\0';
    Il2CppObject* keyObj = read_static_field_obj(k, "key", why, sizeof(why));
    if (!keyObj) {
        char o[256];
        snprintf(o, sizeof(o), "ERR key field unreadable (%s)", why);
        return mk(env, o);
    }

    int32_t len = str_len(keyObj);
    if (len < 0 || len > 512) {
        char o[128];
        snprintf(o, sizeof(o), "ERR key length abnormal: %d", len);
        return mk(env, o);
    }
    const uint16_t* ch = str_chars(keyObj);
    size_t cap = (size_t)len * 4 + 8;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        return mk(env, "ERR oom");
    }
    utf16_to_utf8(ch, len, buf, (int)cap);
    NLOGI("SimplePlayerPrefs.key = [%s] (len=%d)", buf, len);
    jstring r = mk(env, buf);
    free(buf);
    return r;
}

// ---------------------------------------------------------------------------
// 读取运行时真正在用的 AES key
//
// 背景（官方 1.1.8 源码，见 work/live/official_all.cs:184715）：
//   private static string key = ":{j%6j?E:t#}G10mM%9hp5S=%}2,Y26C";
//   private static RijndaelManaged provider = null;
//   public static string Msg { set { key = value; } }      // 只改 key，不重建
//   private static void SetupProvider() {
//       provider = new RijndaelManaged();
//       provider.Key = Encoding.ASCII.GetBytes(key);
//       provider.Mode = CipherMode.ECB;
//   }
// 线上存档用默认 key 解不开 ⇒ Msg 被改过且 ResetProvider() 重建了 provider。
// 静态字段 key 在 HybridCLR 解释类里读不到（v10 实测 null），
// 但 provider 实例里的 KeyValue 就是真正生效的 32 字节。
// 下面两条路径**都只读内存，不调用任何托管方法**，所以不会像
// runtime_invoke 那样把游戏打崩。
// ---------------------------------------------------------------------------

#define kMaxKeyCand 8

struct KeyScanCtx {
    void* objs[kMaxKeyCand];
    int count;
    int seen;
};

// il2cpp_gc_foreach_heap 的回调；只比对类名，不做任何分配。
static void key_scan_cb(void* obj, void* user) {
    KeyScanCtx* c = (KeyScanCtx*)user;
    if (!obj || c->count >= kMaxKeyCand) {
        return;
    }
    ++c->seen;
    Il2CppClass* k = g_api.object_get_class((Il2CppObject*)obj);
    if (!k) {
        return;
    }
    const char* nm = g_api.class_get_name(k);
    if (!nm) {
        return;
    }
    // SimplePlayerPrefs.provider 的运行时类型就是 RijndaelManaged
    if (strcmp(nm, "RijndaelManaged") == 0) {
        c->objs[c->count++] = obj;
    }
}

// 从 RijndaelManaged 实例里把 KeyValue(byte[]) 的字节打印成 hex
static bool key_from_cipher_obj(void* provObj, char* hex, int hexSize, char* asc, int ascSize) {
    Il2CppDomain* dom = g_api.domain_get();
    if (!dom || !provObj) {
        return false;
    }
    Il2CppClass* sa = find_class_ns_name(dom, "System.Security.Cryptography", "SymmetricAlgorithm");
    if (!sa) {
        return false;
    }
    FieldInfo* fKey = g_api.class_get_field_from_name(sa, "KeyValue");
    if (!fKey) {
        return false;
    }
    const size_t offKey = g_api.field_get_offset(fKey);
    void* arr = *(void**)((char*)provObj + (ptrdiff_t)offKey);
    if (!arr) {
        return false;
    }
    // il2cpp 单维数组布局：klass(8) monitor(8) bounds(8) max_length(8) data[]
    unsigned long long len = *(unsigned long long*)((char*)arr + 0x18);
    const unsigned char* data = (const unsigned char*)arr + 0x20;
    if (len == 0 || len > 64) {
        return false;
    }
    if ((int)(len * 3 + 1) > hexSize || (int)len + 1 > ascSize) {
        return false;
    }
    for (unsigned long long i = 0; i < len; ++i) {
        snprintf(hex + i * 3, 4, "%02x", data[i]);
        asc[i] = (data[i] >= 32 && data[i] < 127) ? (char)data[i] : '.';
    }
    hex[len * 3] = '\0';
    asc[len] = '\0';
    return true;
}

/**
 * 返回字符串以状态码开头：
 *   "OK"   —— 拿到了 key（hex/ascii 可直接离线解存档）
 *   "WAIT" —— il2cpp 还没就绪
 *   "ERR"  —— 硬错误
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_kunpo_crack_NativeBridge_nativeDumpKey(JNIEnv* env, jclass clazz) {
    if (!ensure_lib()) {
        return mk(env, "WAIT libil2cpp.so not loaded");
    }
    if (!resolve_all()) {
        return mk(env, "ERR il2cpp symbol resolve failed");
    }
    Il2CppDomain* dom = g_api.domain_get();
    if (!dom) {
        return mk(env, "WAIT il2cpp domain not ready");
    }
    if (!g_api.class_get_field_from_name || !g_api.class_get_static_field_data ||
        !g_api.field_get_offset || !g_api.object_get_class || !g_api.class_get_name) {
        return mk(env, "ERR field/introspection api missing");
    }

    char hex[3 * 64 + 1];
    char asc[65];
    char out[4096];
    out[0] = '\0';

    // ---- 路径 A：静态字段 provider（HybridCLR 类可能读不到，仅先试） ----
    Il2CppClass* sp = find_class_ns_name(dom, "Arvin.PlayerPrefsUtilities", "SimplePlayerPrefs");
    if (sp) {
        // HybridCLR 解释类的 static_fields 布局和原生不同，先把全部字段的
        // name / offset / 当前值打出来，用来确认 provider 到底落在哪个 offset。
        if (g_api.class_get_fields && g_api.field_get_name) {
            void* sdDbg = g_api.class_get_static_field_data(sp);
            // il2cpp_class_get_fields 是迭代器式：每次传同一个 iter，返回下一个
            // FieldInfo*，直到 NULL。不是返回数组。
            void* iter = nullptr;
            FieldInfo* f = nullptr;
            int fi = 0;
            while ((f = g_api.class_get_fields(sp, &iter)) != nullptr && fi < 32) {
                const char* fn = g_api.field_get_name(f);
                const size_t fo = g_api.field_get_offset(f);
                void* val = sdDbg ? *(void**)((char*)sdDbg + (ptrdiff_t)fo) : nullptr;
                NLOGI("  [sp] field[%d] %s off=%zu value=%p", fi, fn ? fn : "?", fo, val);
                ++fi;
            }
        }
        FieldInfo* fProv = g_api.class_get_field_from_name(sp, "provider");
        void* sd = g_api.class_get_static_field_data(sp);
        NLOGI("SimplePlayerPrefs staticData=%p field_provider=%p", sd, (void*)fProv);
        if (fProv && sd) {
            const size_t offProv = g_api.field_get_offset(fProv);
            void* provObj = *(void**)((char*)sd + (ptrdiff_t)offProv);
            NLOGI("provider off=%zu obj=%p", offProv, provObj);
            if (provObj && key_from_cipher_obj(provObj, hex, sizeof(hex), asc, sizeof(asc))) {
                snprintf(out, sizeof(out), "OK via=static provider=%p hex=%s ascii=%s",
                         provObj, hex, asc);
                NLOGI("%s", out);
                return mk(env, out);
            }
        }
    } else {
        NLOGI("SimplePlayerPrefs class not found in assemblies");
    }

    // ---- 路径 B：扫 GC 堆找 RijndaelManaged 实例 ----
    if (!g_api.gc_foreach_heap) {
        return mk(env, "ERR no gc_foreach_heap and static path failed");
    }
    KeyScanCtx c;
    memset(&c, 0, sizeof(c));
    g_api.gc_foreach_heap(key_scan_cb, &c);
    NLOGI("heap scanned=%d RijndaelManaged found=%d", c.seen, c.count);
    if (c.count == 0) {
        snprintf(out, sizeof(out),
                 "ERR no RijndaelManaged on heap (scanned=%d) — 说明还没调用过 Encrypt/Decrypt",
                 c.seen);
        return mk(env, out);
    }

    int n = 0;
    for (int i = 0; i < c.count; ++i) {
        if (!key_from_cipher_obj(c.objs[i], hex, sizeof(hex), asc, sizeof(asc))) {
            continue;
        }
        n++;
        size_t used = strlen(out);
        snprintf(out + used, sizeof(out) - used, "%s[%d] obj=%p hex=%s ascii=%s",
                 used ? " | " : "", i, c.objs[i], hex, asc);
        NLOGI("cand[%d] obj=%p hex=%s ascii=%s", i, c.objs[i], hex, asc);
    }
    if (n == 0) {
        return mk(env, "ERR RijndaelManaged found but KeyValue unreadable");
    }
    char head[64];
    snprintf(head, sizeof(head), "OK count=%d ", n);
    NLOGI("dumpKey result: %s%s", head, out);
    // 前置状态码，保持 Java 侧 ok() 判定一致
    static char finalBuf[4096];
    snprintf(finalBuf, sizeof(finalBuf), "%s%s", head, out);
    return mk(env, finalBuf);
}

// ---------------------------------------------------------------------------
// 打开游戏内置 GM 面板（纯内存写，不调托管方法，不会闪退）
//
// 官方 1.1.8 源码（work/live/official_all.cs，ilspy 反编译 Logic_official.dll）：
//   10830: public class ConstConfig            // 全局命名空间（引用处写作 ConstConfig.IsDev）
//   11032: public static bool IsDev;
//   11231: public static bool OpenGM { get { return _gm; } set { _gm = value; } }
//   11243: public static bool CanShowGM => IsDev & OpenGM;
//   11461: IsDev = true;   // 静态构造里官方自己就设了 true
//   11462: _gm = false;    // 唯一拦住 GM 的就是这个
//   66814: WindowManager.Register("Main", "GMWnd", "GameMain", typeof(GMWnd));
//  138148: WindowManager.ShowWindow("GMWnd", null);
//
// 所以把静态字段 _gm 写成 true，CanShowGM 立刻为 true，GM 入口就出现。
// 这是纯内存写（直接改 static_fields），不进入解释器，所以不会像
// runtime_invoke 那样把游戏打崩。
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jstring JNICALL
Java_com_kunpo_crack_NativeBridge_nativeSetGM(JNIEnv* env, jclass clazz, jboolean on) {
    if (!ensure_lib()) {
        return mk(env, "WAIT libil2cpp.so not loaded");
    }
    if (!resolve_all()) {
        return mk(env, "WAIT il2cpp api not ready");
    }
    Il2CppDomain* dom = g_api.domain_get();
    if (!dom) {
        return mk(env, "WAIT il2cpp domain not ready");
    }
    if (!g_api.class_get_field_from_name || !g_api.class_get_static_field_data ||
        !g_api.field_get_offset) {
        return mk(env, "ERR field api missing");
    }

    // ConstConfig 在全局命名空间；再兼容几种可能的写法
    Il2CppClass* cc = find_class_ns_name(dom, "", "ConstConfig");
    if (!cc) {
        cc = find_class_ns_name(dom, "Kunpo", "ConstConfig");
    }
    if (!cc) {
        cc = find_class_ns_name(dom, "Game", "ConstConfig");
    }
    if (!cc) {
        return mk(env, "WAIT ConstConfig not found yet");
    }

    void* sd = g_api.class_get_static_field_data(cc);
    if (!sd) {
        return mk(env, "ERR ConstConfig static data is null");
    }

    const bool want = (on == JNI_TRUE);

    // _gm 是 OpenGM 的 backing field
    FieldInfo* fGm = g_api.class_get_field_from_name(cc, "_gm");
    if (!fGm) {
        return mk(env, "ERR field '_gm' not found");
    }
    const size_t offGm = g_api.field_get_offset(fGm);
    unsigned char* pGm = (unsigned char*)sd + offGm;
    const unsigned char gmBefore = *pGm;
    *pGm = want ? 1 : 0;

    // IsDev 官方已经是 true，顺手确保一下（不影响其他逻辑）
    size_t offDev = 0;
    int devBefore = -1;
    int devAfter = -1;
    FieldInfo* fDev = g_api.class_get_field_from_name(cc, "IsDev");
    if (fDev) {
        offDev = g_api.field_get_offset(fDev);
        unsigned char* pDev = (unsigned char*)sd + offDev;
        devBefore = *pDev;
        if (want && !devBefore) {
            *pDev = 1;
        }
        devAfter = *pDev;
    }

    char out[256];
    snprintf(out, sizeof(out),
             "OK _gm(off=%zu) %d->%d ; IsDev(off=%zu) %d->%d",
             offGm, gmBefore, *pGm, offDev, devBefore, devAfter);
    NLOGI("setGM: %s", out);
    return mk(env, out);
}

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

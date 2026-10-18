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
typedef Il2CppClass* (*pfn_class_get_image)(Il2CppClass*);
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
};

static Il2CppApi g_api;
static bool g_resolved = false;

// 目标类（热更 DLL 里的登录 UI 类）
static const char* kTargetNs = "KunpoUserSystem_FGUI";
static const char* kTargetClass = "UserSystemWindow";
static const char* kCompleteGetter = "get_Complete";
static const char* kCompleteField = "<Complete>k__BackingField";

// ---------------------------------------------------------------------------
// 绑定
// ---------------------------------------------------------------------------
static bool ensure_lib() {
    if (g_api.handle) {
        return true;
    }
    void* h = dlopen("libil2cpp.so", RTLD_NOW);
    if (!h) {
        h = dlopen("libil2cpp.so", RTLD_LAZY);
    }
    if (!h) {
        return false;
    }
    g_api.handle = h;
    NLOGI("libil2cpp.so loaded, handle=%p", h);
    return true;
}

static bool bind_one(void* slot, const char* name) {
    void* p = dlsym(g_api.handle, name);
    if (!p) {
        NLOGE("dlsym(%s) failed: %s", name, dlerror());
        return false;
    }
    // 函数指针与数据指针在 AArch64/ARM 上都是指针宽度，用 memcpy 避免
    // (void**)(fnptr*) 这种退化转换带来的 UB / 警告。
    memcpy(slot, &p, sizeof(void*));
    return true;
}

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

    // 下面几个只用于诊断 / 兜底，缺了也能跑
    g_api.class_get_image =
        (pfn_class_get_image)dlsym(g_api.handle, "il2cpp_class_get_image");
    g_api.class_get_name =
        (pfn_class_get_name)dlsym(g_api.handle, "il2cpp_class_get_name");
    g_api.class_get_namespace =
        (pfn_class_get_namespace)dlsym(g_api.handle, "il2cpp_class_get_namespace");
    g_api.class_get_field_from_name = (pfn_class_get_field_from_name)dlsym(
        g_api.handle, "il2cpp_class_get_field_from_name");
    g_api.gc_foreach_heap =
        (pfn_gc_foreach_heap)dlsym(g_api.handle, "il2cpp_gc_foreach_heap");

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
            }
        }
        if (found && used + 80 >= diagSize) {
            break;
        }
    }
    return found;
}

// 兜底：热更 image 不在 domain_get_assemblies 里时，从 GC 堆上找
// UserSystemWindow 的派生类实例，拿到那个类所属的 image。
struct HeapScan {
    Il2CppClass* klass;    // 直接命中 UserSystemWindow（理论上有静态引用时可能出现）
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
    const char* nm = g_api.class_get_name ? g_api.class_get_name(k) : nullptr;
    if (nm && strcmp(nm, kTargetClass) == 0) {
        ctx->klass = k;
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
    const MethodInfo* getter =
        g_api.class_get_method_from_name(klass, kCompleteGetter, 0);
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
    //         Il2CppClass::static_fields 里）
    if (!g_api.class_get_field_from_name) {
        return nullptr;
    }
    FieldInfo* f = g_api.class_get_field_from_name(klass, kCompleteField);
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
    Il2CppClass* target = find_class_in_images(dom, &imageCount, diag, sizeof(diag));

    // 兜底：从 GC 堆上找（HybridCLR 的热更 image 未必出现在 assemblies 列表里）
    if (!target && g_api.gc_foreach_heap && g_api.class_get_image) {
        HeapScan ctx;
        ctx.klass = nullptr;
        ctx.image = nullptr;
        ctx.seen = 0;
        ctx.limit = 200000;
        ctx.stop = false;
        g_api.gc_foreach_heap(heap_scan_cb, &ctx);
        if (ctx.klass) {
            target = ctx.klass;
        } else if (ctx.image) {
            target = g_api.class_from_name(ctx.image, kTargetNs, kTargetClass);
            if (target) {
                NLOGI("found %s via GC heap scan (image=%s)", kTargetClass,
                      g_api.image_get_name ? g_api.image_get_name(ctx.image) : "?");
            }
        }
        if (!target && verbose) {
            snprintf(out, sizeof(out),
                     "WAIT class not found; assemblies=%zu, heap scanned=%d",
                     imageCount, ctx.seen);
            return mk(env, out);
        }
    }

    if (!target) {
        if (verbose) {
            snprintf(out, sizeof(out), "WAIT class not found; assemblies=%zu images=%s",
                     imageCount, diag);
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
    snprintf(out, sizeof(out), "probe: assemblies=%zu target=%s images=%s",
             imageCount, target ? "FOUND" : "missing", diag);
    return mk(env, out);
}

package com.kunpo.crack;

import android.app.Application;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;

/**
 * 「免登陆」这一半的实现（Java 侧）。
 *
 * 分工：
 *   - Java 负责时序与线程：用 Handler(Looper.getMainLooper()) 每 400ms 试一次。
 *     Unity Android 上 Java 主线程 == Unity 主线程，所以 il2cpp 调用落在正确的线程上。
 *   - native（libfishcrack.so）负责干活：见 app/src/main/cpp/fishcrack.cpp。
 *
 * 为什么要 native：要调用的 UserSystemWindow / BaseLauncher.Login 都在热更 DLL
 * （HybridCLR 解释执行）里，libil2cpp 没有对应 JNI 方法，纯 Java 摸不到。
 * 好在 libil2cpp.so 导出了完整 il2cpp C API，所以用 dlopen+dlsym 就能进去。
 */
public final class NativeBridge {

    private static final String TAG = CrackConfig.TAG + "[login]";
    private static final String MODULE_PKG = "com.kunpo.crack";
    private static final String LIB_FILE = "libfishcrack.so";

    private static final long FIRST_DELAY_MS = 3000L;
    private static final long INTERVAL_MS = 400L;
    /** 400ms * 1500 ≈ 10 分钟；登录界面通常几秒内就出现。 */
    private static final int MAX_ATTEMPTS = 1500;

    private static volatile Handler mainHandler;
    private static volatile Context appContext;
    private static volatile boolean libLoaded;
    private static volatile boolean triggered;
    private static volatile boolean loading;
    private static volatile int attempts;
    private static volatile String lastStatus = "";
    /** 宿主（游戏）的 ClassLoader：用来把模块 .so 加载进宿主 namespace。 */
    private static volatile ClassLoader hostCl;

    private NativeBridge() {
    }

    private static native String nativeTryTriggerLogin(boolean verbose);

    private static native String nativeProbe();

    // ------------------------------------------------------------------
    // 入口
    // ------------------------------------------------------------------

    /** 由 CrackHook 在 handleLoadPackage 时调用。 */
    public static void install(ClassLoader hostClassLoader) {
        if (!CrackConfig.AUTO_LOGIN) {
            return;
        }
        hostCl = hostClassLoader;
        XposedBridge.log(TAG + ": 免登陆已启用，等待 Application#attach 拿 Context");

        // 主路径：hook Application#attach —— 这段时间就在 Application 创建之前，
        // 一定能拿到 Context，而且不依赖任何 hidden API 反射。
        try {
            XposedHelpers.findAndHookMethod("android.app.Application", hostClassLoader,
                    "attach", Context.class, new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam param) {
                            if (param.args != null && param.args.length > 0
                                    && param.args[0] instanceof Context) {
                                onContext((Context) param.args[0]);
                            }
                        }
                    });
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": hook Application#attach 失败 -> " + t);
        }

        // 兜底：万一 attach 已经过去了（或 hook 失败），反射再捞一次
        main().postDelayed(new Runnable() {
            @Override
            public void run() {
                Context c = contextByReflection();
                if (c != null) {
                    onContext(c);
                }
            }
        }, 2000L);
    }

    // ------------------------------------------------------------------
    // Context
    // ------------------------------------------------------------------

    private static void onContext(Context ctx) {
        if (appContext != null || libLoaded) {
            return;
        }
        Context app = ctx.getApplicationContext();
        appContext = (app != null) ? app : ctx;
        XposedBridge.log(TAG + ": 拿到 Context: " + appContext.getPackageName());
        if (!loadLibrary(appContext)) {
            XposedBridge.log(TAG + ": 加载 native 库失败，免登陆不可用");
            return;
        }
        startPolling();
        // 立刻打一次探针：把 il2cpp 里可见的 image 清单/目标类是否可见落到日志，
        // 排障第一步就看这一行（此时 il2cpp 可能还没起来，返回 domain not ready 正常）。
        try {
            XposedBridge.log(TAG + ": [probe] " + nativeProbe());
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": probe 失败 -> " + t);
        }
    }

    private static Context contextByReflection() {
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Method m = activityThread.getMethod("currentApplication");
            Object o = m.invoke(null);
            if (o instanceof Context) {
                return (Context) o;
            }
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": currentApplication 反射失败 -> " + t);
        }
        return null;
    }

    // ------------------------------------------------------------------
    // 加载 .so（三条退路，覆盖不同 LSPosed / 打包方式）
    // ------------------------------------------------------------------

    private static boolean loadLibrary(Context ctx) {
        if (libLoaded) {
            return true;
        }

        final String abi = primaryAbi();

        // 0) 先算出模块自己的 APK 路径 / so 目录。
        //    完全不依赖 PackageManager —— HMA-OSS 之类的“隐藏应用列表”会让
        //    getApplicationInfo(MODULE_PKG) 直接抛 NameNotFoundException:
        //    com.kunpo.crack（已在真机日志里复现）。
        String[] mp = modulePathsFromClassLoader();
        String clApk = (mp != null) ? mp[0] : null;
        String clLibDir = (mp != null) ? mp[1] : null;
        if (clApk != null) {
            XposedBridge.log(TAG + ": classloader 推出 apk=" + clApk + " libDir=" + clLibDir);
        }

        // 1) 直接 load 系统已解压好的 <apkDir>/lib/<abi>/libfishcrack.so。
        //    build.gradle 里 useLegacyPackaging=true → extractNativeLibs=true，
        //    安装时系统就会把 .so 解压到该目录，路径合法（apk_data_file）
        //    且不受包可见性影响。
        if (clLibDir != null && tryLoad(clLibDir + "/" + LIB_FILE, "classloader libDir")) {
            return true;
        }
        if (clApk != null) {
            File apkDir = new File(clApk).getParentFile();
            for (String a : new String[]{abi, "arm64-v8a", "armeabi-v7a"}) {
                File f = new File(apkDir, "lib/" + a + "/" + LIB_FILE);
                if (tryLoad(f.getAbsolutePath(), "derived lib/" + a)) {
                    return true;
                }
            }
        }

        // 2) 从模块 APK 里抠出 .so，落到宿主 codeCacheDir 再 load
        for (String cand : new String[]{clApk, moduleApkPath()}) {
            if (cand == null) {
                continue;
            }
            try {
                File out = prepareSoFile(ctx);
                if (extractFromApk(cand, out) && tryLoad(out.getAbsolutePath(), "extracted")) {
                    return true;
                }
            } catch (Throwable t) {
                XposedBridge.log(TAG + ": 从 APK 抽取 .so 失败 -> " + t);
            }
        }

        // 3) PackageManager 的 nativeLibraryDir（HMA-OSS 等可能让这里抛异常）
        try {
            ApplicationInfo ai = ctx.getPackageManager().getApplicationInfo(MODULE_PKG, 0);
            if (ai != null && ai.nativeLibraryDir != null
                    && tryLoad(new File(ai.nativeLibraryDir, LIB_FILE).getAbsolutePath(),
                    "nativeLibraryDir")) {
                return true;
            }
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": getApplicationInfo 失败（Android 11+ 包可见性 / HMA）-> " + t);
        }

        // 4) 交给系统找（某些 LSPosed 版本会把模块 lib 目录挂进宿主）
        try {
            System.loadLibrary("fishcrack");
            libLoaded = true;
            XposedBridge.log(TAG + ": loaded via System.loadLibrary");
            return true;
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": System.loadLibrary 失败 -> " + t);
        }
        return false;
    }

    /** System.load 包一层：失败不抛、只记日志，方便逐条路径试。 */
    /**
     * 用宿主（游戏）自己的 ClassLoader 去 load 模块的 .so。
     *
     * 为什么需要：LSPosed 注入时会把模块 .so 装进一个隔离 namespace
     * （真机日志：{@code Load ... using isolated ns clns-10}），那里看不到游戏
     * 自己的 libil2cpp.so，于是 native 侧的 dlopen("libil2cpp.so") 必然失败。
     * Runtime.load0(ClassLoader, String) 能把 namespace 指定为传入 ClassLoader
     * 所在的那个（即应用 default namespace）。
     */
    private static boolean loadWithHostClassLoader(String path) {
        ClassLoader cl = hostCl;
        if (cl == null) {
            return false;
        }
        for (String m : new String[]{"load0", "loadLibrary0"}) {
            try {
                Method mm = Runtime.class.getDeclaredMethod(m, ClassLoader.class, String.class);
                mm.setAccessible(true);
                mm.invoke(Runtime.getRuntime(), cl, path);
                return true;
            } catch (NoSuchMethodException e) {
                continue;
            } catch (Throwable t) {
                XposedBridge.log(TAG + ": Runtime." + m + "(hostCl) 失败 -> " + t);
                return false;
            }
        }
        return false;
    }

    private static boolean tryLoad(String path, String how) {
        if (path == null) {
            return false;
        }
        File f = new File(path);
        if (!f.exists()) {
            XposedBridge.log(TAG + ": [" + how + "] 不存在: " + path);
            return false;
        }
        // 首选宿主 namespace：只有这样 dlopen("libil2cpp.so") 才看得见游戏的 so
        try {
            if (loadWithHostClassLoader(path)) {
                libLoaded = true;
                XposedBridge.log(TAG + ": loaded via " + how + " (host namespace): " + path);
                return true;
            }
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": [" + how + "] host-ns load 异常 -> " + t);
        }
        try {
            System.load(path);
            libLoaded = true;
            XposedBridge.log(TAG + ": loaded via " + how + " (module namespace): " + path);
            return true;
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": [" + how + "] load 失败 -> " + t);
            return false;
        }
    }

    /** 进程主 ABI。 */
    private static String primaryAbi() {
        String[] abis = Build.SUPPORTED_ABIS;
        if (abis != null && abis.length > 0 && abis[0] != null) {
            return abis[0];
        }
        return "arm64-v8a";
    }

    /**
     * 不依赖 PackageManager 定位模块 APK 与其 so 目录。
     *
     * LSPosed 的 LspModuleClassLoader extends BaseDexClassLoader，其
     * nativeLibraryDirectories 形如：
     *   /data/app/~~Rn2Bs-...==/com.kunpo.crack-GEIh...==/base.apk!/lib/arm64-v8a
     * 从 "!/" 处切一刀就得到 APK 路径与 APK 内的 lib 相对路径，再拼成
     * 系统实际解压出来的目录。
     *
     * @return {apkPath, libDirOnDisk}，拿不到返回 null
     */
    private static String[] modulePathsFromClassLoader() {
        try {
            ClassLoader cl = NativeBridge.class.getClassLoader();
            if (cl == null) {
                return null;
            }
            Field f = Class.forName("dalvik.system.BaseDexClassLoader")
                    .getDeclaredField("pathList");
            f.setAccessible(true);
            Object pathList = f.get(cl);
            if (pathList == null) {
                return null;
            }

            // nativeLibraryDirectories 多数版本是 File[]，也有 List<File> 实现；
            // 都拿不到时退回解析 pathList.toString()（真机日志已证明它含该数组）。
            List<String> paths = new ArrayList<>();
            try {
                Field nf = pathList.getClass().getDeclaredField("nativeLibraryDirectories");
                nf.setAccessible(true);
                Object v = nf.get(pathList);
                if (v instanceof Object[]) {
                    for (Object o : (Object[]) v) {
                        if (o != null) {
                            paths.add(String.valueOf(o));
                        }
                    }
                } else if (v instanceof Iterable) {
                    for (Object o : (Iterable<?>) v) {
                        if (o != null) {
                            paths.add(String.valueOf(o));
                        }
                    }
                }
            } catch (Throwable ignored) {
                // 走下面的 toString() 兜底
            }
            if (paths.isEmpty()) {
                String s = String.valueOf(pathList);
                String key = "nativeLibraryDirectories=[";
                int lb = s.indexOf(key);
                if (lb >= 0) {
                    int rb = s.indexOf(']', lb);
                    if (rb > lb) {
                        String body = s.substring(lb + key.length(), rb);
                        for (String p : body.split(",")) {
                            String t = p.trim();
                            if (!t.isEmpty()) {
                                paths.add(t);
                            }
                        }
                    }
                }
            }

            for (String p : paths) {
                int bang = p.indexOf("!/");
                if (bang <= 0) {
                    continue;
                }
                String apk = p.substring(0, bang);
                String inner = p.substring(bang + 2); // 例如 lib/arm64-v8a
                File apkDir = new File(apk).getParentFile();
                if (apkDir == null) {
                    continue;
                }
                return new String[]{apk, new File(apkDir, inner).getAbsolutePath()};
            }
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": modulePathsFromClassLoader 失败 -> " + t);
        }
        return null;
    }

    /** 从模块自身的 BaseDexClassLoader 里挖出模块 APK 路径（不依赖 PackageManager）。 */
    private static String moduleApkPath() {
        try {
            ClassLoader cl = NativeBridge.class.getClassLoader();
            XposedBridge.log(TAG + ": module classloader = " + cl);
            if (cl == null) {
                return null;
            }
            Field f = Class.forName("dalvik.system.BaseDexClassLoader")
                    .getDeclaredField("pathList");
            f.setAccessible(true);
            Object pathList = f.get(cl);
            if (pathList == null) {
                return null;
            }
            Field ef = pathList.getClass().getDeclaredField("dexElements");
            ef.setAccessible(true);
            Object[] elements = (Object[]) ef.get(pathList);
            if (elements == null) {
                return null;
            }
            for (Object el : elements) {
                if (el == null) {
                    continue;
                }
                try {
                    Field df = el.getClass().getDeclaredField("dexFile");
                    df.setAccessible(true);
                    Object dexFile = df.get(el);
                    if (dexFile == null) {
                        continue;
                    }
                    Method getName = dexFile.getClass().getMethod("getName");
                    String p = (String) getName.invoke(dexFile);
                    if (p != null && p.endsWith(".apk")) {
                        return p;
                    }
                } catch (Throwable ignored) {
                    // 换下一个 dexElement
                }
            }
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": moduleApkPath 失败 -> " + t);
        }
        return null;
    }

    /**
     * 准备抽取目标文件。
     *
     * 坑（真机日志坐实）：{@link #extractFromApk} 结束时会把文件设成
     * {@code setWritable(false,false)}（Android 10+ 的 dlopen 要求 .so 不可写，
     * 否则 W^X 拒绝加载）。于是下一次启动再用 FileOutputStream 覆盖它就会
     * {@code EACCES (Permission denied)} —— 而 APK 里的 so 并未解压到 lib/ 目录，
     * 抽取是唯一路径，一撞就整条链全断。
     * 所以先恢复可写并删除；删不掉（例如旧文件还被映射着）就换个唯一名字。
     */
    private static File prepareSoFile(Context ctx) {
        File dir = ctx.getCodeCacheDir();
        File f = new File(dir, LIB_FILE);
        if (f.exists()) {
            //noinspection ResultOfMethodCallIgnored
            f.setWritable(true, true);
            if (!f.delete()) {
                f = new File(dir, LIB_FILE + "." + System.nanoTime());
            }
        }
        return f;
    }

    private static boolean extractFromApk(String apkPath, File outFile) {
        List<String> abis = new ArrayList<>();
        if (Build.SUPPORTED_ABIS != null) {
            for (String a : Build.SUPPORTED_ABIS) {
                abis.add(a);
            }
        }
        abis.add("arm64-v8a");
        abis.add("armeabi-v7a");

        ZipFile zf = null;
        try {
            zf = new ZipFile(apkPath);
            for (String abi : abis) {
                ZipEntry ze = zf.getEntry("lib/" + abi + "/" + LIB_FILE);
                if (ze == null) {
                    continue;
                }
                InputStream in = zf.getInputStream(ze);
                FileOutputStream fos = new FileOutputStream(outFile);
                byte[] buf = new byte[64 * 1024];
                int r;
                while ((r = in.read(buf)) > 0) {
                    fos.write(buf, 0, r);
                }
                fos.flush();
                fos.close();
                in.close();
                outFile.setReadable(true, true);
                // Android 10+ 的 dlopen 要求 .so 不可写（W^X），否则拒绝加载
                outFile.setWritable(false, false);
                XposedBridge.log(TAG + ": 抽取 " + abi + "/" + LIB_FILE + " -> " + outFile);
                return true;
            }
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": extractFromApk 失败 -> " + t);
        } finally {
            if (zf != null) {
                try {
                    zf.close();
                } catch (Throwable ignored) {
                }
            }
        }
        return false;
    }

    // ------------------------------------------------------------------
    // 轮询触发
    // ------------------------------------------------------------------

    private static Handler main() {
        Handler h = mainHandler;
        if (h == null) {
            synchronized (NativeBridge.class) {
                h = mainHandler;
                if (h == null) {
                    h = new Handler(Looper.getMainLooper());
                    mainHandler = h;
                }
            }
        }
        return h;
    }

    private static void startPolling() {
        XposedBridge.log(TAG + ": native 库就绪，开始轮询登录界面（每 " + INTERVAL_MS + "ms 一次）");
        final Runnable poller = new Poller();
        main().postDelayed(poller, FIRST_DELAY_MS);
    }

    private static final class Poller implements Runnable {
        @Override
        public void run() {
            if (triggered) {
                return;
            }
            attempts++;
            if (attempts > MAX_ATTEMPTS) {
                XposedBridge.log(TAG + ": 放弃（已尝试 " + attempts + " 次，最后状态=" + lastStatus + "）");
                return;
            }

            boolean verbose = (attempts <= 3) || (attempts % 200 == 0);
            String r;
            try {
                r = nativeTryTriggerLogin(verbose);
            } catch (Throwable t) {
                XposedBridge.log(TAG + ": native 调用异常 -> " + t);
                main().postDelayed(this, INTERVAL_MS);
                return;
            }
            if (r == null) {
                r = "(null)";
            }
            if (!r.equals(lastStatus) || verbose) {
                XposedBridge.log(TAG + ": [try#" + attempts + "] " + r);
                lastStatus = r;
            }
            if (attempts == 5) {
                try {
                    XposedBridge.log(TAG + ": " + nativeProbe());
                } catch (Throwable t) {
                    XposedBridge.log(TAG + ": probe 失败 -> " + t);
                }
            }
            if (r.startsWith("OK")) {
                triggered = true;
                XposedBridge.log(TAG + ": ===== 免登陆触发成功：已代点“登录”，游戏将直接进入主流程 =====");
                return;
            }
            main().postDelayed(this, INTERVAL_MS);
        }
    }
}

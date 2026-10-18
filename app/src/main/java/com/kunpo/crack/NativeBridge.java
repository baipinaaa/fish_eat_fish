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

        // 1) 模块自己的 nativeLibraryDir（最省事，前提是 PackageManager 能查到本模块）
        try {
            ApplicationInfo ai = ctx.getPackageManager().getApplicationInfo(MODULE_PKG, 0);
            if (ai != null && ai.nativeLibraryDir != null) {
                File f = new File(ai.nativeLibraryDir, LIB_FILE);
                if (f.exists()) {
                    System.load(f.getAbsolutePath());
                    libLoaded = true;
                    XposedBridge.log(TAG + ": loaded via nativeLibraryDir: " + f);
                    return true;
                }
                XposedBridge.log(TAG + ": nativeLibraryDir 里没有 " + LIB_FILE + " -> " + f);
            }
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": getApplicationInfo 失败（Android 11+ 包可见性）-> " + t);
        }

        // 2) 顺着模块 APK 路径把 .so 抠出来，落到宿主私有目录再 load
        try {
            String apk = moduleApkPath();
            if (apk != null) {
                File out = new File(ctx.getCacheDir(), LIB_FILE);
                if (extractFromApk(apk, out)) {
                    System.load(out.getAbsolutePath());
                    libLoaded = true;
                    XposedBridge.log(TAG + ": loaded via extracted " + out + " (apk=" + apk + ")");
                    return true;
                }
            }
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": 从 APK 抽取 .so 失败 -> " + t);
        }

        // 3) 交给系统找（某些 LSPosed 版本会把模块 lib 目录挂进宿主）
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

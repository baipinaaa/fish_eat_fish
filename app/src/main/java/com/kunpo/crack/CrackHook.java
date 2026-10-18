package com.kunpo.crack;

import java.util.HashMap;
import java.util.Map;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * 入口。拦截的是 PlayerPrefs 的**存储层**。
 *
 * Unity（libunity.so）在 Android 上不自己写文件，而是通过 JNI 调
 *   Context.getSharedPreferences(...) / SharedPreferences.getAll() /
 *   SharedPreferences.getString() / SharedPreferences$Editor.putString()
 * 把 PlayerPrefs 落到 android.app.SharedPreferencesImpl（内部就是 XML）。
 *
 * 游戏热更 DLL（Logic.dll）里的 SimplePlayerPrefs 在调用 PlayerPrefs 之前
 * 已经做了 AES-256-ECB 加密，所以这一层看到的键和值都是 Base64 密文，
 * 我们的篡改全部在密文层解开→改→原样加密回去，游戏逻辑完全无感。
 *
 * 四个拦截点：
 *   1. getAll()                       —— Unity 启动时一次性把整张表读进 native 缓存
 *   2. getString(String,String)       —— 逐键读取
 *   3. EditorImpl.putString(String,String) —— 游戏存盘时改写入值，保证落盘即篡改结果
 *   4. EditorImpl.clear()/remove(String)   —— 兜底拦掉反作弊封号的 DeleteAll
 */
public class CrackHook implements IXposedHookLoadPackage {

    private static final String TAG = CrackConfig.TAG;

    private static final String SP_IMPL = "android.app.SharedPreferencesImpl";
    private static final String SP_EDITOR_IMPL = "android.app.SharedPreferencesImpl$EditorImpl";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lpparam) {
        if (!CrackConfig.TARGET_PACKAGE.equals(lpparam.packageName)) {
            return;
        }
        XposedBridge.log(TAG + ": attached pkg=" + lpparam.packageName
                + " proc=" + lpparam.processName);

        ClassLoader cl = lpparam.classLoader;

        // 1) 整表读取
        hookMethod(cl, SP_IMPL, "getAll", new XC_MethodHook() {
            @Override
            @SuppressWarnings("unchecked")
            protected void afterHookedMethod(MethodHookParam param) {
                Object result = param.getResult();
                if (!(result instanceof Map)) {
                    return;
                }
                Map<String, Object> copy = new HashMap<>((Map<String, Object>) result);
                if (ArchiveTamper.tamperMap(copy, "getAll")) {
                    param.setResult(copy);
                }
            }
        });

        // 2) 逐键读取
        hookMethod(cl, SP_IMPL, "getString", new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                if (param.args.length < 2) {
                    return;
                }
                Object key = param.args[0];
                Object result = param.getResult();
                if (!(key instanceof String) || !(result instanceof String)) {
                    return;
                }
                String patched = ArchiveTamper.tamperValue((String) key, (String) result, "getString");
                if (patched != null) {
                    param.setResult(patched);
                }
            }
        }, String.class, String.class);

        // 3) 写入（存盘）——把要落盘的值也换成篡改结果，保证持久化
        if (CrackConfig.PATCH_ON_WRITE) {
            hookMethod(cl, SP_EDITOR_IMPL, "putString", new XC_MethodHook() {
                @Override
                protected void beforeHookedMethod(MethodHookParam param) {
                    if (param.args.length < 2) {
                        return;
                    }
                    Object key = param.args[0];
                    Object value = param.args[1];
                    if (!(key instanceof String) || !(value instanceof String)) {
                        return;
                    }
                    String patched = ArchiveTamper.tamperValue((String) key, (String) value, "putString");
                    if (patched != null) {
                        param.args[1] = patched;
                    }
                }
            }, String.class, String.class);
        }

        // 4) 兜底：反作弊封号会走 PlayerPrefs.DeleteAll() -> Editor.clear()
        if (CrackConfig.BLOCK_WIPE) {
            hookMethod(cl, SP_EDITOR_IMPL, "clear", new XC_MethodHook() {
                @Override
                protected void beforeHookedMethod(MethodHookParam param) {
                    XposedBridge.log(TAG + ": blocked SharedPreferences.Editor.clear()");
                    param.setResult(param.thisObject);
                }
            });
            hookMethod(cl, SP_EDITOR_IMPL, "remove", new XC_MethodHook() {
                @Override
                protected void beforeHookedMethod(MethodHookParam param) {
                    if (param.args.length >= 1 && ArchiveTamper.isArchiveKey((String) param.args[0])) {
                        XposedBridge.log(TAG + ": blocked Editor.remove(archive)");
                        param.setResult(param.thisObject);
                    }
                }
            }, String.class);
        }

        // 5) 免登陆：native 驱动热更层 UserSystemWindow.Complete（跳过微信登录）
        try {
            NativeBridge.install(cl);
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": NativeBridge.install failed -> " + t);
        }

        XposedBridge.log(TAG + ": hooks installed");
    }

    /**
     * 统一挂钩子：优先用包自身的 ClassLoader，失败再退回系统 ClassLoader。
     * 任何一步失败都只记日志，绝不让模块把宿主进程带崩。
     */
    private static void hookMethod(ClassLoader cl, String className, String method,
                                   XC_MethodHook callback, Object... paramTypes) {
        // findAndHookMethod 的变参约定是【参数类型..., 回调】，回调必须在最后一位
        Object[] typesAndCallback = new Object[paramTypes.length + 1];
        System.arraycopy(paramTypes, 0, typesAndCallback, 0, paramTypes.length);
        typesAndCallback[paramTypes.length] = callback;

        try {
            XposedHelpers.findAndHookMethod(className, cl, method, typesAndCallback);
            XposedBridge.log(TAG + ": hooked " + className + "#" + method);
            return;
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": hook " + className + "#" + method
                    + " with pkg cl failed (" + t + "), retry with system cl");
        }
        try {
            XposedHelpers.findAndHookMethod(className, ClassLoader.getSystemClassLoader(),
                    method, typesAndCallback);
            XposedBridge.log(TAG + ": hooked " + className + "#" + method + " (system cl)");
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": hook " + className + "#" + method + " FAILED -> " + t);
        }
    }
}

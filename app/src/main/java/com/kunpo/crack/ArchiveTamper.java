package com.kunpo.crack;

import android.util.Base64;

import org.json.JSONArray;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

import javax.crypto.Cipher;
import javax.crypto.spec.SecretKeySpec;

import de.robv.android.xposed.XposedBridge;

/**
 * 存档篡改核心。
 *
 * 目标游戏的本地存档由热更 DLL Logic.dll 里的
 *   Arvin.PlayerPrefsUtilities.SimplePlayerPrefs
 * 处理：
 *
 *   player/provider.Key  = Encoding.ASCII.GetBytes(":{j%6j?E:t#}G10mM%9hp5S=%}2,Y26C"); // 32B -> AES-256
 *   player/provider.Mode = CipherMode.ECB;                                             // PKCS7
 *   输出 = Convert.ToBase64String(encrypted)
 *
 * 游戏把"键名"本身也做了同样的加密，于是 PlayerPrefs 里出现的是两串 Base64：
 *
 *   EncryptString("PlayerArchive")    = "KFB+U2vEDFEOsMTnW2BrzA=="
 *        -> GameArchives.InitPlayerArchive() 读入并 JsonUtility 反序列化成 FULA
 *           （钻石 FULA.dd、海洋 FULA.uls、强化点 FULA.AddPointData、道具 FULA.props ...）
 *
 *   EncryptString("FishStateArchive") = "qf2E6E69BTo8ZDcKPL8nTkFjfCljFL4eghaTN2mjlvc="
 *        -> GameArchives.InitFishState() 读入反序列化成 AFS（鱼类收集状态）
 *
 * 安卓端 UnityEngine.PlayerPrefs 由 libunity.so 通过 JNI 直接落到
 * android.app.SharedPreferencesImpl（getString / getAll / Editor.putString），
 * 因此在这一层拦截读写即可覆盖游戏全部本地存档读写。
 */
public final class ArchiveTamper {

    private static final String TAG = CrackConfig.TAG;

    /** SimplePlayerPrefs.key —— 32 字节 ASCII，直接当 AES-256 密钥 */
    private static final String KEY = ":{j%6j?E:t#}G10mM%9hp5S=%}2,Y26C";

    /** EncryptString("PlayerArchive") */
    public static final String SP_KEY_PLAYER = "KFB+U2vEDFEOsMTnW2BrzA==";
    /** EncryptString("FishStateArchive") */
    public static final String SP_KEY_FISH = "qf2E6E69BTo8ZDcKPL8nTkFjfCljFL4eghaTN2mjlvc=";

    /** 同一份密文（游戏读档往往一次读多次）只做一次解密+改+加密 */
    private static final int CACHE_MAX = 32;
    private static final Map<String, String> CACHE = new ConcurrentHashMap<>();

    private ArchiveTamper() {
    }

    // ------------------------------------------------------------------
    // 对外入口
    // ------------------------------------------------------------------

    public static boolean isArchiveKey(String key) {
        return SP_KEY_PLAYER.equals(key) || SP_KEY_FISH.equals(key);
    }

    /**
     * 处理单个 PlayerPrefs 键值。
     *
     * @return 篡改后的密文；不需要篡改或出错时返回 null（调用方保持原值）
     */
    public static String tamperValue(String key, String cipherText, String from) {
        if (cipherText == null || cipherText.isEmpty() || !isArchiveKey(key)) {
            return null;
        }
        String hit = CACHE.get(cipherText);
        if (hit != null) {
            return hit;
        }
        try {
            JSONObject obj = new JSONObject(decrypt(cipherText));
            if (SP_KEY_PLAYER.equals(key)) {
                if (!CrackConfig.PATCH_PLAYER) {
                    return null;
                }
                tamperPlayerArchive(obj);
            } else {
                if (!CrackConfig.PATCH_FISH) {
                    return null;
                }
                tamperFishArchive(obj);
            }
            String out = encrypt(obj.toString());
            if (CACHE.size() >= CACHE_MAX) {
                CACHE.clear();
            }
            CACHE.put(cipherText, out);
            XposedBridge.log(TAG + "[" + from + "] patched "
                    + (SP_KEY_PLAYER.equals(key) ? "PlayerArchive" : "FishStateArchive")
                    + " in=" + cipherText.length() + "B out=" + out.length() + "B");
            return out;
        } catch (Throwable t) {
            XposedBridge.log(TAG + "[" + from + "] tamper failed key=" + key + " : " + t);
            return null;
        }
    }

    /** 处理 SharedPreferencesImpl.getAll() / getLong() 等返回的整张表 */
    @SuppressWarnings("unchecked")
    public static boolean tamperMap(Map<?, ?> raw, String from) {
        if (raw == null || raw.isEmpty()) {
            return false;
        }
        boolean changed = false;
        Map<?, ?> snapshot = new HashMap<>(raw);
        for (Map.Entry<?, ?> e : snapshot.entrySet()) {
            Object k = e.getKey();
            Object v = e.getValue();
            if (!(k instanceof String) || !(v instanceof String)) {
                continue;
            }
            String patched = tamperValue((String) k, (String) v, from);
            if (patched != null) {
                ((Map<String, Object>) raw).put((String) k, patched);
                changed = true;
            }
        }
        return changed;
    }

    // ------------------------------------------------------------------
    // FULA（主档）
    //
    // 每一项都要对着 GameArchives.ServerDownloadArchive / Kunpo.Cheat 的判定来定，
    // 目的是让"服务器存档覆盖本地"的所有分支都不成立。
    // ------------------------------------------------------------------

    private static void tamperPlayerArchive(JSONObject o) {
        // GameArchives.Diamond => _fula.dd
        putInt(o, "dd", CrackConfig.DIAMOND);
        // ServerDownloadArchive 第一分支：_fula.banCount < banCount 时服务器覆盖
        putInt(o, "banCount", CrackConfig.BAN_COUNT);
        // CheckArchiveTime：onlineArchive.ts > _fula.ts 时服务器覆盖
        o.remove("ts");
        putLong(o, "ts", CrackConfig.FUTURE_TS);
        // 累充金额（分）
        putInt(o, "rmb", CrackConfig.RMB);
        // 最大分数（长整型）
        putLong(o, "fcMax", 999999999L);

        // 各类累计次数：全部拉到上限，只影响"能做什么"，不参与任何反作弊判定
        String[] counters = {
                "ads", "adTimes", "shareTimes", "fcg", "acg", "ST3", "SST3",
                "bbox", "bbAD", "spad", "efad", "dcdb", "adstc", "rstc",
                "lotteryTimes", "AddDTimes", "ReDTimes", "pzDraw", "yyl",
                "lgr", "tts", "ttc", "dld",
        };
        for (String c : counters) {
            putInt(o, c, CrackConfig.COUNTER);
        }

        // ---- 强化点 GameAddPointData ----
        // CheckAddPointCount: RemainPoint + ΣAllcotionPoint 必须大于服务器值
        JSONObject ap = o.optJSONObject("AddPointData");
        if (ap == null) {
            ap = new JSONObject();
        }
        putInt(ap, "UsePoint", 0);
        putInt(ap, "RemainPoint", CrackConfig.ADD_POINT_REMAIN);
        putInt(ap, "AdPoint", CrackConfig.COUNTER);
        putInt(ap, "PayPoint", CrackConfig.COUNTER);
        JSONArray points = new JSONArray();
        for (int i = 0; i < 6; i++) {
            // GetAllcotionPoint 内部对单值裁剪为 70，超过无效
            points.put(CrackConfig.ADD_POINT_ALLOC);
        }
        ap.remove("AllcotionPoint");
        putQuietly(ap, "AllcotionPoint", points);
        if (!ap.has("LookAdTime")) {
            putQuietly(ap, "LookAdTime", new JSONArray());
        }
        o.remove("AddPointData");
        putQuietly(o, "AddPointData", ap);

        // ---- 海洋解锁 ----
        // NewArchive 初始只给 2001；GetUnlockSeaMaxID() 取 uls 的最后一项，
        // 所以必须升序补齐并且确保 2012 在最末。
        Set<Integer> seas = new HashSet<>();
        List<Integer> seaList = new ArrayList<>();
        JSONArray old = o.optJSONArray("uls");
        if (old != null) {
            for (int i = 0; i < old.length(); i++) {
                int v = old.optInt(i, -1);
                if (v > 0 && v <= CrackConfig.SEA_TO && seas.add(v)) {
                    seaList.add(v);
                }
            }
        }
        for (int s = CrackConfig.SEA_FROM; s <= CrackConfig.SEA_TO; s++) {
            if (seas.add(s)) {
                seaList.add(s);
            }
        }
        JavaSort.ascending(seaList);
        JSONArray uls = new JSONArray();
        for (int s : seaList) {
            uls.put(s);
        }
        o.remove("uls");
        putQuietly(o, "uls", uls);

        // ---- 道具 ----
        JSONArray props = o.optJSONArray("props");
        if (props != null) {
            for (int i = 0; i < props.length(); i++) {
                JSONObject p = props.optJSONObject(i);
                if (p != null) {
                    putInt(p, "num", CrackConfig.PROP_NUM);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // AFS（鱼类收集档）
    // ------------------------------------------------------------------

    private static void tamperFishArchive(JSONObject o) {
        JSONArray afs = o.optJSONArray("afs");
        if (afs == null) {
            afs = new JSONArray();
        }
        Set<Integer> have = new HashSet<>();
        for (int i = 0; i < afs.length(); i++) {
            JSONObject f = afs.optJSONObject(i);
            if (f == null) {
                continue;
            }
            have.add(f.optInt("id"));
            boostFish(f);
        }
        for (int id : CrackConfig.FISH_IDS) {
            if (have.contains(id)) {
                continue;
            }
            JSONObject f = new JSONObject();
            putInt(f, "id", id);
            boostFish(f);
            afs.put(f);
        }
        o.remove("afs");
        putQuietly(o, "afs", afs);
        // AFS.ltR 与 FULA.ts 同理（存档时间对比）
        o.remove("ltR");
        putLong(o, "ltR", CrackConfig.FUTURE_TS);
    }

    /**
     * 单条 FS。必须写全所有字段：Unity 的 JsonUtility 对缺失字段不回填字段初始值，
     * FS.sFm 一旦是 null，CheckFishSkin 里 foreach (item.sFm) 会直接空引用崩溃。
     */
    private static void boostFish(JSONObject f) {
        putInt(f, "cfs", CrackConfig.FISH_STATE_COLLECTED);
        putInt(f, "kn", CrackConfig.FISH_KN);
        putInt(f, "dn", CrackConfig.FISH_DN);
        putInt(f, "an", CrackConfig.FISH_AN);
        // CheckFishSkillCount 求和的是 lv，本地必须 >= 服务器
        putInt(f, "lv", CrackConfig.FISH_LV);
        // CheckFishSkin：ΣsFm + (sn>0 ? 1 : 0)，本地必须 >= 服务器
        putInt(f, "sn", CrackConfig.FISH_SN);
        f.remove("bN");
        putQuietly(f, "bN", Boolean.TRUE);
        f.remove("bHR");
        putQuietly(f, "bHR", Boolean.TRUE);
        f.remove("lR");
        putQuietly(f, "lR", Boolean.TRUE);
        if (!f.has("sI")) {
            putInt(f, "sI", 0);
        }
        if (!f.has("mS")) {
            putInt(f, "mS", 0);
        }
        if (!f.has("sFm")) {
            putQuietly(f, "sFm", new JSONArray());
        }
    }

    // ------------------------------------------------------------------
    // 工具
    // ------------------------------------------------------------------

    private static void putInt(JSONObject o, String name, int value) {
        o.remove(name);
        putQuietly(o, name, Integer.valueOf(value));
    }

    private static void putLong(JSONObject o, String name, long value) {
        o.remove(name);
        putQuietly(o, name, Long.valueOf(value));
    }

    private static void putQuietly(JSONObject o, String name, Object value) {
        try {
            o.put(name, value);
        } catch (Throwable ignored) {
        }
    }

    private static void putQuietly(JSONArray a, Object value) {
        try {
            a.put(value);
        } catch (Throwable ignored) {
        }
    }

    /** AES-256-ECB/PKCS7 → Base64（与 C# Convert.ToBase64String 输出一致） */
    static String encrypt(String plain) throws Exception {
        Cipher c = Cipher.getInstance("AES/ECB/PKCS5Padding");
        c.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(KEY.getBytes(StandardCharsets.US_ASCII), "AES"));
        byte[] out = c.doFinal(plain.getBytes(StandardCharsets.UTF_8));
        return Base64.encodeToString(out, Base64.NO_WRAP);
    }

    static String decrypt(String base64Cipher) throws Exception {
        Cipher c = Cipher.getInstance("AES/ECB/PKCS5Padding");
        c.init(Cipher.DECRYPT_MODE, new SecretKeySpec(KEY.getBytes(StandardCharsets.US_ASCII), "AES"));
        byte[] out = c.doFinal(Base64.decode(base64Cipher, Base64.DEFAULT));
        return new String(out, StandardCharsets.UTF_8);
    }

    /** 避免依赖 java.util.Collections 的泛型推断，保持与桌面验证脚本一致的排序 */
    static final class JavaSort {
        private JavaSort() {
        }

        static void ascending(List<Integer> list) {
            list.sort((a, b) -> Integer.compare(a, b));
        }
    }
}

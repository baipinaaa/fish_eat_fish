package com.kunpo.crack;

/**
 * 破解参数总表。所有取值都经过 Logic.dll 反编译源码逐条核对，
 * 保证不会被游戏自身的"服务器存档覆盖本地"逻辑（GameArchives.ServerDownloadArchive）
 * 以及 Kunpo.Cheat 反作弊链判成"服务器更好"从而覆盖回去。
 *
 * 结论摘要（详见 README.md）：
 *  - 客户端反作弊只上报 TDAnalytics 埋点，不封号；
 *  - 唯一的封号动作（PlayerPrefs.DeleteAll + ClearArchive）被 IsOpenCheat() 挡住，
 *    而 IsOpenCheat() = Remote.OpenCheat && int.Parse(version[2]) >= Remote.LessOpenCheatVersion
 *    = true && (8 >= 133) = false —— 在 versionName 1.1.8 上恒为 false，即封号路径是死代码。
 */
public final class CrackConfig {

    private CrackConfig() {
    }

    /** 目标包名（AndroidManifest 中 package="com.kunpo.bigfish"） */
    public static final String TARGET_PACKAGE = "com.kunpo.bigfish";

    public static final String TAG = "FishCrack";

    // ------------------------------------------------------------------
    // 开关
    // ------------------------------------------------------------------

    /** 主档 FULA（钻石/海洋/强化点/次数） */
    public static final boolean PATCH_PLAYER = true;
    /** 鱼类收集档 AFS */
    public static final boolean PATCH_FISH = true;
    /** 读存档时篡改（必开，否则内存里拿到的还是原档） */
    public static final boolean PATCH_ON_READ = true;
    /** 写存档时篡改（必开，保证落盘的就是篡改后的档） */
    public static final boolean PATCH_ON_WRITE = true;
    /** 拦掉 PlayerPrefs.DeleteAll()/Editor.clear()（反封号兜底） */
    public static final boolean BLOCK_WIPE = true;

    // ------------------------------------------------------------------
    // 数值目标
    // ------------------------------------------------------------------

    /** 钻石 FULA.dd -> GameArchives.Diamond */
    public static final int DIAMOND = 99999999;

    /**
     * FULA.banCount。ServerDownloadArchive 第一分支：
     *   if (_fula.banCount < banCount) { 用服务器覆盖本地 }
     * 抬高即可让服务器永远无法覆盖。
     */
    public static final int BAN_COUNT = 99999999;

    /**
     * FULA.ts / AFS.ltR。CheckArchiveTime:
     *   if (onlineArchive.ts > _fula.ts) { 用服务器覆盖本地 }
     */
    public static final long FUTURE_TS = 9999999999999L;

    /** 累充金额（分）FULA.rmb */
    public static final int RMB = 99999999;

    /** 各种累计次数类字段统一拉到这个值（越大越有利，不影响任何判定） */
    public static final int COUNTER = 999999;

    /** 强化点 FULA.AddPointData —— RemainPoint + ΣAllcotionPoint 必须大于服务器值 */
    public static final int ADD_POINT_REMAIN = 999999;
    /** 单项分配点上限，GameAddPointData.GetAllcotionPoint 内部裁剪为 70，超过无效 */
    public static final int ADD_POINT_ALLOC = 70;

    /** 道具数量 FULA.props[].num */
    public static final int PROP_NUM = 999999;

    // ------------------------------------------------------------------
    // 海洋：ConstConfig.CanSelectLastSea = 2012，NewArchive 初始只给 2001
    // GetUnlockSeaMaxID() => _fula.uls[uls.Count - 1]，必须升序且末位最大
    // ------------------------------------------------------------------

    public static final int SEA_FROM = 2001;
    public static final int SEA_TO = 2012;

    // ------------------------------------------------------------------
    // 鱼类 id 表
    //
    // 来源：APK 内 Addressables 打包命名
    //   assets/aa/Android/battlefish_assets_f_<id>_<hash>.bundle
    // 共 64 个（2001 / 3001-3015 / 3501-3556 区间），与原 fish_ids.txt 一致。
    // ------------------------------------------------------------------

    public static final int[] FISH_IDS = {
            2001,
            3001, 3002, 3003, 3004, 3005, 3006, 3007, 3008, 3009, 3010,
            3011, 3012, 3013, 3014, 3015,
            3501, 3504, 3505, 3506, 3507, 3508, 3509, 3511, 3512, 3513,
            3514, 3515, 3516, 3517, 3518, 3519, 3520, 3521, 3522, 3523,
            3524, 3525, 3526, 3527, 3528, 3529, 3530, 3531, 3532, 3533,
            3534, 3535, 3536, 3537, 3538, 3543, 3544, 3545, 3546, 3547,
            3548, 3549, 3551, 3552, 3553, 3554, 3555, 3556,
    };

    /** FS.cfs == 1 表示已收集（CheckLocalFishSize 统计的就是 cfs==1 的条数） */
    public static final int FISH_STATE_COLLECTED = 1;
    /** FS.lv —— CheckFishSkillCount 求和的是 lv，本地必须大于服务器 */
    public static final int FISH_LV = 999;
    /** FS.kn / dn / an */
    public static final int FISH_KN = 999999;
    public static final int FISH_DN = 999999;
    public static final int FISH_AN = 999999;
    /** FS.sn —— CheckFishSkin 里 sn>0 记 1 点 */
    public static final int FISH_SN = 1;
}

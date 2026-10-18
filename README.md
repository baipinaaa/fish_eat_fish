# FishCrack

针对 `com.kunpo.bigfish`（鱼吃鱼 / Unity 2021.3.10f1 + il2cpp + HybridCLR 热更）的 **Xposed 本地存档篡改模块**。

> ⚠️ 仅用于安全研究 / 防护评估。作者不对任何滥用负责。

---

## 1. 破解点是怎么找到的

| 阶段 | 结论 |
|---|---|
| APK 结构 | `com.kunpo.bigfish-84.apk`（757 MB，versionCode 84 / versionName 1.1.8 / targetSdk 34）。Unity 2021.3.10f1 + il2cpp，游戏**主逻辑不在首包**，而是热更 DLL。 |
| 热更形态 | `assets/hotcode/*.bytes` = 15 个明文 PE（首包 DLL）；`assets/core.bytes` = 追加的 DLL 合集（**Logic.dll，游戏主体逻辑**）。两者都是明文 .NET 程序集，可直接反编译出全部 C# 源码（未随本仓库分发）。 |
| 存档实现 | `Logic.dll` → `Arvin.PlayerPrefsUtilities.SimplePlayerPrefs`：先用 **AES-256-ECB/PKCS7 + Base64** 加密数据，再交给 `UnityEngine.PlayerPrefs`；连 PlayerPrefs 的 **key 本身**也是 `EncryptString()` 的结果。 |
| 关键常量 | `SimplePlayerPrefs.key = ":{j%6j?E:t#}G10mM%9hp5S=%}2,Y26C"`（32 字节 ASCII → AES-256 密钥，ECB 无 IV）。 |
| 存档条目 | `EncryptString("PlayerArchive")` = `KFB+U2vEDFEOsMTnW2BrzA==` → 主档 `FULA`（钻石 `dd`、`rmb`、海洋 `uls`、强化点 `AddPointData`、道具 `props` …）<br>`EncryptString("FishStateArchive")` = `qf2E6E69BTo8ZDcKPL8nTkFjfCljFL4eghaTN2mjlvc=` → 鱼类收集档 `AFS`（`afs` 列表、`ltR`） |
| 键名可自检 | 这两个 Base64 常量能直接用上面的 key 用 AES-256-ECB 解出 `PlayerArchive` / `FishStateArchive` 字符串，算法判定无误（见 `verify_libs/verify_out.py` 第 0 项断言）。 |
| 有没有服务端保护 | **有，但只做单调比较**。`GameArchives.ServerDownloadArchive()` 仅在「服务器值更大」时才用服务器覆盖本地；本地档为空（`IsNewArchive()`，即 `afs.Count == 0`）时才无条件采用服务器档。**没有签名校验、没有反调试、没有 so 完整性校验。** 所以：把本地各项数值抬到服务器之上，服务器就永远覆盖不掉。 |
| 拦截位置 | `libunity.so` 内部字符串含 `getSharedPreferences` / `.v2.playerprefs` / `getAll` / `getString` / `SharedPreferences$Editor` → Unity 的 `PlayerPrefs` 在 Android 上就是落到 `android.app.SharedPreferencesImpl`（`/data/data/com.kunpo.bigfish/shared_prefs/*.v2.playerprefs.xml`）。Unity 启动时用 `getAll()` 一次性读进 native 缓存，之后读内存、写回走 `Editor.putString + commit`。**在 `getAll()` 出口改一次，整局生效；游戏自己 Save 时会把改过的值写回 XML，篡改结果自动持久化。** |

### 1.1 反作弊链（取值必须满足的不等式）

`ServerDownloadArchive()` 逐项比对，任一「服务器更优」就整体覆盖本地档。为避免被覆盖回去，篡改值必须让下面这些不等式全部成立：

| 检查函数 | 判据 | 破解取值 |
|---|---|---|
| `banCount` 分支 | `_fula.banCount < server.banCount` → 覆盖 | `banCount = 99999999` |
| `CheckArchiveTime` | `server.ts > _fula.ts` → 覆盖 | `ts = ltR = 9999999999999` |
| `CheckLocalFishSize` | 统计 `cfs == 1` 的条数，服务器多则覆盖 | 64 条真实鱼全 `cfs = 1` |
| `CheckFishSkin` | `sn > 0` 记 1 点（并**遍历 `sFm`**，为 null 会空引用） | `sn = 1`，且 `sFm` 保持非 null 数组 |
| `CheckFishSkillCount` | 对 `lv` 求和，服务器大则覆盖 | `lv = 999` |
| `CheckLocalCanUnLockFishSize` | 可解锁鱼数比较 | 直接补全全部 `id` |
| 强化点 | `RemainPoint + ΣAllcotionPoint` 需大于服务器 | `RemainPoint = 999999`，`AllcotionPoint = [70]*6`（内部裁剪上限 70，超了无效） |
| 钻石 | `_fula.dd < server.dd` → 覆盖 | `dd = 99999999` |

### 1.2 顺带发现：客户端封号路径是**死代码**

`Kunpo.Cheat` 里的封号动作（`PlayerPrefs.DeleteAll()` + `ClearArchive()`）被 `IsOpenCheat()` 挡住，而：

```
IsOpenCheat() = Remote.OpenCheat && int.Parse(version[2]) >= Remote.LessOpenCheatVersion
              = true && (8 >= 133) = false        // versionName = 1.1.8
```

在 versionName `1.1.8` 上**恒为 false**，即无论客户端检测到什么，封号分支都不会执行。这也解释了为什么流传的破解版能一直用。客户端作弊检测只往 TDAnalytics 上报埋点，不做任何本地惩罚。

---

## 2. 模块做了什么

Xposed 钩住 `android.app.SharedPreferencesImpl`（四处，逐层加防御）：

| 钩子 | 作用 |
|---|---|
| `getAll()` | Unity 启动整表读取 → 解密 → 改 JSON → 重新加密 → `setResult`。**主路径。** |
| `getString(String, String)` | 逐键读取路径，兜底。 |
| `EditorImpl.putString(String, String)` | 游戏存盘时拦截，保证**落盘的就是篡改后的档**，不被原始值覆盖。ECB 确定性加密 → 同一明文永远得到同一密文，因此重复篡改是幂等的。 |
| `EditorImpl.clear()` / `remove(String)` | 兜底拦掉 `DeleteAll()` 清档（防封号 + 防误清）。 |

改的内容：

**主档 `FULA`**
- `dd`（钻石）= `99999999`，`rmb` = `99999999`
- `banCount` = `99999999`，`ts` = `9999999999999`（= `ltR`）→ **免疫服务器存档覆盖**
- `AddPointData`：`RemainPoint` / `AdPoint` / `PayPoint` = `999999`，6 条 `AllcotionPoint` 各 `70`
- `uls` 补齐为 `2001..2012`（升序，因为 `GetUnlockSeaMaxID()` 取末项；`ConstConfig.CanSelectLastSea = 2012`）
- `props[].num` 全部拉满 `999999`
- 其余累计次数类字段统一 `999999`

**鱼类档 `AFS`**
- 已有条目全部提升：`cfs=1`、`kn=dn=an=999999`、`lv=999`、`sn=1`、`bN=bHR=lR=true`、`sI=mS=0`
- **`sFm` 保持原值不动**（只在缺失时补成 `[]`，绝不写 null，因为 `CheckFishSkin` 会遍历它）
- 按真实鱼 id 表（64 个，见下）补齐缺失条目，使上面所有「服务器更优」判断全部失败
- `ltR` 抬到 `9999999999999`

**鱼 id 表（64 个）**：来自 APK 内 Addressables 打包命名 `assets/aa/Android/battlefish_assets_f_<id>_<hash>.bundle`，区间为 `2001` / `3001–3015` / `3501–3556`（不连续）。已硬编码进 `CrackConfig.FISH_IDS`。

任何一步异常都被吞掉并打日志，原值照常返回，**不会让游戏崩溃**。

---

## 3. 使用

1. 设备已 root，装有 **LSPosed**（或 EdXposed / 原版 Xposed）。
2. 安装构建出的 `app-debug.apk`。
3. LSPosed 里启用模块，**作用域勾选 `com.kunpo.bigfish`**（若进程名带后缀，按实际进程勾）。
4. 强制停止游戏再启动。
5. 查看 LSPosed 日志，应出现：
   ```
   FishCrack: attached pkg=com.kunpo.bigfish ...
   FishCrack: hooked android.app.SharedPreferencesImpl#getAll
   FishCrack: hooked android.app.SharedPreferencesImpl#getString
   FishCrack: hooked android.app.SharedPreferencesImpl$EditorImpl#putString
   FishCrack[getAll] PlayerArchive patched: dd=99999999 ...
   FishCrack[getAll] FishStateArchive patched: entries=64 ...
   FishCrack: hooks installed
   ```

**首次生效建议**：先进游戏创建过一次存档（让 `*.v2.playerprefs.xml` 存在）后再开模块，效果最直观。

> 想调温和一点，直接改 `CrackConfig.java` 顶部的常量即可（`DIAMOND` / `SEA_TO` / `FISH_LV` / `PROP_NUM` …），或把 `BLOCK_WIPE`、`PATCH_ON_WRITE` 关掉。

---

## 4. 构建

### GitHub Actions（推荐，本仓库已带 workflow）

推送到 GitHub，`.github/workflows/build.yml` 会自动：
JDK 17 → Android SDK → Gradle 8.5 → `assembleDebug` → 上传 artifact `FishCrack-debug-apk`。

```bash
cd xposed_crack
git init
git add -A
git commit -m "FishCrack"
git remote add origin <你的仓库地址>
git push -u origin main
```

> 本机直连 GitHub 不通，git 已全局配置 `http.proxy=http://127.0.0.1:99`；若推送失败，确认该代理在跑。

### 本地构建
需要 JDK 17 + Android SDK（compileSdk 34）：
```bash
gradle assembleDebug
# 产物: app/build/outputs/apk/debug/app-debug.apk
```

依赖 `de.robv.android.xposed:api:82` 走 `maven.aliyun.com` 镜像（`api.xposed.info` 在部分网络不可达）。

---

## 5. 本地离线验证（不需要手机）

`verify_libs/` 下是无 Android 依赖的纯 JVM 复现，用来在编译前确认算法与取值正确：

* `CrackTest.java` —— 用与 `ArchiveTamper` 相同的 AES-256-ECB 实现，解密真实存档样本 → 跑篡改 → 打印结果。
* `verify_out.py` —— 再用 pycryptodome 独立解密输出档，逐条断言所有反作弊取值。
* `fish_ids.txt` —— 从 Addressables 提取的 64 个真实鱼 id。

```bash
cd work
python verify_libs/verify_out.py verify_libs/sample
# 输出末尾应为 ALL PASS
```

---

## 6. 已知局限 / 风险

* 只改**本地存档**。走服务端结算的行为（真实下单、服务端发放的邮件奖励）不受影响。
* `NetHelper.UploadArchive()` 会把篡改后的档推到服务器（格式 `{"uid":...,"archives":[{"name":"PlayerArchive","data":...},{"name":"FishArchive","data":...}]}`）。服务端目前只做单调比较，所以抬高数值即可规避；**但这条上传通道是日后加服务端校验的天然入口** —— 服务端只要对上报值做上限/合理性检查，本模块即失效。
* `Edge`/`sI`/`mS` 等字段按最有利方向取值，界面上「已收集鱼类」会显示为全满，与正常玩家不一致，容易被人工巡查发现。
* 不同 Android 版本 `android.app.SharedPreferencesImpl` 的私有方法名可能变化；日志里若看不到 `hooked ...` 行说明该机型 Hook 失败（`CrackHook.hookMethod` 会自动退回系统 ClassLoader 重试一次）。

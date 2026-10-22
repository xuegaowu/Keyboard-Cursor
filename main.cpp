// ============================================================
// 光标控制 - 键盘转鼠标映射工具
// ------------------------------------------------------------
// 核心思路：
//   1. 用 WH_KEYBOARD_LL 低级键盘钩子捕获按键，同时"拦截"映射键，
//      使其不再透传给前台窗口（游戏/软件收不到）。
//   2. 钩子回调只把按键"状态"写入 std::atomic 变量（不阻塞、不加锁）。
//   3. 独立线程按固定节拍轮询这些原子变量，做决策并调用 SendInput/
//      SetCursorPos 驱动鼠标。
//   4. 用组合键（1~4 个键同时按下）作为"总开关"，边沿触发切换启用/禁用。
//   5. 双存档体系：save0.ini 为隐式自动档（启动读、退出写），
//      save1..N.ini 为显式档（下拉框选择、手动保存/加载）。
//   6. 存档目录不可写时不再静默失败：可回退到用户目录，或降级为只读模式，
//      并在窗口底部状态行如实显示当前状态。
// ============================================================

// 包含 Windows API 头文件
#include <windows.h>
// 线程库（用于独立线程监听键盘）
#include <thread>
// 原子操作库（线程安全标志）
#include <atomic>
// 时间库（用于控制循环间隔）
#include <chrono>
// 字符串库
#include <string>
// 向量库（存档文件列表）
#include <vector>
// 算法库（排序）
#include <algorithm>
// 标准 C 库：sprintf_s / strcpy_s / atoi / strcmp / getenv 都声明在这里。
// 之前靠 <windows.h> 等头文件的间接包含，换编译器或换包含顺序就会报错。
#include <cstdio>
#include <cstdlib>
#include <cstring>
// 多媒体库：仅用于老系统兜底时的 timeBeginPeriod(1)。
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
// 注册表 API（RegOpenKeyExA / RegSetValueExA）在 advapi32 里。
// MSVC 需要这行；MinGW 默认就链接 advapi32，写了也会被忽略。
#pragma comment(lib, "advapi32.lib")

// ==================== 编译器兼容层 ====================
// MSVC 的 _s 安全函数在 MinGW/GCC 下没有对应声明。这里用宏把调用映射到标准 C 函数，
// 让同一份源码在两种编译器下都能编过。
// 所有调用点的缓冲区都是数组，所以 sizeof 能正确取到容量。
#ifdef __GNUC__
#define sprintf_s(buf, ...) snprintf((buf), sizeof(buf), __VA_ARGS__)
#define strcpy_s(dst, src)  lstrcpynA((dst), (src), (int)sizeof(dst))
#endif

using namespace std::chrono;

// 控制线程的轮询周期（毫秒）。取值原则：略高于目标显示器刷新周期。
//   6ms ≈ 167Hz，覆盖 144/165Hz；240Hz 屏改 4；60Hz 屏 8 都够。
// 注意：MOVE_SPEED 的单位是"每 tick 像素数"，改这个值会等比改变手感，
// 所以默认值按 6ms 标定，不再随显示器变化。
constexpr int TICK_MS = 6;

// ==================== 控件 ID ====================
// 每个子控件在窗口内的唯一标识。数字随意但须保证不重复，
// 因为 WM_COMMAND 通过 LOWORD(wParam) 判断是哪个控件发出的通知。
#define ID_EDIT_SPEED           3001   // 移动速度
#define ID_EDIT_SPEED_MUL       3002   // Shift加速
#define ID_EDIT_WHEEL_INTERVAL  3003   // 滚轮间隔
#define ID_EDIT_WHEEL_SPEED     3004   // 滚轮速度
#define ID_EDIT_WHEEL_MUL       3005   // 滚轮加速
#define ID_BTN_KEY_UP           3011   // 上按键
#define ID_BTN_KEY_DOWN         3012   // 下按键
#define ID_BTN_KEY_LEFT         3013   // 左按键
#define ID_BTN_KEY_RIGHT        3014   // 右按键
#define ID_BTN_KEY_LCLICK       3015   // 左键
#define ID_BTN_KEY_RCLICK       3016   // 右键
#define ID_BTN_KEY_MCLICK       3017   // 中键
#define ID_BTN_KEY_WHEELUP      3018   // 上滚
#define ID_BTN_KEY_WHEELDOWN    3019   // 下滚
#define ID_BTN_SWITCH1          3021   // 开关键1
#define ID_BTN_SWITCH2          3022   // 开关键2
#define ID_BTN_SWITCH3          3023   // 开关键3
#define ID_BTN_SWITCH4          3024   // 开关键4
#define ID_BTN_CLEAR_SWITCH     3025   // 清空开关键
#define ID_CHECK_INTERCEPT      3031   // 启用按键拦截
#define ID_CHECK_TRAY           3032   // 最小化到托盘
#define ID_CHECK_AUTOSTART      3033   // 开机自启动（写 HKCU 的 Run 键，不需要管理员）
#define ID_BTN_APPLY            3041   // 应用
#define ID_BTN_RESET            3042   // 重置
#define ID_TIMER_REFRESH        4001   // 定时器刷新按键显示

// 存档控件 ID
#define ID_COMBO_SAVE           3061   // 存档下拉框
#define ID_BTN_SAVE             3062   // 保存
#define ID_BTN_LOAD             3063   // 加载
#define ID_STATIC_STATUS        3051   // 底部状态提示（存档目录 / 只读 / 读取失败）

// 自定义消息
// WM_APP 是 Windows 留给"应用程序私用消息"的起点，
// 从这里往上加偏移即可避免与系统消息冲突。
#define WM_RESET (WM_APP + 2)          // 重置通知
#define WM_NEW_SAVE (WM_APP + 3)       // 新建存档通知
#define WM_BIND_KEY (WM_APP + 4)       // 按键捕获通知

// ==================== 可调变量 ====================
// 这些值既被 UI 线程修改，也被控制线程读取，因此用 atomic。
// （本例中都是 int，读写本身是原子的，但使用 atomic 可以保证
//   修改立刻对其它线程可见，且不对齐问题被规避。）
std::atomic<int> MOVE_SPEED(2);           // 每帧移动的像素数
std::atomic<int> SPEED_MULTIPLIER(3);     // 按住 Shift 时的速度倍率
std::atomic<int> WHEEL_INTERVAL(80);      // 两次滚轮事件的最小间隔（ms）
std::atomic<int> WHEEL_SPEED(1);          // 每次滚动的"档位"数
std::atomic<int> WHEEL_SPEED_MULTIPLIER(3); // 按住 Shift 时的滚轮倍率

// 按键映射（均为虚拟键码 VK_*，字母直接用其 ASCII 值）
// 用 atomic 是为了让捕获阶段（钩子线程）写入、控制线程读取时看不到半新半旧的值。
std::atomic<int> keyUp('E');         // 上移
std::atomic<int> keyDown('D');       // 下移
std::atomic<int> keyLeft('S');       // 左移
std::atomic<int> keyRight('F');      // 右移
std::atomic<int> keyLClick('J');     // 左键
std::atomic<int> keyRClick('K');     // 右键
std::atomic<int> keyMClick('M');     // 中键
std::atomic<int> keyWheelUp('O');    // 滚轮上
std::atomic<int> keyWheelDown('L');  // 滚轮下

// 开关组合键（0 表示未设置）
// 有效成员必须"全部同时按下"才视为触发。
// 0 是哨兵值：表示该槽位不参与判定，等价于"空位不要求"。
std::atomic<int> switchKey1(VK_CONTROL);
std::atomic<int> switchKey2(VK_SHIFT);
std::atomic<int> switchKey3(0);
std::atomic<int> switchKey4(0);

// 功能开关
std::atomic<bool> isRunning(true);            // 控制线程的运行标志，退出时置 false
std::atomic<bool> isEnabled(true);            // 总开关是否"启用"
std::atomic<bool> isMinimizeToTray(false);    // 关闭窗口时是否最小化到托盘
std::atomic<bool> interceptEnabled(true);     // 是否拦截映射键（关掉后不再吞键）

// ==================== 按键状态 ====================
// 钩子线程写入、控制线程读取。名字保持了历史含义（keyE/keyD...），
// 实际含义是"映射功能是否触发"，而不是"物理 E 键是否按下"。
std::atomic<bool> keyE(false), keyS(false), keyD(false), keyF(false);
std::atomic<bool> keyJ(false), keyK(false);
std::atomic<bool> keyO(false), keyL(false), keyM(false);
std::atomic<bool> keyShift(false), keyCtrl(false);

// 鼠标三键"当前是否处于按下状态"，用于幂等判断（同状态不重发）。
// 与控制线程配合，避免每帧重复发送 DOWN/UP 事件。
std::atomic<bool> mouseLeftDown(false);
std::atomic<bool> mouseRightDown(false);
std::atomic<bool> mouseMiddleDown(false);

// ==================== 全局句柄 ====================
HHOOK g_hKeyboardHook = NULL;   // 低级键盘钩子句柄，退出时 Unhook
HWND g_hMainWnd = NULL;         // 主窗口句柄，钩子里需要 PostMessage 会用到
NOTIFYICONDATAA g_nid = {};     // 托盘图标的数据结构

// 按键捕获状态
// 非 0 表示"正在等待用户按下一个键"，此时钩子不设置状态、不拦截
// 而是直接 PostMessage WM_BIND_KEY 给主线程去更新映射。
std::atomic<int> waitingButtonId(0);   // 正在等待按键的按钮 ID（0=无）

// 存档相关
std::string g_saveDir;                  // save/ 目录路径
std::vector<std::string> g_saveFiles;   // 存档文件名列表（不含路径，不含扩展名）
                                        // 注意：不包含 save0（它是隐式自动档）。
bool g_saveWritable = true;             // false = 只读模式：不写盘、也不谎报成功
bool g_saveDirFallback = false;         // true = 已回退到 %LOCALAPPDATA% 下的目录
bool g_backupBeforeSave = false;        // 读档失败过：下次保存前先备份原文件

// ==================== 按键名称转换 ====================
// 把虚拟键码翻译成 UI 上显示的文字。
// 常见键手写名字（保证中文界面统一），其余交给系统翻译，最后兜底显示键码。
std::string GetKeyName(int vk) {
    if (vk == 0) return "无";
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    switch (vk) {
        case VK_CONTROL: return "Ctrl";
        case VK_SHIFT: return "Shift";
        case VK_MENU: return "Alt";
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_TAB: return "Tab";
        case VK_ESCAPE: return "Esc";
        case VK_BACK: return "Backspace";
        case VK_DELETE: return "Delete";
        case VK_INSERT: return "Insert";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_LEFT: return "左方向";
        case VK_RIGHT: return "右方向";
        case VK_UP: return "上方向";
        case VK_DOWN: return "下方向";
        case VK_F1: return "F1";
        case VK_F2: return "F2";
        case VK_F3: return "F3";
        case VK_F4: return "F4";
        case VK_F5: return "F5";
        case VK_F6: return "F6";
        case VK_F7: return "F7";
        case VK_F8: return "F8";
        case VK_F9: return "F9";
        case VK_F10: return "F10";
        case VK_F11: return "F11";
        case VK_F12: return "F12";
        // 锁定键：以前落到 default 全显示 "Key"，用户根本分不清。
        case VK_CAPITAL: return "CapsLock";
        case VK_NUMLOCK: return "NumLock";
        case VK_SCROLL: return "ScrollLock";
    }

    // 其余键交给系统翻译：小键盘、OEM 键、F13+ 都能拿到本地化名称。
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc != 0) {
        LONG keyParam = (LONG)(sc << 16);
        // 方向键、Home/End 等是"扩展键"，不带扩展位会被查成小键盘上的同名键。
        switch (vk) {
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
            case VK_INSERT: case VK_DELETE: case VK_DIVIDE:
                keyParam |= (1 << 24);
                break;
        }
        char name[64];
        if (GetKeyNameTextA(keyParam, name, 64) > 0) return name;
    }

    // 实在查不到就显示键码，总比一堆重名 "Key" 强。
    char hex[16];
    sprintf_s(hex, "Key%02X", vk);
    return hex;
}

// 更新按钮文字
// 只更新单个按钮；调用者通常在知道具体改了哪个映射后调用。
void UpdateButtonText(HWND hwnd, int btnId, int vk) {
    std::string name = GetKeyName(vk);
    SetDlgItemTextA(hwnd, btnId, name.c_str());
}

// 更新所有按键按钮文字
// 在"重置""清空""加载存档""按键捕获完成"等会大面积改动映射的场合调用。
void UpdateAllButtonText(HWND hwnd) {
    UpdateButtonText(hwnd, ID_BTN_KEY_UP, keyUp.load());
    UpdateButtonText(hwnd, ID_BTN_KEY_DOWN, keyDown.load());
    UpdateButtonText(hwnd, ID_BTN_KEY_LEFT, keyLeft.load());
    UpdateButtonText(hwnd, ID_BTN_KEY_RIGHT, keyRight.load());
    UpdateButtonText(hwnd, ID_BTN_KEY_LCLICK, keyLClick.load());
    UpdateButtonText(hwnd, ID_BTN_KEY_RCLICK, keyRClick.load());
    UpdateButtonText(hwnd, ID_BTN_KEY_MCLICK, keyMClick.load());
    UpdateButtonText(hwnd, ID_BTN_KEY_WHEELUP, keyWheelUp.load());
    UpdateButtonText(hwnd, ID_BTN_KEY_WHEELDOWN, keyWheelDown.load());
    UpdateButtonText(hwnd, ID_BTN_SWITCH1, switchKey1.load());
    UpdateButtonText(hwnd, ID_BTN_SWITCH2, switchKey2.load());
    UpdateButtonText(hwnd, ID_BTN_SWITCH3, switchKey3.load());
    UpdateButtonText(hwnd, ID_BTN_SWITCH4, switchKey4.load());
}

// 底部状态行：正常时显示存档路径，异常时显示原因。
// 放在窗口底部固定一行，避免再往标题栏塞信息。
void SetStatus(HWND hwnd, const std::string& text) {
    SetDlgItemTextA(hwnd, ID_STATIC_STATUS, text.c_str());
}

// ==================== 存档路径 ====================
// 获取 exe 所在目录
std::string GetExeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string exePath(buf);
    size_t pos = exePath.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string(".") : exePath.substr(0, pos);
}

// 获取 save/ 目录路径
// 固定策略：与 exe 同目录下的 save/ 子目录，与 exe 是否在系统盘无关。
// 这样拷到 U 盘也能"带着存档走"。
std::string GetSaveDir() {
    return GetExeDir() + "\\save";
}

// 确保目录存在且真的能写
// CreateDirectory 成功不等于可写（只读介质、组策略、杀毒软件都可能拦），
// 所以实际写一个临时文件来验证，写完立刻删掉。
bool EnsureDirWritable(const std::string& dir) {
    if (!CreateDirectoryA(dir.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    std::string probe = dir + "\\.write_test";
    HANDLE h = CreateFileA(probe.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    DeleteFileA(probe.c_str());
    return true;
}

// 用户目录下的备选存档路径：%LOCALAPPDATA%\KeyboardCursor\save
// 放在用户配置目录里，普通权限即可写，不受 Program Files 的只读限制。
std::string GetLocalSaveDir() {
    // 用 getenv 而不是 MSVC 专有的 _dupenv_s，这样 GCC / MSVC 都能编。
    // getenv 返回的是 CRT 内部缓冲区，调用方不能也不该 free。
    const char* p = getenv("LOCALAPPDATA");
    std::string base = (p != nullptr) ? p : "";
    if (base.empty()) return GetSaveDir();   // 极端情况退回原目录
    return base + "\\KeyboardCursor\\save";
}

// 决定最终使用的存档目录
//   1) exe\save 可写        → 正常模式
//   2) 不可写，用户同意回退  → %LOCALAPPDATA% 下的目录
//   3) 不可写，用户不同意    → 只读模式（程序照常运行，只是不写盘）
// 这一步必须在创建窗口之前完成：WM_CREATE 里就要扫描目录、读 save0.ini，
// 否则会有一段"目录还没定"的时间处于无保护状态。
void ResolveSaveDir() {
    std::string exeSave = GetSaveDir();
    if (EnsureDirWritable(exeSave)) {
        g_saveDir = exeSave;
        g_saveWritable = true;
        return;
    }

    std::string localSave = GetLocalSaveDir();
    std::string msg = "程序所在目录不可写，无法在 exe 旁保存存档。\n\n"
                      "是否改为保存到：\n" + localSave + "\n\n"
                      "选择“否”将以只读模式继续运行：\n"
                      "可以正常使用映射，但设置不会被保存。";
    if (MessageBoxA(NULL, msg.c_str(), "存档目录",
                    MB_YESNO | MB_ICONQUESTION) == IDYES
        && EnsureDirWritable(localSave)) {
        g_saveDir = localSave;
        g_saveWritable = true;
        g_saveDirFallback = true;
    } else {
        // 用户拒绝回退，或回退目标也写不了：降级为只读，不退出程序。
        g_saveDir = exeSave;
        g_saveWritable = false;
    }
}

// 检查文件是否存在
// 只算"普通文件"，目录视为不存在——因为本函数用于判断存档文件，
// 若同名目录存在，绝不能把它当成存档。
bool FileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

// 创建空存档文件
// 只在文件不存在时写默认值；存在则不动。用于首次运行时初始化，
// 后续 SaveToFile 会覆盖式写入——因为 INI 写入是"设置键值"而不是"删除文件"。
void CreateSaveFile(const std::string& name) {
    if (!g_saveWritable) return;   // 只读模式下不碰磁盘

    std::string path = g_saveDir + "\\" + name + ".ini";
    if (!FileExists(path)) {
        // 写入默认设置
        // 与程序内初始值保持一致，避免"加载后突变"。
        char buf[32];
        sprintf_s(buf, "%d", 2);  WritePrivateProfileStringA("Speed", "MOVE_SPEED", buf, path.c_str());
        sprintf_s(buf, "%d", 3);  WritePrivateProfileStringA("Speed", "SPEED_MULTIPLIER", buf, path.c_str());
        sprintf_s(buf, "%d", 80); WritePrivateProfileStringA("Speed", "WHEEL_INTERVAL", buf, path.c_str());
        sprintf_s(buf, "%d", 1);  WritePrivateProfileStringA("Speed", "WHEEL_SPEED", buf, path.c_str());
        sprintf_s(buf, "%d", 3);  WritePrivateProfileStringA("Speed", "WHEEL_SPEED_MULTIPLIER", buf, path.c_str());
        WritePrivateProfileStringA("Keys", "keyUp", "69", path.c_str());
        WritePrivateProfileStringA("Keys", "keyDown", "68", path.c_str());
        WritePrivateProfileStringA("Keys", "keyLeft", "83", path.c_str());
        WritePrivateProfileStringA("Keys", "keyRight", "70", path.c_str());
        WritePrivateProfileStringA("Keys", "keyLClick", "74", path.c_str());
        WritePrivateProfileStringA("Keys", "keyRClick", "75", path.c_str());
        WritePrivateProfileStringA("Keys", "keyMClick", "77", path.c_str());
        WritePrivateProfileStringA("Keys", "keyWheelUp", "79", path.c_str());
        WritePrivateProfileStringA("Keys", "keyWheelDown", "76", path.c_str());
        WritePrivateProfileStringA("Switch", "switchKey1", "17", path.c_str());
        WritePrivateProfileStringA("Switch", "switchKey2", "16", path.c_str());
        WritePrivateProfileStringA("Switch", "switchKey3", "0", path.c_str());
        WritePrivateProfileStringA("Switch", "switchKey4", "0", path.c_str());
        WritePrivateProfileStringA("Options", "interceptEnabled", "1", path.c_str());
        WritePrivateProfileStringA("Options", "isMinimizeToTray", "0", path.c_str());
    }
}

// 扫描 save/ 目录，填入 g_saveFiles
// 只收集 *.ini 文件（排除 save0.ini）；并以文件名排序保证下拉框顺序稳定。
void ScanSaveDir() {
    g_saveFiles.clear();

    std::string pattern = g_saveDir + "\\*.ini";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::string fileName = findData.cFileName;
            // 去掉 .ini 后缀
            size_t pos = fileName.find_last_of('.');
            if (pos != std::string::npos) {
                fileName = fileName.substr(0, pos);
            }
            // 排除 save0
            // save0 是隐式自动档，不展示给用户，避免误保存。
            if (fileName != "save0") {
                g_saveFiles.push_back(fileName);
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }

    // 排序
    // 简单字典序：save1, save10, save11 ... save2。
    // 未做自然排序，但足够用，不影响功能。
    std::sort(g_saveFiles.begin(), g_saveFiles.end());
}

// 确保 save/ 目录和三个默认存档存在
// CreateDirectory 在目录已存在时必然失败，忽略返回值即可；CreateSaveFile
// 内部也判存在，所以"补建默认档"可以无条件做。
// 旧实现只在目录刚创建时才建默认档，用户误删 save*.ini 后下拉框就只剩 "+"。
void EnsureSaveDir() {
    if (!g_saveWritable) return;   // 只读模式下不碰磁盘

    CreateDirectoryA(g_saveDir.c_str(), NULL);
    CreateSaveFile("save1");
    CreateSaveFile("save2");
    CreateSaveFile("save3");
}

// 把存档列表填入下拉框
// 每次刷新都会先 CB_RESETCONTENT，然后在末尾追加 "+"，
// "+" 作为"新建存档"的入口（在 ID_COMBO_SAVE 的 CBN_SELCHANGE 中被识别）。
void FillComboBox(HWND hwnd) {
    HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
    if (!hCombo) return;

    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);

    for (size_t i = 0; i < g_saveFiles.size(); i++) {
        SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)g_saveFiles[i].c_str());
    }

    // 最后加 "+"
    SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"+");

    // 默认选中第一个
    SendMessageA(hCombo, CB_SETCURSEL, 0, 0);
}

// ==================== 存档读写 ====================
// 把当前设置写到指定文件
// 每个配置项独立 WritePrivateProfileStringA，属于"按键值覆盖"，
// 不会清空未写到的键——因此不需要先删除现有文件。
// 返回值：true 表示写入后能读回，false 表示没写成（只读模式 / 磁盘问题）。
bool SaveToFile(const std::string& name) {
    if (!g_saveWritable) return false;   // 只读模式：不写、也不谎报成功

    std::string path = g_saveDir + "\\" + name + ".ini";

    // 之前读档失败过一次：先把原文件备份，避免把可能还有救的数据直接覆盖。
    if (g_backupBeforeSave) {
        std::string bak = path + ".bak";
        CopyFileA(path.c_str(), bak.c_str(), FALSE);   // 备份失败不阻断保存
        g_backupBeforeSave = false;
    }

    char buf[32];

    sprintf_s(buf, "%d", MOVE_SPEED.load());
    WritePrivateProfileStringA("Speed", "MOVE_SPEED", buf, path.c_str());
    sprintf_s(buf, "%d", SPEED_MULTIPLIER.load());
    WritePrivateProfileStringA("Speed", "SPEED_MULTIPLIER", buf, path.c_str());
    sprintf_s(buf, "%d", WHEEL_INTERVAL.load());
    WritePrivateProfileStringA("Speed", "WHEEL_INTERVAL", buf, path.c_str());
    sprintf_s(buf, "%d", WHEEL_SPEED.load());
    WritePrivateProfileStringA("Speed", "WHEEL_SPEED", buf, path.c_str());
    sprintf_s(buf, "%d", WHEEL_SPEED_MULTIPLIER.load());
    WritePrivateProfileStringA("Speed", "WHEEL_SPEED_MULTIPLIER", buf, path.c_str());

    // 按键映射：直接存 VK 码数值，读回时按需解释，不依赖键名表。
    sprintf_s(buf, "%d", keyUp.load());
    WritePrivateProfileStringA("Keys", "keyUp", buf, path.c_str());
    sprintf_s(buf, "%d", keyDown.load());
    WritePrivateProfileStringA("Keys", "keyDown", buf, path.c_str());
    sprintf_s(buf, "%d", keyLeft.load());
    WritePrivateProfileStringA("Keys", "keyLeft", buf, path.c_str());
    sprintf_s(buf, "%d", keyRight.load());
    WritePrivateProfileStringA("Keys", "keyRight", buf, path.c_str());
    sprintf_s(buf, "%d", keyLClick.load());
    WritePrivateProfileStringA("Keys", "keyLClick", buf, path.c_str());
    sprintf_s(buf, "%d", keyRClick.load());
    WritePrivateProfileStringA("Keys", "keyRClick", buf, path.c_str());
    sprintf_s(buf, "%d", keyMClick.load());
    WritePrivateProfileStringA("Keys", "keyMClick", buf, path.c_str());
    sprintf_s(buf, "%d", keyWheelUp.load());
    WritePrivateProfileStringA("Keys", "keyWheelUp", buf, path.c_str());
    sprintf_s(buf, "%d", keyWheelDown.load());
    WritePrivateProfileStringA("Keys", "keyWheelDown", buf, path.c_str());

    // 组合键：0 表示该槽位空，读回时按同样规则解释。
    sprintf_s(buf, "%d", switchKey1.load());
    WritePrivateProfileStringA("Switch", "switchKey1", buf, path.c_str());
    sprintf_s(buf, "%d", switchKey2.load());
    WritePrivateProfileStringA("Switch", "switchKey2", buf, path.c_str());
    sprintf_s(buf, "%d", switchKey3.load());
    WritePrivateProfileStringA("Switch", "switchKey3", buf, path.c_str());
    sprintf_s(buf, "%d", switchKey4.load());
    WritePrivateProfileStringA("Switch", "switchKey4", buf, path.c_str());

    // 布尔值用 "1"/"0" 存，读时用 !=0 判断，对旧存档也兼容。
    WritePrivateProfileStringA("Options", "interceptEnabled",
        interceptEnabled.load() ? "1" : "0", path.c_str());
    WritePrivateProfileStringA("Options", "isMinimizeToTray",
        isMinimizeToTray.load() ? "1" : "0", path.c_str());

    // 抽样复核：API 返回成功不代表真写进去了（磁盘满、被占用等）。
    return GetPrivateProfileIntA("Speed", "MOVE_SPEED", -1, path.c_str()) == MOVE_SPEED.load();
}

// 读档结果
//   LOAD_OK      正常读取
//   LOAD_MISSING 文件不存在（首次运行，静默用默认值，不打扰用户）
//   LOAD_FAILED  文件存在但读不出内容（损坏 / 被占用 / 没权限）
enum LoadResult { LOAD_OK, LOAD_MISSING, LOAD_FAILED };

// 从指定文件读设置
// 读完后会自动同步到 UI 控件，因此调用方不需要再手动刷新界面。
LoadResult LoadFromFile(const std::string& name) {
    std::string path = g_saveDir + "\\" + name + ".ini";

    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return LOAD_MISSING;

    // 用"能不能读到已知键"判断文件是否可用；读不到就当损坏处理。
    char probe[32] = {};
    if (GetPrivateProfileStringA("Speed", "MOVE_SPEED", "", probe, 32, path.c_str()) == 0) {
        g_backupBeforeSave = true;   // 下次保存前先备份，别直接覆盖
        return LOAD_FAILED;
    }

    // 缺省值使用程序的"出厂默认"，这样即便存档缺失/损坏也能保持可用。
    MOVE_SPEED = GetPrivateProfileIntA("Speed", "MOVE_SPEED", 2, path.c_str());
    SPEED_MULTIPLIER = GetPrivateProfileIntA("Speed", "SPEED_MULTIPLIER", 3, path.c_str());
    WHEEL_INTERVAL = GetPrivateProfileIntA("Speed", "WHEEL_INTERVAL", 80, path.c_str());
    WHEEL_SPEED = GetPrivateProfileIntA("Speed", "WHEEL_SPEED", 1, path.c_str());
    WHEEL_SPEED_MULTIPLIER = GetPrivateProfileIntA("Speed", "WHEEL_SPEED_MULTIPLIER", 3, path.c_str());

    keyUp = GetPrivateProfileIntA("Keys", "keyUp", 69, path.c_str());
    keyDown = GetPrivateProfileIntA("Keys", "keyDown", 68, path.c_str());
    keyLeft = GetPrivateProfileIntA("Keys", "keyLeft", 83, path.c_str());
    keyRight = GetPrivateProfileIntA("Keys", "keyRight", 70, path.c_str());
    keyLClick = GetPrivateProfileIntA("Keys", "keyLClick", 74, path.c_str());
    keyRClick = GetPrivateProfileIntA("Keys", "keyRClick", 75, path.c_str());
    keyMClick = GetPrivateProfileIntA("Keys", "keyMClick", 77, path.c_str());
    keyWheelUp = GetPrivateProfileIntA("Keys", "keyWheelUp", 79, path.c_str());
    keyWheelDown = GetPrivateProfileIntA("Keys", "keyWheelDown", 76, path.c_str());

    switchKey1 = GetPrivateProfileIntA("Switch", "switchKey1", 17, path.c_str());
    switchKey2 = GetPrivateProfileIntA("Switch", "switchKey2", 16, path.c_str());
    switchKey3 = GetPrivateProfileIntA("Switch", "switchKey3", 0, path.c_str());
    switchKey4 = GetPrivateProfileIntA("Switch", "switchKey4", 0, path.c_str());

    interceptEnabled = GetPrivateProfileIntA("Options", "interceptEnabled", 1, path.c_str()) != 0;
    isMinimizeToTray = GetPrivateProfileIntA("Options", "isMinimizeToTray", 0, path.c_str()) != 0;

    // 把值填到 UI 控件
    SetDlgItemInt(g_hMainWnd, ID_EDIT_SPEED, MOVE_SPEED.load(), FALSE);
    SetDlgItemInt(g_hMainWnd, ID_EDIT_SPEED_MUL, SPEED_MULTIPLIER.load(), FALSE);
    SetDlgItemInt(g_hMainWnd, ID_EDIT_WHEEL_INTERVAL, WHEEL_INTERVAL.load(), FALSE);
    SetDlgItemInt(g_hMainWnd, ID_EDIT_WHEEL_SPEED, WHEEL_SPEED.load(), FALSE);
    SetDlgItemInt(g_hMainWnd, ID_EDIT_WHEEL_MUL, WHEEL_SPEED_MULTIPLIER.load(), FALSE);

    CheckDlgButton(g_hMainWnd, ID_CHECK_INTERCEPT,
        interceptEnabled.load() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hMainWnd, ID_CHECK_TRAY,
        isMinimizeToTray.load() ? BST_CHECKED : BST_UNCHECKED);

    UpdateAllButtonText(g_hMainWnd);
    return LOAD_OK;
}

// ==================== 判断是否需要拦截 ====================
bool ShouldIntercept(DWORD vkCode) {
    // 总开关未启用、或用户手动关闭拦截时，一律放行。
    if (!isEnabled || !interceptEnabled) return false;

    // 组合键成员不拦截，保证 GetAsyncKeyState 能读到按键状态
    // 否则 Ctrl/Shift 这类键被吞掉，系统层面永远"没按下"，组合键形同失效。
    if (vkCode == switchKey1.load() || vkCode == switchKey2.load() || vkCode == switchKey3.load() || vkCode == switchKey4.load()) return false;

    // 映射键：处于启用+拦截状态时吞掉，防止按键透传到前台窗口。
    return vkCode == keyUp.load() || vkCode == keyDown.load() ||
           vkCode == keyLeft.load() || vkCode == keyRight.load() ||
           vkCode == keyLClick.load() || vkCode == keyRClick.load() ||
           vkCode == keyMClick.load() || vkCode == keyWheelUp.load() ||
           vkCode == keyWheelDown.load();
}

// ==================== 记录按键状态 ====================
// 由钩子线程调用，把"某个物理键按下/松开"翻译成"某个功能触发/停止"。
// 名字里的 keyE / keyD 是老命名，实际含义是"上移功能触发"/"下移功能触发"。
void SetKeyState(DWORD vkCode, bool down) {
    if (vkCode == keyUp.load()) keyE = down;
    else if (vkCode == keyDown.load()) keyD = down;
    else if (vkCode == keyLeft.load()) keyS = down;
    else if (vkCode == keyRight.load()) keyF = down;
    else if (vkCode == keyLClick.load()) keyJ = down;
    else if (vkCode == keyRClick.load()) keyK = down;
    else if (vkCode == keyWheelUp.load()) keyO = down;
    else if (vkCode == keyWheelDown.load()) keyL = down;
    else if (vkCode == keyMClick.load()) keyM = down;
    // Shift/Ctrl 只被"Shift 加速"逻辑读取，且是本程序当前实现里仅有的两个修饰键。
    else if (vkCode == VK_SHIFT) keyShift = down;
    else if (vkCode == VK_CONTROL) keyCtrl = down;
}

// ==================== 键盘钩子 ====================
// WH_KEYBOARD_LL 的回调。注意：
//   1. 该回调在"安装钩子的线程"上被调用（也就是主线程的消息泵里），
//      因此这里不能做耗时操作；
//   2. 返回非 0 表示吞掉该事件，返回 CallNextHookEx(...) 结果表示继续传递。
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;

        // 把左右物理键码归一化为通用键码
        // 因为 GetAsyncKeyState(VK_SHIFT) 只认通用码；如果不归一，
        // 用户按右 Shift 时组合键判定会失效。
        DWORD vk = p->vkCode;
        switch (vk) {
            case VK_LCONTROL: case VK_RCONTROL: vk = VK_CONTROL; break;
            case VK_LSHIFT:   case VK_RSHIFT:   vk = VK_SHIFT;   break;
            case VK_LMENU:    case VK_RMENU:    vk = VK_MENU;    break;
        }

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            // 捕获模式：把按键交给主线程处理，本轮直接吞掉
            // 用 PostMessage 而不是 SetDlgItemText 等，是因为：
            //   1) 我们走在钩子回调里，此时别的线程正在等待输入；
            //   2) 修改 UI 只能在主线程做，钩子本身也在主线程，但
            //      为了不打断钩子链条，还是把"改映射"推迟到消息泵里更稳。
            int btn = waitingButtonId.load();
            if (btn != 0){
                PostMessageA(g_hMainWnd, WM_BIND_KEY, (WPARAM)vk, (LPARAM)btn);
                return 1;
            }

            // 隐藏彩蛋：窗口在前台时 Esc+0 重置
            // 用 GetForegroundWindow 判断，避免游戏里误触。
            if (vk == '0' && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) && GetForegroundWindow() == g_hMainWnd) {
                PostMessageA(g_hMainWnd, WM_RESET, 0, 0);
            }

            SetKeyState(vk, true);
            if (ShouldIntercept(vk)) return 1;
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            // 松开也要同步状态 + 视情况拦截，否则被吞的键会一直"按着"。
            SetKeyState(vk, false);
            if (ShouldIntercept(vk)) return 1;
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

// ==================== 鼠标操作 ====================
// 以下 Down/Up 都用 exchange 做"状态转换"，只有真正变化时才注入事件。
// 若不这样，控制线程按 tick 一轮，按住左键时每秒会发约 200 个 LEFT DOWN，
// 系统会把这些当作连击/重复拖拽，行为完全错乱。
void MouseLeftDown() {
    if (mouseLeftDown.exchange(true)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseLeftUp() {
    if (!mouseLeftDown.exchange(false)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseRightDown() {
    if (mouseRightDown.exchange(true)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseRightUp() {
    if (!mouseRightDown.exchange(false)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseMiddleDown() {
    if (mouseMiddleDown.exchange(true)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseMiddleUp() {
    if (!mouseMiddleDown.exchange(false)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
    SendInput(1, &input, sizeof(INPUT));
}

// delta 为正向上滚、为负向下滚；标准滚轮一圈等于 WHEEL_DELTA(=120)。
void MouseWheel(int delta) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = delta;
    SendInput(1, &input, sizeof(INPUT));
}

// 使用"绝对坐标"模式：0~65535 映射到整个虚拟桌面（多显示器时包含所有屏）。
// 旧实现只按主屏 SM_CXSCREEN 换算、且没加 MOUSEEVENTF_VIRTUALDESK，
// 光标在副屏或主屏左侧/上方（负坐标）时会偏。
void MouseMoveTo(int x, int y) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) return;   // 拿不到桌面尺寸就放弃，别算出除零

    // 夹到虚拟桌面范围内，避免越界坐标被系统截断到边上。
    if (x < vx) x = vx;
    if (x > vx + vw - 1) x = vx + vw - 1;
    if (y < vy) y = vy;
    if (y > vy + vh - 1) y = vy + vh - 1;

    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = (LONG)(((LONGLONG)(x - vx) * 65535LL) / (vw - 1));
    input.mi.dy = (LONG)(((LONGLONG)(y - vy) * 65535LL) / (vh - 1));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &input, sizeof(INPUT));
}


// ==================== 开关检测 ====================

// 判断指定键是否按下（左右分键合并判断）
// 直接用 GetAsyncKeyState 而不是钩子里维护的原子状态——因为组合键
// 成员是"放行"的，如果走自维护状态，中途可能被系统吞掉键、状态卡死。
bool IsKeyDown(int vk) {
    switch (vk) {
        case VK_SHIFT:   return ((GetAsyncKeyState(VK_LSHIFT)   | GetAsyncKeyState(VK_RSHIFT))   & 0x8000) != 0;
        case VK_CONTROL: return ((GetAsyncKeyState(VK_LCONTROL) | GetAsyncKeyState(VK_RCONTROL)) & 0x8000) != 0;
        case VK_MENU:    return ((GetAsyncKeyState(VK_LMENU)    | GetAsyncKeyState(VK_RMENU))    & 0x8000) != 0;
        default:         return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
}

// 判定组合键是否"同时全部按下"。
// 空位（vk==0）视为"不要求"。全空则返回 false，避免误触发。
bool IsSwitchPressed() {
    int k1 = switchKey1.load();
    int k2 = switchKey2.load();
    int k3 = switchKey3.load();
    int k4 = switchKey4.load();

    if (k1 == 0 && k2 == 0 && k3 == 0 && k4 == 0) return false;

    // 空位直接置真，等价于"条件满足、无需检测"。
    bool p1 = (k1 == 0) || IsKeyDown(k1);
    bool p2 = (k2 == 0) || IsKeyDown(k2);
    bool p3 = (k3 == 0) || IsKeyDown(k3);
    bool p4 = (k4 == 0) || IsKeyDown(k4);

    return p1 && p2 && p3 && p4;
}

// ==================== 高精度节拍器 ====================
// 需求：轮询频率略高于显示器刷新率（144/165Hz），但不想为它牺牲功耗。
// 做法：用高精度可等待定时器。等待期间线程真正阻塞（CPU 占用 0%），
// 也不像 timeBeginPeriod 那样把全系统的定时器精度拉高。
// 老系统（Win10 1803 之前）创建失败时退回普通定时器，
// 由调用方补 timeBeginPeriod(1) 兜底。
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

class TickTimer {
public:
    TickTimer() {
        h = CreateWaitableTimerExA(NULL, NULL,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (h == NULL) {
            h = CreateWaitableTimerA(NULL, FALSE, NULL);
            coarse = true;
        }
    }

    ~TickTimer() {
        if (h != NULL) CloseHandle(h);
    }

    // 至少等待 ms 毫秒。定时器用"相对时间"（负值），单位 100ns。
    void Wait(int ms) {
        if (h != NULL) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)ms * 10000LL;
            if (SetWaitableTimer(h, &due, 0, NULL, NULL, FALSE)) {
                // 加一点兜底超时，避免定时器异常时线程永久卡住。
                WaitForSingleObject(h, (DWORD)ms + 50);
                return;
            }
        }
        std::this_thread::sleep_for(milliseconds(ms));
    }

    bool coarse = false;   // true = 退回了普通定时器，需要 timeBeginPeriod 兜底

private:
    HANDLE h = NULL;
};

// ==================== 鼠标控制线程 ====================
// 按 TICK_MS 节拍轮询原子标志，驱动鼠标；所有输出都是"事件注入"或"位置设置"。
void MouseControlThread() {
    bool lastToggleState = false;   // 上一轮的组合键状态，用于"边沿"识别
    auto lastWheelTime = steady_clock::now();   // 滚轮节流（限速）的时间戳

    TickTimer timer;
    if (timer.coarse) timeBeginPeriod(1);   // 老系统：普通定时器精度不够

    while (isRunning) {
        // ---------- 组合键边沿触发切换 ----------
        bool currentToggleState = IsSwitchPressed();
        if (currentToggleState && !lastToggleState) {
            // 只在"松开→按下"这一刻切换一次；按住不放不会反复切换。
            isEnabled = !isEnabled;

            if (isEnabled) {
                // 启用时重放一次"绝对坐标鼠标移动"。
                // 位置没变，但某些全屏/DirectInput 程序只监听 MOUSEMOVE 事件，
                // 补发一次让它们知道"光标现在在这里"，避免坐标错乱。
                POINT pt;
                GetCursorPos(&pt);
                MouseMoveTo(pt.x, pt.y);
            }

            // 更新标题，向用户反馈当前状态。
            HWND hwnd = FindWindowA("MyClass", NULL);
            if (hwnd) {
                std::string title = "光标控制 ";
                title += isEnabled ? "[启用]" : "[禁用]";
                SetWindowTextA(hwnd, title.c_str());
            }
        }
        lastToggleState = currentToggleState;

        // 组合键按住期间跳过映射，避免切换瞬间被组合键成员误触发
        // 同时主动松开三键，防止"按住组合键但鼠标还处于按下"这种矛盾状态。
        if (currentToggleState)
        {
            MouseLeftUp();
            MouseRightUp();
            MouseMiddleUp();
            timer.Wait(TICK_MS);
            continue;
        }

        if (!isEnabled) {
            // 总开关禁用时同样要做"复位"，否则禁用后鼠标仍可能处于按下态。
            MouseLeftUp();
            MouseRightUp();
            MouseMiddleUp();
            timer.Wait(TICK_MS);
            continue;
        }

        // ---------- 读取本帧的所有功能状态 ----------
        bool e = keyE.load();
        bool s = keyS.load();
        bool d = keyD.load();
        bool f = keyF.load();
        bool j = keyJ.load();
        bool k = keyK.load();
        bool o = keyO.load();
        bool l = keyL.load();
        bool m = keyM.load();

        // ---------- 速度 ----------
        // 用 SetCursorPos 而不是 SendInput：SetCursorPos 是精确设置位置，
        // 在 Windows 下不会被 Clipping 等机制干扰，对"移动"意图更直接。
        if (e || s || d || f) {
            POINT pt;
            GetCursorPos(&pt);
            int speed = MOVE_SPEED.load();
            if (keyShift.load()) speed *= SPEED_MULTIPLIER.load();
            if (e) pt.y -= speed;
            if (d) pt.y += speed;
            if (s) pt.x -= speed;
            if (f) pt.x += speed;
            SetCursorPos(pt.x, pt.y);
        }

        // ---------- 三键点击 ----------
        // Down 幂等，Up 幂等，因此可以无脑每帧调用。
        if (j) MouseLeftDown(); else MouseLeftUp();
        if (k) MouseRightDown(); else MouseRightUp();
        if (m) MouseMiddleDown(); else MouseMiddleUp();

        // ---------- 滚轮 ----------
        // 按时间节流：两次事件至少间隔 WHEEL_INTERVAL 毫秒。
        // 这样避免按住滚轮键时"一帧几十个事件"导致页面疯狂滚动。
        auto now = steady_clock::now();
        auto elapsed = duration_cast<milliseconds>(now - lastWheelTime).count();

        if ((o || l) && elapsed >= WHEEL_INTERVAL.load()) {
            int speed = WHEEL_SPEED.load();
            if (keyShift.load()) speed *= WHEEL_SPEED_MULTIPLIER.load();

            if (o) MouseWheel(WHEEL_DELTA * speed);
            if (l) MouseWheel(-WHEEL_DELTA * speed);

            lastWheelTime = now;
        }

        // ---------- 节拍等待 ----------
        // TICK_MS 略高于 144/165Hz 的刷新周期；线程在这里阻塞，不占 CPU。
        timer.Wait(TICK_MS);
    }

    if (timer.coarse) timeEndPeriod(1);

    // 线程退出前的兜底复位：防止因为关程序导致鼠标"粘住"。
    MouseLeftUp();
    MouseRightUp();
    MouseMiddleUp();
}

// ==================== 托盘图标 ====================
// 使用简化的 NIM_ADD 参数；实际上系统会对图标资源要求一个 HICON，
// 这里直接用 IDI_APPLICATION 保证任何一个 Windows 都显示正常。
void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP + 1;   // 托盘事件一律以这个消息通知窗口
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    strcpy_s(g_nid.szTip, "光标控制");
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

// 删除托盘图标；即使没添加过（系统会忽略）也不报错。
void RemoveTrayIcon() {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

// ==================== 读取 UI 值到变量 ====================
// 带范围钳制：任何非法输入（空、越界、非数字）都会被忽略，保持当前值。
// 这样用户误删一个字符再点"应用"也不会把速度刷成 0。
void ApplyUIValues(HWND hwnd) {
    char buf[32];

    GetDlgItemTextA(hwnd, ID_EDIT_SPEED, buf, 32);
    int v = atoi(buf); if (v >= 1 && v <= 100) MOVE_SPEED = v;

    GetDlgItemTextA(hwnd, ID_EDIT_SPEED_MUL, buf, 32);
    v = atoi(buf); if (v >= 1 && v <= 100) SPEED_MULTIPLIER = v;

    GetDlgItemTextA(hwnd, ID_EDIT_WHEEL_INTERVAL, buf, 32);
    v = atoi(buf); if (v >= 1 && v <= 10000) WHEEL_INTERVAL = v;

    GetDlgItemTextA(hwnd, ID_EDIT_WHEEL_SPEED, buf, 32);
    v = atoi(buf); if (v >= 1 && v <= 100) WHEEL_SPEED = v;

    GetDlgItemTextA(hwnd, ID_EDIT_WHEEL_MUL, buf, 32);
    v = atoi(buf); if (v >= 1 && v <= 100) WHEEL_SPEED_MULTIPLIER = v;
}

// ==================== 重置 UI 值 ====================
// 恢复"出厂默认"，同时刷新界面控件与按钮文字。
void ResetUIValues(HWND hwnd) {
    MOVE_SPEED = 2;
    SPEED_MULTIPLIER = 3;
    WHEEL_INTERVAL = 80;
    WHEEL_SPEED = 1;
    WHEEL_SPEED_MULTIPLIER = 3;

    keyUp = 'E'; keyDown = 'D'; keyLeft = 'S'; keyRight = 'F';
    keyLClick = 'J'; keyRClick = 'K'; keyMClick = 'M';
    keyWheelUp = 'O'; keyWheelDown = 'L';

    switchKey1 = VK_CONTROL;
    switchKey2 = VK_SHIFT;
    switchKey3 = 0;
    switchKey4 = 0;

    interceptEnabled = true;
    isMinimizeToTray = false;

    SetDlgItemInt(hwnd, ID_EDIT_SPEED, 2, FALSE);
    SetDlgItemInt(hwnd, ID_EDIT_SPEED_MUL, 3, FALSE);
    SetDlgItemInt(hwnd, ID_EDIT_WHEEL_INTERVAL, 80, FALSE);
    SetDlgItemInt(hwnd, ID_EDIT_WHEEL_SPEED, 1, FALSE);
    SetDlgItemInt(hwnd, ID_EDIT_WHEEL_MUL, 3, FALSE);

    CheckDlgButton(hwnd, ID_CHECK_INTERCEPT, BST_CHECKED);
    CheckDlgButton(hwnd, ID_CHECK_TRAY, BST_UNCHECKED);

    UpdateAllButtonText(hwnd);
}

// ==================== 开机自启动 ====================
// 原理：往 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 写一个字符串值，
// 登录时 Windows 会自动执行它。选 HKCU 而不是 HKLM，是因为前者不需要管理员权限，
// 也不会影响其它用户；代价是只对当前用户生效。
//
// 值名固定为 "KeyboardCursor"，内容是 exe 的绝对路径，而且必须用双引号包起来
// —— 否则路径带空格时（如 C:\Program Files\...）开机会被拆成多段，启动失败。
//
// 注意：这里只做"普通权限的开机启动"。如果目标程序以管理员运行，
// Run 键启动的进程仍是普通权限，钩不住管理员窗口（受 UIPI 限制，
// 既收不到键也注入不进去），那种情况得用任务计划程序的"最高权限"运行。
static const char* kRunKey   = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* kRunValue = "KeyboardCursor";

// 查注册表里是否已有我们的自启动项。
// 只关心"值存在且非空"，不比对路径内容 —— 即使 exe 被移动过，
// 也认为开关是打开的，实际路径会在 SetAutoStart(true) 时被刷新。
bool IsAutoStartEnabled() {
    HKEY hKey = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;   // 键都打不开，当作未设置

    DWORD type = 0;
    DWORD size = 0;
    LONG ret = RegQueryValueExA(hKey, kRunValue, NULL, &type, NULL, &size);
    RegCloseKey(hKey);
    return ret == ERROR_SUCCESS && size > 0;
}

// 打开 / 关闭开机自启动。
// 返回 false 表示注册表操作失败，调用方应当把复选框状态还原并提示用户，
// 避免出现"界面勾着、实际没生效"这种假象。
bool SetAutoStart(bool enable) {
    HKEY hKey = NULL;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kRunKey, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return false;

    LONG ret;
    if (enable) {
        // 用绝对路径：开机时的工作目录是 system32，相对路径必然找不到 exe。
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(NULL, exePath, MAX_PATH);

        // 加引号，防止路径里的空格把命令行拆断。
        std::string quoted = std::string("\"") + exePath + "\"";
        ret = RegSetValueExA(hKey, kRunValue, 0, REG_SZ,
                             (const BYTE*)quoted.c_str(),
                             (DWORD)(quoted.size() + 1));   // 含结尾的 '\0'
    } else {
        ret = RegDeleteValueA(hKey, kRunValue);
        // 本来就没设置过，也视为"关闭成功"。
        if (ret == ERROR_FILE_NOT_FOUND) ret = ERROR_SUCCESS;
    }

    RegCloseKey(hKey);
    return ret == ERROR_SUCCESS;
}

// ==================== 统一退出入口 ====================
// 关窗（非托盘）与托盘右键"退出"都走这里：
// 先把界面上的数字收回内存，再写 save0，最后销毁窗口。
// 旧实现里托盘"退出"直接 DestroyWindow，绕过了 ApplyUIValues，
// 用户改完数字没点"应用"就退出会丢修改。
void QuitApp(HWND hwnd) {
    ApplyUIValues(hwnd);
    SaveToFile("save0");
    DestroyWindow(hwnd);
}

// ==================== 窗口过程 ====================
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            // 创建时一次性把所有控件"布局"出来。
            // 通过 y 逐行推进，比一堆手写固定坐标可读。
            g_hMainWnd = hwnd;

            int y = 10;
            const int labelW = 80;
            const int editW = 60;
            const int rowH = 28;   // 每行的高（包含间隙）

            CreateWindowA("STATIC", "光标控制",
                WS_VISIBLE | WS_CHILD,
                10, y, 200, 20, hwnd, NULL, NULL, NULL);
            y += 25;

            CreateWindowA("STATIC", "── 速度设置 ──",
                WS_VISIBLE | WS_CHILD,
                10, y, 200, 20, hwnd, NULL, NULL, NULL);
            y += 22;

            // 移动速度：像素/帧
            CreateWindowA("STATIC", "移动速度:",
                WS_VISIBLE | WS_CHILD, 10, y + 3, labelW, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "2",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                95, y, editW, 22, hwnd, (HMENU)ID_EDIT_SPEED, NULL, NULL);
            y += rowH;

            // Shift 加速倍率
            CreateWindowA("STATIC", "Shift加速:",
                WS_VISIBLE | WS_CHILD, 10, y + 3, labelW, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "3",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                95, y, editW, 22, hwnd, (HMENU)ID_EDIT_SPEED_MUL, NULL, NULL);
            y += rowH;

            // 滚轮最小间隔（毫秒）
            CreateWindowA("STATIC", "滚轮间隔(ms):",
                WS_VISIBLE | WS_CHILD, 10, y + 3, 100, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "80",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                115, y, editW, 22, hwnd, (HMENU)ID_EDIT_WHEEL_INTERVAL, NULL, NULL);
            y += rowH;

            // 滚轮单次档位
            CreateWindowA("STATIC", "滚轮速度:",
                WS_VISIBLE | WS_CHILD, 10, y + 3, labelW, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "1",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                95, y, editW, 22, hwnd, (HMENU)ID_EDIT_WHEEL_SPEED, NULL, NULL);
            y += rowH;

            // Shift 滚轮倍率
            CreateWindowA("STATIC", "滚轮加速:",
                WS_VISIBLE | WS_CHILD, 10, y + 3, labelW, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "3",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                95, y, editW, 22, hwnd, (HMENU)ID_EDIT_WHEEL_MUL, NULL, NULL);
            y += rowH + 10;

            CreateWindowA("STATIC", "── 按键映射 ──",
                WS_VISIBLE | WS_CHILD,
                10, y, 200, 20, hwnd, NULL, NULL, NULL);
            y += 22;

            // 两列布局：x1 左列，x2 右列；btnW 按钮宽
            int x1 = 10, x2 = 180;
            int btnW = 70;

            // 上 / 下
            CreateWindowA("STATIC", "上:", WS_VISIBLE | WS_CHILD, x1, y + 3, 25, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "E", WS_VISIBLE | WS_CHILD, x1 + 28, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_UP, NULL, NULL);
            CreateWindowA("STATIC", "下:", WS_VISIBLE | WS_CHILD, x2, y + 3, 25, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "D", WS_VISIBLE | WS_CHILD, x2 + 28, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_DOWN, NULL, NULL);
            y += rowH;

            // 左 / 右
            CreateWindowA("STATIC", "左:", WS_VISIBLE | WS_CHILD, x1, y + 3, 25, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "S", WS_VISIBLE | WS_CHILD, x1 + 28, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_LEFT, NULL, NULL);
            CreateWindowA("STATIC", "右:", WS_VISIBLE | WS_CHILD, x2, y + 3, 25, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "F", WS_VISIBLE | WS_CHILD, x2 + 28, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_RIGHT, NULL, NULL);
            y += rowH;

            // 左键 / 右键
            CreateWindowA("STATIC", "左键:", WS_VISIBLE | WS_CHILD, x1, y + 3, 45, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "J", WS_VISIBLE | WS_CHILD, x1 + 48, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_LCLICK, NULL, NULL);
            CreateWindowA("STATIC", "右键:", WS_VISIBLE | WS_CHILD, x2, y + 3, 45, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "K", WS_VISIBLE | WS_CHILD, x2 + 48, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_RCLICK, NULL, NULL);
            y += rowH;

            // 中键 / 上滚
            CreateWindowA("STATIC", "中键:", WS_VISIBLE | WS_CHILD, x1, y + 3, 45, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "M", WS_VISIBLE | WS_CHILD, x1 + 48, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_MCLICK, NULL, NULL);
            CreateWindowA("STATIC", "上滚:", WS_VISIBLE | WS_CHILD, x2, y + 3, 45, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "O", WS_VISIBLE | WS_CHILD, x2 + 48, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_WHEELUP, NULL, NULL);
            y += rowH;

            // 下滚
            CreateWindowA("STATIC", "下滚:", WS_VISIBLE | WS_CHILD, x1, y + 3, 45, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "L", WS_VISIBLE | WS_CHILD, x1 + 48, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_WHEELDOWN, NULL, NULL);
            y += rowH + 10;

            // ========== 开关组合键 ==========
            // 1~4 个按钮分别对应 switchKey1..4；点击后进入"捕获模式"。
            CreateWindowA("STATIC", "── 开关组合键（1~4 个键） ──",
                WS_VISIBLE | WS_CHILD,
                10, y, 300, 20, hwnd, NULL, NULL, NULL);
            y += 22;

            CreateWindowA("BUTTON", "Ctrl", WS_VISIBLE | WS_CHILD, 10, y, 70, 24, hwnd, (HMENU)ID_BTN_SWITCH1, NULL, NULL);
            CreateWindowA("BUTTON", "Shift", WS_VISIBLE | WS_CHILD, 90, y, 70, 24, hwnd, (HMENU)ID_BTN_SWITCH2, NULL, NULL);
            CreateWindowA("BUTTON", "无", WS_VISIBLE | WS_CHILD, 170, y, 70, 24, hwnd, (HMENU)ID_BTN_SWITCH3, NULL, NULL);
            CreateWindowA("BUTTON", "无", WS_VISIBLE | WS_CHILD, 250, y, 70, 24, hwnd, (HMENU)ID_BTN_SWITCH4, NULL, NULL);
            CreateWindowA("BUTTON", "清空全部", WS_VISIBLE | WS_CHILD, 330, y, 80, 24, hwnd, (HMENU)ID_BTN_CLEAR_SWITCH, NULL, NULL);
            y += rowH + 10;

            // ========== 开关 ==========
            CreateWindowA("STATIC", "── 开关 ──",
                WS_VISIBLE | WS_CHILD,
                10, y, 200, 20, hwnd, NULL, NULL, NULL);
            y += 22;

            CreateWindowA("BUTTON", "启用按键拦截",
                WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                10, y, 150, 24, hwnd, (HMENU)ID_CHECK_INTERCEPT, NULL, NULL);
            CheckDlgButton(hwnd, ID_CHECK_INTERCEPT, BST_CHECKED);
            y += rowH;

            CreateWindowA("BUTTON", "关闭时最小化到托盘",
                WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                10, y, 180, 24, hwnd, (HMENU)ID_CHECK_TRAY, NULL, NULL);
            y += rowH;

            // 开机自启动：勾选状态直接读注册表，不经过 ini，
            // 避免"ini 里存的是开、注册表里实际没写"这种两处状态打架。
            CreateWindowA("BUTTON", "开机自启动",
                WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                10, y, 150, 24, hwnd, (HMENU)ID_CHECK_AUTOSTART, NULL, NULL);
            if (IsAutoStartEnabled()) {
                CheckDlgButton(hwnd, ID_CHECK_AUTOSTART, BST_CHECKED);
                // 顺手把路径刷一遍：用户可能把 exe 挪过位置，
                // 这里用当前路径覆盖旧值，省得下次开机找不到程序。
                SetAutoStart(true);
            }
            y += rowH + 10;

            CreateWindowA("BUTTON", "应用", WS_VISIBLE | WS_CHILD, 10, y, 80, 30, hwnd, (HMENU)ID_BTN_APPLY, NULL, NULL);
            CreateWindowA("BUTTON", "重置", WS_VISIBLE | WS_CHILD, 100, y, 80, 30, hwnd, (HMENU)ID_BTN_RESET, NULL, NULL);

            // 提示：Esc+0 只在窗口前台有效
            CreateWindowA("STATIC", "重置快捷键：Esc + 0（仅窗口在前台时）",
                WS_VISIBLE | WS_CHILD,
                10, y + 40, 300, 20, hwnd, NULL, NULL, NULL);

            // 底部状态行：显示存档目录 / 只读 / 读取失败等信息。
            // SS_ENDELLIPSIS 让过长的路径自动变成省略号，不撑破窗口。
            CreateWindowA("STATIC", "",
                WS_VISIBLE | WS_CHILD | SS_ENDELLIPSIS,
                10, y + 62, 480, 18, hwnd, (HMENU)ID_STATIC_STATUS, NULL, NULL);

            // ==================== 右侧存档区 ====================
            int ry = 10;

            CreateWindowA("STATIC", "── 存档 ──",
                WS_VISIBLE | WS_CHILD,
                350, ry, 160, 20, hwnd, NULL, NULL, NULL);
            ry += 25;

            // 下拉框：CBS_DROPDOWNLIST 禁止用户编辑文本，只能从列表中选择
            CreateWindowA("COMBOBOX", "",
                WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | WS_VSCROLL,
                350, ry, 160, 200, hwnd, (HMENU)ID_COMBO_SAVE, NULL, NULL);
            ry += 30;

            CreateWindowA("BUTTON", "保存",
                WS_VISIBLE | WS_CHILD,
                350, ry, 70, 30, hwnd, (HMENU)ID_BTN_SAVE, NULL, NULL);
            CreateWindowA("BUTTON", "加载",
                WS_VISIBLE | WS_CHILD,
                430, ry, 70, 30, hwnd, (HMENU)ID_BTN_LOAD, NULL, NULL);

            // ==================== 存档初始化 ====================
            // g_saveDir 已经在 WinMain 里定好了（正常 / 回退 / 只读三种模式），
            // 这里只按模式决定要不要碰磁盘，不再做目录探测。
            if (g_saveWritable) {
                EnsureSaveDir();      // 只在可写时补建默认档
            } else {
                // 只读：只禁"保存"（唯一的写操作）。
                // 扫描目录和加载都不碰磁盘，照常可用。
                EnableWindow(GetDlgItem(hwnd, ID_BTN_SAVE), FALSE);
            }
            ScanSaveDir();
            FillComboBox(hwnd);

            // 读 save0.ini（默认存档）：缺失属首次运行，损坏则提示并保留备份。
            LoadResult lr = LoadFromFile("save0");
            if (lr == LOAD_FAILED) {
                SetStatus(hwnd, "存档读取失败，已使用默认值；原文件将在下次保存前备份为 .bak。");
            } else if (!g_saveWritable) {
                SetStatus(hwnd, "只读模式：程序目录不可写且未启用回退，设置不会保存。");
            } else if (g_saveDirFallback) {
                SetStatus(hwnd, "存档目录（程序目录不可写，已改用用户目录）：" + g_saveDir);
            } else {
                SetStatus(hwnd, "存档目录：" + g_saveDir);
            }

            // 定时刷新按钮文字：兜底机制，防止某些极端情况下映射
            // 发生了变化（比如被外部修改）而按钮文字没同步。
            SetTimer(hwnd, ID_TIMER_REFRESH, 500, NULL);
            break;
        }

        case WM_TIMER: {
            if (wParam == ID_TIMER_REFRESH) {
                // 只在"没有在等按键"的情况下刷新，避免把"按任意键..."覆盖掉。
                if (waitingButtonId.load() == 0) {
                    UpdateAllButtonText(hwnd);
                }
            }
            break;
        }

        case WM_COMMAND: {
            // 通知源：LOWORD(wParam) 是控件 ID，HIWORD(wParam) 是通知码。
            int id = LOWORD(wParam);

            switch (id) {
                // ---------- 进入捕获模式：任何映射/组合键按钮点击均触发 ----------
                case ID_BTN_KEY_UP:
                case ID_BTN_KEY_DOWN:
                case ID_BTN_KEY_LEFT:
                case ID_BTN_KEY_RIGHT:
                case ID_BTN_KEY_LCLICK:
                case ID_BTN_KEY_RCLICK:
                case ID_BTN_KEY_MCLICK:
                case ID_BTN_KEY_WHEELUP:
                case ID_BTN_KEY_WHEELDOWN:
                case ID_BTN_SWITCH1:
                case ID_BTN_SWITCH2:
                case ID_BTN_SWITCH3:
                case ID_BTN_SWITCH4: {
                    waitingButtonId = id;
                    SetDlgItemTextA(hwnd, id, "按任意键...");
                    SetFocus(hwnd);   // 把焦点从按钮移到窗口，避免按钮抢占按键
                    break;
                }

                // ---------- 清空全部组合键 ----------
                case ID_BTN_CLEAR_SWITCH: {
                    switchKey1 = 0;
                    switchKey2 = 0;
                    switchKey3 = 0;
                    switchKey4 = 0;
                    UpdateAllButtonText(hwnd);
                    break;
                }

                // ---------- 两个布尔开关 ----------
                case ID_CHECK_INTERCEPT: {
                    interceptEnabled = (IsDlgButtonChecked(hwnd, ID_CHECK_INTERCEPT) == BST_CHECKED);
                    break;
                }

                case ID_CHECK_TRAY: {
                    isMinimizeToTray = (IsDlgButtonChecked(hwnd, ID_CHECK_TRAY) == BST_CHECKED);
                    break;
                }

                // ---------- 开机自启动 ----------
                case ID_CHECK_AUTOSTART: {
                    bool want = (IsDlgButtonChecked(hwnd, ID_CHECK_AUTOSTART) == BST_CHECKED);
                    if (!SetAutoStart(want)) {
                        // 注册表写失败（权限、安全软件拦截等）：把勾选状态还原，
                        // 否则界面显示的和实际生效的会不一致。
                        MessageBoxA(hwnd, "无法修改开机自启动设置（注册表写入失败）。",
                                    "错误", MB_OK | MB_ICONERROR);
                        CheckDlgButton(hwnd, ID_CHECK_AUTOSTART,
                                       want ? BST_UNCHECKED : BST_CHECKED);
                    }
                    break;
                }

                // ---------- 应用 / 重置 ----------
                case ID_BTN_APPLY: {
                    ApplyUIValues(hwnd);
                    break;
                }

                case ID_BTN_RESET: {
                    ResetUIValues(hwnd);
                    break;
                }

                // ---------- 保存 ----------
                case ID_BTN_SAVE: {
                    // 只读模式下按钮本来已置灰，这里再兜一层，防止消息伪造。
                    if (!g_saveWritable) {
                        MessageBoxA(hwnd, "当前为只读模式，设置无法保存。", "提示",
                                    MB_OK | MB_ICONINFORMATION);
                        break;
                    }

                    // 读下拉框当前选中
                    HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
                    int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                    if (sel < 0) break;

                    char name[256];
                    SendMessageA(hCombo, CB_GETLBTEXT, sel, (LPARAM)name);

                    // 如果是 "+"，走"新建存档"分支，不覆盖任何已有文件。
                    if (strcmp(name, "+") == 0) {
                        PostMessageA(hwnd, WM_NEW_SAVE, 0, 0);
                        break;
                    }

                    // 先把 UI 值写进变量，再存盘，避免"改完数字没点应用就保存"导致保存了旧值。
                    ApplyUIValues(hwnd);
                    if (SaveToFile(name)) {
                        SetStatus(hwnd, "已保存：" + g_saveDir + "\\" + name + ".ini");
                        MessageBoxA(hwnd, "保存成功", "提示", MB_OK);
                    } else {
                        // 不再无条件报"保存成功"：写不进去就要明确告诉用户。
                        SetStatus(hwnd, "保存失败：无法写入存档目录。");
                        MessageBoxA(hwnd, "保存失败：无法写入存档目录。", "错误",
                                    MB_OK | MB_ICONERROR);
                    }
                    break;
                }

                // ---------- 加载 ----------
                case ID_BTN_LOAD: {
                    HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
                    int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                    if (sel < 0) break;

                    char name[256];
                    SendMessageA(hCombo, CB_GETLBTEXT, sel, (LPARAM)name);

                    // "+" 不是存档，跳过。
                    if (strcmp(name, "+") == 0) break;

                    LoadResult lr = LoadFromFile(name);
                    if (lr == LOAD_OK) {
                        SetStatus(hwnd, "已加载：" + g_saveDir + "\\" + name + ".ini");
                        MessageBoxA(hwnd, "加载成功", "提示", MB_OK);
                    } else if (lr == LOAD_MISSING) {
                        MessageBoxA(hwnd, "存档文件不存在。", "提示", MB_OK | MB_ICONWARNING);
                    } else {
                        SetStatus(hwnd, "存档读取失败，已使用默认值。");
                        MessageBoxA(hwnd, "存档读取失败，已使用默认值。", "错误",
                                    MB_OK | MB_ICONERROR);
                    }
                    break;
                }

                // ---------- 下拉框选择变化 ----------
                case ID_COMBO_SAVE: {
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        // 用户选了 "+"，弹新建对话框（当前实现是静默新建）
                        HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
                        int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                        char name[256];
                        SendMessageA(hCombo, CB_GETLBTEXT, sel, (LPARAM)name);
                        if (strcmp(name, "+") == 0) {
                            PostMessageA(hwnd, WM_NEW_SAVE, 0, 0);
                        }
                    }
                    break;
                }
            }
            break;
        }

        // ---------- 新建存档 ----------
        case WM_NEW_SAVE: {
            if (!g_saveWritable) {
                MessageBoxA(hwnd, "只读模式下无法新建存档。", "提示", MB_OK | MB_ICONINFORMATION);
                break;
            }

            // 找第一个不存在的编号；若 save1~save999 全被占用则报错退出。
            // 旧实现循环结束后 defaultName 停留在 "save99"（已存在），
            // 会直接覆盖 save99 的内容。
            char defaultName[16] = {};
            bool found = false;
            for (int i = 1; i <= 999 && !found; i++) {
                sprintf_s(defaultName, "save%d", i);
                found = !FileExists(g_saveDir + "\\" + defaultName + ".ini");
            }
            if (!found) {
                MessageBoxA(hwnd, "存档编号已用尽（save1~save999）。", "提示",
                            MB_OK | MB_ICONWARNING);
                break;
            }

            // 简化处理：直接以第一个未占用的 saveN 命名新建存档，暂不提供命名对话框
            // 若用户希望自定义名字，可后续在此处插入输入框。
            // 文件不存在时 SaveToFile 的第一次写入就会创建它，无需先 CreateSaveFile。
            ApplyUIValues(hwnd);
            SaveToFile(defaultName);
            ScanSaveDir();
            FillComboBox(hwnd);

            // 刷新后把下拉框选中到新建的这个档
            HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
            int idx = (int)SendMessageA(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)defaultName);
            if (idx != CB_ERR){
                SendMessageA(hCombo, CB_SETCURSEL, idx, 0);
            }

            SetStatus(hwnd, "已新建存档：" + std::string(defaultName));
            break;
        }

        // ---------- 重置快捷键（Esc+0，来自钩子） ----------
        case WM_RESET: {
            ResetUIValues(hwnd);
            return 0;
        }

        // ---------- 捕获到按键，更新对应映射 ----------
        case WM_BIND_KEY:
        {
            int vk = (int)wParam;
            int btnId = (int)lParam;

            // Esc 视为"取消捕获"，保留原映射。
            if (vk == VK_ESCAPE) {
                waitingButtonId = 0;
                UpdateAllButtonText(hwnd);
                return 0;
            }

            // 冲突校验：开关槽和映射槽不能共用同一个键。
            // 否则 ShouldIntercept 会把该键当"组合键成员"放行，映射就永久失效。
            bool isSwitchButton = (btnId >= ID_BTN_SWITCH1 && btnId <= ID_BTN_SWITCH4);
            bool conflict = isSwitchButton
                ? (vk == keyUp.load() || vk == keyDown.load() || vk == keyLeft.load() ||
                   vk == keyRight.load() || vk == keyLClick.load() || vk == keyRClick.load() ||
                   vk == keyMClick.load() || vk == keyWheelUp.load() || vk == keyWheelDown.load())
                : (vk == switchKey1.load() || vk == switchKey2.load() ||
                   vk == switchKey3.load() || vk == switchKey4.load());
            if (conflict) {
                MessageBoxA(hwnd, "该键已被另一种用途占用，请换一个键。", "按键冲突",
                            MB_OK | MB_ICONWARNING);
                waitingButtonId = 0;
                UpdateAllButtonText(hwnd);
                return 0;
            }

            // 根据当前捕获按钮，把新的 VK 码写入对应原子变量。
            if (btnId == ID_BTN_KEY_UP){
                keyUp = vk;
            }else if (btnId == ID_BTN_KEY_DOWN){
                keyDown = vk;
            }else if (btnId == ID_BTN_KEY_LEFT){
                keyLeft = vk;
            }else if (btnId == ID_BTN_KEY_RIGHT){
                keyRight = vk;
            }else if (btnId == ID_BTN_KEY_LCLICK){
                keyLClick = vk;
            }else if (btnId == ID_BTN_KEY_RCLICK){
                keyRClick = vk;
            }else if (btnId == ID_BTN_KEY_MCLICK){
                keyMClick = vk;
            }else if (btnId == ID_BTN_KEY_WHEELUP){
                keyWheelUp = vk;
            }else if (btnId == ID_BTN_KEY_WHEELDOWN){
                keyWheelDown = vk;
            }else if (btnId == ID_BTN_SWITCH1){
                switchKey1 = vk;
            }else if (btnId == ID_BTN_SWITCH2){
                switchKey2 = vk;
            }else if (btnId == ID_BTN_SWITCH3){
                switchKey3 = vk;
            }else if (btnId == ID_BTN_SWITCH4){
                switchKey4 = vk;
            }

            waitingButtonId = 0;
            UpdateAllButtonText(hwnd);
            return 0;
        }

        // ---------- 窗口失去焦点：取消按键捕获 ----------
        // 捕获模式会吞掉除 Esc 以外的所有键，用户一旦点了别的窗口，
        // 不给出口就会一直卡在"按任意键..."（虽然 Esc 能取消，但不必要求用户知道）。
        // 不加超时：用户可能只是想慢慢按，超时反而会打断。
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE && waitingButtonId.load() != 0) {
                waitingButtonId = 0;
                UpdateAllButtonText(hwnd);
            }
            break;
        }

        // ---------- 关闭窗口 ----------
        case WM_CLOSE: {
            // 关闭前先"应用"一次 UI，保证关窗即保存用户最后的修改。
            ApplyUIValues(hwnd);
            if (isMinimizeToTray) {
                // 走托盘：隐藏窗口 + 添加图标；进程继续运行。
                ShowWindow(hwnd, SW_HIDE);
                AddTrayIcon(hwnd);
            } else {
                // 非托盘：走统一退出入口，先收值再写 save0 再销毁。
                QuitApp(hwnd);
            }
            return 0;
        }

        // ---------- 托盘图标交互 ----------
        case WM_APP + 1: {
            if (lParam == WM_LBUTTONUP) {
                // 左键单击：显示窗口，然后移除图标（避免重复）。
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                RemoveTrayIcon();
            } else if (lParam == WM_RBUTTONUP) {
                // 右键单击：弹出菜单。
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, 1, "显示窗口");
                AppendMenuA(hMenu, MF_STRING, 2, "退出");
                // TrackPopupMenu 要求先把窗口置前台并 SetForegroundWindow，
                // 否则菜单点完不会消失（Windows 的经典小坑）。
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
                if (cmd == 1) {
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    RemoveTrayIcon();
                } else if (cmd == 2) {
                    RemoveTrayIcon();
                    // 托盘"退出"同样走 QuitApp，否则会绕过 ApplyUIValues 丢修改。
                    QuitApp(hwnd);
                }
            }
            return 0;
        }

        // ---------- 销毁：全局收尾 ----------
        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER_REFRESH);
            RemoveTrayIcon();
            // save0 已在 QuitApp 里写过；这里不再重复写，
            // 否则会在控件已销毁后再用旧值覆盖一次。
            isRunning = false;     // 让控制线程尽快跳出 while
            PostQuitMessage(0);    // 结束消息泵
            return 0;

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

// ==================== 程序入口点 ====================
// WinMain 是 Windows GUI 应用的入口（跟 main 不同，不出控制台窗口）。
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    // 单实例：重复启动只会挂上第二个全局钩子，导致按键被吞两次、状态错乱。
    // 已有实例则把它叫到前台，本进程直接退出。
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "Local\\KeyboardCursor_SingleInstance");
    if (hMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND oldWnd = FindWindowA("MyClass", NULL);
        if (oldWnd != NULL) {
            ShowWindow(oldWnd, SW_SHOW);
            SetForegroundWindow(oldWnd);
        }
        CloseHandle(hMutex);
        return 0;
    }

    // 存档目录必须在创建窗口之前定下来：WM_CREATE 里就要扫描目录、读 save0.ini。
    // 不可写时这里会询问用户是否回退，拒绝则进入只读模式（程序照常运行）。
    ResolveSaveDir();

    // 注册窗口类：lpfnWndProc 指定窗口过程函数，lpszClassName 是类名，
    // 后续 CreateWindowExA 靠这个类名找到对应过程并调起来。
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "MyClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    // 窗口宽度 530 是为了让右侧存档区放得下；高度 720 容纳全部控件。
    HWND hwnd = CreateWindowExA(
        0, "MyClass", "光标控制 [启用]",
        WS_OVERLAPPEDWINDOW,
        100, 100, 530, 720,   // 宽度 480 → 530
        NULL, NULL, hInst, NULL
    );

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    // 安装低级键盘钩子：
    //   WH_KEYBOARD_LL 是全局钩子（可以监听到焦点不在本程序时的按键），
    //   hMod 传 hInst、dwThreadId 传 0 表示"所有线程"。
    //   低级钩子不需要 DLL 注入，因此天然支持跨越进程。
    g_hKeyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, KeyboardProc, hInst, 0);
    if (g_hKeyboardHook == NULL) {
        MessageBoxA(NULL, "键盘钩子安装失败！", "错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 启动鼠标控制线程；它与主线程靠 atomic 变量通信。
    std::thread controlThread(MouseControlThread);

    // 主消息泵：标准 GetMessage/DispatchMessage 循环。
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 到这里说明消息泵退出（通常是 WM_QUIT）。清理顺序：
    //   1. 先卸钩子（防止继续吞键）；
    //   2. 再通知控制线程退出；
    //   3. 最后 join，保证线程把鼠标复位做完。
    if (g_hKeyboardHook != NULL) {
        UnhookWindowsHookEx(g_hKeyboardHook);
        g_hKeyboardHook = NULL;
    }

    isRunning = false;
    if (controlThread.joinable()) {
        controlThread.join();
    }

    if (hMutex != NULL) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return 0;
}

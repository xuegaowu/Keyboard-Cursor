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

using namespace std::chrono;

// ==================== 控件 ID ====================
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
#define ID_BTN_APPLY            3041   // 应用
#define ID_BTN_RESET            3042   // 重置
#define ID_TIMER_REFRESH        4001   // 定时器刷新按键显示

// 存档控件 ID
#define ID_COMBO_SAVE           3061   // 存档下拉框
#define ID_BTN_SAVE             3062   // 保存
#define ID_BTN_LOAD             3063   // 加载

// 自定义消息
#define WM_RESET (WM_APP + 2)          // 重置通知
#define WM_NEW_SAVE (WM_APP + 3)       // 新建存档通知
#define WM_BIND_KEY (WM_APP + 4)       // 按键捕获通知

// ==================== 可调变量 ====================
std::atomic<int> MOVE_SPEED(2);
std::atomic<int> SPEED_MULTIPLIER(3);
std::atomic<int> WHEEL_INTERVAL(80);
std::atomic<int> WHEEL_SPEED(1);
std::atomic<int> WHEEL_SPEED_MULTIPLIER(3);

// 按键映射
std::atomic<int> keyUp('E');
std::atomic<int> keyDown('D');
std::atomic<int> keyLeft('S');
std::atomic<int> keyRight('F');
std::atomic<int> keyLClick('J');
std::atomic<int> keyRClick('K');
std::atomic<int> keyMClick('M');
std::atomic<int> keyWheelUp('O');
std::atomic<int> keyWheelDown('L');

// 开关组合键（0 表示未设置）
std::atomic<int> switchKey1(VK_CONTROL);
std::atomic<int> switchKey2(VK_SHIFT);
std::atomic<int> switchKey3(0);
std::atomic<int> switchKey4(0);

// 功能开关
std::atomic<bool> isRunning(true);
std::atomic<bool> isEnabled(true);
std::atomic<bool> isMinimizeToTray(false);
std::atomic<bool> interceptEnabled(true);

// ==================== 按键状态 ====================
std::atomic<bool> keyE(false), keyS(false), keyD(false), keyF(false);
std::atomic<bool> keyJ(false), keyK(false);
std::atomic<bool> keyO(false), keyL(false), keyM(false);
std::atomic<bool> keyShift(false), keyCtrl(false);

std::atomic<bool> mouseLeftDown(false);
std::atomic<bool> mouseRightDown(false);
std::atomic<bool> mouseMiddleDown(false);

// ==================== 全局句柄 ====================
HHOOK g_hKeyboardHook = NULL;
HWND g_hMainWnd = NULL;
NOTIFYICONDATAA g_nid = {};

// 按键捕获状态
std::atomic<int> waitingButtonId(0);   // 正在等待按键的按钮 ID（0=无）

// 存档相关
std::string g_saveDir;                  // save/ 目录路径
std::vector<std::string> g_saveFiles;   // 存档文件名列表（不含路径，不含扩展名）

// ==================== 按键名称转换 ====================
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
        default: return "Key";
    }
}

// 更新按钮文字
void UpdateButtonText(HWND hwnd, int btnId, int vk) {
    std::string name = GetKeyName(vk);
    SetDlgItemTextA(hwnd, btnId, name.c_str());
}

// 更新所有按键按钮文字
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

// ==================== 存档路径 ====================
// 获取 save/ 目录路径
std::string GetSaveDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string exePath(buf);
    size_t pos = exePath.find_last_of("\\/");
    std::string exeDir = exePath.substr(0, pos);
    return exeDir + "\\save";
}

// 检查文件是否存在
bool FileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

// 创建空存档文件
void CreateSaveFile(const std::string& name) {
    std::string path = g_saveDir + "\\" + name + ".ini";
    if (!FileExists(path)) {
        // 写入默认设置
        char buf[32];
        sprintf(buf, "%d", 2);  WritePrivateProfileStringA("Speed", "MOVE_SPEED", buf, path.c_str());
        sprintf(buf, "%d", 3);  WritePrivateProfileStringA("Speed", "SPEED_MULTIPLIER", buf, path.c_str());
        sprintf(buf, "%d", 80); WritePrivateProfileStringA("Speed", "WHEEL_INTERVAL", buf, path.c_str());
        sprintf(buf, "%d", 1);  WritePrivateProfileStringA("Speed", "WHEEL_SPEED", buf, path.c_str());
        sprintf(buf, "%d", 3);  WritePrivateProfileStringA("Speed", "WHEEL_SPEED_MULTIPLIER", buf, path.c_str());
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
            if (fileName != "save0") {
                g_saveFiles.push_back(fileName);
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }

    // 排序
    std::sort(g_saveFiles.begin(), g_saveFiles.end());
}

// 确保 save/ 目录和三个默认存档存在
void EnsureSaveDir() {
    DWORD attr = GetFileAttributesA(g_saveDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        CreateDirectoryA(g_saveDir.c_str(), NULL);
        CreateSaveFile("save1");
        CreateSaveFile("save2");
        CreateSaveFile("save3");
    }
}

// 把存档列表填入下拉框
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
void SaveToFile(const std::string& name) {
    std::string path = g_saveDir + "\\" + name + ".ini";
    char buf[32];

    sprintf(buf, "%d", MOVE_SPEED.load());
    WritePrivateProfileStringA("Speed", "MOVE_SPEED", buf, path.c_str());
    sprintf(buf, "%d", SPEED_MULTIPLIER.load());
    WritePrivateProfileStringA("Speed", "SPEED_MULTIPLIER", buf, path.c_str());
    sprintf(buf, "%d", WHEEL_INTERVAL.load());
    WritePrivateProfileStringA("Speed", "WHEEL_INTERVAL", buf, path.c_str());
    sprintf(buf, "%d", WHEEL_SPEED.load());
    WritePrivateProfileStringA("Speed", "WHEEL_SPEED", buf, path.c_str());
    sprintf(buf, "%d", WHEEL_SPEED_MULTIPLIER.load());
    WritePrivateProfileStringA("Speed", "WHEEL_SPEED_MULTIPLIER", buf, path.c_str());

    sprintf(buf, "%d", keyUp.load());
    WritePrivateProfileStringA("Keys", "keyUp", buf, path.c_str());
    sprintf(buf, "%d", keyDown.load());
    WritePrivateProfileStringA("Keys", "keyDown", buf, path.c_str());
    sprintf(buf, "%d", keyLeft.load());
    WritePrivateProfileStringA("Keys", "keyLeft", buf, path.c_str());
    sprintf(buf, "%d", keyRight.load());
    WritePrivateProfileStringA("Keys", "keyRight", buf, path.c_str());
    sprintf(buf, "%d", keyLClick.load());
    WritePrivateProfileStringA("Keys", "keyLClick", buf, path.c_str());
    sprintf(buf, "%d", keyRClick.load());
    WritePrivateProfileStringA("Keys", "keyRClick", buf, path.c_str());
    sprintf(buf, "%d", keyMClick.load());
    WritePrivateProfileStringA("Keys", "keyMClick", buf, path.c_str());
    sprintf(buf, "%d", keyWheelUp.load());
    WritePrivateProfileStringA("Keys", "keyWheelUp", buf, path.c_str());
    sprintf(buf, "%d", keyWheelDown.load());
    WritePrivateProfileStringA("Keys", "keyWheelDown", buf, path.c_str());

    sprintf(buf, "%d", switchKey1.load());
    WritePrivateProfileStringA("Switch", "switchKey1", buf, path.c_str());
    sprintf(buf, "%d", switchKey2.load());
    WritePrivateProfileStringA("Switch", "switchKey2", buf, path.c_str());
    sprintf(buf, "%d", switchKey3.load());
    WritePrivateProfileStringA("Switch", "switchKey3", buf, path.c_str());
    sprintf(buf, "%d", switchKey4.load());
    WritePrivateProfileStringA("Switch", "switchKey4", buf, path.c_str());

    WritePrivateProfileStringA("Options", "interceptEnabled",
        interceptEnabled.load() ? "1" : "0", path.c_str());
    WritePrivateProfileStringA("Options", "isMinimizeToTray",
        isMinimizeToTray.load() ? "1" : "0", path.c_str());
}

// 从指定文件读设置
void LoadFromFile(const std::string& name) {
    std::string path = g_saveDir + "\\" + name + ".ini";

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
}

// ==================== 判断是否需要拦截 ====================
bool ShouldIntercept(DWORD vkCode) {
    if (!isEnabled || !interceptEnabled) return false;

// 组合键成员不拦截，保证 GetAsyncKeyState 能读到按键状态
    if (vkCode == switchKey1.load() || vkCode == switchKey2.load() || vkCode == switchKey3.load() || vkCode == switchKey4.load()) return false;

    return vkCode == keyUp.load() || vkCode == keyDown.load() ||
           vkCode == keyLeft.load() || vkCode == keyRight.load() ||
           vkCode == keyLClick.load() || vkCode == keyRClick.load() ||
           vkCode == keyMClick.load() || vkCode == keyWheelUp.load() ||
           vkCode == keyWheelDown.load();
}

// ==================== 记录按键状态 ====================
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
    else if (vkCode == VK_SHIFT) keyShift = down;
    else if (vkCode == VK_CONTROL) keyCtrl = down;
}

// ==================== 键盘钩子 ====================
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;

        // 把左右物理键码归一化为通用键码
        DWORD vk = p->vkCode;
        switch (vk) {
            case VK_LCONTROL: case VK_RCONTROL: vk = VK_CONTROL; break;
            case VK_LSHIFT:   case VK_RSHIFT:   vk = VK_SHIFT;   break;
            case VK_LMENU:    case VK_RMENU:    vk = VK_MENU;    break;
        }

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            // 捕获模式：把按键交给主线程处理，本轮直接吞掉
            int btn = waitingButtonId.load();
            if (btn != 0){
                PostMessageA(g_hMainWnd, WM_BIND_KEY, (WPARAM)vk, (LPARAM)btn);
                return 1;
            }

            if (vk == '0' && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) && GetForegroundWindow() == g_hMainWnd) {
                PostMessageA(g_hMainWnd, WM_RESET, 0, 0);
            }

            SetKeyState(vk, true);
            if (ShouldIntercept(vk)) return 1;
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            SetKeyState(vk, false);
            if (ShouldIntercept(vk)) return 1;
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

// ==================== 鼠标操作 ====================
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

void MouseWheel(int delta) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = delta;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseMoveTo(int x, int y) {
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = (x * 65535) / screenWidth;
    input.mi.dy = (y * 65535) / screenHeight;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &input, sizeof(INPUT));
}



// ==================== 开关检测 ====================

// 判断指定键是否按下（左右分键合并判断）
bool IsKeyDown(int vk) {
    switch (vk) {
        case VK_SHIFT:   return ((GetAsyncKeyState(VK_LSHIFT)   | GetAsyncKeyState(VK_RSHIFT))   & 0x8000) != 0;
        case VK_CONTROL: return ((GetAsyncKeyState(VK_LCONTROL) | GetAsyncKeyState(VK_RCONTROL)) & 0x8000) != 0;
        case VK_MENU:    return ((GetAsyncKeyState(VK_LMENU)    | GetAsyncKeyState(VK_RMENU))    & 0x8000) != 0;
        default:         return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
}

bool IsSwitchPressed() {
    int k1 = switchKey1.load();
    int k2 = switchKey2.load();
    int k3 = switchKey3.load();
    int k4 = switchKey4.load();

    if (k1 == 0 && k2 == 0 && k3 == 0 && k4 == 0) return false;

    bool p1 = (k1 == 0) || IsKeyDown(k1);
    bool p2 = (k2 == 0) || IsKeyDown(k2);
    bool p3 = (k3 == 0) || IsKeyDown(k3);
    bool p4 = (k4 == 0) || IsKeyDown(k4);

    return p1 && p2 && p3 && p4;
}

// ==================== 鼠标控制线程 ====================
void MouseControlThread() {
    bool lastToggleState = false;
    auto lastWheelTime = steady_clock::now();

    while (isRunning) {
        bool currentToggleState = IsSwitchPressed();
        if (currentToggleState && !lastToggleState) {
            isEnabled = !isEnabled;

            if (isEnabled) {
                POINT pt;
                GetCursorPos(&pt);
                MouseMoveTo(pt.x, pt.y);
            }

            HWND hwnd = FindWindowA("MyClass", NULL);
            if (hwnd) {
                std::string title = "光标控制 ";
                title += isEnabled ? "[启用]" : "[禁用]";
                SetWindowTextA(hwnd, title.c_str());
            }
        }
        lastToggleState = currentToggleState;

        // 组合键按住期间跳过映射，避免切换瞬间被组合键成员误触发
        if (currentToggleState)
        {
            MouseLeftUp();
            MouseRightUp();
            MouseMiddleUp();
            std::this_thread::sleep_for(milliseconds(5));
            continue;
        }

        if (!isEnabled) {
            MouseLeftUp();
            MouseRightUp();
            MouseMiddleUp();
            std::this_thread::sleep_for(milliseconds(5));
            continue;
        }

        bool e = keyE.load();
        bool s = keyS.load();
        bool d = keyD.load();
        bool f = keyF.load();
        bool j = keyJ.load();
        bool k = keyK.load();
        bool o = keyO.load();
        bool l = keyL.load();
        bool m = keyM.load();

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

        if (j) MouseLeftDown(); else MouseLeftUp();
        if (k) MouseRightDown(); else MouseRightUp();
        if (m) MouseMiddleDown(); else MouseMiddleUp();

        auto now = steady_clock::now();
        auto elapsed = duration_cast<milliseconds>(now - lastWheelTime).count();

        if ((o || l) && elapsed >= WHEEL_INTERVAL.load()) {
            int speed = WHEEL_SPEED.load();
            if (keyShift.load()) speed *= WHEEL_SPEED_MULTIPLIER.load();

            if (o) MouseWheel(WHEEL_DELTA * speed);
            if (l) MouseWheel(-WHEEL_DELTA * speed);

            lastWheelTime = now;
        }

        std::this_thread::sleep_for(milliseconds(5));
    }

    MouseLeftUp();
    MouseRightUp();
    MouseMiddleUp();
}

// ==================== 托盘图标 ====================
void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP + 1;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    strcpy_s(g_nid.szTip, "光标控制");
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

// ==================== 读取 UI 值到变量 ====================
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

// ==================== 窗口过程 ====================
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            g_hMainWnd = hwnd;

            int y = 10;
            const int labelW = 80;
            const int editW = 60;
            const int rowH = 28;

            CreateWindowA("STATIC", "光标控制",
                WS_VISIBLE | WS_CHILD,
                10, y, 200, 20, hwnd, NULL, NULL, NULL);
            y += 25;

            CreateWindowA("STATIC", "── 速度设置 ──",
                WS_VISIBLE | WS_CHILD,
                10, y, 200, 20, hwnd, NULL, NULL, NULL);
            y += 22;

            CreateWindowA("STATIC", "移动速度:",
                WS_VISIBLE | WS_CHILD, 10, y + 3, labelW, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "2",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                95, y, editW, 22, hwnd, (HMENU)ID_EDIT_SPEED, NULL, NULL);
            y += rowH;

            CreateWindowA("STATIC", "Shift加速:",
                WS_VISIBLE | WS_CHILD, 10, y + 3, labelW, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "3",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                95, y, editW, 22, hwnd, (HMENU)ID_EDIT_SPEED_MUL, NULL, NULL);
            y += rowH;

            CreateWindowA("STATIC", "滚轮间隔(ms):",
                WS_VISIBLE | WS_CHILD, 10, y + 3, 100, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "80",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                115, y, editW, 22, hwnd, (HMENU)ID_EDIT_WHEEL_INTERVAL, NULL, NULL);
            y += rowH;

            CreateWindowA("STATIC", "滚轮速度:",
                WS_VISIBLE | WS_CHILD, 10, y + 3, labelW, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "1",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                95, y, editW, 22, hwnd, (HMENU)ID_EDIT_WHEEL_SPEED, NULL, NULL);
            y += rowH;

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

            int x1 = 10, x2 = 180;
            int btnW = 70;

            CreateWindowA("STATIC", "上:", WS_VISIBLE | WS_CHILD, x1, y + 3, 25, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "E", WS_VISIBLE | WS_CHILD, x1 + 28, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_UP, NULL, NULL);
            CreateWindowA("STATIC", "下:", WS_VISIBLE | WS_CHILD, x2, y + 3, 25, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "D", WS_VISIBLE | WS_CHILD, x2 + 28, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_DOWN, NULL, NULL);
            y += rowH;

            CreateWindowA("STATIC", "左:", WS_VISIBLE | WS_CHILD, x1, y + 3, 25, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "S", WS_VISIBLE | WS_CHILD, x1 + 28, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_LEFT, NULL, NULL);
            CreateWindowA("STATIC", "右:", WS_VISIBLE | WS_CHILD, x2, y + 3, 25, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "F", WS_VISIBLE | WS_CHILD, x2 + 28, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_RIGHT, NULL, NULL);
            y += rowH;

            CreateWindowA("STATIC", "左键:", WS_VISIBLE | WS_CHILD, x1, y + 3, 45, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "J", WS_VISIBLE | WS_CHILD, x1 + 48, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_LCLICK, NULL, NULL);
            CreateWindowA("STATIC", "右键:", WS_VISIBLE | WS_CHILD, x2, y + 3, 45, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "K", WS_VISIBLE | WS_CHILD, x2 + 48, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_RCLICK, NULL, NULL);
            y += rowH;

            CreateWindowA("STATIC", "中键:", WS_VISIBLE | WS_CHILD, x1, y + 3, 45, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "M", WS_VISIBLE | WS_CHILD, x1 + 48, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_MCLICK, NULL, NULL);
            CreateWindowA("STATIC", "上滚:", WS_VISIBLE | WS_CHILD, x2, y + 3, 45, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "O", WS_VISIBLE | WS_CHILD, x2 + 48, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_WHEELUP, NULL, NULL);
            y += rowH;

            CreateWindowA("STATIC", "下滚:", WS_VISIBLE | WS_CHILD, x1, y + 3, 45, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "L", WS_VISIBLE | WS_CHILD, x1 + 48, y, btnW, 24, hwnd, (HMENU)ID_BTN_KEY_WHEELDOWN, NULL, NULL);
            y += rowH + 10;

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
            y += rowH + 10;

            CreateWindowA("BUTTON", "应用", WS_VISIBLE | WS_CHILD, 10, y, 80, 30, hwnd, (HMENU)ID_BTN_APPLY, NULL, NULL);
            CreateWindowA("BUTTON", "重置", WS_VISIBLE | WS_CHILD, 100, y, 80, 30, hwnd, (HMENU)ID_BTN_RESET, NULL, NULL);

            CreateWindowA("STATIC", "重置快捷键：Esc + 0（仅窗口在前台时）",
                WS_VISIBLE | WS_CHILD,
                10, y + 40, 300, 20, hwnd, NULL, NULL, NULL);

            // ==================== 右侧存档区 ====================
            int ry = 10;

            CreateWindowA("STATIC", "── 存档 ──",
                WS_VISIBLE | WS_CHILD,
                350, ry, 160, 20, hwnd, NULL, NULL, NULL);
            ry += 25;

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
            g_saveDir = GetSaveDir();

            // 确保 save/ 目录和三个默认存档存在
            EnsureSaveDir();

            // 扫描目录
            ScanSaveDir();

            // 填下拉框
            FillComboBox(hwnd);

            // 读 save0.ini（默认存档）
            LoadFromFile("save0");

            SetTimer(hwnd, ID_TIMER_REFRESH, 500, NULL);
            break;
        }

        case WM_TIMER: {
            if (wParam == ID_TIMER_REFRESH) {
                if (waitingButtonId.load() == 0) {
                    UpdateAllButtonText(hwnd);
                }
            }
            break;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);

            switch (id) {
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
                    SetFocus(hwnd);
                    break;
                }

                case ID_BTN_CLEAR_SWITCH: {
                    switchKey1 = 0;
                    switchKey2 = 0;
                    switchKey3 = 0;
                    switchKey4 = 0;
                    UpdateAllButtonText(hwnd);
                    break;
                }

                case ID_CHECK_INTERCEPT: {
                    interceptEnabled = (IsDlgButtonChecked(hwnd, ID_CHECK_INTERCEPT) == BST_CHECKED);
                    break;
                }

                case ID_CHECK_TRAY: {
                    isMinimizeToTray = (IsDlgButtonChecked(hwnd, ID_CHECK_TRAY) == BST_CHECKED);
                    break;
                }

                case ID_BTN_APPLY: {
                    ApplyUIValues(hwnd);
                    break;
                }

                case ID_BTN_RESET: {
                    ResetUIValues(hwnd);
                    break;
                }

                case ID_BTN_SAVE: {
                    // 读下拉框当前选中
                    HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
                    int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                    if (sel < 0) break;

                    char name[256];
                    SendMessageA(hCombo, CB_GETLBTEXT, sel, (LPARAM)name);

                    // 如果是 "+"，新建存档
                    if (strcmp(name, "+") == 0) {
                        PostMessageA(hwnd, WM_NEW_SAVE, 0, 0);
                        break;
                    }

                    ApplyUIValues(hwnd);
                    SaveToFile(name);
                    MessageBoxA(hwnd, "保存成功", "提示", MB_OK);
                    break;
                }

                case ID_BTN_LOAD: {
                    HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
                    int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                    if (sel < 0) break;

                    char name[256];
                    SendMessageA(hCombo, CB_GETLBTEXT, sel, (LPARAM)name);

                    if (strcmp(name, "+") == 0) break;

                    LoadFromFile(name);
                    MessageBoxA(hwnd, "加载成功", "提示", MB_OK);
                    break;
                }

                case ID_COMBO_SAVE: {
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        // 用户选了 "+"，弹新建对话框
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

        case WM_NEW_SAVE: {
            // 找第一个不存在的编号，预填
            char defaultName[16] = "save1";
            for (int i = 1; i <= 99; i++) {
                sprintf(defaultName, "save%d", i);
                std::string path = g_saveDir + "\\" + defaultName + ".ini";
                if (!FileExists(path)) break;
            }

            // 简化处理：直接以第一个未占用的 saveN 命名新建存档，暂不提供命名对话框
            ApplyUIValues(hwnd);
            CreateSaveFile(defaultName);
            SaveToFile(defaultName);
            ScanSaveDir();
            FillComboBox(hwnd);

            HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
            int idx = (int)SendMessageA(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)defaultName);
            if (idx != CB_ERR){
                SendMessageA(hCombo, CB_SETCURSEL, idx, 0);
            }

            break;
        }

        case WM_RESET: {
            ResetUIValues(hwnd);
            return 0;
        }

        case WM_BIND_KEY:
        {
            int vk = (int)wParam;
            int btnId = (int)lParam;

            if (vk == VK_ESCAPE) {
                waitingButtonId = 0;
                UpdateAllButtonText(hwnd);
                return 0;
            }

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

            // case WM_KEYDOWN: {
            //     int btnId = waitingButtonId.load();
            //     if (btnId != 0) {
            //         DWORD vk = (DWORD)wParam;

            //         if (btnId == ID_BTN_KEY_UP) keyUp = vk;
            //         else if (btnId == ID_BTN_KEY_DOWN) keyDown = vk;
            //         else if (btnId == ID_BTN_KEY_LEFT) keyLeft = vk;
            //         else if (btnId == ID_BTN_KEY_RIGHT) keyRight = vk;
            //         else if (btnId == ID_BTN_KEY_LCLICK) keyLClick = vk;
            //         else if (btnId == ID_BTN_KEY_RCLICK) keyRClick = vk;
            //         else if (btnId == ID_BTN_KEY_MCLICK) keyMClick = vk;
            //         else if (btnId == ID_BTN_KEY_WHEELUP) keyWheelUp = vk;
            //         else if (btnId == ID_BTN_KEY_WHEELDOWN) keyWheelDown = vk;
            //         else if (btnId == ID_BTN_SWITCH1) switchKey1 = vk;
            //         else if (btnId == ID_BTN_SWITCH2) switchKey2 = vk;
            //         else if (btnId == ID_BTN_SWITCH3) switchKey3 = vk;
            //         else if (btnId == ID_BTN_SWITCH4) switchKey4 = vk;

            //         waitingButtonId = 0;
            //         UpdateAllButtonText(hwnd);
            //     }
            //     break;
            // }

        case WM_CLOSE: {
            ApplyUIValues(hwnd);
            if (isMinimizeToTray) {
                ShowWindow(hwnd, SW_HIDE);
                AddTrayIcon(hwnd);
            } else {
                DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_APP + 1: {
            if (lParam == WM_LBUTTONUP) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                RemoveTrayIcon();
            } else if (lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, 1, "显示窗口");
                AppendMenuA(hMenu, MF_STRING, 2, "退出");
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
                if (cmd == 1) {
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    RemoveTrayIcon();
                } else if (cmd == 2) {
                    RemoveTrayIcon();
                    DestroyWindow(hwnd);
                }
            }
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER_REFRESH);
            RemoveTrayIcon();
            SaveToFile("save0");   // 退出程序前写 save0.ini
            isRunning = false;
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

// ==================== 程序入口点 ====================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "MyClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        0, "MyClass", "光标控制 [启用]",
        WS_OVERLAPPEDWINDOW,
        100, 100, 530, 720,   // 宽度 480 → 530
        NULL, NULL, hInst, NULL
    );

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    g_hKeyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, KeyboardProc, hInst, 0);
    if (g_hKeyboardHook == NULL) {
        MessageBoxA(NULL, "键盘钩子安装失败！", "错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::thread controlThread(MouseControlThread);

    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hKeyboardHook != NULL) {
        UnhookWindowsHookEx(g_hKeyboardHook);
        g_hKeyboardHook = NULL;
    }

    isRunning = false;
    if (controlThread.joinable()) {
        controlThread.join();
    }
    return 0;
}
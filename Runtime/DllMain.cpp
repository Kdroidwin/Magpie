﻿// DllMain.cpp : Runtime.dll 的入口点。
//


#include "pch.h"
#include "MagWindow.h"


HINSTANCE hInstance = NULL;


// DLL 入口
BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        hInstance = hModule;
        break;
    case DLL_PROCESS_DETACH:
        break;
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}


API_DECLSPEC void WINAPI RunMagWindow(
    void reportStatus(int status, const wchar_t* errorMsg),
    const char* scaleModel,
    int captureMode,
    bool showFPS,
    bool lowLatencyMode,
    bool noVSync,
    bool noDisturb
) {
    reportStatus(1, nullptr);

    try {
        HWND hwnd = GetForegroundWindow();
        Debug::ThrowIfWin32Failed(
            hwnd,
            L"GetForegroundWindow 返回 NULL"
        );

        MagWindow::CreateInstance(hInstance, hwnd, scaleModel, captureMode, showFPS, lowLatencyMode, noVSync, noDisturb);
    } catch(const magpie_exception& e) {
        reportStatus(0, e.what().c_str());
        return;
    } catch (...) {
        Debug::WriteErrorMessage(L"创建全屏窗口发生未知错误");
        reportStatus(0, L"未知错误");
        return;
    }
    
    reportStatus(2, nullptr);

    // 主消息循环
    while (true) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                reportStatus(0, nullptr);
                return;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        MagWindow::$instance->Render();
    }
}

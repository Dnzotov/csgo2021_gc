#include "stdafx.h"
#include "platform.h"
#include "steam_hook.h"
#include "test_accept.h"
#include "test_diag.h"

#if defined(_MSC_VER)
#define DLL_EXPORT extern "C" __declspec(dllexport)
#elif defined(__GNUC__)
#define DLL_EXPORT extern "C" __attribute__((visibility("default")))
#else
#error
#endif

DLL_EXPORT void InstallGC(bool dedicated)
{
    Platform::Initialize();
    SteamHookInstall(dedicated);

    if (!dedicated)
    {
        // TEST ONLY: lets ClientGC see the reserved server's 0x25 (stage 2 / awaiting 0) responses
        AcceptTest::InstallClientRecvHook();
        AcceptTest::DiagInstallNetHooks();
    }
}

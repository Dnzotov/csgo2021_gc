#include "stdafx.h"
#include "test_diag.h"
#include "config.h"
#include "gc_shared.h"

#include <chrono>
#include <cstdarg>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <funchook.h>
#endif

namespace AcceptTest
{

namespace
{

std::atomic<uint32_t> s_attempt{ 0 };
std::atomic<uint32_t> s_gc9107Seq{ 0 };
std::atomic<uint32_t> s_client9107Seq{ 0 };
std::atomic<int64_t> s_epochMs{ 0 };
std::atomic<uint32_t> s_finalGameType{ 0 };
std::atomic<bool> s_handlerHooked{ false };

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char *GcMessageName(uint32_t type)
{
    return MessageName(type);
}

} // namespace

bool DiagEnabled()
{
    static const bool s_enabled = GetConfig().TestDiag();
    return s_enabled;
}

void DiagLog(const char *format, ...)
{
    if (!DiagEnabled())
    {
        return;
    }

    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    int64_t elapsed = NowMs() - s_epochMs.load();
    Platform::Print("[MM-DIAG a%u t=+%lld.%03llds] %s\n", s_attempt.load(),
        static_cast<long long>(elapsed / 1000), static_cast<long long>(elapsed % 1000), text);
}

void DiagBeginAttempt(uint32_t gameType)
{
    if (!DiagEnabled())
    {
        return;
    }

    s_attempt++;
    s_gc9107Seq = 0;
    s_client9107Seq = 0;
    s_epochMs = NowMs();

    DiagLog("---- MatchmakingStart (9101) received, game_type=%u (eGame=%u) ----", gameType, gameType & 0xF);
}

void DiagLog9107(const CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve &message)
{
    if (!DiagEnabled())
    {
        return;
    }

    uint32_t seq = ++s_gc9107Seq;

    const bool hasReservation = message.has_reservation();
    char reservation[64];
    if (hasReservation)
    {
        snprintf(reservation, sizeof(reservation), "yes, reservation.game_type=%u (&0xF=%u)",
            message.reservation().game_type(), message.reservation().game_type() & 0xF);
    }
    else
    {
        snprintf(reservation, sizeof(reservation), "no (client reads game_type=0)");
    }

    uint32_t ip = message.direct_udp_ip();
    DiagLog("GC->client 9107 #%u SENT: reservation=%s reservationid=%llx map=%s direct_udp=%u.%u.%u.%u:%u server_address=%s",
        seq, reservation, static_cast<unsigned long long>(message.reservationid()), message.map().c_str(),
        (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, message.direct_udp_port(),
        message.server_address().c_str());
}

void DiagOnGcMessageDelivered(uint32_t type)
{
    if (!DiagEnabled())
    {
        return;
    }

    uint32_t unmasked = type & 0x7fffffffu;
    if (unmasked >= 9100 && unmasked < 9200)
    {
        DiagLog("client RETRIEVED GC message %s (%u)", GcMessageName(unmasked), unmasked);
    }
}

void DiagOnDatagram19(const void *packet, const void *from, int fromlen)
{
#ifdef _WIN32
    const uint8_t *p = static_cast<const uint8_t *>(packet);

    uint32_t header;
    memcpy(&header, p, sizeof(header));
    if (header != 0xFFFFFFFFu || p[4] != 0x25)
    {
        return;
    }

    uint32_t stage;
    memcpy(&stage, p + 13, sizeof(stage));

    char address[40] = "?";
    const sockaddr_in *fromIn = static_cast<const sockaddr_in *>(from);
    if (from && fromlen >= static_cast<int>(sizeof(sockaddr_in)) && fromIn->sin_family == AF_INET)
    {
        uint32_t ip = ntohl(fromIn->sin_addr.s_addr);
        snprintf(address, sizeof(address), "%u.%u.%u.%u:%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff,
            ip & 0xff, ntohs(fromIn->sin_port));
    }

    DiagLog("UDP 0x25 RECV from %s: stage=%u awaiting=%u total=%u%s", address, stage, p[17], p[18],
        (stage == 2 && p[17] == 0) ? "   <== stage 2 awaiting 0" : "");
#else
    (void)packet;
    (void)from;
    (void)fromlen;
#endif
}

#ifdef _WIN32

namespace
{

// ---- network: client 0x21 -------------------------------------------------------------------------

thread_local int s_sendDepth;

void LogDatagramSent(const void *data, int length, const sockaddr *to, int tolen)
{
    if (length != 33)
    {
        return;
    }

    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint32_t header;
    memcpy(&header, p, sizeof(header));
    if (header != 0xFFFFFFFFu || p[4] != 0x21)
    {
        return;
    }

    uint32_t stage;
    uint64_t cookie, steamId;
    memcpy(&stage, p + 13, sizeof(stage));
    memcpy(&cookie, p + 17, sizeof(cookie));
    memcpy(&steamId, p + 25, sizeof(steamId));

    char address[40] = "?";
    const sockaddr_in *toIn = reinterpret_cast<const sockaddr_in *>(to);
    if (to && tolen >= static_cast<int>(sizeof(sockaddr_in)) && toIn->sin_family == AF_INET)
    {
        uint32_t ip = ntohl(toIn->sin_addr.s_addr);
        snprintf(address, sizeof(address), "%u.%u.%u.%u:%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff,
            ip & 0xff, ntohs(toIn->sin_port));
    }

    DiagLog("UDP 0x21 SENT to %s: stage=%u cookie=%llx account=%u%s", address, stage,
        static_cast<unsigned long long>(cookie), static_cast<uint32_t>(steamId & 0xffffffffu),
        stage == 2 ? "   <== stage 2" : "");
}

int(WSAAPI *Og_sendto)(SOCKET s, const char *buf, int len, int flags, const sockaddr *to, int tolen);
int(WSAAPI *Og_WSASendTo)(SOCKET s, LPWSABUF buffers, DWORD bufferCount, LPDWORD bytesSent, DWORD flags,
    const sockaddr *to, int tolen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion);

int WSAAPI Hk_sendto(SOCKET s, const char *buf, int len, int flags, const sockaddr *to, int tolen)
{
    if (s_sendDepth == 0 && buf)
    {
        LogDatagramSent(buf, len, to, tolen);
    }

    s_sendDepth++;
    int result = Og_sendto(s, buf, len, flags, to, tolen);
    s_sendDepth--;
    return result;
}

int WSAAPI Hk_WSASendTo(SOCKET s, LPWSABUF buffers, DWORD bufferCount, LPDWORD bytesSent, DWORD flags,
    const sockaddr *to, int tolen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    if (s_sendDepth == 0 && bufferCount == 1 && buffers)
    {
        LogDatagramSent(buffers[0].buf, static_cast<int>(buffers[0].len), to, tolen);
    }

    s_sendDepth++;
    int result = Og_WSASendTo(s, buffers, bufferCount, bytesSent, flags, to, tolen, overlapped, completion);
    s_sendDepth--;
    return result;
}

bool HookFunction(const char *name, void *target, void *hook, void **bridge)
{
    funchook_t *funchook = funchook_create();
    void *temp = target;
    if (!funchook || funchook_prepare(funchook, &temp, hook) != 0 || funchook_install(funchook, 0) != 0)
    {
        Platform::Print("[MM-DIAG] hooking %s failed\n", name);
        return false;
    }

    *bridge = temp;
    return true;
}

// ---- client.dll -------------------------------------------------------------------------------------
// RVAs are the ones of the live client.dll (build 1352, sha1 53ba71b7ed66a9445cb5eb7783b85c909fbddda9),
// RESEARCH_FINDINGS.md #38.14/#43. Every hook checks the function's first bytes before installing.

uintptr_t s_clientBase;

constexpr uintptr_t IdaBase = 0x10000000;
uintptr_t Rva(uintptr_t idaAddress) { return idaAddress - IdaBase; }
template<typename T> T ClientPtr(uintptr_t idaAddress) { return reinterpret_cast<T>(s_clientBase + Rva(idaAddress)); }

// string helpers must not contain C++ objects with destructors (they use SEH)
void SafeCopyString(const void *pointer, char *out, size_t outSize)
{
    strcpy_s(out, outSize, "(n/a)");
    if (reinterpret_cast<uintptr_t>(pointer) < 0x10000)
    {
        return;
    }

    __try
    {
        const char *s = static_cast<const char *>(pointer);
        size_t i = 0;
        for (; i + 1 < outSize && s[i]; i++)
        {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 32 || c > 126)
            {
                if (i == 0)
                {
                    return;
                }

                break;
            }

            out[i] = static_cast<char>(c);
        }

        out[i] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        strcpy_s(out, outSize, "(unreadable)");
    }
}

// game/mmqueue exactly like sub_103F7C00/sub_103F49F0 read it: session = sessionMgr->vfn13(), session->vfn1(),
// then the KeyValues string getter sub_1094F690("game/mmqueue", &Locale)
void ReadMmQueue(char *out, size_t outSize)
{
    strcpy_s(out, outSize, "(n/a)");

    __try
    {
        int manager = *ClientPtr<int *>(0x152F358C);
        if (!manager)
        {
            strcpy_s(out, outSize, "(no match framework)");
            return;
        }

        int session = reinterpret_cast<int(__thiscall *)(int)>((*reinterpret_cast<int **>(manager))[13])(manager);
        if (!session)
        {
            strcpy_s(out, outSize, "(no session)");
            return;
        }

        reinterpret_cast<void(__thiscall *)(int)>((*reinterpret_cast<int **>(session))[1])(session);

        const char *value = ClientPtr<const char *(__stdcall *)(const char *, int)>(0x1094F690)(
            "game/mmqueue", static_cast<int>(s_clientBase + Rva(0x10B3E1B7)));

        char buffer[64];
        SafeCopyString(value, buffer, sizeof(buffer));
        snprintf(out, outSize, "\"%s\"", buffer);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        strcpy_s(out, outSize, "(exception)");
    }
}

// the active reservation-check callback (dword_152EAD0C): stage (+140), mode (+132), game_type (+92), reservationid (+40)
void DescribeActiveCallback(char *out, size_t outSize)
{
    strcpy_s(out, outSize, "none");

    __try
    {
        int callback = *ClientPtr<int *>(0x152EAD0C);
        if (callback)
        {
            unsigned long long reservationId;
            memcpy(&reservationId, reinterpret_cast<const void *>(callback + 40), sizeof(reservationId));
            snprintf(out, outSize, "%p(stage=%u mode=%u game_type=%u reservationid=%llx)", reinterpret_cast<void *>(callback),
                *reinterpret_cast<unsigned *>(callback + 140), *reinterpret_cast<unsigned *>(callback + 132),
                *reinterpret_cast<unsigned *>(callback + 92), reservationId);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        strcpy_s(out, outSize, "(exception)");
    }
}

int ReadCallbackState(int request)
{
    __try
    {
        return reinterpret_cast<int(__thiscall *)(int)>((*reinterpret_cast<int **>(request))[1])(request);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

// Post-Accept fix: right after the 9107 handler built its callback, give it the accept game_type (see header).
// Returns 0 = patched, 1 = no active callback, 2 = unexpected callback state (left untouched), 3 = exception
int PatchCallbackGameType(uint32_t gameType, unsigned *stage, unsigned *mode, unsigned *previous)
{
    __try
    {
        int callback = *ClientPtr<int *>(0x152EAD0C);
        if (!callback)
        {
            return 1;
        }

        *stage = *reinterpret_cast<unsigned *>(callback + 140);
        *mode = *reinterpret_cast<unsigned *>(callback + 132);
        *previous = *reinterpret_cast<unsigned *>(callback + 92);

        // exactly what the handler builds for a 9107 without reservation: (stage 2, mode 2, game_type 0)
        if (*stage != 2 || *mode != 2 || *previous != 0)
        {
            return 2;
        }

        *reinterpret_cast<unsigned *>(callback + 92) = gameType;
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 3;
    }
}

void ApplyFinalAcceptGameType()
{
    uint32_t gameType = s_finalGameType.exchange(0);
    if (!gameType)
    {
        return;
    }

    unsigned stage = 0, mode = 0, previous = 0;
    switch (PatchCallbackGameType(gameType, &stage, &mode, &previous))
    {
    case 0:
        Platform::Print("[MM-ACCEPT] final transition: callback (stage=%u mode=%u) game_type %u -> %u "
            "(accept-type connect, no second Match Found popup)\n", stage, mode, previous, gameType);
        break;

    case 1:
        Platform::Print("[MM-ACCEPT] final transition: 9107 handler left no active callback, nothing patched\n");
        break;

    case 2:
        Platform::Print("[MM-ACCEPT] final transition: unexpected callback (stage=%u mode=%u game_type=%u), left untouched\n",
            stage, mode, previous);
        break;

    default:
        Platform::Print("[MM-ACCEPT] final transition: exception while patching the callback, left untouched\n");
        break;
    }
}

// sub_103F49F0: MatchmakingGC2ClientReserve (9107) job handler
char(__stdcall *Og_Handler9107)(int);
char __stdcall Hk_Handler9107(int a1)
{
    uint32_t seq = ++s_client9107Seq;
    const bool diag = DiagEnabled();

    char mmqueue[80], callback[160];
    if (diag)
    {
        ReadMmQueue(mmqueue, sizeof(mmqueue));
        DescribeActiveCallback(callback, sizeof(callback));
        DiagLog("client.dll 9107 handler ENTER (#%u): game/mmqueue=%s active callback=%s", seq, mmqueue, callback);
    }

    char result = Og_Handler9107(a1);

    ApplyFinalAcceptGameType();

    if (diag)
    {
        ReadMmQueue(mmqueue, sizeof(mmqueue));
        DescribeActiveCallback(callback, sizeof(callback));
        DiagLog("client.dll 9107 handler EXIT  (#%u): game/mmqueue=%s active callback=%s", seq, mmqueue, callback);
    }

    return result;
}

// sub_103F7C00: CServerConfirmedReservationCheckCallback::vftable[0] (0x25 status changes)
void(__fastcall *Og_Callback)(int, int, int);
void __fastcall Hk_Callback(int self, int edx, int request)
{
    bool relevant = false;
    int state = -2;
    char mmqueue[80], callback[160];

    __try
    {
        relevant = request && request == *reinterpret_cast<int *>(self + 128);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    if (relevant)
    {
        state = ReadCallbackState(request);
        ReadMmQueue(mmqueue, sizeof(mmqueue));
        DescribeActiveCallback(callback, sizeof(callback));
        DiagLog("client.dll reservation callback ENTER: request state=%d game/mmqueue=%s active callback=%s "
            "(state 4 = server confirmed)", state, mmqueue, callback);
    }

    Og_Callback(self, edx, request);

    if (relevant)
    {
        // self may be gone by now (the callback destroys itself when it is done)
        ReadMmQueue(mmqueue, sizeof(mmqueue));
        DescribeActiveCallback(callback, sizeof(callback));
        DiagLog("client.dll reservation callback EXIT : game/mmqueue=%s active callback=%s", mmqueue, callback);
    }
}

// sub_103F7980: callback constructor (this, a2 = source, stage, mode)
int(__fastcall *Og_CallbackCtor)(int, int, int, int, int);
int __fastcall Hk_CallbackCtor(int self, int edx, int source, int stage, int mode)
{
    DiagLog("client.dll reservation callback CREATED: stage=%d mode=%d%s", stage, mode,
        (stage == 1 && mode == 0) ? "  (accept / ready-up variant)" : (mode == 2 ? "  (direct connect variant)" : ""));
    return Og_CallbackCtor(self, edx, source, stage, mode);
}

// sub_103F7B60: Accept (stage -> 2, new 0x21)
char(__stdcall *Og_Accept)(int);
char __stdcall Hk_Accept(int a1)
{
    char callback[160];
    DescribeActiveCallback(callback, sizeof(callback));
    DiagLog("client.dll ACCEPT (SetLocalPlayerReady accept -> stage 2) active callback=%s", callback);
    return Og_Accept(a1);
}

// sub_103FB5C0: raises the ServerReserved Panorama event (Match Found popup, map starting with '@' = popup without Accept)
int(__fastcall *Og_ServerReserved)(int, int);
int __fastcall Hk_ServerReserved(int a1, int a2)
{
    char first[48], second[48];
    SafeCopyString(reinterpret_cast<const void *>(static_cast<uintptr_t>(a1)), first, sizeof(first));
    SafeCopyString(reinterpret_cast<const void *>(static_cast<uintptr_t>(a2)), second, sizeof(second));
    DiagLog("client.dll ServerReserved event RAISED (Match Found popup): a1=0x%x('%s') a2=0x%x('%s')", a1, first, a2, second);
    return Og_ServerReserved(a1, a2);
}

// sub_10432490: PlaySoundEffect(name, 0) -- popup_accept_match_found / popup_accept_match_confirmed
int(__stdcall *Og_PlaySound)(int, int);
int __stdcall Hk_PlaySound(int name, int a2)
{
    char text[64];
    SafeCopyString(reinterpret_cast<const void *>(static_cast<uintptr_t>(name)), text, sizeof(text));
    DiagLog("client.dll PlaySoundEffect('%s')", text);
    return Og_PlaySound(name, a2);
}

// sub_103F7240: stores the QueueConnect KV and raises QueueConnectToServer (a2 == 1: non-accept variant, +2000 ms helper_time)
char(__fastcall *Og_StoreQueueConnect)(int, int);
char __fastcall Hk_StoreQueueConnect(int keyValues, int variant)
{
    DiagLog("client.dll QueueConnectToServer RAISED (QueueConnect KV stored, variant=%d%s)", variant,
        variant == 1 ? ", '@map' popup variant, connect delayed +2000ms" : "");
    return Og_StoreQueueConnect(keyValues, variant);
}

// sub_105C2DB0: RaiseReadyUp(bool, int, int) -> PanoramaComponent_Lobby_ReadyUpForMatch
int(__fastcall *Og_RaiseReadyUp)(int, int, int, int, int);
int __fastcall Hk_RaiseReadyUp(int self, int edx, int flag, int a3, int a4)
{
    DiagLog("client.dll RaiseReadyUp(bool=%d, %d, %d) -> JS PopupAcceptMatch.ReadyForMatch", flag & 0xff, a3, a4);
    return Og_RaiseReadyUp(self, edx, flag, a3, a4);
}

struct ClientHook
{
    const char *name;
    uintptr_t address;       // IDA address in client.dll
    const char *prologue;    // expected first bytes (hex)
    void *hook;
    void **original;
    bool diagOnly;           // false: also needed by the post-Accept fix, installed even when test_diag is off
};

bool PrologueMatches(uintptr_t address, const char *hex)
{
    const uint8_t *code = reinterpret_cast<const uint8_t *>(s_clientBase + Rva(address));
    for (size_t i = 0; hex[i * 2] && hex[i * 2 + 1]; i++)
    {
        unsigned byte;
        if (sscanf_s(hex + i * 2, "%2x", &byte) != 1 || code[i] != byte)
        {
            return false;
        }
    }

    return true;
}

} // namespace

void DiagInstallNetHooks()
{
    if (!DiagEnabled())
    {
        return;
    }

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2)
    {
        ws2 = LoadLibraryA("ws2_32.dll");
    }

    void *sendtoAddress = ws2 ? reinterpret_cast<void *>(GetProcAddress(ws2, "sendto")) : nullptr;
    void *wsaSendToAddress = ws2 ? reinterpret_cast<void *>(GetProcAddress(ws2, "WSASendTo")) : nullptr;

    bool ok = true;
    if (sendtoAddress)
    {
        ok &= HookFunction("ws2_32!sendto", sendtoAddress, reinterpret_cast<void *>(Hk_sendto), reinterpret_cast<void **>(&Og_sendto));
    }

    if (wsaSendToAddress)
    {
        ok &= HookFunction("ws2_32!WSASendTo", wsaSendToAddress, reinterpret_cast<void *>(Hk_WSASendTo), reinterpret_cast<void **>(&Og_WSASendTo));
    }

    Platform::Print("[MM-DIAG] network hooks (sendto/WSASendTo) %s\n", ok ? "installed" : "PARTIAL/FAILED");
}

void DiagTick()
{
    static bool s_done;
    if (s_done)
    {
        return;
    }

    HMODULE module = GetModuleHandleA("client.dll");
    if (!module)
    {
        return;
    }

    s_done = true;
    s_clientBase = reinterpret_cast<uintptr_t>(module);

    const ClientHook hooks[] = {
        { "9107 handler sub_103F49F0", 0x103F49F0, "558bec83e4f881ecdc000000", reinterpret_cast<void *>(Hk_Handler9107), reinterpret_cast<void **>(&Og_Handler9107), false },
        { "reservation callback sub_103F7C00", 0x103F7C00, "558bec83e4f881ec74010000", reinterpret_cast<void *>(Hk_Callback), reinterpret_cast<void **>(&Og_Callback), true },
        { "callback ctor sub_103F7980", 0x103F7980, "558bec83ec28535657ff7508", reinterpret_cast<void *>(Hk_CallbackCtor), reinterpret_cast<void **>(&Og_CallbackCtor), true },
        { "accept sub_103F7B60", 0x103F7B60, "558bec51568b350cad2e1557", reinterpret_cast<void *>(Hk_Accept), reinterpret_cast<void **>(&Og_Accept), true },
        { "ServerReserved raiser sub_103FB5C0", 0x103FB5C0, "8b0dd4362f155368b8cedb10", reinterpret_cast<void *>(Hk_ServerReserved), reinterpret_cast<void **>(&Og_ServerReserved), true },
        { "PlaySoundEffect sub_10432490", 0x10432490, "558bec8b450c85c08b0dd436", reinterpret_cast<void *>(Hk_PlaySound), reinterpret_cast<void **>(&Og_PlaySound), true },
        { "QueueConnect store sub_103F7240", 0x103F7240, "558bec83e4f85153568bf157", reinterpret_cast<void *>(Hk_StoreQueueConnect), reinterpret_cast<void **>(&Og_StoreQueueConnect), true },
        { "RaiseReadyUp sub_105C2DB0", 0x105C2DB0, "558bec538a5d08568bf184db", reinterpret_cast<void *>(Hk_RaiseReadyUp), reinterpret_cast<void **>(&Og_RaiseReadyUp), true },
    };

    int installed = 0;
    for (const ClientHook &hook : hooks)
    {
        if (hook.diagOnly && !DiagEnabled())
        {
            continue;
        }

        if (!PrologueMatches(hook.address, hook.prologue))
        {
            Platform::Print("[MM-DIAG] client.dll %s: unexpected prologue (different build?), not hooked\n", hook.name);
            continue;
        }

        if (HookFunction(hook.name, reinterpret_cast<void *>(s_clientBase + Rva(hook.address)), hook.hook, hook.original))
        {
            installed++;

            if (hook.original == reinterpret_cast<void **>(&Og_Handler9107))
            {
                s_handlerHooked = true;
            }
        }
    }

    Platform::Print("[MM-DIAG] client.dll base=%p, %d/%zu diagnostic hooks installed\n", reinterpret_cast<void *>(s_clientBase),
        installed, sizeof(hooks) / sizeof(hooks[0]));
}

#else // !_WIN32

void DiagInstallNetHooks() {}
void DiagTick() {}

#endif

bool SetFinalAcceptGameType(uint32_t eGame)
{
    if (!s_handlerHooked)
    {
        return false;
    }

    s_finalGameType = eGame & 0xF;
    return true;
}

} // namespace AcceptTest

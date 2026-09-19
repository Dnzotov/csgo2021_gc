#pragma once

// TEST ONLY runtime diagnostics for the (working) Accept flow, see RESEARCH_FINDINGS.md #43.
// Everything here only logs ("[MM-DIAG a<attempt> t=+<seconds>]"), no behavior is changed:
//   * GC side: every MatchmakingGC2ClientReserve (9107) we hand to the game and every matchmaking message
//     the game actually retrieves from the GC
//   * client.dll side (inline hooks that call the original): 9107 handler, reservation-check callback, callback
//     ctor, Accept handler, ServerReserved raiser, popup sound, RaiseReadyUp, QueueConnect KV store
//   * network side: client 0x21 datagrams sent, 0x25 datagrams received
// Controlled by matchmaking.test_diag in config.txt (default on). Client (csgo.exe) only, Windows only.

#include <cstdint>

class CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve;

namespace AcceptTest
{

bool DiagEnabled();

// 9101 arrived: new attempt number, sequence counters and the timestamp origin are reset
void DiagBeginAttempt(uint32_t gameType);

void DiagLog(const char *format, ...);

// contents of a 9107 we are about to send to the game, numbered within the attempt
void DiagLog9107(const CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve &message);

// a GC message was just handed to the game via ISteamGameCoordinator::RetrieveMessage (masked type)
void DiagOnGcMessageDelivered(uint32_t type);

// a 19-byte datagram was received (candidate 0x25), from is a sockaddr_in
void DiagOnDatagram19(const void *packet, const void *from, int fromlen);

// ws2_32!sendto / WSASendTo hooks (logs client 0x21), installed once from InstallGC on the main thread
void DiagInstallNetHooks();

// called every frame from the main thread; installs the client.dll hooks as soon as the module is loaded
void DiagTick();

// ---- post-Accept final transition fix (RESEARCH_FINDINGS.md #44) ------------------------------------------
// The final MatchmakingGC2ClientReserve (9107) of an Accept mode must not carry an accept-type reservation (the
// 9107 handler would rebuild the ready-up callback and show the Accept popup again), but without one the handler
// builds the callback with game_type 0, and the callback then treats the connect as a non-accept match and shows
// the second "@map" Match Found popup. SetFinalAcceptGameType() arms a one-shot: right after the 9107 handler
// created its callback, client.dll's callback game_type (callback+92) is set to eGame, i.e. the state a retail
// accept-mode connect callback has. Returns false when the client.dll hook is not installed (nothing armed).
bool SetFinalAcceptGameType(uint32_t eGame);

} // namespace AcceptTest

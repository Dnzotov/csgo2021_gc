#pragma once

// EXPERIMENTAL, isolated test component -- see RESEARCH_FINDINGS.md #26 for the wire
// protocol this implements (A2S_RESERVE_CHECK 0x21 -> S2A_RESERVE_CHECK_RESPONSE 0x25,
// byte-exact retail/2021 layout, RE-confirmed via engine.dll raw disassembly and
// engine/baseserver.cpp source). This is NOT a real matchmaking queue: it always answers
// with awaiting_clients=0 (immediate success), regardless of who's asking or what the
// reservation cookie is. Only meant to prove that the retail client-side reservation-check
// pipeline can be driven to "queue connect" by our own GC + this responder.
//
// Not mixed into gc_client.cpp's matchmaking logic on purpose -- see the task that asked
// for this file for the reasoning.
class TestMM
{
public:
    // Starts a background UDP responder thread bound to 0.0.0.0:port, if one for this
    // exact port isn't already running. Safe to call repeatedly (e.g. once per
    // MatchmakingStart). Never blocks the caller.
    static void EnsureStarted(uint16_t port);
};

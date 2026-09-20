#include "stdafx.h"
#include "server_roster.h"
#include "backend_client.h"

#include <algorithm>
#include <chrono>

namespace RosterFeed
{

namespace
{

constexpr auto PollInterval = std::chrono::milliseconds(1000);
constexpr int MaxConfirmAttempts = 8; // a confirmation that does not get through for this many polls is dropped

std::string Join(const std::vector<uint32_t> &ids)
{
    std::string text;
    char token[16];
    for (uint32_t id : ids)
    {
        snprintf(token, sizeof(token), "%s%x", text.empty() ? "" : ",", id);
        text += token;
    }

    return text;
}

// the answer in one line for the log
std::string Describe(const Snapshot &s)
{
    switch (s.state)
    {
    case Snapshot::State::Match:
    {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "match %s %s %s: %zu participants (%zu real + %u fake) of %u",
            s.matchId.c_str(), s.status.c_str(), s.mode.c_str(), s.participants.size(),
            s.participants.size() - s.fakeCount, s.fakeCount, s.requiredPlayers);
        return buffer;
    }

    case Snapshot::State::NoMatch:
        return "no match on this server (" + s.message + ")";

    default:
        return "backend not available (" + s.message + ")";
    }
}

} // namespace

std::string Serialize(const Snapshot &s)
{
    // one field per line; the message is last-but-one and never contains a newline (single line by construction)
    std::string message = s.message;
    for (char &c : message)
    {
        if (c == '\n' || c == '\r')
        {
            c = ' ';
        }
    }

    std::string text = std::to_string(static_cast<int>(s.state)) + "\n" + s.matchId + "\n" + s.mode + "\n" + s.status + "\n"
        + (s.acceptRequired ? "1" : "0") + "\n" + std::to_string(s.requiredPlayers) + "\n" + std::to_string(s.fakeCount) + "\n"
        + Join(s.participants) + "\n" + message + "\n";
    return text;
}

bool Deserialize(const std::string &text, Snapshot &s)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t end = text.find('\n', start);
        if (end == std::string::npos)
        {
            break;
        }

        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }

    if (lines.size() < 9)
    {
        return false;
    }

    s = {};
    const int state = atoi(lines[0].c_str());
    if (state < 0 || state > 2)
    {
        return false;
    }

    s.state = static_cast<Snapshot::State>(state);
    s.matchId = lines[1];
    s.mode = lines[2];
    s.status = lines[3];
    s.acceptRequired = lines[4] == "1";
    s.requiredPlayers = static_cast<uint32_t>(strtoul(lines[5].c_str(), nullptr, 10));
    s.fakeCount = static_cast<uint32_t>(strtoul(lines[6].c_str(), nullptr, 10));
    for (size_t p = 0; p < lines[7].size();)
    {
        const size_t comma = lines[7].find(',', p);
        const std::string token = lines[7].substr(p, comma == std::string::npos ? std::string::npos : comma - p);
        if (!token.empty())
        {
            s.participants.push_back(static_cast<uint32_t>(strtoul(token.c_str(), nullptr, 16)));
        }

        p = comma == std::string::npos ? lines[7].size() : comma + 1;
    }

    s.message = lines[8];
    return true;
}

// ---- Controller ---------------------------------------------------------------------------------------------

Controller::Controller(std::string modeName, uint32_t requiredPlayers, Host host)
    : m_modeName{ std::move(modeName) }
    , m_requiredPlayers{ requiredPlayers }
    , m_host{ std::move(host) }
{
}

bool Controller::InArmedRoster(uint32_t accountId) const
{
    return std::find(m_armedParticipants.begin(), m_armedParticipants.end(), accountId) != m_armedParticipants.end();
}

void Controller::OnSnapshot(const Snapshot &snapshot)
{
    m_snapshot = snapshot;
    Apply();
}

void Controller::OnFakesReady(bool ready)
{
    m_fakesReady = ready;
    ConfirmIfReady();
}

void Controller::OnLegacyArmed(const std::vector<uint32_t> &participants)
{
    m_armed = true;
    m_armedParticipants = participants;
    m_armedSource = "legacy";
    m_armedFromBackend = false;
    m_armedMatchId.clear();
    m_fakesReady = false;
}

void Controller::OnPlayersChanged()
{
    Apply();
}

void Controller::Apply()
{
    m_usable = false;

    if (m_snapshot.state == Snapshot::State::NoMatch && m_armed && m_armedFromBackend)
    {
        // The backend answered that this server has no match, yet a backend match is armed: it was cancelled (Accept timeout,
        // a player left) or it ended. Not while somebody is on the server - that match is being played.
        if (m_host.playerOnServer())
        {
            return;
        }

        const std::string matchId = m_armedMatchId;
        Platform::Print("[MM-ACCEPT] backend match %s is gone (%s): releasing its reservation\n", matchId.c_str(),
            m_snapshot.message.c_str());
        m_armed = false;
        m_armedParticipants.clear();
        m_armedSource.clear();
        m_armedFromBackend = false;
        m_armedMatchId.clear();
        m_fakesReady = false;
        if (m_host.release)
        {
            m_host.release(matchId);
        }

        return;
    }

    if (m_snapshot.state != Snapshot::State::Match || !m_snapshot.Complete())
    {
        return; // no match / backend down / still gathering real players: nothing to arm (the legacy roster stays in charge)
    }

    const Snapshot &roster = m_snapshot;
    const bool realPresent = std::any_of(roster.participants.begin(), roster.participants.end(),
        [](uint32_t id) { return (id & 0xFFFF0000u) != 0xFA4E0000u; }); // AcceptTest::IsFakeAccountId
    if (!roster.acceptRequired || roster.mode != m_modeName || roster.participants.size() != m_requiredPlayers || !realPresent)
    {
        Platform::Print("[MM-ACCEPT] backend roster of match %s (%s, %zu participants) does not fit this srcds (%s needs %u): "
            "using the legacy roster, the players are not held\n",
            roster.matchId.c_str(), roster.mode.c_str(), roster.participants.size(), m_modeName.c_str(), m_requiredPlayers);
        if (m_confirmedMatchId != roster.matchId)
        {
            m_confirmedMatchId = roster.matchId;
            m_host.confirm(roster.matchId); // nothing to wait for: the backend hands the server out at once
        }

        return;
    }

    m_usable = true;

    if (m_armed && m_armedParticipants == roster.participants)
    {
        // already armed with exactly this roster (e.g. the same players again): keep it, it is at stage 1 already
        m_armedSource = "backend match " + roster.matchId;
        m_armedMatchId = roster.matchId;
        m_armedFromBackend = true;
        ConfirmIfReady();
        return;
    }

    if (m_host.playerOnServer())
    {
        Platform::Print("[MM-ACCEPT] backend roster of match %s waits: a player is still on this server\n", roster.matchId.c_str());
        return;
    }

    const bool replacing = m_armed;
    m_armed = true;
    m_armedParticipants = roster.participants;
    m_armedSource = "backend match " + roster.matchId;
    m_armedMatchId = roster.matchId;
    m_armedFromBackend = true;
    m_fakesReady = false;
    m_host.arm(roster.participants, m_armedSource, replacing);
}

// the reservation is ready for the real player (this roster armed, every fake at stage 1): tell the backend
void Controller::ConfirmIfReady()
{
    if (!m_armed || !m_fakesReady || !m_usable || !m_snapshot.Complete() || m_armedParticipants != m_snapshot.participants
        || m_confirmedMatchId == m_snapshot.matchId)
    {
        return;
    }

    m_confirmedMatchId = m_snapshot.matchId;
    Platform::Print("[MM-ACCEPT] roster of match %s is armed and every fake participant is at stage 1: telling the backend\n",
        m_confirmedMatchId.c_str());
    m_host.confirm(m_confirmedMatchId);
}

// ---- Poller -------------------------------------------------------------------------------------------------

Poller::Poller(std::string address, uint16_t port, Handler handler)
    : m_address{ std::move(address) }
    , m_port{ port }
    , m_handler{ std::move(handler) }
{
    m_thread = std::thread{ &Poller::Run, this };
}

Poller::~Poller()
{
    {
        std::lock_guard lock{ m_mutex };
        m_stopping = true;
    }

    m_cv.notify_all();
    m_thread.join();
}

void Poller::Confirm(const std::string &matchId)
{
    {
        std::lock_guard lock{ m_mutex };
        for (const auto &pending : m_confirms)
        {
            if (pending.first == matchId)
            {
                return;
            }
        }

        m_confirms.emplace_back(matchId, 0);
        m_wake = true;
    }

    m_cv.notify_all();
}

void Poller::Run()
{
    Platform::Print("[MM-ACCEPT] backend roster: asking %s for the roster of %s:%u once a second\n",
        BackendClient::Enabled() ? "the backend" : "(backend not configured)", m_address.c_str(), m_port);

    std::string lastKey;
    while (true)
    {
        // 1. tell the backend which rosters are armed (before the next poll: the players are waiting for it)
        std::vector<std::pair<std::string, int>> confirms;
        {
            std::lock_guard lock{ m_mutex };
            if (m_stopping)
            {
                return;
            }

            confirms.swap(m_confirms);
        }

        for (auto &pending : confirms)
        {
            std::string message;
            const bool ok = BackendClient::ConfirmServerRoster(m_address, m_port, pending.first, message);
            Platform::Print("[MM-ACCEPT] backend roster: match %s armed -> %s (%s)\n", pending.first.c_str(),
                ok ? "confirmed" : "NOT delivered", message.c_str());
            if (!ok && ++pending.second < MaxConfirmAttempts)
            {
                std::lock_guard lock{ m_mutex };
                m_confirms.push_back(pending);
            }
        }

        // 2. the roster of the match on this server
        Snapshot snapshot;
        BackendClient::ServerRoster roster;
        const BackendClient::RosterResult result = BackendClient::FetchServerRoster(m_address, m_port, roster, snapshot.message);
        if (result == BackendClient::RosterResult::Ok)
        {
            snapshot.state = Snapshot::State::Match;
            snapshot.matchId = roster.matchId;
            snapshot.mode = roster.mode;
            snapshot.status = roster.status;
            snapshot.acceptRequired = roster.acceptRequired;
            snapshot.requiredPlayers = roster.requiredPlayers;
            for (const BackendClient::RosterPlayer &player : roster.players)
            {
                snapshot.participants.push_back(player.accountId);
                snapshot.fakeCount += player.fake ? 1 : 0;
            }
        }
        else if (result == BackendClient::RosterResult::NoMatch)
        {
            snapshot.state = Snapshot::State::NoMatch;
        }
        else
        {
            snapshot.state = Snapshot::State::Unavailable;
        }

        // only a change is reported (and logged): the roster of a running match is polled every second for minutes
        const std::string key = snapshot.state == Snapshot::State::Match
            ? snapshot.matchId + "|" + snapshot.status + "|" + Join(snapshot.participants)
            : std::to_string(static_cast<int>(snapshot.state)) + snapshot.message;
        if (key != lastKey)
        {
            lastKey = key;
            Platform::Print("[MM-ACCEPT] backend roster: %s\n", Describe(snapshot).c_str());
            m_handler(snapshot);
        }

        std::unique_lock lock{ m_mutex };
        m_cv.wait_for(lock, PollInterval, [this] { return m_stopping || m_wake; });
        m_wake = false;
        if (m_stopping)
        {
            return;
        }
    }
}

} // namespace RosterFeed

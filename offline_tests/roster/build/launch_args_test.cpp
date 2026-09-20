// -backend_ip / -backend_port of srcds: which address the backend roster is asked for. The REAL launch_args.h (what
// GCConfig::BackendServerAddress()/BackendServerPort()/DedicatedServer*() run) and the REAL RosterFeed::Poller + BackendClient,
// against a capturing HTTP server on 127.0.0.1:18093 that stands in for the Java backend.
//
// The gc_server.cpp line that hands config.BackendServerAddress()/Port() to the Poller is not in here (it needs the DLL); it is
// a one-line call in ServerGC::StartAcceptTestRoster.
#include "stdafx.h"
#include "config.h"
#include "launch_args.h"
#include "server_roster.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdarg>
#include <mutex>

// ---- stand-ins -------------------------------------------------------------------------------------------------------
static std::mutex g_printMutex;
static std::vector<std::string> g_printed;

void Platform::Print(const char *format, ...)
{
    char buf[4096];
    va_list ap; va_start(ap, format); vsnprintf(buf, sizeof(buf), format, ap); va_end(ap);
    std::lock_guard l{ g_printMutex };
    g_printed.push_back(buf);
}
std::string Platform::CommandLine() { return {}; }
const GCConfig &GetConfig() { static GCConfig c; return c; }
GCConfig::GCConfig() { m_url = "http://127.0.0.1:18093"; m_key = "test-api"; }

static bool PrintedContains(const std::string &text)
{
    std::lock_guard l{ g_printMutex };
    for (const std::string &line : g_printed) if (line.find(text) != std::string::npos) return true;
    return false;
}

static int g_failed = 0;
static void Expect(bool ok, const std::string &what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    g_failed += ok ? 0 : 1;
}

// ---- the backend stand-in: records every request, answers 404 to GET (no match) and 200 to POST ----------------------------
class CapturingBackend
{
public:
    CapturingBackend()
    {
        WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
        m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL yes = TRUE; setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(18093); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (bind(m_listen, (sockaddr *)&a, sizeof(a)) != 0 || listen(m_listen, 8) != 0)
        {
            printf("cannot listen on 127.0.0.1:18093 (is another test running?)\n");
            exit(2);
        }
        m_thread = std::thread([this] { Loop(); });
    }
    ~CapturingBackend()
    {
        m_stop = true;
        closesocket(m_listen);
        m_thread.join();
    }

    void Clear() { std::lock_guard l{ m_mutex }; m_requests.clear(); }

    // the first request whose request line starts with `prefix` ("GET /api/v1/servers/roster", "POST ..."), waiting for it
    bool WaitFor(const std::string &prefix, std::string &request, int timeoutMs)
    {
        for (int waited = 0; waited <= timeoutMs; waited += 20)
        {
            {
                std::lock_guard l{ m_mutex };
                for (const std::string &r : m_requests)
                {
                    if (r.compare(0, prefix.size(), prefix) == 0) { request = r; return true; }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

private:
    void Loop()
    {
        while (!m_stop)
        {
            SOCKET c = accept(m_listen, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            std::string request;
            char buf[4096];
            size_t headerEnd = std::string::npos;
            size_t total = 0;
            while (true)
            {
                int n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                request.append(buf, (size_t)n);
                if (headerEnd == std::string::npos) headerEnd = request.find("\r\n\r\n");
                if (headerEnd != std::string::npos)
                {
                    size_t cl = request.find("Content-Length: ");
                    total = headerEnd + 4 + (cl != std::string::npos ? (size_t)atoi(request.c_str() + cl + 16) : 0);
                    if (request.size() >= total) break;
                }
            }
            const bool get = request.compare(0, 4, "GET ") == 0;
            const char *body = get ? "{\"message\":\"no forming or ready match on this server\"}" : "{}";
            std::string response = std::string(get ? "HTTP/1.1 404 Not Found" : "HTTP/1.1 200 OK")
                + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(strlen(body)) + "\r\nConnection: close\r\n\r\n" + body;
            send(c, response.data(), (int)response.size(), 0);
            closesocket(c);
            std::lock_guard l{ m_mutex };
            m_requests.push_back(request);
        }
    }

    SOCKET m_listen{};
    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    std::mutex m_mutex;
    std::vector<std::string> m_requests;
};

static std::string RequestLine(const std::string &request) { return request.substr(0, request.find("\r\n")); }
static std::string Body(const std::string &request) { size_t e = request.find("\r\n\r\n"); return e == std::string::npos ? std::string{} : request.substr(e + 4); }

// srcds started with `commandLine`: what its game server uses and what the Poller sends to the backend
static void RunCase(CapturingBackend &backend, const char *title, const std::string &commandLine, const char *gameAddress,
    unsigned gamePort, const char *backendAddress, unsigned backendPort)
{
    printf("== %s\n   %s\n", title, commandLine.c_str());
    const LaunchArgs::ServerEndpoints ep = LaunchArgs::ResolveServerEndpoints(commandLine);

    // the game server keeps what -ip/-port say (this is what the engine binds and what the fake participants send their 0x21 to)
    Expect(ep.gameAddress == gameAddress && ep.gamePort == gamePort,
        std::string("game endpoint stays ") + gameAddress + ":" + std::to_string(gamePort) + " (got " + ep.gameAddress + ":" + std::to_string(ep.gamePort) + ")");
    Expect(ep.backendAddress == backendAddress && ep.backendPort == backendPort,
        std::string("backend identity is ") + backendAddress + ":" + std::to_string(backendPort) + " (got " + ep.backendAddress + ":" + std::to_string(ep.backendPort) + ")");

    // the Poller as gc_server.cpp builds it: config.BackendServerAddress(), config.BackendServerPort()
    backend.Clear();
    {
        std::atomic<int> snapshots{ 0 };
        RosterFeed::Poller poller(ep.backendAddress, ep.backendPort, [&](const RosterFeed::Snapshot &s)
        {
            if (s.state == RosterFeed::Snapshot::State::NoMatch) snapshots++;
        });

        std::string get;
        const bool gotGet = backend.WaitFor("GET /api/v1/servers/roster", get, 5000);
        Expect(gotGet, "the Poller polls GET /api/v1/servers/roster");
        const std::string expected = std::string("GET /api/v1/servers/roster?address=") + backendAddress + "&port=" + std::to_string(backendPort) + " HTTP/1.1";
        Expect(gotGet && RequestLine(get) == expected, "... exactly " + expected + " (got " + (gotGet ? RequestLine(get) : "nothing") + ")");
        if (std::string(gameAddress) != backendAddress)
        {
            Expect(gotGet && RequestLine(get).find(std::string("address=") + gameAddress) == std::string::npos, std::string("... and the game address ") + gameAddress + " is NOT in it");
        }
        if (gamePort != backendPort)
        {
            Expect(gotGet && RequestLine(get).find("port=" + std::to_string(gamePort) + " ") == std::string::npos, "... and the game port " + std::to_string(gamePort) + " is NOT in it");
        }

        // "the roster is armed" goes to the backend under the same identity
        poller.Confirm("m-launch-args");
        std::string post;
        const bool gotPost = backend.WaitFor("POST /api/v1/servers/roster/ready", post, 5000);
        const std::string expectedBody = std::string("{\"address\":\"") + backendAddress + "\",\"port\":" + std::to_string(backendPort) + ",\"match_id\":\"m-launch-args\"}";
        Expect(gotPost && Body(post) == expectedBody, "roster/ready is sent for the same identity: " + expectedBody + " (got " + (gotPost ? Body(post) : "nothing") + ")");

        for (int i = 0; i < 100 && snapshots == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Expect(snapshots > 0, "the 404 of the backend is read as \"no match\" (the poll really went through the Poller)");
    }
    Expect(PrintedContains(std::string("asking the backend for the roster of ") + backendAddress + ":" + std::to_string(backendPort) + " once a second"),
        std::string("the Poller's log line names ") + backendAddress + ":" + std::to_string(backendPort));
}

int main()
{
    CapturingBackend backend;

    // the exact scenarios of the request
    RunCase(backend, "Case 1: -backend_ip/-backend_port given, the game listens elsewhere",
        "srcds.exe -game csgo -console -usercon -insecure -ip 192.168.1.150 -port 27016 -backend_ip 146.158.123.140 -backend_port 27016 -gc_mode competitive",
        "192.168.1.150", 27016, "146.158.123.140", 27016);

    RunCase(backend, "Case 2: an old command line, no -backend_*: the game address is asked for (compatibility)",
        "srcds.exe -game csgo -console -usercon -insecure -ip 192.168.1.150 -port 27016 -gc_mode competitive",
        "192.168.1.150", 27016, "192.168.1.150", 27016);

    RunCase(backend, "Case 3: the backend port differs from the game port",
        "srcds.exe -ip 192.168.1.150 -port 27016 -backend_ip 146.158.123.140 -backend_port 27099 -gc_mode competitive",
        "192.168.1.150", 27016, "146.158.123.140", 27099);

    // resolution details (no network)
    printf("== resolution details\n");
    auto ep = [](const char *line) { return LaunchArgs::ResolveServerEndpoints(line); };
    {
        auto e = ep("srcds.exe -ip 192.168.1.150 -port 27016 -backend_ip 146.158.123.140");
        Expect(e.backendAddress == "146.158.123.140" && e.backendPort == 27016, "only -backend_ip: the port falls back on -port");
    }
    {
        auto e = ep("srcds.exe -ip 192.168.1.150 -port 27016 -backend_port 27099");
        Expect(e.backendAddress == "192.168.1.150" && e.backendPort == 27099, "only -backend_port: the address falls back on -ip");
    }
    {
        auto e = ep("srcds.exe -backend_port 27099 -backend_ip 146.158.123.140 -gc_mode wingman -port 27017 -ip 10.0.0.5");
        Expect(e.gameAddress == "10.0.0.5" && e.gamePort == 27017 && e.backendAddress == "146.158.123.140" && e.backendPort == 27099, "the order of the arguments does not matter");
    }
    {
        auto e = ep("srcds.exe -console");
        Expect(e.gameAddress == "127.0.0.1" && e.gamePort == 27015 && e.backendAddress == "127.0.0.1" && e.backendPort == 27015, "no -ip/-port at all: loopback:27015 as before, the backend identity follows it");
    }
    {
        auto e = ep("srcds.exe -ip 192.168.1.150 -port 27016 -backend_ip");
        Expect(e.backendAddress == "192.168.1.150" && e.backendPort == 27016, "-backend_ip without a value at the end of the line: fallback");
    }
    {
        auto e = ep("srcds.exe -ip 192.168.1.150 -port 27016 -backend_port abc");
        Expect(e.backendPort == 27016 && e.gamePort == 27016, "-backend_port that is not a port: fallback on -port");
        e = ep("srcds.exe -ip 192.168.1.150 -port 27016 -backend_port 0");
        Expect(e.backendPort == 27016, "-backend_port 0: fallback on -port");
        e = ep("srcds.exe -ip 192.168.1.150 -port 27016 -backend_port 70000");
        Expect(e.backendPort == 27016, "-backend_port 70000: fallback on -port");
    }
    {
        auto e = ep("srcds.exe -ip 192.168.1.150 -port 27016 -backend_ip 146.158.123.140 -backend_port 27099");
        Expect(e.gameAddress == "192.168.1.150" && e.gamePort == 27016, "-backend_* never change what -ip/-port resolve to");
        e = ep("srcds.exe -ip 192.168.1.150 -port 27016");
        Expect(e.gameAddress == "192.168.1.150" && e.gamePort == 27016, "... and the same without them");
    }

    printf("\nRESULT: %s (%d failed)\n", g_failed ? "FAILED" : "OK", g_failed);
    return g_failed ? 1 : 0;
}

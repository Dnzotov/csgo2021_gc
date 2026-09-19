#include "stdafx.h"
#include "backend_client.h"
#include "config.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
static constexpr SocketHandle InvalidSocketHandle = INVALID_SOCKET;
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle InvalidSocketHandle = -1;
#endif

namespace BackendClient
{

namespace
{

using Clock = std::chrono::steady_clock;

constexpr int ConnectTimeoutMs = 1500;    // per address
constexpr int RequestTimeoutMs = 3000;    // resolve + connect + send + answer, all together
constexpr int PollIntervalMs = 1000;      // GET /search/<id> while a search runs (also the search's heartbeat)
constexpr int RetryIntervalMs = 2000;     // register attempts while the backend is unreachable
constexpr int MaxSessionMs = 15 * 60 * 1000; // a search the client waits for longer than this is given up
constexpr int LogEveryNthFailure = 10;    // a backend that is down must not fill the log
constexpr size_t QueueLimit = 16;         // cancel requests waiting for the worker, oldest dropped beyond this
constexpr int JobMaxAgeMs = 30000;        // a cancel that waited this long in the queue is stale, drop it
constexpr size_t MaxResponseBytes = 64 * 1024;

constexpr const char *SearchPath = "/api/v1/matchmaking/search";
constexpr const char *CancelPath = "/api/v1/matchmaking/cancel";

void Log(const char *format, ...)
{
    char message[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);
    Platform::Print("[BACKEND] %s\n", message);
}

// ---------------------------------------------------------------------------------------------------- JSON

void AppendJsonString(std::string &out, std::string_view text)
{
    out += '"';
    for (char c : text)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char escaped[8];
                snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                out += escaped;
            }
            else
            {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

// A small JSON reader, enough for the backend's answers (objects, arrays, strings, numbers, bool, null).
struct Json
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type{ Type::Null };
    bool boolean{};
    double number{};
    std::string string;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    const Json *Get(std::string_view key) const
    {
        if (type != Type::Object)
        {
            return nullptr;
        }

        for (const auto &member : object)
        {
            if (member.first == key)
            {
                return &member.second;
            }
        }

        return nullptr;
    }

    std::string String(std::string_view key, std::string fallback = {}) const
    {
        const Json *value = Get(key);
        return value && value->type == Type::String ? value->string : fallback;
    }

    double Number(std::string_view key, double fallback = 0) const
    {
        const Json *value = Get(key);
        return value && value->type == Type::Number ? value->number : fallback;
    }

    bool Bool(std::string_view key, bool fallback = false) const
    {
        const Json *value = Get(key);
        return value && value->type == Type::Bool ? value->boolean : fallback;
    }
};

class JsonReader
{
public:
    explicit JsonReader(std::string_view text) : m_text{ text } {}

    bool Parse(Json &out)
    {
        SkipSpace();
        if (!ParseValue(out, 0))
        {
            return false;
        }

        SkipSpace();
        return m_position == m_text.size();
    }

private:
    static constexpr int MaxDepth = 16;

    void SkipSpace()
    {
        while (m_position < m_text.size()
            && (m_text[m_position] == ' ' || m_text[m_position] == '\n' || m_text[m_position] == '\r' || m_text[m_position] == '\t'))
        {
            m_position++;
        }
    }

    bool Consume(char c)
    {
        if (m_position < m_text.size() && m_text[m_position] == c)
        {
            m_position++;
            return true;
        }

        return false;
    }

    bool ConsumeWord(std::string_view word)
    {
        if (m_text.substr(m_position, word.size()) == word)
        {
            m_position += word.size();
            return true;
        }

        return false;
    }

    static void AppendUtf8(std::string &out, unsigned codePoint)
    {
        if (codePoint < 0x80)
        {
            out += static_cast<char>(codePoint);
        }
        else if (codePoint < 0x800)
        {
            out += static_cast<char>(0xC0 | (codePoint >> 6));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xE0 | (codePoint >> 12));
            out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
        }
    }

    bool ParseString(std::string &out)
    {
        if (!Consume('"'))
        {
            return false;
        }

        while (m_position < m_text.size())
        {
            char c = m_text[m_position++];
            if (c == '"')
            {
                return true;
            }

            if (c != '\\')
            {
                out += c;
                continue;
            }

            if (m_position >= m_text.size())
            {
                return false;
            }

            char escape = m_text[m_position++];
            switch (escape)
            {
            case '"':
            case '\\':
            case '/':
                out += escape;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u':
            {
                if (m_position + 4 > m_text.size())
                {
                    return false;
                }

                unsigned codePoint = 0;
                for (int i = 0; i < 4; i++)
                {
                    char h = m_text[m_position++];
                    codePoint <<= 4;
                    if (h >= '0' && h <= '9')
                    {
                        codePoint |= static_cast<unsigned>(h - '0');
                    }
                    else if (h >= 'a' && h <= 'f')
                    {
                        codePoint |= static_cast<unsigned>(h - 'a' + 10);
                    }
                    else if (h >= 'A' && h <= 'F')
                    {
                        codePoint |= static_cast<unsigned>(h - 'A' + 10);
                    }
                    else
                    {
                        return false;
                    }
                }

                AppendUtf8(out, codePoint); // surrogate pairs are not needed for our data, they stay as 3-byte units
                break;
            }
            default:
                return false;
            }
        }

        return false;
    }

    bool ParseValue(Json &out, int depth)
    {
        if (depth > MaxDepth || m_position >= m_text.size())
        {
            return false;
        }

        char c = m_text[m_position];
        if (c == '{')
        {
            m_position++;
            out.type = Json::Type::Object;
            SkipSpace();
            if (Consume('}'))
            {
                return true;
            }

            while (true)
            {
                SkipSpace();
                std::string key;
                if (!ParseString(key))
                {
                    return false;
                }

                SkipSpace();
                if (!Consume(':'))
                {
                    return false;
                }

                SkipSpace();
                Json value;
                if (!ParseValue(value, depth + 1))
                {
                    return false;
                }

                out.object.emplace_back(std::move(key), std::move(value));
                SkipSpace();
                if (Consume(','))
                {
                    continue;
                }

                return Consume('}');
            }
        }

        if (c == '[')
        {
            m_position++;
            out.type = Json::Type::Array;
            SkipSpace();
            if (Consume(']'))
            {
                return true;
            }

            while (true)
            {
                SkipSpace();
                Json value;
                if (!ParseValue(value, depth + 1))
                {
                    return false;
                }

                out.array.push_back(std::move(value));
                SkipSpace();
                if (Consume(','))
                {
                    continue;
                }

                return Consume(']');
            }
        }

        if (c == '"')
        {
            out.type = Json::Type::String;
            return ParseString(out.string);
        }

        if (ConsumeWord("true"))
        {
            out.type = Json::Type::Bool;
            out.boolean = true;
            return true;
        }

        if (ConsumeWord("false"))
        {
            out.type = Json::Type::Bool;
            return true;
        }

        if (ConsumeWord("null"))
        {
            return true;
        }

        // number
        const size_t start = m_position;
        while (m_position < m_text.size()
            && (m_text[m_position] == '-' || m_text[m_position] == '+' || m_text[m_position] == '.'
                || m_text[m_position] == 'e' || m_text[m_position] == 'E'
                || (m_text[m_position] >= '0' && m_text[m_position] <= '9')))
        {
            m_position++;
        }

        if (m_position == start)
        {
            return false;
        }

        out.type = Json::Type::Number;
        out.number = strtod(std::string(m_text.substr(start, m_position - start)).c_str(), nullptr);
        return true;
    }

    std::string_view m_text;
    size_t m_position{};
};

// {"search": {...}} -> SearchResult; false when the answer is not a search
bool ParseSearchResponse(const std::string &body, SearchResult &result)
{
    Json root;
    if (!JsonReader{ body }.Parse(root))
    {
        return false;
    }

    const Json *search = root.Get("search");
    if (!search || search->type != Json::Type::Object)
    {
        return false;
    }

    result = {};
    result.requestId = search->String("request_id");
    result.status = search->String("status");
    if (result.status.empty())
    {
        return false;
    }

    if (const Json *match = search->Get("match"))
    {
        result.matchPlayers = static_cast<uint32_t>(match->Number("players"));
        result.matchRequired = static_cast<uint32_t>(match->Number("required_players"));
    }

    if (const Json *assignment = search->Get("assignment"))
    {
        Assignment &a = result.assignment;
        a.matchId = assignment->String("match_id");
        a.serverAddress = assignment->String("server_address");
        a.serverPort = static_cast<uint16_t>(assignment->Number("server_port"));
        a.map = assignment->String("map");
        a.acceptRequired = assignment->Bool("accept_required");
        a.requiredPlayers = static_cast<uint32_t>(assignment->Number("required_players"));
        result.hasAssignment = !a.serverAddress.empty() && a.serverPort != 0;
    }

    return true;
}

// GET /servers/roster -> ServerRoster; false when the answer is not a roster
bool ParseRosterResponse(const std::string &body, ServerRoster &roster)
{
    Json root;
    if (!JsonReader{ body }.Parse(root) || root.type != Json::Type::Object)
    {
        return false;
    }

    roster = {};
    roster.matchId = root.String("match_id");
    roster.mode = root.String("mode");
    roster.status = root.String("status");
    roster.map = root.String("map");
    roster.acceptRequired = root.Bool("accept_required");
    roster.requiredPlayers = static_cast<uint32_t>(root.Number("required_players"));

    const Json *players = root.Get("players");
    if (roster.matchId.empty() || !players || players->type != Json::Type::Array)
    {
        return false;
    }

    for (const Json &player : players->array)
    {
        const double id = player.Number("account_id", -1);
        if (id < 1 || id > 4294967295.0)
        {
            return false;
        }

        roster.players.push_back({ static_cast<uint32_t>(id), player.Bool("fake") });
    }

    return true;
}

// ---------------------------------------------------------------------------------------------------- URL

struct Endpoint
{
    std::string host;       // for name resolution
    std::string port;       // numeric string
    std::string authority;  // as written in the url, for the Host header
    std::string basePath;   // "" or "/prefix", no trailing slash
};

bool ParseUrl(std::string_view url, Endpoint &endpoint, std::string &error)
{
    auto startsWith = [&](std::string_view prefix)
    {
        if (url.size() < prefix.size())
        {
            return false;
        }

        for (size_t i = 0; i < prefix.size(); i++)
        {
            char c = url[i];
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }

            if (c != prefix[i])
            {
                return false;
            }
        }

        return true;
    };

    if (startsWith("https://"))
    {
        error = "https:// is not supported (plain http only), use http://host:port";
        return false;
    }

    if (!startsWith("http://"))
    {
        error = "backend_url must start with http://";
        return false;
    }

    std::string_view rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string_view authority = rest.substr(0, slash);
    std::string_view path = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);

    while (!path.empty() && path.back() == '/')
    {
        path.remove_suffix(1);
    }

    std::string_view host = authority;
    std::string_view port = "80";

    if (!authority.empty() && authority.front() == '[')
    {
        // [ipv6]:port
        size_t close = authority.find(']');
        if (close == std::string_view::npos)
        {
            error = "unterminated [ in backend_url";
            return false;
        }

        host = authority.substr(1, close - 1);
        std::string_view after = authority.substr(close + 1);
        if (!after.empty())
        {
            if (after.front() != ':')
            {
                error = "bad port in backend_url";
                return false;
            }
            port = after.substr(1);
        }
    }
    else
    {
        size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos)
        {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
    }

    if (host.empty())
    {
        error = "no host in backend_url";
        return false;
    }

    unsigned portNumber = 0;
    if (port.empty() || port.size() > 5)
    {
        error = "bad port in backend_url";
        return false;
    }

    for (char c : port)
    {
        if (c < '0' || c > '9')
        {
            error = "bad port in backend_url";
            return false;
        }
        portNumber = portNumber * 10 + static_cast<unsigned>(c - '0');
    }

    if (portNumber == 0 || portNumber > 65535)
    {
        error = "bad port in backend_url";
        return false;
    }

    for (char c : url)
    {
        if (static_cast<unsigned char>(c) <= ' ')
        {
            error = "backend_url must not contain spaces or control characters";
            return false;
        }
    }

    endpoint.host = std::string(host);
    endpoint.port = std::string(port);
    endpoint.authority = std::string(authority);
    endpoint.basePath = std::string(path);
    return true;
}

// ---------------------------------------------------------------------------------------------------- sockets

int LastSocketError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool IsWouldBlock(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINTR;
#else
    return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS || error == EINTR;
#endif
}

bool IsConnectionRefused(int error)
{
#ifdef _WIN32
    return error == WSAECONNREFUSED;
#else
    return error == ECONNREFUSED;
#endif
}

void CloseSocket(SocketHandle sock)
{
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

bool SetNonBlocking(SocketHandle sock)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    return flags != -1 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// waits until the socket is readable / writable (or, for a connect, failed); false on timeout or select error
bool WaitSocket(SocketHandle sock, bool forWrite, int timeoutMs)
{
    if (timeoutMs < 0)
    {
        timeoutMs = 0;
    }

    fd_set set;
    fd_set errorSet;
    FD_ZERO(&set);
    FD_ZERO(&errorSet);
    FD_SET(sock, &set);
    FD_SET(sock, &errorSet);

    timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    // a failed non-blocking connect is reported through the except set on Windows
    int result = select(static_cast<int>(sock) + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr,
        forWrite ? &errorSet : nullptr, &timeout);
    return result > 0;
}

int RemainingMs(Clock::time_point deadline)
{
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
}

std::string DescribeSocketError(int error)
{
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "socket error %d", error);
    return buffer;
}

// non-blocking connect to the first address that answers within the timeout
SocketHandle ConnectTo(const Endpoint &endpoint, Clock::time_point deadline, std::string &error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo *results = nullptr;
    if (getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &results) != 0 || !results)
    {
        error = "cannot resolve host '" + endpoint.host + "'";
        return InvalidSocketHandle;
    }

    SocketHandle connected = InvalidSocketHandle;
    for (const addrinfo *ai = results; ai && connected == InvalidSocketHandle; ai = ai->ai_next)
    {
        int remaining = RemainingMs(deadline);
        if (remaining <= 0)
        {
            error = "timed out";
            break;
        }

        SocketHandle sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == InvalidSocketHandle)
        {
            error = DescribeSocketError(LastSocketError());
            continue;
        }

        if (!SetNonBlocking(sock))
        {
            error = DescribeSocketError(LastSocketError());
            CloseSocket(sock);
            continue;
        }

        bool ok = false;
        if (connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
        {
            ok = true;
        }
        else if (IsWouldBlock(LastSocketError()))
        {
            int wait = remaining < ConnectTimeoutMs ? remaining : ConnectTimeoutMs;
            if (!WaitSocket(sock, true, wait))
            {
                error = "connect timed out (backend not running or not reachable?)";
            }
            else
            {
                int soError = 0;
#ifdef _WIN32
                int length = sizeof(soError);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soError), &length);
#else
                socklen_t length = sizeof(soError);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &soError, &length);
#endif
                if (soError == 0)
                {
                    ok = true;
                }
                else if (IsConnectionRefused(soError))
                {
                    error = "connection refused (backend not running?)";
                }
                else
                {
                    error = "connect failed, " + DescribeSocketError(soError);
                }
            }
        }
        else
        {
            error = "connect failed, " + DescribeSocketError(LastSocketError());
        }

        if (ok)
        {
            connected = sock;
        }
        else
        {
            CloseSocket(sock);
        }
    }

    freeaddrinfo(results);
    return connected;
}

bool SendAll(SocketHandle sock, const std::string &data, Clock::time_point deadline, std::string &error)
{
    size_t sent = 0;
    while (sent < data.size())
    {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        int n = static_cast<int>(send(sock, data.data() + sent, static_cast<int>(data.size() - sent), flags));
        if (n > 0)
        {
            sent += static_cast<size_t>(n);
            continue;
        }

        if (n < 0 && IsWouldBlock(LastSocketError()))
        {
            if (!WaitSocket(sock, true, RemainingMs(deadline)))
            {
                error = "send timed out";
                return false;
            }
            continue;
        }

        error = "send failed, " + DescribeSocketError(LastSocketError());
        return false;
    }

    return true;
}

// case-insensitive "Content-Length: N" lookup in the response headers, -1 if absent
long long ContentLength(const std::string &headers)
{
    static constexpr std::string_view Name = "content-length:";
    for (size_t i = 0; i + Name.size() < headers.size(); i++)
    {
        size_t k = 0;
        for (; k < Name.size(); k++)
        {
            char c = headers[i + k];
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != Name[k])
            {
                break;
            }
        }

        if (k == Name.size())
        {
            size_t p = i + Name.size();
            while (p < headers.size() && headers[p] == ' ')
            {
                p++;
            }

            long long value = 0;
            bool any = false;
            while (p < headers.size() && headers[p] >= '0' && headers[p] <= '9')
            {
                value = value * 10 + (headers[p] - '0');
                any = true;
                p++;
            }
            return any ? value : -1;
        }
    }

    return -1;
}

// does the header block contain "name: ...value..." (case-insensitive)
bool HeaderContains(const std::string &headers, std::string_view name, std::string_view value)
{
    auto lower = [](std::string text)
    {
        for (char &c : text)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return text;
    };

    const std::string haystack = lower(headers);
    size_t position = haystack.find(std::string(name) + ":");
    if (position == std::string::npos)
    {
        return false;
    }

    size_t end = haystack.find("\r\n", position);
    return haystack.substr(position, end == std::string::npos ? std::string::npos : end - position).find(value) != std::string::npos;
}

// Transfer-Encoding: chunked (what Tomcat uses for the JSON answers); a truncated body just yields what arrived
std::string DecodeChunked(const std::string &raw)
{
    std::string decoded;
    size_t position = 0;
    while (position < raw.size())
    {
        size_t eol = raw.find("\r\n", position);
        if (eol == std::string::npos)
        {
            break;
        }

        const unsigned long size = strtoul(raw.substr(position, eol - position).c_str(), nullptr, 16);
        position = eol + 2;
        if (size == 0)
        {
            break;
        }

        if (position + size > raw.size())
        {
            decoded.append(raw, position, std::string::npos);
            break;
        }

        decoded.append(raw, position, size);
        position += size + 2;
    }

    return decoded;
}

struct HttpResult
{
    int status{}; // 0 = no HTTP answer, see error
    std::string body;
    std::string error;
};

// method "GET" (body == nullptr) or "POST"
HttpResult HttpRequest(const Endpoint &endpoint, const std::string &apiKey, const char *method, const std::string &path,
    const std::string *body)
{
    HttpResult result;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(RequestTimeoutMs);

    SocketHandle sock = ConnectTo(endpoint, deadline, result.error);
    if (sock == InvalidSocketHandle)
    {
        return result;
    }

    std::string request;
    request.reserve((body ? body->size() : 0) + 256);
    request += std::string(method) + " " + endpoint.basePath + path + " HTTP/1.1\r\n";
    request += "Host: " + endpoint.authority + "\r\n";
    request += "User-Agent: csgo_gc\r\n";
    request += "Accept: application/json\r\n";
    if (body)
    {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body->size()) + "\r\n";
    }
    if (!apiKey.empty())
    {
        request += "X-Api-Key: " + apiKey + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    if (body)
    {
        request += *body;
    }

    if (SendAll(sock, request, deadline, result.error))
    {
        std::string response;
        char buffer[4096];
        size_t headerEnd = std::string::npos;

        while (response.size() < MaxResponseBytes)
        {
            if (!WaitSocket(sock, false, RemainingMs(deadline)))
            {
                break; // timeout, judged below by what we have
            }

            int n = static_cast<int>(recv(sock, buffer, sizeof(buffer), 0));
            if (n > 0)
            {
                response.append(buffer, static_cast<size_t>(n));
            }
            else if (n == 0)
            {
                break; // closed: Connection: close, the answer is complete
            }
            else if (!IsWouldBlock(LastSocketError()))
            {
                break;
            }

            // stop as soon as headers and the announced body are here, no need to wait for the close
            if (headerEnd == std::string::npos)
            {
                headerEnd = response.find("\r\n\r\n");
            }

            if (headerEnd != std::string::npos)
            {
                const std::string headers = response.substr(0, headerEnd);
                long long length = ContentLength(headers);
                if (length >= 0 && response.size() >= headerEnd + 4 + static_cast<size_t>(length))
                {
                    break;
                }

                // chunked answer: the last chunk is "0\r\n\r\n"
                if (length < 0 && response.size() >= headerEnd + 9 && response.compare(response.size() - 5, 5, "0\r\n\r\n") == 0
                    && HeaderContains(headers, "transfer-encoding", "chunked"))
                {
                    break;
                }
            }
        }

        // "HTTP/1.1 200 OK"
        if (response.size() >= 12 && response.compare(0, 5, "HTTP/") == 0 && response[8] == ' ')
        {
            result.status = (response[9] - '0') * 100 + (response[10] - '0') * 10 + (response[11] - '0');
            size_t bodyStart = response.find("\r\n\r\n");
            if (bodyStart != std::string::npos)
            {
                result.body = response.substr(bodyStart + 4);
                if (HeaderContains(response.substr(0, bodyStart), "transfer-encoding", "chunked"))
                {
                    result.body = DecodeChunked(result.body);
                }
            }
        }
        else if (result.error.empty())
        {
            result.error = response.empty() ? "no answer within the timeout" : "answer is not HTTP";
        }
    }

    CloseSocket(sock);
    return result;
}

std::string OneLine(const std::string &text, size_t limit)
{
    std::string line = text.substr(0, limit);
    for (char &c : line)
    {
        if (static_cast<unsigned char>(c) < ' ')
        {
            c = ' '; // keep the log line on one line
        }
    }
    return line;
}

// ---------------------------------------------------------------------------------------------------- worker

struct Job
{
    const char *kind;
    const char *path;
    std::string body;
    Clock::time_point queuedAt;
};

// an IP address or host name: what can go into a query string as it is
bool SafeHost(const std::string &host)
{
    if (host.empty() || host.size() > 253)
    {
        return false;
    }

    for (char c : host)
    {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-'
            || c == '_' || c == ':';
        if (!ok)
        {
            return false;
        }
    }

    return true;
}

class Client
{
public:
    // never destroyed: the worker thread is detached and process exit must not wait for a socket
    static Client &Get()
    {
        static Client *instance = new Client;
        return *instance;
    }

    bool Enabled() const { return m_enabled; }

    // srcds: the roster of the match on this game server. Blocking, see backend_client.h
    RosterResult FetchServerRoster(const std::string &address, uint16_t port, ServerRoster &roster, std::string &message)
    {
        if (!m_enabled)
        {
            message = "matchmaking.backend_url is not configured";
            return RosterResult::Unreachable;
        }

        if (!SafeHost(address))
        {
            message = "bad server address '" + address + "'";
            return RosterResult::Rejected;
        }

        const HttpResult result = HttpRequest(m_endpoint, m_apiKey, "GET",
            "/api/v1/servers/roster?address=" + address + "&port=" + std::to_string(port), nullptr);
        if (result.status == 200)
        {
            if (!ParseRosterResponse(result.body, roster))
            {
                message = "not a roster: " + OneLine(result.body, 160);
                return RosterResult::Rejected;
            }

            return RosterResult::Ok;
        }

        if (result.status == 404)
        {
            Json error;
            message = JsonReader{ result.body }.Parse(error) ? error.String("message") : OneLine(result.body, 160);
            return RosterResult::NoMatch;
        }

        if (result.status != 0)
        {
            message = "HTTP " + std::to_string(result.status) + ": " + OneLine(result.body, 160);
            return RosterResult::Rejected;
        }

        message = result.error;
        return RosterResult::Unreachable;
    }

    bool ConfirmServerRoster(const std::string &address, uint16_t port, const std::string &matchId, std::string &message)
    {
        if (!m_enabled || !SafeHost(address) || matchId.empty())
        {
            message = "not configured / bad arguments";
            return false;
        }

        std::string body = "{\"address\":";
        AppendJsonString(body, address);
        body += ",\"port\":" + std::to_string(port) + ",\"match_id\":";
        AppendJsonString(body, matchId);
        body += '}';

        const HttpResult result = HttpRequest(m_endpoint, m_apiKey, "POST", "/api/v1/servers/roster/ready", &body);
        if (result.status >= 200 && result.status < 300)
        {
            message = OneLine(result.body, 160);
            return true;
        }

        message = result.status != 0 ? "HTTP " + std::to_string(result.status) + ": " + OneLine(result.body, 160) : result.error;
        return false;
    }

    void SetResultHandler(ResultHandler handler, void *context)
    {
        // held while the handler runs, so clearing it waits for a call in flight
        std::lock_guard lock{ m_handlerMutex };
        m_handler = handler;
        m_handlerContext = context;
    }

    std::string ActiveRequestId()
    {
        std::lock_guard lock{ m_mutex };
        return m_activeRequestId;
    }

    void SearchStarted(const SearchInfo &info, uint64_t steamId)
    {
        if (!m_enabled)
        {
            return;
        }

        if (info.accountId == 0)
        {
            Log("search not reported: the account id is unknown");
            return;
        }

        const auto unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string body;
        std::string requestId;
        {
            std::lock_guard lock{ m_mutex };
            char buffer[96];
            snprintf(buffer, sizeof(buffer), "%llu-%lld-%u", static_cast<unsigned long long>(steamId),
                static_cast<long long>(unixMs), ++m_counter);
            requestId = buffer;

            body += "{\"account_id\":" + std::to_string(info.accountId);
            body += ",\"game_type\":" + std::to_string(info.gameType);
            if (!info.mode.empty())
            {
                body += ",\"mode\":";
                AppendJsonString(body, info.mode);
            }
            if (!info.gameMode.empty())
            {
                body += ",\"game_mode\":";
                AppendJsonString(body, info.gameMode);
            }
            if (!info.maps.empty())
            {
                body += ",\"maps\":[";
                for (size_t i = 0; i < info.maps.size(); i++)
                {
                    if (i)
                    {
                        body += ',';
                    }
                    AppendJsonString(body, info.maps[i]);
                }
                body += ']';
            }
            if (!info.variants.empty())
            {
                body += ",\"variants\":[";
                for (size_t v = 0; v < info.variants.size(); v++)
                {
                    const SearchInfo::Variant &variant = info.variants[v];
                    body += v ? ",{\"name\":" : "{\"name\":";
                    AppendJsonString(body, variant.name);
                    body += ",\"game_mode\":";
                    AppendJsonString(body, variant.gameMode);
                    body += ",\"maps\":[";
                    for (size_t i = 0; i < variant.maps.size(); i++)
                    {
                        if (i)
                        {
                            body += ',';
                        }
                        AppendJsonString(body, variant.maps[i]);
                    }
                    body += "]}";
                }
                body += ']';
            }
            body += ",\"request_id\":";
            AppendJsonString(body, requestId);
            body += '}';

            // this search replaces whatever was tracked (the backend does the same for the account)
            m_serial++;
            m_activeAccountId = info.accountId;
            m_activeRequestId = requestId;
            m_session = Session{};
            m_session.active = true;
            m_session.serial = m_serial;
            m_session.requestId = requestId;
            m_session.registerBody = std::move(body);
            m_session.phase = Phase::Register;
            m_session.nextAt = Clock::now();
            m_session.startedAt = Clock::now();
        }

        if (!StartThread())
        {
            return;
        }

        m_cv.notify_one();
    }

    void SearchCancelled()
    {
        if (!m_enabled)
        {
            return;
        }

        uint32_t accountId;
        std::string requestId;
        {
            std::lock_guard lock{ m_mutex };
            if (m_activeRequestId.empty())
            {
                return;
            }

            accountId = m_activeAccountId;
            requestId = std::move(m_activeRequestId);
            m_activeRequestId.clear();
            m_activeAccountId = 0;

            // stop polling; a register that is in flight finishes, the cancel below queues behind it
            m_serial++;
            m_session = Session{};
        }

        std::string body = "{\"account_id\":" + std::to_string(accountId) + ",\"request_id\":";
        AppendJsonString(body, requestId);
        body += '}';

        Enqueue({ "cancel", CancelPath, std::move(body), Clock::now() });
    }

private:
    enum class Phase { Register, Poll, Done };

    struct Session
    {
        bool active{};
        uint64_t serial{};
        std::string requestId;
        std::string registerBody;
        Phase phase{ Phase::Done };
        Clock::time_point nextAt;
        Clock::time_point startedAt;
        uint32_t failures{};
        std::string lastStatus;
        uint32_t lastPlayers{};
    };

    Client()
    {
        const std::string_view url = GetConfig().BackendUrl();
        if (url.empty())
        {
            Log("disabled (matchmaking.backend_url is empty): no server can be assigned to a search");
            return;
        }

        std::string error;
        if (!ParseUrl(url, m_endpoint, error))
        {
            Log("disabled: %s (matchmaking.backend_url = '%.*s')", error.c_str(), static_cast<int>(url.size()), url.data());
            return;
        }

        m_apiKey = std::string(GetConfig().BackendApiKey());
        for (char c : m_apiKey)
        {
            if (static_cast<unsigned char>(c) < ' ')
            {
                Log("disabled: matchmaking.backend_api_key contains control characters");
                return;
            }
        }

        m_enabled = true;
        Log("enabled: the backend at http://%s%s picks the server of every search (api key %s)",
            m_endpoint.authority.c_str(), m_endpoint.basePath.c_str(), m_apiKey.empty() ? "NOT set" : "set");
    }

    bool StartThread()
    {
        std::lock_guard lock{ m_mutex };
        if (m_threadStarted)
        {
            return true;
        }

        try
        {
            std::thread{ &Client::Run, this }.detach();
            m_threadStarted = true;
            return true;
        }
        catch (...)
        {
            Log("could not start the worker thread, backend reporting is off");
            m_enabled = false;
            m_queue.clear();
            m_session = Session{};
            return false;
        }
    }

    void Enqueue(Job job)
    {
        {
            std::lock_guard lock{ m_mutex };
            if (m_queue.size() >= QueueLimit)
            {
                Log("queue is full (backend too slow?), dropping the oldest %s request", m_queue.front().kind);
                m_queue.pop_front();
            }

            m_queue.push_back(std::move(job));
        }

        if (StartThread())
        {
            m_cv.notify_one();
        }
    }

    void Run()
    {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            Log("WSAStartup failed, backend reporting is off");
            std::lock_guard lock{ m_mutex };
            m_enabled = false;
            m_queue.clear();
            m_session = Session{};
            return;
        }
#endif

        while (true)
        {
            Job job{};
            bool haveJob = false;
            Session step;
            bool haveStep = false;
            {
                std::unique_lock lock{ m_mutex };
                while (true)
                {
                    // cancels first: they must not wait behind a poll
                    if (!m_queue.empty())
                    {
                        job = std::move(m_queue.front());
                        m_queue.pop_front();
                        haveJob = true;
                        break;
                    }

                    if (m_session.active && m_session.phase != Phase::Done)
                    {
                        if (Clock::now() >= m_session.nextAt)
                        {
                            step = m_session;
                            haveStep = true;
                            break;
                        }

                        m_cv.wait_until(lock, m_session.nextAt);
                    }
                    else
                    {
                        m_cv.wait(lock);
                    }
                }
            }

            try
            {
                if (haveJob)
                {
                    ExecuteCancel(job);
                }
                else if (haveStep)
                {
                    RunSessionStep(step);
                }
            }
            catch (...)
            {
                Log("request failed with an exception, ignored");
            }
        }
    }

    void ExecuteCancel(const Job &job)
    {
        const auto started = Clock::now();
        const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(started - job.queuedAt).count();
        if (waitedMs > JobMaxAgeMs)
        {
            Log("%s request dropped, it waited %lld ms in the queue (backend unreachable?)", job.kind,
                static_cast<long long>(waitedMs));
            return;
        }

        Log("POST %s%s %s", m_endpoint.basePath.c_str(), job.path, job.body.c_str());

        HttpResult result = HttpRequest(m_endpoint, m_apiKey, "POST", job.path, &job.body);
        const auto tookMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();

        if (result.status >= 200 && result.status < 300)
        {
            Log("%s -> HTTP %d in %lld ms: %s", job.kind, result.status, static_cast<long long>(tookMs),
                OneLine(result.body, 200).c_str());
        }
        else if (result.status != 0)
        {
            Log("%s FAILED: HTTP %d in %lld ms: %s -- matchmaking is not affected", job.kind, result.status,
                static_cast<long long>(tookMs), OneLine(result.body, 200).c_str());
        }
        else
        {
            Log("%s FAILED: %s (%s:%s, %lld ms) -- matchmaking is not affected", job.kind, result.error.c_str(),
                m_endpoint.host.c_str(), m_endpoint.port.c_str(), static_cast<long long>(tookMs));
        }
    }

    // one register or poll of the tracked search (called without the lock, `step` is a copy of the session)
    void RunSessionStep(const Session &step)
    {
        const auto started = Clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(started - step.startedAt).count() > MaxSessionMs)
        {
            Log("giving up on search %s after %d s without a result", step.requestId.c_str(), MaxSessionMs / 1000);
            FinishSession(step.serial);
            return;
        }

        const bool registering = step.phase == Phase::Register;
        HttpResult result;
        if (registering)
        {
            if (step.failures == 0)
            {
                Log("POST %s%s %s", m_endpoint.basePath.c_str(), SearchPath, step.registerBody.c_str());
            }
            result = HttpRequest(m_endpoint, m_apiKey, "POST", SearchPath, &step.registerBody);
        }
        else
        {
            result = HttpRequest(m_endpoint, m_apiKey, "GET", std::string(SearchPath) + "/" + step.requestId, nullptr);
        }

        const auto tookMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();

        SearchResult parsed;
        const bool ok = result.status >= 200 && result.status < 300 && ParseSearchResponse(result.body, parsed);

        {
            std::lock_guard lock{ m_mutex };
            if (!m_session.active || m_session.serial != step.serial)
            {
                // cancelled or replaced while the request was running
                return;
            }

            Session &session = m_session;
            if (ok)
            {
                if (session.failures)
                {
                    Log("backend reachable again");
                }

                session.failures = 0;
                session.phase = Phase::Poll;
                session.nextAt = Clock::now() + std::chrono::milliseconds(PollIntervalMs);
            }
            else if (!registering && result.status == 404)
            {
                // the backend does not know the search (restarted, expired): register it again
                Log("the backend lost search %s, registering it again", step.requestId.c_str());
                session.phase = Phase::Register;
                session.failures = 0;
                session.nextAt = Clock::now();
                return;
            }
            else if (result.status >= 400 && result.status < 500 && result.status != 401 && result.status != 404 && result.status != 408)
            {
                // the backend refuses this search (e.g. unsupported mode), retrying cannot help
                Log("search %s rejected: HTTP %d %s", step.requestId.c_str(), result.status, OneLine(result.body, 200).c_str());
                session.phase = Phase::Done;
                return;
            }
            else
            {
                session.failures++;
                session.nextAt = Clock::now() + std::chrono::milliseconds(registering ? RetryIntervalMs : PollIntervalMs);
                if (session.failures == 1 || session.failures % LogEveryNthFailure == 0)
                {
                    if (result.status != 0)
                    {
                        Log("%s search %s: HTTP %d in %lld ms: %s (attempt %u, retrying)", registering ? "register" : "poll",
                            step.requestId.c_str(), result.status, static_cast<long long>(tookMs),
                            OneLine(result.body, 160).c_str(), session.failures);
                    }
                    else
                    {
                        Log("%s search %s FAILED: %s (%s:%s, attempt %u, retrying) -- no server can be assigned while the backend is unreachable",
                            registering ? "register" : "poll", step.requestId.c_str(), result.error.c_str(),
                            m_endpoint.host.c_str(), m_endpoint.port.c_str(), session.failures);
                    }
                }
                return;
            }

            // a state to report: first answer, a status change, more players, or the assignment
            const bool changed = parsed.status != session.lastStatus || parsed.matchPlayers != session.lastPlayers;
            session.lastStatus = parsed.status;
            session.lastPlayers = parsed.matchPlayers;
            if (parsed.IsAssigned() || parsed.IsEnded())
            {
                session.phase = Phase::Done;
            }

            if (!changed && session.phase != Phase::Done)
            {
                return;
            }
        }

        if (parsed.requestId.empty())
        {
            parsed.requestId = step.requestId;
        }

        Log("search %s: %s%s (%lld ms)", step.requestId.c_str(), parsed.status.c_str(),
            parsed.IsAssigned() ? (" -> " + parsed.assignment.serverAddress + ":" + std::to_string(parsed.assignment.serverPort)
                + " map=" + parsed.assignment.map + " match=" + parsed.assignment.matchId).c_str() : "",
            static_cast<long long>(tookMs));

        Deliver(parsed);
    }

    void FinishSession(uint64_t serial)
    {
        std::lock_guard lock{ m_mutex };
        if (m_session.active && m_session.serial == serial)
        {
            m_session.phase = Phase::Done;
        }
    }

    void Deliver(const SearchResult &result)
    {
        std::lock_guard lock{ m_handlerMutex };
        if (m_handler)
        {
            m_handler(m_handlerContext, result);
        }
    }

    std::atomic<bool> m_enabled{ false }; // false also after a fatal worker error
    Endpoint m_endpoint;
    std::string m_apiKey;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Job> m_queue;
    bool m_threadStarted{};

    Session m_session;
    uint64_t m_serial{};

    // the search being tracked (its request id lets the backend tell searches apart); cleared by SearchCancelled
    uint32_t m_activeAccountId{};
    std::string m_activeRequestId;
    uint32_t m_counter{};

    std::mutex m_handlerMutex;
    ResultHandler m_handler{};
    void *m_handlerContext{};
};

} // namespace

std::string SerializeResult(const SearchResult &result)
{
    std::string text;
    text += "request_id=" + result.requestId + "\n";
    text += "status=" + result.status + "\n";
    text += "players=" + std::to_string(result.matchPlayers) + "\n";
    text += "required=" + std::to_string(result.matchRequired) + "\n";
    if (result.hasAssignment)
    {
        const Assignment &a = result.assignment;
        text += "match_id=" + a.matchId + "\n";
        text += "server_address=" + a.serverAddress + "\n";
        text += "server_port=" + std::to_string(a.serverPort) + "\n";
        text += "map=" + a.map + "\n";
        text += std::string("accept_required=") + (a.acceptRequired ? "1" : "0") + "\n";
        text += "required_players=" + std::to_string(a.requiredPlayers) + "\n";
    }
    return text;
}

bool DeserializeResult(std::string_view text, SearchResult &result)
{
    result = {};
    bool any = false;
    size_t position = 0;
    while (position < text.size())
    {
        size_t eol = text.find('\n', position);
        std::string_view line = text.substr(position, eol == std::string_view::npos ? std::string_view::npos : eol - position);
        position = eol == std::string_view::npos ? text.size() : eol + 1;

        size_t equals = line.find('=');
        if (equals == std::string_view::npos)
        {
            continue;
        }

        const std::string_view key = line.substr(0, equals);
        const std::string value{ line.substr(equals + 1) };
        Assignment &a = result.assignment;
        if (key == "request_id") { result.requestId = value; any = true; }
        else if (key == "status") { result.status = value; }
        else if (key == "players") { result.matchPlayers = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10)); }
        else if (key == "required") { result.matchRequired = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10)); }
        else if (key == "match_id") { a.matchId = value; result.hasAssignment = true; }
        else if (key == "server_address") { a.serverAddress = value; }
        else if (key == "server_port") { a.serverPort = static_cast<uint16_t>(strtoul(value.c_str(), nullptr, 10)); }
        else if (key == "map") { a.map = value; }
        else if (key == "accept_required") { a.acceptRequired = value == "1"; }
        else if (key == "required_players") { a.requiredPlayers = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10)); }
    }

    return any && !result.status.empty();
}

void SetResultHandler(ResultHandler handler, void *context)
{
    try
    {
        Client::Get().SetResultHandler(handler, context);
    }
    catch (...)
    {
    }
}

bool Enabled()
{
    try
    {
        return Client::Get().Enabled();
    }
    catch (...)
    {
        return false;
    }
}

void SearchStarted(const SearchInfo &info, uint64_t steamId)
{
    try
    {
        Client::Get().SearchStarted(info, steamId);
    }
    catch (...)
    {
        // a side path must never take the GC down
    }
}

void SearchCancelled()
{
    try
    {
        Client::Get().SearchCancelled();
    }
    catch (...)
    {
    }
}

std::string ActiveRequestId()
{
    try
    {
        return Client::Get().ActiveRequestId();
    }
    catch (...)
    {
        return {};
    }
}

RosterResult FetchServerRoster(const std::string &address, uint16_t port, ServerRoster &roster, std::string &message)
{
    try
    {
        return Client::Get().FetchServerRoster(address, port, roster, message);
    }
    catch (...)
    {
        message = "exception";
        return RosterResult::Unreachable;
    }
}

bool ConfirmServerRoster(const std::string &address, uint16_t port, const std::string &matchId, std::string &message)
{
    try
    {
        return Client::Get().ConfirmServerRoster(address, port, matchId, message);
    }
    catch (...)
    {
        message = "exception";
        return false;
    }
}

} // namespace BackendClient

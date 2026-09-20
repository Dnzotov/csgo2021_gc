#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

// The address of a srcds process, read from ITS command line. Header only and free of the DLL's dependencies, so the
// offline tests (offline_tests/roster/launch_args_test.cpp) run the very code the DLL runs.
//
//   -ip <address> -port <port>                 where the game server itself listens (the engine binds it; the fake participants of
//                                              the test roster send their reservation checks there)
//   -backend_ip <address> -backend_port <port> how the Java backend knows this game server (game_server.host/port of its
//                                              registry): ONLY the identification in GET /api/v1/servers/roster and
//                                              POST /api/v1/servers/roster/ready. They never reach the engine.
//
// Without -backend_ip / -backend_port the backend is asked for the game address (-ip / -port), which is what a server does that
// is registered under its own address. Each of the two falls back on its own.
namespace LaunchArgs
{

// "-name value" from the command line; the same tokenizing the -gc_mode / -ip / -port parsing always had
inline std::string Value(std::string_view commandLine, std::string_view name)
{
    size_t position = 0;
    std::string previous;
    while (position < commandLine.size())
    {
        while (position < commandLine.size() && commandLine[position] == ' ')
        {
            position++;
        }

        size_t end = commandLine.find(' ', position);
        if (end == std::string_view::npos)
        {
            end = commandLine.size();
        }

        std::string token{ commandLine.substr(position, end - position) };
        if (previous == name)
        {
            return token;
        }

        previous = token;
        position = end;
    }

    return {};
}

// 1..65535, 0 when it is not a port. Not strict = leading digits count ("27016abc" is 27016), which is how -port was read
// before -backend_port existed
inline uint16_t ParsePort(std::string_view text, bool strict = true)
{
    unsigned value = 0;
    const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || (strict && result.ptr != text.data() + text.size()) || value == 0 || value > 65535)
    {
        return 0;
    }

    return static_cast<uint16_t>(value);
}

struct ServerEndpoints
{
    std::string gameAddress;    // -ip, loopback without it
    uint16_t gamePort;          // -port, srcds' default 27015 without it
    std::string backendAddress; // -backend_ip, else gameAddress
    uint16_t backendPort;       // -backend_port, else gamePort
};

inline ServerEndpoints ResolveServerEndpoints(std::string_view commandLine)
{
    ServerEndpoints endpoints;

    endpoints.gameAddress = Value(commandLine, "-ip");
    if (endpoints.gameAddress.empty())
    {
        endpoints.gameAddress = "127.0.0.1";
    }

    // as it always was: a value that is not a number is port 0 (not silently srcds' default)
    const std::string port = Value(commandLine, "-port");
    endpoints.gamePort = port.empty() ? static_cast<uint16_t>(27015) : ParsePort(port, false);

    endpoints.backendAddress = Value(commandLine, "-backend_ip");
    if (endpoints.backendAddress.empty())
    {
        endpoints.backendAddress = endpoints.gameAddress;
    }

    endpoints.backendPort = ParsePort(Value(commandLine, "-backend_port"));
    if (endpoints.backendPort == 0)
    {
        endpoints.backendPort = endpoints.gamePort;
    }

    return endpoints;
}

} // namespace LaunchArgs

#pragma once
// test stand-in for config.h: reads matchmaking.backend_url / backend_api_key from csgo_gc/config.txt with the real
// KeyValue parser, exactly like GCConfig does (same GetSubkey/GetString calls)
#include <string>
#include <string_view>
class GCConfig
{
public:
    GCConfig();
    std::string_view BackendUrl() const { return m_url; }
    std::string_view BackendApiKey() const { return m_key; }
private:
    std::string m_url, m_key;
};
const GCConfig &GetConfig();

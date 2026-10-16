// Rev3 netplay (milestone M4): REV3_NETSIM, the seeded network simulator. See Rev3NetSim.h for what it does and how it fits in.
// Licensed under GPLv2+. No sockets, no clock, no Dolphin core.

#include "Core/Slippi/Rev3NetSim.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace Rev3::Net
{
namespace
{
std::string Fmt(const char* format, ...)
{
  char buf[256];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (n < 0)
    return {};
  return std::string(buf, std::min<size_t>(static_cast<size_t>(n), sizeof(buf) - 1));
}

std::string Trimmed(const std::string& s)
{
  size_t b = 0;
  size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0)
    ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0)
    --e;
  return s.substr(b, e - b);
}

std::string Lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool ParseWhole(const std::string& text, u64 max, u64* out)
{
  if (text.empty() || text[0] < '0' || text[0] > '9')
    return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' || v > max)
    return false;
  *out = v;
  return true;
}

// splitmix64 (the same generator as the session tests use): small, portable, good enough for a test network.
u64 SplitMix(u64* state)
{
  *state += 0x9E3779B97F4A7C15ull;
  u64 z = *state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
}  // namespace

bool ParseNetSim(const std::string& text, NetSimConfig* out, std::string* error)
{
  NetSimConfig config;
  const std::string all = Trimmed(text);
  size_t pos = 0;
  while (pos < all.size())
  {
    size_t comma = all.find(',', pos);
    if (comma == std::string::npos)
      comma = all.size();
    const std::string item = Trimmed(all.substr(pos, comma - pos));
    pos = comma + 1;
    if (item.empty())
      continue;
    const size_t eq = item.find('=');
    if (eq == std::string::npos)
    {
      *error = "'" + item + "' is not key=value";
      return false;
    }
    const std::string key = Lower(Trimmed(item.substr(0, eq)));
    const std::string value = Trimmed(item.substr(eq + 1));
    u64 whole = 0;
    if (key == "delay_ms" || key == "jitter_ms")
    {
      if (!ParseWhole(value, NETSIM_MAX_MS, &whole))
      {
        *error = Fmt("%s=%s is not a whole number in 0..%u", key.c_str(), value.c_str(), NETSIM_MAX_MS);
        return false;
      }
      (key == "delay_ms" ? config.delay_ms : config.jitter_ms) = static_cast<u32>(whole);
    }
    else if (key == "loss_pct")
    {
      errno = 0;
      char* end = nullptr;
      const double v = std::strtod(value.c_str(), &end);
      if (value.empty() || errno != 0 || end == nullptr || *end != '\0' || !(v >= 0.0 && v <= 100.0))
      {
        *error = Fmt("loss_pct=%s is not a number in 0..100", value.c_str());
        return false;
      }
      config.loss_pct = v;
    }
    else if (key == "seed")
    {
      if (!ParseWhole(value, ~0ull, &whole))
      {
        *error = Fmt("seed=%s is not a whole number", value.c_str());
        return false;
      }
      config.seed = whole;
    }
    else
    {
      *error = "unknown key '" + key + "' (delay_ms, jitter_ms, loss_pct, seed)";
      return false;
    }
  }
  *out = config;
  return true;
}

std::string NetSimText(const NetSimConfig& config)
{
  return Fmt("delay_ms=%u jitter_ms=%u loss_pct=%g seed=%llu", config.delay_ms, config.jitter_ms, config.loss_pct,
             static_cast<unsigned long long>(config.seed));
}

NetSim::NetSim(const NetSimConfig& config, u32 stream) : m_cfg(config)
{
  // Two independent streams per instance: the jitter draws must not shift when the number of reliable messages (which never draw a loss)
  // differs between runs, and the loss pattern must not depend on how many messages jittered.
  m_jitter_state = config.seed * 0x9E3779B97F4A7C15ull + (2ull * stream + 1ull) * 0xD1B54A32D192ED03ull;
  m_loss_state = config.seed * 0xC2B2AE3D27D4EB4Full + (2ull * stream + 2ull) * 0x165667B19E3779F9ull;
  m_stats.active = config.Active();
}

bool NetSim::Submit(u64 now_us, bool droppable, u64 tag, std::vector<u8> bytes)
{
  ++m_stats.submitted;
  // The draws happen in a fixed order for every message (loss first, only for droppable ones; then jitter), so a seed fixes the whole run.
  if (droppable && m_cfg.loss_pct > 0)
  {
    const u64 draw = SplitMix(&m_loss_state) % 1000000ull;  // 0..999999: resolution of 0.0001 percent
    if (static_cast<double>(draw) < m_cfg.loss_pct * 10000.0)
    {
      ++m_stats.dropped;
      return false;
    }
  }
  s64 hold_us = static_cast<s64>(m_cfg.delay_ms) * 1000;
  if (m_cfg.jitter_ms != 0)
  {
    const u64 span = 2ull * m_cfg.jitter_ms * 1000ull + 1ull;  // -jitter .. +jitter in microseconds
    hold_us += static_cast<s64>(SplitMix(&m_jitter_state) % span) - static_cast<s64>(m_cfg.jitter_ms) * 1000;
  }
  if (hold_us < 0)
    hold_us = 0;
  if (hold_us > 0)
    ++m_stats.held;
  Entry entry;
  entry.tag = tag;
  entry.bytes = std::move(bytes);
  m_queue.emplace(std::make_pair(now_us + static_cast<u64>(hold_us), m_order++), std::move(entry));
  return true;
}

bool NetSim::PopDue(u64 now_us, u64* tag, std::vector<u8>* bytes)
{
  if (m_queue.empty() || m_queue.begin()->first.first > now_us)
    return false;
  const auto it = m_queue.begin();
  *tag = it->second.tag;
  *bytes = std::move(it->second.bytes);
  m_queue.erase(it);
  ++m_stats.released;
  return true;
}
}  // namespace Rev3::Net

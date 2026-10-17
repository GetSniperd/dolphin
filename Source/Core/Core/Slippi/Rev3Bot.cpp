// Rev3 unattended soak testing: the bot pad, the end-of-run decisions, the option parsing. See Rev3Bot.h for what it does and how it fits in.
// Licensed under GPLv2+. No Dolphin core, no clock, no files.

#include "Core/Slippi/Rev3Bot.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace Rev3::Bot
{
namespace
{
std::string Fmt(const char* format, ...)
{
  char buf[512];
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

// A whole number, decimal or 0x hex, no sign, no trailing characters, at most `max`.
bool ParseWhole(const std::string& text, u64 max, u64* out)
{
  if (text.empty() || text[0] < '0' || text[0] > '9')
    return false;
  const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text.c_str() + (hex ? 2 : 0), &end, hex ? 16 : 10);
  if (errno != 0 || end == nullptr || *end != '\0' || end == text.c_str() + (hex ? 2 : 0) || v > max)
    return false;
  *out = v;
  return true;
}

// splitmix64: small, portable, the same generator Rev3NetSim uses. Output is identical on every platform (no std:: distributions).
u64 Mix(u64 z)
{
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

struct Rng
{
  u64 state;
  Rng(u64 seed, u32 port, u32 index)
      : state(Mix(seed ^ (Mix(static_cast<u64>(port) + 1) * 0xD1B54A32D192ED03ull) ^ (Mix(static_cast<u64>(index) + 0x51) * 0x9E3779B97F4A7C15ull)))
  {
  }
  u64 Next64()
  {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  // 0 .. n-1
  u32 Below(u32 n) { return static_cast<u32>(((Next64() >> 32) * static_cast<u64>(n)) >> 32); }
  // lo .. hi inclusive
  u32 Range(u32 lo, u32 hi) { return lo + Below(hi - lo + 1); }
};

// A stick push in direction (dx, dy), each -1 / 0 / 1 (x right, y up); diagonals use the smaller per-axis value.
void Dir(int dx, int dy, std::int8_t* x, std::int8_t* y)
{
  const int mag = (dx != 0 && dy != 0) ? STICK_DIAG : STICK_FULL;
  *x = static_cast<std::int8_t>(dx * mag);
  *y = static_cast<std::int8_t>(dy * mag);
}

PadStep Step(u32 frames, u16 buttons, int dx, int dy)
{
  PadStep s;
  s.frames = std::max<u32>(frames, 1);
  s.buttons = buttons;
  Dir(dx, dy, &s.stick_x, &s.stick_y);
  if ((buttons & BTN_L) != 0)
    s.trigger_l = TRIGGER_FULL;
  if ((buttons & BTN_R) != 0)
    s.trigger_r = TRIGGER_FULL;
  return s;
}

// A direction for a held push: 8 directions, left / right twice as likely as the others (weights 3 3 1 1 1 1 1 1 of 12).
void PickDirection(Rng* rng, int* dx, int* dy)
{
  static const int DIRS[8][2] = {{-1, 0}, {1, 0}, {0, 1}, {0, -1}, {-1, 1}, {1, 1}, {-1, -1}, {1, -1}};
  static const u32 WEIGHT[8] = {3, 3, 1, 1, 1, 1, 1, 1};
  u32 pick = rng->Below(12);
  for (size_t i = 0; i < 8; ++i)
  {
    if (pick < WEIGHT[i])
    {
      *dx = DIRS[i][0];
      *dy = DIRS[i][1];
      return;
    }
    pick -= WEIGHT[i];
  }
  *dx = 1;
  *dy = 0;
}

// The stick held through a button burst: neutral 40 %, left 20 %, right 20 %, up 10 %, down 10 %.
void PickBurstStick(Rng* rng, int* dx, int* dy)
{
  const u32 p = rng->Below(10);
  *dx = 0;
  *dy = 0;
  if (p < 4)
    return;
  if (p < 6)
    *dx = -1;
  else if (p < 8)
    *dx = 1;
  else if (p == 8)
    *dy = 1;
  else
    *dy = -1;
}
}  // namespace

const char* SegmentKindName(SegmentKind kind)
{
  switch (kind)
  {
  case SegmentKind::Idle:
    return "idle";
  case SegmentKind::Move:
    return "move";
  case SegmentKind::Jump:
    return "jump";
  case SegmentKind::Attack:
    return "attack";
  case SegmentKind::Jutsu:
    return "jutsu";
  case SegmentKind::Throw:
    return "throw";
  case SegmentKind::Sidestep:
    return "sidestep";
  case SegmentKind::Chakra:
    return "chakra";
  case SegmentKind::Super:
    return "super";
  case SegmentKind::Guard:
    return "guard";
  case SegmentKind::Count:
    break;
  }
  return "?";
}

u32 Segment::Length() const
{
  u32 n = 0;
  for (const PadStep& s : steps)
    n += s.frames;
  return std::max<u32>(n, 1);
}

Segment BuildSegment(u64 seed, u32 port, u32 index)
{
  Rng rng(seed, port, index);
  Segment seg;
  u32 pick = rng.Below(100);
  seg.kind = SegmentKind::Idle;
  for (size_t k = 0; k < SEGMENT_WEIGHTS.size(); ++k)
  {
    if (pick < SEGMENT_WEIGHTS[k])
    {
      seg.kind = static_cast<SegmentKind>(k);
      break;
    }
    pick -= SEGMENT_WEIGHTS[k];
  }
  std::vector<PadStep>& steps = seg.steps;
  steps.reserve(6);  // longest segment has 5 steps; also keeps GCC's stringop-overflow false positive quiet
  switch (seg.kind)
  {
  case SegmentKind::Idle:
    steps.push_back(Step(rng.Range(MOVE_MIN_FRAMES, MOVE_MAX_FRAMES), 0, 0, 0));
    break;
  case SegmentKind::Move:
  {
    int dx = 0;
    int dy = 0;
    PickDirection(&rng, &dx, &dy);
    steps.push_back(Step(rng.Range(MOVE_MIN_FRAMES, MOVE_MAX_FRAMES), 0, dx, dy));
    break;
  }
  case SegmentKind::Guard:
  {
    // Guard is automatic unless moving towards the opponent or pressing an action; holding back guards (docs/findings.md "Guard in the takes").
    const int dx = rng.Below(2) == 0 ? -1 : 1;
    steps.push_back(Step(rng.Range(20, MOVE_MAX_FRAMES), 0, dx, 0));
    break;
  }
  case SegmentKind::Jump:
  {
    const u32 side = rng.Below(3);  // 0 straight up, 1 up-left, 2 up-right
    const int dx = side == 0 ? 0 : (side == 1 ? -1 : 1);
    steps.push_back(Step(rng.Range(2, 4), 0, dx, 1));
    steps.push_back(Step(rng.Range(6, 18), 0, dx, 0));
    if (rng.Below(2) == 0)
      steps.push_back(Step(3, BTN_B, 0, 0));  // an air attack
    steps.push_back(Step(rng.Range(2, 10), 0, 0, 0));
    break;
  }
  case SegmentKind::Attack:
  {
    int dx = 0;
    int dy = 0;
    PickBurstStick(&rng, &dx, &dy);
    const u32 presses = rng.Range(1, 5);
    for (u32 i = 0; i < presses; ++i)
    {
      const u16 button = rng.Below(10) < 6 ? BTN_B : BTN_A;  // B weak 60 %, A strong 40 %
      steps.push_back(Step(rng.Range(2, 4), button, dx, dy));
      steps.push_back(Step(rng.Range(2, 8), 0, dx, dy));
    }
    break;
  }
  case SegmentKind::Jutsu:
  {
    const int dy = rng.Below(10) < 3 ? -1 : 0;
    const u32 presses = rng.Range(1, 2);
    for (u32 i = 0; i < presses; ++i)
    {
      steps.push_back(Step(rng.Range(3, 4), BTN_X, 0, dy));
      steps.push_back(Step(rng.Range(6, 14), 0, 0, 0));
    }
    break;
  }
  case SegmentKind::Throw:
  {
    const int dx = rng.Below(2) == 0 ? -1 : 1;
    steps.push_back(Step(rng.Range(4, 8), 0, dx, 0));
    steps.push_back(Step(3, BTN_Y, dx, 0));
    steps.push_back(Step(10, 0, 0, 0));
    break;
  }
  case SegmentKind::Sidestep:
    steps.push_back(Step(3, rng.Below(2) == 0 ? BTN_L : BTN_R, 0, 0));
    steps.push_back(Step(10, 0, 0, 0));
    break;
  case SegmentKind::Chakra:
    steps.push_back(Step(rng.Range(15, 40), BTN_Z, 0, 0));
    break;
  case SegmentKind::Super:
  {
    const int m = rng.Below(2) == 0 ? -1 : 1;
    if (rng.Below(2) == 0)
    {
      // a quarter-circle motion into X: down, down-forward, forward + X
      steps.push_back(Step(3, 0, 0, -1));
      steps.push_back(Step(3, 0, m, -1));
      steps.push_back(Step(3, 0, m, 0));
      steps.push_back(Step(4, BTN_X, m, 0));
    }
    else
    {
      // 2X: down + X (rev3 findings: the 5X / 2X cinematics are supers)
      steps.push_back(Step(5, BTN_X, 0, -1));
    }
    steps.push_back(Step(6, 0, 0, 0));
    break;
  }
  case SegmentKind::Count:
    steps.push_back(Step(1, 0, 0, 0));
    break;
  }
  return seg;
}

BotPad ToBotPad(const PadStep& step)
{
  BotPad pad{};
  pad[0] = static_cast<u8>(step.buttons >> 8);
  pad[1] = static_cast<u8>(step.buttons & 0xFF);
  pad[2] = static_cast<u8>(step.stick_x);
  pad[3] = static_cast<u8>(step.stick_y);
  pad[4] = static_cast<u8>(step.cstick_x);
  pad[5] = static_cast<u8>(step.cstick_y);
  pad[6] = step.trigger_l;
  pad[7] = step.trigger_r;
  // analogA / analogB (8, 9) = 0 and err (10) = 0: a good read (the session sends 8 bytes and rebuilds the rest the same way)
  return pad;
}

PadGenerator::PadGenerator(u64 seed, u32 port) : m_seed(seed), m_port(port) {}

void PadGenerator::Ensure(u32 frame)
{
  while (m_end.empty() || m_end.back() <= frame)
  {
    const u32 index = static_cast<u32>(m_end.size());
    const u32 start = m_end.empty() ? 0 : m_end.back();
    m_end.push_back(start + BuildSegment(m_seed, m_port, index).Length());
  }
}

BotPad PadGenerator::FightPad(u32 frame)
{
  if (frame >= GENERATOR_MAX_FRAME)
    return BotPad{};
  Ensure(frame);
  const u32 index = static_cast<u32>(std::upper_bound(m_end.begin(), m_end.end(), frame) - m_end.begin());
  const u32 start = index == 0 ? 0 : m_end[index - 1];
  if (index != m_cached_index)
  {
    m_cached = BuildSegment(m_seed, m_port, index);
    m_cached_index = index;
  }
  u32 offset = frame - start;
  for (const PadStep& step : m_cached.steps)
  {
    if (offset < step.frames)
      return ToBotPad(step);
    offset -= step.frames;
  }
  return BotPad{};
}

SegmentKind PadGenerator::KindAt(u32 frame)
{
  if (frame >= GENERATOR_MAX_FRAME)
    return SegmentKind::Idle;
  Ensure(frame);
  const u32 index = static_cast<u32>(std::upper_bound(m_end.begin(), m_end.end(), frame) - m_end.begin());
  return BuildSegment(m_seed, m_port, index).kind;
}

BotPad PadGenerator::MenuPad(u32 frame) const
{
  const u32 cycle = frame / MENU_PERIOD_FRAMES;
  const u32 phase = frame % MENU_PERIOD_FRAMES;
  Rng rng(m_seed, m_port, cycle ^ 0x40000000u);  // a stream of its own, apart from the fight segments
  const u32 jitter = rng.Below(MENU_JITTER_FRAMES + 1);
  PadStep step;
  if (phase >= jitter && phase < jitter + MENU_PRESS_FRAMES)
    step.buttons = (cycle % MENU_START_EVERY == MENU_START_EVERY - 1) ? BTN_START : BTN_A;
  return ToBotPad(step);
}

// ---- controller -------------------------------------------------------------------------------------------------------------------

PadBotController::PadBotController(u64 seed, u32 port, LogFn log) : m_gen(seed, port), m_log(std::move(log)) {}

bool PadBotController::Choose(u32 frame, bool script_active, const GameObs& obs, BotPad* pad)
{
  if (!m_latched && obs.fighters_found)
  {
    m_latched = true;
    m_fight_frame = frame;
    if (m_log)
      m_log(Fmt("BOT fight found frame=%u script_active=%d", frame, script_active ? 1 : 0));
  }
  if (!m_latched && script_active)
    return false;  // the script is still playing the menus
  if (!m_took_over)
  {
    m_took_over = true;
    if (m_log)
      m_log(Fmt("BOT takeover frame=%u why=%s seed=%llu port=%u", frame, m_latched ? "fighters found" : "script ended or no script",
                static_cast<unsigned long long>(m_gen.seed()), m_gen.port()));
  }
  const bool fight = obs.fighters_found;
  if (!m_mode_known || fight != m_in_fight)
  {
    m_mode_known = true;
    m_in_fight = fight;
    if (m_log)
      m_log(Fmt("BOT mode=%s frame=%u", fight ? "fight" : "menu", frame));
  }
  *pad = fight ? m_gen.FightPad(frame) : m_gen.MenuPad(frame);
  return true;
}

// ---- exit -------------------------------------------------------------------------------------------------------------------------

bool ParseExitSpec(const std::string& text, ExitSpec* out, std::string* error)
{
  *out = ExitSpec();
  std::string t;
  for (const char c : Lower(Trimmed(text)))
    if (std::isspace(static_cast<unsigned char>(c)) == 0)
      t.push_back(c);
  if (t.empty())
    return true;
  if (t == "match")
  {
    out->enabled = true;
    out->kind = ExitKind::Match;
    return true;
  }
  const size_t colon = t.find(':');
  if (colon != std::string::npos)
  {
    const std::string what = t.substr(0, colon);
    u64 n = 0;
    if (what == "frames" && ParseWhole(t.substr(colon + 1), 100000000, &n) && n >= 1)
    {
      out->enabled = true;
      out->kind = ExitKind::Frames;
      out->n = static_cast<u32>(n);
      return true;
    }
    if (what == "rounds" && ParseWhole(t.substr(colon + 1), 99, &n) && n >= 1)
    {
      out->enabled = true;
      out->kind = ExitKind::Rounds;
      out->n = static_cast<u32>(n);
      return true;
    }
  }
  if (error != nullptr)
    *error = "REV3_NET_EXIT=" + text + " is not match, frames:N (1..100000000) or rounds:N (1..99)";
  return false;
}

std::string ExitSpecText(const ExitSpec& spec)
{
  if (!spec.enabled)
    return "off";
  switch (spec.kind)
  {
  case ExitKind::Match:
    return "match";
  case ExitKind::Frames:
    return Fmt("frames:%u", spec.n);
  case ExitKind::Rounds:
    return Fmt("rounds:%u", spec.n);
  }
  return "off";
}

ExitController::ExitController(const ExitSpec& spec, const ExitTuning& tuning, LogFn log)
    : m_spec(spec), m_tuning(tuning), m_log(std::move(log))
{
}

void ExitController::Decide(u32 frame, const std::string& reason)
{
  m_decided = true;
  m_decide_call = m_calls;
  m_reason = reason;
  if (m_log)
    m_log(Fmt("EXIT planned reason=\"%s\" frame=%u ko=%u stop_in_calls=%u", reason.c_str(), frame, m_ko, m_tuning.wait_calls));
}

bool ExitController::NoteLeave(u32 frame, const std::string& why)
{
  if (!m_spec.enabled || m_decided || m_stop_sent)
    return false;
  m_decided = true;
  m_left = true;
  m_decide_call = m_calls;
  m_reason = "net session left: " + why;
  if (m_log)
    m_log(Fmt("EXIT unplanned leave reason=\"%s\" frame=%u ko=%u stop_in_calls=%u (exit code %d)", why.c_str(), frame, m_ko, m_tuning.wait_calls,
              EXIT_CODE_LEFT));
  return true;
}

ExitStep ExitController::OnCall(u32 frame, const GameObs& obs)
{
  ExitStep step;
  if (!m_spec.enabled || m_stop_sent)
    return step;
  ++m_calls;

  if (obs.fighters_found && !m_fight_seen)
  {
    m_fight_seen = true;
    m_fight_start_call = m_calls;
    m_scene_known = obs.scene_valid;
    m_fight_scene = obs.scene_type;
    if (m_log)
      m_log(Fmt("EXIT watching (%s): fight found frame=%u scene=%u", ExitSpecText(m_spec).c_str(), frame, obs.scene_valid ? obs.scene_type : 0));
  }

  if (m_fight_seen && !m_decided)
  {
    // KOs: a fighter at health 0 for ko_debounce_calls after both were healthy. Fighters not found are not a KO (the run of zeros restarts).
    if (obs.fighters_found && !obs.health_zero)
    {
      m_healthy_seen = true;
      m_zero_run = 0;
    }
    else if (obs.fighters_found && obs.health_zero && m_healthy_seen)
    {
      if (++m_zero_run == std::max<u32>(m_tuning.ko_debounce_calls, 1))
      {
        ++m_ko;
        m_healthy_seen = false;
        m_last_ko_call = m_calls;
        if (m_log)
          m_log(Fmt("EXIT ko=%u frame=%u", m_ko, frame));
      }
    }
    else
    {
      m_zero_run = 0;
    }
    m_lost_run = obs.fighters_found ? 0 : m_lost_run + 1;
    m_away_run = (m_scene_known && obs.scene_valid && obs.scene_type != m_fight_scene) ? m_away_run + 1 : 0;

    switch (m_spec.kind)
    {
    case ExitKind::Frames:
      if (m_calls - m_fight_start_call + 1 >= m_spec.n)
        Decide(frame, Fmt("frames:%u", m_spec.n));
      break;
    case ExitKind::Rounds:
      if (m_ko >= m_spec.n)
        Decide(frame, Fmt("rounds:%u", m_spec.n));
      break;
    case ExitKind::Match:
      if (m_ko >= m_tuning.min_rounds_for_scene_rule && m_away_run >= m_tuning.scene_away_calls)
        Decide(frame, Fmt("match: the scene changed (%u -> %u) after %u KOs", m_fight_scene, obs.scene_type, m_ko));
      else if (m_ko >= m_tuning.min_rounds_for_scene_rule && m_lost_run >= m_tuning.lost_calls)
        Decide(frame, Fmt("match: no fighters for %u calls after %u KOs", m_lost_run, m_ko));
      else if (m_ko >= 1 && !m_healthy_seen && m_calls - m_last_ko_call >= m_tuning.idle_after_ko_calls)
        Decide(frame, Fmt("match: no new round %u calls after KO %u", m_calls - m_last_ko_call, m_ko));
      break;
    }
  }

  if (m_decided && m_calls - m_decide_call >= m_tuning.wait_calls)
  {
    m_stop_sent = true;
    step.stop = true;
    step.left = m_left;
    step.reason = m_reason;
  }
  return step;
}

bool IsPlannedLeaveReason(const std::string& why)
{
  return why.rfind("the peer left (planned exit", 0) == 0 || why.rfind("the peer left (the emulator was closed", 0) == 0;
}

bool FreezeExitDue(u64 age_ms, bool seen, bool blocked, u32 stall_ms, u32 exit_s)
{
  return exit_s != 0 && seen && !blocked && age_ms >= static_cast<u64>(stall_ms) + static_cast<u64>(exit_s) * 1000;
}

bool MaxSecondsDue(u64 elapsed_ms, u32 max_s)
{
  return max_s != 0 && elapsed_ms >= static_cast<u64>(max_s) * 1000;
}

bool StopHangDue(u64 since_stop_ms)
{
  return since_stop_ms >= STOP_GRACE_MS;
}

// ---- options ----------------------------------------------------------------------------------------------------------------------

namespace
{
// A number variable: unset keeps *value; set but not a whole number in [lo, hi] is a warning and *value stays.
void ReadNumber(const GetEnvFn& get_env, const char* name, u64 lo, u64 hi, u64* value, std::vector<std::string>* warnings)
{
  const std::string text = Trimmed(get_env(name));
  if (text.empty())
    return;
  u64 v = 0;
  if (ParseWhole(text, hi, &v) && v >= lo)
    *value = v;
  else
    warnings->push_back(Fmt("%s=%s is not a whole number in %llu..%llu; using %llu", name, text.c_str(), static_cast<unsigned long long>(lo),
                            static_cast<unsigned long long>(hi), static_cast<unsigned long long>(*value)));
}
}  // namespace

BotOptions ParseBotOptions(const GetEnvFn& get_env)
{
  BotOptions o;
  const std::string bot = Trimmed(get_env("REV3_PAD_BOT"));
  if (!bot.empty())
  {
    u64 seed = 0;
    if (ParseWhole(bot, ~0ull, &seed))
    {
      o.bot = true;
      o.seed = seed;
    }
    else
    {
      o.warnings.push_back("REV3_PAD_BOT=" + bot + " is not a whole number (the seed); the bot is off");
    }
  }
  const std::string exit_text = get_env("REV3_NET_EXIT");
  std::string error;
  if (!ParseExitSpec(exit_text, &o.exit, &error))
  {
    o.warnings.push_back(error + "; no automatic exit");
    o.exit = ExitSpec();
  }
  u64 wait_s = EXIT_WAIT_DEFAULT_S;
  ReadNumber(get_env, "REV3_NET_EXIT_WAIT_S", 0, 3600, &wait_s, &o.warnings);
  o.tuning.wait_calls = static_cast<u32>(wait_s) * EXIT_CALLS_PER_SECOND;
  u64 freeze_s = 0;
  ReadNumber(get_env, "REV3_NET_FREEZE_EXIT_S", 0, 86400, &freeze_s, &o.warnings);
  o.freeze_exit_s = static_cast<u32>(freeze_s);
  u64 max_s = 0;
  ReadNumber(get_env, "REV3_NET_MAX_S", 0, 604800, &max_s, &o.warnings);
  o.max_s = static_cast<u32>(max_s);
  return o;
}
}  // namespace Rev3::Bot

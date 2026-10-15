// Drives the plugin's per-draw classifiers with synthetic values at several
// frame rates, the way the passes call them, and checks two things:
//
//   - with few draws per tick (90 to 144 FPS), a value that changes on ticks
//     comes out smooth whether its poses arrive on the tick draw or a draw
//     late, and a value driven every frame is shown current on most draws;
//   - with many draws per tick (165 FPS and up, hitches included), and at
//     any rate with bLowRateRules=0, every value shown is bit-for-bit what
//     version 1.0.1 showed (its rules are copied below);
//   - the player's position, which changes on tick draws in ordinary play
//     and a draw late while the player dies, is shown as 1.0.1 showed it in
//     the first case and smooth in the second, at every rate.
//
// The plugin source is included so that its internal functions can be
// called. Nothing is installed: no hook, no game memory.
#include "../src/FalloutNVHFR.cpp"

#include <algorithm>
#include <random>
#include <vector>

namespace {

constexpr double kTickMs = 1000.0 / 60.0;

struct Draw {
    double wall;
    bool tick;
    double alpha;
    uint32_t ticks;
};

// The accumulator of BeginTickFrame at a fixed frame rate with some jitter,
// and with `hitches` the share of frames that take four to eight times as long.
std::vector<Draw> Schedule(double fps, int count, double jitter, uint32_t seed,
                           double hitches = 0.0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double period = 1000.0 / fps;
    std::vector<Draw> draws;
    double accumulator = 0.0;
    double wall = 0.0;
    uint32_t ticks = 0;
    for (int index = 0; index < count; ++index) {
        double delta = period * (1.0 + jitter * gauss(rng));
        if (hitches > 0.0 && unit(rng) < hitches) delta *= 4.0 + 4.0 * unit(rng);
        if (delta < 0.1) delta = 0.1;
        wall += delta;
        accumulator += delta;
        const bool tick = accumulator >= kTickMs;
        if (tick) {
            accumulator -= kTickMs;
            if (accumulator > kTickMs) accumulator = kTickMs;
            ++ticks;
        }
        draws.push_back({wall, tick, std::min(std::max(accumulator / kTickMs, 0.0), 1.0), ticks});
    }
    return draws;
}

enum class Source { OnTime, Late, TwoPart, Mouse, Drift, Chaos, ThreePart, Late2, Steady };
const char *SourceName(Source source) {
    switch (source) {
    case Source::OnTime: return "on time";
    case Source::Late: return "late";
    case Source::TwoPart: return "two-part";
    case Source::ThreePart: return "3-part";
    case Source::Late2: return "late 2";
    case Source::Steady: return "steady";
    case Source::Mouse: return "mouse";
    case Source::Drift: return "drift";
    default: return "chaos";
    }
}

constexpr double kStep = 2.0;  // units per tick
constexpr double kMouse = 0.5; // units per ms

// What the engine shows on each draw.
std::vector<double> Values(Source source, const std::vector<Draw> &draws, uint32_t seed) {
    std::mt19937 rng(seed * 7919u + 13u);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<double> values;
    double mouse = 0.0;
    bool moving = false;
    double until = 0.0;
    double last = 0.0;
    double extra = 0.0;
    int mode = 0;
    double base = 0.0;
    uint32_t baseTicks = 0;
    for (size_t index = 0; index < draws.size(); ++index) {
        const Draw &draw = draws[index];
        const uint32_t previousTicks = index != 0 ? draws[index - 1].ticks : 0;
        switch (source) {
        case Source::OnTime:
            values.push_back(kStep * draw.ticks);
            break;
        case Source::Late:
            values.push_back(kStep * previousTicks);
            break;
        case Source::ThreePart:
            // The death camera: its own move on the tick draw, the parts
            // that follow the late target one and two draws later.
            values.push_back(kStep * (0.4 * draw.ticks + 0.3 * previousTicks +
                                      0.3 * (index >= 2 ? draws[index - 2].ticks : 0)));
            break;
        case Source::Late2:
            values.push_back(kStep * (index >= 2 ? draws[index - 2].ticks : 0));
            break;
        case Source::Steady:
            values.push_back(kMouse * draw.wall);
            break;
        case Source::TwoPart:
            values.push_back(kStep * previousTicks + 0.5 * kStep * (draw.ticks - previousTicks));
            break;
        case Source::Mouse:
            if (draw.wall >= until) {
                moving = !moving;
                until = draw.wall + (moving ? 80.0 + 320.0 * unit(rng) : 40.0 + 210.0 * unit(rng));
            }
            if (moving) mouse += kMouse * (draw.wall - last);
            last = draw.wall;
            values.push_back(mouse);
            break;
        case Source::Drift:
            if (draw.tick) {
                extra = 0.0;
            } else if (unit(rng) < 0.08) {
                extra += 0.05 + 0.25 * unit(rng);
            }
            values.push_back(kStep * draw.ticks + extra);
            break;
        default:
            // Stretches of every kind of behaviour, each continuing from
            // where the last one left the value.
            if (draw.wall >= until) {
                base = values.empty() ? 0.0 : values.back();
                baseTicks = draw.ticks;
                mode = static_cast<int>(unit(rng) * 6.0);
                until = draw.wall + 100.0 + 600.0 * unit(rng);
                last = draw.wall;
                mouse = 0.0;
            }
            switch (mode) {
            case 0: values.push_back(base + kStep * (draw.ticks - baseTicks)); break;
            case 1:
                values.push_back(base + kStep * (previousTicks > baseTicks ? previousTicks - baseTicks : 0));
                break;
            case 2:
                mouse += kMouse * (draw.wall - last);
                last = draw.wall;
                values.push_back(base + mouse);
                break;
            case 3:
                if (unit(rng) < 0.1) extra = 0.05 + 0.4 * unit(rng);
                if (draw.tick) extra = 0.0;
                values.push_back(base + kStep * (draw.ticks - baseTicks) + extra);
                break;
            case 4:
                if (unit(rng) < 0.05) base += 1.0 + 3.0 * unit(rng);
                values.push_back(base + kStep * (draw.ticks - baseTicks));
                break;
            default: values.push_back(base); break;
            }
            break;
        }
    }
    return values;
}

// A pose that moves along x and turns a little about z with the value.
NodePose PoseOf(double value) {
    NodePose pose{};
    const double angle = value * 0.004;
    pose.value[0] = static_cast<float>(std::cos(angle));
    pose.value[1] = static_cast<float>(-std::sin(angle));
    pose.value[3] = static_cast<float>(std::sin(angle));
    pose.value[4] = static_cast<float>(std::cos(angle));
    pose.value[8] = 1.0f;
    pose.value[9] = static_cast<float>(value);
    pose.value[12] = 1.0f;
    return pose;
}

int KindOf(bool tick, bool previousWasTick) {
    return tick ? (previousWasTick ? 2 : 0) : (previousWasTick ? 1 : 3);
}

void ResetDrawsPerTick() {
    g_drawsPerTick = 6.0;
    g_drawsSinceTick = 0;
    g_fewDrawsPerTick = false;
}

// ---- 1.0.1's rules, copied --------------------------------------------------

struct OldFirstPersonHistory {
    uint32_t lastChange;
    uint8_t offTickScore;
    bool lastChangeLagged;
    uint32_t changePeriod;
    bool lag;
    NodePose from;
    NodePose to;
};

bool OldFirstPersonNode(OldFirstPersonHistory &history, const NodePose &current, bool frameIsTick,
                        int frameKind, uint32_t serial, uint32_t tickCount, uint32_t ticksBefore,
                        double frameAlpha, double previousAlpha, NodePose &shown) {
    if (history.offTickScore != 0) --history.offTickScore;
    if (memcmp(history.to.value, current.value, sizeof(current.value)) != 0) {
        const bool changedLastPass = history.lastChange + 1 == serial;
        history.lastChange = serial;
        const bool latePose = frameKind == 1 && !changedLastPass;
        const bool offTick = (frameKind == 1 && !latePose) || frameKind == 3;
        const bool blendLate = latePose && history.lastChangeLagged;
        history.lastChangeLagged = latePose;
        if (offTick) {
            history.offTickScore = static_cast<uint8_t>(
                history.offTickScore > 12 ? 16 : history.offTickScore + 4);
        }
        const bool perFrame = history.offTickScore >= 8;
        if (offTick || perFrame || (latePose && !blendLate)) {
            history.from = current;
            history.to = current;
            history.changePeriod = 0;
            return false;
        }
        history.from = history.to;
        history.to = current;
        history.lag = !frameIsTick;
        history.changePeriod = history.lag ? ticksBefore : tickCount;
    }
    const uint32_t period = history.lag ? ticksBefore : tickCount;
    if (history.changePeriod == 0 || history.changePeriod != period) return false;
    float stepSquared = 0.0f;
    for (size_t axis = 9; axis < 12; ++axis) {
        const float delta = history.to.value[axis] - history.from.value[axis];
        stepSquared += delta * delta;
    }
    if (!(stepSquared <= 900.0f)) return false;
    return BlendPose(history.from, history.to, history.lag ? previousAlpha : frameAlpha, shown);
}

struct OldBodyHistory {
    uint32_t changePeriod;
    uint8_t offTickScore;
    bool lag;
    uint32_t lastChange;
    NodePose from;
    NodePose to;
};

void OldBodyNode(OldBodyHistory &history, const NodePose &relative, bool frameIsTick,
                 int frameKind, uint32_t serial, uint32_t tickCount, uint32_t ticksBefore,
                 double frameAlpha, double previousAlpha, NodePose &shownRelative) {
    shownRelative = relative;
    if (history.offTickScore != 0) --history.offTickScore;
    if (RelativePoseDiffers(relative, history.to)) {
        const bool changedLastDraw = history.lastChange + 1 == serial;
        history.lastChange = serial;
        const bool lagged = frameKind == 1 && !changedLastDraw;
        const bool offTick = !frameIsTick && !lagged;
        bool minor = false;
        if (offTick) {
            float rotationStep = 0.0f;
            float translationStep = 0.0f;
            for (size_t index = 0; index < kPoseFloats; ++index) {
                const float difference = std::fabs(relative.value[index] - history.to.value[index]);
                float &largest = (index >= 9 && index < 12) ? translationStep : rotationStep;
                if (!(difference <= largest)) largest = difference;
            }
            if (rotationStep <= 0.03f && translationStep <= 0.5f) {
                history.lastChange = serial - 1;
                minor = true;
            }
        }
        if (!minor) {
            if (offTick) {
                history.offTickScore = static_cast<uint8_t>(
                    history.offTickScore > 12 ? 16 : history.offTickScore + 4);
            }
            if (offTick || history.offTickScore >= 8) {
                history.from = relative;
                history.to = relative;
                history.changePeriod = 0;
            } else {
                history.from = history.to;
                history.to = relative;
                history.lag = lagged;
                history.changePeriod = lagged ? ticksBefore : tickCount;
            }
        }
    }
    const uint32_t period = history.lag ? ticksBefore : tickCount;
    if (history.changePeriod != 0 && history.changePeriod == period) {
        float stepSquared = 0.0f;
        for (size_t axis = 9; axis < 12; ++axis) {
            const float delta = history.to.value[axis] - history.from.value[axis];
            stepSquared += delta * delta;
        }
        if (!(stepSquared <= 900.0f) ||
            !BlendPose(history.from, history.to, history.lag ? previousAlpha : frameAlpha,
                       shownRelative)) {
            shownRelative = relative;
        }
    }
}

struct OldTickTrack {
    float from;
    float to;
    uint8_t score;
    bool valid;
};

float OldAdvanceTickTrack(OldTickTrack &track, float value, bool tick, double alpha,
                          float tolerance) {
    if (track.score != 0) --track.score;
    if (!track.valid || !std::isfinite(value)) {
        track.from = track.to = value;
        track.valid = std::isfinite(value);
        track.score = 0;
        return value;
    }
    const float step = std::fabs(value - track.to);
    if (step > tolerance) {
        if (!tick) {
            track.score = static_cast<uint8_t>(track.score > 12 ? 16 : track.score + 4);
            track.from = value;
        } else if (track.score >= 8 || step > 40.0f) {
            track.from = value;
        } else {
            track.from = track.to;
        }
        track.to = value;
    } else if (tick) {
        track.from = track.to;
    }
    return static_cast<float>(track.from + (static_cast<double>(track.to) - track.from) * alpha);
}

struct OldPositionHistory {
    bool valid;
    float from[3];
    float to[3];
};

void OldAdvancePlayerPosition(OldPositionHistory &history, const float current[3], bool tick,
                              double alpha, double shown[3]) {
    if (!history.valid) {
        memcpy(history.from, current, sizeof(history.from));
        memcpy(history.to, current, sizeof(history.to));
        history.valid = true;
    } else if (DistanceSquared(current, history.to) > 1.0e-6f) {
        if (DistanceSquared(current, history.to) <= kTeleportDistanceSquared) {
            memcpy(history.from, history.to, sizeof(history.from));
        } else {
            memcpy(history.from, current, sizeof(history.from));
        }
        memcpy(history.to, current, sizeof(history.to));
    } else if (tick) {
        memcpy(history.from, history.to, sizeof(history.from));
    }
    for (int axis = 0; axis < 3; ++axis) {
        shown[axis] = static_cast<double>(history.from[axis]) +
                      (static_cast<double>(history.to[axis]) - history.from[axis]) * alpha;
    }
}

// ---- The passes, as they call the classifiers -------------------------------
// Each returns the pose shown on every draw, for the new rules (`old` false)
// or the copied ones.

std::vector<NodePose> RunFirstPerson(const std::vector<Draw> &draws,
                                     const std::vector<double> &values, bool old) {
    ResetDrawsPerTick();
    FirstPersonLocalHistory history{};
    OldFirstPersonHistory oldHistory{};
    uint32_t serial = 1;
    uint32_t tickCount = 0;
    bool previousWasTick = false;
    double previousAlpha = 0.0;
    std::vector<NodePose> shownPoses;
    for (size_t index = 0; index < draws.size(); ++index) {
        const Draw &draw = draws[index];
        NoteDrawsPerTick(draw.tick);
        const uint32_t ticksBefore = tickCount;
        if (draw.tick) ++tickCount;
        const int kind = KindOf(draw.tick, previousWasTick);
        const double alphaBefore = previousAlpha;
        previousWasTick = draw.tick;
        previousAlpha = draw.alpha;
        ++serial;
        const NodePose current = PoseOf(values[index]);
        NodePose shown = current;
        if (index == 0) {
            history.from = history.to = current;
            ResetChangeEvidence(history.evidence, serial, tickCount);
            oldHistory.from = oldHistory.to = current;
        } else if (old) {
            NodePose blended;
            if (OldFirstPersonNode(oldHistory, current, draw.tick, kind, serial, tickCount,
                                   ticksBefore, draw.alpha, alphaBefore, blended)) {
                shown = blended;
            }
        } else {
            const RenderPhase phase{serial, tickCount, ticksBefore, kind, draw.alpha, alphaBefore};
            bool tooBig = false;
            bool blend = false;
            NodePose blended;
            AdvanceFirstPersonNode(history, current, phase, tooBig, blended, blend);
            if (blend) shown = blended;
        }
        shownPoses.push_back(shown);
    }
    return shownPoses;
}

std::vector<NodePose> RunBody(const std::vector<Draw> &draws, const std::vector<double> &values,
                              bool old) {
    ResetDrawsPerTick();
    memset(g_bodyHistory, 0, sizeof(g_bodyHistory));
    g_bodySerial = 1;
    g_bodyTickCount = 0;
    g_bodyPreviousDrawWasTick = false;
    g_bodyPreviousDrawAlpha = 0.0;
    OldBodyHistory oldHistory{};
    const uintptr_t object = 0x00123450;
    std::vector<NodePose> shownPoses;
    for (size_t index = 0; index < draws.size(); ++index) {
        const Draw &draw = draws[index];
        NoteDrawsPerTick(draw.tick);
        // As ApplyPlayerBodyPose.
        const uint32_t ticksBefore = g_bodyTickCount;
        if (draw.tick) ++g_bodyTickCount;
        ++g_bodySerial;
        const bool previousWasTick = g_bodyPreviousDrawWasTick;
        const double previousAlpha = g_bodyPreviousDrawAlpha;
        g_bodyPreviousDrawWasTick = draw.tick;
        g_bodyPreviousDrawAlpha = draw.alpha;
        const int kind = KindOf(draw.tick, previousWasTick);
        const NodePose relative = PoseOf(values[index]);
        NodePose shown = relative;
        if (old) {
            if (index == 0) {
                oldHistory.from = oldHistory.to = relative;
            } else {
                OldBodyNode(oldHistory, relative, draw.tick, kind, g_bodySerial, g_bodyTickCount,
                            ticksBefore, draw.alpha, previousAlpha, shown);
            }
        } else {
            bool blended = false;
            AdvanceBodyNode(object, relative, kind, ticksBefore, draw.alpha, previousAlpha, shown,
                            blended);
        }
        shownPoses.push_back(shown);
    }
    return shownPoses;
}

std::vector<NodePose> RunOffset(const std::vector<Draw> &draws, const std::vector<double> &values,
                                bool old) {
    ResetDrawsPerTick();
    TickTrack track{};
    OldTickTrack oldTrack{};
    g_thirdPersonSerial = 1;
    g_thirdPersonTickCount = 0;
    g_thirdPersonPreviousWasTick = false;
    std::vector<NodePose> shownPoses;
    for (const Draw &draw : draws) {
        NoteDrawsPerTick(draw.tick);
        // As ThirdPersonCameraDelta.
        ++g_thirdPersonSerial;
        if (draw.tick) ++g_thirdPersonTickCount;
        const int kind = KindOf(draw.tick, g_thirdPersonPreviousWasTick);
        g_thirdPersonPreviousWasTick = draw.tick;
        const float value = static_cast<float>(values[&draw - draws.data()]);
        const float shown = old ? OldAdvanceTickTrack(oldTrack, value, draw.tick, draw.alpha, 0.01f)
                                : AdvanceTickTrack(track, value, draw.tick, kind, draw.alpha, 0.01f);
        NodePose pose{};
        pose.value[9] = shown;
        shownPoses.push_back(pose);
    }
    return shownPoses;
}

std::vector<NodePose> RunKillCam(const std::vector<Draw> &draws, const std::vector<double> &values,
                                 bool /*old*/) {
    ResetDrawsPerTick();
    g_killCamTrack = {};
    g_killCamSerial = 1;
    g_killCamDrawsSinceTick = 0;
    g_killCamTickCount = 0;
    g_killCamPreviousWasTick = false;
    g_killCamPreviousAlpha = 0.0;
    alignas(16) static uint8_t camera[0x100];
    std::vector<NodePose> shownPoses;
    for (size_t index = 0; index < draws.size(); ++index) {
        const Draw &draw = draws[index];
        NoteDrawsPerTick(draw.tick);
        const NodePose current = PoseOf(values[index]);
        memcpy(camera + kNodeWorldTransform, current.value, sizeof(current.value));
        NodePose shown;
        if (!AdvanceKillCamTrack(reinterpret_cast<uintptr_t>(camera), true, draw.tick, draw.alpha,
                                 shown)) {
            shown = current;
        }
        shownPoses.push_back(shown);
    }
    return shownPoses;
}

// The position shown in value[9], the correction it makes in value[10].
std::vector<NodePose> RunPosition(const std::vector<Draw> &draws,
                                  const std::vector<double> &values, bool old) {
    ResetDrawsPerTick();
    g_playerHistory = {};
    g_positionSerial = 1;
    g_positionTickCount = 0;
    g_positionPreviousWasTick = false;
    g_positionPreviousAlpha = 0.0;
    OldPositionHistory oldHistory{};
    std::vector<NodePose> shownPoses;
    for (size_t index = 0; index < draws.size(); ++index) {
        const Draw &draw = draws[index];
        NoteDrawsPerTick(draw.tick);
        const float current[3] = {static_cast<float>(values[index]), 0.0f, 0.0f};
        double shown[3];
        if (old) {
            OldAdvancePlayerPosition(oldHistory, current, draw.tick, draw.alpha, shown);
        } else {
            AdvancePlayerPosition(current, draw.tick, draw.alpha, shown);
        }
        NodePose pose{};
        pose.value[9] = static_cast<float>(shown[0]);
        pose.value[10] = static_cast<float>(shown[0] - current[0]);
        shownPoses.push_back(pose);
    }
    return shownPoses;
}

using Runner = std::vector<NodePose> (*)(const std::vector<Draw> &, const std::vector<double> &,
                                         bool);

// ---- Measures ------------------------------------------------------------------

constexpr int kDraws = 6000;
constexpr int kSkip = 400; // the draws-per-tick estimate settles in this

struct Quality {
    double maxDeviation; // ticks of motion, from a constant delay
    double rmsDeviation;
    double lateShare;    // mouse: share of draws more than 2 ms behind
};

Quality Measure(Runner run, Source source, double fps, uint32_t seed, bool old = false) {
    const std::vector<Draw> draws = Schedule(fps, kDraws, 0.02, seed);
    const std::vector<double> values = Values(source, draws, seed);
    const std::vector<NodePose> shown = run(draws, values, old);
    Quality quality{};
    if (source == Source::Mouse) {
        int late = 0;
        for (int index = kSkip; index < kDraws; ++index) {
            if (std::fabs(values[index] - shown[index].value[9]) / kMouse > 2.0) ++late;
        }
        quality.lateShare = static_cast<double>(late) / (kDraws - kSkip);
        return quality;
    }
    std::vector<double> errors;
    for (int index = kSkip; index < kDraws; ++index) {
        errors.push_back(source == Source::Steady
                             ? (shown[index].value[9] - kMouse * draws[index].wall) /
                                   (kMouse * kTickMs)
                             : (shown[index].value[9] - kStep * draws[index].wall / kTickMs) /
                                   kStep);
    }
    std::vector<double> sorted = errors;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    double squares = 0.0;
    for (const double error : errors) {
        const double deviation = std::fabs(error - median);
        quality.maxDeviation = std::max(quality.maxDeviation, deviation);
        squares += deviation * deviation;
    }
    quality.rmsDeviation = std::sqrt(squares / errors.size());
    return quality;
}

int g_failures = 0;

// `before` is what 1.0.1's rules give, for comparison (negative: not run).
void Expect(bool condition, const char *what, const char *pass, Source source, double fps,
            double value, double bound, double before) {
    char comparison[32] = "";
    if (before >= 0.0) snprintf(comparison, sizeof(comparison), ", 1.0.1 %.3f", before);
    std::printf("  %-6s %-8s %3.0f FPS: %-28s %.3f (bound %.3f%s)%s\n", pass, SourceName(source),
                fps, what, value, bound, comparison, condition ? "" : "  FAILED");
    if (!condition) ++g_failures;
}

} // namespace

int main() {
    const uint32_t seeds[] = {1, 2, 3};

    // Few draws per tick: tick-driven values smooth, mouse-driven current.
    std::printf("few draws per tick\n");
    struct Pass {
        const char *name;
        Runner run;
        bool late;    // handles late poses
        bool mouse;   // tells values driven every frame
        bool twoPart; // takes a pose arriving in two parts
        bool old;     // 1.0.1's rules are copied above
    };
    const Pass passes[] = {
        {"first", &RunFirstPerson, true, true, false, true},
        {"body", &RunBody, true, true, false, true},
        {"offset", &RunOffset, false, true, false, true},
        {"kill", &RunKillCam, true, false, true, false},
        {"place", &RunPosition, true, false, false, true},
    };
    for (const double fps : {90.0, 100.0, 120.0, 144.0}) {
        for (const Pass &pass : passes) {
            Quality worst[2]{};
            Quality worstLate[2]{};
            Quality worstMouse[2]{};
            Quality worstTwoPart[2]{};
            for (int old = 0; old < (pass.old ? 2 : 1); ++old) {
                for (const uint32_t seed : seeds) {
                    const Quality onTime = Measure(pass.run, Source::OnTime, fps, seed, old != 0);
                    const Quality late = Measure(pass.run, Source::Late, fps, seed, old != 0);
                    const Quality mouse = Measure(pass.run, Source::Mouse, fps, seed, old != 0);
                    const Quality twoPart = Measure(pass.run, Source::TwoPart, fps, seed, old != 0);
                    worst[old].maxDeviation = std::max(worst[old].maxDeviation, onTime.maxDeviation);
                    worstLate[old].maxDeviation =
                        std::max(worstLate[old].maxDeviation, late.maxDeviation);
                    worstMouse[old].lateShare = std::max(worstMouse[old].lateShare, mouse.lateShare);
                    worstTwoPart[old].maxDeviation =
                        std::max(worstTwoPart[old].maxDeviation, twoPart.maxDeviation);
                }
            }
            const auto before = [&pass](const Quality *values, double Quality::*field) {
                return pass.old ? values[1].*field : -1.0;
            };
            Expect(worst[0].maxDeviation <= 0.01, "on time: max deviation", pass.name,
                   Source::OnTime, fps, worst[0].maxDeviation, 0.01,
                   before(worst, &Quality::maxDeviation));
            if (pass.late) {
                Expect(worstLate[0].maxDeviation <= 0.1, "late: max deviation", pass.name,
                       Source::Late, fps, worstLate[0].maxDeviation, 0.1,
                       before(worstLate, &Quality::maxDeviation));
            }
            if (pass.mouse) {
                Expect(worstMouse[0].lateShare <= 0.08, "mouse: share of draws behind", pass.name,
                       Source::Mouse, fps, worstMouse[0].lateShare, 0.08,
                       before(worstMouse, &Quality::lateShare));
            }
            if (pass.twoPart) {
                Expect(worstTwoPart[0].maxDeviation <= 0.01, "two-part: max deviation", pass.name,
                       Source::TwoPart, fps, worstTwoPart[0].maxDeviation, 0.01, -1.0);
            }
        }
    }

    // Many draws per tick, or the rules switched off: exactly 1.0.1.
    std::printf("many draws per tick, or bLowRateRules=0: same as 1.0.1\n");
    const struct {
        const char *name;
        Runner run;
    } same[] = {{"first", &RunFirstPerson}, {"body", &RunBody}, {"offset", &RunOffset}};
    const struct {
        double fps;
        double hitches;
        bool rules;
    } cases[] = {{165.0, 0.0, true},  {240.0, 0.0, true},  {350.0, 0.0, true},
                 {240.0, 0.01, true}, {350.0, 0.01, true}, {90.0, 0.0, false},
                 {120.0, 0.0, false}};
    for (const auto &rate : cases) {
        g_lowRateRulesEnabled = rate.rules;
        for (const auto &pass : same) {
            for (const Source source :
                 {Source::OnTime, Source::Late, Source::Mouse, Source::Drift, Source::Chaos}) {
                size_t differing = 0;
                size_t total = 0;
                size_t afterTick = 0;
                for (const uint32_t seed : seeds) {
                    const std::vector<Draw> draws = Schedule(rate.fps, kDraws, 0.02, seed,
                                                             rate.hitches);
                    for (size_t index = 1; index < draws.size(); ++index) {
                        if (draws[index].tick && draws[index - 1].tick) ++afterTick;
                    }
                    const std::vector<double> values = Values(source, draws, seed);
                    const std::vector<NodePose> fresh = pass.run(draws, values, false);
                    const std::vector<NodePose> old = pass.run(draws, values, true);
                    for (size_t index = 0; index < fresh.size(); ++index) {
                        ++total;
                        if (memcmp(fresh[index].value, old[index].value, sizeof(NodePose)) != 0) {
                            ++differing;
                        }
                    }
                }
                std::printf("  %-6s %-8s %3.0f FPS%s%s: %zu of %zu draws differ (%zu tick draws "
                            "after a tick draw)%s\n",
                            pass.name, SourceName(source), rate.fps,
                            rate.hitches > 0.0 ? ", hitches" : "", rate.rules ? "" : ", rules off",
                            differing, total, afterTick, differing == 0 ? "" : "  FAILED");
                if (differing != 0) ++g_failures;
            }
        }
    }
    g_lowRateRulesEnabled = true;

    // The player's position at every rate: late changes smooth; on-time
    // ones, and everything with bLatePlayerPosition=0, as 1.0.1. With few
    // draws per tick, a small stray change on the draw after three or more
    // tick draws in a row cannot be told from a late pose and is shown with
    // the draw before's phase: there it has to stay within a stray's size of
    // 1.0.1, on under 1 % of draws. (In ordinary play the position has not
    // been seen to change off tick at all.)
    // The kill camera at every rate: motion arriving in parts up to two draws
    // after the tick smooth from 144 FPS up (below, the draws after a tick
    // are tick draws themselves); motion every frame shown as it is from
    // 200 FPS up. At 144 and 165 FPS no draw comes three after a tick, so
    // motion every frame cannot be told from late parts and is blended
    // between tick draws: smooth to within a fifth of a tick.
    std::printf("kill camera at every rate\n");
    for (const double fps : {90.0, 120.0, 144.0, 165.0, 200.0, 240.0, 300.0, 350.0}) {
        for (const Source source : {Source::OnTime, Source::Late, Source::TwoPart,
                                    Source::ThreePart, Source::Late2, Source::Steady}) {
            if (fps < 140.0 && (source == Source::ThreePart || source == Source::Late2 ||
                                source == Source::Steady)) {
                continue;
            }
            double worst = 0.0;
            for (const uint32_t seed : seeds) {
                worst = std::max(worst, Measure(&RunKillCam, source, fps, seed).maxDeviation);
            }
            const double bound = source == Source::Steady && fps < 200.0 ? 0.25 : 0.01;
            Expect(worst <= bound, "max deviation", "kill", source, fps, worst, bound, -1.0);
        }
        if (fps >= 240.0) {
            double worst = 0.0;
            for (const uint32_t seed : seeds) {
                worst = std::max(worst, Measure(&RunKillCam, Source::Mouse, fps, seed).lateShare);
            }
            const double bound = fps >= 300.0 ? 0.02 : 0.04;
            Expect(worst <= bound, "mouse: share of draws behind", "kill", Source::Mouse, fps,
                   worst, bound, -1.0);
        }
    }

    std::printf("player position\n");
    for (const double fps : {90.0, 120.0, 165.0, 240.0, 350.0}) {
        Quality late[2]{};
        for (int old = 0; old < 2; ++old) {
            for (const uint32_t seed : seeds) {
                late[old].maxDeviation =
                    std::max(late[old].maxDeviation,
                             Measure(&RunPosition, Source::Late, fps, seed, old != 0).maxDeviation);
            }
        }
        Expect(late[0].maxDeviation <= 0.06, "late: max deviation", "place", Source::Late, fps,
               late[0].maxDeviation, 0.06, late[1].maxDeviation);
        for (const bool enabled : {true, false}) {
            g_latePlayerPositionEnabled = enabled;
            for (const Source source : {Source::OnTime, Source::Drift, Source::Late, Source::Mouse,
                                        Source::Chaos}) {
                if (enabled && source != Source::OnTime && source != Source::Drift) continue;
                size_t differing = 0;
                size_t total = 0;
                double largest = 0.0;
                for (const uint32_t seed : seeds) {
                    const std::vector<Draw> draws = Schedule(fps, kDraws, 0.02, seed);
                    const std::vector<double> values = Values(source, draws, seed);
                    const std::vector<NodePose> fresh = RunPosition(draws, values, false);
                    const std::vector<NodePose> old = RunPosition(draws, values, true);
                    for (size_t index = 0; index < fresh.size(); ++index) {
                        ++total;
                        if (memcmp(fresh[index].value, old[index].value, sizeof(NodePose)) != 0) {
                            ++differing;
                            largest = std::max(largest, static_cast<double>(std::fabs(
                                                            fresh[index].value[9] -
                                                            old[index].value[9])));
                        }
                    }
                }
                const bool strayAllowed = enabled && source == Source::Drift && fps < 150.0;
                const bool ok = strayAllowed
                    ? largest <= 0.3 && differing * 100 < total
                    : differing == 0;
                std::printf("  place  %-8s %3.0f FPS%s: %zu of %zu draws differ from 1.0.1, by up "
                            "to %.3f%s\n",
                            SourceName(source), fps, enabled ? "" : ", switch off", differing,
                            total, largest, ok ? "" : "  FAILED");
                if (!ok) ++g_failures;
            }
        }
        g_latePlayerPositionEnabled = true;
    }

    if (g_failures != 0) {
        std::printf("classifier test: %d failed\n", g_failures);
        return 1;
    }
    std::printf("classifier test ok\n");
    return 0;
}

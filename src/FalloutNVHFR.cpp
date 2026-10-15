// FalloutNVHFR: high-frame-rate patch for Fallout: New Vegas, as an ASI plugin.
//
// This build covers steps 0 to 4 of the plan: it recognises the executable,
// gives the engine a fixed simulation tick decided from the wall clock, keeps
// the engine's timers off their sleeping path, freezes the simulation on the
// frames between ticks with the engine's own freeze-time flag, makes the
// physics step equal to the tick, and delivers key and button presses on
// ticks. For the main world draw it writes interpolated values and puts the
// engine's back afterwards: the scene camera follows the player's position
// blended between the last two ticks, the sky is shifted with it, and in
// third person the camera's offset and the player's body are blended too. In
// first person the local transforms of the arms and weapon skeleton are
// blended for the whole of the render, and the camera takes the blended
// position and rotation of the skeleton's camera bone. The field of view is
// blended where it is handed to the cameras, and other characters in the
// high-process list get blended world poses for the main world draw, as do
// the objects of the loaded cells that moved recently or carry an animation
// controller (carried and dropped items, doors).

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr char kPatchVersion[] = "1.0.3";

// The build this was developed against (GOG 1.4.0.525). Reported in the log
// for comparison only; nothing is refused because of it.
constexpr DWORD kReferencePeTimestamp = 0x4E0D50ED;
constexpr uintptr_t kImageBase = 0x00400000;

// ---- Addresses, GOG FalloutNV.exe 1.4.0.525 --------------------------------

// call FUN_0086E650 (the per-frame function), ecx = [0x011DEA0C].
constexpr uintptr_t kFrameCall = 0x0086B3E3;
constexpr uint8_t kFrameCallBytes[5] = {0xE8, 0x68, 0x32, 0x00, 0x00};
constexpr uintptr_t kFrameFunction = 0x0086E650;

// call FUN_00771520 (BSTimer update from GetTickCount), ecx = 0x011F6394.
constexpr uintptr_t kTimerCall = 0x0086F296;
constexpr uint8_t kTimerCallBytes[5] = {0xE8, 0x85, 0x22, 0xF0, 0xFF};
constexpr uintptr_t kTimerUpdateFunction = 0x00771520;

// The other callers of the same timer update. Each updates a timer that has
// no clamp, so at a high frame rate the stock update sleeps up to 9 ms.
struct AuxTimerSite {
    uintptr_t address;
    uint8_t bytes[5];
    const char *what;
};
constexpr AuxTimerSite kAuxTimerSites[] = {
    {0x00770482, {0xE8, 0x99, 0x10, 0x00, 0x00}, "timer at [0x011D96C0]+0x224, site A"},
    {0x00773287, {0xE8, 0x94, 0xE2, 0xFF, 0xFF}, "timer at [0x011D96C0]+0x224, site B"},
    {0x007AA2CE, {0xE8, 0x4D, 0x72, 0xFC, 0xFF}, "timer at this+0x70"},
};
constexpr size_t kAuxTimerSiteCount = sizeof(kAuxTimerSites) / sizeof(kAuxTimerSites[0]);

// call OSInputGlobals poll (FUN_00A23010), inside the per-frame function's
// input step. It rewrites the key and button buffers; the player's controls
// are read after it in the same frame.
constexpr uintptr_t kInputPollCall = 0x0086F39E;
constexpr uint8_t kInputPollCallBytes[5] = {0xE8, 0x6D, 0x3C, 0x1B, 0x00};
constexpr uintptr_t kInputPollFunction = 0x00A23010;

// call FUN_009445B0, the player's look input (thiscall on the player, four
// arguments, the first a delta), from FUN_0093E860, the player's per-frame
// controls (PlayerCharacter vtable +0x2F8). The frame's input step calls
// that on every frame outside menus and fly-cam, frozen frames included,
// with the main timer's delta times the VATS time multiplier, and it passes
// the delta on unchanged. Mouse look does not use it; the vanity (free look)
// orbit, gamepad look and a camera smoothing step multiply a per-frame input
// by it.
constexpr uintptr_t kLookInputCall = 0x0093F8D9;
constexpr uint8_t kLookInputCallBytes[5] = {0xE8, 0xD2, 0x4C, 0x00, 0x00};
constexpr uintptr_t kLookInputFunction = 0x009445B0;

// The held-object update FUN_00960520 runs on every frame from the player's
// controls and does not use its delta. When the held object is a ragdoll
// farther than fZKeyComplexHelperMinDistance from the grab point, it pushes
// it towards it with an impulse built from fZKeyComplexHelperScale and the
// weight, on every frame: through FUN_0062B8D0 (cdecl: node, impulse, 0;
// every rigid body below an actor's node) or bhkRigidBody FUN_00561690
// (thiscall: impulse). An impulse changes the velocity at once, so the
// pushes of the frames between ticks add up before the next physics step.
constexpr uintptr_t kGrabHelperActorCall = 0x00960F8E;
constexpr uint8_t kGrabHelperActorCallBytes[5] = {0xE8, 0x3D, 0xA9, 0xCC, 0xFF};
constexpr uintptr_t kGrabHelperActorFunction = 0x0062B8D0;
constexpr uintptr_t kGrabHelperBodyCall = 0x00961051;
constexpr uint8_t kGrabHelperBodyCallBytes[5] = {0xE8, 0x3A, 0x06, 0xC0, 0xFF};
constexpr uintptr_t kGrabHelperBodyFunction = 0x00561690;

// call FUN_00873200, the main world draw (thiscall, four arguments), from the
// two variants of the composite render function.
struct WorldDrawSite {
    uintptr_t address;
    uint8_t bytes[5];
};
constexpr WorldDrawSite kWorldDrawSites[] = {
    {0x00870E18, {0xE8, 0xE3, 0x23, 0x00, 0x00}},
    {0x00870AE8, {0xE8, 0x13, 0x27, 0x00, 0x00}},
};
constexpr size_t kWorldDrawSiteCount = sizeof(kWorldDrawSites) / sizeof(kWorldDrawSites[0]);
constexpr uintptr_t kWorldDrawFunction = 0x00873200;

// The render-frame function's calls of the two composite render functions
// that draw the world (thiscall, two arguments). The first-person arms are
// drawn somewhere inside them.
struct CompositeSite {
    uintptr_t address;
    uint8_t bytes[5];
    uintptr_t target;
};
constexpr CompositeSite kCompositeSites[] = {
    {0x0087074B, {0xE8, 0xB0, 0x02, 0x00, 0x00}, 0x00870A00},
    {0x0087075E, {0xE8, 0x6D, 0x04, 0x00, 0x00}, 0x00870BD0},
};
constexpr size_t kCompositeSiteCount = sizeof(kCompositeSites) / sizeof(kCompositeSites[0]);

// The functions that draw the first-person node, with the scene camera itself
// (FUN_006629F0), after the main world draw has been given its camera and had
// it put back. FUN_00875110 (thiscall, four arguments) is the one of ordinary
// play, called from the two hooked composites; FUN_008746D0 (thiscall, one
// argument) runs instead of nothing only while an image-space effect is on.
struct FirstPersonPassSite {
    uintptr_t address;
    uint8_t bytes[5];
    uintptr_t target;
};
constexpr FirstPersonPassSite kFirstPersonPassSites[] = {
    {0x00870ED0, {0xE8, 0xFB, 0x37, 0x00, 0x00}, 0x008746D0},
    {0x00870B21, {0xE8, 0xEA, 0x45, 0x00, 0x00}, 0x00875110},
    {0x00870F74, {0xE8, 0x97, 0x41, 0x00, 0x00}, 0x00875110},
};
constexpr size_t kFirstPersonPassSiteCount =
    sizeof(kFirstPersonPassSites) / sizeof(kFirstPersonPassSites[0]);

// VATS camera data (xNVSE, JIP): mode at +0x08, 4 while VATS plays back,
// which includes the kill camera. FUN_009C8CC0 (the VATS time multiplier)
// tests it the same way.
constexpr uintptr_t kVatsCameraData = 0x011F2250;
constexpr uintptr_t kVatsCameraMode = 0x08;
constexpr uint32_t kVatsModePlayback = 4;
constexpr uintptr_t kNodeName = 0x08;
constexpr uintptr_t kNodeLocalTransform = 0x34;
constexpr uintptr_t kPlayerFirstPersonRoot = 0x694;

// Field of view. The player keeps three values (+0x670, +0x674, +0x678) that
// change on ticks while aiming. They reach the cameras through two functions,
// called in pairs from many places every frame: FUN_00C52020 (thiscall on a
// scene graph: field of view and three more arguments) and FUN_00B54000
// (cdecl, field of view first). Every call site of both is redirected; a
// value equal to one of the player's three is replaced by its blended value.
// The fields themselves are not touched.
constexpr uintptr_t kSetFovScene = 0x00C52020;
constexpr uintptr_t kSetFovGlobal = 0x00B54000;
constexpr uintptr_t kPlayerFov = 0x670;
struct FovSite {
    uintptr_t address;
    uint8_t bytes[5];
};
constexpr FovSite kFovSceneSites[] = {
    {0x005C595C, {0xE8, 0xBF, 0xC6, 0x68, 0x00}},
    {0x0071091F, {0xE8, 0xFC, 0x16, 0x54, 0x00}},
    {0x00713370, {0xE8, 0xAB, 0xEC, 0x53, 0x00}},
    {0x0086EB8B, {0xE8, 0x90, 0x34, 0x3E, 0x00}},
    {0x00872B6B, {0xE8, 0xB0, 0xF4, 0x3D, 0x00}},
    {0x008747DF, {0xE8, 0x3C, 0xD8, 0x3D, 0x00}},
    {0x0087486F, {0xE8, 0xAC, 0xD7, 0x3D, 0x00}},
    {0x00874D28, {0xE8, 0xF3, 0xD2, 0x3D, 0x00}},
    {0x008752BA, {0xE8, 0x61, 0xCD, 0x3D, 0x00}},
    {0x008759F4, {0xE8, 0x27, 0xC6, 0x3D, 0x00}},
    {0x00875B92, {0xE8, 0x89, 0xC4, 0x3D, 0x00}},
    {0x00875E97, {0xE8, 0x84, 0xC1, 0x3D, 0x00}},
    {0x00875F0F, {0xE8, 0x0C, 0xC1, 0x3D, 0x00}},
    {0x0094AF2E, {0xE8, 0xED, 0x70, 0x30, 0x00}},
    {0x0094B4BE, {0xE8, 0x5D, 0x6B, 0x30, 0x00}},
    {0x0095063D, {0xE8, 0xDE, 0x19, 0x30, 0x00}},
    {0x00C51D67, {0xE8, 0xB4, 0x02, 0x00, 0x00}},
    {0x00C5257F, {0xE8, 0x9C, 0xFA, 0xFF, 0xFF}},
};
constexpr FovSite kFovGlobalSites[] = {
    {0x005C5968, {0xE8, 0x93, 0xE6, 0x58, 0x00}},
    {0x0086EB9F, {0xE8, 0x5C, 0x54, 0x2E, 0x00}},
    {0x00872B7F, {0xE8, 0x7C, 0x14, 0x2E, 0x00}},
    {0x008747F3, {0xE8, 0x08, 0xF8, 0x2D, 0x00}},
    {0x00874883, {0xE8, 0x78, 0xF7, 0x2D, 0x00}},
    {0x008752CE, {0xE8, 0x2D, 0xED, 0x2D, 0x00}},
    {0x00875A08, {0xE8, 0xF3, 0xE5, 0x2D, 0x00}},
    {0x00875BA6, {0xE8, 0x55, 0xE4, 0x2D, 0x00}},
    {0x00875EAB, {0xE8, 0x50, 0xE1, 0x2D, 0x00}},
    {0x0094AF43, {0xE8, 0xB8, 0x90, 0x20, 0x00}},
    {0x0094B4D3, {0xE8, 0x28, 0x8B, 0x20, 0x00}},
    {0x00950649, {0xE8, 0xB2, 0x39, 0x20, 0x00}},
    {0x00B545CB, {0xE8, 0x30, 0xFA, 0xFF, 0xFF}},
};

// Process manager: object array data at +0x08, its used length at +0x0E, and
// the high-process range [+0x14, +0x24). Same layout as Fallout 3 (xNVSE and
// JIP agree) and proved at run time: every object in the range has to have a
// process (+0x68) whose level (+0x28) is 0, high. The player is not in it.
constexpr uintptr_t kProcessManager = 0x011E0E80;

// Loaded cells: [kTesPointer] + 0x08 is the grid; its size is at +0x0C and
// its array of size * size cell pointers at +0x10. A cell's references are a
// singly linked list of {item, next} at +0xAC; a reference's cell is at
// +0x40. From the xNVSE and JIP headers, proved at run time before use.
constexpr uintptr_t kTesPointer = 0x011DEA10;
constexpr uintptr_t kCellReferenceList = 0xAC;
constexpr uintptr_t kReferenceParentCell = 0x40;
constexpr uintptr_t kNodeController = 0x0C;

constexpr uintptr_t kSceneGraphPointer = 0x011DEB7C; // +0xAC main camera
constexpr uintptr_t kSceneGraphCamera = 0xAC;
constexpr uintptr_t kNodeWorldTranslation = 0x8C;
// Scene node: parent, world transform (3x3 rotation, translation, scale: 13
// floats), child array pointer and its used length.
constexpr uintptr_t kNodeParent = 0x18;
constexpr uintptr_t kNodeWorldTransform = 0x68;
constexpr uintptr_t kNodeChildren = 0xA0;
constexpr uintptr_t kNodeChildCount = 0xA6;
// NiObject vtable slot 3 is this function for every node class and another
// one (returning 0) for everything else.
constexpr uintptr_t kIsNodeFunction = 0x006815C0;
constexpr uintptr_t kSkyPointer = 0x011DEA20;        // node roots at +0x04, +0x08
constexpr uintptr_t kReferenceRenderState = 0x64;    // -> +0x14 root node
constexpr uintptr_t kPlayerNotFirstPerson = 0x64A;
constexpr uintptr_t kPlayerThirdPerson = 0x64C;
constexpr uintptr_t kReferencePosition = 0x30;

constexpr uintptr_t kMainObjectPointer = 0x011DEA0C; // +6 fly-cam, +7 freeze time
constexpr uintptr_t kPlayerPointer = 0x011DEA3C;     // +0x40 parent cell
constexpr uintptr_t kInputPointer = 0x011F35CC;
// Input object: current and previous keys, current and previous buttons.
constexpr uintptr_t kInputKeys = 0x18F8;
constexpr uintptr_t kInputPreviousKeys = 0x19F8;
constexpr uintptr_t kInputButtons = 0x1B30;
constexpr uintptr_t kInputPreviousButtons = 0x1B44;

// The four [Controls] fForegroundMouse* settings (value at +4 of each
// object). The menu cursor's speed is multiplied by a term built from them
// that grows with the size of the mouse movement.
constexpr uintptr_t kCursorSettings[4] = {0x011F35F4, 0x011F3600, 0x011F360C, 0x011F3618};

constexpr uintptr_t kMainTimer = 0x011F6394;
constexpr uintptr_t kHavokMaxTime = 0x01267B38;
constexpr uintptr_t kTimeMultiplier = 0x011AC3A0;
// Recomputed by the per-frame function and tested throughout it. Set in the
// main menu, in conversations and other menus; clear in ordinary gameplay.
constexpr uintptr_t kFrameFlagByte = 0x011DEA2B;

struct BSTimer {
    uint8_t disableCounter;
    uint8_t padding01[3];
    float clampMs;
    float clampRemainderMs;
    float deltaSeconds;
    float realDeltaSeconds;
    uint32_t lastTimeMs;
    uint32_t firstTimeMs;
    uint8_t flag1C;
    uint8_t sameFrameDelta;
    uint8_t padding1E[2];
};
static_assert(sizeof(BSTimer) == 0x20);

// What decides whether the patch may be installed is the executable's layout,
// not its identity: every hook site is checked against the exact call bytes
// expected there (which also fixes the address of the function it calls), and
// the fixed data addresses the patch reads are checked here, each through
// two instructions of the game that reference it.
struct LayoutAnchor {
    uintptr_t address;
    uint8_t length;
    uint8_t bytes[6];
    const char *what;
};
constexpr LayoutAnchor kLayoutAnchors[] = {
    {0x0086F277, 5, {0xB9, 0x94, 0x63, 0x1F, 0x01, 0x00}, "main timer"},
    {0x0086F291, 5, {0xB9, 0x94, 0x63, 0x1F, 0x01, 0x00}, "main timer"},
    {0x00C66844, 6, {0xD9, 0x05, 0x38, 0x7B, 0x26, 0x01}, "Havok maximum step"},
    {0x00C66984, 6, {0xD8, 0x35, 0x38, 0x7B, 0x26, 0x01}, "Havok maximum step"},
    {0x00AA5020, 6, {0xD8, 0x0D, 0xA0, 0xC3, 0x1A, 0x01}, "global time multiplier"},
    {0x00AA4E43, 6, {0xD9, 0x05, 0xA0, 0xC3, 0x1A, 0x01}, "global time multiplier"},
    {0x0086B3DD, 6, {0x8B, 0x0D, 0x0C, 0xEA, 0x1D, 0x01}, "main object pointer"},
    {0x00440389, 6, {0x8B, 0x0D, 0x0C, 0xEA, 0x1D, 0x01}, "main object pointer"},
    {0x0086F281, 7 - 1, {0x0F, 0xB6, 0x0D, 0x2B, 0xEA, 0x1D}, "frame flag byte"},
    {0x0086F2E5, 7 - 1, {0x0F, 0xB6, 0x15, 0x2B, 0xEA, 0x1D}, "frame flag byte"},
    {0x0086F3CA, 6, {0x8B, 0x15, 0x3C, 0xEA, 0x1D, 0x01}, "player pointer"},
    {0x0070C917, 6, {0x8B, 0x0D, 0x3C, 0xEA, 0x1D, 0x01}, "player pointer"},
    {0x0086E77F, 4, {0x0F, 0xB6, 0x51, 0x07, 0x00, 0x00}, "freeze-time flag test"},
    {0x0086E91D, 4, {0x0F, 0xB6, 0x51, 0x07, 0x00, 0x00}, "freeze-time flag test"},
    {0x00A23A06, 6, {0x8B, 0x81, 0x24, 0x1B, 0x00, 0x00}, "input mouse state offset"},
    {0x00711D87, 5, {0xB9, 0xF4, 0x35, 0x1F, 0x01, 0x00}, "cursor setting"},
    {0x00711DA7, 5, {0xB9, 0x00, 0x36, 0x1F, 0x01, 0x00}, "cursor setting"},
    {0x00711DC7, 5, {0xB9, 0x0C, 0x36, 0x1F, 0x01, 0x00}, "cursor setting"},
    {0x00711DE7, 5, {0xB9, 0x18, 0x36, 0x1F, 0x01, 0x00}, "cursor setting"},
    {0x0045C673, 5, {0xB9, 0x7C, 0xEB, 0x1D, 0x01, 0x00}, "scene graph pointer"},
    {0x00524C93, 5, {0xB9, 0x7C, 0xEB, 0x1D, 0x01, 0x00}, "scene graph pointer"},
    {0x006629FA, 6, {0x81, 0xC1, 0xAC, 0x00, 0x00, 0x00}, "scene graph camera offset"},
    {0x006815C3, 6, {0x51, 0x89, 0x4D, 0xFC, 0x8B, 0x45}, "is-node function"},
    {0x0086CFAA, 6, {0x8B, 0x0D, 0x20, 0xEA, 0x1D, 0x01}, "sky pointer"},
    {0x0086D025, 5, {0xA1, 0x20, 0xEA, 0x1D, 0x01, 0x00}, "sky pointer"},
    {0x00429755, 5, {0xB9, 0x80, 0x0E, 0x1E, 0x01, 0x00}, "process manager"},
    {0x00436A6E, 5, {0xB9, 0x80, 0x0E, 0x1E, 0x01, 0x00}, "process manager"},
    {0x0086E8CD, 6, {0x8B, 0x0D, 0x10, 0xEA, 0x1D, 0x01}, "TES pointer"},
    {0x00870FED, 6, {0x8B, 0x0D, 0x10, 0xEA, 0x1D, 0x01}, "TES pointer"},
    {0x00498E06, 5, {0xB9, 0x50, 0x22, 0x1F, 0x01, 0x00}, "VATS camera data"},
    {0x0052422E, 5, {0xB9, 0x50, 0x22, 0x1F, 0x01, 0x00}, "VATS camera data"},
    {0x009C8CD1, 4, {0x83, 0x78, 0x08, 0x04, 0x00, 0x00}, "VATS camera mode test"},
};

// ---- State -----------------------------------------------------------------

HINSTANCE g_self = nullptr;
INIT_ONCE g_patchInitOnce = INIT_ONCE_STATIC_INIT;
std::atomic<bool> g_stop{false};
HANDLE g_logThread = nullptr;
LARGE_INTEGER g_qpcFrequency{};

bool g_enabled = true;
bool g_telemetryEnabled = false;
bool g_fixedTickEnabled = true;
bool g_noTimerSleepsEnabled = true;
bool g_freezeFramesEnabled = true;
bool g_alignHavokStepEnabled = true;
bool g_latchInputEnabled = true;
bool g_lookInputRealDeltaEnabled = true;
bool g_fixedGrabForceEnabled = true;
bool g_menuMouseNoAcceleration = true;
bool g_interpolateCameraEnabled = true;
bool g_shiftSkyEnabled = true;
bool g_thirdPersonCameraEnabled = true;
bool g_thirdPersonBodyEnabled = true;
bool g_firstPersonEnabled = true;
bool g_interpolateFovEnabled = true;
bool g_interpolateActorsEnabled = true;
bool g_interpolateObjectsEnabled = true;
bool g_menuRealTimeEnabled = true;
bool g_firstPersonPassCameraEnabled = true;
bool g_firstPersonWorldEnabled = true;
bool g_killCamCameraEnabled = true;
bool g_lowRateRulesEnabled = true;
bool g_latePlayerPositionEnabled = true;
bool g_killCamTickPosesEnabled = true;
double g_simulationStepMs = 1000.0 / 60.0;
double g_maxWallDeltaMs = 250.0;

// A non-zero clamp selects the timer's explicit-step path, which never
// sleeps. Exactly 0 would select the path that does.
constexpr float kNearZeroClampMs = 0.000001f;

// Main thread only. The frame hook owns the accumulator and decides whether
// the frame is a tick; the timer hook applies the decision.
double g_accumulatorMs = 0.0;
int64_t g_tickLastQpc = 0;
int g_frameTickDecision = -1; // -1 outside a frame, else 0 or 1
// A frame that cannot be followed by frozen ones (menu, conversation,
// loading, and the warm-up after them) is given the real frame time instead
// of a tick or almost nothing. With quantised stepping such frames run the
// whole update with almost no time, and poses then arrive late and more
// than once per tick, which cannot be interpolated.
bool g_frameNativeStep = false;
double g_frameNativeDeltaMs = 0.0;
double g_frameWallDeltaMs = 0.0;
DWORD g_mainThreadId = 0;
bool g_frameEligible = false;  // ordinary gameplay: the frame may be frozen
int g_eligibleFrames = 0;
// Frames per tick, smoothed over about ten ticks. At up to 2.5 (about 150
// FPS) there are tick periods with no draw in them that is neither a tick
// draw nor the draw after one, and two ticks often fall on consecutive
// draws; the classifiers then use the rules of ClassifyChange. Above it,
// or with bLowRateRules=0, every decision is the one 1.0.1 made.
constexpr double kFewDrawsPerTick = 2.5;
double g_drawsPerTick = 6.0;
uint32_t g_drawsSinceTick = 0;
bool g_fewDrawsPerTick = false;
constexpr int kFreezeWarmUpFrames = 30;
float g_havokMaxTimeOriginal = 0.0f;
bool g_havokMaxTimeOverridden = false;

// Key and button state as the player's controls last saw it on a tick.
struct InputLatch {
    bool valid;
    uint8_t keys[256];
    uint8_t buttons[8];
    uint8_t pendingKeys[256];
    uint8_t pendingButtons[8];
};
InputLatch g_inputLatch{};

// Set once hooks are in; cleared if the first-frame check finds that
// something else has patched the same sites since.
std::atomic<bool> g_hooksInstalled{false};
std::atomic<bool> g_firstFrameChecked{false};
uint8_t g_frameCallWritten[5]{};
uint8_t g_timerCallWritten[5]{};
uint8_t g_inputPollWritten[5]{};
uint8_t g_worldDrawWritten[kWorldDrawSiteCount][5]{};
uint8_t g_compositeWritten[kCompositeSiteCount][5]{};
bool g_compositeInstalled[kCompositeSiteCount]{};
uint8_t g_fpPassWritten[kFirstPersonPassSiteCount][5]{};
bool g_fpPassSiteInstalled[kFirstPersonPassSiteCount]{};
bool g_fpPassInstalled = false; // every site
bool g_worldDrawInstalled[kWorldDrawSiteCount]{};
bool g_inputPollInstalled = false;
uint8_t g_auxTimerWritten[kAuxTimerSiteCount][5]{};
bool g_auxTimerInstalled[kAuxTimerSiteCount]{};

// Written by the game's main thread, read by the log thread. Each field is
// an independent counter; a status line does not need them to be mutually
// consistent to the frame.
struct Counters {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> timerUpdates{0};
    std::atomic<int64_t> wallTicks{0};         // QPC ticks between frame starts
    std::atomic<int64_t> timerInsideTicks{0};  // QPC ticks spent in the stock timer update
    std::atomic<int64_t> engineMicros{0};      // sum of the timer's delta
    std::atomic<int64_t> realMicros{0};        // sum of the timer's unscaled delta
    std::atomic<uint32_t> maxFrameMicros{0};
    std::atomic<uint32_t> minDeltaMicros{0xFFFFFFFFu};
    std::atomic<uint32_t> maxDeltaMicros{0};
    std::atomic<uint64_t> flagFrames{0};
    std::atomic<uint64_t> slowTimerUpdates{0}; // stock update took over 0.5 ms
    std::atomic<uint64_t> ticks{0};
    std::atomic<uint64_t> nativeFrames{0};        // frames given the real frame time
    std::atomic<uint64_t> passThroughUpdates{0}; // main timer updated outside a decided frame
    std::atomic<uint64_t> auxCalls[kAuxTimerSiteCount]{};
    std::atomic<uint64_t> auxNoSleep[kAuxTimerSiteCount]{};
    std::atomic<int32_t> accumulatorMicros{0};
    std::atomic<uint64_t> frozenFrames{0};
    std::atomic<uint64_t> ineligibleFrames{0};
    std::atomic<uint64_t> warmUpFrames{0};
    std::atomic<uint64_t> latchPolls{0};
    std::atomic<uint64_t> latchEdgesDeferred{0};
    std::atomic<uint64_t> latchEdgesDelivered{0};
    std::atomic<uint64_t> latchTapsRecovered{0};
    std::atomic<uint64_t> lookInputCalls{0};
    std::atomic<uint64_t> lookInputRealDelta{0}; // given the frame's real time
    std::atomic<uint64_t> grabHelperApplied{0};
    std::atomic<uint64_t> grabHelperSkipped{0};  // frame between ticks: not pushed
    std::atomic<uint64_t> faults{0};
    std::atomic<uint64_t> draws{0};
    std::atomic<uint64_t> drawsOutsideFrame{0};
    std::atomic<uint64_t> drawsOtherThread{0};
    std::atomic<uint64_t> drawsIneligible{0};
    std::atomic<uint64_t> cameraApplied{0};
    std::atomic<uint64_t> cameraRestoreConflicts{0};
    std::atomic<uint64_t> playerPosesOnTick{0};    // player position changed on a tick draw
    std::atomic<uint64_t> playerPosesOffTick{0};   // ... on any other draw
    std::atomic<uint64_t> cameraMovesOnTick{0};    // camera position changed on a tick draw
    std::atomic<uint64_t> cameraMovesOffTick{0};
    std::atomic<uint64_t> snaps{0};                // step above the teleport limit
    std::atomic<uint32_t> maxCorrectionMilli{0};   // largest correction, thousandths of a unit
    std::atomic<uint64_t> skyNodesWritten{0};
    std::atomic<uint64_t> skyRestoreConflicts{0};
    std::atomic<uint64_t> thirdPersonDraws{0};
    std::atomic<uint64_t> thirdPersonSteps{0};     // offset number stepped on a tick
    std::atomic<uint64_t> thirdPersonOffTick{0};   // ... changed between ticks
    std::atomic<uint64_t> bodyNodesWritten{0};
    std::atomic<uint64_t> bodyBlends{0};
    std::atomic<uint64_t> bodyOffTick{0};
    std::atomic<uint64_t> bodyMinor{0};
    std::atomic<uint64_t> bodyChanges[4]{};        // by draw kind: tick, after tick, both, neither
    std::atomic<uint64_t> bodyRestoreConflicts{0};
    std::atomic<uint64_t> bodyRootMismatch{0};
    std::atomic<uint64_t> fpRenders{0};            // renders with the first-person step active
    std::atomic<uint64_t> fpNodes{0};              // nodes visited
    std::atomic<uint64_t> fpChanges[4]{};          // local changes by frame kind
    std::atomic<uint64_t> fpBlends{0};
    std::atomic<uint64_t> fpLateBlends{0};
    std::atomic<uint64_t> fpPerFrame{0};
    std::atomic<uint64_t> fpWritten{0};
    std::atomic<uint64_t> fpRestoreConflicts{0};
    std::atomic<uint64_t> fpWorldRecomputed{0};    // a blended node's world changed during the render
    std::atomic<uint64_t> fpWorldUnchanged{0};
    std::atomic<uint64_t> fpNoCameraBone{0};
    std::atomic<uint64_t> fpBoneWrites{0};
    std::atomic<uint64_t> boneDeltaApplied{0};
    std::atomic<uint64_t> boneDeltaMismatch{0};    // scene camera is not on the bone
    std::atomic<uint32_t> boneMismatchMilli{0};    // largest distance seen, thousandths
    std::atomic<uint64_t> cameraRotationWrites{0};
    std::atomic<uint64_t> cameraRelationChanges{0};
    std::atomic<uint64_t> cameraRotationConflicts{0};
    // World transforms below the first-person root, worked out from the
    // blended locals for nodes whose world the engine had last computed from
    // its own locals.
    std::atomic<uint64_t> fpWorldWrites{0};
    std::atomic<uint64_t> fpWorldSkipped{0};     // world not the engine's own: left alone
    std::atomic<uint32_t> fpWorldSkippedMilli{0}; // largest distance of a skipped one, thousandths
    std::atomic<uint64_t> fpWorldAdjusted{0};    // engine adjustment carried onto the blend
    std::atomic<uint64_t> fpWorldKept{0};        // still ours after the render: the render used it
    std::atomic<uint64_t> fpWorldOverwritten{0}; // the render recomputed it itself
    std::atomic<uint64_t> fpPassCameraWrites{0}; // first-person pass given the blended camera
    std::atomic<uint64_t> fpPassCameraSkipped{0}; // camera not as left by the main draw
    std::atomic<uint64_t> fpPassCameraConflicts{0};
    std::atomic<uint64_t> fpPassCameraUpdated{0}; // the pass recomputed the camera from its local
    std::atomic<uint64_t> killCamDraws{0};       // eligible draws in VATS playback
    std::atomic<uint64_t> killCamFirstPerson{0};
    std::atomic<uint64_t> killCamIneligible{0};  // VATS playback on a frame that is not frozen
    std::atomic<uint64_t> killCamChanges[4]{};   // camera changes by draw kind
    std::atomic<uint64_t> killCamLate{0};        // change first seen on the draw after a tick
    std::atomic<uint64_t> killCamAsIs{0};        // changed off a tick, or driven every frame
    std::atomic<uint64_t> killCamSnaps{0};       // cut: shown at once
    std::atomic<uint64_t> killCamWritten{0};
    std::atomic<uint64_t> killCamMinor{0};        // small change away from a tick, let through
    std::atomic<uint32_t> killCamTickMoveMilli{0}; // largest change near a tick, thousandths
    std::atomic<uint32_t> killCamTickTurnMicro{0};
    std::atomic<uint32_t> killCamOffMoveMilli{0};  // ... on other draws
    std::atomic<uint32_t> killCamOffTurnMicro{0};
    std::atomic<uint64_t> fovCalls{0};
    std::atomic<uint64_t> fovSteps{0};          // a value stepped on a tick
    std::atomic<uint64_t> fovAsIs{0};           // changed off a tick or jumped
    std::atomic<uint64_t> fovSubstitutions{0};
    std::atomic<uint32_t> fovSites{0};
    std::atomic<uint64_t> actorPasses{0};
    std::atomic<uint64_t> actorListRejected{0};  // range not plausible
    std::atomic<uint64_t> actorNoPlayer{0};      // an object in the range is not high-process
    std::atomic<uint64_t> actorsTracked{0};
    std::atomic<uint64_t> actorNodesWritten{0};
    std::atomic<uint64_t> actorLag[4]{};         // pose accepted 0, 1, 2, 3 draws after its tick
    std::atomic<uint64_t> actorMinor{0};
    std::atomic<uint64_t> actorAsIs{0};          // moved away from a tick, or driven every frame
    std::atomic<uint64_t> actorSnaps{0};
    std::atomic<uint64_t> actorDuplicates{0};
    std::atomic<uint64_t> actorRestoreConflicts{0};
    std::atomic<uint64_t> actorTableResets{0};
    std::atomic<uint64_t> actorOverflow{0};
    std::atomic<uint64_t> objectWalks{0};
    std::atomic<uint64_t> objectListRejected{0};  // the player's cell failed the list proof
    std::atomic<uint64_t> objectGridRejected{0};  // the grid failed its proof
    std::atomic<uint64_t> objectCellsRead{0};
    std::atomic<uint64_t> objectReferences{0};
    std::atomic<uint64_t> objectChanges[4]{};     // root moved, by draw kind
    std::atomic<uint64_t> objectSubtrees{0};      // active roots handed to the pose pass
    std::atomic<uint64_t> objectTableFull{0};
    std::atomic<uint32_t> objectCells{0};         // cells in the last walk
    std::atomic<uint32_t> objectActiveMax{0};
    std::atomic<uint32_t> objectControlled{0};
    // Few draws per tick (see ClassifyChange).
    std::atomic<uint32_t> drawsPerTickCenti{0};
    std::atomic<uint64_t> fewDrawFrames{0};
    std::atomic<uint64_t> fpMixed{0};             // changed on time and late within two draws
    std::atomic<uint64_t> fpLateAfterTick{0};     // late pose seen on a tick draw after a tick draw
    std::atomic<uint64_t> bodyMixed{0};
    std::atomic<uint64_t> bodyLateAfterTick{0};
    std::atomic<uint64_t> offsetMixed{0};
    std::atomic<uint64_t> killCamTickPoses{0};    // kill camera blended between tick draws' poses
    std::atomic<uint64_t> fovLateAfterTick{0};
    std::atomic<uint64_t> cameraRelationDrift{0}; // relation moved a little: rotation still written
    std::atomic<uint32_t> cameraRelationMicro{0}; // largest relation change, millionths
    std::atomic<uint64_t> positionChanges[4]{};    // player position changes by draw kind
    std::atomic<uint64_t> positionLate{0};         // ... taken as a draw late
    // Kill camera changes by draws since the tick draw (0 to 3, 4 or more),
    // all and those beyond the small-change limits.
    std::atomic<uint64_t> killCamAfterTick[5]{};
    std::atomic<uint64_t> killCamLargeAfterTick[5]{};
};
Counters g_counters;
int64_t g_lastFrameQpc = 0;

// ---- Small helpers ---------------------------------------------------------

std::wstring ModuleSiblingPath(const wchar_t *fileName) {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring result(path, length);
    const size_t slash = result.find_last_of(L"\\/");
    result.resize(slash == std::wstring::npos ? 0 : slash + 1);
    result += fileName;
    return result;
}

std::wstring GameDirectoryPath(const wchar_t *relative) {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path, length);
    const size_t slash = result.find_last_of(L"\\/");
    result.resize(slash == std::wstring::npos ? 0 : slash + 1);
    result += relative;
    return result;
}

bool FileExists(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

SRWLOCK g_logLock = SRWLOCK_INIT;

void AppendStatus(const char *format, ...) {
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    const int messageLength = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (messageLength < 0) return;

    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[1100];
    const int lineLength = snprintf(
        line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n", now.wYear, now.wMonth,
        now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
    if (lineLength <= 0) return;

    AcquireSRWLockExclusive(&g_logLock);
    const std::wstring path = ModuleSiblingPath(L"FalloutNVHFR.log");
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(lineLength), &written, nullptr);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

int64_t QpcNow() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

double QpcToMs(int64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_qpcFrequency.QuadPart);
}

bool IsReadable(uintptr_t address, size_t length) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void *>(address), &info, sizeof(info)) == 0) {
        return false;
    }
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address + length <= regionEnd;
}

bool MatchesBytes(uintptr_t address, const uint8_t *expected, size_t length) {
    return IsReadable(address, length) &&
           memcmp(reinterpret_cast<const void *>(address), expected, length) == 0;
}

bool WriteBytes(uintptr_t address, const uint8_t *bytes, size_t length) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void *>(address), length, PAGE_EXECUTE_READWRITE,
                        &oldProtect)) {
        return false;
    }
    memcpy(reinterpret_cast<void *>(address), bytes, length);
    VirtualProtect(reinterpret_cast<void *>(address), length, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void *>(address), length);
    return true;
}

// Redirects a 5-byte E8 call. Only whole call instructions are ever patched.
bool PatchRelativeCall(uintptr_t site, const void *target, const uint8_t expected[5],
                       uint8_t written[5]) {
    if (!MatchesBytes(site, expected, 5)) return false;
    const int32_t relative =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    written[0] = 0xE8;
    memcpy(&written[1], &relative, 4);
    return WriteBytes(site, written, 5);
}

void RestoreBytes(uintptr_t site, const uint8_t expected[5]) { WriteBytes(site, expected, 5); }

void AtomicMax(std::atomic<uint32_t> &target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

void AtomicMin(std::atomic<uint32_t> &target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value < current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

// ---- Hooks -----------------------------------------------------------------

using ThisCallNoArgs = void(__thiscall *)(void *);
bool g_firstFrameIntact = false;

// The one check that runs after installation: on the first frame, see whether
// the redirected sites still hold this plugin's bytes. A mod loaded later may
// have patched the same sites; if so this plugin stops acting and says so.
void FirstFrameCheck() {
    const bool frameIntact = MatchesBytes(kFrameCall, g_frameCallWritten, 5);
    const bool timerIntact = MatchesBytes(kTimerCall, g_timerCallWritten, 5);
    const bool nvsePresent = GetModuleHandleW(L"nvse_1_4.dll") != nullptr;
    bool auxIntact = true;
    for (size_t i = 0; i < kAuxTimerSiteCount; ++i) {
        if (g_auxTimerInstalled[i] &&
            !MatchesBytes(kAuxTimerSites[i].address, g_auxTimerWritten[i], 5)) {
            auxIntact = false;
        }
    }
    if (g_inputPollInstalled && !MatchesBytes(kInputPollCall, g_inputPollWritten, 5)) {
        auxIntact = false;
    }
    for (size_t i = 0; i < kWorldDrawSiteCount; ++i) {
        if (g_worldDrawInstalled[i] &&
            !MatchesBytes(kWorldDrawSites[i].address, g_worldDrawWritten[i], 5)) {
            auxIntact = false;
        }
    }
    for (size_t i = 0; i < kCompositeSiteCount; ++i) {
        if (g_compositeInstalled[i] &&
            !MatchesBytes(kCompositeSites[i].address, g_compositeWritten[i], 5)) {
            auxIntact = false;
        }
    }
    for (size_t i = 0; i < kFirstPersonPassSiteCount; ++i) {
        if (g_fpPassSiteInstalled[i] &&
            !MatchesBytes(kFirstPersonPassSites[i].address, g_fpPassWritten[i], 5)) {
            auxIntact = false;
        }
    }
    if (!frameIntact || !timerIntact || !auxIntact) {
        g_hooksInstalled.store(false, std::memory_order_release);
        AppendStatus("first frame: hook sites were patched by something else after "
                     "installation (frame call intact=%d, timer call intact=%d, other timer calls "
                     "intact=%d, nvse=%d); this plugin is now inactive",
                     frameIntact ? 1 : 0, timerIntact ? 1 : 0, auxIntact ? 1 : 0,
                     nvsePresent ? 1 : 0);
        return;
    }
    AppendStatus("first frame: hook sites intact, nvse=%d", nvsePresent ? 1 : 0);
    g_firstFrameIntact = true;
}

void NoteDrawsPerTick(bool tick) {
    ++g_drawsSinceTick;
    if (!tick) return;
    g_drawsPerTick += (static_cast<double>(g_drawsSinceTick) - g_drawsPerTick) * 0.1;
    g_drawsSinceTick = 0;
    g_fewDrawsPerTick = g_lowRateRulesEnabled && g_drawsPerTick <= kFewDrawsPerTick;
    g_counters.drawsPerTickCenti.store(static_cast<uint32_t>(g_drawsPerTick * 100.0),
                                       std::memory_order_relaxed);
}

// Advances the accumulator by the wall time since the last frame and decides
// whether this frame is a tick. At most one tick per frame, and the backlog is
// bounded, so the engine is never stepped more often than the tick rate.
void BeginTickFrame() {
    g_frameTickDecision = -1;
    g_frameNativeStep = false;
    if (!g_fixedTickEnabled || !g_hooksInstalled.load(std::memory_order_relaxed)) {
        g_tickLastQpc = 0;
        return;
    }
    const int64_t now = QpcNow();
    double wallDeltaMs = g_tickLastQpc != 0 ? QpcToMs(now - g_tickLastQpc) : 0.0;
    g_tickLastQpc = now;
    if (wallDeltaMs < 0.0) wallDeltaMs = 0.0;
    if (wallDeltaMs > g_maxWallDeltaMs) wallDeltaMs = g_maxWallDeltaMs;
    g_frameWallDeltaMs = wallDeltaMs;
    g_accumulatorMs += wallDeltaMs;
    const bool tick = g_accumulatorMs >= g_simulationStepMs;
    if (tick) {
        g_accumulatorMs -= g_simulationStepMs;
        // Below the tick rate the backlog would otherwise grow and be paid
        // back later as a burst of consecutive tick frames.
        if (g_accumulatorMs > g_simulationStepMs) g_accumulatorMs = g_simulationStepMs;
        g_counters.ticks.fetch_add(1, std::memory_order_relaxed);
    }
    NoteDrawsPerTick(tick);
    if (g_fewDrawsPerTick) g_counters.fewDrawFrames.fetch_add(1, std::memory_order_relaxed);
    g_frameTickDecision = tick ? 1 : 0;
    g_counters.accumulatorMicros.store(static_cast<int32_t>(g_accumulatorMs * 1000.0),
                                       std::memory_order_relaxed);

    // Code that runs before this frame's timer update reads the delta left by
    // the previous frame. Publish this frame's delta now so that it agrees
    // with the simulation on which frames are ticks.
    auto *timer = reinterpret_cast<BSTimer *>(kMainTimer);
    const float stepSeconds =
        static_cast<float>((tick ? g_simulationStepMs : kNearZeroClampMs) * 0.001);
    timer->realDeltaSeconds = stepSeconds;
    timer->deltaSeconds = stepSeconds * *reinterpret_cast<const float *>(kTimeMultiplier);
}

// Havok's 16 ms maximum step against a 16.667 ms tick leaves a remainder
// that is retired as extra partial steps on the frames between ticks.
void AlignHavokStep(bool active) {
    auto *maxTime = reinterpret_cast<float *>(kHavokMaxTime);
    if (active) {
        if (!g_havokMaxTimeOverridden) {
            g_havokMaxTimeOriginal = *maxTime;
            g_havokMaxTimeOverridden = true;
        }
        *maxTime = static_cast<float>(g_simulationStepMs / 1000.0);
    } else if (g_havokMaxTimeOverridden) {
        *maxTime = g_havokMaxTimeOriginal;
        g_havokMaxTimeOverridden = false;
    }
}

// Only ordinary in-world gameplay is frozen: never a menu or loading frame,
// a fly-cam session, a console freeze, or a frame without a loaded cell.
bool IsFrameEligible(void *mainObject) {
    bool eligible = false;
    __try {
        const auto *flags = static_cast<const uint8_t *>(mainObject);
        const uintptr_t player = *reinterpret_cast<const uintptr_t *>(kPlayerPointer);
        eligible = mainObject != nullptr &&
                   reinterpret_cast<uintptr_t>(mainObject) ==
                       *reinterpret_cast<const uintptr_t *>(kMainObjectPointer) &&
                   flags[6] == 0 && flags[7] == 0 &&
                   *reinterpret_cast<const uint8_t *>(kFrameFlagByte) == 0 &&
                   player >= 0x10000 &&
                   *reinterpret_cast<const uintptr_t *>(player + 0x40) >= 0x10000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        eligible = false;
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
    return eligible;
}

// Returns true when it set the engine's freeze-time flag for this frame. The
// stock frame then skips its world, actor and physics work but still reads
// the mouse, updates the camera and draws.
bool BeginFrozenFrame(void *mainObject) {
    g_frameEligible = false;
    if (g_frameTickDecision < 0) {
        g_eligibleFrames = 0;
        AlignHavokStep(false);
        return false;
    }
    g_frameEligible = IsFrameEligible(mainObject);
    const bool warmUp = g_frameEligible && g_eligibleFrames < kFreezeWarmUpFrames;
    if (g_menuRealTimeEnabled && g_freezeFramesEnabled && (!g_frameEligible || warmUp)) {
        // Real time on this frame: every such frame is a full step of its
        // own length, and the accumulator starts again afterwards.
        g_frameNativeStep = true;
        g_frameNativeDeltaMs = g_frameWallDeltaMs < 0.01
            ? 0.01 : (g_frameWallDeltaMs > 100.0 ? 100.0 : g_frameWallDeltaMs);
        g_accumulatorMs = 0.0;
        g_frameTickDecision = 1;
        g_counters.nativeFrames.fetch_add(1, std::memory_order_relaxed);
        auto *timer = reinterpret_cast<BSTimer *>(kMainTimer);
        const float stepSeconds = static_cast<float>(g_frameNativeDeltaMs * 0.001);
        timer->realDeltaSeconds = stepSeconds;
        timer->deltaSeconds = stepSeconds * *reinterpret_cast<const float *>(kTimeMultiplier);
    }
    // On a real-time frame Havok keeps its own step.
    AlignHavokStep(g_alignHavokStepEnabled && !g_frameNativeStep);
    if (!g_frameEligible) {
        g_eligibleFrames = 0;
        g_counters.ineligibleFrames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!g_freezeFramesEnabled) return false;
    if (warmUp) {
        ++g_eligibleFrames;
        g_counters.warmUpFrames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (g_frameTickDecision != 0) return false;
    static_cast<uint8_t *>(mainObject)[7] = 1;
    g_counters.frozenFrames.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// A press or release is visible to the player's controls for one frame,
// whichever frame that is; on a frozen frame it would be lost. This samples
// keys and buttons at ticks. On a frame between ticks both buffers show what
// the last tick saw (same held state, no edges); on a tick the previous
// buffers are set to that state, so every change since then shows as an edge.
// Held state is latched too: tap-versus-hold logic resets when it sees a key
// up without a release edge. Mouse movement is untouched. The next poll
// rewrites all four buffers.
void ApplyInputLatch(uint8_t *input, bool tick) {
    uint8_t *keys = input + kInputKeys;
    uint8_t *previousKeys = input + kInputPreviousKeys;
    uint8_t *buttons = input + kInputButtons;
    uint8_t *previousButtons = input + kInputPreviousButtons;
    InputLatch &latch = g_inputLatch;
    if (!latch.valid) {
        memcpy(latch.keys, keys, sizeof(latch.keys));
        memcpy(latch.buttons, buttons, sizeof(latch.buttons));
        memset(latch.pendingKeys, 0, sizeof(latch.pendingKeys));
        memset(latch.pendingButtons, 0, sizeof(latch.pendingButtons));
        latch.valid = true;
        return;
    }
    const auto step = [tick](uint8_t *current, uint8_t *previous, uint8_t *shown,
                             uint8_t *pending, size_t count) {
        for (size_t index = 0; index < count; ++index) {
            const bool down = (current[index] & 0x80) != 0;
            const bool shownDown = (shown[index] & 0x80) != 0;
            if (!tick) {
                if (down != shownDown) {
                    g_counters.latchEdgesDeferred.fetch_add(1, std::memory_order_relaxed);
                }
                if (down && !shownDown) pending[index] = 1;
                current[index] = shown[index];
                previous[index] = shown[index];
                continue;
            }
            if (!down && !shownDown && pending[index] != 0) {
                // Pressed and released between two ticks: show it as down for
                // this tick so the press is not lost; the release follows on
                // the next tick.
                current[index] = 0x80;
                g_counters.latchTapsRecovered.fetch_add(1, std::memory_order_relaxed);
            }
            pending[index] = 0;
            if (((current[index] ^ shown[index]) & 0x80) != 0) {
                g_counters.latchEdgesDelivered.fetch_add(1, std::memory_order_relaxed);
            }
            previous[index] = shown[index];
            shown[index] = current[index];
        }
    };
    step(keys, previousKeys, latch.keys, latch.pendingKeys, sizeof(latch.keys));
    step(buttons, previousButtons, latch.buttons, latch.pendingButtons, sizeof(latch.buttons));
}

// ---- Presentation for the main world draw -----------------------------------
//
// One rule for everything here: a value that changes on ticks is kept as a
// from/to pair, the draw is given the blend at alpha (how far the wall clock
// is into the current tick), and the engine's value is put back after the
// draw. Every write is saved with the bytes written; a restore that finds
// other bytes leaves them alone and counts a conflict.

constexpr size_t kPoseFloats = 13;
struct NodePose {
    float value[kPoseFloats];
};
struct SavedNodePose {
    uintptr_t node;
    NodePose pose;    // the engine's bytes
    NodePose written; // what was put there for the draw
};
struct SavedTranslation {
    uintptr_t node;
    float original[3];
    float written[3];
};

bool IsSceneNode(uintptr_t object) {
    const uintptr_t vtable = *reinterpret_cast<const uintptr_t *>(object);
    if (vtable < kImageBase || vtable >= kImageBase + 0x01000000) return false;
    return *reinterpret_cast<const uintptr_t *>(vtable + 0x0C) == kIsNodeFunction;
}

float DistanceSquared(const float *a, const float *b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

void MatrixToQuaternion(const float *m, double q[4]) {
    const double trace = static_cast<double>(m[0]) + m[4] + m[8];
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q[0] = 0.25 * s;
        q[1] = (m[7] - m[5]) / s;
        q[2] = (m[2] - m[6]) / s;
        q[3] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const double s = std::sqrt(1.0 + m[0] - m[4] - m[8]) * 2.0;
        q[0] = (m[7] - m[5]) / s;
        q[1] = 0.25 * s;
        q[2] = (m[1] + m[3]) / s;
        q[3] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const double s = std::sqrt(1.0 + m[4] - m[0] - m[8]) * 2.0;
        q[0] = (m[2] - m[6]) / s;
        q[1] = (m[1] + m[3]) / s;
        q[2] = 0.25 * s;
        q[3] = (m[5] + m[7]) / s;
    } else {
        const double s = std::sqrt(1.0 + m[8] - m[0] - m[4]) * 2.0;
        q[0] = (m[3] - m[1]) / s;
        q[1] = (m[2] + m[6]) / s;
        q[2] = (m[5] + m[7]) / s;
        q[3] = 0.25 * s;
    }
}

void QuaternionToMatrix(const double q[4], float *m) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    m[0] = static_cast<float>(1.0 - 2.0 * (y * y + z * z));
    m[1] = static_cast<float>(2.0 * (x * y - z * w));
    m[2] = static_cast<float>(2.0 * (x * z + y * w));
    m[3] = static_cast<float>(2.0 * (x * y + z * w));
    m[4] = static_cast<float>(1.0 - 2.0 * (x * x + z * z));
    m[5] = static_cast<float>(2.0 * (y * z - x * w));
    m[6] = static_cast<float>(2.0 * (x * z - y * w));
    m[7] = static_cast<float>(2.0 * (y * z + x * w));
    m[8] = static_cast<float>(1.0 - 2.0 * (x * x + y * y));
}

// Returns false when the result is not usable (non-finite or degenerate).
bool BlendPose(const NodePose &from, const NodePose &to, double alpha, NodePose &out) {
    out = to;
    if (memcmp(from.value, to.value, 9 * sizeof(float)) != 0) {
        double a[4], b[4];
        MatrixToQuaternion(from.value, a);
        MatrixToQuaternion(to.value, b);
        const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        const double sign = dot < 0.0 ? -1.0 : 1.0;
        double q[4];
        double length = 0.0;
        for (size_t index = 0; index < 4; ++index) {
            q[index] = a[index] + (sign * b[index] - a[index]) * alpha;
            length += q[index] * q[index];
        }
        if (!std::isfinite(length) || length < 1e-12) return false;
        length = std::sqrt(length);
        for (double &component : q) component /= length;
        QuaternionToMatrix(q, out.value);
    }
    for (size_t index = 9; index < kPoseFloats; ++index) {
        out.value[index] = static_cast<float>(
            static_cast<double>(from.value[index]) +
            (static_cast<double>(to.value[index]) - from.value[index]) * alpha);
    }
    for (const float component : out.value) {
        if (!std::isfinite(component)) return false;
    }
    return true;
}

// `node` expressed relative to `root`, and the reverse.
void RelativePose(const NodePose &root, const NodePose &node, NodePose &out) {
    const float *r = root.value;
    const float *n = node.value;
    const float scale = r[12] != 0.0f ? r[12] : 1.0f;
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            out.value[row * 3 + column] = r[0 + row] * n[0 + column] +
                r[3 + row] * n[3 + column] + r[6 + row] * n[6 + column];
        }
        const float dx = n[9] - r[9], dy = n[10] - r[10], dz = n[11] - r[11];
        out.value[9 + row] = (r[0 + row] * dx + r[3 + row] * dy + r[6 + row] * dz) / scale;
    }
    out.value[12] = n[12] / scale;
}

void ComposePose(const NodePose &root, const NodePose &relative, NodePose &out) {
    const float *r = root.value;
    const float *l = relative.value;
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            out.value[row * 3 + column] = r[row * 3 + 0] * l[0 + column] +
                r[row * 3 + 1] * l[3 + column] + r[row * 3 + 2] * l[6 + column];
        }
        out.value[9 + row] = (r[row * 3 + 0] * l[9] + r[row * 3 + 1] * l[10] +
                              r[row * 3 + 2] * l[11]) * r[12] + r[9 + row];
    }
    out.value[12] = r[12] * l[12];
}

// Relative poses are recomputed from world transforms every draw, so they
// carry rounding noise; only a real change counts.
bool RelativePoseDiffers(const NodePose &left, const NodePose &right) {
    for (size_t index = 0; index < kPoseFloats; ++index) {
        const float tolerance = (index >= 9 && index < 12) ? 0.005f : 0.0001f;
        if (!(std::fabs(left.value[index] - right.value[index]) <= tolerance)) return true;
    }
    return false;
}

// ---- On time, late, or every frame ----------------------------------------------
// A value driven by ticks changes either on tick draws (its pose is on time)
// or on the draw after a tick draw (it arrives a draw late), not on both. The
// draw kinds: 0 a tick draw after one that was not, 1 the draw after a tick
// draw, not itself one, 2 a tick draw after a tick draw, 3 neither. A change
// on kind 0 can only be on time, one on kind 1 only late, one on kind 2
// either, one on kind 3 neither. With many draws per tick a value driven every
// frame (by the mouse) shows itself by changing on kind-3 draws, which the
// off-tick scores count. At about 90 FPS there are none: every draw is a tick
// draw or the draw after one, two ticks often fall on consecutive draws, and
// the "draw after a tick" is then the next tick draw. There the evidence is:
// a change on kind 0 and one on kind 1 within two draws means the value is
// driven every frame; a late value's change on kind 2 belongs to the tick of
// the draw before. Both are used only with few draws per tick.
struct ChangeEvidence {
    uint32_t lastChange;     // serial of the last change recorded
    uint32_t onTimeChange;   // ... of the last one on a kind-0 draw
    uint32_t afterTickChange; // ... on a kind-1 draw
    uint32_t perFrameUntil;  // tick count up to which the value is shown as it is
    bool late;               // its last unambiguous change arrived a draw late
};

struct ChangeDecision {
    bool late;    // the tick of the draw before: shown with that draw's phase
    bool offTick; // belongs to no tick
    bool scores;  // counts towards the off-tick score
    bool mixed;   // with few draws per tick: driven every frame
};

void ResetChangeEvidence(ChangeEvidence &evidence, uint32_t serial, uint32_t tickCount) {
    evidence.lastChange = serial - 100;
    evidence.onTimeChange = serial - 100;
    evidence.afterTickChange = serial - 100;
    evidence.perFrameUntil = tickCount - 100;
    evidence.late = false;
}

bool HeldPerFrame(const ChangeEvidence &evidence, uint32_t tickCount) {
    return static_cast<int32_t>(evidence.perFrameUntil - tickCount) >= 0;
}

// With many draws per tick a change on a kind-1 draw is late unless the
// value also changed on the draw before (a kind-0 draw then), which makes it
// off tick. With few, the draw before may be a kind-2 draw where a late value
// changed too; it is off tick only if the value changed on a kind-0 draw.
ChangeDecision ClassifyChange(const ChangeEvidence &evidence, int kind, uint32_t serial) {
    ChangeDecision decision{};
    switch (kind) {
    case 0:
        decision.mixed = g_fewDrawsPerTick && serial - evidence.afterTickChange <= 2;
        decision.offTick = decision.mixed;
        break;
    case 1: {
        const bool conflict = g_fewDrawsPerTick ? serial - evidence.onTimeChange <= 2
                                                : evidence.lastChange + 1 == serial;
        decision.late = !conflict;
        decision.offTick = conflict;
        decision.scores = conflict;
        decision.mixed = conflict && g_fewDrawsPerTick;
        break;
    }
    case 2:
        decision.late = g_fewDrawsPerTick && evidence.late;
        break;
    default:
        decision.offTick = true;
        decision.scores = true;
        break;
    }
    return decision;
}

void RecordChange(ChangeEvidence &evidence, int kind, uint32_t serial, uint32_t tickCount,
                  const ChangeDecision &decision) {
    evidence.lastChange = serial;
    if (kind == 0) {
        // A change on a draw where only an on-time pose can arrive clears a
        // late one taken from a stray change before it.
        evidence.onTimeChange = serial;
        evidence.late = false;
    } else if (kind == 1) {
        evidence.afterTickChange = serial;
        if (decision.late) evidence.late = true;
    }
    // Until the end of the next tick period. At 90 FPS a value driven every
    // frame gives evidence on each kind-1 draw and the kind-0 draw after it,
    // so this lasts until the next.
    if (decision.mixed) evidence.perFrameUntil = tickCount + 1;
}

// ---- Sky ---------------------------------------------------------------------
// The sky is re-centred on the camera once per tick. When the camera is moved
// for a draw the sky has to move by the same amount, or it shifts against the
// view by up to one tick of movement.
SavedTranslation g_skySaved[256];
size_t g_skySavedCount = 0;

void ShiftSkyForDraw(const float delta[3]) {
    g_skySavedCount = 0;
    __try {
        const uintptr_t sky = *reinterpret_cast<const uintptr_t *>(kSkyPointer);
        if (sky < 0x10000) return;
        const uintptr_t roots[2] = {*reinterpret_cast<const uintptr_t *>(sky + 0x04),
                                    *reinterpret_cast<const uintptr_t *>(sky + 0x08)};
        uintptr_t stack[256];
        size_t top = 0;
        if (roots[0] >= 0x10000 && IsSceneNode(roots[0])) stack[top++] = roots[0];
        if (roots[1] >= 0x10000 && roots[1] != roots[0] && IsSceneNode(roots[1])) {
            // Skip the second root if it already hangs below the first.
            bool below = false;
            uintptr_t ancestor = *reinterpret_cast<const uintptr_t *>(roots[1] + kNodeParent);
            for (int depth = 0; depth < 16 && ancestor >= 0x10000; ++depth) {
                if (ancestor == roots[0]) {
                    below = true;
                    break;
                }
                ancestor = *reinterpret_cast<const uintptr_t *>(ancestor + kNodeParent);
            }
            if (!below) stack[top++] = roots[1];
        }
        while (top != 0 && g_skySavedCount < 256) {
            const uintptr_t object = stack[--top];
            if (object < 0x10000) continue;
            if (IsSceneNode(object)) {
                const uintptr_t children =
                    *reinterpret_cast<const uintptr_t *>(object + kNodeChildren);
                const uint32_t count =
                    *reinterpret_cast<const uint16_t *>(object + kNodeChildCount);
                if (children >= 0x10000 && count <= 256) {
                    for (uint32_t index = 0; index < count && top < 256; ++index) {
                        stack[top++] =
                            *reinterpret_cast<const uintptr_t *>(children + index * 4);
                    }
                }
            }
            SavedTranslation &saved = g_skySaved[g_skySavedCount];
            saved.node = object;
            memcpy(saved.original,
                   reinterpret_cast<const void *>(object + kNodeWorldTranslation),
                   sizeof(saved.original));
            bool finite = true;
            for (int axis = 0; axis < 3; ++axis) {
                saved.written[axis] = saved.original[axis] + delta[axis];
                finite = finite && std::isfinite(saved.written[axis]);
            }
            if (!finite) continue;
            memcpy(reinterpret_cast<void *>(object + kNodeWorldTranslation), saved.written,
                   sizeof(saved.written));
            ++g_skySavedCount;
            g_counters.skyNodesWritten.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void RestoreSkyAfterDraw() {
    __try {
        for (size_t index = g_skySavedCount; index-- > 0;) {
            const SavedTranslation &saved = g_skySaved[index];
            auto *live = reinterpret_cast<void *>(saved.node + kNodeWorldTranslation);
            if (memcmp(live, saved.written, sizeof(saved.written)) != 0) {
                g_counters.skyRestoreConflicts.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            memcpy(live, saved.original, sizeof(saved.original));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
    g_skySavedCount = 0;
}

// ---- Third person: the camera's offset -----------------------------------------
// The camera's position relative to the player is written as
// height * up + back * forward + side * right, with `forward` the camera's
// view direction and `right` horizontal. Turning the camera with the mouse
// changes the directions every frame but not the three numbers; crouching,
// zooming and the engine's own smoothing change the numbers, on ticks. Each
// number gets a tick-aligned from/to pair and stops being blended for a
// while if it is seen to change off a tick.
struct TickTrack {
    float from;
    float to;
    uint8_t score;
    bool valid;
    ChangeEvidence evidence;
};
TickTrack g_thirdPersonTracks[3]{};
uint32_t g_thirdPersonSerial = 1;
uint32_t g_thirdPersonTickCount = 0;
bool g_thirdPersonPreviousWasTick = false;

float AdvanceTickTrack(TickTrack &track, float value, bool tick, int frameKind, double alpha,
                       float tolerance) {
    if (track.score != 0) --track.score;
    if (!track.valid || !std::isfinite(value)) {
        track.from = track.to = value;
        track.valid = std::isfinite(value);
        track.score = 0;
        ResetChangeEvidence(track.evidence, g_thirdPersonSerial, g_thirdPersonTickCount);
        return value;
    }
    const float step = std::fabs(value - track.to);
    if (step > tolerance) {
        const ChangeDecision decision =
            ClassifyChange(track.evidence, frameKind, g_thirdPersonSerial);
        RecordChange(track.evidence, frameKind, g_thirdPersonSerial, g_thirdPersonTickCount,
                     decision);
        if (decision.mixed) g_counters.offsetMixed.fetch_add(1, std::memory_order_relaxed);
        if (!tick) {
            track.score = static_cast<uint8_t>(track.score > 12 ? 16 : track.score + 4);
            track.from = value;
            g_counters.thirdPersonOffTick.fetch_add(1, std::memory_order_relaxed);
        } else if (track.score >= 8 || step > 40.0f ||
                   HeldPerFrame(track.evidence, g_thirdPersonTickCount)) {
            track.from = value;
        } else {
            track.from = track.to;
            g_counters.thirdPersonSteps.fetch_add(1, std::memory_order_relaxed);
        }
        track.to = value;
    } else if (tick) {
        track.from = track.to;
    }
    return static_cast<float>(track.from +
                              (static_cast<double>(track.to) - track.from) * alpha);
}

// Returns true when `delta` is a usable, non-zero correction for the camera.
bool ThirdPersonCameraDelta(uintptr_t camera, const float cameraPosition[3],
                            const float playerPosition[3], bool tick, double alpha,
                            float delta[3]) {
    delta[0] = delta[1] = delta[2] = 0.0f;
    ++g_thirdPersonSerial;
    if (tick) ++g_thirdPersonTickCount;
    const int frameKind =
        tick ? (g_thirdPersonPreviousWasTick ? 2 : 0) : (g_thirdPersonPreviousWasTick ? 1 : 3);
    g_thirdPersonPreviousWasTick = tick;
    const float *rotation = reinterpret_cast<const float *>(camera + kNodeWorldTransform);
    const float forward[3] = {rotation[0], rotation[3], rotation[6]};
    // right = up x forward, horizontal; undefined when the view is straight
    // up or down.
    float right[3] = {-forward[1], forward[0], 0.0f};
    const float rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1]);
    const float vertical = forward[2];
    if (!(rightLength > 0.15f) || !(std::fabs(vertical) < 0.985f)) {
        for (TickTrack &track : g_thirdPersonTracks) track = {};
        return false;
    }
    right[0] /= rightLength;
    right[1] /= rightLength;
    float offset[3];
    for (int axis = 0; axis < 3; ++axis) offset[axis] = cameraPosition[axis] - playerPosition[axis];
    const float side = offset[0] * right[0] + offset[1] * right[1];
    const float inPlane[3] = {offset[0] - side * right[0], offset[1] - side * right[1],
                              offset[2]};
    const float alongForward =
        inPlane[0] * forward[0] + inPlane[1] * forward[1] + inPlane[2] * forward[2];
    const float back = (alongForward - vertical * inPlane[2]) / (1.0f - vertical * vertical);
    const float height = inPlane[2] - back * vertical;
    const float engineValues[3] = {height, back, side};
    float shownValues[3];
    for (int index = 0; index < 3; ++index) {
        shownValues[index] = AdvanceTickTrack(g_thirdPersonTracks[index], engineValues[index],
                                              tick, frameKind, alpha, 0.01f);
    }
    const float dHeight = shownValues[0] - height;
    const float dBack = shownValues[1] - back;
    const float dSide = shownValues[2] - side;
    delta[0] = dBack * forward[0] + dSide * right[0];
    delta[1] = dBack * forward[1] + dSide * right[1];
    delta[2] = dHeight + dBack * forward[2];
    return std::isfinite(delta[0]) && std::isfinite(delta[1]) && std::isfinite(delta[2]) &&
           (std::fabs(delta[0]) > 0.0001f || std::fabs(delta[1]) > 0.0001f ||
            std::fabs(delta[2]) > 0.0001f);
}

// ---- Third person: the player's body -------------------------------------------
// The body turns with the mouse every frame, so in world space, or relative
// to the root, every node looks "driven every frame". Each node is therefore
// taken relative to its parent, where only animation (on ticks) shows, and
// the world poses are rebuilt from the root down, the root carrying the
// camera's translation correction.
struct BodyPoseHistory {
    uintptr_t node;
    uint32_t lastSeen;
    uint32_t changePeriod;
    uint8_t offTickScore; // +4 per off-tick change, -1 per draw
    bool lag;             // the last change arrived on the draw after its tick
    ChangeEvidence evidence;
    NodePose from;
    NodePose to;
};
constexpr size_t kBodyHistorySize = 2048; // power of two
BodyPoseHistory g_bodyHistory[kBodyHistorySize]{};
SavedNodePose g_bodySaved[1024];
size_t g_bodySavedCount = 0;
bool g_bodyPreviousDrawWasTick = false;
double g_bodyPreviousDrawAlpha = 0.0;
uint32_t g_bodySerial = 1;
uint32_t g_bodyTickCount = 0;

void RestorePlayerBodyPose() {
    __try {
        for (size_t index = g_bodySavedCount; index-- > 0;) {
            const SavedNodePose &saved = g_bodySaved[index];
            auto *live = reinterpret_cast<void *>(saved.node + kNodeWorldTransform);
            if (memcmp(live, saved.written.value, sizeof(saved.written.value)) != 0) {
                g_counters.bodyRestoreConflicts.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            memcpy(live, saved.pose.value, sizeof(saved.pose.value));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
    g_bodySavedCount = 0;
}

BodyPoseHistory *FindBodyHistory(uintptr_t object) {
    BodyPoseHistory *history = nullptr;
    size_t slot = (object >> 4) & (kBodyHistorySize - 1);
    for (size_t probe = 0; probe < 32; ++probe) {
        BodyPoseHistory &entry = g_bodyHistory[slot];
        if (entry.node == object) return &entry;
        if (entry.node == 0) return history != nullptr ? history : &entry;
        if (history == nullptr && entry.lastSeen + 600 < g_bodySerial) history = &entry;
        slot = (slot + 1) & (kBodyHistorySize - 1);
    }
    return history;
}

// Advances one node's history with this draw's parent-relative pose and
// returns the pose to show. `blended` says whether it differs from the
// engine's.
void AdvanceBodyNode(uintptr_t object, const NodePose &relative, int frameKind,
                     uint32_t ticksBefore, double frameAlpha, double previousAlpha,
                     NodePose &shownRelative, bool &blended) {
    shownRelative = relative;
    blended = false;
    BodyPoseHistory *history = FindBodyHistory(object);
    if (history == nullptr) return;
    const bool known = history->node == object && history->lastSeen + 1 == g_bodySerial;
    history->node = object;
    history->lastSeen = g_bodySerial;
    if (!known) {
        history->from = relative;
        history->to = relative;
        history->changePeriod = 0;
        history->offTickScore = 0;
        ResetChangeEvidence(history->evidence, g_bodySerial, g_bodyTickCount);
        return;
    }
    if (history->offTickScore != 0) --history->offTickScore;
    if (RelativePoseDiffers(relative, history->to)) {
        g_counters.bodyChanges[frameKind].fetch_add(1, std::memory_order_relaxed);
        // A pose arrives either on the tick draw or on the draw after it. A
        // change on the draw after a tick counts as that tick's, unless the
        // node also changed on the tick draw itself, which means it moves
        // every frame. With few draws per tick, see ClassifyChange.
        const ChangeDecision decision =
            ClassifyChange(history->evidence, frameKind, g_bodySerial);
        const bool lagged = decision.late;
        const bool offTick = decision.offTick;
        bool minor = false;
        if (offTick) {
            // Foot placement re-solves the legs whenever the body turns,
            // between ticks as well, by small amounts. A small off-tick
            // change is let through: the tick-to-tick blend carries on, and
            // the adjustment is picked up at the next tick.
            float rotationStep = 0.0f;
            float translationStep = 0.0f;
            for (size_t index = 0; index < kPoseFloats; ++index) {
                const float difference =
                    std::fabs(relative.value[index] - history->to.value[index]);
                float &largest = (index >= 9 && index < 12) ? translationStep : rotationStep;
                if (!(difference <= largest)) largest = difference;
            }
            if (rotationStep <= 0.03f && translationStep <= 0.5f) {
                g_counters.bodyMinor.fetch_add(1, std::memory_order_relaxed);
                minor = true;
            }
        }
        if (!minor) {
            RecordChange(history->evidence, frameKind, g_bodySerial, g_bodyTickCount, decision);
            if (decision.mixed) g_counters.bodyMixed.fetch_add(1, std::memory_order_relaxed);
            if (frameKind == 2 && lagged) {
                g_counters.bodyLateAfterTick.fetch_add(1, std::memory_order_relaxed);
            }
            if (decision.scores) {
                history->offTickScore = static_cast<uint8_t>(
                    history->offTickScore > 12 ? 16 : history->offTickScore + 4);
            }
            if (offTick) g_counters.bodyOffTick.fetch_add(1, std::memory_order_relaxed);
            if (offTick || history->offTickScore >= 8 ||
                HeldPerFrame(history->evidence, g_bodyTickCount)) {
                history->from = relative;
                history->to = relative;
                history->changePeriod = 0;
            } else {
                history->from = history->to;
                history->to = relative;
                history->lag = lagged;
                history->changePeriod = lagged ? ticksBefore : g_bodyTickCount;
            }
        }
    }
    const uint32_t period = history->lag ? ticksBefore : g_bodyTickCount;
    if (history->changePeriod != 0 && history->changePeriod == period) {
        float stepSquared = 0.0f;
        for (size_t axis = 9; axis < 12; ++axis) {
            const float delta = history->to.value[axis] - history->from.value[axis];
            stepSquared += delta * delta;
        }
        if (stepSquared <= 900.0f &&
            BlendPose(history->from, history->to, history->lag ? previousAlpha : frameAlpha,
                      shownRelative)) {
            blended = true;
            g_counters.bodyBlends.fetch_add(1, std::memory_order_relaxed);
        } else {
            shownRelative = relative;
        }
    }
}

struct BodyStackEntry {
    uintptr_t node;
    NodePose parentEngine;
    NodePose parentShown;
    bool parentMoved;
};
BodyStackEntry g_bodyStack[512];

bool ApplyPlayerBodyPose(uintptr_t player, const float playerPosition[3],
                         const float correction[3], bool frameIsTick, double frameAlpha,
                         bool interpolate) {
    g_bodySavedCount = 0;
    __try {
        const uint32_t ticksBefore = g_bodyTickCount;
        if (frameIsTick) ++g_bodyTickCount;
        ++g_bodySerial;
        const bool previousWasTick = g_bodyPreviousDrawWasTick;
        const double previousAlpha = g_bodyPreviousDrawAlpha;
        g_bodyPreviousDrawWasTick = frameIsTick;
        g_bodyPreviousDrawAlpha = frameAlpha;
        const int frameKind =
            frameIsTick ? (previousWasTick ? 2 : 0) : (previousWasTick ? 1 : 3);
        const uintptr_t renderState =
            *reinterpret_cast<const uintptr_t *>(player + kReferenceRenderState);
        const uintptr_t rootNode = renderState >= 0x10000
            ? *reinterpret_cast<const uintptr_t *>(renderState + 0x14) : 0;
        if (rootNode < 0x10000 || !IsSceneNode(rootNode)) return false;
        NodePose rootEngine;
        memcpy(rootEngine.value, reinterpret_cast<const void *>(rootNode + kNodeWorldTransform),
               sizeof(rootEngine.value));
        // The root has to be where the player is, or this is not the body.
        if (DistanceSquared(&rootEngine.value[9], playerPosition) > 1.0f) {
            g_counters.bodyRootMismatch.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        NodePose rootShown = rootEngine;
        const bool corrected =
            correction[0] != 0.0f || correction[1] != 0.0f || correction[2] != 0.0f;
        for (size_t axis = 0; axis < 3; ++axis) rootShown.value[9 + axis] += correction[axis];

        size_t top = 0;
        g_bodyStack[top].node = rootNode;
        g_bodyStack[top].parentEngine = rootEngine;
        g_bodyStack[top].parentShown = rootShown;
        g_bodyStack[top].parentMoved = false;
        ++top;
        while (top != 0) {
            --top;
            const uintptr_t object = g_bodyStack[top].node;
            const NodePose parentEngine = g_bodyStack[top].parentEngine;
            const NodePose parentShown = g_bodyStack[top].parentShown;
            const bool parentMoved = g_bodyStack[top].parentMoved;
            if (object < 0x10000) continue;
            NodePose engine;
            memcpy(engine.value, reinterpret_cast<const void *>(object + kNodeWorldTransform),
                   sizeof(engine.value));
            NodePose shown = rootShown;
            bool blended = false;
            if (object != rootNode) {
                NodePose relative;
                RelativePose(parentEngine, engine, relative);
                NodePose shownRelative;
                AdvanceBodyNode(object, relative, frameKind, ticksBefore, frameAlpha,
                                previousAlpha, shownRelative, blended);
                ComposePose(parentShown, shownRelative, shown);
            }
            const bool moved = parentMoved || blended;
            if (IsSceneNode(object)) {
                const uintptr_t children =
                    *reinterpret_cast<const uintptr_t *>(object + kNodeChildren);
                const uint32_t count =
                    *reinterpret_cast<const uint16_t *>(object + kNodeChildCount);
                if (children >= 0x10000 && count <= 1024) {
                    for (uint32_t index = 0; index < count && top < 512; ++index) {
                        g_bodyStack[top].node =
                            *reinterpret_cast<const uintptr_t *>(children + index * 4);
                        g_bodyStack[top].parentEngine = engine;
                        g_bodyStack[top].parentShown = shown;
                        g_bodyStack[top].parentMoved = moved;
                        ++top;
                    }
                }
            }
            // Rebuilding a pose that is neither moved nor blended would only
            // write rounding noise.
            if (!interpolate || (!corrected && !moved)) continue;
            bool finite = true;
            for (const float component : shown.value) finite = finite && std::isfinite(component);
            if (!finite || g_bodySavedCount >= 1024) continue;
            SavedNodePose &saved = g_bodySaved[g_bodySavedCount++];
            saved.node = object;
            saved.pose = engine;
            saved.written = shown;
            memcpy(reinterpret_cast<void *>(object + kNodeWorldTransform), shown.value,
                   sizeof(shown.value));
            g_counters.bodyNodesWritten.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
    return g_bodySavedCount != 0;
}

// ---- First person: arms, weapon and the camera bone -----------------------------
// The first-person skeleton hangs below the player's first-person root and is
// drawn in its own pass, which recomputes world transforms from local ones.
// The blend is therefore applied to the local transforms (rotation,
// translation, scale: 13 floats) of every node below the root, written before
// the composite render and put back after it. The root follows the camera and
// is left alone.
struct FirstPersonLocalHistory {
    uintptr_t node;
    uint32_t lastSeen;
    uint8_t offTickScore;   // +4 per off-tick change, -1 per render
    bool lastChangeLagged;  // the previous change came a draw after its tick
    uint32_t changePeriod;  // tick period the last change belongs to
    bool lag;               // change first seen on the frame after its tick
    ChangeEvidence evidence;
    NodePose from;
    NodePose to;
};
constexpr size_t kFirstPersonHistorySize = 2048; // power of two
FirstPersonLocalHistory g_firstPersonLocal[kFirstPersonHistorySize]{};
uint32_t g_fpSerial = 1;
uint32_t g_fpTickCount = 0;
bool g_fpPreviousFrameWasTick = false;
double g_fpPreviousFrameAlpha = 0.0;
// Blended local transforms worked out for this frame: node, the engine's
// local transform, and the one to show.
SavedNodePose g_fpPending[512];
size_t g_fpPendingCount = 0;
SavedNodePose g_fpSaved[512];
size_t g_fpSavedCount = 0;
bool g_fpAdvanced = false;
uintptr_t g_fpRoot = 0;
uintptr_t g_fpCameraBone = 0;
// One blended node's world transform as it was before the render, to see
// whether the render recomputes world transforms from the blended locals.
uintptr_t g_fpProbeNode = 0;
NodePose g_fpProbeWorld{};
// The camera bone's world position written for the pass. If the render does
// not recompute it, the engine's value is put back afterwards.
SavedTranslation g_fpBoneSaved{};
// World transforms written below the root (see WriteFirstPersonWorlds).
SavedNodePose g_fpWorldSaved[1024];
size_t g_fpWorldSavedCount = 0;
void WriteFirstPersonWorlds();

// Where a render is relative to the ticks.
struct RenderPhase {
    uint32_t serial;
    uint32_t tickCount;   // ticks up to and including this render
    uint32_t ticksBefore; // ... up to the render before
    int kind;             // see ClassifyChange
    double alpha;
    double previousAlpha;
};

// Advances one node's history with this render's local transform. Returns
// what was decided about a change on this render, as the aim trace numbers
// it (0 no change, 1 accepted on the tick draw, 2 accepted late, 3 shown as
// is: off tick, 4 shown as is: driven every frame, 5 shown as is: first late
// pose). `tooBig` is set when a blend is in progress but its step is above 30
// units; `blend` when `shown` holds the pose to show.
uint8_t AdvanceFirstPersonNode(FirstPersonLocalHistory &history, const NodePose &current,
                               const RenderPhase &phase, bool &tooBig, NodePose &shown,
                               bool &blend) {
    tooBig = false;
    blend = false;
    uint8_t decision = 0;
    if (history.offTickScore != 0) --history.offTickScore;
    if (memcmp(history.to.value, current.value, sizeof(current.value)) != 0) {
        g_counters.fpChanges[phase.kind].fetch_add(1, std::memory_order_relaxed);
        // Animation moves a node on tick frames only. A node that keeps
        // changing on other frames as well is being driven every frame (the
        // pitch-following nodes while the mouse moves); blending it would
        // make it trail the view. A single off-tick change is shown as it is
        // without reclassifying the node. A change on the draw after a tick,
        // with none on the tick draw before it, is a late pose: the first one
        // is shown as it is, and from the second in a row the node is blended
        // with the previous draw's phase. With few draws per tick, see
        // ClassifyChange.
        const ChangeDecision change =
            ClassifyChange(history.evidence, phase.kind, phase.serial);
        RecordChange(history.evidence, phase.kind, phase.serial, phase.tickCount, change);
        if (change.mixed) g_counters.fpMixed.fetch_add(1, std::memory_order_relaxed);
        const bool latePose = change.late;
        const bool offTick = change.offTick;
        const bool blendLate = latePose && history.lastChangeLagged;
        history.lastChangeLagged = latePose;
        if (change.scores) {
            history.offTickScore = static_cast<uint8_t>(
                history.offTickScore > 12 ? 16 : history.offTickScore + 4);
        }
        const bool perFrame = history.offTickScore >= 8 ||
                              HeldPerFrame(history.evidence, phase.tickCount);
        if (blendLate && !perFrame) {
            g_counters.fpLateBlends.fetch_add(1, std::memory_order_relaxed);
        }
        if (offTick || perFrame || (latePose && !blendLate)) {
            g_counters.fpPerFrame.fetch_add(1, std::memory_order_relaxed);
            history.from = current;
            history.to = current;
            history.changePeriod = 0;
            return offTick ? 3 : (perFrame ? 4 : 5);
        }
        if (phase.kind == 2 && latePose) {
            g_counters.fpLateAfterTick.fetch_add(1, std::memory_order_relaxed);
        }
        history.from = history.to;
        history.to = current;
        history.lag = latePose;
        history.changePeriod = latePose ? phase.ticksBefore : phase.tickCount;
        decision = latePose ? 2 : 1;
    }
    const uint32_t period = history.lag ? phase.ticksBefore : phase.tickCount;
    if (history.changePeriod == 0 || history.changePeriod != period) return decision;
    float stepSquared = 0.0f;
    for (size_t axis = 9; axis < 12; ++axis) {
        const float delta = history.to.value[axis] - history.from.value[axis];
        stepSquared += delta * delta;
    }
    if (!(stepSquared <= 900.0f)) {
        tooBig = true;
        return decision;
    }
    blend = BlendPose(history.from, history.to,
                      history.lag ? phase.previousAlpha : phase.alpha, shown);
    return decision;
}

// ---- Field of view ------------------------------------------------------------
struct FovTrack {
    float from;
    float to;
    float shown;
    bool valid;
    bool lag;  // change first seen on the frame after its tick
    bool late; // its last change on a kind-0 or kind-1 draw (see ClassifyChange) was late
    uint32_t changePeriod;
};
FovTrack g_fovTracks[3]{};
uint32_t g_fovFrameSerial = 1; // advanced at the start of every frame
uint32_t g_fovUpdatedFrame = 0;
uint32_t g_fovTickCount = 0;
bool g_fovPreviousFrameWasTick = false;
double g_fovPreviousFrameAlpha = 0.0;

void UpdateFovTracks(uintptr_t player) {
    const bool frameIsTick = g_frameTickDecision != 0;
    double frameAlpha = g_accumulatorMs / g_simulationStepMs;
    if (frameAlpha < 0.0) frameAlpha = 0.0;
    if (frameAlpha > 1.0) frameAlpha = 1.0;
    const uint32_t ticksBefore = g_fovTickCount;
    if (frameIsTick) ++g_fovTickCount;
    const bool previousWasTick = g_fovPreviousFrameWasTick;
    const double previousAlpha = g_fovPreviousFrameAlpha;
    g_fovPreviousFrameWasTick = frameIsTick;
    g_fovPreviousFrameAlpha = frameAlpha;
    for (int index = 0; index < 3; ++index) {
        FovTrack &track = g_fovTracks[index];
        const float engine = *reinterpret_cast<const float *>(player + kPlayerFov + index * 4);
        if (!track.valid || !std::isfinite(engine)) {
            track.from = track.to = track.shown = engine;
            track.valid = std::isfinite(engine);
            track.changePeriod = 0;
            track.late = false;
            continue;
        }
        if (engine != track.to) {
            const float step = std::fabs(engine - track.to);
            if ((!frameIsTick && !previousWasTick) || step > 30.0f) {
                // Changed away from a tick, or jumped: show it as it is.
                track.from = track.to = engine;
                track.changePeriod = 0;
                g_counters.fovAsIs.fetch_add(1, std::memory_order_relaxed);
            } else {
                // On a tick draw after a tick draw the change is either that
                // tick's or, if the values arrive a draw late, the one before.
                if (frameIsTick != previousWasTick) track.late = !frameIsTick;
                track.from = track.to;
                track.to = engine;
                track.lag = !frameIsTick || (previousWasTick && g_fewDrawsPerTick && track.late);
                track.changePeriod = track.lag ? ticksBefore : g_fovTickCount;
                if (frameIsTick && track.lag) {
                    g_counters.fovLateAfterTick.fetch_add(1, std::memory_order_relaxed);
                }
                g_counters.fovSteps.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const uint32_t period = track.lag ? ticksBefore : g_fovTickCount;
        if (track.changePeriod != 0 && track.changePeriod == period) {
            const double alpha = track.lag ? previousAlpha : frameAlpha;
            track.shown = static_cast<float>(
                track.from + (static_cast<double>(track.to) - track.from) * alpha);
        } else {
            track.shown = track.to;
        }
    }
}

// Called from the thunks below with the address of the field-of-view
// argument on the caller's stack.
void __cdecl AdjustFovArgument(float *fieldOfView) {
    g_counters.fovCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_interpolateFovEnabled || !g_hooksInstalled.load(std::memory_order_relaxed) ||
        g_frameTickDecision < 0 || !g_frameEligible ||
        GetCurrentThreadId() != g_mainThreadId) {
        return;
    }
    __try {
        const uintptr_t player = *reinterpret_cast<const uintptr_t *>(kPlayerPointer);
        if (player < 0x10000) return;
        if (g_fovUpdatedFrame != g_fovFrameSerial) {
            g_fovUpdatedFrame = g_fovFrameSerial;
            UpdateFovTracks(player);
        }
        for (int index = 0; index < 3; ++index) {
            const FovTrack &track = g_fovTracks[index];
            if (!track.valid) continue;
            const float engine =
                *reinterpret_cast<const float *>(player + kPlayerFov + index * 4);
            if (memcmp(&engine, fieldOfView, sizeof(engine)) != 0) continue;
            if (track.shown != engine && std::isfinite(track.shown)) {
                *fieldOfView = track.shown;
                g_counters.fovSubstitutions.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
}

// The call sites are redirected here instead of to their stock targets. The
// stock function is entered with the stack exactly as the caller left it,
// except for the adjusted argument.
__declspec(naked) void FovSceneThunk() {
    __asm {
        push ecx
        lea eax, [esp + 8]
        push eax
        call AdjustFovArgument
        add esp, 4
        pop ecx
        mov eax, kSetFovScene
        jmp eax
    }
}
__declspec(naked) void FovGlobalThunk() {
    __asm {
        lea eax, [esp + 4]
        push eax
        call AdjustFovArgument
        add esp, 4
        mov eax, kSetFovGlobal
        jmp eax
    }
}

// Diagnostic burst: one row per render for a while after the aim button
// changes, written to a ring here and to the log by the log thread. Each row
// has the frame kind, the blend phase, how many nodes took each decision, and
// up to five nodes of interest by name.
//   decisions: 1 accepted on the tick draw, 2 accepted late, 3 shown as is
//   (off tick), 4 shown as is (driven every frame), 5 shown as is (first late
//   pose), 6 not blended (step above 30 units)
struct FpTraceNote {
    char name[24];
    uint8_t decision;
    float translationStep;
    float rotationStep;
};
struct FpTraceRow {
    uint32_t serial;
    uint8_t kind;
    uint8_t noteCount;
    float alpha;
    uint16_t counts[8]; // [0] changed, [1..6] by decision, [7] blends in progress
    float fovEngine[3];
    float fovShown[3];
    FpTraceNote notes[5];
};
constexpr uint32_t kFpTraceSize = 1024; // power of two
FpTraceRow g_fpTrace[kFpTraceSize];
std::atomic<uint32_t> g_fpTraceWrite{0};
uint32_t g_fpTraceRead = 0;
uint32_t g_fpTraceHot = 0;
bool g_fpTraceEnabled = false;

void NoteNode(FpTraceRow &row, uintptr_t object, uint8_t decision, const NodePose &from,
              const NodePose &to) {
    float translation = 0.0f, rotation = 0.0f;
    for (size_t index = 0; index < kPoseFloats; ++index) {
        const float difference = std::fabs(to.value[index] - from.value[index]);
        float &largest = (index >= 9 && index < 12) ? translation : rotation;
        if (!(difference <= largest)) largest = difference;
    }
    // Keep the as-is decisions; of the accepted ones keep the largest step.
    FpTraceNote *note = nullptr;
    if (row.noteCount < 5) {
        note = &row.notes[row.noteCount++];
    } else if (decision >= 3) {
        for (FpTraceNote &candidate : row.notes) {
            if (candidate.decision < 3) {
                note = &candidate;
                break;
            }
        }
    } else {
        for (FpTraceNote &candidate : row.notes) {
            if (candidate.decision < 3 &&
                candidate.translationStep + candidate.rotationStep < translation + rotation &&
                (note == nullptr || candidate.translationStep + candidate.rotationStep <
                                        note->translationStep + note->rotationStep)) {
                note = &candidate;
            }
        }
    }
    if (note == nullptr) return;
    note->decision = decision;
    note->translationStep = translation;
    note->rotationStep = rotation;
    note->name[0] = '\0';
    const uintptr_t text = *reinterpret_cast<const uintptr_t *>(object + kNodeName);
    if (text >= 0x10000) {
        const char *source = reinterpret_cast<const char *>(text);
        size_t length = 0;
        while (length + 1 < sizeof(note->name) && source[length] != '\0') {
            const char c = source[length];
            note->name[length] = (c > 0x20 && c < 0x7F && c != ',') ? c : '_';
            ++length;
        }
        note->name[length] = '\0';
    }
}

bool NodeNameIs(uintptr_t object, const char *name) {
    const uintptr_t text = *reinterpret_cast<const uintptr_t *>(object + kNodeName);
    if (text < 0x10000) return false;
    return strncmp(reinterpret_cast<const char *>(text), name, 16) == 0;
}

// Advances every node's history with this frame's local transform and fills
// g_fpPending. Once per frame.
void FirstPersonAdvance(uintptr_t player, bool frameIsTick, double frameAlpha) {
    g_fpAdvanced = false;
    g_fpPendingCount = 0;
    g_fpRoot = 0;
    g_fpCameraBone = 0;
    __try {
        const uintptr_t rootNode =
            *reinterpret_cast<const uintptr_t *>(player + kPlayerFirstPersonRoot);
        if (rootNode < 0x10000 || !IsSceneNode(rootNode)) return;
        const uint32_t ticksBefore = g_fpTickCount;
        if (frameIsTick) ++g_fpTickCount;
        const bool previousWasTick = g_fpPreviousFrameWasTick;
        const double previousAlpha = g_fpPreviousFrameAlpha;
        g_fpPreviousFrameWasTick = frameIsTick;
        g_fpPreviousFrameAlpha = frameAlpha;
        ++g_fpSerial;
        const int frameKind =
            frameIsTick ? (previousWasTick ? 2 : 0) : (previousWasTick ? 1 : 3);
        const RenderPhase phase{g_fpSerial, g_fpTickCount, ticksBefore,
                                frameKind,  frameAlpha,    previousAlpha};
        const bool tracing = g_fpTraceEnabled && g_fpTraceHot != 0;
        static FpTraceRow row;
        if (tracing) {
            --g_fpTraceHot;
            row = {};
            row.serial = g_fpSerial;
            row.kind = static_cast<uint8_t>(frameKind);
            row.alpha = static_cast<float>(frameAlpha);
            for (int index = 0; index < 3; ++index) {
                row.fovEngine[index] =
                    *reinterpret_cast<const float *>(player + kPlayerFov + index * 4);
                row.fovShown[index] = g_fovTracks[index].shown;
            }
        }

        static uintptr_t stack[512];
        size_t top = 0;
        stack[top++] = rootNode;
        while (top != 0) {
            const uintptr_t object = stack[--top];
            if (object < 0x10000) continue;
            if (IsSceneNode(object)) {
                const uintptr_t children =
                    *reinterpret_cast<const uintptr_t *>(object + kNodeChildren);
                const uint32_t count =
                    *reinterpret_cast<const uint16_t *>(object + kNodeChildCount);
                if (children >= 0x10000 && count <= 1024) {
                    for (uint32_t index = 0; index < count && top < 512; ++index) {
                        stack[top++] =
                            *reinterpret_cast<const uintptr_t *>(children + index * 4);
                    }
                }
            }
            if (object == rootNode) continue;
            g_counters.fpNodes.fetch_add(1, std::memory_order_relaxed);
            if (g_fpCameraBone == 0 && NodeNameIs(object, "Camera1st")) g_fpCameraBone = object;

            NodePose current;
            memcpy(current.value, reinterpret_cast<const void *>(object + kNodeLocalTransform),
                   sizeof(current.value));
            FirstPersonLocalHistory *history = nullptr;
            size_t slot = (object >> 4) & (kFirstPersonHistorySize - 1);
            for (size_t probe = 0; probe < 32; ++probe) {
                FirstPersonLocalHistory &entry = g_firstPersonLocal[slot];
                if (entry.node == object || entry.node == 0) {
                    history = &entry;
                    break;
                }
                if (history == nullptr && entry.lastSeen + 600 < g_fpSerial) history = &entry;
                slot = (slot + 1) & (kFirstPersonHistorySize - 1);
            }
            if (history == nullptr) continue;
            const bool known = history->node == object && history->lastSeen + 1 == g_fpSerial;
            history->node = object;
            history->lastSeen = g_fpSerial;
            if (!known) {
                history->from = current;
                history->to = current;
                history->offTickScore = 0;
                history->changePeriod = 0;
                history->lastChangeLagged = false;
                ResetChangeEvidence(history->evidence, g_fpSerial, g_fpTickCount);
                continue;
            }
            NodePose previousTo{};
            if (tracing) previousTo = history->to;
            bool tooBig = false;
            bool blend = false;
            NodePose shown;
            const uint8_t decision =
                AdvanceFirstPersonNode(*history, current, phase, tooBig, shown, blend);
            if (tracing) {
                if (decision != 0) {
                    ++row.counts[0];
                    ++row.counts[decision];
                    NoteNode(row, object, decision, previousTo, current);
                }
                if (tooBig) {
                    ++row.counts[6];
                    NoteNode(row, object, 6, history->from, history->to);
                }
            }
            if (!blend) continue;
            if (g_fpPendingCount >= 512) continue;
            if (tracing) ++row.counts[7];
            SavedNodePose &pending = g_fpPending[g_fpPendingCount++];
            pending.node = object;
            pending.pose = current;
            pending.written = shown;
            g_counters.fpBlends.fetch_add(1, std::memory_order_relaxed);
        }
        if (tracing) {
            const uint32_t write = g_fpTraceWrite.load(std::memory_order_relaxed);
            g_fpTrace[write & (kFpTraceSize - 1)] = row;
            g_fpTraceWrite.store(write + 1, std::memory_order_release);
        }
        g_fpRoot = rootNode;
        g_fpAdvanced = true;
        if (g_fpCameraBone == 0) g_counters.fpNoCameraBone.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
        g_fpAdvanced = false;
        g_fpPendingCount = 0;
    }
}

// Writes the pending locals and, for the pass's camera, the camera bone's
// world position worked out from them. Runs before the composite render,
// while the engine's locals are still in place.
void FirstPersonWrite() {
    g_fpSavedCount = 0;
    g_fpProbeNode = 0;
    g_fpBoneSaved.node = 0;
    __try {
        for (size_t index = 0; index < g_fpPendingCount; ++index) {
            const SavedNodePose &pending = g_fpPending[index];
            auto *live = reinterpret_cast<void *>(pending.node + kNodeLocalTransform);
            if (memcmp(live, pending.pose.value, sizeof(pending.pose.value)) != 0) continue;
            if (g_fpSavedCount >= 512) break;
            g_fpSaved[g_fpSavedCount++] = pending;
            memcpy(live, pending.written.value, sizeof(pending.written.value));
            g_counters.fpWritten.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_firstPersonWorldEnabled) WriteFirstPersonWorlds();
        if (g_fpSavedCount != 0) {
            g_fpProbeNode = g_fpSaved[0].node;
            memcpy(g_fpProbeWorld.value,
                   reinterpret_cast<const void *>(g_fpProbeNode + kNodeWorldTransform),
                   sizeof(g_fpProbeWorld.value));
        }
        // The pass may read the camera bone's world position before it
        // updates the skeleton; give it the position that the blended locals
        // will produce. WriteFirstPersonWorlds covers it when it runs.
        const uintptr_t cameraNode = g_fpCameraBone;
        if (!g_firstPersonWorldEnabled && cameraNode >= 0x10000 && cameraNode != g_fpRoot &&
            g_fpSavedCount != 0) {
            float position[3];
            memcpy(position, reinterpret_cast<const void *>(cameraNode + kNodeLocalTransform + 36),
                   sizeof(position));
            uintptr_t ancestor = *reinterpret_cast<const uintptr_t *>(cameraNode + kNodeParent);
            bool reachedRoot = false;
            for (int depth = 0; depth < 32 && ancestor >= 0x10000; ++depth) {
                const bool isRoot = ancestor == g_fpRoot;
                float transform[13];
                memcpy(transform,
                       reinterpret_cast<const void *>(
                           ancestor + (isRoot ? kNodeWorldTransform : kNodeLocalTransform)),
                       sizeof(transform));
                float moved[3];
                for (int row = 0; row < 3; ++row) {
                    moved[row] = (transform[row * 3 + 0] * position[0] +
                                  transform[row * 3 + 1] * position[1] +
                                  transform[row * 3 + 2] * position[2]) *
                                     transform[12] + transform[9 + row];
                }
                memcpy(position, moved, sizeof(position));
                if (isRoot) {
                    reachedRoot = true;
                    break;
                }
                ancestor = *reinterpret_cast<const uintptr_t *>(ancestor + kNodeParent);
            }
            if (reachedRoot && std::isfinite(position[0]) && std::isfinite(position[1]) &&
                std::isfinite(position[2])) {
                auto *live = reinterpret_cast<void *>(cameraNode + kNodeWorldTranslation);
                memcpy(g_fpBoneSaved.original, live, sizeof(position));
                memcpy(g_fpBoneSaved.written, position, sizeof(position));
                g_fpBoneSaved.node = cameraNode;
                memcpy(live, position, sizeof(position));
                g_counters.fpBoneWrites.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void FirstPersonRestore() {
    __try {
        if (g_fpProbeNode != 0) {
            const bool changed =
                memcmp(reinterpret_cast<const void *>(g_fpProbeNode + kNodeWorldTransform),
                       g_fpProbeWorld.value, sizeof(g_fpProbeWorld.value)) != 0;
            (changed ? g_counters.fpWorldRecomputed : g_counters.fpWorldUnchanged)
                .fetch_add(1, std::memory_order_relaxed);
        }
        // A world transform still holding what was written was used as it
        // is and gets the engine's back; one the render recomputed is left.
        for (size_t index = g_fpWorldSavedCount; index-- > 0;) {
            const SavedNodePose &saved = g_fpWorldSaved[index];
            auto *live = reinterpret_cast<void *>(saved.node + kNodeWorldTransform);
            if (memcmp(live, saved.written.value, sizeof(saved.written.value)) != 0) {
                g_counters.fpWorldOverwritten.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            memcpy(live, saved.pose.value, sizeof(saved.pose.value));
            g_counters.fpWorldKept.fetch_add(1, std::memory_order_relaxed);
        }
        g_fpWorldSavedCount = 0;
        if (g_fpBoneSaved.node != 0) {
            auto *bone = reinterpret_cast<void *>(g_fpBoneSaved.node + kNodeWorldTranslation);
            if (memcmp(bone, g_fpBoneSaved.written, sizeof(g_fpBoneSaved.written)) == 0) {
                memcpy(bone, g_fpBoneSaved.original, sizeof(g_fpBoneSaved.original));
            }
            g_fpBoneSaved.node = 0;
        }
        for (size_t index = g_fpSavedCount; index-- > 0;) {
            const SavedNodePose &saved = g_fpSaved[index];
            auto *live = reinterpret_cast<void *>(saved.node + kNodeLocalTransform);
            if (memcmp(live, saved.written.value, sizeof(saved.written.value)) != 0) {
                g_counters.fpRestoreConflicts.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            memcpy(live, saved.pose.value, sizeof(saved.pose.value));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
    g_fpSavedCount = 0;
    g_fpPendingCount = 0;
    g_fpWorldSavedCount = 0;
    g_fpProbeNode = 0;
    g_fpAdvanced = false;
}

// In first person the scene camera sits on the camera bone. Crouching,
// standing up and view animations move that bone on ticks; looking up or
// down moves it every frame through nodes that are not blended. The camera's
// correction is the bone's position worked out with the blended locals minus
// the same with the engine's locals, so the per-frame part cancels. Must run
// while the engine's locals are in place, so it uses the pending table and
// the saved engine bytes, not the live (already blended) locals.
const float *EngineLocal(uintptr_t node, float *buffer) {
    for (size_t index = 0; index < g_fpSavedCount; ++index) {
        if (g_fpSaved[index].node == node) return g_fpSaved[index].pose.value;
    }
    memcpy(buffer, reinterpret_cast<const void *>(node + kNodeLocalTransform),
           sizeof(float) * 13);
    return buffer;
}
const float *BlendedLocal(uintptr_t node, const float *engineLocal, bool &anyBlended) {
    for (size_t index = 0; index < g_fpSavedCount; ++index) {
        if (g_fpSaved[index].node == node) {
            anyBlended = true;
            return g_fpSaved[index].written.value;
        }
    }
    return engineLocal;
}
// The blended locals only show if something recomputes the world transforms
// from them before the pass draws. The render does that on most frames; on
// others (seen while the view moves, and suspected while crouching and
// aiming) the skeleton's world transforms are the ones the engine computed
// from its own locals earlier in the frame, and the arms show the tick's pose.
// So every node below a blended one gets the world transform its blended
// chain gives, worked out from the root down. Each node is checked against
// its parent's world in memory: where its world is parent * local, it is
// derived from its local; where the engine has adjusted it afterwards (seen
// on Bip01 Spine2, which a check against the chain from the root therefore
// failed for the whole arms and weapon below it), the adjustment is carried
// over onto the blended pose; a world not related to the parent at all is
// left alone, and so are its children's positions relative to it. Restored
// after the composite unless the render overwrote it.
bool WorldMatchesEngineChain(const float *live, const NodePose &computed) {
    for (size_t index = 0; index < kPoseFloats; ++index) {
        const float expected = computed.value[index];
        const float tolerance = (index >= 9 && index < 12)
            ? 0.05f + std::fabs(expected) * 2.0e-6f : 2.0e-4f;
        if (!(std::fabs(live[index] - expected) <= tolerance)) return false;
    }
    return true;
}

// Diagnostic: one table of the whole first-person tree for a render, taken a
// few times after the aim button changes. Per node: depth, parent row, name,
// flags, and how far its world transform in memory is from (a) the engine
// chain from the root, (b) its parent's world in memory times its own engine
// local, (c) the same with its blended local. Written by the main thread,
// printed by the log thread.
struct WorldDumpRow {
    char name[24];
    uint16_t parent;
    uint8_t depth;
    uint8_t decision; // 0 not below a blend, 1 derived, 2 adjustment carried over, 3 left alone
    uint8_t blended;  // its own local is blended
    uint8_t isNode;
    uint8_t controller;
    uint32_t flags;
    float chain[2];        // translation, rotation distance of what is shown
    float parentEngine[2]; // ... from live parent * engine local
    float parentBlend[2];  // ... from live parent * blended local
};
WorldDumpRow g_worldDump[1024];
uint32_t g_worldDumpCount = 0;
uint32_t g_worldDumpSerial = 0;
uint8_t g_worldDumpKind = 0;
float g_worldDumpAlpha = 0.0f;
std::atomic<bool> g_worldDumpReady{false};
bool g_worldDumpEnabled = false;
uint32_t g_worldDumpCountdown = 0; // renders until the next dump; 0 = none pending
uint32_t g_worldDumpsLeft = 12;
uint32_t g_worldDumpsTaken = 0;

void TransposeRotation(const float *in, float *out);

void PoseDistance(const float *live, const NodePose &expected, float out[2]) {
    out[0] = out[1] = 0.0f;
    for (size_t index = 0; index < kPoseFloats; ++index) {
        const float difference = std::fabs(live[index] - expected.value[index]);
        float &largest = (index >= 9 && index < 12) ? out[0] : out[1];
        if (!(difference <= largest)) largest = difference;
    }
}

void WriteFirstPersonWorlds() {
    g_fpWorldSavedCount = 0;
    if (g_fpSavedCount == 0 || g_fpRoot < 0x10000) return;
    struct Entry {
        NodePose liveBefore; // the world in memory before this function wrote anything
        NodePose blended;    // the world to show
        bool moved;          // a blended local on the way from the root
    };
    bool dumping = false;
    if (g_worldDumpEnabled && g_worldDumpCountdown != 0 && --g_worldDumpCountdown == 0 &&
        !g_worldDumpReady.load(std::memory_order_acquire)) {
        dumping = true;
        g_worldDumpCount = 0;
    }
    static Entry entries[1024];
    static struct {
        uintptr_t object;
        uint16_t parent;
    } stack[512];
    size_t entryCount = 1;
    memcpy(entries[0].liveBefore.value,
           reinterpret_cast<const void *>(g_fpRoot + kNodeWorldTransform),
           sizeof(entries[0].liveBefore.value));
    entries[0].blended = entries[0].liveBefore;
    entries[0].moved = false;
    static uint8_t depths[1024];
    static uint16_t dumpRows[1024]; // entry index -> dump row
    depths[0] = 0;
    dumpRows[0] = 0xFFFF;
    size_t top = 0;
    const auto pushChildren = [&](uintptr_t node, uint16_t index) {
        if (!IsSceneNode(node)) return;
        const uintptr_t children = *reinterpret_cast<const uintptr_t *>(node + kNodeChildren);
        const uint32_t count = *reinterpret_cast<const uint16_t *>(node + kNodeChildCount);
        if (children < 0x10000 || count > 1024) return;
        for (uint32_t child = 0; child < count && top < 512; ++child) {
            stack[top].object = *reinterpret_cast<const uintptr_t *>(children + child * 4);
            stack[top].parent = index;
            ++top;
        }
    };
    pushChildren(g_fpRoot, 0);
    while (top != 0 && entryCount < 1024) {
        const auto item = stack[--top];
        const uintptr_t object = item.object;
        if (object < 0x10000) continue;
        const Entry &parent = entries[item.parent];
        Entry &entry = entries[entryCount];
        const uint16_t index = static_cast<uint16_t>(entryCount++);
        pushChildren(object, index);
        float buffer[13];
        const float *engineLocal = EngineLocal(object, buffer);
        bool blendedHere = false;
        const float *blendedLocal = BlendedLocal(object, engineLocal, blendedHere);
        auto *live = reinterpret_cast<float *>(object + kNodeWorldTransform);
        memcpy(entry.liveBefore.value, live, sizeof(entry.liveBefore.value));
        entry.moved = parent.moved || blendedHere;

        // What the engine's own rule gives from the parent's world in
        // memory: if the node's world is that, it is derived from its local.
        // If not, the engine has adjusted it after (Bip01 Spine2 carries
        // such an adjustment): the adjustment is kept, as a transform
        // applied after parent * local, on top of the blended pose.
        NodePose local;
        NodePose expected;
        memcpy(local.value, engineLocal, sizeof(local.value));
        ComposePose(parent.liveBefore, local, expected);
        const bool derived = WorldMatchesEngineChain(live, expected);
        uint8_t decision = 0;
        if (!entry.moved) {
            entry.blended = entry.liveBefore;
        } else {
            NodePose blendedPose;
            memcpy(local.value, blendedLocal, sizeof(local.value));
            ComposePose(parent.blended, local, blendedPose);
            if (derived) {
                entry.blended = blendedPose;
                decision = 1;
            } else {
                // adjustment = expected^-1 * live
                NodePose inverse;
                const float scale = expected.value[12];
                bool usable = std::isfinite(scale) && std::fabs(scale) > 1.0e-6f;
                if (usable) {
                    TransposeRotation(expected.value, inverse.value);
                    inverse.value[12] = 1.0f / scale;
                    for (int row = 0; row < 3; ++row) {
                        inverse.value[9 + row] =
                            -(inverse.value[row * 3 + 0] * expected.value[9] +
                              inverse.value[row * 3 + 1] * expected.value[10] +
                              inverse.value[row * 3 + 2] * expected.value[11]) /
                            scale;
                    }
                    NodePose adjustment;
                    ComposePose(inverse, entry.liveBefore, adjustment);
                    // An adjustment that moves the node far from where its
                    // parent puts it means the world is not related to the
                    // parent at all: then the node is left where it is.
                    usable = std::fabs(adjustment.value[9]) <= 50.0f &&
                             std::fabs(adjustment.value[10]) <= 50.0f &&
                             std::fabs(adjustment.value[11]) <= 50.0f;
                    if (usable) {
                        ComposePose(blendedPose, adjustment, entry.blended);
                        decision = 2;
                        g_counters.fpWorldAdjusted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (!usable) {
                    entry.blended = entry.liveBefore;
                    decision = 3;
                    g_counters.fpWorldSkipped.fetch_add(1, std::memory_order_relaxed);
                    float distance = 0.0f;
                    for (size_t axis = 9; axis < 12; ++axis) {
                        const float difference = std::fabs(live[axis] - expected.value[axis]);
                        if (!(difference <= distance)) distance = difference;
                    }
                    AtomicMax(g_counters.fpWorldSkippedMilli,
                              std::isfinite(distance) && distance < 4.0e6f
                                  ? static_cast<uint32_t>(distance * 1000.0f) : 0xFFFFFFFFu);
                }
            }
        }

        depths[index] =
            static_cast<uint8_t>(depths[item.parent] < 255 ? depths[item.parent] + 1 : 255);
        dumpRows[index] = 0xFFFF;
        if (dumping && g_worldDumpCount < 1024) {
            dumpRows[index] = static_cast<uint16_t>(g_worldDumpCount);
            WorldDumpRow *row = &g_worldDump[g_worldDumpCount++];
            *row = {};
            row->parent = dumpRows[item.parent];
            row->depth = depths[index];
            row->blended = blendedHere ? 1 : 0;
            row->decision = decision;
            row->isNode = IsSceneNode(object) ? 1 : 0;
            row->controller =
                *reinterpret_cast<const uintptr_t *>(object + kNodeController) >= 0x10000 ? 1 : 0;
            row->flags = *reinterpret_cast<const uint32_t *>(object + 0x30);
            const uintptr_t text = *reinterpret_cast<const uintptr_t *>(object + kNodeName);
            if (text >= 0x10000) {
                const char *source = reinterpret_cast<const char *>(text);
                for (size_t length = 0; length + 1 < sizeof(row->name) && source[length] != 0;
                     ++length) {
                    const char c = source[length];
                    row->name[length] = (c > 0x20 && c < 0x7F && c != ',') ? c : '_';
                }
            }
            PoseDistance(live, entry.blended, row->chain);
            PoseDistance(live, expected, row->parentEngine);
            NodePose composed;
            memcpy(local.value, blendedLocal, sizeof(local.value));
            ComposePose(parent.liveBefore, local, composed);
            PoseDistance(live, composed, row->parentBlend);
        }

        if (decision == 0 || decision == 3) continue;
        bool finite = true;
        for (const float component : entry.blended.value) finite = finite && std::isfinite(component);
        if (!finite) {
            entry.blended = entry.liveBefore;
            continue;
        }
        if (g_fpWorldSavedCount >= 1024) continue;
        if (memcmp(live, entry.blended.value, sizeof(entry.blended.value)) == 0) continue;
        SavedNodePose &saved = g_fpWorldSaved[g_fpWorldSavedCount++];
        saved.node = object;
        memcpy(saved.pose.value, live, sizeof(saved.pose.value));
        saved.written = entry.blended;
        memcpy(live, entry.blended.value, sizeof(entry.blended.value));
        g_counters.fpWorldWrites.fetch_add(1, std::memory_order_relaxed);
    }
    if (dumping) {
        g_worldDumpSerial = g_fpSerial;
        g_worldDumpKind = g_fpPreviousFrameWasTick ? 1 : 0;
        g_worldDumpAlpha = static_cast<float>(g_fpPreviousFrameAlpha);
        ++g_worldDumpsTaken;
        g_worldDumpReady.store(true, std::memory_order_release);
    }
}

void FlushWorldDump() {
    if (!g_worldDumpReady.load(std::memory_order_acquire)) return;
    AppendStatus("world dump %u: serial=%u tick_frame=%u alpha=%.3f nodes=%u (columns: row parent "
                 "depth name node controller flags own_blend decision[0 not below a blend, 1 "
                 "derived from its local, 2 engine adjustment carried over, 3 left alone] | "
                 "shown t r | parent*engine t r | parent*blend t r)",
                 g_worldDumpsTaken, g_worldDumpSerial, g_worldDumpKind, g_worldDumpAlpha,
                 g_worldDumpCount);
    for (uint32_t index = 0; index < g_worldDumpCount; ++index) {
        const WorldDumpRow &row = g_worldDump[index];
        AppendStatus("wd %u %d %u %s %u %u %08X %u %u | %.3f %.5f | %.3f %.5f | %.3f %.5f", index,
                     row.parent == 0xFFFF ? -1 : static_cast<int>(row.parent), row.depth,
                     row.name[0] != 0 ? row.name : "-", row.isNode, row.controller, row.flags,
                     row.blended, row.decision, row.chain[0], row.chain[1], row.parentEngine[0],
                     row.parentEngine[1], row.parentBlend[0], row.parentBlend[1]);
    }
    g_worldDumpReady.store(false, std::memory_order_release);
}

bool ComputeCameraBoneDelta(const float *cameraPosition, float *delta) {
    delta[0] = delta[1] = delta[2] = 0.0f;
    if (!g_fpAdvanced || g_fpSavedCount == 0) return false;
    __try {
        const uintptr_t rootNode = g_fpRoot;
        const uintptr_t cameraNode = g_fpCameraBone;
        if (rootNode < 0x10000 || cameraNode < 0x10000 || cameraNode == rootNode) return false;
        float enginePosition[3] = {0.0f, 0.0f, 0.0f};
        float blendedPosition[3] = {0.0f, 0.0f, 0.0f};
        bool anyBlended = false;
        bool reachedRoot = false;
        bool first = true;
        uintptr_t node = cameraNode;
        for (int depth = 0; depth < 32 && node >= 0x10000; ++depth) {
            if (node == rootNode) {
                reachedRoot = true;
                break;
            }
            float buffer[13];
            const float *engineLocal = EngineLocal(node, buffer);
            const float *blendedLocal = BlendedLocal(node, engineLocal, anyBlended);
            if (first) {
                for (int axis = 0; axis < 3; ++axis) {
                    enginePosition[axis] = engineLocal[9 + axis];
                    blendedPosition[axis] = blendedLocal[9 + axis];
                }
                first = false;
            } else {
                float moved[3];
                for (int row = 0; row < 3; ++row) {
                    moved[row] = (engineLocal[row * 3 + 0] * enginePosition[0] +
                                  engineLocal[row * 3 + 1] * enginePosition[1] +
                                  engineLocal[row * 3 + 2] * enginePosition[2]) *
                                     engineLocal[12] + engineLocal[9 + row];
                }
                memcpy(enginePosition, moved, sizeof(moved));
                for (int row = 0; row < 3; ++row) {
                    moved[row] = (blendedLocal[row * 3 + 0] * blendedPosition[0] +
                                  blendedLocal[row * 3 + 1] * blendedPosition[1] +
                                  blendedLocal[row * 3 + 2] * blendedPosition[2]) *
                                     blendedLocal[12] + blendedLocal[9 + row];
                }
                memcpy(blendedPosition, moved, sizeof(moved));
            }
            node = *reinterpret_cast<const uintptr_t *>(node + kNodeParent);
        }
        if (!reachedRoot) return false;

        float root[13];
        memcpy(root, reinterpret_cast<const void *>(rootNode + kNodeWorldTransform),
               sizeof(root));
        float result[3];
        float mismatch = 0.0f;
        for (int row = 0; row < 3; ++row) {
            const float engineWorld = (root[row * 3 + 0] * enginePosition[0] +
                                       root[row * 3 + 1] * enginePosition[1] +
                                       root[row * 3 + 2] * enginePosition[2]) *
                                          root[12] + root[9 + row];
            const float distance = std::fabs(engineWorld - cameraPosition[row]);
            if (!(distance <= mismatch)) mismatch = distance;
            result[row] = (root[row * 3 + 0] * (blendedPosition[0] - enginePosition[0]) +
                           root[row * 3 + 1] * (blendedPosition[1] - enginePosition[1]) +
                           root[row * 3 + 2] * (blendedPosition[2] - enginePosition[2])) *
                              root[12];
        }
        // The camera has to be where the engine's bone is, or it is not
        // following the bone on this frame.
        if (!(mismatch <= 0.05f)) {
            g_counters.boneDeltaMismatch.fetch_add(1, std::memory_order_relaxed);
            AtomicMax(g_counters.boneMismatchMilli,
                      std::isfinite(mismatch) && mismatch < 4.0e6f
                          ? static_cast<uint32_t>(mismatch * 1000.0f) : 0xFFFFFFFFu);
            return false;
        }
        if (!anyBlended) return false;
        for (int row = 0; row < 3; ++row) {
            if (!(std::fabs(result[row]) <= 30.0f)) return false;
        }
        if (std::fabs(result[0]) <= 0.0001f && std::fabs(result[1]) <= 0.0001f &&
            std::fabs(result[2]) <= 0.0001f) {
            return false;
        }
        memcpy(delta, result, sizeof(result));
        g_counters.boneDeltaApplied.fetch_add(1, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
        delta[0] = delta[1] = delta[2] = 0.0f;
        return false;
    }
}

// The scene camera also takes its orientation from the camera bone (reload
// and landing animations turn it). For the main world draw the camera's
// rotation is multiplied by the difference between the bone's world rotation
// with the blended locals and with the engine's. Mouse look reaches the
// camera through locals that are not blended and cancels out.
void MultiplyRotation(const float *left, const float *right, float *out) {
    float result[9];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result[row * 3 + column] = left[row * 3 + 0] * right[0 * 3 + column] +
                                       left[row * 3 + 1] * right[1 * 3 + column] +
                                       left[row * 3 + 2] * right[2 * 3 + column];
        }
    }
    memcpy(out, result, sizeof(result));
}

void TransposeRotation(const float *in, float *out) {
    float result[9];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) result[row * 3 + column] = in[column * 3 + row];
    }
    memcpy(out, result, sizeof(result));
}

struct SavedCameraRotation {
    uintptr_t camera;
    float original[9];
    float written[9];
};
SavedCameraRotation g_cameraRotationSaved{};
float g_cameraBoneRelation[9]{};
bool g_cameraBoneRelationValid = false;

bool ApplyCameraAnimationRotation(uintptr_t camera) {
    g_cameraRotationSaved.camera = 0;
    if (!g_fpAdvanced) return false;
    __try {
        const uintptr_t rootNode = g_fpRoot;
        const uintptr_t cameraNode = g_fpCameraBone;
        if (rootNode < 0x10000 || cameraNode < 0x10000 || cameraNode == rootNode) return false;
        float engineChain[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        float blendedChain[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        bool anyBlended = false;
        bool reachedRoot = false;
        uintptr_t node = cameraNode;
        for (int depth = 0; depth < 32 && node >= 0x10000; ++depth) {
            if (node == rootNode) {
                reachedRoot = true;
                break;
            }
            float buffer[13];
            const float *engineLocal = EngineLocal(node, buffer);
            const float *blendedLocal = BlendedLocal(node, engineLocal, anyBlended);
            MultiplyRotation(engineLocal, engineChain, engineChain);
            MultiplyRotation(blendedLocal, blendedChain, blendedChain);
            node = *reinterpret_cast<const uintptr_t *>(node + kNodeParent);
        }
        if (!reachedRoot) return false;

        float rootRotation[9];
        memcpy(rootRotation, reinterpret_cast<const void *>(rootNode + kNodeWorldTransform),
               sizeof(rootRotation));
        float engineWorld[9], blendedWorld[9], engineWorldInverse[9];
        MultiplyRotation(rootRotation, engineChain, engineWorld);
        MultiplyRotation(rootRotation, blendedChain, blendedWorld);
        TransposeRotation(engineWorld, engineWorldInverse);

        SavedCameraRotation &saved = g_cameraRotationSaved;
        memcpy(saved.original, reinterpret_cast<const void *>(camera + kNodeWorldTransform),
               sizeof(saved.original));
        // The camera is expected to be the bone's rotation times a constant.
        // If that relation jumps, the camera is not following the bone (or
        // not yet), and nothing is written on this frame. It also moves by
        // small amounts from one frame to the next while the view turns, more
        // the longer the frame (about 5 % of renders at 300 FPS, 7 % at 175
        // in the logs). Skipping such a frame shows the engine's rotation on
        // it: a one-frame twitch of the view and the arms as large as the
        // blend. With few draws per tick those frames are written like any
        // other and only a jump skips one.
        float relation[9];
        MultiplyRotation(engineWorldInverse, saved.original, relation);
        float deviation = 0.0f;
        for (int index = 0; index < 9; ++index) {
            if (!std::isfinite(relation[index])) return false;
            const float difference = std::fabs(relation[index] - g_cameraBoneRelation[index]);
            if (difference > deviation) deviation = difference;
        }
        if (!g_cameraBoneRelationValid || deviation > 0.002f) {
            const bool jump = !g_cameraBoneRelationValid || !g_fewDrawsPerTick ||
                              !(deviation <= 0.1f);
            if (g_cameraBoneRelationValid) {
                (jump ? g_counters.cameraRelationChanges : g_counters.cameraRelationDrift)
                    .fetch_add(1, std::memory_order_relaxed);
                AtomicMax(g_counters.cameraRelationMicro,
                          std::isfinite(deviation) && deviation < 4.0e3f
                              ? static_cast<uint32_t>(deviation * 1.0e6f) : 0xFFFFFFFFu);
            }
            memcpy(g_cameraBoneRelation, relation, sizeof(relation));
            g_cameraBoneRelationValid = true;
            if (jump) return false;
        }
        if (!anyBlended) return false;

        float difference[9];
        MultiplyRotation(blendedWorld, engineWorldInverse, difference);
        MultiplyRotation(difference, saved.original, saved.written);
        for (int index = 0; index < 9; ++index) {
            if (!std::isfinite(saved.written[index])) return false;
        }
        if (memcmp(saved.written, saved.original, sizeof(saved.written)) == 0) return false;
        memcpy(reinterpret_cast<void *>(camera + kNodeWorldTransform), saved.written,
               sizeof(saved.written));
        saved.camera = camera;
        g_counters.cameraRotationWrites.fetch_add(1, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
        g_cameraRotationSaved.camera = 0;
        return false;
    }
}

void RestoreCameraAnimationRotation() {
    SavedCameraRotation &saved = g_cameraRotationSaved;
    if (saved.camera == 0) return;
    __try {
        auto *live = reinterpret_cast<void *>(saved.camera + kNodeWorldTransform);
        if (memcmp(live, saved.written, sizeof(saved.written)) == 0) {
            memcpy(live, saved.original, sizeof(saved.original));
        } else {
            g_counters.cameraRotationConflicts.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
    saved.camera = 0;
}

// The first-person pass draws the arms with the scene camera, which by then
// has the engine's transform back. The arms are posed from the blended locals
// about the engine's root, so the camera they need is the blended camera bone
// about the same root: the engine's camera plus the bone's blended-minus-
// engine position, with the blended rotation, and without the player's
// position correction (the arms do not carry it). Without this the arms are
// seen from the tick's camera bone, and every tick-driven move of that bone
// (crouching, aiming) shows as a step of the arms against the screen.
// The pass sets its field of view through FUN_00C52020, which ends with an
// update of the camera node (FUN_00A59C60): the world transform is recomputed
// from the local one, and a world-only write would be lost before the draw.
// So the local transform is changed as well, to the one that gives the
// blended world transform under the same parent.
struct FirstPassCamera {
    bool valid;
    uintptr_t camera;
    float engine[12];  // world rotation and translation as the main draw left them
    float written[12];
    float localEngine[12];
    float localWritten[12];
    bool local;        // the local transform was written too
};
FirstPassCamera g_fpPassCamera{};

bool BeginFirstPersonPassCamera() {
    FirstPassCamera &pass = g_fpPassCamera;
    if (!pass.valid || !g_hooksInstalled.load(std::memory_order_relaxed) ||
        GetCurrentThreadId() != g_mainThreadId) {
        return false;
    }
    __try {
        auto *live = reinterpret_cast<float *>(pass.camera + kNodeWorldTransform);
        if (memcmp(live, pass.engine, sizeof(pass.engine)) != 0) {
            g_counters.fpPassCameraSkipped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Under the same parent P: world = P * local, so P's rotation is
        // W.R * L.R^T and its scale W.s / L.s. The local that gives the
        // written world is L.R * W.R^T * R' for the rotation and
        // L.t + (L.s / W.s) * L.R * W.R^T * (t' - t) for the translation.
        auto *local = reinterpret_cast<float *>(pass.camera + kNodeLocalTransform);
        const float worldScale = live[12];
        const float localScale = local[12];
        pass.local = false;
        if (std::isfinite(worldScale) && std::isfinite(localScale) &&
            std::fabs(worldScale) > 1.0e-6f) {
            float worldInverse[9];
            float toParent[9];
            TransposeRotation(pass.engine, worldInverse);
            MultiplyRotation(local, worldInverse, toParent);
            float result[12];
            MultiplyRotation(toParent, pass.written, result);
            const float move[3] = {pass.written[9] - pass.engine[9],
                                   pass.written[10] - pass.engine[10],
                                   pass.written[11] - pass.engine[11]};
            const float ratio = localScale / worldScale;
            bool finite = true;
            for (int row = 0; row < 3; ++row) {
                result[9 + row] = local[9 + row] +
                    ratio * (toParent[row * 3 + 0] * move[0] + toParent[row * 3 + 1] * move[1] +
                             toParent[row * 3 + 2] * move[2]);
            }
            for (const float component : result) finite = finite && std::isfinite(component);
            if (finite) {
                memcpy(pass.localEngine, local, sizeof(pass.localEngine));
                memcpy(pass.localWritten, result, sizeof(pass.localWritten));
                memcpy(local, result, sizeof(result));
                pass.local = true;
            }
        }
        memcpy(live, pass.written, sizeof(pass.written));
        g_counters.fpPassCameraWrites.fetch_add(1, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

void EndFirstPersonPassCamera() {
    const FirstPassCamera &pass = g_fpPassCamera;
    __try {
        auto *live = reinterpret_cast<float *>(pass.camera + kNodeWorldTransform);
        if (pass.local) {
            // The world transform may have been recomputed from the written
            // local in the pass; it is the engine's again once the local is.
            auto *local = reinterpret_cast<float *>(pass.camera + kNodeLocalTransform);
            if (memcmp(local, pass.localWritten, sizeof(pass.localWritten)) == 0) {
                memcpy(local, pass.localEngine, sizeof(pass.localEngine));
                memcpy(live, pass.engine, sizeof(pass.engine));
                if (memcmp(live, pass.written, sizeof(pass.written)) != 0) {
                    g_counters.fpPassCameraUpdated.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                g_counters.fpPassCameraConflicts.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (memcmp(live, pass.written, sizeof(pass.written)) == 0) {
            memcpy(live, pass.engine, sizeof(pass.engine));
        } else {
            g_counters.fpPassCameraConflicts.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void __fastcall FirstPersonPassHook(void *self, void * /*edx*/, uint32_t arg0) {
    using PassFn = void(__thiscall *)(void *, uint32_t);
    const auto stock = reinterpret_cast<PassFn>(kFirstPersonPassSites[0].target);
    const bool wrote = BeginFirstPersonPassCamera();
    stock(self, arg0);
    if (wrote) EndFirstPersonPassCamera();
}

void __fastcall FirstPersonDrawHook(void *self, void * /*edx*/, uint32_t arg0, uint32_t arg1,
                                    uint32_t arg2, uint32_t arg3) {
    using DrawFn = void(__thiscall *)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
    const auto stock = reinterpret_cast<DrawFn>(kFirstPersonPassSites[1].target);
    const bool wrote = BeginFirstPersonPassCamera();
    stock(self, arg0, arg1, arg2, arg3);
    if (wrote) EndFirstPersonPassCamera();
}

const void *const kFirstPersonPassHooks[kFirstPersonPassSiteCount] = {
    reinterpret_cast<const void *>(&FirstPersonPassHook),
    reinterpret_cast<const void *>(&FirstPersonDrawHook),
    reinterpret_cast<const void *>(&FirstPersonDrawHook),
};

// ---- Other characters -----------------------------------------------------------
// Every node below the 3D root of each actor in the process manager's high
// list gets a from/to history of its complete world transform. World
// transforms change on ticks, but the draw first sees a new pose some draws
// after the tick draw (one on frozen frames in Fallout 3), so each history
// records how many draws late its pose arrived and is shown with the phase
// of the draw that many back. Written before the main world draw, exact
// bytes put back after it. Local transforms and the references are not
// touched.
struct NodePoseHistory {
    uintptr_t node;
    uint32_t lastSeen;
    uint32_t changePeriod;   // tick period the from/to pair belongs to
    uint8_t lagDraws;        // draws after the tick draw at which it arrived
    uint8_t changesInPeriod; // large changes seen in changePeriod
    uint32_t perFrameUntil;  // tick period up to which it is shown as is
    NodePose from;
    NodePose to;
    NodePose periodFrom;     // `to` as it was before changePeriod began
};
constexpr size_t kPoseTableSize = 65536; // power of two
constexpr uint32_t kMaxLagDraws = 3;
NodePoseHistory g_poseTable[kPoseTableSize]{};
// Copy of the draw hook's count of consecutive eligible draws, for the
// object walk's "nothing can have moved" shortcut.
uint32_t g_cameraEligibleDrawsForObjects = 0;
size_t g_poseTableUsed = 0;
uint32_t g_poseDrawSerial = 1;
uint32_t g_poseTickCount = 1;
uint32_t g_poseDrawsSinceTick = 0;
double g_poseAlphaRing[8]{};
uint32_t g_posePeriodRing[8]{};
bool g_posePreviousDrawWasTick = false;
SavedNodePose g_actorSaved[16384];
size_t g_actorSavedCount = 0;

void ResetActorInterpolation() {
    if (g_poseTableUsed == 0) return;
    for (NodePoseHistory &entry : g_poseTable) entry.node = 0;
    g_poseTableUsed = 0;
}

NodePoseHistory *FindPoseHistory(uintptr_t node) {
    size_t slot = (node >> 4) & (kPoseTableSize - 1);
    for (size_t probe = 0; probe < 64; ++probe) {
        NodePoseHistory &entry = g_poseTable[slot];
        if (entry.node == node) return &entry;
        if (entry.node == 0) {
            entry.node = node;
            entry.lastSeen = 0;
            ++g_poseTableUsed;
            return &entry;
        }
        slot = (slot + 1) & (kPoseTableSize - 1);
    }
    return nullptr;
}

void InterpolateSubtree(uintptr_t object, bool tick, bool interpolate, uint32_t depth) {
    if (object < 0x10000 || depth > 48) return;
    NodePose current;
    memcpy(current.value, reinterpret_cast<const void *>(object + kNodeWorldTransform),
           sizeof(current.value));
    NodePoseHistory *history = FindPoseHistory(object);
    if (history != nullptr && history->lastSeen == g_poseDrawSerial) {
        // Reached a second time in the same draw. Its current bytes are the
        // blended pose, not the engine's.
        g_counters.actorDuplicates.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (history != nullptr) {
        const bool continuous = history->lastSeen + 1 == g_poseDrawSerial;
        if (!continuous) {
            history->from = current;
            history->to = current;
            history->periodFrom = current;
            history->changePeriod = 0;
            history->changesInPeriod = 0;
            history->perFrameUntil = 0;
            history->lagDraws = 1;
        } else if (memcmp(history->to.value, current.value, sizeof(current.value)) != 0) {
            float rotationStep = 0.0f;
            float translationStep = 0.0f;
            for (size_t component = 0; component < kPoseFloats; ++component) {
                const float difference =
                    std::fabs(current.value[component] - history->to.value[component]);
                float &largest = (component >= 9 && component < 12) ? translationStep
                                                                    : rotationStep;
                if (!(difference <= largest)) largest = difference;
            }
            // How many draws back the tick draw is that this pose belongs
            // to. A pose seen on the draw after a tick draw belongs to that
            // tick even when this draw is a tick draw too.
            const uint32_t lagDraws = tick ? 1 : g_poseDrawsSinceTick;
            const bool nearTick = lagDraws <= kMaxLagDraws;
            const uint32_t posePeriod =
                nearTick ? g_posePeriodRing[(g_poseDrawSerial - lagDraws) & 7] : 0;
            const bool newPeriod = history->changePeriod != posePeriod;
            const bool perFrame = history->perFrameUntil > g_poseTickCount;
            // After a pose has been taken for this tick, later small changes
            // are let through: the blend carries on and the next tick's pose
            // includes them.
            const bool minor = !newPeriod && rotationStep <= 0.02f && translationStep <= 0.2f;
            if (minor) {
                g_counters.actorMinor.fetch_add(1, std::memory_order_relaxed);
            } else if (nearTick && !perFrame) {
                // This tick's pose: the first change within three draws of
                // the tick draw, or a later one replacing it.
                if (newPeriod) {
                    history->periodFrom = history->to;
                    history->changesInPeriod = 0;
                }
                if (history->changesInPeriod != 0xFF) ++history->changesInPeriod;
                history->from = history->periodFrom;
                history->to = current;
                history->changePeriod = posePeriod;
                history->lagDraws = static_cast<uint8_t>(lagDraws);
                if (history->changesInPeriod >= 3) {
                    // Three large changes in one tick: driven every frame.
                    history->perFrameUntil = g_poseTickCount + 30;
                    history->from = current;
                    history->changePeriod = 0;
                }
                g_counters.actorLag[lagDraws & 3].fetch_add(1, std::memory_order_relaxed);
            } else {
                // Moved away from a tick, or driven every frame: as it is.
                history->from = current;
                history->to = current;
                history->changePeriod = 0;
                g_counters.actorAsIs.fetch_add(1, std::memory_order_relaxed);
            }
        }
        history->lastSeen = g_poseDrawSerial;

        float stepSquared = 0.0f;
        for (size_t axis = 9; axis < 12; ++axis) {
            const float delta = history->to.value[axis] - history->from.value[axis];
            stepSquared += delta * delta;
        }
        if (!(stepSquared <= 900.0f)) {
            // More than 30 units in one tick (teleport, fast fall): no blend.
            history->from = history->to;
            history->changePeriod = 0;
            g_counters.actorSnaps.fetch_add(1, std::memory_order_relaxed);
        }

        // The pair is shown with the alpha of the draw `lagDraws` back, for
        // as long as that draw belongs to the pair's tick period.
        const uint32_t laggedDraw = (g_poseDrawSerial - history->lagDraws) & 7;
        const bool active = history->changePeriod != 0 &&
                            history->changePeriod == g_posePeriodRing[laggedDraw];
        NodePose shown;
        if (interpolate && active &&
            memcmp(history->from.value, history->to.value, sizeof(current.value)) != 0 &&
            BlendPose(history->from, history->to, g_poseAlphaRing[laggedDraw], shown)) {
            if (g_actorSavedCount >= 16384) {
                g_counters.actorOverflow.fetch_add(1, std::memory_order_relaxed);
            } else {
                SavedNodePose &saved = g_actorSaved[g_actorSavedCount++];
                saved.node = object;
                saved.pose = current;
                saved.written = shown;
                memcpy(reinterpret_cast<void *>(object + kNodeWorldTransform), shown.value,
                       sizeof(shown.value));
            }
        }
    }
    if (!IsSceneNode(object)) return;
    const uintptr_t children = *reinterpret_cast<const uintptr_t *>(object + kNodeChildren);
    const uint32_t count = *reinterpret_cast<const uint16_t *>(object + kNodeChildCount);
    if (children < 0x10000 || count > 1024) return;
    for (uint32_t index = 0; index < count; ++index) {
        InterpolateSubtree(*reinterpret_cast<const uintptr_t *>(children + index * 4), tick,
                           interpolate, depth + 1);
    }
}

// ---- Objects of the loaded cells ---------------------------------------------------
// Physics bodies, doors, carried and dropped items, and actors that are not
// in the high-process list. For every reference with a 3D root in the
// player's cell and the cells loaded around it, the root's world transform
// is compared with the last one seen. A reference whose root moved stays
// active for 300 walks; one whose root carries an animation controller and
// is within 6000 units is always active (a door moves child nodes under a
// root that never moves). The pose pass then blends the subtrees of the
// active ones like an actor's.
struct ObservedObject {
    uintptr_t root;
    uint32_t lastSeen;
    uint32_t activeUntil;
    NodePose pose;
};
constexpr size_t kObservedObjectsSize = 65536; // power of two
ObservedObject g_observedObjects[kObservedObjectsSize]{};
uintptr_t g_objectActiveRoots[2048];
size_t g_objectActiveCount = 0;
uint32_t g_objectSerial = 1;
bool g_objectPreviousDrawWasTick = false;
uint32_t g_objectLastEligibleDraw = 0;
uintptr_t g_objectProvenCell = 0; // the last player cell whose list passed the proof
bool g_objectListProven = false;
// Cells without an active object are read every sixth walk only.
struct CellHeat {
    uintptr_t cell;
    uint32_t hotUntil;
};
CellHeat g_cellHeat[64]{};
constexpr uint32_t kColdCellPeriod = 6;

// The list at +0xAC is accepted for a cell only if it ends and every item in
// it is a reference whose parent cell is this cell.
int CountCellListReferences(uintptr_t cell) {
    __try {
        uintptr_t node = cell + kCellReferenceList;
        int count = 0;
        for (int guard = 0; guard < 16384 && node >= 0x10000; ++guard) {
            const uintptr_t item = *reinterpret_cast<const uintptr_t *>(node);
            if (item != 0) {
                if (item < 0x10000) return -1;
                if (*reinterpret_cast<const uintptr_t *>(item + kReferenceParentCell) != cell) {
                    return -1;
                }
                ++count;
            }
            node = *reinterpret_cast<const uintptr_t *>(node + 4);
        }
        return node == 0 ? count : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The player's cell first, then the other cells of the grid. The grid is
// used only if every entry is null or an object of the same class as the
// player's cell, and the player's cell is in it (indoors it is not).
size_t CollectLoadedCells(uintptr_t playerCell, uintptr_t *cells, size_t capacity) {
    size_t count = 0;
    cells[count++] = playerCell;
    __try {
        const uintptr_t tes = *reinterpret_cast<const uintptr_t *>(kTesPointer);
        const uintptr_t grid = tes >= 0x10000 ? *reinterpret_cast<const uintptr_t *>(tes + 0x08) : 0;
        if (grid < 0x10000) return count;
        const uint32_t size = *reinterpret_cast<const uint32_t *>(grid + 0x0C);
        const uintptr_t array = *reinterpret_cast<const uintptr_t *>(grid + 0x10);
        if (size == 0 || size > 11 || array < 0x10000) return count;
        const uintptr_t cellVtable = *reinterpret_cast<const uintptr_t *>(playerCell);
        bool found = false;
        bool valid = true;
        for (uint32_t index = 0; index < size * size && valid; ++index) {
            const uintptr_t cell = *reinterpret_cast<const uintptr_t *>(array + index * 4);
            if (cell == 0) continue;
            valid = cell >= 0x10000 && *reinterpret_cast<const uintptr_t *>(cell) == cellVtable;
            if (cell == playerCell) found = true;
        }
        if (!valid) {
            g_counters.objectGridRejected.fetch_add(1, std::memory_order_relaxed);
            return count;
        }
        if (!found) return count;
        for (uint32_t index = 0; index < size * size && count < capacity; ++index) {
            const uintptr_t cell = *reinterpret_cast<const uintptr_t *>(array + index * 4);
            if (cell != 0 && cell != playerCell) cells[count++] = cell;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return count;
}

CellHeat &FindCellHeat(uintptr_t cell) {
    CellHeat *reusable = nullptr;
    for (CellHeat &entry : g_cellHeat) {
        if (entry.cell == cell) return entry;
        if (reusable == nullptr && (entry.cell == 0 || entry.hotUntil + 2000 < g_objectSerial)) {
            reusable = &entry;
        }
    }
    if (reusable == nullptr) reusable = &g_cellHeat[(cell >> 4) & 63];
    reusable->cell = cell;
    reusable->hotUntil = 0;
    return *reusable;
}

ObservedObject *FindObservedObject(uintptr_t root, bool &existing) {
    ObservedObject *reusable = nullptr;
    size_t slot = (root >> 4) & (kObservedObjectsSize - 1);
    for (size_t probe = 0; probe < 64; ++probe) {
        ObservedObject &entry = g_observedObjects[slot];
        if (entry.root == root) {
            existing = true;
            return &entry;
        }
        if (entry.root == 0) {
            existing = false;
            return reusable != nullptr ? reusable : &entry;
        }
        if (reusable == nullptr && entry.lastSeen + 4000 < g_objectSerial) reusable = &entry;
        slot = (slot + 1) & (kObservedObjectsSize - 1);
    }
    existing = false;
    return reusable;
}

void ObserveCellObjects(uintptr_t player, bool frameIsTick) {
    const bool previousWasTick = g_objectPreviousDrawWasTick;
    g_objectPreviousDrawWasTick = frameIsTick;
    const int frameKind = frameIsTick ? (previousWasTick ? 2 : 0) : (previousWasTick ? 1 : 3);
    const bool consecutive = g_cameraEligibleDrawsForObjects == g_objectLastEligibleDraw + 1;
    g_objectLastEligibleDraw = g_cameraEligibleDrawsForObjects;
    // Nothing moves on a draw that is neither a tick draw nor the one after;
    // the active list of the last walk stays in use.
    if (frameKind == 3 && consecutive) return;

    ++g_objectSerial;
    g_objectActiveCount = 0;
    g_counters.objectWalks.fetch_add(1, std::memory_order_relaxed);
    __try {
        const uintptr_t playerCell =
            *reinterpret_cast<const uintptr_t *>(player + kReferenceParentCell);
        if (playerCell < 0x10000) return;
        if (playerCell != g_objectProvenCell) {
            g_objectProvenCell = playerCell;
            g_objectListProven = CountCellListReferences(playerCell) >= 1;
            if (!g_objectListProven) {
                g_counters.objectListRejected.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!g_objectListProven) return;

        float playerPosition[3];
        memcpy(playerPosition, reinterpret_cast<const void *>(player + kReferencePosition),
               sizeof(playerPosition));
        uintptr_t cells[128];
        const size_t cellCount = CollectLoadedCells(playerCell, cells, 128);
        g_counters.objectCells.store(static_cast<uint32_t>(cellCount), std::memory_order_relaxed);

        uint32_t controlledCount = 0;
        for (size_t cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
            const uintptr_t cell = cells[cellIndex];
            CellHeat &heat = FindCellHeat(cell);
            const bool hot = cellIndex == 0 || heat.hotUntil > g_objectSerial;
            if (!hot && (cellIndex + g_objectSerial) % kColdCellPeriod != 0) continue;
            g_counters.objectCellsRead.fetch_add(1, std::memory_order_relaxed);
            bool cellHasActive = false;
            uintptr_t node = cell + kCellReferenceList;
            for (int guard = 0; guard < 16384 && node >= 0x10000; ++guard) {
                const uintptr_t reference = *reinterpret_cast<const uintptr_t *>(node);
                node = *reinterpret_cast<const uintptr_t *>(node + 4);
                if (reference == 0) continue;
                if (reference < 0x10000 ||
                    *reinterpret_cast<const uintptr_t *>(reference + kReferenceParentCell) !=
                        cell) {
                    break;
                }
                if (reference == player) continue;
                g_counters.objectReferences.fetch_add(1, std::memory_order_relaxed);
                const uintptr_t renderState =
                    *reinterpret_cast<const uintptr_t *>(reference + kReferenceRenderState);
                const uintptr_t root = renderState >= 0x10000
                    ? *reinterpret_cast<const uintptr_t *>(renderState + 0x14) : 0;
                if (root < 0x10000 || !IsSceneNode(root)) continue;
                NodePose current;
                memcpy(current.value,
                       reinterpret_cast<const void *>(root + kNodeWorldTransform),
                       sizeof(current.value));
                bool controlled =
                    *reinterpret_cast<const uintptr_t *>(root + kNodeController) >= 0x10000;
                if (controlled) {
                    controlled =
                        DistanceSquared(&current.value[9], playerPosition) <= 6000.0f * 6000.0f;
                }
                if (controlled) ++controlledCount;
                bool existing = false;
                ObservedObject *entry = FindObservedObject(root, existing);
                if (entry == nullptr) {
                    g_counters.objectTableFull.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (existing && entry->lastSeen + 2 * kColdCellPeriod >= g_objectSerial) {
                    float largest = 0.0f;
                    for (size_t component = 0; component < kPoseFloats; ++component) {
                        const float difference =
                            std::fabs(current.value[component] - entry->pose.value[component]);
                        if (difference > largest) largest = difference;
                    }
                    if (largest > 0.001f) {
                        g_counters.objectChanges[frameKind].fetch_add(1, std::memory_order_relaxed);
                        entry->activeUntil = g_objectSerial + 300;
                    }
                    if (controlled || entry->activeUntil > g_objectSerial) {
                        cellHasActive = true;
                        if (hot && g_objectActiveCount < 2048) {
                            g_objectActiveRoots[g_objectActiveCount++] = root;
                        }
                    }
                } else {
                    entry->root = root;
                    entry->activeUntil = 0;
                }
                entry->lastSeen = g_objectSerial;
                entry->pose = current;
            }
            if (cellHasActive) heat.hotUntil = g_objectSerial + 2;
        }
        g_counters.objectControlled.store(controlledCount, std::memory_order_relaxed);
        AtomicMax(g_counters.objectActiveMax, static_cast<uint32_t>(g_objectActiveCount));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
        g_objectProvenCell = 0;
        g_objectListProven = false;
        g_objectActiveCount = 0;
    }
}

void RestoreActorPoses() {
    __try {
        // Newest first, so a node saved twice ends with its first-saved bytes.
        for (size_t index = g_actorSavedCount; index-- > 0;) {
            const SavedNodePose &saved = g_actorSaved[index];
            auto *live = reinterpret_cast<void *>(saved.node + kNodeWorldTransform);
            if (memcmp(live, saved.written.value, sizeof(saved.written.value)) != 0) {
                g_counters.actorRestoreConflicts.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            memcpy(live, saved.pose.value, sizeof(saved.pose.value));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
    g_actorSavedCount = 0;
}

bool ApplyActorPoses(uintptr_t player, bool frameIsTick, double frameAlpha, bool interpolate) {
    g_actorSavedCount = 0;
    __try {
        const uintptr_t data = *reinterpret_cast<const uintptr_t *>(kProcessManager + 0x08);
        const uint32_t firstFree = *reinterpret_cast<const uint16_t *>(kProcessManager + 0x0E);
        const uint32_t begin = *reinterpret_cast<const uint32_t *>(kProcessManager + 0x14);
        const uint32_t end = *reinterpret_cast<const uint32_t *>(kProcessManager + 0x24);
        if (data < 0x10000 || begin > end || end > firstFree || end - begin > 512) {
            g_counters.actorListRejected.fetch_add(1, std::memory_order_relaxed);
            ResetActorInterpolation();
            return false;
        }
        // The proof that this is the high-process list: everything in it is
        // at process level 0.
        bool allHigh = true;
        for (uint32_t index = begin; index < end && allHigh; ++index) {
            const uintptr_t reference = *reinterpret_cast<const uintptr_t *>(data + index * 4);
            if (reference < 0x10000) continue;
            const uintptr_t process = *reinterpret_cast<const uintptr_t *>(reference + 0x68);
            allHigh = process >= 0x10000 &&
                      *reinterpret_cast<const uint32_t *>(process + 0x28) == 0;
        }
        if (!allHigh) {
            g_counters.actorNoPlayer.fetch_add(1, std::memory_order_relaxed);
            ResetActorInterpolation();
            return false;
        }
        if (g_poseTableUsed > kPoseTableSize / 2) {
            // Stale entries are never removed one by one; start over instead.
            ResetActorInterpolation();
            g_counters.actorTableResets.fetch_add(1, std::memory_order_relaxed);
        }
        const bool tick = g_posePreviousDrawWasTick;
        g_posePreviousDrawWasTick = frameIsTick;
        if (frameIsTick) {
            ++g_poseTickCount;
            g_poseDrawsSinceTick = 0;
        } else if (g_poseDrawsSinceTick < 1000) {
            ++g_poseDrawsSinceTick;
        }
        ++g_poseDrawSerial;
        g_poseAlphaRing[g_poseDrawSerial & 7] = frameAlpha;
        g_posePeriodRing[g_poseDrawSerial & 7] = g_poseTickCount;
        g_counters.actorPasses.fetch_add(1, std::memory_order_relaxed);
        if (g_interpolateObjectsEnabled) {
            ObserveCellObjects(player, frameIsTick);
        } else {
            g_objectActiveCount = 0;
        }

        for (uint32_t index = begin; index < end; ++index) {
            const uintptr_t reference = *reinterpret_cast<const uintptr_t *>(data + index * 4);
            if (reference < 0x10000 || reference == player) continue;
            const uintptr_t renderState =
                *reinterpret_cast<const uintptr_t *>(reference + kReferenceRenderState);
            const uintptr_t root = renderState >= 0x10000
                ? *reinterpret_cast<const uintptr_t *>(renderState + 0x14) : 0;
            if (root < 0x10000 || !IsSceneNode(root)) continue;
            g_counters.actorsTracked.fetch_add(1, std::memory_order_relaxed);
            InterpolateSubtree(root, tick, interpolate, 0);
        }
        // Active cell objects, after the actors so that an actor found in
        // both lists is handled once, by the loop above.
        for (size_t index = 0; index < g_objectActiveCount; ++index) {
            const uintptr_t root = g_objectActiveRoots[index];
            const NodePoseHistory *seen = FindPoseHistory(root);
            if (seen != nullptr && seen->lastSeen == g_poseDrawSerial) continue;
            g_counters.objectSubtrees.fetch_add(1, std::memory_order_relaxed);
            InterpolateSubtree(root, tick, interpolate, 0);
        }
        g_counters.actorNodesWritten.fetch_add(g_actorSavedCount, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // What was changed is still restored by the caller.
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
    return g_actorSavedCount != 0;
}

// ---- The draw ----------------------------------------------------------------

// The player's position changes on tick draws in ordinary play (measured
// in the open world). While the player dies it changes on the draw after the
// tick draw instead (measured 2026-10-15: 161 of 177 changes at 350 FPS);
// blended as if on time, it moved the body root at the wrong phase, a small
// hitch at every tick at 350 FPS and steps of whole ticks at 90. Each change
// is classified as the body's nodes are (ClassifyChange), and a late one is
// shown with the phase of the draw before.
struct PositionHistory {
    bool valid;
    bool lag;              // the last change arrived on the draw after its tick
    uint32_t changePeriod; // tick period the from/to pair belongs to
    ChangeEvidence evidence;
    float from[3];
    float to[3];
};
PositionHistory g_playerHistory{};
uint32_t g_positionSerial = 1;
uint32_t g_positionTickCount = 0;
bool g_positionPreviousWasTick = false;
double g_positionPreviousAlpha = 0.0;
uint32_t g_cameraEligibleDraws = 0;
float g_lastCameraPosition[3]{};
bool g_lastCameraPositionValid = false;
constexpr uint32_t kCameraWarmUpDraws = 60;
// A step above 150 units in one tick is a teleport and is shown at once.
constexpr float kTeleportDistanceSquared = 22500.0f;

// VATS playback, kill camera included. The camera is placed by the VATS
// camera, not by the player, and it is not under the mouse: none of the
// player-relative corrections apply, and it moves on ticks like any animated
// node. Its whole world transform gets a tick-aligned from/to pair, with the
// rules of the character pass. Measured in the first run: it changes on the
// tick draw and again on the draw after (it follows the target, whose pose
// arrives a draw late), and by some amount on most other draws. So a change
// on either draw is this tick's pose, the second replacing the first and
// shown with the previous draw's phase; a small change on any other draw is
// let through (the next tick's pose includes it); a large one means it is
// driven every frame and it is shown as it is for a while; a cut is shown at
// once. That is 1.0.1's rule, kept behind bKillCamTickPoses=0 above 150 FPS.
// The rule used now: the camera as it is on each tick draw is that tick's
// pose, and a change up to two draws after a tick draw is left to the next
// tick draw's pose. With few draws per tick the draw after a tick draw is
// often the next tick draw, where the two changes cannot be told apart by
// draw. The death camera (2026-10-15, 350 FPS) moves on the tick draw and
// one and two draws after it (318, 129 and 129 changes in 5 s, none later),
// following the dying player, whose pose arrives late; the old rule showed
// the change two draws after as it is when it was large (up to 70 units),
// and the camera stepped. A large change later than that means the camera
// moves every frame: it is then shown as it is until a draw on which it
// does not move by a large amount.
constexpr uint32_t kKillCamLateDraws = 2;
struct CameraTrack {
    bool valid;
    bool lag;
    bool tickPoses;      // the camera on each tick draw is that tick's pose
    bool perFrame;       // moves every frame: shown as it is
    uint8_t offTickScore;
    uint32_t lastSeen;
    uint32_t changePeriod;
    NodePose from;
    NodePose to;
    NodePose periodFrom; // what was shown before this tick's first change
    NodePose last;       // the camera as the draw before saw it
};
CameraTrack g_killCamTrack{};
uint32_t g_killCamSerial = 1;
uint32_t g_killCamDrawsSinceTick = 0;
uint32_t g_killCamTickCount = 0;
bool g_killCamPreviousWasTick = false;
double g_killCamPreviousAlpha = 0.0;

// Advances the player's position history with this draw's position and
// returns the position to show.
void AdvancePlayerPosition(const float current[3], bool tick, double alpha, double shown[3]) {
    const uint32_t ticksBefore = g_positionTickCount;
    if (tick) ++g_positionTickCount;
    ++g_positionSerial;
    const int kind =
        tick ? (g_positionPreviousWasTick ? 2 : 0) : (g_positionPreviousWasTick ? 1 : 3);
    const double previousAlpha = g_positionPreviousAlpha;
    g_positionPreviousWasTick = tick;
    g_positionPreviousAlpha = alpha;
    PositionHistory &history = g_playerHistory;
    if (!history.valid) {
        memcpy(history.from, current, sizeof(history.from));
        memcpy(history.to, current, sizeof(history.to));
        history.valid = true;
        history.lag = false;
        history.changePeriod = 0;
        ResetChangeEvidence(history.evidence, g_positionSerial, g_positionTickCount);
    } else if (DistanceSquared(current, history.to) > 1.0e-6f) {
        const ChangeDecision decision = ClassifyChange(history.evidence, kind, g_positionSerial);
        RecordChange(history.evidence, kind, g_positionSerial, g_positionTickCount, decision);
        const bool late = decision.late && g_latePlayerPositionEnabled;
        if (DistanceSquared(current, history.to) <= kTeleportDistanceSquared) {
            memcpy(history.from, history.to, sizeof(history.from));
        } else {
            memcpy(history.from, current, sizeof(history.from));
            g_counters.snaps.fetch_add(1, std::memory_order_relaxed);
        }
        memcpy(history.to, current, sizeof(history.to));
        history.lag = late;
        history.changePeriod = late ? ticksBefore : g_positionTickCount;
        (tick ? g_counters.playerPosesOnTick : g_counters.playerPosesOffTick)
            .fetch_add(1, std::memory_order_relaxed);
        g_counters.positionChanges[kind].fetch_add(1, std::memory_order_relaxed);
        if (late) g_counters.positionLate.fetch_add(1, std::memory_order_relaxed);
    }
    // Outside the pair's tick period (a tick that did not move the player)
    // the position is where the last change put it.
    const uint32_t period = history.lag ? ticksBefore : g_positionTickCount;
    const bool blending = history.changePeriod == period;
    const double phase = history.lag ? previousAlpha : alpha;
    for (int axis = 0; axis < 3; ++axis) {
        shown[axis] = blending ? static_cast<double>(history.from[axis]) +
                                     (static_cast<double>(history.to[axis]) - history.from[axis]) *
                                         phase
                               : static_cast<double>(history.to[axis]);
    }
}

bool InVatsPlayback() {
    return *reinterpret_cast<const uint32_t *>(kVatsCameraData + kVatsCameraMode) ==
           kVatsModePlayback;
}

// Called on every eligible draw so that the tick history is current; returns
// true with the pose to show when the camera is in a blend.
bool AdvanceKillCamTrack(uintptr_t camera, bool active, bool tick, double alpha,
                         NodePose &shown) {
    const uint32_t ticksBefore = g_killCamTickCount;
    if (tick) ++g_killCamTickCount;
    const bool previousWasTick = g_killCamPreviousWasTick;
    const double previousAlpha = g_killCamPreviousAlpha;
    g_killCamPreviousWasTick = tick;
    g_killCamPreviousAlpha = alpha;
    ++g_killCamSerial;
    if (tick) {
        g_killCamDrawsSinceTick = 0;
    } else if (g_killCamDrawsSinceTick < 1000) {
        ++g_killCamDrawsSinceTick;
    }
    CameraTrack &track = g_killCamTrack;
    if (!active) {
        track.valid = false;
        return false;
    }
    const int frameKind = tick ? (previousWasTick ? 2 : 0) : (previousWasTick ? 1 : 3);
    NodePose current;
    memcpy(current.value, reinterpret_cast<const void *>(camera + kNodeWorldTransform),
           sizeof(current.value));
    const bool tickPoses = g_killCamTickPosesEnabled || g_fewDrawsPerTick;
    if (!track.valid || track.lastSeen + 1 != g_killCamSerial || track.tickPoses != tickPoses) {
        track = {};
        track.valid = true;
        track.tickPoses = tickPoses;
        track.lastSeen = g_killCamSerial;
        track.from = track.to = track.periodFrom = track.last = current;
        return false;
    }
    track.lastSeen = g_killCamSerial;
    if (track.offTickScore != 0) --track.offTickScore;
    // Diagnostic: how many draws after the tick draw the camera moves, and
    // by how much, to tell parts that arrive late from motion every frame.
    if (memcmp(track.last.value, current.value, 12 * sizeof(float)) != 0) {
        float turn = 0.0f;
        for (size_t index = 0; index < 9; ++index) {
            const float difference = std::fabs(current.value[index] - track.last.value[index]);
            if (!(difference <= turn)) turn = difference;
        }
        const float moveSquared = DistanceSquared(&current.value[9], &track.last.value[9]);
        const size_t slot = g_killCamDrawsSinceTick < 4 ? g_killCamDrawsSinceTick : 4;
        g_counters.killCamAfterTick[slot].fetch_add(1, std::memory_order_relaxed);
        if (!(turn <= 0.002f) || !(moveSquared <= 0.25f * 0.25f)) {
            g_counters.killCamLargeAfterTick[slot].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!track.tickPoses) track.last = current;
    if (track.tickPoses) {
        const NodePose previous = track.last;
        track.last = current;
        const bool changed = memcmp(previous.value, current.value, 12 * sizeof(float)) != 0;
        float turn = 0.0f;
        float move = 0.0f;
        if (changed) {
            g_counters.killCamChanges[frameKind].fetch_add(1, std::memory_order_relaxed);
            for (size_t index = 0; index < 9; ++index) {
                const float difference = std::fabs(current.value[index] - previous.value[index]);
                if (!(difference <= turn)) turn = difference;
            }
            const float moveSquared = DistanceSquared(&current.value[9], &previous.value[9]);
            move = std::sqrt(moveSquared);
            const bool nearTick = frameKind != 3;
            AtomicMax(nearTick ? g_counters.killCamTickMoveMilli : g_counters.killCamOffMoveMilli,
                      std::isfinite(move) && move < 4.0e6f ? static_cast<uint32_t>(move * 1000.0f)
                                                           : 0xFFFFFFFFu);
            AtomicMax(nearTick ? g_counters.killCamTickTurnMicro : g_counters.killCamOffTurnMicro,
                      std::isfinite(turn) && turn < 4.0e3f ? static_cast<uint32_t>(turn * 1.0e6f)
                                                           : 0xFFFFFFFFu);
            if (!(moveSquared <= kTeleportDistanceSquared) || !(turn <= 0.5f)) {
                g_counters.killCamSnaps.fetch_add(1, std::memory_order_relaxed);
                track.from = track.to = current;
                track.perFrame = false;
                return false;
            }
        }
        const bool large = changed && (!(turn <= 0.002f) || !(move <= 0.25f));
        if (track.perFrame) {
            if (large) {
                g_counters.killCamAsIs.fetch_add(1, std::memory_order_relaxed);
                track.from = track.to = current;
                return false;
            }
            track.perFrame = false;
            track.from = track.to = current;
        }
        if (tick) {
            track.from = track.to;
            track.to = current;
            g_counters.killCamTickPoses.fetch_add(1, std::memory_order_relaxed);
        } else if (changed) {
            if (g_killCamDrawsSinceTick <= kKillCamLateDraws) {
                g_counters.killCamLate.fetch_add(1, std::memory_order_relaxed);
            } else if (!large) {
                g_counters.killCamMinor.fetch_add(1, std::memory_order_relaxed);
            } else {
                track.perFrame = true;
                g_counters.killCamAsIs.fetch_add(1, std::memory_order_relaxed);
                track.from = track.to = current;
                return false;
            }
        }
        if (!BlendPose(track.from, track.to, alpha, shown)) return false;
        return memcmp(shown.value, current.value, 12 * sizeof(float)) != 0;
    }
    if (memcmp(track.to.value, current.value, 12 * sizeof(float)) != 0) {
        g_counters.killCamChanges[frameKind].fetch_add(1, std::memory_order_relaxed);
        float turn = 0.0f;
        for (size_t index = 0; index < 9; ++index) {
            const float difference = std::fabs(current.value[index] - track.to.value[index]);
            if (!(difference <= turn)) turn = difference;
        }
        const float moveSquared = DistanceSquared(&current.value[9], &track.to.value[9]);
        const float move = std::sqrt(moveSquared);
        const bool nearTick = frameKind != 3;
        AtomicMax(nearTick ? g_counters.killCamTickMoveMilli : g_counters.killCamOffMoveMilli,
                  std::isfinite(move) && move < 4.0e6f ? static_cast<uint32_t>(move * 1000.0f)
                                                       : 0xFFFFFFFFu);
        AtomicMax(nearTick ? g_counters.killCamTickTurnMicro : g_counters.killCamOffTurnMicro,
                  std::isfinite(turn) && turn < 4.0e3f ? static_cast<uint32_t>(turn * 1.0e6f)
                                                       : 0xFFFFFFFFu);
        if (!(moveSquared <= kTeleportDistanceSquared) || !(turn <= 0.5f)) {
            g_counters.killCamSnaps.fetch_add(1, std::memory_order_relaxed);
            track.from = track.to = track.periodFrom = current;
            track.changePeriod = 0;
            return false;
        }
        const bool perFrame = track.offTickScore >= 8;
        if (nearTick && !perFrame) {
            // The period this pose belongs to: the tick of this draw, or of
            // the draw before when this one is not a tick.
            if (track.changePeriod != g_killCamTickCount) track.periodFrom = track.to;
            track.from = track.periodFrom;
            track.to = current;
            track.lag = !tick;
            if (track.lag) g_counters.killCamLate.fetch_add(1, std::memory_order_relaxed);
            track.changePeriod = g_killCamTickCount;
        } else if (!nearTick && !perFrame && turn <= 0.002f && move <= 0.25f) {
            // Small, away from a tick: let through.
            g_counters.killCamMinor.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (!nearTick) {
                track.offTickScore = static_cast<uint8_t>(
                    track.offTickScore > 12 ? 16 : track.offTickScore + 4);
            }
            g_counters.killCamAsIs.fetch_add(1, std::memory_order_relaxed);
            track.from = track.to = track.periodFrom = current;
            track.changePeriod = 0;
            return false;
        }
    }
    const uint32_t period = track.lag ? ticksBefore : g_killCamTickCount;
    if (track.changePeriod == 0 || track.changePeriod != period) return false;
    if (!BlendPose(track.from, track.to, track.lag ? previousAlpha : alpha, shown)) return false;
    return memcmp(shown.value, current.value, 12 * sizeof(float)) != 0;
}

void ResetDrawInterpolation() {
    g_killCamTrack.valid = false;
    g_playerHistory = {};
    g_cameraEligibleDraws = 0;
    g_lastCameraPositionValid = false;
    for (TickTrack &track : g_thirdPersonTracks) track = {};
}

struct DrawWrites {
    bool actors;
    bool rotation;
    bool camera;
    float *cameraPosition;
    float cameraEngine[3];
    float cameraWritten[3];
    bool sky;
    bool body;
};

void PrepareDraw(DrawWrites &writes) {
    writes = {};
    g_fpPassCamera.valid = false;
    if (g_frameTickDecision < 0) {
        g_counters.drawsOutsideFrame.fetch_add(1, std::memory_order_relaxed);
        ResetDrawInterpolation();
        return;
    }
    if (GetCurrentThreadId() != g_mainThreadId) {
        g_counters.drawsOtherThread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_frameEligible) {
        g_counters.drawsIneligible.fetch_add(1, std::memory_order_relaxed);
        if (InVatsPlayback()) g_counters.killCamIneligible.fetch_add(1, std::memory_order_relaxed);
        ResetDrawInterpolation();
        return;
    }
    uintptr_t player = 0;
    float current[3]{};
    float correction[3]{};
    bool firstPerson = true;
    bool tracking = false;
    bool warm = false;
    const bool tick = g_frameTickDecision != 0;
    double alpha = g_accumulatorMs / g_simulationStepMs;
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    __try {
        player = *reinterpret_cast<const uintptr_t *>(kPlayerPointer);
        const uintptr_t sceneGraph = *reinterpret_cast<const uintptr_t *>(kSceneGraphPointer);
        const uintptr_t camera = sceneGraph >= 0x10000
            ? *reinterpret_cast<const uintptr_t *>(sceneGraph + kSceneGraphCamera) : 0;
        if (player < 0x10000 || camera < 0x10000) {
            g_counters.drawsIneligible.fetch_add(1, std::memory_order_relaxed);
            ResetDrawInterpolation();
            return;
        }
        firstPerson = *reinterpret_cast<const uint8_t *>(player + kPlayerNotFirstPerson) == 0 &&
                      *reinterpret_cast<const uint8_t *>(player + kPlayerThirdPerson) == 0;
        if (!firstPerson) g_counters.thirdPersonDraws.fetch_add(1, std::memory_order_relaxed);
        memcpy(current, reinterpret_cast<const void *>(player + kReferencePosition),
               sizeof(current));
        float *position = reinterpret_cast<float *>(camera + kNodeWorldTranslation);
        memcpy(writes.cameraEngine, position, sizeof(float) * 3);
        float cameraTransform[12];
        memcpy(cameraTransform, reinterpret_cast<const void *>(camera + kNodeWorldTransform),
               sizeof(cameraTransform));
        const bool killCam = g_killCamCameraEnabled && InVatsPlayback();
        NodePose killCamShown{};
        const bool killCamMoves =
            AdvanceKillCamTrack(camera, killCam, tick, alpha, killCamShown);
        if (killCam) {
            g_counters.killCamDraws.fetch_add(1, std::memory_order_relaxed);
            if (firstPerson) g_counters.killCamFirstPerson.fetch_add(1, std::memory_order_relaxed);
        }

        if (g_lastCameraPositionValid &&
            DistanceSquared(writes.cameraEngine, g_lastCameraPosition) > 1.0e-6f) {
            (tick ? g_counters.cameraMovesOnTick : g_counters.cameraMovesOffTick)
                .fetch_add(1, std::memory_order_relaxed);
        }
        memcpy(g_lastCameraPosition, writes.cameraEngine, sizeof(g_lastCameraPosition));
        g_lastCameraPositionValid = true;

        double shownPosition[3];
        AdvancePlayerPosition(current, tick, alpha, shownPosition);
        tracking = true;
        ++g_cameraEligibleDraws;
        warm = g_cameraEligibleDraws > kCameraWarmUpDraws;

        // Third person: the tick-driven part of the camera's offset. The
        // tracks are advanced on every draw so that they are current when
        // blending starts.
        float thirdPersonDelta[3] = {0.0f, 0.0f, 0.0f};
        bool thirdPersonMoves = false;
        if (!killCam && !firstPerson && g_thirdPersonCameraEnabled) {
            thirdPersonMoves = ThirdPersonCameraDelta(camera, writes.cameraEngine, current, tick,
                                                      alpha, thirdPersonDelta);
        } else {
            for (TickTrack &track : g_thirdPersonTracks) track = {};
        }

        if (warm) {
            float squared = 0.0f;
            bool finite = true;
            for (int axis = 0; axis < 3; ++axis) {
                correction[axis] = static_cast<float>(shownPosition[axis] - current[axis]);
                squared += correction[axis] * correction[axis];
                finite = finite && std::isfinite(correction[axis]);
            }
            if (!finite || squared > kTeleportDistanceSquared) {
                correction[0] = correction[1] = correction[2] = 0.0f;
                squared = 0.0f;
            }
            float boneDelta[3] = {0.0f, 0.0f, 0.0f};
            const bool boneMoves = !killCam && firstPerson && g_firstPersonEnabled &&
                                   ComputeCameraBoneDelta(writes.cameraEngine, boneDelta);
            if (!killCam && firstPerson && g_firstPersonEnabled) {
                writes.rotation = ApplyCameraAnimationRotation(camera);
            }
            if (!killCam && firstPerson && g_firstPersonEnabled && g_firstPersonPassCameraEnabled &&
                g_fpPassInstalled && (boneMoves || writes.rotation)) {
                FirstPassCamera &pass = g_fpPassCamera;
                pass.camera = camera;
                memcpy(pass.engine, cameraTransform, sizeof(pass.engine));
                memcpy(pass.written, cameraTransform, sizeof(pass.written));
                if (writes.rotation) {
                    memcpy(pass.written, g_cameraRotationSaved.written, 9 * sizeof(float));
                }
                for (int axis = 0; axis < 3; ++axis) pass.written[9 + axis] += boneDelta[axis];
                pass.valid = true;
            }
            if (killCam) {
                // The VATS camera's own blend replaces every other correction
                // of the camera. The player's correction is still worked out
                // above for the body.
                if (killCamMoves) {
                    SavedCameraRotation &saved = g_cameraRotationSaved;
                    saved.camera = camera;
                    memcpy(saved.original, cameraTransform, sizeof(saved.original));
                    memcpy(saved.written, killCamShown.value, sizeof(saved.written));
                    memcpy(reinterpret_cast<void *>(camera + kNodeWorldTransform), saved.written,
                           sizeof(saved.written));
                    writes.rotation = true;
                    float total[3];
                    for (int axis = 0; axis < 3; ++axis) {
                        writes.cameraWritten[axis] = killCamShown.value[9 + axis];
                        total[axis] = writes.cameraWritten[axis] - writes.cameraEngine[axis];
                    }
                    memcpy(position, writes.cameraWritten, sizeof(float) * 3);
                    writes.cameraPosition = position;
                    writes.camera = true;
                    g_counters.killCamWritten.fetch_add(1, std::memory_order_relaxed);
                    if (g_shiftSkyEnabled) {
                        ShiftSkyForDraw(total);
                        writes.sky = g_skySavedCount != 0;
                    }
                }
            } else if (squared > 1.0e-8f || thirdPersonMoves || boneMoves) {
                float total[3];
                for (int axis = 0; axis < 3; ++axis) {
                    total[axis] = correction[axis] + boneDelta[axis] +
                                  (thirdPersonMoves ? thirdPersonDelta[axis] : 0.0f);
                    writes.cameraWritten[axis] = writes.cameraEngine[axis] + total[axis];
                }
                memcpy(position, writes.cameraWritten, sizeof(float) * 3);
                writes.cameraPosition = position;
                writes.camera = true;
                g_counters.cameraApplied.fetch_add(1, std::memory_order_relaxed);
                AtomicMax(g_counters.maxCorrectionMilli,
                          static_cast<uint32_t>(std::sqrt(squared) * 1000.0f));
                if (g_shiftSkyEnabled) {
                    ShiftSkyForDraw(total);
                    writes.sky = g_skySavedCount != 0;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
        ResetDrawInterpolation();
        tracking = false;
    }
    // The body's history is kept current from the first eligible draw;
    // writing starts with the camera's.
    if (tracking && !firstPerson && g_thirdPersonBodyEnabled) {
        writes.body = ApplyPlayerBodyPose(player, current, correction, tick, alpha, warm);
    }
    // Other characters follow the same eligibility and warm-up.
    if (tracking && g_interpolateActorsEnabled) {
        g_cameraEligibleDrawsForObjects = g_cameraEligibleDraws;
        writes.actors = ApplyActorPoses(player, tick, alpha, warm);
    } else {
        ResetActorInterpolation();
    }
}

void RestoreDraw(const DrawWrites &writes) {
    if (writes.actors) RestoreActorPoses();
    if (writes.rotation) RestoreCameraAnimationRotation();
    if (writes.body) RestorePlayerBodyPose();
    if (writes.sky) RestoreSkyAfterDraw();
    if (!writes.camera) return;
    __try {
        if (memcmp(writes.cameraPosition, writes.cameraWritten, sizeof(float) * 3) == 0) {
            memcpy(writes.cameraPosition, writes.cameraEngine, sizeof(float) * 3);
        } else {
            g_counters.cameraRestoreConflicts.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_counters.faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void __fastcall WorldDrawHook(void *self, void * /*edx*/, uint32_t arg0, uint32_t arg1,
                              uint32_t arg2, uint32_t arg3) {
    using WorldDrawFn = void(__thiscall *)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
    const auto stock = reinterpret_cast<WorldDrawFn>(kWorldDrawFunction);
    g_counters.draws.fetch_add(1, std::memory_order_relaxed);
    static DrawWrites writes;
    writes = {};
    if (g_interpolateCameraEnabled && g_hooksInstalled.load(std::memory_order_relaxed)) {
        PrepareDraw(writes);
    }
    stock(self, arg0, arg1, arg2, arg3);
    RestoreDraw(writes);
}

// Around the whole composite render: the first-person skeleton's blended
// locals are in place from before the main world draw (which needs the
// blended camera bone) until after the first-person pass, wherever in the
// composite that is.
template <size_t Site>
void __fastcall CompositeRenderHook(void *self, void * /*edx*/, uint32_t arg0, uint32_t arg1) {
    using CompositeFn = void(__thiscall *)(void *, uint32_t, uint32_t);
    const auto stock = reinterpret_cast<CompositeFn>(kCompositeSites[Site].target);
    bool active = false;
    if (g_firstPersonEnabled && g_interpolateCameraEnabled &&
        g_hooksInstalled.load(std::memory_order_relaxed) && g_frameTickDecision >= 0 &&
        g_frameEligible && GetCurrentThreadId() == g_mainThreadId &&
        g_cameraEligibleDraws > kCameraWarmUpDraws) {
        uintptr_t player = 0;
        bool firstPerson = false;
        __try {
            player = *reinterpret_cast<const uintptr_t *>(kPlayerPointer);
            firstPerson = player >= 0x10000 &&
                *reinterpret_cast<const uint8_t *>(player + kPlayerNotFirstPerson) == 0 &&
                *reinterpret_cast<const uint8_t *>(player + kPlayerThirdPerson) == 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            firstPerson = false;
        }
        if (firstPerson) {
            double alpha = g_accumulatorMs / g_simulationStepMs;
            if (alpha < 0.0) alpha = 0.0;
            if (alpha > 1.0) alpha = 1.0;
            FirstPersonAdvance(player, g_frameTickDecision != 0, alpha);
            if (g_fpAdvanced) {
                FirstPersonWrite();
                active = true;
                g_counters.fpRenders.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    stock(self, arg0, arg1);
    if (active) FirstPersonRestore();
}
const void *const kCompositeHooks[kCompositeSiteCount] = {
    reinterpret_cast<const void *>(&CompositeRenderHook<0>),
    reinterpret_cast<const void *>(&CompositeRenderHook<1>),
};

void __fastcall InputPollHook(void *self, void * /*edx*/) {
    reinterpret_cast<ThisCallNoArgs>(kInputPollFunction)(self);
    if (!g_latchInputEnabled || !g_hooksInstalled.load(std::memory_order_relaxed) ||
        !g_frameEligible || g_frameTickDecision < 0 || self == nullptr ||
        reinterpret_cast<uintptr_t>(self) != *reinterpret_cast<const uintptr_t *>(kInputPointer)) {
        g_inputLatch.valid = false;
        return;
    }
    g_counters.latchPolls.fetch_add(1, std::memory_order_relaxed);
    ApplyInputLatch(static_cast<uint8_t *>(self), g_frameTickDecision != 0);
    // The right mouse button as the controls see it: aiming starts or ends.
    static uint8_t lastAimButton = 0;
    const uint8_t aimButton = g_inputLatch.buttons[1] & 0x80;
    if (aimButton != lastAimButton) {
        lastAimButton = aimButton;
        if (g_fpTraceEnabled) g_fpTraceHot = 150;
        // One table per change, alternately early in the transition (8
        // renders) and later (30 renders).
        if (g_worldDumpEnabled && g_worldDumpsLeft != 0 && g_worldDumpCountdown == 0) {
            g_worldDumpCountdown = g_worldDumpsLeft % 2 == 0 ? 8 : 30;
            --g_worldDumpsLeft;
        }
    }
}

// The player's look input multiplies the vanity (free look) mouse movement
// and the gamepad stick by the frame's delta. Under the fixed tick that delta
// is one tick on tick frames and almost nothing on the frames between, so
// the movement read on those frames was lost and free look and stick look
// stepped at the tick rate. The function tests no freeze flag and runs in
// full on every frame, so it is given the frame's real time instead, scaled
// by the time multiplier as the timer's delta is. Over a second the time it
// is given is the same; only its spread over the frames changes. Frames that
// already run on real time are left as they are. The delta arrives scaled by
// the global and the VATS time multipliers; their product is the delta over
// the timer's unscaled one, and the real time is scaled by the same.
uint32_t __fastcall LookInputHook(void *self, void * /*edx*/, float delta, uint32_t arg1,
                                  uint32_t arg2, uint32_t arg3) {
    using LookInputFn = uint32_t(__thiscall *)(void *, float, uint32_t, uint32_t, uint32_t);
    g_counters.lookInputCalls.fetch_add(1, std::memory_order_relaxed);
    const float unscaled = reinterpret_cast<const BSTimer *>(kMainTimer)->realDeltaSeconds;
    if (g_lookInputRealDeltaEnabled && g_hooksInstalled.load(std::memory_order_relaxed) &&
        g_frameTickDecision >= 0 && !g_frameNativeStep &&
        GetCurrentThreadId() == g_mainThreadId && unscaled > 0.0f) {
        const double multiplier = static_cast<double>(delta) / unscaled;
        const double wallMs = g_frameWallDeltaMs > 100.0 ? 100.0 : g_frameWallDeltaMs;
        const float scaled = static_cast<float>(wallMs * 0.001 * multiplier);
        if (std::isfinite(scaled) && multiplier >= 0.0 && multiplier <= 20.0) {
            delta = scaled;
            g_counters.lookInputRealDelta.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return reinterpret_cast<LookInputFn>(kLookInputFunction)(self, delta, arg1, arg2, arg3);
}

// A held ragdoll is pushed once per frame (see kGrabHelperActorCall), so the
// force grew with the frame rate. Under the fixed tick the push is given on
// tick frames only: one per tick, as the game gives it at 60 frames a
// second. Real-time frames keep the engine's behaviour. Changes the
// simulation, so it has its own switch.
bool SkipGrabHelperImpulse() {
    const bool skip = g_fixedGrabForceEnabled && g_hooksInstalled.load(std::memory_order_relaxed) &&
                      g_frameTickDecision == 0 && !g_frameNativeStep &&
                      GetCurrentThreadId() == g_mainThreadId;
    (skip ? g_counters.grabHelperSkipped : g_counters.grabHelperApplied)
        .fetch_add(1, std::memory_order_relaxed);
    return skip;
}

void __cdecl GrabHelperActorHook(void *node, const float *impulse, uint32_t flags) {
    using ImpulseFn = void(__cdecl *)(void *, const float *, uint32_t);
    if (SkipGrabHelperImpulse()) return;
    reinterpret_cast<ImpulseFn>(kGrabHelperActorFunction)(node, impulse, flags);
}

void __fastcall GrabHelperBodyHook(void *body, void * /*edx*/, const float *impulse) {
    using ImpulseFn = void(__thiscall *)(void *, const float *);
    if (SkipGrabHelperImpulse()) return;
    reinterpret_cast<ImpulseFn>(kGrabHelperBodyFunction)(body, impulse);
}

// Sets the four cursor settings to 0, which makes the cursor's speed factor
// a constant 1. Changes values the engine keeps; done once.
void RemoveMenuMouseAcceleration() {
    float before[4];
    for (int i = 0; i < 4; ++i) {
        auto *value = reinterpret_cast<float *>(kCursorSettings[i] + 4);
        before[i] = *value;
        *value = 0.0f;
    }
    AppendStatus("menu mouse: acceleration settings were %.4f %.4f %.4f %.4f, now 0", before[0],
                 before[1], before[2], before[3]);
}

void __fastcall GameFrameHook(void *self, void * /*edx*/) {
    if (!g_firstFrameChecked.exchange(true, std::memory_order_acq_rel)) {
        FirstFrameCheck();
        if (g_firstFrameIntact && g_menuMouseNoAcceleration) RemoveMenuMouseAcceleration();
    }
    if (g_telemetryEnabled && g_hooksInstalled.load(std::memory_order_relaxed)) {
        const int64_t now = QpcNow();
        if (g_lastFrameQpc != 0) {
            const int64_t elapsed = now - g_lastFrameQpc;
            g_counters.wallTicks.fetch_add(elapsed, std::memory_order_relaxed);
            AtomicMax(g_counters.maxFrameMicros,
                      static_cast<uint32_t>(QpcToMs(elapsed) * 1000.0));
        }
        g_lastFrameQpc = now;
        g_counters.frames.fetch_add(1, std::memory_order_relaxed);
        if (*reinterpret_cast<const uint8_t *>(kFrameFlagByte) != 0) {
            g_counters.flagFrames.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_mainThreadId = GetCurrentThreadId();
    ++g_fovFrameSerial;
    BeginTickFrame();
    const bool frozen = BeginFrozenFrame(self);
    reinterpret_cast<ThisCallNoArgs>(kFrameFunction)(self);
    if (frozen) static_cast<uint8_t *>(self)[7] = 0;
    g_fpPassCamera.valid = false;
    g_frameTickDecision = -1;
    g_frameEligible = false;
    g_frameNativeStep = false;
}

// Runs the stock update with the clamp that carries this frame's decision,
// then puts the engine's own clamp back.
void UpdateMainTimer(void *self) {
    auto *timer = static_cast<BSTimer *>(self);
    if (g_frameTickDecision < 0 || !g_hooksInstalled.load(std::memory_order_relaxed)) {
        if (g_fixedTickEnabled) {
            g_counters.passThroughUpdates.fetch_add(1, std::memory_order_relaxed);
        }
        reinterpret_cast<ThisCallNoArgs>(kTimerUpdateFunction)(self);
        return;
    }
    const float engineClampMs = timer->clampMs;
    timer->clampMs = g_frameNativeStep
        ? static_cast<float>(g_frameNativeDeltaMs)
        : (g_frameTickDecision != 0 ? static_cast<float>(g_simulationStepMs) : kNearZeroClampMs);
    reinterpret_cast<ThisCallNoArgs>(kTimerUpdateFunction)(self);
    timer->clampMs = engineClampMs;
}

// For a timer without a clamp, what the stock update computes but from the
// real elapsed time and without its Sleep. The stock update reports at least
// 10 ms and sleeps to make that true.
struct AuxTimerClock {
    const void *timer;
    int64_t lastQpc;
};
AuxTimerClock g_auxTimerClocks[8]{};

bool UpdateTimerWithoutSleep(BSTimer *timer) {
    if (timer->disableCounter != 0 || timer->clampMs != 0.0f) return false;
    AuxTimerClock *clock = nullptr;
    for (AuxTimerClock &candidate : g_auxTimerClocks) {
        if (candidate.timer == timer || candidate.timer == nullptr) {
            clock = &candidate;
            break;
        }
    }
    if (clock == nullptr) return false;
    const int64_t now = QpcNow();
    const bool known = clock->timer == timer;
    const int64_t last = clock->lastQpc;
    clock->timer = timer;
    clock->lastQpc = now;
    if (!known) return false; // first sight of this timer: the stock update runs once

    double elapsedSeconds = QpcToMs(now - last) * 0.001;
    if (elapsedSeconds < 0.0) elapsedSeconds = 0.0;
    if (elapsedSeconds > 0.166) elapsedSeconds = 0.166;
    timer->sameFrameDelta = 0;
    timer->lastTimeMs = GetTickCount() - timer->firstTimeMs;
    timer->realDeltaSeconds = static_cast<float>(elapsedSeconds);
    timer->deltaSeconds =
        static_cast<float>(elapsedSeconds) * *reinterpret_cast<const float *>(kTimeMultiplier);
    return true;
}

template <size_t Site>
void __fastcall AuxTimerUpdateHook(void *self, void * /*edx*/) {
    g_counters.auxCalls[Site].fetch_add(1, std::memory_order_relaxed);
    if (g_noTimerSleepsEnabled && g_hooksInstalled.load(std::memory_order_relaxed) &&
        self != nullptr && UpdateTimerWithoutSleep(static_cast<BSTimer *>(self))) {
        g_counters.auxNoSleep[Site].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    reinterpret_cast<ThisCallNoArgs>(kTimerUpdateFunction)(self);
}
const void *const kAuxTimerHooks[kAuxTimerSiteCount] = {
    reinterpret_cast<const void *>(&AuxTimerUpdateHook<0>),
    reinterpret_cast<const void *>(&AuxTimerUpdateHook<1>),
    reinterpret_cast<const void *>(&AuxTimerUpdateHook<2>),
};

void __fastcall TimerUpdateHook(void *self, void * /*edx*/) {
    if (!g_telemetryEnabled || !g_hooksInstalled.load(std::memory_order_relaxed)) {
        UpdateMainTimer(self);
        return;
    }
    const int64_t before = QpcNow();
    UpdateMainTimer(self);
    const int64_t inside = QpcNow() - before;

    const BSTimer *timer = static_cast<const BSTimer *>(self);
    const uint32_t deltaMicros = static_cast<uint32_t>(timer->deltaSeconds * 1.0e6f);
    g_counters.timerUpdates.fetch_add(1, std::memory_order_relaxed);
    g_counters.timerInsideTicks.fetch_add(inside, std::memory_order_relaxed);
    g_counters.engineMicros.fetch_add(deltaMicros, std::memory_order_relaxed);
    g_counters.realMicros.fetch_add(static_cast<int64_t>(timer->realDeltaSeconds * 1.0e6f),
                                    std::memory_order_relaxed);
    AtomicMax(g_counters.maxDeltaMicros, deltaMicros);
    AtomicMin(g_counters.minDeltaMicros, deltaMicros);
    if (QpcToMs(inside) > 0.5) {
        g_counters.slowTimerUpdates.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---- Status line -----------------------------------------------------------

void FlushFirstPersonTrace() {
    const uint32_t write = g_fpTraceWrite.load(std::memory_order_acquire);
    if (write - g_fpTraceRead > kFpTraceSize) g_fpTraceRead = write - kFpTraceSize;
    while (g_fpTraceRead != write) {
        const FpTraceRow row = g_fpTrace[g_fpTraceRead & (kFpTraceSize - 1)];
        ++g_fpTraceRead;
        char notes[400];
        int used = 0;
        notes[0] = '\0';
        for (uint8_t index = 0; index < row.noteCount && used < 300; ++index) {
            const FpTraceNote &note = row.notes[index];
            used += snprintf(notes + used, sizeof(notes) - used, " %s:%u:t%.3f:r%.4f", note.name,
                             note.decision, note.translationStep, note.rotationStep);
        }
        AppendStatus("aim serial=%u kind=%u alpha=%.3f fov=%.3f/%.3f/%.3f shown=%.3f/%.3f/%.3f "
                     "changed=%u tick=%u late=%u offtick=%u "
                     "perframe=%u firstlate=%u toobig=%u blending=%u |%s",
                     row.serial, row.kind, row.alpha, row.fovEngine[0], row.fovEngine[1],
                     row.fovEngine[2], row.fovShown[0], row.fovShown[1], row.fovShown[2],
                     row.counts[0], row.counts[1],
                     row.counts[2], row.counts[3], row.counts[4], row.counts[5], row.counts[6],
                     row.counts[7], notes);
    }
}

DWORD WINAPI LogThread(LPVOID) {
    uint64_t lastFrames = 0;
    uint64_t lastTimerUpdates = 0;
    int64_t lastWallTicks = 0;
    int64_t lastInsideTicks = 0;
    int64_t lastEngineMicros = 0;
    int64_t lastRealMicros = 0;
    uint64_t lastFlagFrames = 0;
    uint64_t lastSlow = 0;
    uint64_t lastTicks = 0;
    uint64_t lastPassThrough = 0;
    uint64_t lastFrozen = 0, lastIneligible = 0, lastWarmUp = 0;
    uint64_t lastPolls = 0, lastDeferred = 0, lastDelivered = 0, lastTaps = 0;
    uint64_t lastAuxCalls[kAuxTimerSiteCount]{};
    uint64_t lastAuxNoSleep[kAuxTimerSiteCount]{};
    while (!g_stop.load(std::memory_order_acquire)) {
        for (int i = 0; i < 50 && !g_stop.load(std::memory_order_acquire); ++i) {
            Sleep(100);
            FlushFirstPersonTrace();
            FlushWorldDump();
        }
        if (!g_hooksInstalled.load(std::memory_order_acquire)) continue;

        const uint64_t frames = g_counters.frames.load(std::memory_order_relaxed);
        const uint64_t timerUpdates = g_counters.timerUpdates.load(std::memory_order_relaxed);
        const int64_t wallTicks = g_counters.wallTicks.load(std::memory_order_relaxed);
        const int64_t insideTicks = g_counters.timerInsideTicks.load(std::memory_order_relaxed);
        const int64_t engineMicros = g_counters.engineMicros.load(std::memory_order_relaxed);
        const int64_t realMicros = g_counters.realMicros.load(std::memory_order_relaxed);
        const uint64_t flagFrames = g_counters.flagFrames.load(std::memory_order_relaxed);
        const uint64_t slow = g_counters.slowTimerUpdates.load(std::memory_order_relaxed);
        const uint32_t maxFrame = g_counters.maxFrameMicros.exchange(0, std::memory_order_relaxed);
        const uint32_t maxDelta = g_counters.maxDeltaMicros.exchange(0, std::memory_order_relaxed);
        const uint32_t minDelta =
            g_counters.minDeltaMicros.exchange(0xFFFFFFFFu, std::memory_order_relaxed);

        const uint64_t dFrames = frames - lastFrames;
        const uint64_t dTimer = timerUpdates - lastTimerUpdates;
        if (dFrames == 0) continue;
        const double wallMs = QpcToMs(wallTicks - lastWallTicks);
        const double insideMs = QpcToMs(insideTicks - lastInsideTicks);
        const double engineMs = static_cast<double>(engineMicros - lastEngineMicros) / 1000.0;
        const double realMs = static_cast<double>(realMicros - lastRealMicros) / 1000.0;

        const BSTimer *timer = reinterpret_cast<const BSTimer *>(kMainTimer);
        AppendStatus(
            "status frames=%llu fps=%.1f wall_ms=%.0f timer_updates=%llu engine_ms=%.0f "
            "real_ms=%.0f engine/wall=%.4f timer_inside_ms=%.0f (%.2f per update, %llu over "
            "0.5ms) delta_ms min=%.3f max=%.3f frame_ms max=%.3f clamp_ms=%.3f max_time=%.5f "
            "time_mult=%.3f flag_frames=%llu",
            dFrames, wallMs > 0.0 ? static_cast<double>(dFrames) * 1000.0 / wallMs : 0.0, wallMs,
            dTimer, engineMs, realMs, wallMs > 0.0 ? engineMs / wallMs : 0.0, insideMs,
            dTimer != 0 ? insideMs / static_cast<double>(dTimer) : 0.0, slow - lastSlow,
            minDelta == 0xFFFFFFFFu ? 0.0 : minDelta / 1000.0, maxDelta / 1000.0,
            maxFrame / 1000.0, timer->clampMs, *reinterpret_cast<const float *>(kHavokMaxTime),
            *reinterpret_cast<const float *>(kTimeMultiplier), flagFrames - lastFlagFrames);

        const uint64_t ticks = g_counters.ticks.load(std::memory_order_relaxed);
        const uint64_t passThrough =
            g_counters.passThroughUpdates.load(std::memory_order_relaxed);
        uint64_t auxCalls[kAuxTimerSiteCount];
        uint64_t auxNoSleep[kAuxTimerSiteCount];
        for (size_t i = 0; i < kAuxTimerSiteCount; ++i) {
            auxCalls[i] = g_counters.auxCalls[i].load(std::memory_order_relaxed);
            auxNoSleep[i] = g_counters.auxNoSleep[i].load(std::memory_order_relaxed);
        }
        AppendStatus(
            "tick fixed=%d ticks=%llu tick_hz=%.2f accumulator_ms=%.3f pass_through=%llu "
            "aux_timers calls/no_sleep A=%llu/%llu B=%llu/%llu C=%llu/%llu",
            g_fixedTickEnabled ? 1 : 0, ticks - lastTicks,
            wallMs > 0.0 ? static_cast<double>(ticks - lastTicks) * 1000.0 / wallMs : 0.0,
            g_counters.accumulatorMicros.load(std::memory_order_relaxed) / 1000.0,
            passThrough - lastPassThrough, auxCalls[0] - lastAuxCalls[0],
            auxNoSleep[0] - lastAuxNoSleep[0], auxCalls[1] - lastAuxCalls[1],
            auxNoSleep[1] - lastAuxNoSleep[1], auxCalls[2] - lastAuxCalls[2],
            auxNoSleep[2] - lastAuxNoSleep[2]);
        const uint64_t frozen = g_counters.frozenFrames.load(std::memory_order_relaxed);
        const uint64_t ineligible = g_counters.ineligibleFrames.load(std::memory_order_relaxed);
        const uint64_t warmUp = g_counters.warmUpFrames.load(std::memory_order_relaxed);
        const uint64_t nativeFrames = g_counters.nativeFrames.load(std::memory_order_relaxed);
        static uint64_t lastNative = 0;
        const uint64_t polls = g_counters.latchPolls.load(std::memory_order_relaxed);
        const uint64_t deferred = g_counters.latchEdgesDeferred.load(std::memory_order_relaxed);
        const uint64_t delivered =
            g_counters.latchEdgesDelivered.load(std::memory_order_relaxed);
        const uint64_t taps = g_counters.latchTapsRecovered.load(std::memory_order_relaxed);
        static uint64_t lastLookCalls = 0, lastLookReal = 0;
        const uint64_t lookCalls = g_counters.lookInputCalls.load(std::memory_order_relaxed);
        const uint64_t lookReal = g_counters.lookInputRealDelta.load(std::memory_order_relaxed);
        static uint64_t lastGrabApplied = 0, lastGrabSkipped = 0;
        const uint64_t grabApplied = g_counters.grabHelperApplied.load(std::memory_order_relaxed);
        const uint64_t grabSkipped = g_counters.grabHelperSkipped.load(std::memory_order_relaxed);
        AppendStatus(
"freeze frozen=%llu ineligible=%llu warm_up=%llu real_time=%llu havok_max_time=%.6f | latch "
            "polls=%llu edges deferred=%llu delivered=%llu taps_recovered=%llu | look_input "
            "calls=%llu real_delta=%llu | grab_push applied=%llu skipped=%llu | faults=%llu",
            frozen - lastFrozen, ineligible - lastIneligible, warmUp - lastWarmUp,
            nativeFrames - lastNative, *reinterpret_cast<const float *>(kHavokMaxTime), polls - lastPolls,
            deferred - lastDeferred, delivered - lastDelivered, taps - lastTaps,
            lookCalls - lastLookCalls, lookReal - lastLookReal, grabApplied - lastGrabApplied,
            grabSkipped - lastGrabSkipped, g_counters.faults.load(std::memory_order_relaxed));
        lastLookCalls = lookCalls;
        lastLookReal = lookReal;
        lastGrabApplied = grabApplied;
        lastGrabSkipped = grabSkipped;
        {
            static uint64_t last[11]{};
            const uint64_t now[11] = {
                g_counters.draws.load(std::memory_order_relaxed),
                g_counters.cameraApplied.load(std::memory_order_relaxed),
                g_counters.drawsIneligible.load(std::memory_order_relaxed),
                g_counters.drawsOutsideFrame.load(std::memory_order_relaxed),
                g_counters.drawsOtherThread.load(std::memory_order_relaxed),
                g_counters.playerPosesOnTick.load(std::memory_order_relaxed),
                g_counters.playerPosesOffTick.load(std::memory_order_relaxed),
                g_counters.cameraMovesOnTick.load(std::memory_order_relaxed),
                g_counters.cameraMovesOffTick.load(std::memory_order_relaxed),
                g_counters.snaps.load(std::memory_order_relaxed),
                g_counters.cameraRestoreConflicts.load(std::memory_order_relaxed)};
            AppendStatus(
                "camera draws=%llu applied=%llu ineligible=%llu outside_frame=%llu "
                "other_thread=%llu | player_pos changes tick=%llu off_tick=%llu | camera_pos "
                "changes tick=%llu off_tick=%llu | snaps=%llu restore_conflicts=%llu "
                "max_correction=%.3f",
                now[0] - last[0], now[1] - last[1], now[2] - last[2], now[3] - last[3],
                now[4] - last[4], now[5] - last[5], now[6] - last[6], now[7] - last[7],
                now[8] - last[8], now[9] - last[9], now[10] - last[10],
                g_counters.maxCorrectionMilli.exchange(0, std::memory_order_relaxed) / 1000.0);
            memcpy(last, now, sizeof(last));
        }
        {
            static uint64_t last[15]{};
            const uint64_t now[15] = {
                g_counters.skyNodesWritten.load(std::memory_order_relaxed),
                g_counters.skyRestoreConflicts.load(std::memory_order_relaxed),
                g_counters.thirdPersonDraws.load(std::memory_order_relaxed),
                g_counters.thirdPersonSteps.load(std::memory_order_relaxed),
                g_counters.thirdPersonOffTick.load(std::memory_order_relaxed),
                g_counters.bodyNodesWritten.load(std::memory_order_relaxed),
                g_counters.bodyBlends.load(std::memory_order_relaxed),
                g_counters.bodyOffTick.load(std::memory_order_relaxed),
                g_counters.bodyMinor.load(std::memory_order_relaxed),
                g_counters.bodyChanges[0].load(std::memory_order_relaxed),
                g_counters.bodyChanges[1].load(std::memory_order_relaxed),
                g_counters.bodyChanges[2].load(std::memory_order_relaxed),
                g_counters.bodyChanges[3].load(std::memory_order_relaxed),
                g_counters.bodyRestoreConflicts.load(std::memory_order_relaxed),
                g_counters.bodyRootMismatch.load(std::memory_order_relaxed)};
            AppendStatus(
                "sky nodes=%llu restore_conflicts=%llu | third draws=%llu offset steps=%llu "
                "off_tick=%llu | body nodes=%llu blends=%llu off_tick=%llu minor=%llu changes "
                "tick=%llu after=%llu both=%llu neither=%llu restore_conflicts=%llu "
                "root_mismatch=%llu",
                now[0] - last[0], now[1] - last[1], now[2] - last[2], now[3] - last[3],
                now[4] - last[4], now[5] - last[5], now[6] - last[6], now[7] - last[7],
                now[8] - last[8], now[9] - last[9], now[10] - last[10], now[11] - last[11],
                now[12] - last[12], now[13] - last[13], now[14] - last[14]);
            memcpy(last, now, sizeof(last));
        }
        {
            static uint64_t last[19]{};
            const uint64_t now[19] = {
                g_counters.fpRenders.load(std::memory_order_relaxed),
                g_counters.fpNodes.load(std::memory_order_relaxed),
                g_counters.fpChanges[0].load(std::memory_order_relaxed),
                g_counters.fpChanges[1].load(std::memory_order_relaxed),
                g_counters.fpChanges[2].load(std::memory_order_relaxed),
                g_counters.fpChanges[3].load(std::memory_order_relaxed),
                g_counters.fpBlends.load(std::memory_order_relaxed),
                g_counters.fpLateBlends.load(std::memory_order_relaxed),
                g_counters.fpPerFrame.load(std::memory_order_relaxed),
                g_counters.fpWritten.load(std::memory_order_relaxed),
                g_counters.fpRestoreConflicts.load(std::memory_order_relaxed),
                g_counters.fpWorldRecomputed.load(std::memory_order_relaxed),
                g_counters.fpWorldUnchanged.load(std::memory_order_relaxed),
                g_counters.fpNoCameraBone.load(std::memory_order_relaxed),
                g_counters.fpBoneWrites.load(std::memory_order_relaxed),
                g_counters.boneDeltaApplied.load(std::memory_order_relaxed),
                g_counters.boneDeltaMismatch.load(std::memory_order_relaxed),
                g_counters.cameraRotationWrites.load(std::memory_order_relaxed),
                g_counters.cameraRelationChanges.load(std::memory_order_relaxed)};
            AppendStatus(
                "first renders=%llu nodes=%llu changes tick=%llu after=%llu both=%llu "
                "neither=%llu blends=%llu late=%llu as_is=%llu written=%llu "
                "restore_conflicts=%llu world recomputed=%llu unchanged=%llu | bone "
                "missing=%llu world_writes=%llu delta applied=%llu mismatch=%llu "
                "(max %.3f) | rotation writes=%llu relation_changes=%llu conflicts=%llu",
                now[0] - last[0], now[1] - last[1], now[2] - last[2], now[3] - last[3],
                now[4] - last[4], now[5] - last[5], now[6] - last[6], now[7] - last[7],
                now[8] - last[8], now[9] - last[9], now[10] - last[10], now[11] - last[11],
                now[12] - last[12], now[13] - last[13], now[14] - last[14], now[15] - last[15],
                now[16] - last[16],
                g_counters.boneMismatchMilli.exchange(0, std::memory_order_relaxed) / 1000.0,
                now[17] - last[17], now[18] - last[18],
                g_counters.cameraRotationConflicts.load(std::memory_order_relaxed));
            memcpy(last, now, sizeof(last));
        }
        {
            static uint64_t last[4]{};
            const uint64_t now[4] = {g_counters.fovCalls.load(std::memory_order_relaxed),
                                     g_counters.fovSteps.load(std::memory_order_relaxed),
                                     g_counters.fovAsIs.load(std::memory_order_relaxed),
                                     g_counters.fovSubstitutions.load(std::memory_order_relaxed)};
            AppendStatus("fov sites=%u calls=%llu steps=%llu as_is=%llu substitutions=%llu",
                         g_counters.fovSites.load(std::memory_order_relaxed), now[0] - last[0],
                         now[1] - last[1], now[2] - last[2], now[3] - last[3]);
            memcpy(last, now, sizeof(last));
        }
        {
            static uint64_t last[18]{};
            static uint64_t lastKillCamMinor = 0;
            static uint64_t lastPassUpdated = 0;
            static uint64_t lastWorldAdjusted = 0;
            const uint64_t now[18] = {
                g_counters.fpWorldWrites.load(std::memory_order_relaxed),
                g_counters.fpWorldSkipped.load(std::memory_order_relaxed),
                g_counters.fpWorldKept.load(std::memory_order_relaxed),
                g_counters.fpWorldOverwritten.load(std::memory_order_relaxed),
                g_counters.fpPassCameraWrites.load(std::memory_order_relaxed),
                g_counters.fpPassCameraSkipped.load(std::memory_order_relaxed),
                g_counters.fpPassCameraConflicts.load(std::memory_order_relaxed),
                g_counters.killCamDraws.load(std::memory_order_relaxed),
                g_counters.killCamFirstPerson.load(std::memory_order_relaxed),
                g_counters.killCamIneligible.load(std::memory_order_relaxed),
                g_counters.killCamChanges[0].load(std::memory_order_relaxed),
                g_counters.killCamChanges[1].load(std::memory_order_relaxed),
                g_counters.killCamChanges[2].load(std::memory_order_relaxed),
                g_counters.killCamChanges[3].load(std::memory_order_relaxed),
                g_counters.killCamLate.load(std::memory_order_relaxed),
                g_counters.killCamAsIs.load(std::memory_order_relaxed),
                g_counters.killCamSnaps.load(std::memory_order_relaxed),
                g_counters.killCamWritten.load(std::memory_order_relaxed)};
            AppendStatus(
                "first_world writes=%llu skipped=%llu (max %.3f) adjusted=%llu kept=%llu "
                "overwritten=%llu | first_pass "
                "camera writes=%llu skipped=%llu conflicts=%llu recomputed=%llu | killcam draws=%llu "
                "first_person=%llu ineligible=%llu changes tick=%llu after=%llu both=%llu "
                "neither=%llu late=%llu as_is=%llu snaps=%llu written=%llu minor=%llu vats_mode=%u "
                "largest change near tick %.3f units %.6f, other draws %.3f units %.6f",
                now[0] - last[0], now[1] - last[1],
                g_counters.fpWorldSkippedMilli.exchange(0, std::memory_order_relaxed) / 1000.0,
                g_counters.fpWorldAdjusted.load(std::memory_order_relaxed) - lastWorldAdjusted,
                now[2] - last[2], now[3] - last[3],
                now[4] - last[4], now[5] - last[5], now[6] - last[6],
                g_counters.fpPassCameraUpdated.load(std::memory_order_relaxed) - lastPassUpdated,
                now[7] - last[7],
                now[8] - last[8], now[9] - last[9], now[10] - last[10], now[11] - last[11],
                now[12] - last[12], now[13] - last[13], now[14] - last[14], now[15] - last[15],
                now[16] - last[16], now[17] - last[17],
                g_counters.killCamMinor.load(std::memory_order_relaxed) - lastKillCamMinor,
                *reinterpret_cast<const uint32_t *>(kVatsCameraData + kVatsCameraMode),
                g_counters.killCamTickMoveMilli.exchange(0, std::memory_order_relaxed) / 1000.0,
                g_counters.killCamTickTurnMicro.exchange(0, std::memory_order_relaxed) / 1.0e6,
                g_counters.killCamOffMoveMilli.exchange(0, std::memory_order_relaxed) / 1000.0,
                g_counters.killCamOffTurnMicro.exchange(0, std::memory_order_relaxed) / 1.0e6);
            lastKillCamMinor = g_counters.killCamMinor.load(std::memory_order_relaxed);
            lastPassUpdated = g_counters.fpPassCameraUpdated.load(std::memory_order_relaxed);
            lastWorldAdjusted = g_counters.fpWorldAdjusted.load(std::memory_order_relaxed);
            memcpy(last, now, sizeof(last));
        }
        {
            static uint64_t last[15]{};
            const uint64_t now[15] = {
                g_counters.actorPasses.load(std::memory_order_relaxed),
                g_counters.actorsTracked.load(std::memory_order_relaxed),
                g_counters.actorNodesWritten.load(std::memory_order_relaxed),
                g_counters.actorLag[0].load(std::memory_order_relaxed),
                g_counters.actorLag[1].load(std::memory_order_relaxed),
                g_counters.actorLag[2].load(std::memory_order_relaxed),
                g_counters.actorLag[3].load(std::memory_order_relaxed),
                g_counters.actorMinor.load(std::memory_order_relaxed),
                g_counters.actorAsIs.load(std::memory_order_relaxed),
                g_counters.actorSnaps.load(std::memory_order_relaxed),
                g_counters.actorDuplicates.load(std::memory_order_relaxed),
                g_counters.actorRestoreConflicts.load(std::memory_order_relaxed),
                g_counters.actorListRejected.load(std::memory_order_relaxed),
                g_counters.actorNoPlayer.load(std::memory_order_relaxed),
                g_counters.actorOverflow.load(std::memory_order_relaxed)};
            AppendStatus(
                "actors passes=%llu tracked=%llu nodes=%llu poses lag0=%llu lag1=%llu "
                "lag2=%llu lag3=%llu minor=%llu as_is=%llu snaps=%llu duplicates=%llu "
                "restore_conflicts=%llu | list rejected=%llu not_high=%llu overflow=%llu "
                "table_resets=%llu",
                now[0] - last[0], now[1] - last[1], now[2] - last[2], now[3] - last[3],
                now[4] - last[4], now[5] - last[5], now[6] - last[6], now[7] - last[7],
                now[8] - last[8], now[9] - last[9], now[10] - last[10], now[11] - last[11],
                now[12] - last[12], now[13] - last[13], now[14] - last[14],
                g_counters.actorTableResets.load(std::memory_order_relaxed));
            memcpy(last, now, sizeof(last));
        }
        {
            static uint64_t last[11]{};
            const uint64_t now[11] = {
                g_counters.objectWalks.load(std::memory_order_relaxed),
                g_counters.objectCellsRead.load(std::memory_order_relaxed),
                g_counters.objectReferences.load(std::memory_order_relaxed),
                g_counters.objectChanges[0].load(std::memory_order_relaxed),
                g_counters.objectChanges[1].load(std::memory_order_relaxed),
                g_counters.objectChanges[2].load(std::memory_order_relaxed),
                g_counters.objectChanges[3].load(std::memory_order_relaxed),
                g_counters.objectSubtrees.load(std::memory_order_relaxed),
                g_counters.objectListRejected.load(std::memory_order_relaxed),
                g_counters.objectGridRejected.load(std::memory_order_relaxed),
                g_counters.objectTableFull.load(std::memory_order_relaxed)};
            AppendStatus(
                "objects walks=%llu cells=%u cells_read=%llu references=%llu root changes "
                "tick=%llu after=%llu both=%llu neither=%llu subtrees=%llu active_max=%u "
                "controlled=%u | list_rejected=%llu grid_rejected=%llu table_full=%llu",
                now[0] - last[0], g_counters.objectCells.load(std::memory_order_relaxed),
                now[1] - last[1], now[2] - last[2], now[3] - last[3], now[4] - last[4],
                now[5] - last[5], now[6] - last[6], now[7] - last[7],
                g_counters.objectActiveMax.exchange(0, std::memory_order_relaxed),
                g_counters.objectControlled.load(std::memory_order_relaxed), now[8] - last[8],
                now[9] - last[9], now[10] - last[10]);
            memcpy(last, now, sizeof(last));
        }
        {
            static uint64_t last[10]{};
            const uint64_t now[10] = {
                g_counters.fewDrawFrames.load(std::memory_order_relaxed),
                g_counters.fpMixed.load(std::memory_order_relaxed),
                g_counters.fpLateAfterTick.load(std::memory_order_relaxed),
                g_counters.bodyMixed.load(std::memory_order_relaxed),
                g_counters.bodyLateAfterTick.load(std::memory_order_relaxed),
                g_counters.offsetMixed.load(std::memory_order_relaxed),
                g_counters.killCamTickPoses.load(std::memory_order_relaxed),
                g_counters.fovLateAfterTick.load(std::memory_order_relaxed),
                g_counters.cameraRelationDrift.load(std::memory_order_relaxed),
                g_counters.cameraRelationChanges.load(std::memory_order_relaxed)};
            AppendStatus(
                "phase draws_per_tick=%.2f few_draw_frames=%llu | first mixed=%llu "
                "late_after_tick=%llu | body mixed=%llu late_after_tick=%llu | offset "
                "mixed=%llu | killcam tick_poses=%llu | fov late_after_tick=%llu | relation "
                "drift_written=%llu jumps=%llu largest=%.6f",
                g_counters.drawsPerTickCenti.load(std::memory_order_relaxed) / 100.0,
                now[0] - last[0], now[1] - last[1], now[2] - last[2],
                now[3] - last[3], now[4] - last[4], now[5] - last[5], now[6] - last[6],
                now[7] - last[7], now[8] - last[8], now[9] - last[9],
                g_counters.cameraRelationMicro.exchange(0, std::memory_order_relaxed) / 1.0e6);
            memcpy(last, now, sizeof(last));
        }
        {
            static uint64_t last[15]{};
            uint64_t now[15];
            for (size_t index = 0; index < 4; ++index) {
                now[index] = g_counters.positionChanges[index].load(std::memory_order_relaxed);
            }
            now[4] = g_counters.positionLate.load(std::memory_order_relaxed);
            for (size_t index = 0; index < 5; ++index) {
                now[5 + index] = g_counters.killCamAfterTick[index].load(std::memory_order_relaxed);
                now[10 + index] =
                    g_counters.killCamLargeAfterTick[index].load(std::memory_order_relaxed);
            }
            AppendStatus(
                "position changes by kind %llu/%llu/%llu/%llu late=%llu | killcam changes by draws "
                "after the tick 0/1/2/3/4+ all %llu/%llu/%llu/%llu/%llu large "
                "%llu/%llu/%llu/%llu/%llu",
                now[0] - last[0], now[1] - last[1], now[2] - last[2], now[3] - last[3],
                now[4] - last[4], now[5] - last[5], now[6] - last[6], now[7] - last[7],
                now[8] - last[8], now[9] - last[9], now[10] - last[10], now[11] - last[11],
                now[12] - last[12], now[13] - last[13], now[14] - last[14]);
            memcpy(last, now, sizeof(last));
        }
        lastFrozen = frozen;
        lastIneligible = ineligible;
        lastWarmUp = warmUp;
        lastNative = nativeFrames;
        lastPolls = polls;
        lastDeferred = deferred;
        lastDelivered = delivered;
        lastTaps = taps;
        lastTicks = ticks;
        lastPassThrough = passThrough;
        for (size_t i = 0; i < kAuxTimerSiteCount; ++i) {
            lastAuxCalls[i] = auxCalls[i];
            lastAuxNoSleep[i] = auxNoSleep[i];
        }

        lastFrames = frames;
        lastTimerUpdates = timerUpdates;
        lastWallTicks = wallTicks;
        lastInsideTicks = insideTicks;
        lastEngineMicros = engineMicros;
        lastRealMicros = realMicros;
        lastFlagFrames = flagFrames;
        lastSlow = slow;
    }
    return 0;
}

// ---- Installation ----------------------------------------------------------

void ReadConfiguration() {
    const std::wstring path = ModuleSiblingPath(L"FalloutNVHFR.ini");
    g_enabled = GetPrivateProfileIntW(L"General", L"bEnabled", 1, path.c_str()) != 0;
    g_telemetryEnabled = GetPrivateProfileIntW(L"General", L"bTelemetry", 0, path.c_str()) != 0;
    g_fixedTickEnabled = GetPrivateProfileIntW(L"Timing", L"bFixedTick", 1, path.c_str()) != 0;
    g_noTimerSleepsEnabled =
        GetPrivateProfileIntW(L"Timing", L"bNoTimerSleeps", 1, path.c_str()) != 0;

    g_freezeFramesEnabled =
        g_fixedTickEnabled &&
        GetPrivateProfileIntW(L"Timing", L"bFreezeFrames", 1, path.c_str()) != 0;
    g_alignHavokStepEnabled =
        g_fixedTickEnabled &&
        GetPrivateProfileIntW(L"Timing", L"bAlignHavokStep", 1, path.c_str()) != 0;
    g_menuRealTimeEnabled =
        GetPrivateProfileIntW(L"Timing", L"bMenuRealTime", 1, path.c_str()) != 0;
    g_latchInputEnabled =
        g_fixedTickEnabled &&
        GetPrivateProfileIntW(L"Input", L"bLatchInput", 1, path.c_str()) != 0;
    g_lookInputRealDeltaEnabled =
        g_fixedTickEnabled &&
        GetPrivateProfileIntW(L"Input", L"bLookInputRealDelta", 1, path.c_str()) != 0;
    g_fixedGrabForceEnabled =
        g_fixedTickEnabled &&
        GetPrivateProfileIntW(L"Timing", L"bFixedGrabForce", 1, path.c_str()) != 0;
    g_menuMouseNoAcceleration =
        GetPrivateProfileIntW(L"Input", L"bMenuMouseNoAcceleration", 1, path.c_str()) != 0;

    g_interpolateCameraEnabled =
        g_freezeFramesEnabled &&
        GetPrivateProfileIntW(L"Presentation", L"bInterpolateCamera", 1, path.c_str()) != 0;

    g_shiftSkyEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bShiftSky", 1, path.c_str()) != 0;
    g_thirdPersonCameraEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bThirdPersonCamera", 1, path.c_str()) != 0;
    g_thirdPersonBodyEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bThirdPersonBody", 1, path.c_str()) != 0;
    g_firstPersonEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bFirstPerson", 1, path.c_str()) != 0;
    g_interpolateActorsEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bInterpolateActors", 1, path.c_str()) != 0;
    g_interpolateObjectsEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bInterpolateObjects", 1, path.c_str()) != 0;
    g_interpolateFovEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bInterpolateFov", 1, path.c_str()) != 0;
    g_firstPersonPassCameraEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bFirstPersonPassCamera", 1, path.c_str()) != 0;
    g_firstPersonWorldEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bFirstPersonWorld", 1, path.c_str()) != 0;
    g_killCamCameraEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bKillCamCamera", 1, path.c_str()) != 0;
    g_lowRateRulesEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bLowRateRules", 1, path.c_str()) != 0;
    g_latePlayerPositionEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bLatePlayerPosition", 1, path.c_str()) != 0;
    g_killCamTickPosesEnabled =
        GetPrivateProfileIntW(L"Presentation", L"bKillCamTickPoses", 1, path.c_str()) != 0;
    g_worldDumpEnabled = g_telemetryEnabled &&
        GetPrivateProfileIntW(L"Tracing", L"bWorldDump", 0, path.c_str()) != 0;
    g_fpTraceEnabled = g_telemetryEnabled &&
        GetPrivateProfileIntW(L"Tracing", L"bAimTrace", 0, path.c_str()) != 0;

    wchar_t value[64]{};
    GetPrivateProfileStringW(L"Timing", L"fSimulationHz", L"60.0", value, 64, path.c_str());
    double hz = wcstod(value, nullptr);
    // Never more than 60 ticks a second.
    if (!(hz >= 20.0)) hz = 20.0;
    if (hz > 60.0) hz = 60.0;
    g_simulationStepMs = 1000.0 / hz;
    GetPrivateProfileStringW(L"Timing", L"fMaxWallDeltaMs", L"250.0", value, 64, path.c_str());
    g_maxWallDeltaMs = wcstod(value, nullptr);
    if (!(g_maxWallDeltaMs >= g_simulationStepMs)) g_maxWallDeltaMs = 250.0;
}

bool VerifyTargetLayout() {
    const HMODULE executable = GetModuleHandleW(nullptr);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(executable);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(
        reinterpret_cast<const uint8_t *>(executable) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        AppendStatus("layout: not a 32-bit executable");
        return false;
    }
    AppendStatus("executable timestamp=%08lX (reference %08lX) base=%08IX",
                 nt->FileHeader.TimeDateStamp, kReferencePeTimestamp,
                 reinterpret_cast<uintptr_t>(executable));
    if (reinterpret_cast<uintptr_t>(executable) != kImageBase) {
        AppendStatus("layout: image is not at %08IX", kImageBase);
        return false;
    }
    for (const LayoutAnchor &anchor : kLayoutAnchors) {
        if (!MatchesBytes(anchor.address, anchor.bytes, anchor.length)) {
            AppendStatus("layout: %s is not referenced at %08IX as expected", anchor.what,
                         anchor.address);
            return false;
        }
    }
    if (!MatchesBytes(kFrameCall, kFrameCallBytes, 5)) {
        AppendStatus("layout: frame call at %08IX does not hold the expected bytes", kFrameCall);
        return false;
    }
    if (!MatchesBytes(kTimerCall, kTimerCallBytes, 5)) {
        AppendStatus("layout: timer call at %08IX does not hold the expected bytes", kTimerCall);
        return false;
    }
    for (const CompositeSite &site : kCompositeSites) {
        if (!MatchesBytes(site.address, site.bytes, 5)) {
            AppendStatus("layout: composite render call at %08IX does not hold the expected "
                         "bytes", site.address);
            return false;
        }
    }
    for (const FirstPersonPassSite &site : kFirstPersonPassSites) {
        if (!MatchesBytes(site.address, site.bytes, 5)) {
            AppendStatus("layout: first-person draw call at %08IX does not hold the expected "
                         "bytes", site.address);
            return false;
        }
    }
    for (const WorldDrawSite &site : kWorldDrawSites) {
        if (!MatchesBytes(site.address, site.bytes, 5)) {
            AppendStatus("layout: world draw call at %08IX does not hold the expected bytes",
                         site.address);
            return false;
        }
    }
    if (!MatchesBytes(kInputPollCall, kInputPollCallBytes, 5)) {
        AppendStatus("layout: input poll call at %08IX does not hold the expected bytes",
                     kInputPollCall);
        return false;
    }
    for (const AuxTimerSite &site : kAuxTimerSites) {
        if (!MatchesBytes(site.address, site.bytes, 5)) {
            AppendStatus("layout: call at %08IX (%s) does not hold the expected bytes",
                         site.address, site.what);
            return false;
        }
    }
    return true;
}

// Mods that patch the same timer. They may load after this plugin, so their
// files are looked for instead of their patches.
//
// NVTF patches the timer only when [Main] bGTCFix is set in its INI: that one
// switch gates its GetTickCount replacement, its timer hook and its writes to
// the Havok maximum step. Its other parts (fast exit, threading, hash tables,
// DirectX) patch none of this plugin's sites. With bGTCFix=0 it is accepted.
// The INI is read the way NVTF reads it, with the same default.
const wchar_t *FindConflictingMod() {
    static const wchar_t kNvtf[] = L"Data\\NVSE\\Plugins\\NVTF.dll";
    if (FileExists(GameDirectoryPath(kNvtf))) {
        const std::wstring ini = GameDirectoryPath(L"Data\\NVSE\\Plugins\\NVTF.ini");
        const UINT gtcFix = GetPrivateProfileIntW(L"Main", L"bGTCFix", 0, ini.c_str());
        if (gtcFix != 0) return kNvtf;
        AppendStatus("NVTF is installed with bGTCFix=0 (its timer changes are off); continuing");
    }
    static const wchar_t kStutterRemover[] =
        L"Data\\NVSE\\Plugins\\sr_New_Vegas_Stutter_Remover.dll";
    if (FileExists(GameDirectoryPath(kStutterRemover))) return kStutterRemover;
    return nullptr;
}

BOOL CALLBACK InitializePatch(PINIT_ONCE, PVOID, PVOID *) {
    QueryPerformanceFrequency(&g_qpcFrequency);
    ReadConfiguration();
    AppendStatus("FalloutNVHFR %s starting (ASI plugin) enabled=%d telemetry=%d nvse_loaded=%d",
                 kPatchVersion, g_enabled ? 1 : 0, g_telemetryEnabled ? 1 : 0,
                 GetModuleHandleW(L"nvse_1_4.dll") != nullptr ? 1 : 0);
    if (!g_enabled) {
        AppendStatus("hooks disabled: bEnabled=0");
        return TRUE;
    }
    if (!VerifyTargetLayout()) {
        AppendStatus("hooks disabled: executable layout not recognised");
        return TRUE;
    }
    if (const wchar_t *conflict = FindConflictingMod()) {
        AppendStatus("hooks disabled: %ls is installed and patches the same timer (for NVTF, "
                     "set bGTCFix=0 in NVTF.ini to use both)", conflict);
        return TRUE;
    }

    const bool frameOk = PatchRelativeCall(kFrameCall, reinterpret_cast<const void *>(&GameFrameHook),
                                           kFrameCallBytes, g_frameCallWritten);
    const bool timerOk = frameOk &&
        PatchRelativeCall(kTimerCall, reinterpret_cast<const void *>(&TimerUpdateHook),
                          kTimerCallBytes, g_timerCallWritten);
    bool auxOk = frameOk && timerOk;
    for (size_t i = 0; auxOk && g_noTimerSleepsEnabled && i < kAuxTimerSiteCount; ++i) {
        g_auxTimerInstalled[i] = PatchRelativeCall(kAuxTimerSites[i].address, kAuxTimerHooks[i],
                                                   kAuxTimerSites[i].bytes, g_auxTimerWritten[i]);
        auxOk = g_auxTimerInstalled[i];
    }
    if (auxOk && g_latchInputEnabled) {
        g_inputPollInstalled =
            PatchRelativeCall(kInputPollCall, reinterpret_cast<const void *>(&InputPollHook),
                              kInputPollCallBytes, g_inputPollWritten);
        auxOk = g_inputPollInstalled;
    }
    for (size_t i = 0; auxOk && g_interpolateCameraEnabled && i < kWorldDrawSiteCount; ++i) {
        g_worldDrawInstalled[i] =
            PatchRelativeCall(kWorldDrawSites[i].address,
                              reinterpret_cast<const void *>(&WorldDrawHook),
                              kWorldDrawSites[i].bytes, g_worldDrawWritten[i]);
        auxOk = g_worldDrawInstalled[i];
    }
    for (size_t i = 0;
         auxOk && g_interpolateCameraEnabled && g_firstPersonEnabled && i < kCompositeSiteCount;
         ++i) {
        g_compositeInstalled[i] =
            PatchRelativeCall(kCompositeSites[i].address, kCompositeHooks[i],
                              kCompositeSites[i].bytes, g_compositeWritten[i]);
        auxOk = g_compositeInstalled[i];
    }
    if (auxOk && g_interpolateCameraEnabled && g_firstPersonEnabled &&
        g_firstPersonPassCameraEnabled) {
        for (size_t i = 0; auxOk && i < kFirstPersonPassSiteCount; ++i) {
            g_fpPassSiteInstalled[i] =
                PatchRelativeCall(kFirstPersonPassSites[i].address, kFirstPersonPassHooks[i],
                                  kFirstPersonPassSites[i].bytes, g_fpPassWritten[i]);
            auxOk = g_fpPassSiteInstalled[i];
        }
        g_fpPassInstalled = auxOk;
    }
    if (!frameOk || !timerOk || !auxOk) {
        for (size_t i = 0; i < kFirstPersonPassSiteCount; ++i) {
            if (g_fpPassSiteInstalled[i]) {
                RestoreBytes(kFirstPersonPassSites[i].address, kFirstPersonPassSites[i].bytes);
                g_fpPassSiteInstalled[i] = false;
            }
        }
        g_fpPassInstalled = false;
        for (size_t i = 0; i < kCompositeSiteCount; ++i) {
            if (g_compositeInstalled[i]) {
                RestoreBytes(kCompositeSites[i].address, kCompositeSites[i].bytes);
                g_compositeInstalled[i] = false;
            }
        }
        for (size_t i = 0; i < kWorldDrawSiteCount; ++i) {
            if (g_worldDrawInstalled[i]) {
                RestoreBytes(kWorldDrawSites[i].address, kWorldDrawSites[i].bytes);
                g_worldDrawInstalled[i] = false;
            }
        }
        if (g_inputPollInstalled) {
            RestoreBytes(kInputPollCall, kInputPollCallBytes);
            g_inputPollInstalled = false;
        }
        for (size_t i = 0; i < kAuxTimerSiteCount; ++i) {
            if (g_auxTimerInstalled[i]) {
                RestoreBytes(kAuxTimerSites[i].address, kAuxTimerSites[i].bytes);
                g_auxTimerInstalled[i] = false;
            }
        }
        if (timerOk) RestoreBytes(kTimerCall, kTimerCallBytes);
        if (frameOk) RestoreBytes(kFrameCall, kFrameCallBytes);
        AppendStatus("hooks rolled back after partial installation failure (frame=%d timer=%d "
                     "other timers=%d)",
                     frameOk ? 1 : 0, timerOk ? 1 : 0, auxOk ? 1 : 0);
        return TRUE;
    }
    if (g_interpolateFovEnabled && g_interpolateCameraEnabled) {
        // Not essential: a site that does not match is left as it is.
        uint32_t sites = 0;
        uint8_t written[5];
        for (const FovSite &site : kFovSceneSites) {
            if (PatchRelativeCall(site.address, reinterpret_cast<const void *>(&FovSceneThunk),
                                  site.bytes, written)) {
                ++sites;
            }
        }
        for (const FovSite &site : kFovGlobalSites) {
            if (PatchRelativeCall(site.address, reinterpret_cast<const void *>(&FovGlobalThunk),
                                  site.bytes, written)) {
                ++sites;
            }
        }
        g_counters.fovSites.store(sites, std::memory_order_relaxed);
    }
    if (g_lookInputRealDeltaEnabled) {
        // Not essential: if the site does not match, look input keeps the
        // engine's delta.
        uint8_t written[5];
        g_lookInputRealDeltaEnabled =
            PatchRelativeCall(kLookInputCall, reinterpret_cast<const void *>(&LookInputHook),
                              kLookInputCallBytes, written);
        if (!g_lookInputRealDeltaEnabled) {
            AppendStatus("look input call at %08IX does not hold the expected bytes; left as it "
                         "is", kLookInputCall);
        }
    }
    if (g_fixedGrabForceEnabled) {
        // Not essential: both sites or neither, so that the two kinds of
        // held body behave alike.
        if (MatchesBytes(kGrabHelperActorCall, kGrabHelperActorCallBytes, 5) &&
            MatchesBytes(kGrabHelperBodyCall, kGrabHelperBodyCallBytes, 5)) {
            uint8_t written[5];
            g_fixedGrabForceEnabled =
                PatchRelativeCall(kGrabHelperActorCall,
                                  reinterpret_cast<const void *>(&GrabHelperActorHook),
                                  kGrabHelperActorCallBytes, written) &&
                PatchRelativeCall(kGrabHelperBodyCall,
                                  reinterpret_cast<const void *>(&GrabHelperBodyHook),
                                  kGrabHelperBodyCallBytes, written);
        } else {
            g_fixedGrabForceEnabled = false;
        }
        if (!g_fixedGrabForceEnabled) {
            AppendStatus("held-body push calls do not hold the expected bytes; left as they are");
        }
    }
    g_hooksInstalled.store(true, std::memory_order_release);
    if (g_telemetryEnabled) {
        g_logThread = CreateThread(nullptr, 0, LogThread, nullptr, 0, nullptr);
    }
    AppendStatus("installed: frame hook=1 timer hook=1 fixed_tick=%d sim_hz=%.3f "
                 "no_timer_sleeps=%d max_wall_delta_ms=%.1f freeze_frames=%d "
                 "align_havok_step=%d fixed_grab_force=%d latch_input=%d look_input_real_delta=%d "
                 "menu_mouse_no_acceleration=%d interpolate_camera=%d sky=%d third_person_camera=%d "
                 "third_person_body=%d first_person=%d first_person_pass_camera=%d "
                 "first_person_world=%d kill_cam_camera=%d interpolate_fov=%d actors=%d "
                 "objects=%d menu_real_time=%d low_rate_rules=%d late_player_position=%d "
                 "kill_cam_tick_poses=%d",
                 g_fixedTickEnabled ? 1 : 0, 1000.0 / g_simulationStepMs,
                 g_noTimerSleepsEnabled ? 1 : 0, g_maxWallDeltaMs,
                 g_freezeFramesEnabled ? 1 : 0, g_alignHavokStepEnabled ? 1 : 0,
                 g_fixedGrabForceEnabled ? 1 : 0,
                 g_latchInputEnabled ? 1 : 0, g_lookInputRealDeltaEnabled ? 1 : 0,
                 g_menuMouseNoAcceleration ? 1 : 0,
                 g_interpolateCameraEnabled ? 1 : 0, g_shiftSkyEnabled ? 1 : 0,
                 g_thirdPersonCameraEnabled ? 1 : 0, g_thirdPersonBodyEnabled ? 1 : 0,
                 g_firstPersonEnabled ? 1 : 0, g_fpPassInstalled ? 1 : 0,
                 g_firstPersonWorldEnabled ? 1 : 0, g_killCamCameraEnabled ? 1 : 0,
                 g_interpolateFovEnabled ? 1 : 0, g_interpolateActorsEnabled ? 1 : 0,
                 g_interpolateObjectsEnabled ? 1 : 0, g_menuRealTimeEnabled ? 1 : 0,
                 g_lowRateRulesEnabled ? 1 : 0, g_latePlayerPositionEnabled ? 1 : 0,
                 g_killCamTickPosesEnabled ? 1 : 0);
    return TRUE;
}

} // namespace

// An ASI loader loads the file at process start and the patch is installed
// from DllMain. That runs under the loader lock, which is why installation
// uses nothing that loads further modules: it reads an INI, compares bytes,
// patches call sites and creates a thread. InitializeASI is the entry some
// loaders call after loading; it leads to the same once-only installation.
extern "C" __declspec(dllexport) void __cdecl InitializeASI() {
    InitOnceExecuteOnce(&g_patchInitOnce, InitializePatch, nullptr, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        InitOnceExecuteOnce(&g_patchInitOnce, InitializePatch, nullptr, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stop.store(true, std::memory_order_release);
    }
    return TRUE;
}

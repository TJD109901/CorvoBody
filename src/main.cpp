// CorvoBody: first-person body awareness for Dishonored (2012), retail Steam exe.
//
// Loaded as a dinput8.dll proxy. Hooks UObject::ProcessEvent, and on the player's PlayerTick:
//   1. async-loads a DLC package that contains Corvo's body mesh (Knife of Dunwall / Brigmore Witches),
//   2. creates a SkeletalMeshComponent with that mesh on the player pawn,
//   3. drives it from the player's own first-person mesh (full-body skeleton) via SetParentAnimComponent.
// No hotkeys and no ini (build 201): the settings are fixed (baked_settings.h); Body Swap is chosen in Pause > Mods.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>

#include "sdk.hpp"
#include "MinHook.h"

// ---------------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------------
static FILE* g_log = nullptr;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------------------------------------
// Retail engine addresses (Dishonored.exe sha256 66443f3d..., image base 0x400000).
// From dishonoredrecompiled's 2013 symbol tables and dismod's SDK; see MODLOG.md.
// ---------------------------------------------------------------------------------------------
namespace addr {
    constexpr uintptr_t ProcessEvent          = 0x00470640;
    constexpr uintptr_t GameEngineTick        = 0x00632860;  // UGameEngine::Tick(float)
    constexpr uintptr_t StaticConstructObject = 0x004955A0;
    constexpr uintptr_t GObjObjects_rva       = 0x01023630;
    constexpr uintptr_t GNames_rva            = 0x01035674;
    constexpr uintptr_t GWorld                = 0x01449888;
    constexpr uintptr_t PlayerPawnInstance    = 0x0145F628;  // ADishonoredPlayerPawn::s_pInstance
}

using ProcessEvent_t = void(__thiscall*)(UObject*, UFunction*, void*, void*);
// FName is passed by value (Index, Number) as two dwords. Don't use the SDK FName type here: it has a
// user-defined copy constructor, so the MinGW ABI would pass it by pointer and shift every later argument.
using StaticConstructObject_t = UObject*(__cdecl*)(UClass*, UObject*, int32_t nameIndex, int32_t nameNumber, uint64_t flags,
                                                   UObject* tmpl, void* error, UObject* subobjectRoot, void* instanceGraph);
using LoadPackageAsync_t = void(__cdecl*)(const FString&, void(__cdecl*)(UObject*, void*), void*, const void*);

static ProcessEvent_t g_origProcessEvent = nullptr;
static auto StaticConstructObject = reinterpret_cast<StaticConstructObject_t>(addr::StaticConstructObject);
static LoadPackageAsync_t LoadPackageAsync = nullptr;

// ---------------------------------------------------------------------------------------------
// Pattern scan (".text" of Dishonored.exe)
// ---------------------------------------------------------------------------------------------
static uintptr_t FindPattern(const char* pattern) {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    uint8_t* start = base + nt->OptionalHeader.BaseOfCode;
    size_t size = nt->OptionalHeader.SizeOfCode;

    uint8_t bytes[128];
    bool wild[128];
    int n = 0;
    for (const char* p = pattern; *p && n < 128;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { wild[n] = true; bytes[n++] = 0; while (*p == '?') ++p; continue; }
        wild[n] = false;
        bytes[n++] = static_cast<uint8_t>(strtoul(std::string(p, 2).c_str(), nullptr, 16));
        p += 2;
    }
    for (size_t i = 0; i + n <= size; ++i) {
        int j = 0;
        while (j < n && (wild[j] || start[i + j] == bytes[j])) ++j;
        if (j == n) return reinterpret_cast<uintptr_t>(start + i);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
struct Settings {
    bool enabled = true;           // spawn the body automatically
    bool hideArms = true;          // hide the body's arms (the first-person arms stay)
    int attachMode = 1;            // 0 = attach to the 1P mesh root bone, 1 = attach to pawn and copy 1P transform
    float offsetX = 0, offsetY = 0, offsetZ = 3.0f;  // extra offset in the 1P mesh's space (Z 3: feet clipped the floor)
    float thirdPersonDist = 200.0f; // hide the body if the camera is further than this from the eyes (cutscenes; a full corner lean looking down is ~121)
    float runBlendStart = 420.0f;  // uu/s where the run starts mixing in (full-stick walk is ~400)
    float runBlendEnd = 590.0f;    // uu/s where it is all run (sprint tops out at ~600)
    bool hideTorsoCrouched = false; // hide the upper body while crouched (only useful with the crouch pose)
    float walkAnimRate = 1.5f;     // play rate of the walk cycle (guards walk slower than Corvo)
    float runAnimRate = 1.0f;      // play rate of the run cycle
    float walkAnimSpeed = 270.0f;  // uu/s the guard walk cycle covers at rate 1 (rate = speed / this)
    float runAnimSpeed = 460.0f;   // uu/s the run cycle covers at rate 1 (460: sprint plays ~1.3x to match footsteps)
    bool footLock = true;          // a walk/run clip that steps the wrong way for its direction is played in reverse
    float walkBackRate = 1.0f;     // backwards step rate relative to forwards (1 = same cadence)
    float walkSideRate = 1.0f;     // sideways (strafe) step rate relative to forwards
    // Procedural crouch-walk
    int crouchLegSpace = 4;        // EBoneControlSpace: 2 component, 3 parent bone, 4 bone
    int crouchLegAxis = 0;         // 0 pitch, 1 yaw, 2 roll (cycle in game with F7); legs hinge on local Y = pitch
    float hipSwingDeg = 0.0f;      // procedural crouch-walk swing (off: foot IK + walk cycles instead)
    float kneeBendDeg = 0.0f;
    float kneeSign = 1.0f;         // -1 if the knees bend the wrong way
    float crouchStride = 45.0f;    // uu travelled per step
    float crouchOffsetX = 0.0f;    // move the body back while crouched (only useful with the crouch pose)
    bool useCrouchPose = false;    // play the Whaler crouch pose when crouched (default: walk cycles + foot IK)
    bool footIK = true;            // plant the feet on the floor (makes crouching bend the knees)
    float footOffset = 10.0f;      // ankle height above the sole
    float footMaxUp = 80.0f;       // cap on the foot lift (slides sink ~54 + extra)
    float footLiftExtra = 4.0f;    // extra lift on top of the measured drop (test 37: feet dipped slightly)
    float armSpreadDeg = 35.0f;    // crouched + hand free: swing that arm out from the shoulder (negative flips)
    float elbowBendDeg = 25.0f;    // crouched: extra elbow bend on top of the animation (negative flips)
    bool headShadow = true;        // invisible head that only casts a shadow
    bool shadowBody = true;        // invisible full body (coat + arms) casts the shadow instead of the visible one
    float cameraFollow = 1.0f;     // visible body follows the camera's sideways/forward bob (0 = off, 1 = fully)
    float cameraFollowMax = 40.0f; // most it may shift (uu)
    float cameraFollowDown = 1.0f; // visible body also dips when the camera is below eye height (stairs), 0..1
    float hideUpperDrop = 8.0f;   // hide the upper body when the camera is this far below the eyes, or airborne
    float hideUpperSpeed = 470.0f; // ...and above this speed (sprint: the collar edges into view)
    bool hideTorso = true;         // always hide the visible body's chest/collar (Torso_jnt) - not the shadow's
    bool hideUpperWhenClipping = false;  // hide the whole upper body (incl. arms) while airborne/sprinting/looking up/dipping
    bool hideShouldersUp = false;  // always hide spine_3 and everything above it (shoulders, arms, chest, neck) on the visible body
    float slideRaise = 5.0f;       // move the body up (+) / down (-) while sliding: hips just above the ground (user reference image)
    float slideBack = -10.0f;      // ...and back (+) / forward (-): camera over the hips looking down both legs (user: only a foot showed at 30)
    float deepCrouchSink = 35.0f;   // body sunk more than this (normal crouch 22.5, slide/low crouch 54.5): hide the torso
    bool lowCrouchHideLegs = true;  // ...and (not sliding) fold the legs away into the hips, e.g. crouching under tables
    bool takedownHideTorso = true;  // hide the visible torso during choke / assassination takedowns (the camera leans in)
    float hitCameraScale = 0.3f;    // how far the camera is thrown when hit (1 = the game's own, 0.3 = 70% less)
    float lookUpHideTorsoDeg = 45.0f; // looking up more than this: hide the visible torso from the waist (0 = off)
    bool lookUpSteady = true;       // looking up (30-60 deg and beyond): camera held steady at the eyes, no walking bob sway
    bool hitHideTorso = true;       // also hide the visible torso when hit / stunned (build 195: the smaller kick alone left a little)
    float hitHideMs = 600.0f;       // ...for this long after each hit (then the usual hold and ease back)
    float lowCrouchLegsBack = 2.5f; // how fast the legs grow back out afterwards (per second; 2.5 = ~0.4 s)
    float mantleCamRelease = 6.0f;  // how fast the held mantle camera is let go (per second; 6 = ~0.17 s)
    bool mantleInfluenceHide = true; // hide during the camera's step-up/crouch mantle moves
    bool takeoffHide = false;       // hide the body for the first 3 frames of every jump (mantle detection lags 1-2 frames)
    bool mantleFollow = false;        // visible body moves back with the camera while mantling/vaulting
    bool mantleHide = true;         // hide the visible body while the game pulls the camera back/up (mantling)
    float mantleHideBack = 25.0f;   // uu behind its usual spot
    float mantleHideUp = 30.0f;     // uu above its usual height while airborne
    bool swimPose = true;           // paddling legs + hidden torso while swimming
    int swimPhysics = 3;            // the game's swimming physics mode (log)
    float swimKickRate = 1.0f;      // kicks per second (scaled 0.6..1.6 by speed)
    float swimKickHip = 25.0f;      // hip swing each way (degrees)
    float swimKickKnee = 40.0f;     // extra knee bend on the back-swing (degrees)
    int slidePhysics = 14;          // the game's slide physics mode (-1 = any; cylinder height alone matched crawling/swimming)
    float slideFrontTwist = 60.0f;  // slide: twist the extended right leg (degrees) so the foot turns over; flip the sign if it turns the wrong way
    float slideFrontInward = 15.0f; // slide: swing the extended right leg in toward the middle (degrees; flip if it goes outward)
    float slideGetUpTuck = 0.0f;    // 0 = legs blend straight from the slide into the crouch, 1 = draw them in first
    float slideGetUpTime = 0.6f;    // seconds for the legs to draw in from the slide pose into the crouch
    float slideRecoverSpeed = 90.0f; // uu/s the body rises from the slide height back to the crouch
    float slideRecoverDown = 0.0f;  // as the slide ends: body tucked this much down...
    float slideRecoverBack = 30.0f; // ...and back (fades out with the slide pose)
    float slideCameraUp = 25.0f;   // sliding: raise the view this much (the game drops it to hip height)
    float footIKFadeLift = 18.0f;   // crouch leg IK reaches full strength at this lift (uu); lower = sooner
    float crouchMoveBack = 10.0f;  // visible body eases back this much while crouch-walking (chest out of view)
    float crouchHidePitch = -30.0f; // crouched and looking above this pitch: hide the visible upper body
    float slideRecline = 45.0f;    // ...upper body leans back this many degrees (and is hidden from view)
    float hideUpperPitch = 20.0f;  // ...and when looking up more than this (degrees): the camera pivots back into the neck
    bool climbDangle = true;        // ladders/chains: legs dangle (simulated pendulum) instead of stepping
    float dangleSwing = 1.0f;       // how much movement swings them
    float dangleStiffness = 30.0f;  // pull back to hanging (higher = quicker, tighter swing)
    float dangleDamping = 4.0f;     // how quickly the swinging dies down
    float climbSideShift = 15.0f;   // chain: body moved this far left (negative = right) so the chain is between the legs
    float climbForwardShift = 0.0f; // ...and forward (+) / back (-)
    float climbCross = 14.0f;       // chain grip: hips turned in so the legs cross at the ankles (flip the sign if they splay)
    float climbGripHip = -60.0f;    // chain grip: thighs raised
    float climbGripKnee = 85.0f;    // ...knees bent, shins hanging down the chain
    int climbPhysics = 9;          // EPhysics value used on chains/ladders (UE3 PHYS_Ladder = 9; the log shows changes)
    float climbHipDeg = -60.0f;    // climbing: how far each leg lifts (same sign as the slide hips)
    float climbKneeDeg = 70.0f;    // climbing: knee bend on the raised leg
    float climbStride = 30.0f;     // uu climbed per step
    float turnThreshold = 40.0f;  // deg/s of turning (while standing still) that starts the stepping animation
    float turnAnimDegPerSec = 90.0f;  // turn speed the step animation plays at rate 1
    float runBlendMax = 1.0f;      // most of the guard run mixed in at full sprint (user preferred the full run)
    float slideHipDeg = -85.0f;    // slide pose (Dishonored 2 style: both legs out front): front leg (left) hip
    float slideKneeDeg = 5.0f;     // slide pose: front knee (left), nearly straight
    float slideBackHipDeg = -60.0f;  // slide pose: other leg (right) hip, also forward
    float slideBackKneeDeg = 95.0f;  // slide pose: other knee (right), slightly bent
    float slideHeight = 45.0f;     // cylinder height below which a crouch counts as a slide (crouch 65, slide 33)
    float crouchSmoothSpeed = 70.0f;   // uu/s the body follows the instant crouch drop
    int coatMode = 1;              // 1 = hide the lower coat while crouched (as if it trails behind), 0 = keep it
    float coatHangStrength = 0.7f; // mode 2: 0 = follow the thighs, 1 = hang straight down
    float coatHangPitch = -90.0f;  // mode 2: panel direction in player space (bone +X runs down the panel)
    float coatHangYaw = 0.0f, coatHangRoll = 0.0f;
    float coatRearFlare = 15.0f;   // swing the rear coat panels back off the legs (degrees, bone-space pitch; flip if inward)
    float footLiftSign = -1.0f;    // the foot bone's -X points up the shin
    float kneeForward = 60.0f, kneeSide = 18.0f, kneeHeight = -30.0f;  // knee pole, player space (side = how far the knees splay)
    float crouchOffsetZ = 0.0f;    // and up/down
    char crouchHideBone[64] = "spine_3_jnt";  // hidden (with everything above it) while crouched
    bool rigidCamera = true;        // remove the camera's pitch arc: the view pivots in place when looking up/down
    bool rigidCameraVertical = false;  // ...including the up/down part of the arc
    float rigidCameraPivot = -85.0f;  // pitch (degrees) whose horizontal camera spot is kept at every pitch
    float rigidCameraForward = 0.0f;  // extra uu forward (+) / back (-) of that spot
    bool rigidCameraArms = true;    // move the 1P arms/weapons with the fixed camera
    bool rigidCameraBodyFollow = true;  // move the visible body forward with the fixed camera (keeps arms in view)
    bool rigidCameraLearn = false;
    float lookUpHideDeg = 70.0f;    // hide the visible upper body above this pitch (0 = never); the lean handles most of it
    float leanUp = 0.6f;            // spine lean per degree of looking up (back)
    float leanDown = 0.25f;         // ...per degree of looking down (forward)
    float leanMax = 45.0f;          // most the spine bends (degrees)
    float leanSign = 1.0f;          // flip (-1) if Corvo leans forward when looking up
    float stairFollow = 1.0f;       // visible body follows the camera's stair lag (0..1, rigid camera only)
    float stairFollowMax = 40.0f;   // ...at most this far (uu)
    bool armIK = false;             // (old) shadow body hands IK-follow the first-person hands; superseded by FirstPersonArmShadow
    float armShadowPitchFollow = 0.3f;  // share of the view pitch the arm/item shadows keep (0 = always level)
    bool playerShadows = false;     // master switch: Corvo casts no shadow at all when 0 (user, 2026-10-06)
    bool fpArmShadow = true;        // the first-person arms themselves cast the arm shadow while a hand holds gear
    FVector armIKEye = { 8.0f, 0.0f, 8.0f };    // eye point from the shadow head bone (forward, right, up)
    FVector armIKElbow = { -30.0f, 60.0f, 0.0f }; // elbow pole in player space (back, out, up)
    float shadowArmSteady = 1.0f;   // 0..1: how much a gear-holding shadow arm drops the walk/run swing
    bool itemShadows = true;       // held items (sword, pistol, crossbow, Heart...) cast a shadow from the shadow body's hands
    bool shadowWeaponArms = true;  // shadow arms raise when that hand holds a weapon/power
    float shadowArmRaiseDeg = 100.0f; // percent of the per-item forward raise (ShadowArmPose)
    float shadowArmInDeg = 100.0f;  // percent of the per-item pull toward the middle (negative flips)
    float shadowElbowBendDeg = 100.0f; // percent of the per-item elbow bend (negative flips)
    float clipPushSpeed = 250.0f;   // uu/s the clip-guard push is eased in/out
    float slideEndPush = 0.0f;      // push-away started the moment a slide ends (eases back)
    float clipPushMax = 60.0f;      // most the clip guard pushes the body away before hiding it
    float clipGuardDist = 22.0f;    // hide the visible body while a hip/thigh/knee bone is closer than this to the camera
    bool viewGuard = true;          // ease the visible body back whenever a shoulder/collar/neck (or, looking ahead, an arm) is in view
    bool coatHideSprint = true;     // hide the visible coat while sprinting (eased in/out)
    float coatSprintSpeed = 470.0f; // ...above this speed (walk ~400, sprint ~600)
    float reappearSpeed = 3.0f;     // how fast hidden arms/chest/coat grow back (per second; 3 = ~0.33 s)
    bool jumpArms = true;           // jumping/falling with holstered hands: arms float up in front
    float jumpArmRaise = 60.0f;     // ...this far forward from the shoulder (degrees)
    float jumpArmIn = 10.0f;        // ...pulled in toward the middle
    float jumpArmElbow = 35.0f;     // ...elbow bend
    bool jumpHideTorso = true;      // hide the visible torso while airborne/vaulting/landing (camera clipped the coat)
    bool jumpPose = true;          // procedural jump/fall pose + landing squat
    float jumpHipDeg = -40.0f;      // lead hip in the air (same sign as the slide hips: negative = forward)
    float jumpKneeDeg = 60.0f;      // knee tuck in the air (trailing knee; the lead knee gets 70%)
    bool hoverPose = true;          // floating legs while Daud's Transversal holds you in the air (toes down, knees up, drifting)
    float hoverHipDeg = -22.0f;     // ...hips (negative = knees forward/up)
    float hoverKneeDeg = 38.0f;     // ...knee bend
    float hoverToeDeg = 40.0f;      // ...toes pointed down (negative flips if they point up)
    float hoverSwayDeg = 7.0f;      // ...size of the slow drifting motion
    float hoverSwayRate = 0.405f;   // ...drift cycles per second (user: 10% slower than 0.45)
    float hoverSpread = 1.2f;       // ...how far apart the two legs are, x the build 190 pose (user: 20% more)
    float jumpLandDip = 12.0f;      // uu the body squats on a hard landing
    float jumpBodyBack = 15.0f;     // uu the visible body eases back while airborne (camera clear of the chest)
    float jumpBodyDown = 8.0f;      // ...and down
    bool jumpHideUpper = true;      // hide the visible upper body mid-jump when moving fast and looking down
    float cornerLeanHips = 0.5f;   // share of the corner lean done by shifting the hips (rest = spine tilt)
    float armForwardDeg = 30.0f;   // holstered arms swing this far forward when looking down (visible body only)
    bool cornerLean = true;        // tilt the upper body sideways with the game's corner lean
    float leanSideSign = 1.0f;      // flip (-1) if the body tilts away from the lean
    float leanPivotHeight = 50.0f;  // uu from the spine pivot up to the shoulders (smaller = more tilt per uu of lean)
    float sprintBodyBack = 12.0f;   // uu the visible body eases back while sprinting (hides the collar/neck)  // re-measure the camera arc during play (off: built-in clean measurement)
};
static Settings g_cfg;
static char g_iniPath[MAX_PATH];   // the mod's folder (log, data files); CorvoBody.ini itself is no longer read

// Settings (build 201): fixed, nothing for players to adjust (user) - CorvoBody.ini is no longer read and the F6-F11
// keys are gone. The tuned values live in baked_settings.h; these look them up exactly as the ini reads did
// (GetPrivateProfileInt/String semantics: key names case-insensitive, missing key -> the default).
#include "baked_settings.h"
static const char* BakedSetting(const char* key) {
    for (const auto& kv : kBakedSettings) if (_stricmp(kv[0], key) == 0) return kv[1];
    return nullptr;
}
static int SettingInt(const char* key, int def) {
    const char* v = BakedSetting(key);
    return v ? atoi(v) : def;
}
static void SettingString(const char* key, const char* def, char* buf, size_t size) {
    const char* v = BakedSetting(key);
    strncpy(buf, v ? v : def, size - 1);
    buf[size - 1] = 0;
}
// Body Swap: which character's body / first-person arms to show (0 Corvo, 1 Daud). Read from the ini once at
// startup; after that the Body Swap mod-loader plugin sets them through CorvoBody_SetCharacter.
static volatile LONG g_bodyChar = 0;
static volatile LONG g_armsChar = 0;
static volatile LONG g_bladeChar = 0;
static volatile LONG g_bowChar = 0;     // 1 = Daud's wristbow
static volatile LONG g_powerChar = 0;   // 1 = Daud's power set
static volatile LONG g_charsSet = 0;   // 1 once the ini or the plugin has set them (the plugin may run first)

static void LoadSettings() {
    if (InterlockedExchange(&g_charsSet, 1) == 0) {
        // Body / arms / blade: 0 Corvo, 1 Daud, 2 Daud's High Overseer disguise (build 200).
        auto char3 = [](const char* key) { int v = SettingInt(key, 0); return (v >= 0 && v <= 2) ? v : 0; };
        g_bodyChar = char3("BodyCharacter");
        g_armsChar = char3("ArmsCharacter");
        g_bladeChar = char3("BladeCharacter");
        g_bowChar = SettingInt("CrossbowCharacter", 0) == 1 ? 1 : 0;
        g_powerChar = SettingInt("PowerCharacter", 0) == 1 ? 1 : 0;
    }
    auto readBool = [](const char* key, bool def) {
        return SettingInt(key, def ? 1 : 0) != 0;
    };
    auto readFloat = [](const char* key, float def) {
        char buf[64];
        SettingString(key, "", buf, sizeof(buf));
        return buf[0] ? static_cast<float>(atof(buf)) : def;
    };
    g_cfg.enabled = readBool("Enabled", g_cfg.enabled);
    g_cfg.hideArms = readBool("HideBodyArms", g_cfg.hideArms);
    g_cfg.attachMode = SettingInt("AttachMode", g_cfg.attachMode);
    g_cfg.offsetX = readFloat("OffsetX", g_cfg.offsetX);
    g_cfg.offsetY = readFloat("OffsetY", g_cfg.offsetY);
    g_cfg.offsetZ = readFloat("OffsetZ", g_cfg.offsetZ);
    g_cfg.thirdPersonDist = readFloat("ThirdPersonDistance", g_cfg.thirdPersonDist);
    g_cfg.runBlendStart = readFloat("RunBlendStart", g_cfg.runBlendStart);
    g_cfg.runBlendEnd = readFloat("RunBlendEnd", g_cfg.runBlendEnd);
    g_cfg.hideTorsoCrouched = readBool("HideTorsoWhenCrouched", g_cfg.hideTorsoCrouched);
    g_cfg.walkAnimRate = readFloat("WalkAnimRate", g_cfg.walkAnimRate);
    g_cfg.runAnimRate = readFloat("RunAnimRate", g_cfg.runAnimRate);
    g_cfg.walkAnimSpeed = readFloat("WalkAnimSpeed", g_cfg.walkAnimSpeed);
    g_cfg.runAnimSpeed = readFloat("RunAnimSpeed", g_cfg.runAnimSpeed);
    g_cfg.footLock = readBool("FootLock", g_cfg.footLock);
    g_cfg.walkBackRate = std::min(std::max(readFloat("WalkBackRate", g_cfg.walkBackRate), 0.2f), 3.0f);
    g_cfg.walkSideRate = std::min(std::max(readFloat("WalkSideRate", g_cfg.walkSideRate), 0.2f), 3.0f);
    g_cfg.crouchLegSpace = SettingInt("CrouchLegSpace", g_cfg.crouchLegSpace);
    g_cfg.crouchLegAxis = SettingInt("CrouchLegAxis", g_cfg.crouchLegAxis);
    g_cfg.hipSwingDeg = readFloat("HipSwingDegrees", g_cfg.hipSwingDeg);
    g_cfg.kneeBendDeg = readFloat("KneeBendDegrees", g_cfg.kneeBendDeg);
    g_cfg.kneeSign = readFloat("KneeSign", g_cfg.kneeSign);
    g_cfg.crouchStride = readFloat("CrouchStride", g_cfg.crouchStride);
    g_cfg.crouchOffsetX = readFloat("CrouchOffsetX", g_cfg.crouchOffsetX);
    g_cfg.useCrouchPose = readBool("UseCrouchPose", g_cfg.useCrouchPose);
    g_cfg.footIK = readBool("FootIK", g_cfg.footIK);
    g_cfg.footOffset = readFloat("FootOffset", g_cfg.footOffset);
    g_cfg.footLiftExtra = readFloat("FootLiftExtra", g_cfg.footLiftExtra);
    g_cfg.footLiftSign = readFloat("FootLiftSign", g_cfg.footLiftSign);
    g_cfg.armSpreadDeg = readFloat("ArmSpreadDegrees", g_cfg.armSpreadDeg);
    g_cfg.elbowBendDeg = readFloat("ElbowBendDegrees", g_cfg.elbowBendDeg);
    g_cfg.headShadow = readBool("HeadShadow", g_cfg.headShadow);
    g_cfg.shadowBody = readBool("ShadowBody", g_cfg.shadowBody);
    g_cfg.cameraFollow = readFloat("CameraFollow", g_cfg.cameraFollow);
    g_cfg.cameraFollowMax = readFloat("CameraFollowMax", g_cfg.cameraFollowMax);
    g_cfg.cameraFollowDown = readFloat("CameraFollowDown", g_cfg.cameraFollowDown);
    g_cfg.hideUpperDrop = readFloat("HideUpperBodyDrop", g_cfg.hideUpperDrop);
    g_cfg.hideUpperSpeed = readFloat("HideUpperBodySpeed", g_cfg.hideUpperSpeed);
    g_cfg.hideUpperPitch = readFloat("HideUpperBodyLookUp", g_cfg.hideUpperPitch);
    g_cfg.hideTorso = readBool("HideTorso", g_cfg.hideTorso);
    g_cfg.hideShouldersUp = readBool("HideShouldersUp", g_cfg.hideShouldersUp);
    g_cfg.hideUpperWhenClipping = readBool("HideUpperBodyWhenClipping", g_cfg.hideUpperWhenClipping);
    g_cfg.rigidCamera = readBool("RigidCamera", g_cfg.rigidCamera);
    g_cfg.rigidCameraVertical = readBool("RigidCameraVertical", g_cfg.rigidCameraVertical);
    g_cfg.rigidCameraPivot = readFloat("RigidCameraPivot", g_cfg.rigidCameraPivot);
    g_cfg.rigidCameraForward = readFloat("RigidCameraForward", g_cfg.rigidCameraForward);
    g_cfg.rigidCameraArms = readBool("RigidCameraArms", g_cfg.rigidCameraArms);
    g_cfg.rigidCameraLearn = readBool("RigidCameraLearn", g_cfg.rigidCameraLearn);
    g_cfg.sprintBodyBack = readFloat("SprintBodyBack", g_cfg.sprintBodyBack);
    g_cfg.lookUpHideDeg = readFloat("LookUpHideDegrees", g_cfg.lookUpHideDeg);
    g_cfg.leanUp = readFloat("LeanUp", g_cfg.leanUp);
    g_cfg.leanDown = readFloat("LeanDown", g_cfg.leanDown);
    g_cfg.leanMax = readFloat("LeanMax", g_cfg.leanMax);
    g_cfg.leanSign = readFloat("LeanSign", g_cfg.leanSign);
    g_cfg.cornerLean = readBool("CornerLean", g_cfg.cornerLean);
    g_cfg.armForwardDeg = readFloat("ArmForwardDegrees", g_cfg.armForwardDeg);
    g_cfg.cornerLeanHips = readFloat("CornerLeanHips", g_cfg.cornerLeanHips);
    g_cfg.jumpPose = readBool("JumpPose", g_cfg.jumpPose);
    g_cfg.jumpHideTorso = readBool("JumpHideTorso", g_cfg.jumpHideTorso);
    g_cfg.jumpArms = readBool("JumpArms", g_cfg.jumpArms);
    g_cfg.jumpArmRaise = readFloat("JumpArmRaise", g_cfg.jumpArmRaise);
    g_cfg.jumpArmIn = readFloat("JumpArmIn", g_cfg.jumpArmIn);
    g_cfg.jumpArmElbow = readFloat("JumpArmElbow", g_cfg.jumpArmElbow);
    g_cfg.reappearSpeed = readFloat("ReappearSpeed", g_cfg.reappearSpeed);
    g_cfg.coatHideSprint = readBool("CoatHideSprint", g_cfg.coatHideSprint);
    g_cfg.coatSprintSpeed = readFloat("CoatSprintSpeed", g_cfg.coatSprintSpeed);
    g_cfg.clipGuardDist = readFloat("ClipGuardDistance", g_cfg.clipGuardDist);
    g_cfg.clipPushMax = readFloat("ClipGuardPush", g_cfg.clipPushMax);
    g_cfg.viewGuard = readBool("ViewGuard", g_cfg.viewGuard);
    g_cfg.slideEndPush = readFloat("SlideEndPush", g_cfg.slideEndPush);
    g_cfg.clipPushSpeed = readFloat("ClipGuardPushSpeed", g_cfg.clipPushSpeed);
    g_cfg.shadowWeaponArms = readBool("ShadowWeaponArms", g_cfg.shadowWeaponArms);
    g_cfg.itemShadows = readBool("ItemShadows", g_cfg.itemShadows);
    g_cfg.armIK = readBool("ShadowArmsFollowHands", g_cfg.armIK);
    g_cfg.fpArmShadow = readBool("FirstPersonArmShadow", g_cfg.fpArmShadow);
    g_cfg.playerShadows = readBool("PlayerShadows", g_cfg.playerShadows);
    if (!g_cfg.playerShadows) { g_cfg.shadowBody = false; g_cfg.headShadow = false; g_cfg.fpArmShadow = false; g_cfg.itemShadows = false; }
    g_cfg.armShadowPitchFollow = readFloat("ArmShadowPitchFollow", g_cfg.armShadowPitchFollow);
    g_cfg.armIKEye = { readFloat("ShadowEyeForward", g_cfg.armIKEye.X), 0.0f, readFloat("ShadowEyeUp", g_cfg.armIKEye.Z) };
    g_cfg.armIKElbow = { readFloat("ShadowElbowBack", -g_cfg.armIKElbow.X) * -1.0f, readFloat("ShadowElbowOut", g_cfg.armIKElbow.Y),
                         readFloat("ShadowElbowUp", g_cfg.armIKElbow.Z) };
    g_cfg.shadowArmSteady = readFloat("ShadowArmSteady", g_cfg.shadowArmSteady);
    g_cfg.stairFollow = readFloat("StairFollow", g_cfg.stairFollow);
    g_cfg.stairFollowMax = readFloat("StairFollowMax", g_cfg.stairFollowMax);
    g_cfg.shadowArmRaiseDeg = readFloat("ShadowArmRaise", g_cfg.shadowArmRaiseDeg);
    g_cfg.shadowArmInDeg = readFloat("ShadowArmIn", g_cfg.shadowArmInDeg);
    g_cfg.shadowElbowBendDeg = readFloat("ShadowElbowBend", g_cfg.shadowElbowBendDeg);
    g_cfg.jumpHipDeg = readFloat("JumpHip", g_cfg.jumpHipDeg);
    g_cfg.jumpKneeDeg = readFloat("JumpKnee", g_cfg.jumpKneeDeg);
    g_cfg.hoverPose = readBool("HoverPose", g_cfg.hoverPose);
    g_cfg.hoverHipDeg = readFloat("HoverHip", g_cfg.hoverHipDeg);
    g_cfg.hoverKneeDeg = readFloat("HoverKnee", g_cfg.hoverKneeDeg);
    g_cfg.hoverToeDeg = readFloat("HoverToe", g_cfg.hoverToeDeg);
    g_cfg.hoverSwayDeg = readFloat("HoverSway", g_cfg.hoverSwayDeg);
    g_cfg.hoverSwayRate = readFloat("HoverSwayRate", g_cfg.hoverSwayRate);
    g_cfg.hoverSpread = readFloat("HoverSpread", g_cfg.hoverSpread);
    g_cfg.jumpLandDip = readFloat("JumpLandDip", g_cfg.jumpLandDip);
    g_cfg.jumpBodyBack = readFloat("JumpBodyBack", g_cfg.jumpBodyBack);
    g_cfg.jumpBodyDown = readFloat("JumpBodyDown", g_cfg.jumpBodyDown);
    g_cfg.jumpHideUpper = readBool("JumpHideUpper", g_cfg.jumpHideUpper);
    g_cfg.leanSideSign = readFloat("CornerLeanSign", g_cfg.leanSideSign);
    g_cfg.leanPivotHeight = readFloat("CornerLeanPivot", g_cfg.leanPivotHeight);
    g_cfg.rigidCameraBodyFollow = readBool("RigidCameraBodyFollow", g_cfg.rigidCameraBodyFollow);
    g_cfg.slideRaise = readFloat("SlideRaise", g_cfg.slideRaise);
    g_cfg.slideBack = readFloat("SlideBack", g_cfg.slideBack);
    g_cfg.slideRecline = readFloat("SlideRecline", g_cfg.slideRecline);
    g_cfg.crouchMoveBack = readFloat("CrouchMoveBack", g_cfg.crouchMoveBack);
    g_cfg.footIKFadeLift = readFloat("CrouchLegBlend", g_cfg.footIKFadeLift);
    g_cfg.slideCameraUp = readFloat("SlideCameraUp", g_cfg.slideCameraUp);
    g_cfg.slideRecoverDown = readFloat("SlideRecoverDown", g_cfg.slideRecoverDown);
    g_cfg.slideRecoverSpeed = readFloat("SlideToCrouchSpeed", g_cfg.slideRecoverSpeed);
    g_cfg.slideGetUpTime = readFloat("SlideGetUpTime", g_cfg.slideGetUpTime);
    g_cfg.slideGetUpTuck = readFloat("SlideGetUpTuck", g_cfg.slideGetUpTuck);
    g_cfg.slideFrontTwist = readFloat("SlideFrontTwist", g_cfg.slideFrontTwist);
    g_cfg.slideFrontInward = readFloat("SlideFrontInward", g_cfg.slideFrontInward);
    g_cfg.slideRecoverBack = readFloat("SlideRecoverBack", g_cfg.slideRecoverBack);
    g_cfg.slidePhysics = SettingInt("SlidePhysics", g_cfg.slidePhysics);
    g_cfg.swimPose = readBool("SwimPose", g_cfg.swimPose);
    g_cfg.mantleHide = readBool("MantleHideBody", g_cfg.mantleHide);
    g_cfg.mantleFollow = readBool("MantleFollowCamera", g_cfg.mantleFollow);
    g_cfg.takeoffHide = readBool("MantleTakeoffHide", g_cfg.takeoffHide);
    g_cfg.mantleInfluenceHide = readBool("MantleCameraHide", g_cfg.mantleInfluenceHide);
    g_cfg.mantleCamRelease = readFloat("MantleCameraRelease", g_cfg.mantleCamRelease);
    g_cfg.deepCrouchSink = readFloat("LowCrouchHideTorso", g_cfg.deepCrouchSink);
    g_cfg.lowCrouchHideLegs = readBool("LowCrouchHideLegs", g_cfg.lowCrouchHideLegs);
    g_cfg.takedownHideTorso = readBool("TakedownHideTorso", g_cfg.takedownHideTorso);
    g_cfg.hitCameraScale = readFloat("HitCameraScale", g_cfg.hitCameraScale);
    g_cfg.lookUpHideTorsoDeg = readFloat("LookUpHideTorsoDegrees", g_cfg.lookUpHideTorsoDeg);
    g_cfg.lookUpSteady = readBool("LookUpSteadyCamera", g_cfg.lookUpSteady);
    g_cfg.hitHideTorso = readBool("HitHideTorso", g_cfg.hitHideTorso);
    g_cfg.hitHideMs = readFloat("HitHideMs", g_cfg.hitHideMs);
    if (g_cfg.hitHideMs < 0.0f) g_cfg.hitHideMs = 0.0f;
    g_cfg.lowCrouchLegsBack = readFloat("LowCrouchLegsBackSpeed", g_cfg.lowCrouchLegsBack);
    if (g_cfg.lowCrouchLegsBack < 0.2f) g_cfg.lowCrouchLegsBack = 0.2f;
    g_cfg.mantleHideBack = readFloat("MantleHideBack", g_cfg.mantleHideBack);
    g_cfg.mantleHideUp = readFloat("MantleHideUp", g_cfg.mantleHideUp);
    g_cfg.swimPhysics = SettingInt("SwimPhysics", g_cfg.swimPhysics);
    g_cfg.swimKickRate = readFloat("SwimKickRate", g_cfg.swimKickRate);
    g_cfg.swimKickHip = readFloat("SwimKickHip", g_cfg.swimKickHip);
    g_cfg.swimKickKnee = readFloat("SwimKickKnee", g_cfg.swimKickKnee);
    g_cfg.crouchHidePitch = readFloat("CrouchHideUpperPitch", g_cfg.crouchHidePitch);
    g_cfg.runBlendMax = readFloat("RunBlendMax", g_cfg.runBlendMax);
    g_cfg.turnThreshold = readFloat("TurnStepThreshold", g_cfg.turnThreshold);
    g_cfg.climbPhysics = SettingInt("ClimbPhysics", g_cfg.climbPhysics);
    g_cfg.climbHipDeg = readFloat("ClimbHip", g_cfg.climbHipDeg);
    g_cfg.climbKneeDeg = readFloat("ClimbKnee", g_cfg.climbKneeDeg);
    g_cfg.climbStride = readFloat("ClimbStride", g_cfg.climbStride);
    g_cfg.climbGripHip = readFloat("ClimbGripHip", g_cfg.climbGripHip);
    g_cfg.climbDangle = readBool("ClimbDangle", g_cfg.climbDangle);
    g_cfg.climbCross = readFloat("ClimbCross", g_cfg.climbCross);
    g_cfg.climbSideShift = readFloat("ClimbBodyLeft", g_cfg.climbSideShift);
    g_cfg.climbForwardShift = readFloat("ClimbBodyForward", g_cfg.climbForwardShift);
    g_cfg.dangleSwing = readFloat("DangleSwing", g_cfg.dangleSwing);
    g_cfg.dangleStiffness = readFloat("DangleStiffness", g_cfg.dangleStiffness);
    g_cfg.dangleDamping = readFloat("DangleDamping", g_cfg.dangleDamping);
    g_cfg.climbGripKnee = readFloat("ClimbGripKnee", g_cfg.climbGripKnee);
    g_cfg.turnAnimDegPerSec = readFloat("TurnStepSpeed", g_cfg.turnAnimDegPerSec);
    g_cfg.slideHipDeg = readFloat("SlideFrontHip", g_cfg.slideHipDeg);
    g_cfg.slideKneeDeg = readFloat("SlideFrontKnee", g_cfg.slideKneeDeg);
    g_cfg.slideBackHipDeg = readFloat("SlideBackHip", g_cfg.slideBackHipDeg);
    g_cfg.slideBackKneeDeg = readFloat("SlideBackKnee", g_cfg.slideBackKneeDeg);
    g_cfg.slideHeight = readFloat("SlideHeight", g_cfg.slideHeight);
    g_cfg.crouchSmoothSpeed = readFloat("CrouchSmoothSpeed", g_cfg.crouchSmoothSpeed);
    g_cfg.coatMode = SettingInt("CoatMode", g_cfg.coatMode);
    g_cfg.coatHangStrength = readFloat("CoatHangStrength", g_cfg.coatHangStrength);
    g_cfg.coatHangPitch = readFloat("CoatHangPitch", g_cfg.coatHangPitch);
    g_cfg.coatHangYaw = readFloat("CoatHangYaw", g_cfg.coatHangYaw);
    g_cfg.coatHangRoll = readFloat("CoatHangRoll", g_cfg.coatHangRoll);
    g_cfg.coatRearFlare = readFloat("CoatRearFlare", g_cfg.coatRearFlare);
    g_cfg.kneeForward = readFloat("KneeTargetForward", g_cfg.kneeForward);
    g_cfg.kneeSide = readFloat("KneeTargetSide", g_cfg.kneeSide);
    g_cfg.kneeHeight = readFloat("KneeTargetHeight", g_cfg.kneeHeight);
    g_cfg.crouchOffsetZ = readFloat("CrouchOffsetZ", g_cfg.crouchOffsetZ);
    {
        char bone[64];
        SettingString("CrouchHideBone", "spine_3_jnt", bone, sizeof(bone));
        strncpy(g_cfg.crouchHideBone, bone, sizeof(g_cfg.crouchHideBone) - 1);
    }
    Log("animation: RunBlend %.0f..%.0f WalkAnimRate=%.2f RunAnimRate=%.2f", g_cfg.runBlendStart, g_cfg.runBlendEnd, g_cfg.walkAnimRate, g_cfg.runAnimRate);
    Log("settings: Enabled=%d HideBodyArms=%d AttachMode=%d Offset=(%.1f %.1f %.1f)", g_cfg.enabled,
        g_cfg.hideArms, g_cfg.attachMode, g_cfg.offsetX, g_cfg.offsetY, g_cfg.offsetZ);
}

// ---------------------------------------------------------------------------------------------
// Body state
// ---------------------------------------------------------------------------------------------
struct BodySource { const wchar_t* package; const char* mesh; };
static const BodySource kSources[] = {
    { L"L_DLC07_Endgame_Script", "DLC07_skm_Corvo_Body" },    // Brigmore Witches, 10.8 MB
    { L"DLC06_Tower_Script",     "DLC06_Skm_Corvo_Body" },    // Knife of Dunwall, 22.7 MB
    { L"L_DLC07_BaseIntro_Script", "DLC07_skm_Corvo_Body" },
};
// Hide from the shoulders down: hiding only the upper arms left the sleeve tops visible next to the 1P arms.
static const char* kArmBones[] = { "shoulder_L_jnt", "shoulder_R_jnt" };

enum class LoadState { Idle, Loading, Loaded, Failed };
static volatile LoadState g_loadState = LoadState::Idle;
static int g_sourceIndex = 0;
static DWORD g_loadStartTick = 0;

static USkeletalMesh* g_bodyMesh = nullptr;
static USkeletalMeshComponent* g_body = nullptr;
static ADishonoredPlayerPawn* g_bodyPawn = nullptr;
static bool g_bodyArmsHidden = false;
static bool g_bodyAnimated = false;  // the body runs its own locomotion AnimTree (not the 1P pose copy)
static bool g_isCrouched = false;    // set by UpdatePosture
static bool g_isSliding = false;     // set by UpdatePosture (crouch flag + cylinder below SlideHeight)
static bool g_isClimbing = false;    // set by UpdateClimb (Physics == ClimbPhysics)
static float g_slideAmount = 0.0f;   // eased 0..1 slide pose weight (UpdateSlide)
static bool g_hideUpperNow = false;  // SyncTransform: the upper body would clip right now
static FVector g_camCorrection = { 0, 0, 0 };   // rigid camera: final view minus the game's view (SyncTransform uses it)
static FVector g_camTarget = { 0, 0, 0 };       // rigid camera: world offset from the vanilla view to the fixed view
static FVector g_bodyFollowTarget = { 0, 0, 0 }; // ...the part the visible body follows (capped at the level-look amount)
static UObject* g_handItem[2] = { nullptr, nullptr };   // per hand (0 left, 1 right): the held inventory item (UDishonoredInventoryItem)
static FVector g_shadowBodyT = { 0, 0, 0 };     // SyncTransform: the shadow body's Translation (actor space)
static FVector g_armsShadowOffset = { 0, 0, 0 };   // world offset from the 1P arms to the arm shadow copy (shoulders onto the body)
static USkeletalMeshComponent* g_armsShadow = nullptr;  // shadow-only copy of the first-person arms (pose copied from pawn->Mesh)
static USkeletalMeshComponent* g_itemShadow[2] = { nullptr, nullptr };   // shadow-only copies of the held items' models
static bool g_bodyVisible = false;             // UpdateVisibility: first-person view (not a cutscene)
static int g_handKind[2] = { 0, 0 };          // per hand (0 left, 1 right): 0 empty, 1 sword, 2 pistol/crossbow, 3 power/other
static float g_weaponRaise[2] = { 0, 0 };     // eased 0..1 per hand (0 left, 1 right): shadow arm raised for a weapon
static float g_crouchSink = 0.0f;              // set by UpdatePosture: how far crouching sank the body (uu)
static FVector g_viewLoc = { 0, 0, 0 };        // final view location from the GetPlayerViewPoint hook
static DWORD g_viewLocTick = 0;
static FRotator g_viewRot = { 0, 0, 0 };       // ...its rotation
static float g_viewFov = 90.0f;                // ...horizontal field of view (degrees)
static float g_viewGuardBack = 0.0f;           // view guard: uu the visible body is eased back so no upper-body part is in view
static float g_landHold = 0.0f;                // 1 -> 0 over 0.4 s after landing (UpdateJump)
static float g_slideLegW = 0.0f;              // UpdateSlide: weight of the slide leg pose (foot IK takes the rest)
static float g_upperAmount = 0.0f;            // eased 0..1: visible upper body (spine_3 up) hidden
static float g_torsoAmount = 0.0f;            // eased 0..1: visible torso from the waist up hidden
static float g_legHideAmount = 0.0f;          // eased 0..1: visible legs folded into the hips (crouching under tables)
static float g_hoverAmount = 0.0f;            // eased 0..1: floating leg pose (held in the air by Daud's Transversal)
static bool g_hitActive = false;              // UpdateHitState: just hit / stunned (hit reaction under way)
static FVector g_clipDir = { -0.8f, 0.0f, -0.6f };   // clip guard push direction (player space, unit)
static float g_clipPush = 0.0f;                // clip guard: uu the visible body is pushed back/down from the camera
static float g_climbAmount = 0.0f;            // UpdateClimb: eased 0..1 on a chain/ladder
static const char* g_lastHideWhy = nullptr;    // UpdateVisibility: reason of the current hide (log filter)
static bool g_mantleWant = false;              // UpdateVisibility: camera pulled back by a mantle
static float g_mantleAmount = 0.0f;            // eased 0..1: visible body shrunk away for the mantle
static bool g_torsoHidden = false;             // UpdateLean: visible torso hidden from the waist up this frame
static float g_swimAmount = 0.0f;             // 0..1 eased swimming pose weight (UpdateSwim)
static float g_jumpAmount = 0.0f;             // 0..1 eased airborne pose weight (UpdateJump)
static float g_jumpBodyW = 0.0f;              // 0..1 weight of the jump's body back/down offset (held through the landing)
static float g_landDip = 0.0f;                 // uu (<= 0) both bodies drop on landing; foot IK bends the knees
static float g_viewPitchDeg = 0.0f;            // view pitch in degrees (+ up), set by UpdateLean
static float g_cornerLeanRight = 0.0f;         // game's camera sideways offset from the pitch curve (corner lean), uu
static float g_stairDz = 0.0f;                 // game's camera height minus the pitch curve's (stairs lag), uu
static float g_cornerLeanFwd = 0.0f;           // ...and forward
static FVector g_armsShiftLocal = { 0, 0, 0 };  // rigid camera: shift applied to the 1P arms' Translation (actor space)
static FVector g_armsShiftWorld = { 0, 0, 0 };  // ...the same in world space
static int g_camFollowsArms = 1;                // 1 the game's camera rides the 1P arms (camera_jnt; detected 15/0 in two sessions), 0 not, -1 detect
static void LogPitchCurve();
static void ApplyArmsShift(ADishonoredPlayerPawn* pawn);
static float g_cameraDip = 0.0f;     // SyncTransform: how far (uu, <= 0) the visible body is lowered with the camera
static bool g_rightHandBusy = false, g_leftHandBusy = false;   // set by UpdateHands
static float g_footLift = 0.0f;     // current (eased) foot lift; the coat follows this, not the crouch flag
static float g_smoothedSink = 0.0f; // body's eased crouch drop (SyncTransform); feet learn the standing gap only when settled
static USkeletalMesh* g_headMesh = nullptr;        // DLC07_skm_Corvo_Mask (masked head), same package
static USkeletalMeshComponent* g_head = nullptr;   // hidden head that only casts a shadow
// Body Swap (Daud): same package as Corvo's body (L_DLC07_Endgame_Script); Daud's 1P arms come from his player package.
static USkeletalMesh* g_daudBodyMesh = nullptr;   // skm_Daud_Body (same skeleton as Corvo's; coat panels named differently)
static USkeletalMesh* g_daudHeadMesh = nullptr;   // skm_Daud_Head
static USkeletalMesh* g_daudArmsMesh = nullptr;   // DLC07_Skm_Player_Daud (same 79-bone skeleton as Skm_Player)
// Body Swap (Overseer, build 200): Daud's High Overseer disguise (Brigmore Witches, Coldridge Prison). One map package
// has all four, L_DLC07_BaseIntro_Script: Skm_Overseer_Body (same rig as Corvo's/Daud's; coat panels named like
// Daud's), Skm_Overseer_Head2, DLC07_Skm_Player_DaudOverseer (1P arms, the same 79 bones as Skm_Player) and
// OverseerSword (one rigid bone, like Daud's blade). Loaded on demand like Daud's player package.
static USkeletalMesh* g_ovsBodyMesh = nullptr;
static USkeletalMesh* g_ovsHeadMesh = nullptr;
static USkeletalMesh* g_ovsArmsMesh = nullptr;
static USkeletalMesh* g_ovsSwordMesh = nullptr;
static int g_builtChar = -1;                      // character the current body was built with
static USkeletalMeshComponent* g_shadowBody = nullptr;   // hidden full body (coat + arms) that only casts the shadow

static thread_local int g_inHook = 0;

static ADishonoredPlayerPawn* GetPlayerPawn() {
    auto* pawn = *reinterpret_cast<ADishonoredPlayerPawn**>(addr::PlayerPawnInstance);
    // During map changes s_pInstance can still point at a pawn that is being destroyed.
    if (pawn && (pawn->bDeleteMe || pawn->bPendingDelete)) return nullptr;
    return pawn;
}

static void* GetWorld() { return *reinterpret_cast<void**>(addr::GWorld); }

static std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s += static_cast<char>(*w);
    return s;
}

static void GrabAssetsNow();   // defined after the asset helpers

static void __cdecl OnPackageLoaded(UObject* package, void* /*user*/) {
    Log("package load finished: %s", package ? "ok" : "FAILED (null package)");
    g_loadState = package ? LoadState::Loaded : LoadState::Failed;
    // Root what we need right here, inside the load: the engine's post-load GC marks everything nothing
    // references as unreachable and purges it a few seconds later (test 16: AnimSequences freed ~4 s in).
    if (package) GrabAssetsNow();
}

static void StartLoad() {
    if (!LoadPackageAsync || g_sourceIndex >= static_cast<int>(sizeof(kSources) / sizeof(kSources[0]))) {
        g_loadState = LoadState::Failed;
        Log("no more body sources to try");
        return;
    }
    const BodySource& src = kSources[g_sourceIndex];
    Log("async loading package %s for mesh %s", Narrow(src.package).c_str(), src.mesh);
    g_loadState = LoadState::Loading;
    g_loadStartTick = GetTickCount();
    FString name(src.package);
    LoadPackageAsync(name, &OnPackageLoaded, nullptr, nullptr);
}

static USkeletalMesh* FindLoadedMesh(const char* meshName) {
    UClass* meshClass = USkeletalMesh::StaticClass();
    auto* objects = UObject::GObjObjects();
    USkeletalMesh* found = nullptr;
    int matches = 0;
    for (int i = 0; i < objects->size(); ++i) {
        UObject* obj = objects->at(i);
        if (!obj || obj->Class != meshClass) continue;
        if (obj->GetName() == meshName) { found = static_cast<USkeletalMesh*>(obj); ++matches; }
    }
    if (matches > 1) Log("warning: %d meshes named %s, using the newest", matches, meshName);
    return found;
}

// ---------------------------------------------------------------------------------------------
// Body animation. The 1P animations only move the camera and arms (legs stay in ref pose; see MODLOG),
// so the body gets its own AnimTree built from stock UE3 nodes, playing City Watch locomotion:
//   AnimTree -> BlendBySpeed [ Idle | Directional(Walk N/S/W/E) | Directional(Run N/S/W/E) ]
// The nodes read the owner's (player pawn's) velocity and rotation natively, so nothing runs per frame.
// ---------------------------------------------------------------------------------------------
static const char* kAnimSetName = "Npc_EliteGuard_empty_as";   // ships in the same DLC package as the body
static const wchar_t* kAnimPackage = L"L_Distillery_Ext_Script";   // smallest script package with it (6.7 MB)

static UAnimSet* g_animSet = nullptr;
static UAnimSet* g_crouchSet = nullptr;   // DLC06_NPC_Assassins_Distract (Assassin_Crouch_loop), same package
static const char* kCrouchSetName = "DLC06_NPC_Assassins_Distract";
static volatile LoadState g_animLoadState = LoadState::Idle;
static DWORD g_animLoadStartTick = 0;

static void __cdecl OnAnimPackageLoaded(UObject* package, void*) {
    Log("anim package load finished: %s", package ? "ok" : "FAILED");
    g_animLoadState = package ? LoadState::Loaded : LoadState::Failed;
}

static UObject* FindObjectByName(const char* className, const char* objectName) {
    auto* objects = UObject::GObjObjects();
    UObject* found = nullptr;
    for (int i = 0; i < objects->size(); ++i) {
        UObject* obj = objects->at(i);
        if (!obj || !obj->Class) continue;
        if (obj->GetName() == objectName && obj->Class->GetName() == className) found = obj;
    }
    return found;
}

// Engine heap (GMalloc), so arrays we fill can later be freed or grown by the engine itself.
// appMalloc(size, alignment) @0x404280, exactly as the engine's own TArrays allocate (alignment 8).
// Test 18: GMalloc->Malloc(size, 16) blocks crashed the allocator when the engine freed them on a level change.
static void* EngineMalloc(size_t size) {
    using appMalloc_t = void*(__cdecl*)(DWORD, DWORD);
    void* p = reinterpret_cast<appMalloc_t>(0x00404280)(static_cast<DWORD>(size), 8);
    if (p) memset(p, 0, size);
    return p;
}

struct RawArray { void* Data; int32_t Num; int32_t Max; };

// The engine's TArray allocator, sub_4246E0(data, old bytes, new bytes, align) cdecl. Old bytes is always
// Max * stride. Blocks of 1-31 bytes come from a small-block pool (0x141B2A0), bigger ones from GMalloc, and a free
// picks the pool from the old size alone (sub_423D70). So a buffer the engine may free or grow must come from here:
// a 20-byte GMalloc block freed as "20 bytes" crashed the level-change purge (build 182, wristbow anim states), and
// growing a small engine array with GMalloc realloc hands it a small-pool pointer.
static void* ArrayRealloc(void* data, int oldBytes, int newBytes) {
    return reinterpret_cast<void*(__cdecl*)(void*, int, int, int)>(0x004246E0)(data, oldBytes, newBytes, 8);
}

static void* ArrayAlloc(int bytes) {
    if (bytes <= 0) return nullptr;
    void* p = ArrayRealloc(nullptr, 0, bytes);
    if (p) memset(p, 0, bytes);
    return p;
}

static bool GrowArray(RawArray* arr, int stride, int newMax) {
    if (arr->Max >= newMax) return true;
    void* grown = ArrayRealloc(arr->Data, arr->Max * stride, newMax * stride);
    if (!grown) return false;
    arr->Data = grown;
    arr->Max = newMax;
    return true;
}

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Every array buffer we hand to the engine, and the field it went into.
struct OurArray { RawArray* field; void* data; };
static OurArray g_ourArrays[256];
static int g_ourArrayCount = 0;

static bool IsOurBuffer(void* p) {
    for (int i = 0; i < g_ourArrayCount; ++i) if (g_ourArrays[i].data == p) return true;
    return false;
}

static bool FillArray(void* arrayField, const void* items, int count, size_t stride) {
    auto* arr = reinterpret_cast<RawArray*>(arrayField);
    void* data = EngineMalloc(stride * count);
    if (!data) return false;
    if (items) memcpy(data, items, stride * count);
    arr->Data = data;
    arr->Num = arr->Max = count;
    if (g_ourArrayCount < 256) g_ourArrays[g_ourArrayCount++] = { arr, data };
    return true;
}

// Before a level unload: empty every array that still points at one of our buffers, in our template nodes,
// the body component, and (in case CopyAnimTree copied the pointers) the instanced tree. The engine then
// never frees our memory; the buffers leak (a few hundred bytes per level), which is harmless.
static void DetachNodeArrays(UObject* node, int depth) {
    if (!node || depth > 8) return;
    uint8_t* base = reinterpret_cast<uint8_t*>(node);
    static const int kArrayOffsets[] = { 0xCC, 0xE0, 0x110, 0x114 };   // Children; BlendList TargetWeight; AnimTree SkelControlLists; BlendBySpeed Constraints
    RawArray* children = reinterpret_cast<RawArray*>(base + 0xCC);
    // Recurse first, while the Children array is still intact.
    if (children->Data && children->Num > 0 && children->Num < 16) {
        for (int i = 0; i < children->Num; ++i)
            DetachNodeArrays(*reinterpret_cast<UObject**>(static_cast<uint8_t*>(children->Data) + i * 0x50 + 8), depth + 1);
    }
    for (int off : kArrayOffsets) {
        auto* arr = reinterpret_cast<RawArray*>(base + off);
        if (arr->Data && IsOurBuffer(arr->Data)) {
            Log("  instanced node %p shares our buffer at +0x%X: detaching", node, off);
            arr->Data = nullptr; arr->Num = arr->Max = 0;
        }
    }
}

static void DetachOurArrays(USkeletalMeshComponent* comp) {
    if (comp && comp->Animations) DetachNodeArrays(comp->Animations, 0);
    int detached = 0;
    for (int i = 0; i < g_ourArrayCount; ++i) {
        OurArray& a = g_ourArrays[i];
        if (a.field->Data == a.data) { a.field->Data = nullptr; a.field->Num = a.field->Max = 0; ++detached; }
    }
    Log("detached %d of our %d array buffers before the purge", detached, g_ourArrayCount);
    g_ourArrayCount = 0;
}

constexpr size_t kAnimBlendChildStride = 0x50;  // FAnimBlendChild 0x48 + alignment padding (SDK)

static bool SetChildren(UAnimNodeBlendBase* parent, UAnimNode* const* kids, int count) {
    auto* arr = reinterpret_cast<RawArray*>(&parent->Children);
    if (!FillArray(arr, nullptr, count, kAnimBlendChildStride)) return false;
    for (int i = 0; i < count; ++i) {
        uint8_t* child = static_cast<uint8_t*>(arr->Data) + i * kAnimBlendChildStride;
        *reinterpret_cast<UAnimNode**>(child + 0x08) = kids[i];                 // Anim
        *reinterpret_cast<float*>(child + 0x0C) = (i == 0) ? 1.0f : 0.0f;       // Weight
    }
    return true;
}

template <typename T>
static T* NewObject(const char* classFullName, UObject* outer) {
    UClass* cls = UObject::FindClass(classFullName);
    if (!cls) { Log("class not found: %s", classFullName); return nullptr; }
    return static_cast<T*>(StaticConstructObject(cls, outer, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
}

static UAnimNodeSequence* NewSequence(UObject* outer, const char* animName, float rate = 1.0f) {
    auto* seq = NewObject<UAnimNodeSequence>("Class Engine.AnimNodeSequence", outer);
    if (!seq) return nullptr;
    seq->AnimSeqName = FName(animName);
    if (seq->AnimSeqName.FNameEntryId < 0) Log("anim name %s not in name table", animName);
    seq->Rate = rate;
    seq->bPlaying = 1;
    seq->bLooping = 1;
    // Keep the body in place: discard any root translation the NPC clips carry.
    seq->RootBoneOption[0] = seq->RootBoneOption[1] = seq->RootBoneOption[2] = ERootBoneAxis::RBA_Discard;
    return seq;
}

static UAnimNodeBlendDirectional* NewDirectional(UObject* outer, float rate, const char* n, const char* s, const char* w, const char* e) {
    auto* dir = NewObject<UAnimNodeBlendDirectional>("Class Engine.AnimNodeBlendDirectional", outer);
    if (!dir) return nullptr;
    dir->DirDegreesPerSecond = 720.0f;
    // UE3 child order: forward, backward, left, right.
    UAnimNode* kids[4] = { NewSequence(outer, n, rate), NewSequence(outer, s, rate), NewSequence(outer, w, rate),
                           NewSequence(outer, e, rate) };
    for (UAnimNode* k : kids) if (!k) return nullptr;
    return SetChildren(dir, kids, 4) ? dir : nullptr;
}

// Procedural crouch-walk (no character in Dishonored has one): one SkelControlSingleBone per hip and knee,
// adding a rotation on top of the crouch pose. UpdateCrouchLegs drives them from speed every frame.
// ControlName = the bone name (an FName that already exists, so FindSkelControl can look it up).
static const char* kLegBones[4] = { "upper_leg_L_jnt", "upper_leg_R_jnt", "lower_leg_L_jnt", "lower_leg_R_jnt" };

static void AddFootIK(UAnimTree* tree, void* heads, int& count);
static void AddCoatControls(UAnimTree* tree, void* heads, int& count);
static void AddLeanControls(UAnimTree* tree, void* heads, int& count);
static void AddArmIK(UAnimTree* tree, void* heads, int& count);

static void AddLegControls(UAnimTree* tree) {
    struct ListHead { int32_t nameIndex, nameNumber; USkelControlBase* head; int32_t drawY; };
    static_assert(sizeof(ListHead) == 16, "FSkelControlListHead is 16 bytes");
    ListHead heads[32] = {};
    int count = 0;
    for (const char* bone : kLegBones) {
        FName name(bone);
        if (name.FNameEntryId < 0) { Log("leg bone %s not in name table", bone); continue; }
        auto* ctl = NewObject<USkelControlSingleBone>("Class Engine.SkelControlSingleBone", tree);
        if (!ctl) continue;
        ctl->ControlName = name;
        ctl->ControlStrength = 0.0f;
        ctl->StrengthTarget = 0.0f;
        ctl->bApplyRotation = 1;
        ctl->bAddRotation = 1;
        ctl->BoneRotationSpace = static_cast<EBoneControlSpace>(g_cfg.crouchLegSpace);
        heads[count++] = { name.FNameEntryId, 0, ctl, 0 };
    }
    int legCount = count;
    if (g_cfg.footIK) AddFootIK(tree, heads, count);
    AddCoatControls(tree, heads, count);
    AddLeanControls(tree, heads, count);
    AddArmIK(tree, heads, count);
    if (count) FillArray(&tree->SkelControlLists, heads, count, sizeof(ListHead));
    Log("skel controls: %d leg swing, %d foot IK", legCount, count - legCount);
}

// Foot IK: SkelControlFootPlacement on each foot. Corvo's leg bones run along local +X (child offsets 45.7 / 42.5
// on X) and hinge around local Y (leg rotations are pure Y), so BoneAxis = X, JointAxis = Y. Crouching sinks the
// body ~22 uu (the game lowers the pawn but not the 1P mesh); the IK lifts each foot back onto the floor, which
// bends the knees: the normal walk/idle cycles become a crouch-walk at any stick speed.
static void AddFootIK(UAnimTree* tree, void* heads, int& count) {
    struct ListHead { int32_t nameIndex, nameNumber; USkelControlBase* head; int32_t drawY; };
    auto* list = static_cast<ListHead*>(heads);
    static const char* feet[2] = { "foot_L_jnt", "foot_R_jnt" };
    for (int i = 0; i < 2; ++i) {
        FName name(feet[i]);
        if (name.FNameEntryId < 0) { Log("foot bone %s not in name table", feet[i]); continue; }
        // Plain two-bone IK, no floor trace (SkelControlFootPlacement's trace never hit anything for this body).
        // The effector is an offset in the foot's own (animated) bone space, set every frame by UpdateFootIK:
        // lift = how far crouching sank the body, along the shin (foot bone -X), so the walk cycle's steps are kept.
        auto* ik = NewObject<USkelControlLimb>("Class Engine.SkelControlLimb", tree);
        if (!ik) continue;
        ik->ControlName = name;
        ik->ControlStrength = 0.0f;
        ik->StrengthTarget = 0.0f;
        ik->BoneAxis = EAxis::AXIS_X;
        ik->JointAxis = EAxis::AXIS_Y;
        ik->EffectorLocationSpace = EBoneControlSpace::BCS_BoneSpace;
        ik->EffectorLocation = FVector{ 0, 0, 0 };
        // Knee pole: in front of the knee, in the player's space (X forward, Y right, Z up from the pawn's centre).
        ik->JointTargetLocationSpace = EBoneControlSpace::BCS_ActorSpace;
        ik->JointTargetLocation = FVector{ g_cfg.kneeForward, i == 0 ? -g_cfg.kneeSide : g_cfg.kneeSide, g_cfg.kneeHeight };
        ik->bMaintainEffectorRelRot = 0;   // keep the foot's animated (flat) orientation, not its angle to the shin
        // Hover pose (build 190): toes pointed down while Daud's Transversal holds the player in the air. One list head
        // per bone, so it is chained after the IK on the same foot; found as the foot's name with instance number 1.
        if (auto* toe = NewObject<USkelControlSingleBone>("Class Engine.SkelControlSingleBone", tree)) {
            FName toeName = name;
            toeName.InstanceNumber = 1;
            toe->ControlName = toeName;
            toe->ControlStrength = toe->StrengthTarget = 0.0f;
            toe->bApplyRotation = 1;
            toe->bAddRotation = 1;
            toe->BoneRotationSpace = static_cast<EBoneControlSpace>(g_cfg.crouchLegSpace);
            ik->NextControl = toe;
        }
        list[count++] = { name.FNameEntryId, 0, ik, 0 };
    }
}

// Arm IK (shadow body): while a hand holds gear, that hand follows the first-person hand live - same place relative
// to the head as the 1P hand is to the camera - so sword swings, blocks, aiming and power casts show in the shadow
// (user). Two-bone IK on hand_X (upper arm -> lower arm -> hand; Corvo's arm bones run along +X and the elbow
// hinges on Y, like the legs), plus a world-space rotation on Offset_hand_X (the item shadow hangs off that bone).
static const char* kHandBones[2] = { "hand_L_jnt", "hand_R_jnt" };
static const char* kHandOffsetBones[2] = { "Offset_hand_L_jnt", "Offset_hand_R_jnt" };

static void AddArmIK(UAnimTree* tree, void* heads, int& count) {
    struct ListHead { int32_t nameIndex, nameNumber; USkelControlBase* head; int32_t drawY; };
    auto* list = static_cast<ListHead*>(heads);
    for (int i = 0; i < 2; ++i) {
        FName hand(kHandBones[i]), offset(kHandOffsetBones[i]);
        if (hand.FNameEntryId < 0) continue;
        auto* ik = NewObject<USkelControlLimb>("Class Engine.SkelControlLimb", tree);
        if (!ik) continue;
        ik->ControlName = hand;
        ik->ControlStrength = ik->StrengthTarget = 0.0f;
        ik->BoneAxis = EAxis::AXIS_X;
        ik->JointAxis = EAxis::AXIS_Y;
        ik->EffectorLocationSpace = EBoneControlSpace::BCS_WorldSpace;
        ik->EffectorLocation = FVector{ 0, 0, 0 };
        ik->JointTargetLocationSpace = EBoneControlSpace::BCS_ActorSpace;   // elbow pole: back, out and down
        ik->JointTargetLocation = FVector{ -30.0f, i == 0 ? -60.0f : 60.0f, 0.0f };
        ik->bMaintainEffectorRelRot = 0;
        list[count++] = { hand.FNameEntryId, 0, ik, 0 };
        if (offset.FNameEntryId < 0) continue;
        auto* rot = NewObject<USkelControlSingleBone>("Class Engine.SkelControlSingleBone", tree);
        if (!rot) continue;
        rot->ControlName = offset;
        rot->ControlStrength = rot->StrengthTarget = 0.0f;
        rot->bApplyRotation = 1;
        rot->bAddRotation = 0;
        rot->BoneRotationSpace = EBoneControlSpace::BCS_WorldSpace;
        list[count++] = { offset.FNameEntryId, 0, rot, 0 };
    }
}

// Coat: the four panel chains (front Le/Re, rear L_Rear/R_Rear) are parented straight to the thighs, so they move
// rigidly with the legs and stick out over the knees when crouched. Mode 2 puts a SkelControlSingleBone on each
// chain's first bone that sets an absolute player-space rotation (panel hanging down), blended by CoatHangStrength.
// Daud's coat has the same four chains, named *_02 (front Le/Re, rear L/R); SelectCoatBones swaps entries 0-3.
static const char* kCoatBones[12] = { "Back_pan_Le_01_jnt", "Back_pan_Re_01_jnt", "Back_pan_L_Rear_01_jnt", "Back_pan_R_Rear_01_jnt",
                                     "shoulder_L_jnt", "shoulder_R_jnt",     // 4-5: the body's arms (shrunk per busy hand)
                                     "lower_arm_L_jnt", "lower_arm_R_jnt",    // 6-7: elbows (extra bend while crouched)
                                     "spine_3_jnt",                            // 8: whole upper body (shrunk only while it would clip)
                                     "Torso_jnt", "neck_jnt",                  // 9-10: chest + neck (always shrunk on the visible body)
                                     "Root_jnt" };                             // 11: pelvis (thighs + spine hang off it): legs folded away under tables

// The body character the menu asks for and whose mesh is loaded: 0 Corvo, 1 Daud, 2 Overseer (Corvo until loaded).
static int WantedBodyChar() {
    if (g_bodyChar == 1 && g_daudBodyMesh) return 1;
    if (g_bodyChar == 2 && g_ovsBodyMesh) return 2;
    return 0;
}

static const char* CharName(int c) { return c == 2 ? "Overseer" : c == 1 ? "Daud" : "Corvo"; }

// Decides which character the next body is built with and points the coat entries at that mesh's panel bones.
static void SelectCoatBones() {
    g_builtChar = WantedBodyChar();
    static const char* const kPanels[3][4] = {
        { "Back_pan_Le_01_jnt", "Back_pan_Re_01_jnt", "Back_pan_L_Rear_01_jnt", "Back_pan_R_Rear_01_jnt" },
        { "Back_pan_Le_02_jnt", "Back_pan_Re_02_jnt", "Back_pan_L_02_jnt",      "Back_pan_R_02_jnt" },
        { "Back_pan_Le_02_jnt", "Back_pan_Re_02_jnt", "Back_pan_L_02_jnt",      "Back_pan_R_02_jnt" } };   // Overseer: Daud's names (psk)
    for (int i = 0; i < 4; ++i) kCoatBones[i] = kPanels[g_builtChar][i];
}

static USkeletalMesh* ActiveBodyMesh() {
    if (g_builtChar == 1 && g_daudBodyMesh) return g_daudBodyMesh;
    if (g_builtChar == 2 && g_ovsBodyMesh) return g_ovsBodyMesh;
    return g_bodyMesh;
}

static void AddCoatControls(UAnimTree* tree, void* heads, int& count) {
    struct ListHead { int32_t nameIndex, nameNumber; USkelControlBase* head; int32_t drawY; };
    auto* list = static_cast<ListHead*>(heads);
    for (const char* bone : kCoatBones) {
        FName name(bone);
        if (name.FNameEntryId < 0) { Log("coat bone %s not in name table", bone); continue; }
        auto* ctl = NewObject<USkelControlSingleBone>("Class Engine.SkelControlSingleBone", tree);
        if (!ctl) continue;
        ctl->ControlName = name;
        ctl->ControlStrength = 0.0f;
        ctl->StrengthTarget = 0.0f;
        ctl->bApplyRotation = 1;
        ctl->bAddRotation = 0;
        ctl->BoneRotationSpace = EBoneControlSpace::BCS_ActorSpace;
        list[count++] = { name.FNameEntryId, 0, ctl, 0 };
    }
}

// Upper-body lean (user: "a flexible Corvo upper body that stays with the camera"): the spine bends with the
// view pitch like Dishonored 2 - back when looking up (the collar moves away from the camera instead of the
// camera ending up at the back of the neck), slightly forward when looking down. Actor-space additive pitch on
// spine_1 and spine_2 (half each; the second adds on top of the first), on both the visible and shadow body.
static const char* kSpineBones[2] = { "spine_1_jnt", "spine_2_jnt" };

static void AddLeanControls(UAnimTree* tree, void* heads, int& count) {
    struct ListHead { int32_t nameIndex, nameNumber; USkelControlBase* head; int32_t drawY; };
    auto* list = static_cast<ListHead*>(heads);
    for (const char* bone : kSpineBones) {
        FName name(bone);
        if (name.FNameEntryId < 0) { Log("spine bone %s not in name table", bone); continue; }
        auto* ctl = NewObject<USkelControlSingleBone>("Class Engine.SkelControlSingleBone", tree);
        if (!ctl) continue;
        ctl->ControlName = name;
        ctl->ControlStrength = ctl->StrengthTarget = 0.0f;
        ctl->bApplyRotation = 1;
        ctl->bAddRotation = 1;
        ctl->BoneRotationSpace = EBoneControlSpace::BCS_ActorSpace;
        list[count++] = { name.FNameEntryId, 0, ctl, 0 };
    }
}

// Choke / assassination takedowns (build 193): the takedown pulls the camera forward over the NPC's shoulder while
// the body stays, and the shrunk collar/neck's stretched polygons filled the bottom of the view (video 19-41-57).
// The choke also runs on physics 14 - the slide's - so from a crouch it would have read as a slide.
static bool InTakedown(ADishonoredPlayerPawn* pawn) {
    static UClass* chokeCls = nullptr;
    static UClass* assassinateCls = nullptr;
    if (!chokeCls) chokeCls = UObject::FindClass("Class DishonoredGame.StatePlayerMasterChoke");
    if (!assassinateCls) assassinateCls = UObject::FindClass("Class DishonoredGame.StatePlayerMasterAssassinate");
    UObject* st = (pawn && pawn->m_pPlayerMasterFSM) ? pawn->m_pPlayerMasterFSM->m_pCurrentState : nullptr;
    return st && ((chokeCls && st->IsA(chokeCls)) || (assassinateCls && st->IsA(assassinateCls)));
}

// Hit camera (build 194, user: "we could just reduce the camera movement when hit"). The player's hit reaction
// (sub_B58190) kicks the camera spring (UDishonoredCamera_PhysicalReact::OnImpact) by RandRange(m_fMinHitReact_Camera,
// m_fMaxHitReact_Camera) from the pawn's combat tweaks (pawn tweaks +0x100 m_pCombatTweaks, +0x9C/+0xA0); the
// knock-back itself is a separate impulse (m_fMin/MaxHitReact -> ImpulsePawn), left alone. Scale the camera pair once
// per tweaks object (shared asset: remember what we wrote so it is never scaled twice; re-read if the game reset it).
static void ApplyHitCameraScale(ADishonoredPlayerPawn* pawn) {
    static UObject* scaledFor = nullptr;
    static float wroteMin = 0.0f, wroteMax = 0.0f;
    UDisTweaks_Pawn* tweaks = pawn ? pawn->m_pPawnTweaks : nullptr;
    UObject* combat = tweaks ? reinterpret_cast<UObject*>(tweaks->m_pCombatTweaks) : nullptr;
    if (!combat) return;
    float& mn = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(combat) + 0x9C);
    float& mx = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(combat) + 0xA0);
    if (combat == scaledFor && mn == wroteMin && mx == wroteMax) return;
    float scale = Clamp(g_cfg.hitCameraScale, 0.0f, 2.0f);
    Log("hit camera: %s min %.1f max %.1f -> x%.2f", combat->GetFullName().c_str(), mn, mx, scale);
    mn *= scale;
    mx *= scale;
    scaledFor = combat;
    wroteMin = mn;
    wroteMax = mx;
}

// Hits and knock-backs (builds 194-195): the hit reaction throws the camera back for a moment (scaled down by
// HitCameraScale) and the knock-back hops the pawn into the air for ~90 ms (log 20:44: physics 1 -> 2 -> 1). The body
// logic read that as a take-off (3-frame hide), a quick mantle (camera pulled back: whole body shrunk, then regrown at
// the bottom of the view) and a corner lean (the camera's sideways sway tilted the spine - the neck collapse point
// ended 5 uu in front of the camera). Health dropping is the hit itself; the stunned state covers being knocked about.
static void UpdateHitState(ADishonoredPlayerPawn* pawn) {
    static ADishonoredPlayerPawn* hpPawn = nullptr;
    static int lastHealth = -1;
    static DWORD hitUntil = 0;
    static UClass* stunCls = nullptr;
    if (!stunCls) stunCls = UObject::FindClass("Class DishonoredGame.StatePlayerMasterStunned");
    DWORD now = GetTickCount();
    if (hpPawn != pawn) { hpPawn = pawn; lastHealth = -1; hitUntil = 0; }
    int hp = pawn->Health;
    if (lastHealth >= 0 && hp < lastHealth && hp > 0) {
        hitUntil = now + static_cast<DWORD>(g_cfg.hitHideMs);
        Log("hit: health %d -> %d - no mantle/take-off/lean reactions%s for %.0f ms", lastHealth, hp,
            g_cfg.hitHideTorso ? ", torso hidden" : "", g_cfg.hitHideMs);
    }
    lastHealth = hp;
    UObject* st = pawn->m_pPlayerMasterFSM ? pawn->m_pPlayerMasterFSM->m_pCurrentState : nullptr;
    bool stunned = st && stunCls && st->IsA(stunCls);
    static bool wasStunned = false;
    if (stunned != wasStunned) { wasStunned = stunned; Log("stunned %s", stunned ? "" : "over"); }
    g_hitActive = stunned || (hitUntil && now < hitUntil);
}

static void UpdateLean(ADishonoredPlayerPawn* pawn) {
    if (!g_body || !g_bodyAnimated || !pawn) return;
    ApplyHitCameraScale(pawn);
    // Visible torso hidden from the waist up whenever the camera can end up inside it: slides (and rising out of
    // them), swimming, very low crouch, jumps/vaults and landings, mantles (physics 0). Held 0.3 s after the last
    // trigger so single-frame gaps at the start/end don't flash the inside of the coat (video 19-57-54).
    {
        int phys = static_cast<int>(pawn->Physics);
        bool takedown = g_cfg.takedownHideTorso && InTakedown(pawn);
        static bool wasTakedown = false;
        if (takedown != wasTakedown) { wasTakedown = takedown; Log("takedown %s", takedown ? "started: torso hidden" : "over"); }
        // Hits (UpdateHitState): the torso is hidden for the reaction, then the usual 0.3 s hold and ease back.
        bool hit = g_cfg.hitHideTorso && g_hitActive;
        // Looking steeply up (build 197, video 21-02-00): the look-up hide only collapsed spine_3 and up, and the coat's
        // lapel/collar vertices partly weighted below it stretched into long sheets that swept past the camera with
        // every step while walking/sprinting. Nothing of the body is in view up there: collapse from the waist instead
        // (sheets then point at the belly, far below). Small hysteresis so the view bob doesn't flicker it.
        static bool lookUp = false;
        lookUp = g_cfg.lookUpHideTorsoDeg > 0.0f && g_viewPitchDeg > g_cfg.lookUpHideTorsoDeg - (lookUp ? 5.0f : 0.0f);
        bool want = takedown || hit || lookUp || g_isSliding || g_slideAmount > 0.0f || g_swimAmount > 0.0f || g_climbAmount > 0.0f || g_smoothedSink > g_cfg.deepCrouchSink ||
                    g_smoothedSink - g_crouchSink > 3.0f ||
                    (g_cfg.mantleFollow && phys != 1 && (-g_cornerLeanFwd > 15.0f || g_stairDz < -15.0f)) ||   // mantle: camera pulled back behind the body (user screenshot)
                    (g_cfg.jumpHideTorso && !g_cfg.jumpArms && (g_jumpAmount > 0.0f || g_landDip < -0.5f || phys == 0 || phys == 2));
        static DWORD lastWant = 0;
        DWORD nowT = GetTickCount();
        if (want) lastWant = nowT;
        g_torsoHidden = want || (lastWant && nowT - lastWant < 300);
    }
    auto* pc = static_cast<APlayerController*>(pawn->Controller);
    float deg = 0.0f;
    if (pc) {
        int p = pc->Rotation.Pitch & 0xFFFF;
        deg = (p > 32768 ? p - 65536 : p) * (360.0f / 65536.0f);
    }
    g_viewPitchDeg = deg;
    // No lean while the legs own a special pose or the body is airborne/climbing.
    bool allowed = !g_isSliding && !g_isClimbing;
    float lean = deg > 0.0f ? deg * g_cfg.leanUp : deg * g_cfg.leanDown;
    lean = allowed ? Clamp(lean, -g_cfg.leanMax, g_cfg.leanMax) : 0.0f;
    if (g_slideAmount > 0.0f && !g_isClimbing) lean = g_slideAmount * g_cfg.slideRecline;   // slide: recline (back)
    static float smoothed = 0.0f;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    float dt = last ? Clamp((now - last) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    last = now;
    smoothed += Clamp(lean - smoothed, -dt * 360.0f, dt * 360.0f);   // fast, but no pop when a slide ends
    int units = static_cast<int>(g_cfg.leanSign * smoothed * 0.5f * 65536.0f / 360.0f);
    // Corner lean: tilt sideways so the shoulders end up under the camera (spine pivot ~LeanPivotHeight below
    // the eyes), and add a little forward bend for peeking forward. Dead zone ignores walking sway.
    static float side = 0.0f;
    float r = fabsf(g_cornerLeanRight) > 3.0f ? g_cornerLeanRight * (1.0f - Clamp(g_cfg.cornerLeanHips, 0.0f, 1.0f)) : 0.0f;
    // Not from a hit's camera sway (it read as a lean and tilted the neck into the view - log 20:44, lean 10).
    float sideWant = allowed && g_cfg.cornerLean && !g_hitActive
        ? Clamp(atan2f(r, g_cfg.leanPivotHeight) * 57.29578f, -g_cfg.leanMax, g_cfg.leanMax) : 0.0f;
    side += Clamp(sideWant - side, -dt * 240.0f, dt * 240.0f);
    int rollUnits = static_cast<int>(g_cfg.leanSideSign * side * 0.5f * 65536.0f / 360.0f);
    float f = (allowed && g_cfg.cornerLean && !g_hitActive && g_cornerLeanFwd > 3.0f) ? g_cornerLeanFwd : 0.0f;
    units += static_cast<int>(-g_cfg.leanSign * Clamp(atan2f(f, g_cfg.leanPivotHeight) * 57.29578f, 0.0f, 30.0f) * 0.5f * 65536.0f / 360.0f);
    USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
    for (USkeletalMeshComponent* comp : comps) {
        if (!comp) continue;
        for (const char* bone : kSpineBones) {
            auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(bone)));
            if (!ctl) continue;
            ctl->BoneRotation = FRotator{ units, 0, rollUnits };
            // Sliding: hide the whole visible torso from the waist up (spine_1 and everything above). Hiding from
            // spine_3 left the lower chest/belly, which the camera clipped into (user).
            bool hideTorso = comp == g_body && bone == kSpineBones[0] && g_torsoAmount > 0.0f;
            ctl->BoneScale = hideTorso ? 1.0f - 0.999f * g_torsoAmount : 1.0f;
            ctl->ControlStrength = ctl->StrengthTarget = (units || rollUnits || hideTorso) ? 1.0f : 0.0f;
        }
    }
}

// Shadow arm pose per held item, as Corvo holds them in first person (degrees: forward raise from the shoulder,
// pull toward the middle, elbow bend). Sword: low ready stance, hand by the hip, blade forward. Pistol/crossbow:
// arm out in front of the chest, nearly straight, aimed inward. Power: hand raised in front, elbow half bent.
// ShadowArmRaise/In/ElbowBend in the ini scale all of them (percent of these, 100 = as designed).
struct ArmPose { float raise, in, elbow, forearmIn; };
static ArmPose ShadowArmPose(int hand) {
    // Sword arm low (user: "his right arm would be lower to compensate for the sword's length").
    // Power: upper arm nearly straight down under the shoulder, forearm raised to in front of the chest (user).
    // Pistol/crossbow: arm half extended forward (user). Elbow and forearm-in are in player space (build 81).
    static const ArmPose kPoses[4] = { { 0, 0, 0, 0 }, { 20, -6, 40, 0 }, { 45, 10, 45, 10 }, { 12, 10, 110, 30 } };
    int k = g_handKind[hand];
    ArmPose p = kPoses[(k >= 0 && k <= 3) ? k : 0];
    return { p.raise * g_cfg.shadowArmRaiseDeg / 100.0f, p.in * g_cfg.shadowArmInDeg / 100.0f, p.elbow * g_cfg.shadowElbowBendDeg / 100.0f,
             p.forearmIn * g_cfg.shadowArmInDeg / 100.0f };
}

// 0..1 how far a free hand's arm floats up in front while jumping/falling (eased with the jump pose).
static float JumpArmAmount(int hand) {
    if (!g_cfg.jumpArms) return 0.0f;
    bool busy = hand == 0 ? g_leftHandBusy : g_rightHandBusy;
    return busy ? 0.0f : g_jumpAmount;
}

static void UpdateCoat() {
    static int hiddenFront = -1;   // -1 unknown, 0 shown, 1 hidden
    static USkeletalMeshComponent* lastComp = nullptr;
    if (!g_body || !g_bodyAnimated) return;
    if (g_body != lastComp) { lastComp = g_body; hiddenFront = -1; }

    // Mode 1: hide the whole lower coat (all four panels) while crouched.
    int wantHidden = 0;   // (instant HideBoneByName popped the coat; the eased BoneScale below does it)
    if (wantHidden != hiddenFront) {
        for (int i = 0; i < 4; ++i) {   // coat panels only
            FName name(kCoatBones[i]);
            if (name.FNameEntryId < 0) continue;
            if (wantHidden) g_body->HideBoneByName(name, EPhysBodyOp::PBO_None);
            else g_body->UnHideBoneByName(name);
        }
        hiddenFront = wantHidden;
    }

    // HideBoneByName has no visible effect on these bones (test 33; it works on the shoulders), so also shrink
    // each panel chain through its own SkelControl: BoneScale ~0 scales the chain root and everything below it.
    // Follow the legs (eased lift), not the instant crouch flag: otherwise the coat popped back while the knees
    // were still bent wide (test video 10-30-38). Hidden as soon as the knees start to bend.
    bool shrink = g_cfg.coatMode == 1 && (g_isCrouched || (g_footLift > 4.0f && g_smoothedSink > 1.0f));   // not on stair dips
    // Jumping with the arms shown: the torso stays (the arms hang off it) but the coat still hides, as before.
    if (g_cfg.jumpArms && g_cfg.jumpHideTorso && (g_jumpAmount > 0.0f || g_landDip < -0.5f || g_hoverAmount > 0.0f)) shrink = true;
    static int lastShrink = -1;
    if ((shrink ? 1 : 0) != lastShrink) {
        Log("coat %s", shrink ? "hidden (crouched)" : "shown");
        lastShrink = shrink ? 1 : 0;
    }
    // Shrink/grow the coat gradually (~0.3 s) instead of popping it in one frame (user: crouching jittery).
    static float coatShrink = 0.0f;
    static DWORD coatLast = 0;
    {
        DWORD nowC = GetTickCount();
        float dtC = coatLast ? Clamp((nowC - coatLast) / 1000.0f, 0.0f, 0.1f) : 0.0f;
        coatLast = nowC;
        // Sprinting: the coat flaps into view - hide it the same way (user), back when slowing down.
        bool sprinting = false;
        if (g_cfg.coatHideSprint && g_bodyPawn) {
            FVector v = g_bodyPawn->Velocity;
            sprinting = sqrtf(v.X * v.X + v.Y * v.Y) > g_cfg.coatSprintSpeed && !g_isSliding;
        }
        coatShrink += Clamp((shrink || sprinting ? 1.0f : 0.0f) - coatShrink, -dtC * g_cfg.reappearSpeed, dtC * 3.5f);
        // Hide fast (clipping), show again slowly so arms/chest/coat grow back into place (user, video 20-17-53).
        bool upWant = g_cfg.hideShouldersUp || g_hideUpperNow;
        g_upperAmount += Clamp((upWant ? 1.0f : 0.0f) - g_upperAmount, -dtC * g_cfg.reappearSpeed, dtC * 15.0f);
        g_torsoAmount += Clamp((g_torsoHidden ? 1.0f : 0.0f) - g_torsoAmount, -dtC * g_cfg.reappearSpeed, dtC * 15.0f);
        // Crouching under tables (build 189): the game drops the cylinder to 33 (sink 54.5, same as a slide) and the
        // crouched legs folded up into the low camera (video 18-36-15). Fold the legs into the hips there (the torso
        // is already hidden), and grow them back out as the crouch returns to its normal height. Not in slides (the
        // slide pose is the point); held 0.1 s so the end of a slide doesn't pulse.
        static DWORD deepSince = 0;
        bool deep = g_cfg.lowCrouchHideLegs && g_crouchSink > g_cfg.deepCrouchSink && !g_isSliding && g_slideAmount <= 0.0f &&
                    !g_isClimbing && g_swimAmount <= 0.0f;
        if (!deep) deepSince = 0; else if (!deepSince) deepSince = nowC;
        bool hideLegs = deep && nowC - deepSince >= 100;
        float before = g_legHideAmount;
        g_legHideAmount += Clamp((hideLegs ? 1.0f : 0.0f) - g_legHideAmount, -dtC * g_cfg.lowCrouchLegsBack, dtC * 8.0f);
        if ((before <= 0.0f) != (g_legHideAmount <= 0.0f) || (before >= 1.0f) != (g_legHideAmount >= 1.0f))
            Log("low crouch: legs %s", g_legHideAmount >= 1.0f ? "folded away" : g_legHideAmount <= 0.0f ? "back" : hideLegs ? "folding away" : "coming back");
    }

    // Mode 2: pull all four panels toward hanging straight down.
    float hangStrength = g_cfg.coatMode == 2 ? Clamp(g_cfg.coatHangStrength, 0.0f, 1.0f) : 0.0f;
    auto toUnits = [](float deg) { return static_cast<int>(deg * 65536.0f / 360.0f); };
    FRotator hang{ toUnits(g_cfg.coatHangPitch), toUnits(g_cfg.coatHangYaw), toUnits(g_cfg.coatHangRoll) };
    // Shadow arms follow what each hand holds (eased so drawing/holstering doesn't pop).
    {
        static DWORD lastW = 0;
        DWORD nowW = GetTickCount();
        float dtW = lastW ? Clamp((nowW - lastW) / 1000.0f, 0.0f, 0.1f) : 0.0f;
        lastW = nowW;
        bool busy[2] = { g_leftHandBusy, g_rightHandBusy };
        for (int h = 0; h < 2; ++h)
            g_weaponRaise[h] += Clamp((busy[h] && g_cfg.shadowWeaponArms ? 1.0f : 0.0f) - g_weaponRaise[h], -dtW * 5.0f, dtW * 5.0f);
    }
    // Both bodies get the same pose controls; only the visible one hides its coat and busy arms. The shadow body
    // keeps them, so the shadow always has a coat and (fake) arms.
    USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
    for (int ci = 0; ci < 2; ++ci) {
    USkeletalMeshComponent* comp = comps[ci];
    if (!comp) continue;
    bool isShadow = ci == 1;
    for (int b = 0; b < 12; ++b) {
        FName name(kCoatBones[b]);
        auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(name));
        if (!ctl) continue;
        if (b == 11) {   // pelvis: legs folded away under tables (visible body only; the shadow keeps its legs)
            bool fold = !isShadow && g_legHideAmount > 0.0f;
            ctl->bApplyRotation = 0;
            ctl->BoneScale = fold ? 1.0f - 0.999f * g_legHideAmount : 1.0f;
            ctl->ControlStrength = ctl->StrengthTarget = fold ? 1.0f : 0.0f;
            continue;
        }
        if (b >= 9) {   // chest (Torso_jnt) + neck: always shrunk on the body you see; shoulders/arms are siblings, so they stay
            bool hideIt = !isShadow && g_cfg.hideTorso;
            ctl->bApplyRotation = 0;
            ctl->BoneScale = hideIt ? 0.001f : 1.0f;
            ctl->ControlStrength = ctl->StrengthTarget = hideIt ? 1.0f : 0.0f;
            continue;
        }
        if (b == 8) {   // whole upper body: shrunk while it would clip (airborne, sprint, look-up, camera dip) or always if HideShouldersUp
            bool hideChest = !isShadow && g_upperAmount > 0.0f;
            ctl->bApplyRotation = 0;
            ctl->BoneScale = hideChest ? 1.0f - 0.999f * g_upperAmount : 1.0f;
            ctl->ControlStrength = ctl->StrengthTarget = hideChest ? 1.0f : 0.0f;
            continue;
        }
        if (b >= 6) {   // elbows: forearms hinge on local Y (rest quats are pure Y) -> pitch in bone space
            float crouchAmount = Clamp(g_footLift / 30.0f, 0.0f, 1.0f);
            ctl->bApplyRotation = 1;
            ctl->bAddRotation = 1;
            ctl->BoneRotationSpace = EBoneControlSpace::BCS_BoneSpace;
            float weapon = (isShadow && !g_cfg.armIK && !g_cfg.fpArmShadow) ? g_weaponRaise[b == 6 ? 0 : 1] : 0.0f;
            if (weapon > 0.0f) {
                // Shadow body, hand holding a weapon/power: raise the forearm in PLAYER space (forward/up, then in
                // toward the middle). In bone space the elbow hinge follows the idle arm's twist and the forearm
                // swung out to the side (video 15-44-20).
                ArmPose pose = ShadowArmPose(b == 6 ? 0 : 1);
                float side = b == 6 ? -1.0f : 1.0f;
                float pitch = g_cfg.leanSign * (pose.elbow + g_cfg.elbowBendDeg * crouchAmount) * weapon;
                float yaw = -side * pose.forearmIn * weapon;
                ctl->BoneRotationSpace = EBoneControlSpace::BCS_ActorSpace;
                ctl->BoneRotation = FRotator{ static_cast<int>(pitch * 65536.0f / 360.0f), static_cast<int>(yaw * 65536.0f / 360.0f), 0 };
                ctl->BoneScale = 1.0f;
                ctl->ControlStrength = ctl->StrengthTarget = 1.0f;
                continue;
            }
            float bend = g_cfg.elbowBendDeg * crouchAmount + JumpArmAmount(b == 6 ? 0 : 1) * g_cfg.jumpArmElbow;
            ctl->BoneRotation = FRotator{ static_cast<int>(bend * 65536.0f / 360.0f), 0, 0 };
            ctl->BoneScale = 1.0f;
            ctl->ControlStrength = ctl->StrengthTarget = bend != 0.0f ? 1.0f : 0.0f;
            continue;
        }
        if (b >= 4) {   // arms: shrink the side whose hand holds something (the 1P arm is showing there)
            bool handBusy = b == 4 ? g_leftHandBusy : g_rightHandBusy;
            // Shadow body: a busy hand's arm is cast by the first-person arms copy instead (UpdateArmsShadow).
            // Shadow body keeps its arms (they reach the 1P hands by IK, UpdateArmsShadow): the 1P arms mesh is only
            // forearms and hands, so its shadow alone floated in front of the chest (video 17-08-39).
            bool hide = handBusy && !isShadow && g_cfg.hideArms;
            // Crouched with that hand free: the walk cycle swings the hands through the view between the knees,
            // so roll the arm out from the shoulder (player space, around the forward axis), eased with the crouch.
            float crouchAmount = Clamp(g_footLift / 30.0f, 0.0f, 1.0f);
            bool spread = !hide && crouchAmount > 0.01f && g_cfg.armSpreadDeg != 0.0f;
            float side = b == 4 ? -1.0f : 1.0f;   // left arm rolls the other way
            // Standing and looking down: the hanging arms sat just below the bottom of the view (user, video
            // 14-55-18; they only showed when the sprint swing brought them forward). Swing them forward from the
            // shoulder as the view tilts down, visible body only.
            float down = Clamp(-g_viewPitchDeg / 45.0f, 0.0f, 1.0f) * (1.0f - crouchAmount);
            bool forward = !hide && !isShadow && g_cfg.armForwardDeg != 0.0f && down > 0.01f;
            float fwdDeg = forward ? g_cfg.leanSign * g_cfg.armForwardDeg * down : 0.0f;
            float rollDeg = spread ? side * g_cfg.armSpreadDeg * crouchAmount : 0.0f;
            // Jumping/falling with that hand free: the arm floats up in front (user reference), back to the side on landing.
            float jumpArm = hide ? 0.0f : JumpArmAmount(b == 4 ? 0 : 1);
            if (jumpArm > 0.0f) {
                fwdDeg += g_cfg.leanSign * g_cfg.jumpArmRaise * jumpArm;
                rollDeg = rollDeg * (1.0f - jumpArm) - side * g_cfg.jumpArmIn * jumpArm;
                forward = true;
            }
            // Shadow body: an arm whose hand holds a weapon/power is raised forward like the 1P arm (the shadow
            // kept the idle arms-down pose with weapons out, user screenshot), and pulled in a little.
            float weapon = (isShadow && !g_cfg.armIK && !g_cfg.fpArmShadow) ? g_weaponRaise[b == 4 ? 0 : 1] : 0.0f;
            if (weapon > 0.0f) {
                ArmPose pose = ShadowArmPose(b == 4 ? 0 : 1);
                fwdDeg += g_cfg.leanSign * pose.raise * weapon;
                rollDeg = rollDeg * (1.0f - weapon) - side * pose.in * weapon;
                forward = true;
            }
            ctl->bApplyRotation = (spread || forward) ? 1 : 0;
            ctl->bAddRotation = 1;
            ctl->BoneRotationSpace = EBoneControlSpace::BCS_ActorSpace;
            ctl->BoneRotation = FRotator{ static_cast<int>(fwdDeg * 65536.0f / 360.0f), 0, static_cast<int>(rollDeg * 65536.0f / 360.0f) };
            ctl->BoneScale = hide ? 0.001f : 1.0f;
            float strength = (hide || spread || forward) ? 1.0f : 0.0f;
            ctl->ControlStrength = ctl->StrengthTarget = strength;
            continue;
        }
        ctl->BoneRotation = hang;
        ctl->bApplyRotation = hangStrength > 0.0f ? 1 : 0;
        // The coat panels hang off the thighs, not the spine: hide them with the torso (jump/vault frames showed the
        // inside of the coat - video 19-57-54).
        float coatHide = g_torsoAmount > coatShrink ? g_torsoAmount : coatShrink;
        bool shrinkThis = coatHide > 0.0f && !isShadow;
        ctl->BoneScale = shrinkThis ? 1.0f - 0.999f * coatHide : 1.0f;
        float strength = shrinkThis ? 1.0f : hangStrength;
        if (b >= 2 && !shrinkThis && hangStrength <= 0.0f && g_cfg.coatRearFlare != 0.0f) {
            // Rear panels: flare back off the legs (they are parented to the thighs and hugged them).
            ctl->bApplyRotation = 1;
            ctl->bAddRotation = 1;
            ctl->BoneRotationSpace = EBoneControlSpace::BCS_BoneSpace;
            ctl->BoneRotation = FRotator{ static_cast<int>(g_cfg.coatRearFlare * 65536.0f / 360.0f), 0, 0 };
            strength = 1.0f;
        } else if (b < 4) {
            ctl->bAddRotation = 0;
            ctl->BoneRotationSpace = EBoneControlSpace::BCS_ActorSpace;
        }
        ctl->ControlStrength = strength;
        ctl->StrengthTarget = strength;
    }
    }
}

// Which hands hold something: UDishonoredInventory::m_EquipUsageInfo[usage].m_iEquippedSlot indexes m_Slots
// (FPawnInventorySlot, 12 bytes, item pointer first). Usage 1 = primary (right hand: sword), 2 = secondary (left:
// pistol, crossbow, powers...). An item whose class name contains "Empty" is the empty-hands item.

static void UpdateHands(ADishonoredPlayerPawn* pawn) {
    if (!pawn || !pawn->m_pInventory) return;
    UDishonoredInventory* inv = pawn->m_pInventory;
    auto* slots = reinterpret_cast<RawArray*>(&inv->m_Slots);
    bool busy[3] = { false, false, false };
    std::string names[3];
    for (int usage = 1; usage <= 2; ++usage) {
        int slot = inv->m_EquipUsageInfo[usage].m_iEquippedSlot;
        if (slot < 0 || slot >= slots->Num || !slots->Data) continue;
        auto* item = *reinterpret_cast<UObject**>(static_cast<uint8_t*>(slots->Data) + slot * 12);
        if (!item || !item->Class) continue;
        names[usage] = item->Class->GetName();
        busy[usage] = names[usage].find("Empty") == std::string::npos;
        g_handItem[usage == 2 ? 0 : 1] = busy[usage] ? item : nullptr;
    }
    for (int usage = 1; usage <= 2; ++usage) if (!busy[usage]) g_handItem[usage == 2 ? 0 : 1] = nullptr;
    // What each hand holds, for the shadow arm pose: 1 sword, 2 gun (pistol/crossbow), 3 power or other.
    for (int usage = 1; usage <= 2; ++usage) {
        const std::string& n = names[usage];
        int kind = !busy[usage] ? 0 : n.find("Sword") != std::string::npos ? 1
                 : (n.find("Pistol") != std::string::npos || n.find("Crossbow") != std::string::npos) ? 2 : 3;
        g_handKind[usage == 2 ? 0 : 1] = kind;
    }
    if (busy[1] != g_rightHandBusy || busy[2] != g_leftHandBusy)
        Log("hands: right %s (%s), left %s (%s)", busy[1] ? "busy" : "free", names[1].c_str(),
            busy[2] ? "busy" : "free", names[2].c_str());
    g_rightHandBusy = busy[1];
    g_leftHandBusy = busy[2];
}

// Diagnostic for "head shadow slides back when crouching": every 2 s while crouched (and once standing), log the
// head component's head bone against the shadow body's head and neck bones, in world space.
static void DiagHeadShadow() {
    static DWORD last = 0;
    static int standingLogs = 0;
    if (!g_head || !g_shadowBody) return;
    DWORD now = GetTickCount();
    if (now - last < 2000) return;
    if (!g_isCrouched && standingLogs >= 2) return;
    last = now;
    if (!g_isCrouched) ++standingLogs;
    FName head("head_jnt"), neck("neck_jnt");
    if (head.FNameEntryId < 0 || neck.FNameEntryId < 0) return;
    FVector h = g_head->GetBoneLocation(head, 0);
    FVector sh = g_shadowBody->GetBoneLocation(head, 0);
    FVector sn = g_shadowBody->GetBoneLocation(neck, 0);
    Log("head diag (%s): headComp head (%.0f %.0f %.0f)  shadow head (%.0f %.0f %.0f)  shadow neck (%.0f %.0f %.0f)  parent %p",
        g_isCrouched ? "crouched" : "standing", h.X, h.Y, h.Z, sh.X, sh.Y, sh.Z, sn.X, sn.Y, sn.Z,
        g_head->ParentAnimComponent);
}

// Held item shadows: a shadow-only copy of each held item's first-person model (sword, pistol, crossbow, Heart,
// grenades...) is kept in the shadow body's hand. Each frame the real model's transform relative to the 1P arms'
// hand bone is measured and re-applied relative to the shadow body's hand bone, so whatever pose the shadow arm
// has, the item sits in its hand the way the game holds it. Row-vector affine transforms (UE3 FMatrix layout).
struct Xf { float m[3][3]; float t[3]; };

static Xf g_armsShadowUnpitch = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { 0, 0, 0 } };   // arm/item shadow view-pitch undo

static Xf XfFromMatrix(const FMatrix& M) {
    const FPlane* r[4] = { &M.XPlane, &M.YPlane, &M.ZPlane, &M.WPlane };
    Xf x;
    for (int i = 0; i < 3; ++i) { x.m[i][0] = r[i]->X; x.m[i][1] = r[i]->Y; x.m[i][2] = r[i]->Z; }
    x.t[0] = r[3]->X; x.t[1] = r[3]->Y; x.t[2] = r[3]->Z;
    return x;
}

static Xf XfMul(const Xf& a, const Xf& b) {   // apply a, then b
    Xf o;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            o.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    for (int j = 0; j < 3; ++j) o.t[j] = a.t[0] * b.m[0][j] + a.t[1] * b.m[1][j] + a.t[2] * b.m[2][j] + b.t[j];
    return o;
}

static bool XfInv(const Xf& a, Xf& o) {
    const float (*m)[3] = a.m;
    float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (fabsf(det) < 1e-8f) return false;
    float id = 1.0f / det;
    o.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
    o.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    o.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    o.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
    o.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    o.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    o.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
    o.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    o.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    for (int j = 0; j < 3; ++j) o.t[j] = -(a.t[0] * o.m[0][j] + a.t[1] * o.m[1][j] + a.t[2] * o.m[2][j]);
    return true;
}

static Xf XfFromRotator(const FRotator& r, const FVector& loc) {   // FRotationTranslationMatrix
    const float k = 6.2831853f / 65536.0f;
    float sp = sinf(r.Pitch * k), cp = cosf(r.Pitch * k), sy = sinf(r.Yaw * k), cy = cosf(r.Yaw * k);
    float sr = sinf(r.Roll * k), cr = cosf(r.Roll * k);
    Xf x;
    x.m[0][0] = cp * cy;                 x.m[0][1] = cp * sy;                 x.m[0][2] = sp;
    x.m[1][0] = sr * sp * cy - cr * sy;  x.m[1][1] = sr * sp * sy + cr * cy;  x.m[1][2] = -sr * cp;
    x.m[2][0] = -(cr * sp * cy + sr * sy); x.m[2][1] = cy * sr - cr * sp * sy; x.m[2][2] = cr * cp;
    x.t[0] = loc.X; x.t[1] = loc.Y; x.t[2] = loc.Z;
    return x;
}

static void XfDecompose(const Xf& x, FVector& loc, FRotator& rot, float& scale) {
    float ax[3][3], s[3];
    for (int i = 0; i < 3; ++i) {
        s[i] = sqrtf(x.m[i][0] * x.m[i][0] + x.m[i][1] * x.m[i][1] + x.m[i][2] * x.m[i][2]);
        for (int j = 0; j < 3; ++j) ax[i][j] = s[i] > 1e-6f ? x.m[i][j] / s[i] : 0.0f;
    }
    scale = (s[0] + s[1] + s[2]) / 3.0f;
    const float k = 65536.0f / 6.2831853f;
    float pitch = atan2f(ax[0][2], sqrtf(ax[0][0] * ax[0][0] + ax[0][1] * ax[0][1]));
    float yaw = atan2f(ax[0][1], ax[0][0]);
    float syx = -sinf(yaw), syy = cosf(yaw);   // Y axis of FRotator(pitch, yaw, 0)
    float roll = atan2f(ax[2][0] * syx + ax[2][1] * syy, ax[1][0] * syx + ax[1][1] * syy);
    rot = FRotator{ static_cast<int>(pitch * k), static_cast<int>(yaw * k), static_cast<int>(roll * k) };
    loc = FVector{ x.t[0], x.t[1], x.t[2] };
}

// World transform of a bone, read straight from SpaceBases (component-space FBoneAtom: quat, translation, scale)
// times the component's LocalToWorld. Not GetBoneMatrix: its FMatrix return through ProcessEvent overran the
// stack and crashed on startup (build 83).
static bool BoneXf(USkeletalMeshComponent* comp, const char* bone, Xf& out) {
    if (!comp || !comp->SkeletalMesh) return false;
    FName name(bone);
    if (name.FNameEntryId < 0) return false;
    int idx = comp->MatchRefBone(name);
    auto* bases = reinterpret_cast<RawArray*>(&comp->SpaceBases);
    if (idx < 0 || !bases->Data || idx >= bases->Num) return false;
    const float* a = reinterpret_cast<const float*>(static_cast<uint8_t*>(bases->Data) + idx * 32);
    float x = a[0], y = a[1], z = a[2], w = a[3], s = a[7];
    if (!(s > 0.0f && s < 100.0f)) s = 1.0f;
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;
    Xf atom;
    atom.m[0][0] = (1.0f - (yy + zz)) * s; atom.m[0][1] = (xy + wz) * s;          atom.m[0][2] = (xz - wy) * s;
    atom.m[1][0] = (xy - wz) * s;          atom.m[1][1] = (1.0f - (xx + zz)) * s; atom.m[1][2] = (yz + wx) * s;
    atom.m[2][0] = (xz + wy) * s;          atom.m[2][1] = (yz - wx) * s;          atom.m[2][2] = (1.0f - (xx + yy)) * s;
    atom.t[0] = a[4]; atom.t[1] = a[5]; atom.t[2] = a[6];
    // UE3 GetTransformMatrix: bones live under the mesh's RotOrigin/Origin before LocalToWorld. The 1P arms mesh
    // uses it to stand up (it lies on its side otherwise) - leaving it out put every 1P bone in the wrong place
    // (arm shadow builds 89-93 all landed below/beside the body).
    Xf origin = XfFromRotator(comp->SkeletalMesh->RotOrigin, comp->SkeletalMesh->Origin);
    out = XfMul(XfMul(atom, origin), XfFromMatrix(comp->LocalToWorld));
    return true;
}

// Per frame: the shadow body's hands follow the 1P hands (see AddArmIK), only while that hand holds gear.
static void UpdateArmIK(ADishonoredPlayerPawn* pawn) {
    if (!g_shadowBody || !pawn || !pawn->Mesh) return;
    Xf cam1P, head;
    bool ok = BoneXf(pawn->Mesh, "camera_jnt", cam1P) && BoneXf(g_shadowBody, "head_jnt", head);
    float yaw = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
    float cy = cosf(yaw), sy = sinf(yaw);
    // Eye point of the shadow body: head bone + ArmIKEye (forward, right, up) in player space.
    float eye[3] = { head.t[0] + g_cfg.armIKEye.X * cy - g_cfg.armIKEye.Y * sy,
                     head.t[1] + g_cfg.armIKEye.X * sy + g_cfg.armIKEye.Y * cy,
                     head.t[2] + g_cfg.armIKEye.Z };
    for (int h = 0; h < 2; ++h) {
        auto* ik = static_cast<USkelControlLimb*>(g_shadowBody->FindSkelControl(FName(kHandBones[h])));
        auto* rot = static_cast<USkelControlSingleBone*>(g_shadowBody->FindSkelControl(FName(kHandOffsetBones[h])));
        Xf hand1P;
        float strength = (ok && g_cfg.armIK && BoneXf(pawn->Mesh, kHandBones[h], hand1P)) ? g_weaponRaise[h] : 0.0f;
        if (ik) {
            if (strength > 0.0f)
                ik->EffectorLocation = FVector{ eye[0] + hand1P.t[0] - cam1P.t[0], eye[1] + hand1P.t[1] - cam1P.t[1],
                                                eye[2] + hand1P.t[2] - cam1P.t[2] };
            ik->JointTargetLocation = FVector{ g_cfg.armIKElbow.X, h == 0 ? -g_cfg.armIKElbow.Y : g_cfg.armIKElbow.Y, g_cfg.armIKElbow.Z };
            ik->ControlStrength = ik->StrengthTarget = strength;
        }
        static DWORD lastDiag = 0;
        if (g_cfg.armIK && (g_weaponRaise[0] > 0.5f || g_weaponRaise[1] > 0.5f) && GetTickCount() - lastDiag > 2000 && h == 1) {
            lastDiag = GetTickCount();
            Xf hs;
            bool hsOk = BoneXf(g_shadowBody, kHandBones[h], hs);
            FVector gl = g_shadowBody->GetBoneLocation(FName(kHandBones[h]), 0);
            Log("arm IK diag: ok %d (cam1P %d head %d hand1P %d) ik %p rot %p strength %.2f | eye (%.0f %.0f %.0f) cam1P (%.0f %.0f %.0f) "
                "hand1P (%.0f %.0f %.0f) effector (%.0f %.0f %.0f) shadow hand now (%.0f %.0f %.0f)/(%.0f %.0f %.0f) bases %d",
                ok ? 1 : 0, BoneXf(pawn->Mesh, "camera_jnt", hs) ? 1 : 0, BoneXf(g_shadowBody, "head_jnt", hs) ? 1 : 0,
                BoneXf(pawn->Mesh, kHandBones[h], hs) ? 1 : 0, ik, rot, strength, eye[0], eye[1], eye[2],
                cam1P.t[0], cam1P.t[1], cam1P.t[2], hand1P.t[0], hand1P.t[1], hand1P.t[2],
                ik ? ik->EffectorLocation.X : 0.0f, ik ? ik->EffectorLocation.Y : 0.0f, ik ? ik->EffectorLocation.Z : 0.0f,
                hsOk ? 0.0f : -1.0f, 0.0f, 0.0f, gl.X, gl.Y, gl.Z,
                reinterpret_cast<RawArray*>(&g_shadowBody->SpaceBases)->Num);
        }
        if (rot) {
            if (strength > 0.0f) {
                FVector l; FRotator r; float sc;
                XfDecompose(hand1P, l, r, sc);
                rot->BoneRotation = r;
            }
            rot->ControlStrength = rot->StrengthTarget = strength;
        }
    }
}

// First-person arm shadow (user's idea): a never-rendered copy of the 1P arms mesh that copies the 1P pose
// (SetParentAnimComponent) and sits where the 1P arms are (minus the rigid camera's shift), so every draw,
// holster, swing, block, aim and cast shows in the shadow exactly. Each arm is only cast while its hand holds gear;
// the shadow body hides that arm (UpdateCoat) so there is one arm per side.
// The pawn's LocalToWorld as UE3 builds it: -PrePivot, then scale (DrawScale x DrawScale3D), rotation, location.
static Xf OwnerXf(ADishonoredPlayerPawn* pawn) {
    Xf x = XfFromRotator(pawn->Rotation, pawn->Location);
    float s[3] = { pawn->DrawScale * pawn->DrawScale3D.X, pawn->DrawScale * pawn->DrawScale3D.Y, pawn->DrawScale * pawn->DrawScale3D.Z };
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) x.m[i][j] *= s[i];
    Xf pre = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { -pawn->PrePivot.X, -pawn->PrePivot.Y, -pawn->PrePivot.Z } };
    return XfMul(pre, x);
}

// World frame of a component attached to the pawn with this Translation/Rotation/Scale.
static bool CopyFrameXf(ADishonoredPlayerPawn* pawn, const FVector& t, const FRotator& r, float scale, Xf& out) {
    Xf local = XfFromRotator(r, t);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) local.m[i][j] *= scale;
    out = XfMul(local, OwnerXf(pawn));
    return true;
}

// A bone in its component's own space, including the mesh's RotOrigin/Origin (no LocalToWorld).
static bool CompBoneXf(USkeletalMeshComponent* comp, const char* bone, Xf& out) {
    Xf world, invL;
    if (!BoneXf(comp, bone, world) || !XfInv(XfFromMatrix(comp->LocalToWorld), invL)) return false;
    out = XfMul(world, invL);   // = atom x origin
    return true;
}

static FVector g_armsShadowT = { 0, 0, 0 };   // the arm copy's Translation this frame
static Xf g_armsCopyFrame = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { 0, 0, 0 } };   // the arm copy's world frame this frame (items use it)

static bool ArmsCopyBoneXf(ADishonoredPlayerPawn* pawn, const char* bone, Xf& out);static void UpdateArmsShadow(ADishonoredPlayerPawn* pawn) {
    static int hidden[2] = { -1, -1 };
    USkeletalMeshComponent* arms = pawn ? pawn->Mesh : nullptr;
    bool want = g_cfg.fpArmShadow && arms && arms->SkeletalMesh && g_shadowBody && g_bodyVisible;
    if (!want) {
        if (g_armsShadow) g_armsShadow->SetScale(0.0001f);
        return;
    }
    if (!g_armsShadow) {
        auto* c = static_cast<USkeletalMeshComponent*>(
            StaticConstructObject(USkeletalMeshComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
        if (!c) return;
        c->SetSkeletalMesh(arms->SkeletalMesh, false);
        c->bUpdateSkelWhenNotRendered = 1;
        if (arms->LightEnvironment) c->SetLightEnvironment(arms->LightEnvironment);
        c->HiddenGame = 1;
        c->CastShadow = 1;
        c->bCastDynamicShadow = 1;
        c->bCastHiddenShadow = 1;
        pawn->AttachComponent(c);
        c->SetParentAnimComponent(arms);
        c->UpdateParentBoneMap();
        c->SetHidden(true);
        g_armsShadow = c;
        hidden[0] = hidden[1] = -1;
        Log("first-person arm shadow created: %p (%s)", c, arms->SkeletalMesh->GetName().c_str());
    }
    // Placement: the 1P arms' own Translation/Rotation (minus the rigid camera shift) - their LocalToWorld is set
    // by the game separately (rotated with the camera, ~70 uu lower; build 89 log), so it can't be used - plus a
    // correction that walks the copy's measured shoulders onto the shadow body's shoulders (damped, actor space).
    // Component space only (the LocalToWorld fields of these components do not match where they render - build 95
    // measured ~950 uu off). The arm copy and the shadow body are both on the pawn with the same Rotation/Scale;
    // they differ by Translation and mesh origin. Shift the copy so its shoulder midpoint matches the body's.
    bool any = g_leftHandBusy || g_rightHandBusy;
    Xf cL, cR, bL, bR;
    FVector t = arms->Translation;
    t.X -= g_armsShiftLocal.X; t.Y -= g_armsShiftLocal.Y; t.Z -= g_armsShiftLocal.Z;
    FVector zero{ 0, 0, 0 };
    if (CompBoneXf(arms, "shoulder_L_jnt", cL) && CompBoneXf(arms, "shoulder_R_jnt", cR) &&
        CompBoneXf(g_shadowBody, "shoulder_L_jnt", bL) && CompBoneXf(g_shadowBody, "shoulder_R_jnt", bR)) {
        Xf rs = XfFromRotator(arms->Rotation, zero);   // component -> actor (rotation, no translation)
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) rs.m[i][j] *= arms->Scale;
        float diff[3];
        for (int k = 0; k < 3; ++k) diff[k] = 0.5f * (bL.t[k] + bR.t[k]) - 0.5f * (cL.t[k] + cR.t[k]);
        float d[3];
        for (int j = 0; j < 3; ++j) d[j] = diff[0] * rs.m[0][j] + diff[1] * rs.m[1][j] + diff[2] * rs.m[2][j];
        t.X = g_shadowBodyT.X + d[0]; t.Y = g_shadowBodyT.Y + d[1]; t.Z = g_shadowBodyT.Z + d[2];
        static DWORD lastDiag = 0;
        if (any && GetTickCount() - lastDiag > 3000) {
            lastDiag = GetTickCount();
            Log("arm shadow: body T (%.0f %.0f %.0f), copy T (%.0f %.0f %.0f), 1P arms T (%.0f %.0f %.0f)",
                g_shadowBodyT.X, g_shadowBodyT.Y, g_shadowBodyT.Z, t.X, t.Y, t.Z, arms->Translation.X, arms->Translation.Y, arms->Translation.Z);
        }
    }
    g_armsShadowT = t;
    // Shadow body arms reach the copy's hands (two-bone IK on hand_X, effector in actor space), so the upper arm and
    // elbow join the 1P forearm/hand shadow into one arm.
    {
        Xf copyLocal = XfFromRotator(arms->Rotation, t);
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) copyLocal.m[i][j] *= arms->Scale;
        for (int h = 0; h < 2; ++h) {
            auto* ik = static_cast<USkelControlLimb*>(g_shadowBody->FindSkelControl(FName(kHandBones[h])));
            if (!ik) continue;
            Xf hc;
            float strength = (g_cfg.fpArmShadow && CompBoneXf(arms, kHandBones[h], hc)) ? g_weaponRaise[h] : 0.0f;
            if (strength > 0.0f) {
                Xf ha = XfMul(hc, copyLocal);
                ik->EffectorLocationSpace = EBoneControlSpace::BCS_ActorSpace;
                ik->EffectorLocation = FVector{ ha.t[0], ha.t[1], ha.t[2] };
                ik->JointTargetLocation = FVector{ g_cfg.armIKElbow.X, h == 0 ? -g_cfg.armIKElbow.Y : g_cfg.armIKElbow.Y, g_cfg.armIKElbow.Z };
            }
            ik->ControlStrength = ik->StrengthTarget = strength;
        }
    }
    g_armsShadow->SetTranslation(t);
    g_armsShadow->SetRotation(arms->Rotation);
    g_armsShadow->SetScale(any ? arms->Scale : 0.0001f);    // Only the arm(s) holding gear.
    static const char* kArmRoots[2] = { "shoulder_L_jnt", "shoulder_R_jnt" };
    bool busy[2] = { g_leftHandBusy, g_rightHandBusy };
    for (int h = 0; h < 2; ++h) {
        int want = busy[h] ? 0 : 1;
        if (want == hidden[h]) continue;
        FName bone(kArmRoots[h]);
        if (bone.FNameEntryId < 0) continue;
        if (want) g_armsShadow->HideBoneByName(bone, EPhysBodyOp::PBO_None);
        else g_armsShadow->UnHideBoneByName(bone);
        hidden[h] = want;
    }
}

// Item shadows: a shadow-only copy of each held item's first-person model, attached to the pawn (so it is always
// referenced) and moved every frame to the real 1P item's transform shifted like the arm shadow (minus the rigid
// camera's arms shift). The arm shadow is the 1P arms with the same shift, so the item sits exactly in its hand.
// (Builds 84-86 attached the copies to hand bones and re-attached them; the detach/attach churn crashed.)
// A bone of the arm-shadow copy in world space. The copy borrows its pose from the 1P arms
// (SetParentAnimComponent) and has no SpaceBases of its own (build 90 log: the shoulder fix never moved and the
// sword never showed), so: the 1P arms' component-space bone x the copy's own LocalToWorld.
static bool ArmsCopyBoneXf(ADishonoredPlayerPawn* pawn, const char* bone, Xf& out) {
    if (!g_armsShadow || !pawn || !pawn->Mesh) return false;
    Xf world1P, inv1P;
    if (!BoneXf(pawn->Mesh, bone, world1P) || !XfInv(XfFromMatrix(pawn->Mesh->LocalToWorld), inv1P)) return false;
    out = XfMul(XfMul(world1P, inv1P), g_armsCopyFrame);   // the frame we just gave the copy
    return true;
}

static void UpdateItemShadows(ADishonoredPlayerPawn* pawn) {
    static USkeletalMesh* lastMesh[2] = { nullptr, nullptr };
    for (int h = 0; h < 2; ++h) {
        USkeletalMeshComponent*& copy = g_itemShadow[h];
        auto* item = static_cast<UDishonoredInventoryItem*>(g_handItem[h]);
        USkeletalMeshComponent* src = item ? item->m_pPlayerMesh : nullptr;
        bool want = g_cfg.itemShadows && g_cfg.fpArmShadow && g_armsShadow && pawn && pawn->Mesh && g_bodyVisible &&
                    src && src->SkeletalMesh && (h ? g_rightHandBusy : g_leftHandBusy);
        if (!want) {
            if (copy) copy->SetScale(0.0001f);   // keep it (attached to the pawn), just cast nothing
            continue;
        }
        if (!copy) {
            copy = static_cast<USkeletalMeshComponent*>(
                StaticConstructObject(USkeletalMeshComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
            if (!copy) continue;
            copy->bUpdateSkelWhenNotRendered = 1;
            if (pawn->Mesh->LightEnvironment) copy->SetLightEnvironment(pawn->Mesh->LightEnvironment);
            copy->HiddenGame = 1;
            copy->CastShadow = 1;
            copy->bCastDynamicShadow = 1;
            copy->bCastHiddenShadow = 1;
            pawn->AttachComponent(copy);
            copy->SetHidden(true);
            lastMesh[h] = nullptr;
            Log("item shadow component created for the %s hand", h ? "right" : "left");
        }
        if (lastMesh[h] != src->SkeletalMesh) {
            copy->SetSkeletalMesh(src->SkeletalMesh, false);
            // The item's own skeleton is animated (the blade's place comes from it); copy that pose, or the ref pose
            // left the sword shadow floating in the air (video 17-08-39).
            copy->SetParentAnimComponent(src);
            copy->UpdateParentBoneMap();
            lastMesh[h] = src->SkeletalMesh;
            Log("item shadow (%s hand): %s", h ? "right" : "left", src->SkeletalMesh->GetName().c_str());
        }
        // Where the game attached the item on the 1P arms (bone + relative transform, from pawn->Mesh->Attachments),
        // applied to the same bone of the arm copy - all in actor space (copy: Translation g_armsShadowT, the 1P
        // Rotation/Scale). No LocalToWorld (they don't match where things render).
        struct RawAttachment { UActorComponent* Component; int32_t boneIndex, boneNumber; FVector RelLoc; FRotator RelRot; FVector RelScale; };
        static_assert(sizeof(RawAttachment) == 0x30, "FAttachment is 0x30 bytes");
        auto* att = reinterpret_cast<RawArray*>(&pawn->Mesh->Attachments);
        const RawAttachment* found = nullptr;
        for (int i = 0; att->Data && i < att->Num; ++i) {
            auto* e = static_cast<const RawAttachment*>(att->Data) + i;
            if (e->Component == src) { found = e; break; }
        }
        static int loggedMissing[2] = { 0, 0 };
        if (!found) {
            if (!loggedMissing[h]) { loggedMissing[h] = 1; Log("item shadow (%s hand): item not in the 1P arms' Attachments", h ? "right" : "left"); }
            copy->SetScale(0.0001f);
            continue;
        }
        static int loggedFound[2] = { 0, 0 };
        if (!loggedFound[h]) {
            loggedFound[h] = 1;
            FName bn; bn.FNameEntryId = found->boneIndex; bn.InstanceNumber = found->boneNumber;
            Log("item shadow (%s hand): attached to %s at (%.1f %.1f %.1f) rot (%d %d %d)", h ? "right" : "left", bn.ToString().c_str(),
                found->RelLoc.X, found->RelLoc.Y, found->RelLoc.Z, found->RelRot.Pitch, found->RelRot.Yaw, found->RelRot.Roll);
        }
        FName boneName; boneName.FNameEntryId = found->boneIndex; boneName.InstanceNumber = found->boneNumber;
        int idx = pawn->Mesh->MatchRefBone(boneName);
        auto* bases = reinterpret_cast<RawArray*>(&pawn->Mesh->SpaceBases);
        if (idx < 0 || !bases->Data || idx >= bases->Num) { copy->SetScale(0.0001f); continue; }
        Xf boneComp;
        {
            const char* nm = nullptr;   // reuse CompBoneXf by index: build the atom directly
            (void)nm;
            Xf world, invL;
            const float* a = reinterpret_cast<const float*>(static_cast<uint8_t*>(bases->Data) + idx * 32);
            float x = a[0], y = a[1], z = a[2], wq = a[3], s = (a[7] > 0.0f && a[7] < 100.0f) ? a[7] : 1.0f;
            float x2 = x + x, y2 = y + y, z2 = z + z, xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
            float wx = wq * x2, wy = wq * y2, wz = wq * z2;
            Xf atom;
            atom.m[0][0] = (1 - (yy + zz)) * s; atom.m[0][1] = (xy + wz) * s; atom.m[0][2] = (xz - wy) * s;
            atom.m[1][0] = (xy - wz) * s; atom.m[1][1] = (1 - (xx + zz)) * s; atom.m[1][2] = (yz + wx) * s;
            atom.m[2][0] = (xz + wy) * s; atom.m[2][1] = (yz - wx) * s; atom.m[2][2] = (1 - (xx + yy)) * s;
            atom.t[0] = a[4]; atom.t[1] = a[5]; atom.t[2] = a[6];
            boneComp = XfMul(atom, XfFromRotator(pawn->Mesh->SkeletalMesh->RotOrigin, pawn->Mesh->SkeletalMesh->Origin));
        }
        Xf rel = XfFromRotator(found->RelRot, found->RelLoc);
        float rs3[3] = { found->RelScale.X, found->RelScale.Y, found->RelScale.Z };
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) rel.m[i][j] *= (rs3[i] > 0.0f ? rs3[i] : 1.0f);
        Xf copyLocal = XfFromRotator(pawn->Mesh->Rotation, g_armsShadowT);   // copy component -> actor
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) copyLocal.m[i][j] *= pawn->Mesh->Scale;
        Xf itemActor = XfMul(XfMul(rel, boneComp), copyLocal);
        FVector loc; FRotator rot; float scale;
        XfDecompose(itemActor, loc, rot, scale);
        if (!(scale > 0.01f && scale < 100.0f) || !(fabsf(loc.X) < 500.0f && fabsf(loc.Y) < 500.0f && fabsf(loc.Z) < 500.0f)) {
            copy->SetScale(0.0001f);
            continue;
        }        copy->SetTranslation(loc);
        copy->SetRotation(rot);
        copy->SetScale(scale);
    }
}
// View guard (build 184): strafing put a shoulder/collar piece of Daud's body in the bottom corners for ~0.2 s, and
// landings put the shrunk chest/neck (their collapsed, stretched polygons) under a camera looking down (video
// 16-43-24). Each frame, test the visible body's upper-body bones against the camera's view (with a margin for the
// geometry around them); if any is in view, ease the body back just far enough to clear them all. The push is the
// smallest one (searching from "none") that keeps them out, so it lets go by itself once they're clear.
static float NeededViewGuardBack(ADishonoredPlayerPawn* pawn, bool* logIt) {
    struct GuardBone { const char* name; float margin; bool armsOnly; };
    static const GuardBone kBones[] = {
        { "neck_jnt", 8.0f, false }, { "Torso_jnt", 8.0f, false }, { "spine_3_jnt", 8.0f, false },
        { "shoulder_L_jnt", 10.0f, false }, { "shoulder_R_jnt", 10.0f, false },
        // The arms hang in view when looking down (wanted) and float up mid-jump (JumpArms): only while looking ahead.
        { "lower_arm_L_jnt", 6.0f, true }, { "lower_arm_R_jnt", 6.0f, true }, { "hand_L_jnt", 6.0f, true }, { "hand_R_jnt", 6.0f, true } };
    constexpr float kRad = 6.2831853f / 65536.0f;
    float yaw = g_viewRot.Yaw * kRad;
    int ip = g_viewRot.Pitch & 0xFFFF;
    float pitch = (ip > 32768 ? ip - 65536 : ip) * kRad;
    FVector F{ cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch) };
    FVector R{ -sinf(yaw), cosf(yaw), 0.0f };
    FVector U{ -sinf(pitch) * cosf(yaw), -sinf(pitch) * sinf(yaw), cosf(pitch) };
    float tanH = tanf(Clamp(g_viewFov, 40.0f, 140.0f) * 0.5f * 0.0174533f);
    float tanV = tanH * 9.0f / 16.0f;
    float pyaw = pawn->Rotation.Yaw * kRad;
    FVector back{ -cosf(pyaw), -sinf(pyaw), 0.0f };
    bool armsToo = pitch * 57.29578f > -25.0f && g_jumpAmount <= 0.0f;
    FVector pts[9];
    float margins[9];
    const char* names[9];
    int n = 0;
    for (const GuardBone& b : kBones) {
        if (b.armsOnly && !armsToo) continue;
        FName name(b.name);
        if (name.FNameEntryId < 0 || g_body->MatchRefBone(name) < 0) continue;
        FVector p = g_body->GetBoneLocation(name, 0);
        pts[n] = { p.X - g_viewLoc.X, p.Y - g_viewLoc.Y, p.Z - g_viewLoc.Z };
        margins[n] = b.margin;
        names[n] = b.name;
        ++n;
    }
    // Which bone (if any) is in view after moving the body s uu further back.
    auto firstInView = [&](float s, float* cx, float* cy, float* cz) {
        for (int i = 0; i < n; ++i) {
            FVector d{ pts[i].X + back.X * s, pts[i].Y + back.Y * s, pts[i].Z };
            float x = d.X * F.X + d.Y * F.Y + d.Z * F.Z;
            if (x <= 1.0f) continue;
            float y = d.X * R.X + d.Y * R.Y + d.Z * R.Z, z = d.X * U.X + d.Y * U.Y + d.Z * U.Z;
            if (fabsf(y) < x * tanH + margins[i] && fabsf(z) < x * tanV + margins[i]) {
                if (cx) { *cx = x; *cy = y; *cz = z; }
                return i;
            }
        }
        return -1;
    };
    constexpr float kMaxBack = 40.0f;
    float cx = 0, cy = 0, cz = 0;
    int now = firstInView(0.0f, &cx, &cy, &cz);
    float s = -g_viewGuardBack;
    while (s < kMaxBack - g_viewGuardBack && firstInView(s, nullptr, nullptr, nullptr) >= 0) s += 1.0f;
    float want = Clamp(g_viewGuardBack + s, 0.0f, kMaxBack);
    if (logIt && now >= 0) {
        *logIt = true;
        FVector v = pawn->Velocity;
        Log("view guard: %s in view (cam fwd %.0f right %.0f up %.0f) pitch %.0f | vel fwd %.0f right %.0f phys %d | jumpW %.2f landDip %.0f lean %.0f stairDz %.0f | back %.0f -> %.0f",
            names[now], cx, cy, cz, pitch * 57.29578f, -(v.X * back.X + v.Y * back.Y), v.X * back.Y - v.Y * back.X,
            static_cast<int>(pawn->Physics), g_jumpBodyW, g_landDip, g_cornerLeanRight, g_stairDz, g_viewGuardBack, want);
    }
    return want;
}

static void UpdateViewGuard(ADishonoredPlayerPawn* pawn, bool firstPerson) {
    static DWORD last = 0, lastLog = 0;
    DWORD now = GetTickCount();
    float dt = last ? Clamp((now - last) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    last = now;
    int phys = static_cast<int>(pawn->Physics);
    bool active = g_cfg.viewGuard && firstPerson && g_body && g_bodyAnimated && now - g_viewLocTick < 500 &&
                  (phys == 1 || phys == 2) && !g_isSliding && g_slideAmount <= 0.0f && !g_isClimbing && !g_mantleWant &&
                  g_swimAmount <= 0.0f;
    float want = 0.0f;
    if (active) {
        bool logged = false;
        bool mayLog = now - lastLog > 250;
        want = NeededViewGuardBack(pawn, mayLog ? &logged : nullptr);
        if (logged) lastLog = now;
    }
    // In fast (it's a few frames' problem), out slowly.
    g_viewGuardBack += Clamp(want - g_viewGuardBack, -dt * 30.0f, dt * 400.0f);
}

// Cutscenes / third-person shots: the body (which has no head) must only show in the first-person view.
// Hide it when the controller is in cinematic mode, the camera is looking through something other than the
// player, or the game has hidden the first-person arms or the pawn.
static void UpdateVisibility(ADishonoredPlayerPawn* pawn) {
    static int shown = -1;
    static USkeletalMeshComponent* lastComp = nullptr;
    if (!g_body || !pawn) return;
    if (g_body != lastComp) { lastComp = g_body; shown = -1; }

    bool firstPerson = true;
    const char* why = "";
    auto* pc = static_cast<APlayerController*>(pawn->Controller);
    if (pc && pc->bCinematicMode) { firstPerson = false; why = "cinematic mode"; }
    else if (pc && pc->PlayerCamera && pc->PlayerCamera->ViewTarget.Target &&
             pc->PlayerCamera->ViewTarget.Target != pawn && pc->PlayerCamera->ViewTarget.Target != pc) {
        firstPerson = false; why = "camera view target is not the player";
    }
    else if (pawn->bHidden) { firstPerson = false; why = "pawn hidden"; }
    else if (pc && pc->PlayerCamera) {
        // Some cutscenes (the Empress intro) keep the player as view target but move the camera away.
        // Measured from the game's own camera (without the rigid camera's arms shift, which the view rides) and the
        // current eye height (crouch/slide lower it): both pushed the distance past the limit and the body
        // flickered off while sprinting and sliding (video 14-14-37).
        FVector cam = pc->PlayerCamera->CameraCache.POV.Location;
        if (g_camFollowsArms == 1) { cam.X -= g_armsShiftWorld.X; cam.Y -= g_armsShiftWorld.Y; cam.Z -= g_armsShiftWorld.Z; }
        float dx = cam.X - pawn->Location.X, dy = cam.Y - pawn->Location.Y;
        float dz = cam.Z - (pawn->Location.Z + pawn->EyeHeight);
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        static DWORD lastLog = 0;
        if (GetTickCount() - lastLog > 15000) { lastLog = GetTickCount(); Log("camera is %.0f uu from the eyes", dist); LogPitchCurve(); }
        // Only when it stays away (cutscenes): sprint/slide/landing spikes flickered the body (build 63 log).
        static DWORD awaySince = 0;
        DWORD nowT = GetTickCount();
        if (dist > g_cfg.thirdPersonDist) {
            if (!awaySince) {
                awaySince = nowT;
                Log("camera %.0f uu from the eyes (fwd/right/up %.0f %.0f %.0f, arms shift %.0f %.0f %.0f, phys %d)", dist,
                    dx, dy, dz, g_armsShiftWorld.X, g_armsShiftWorld.Y, g_armsShiftWorld.Z, static_cast<int>(pawn->Physics));
            }
            if (nowT - awaySince > 400) { firstPerson = false; why = "camera away from the player's eyes"; }
        } else {
            awaySince = 0;
        }
    }
    else if (pawn->Mesh && pawn->Mesh->HiddenGame) { firstPerson = false; why = "first-person arms hidden"; }
    // Slide just ended: the game stands the pawn up in one frame while the body eases out of the slide, and the
    // camera passed through the hips for a frame (video 18-16-03). Hide the body during that moment.
    // (Hiding it blinked the legs off for a moment - user. SyncTransform now moves the body down/back instead.)
    // Mantling/climbing over things: the game pulls the camera back and up while the body stays, so the body was
    // seen from behind (video 18-18-39). Hide it while the camera sits well behind/above where looking puts it.
    g_mantleWant = false;
    // The game's own player state machine says when a mantle starts (UStatePlayerMasterMantle) - set before the
    // camera moves, so no frame of lag (the camera-offset test below lags 1-2 frames: a foot showed, video 22-20-25).
    if (firstPerson && g_cfg.mantleHide && pawn->m_pPlayerMasterFSM && pawn->m_pPlayerMasterFSM->m_pCurrentState) {
        UObject* st = pawn->m_pPlayerMasterFSM->m_pCurrentState;
        static UObject* lastSt = nullptr;
        if (st != lastSt) { lastSt = st; Log("player state: %s", st->Class ? st->Class->GetName().c_str() : "?"); }
        static UClass* mantleCls = nullptr;
        if (!mantleCls) mantleCls = UStatePlayerMasterMantle::StaticClass();
        if (mantleCls && st->IsA(mantleCls)) g_mantleWant = true;
    }
    // Quick mantles never enter the mantle state (log 22:27: Jump -> Falling -> Walk); the camera's step-up / crouch
    // mantle influences drive the pull-back. Use them (same frame as the camera update) and log what they do.
    if (firstPerson && g_cfg.mantleHide && pc && pc->PlayerCamera) {
        auto* cam = reinterpret_cast<ADishonoredPlayerCamera*>(pc->PlayerCamera);
        if (cam->IsA(ADishonoredPlayerCamera::StaticClass())) {
            UDishonoredCameraInfluence* infl[2] = { cam->m_pStepUpMantleOffset_Influence, cam->m_pCrouchMantleOffset_Influence };
            static bool lastOn[2] = { false, false };
            for (int k = 0; k < 2; ++k) {
                if (!infl[k]) continue;
                bool on = infl[k]->m_bActive || infl[k]->m_Weight > 0.01f;
                if (on != lastOn[k]) {
                    Log("camera %s influence %s (weight %.2f target %.2f)", k ? "crouch-mantle" : "step-up-mantle", on ? "on" : "off",
                        infl[k]->m_Weight, infl[k]->m_TargetWeight);
                    lastOn[k] = on;
                }
                if (on && g_cfg.mantleInfluenceHide) g_mantleWant = true;
            }
        }
    }
    if (firstPerson && g_cfg.mantleHide) {
        static bool mantling = false;
        // Jumping again right after landing: the game's camera is still in its landing dip, so the height test
        // below read "camera far below in the air" = mantle and hid the body for the whole jump (video 14-31-02,
        // every jump within ~0.1 s of landing). Skip the height test for half a second after landing; real mantles
        // are still caught by the mantle state, the mantle camera influences and the pulled-back test.
        static int prevPhys = 1;
        static DWORD landTick = 0;
        int phys = static_cast<int>(pawn->Physics);
        if (phys == 1 && prevPhys == 2) landTick = GetTickCount();
        // Crouch jumps start with the camera ~30 below the standing curve (crouched eye height) - that alone read as
        // a mantle and hid the legs (video 15-34-12, offset -33). Measure the height change since leaving the ground.
        static float takeoffDz = 0.0f;
        if (phys != 1 && prevPhys == 1) takeoffDz = g_stairDz < 0.0f ? g_stairDz : 0.0f;
        if (phys == 1) takeoffDz = 0.0f;
        float airDz = g_stairDz - takeoffDz;
        prevPhys = phys;
        bool justLanded = landTick && GetTickCount() - landTick < 500;
        float back = -g_cornerLeanFwd;   // + = camera behind its usual spot
        bool off = static_cast<int>(pawn->Physics) != 1 || back > 0.0f;
        // Now only as a last resort (SyncTransform moves the body back with the camera instead - hiding blinked).
        bool on = back > (mantling ? 15.0f : g_cfg.mantleHideBack) ||
                  // Upward: absolute. Relative to take-off it fired at the top of a jump made right after landing, when
                  // the camera came back up out of the landing dip (video 15-37-23: '+46 since take-off', +6 absolute).
                  (!justLanded && static_cast<int>(pawn->Physics) != 1 && g_stairDz > g_cfg.mantleHideUp);
        // Also when the camera drops far below (log: ~110 down while the pawn climbs), and once started, held until
        // landed - "mantle end" fired mid-air and the body grew back while the camera was still low (video 22-17-32).
        if (!justLanded && static_cast<int>(pawn->Physics) != 1 && airDz < -g_cfg.mantleHideUp) on = true;
        if (on && !mantling) Log("mantle test: back %.0f, camera height offset %.0f (%.0f since take-off)%s", back, g_stairDz, airDz, justLanded ? ", just landed" : "");
        mantling = (off && on) || (mantling && static_cast<int>(pawn->Physics) != 1);
        // Not for slides (physics 14): the slide start pulls the camera too, and holding until "landed" hid the
        // legs for the whole slide (video 22-33-19).
        if (g_isSliding || static_cast<int>(pawn->Physics) == g_cfg.slidePhysics || g_isClimbing) mantling = false;
        if (g_hitActive) mantling = false;   // a hit's camera kick pulls the view back too (log 20:44: shrank and regrew the body)
        // Shrink the body away smoothly instead of switching it off (the on/off hide blinked - video 21-55-50);
        // SyncTransform applies g_mantleAmount, growing it back at ReappearSpeed like the arms and coat.
        g_mantleWant = g_mantleWant || mantling;
        static bool loggedM = false;
        if (mantling != loggedM) { Log("mantle %s", mantling ? "start: shrinking body" : "end"); loggedM = mantling; }
    }

    // Take-off frames: a mantle throws the camera ~60 back and ~110 down at once, and the follow reads that a frame
    // late; hide the body for the first two frames in the air so that frame isn't seen (2 frames: not noticeable).
    {
        static int lastPhys = 1, airFrames = 0;
        int phys = static_cast<int>(pawn->Physics);
        if (phys == 2 && lastPhys == 1 && !g_hitActive) airFrames = 3;   // not a knock-back hop
        lastPhys = phys;
        if (airFrames > 0) {
            --airFrames;
            if (firstPerson && g_cfg.takeoffHide) { firstPerson = false; why = "take-off"; }
        }
    }
    UpdateViewGuard(pawn, firstPerson);
    // Clip guard: if the camera gets inside the body (hips/thighs/knees near the view point), first push the visible
    // body back/down away from the camera (SyncTransform applies g_clipPush) so the legs stay in view; only if that
    // is not enough, hide it (held 0.15 s). Hiding alone made the legs vanish out of a slide (user).
    if (firstPerson && g_cfg.clipGuardDist > 0.0f && GetTickCount() - g_viewLocTick < 500) {
        static const char* kGuard[7] = { "spine_0_jnt", "spine_1_jnt", "upper_leg_L_jnt", "upper_leg_R_jnt", "lower_leg_L_jnt", "lower_leg_R_jnt", "Root_jnt" };
        static DWORD clipUntil = 0, lastT = 0;
        DWORD nowG = GetTickCount();
        float dtG = lastT ? Clamp((nowG - lastT) / 1000.0f, 0.0f, 0.1f) : 0.0f;
        lastT = nowG;
        float best = 1e9f;
        const char* bestName = "";
        float bdx = 0, bdy = 0, bdz = 0;
        for (const char* b : kGuard) {
            if (g_torsoAmount > 0.5f && b == kGuard[1]) continue;   // spine_1 is not drawn while the torso is hidden
            FName n(b);
            if (n.FNameEntryId < 0) continue;
            FVector p = g_body->GetBoneLocation(n, 0);
            float dx = p.X - g_viewLoc.X, dy = p.Y - g_viewLoc.Y, dz = p.Z - g_viewLoc.Z;
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < best) { best = d; bestName = b; bdx = dx; bdy = dy; bdz = dz; }
        }
        // Push along the camera->bone direction (in player space), so the closest part moves straight away from the
        // view; Root_jnt is the pelvis and sat 3-5 uu from the camera right after a slide (log 20:59).
        if (best < 1e8f) {
            float len = sqrtf(bdx * bdx + bdy * bdy + bdz * bdz);
            if (len > 0.5f) {
                float yaw = (g_bodyPawn ? g_bodyPawn->Rotation.Yaw : 0) * (6.2831853f / 65536.0f);
                float cy = cosf(yaw), sy = sinf(yaw);
                float fx = (bdx * cy + bdy * sy) / len, fy = (-bdx * sy + bdy * cy) / len, fz = bdz / len;
                // Never push forward or up into view more than a little; mostly back/down/sideways.
                if (fx > 0.3f) fx = 0.3f;
                if (fz > 0.0f) fz = 0.0f;
                float n = sqrtf(fx * fx + fy * fy + fz * fz);
                // Turn toward the new direction instead of jumping to it: getting up out of a slide the closest bone
                // changed every frame and the push swung the body around (log 15:54:08, video 15-53-52).
                if (n > 0.01f) {
                    float k = Clamp(dtG * 7.0f, 0.0f, 1.0f);
                    float nx = g_clipDir.X + (fx / n - g_clipDir.X) * k, ny = g_clipDir.Y + (fy / n - g_clipDir.Y) * k,
                          nz = g_clipDir.Z + (fz / n - g_clipDir.Z) * k;
                    float m = sqrtf(nx * nx + ny * ny + nz * nz);
                    if (m > 0.01f) g_clipDir = FVector{ nx / m, ny / m, nz / m };
                }
            }
        }
        // Not while sliding (jittered looking down), nor with the legs folded into the hips (all leg bones sit at the
        // pelvis then; pushing would leave the legs displaced as they grow back).
        float deficit = (g_isSliding || g_legHideAmount > 0.0f) ? -1.0f : g_cfg.clipGuardDist + 4.0f - best;
        // Grow at most 200 uu/s (the whole deficit in one frame made the push jump 0 -> 58 within 80 ms).
        if (deficit > 0.0f) g_clipPush = Clamp(g_clipPush + (deficit < dtG * 200.0f ? deficit : dtG * 200.0f), 0.0f, g_cfg.clipPushMax);
        else g_clipPush = Clamp(g_clipPush - dtG * 60.0f, 0.0f, g_cfg.clipPushMax);   // ease back once clear
        // Hide only if it is still too close after the push has had a few frames to take effect.
        static int atMax = 0;
        atMax = (best < g_cfg.clipGuardDist && g_clipPush >= g_cfg.clipPushMax - 0.5f) ? atMax + 1 : 0;
        if (atMax >= 4) {
            if (nowG >= clipUntil) Log("clip guard: %s %.0f uu from the camera, pushed %.0f - hiding", bestName, best, g_clipPush);
            clipUntil = nowG + 150;
        }
        if (nowG < clipUntil) { firstPerson = false; why = "camera inside the body"; }
    }    int want = firstPerson ? 1 : 0;
    if (want == shown) return;
    g_body->SetHidden(!firstPerson);
    // The head is always hidden but casts a shadow; take it off the pawn entirely during cutscenes.
    if (g_shadowBody) { if (firstPerson) pawn->AttachComponent(g_shadowBody); else pawn->DetachComponent(g_shadowBody); }
    if (g_head) { if (firstPerson) pawn->AttachComponent(g_head); else pawn->DetachComponent(g_head); }
    g_bodyVisible = firstPerson;
    if (strcmp(why, "take-off") != 0 && !(firstPerson && g_lastHideWhy && strcmp(g_lastHideWhy, "take-off") == 0))
        Log("body %s%s%s", firstPerson ? "shown" : "hidden (", why, firstPerson ? "" : ")");
    g_lastHideWhy = firstPerson ? nullptr : why;
    shown = want;
}

// Per frame: lift both feet by the crouch sink (standing cylinder - current), eased.
// g_crouchSink (set by UpdatePosture) is declared with the other shared state near the top.

// Planted feet: every frame, measure how high the body's root bone sits above the floor (bottom of the
// collision cylinder) and compare with the standing height. Lift the feet by exactly the difference, with no
// easing, so however the game lowers the body (cylinder shrink, camera/mesh offsets) the feet stay on the floor
// and the knees absorb it. Replaces the old cylinder-delta + FootLiftExtra guess.
static void UpdateFootIK(ADishonoredPlayerPawn* pawn) {
    static float standingGap = 0.0f;
    static bool learned = false;   // the gap is negative (root ~6.5 below the cylinder bottom), so track it separately
    static USkeletalMeshComponent* lastComp = nullptr;
    if (!g_body || !g_bodyAnimated || !g_cfg.footIK || !pawn || !pawn->CylinderComponent) return;
    if (g_body != lastComp) { lastComp = g_body; learned = false; }
    FName root("root0_jnt");
    if (root.FNameEntryId < 0) return;
    FVector rootWorld = g_body->GetBoneLocation(root, 0);
    float floorZ = pawn->Location.Z - pawn->CylinderComponent->CollisionHeight;
    float gap = rootWorld.Z - floorZ;
    // Learn it only while standing AND settled: during the stand-up catch-up the body is still offset down, and
    // learning then made lift 0 and the feet clip the floor (test video 10-55-42).
    if (!g_isCrouched && g_crouchSink < 1.0f && fabsf(g_smoothedSink) < 0.5f && fabsf(g_cameraDip) < 1.0f && g_landDip > -0.5f && g_jumpAmount <= 0.0f) { standingGap = gap; learned = true; }
    float extra = (g_isCrouched || g_crouchSink > 1.0f) ? g_cfg.footLiftExtra : 0.0f;   // standing: no extra (it bent the knees)
    float lift = learned ? Clamp(standingGap - gap + extra, 0.0f, g_cfg.footMaxUp) : 0.0f;
    // Ignore tiny differences (straight legs standing), but without a step: fade from 2 to 6 uu.
    // Small dead zone only (standing noise); the old 2..6 ramp under-lifted, so the feet sank while standing up.
    lift = lift < 1.5f ? 0.0f : lift;
    // Crouched jumps: in the air the gap to the cylinder bottom is meaningless (lift read 47-74 vs 39 crouched) and
    // the instant lift made a crouched landing's legs jitter (video 15-43-55). While crouched and airborne, hold the
    // lift from take-off; after landing ease to the measured one (~0.1 s, max 150 uu/s) until it catches up.
    // Standing jumps keep the original instant lift (easing it there left a squat after every landing, 15-48-44).
    {
        static float smoothLift = 0.0f, groundLift = 0.0f;
        static DWORD lastT = 0, landT = 0;
        static int lastPhys = 1;
        static bool easing = false;
        DWORD nowT = GetTickCount();
        float dt = lastT ? Clamp((nowT - lastT) / 1000.0f, 0.0f, 0.1f) : 0.0f;
        lastT = nowT;
        int phys = static_cast<int>(pawn->Physics);
        if (phys == 1 && lastPhys == 2) landT = nowT;
        lastPhys = phys;
        bool crouchy = g_isCrouched || g_crouchSink > 1.0f;
        if (phys == 1 && !easing) groundLift = lift;                       // last settled ground value
        if (crouchy && phys == 2) {
            lift = smoothLift = groundLift;                                // hold the crouch through the jump
            easing = true;
        } else if (crouchy && (easing || (landT && nowT - landT < 400))) {
            float k = Clamp(dt * 10.0f, 0.0f, 1.0f);
            smoothLift += Clamp((lift - smoothLift) * k, -dt * 150.0f, dt * 150.0f);
            easing = (landT && nowT - landT < 400) || fabsf(lift - smoothLift) > 1.0f;
            lift = smoothLift;
        } else {
            easing = false;
            smoothLift = lift;
        }
    }    static int logged = 0;
    if (g_isCrouched && logged < 3) { ++logged; Log("planted feet: standing gap %.1f, now %.1f, lift %.1f", standingGap, gap, lift); }
    static DWORD lastLiftLog = 0;
    if (g_isCrouched && GetTickCount() - lastLiftLog > 2000) {
        lastLiftLog = GetTickCount();
        Log("crouched: lift %.1f, cylinder %.1f, sink %.1f", lift, pawn->CylinderComponent->CollisionHeight, g_crouchSink);
    }
    g_footLift = lift;
    static const char* feet[2] = { "foot_L_jnt", "foot_R_jnt" };
    USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };   // same lift for the shadow body
    for (USkeletalMeshComponent* comp : comps)
    for (const char* foot : feet) {
        if (!comp) break;
        FName name(foot);
        auto* ik = static_cast<USkelControlLimb*>(comp->FindSkelControl(name));
        if (!ik) continue;
        ik->EffectorLocation = FVector{ g_cfg.footLiftSign * lift, 0, 0 };
        // Knee direction from the ini every frame, so F6 tuning applies live. Left foot is the first entry.
        bool left = foot == feet[0];
        // Deeper sinks (after a slide the player stays lower than a normal crouch) splayed the knees very wide
        // (user, video 14-21-36): beyond a normal crouch lift, aim the knees more forward than sideways.
        float kf = Clamp(30.0f / (lift > 1.0f ? lift : 1.0f), 0.0f, 1.0f);
        float blend = Clamp(lift / (g_cfg.footIKFadeLift > 1.0f ? g_cfg.footIKFadeLift : 1.0f), 0.0f, 1.0f);
        // The knee spread grows with the crouch (no snap into a wide pole plane); the IK itself is always at full
        // strength so the feet stay planted (fading the strength let them sink while standing up - video 22-38-12).
        float side = g_cfg.kneeSide * Clamp(kf * kf, 0.25f, 1.0f) * blend;
        ik->JointTargetLocation = FVector{ g_cfg.kneeForward, left ? -side : side, g_cfg.kneeHeight };
        // Fade the IK in with the lift: switched fully on/off it swung each leg into the knee pole's plane in one
        // frame (with a wide KneeTargetSide the legs snapped out entering a crouch and back leaving it - user).
        float fade = Clamp(lift / (g_cfg.footIKFadeLift > 1.0f ? g_cfg.footIKFadeLift : 1.0f), 0.0f, 1.0f);
        float strength = (lift > 0.0f && !g_isSliding && !g_isClimbing) ? (1.0f - g_slideLegW) * (1.0f - g_swimAmount) : 0.0f;   // crossfades with the slide leg pose (one motion)   // slide/climb/swim poses own the legs; fades back in after
        // Legs folded into the hips (low crouch, visible body only): no IK on near-zero-length legs, back in as they grow out.
        if (comp == g_body) strength *= 1.0f - g_legHideAmount;
        strength *= 1.0f - g_hoverAmount;   // floating (Transversal): nothing under the feet to plant them on
        ik->ControlStrength = strength;
        ik->StrengthTarget = strength;
    }
}

static void SetLegRotation(USkelControlSingleBone* ctl, float degrees, float strength) {
    int units = static_cast<int>(degrees * 65536.0f / 360.0f);
    FRotator r{ 0, 0, 0 };
    if (g_cfg.crouchLegAxis == 0) r.Pitch = units; else if (g_cfg.crouchLegAxis == 1) r.Yaw = units; else r.Roll = units;
    ctl->BoneRotation = r;
    ctl->ControlStrength = strength;
    ctl->StrengthTarget = strength;
}

// Slide: no character in Dishonored has a slide animation, so pose the legs with the hip/knee SingleBone controls
// (bone-space pitch: Corvo's leg bones hinge on local Y). Front (left) leg out and nearly straight, back (right)
// leg folded under. Eased in/out; foot IK is off while sliding.
static UAnimNode* ChildAnim(UAnimNode* node, int i);
static void UpdateSlide() {
    static DWORD last = 0;
    static float amount = 0.0f;
    DWORD now = GetTickCount();
    float dt = last ? Clamp((now - last) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    last = now;
    // Out over ~0.4 s (in at 6/s): ending in 0.17 s snapped legs, feet and camera at once (user: janky).
    amount += Clamp((g_isSliding ? 1.0f : 0.0f) - amount, -dt * 2.5f, dt * 6.0f);
    g_slideAmount = amount;
    static int logged = -1;
    if ((g_isSliding ? 1 : 0) != logged) {
        Log("slide %s", g_isSliding ? "started" : "ended");
        // The clip guard reacts a frame late (it measures the last drawn pose), so the first frame after the slide
        // still clipped (video 20-38-29): start the push-away as the slide ends; it eases back by itself.
        if (logged == 1 && !g_isSliding && g_clipPush < g_cfg.slideEndPush) g_clipPush = g_cfg.slideEndPush;
        logged = g_isSliding ? 1 : 0;
    }
    // While sliding the legs hold still: the speed blend would pick the walk/run cycle from the slide's speed and the
    // pose was layered on stepping legs (video 17-30-00). Raise its walk threshold so it plays the idle instead.
    {
        static USkeletalMeshComponent* setFor[2] = { nullptr, nullptr };
        static int state[2] = { -1, -1 };
        USkeletalMeshComponent* cs[2] = { g_body, g_shadowBody };
        for (int ci = 0; ci < 2; ++ci) {
            if (!cs[ci]) continue;
            if (setFor[ci] != cs[ci]) { setFor[ci] = cs[ci]; state[ci] = -1; }
            int want = (g_isSliding || g_slideLegW > 0.15f || g_swimAmount > 0.0f || g_climbAmount > 0.0f) ? 1 : 0;   // legs still until the pose has mostly faded; swimming paddles instead
            if (want == state[ci]) continue;
            auto* speedNode = ChildAnim(ChildAnim(cs[ci]->Animations, 0), 0);
            if (!speedNode) continue;
            auto* c = reinterpret_cast<RawArray*>(&static_cast<UAnimNodeBlendBySpeed*>(speedNode)->Constraints);
            if (c->Data && c->Num >= 3) static_cast<float*>(c->Data)[1] = want ? 1000000.0f : 30.0f;
            state[ci] = want;
        }
    }
    static bool wrote = false;
    if (amount <= 0.0f && !g_isSliding && g_slideLegW <= 0.0f) {   // hand the leg controls back cleanly (to the crouch/jump code)
        if (wrote) {
            USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
            for (USkeletalMeshComponent* comp : comps) {
                if (!comp) continue;
                for (int i = 0; i < 4; ++i)
                    if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kLegBones[i])))) SetLegRotation(ctl, 0.0f, 0.0f);
            }
            wrote = false;
        }
        return;
    }
    wrote = true;
    // kLegBones = { upper L, upper R, lower L, lower R }. Front (extended) leg = right, back (folded under) = left (user).
    float values[4] = { g_cfg.slideBackHipDeg, g_cfg.slideHipDeg, g_cfg.slideBackKneeDeg, g_cfg.slideKneeDeg };
    // Leaving the slide the straight right leg swung up through the camera as its hip eased back (video 18-08-43):
    // bend that knee while fading out so the leg folds down into the crouch.
    // Leaving the slide the legs fold out of the slide pose quickly (thighs pointing at the camera while the body
    // rose clipped - video 19-47-13); the body height, camera and hiding still ease out on the slower amount.
    // Get-up motion (user: "one fluid motion"): over SlideGetUpTime the legs are drawn in - the straight right leg
    // pulls its knee up and in, the folded left leg comes forward under the body - then the pose hands over to the
    // crouch legs (foot IK) over the second half.
    static float getUp = 1.0f;   // 0..1 progress after the slide ends (1 = done)
    if (g_isSliding) getUp = 0.0f;
    else if (getUp < 1.0f) getUp = Clamp(getUp + dt / (g_cfg.slideGetUpTime > 0.05f ? g_cfg.slideGetUpTime : 0.05f), 0.0f, 1.0f);
    float legW;
    if (g_isSliding) {
        legW = amount;
    } else {
        // Straight crossfade from the slide pose into the crouch legs (smoothstep). The tucked mid pose made the legs
        // rise up before settling (user); SlideGetUpTuck > 0 brings it back partly.
        float tuck = Clamp(g_cfg.slideGetUpTuck, 0.0f, 1.0f);
        if (tuck > 0.0f) {
            float a = Clamp(getUp / 0.5f, 0.0f, 1.0f);
            a = a * a * (3.0f - 2.0f * a) * tuck;
            const float mid[4] = { -35.0f, -50.0f, 100.0f, 95.0f };   // L hip, R hip, L knee, R knee
            for (int i = 0; i < 4; ++i) values[i] += (mid[i] - values[i]) * a;
        }
        float b = getUp;
        legW = 1.0f - b * b * (3.0f - 2.0f * b);
    }
    g_slideLegW = legW;
    USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
    for (USkeletalMeshComponent* comp : comps) {
        if (!comp) continue;
        for (int i = 0; i < 4; ++i) {
            FName name(kLegBones[i]);
            auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(name));
            if (ctl) SetLegRotation(ctl, values[i], legW);
            // Extended right leg: twist it about its own length so the foot turns over to the left instead of
            // pointing up (user sketch). Corvo's leg bones run along X, so the twist is the bone-space roll.
            if (ctl && i == 1 && g_cfg.slideFrontTwist != 0.0f)
                ctl->BoneRotation.Roll = static_cast<int>(g_cfg.slideFrontTwist * 65536.0f / 360.0f);
            // ...and swing it in toward the middle so it points straight ahead (user: it pointed off to the right).
            if (ctl && i == 1 && g_cfg.slideFrontInward != 0.0f)
                ctl->BoneRotation.Yaw = static_cast<int>(g_cfg.slideFrontInward * 65536.0f / 360.0f);
        }
    }
}

// Chain/ladder climbing: no climb animation exists for NPCs, so alternate the legs procedurally (hip and knee
// SingleBone controls, bone-space pitch) at a rate driven by the vertical speed. The upper body (and its arms) is
// already hidden while not walking; the 1P hands do the gripping.
static void UpdateClimb(ADishonoredPlayerPawn* pawn) {
    static DWORD last = 0;
    static float amount = 0.0f, phase = 0.0f, moveBlend = 0.0f;
    static int lastPhys = -1;
    static bool wrote = false;
    if (!pawn) return;
    int phys = static_cast<int>(pawn->Physics);
    if (phys != lastPhys) { Log("physics %d -> %d", lastPhys, phys); lastPhys = phys; }
    g_isClimbing = phys == g_cfg.climbPhysics;
    DWORD now = GetTickCount();
    float dt = last ? Clamp((now - last) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    last = now;
    // Grab/let go eased over ~0.25 s so the legs swing onto the chain instead of snapping.
    amount += Clamp((g_isClimbing ? 1.0f : 0.0f) - amount, -dt * 4.0f, dt * 4.0f);
    g_climbAmount = amount;
    if (amount <= 0.0f && !g_isClimbing) {
        if (wrote) {   // hand the leg controls back cleanly
            USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
            for (USkeletalMeshComponent* comp : comps) {
                if (!comp) continue;
                for (int i = 0; i < 4; ++i)
                    if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kLegBones[i])))) SetLegRotation(ctl, 0.0f, 0.0f);
            }
            wrote = false;
        }
        phase = 0.0f;
        return;
    }
    wrote = true;
    float hL, kL, hR, kR, rL = 0.0f, rR = 0.0f;
    if (g_cfg.climbDangle) {
        // Dangling legs (user: "ragdoll the legs, climbing with the arms only"): simulated, not engine physics (a
        // physics ragdoll on part of this body would be unstable). Each leg is a damped pendulum hanging from the hip:
        // climbing jerks (vertical acceleration) swing it forward/back, turning swings it sideways, the knee trails
        // the thigh; left and right differ slightly so they don't move as one.
        static float swing[2] = { 0, 0 }, swingV[2] = { 0, 0 }, side[2] = { 0, 0 }, sideV[2] = { 0, 0 }, knee[2] = { 0, 0 };
        static float lastVz = 0.0f, lastYaw = 0.0f;
        static bool init = false;
        float vz = pawn->Velocity.Z;
        float yaw = pawn->Rotation.Yaw * (360.0f / 65536.0f);
        if (!init || amount < 0.05f) { lastVz = vz; lastYaw = yaw; init = true; }
        float az = dt > 0.0f ? (vz - lastVz) / dt : 0.0f;
        float dyaw = yaw - lastYaw;
        if (dyaw > 180.0f) dyaw -= 360.0f;
        if (dyaw < -180.0f) dyaw += 360.0f;
        float yawRate = dt > 0.0f ? dyaw / dt : 0.0f;
        lastVz = vz; lastYaw = yaw;
        const float k = g_cfg.dangleStiffness, c = g_cfg.dangleDamping;
        for (int h = 0; h < 2; ++h) {
            float f = h ? 1.0f : 0.85f;   // slightly different per leg
            float kick = -az * 0.02f * g_cfg.dangleSwing * f + vz * 0.004f * g_cfg.dangleSwing;   // pulled up: legs swing forward
            swingV[h] += (-k * f * swing[h] - c * swingV[h] + kick) * dt;
            swing[h] += swingV[h] * dt;
            swing[h] = Clamp(swing[h], -40.0f, 40.0f);
            float sk = -yawRate * 0.05f * g_cfg.dangleSwing * f;
            sideV[h] += (-k * side[h] - c * sideV[h] + sk) * dt;
            side[h] += sideV[h] * dt;
            side[h] = Clamp(side[h], -25.0f, 25.0f);
            // Knee trails the thigh: bends when the thigh swings forward, straightens hanging.
            float kneeT = g_cfg.climbGripKnee + Clamp(-swing[h], 0.0f, 40.0f) * 0.4f;
            knee[h] += (kneeT - knee[h]) * Clamp(dt * 6.0f, 0.0f, 1.0f);
        }
        // Rope-climb grip (user reference photo): thighs raised, knees bent, legs drawn together and crossed at the
        // ankles around the chain - both hips turned in by ClimbCross (the right a little more, so it crosses over).
        hL = g_cfg.climbGripHip + swing[0]; kL = knee[0]; rL = g_cfg.climbCross - side[0];
        hR = g_cfg.climbGripHip + swing[1]; kR = knee[1]; rR = -g_cfg.climbCross * 1.3f + side[1];
    } else {
        // Ladder steps: the stepping leg lifts its knee to the next rung while the other stays straight underneath.
        float vz = pawn->Velocity.Z;
        float speed = fabsf(vz);
        moveBlend += Clamp(Clamp(speed / 60.0f, 0.0f, 1.0f) - moveBlend, -dt * 4.0f, dt * 4.0f);
        phase += (vz >= 0.0f ? 1.0f : -1.0f) * 3.14159265f * speed * dt / (g_cfg.climbStride > 1.0f ? g_cfg.climbStride : 1.0f);
        if (phase > 6.2831853f) phase -= 6.2831853f;
        if (phase < 0.0f) phase += 6.2831853f;
        float sL = sinf(phase), sR = -sL;
        auto leg = [&](float s, float& hip, float& kn) {
            float up = 0.5f + 0.5f * s;
            float lift = up * up * (3.0f - 2.0f * up);
            hip = g_cfg.climbGripHip + moveBlend * lift * g_cfg.climbHipDeg;
            kn = g_cfg.climbGripKnee + moveBlend * lift * g_cfg.climbKneeDeg;
        };
        leg(sL, hL, kL);
        leg(sR, hR, kR);
    }
    float values[4] = { hL, hR, kL, kR };
    USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
    for (USkeletalMeshComponent* comp : comps) {
        if (!comp) continue;
        for (int i = 0; i < 4; ++i) {
            FName name(kLegBones[i]);
            auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(name));
            if (ctl) SetLegRotation(ctl, values[i], amount);
            // Sideways swing of the dangling legs (bone-space yaw on the hips).
            if (ctl && i < 2 && g_cfg.climbDangle) ctl->BoneRotation.Yaw = static_cast<int>((i == 0 ? rL : rR) * 65536.0f / 360.0f);
        }
    }
}

// Jumping: no usable jump animation exists for NPCs (only attack leaps in other map packages, and loading a
// second map package tears down our mesh), so pose the legs procedurally like the slide. In the air the stride
// freezes (UpdateLocomotionBlend) and the knees tuck - lead leg forward, trailing leg folded - then reach down
// toward the ground while falling. Landing drops both bodies briefly (g_landDip); the planted-feet IK turns that
// into a knee bend. Same controls on the shadow body, so the shadow mirrors it.
// Swimming (physics 3 in the log): legs paddle - an alternating kick from the hips with the knees bending on the
// back-swing - faster when swimming faster. The walk cycle is held on idle (UpdateSlide's speed threshold) and the
// torso is hidden (the collar/hood ringed the view underwater). Same controls on both bodies.
static void UpdateSwim(ADishonoredPlayerPawn* pawn) {
    static DWORD last = 0;
    static float amount = 0.0f, phase = 0.0f;
    static bool wrote = false;
    if (!pawn || !g_body || !g_bodyAnimated) return;
    DWORD now = GetTickCount();
    float dt = last ? Clamp((now - last) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    last = now;
    bool swimming = g_cfg.swimPose && static_cast<int>(pawn->Physics) == g_cfg.swimPhysics;
    amount += Clamp((swimming ? 1.0f : 0.0f) - amount, -dt * 4.0f, dt * 4.0f);
    g_swimAmount = amount;
    static int logged = -1;
    if ((swimming ? 1 : 0) != logged) { Log("swimming %s", swimming ? "started" : "ended"); logged = swimming ? 1 : 0; }
    if (amount <= 0.0f) {
        if (wrote) {
            USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
            for (USkeletalMeshComponent* comp : comps) {
                if (!comp) continue;
                for (int i = 0; i < 4; ++i)
                    if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kLegBones[i])))) SetLegRotation(ctl, 0.0f, 0.0f);
            }
            wrote = false;
        }
        return;
    }
    FVector v = pawn->Velocity;
    float speed = sqrtf(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    float hz = g_cfg.swimKickRate * (0.6f + Clamp(speed / 300.0f, 0.0f, 1.0f));   // treading water ~0.6x, swimming 1.6x
    phase += 6.2831853f * hz * dt;
    if (phase > 6.2831853f) phase -= 6.2831853f;
    float sL = sinf(phase), sR = sinf(phase + 3.14159265f);
    // Hip: around a slight forward lean (negative = forward, like the slide); knee bends while the leg swings back.
    float values[4] = {
        -15.0f + g_cfg.swimKickHip * sL,
        -15.0f + g_cfg.swimKickHip * sR,
        20.0f + g_cfg.swimKickKnee * Clamp(sL, 0.0f, 1.0f),
        20.0f + g_cfg.swimKickKnee * Clamp(sR, 0.0f, 1.0f) };
    USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
    for (USkeletalMeshComponent* comp : comps) {
        if (!comp) continue;
        for (int i = 0; i < 4; ++i)
            if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kLegBones[i])))) SetLegRotation(ctl, values[i], amount);
    }
    wrote = true;
}

static void UpdateJump(ADishonoredPlayerPawn* pawn) {
    static DWORD last = 0;
    static float amount = 0.0f, land = 0.0f, lastVz = 0.0f;
    static bool wasAir = false, wrote = false;
    if (!pawn || !g_body || !g_bodyAnimated) return;
    DWORD now = GetTickCount();
    float dt = last ? Clamp((now - last) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    last = now;
    bool air = g_cfg.jumpPose && static_cast<int>(pawn->Physics) == 2 && !g_isClimbing && !g_isSliding;
    float vz = pawn->Velocity.Z;
    // Build 184: the body's jump offset (JumpBodyBack/Down) let go in 0.1 s on landing, sliding the chest and the
    // shrunk collar/neck forward under a camera looking down (video 16-43-24). Hold it through the landing and let
    // it go over ~0.4 s.
    static float landHold = 0.0f;
    if (wasAir && !air && static_cast<int>(pawn->Physics) == 1) landHold = 1.0f;
    if (wasAir && !air && static_cast<int>(pawn->Physics) == 1 && lastVz < -150.0f) {
        land = Clamp(-lastVz / 700.0f, 0.3f, 1.0f);   // harder landings squat deeper
        Log("landed (fall speed %.0f): dip %.0f", -lastVz, land * g_cfg.jumpLandDip);
    }
    wasAir = air;
    if (air) lastVz = vz;
    amount += Clamp((air ? 1.0f : 0.0f) - amount, -dt * 10.0f, dt * 8.0f);
    land = Clamp(land - dt / 0.35f, 0.0f, 1.0f);
    landHold = air ? 0.0f : Clamp(landHold - dt / 0.4f, 0.0f, 1.0f);
    g_landHold = landHold;
    g_jumpAmount = amount;
    {
        float h = landHold * landHold * (3.0f - 2.0f * landHold);
        g_jumpBodyW = amount > h ? amount : h;
    }
    g_landDip = -g_cfg.jumpLandDip * sinf(land * 1.5707963f);   // eases back up

    // Hover (build 190): Daud's Transversal held in the air stops time with physics 0 while the player state stays
    // Falling/Jump (log 18:49); the body read that as standing - idle legs on nothing, even the turn-on-the-spot step
    // (user). Float instead: knees up, toes down, a slow drift; blends with the jump pose either side of it.
    static float hover = 0.0f, hoverPhase = 0.0f;
    {
        static UClass* fallCls = nullptr;
        static UClass* jumpCls = nullptr;
        if (!fallCls) fallCls = UObject::FindClass("Class DishonoredGame.StatePlayerMasterFalling");
        if (!jumpCls) jumpCls = UObject::FindClass("Class DishonoredGame.StatePlayerMasterJump");
        UObject* st = pawn->m_pPlayerMasterFSM ? pawn->m_pPlayerMasterFSM->m_pCurrentState : nullptr;
        bool airState = st && ((fallCls && st->IsA(fallCls)) || (jumpCls && st->IsA(jumpCls)));
        bool hovering = g_cfg.hoverPose && static_cast<int>(pawn->Physics) == 0 && airState && !g_isClimbing && !g_isSliding;
        float before = hover;
        hover += Clamp((hovering ? 1.0f : 0.0f) - hover, -dt * 4.0f, dt * 5.0f);
        if ((before <= 0.0f) != (hover <= 0.0f)) Log("hover pose %s", hover > 0.0f ? "on (held in the air)" : "off");
        hoverPhase += 6.2831853f * g_cfg.hoverSwayRate * dt;
        if (hoverPhase > 6.2831853f) hoverPhase -= 6.2831853f;
    }
    g_hoverAmount = hover;

    float w = amount > hover ? amount : hover;
    if (w <= 0.0f) {
        if (wrote) {   // hand the leg controls back cleanly (crouch/slide/climb own them otherwise)
            USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
            for (USkeletalMeshComponent* comp : comps) {
                if (!comp) continue;
                for (int i = 0; i < 4; ++i)
                    if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kLegBones[i])))) SetLegRotation(ctl, 0.0f, 0.0f);
                for (const char* foot : { "foot_L_jnt", "foot_R_jnt" }) {
                    FName toe(foot);
                    toe.InstanceNumber = 1;
                    if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(toe))) SetLegRotation(ctl, 0.0f, 0.0f);
                }
            }
            wrote = false;
        }
        return;
    }
    // Rising: tucked. Falling: legs reach down for the landing (less knee, hips nearer straight).
    float fall = Clamp(-vz / 400.0f, 0.0f, 1.0f);
    float tuck = 1.0f - 0.6f * fall;
    float jumpPose[4] = {
        g_cfg.jumpHipDeg * tuck,                 // left hip: lead leg forward
        g_cfg.jumpHipDeg * 0.3f * tuck,          // right hip: trailing leg nearly under the body
        g_cfg.jumpKneeDeg * 0.7f * tuck,         // left knee
        g_cfg.jumpKneeDeg * tuck };              // right knee: folded more
    // Floating: both knees up a little (one more than the other), legs drifting slowly out of step.
    float s = sinf(hoverPhase), c = sinf(hoverPhase + 1.2f);
    float sway = g_cfg.hoverSwayDeg;
    float hoverPose[4] = {
        g_cfg.hoverHipDeg + sway * s,
        g_cfg.hoverHipDeg * 0.6f - sway * s,
        g_cfg.hoverKneeDeg + sway * 1.2f * c,
        g_cfg.hoverKneeDeg * 1.2f - sway * 1.2f * c };
    // HoverSpread: scale each pair's difference about its middle (the pose stays put, the legs part further).
    for (int i = 0; i < 4; i += 2) {
        float mid = (hoverPose[i] + hoverPose[i + 1]) * 0.5f, half = (hoverPose[i] - hoverPose[i + 1]) * 0.5f * g_cfg.hoverSpread;
        hoverPose[i] = mid + half;
        hoverPose[i + 1] = mid - half;
    }
    float toes[2] = { g_cfg.kneeSign * g_cfg.hoverToeDeg + sway * 0.6f * c, g_cfg.kneeSign * g_cfg.hoverToeDeg - sway * 0.6f * c };
    float values[4];
    for (int i = 0; i < 4; ++i) values[i] = (jumpPose[i] * amount + hoverPose[i] * hover) / w;
    USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
    for (USkeletalMeshComponent* comp : comps) {
        if (!comp) continue;
        for (int i = 0; i < 4; ++i)
            if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kLegBones[i])))) SetLegRotation(ctl, values[i], w);
        static const char* kFeet[2] = { "foot_L_jnt", "foot_R_jnt" };
        for (int f = 0; f < 2; ++f) {
            FName toe(kFeet[f]);
            toe.InstanceNumber = 1;
            if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(toe))) SetLegRotation(ctl, toes[f], hover);
        }
    }
    wrote = true;
}

static void UpdateCrouchLegs(ADishonoredPlayerPawn* pawn, bool crouched) {
    if (g_isSliding || g_isClimbing) return;   // the slide / climb poses own the leg controls
    static DWORD last = 0;
    static float phase = 0.0f, strength = 0.0f;
    if (!g_body || !g_bodyAnimated || !pawn) return;
    DWORD now = GetTickCount();
    float dt = last ? Clamp((now - last) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    last = now;
    FVector v = pawn->Velocity;
    float speed = sqrtf(v.X * v.X + v.Y * v.Y);
    // Fade the swing in with speed while crouched, out otherwise.
    float target = crouched ? Clamp(speed / 120.0f, 0.0f, 1.0f) : 0.0f;
    strength += Clamp(target - strength, -dt * 4.0f, dt * 4.0f);
    // One full cycle (left + right step) per 2 * CrouchStride units travelled.
    phase += 3.14159265f * speed * dt / (g_cfg.crouchStride > 1.0f ? g_cfg.crouchStride : 1.0f);
    if (phase > 6.2831853f) phase -= 6.2831853f;

    float s = sinf(phase);
    float hip[2] = { g_cfg.hipSwingDeg * s, -g_cfg.hipSwingDeg * s };
    // Each knee bends while its leg swings forward.
    float knee[2] = { g_cfg.kneeSign * g_cfg.kneeBendDeg * Clamp(cosf(phase), 0.0f, 1.0f),
                      g_cfg.kneeSign * g_cfg.kneeBendDeg * Clamp(-cosf(phase), 0.0f, 1.0f) };
    float values[4] = { hip[0], hip[1], knee[0], knee[1] };
    for (int i = 0; i < 4; ++i) {
        FName name(kLegBones[i]);
        auto* ctl = static_cast<USkelControlSingleBone*>(g_body->FindSkelControl(name));
        if (ctl) SetLegRotation(ctl, values[i], strength);
    }
}

// AnimTree
//  └ BlendList "posture" (set from code each frame: 0 standing, 1 crouched)
//     ├ BlendBySpeed [ Idle | Directional(Walk) | Directional(Run) ]
//     └ Assassin_Crouch_loop   (no character in Dishonored has a crouch-walk; held while crouch-moving)
static UAnimTree* BuildBodyAnimTree(UObject* outer) {
    auto* tree = NewObject<UAnimTree>("Class Engine.AnimTree", outer);
    if (!tree) return nullptr;
    auto* posture = NewObject<UAnimNodeBlendList>("Class Engine.AnimNodeBlendList", tree);
    auto* speed = NewObject<UAnimNodeBlendBySpeed>("Class Engine.AnimNodeBlendBySpeed", tree);
    UAnimNode* idle = NewSequence(tree, "Empty_Idle");
    UAnimNode* walk = NewDirectional(tree, g_cfg.walkAnimRate, "Empty_WalkN", "Empty_WalkS_Revert", "Empty_WalkW_Revert", "Empty_WalkE");
    UAnimNode* run = NewDirectional(tree, g_cfg.runAnimRate, "Empty_RunN", "Empty_RunS_Revert", "Empty_RunW_Revert", "Empty_RunE");
    UAnimNode* crouch = g_crouchSet ? NewSequence(tree, "Assassin_Crouch_loop") : nullptr;
    if (!posture || !speed || !idle || !walk || !run) { Log("failed to build anim nodes"); return nullptr; }

    // Walk and run are mixed continuously by speed (analogue stick): Child2Weight is set every frame in
    // UpdateLocomotionBlend from RunBlendStart..RunBlendEnd.
    auto* loco = NewObject<UAnimNodeBlend>("Class Engine.AnimNodeBlend", tree);
    if (!loco) return nullptr;
    UAnimNode* locoKids[2] = { walk, run };
    if (!SetChildren(loco, locoKids, 2)) return nullptr;

    // Turning on the spot: idle slot = BlendList [Idle, TurnLeft45, TurnRight45], switched by UpdateTurnInPlace.
    // The turn clips rotate the NPC's root; discard that (the view already turns), keep only the foot shuffle.
    auto* idleTurn = NewObject<UAnimNodeBlendList>("Class Engine.AnimNodeBlendList", tree);
    auto* turnL = NewSequence(tree, "Empty_TurnLeft45");
    auto* turnR = NewSequence(tree, "Empty_TurnRight45");
    if (idleTurn && turnL && turnR) {
        for (UAnimNodeSequence* s : { turnL, turnR })
            s->RootRotationOption[0] = s->RootRotationOption[1] = s->RootRotationOption[2] = ERootRotationOption::RRO_Discard;
        UAnimNode* turnKids[3] = { idle, turnL, turnR };
        float turnTargets[3] = { 1.0f, 0.0f, 0.0f };
        if (SetChildren(idleTurn, turnKids, 3)) {
            FillArray(&idleTurn->TargetWeight, turnTargets, 3, sizeof(float));
            idle = idleTurn;
        }
    }
    // Arms holding gear barely move in first person (user), so each arm can be switched from the walk/run swing to
    // the idle pose: BlendPerBone(L) [loco, idle] inside BlendPerBone(R) [.., idle], branch from the upper arm.
    // Weights are set per frame from what each hand holds (shadow body only; see UpdateArmBlend).
    UAnimNode* moving = loco;
    {
        const char* bones[2] = { "upper_arm_L_jnt", "upper_arm_R_jnt" };
        for (int side = 0; side < 2; ++side) {
            auto* per = NewObject<UAnimNodeBlendPerBone>("Class Engine.AnimNodeBlendPerBone", tree);
            UAnimNode* still = NewSequence(tree, "Empty_Idle");
            FName bone(bones[side]);
            if (!per || !still || bone.FNameEntryId < 0) break;
            UAnimNode* kids[2] = { moving, still };
            int32_t nameRaw[2] = { bone.FNameEntryId, 0 };
            if (!SetChildren(per, kids, 2) || !FillArray(reinterpret_cast<RawArray*>(&per->BranchStartBoneName), nameRaw, 1, 8)) break;
            per->Child2Weight = per->Child2WeightTarget = 0.0f;
            moving = per;
        }
    }
    UAnimNode* speedKids[2] = { idle, moving };
    if (!SetChildren(speed, speedKids, 2)) return nullptr;
    float speedTargets[2] = { 1.0f, 0.0f };
    FillArray(&speed->TargetWeight, speedTargets, 2, sizeof(float));
    // Child i plays between Constraints[i] and Constraints[i+1] (uu/s): idle below 30, moving above.
    float constraints[3] = { 0.0f, 30.0f, 5000.0f };
    FillArray(&speed->Constraints, constraints, 3, sizeof(float));
    speed->BlendUpTime = 0.15f;
    speed->BlendDownTime = 0.25f;
    speed->BlendDownPerc = 0.2f;
    speed->BlendUpDelay = 0.0f;

    UAnimNode* postureKids[2] = { speed, crouch ? crouch : idle };
    if (!SetChildren(posture, postureKids, 2)) return nullptr;
    float postureTargets[2] = { 1.0f, 0.0f };
    FillArray(&posture->TargetWeight, postureTargets, 2, sizeof(float));

    UAnimNode* treeKids[1] = { posture };
    if (!SetChildren(tree, treeKids, 1)) return nullptr;
    AddLegControls(tree);
    return tree;
}

static bool SetupBodyAnimation(USkeletalMeshComponent* comp) {
    if (!g_animSet) return false;
    UAnimSet* sets[2] = { g_animSet, g_crouchSet };
    if (!FillArray(&comp->AnimSets, sets, g_crouchSet ? 2 : 1, sizeof(UAnimSet*))) { Log("could not allocate AnimSets"); return false; }
    UAnimTree* tree = BuildBodyAnimTree(comp);
    if (!tree) return false;
    comp->SetAnimTreeTemplate(tree);
    Log("body anim tree set: template=%p instance=%p crouch=%s", tree, comp->Animations, g_crouchSet ? "yes" : "no");
    return comp->Animations != nullptr;
}

// The instanced posture BlendList: Animations (AnimTree) -> Children[0].Anim.
static UAnimNodeBlendList* GetPostureNode(USkeletalMeshComponent* comp) {
    if (!comp || !comp->Animations) return nullptr;
    auto* children = reinterpret_cast<RawArray*>(&static_cast<UAnimNodeBlendBase*>(comp->Animations)->Children);
    if (!children->Data || children->Num < 1) return nullptr;
    return *reinterpret_cast<UAnimNodeBlendList**>(static_cast<uint8_t*>(children->Data) + 8);
}

// Child i's AnimNode of an instanced blend node (FAnimBlendChild: 0x50 bytes, Anim at +8).
static UAnimNode* ChildAnim(UAnimNode* node, int i) {
    if (!node) return nullptr;
    auto* children = reinterpret_cast<RawArray*>(&static_cast<UAnimNodeBlendBase*>(node)->Children);
    if (!children->Data || i < 0 || i >= children->Num) return nullptr;
    return *reinterpret_cast<UAnimNode**>(static_cast<uint8_t*>(children->Data) + i * 0x50 + 8);
}


// Analogue stick, per frame:
//  1. mix walk -> run continuously between RunBlendStart and RunBlendEnd;
//  2. play each cycle at speed / (the speed it was authored for), so the feet stay planted at any stick angle.
// Instanced tree: AnimTree -> posture BlendList [0] -> BlendBySpeed [1] -> AnimNodeBlend (walk dir, run dir).
// Shadow body: an arm holding gear plays the idle pose instead of the walk/run swing (eased with g_weaponRaise);
// first-person arms holding gear barely move (user).
static void UpdateArmBlend() {
    USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
    for (int ci = 0; ci < 2; ++ci) {
        if (!comps[ci]) continue;
        UAnimNode* outer = ChildAnim(ChildAnim(ChildAnim(comps[ci]->Animations, 0), 0), 1);   // per-bone R
        if (!outer || outer->Class != UAnimNodeBlendPerBone::StaticClass()) continue;
        UAnimNode* inner = ChildAnim(outer, 0);                                               // per-bone L
        float steady = Clamp(g_cfg.shadowArmSteady, 0.0f, 1.0f);
        float r = ci == 1 ? g_weaponRaise[1] * steady : 0.0f;
        float l = ci == 1 ? g_weaponRaise[0] * steady : 0.0f;
        auto* pr = static_cast<UAnimNodeBlend*>(outer);
        if (fabsf(pr->Child2WeightTarget - r) > 0.01f) pr->SetBlendTarget(r, 0.0f);
        if (inner && inner->Class == UAnimNodeBlendPerBone::StaticClass()) {
            auto* pl = static_cast<UAnimNodeBlend*>(inner);
            if (fabsf(pl->Child2WeightTarget - l) > 0.01f) pl->SetBlendTarget(l, 0.0f);
        }
    }
}

// Foot lock (build 185): walking backwards looked like a moonwalk (user, video 17-03-12). While a foot is planted,
// its velocity relative to the pawn (pawn frame) must be minus the pawn's velocity; a clip that steps the wrong way
// (planted foot moving with the body, ratio < 0) is played in reverse (AnimNodeSequence::AdvanceBy wraps negative
// time). Build 185 measured it: Empty_WalkS_Revert is a forward stride (ratio -0.25 -> reversed, then 1.0 at 1.9x).
// Build 186: matching the step rate to the ground too made the legs frantic (Dishonored walks ~360 uu/s, the guard
// clip is a slow walk: backward 1.9x, forward ran up to the 2.5x cap - user: "walking speeds don't match"). Every
// direction now steps at the forward cadence the user had approved; backwards starts reversed; the measurement only
// flips a clip that steps the wrong way.
static float g_footRate[2][4] = { { 1, -1, 1, 1 }, { 1, -1, 1, 1 } };   // [walk, run][forward, back, left, right]: +1 / -1 (reversed)

static void UpdateFootLock(ADishonoredPlayerPawn* pawn, float speed, float alpha) {
    static FVector prev[2];
    static bool havePrev = false;
    static int prevYaw = 0;
    static LARGE_INTEGER last = {}, freq = {};
    static float avg[2][4] = {}, seen[2][4] = {};
    static DWORD lastLog = 0;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER nowQ;
    QueryPerformanceCounter(&nowQ);
    float dt = last.QuadPart ? static_cast<float>(nowQ.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    last = nowQ;
    if (!g_cfg.footLock || !g_body) { havePrev = false; return; }
    float yaw = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
    float cy = cosf(yaw), sy = sinf(yaw);
    static const char* kFeet[2] = { "foot_L_jnt", "foot_R_jnt" };
    FVector rel[2];
    for (int f = 0; f < 2; ++f) {
        FVector p = g_body->GetBoneLocation(FName(kFeet[f]), 0);
        float dx = p.X - pawn->Location.X, dy = p.Y - pawn->Location.Y;
        rel[f] = { dx * cy + dy * sy, -dx * sy + dy * cy, p.Z - pawn->Location.Z };
    }
    int dyaw = (pawn->Rotation.Yaw - prevYaw) & 0xFFFF;
    if (dyaw > 32768) dyaw -= 65536;
    prevYaw = pawn->Rotation.Yaw;
    FVector v = pawn->Velocity;
    float vf = v.X * cy + v.Y * sy, vr = -v.X * sy + v.Y * cy;
    float ang = atan2f(vr, vf) * 57.29578f;
    int dir = fabsf(ang) < 30.0f ? 0 : fabsf(ang) > 150.0f ? 1 : (ang < -60.0f && ang > -120.0f) ? 2 : (ang > 60.0f && ang < 120.0f) ? 3 : -1;
    int gait = alpha < 0.1f ? 0 : (alpha > 0.9f ? 1 : -1);
    bool steady = havePrev && dt > 0.004f && dt < 0.1f && abs(dyaw) < 100 && static_cast<int>(pawn->Physics) == 1 &&
                  !g_isCrouched && !g_isSliding && g_jumpAmount <= 0.0f && g_landDip > -0.5f && speed > 80.0f;
    int lo = rel[0].Z < rel[1].Z ? 0 : 1;
    if (steady && gait >= 0 && dir >= 0 && rel[1 - lo].Z - rel[lo].Z > 2.0f) {
        float uf = (rel[lo].X - prev[lo].X) / dt, ur = (rel[lo].Y - prev[lo].Y) / dt;
        float r = Clamp(-(uf * vf + ur * vr) / (speed * speed), -3.0f, 3.0f);   // 1 = the planted foot stays put
        float& a = avg[gait][dir];
        float& m = g_footRate[gait][dir];
        a += (r - a) * Clamp(dt * 1.5f, 0.0f, 1.0f);
        seen[gait][dir] += dt;
        static const char* kDir[4] = { "forward", "backward", "left", "right" };
        if (seen[gait][dir] > 1.0f && a < -0.35f) {
            m = -m;
            Log("foot lock: %s %s clip steps the wrong way (planted-foot ratio %.2f) - playing it %s",
                gait ? "run" : "walk", kDir[dir], a, m < 0.0f ? "in reverse" : "forwards again");
            a = 0.5f;
            seen[gait][dir] = 0.0f;
        }
        if (GetTickCount() - lastLog > 2000) {
            lastLog = GetTickCount();
            Log("foot lock: %s %s speed %.0f planted-foot ratio %.2f (1 = no sliding), %s", gait ? "run" : "walk",
                kDir[dir], speed, a, m < 0.0f ? "reversed" : "forwards");
        }
    }
    prev[0] = rel[0];
    prev[1] = rel[1];
    havePrev = true;
}

static void UpdateLocomotionBlend(ADishonoredPlayerPawn* pawn) {
    if (!g_body || !g_bodyAnimated || !pawn) return;
    FVector v = pawn->Velocity;
    float speed = sqrtf(v.X * v.X + v.Y * v.Y);
    float span = g_cfg.runBlendEnd - g_cfg.runBlendStart;
    float alpha = span > 1.0f ? (speed - g_cfg.runBlendStart) / span : (speed >= g_cfg.runBlendEnd ? 1.0f : 0.0f);
    alpha = Clamp(alpha, 0.0f, 1.0f) * Clamp(g_cfg.runBlendMax, 0.0f, 1.0f);
    UpdateFootLock(pawn, speed, alpha);
    float walkRate = Clamp(speed / g_cfg.walkAnimSpeed, 0.2f, 3.0f);
    float runRate = Clamp(speed / g_cfg.runAnimSpeed, 0.2f, 3.0f);
    // Airborne: freeze the stride mid-step (the jump pose is layered on top by UpdateJump).
    walkRate *= 1.0f - 0.95f * g_jumpAmount;
    runRate *= 1.0f - 0.95f * g_jumpAmount;

    // Same values for the visible body and the shadow body, so the shadow moves with the legs you see.
    USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
    for (USkeletalMeshComponent* comp : comps) {
        if (!comp) continue;
        UAnimNode* n = ChildAnim(ChildAnim(ChildAnim(comp->Animations, 0), 0), 1);
        while (n && n->Class == UAnimNodeBlendPerBone::StaticClass()) n = ChildAnim(n, 0);   // the arm blends wrap loco
        auto* loco = static_cast<UAnimNodeBlend*>(n);
        if (!loco) continue;
        if (fabsf(alpha - loco->Child2WeightTarget) > 0.01f) loco->SetBlendTarget(alpha, 0.1f);
        for (int d = 0; d < 2; ++d) {
            UAnimNode* dir = ChildAnim(loco, d);
            for (int i = 0; i < 4; ++i)
                if (auto* seq = static_cast<UAnimNodeSequence*>(ChildAnim(dir, i)))
                    seq->Rate = (d == 0 ? walkRate : runRate) * g_footRate[d][i] *
                                (i == 1 ? g_cfg.walkBackRate : (i >= 2 ? g_cfg.walkSideRate : 1.0f));
        }
    }
}

// Turning on the spot: measure the yaw rate; while standing still and turning faster than TurnStepThreshold, play
// the guard's 45-degree turn step (left/right) at a rate proportional to the turn speed. Held for 0.25 s after the
// turn stops so stick jitter doesn't flicker it. Instanced tree: Animations -> posture [0] -> BlendBySpeed [0].
static void UpdateTurnInPlace(ADishonoredPlayerPawn* pawn) {
    static DWORD last = 0, lastTurning = 0;
    static int lastYaw = 0, activeChild = 0;
    static bool haveYaw = false;
    if (!g_body || !g_bodyAnimated || !pawn) return;
    DWORD now = GetTickCount();
    float dt = last ? (now - last) / 1000.0f : 0.0f;
    last = now;
    int yaw = pawn->Rotation.Yaw & 0xFFFF;
    int dyaw = haveYaw ? yaw - lastYaw : 0;
    if (dyaw > 32768) dyaw -= 65536; else if (dyaw < -32768) dyaw += 65536;
    lastYaw = yaw;
    haveYaw = true;
    if (dt <= 0.0f || dt > 0.2f) return;
    float rate = dyaw * (360.0f / 65536.0f) / dt;   // deg/s, positive = turning right

    FVector v = pawn->Velocity;
    bool still = sqrtf(v.X * v.X + v.Y * v.Y) < 30.0f && !g_isSliding && g_hoverAmount <= 0.0f;   // no foot shuffle mid-air
    int want = 0;
    if (still && fabsf(rate) > g_cfg.turnThreshold) { want = rate > 0 ? 2 : 1; lastTurning = now; }
    else if (activeChild != 0 && still && now - lastTurning < 250) want = activeChild;   // hold briefly
    float playRate = Clamp(fabsf(rate) / (g_cfg.turnAnimDegPerSec > 1.0f ? g_cfg.turnAnimDegPerSec : 1.0f), 0.6f, 2.5f);

    USkeletalMeshComponent* comps[2] = { g_body, g_shadowBody };
    for (USkeletalMeshComponent* comp : comps) {
        if (!comp) continue;
        auto* list = static_cast<UAnimNodeBlendList*>(ChildAnim(ChildAnim(ChildAnim(comp->Animations, 0), 0), 0));
        if (!list || reinterpret_cast<RawArray*>(&list->Children)->Num != 3) continue;
        if (want != activeChild) list->SetActiveChild(want, 0.15f);
        if (want != 0)
            if (auto* seq = static_cast<UAnimNodeSequence*>(ChildAnim(list, want))) seq->Rate = playRate;
    }
    activeChild = want;
}

static void ApplyArmHiding();

static void UpdatePosture(ADishonoredPlayerPawn* pawn) {
    static int lastChild = -1;
    static USkeletalMeshComponent* lastComp = nullptr;
    if (!g_body || !g_bodyAnimated || !pawn) return;
    static float standingHeight = 0.0f;
    if (g_body != lastComp) { lastComp = g_body; lastChild = -1; standingHeight = 0.0f; }
    // Dishonored doesn't use APawn::bIsCrouched, and CrouchedPct reads 0.69 while standing (test 21). The collision
    // cylinder does shrink when crouching, so compare it with the tallest height seen for this body.
    float height = pawn->CylinderComponent ? pawn->CylinderComponent->CollisionHeight : 0.0f;
    if (height > standingHeight) standingHeight = height;
    bool crouched = pawn->bIsCrouched || (standingHeight > 0.0f && height < standingHeight * 0.8f);
    UpdateCrouchLegs(pawn, crouched);
    g_isCrouched = crouched;
    // Only the game's slide movement (physics 14, log): ducking under things and swimming also shrink the
    // cylinder and wrongly played the slide pose (user, video 18-03-29).
    g_isSliding = pawn->bIsCrouched && height > 0.0f && height < g_cfg.slideHeight &&
                  (g_cfg.slidePhysics < 0 || static_cast<int>(pawn->Physics) == g_cfg.slidePhysics) &&
                  !InTakedown(pawn);   // a choke also runs on physics 14 (log 19:41)
    g_crouchSink = standingHeight > height ? standingHeight - height : 0.0f;
    int child = (crouched && g_cfg.useCrouchPose) ? 1 : 0;
    if (child == lastChild) return;
    UAnimNodeBlendList* posture = GetPostureNode(g_body);
    if (!posture) return;
    posture->SetActiveChild(child, lastChild < 0 ? 0.0f : 0.25f);
    if (g_cfg.hideTorsoCrouched) {
        FName torso(g_cfg.crouchHideBone);
        if (torso.FNameEntryId >= 0) {
            if (crouched) g_body->HideBoneByName(torso, EPhysBodyOp::PBO_None);
            else g_body->UnHideBoneByName(torso);
        }
        ApplyArmHiding();
    }
    Log("posture -> %s (cylinder %.1f of %.1f, eye %.1f, bIsCrouched %d)", crouched ? "crouched" : "standing", height,
        standingHeight, pawn->EyeHeight, pawn->bIsCrouched);
    lastChild = child;
}

// RF_RootSet (0x4000): never garbage collect. Needed between finding an object and referencing it from the body:
// loading the next package runs a GC that frees still-unreferenced objects (crash in test 12).
static void KeepAlive(UObject* obj) {
    if (obj) *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(obj) + 0x08) |= 0x0000000000004000ULL;
}

static void LetGo(UObject* obj) {
    if (obj) *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(obj) + 0x08) &= ~0x0000000000004000ULL;
}

constexpr uint64_t kUnreachableFlag = 0x0000000200000000ULL;

// Rooted and not marked for purge. Run every frame as a backstop: if a GC already marked one of ours
// unreachable before we rooted it, clearing the mark before the incremental purge reaches it saves it.
static void Protect(UObject* obj) {
    if (!obj) return;
    auto& flags = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(obj) + 0x08);
    static int warned = 0;
    if ((flags & kUnreachableFlag) && warned++ < 10) Log("un-marking unreachable object %p", obj);
    flags = (flags | 0x0000000000004000ULL) & ~kUnreachableFlag;
}

static void ProtectAssets() {
    Protect(g_bodyMesh);
    Protect(g_headMesh);
    Protect(g_daudBodyMesh);
    Protect(g_daudHeadMesh);
    for (UAnimSet* set : { g_animSet, g_crouchSet }) {
        Protect(set);
        if (set) for (int i = 0; i < set->Sequences.size(); ++i) Protect(set->Sequences.at(i));
    }
}

// Release what we rooted; called whenever the level or pawn changes (we reload per level).
static void ReleaseLoadedAssets() {
    LetGo(g_bodyMesh);
    LetGo(g_headMesh);
    LetGo(g_daudBodyMesh);
    LetGo(g_daudHeadMesh);
    for (UAnimSet* set : { g_animSet, g_crouchSet }) {
        LetGo(set);
        if (set) for (int i = 0; i < set->Sequences.size(); ++i) LetGo(set->Sequences.at(i));
    }
}

static void* GetWorld();
extern void* g_assetsWorld;

static void GrabAssetsNow() {
    g_assetsWorld = GetWorld();
    if (!g_bodyMesh) g_bodyMesh = FindLoadedMesh(kSources[g_sourceIndex].mesh);
    if (!g_animSet) g_animSet = static_cast<UAnimSet*>(FindObjectByName("AnimSet", kAnimSetName));
    if (!g_crouchSet) g_crouchSet = static_cast<UAnimSet*>(FindObjectByName("AnimSet", kCrouchSetName));
    if (!g_headMesh) g_headMesh = FindLoadedMesh("DLC07_skm_Corvo_Mask");
    if (!g_daudBodyMesh) g_daudBodyMesh = FindLoadedMesh("skm_Daud_Body");
    if (!g_daudHeadMesh) g_daudHeadMesh = FindLoadedMesh("skm_Daud_Head");
    Log("Daud body: %p, Daud head: %p", g_daudBodyMesh, g_daudHeadMesh);
    Log("crouch set: %p, head mesh: %p", g_crouchSet, g_headMesh);
    ProtectAssets();
    if (!g_animSet) g_animLoadState = LoadState::Failed;   // never load a second map package (tests 12-15)
    Log("grabbed in load callback: mesh %p, anim set %p (%d sequences)", g_bodyMesh, g_animSet,
        g_animSet ? g_animSet->Sequences.size() : 0);
}

static void ApplyArmHiding() {
    if (!g_body) return;
    for (const char* bone : kArmBones) {
        FName boneName(bone);
        if (boneName.FNameEntryId < 0) { Log("bone name %s not in name table", bone); continue; }
        // Arms are now shrunk per hand by their SkelControls (UpdateCoat), so never hard-hide them here.
        g_body->UnHideBoneByName(boneName);
    }
    // No head on the body: the neck stump and collar top are what the camera meets when it pivots. Hide them.
    FName neck("neck_jnt");
    if (neck.FNameEntryId >= 0) g_body->HideBoneByName(neck, EPhysBodyOp::PBO_None);
    // Chest/collar too (Torso_jnt; the shoulders hang off spine_3, so the arms stay). The shadow body keeps it all.
    FName torso("Torso_jnt");
    if (g_cfg.hideTorso && torso.FNameEntryId >= 0) g_body->HideBoneByName(torso, EPhysBodyOp::PBO_None);
    g_bodyArmsHidden = g_cfg.hideArms;
}

static void AttachBody(ADishonoredPlayerPawn* pawn) {
    USkeletalMeshComponent* arms = pawn->Mesh;
    FVector offset{ g_cfg.offsetX, g_cfg.offsetY, g_cfg.offsetZ };
    FRotator zeroRot{ 0, 0, 0 };
    FVector one{ 1, 1, 1 };
    if (g_cfg.attachMode == 0) {
        FName root("root0_jnt");
        arms->AttachComponent(g_body, root, offset, zeroRot, one);
        Log("attached body to 1P mesh bone root0_jnt (name id %d)", root.FNameEntryId);
    } else {
        pawn->AttachComponent(g_body);
        Log("attached body to pawn (copying 1P mesh transform each frame)");
    }
}

// Head shadow: Corvo's masked head (DLC07_skm_Corvo_Mask, same skeleton) as a second component that copies the
// body's pose (SetParentAnimComponent), is never rendered (HiddenGame) but still casts a shadow
// (bCastHiddenShadow) - so the shadow on the floor has a head, and nothing can clip into the camera.
static void CreateHeadShadow(ADishonoredPlayerPawn* pawn, USkeletalMeshComponent* body) {
    if (!g_cfg.headShadow || !g_headMesh || !pawn || !body) return;
    auto* head = static_cast<USkeletalMeshComponent*>(
        StaticConstructObject(USkeletalMeshComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
    if (!head) { Log("could not create the head shadow component"); return; }
    head->SetSkeletalMesh((g_builtChar == 1 && g_daudHeadMesh) ? g_daudHeadMesh : (g_builtChar == 2 && g_ovsHeadMesh) ? g_ovsHeadMesh : g_headMesh, false);
    head->bUpdateSkelWhenNotRendered = 1;   // it is never rendered, but its shadow still needs the pose
    if (pawn->Mesh && pawn->Mesh->LightEnvironment) head->SetLightEnvironment(pawn->Mesh->LightEnvironment);
    head->HiddenGame = 1;
    head->CastShadow = 1;
    head->bCastDynamicShadow = 1;
    head->bCastHiddenShadow = 1;
    // No SetParentAnimComponent: the pose copy ran frames behind and the head shadow trailed 20-37 uu while
    // moving (test 41 diag). The head is rigid, so its reference pose under the same transform as the body is enough.
    pawn->AttachComponent(head);
    head->SetHidden(true);
    g_head = head;
    Log("head shadow created: %p", head);
}

// Shadow body: a second, never-rendered copy of Corvo's body that casts the shadow. It runs its own copy of the
// locomotion tree (same per-frame inputs as the visible body) but never hides its coat or arms, so the shadow keeps
// the coat when crouched and has (fake, walk-cycle) arms while weapons are out. Its own tree rather than
// SetParentAnimComponent: a pose copy would also copy the visible body's shrunk coat bones.
static void CreateShadowBody(ADishonoredPlayerPawn* pawn) {
    if (!g_cfg.shadowBody || !g_bodyAnimated || !pawn) return;
    auto* shadow = static_cast<USkeletalMeshComponent*>(
        StaticConstructObject(USkeletalMeshComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
    if (!shadow) { Log("could not create the shadow body"); return; }
    shadow->SetSkeletalMesh(ActiveBodyMesh(), false);
    shadow->bUpdateSkelWhenNotRendered = 1;
    // Never rendered, so by default its SkelControls (foot IK, crouch knees, slide/climb poses) were skipped and
    // the crouched shadow stood straight-legged into the ground (video 14-09-17).
    shadow->bIgnoreControllersWhenNotRendered = 0;
    if (pawn->Mesh && pawn->Mesh->LightEnvironment) shadow->SetLightEnvironment(pawn->Mesh->LightEnvironment);
    shadow->HiddenGame = 1;
    shadow->CastShadow = 1;
    shadow->bCastDynamicShadow = 1;
    shadow->bCastHiddenShadow = 1;
    pawn->AttachComponent(shadow);
    if (!SetupBodyAnimation(shadow)) { Log("shadow body animation failed"); pawn->DetachComponent(shadow); return; }
    shadow->SetHidden(true);
    g_shadowBody = shadow;
    Log("shadow body created: %p", shadow);
}

static void DetachBody() {
    if (g_head && g_bodyPawn) g_bodyPawn->DetachComponent(g_head);
    if (g_shadowBody && g_bodyPawn) g_bodyPawn->DetachComponent(g_shadowBody);
    if (!g_body || !g_bodyPawn) return;
    if (g_bodyPawn->Mesh) g_bodyPawn->Mesh->DetachComponent(g_body);
    g_bodyPawn->DetachComponent(g_body);
}

static void DestroyBody() {
    if (!g_body) return;
    if (GetPlayerPawn() == g_bodyPawn) DetachBody();
    Log("body removed");
    g_body = nullptr;
    g_head = nullptr;
    g_shadowBody = nullptr;
    g_itemShadow[0] = g_itemShadow[1] = nullptr;   // owned by the pawn; dropped with it
        g_armsShadow = nullptr;
    g_bodyPawn = nullptr;
}

static void CreateBody(ADishonoredPlayerPawn* pawn) {
    USkeletalMeshComponent* arms = pawn->Mesh;
    if (!arms) { Log("player pawn has no Mesh yet"); return; }

    auto* comp = static_cast<USkeletalMeshComponent*>(
        StaticConstructObject(USkeletalMeshComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
    if (!comp) { Log("StaticConstructObject(SkeletalMeshComponent) failed"); return; }
    g_body = comp;
    g_bodyPawn = pawn;

    SelectCoatBones();
    comp->SetSkeletalMesh(ActiveBodyMesh(), false);
    comp->bUpdateSkelWhenNotRendered = 1;
    if (arms->LightEnvironment) comp->SetLightEnvironment(arms->LightEnvironment);
    // The visible body hides its coat (crouched) and busy arms, so it must not cast the shadow: a hidden
    // full copy (CreateShadowBody) does that instead.
    if (g_cfg.shadowBody || !g_cfg.playerShadows) { comp->CastShadow = 0; comp->bCastDynamicShadow = 0; }
    AttachBody(pawn);
    // Own locomotion animation if we already have the NPC set; otherwise copy the 1P pose for now.
    g_bodyAnimated = SetupBodyAnimation(comp);
    if (!g_bodyAnimated) {
        Log("no body animation yet, copying the 1P pose");
        comp->SetParentAnimComponent(arms);
        comp->UpdateParentBoneMap();
    }
    ApplyArmHiding();
    CreateShadowBody(pawn);
    CreateHeadShadow(pawn, g_shadowBody ? g_shadowBody : comp);

    Log("body created (%s): comp=%p mesh=%p parent=%p armsDepthGroup=%d armsLOD=%d", CharName(g_builtChar),
        comp, ActiveBodyMesh(), arms, static_cast<int>(arms->DepthPriorityGroup), arms->ForcedLodModel);
}

// ---------------------------------------------------------------------------------------------
// Body Swap. The body: Daud's body and head ship in the package we already load, so a swap just rebuilds our
// components (same steps as a pawn change, minus dropping the assets). The arms: Daud's first-person arms
// (same skeleton as Corvo's) live in his DLC player package, loaded on demand once our body is in use, and are
// put on the pawn's own 1P arms component; Corvo's mesh is remembered and put back when switching back.
// ---------------------------------------------------------------------------------------------
static const wchar_t* kDaudPackage = L"DLC07_Twk_Player_Daud_SF";
static const char* kDaudArmsMesh = "DLC07_Skm_Player_Daud";
static const char* kDaudSwordMesh = "DLC06_DaudSword";       // his player sword (DLC07_Inv_Sword_Daud)
static USkeletalMesh* g_daudSwordMesh = nullptr;
static const char* kDaudBowMesh = "DLC06_WristBow";          // his wristbow (DLC07_Inv_WristbowDaud)
static USkeletalMesh* g_daudBowMesh = nullptr;
static volatile LoadState g_daudLoadState = LoadState::Idle;
static DWORD g_daudLoadStartTick = 0;
static DWORD g_bodyBuiltTick = 0;

// One swapped component (1P arms, 1P sword): the game's own mesh is remembered and put back when switching back.
struct MeshSwap {
    USkeletalMeshComponent* comp = nullptr;   // the component the state below belongs to
    USkeletalMesh* orig = nullptr;            // the game's mesh for it ("Corvo's")
    UMaterialInterface* mats[8] = {};         // its per-component material overrides, cleared while Daud's is on
    bool matWasRooted[8] = {};                // rooted by someone else already (then we must not un-root it)
    int matCount = -1;                        // -1: nothing saved
};
static MeshSwap g_armsSwap, g_swordSwap, g_bowSwap;

// A saved override is referenced by nothing once it is off the component (they are transient material instances),
// so the next GC freed it and putting it back crashed in GetMaterialViewRelevance (build 153). Root it while held.
static void HoldMaterial(MeshSwap& s, int i, UMaterialInterface* m) {
    if (s.mats[i] && s.mats[i] != m && !s.matWasRooted[i]) LetGo(s.mats[i]);
    s.mats[i] = m;
    s.matWasRooted[i] = m && (*reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(m) + 0x08) & 0x4000ULL);
    KeepAlive(m);
}

static void ReleaseMaterials(MeshSwap& s) {
    for (int i = 0; i < s.matCount && i < 8; ++i) if (s.mats[i] && !s.matWasRooted[i]) LetGo(s.mats[i]);
    s.matCount = -1;
}

static void GrabDaudTweaks();

static void __cdecl OnDaudPackageLoaded(UObject* package, void*) {
    Log("Daud package load finished: %s", package ? "ok" : "FAILED");
    if (package) {
        g_daudArmsMesh = FindLoadedMesh(kDaudArmsMesh);
        g_daudSwordMesh = FindLoadedMesh(kDaudSwordMesh);
        g_daudBowMesh = FindLoadedMesh(kDaudBowMesh);
    }
    Protect(g_daudArmsMesh);
    Protect(g_daudSwordMesh);
    Protect(g_daudBowMesh);
    if (package) GrabDaudTweaks();
    Log("Daud arms mesh: %s, Daud sword mesh: %s", g_daudArmsMesh ? g_daudArmsMesh->GetFullName().c_str() : "NOT FOUND",
        g_daudSwordMesh ? g_daudSwordMesh->GetFullName().c_str() : "NOT FOUND");
    g_daudLoadState = (g_daudArmsMesh || g_daudSwordMesh) ? LoadState::Loaded : LoadState::Failed;
}

// Overseer disguise (build 200): see g_ovsBodyMesh. A map script package (19 MB): its meshes are rooted inside the
// load callback like everything else we take from a package (the post-load GC purges whatever nothing references).
static const wchar_t* kOvsPackage = L"L_DLC07_BaseIntro_Script";
static volatile LoadState g_ovsLoadState = LoadState::Idle;
static DWORD g_ovsLoadStartTick = 0;

static void __cdecl OnOvsPackageLoaded(UObject* package, void*) {
    Log("Overseer package load finished: %s", package ? "ok" : "FAILED");
    if (package) {
        g_ovsBodyMesh = FindLoadedMesh("Skm_Overseer_Body");
        g_ovsHeadMesh = FindLoadedMesh("Skm_Overseer_Head2");
        g_ovsArmsMesh = FindLoadedMesh("DLC07_Skm_Player_DaudOverseer");
        g_ovsSwordMesh = FindLoadedMesh("OverseerSword");
    }
    for (USkeletalMesh* m : { g_ovsBodyMesh, g_ovsHeadMesh, g_ovsArmsMesh, g_ovsSwordMesh }) Protect(m);
    auto name = [](USkeletalMesh* m) { return m ? m->GetFullName() : std::string("NOT FOUND"); };
    Log("Overseer meshes: body %s, head %s, arms %s, sword %s", name(g_ovsBodyMesh).c_str(), name(g_ovsHeadMesh).c_str(),
        name(g_ovsArmsMesh).c_str(), name(g_ovsSwordMesh).c_str());
    g_ovsLoadState = (g_ovsBodyMesh || g_ovsArmsMesh || g_ovsSwordMesh) ? LoadState::Loaded : LoadState::Failed;
}

static void ForgetOverseer() {
    for (USkeletalMesh** m : { &g_ovsBodyMesh, &g_ovsHeadMesh, &g_ovsArmsMesh, &g_ovsSwordMesh }) { LetGo(*m); *m = nullptr; }
    if (g_ovsLoadState != LoadState::Loading) g_ovsLoadState = LoadState::Idle;
}

static void ForgetPowerSwaps();

static void ForgetArmsSwap() {
    ForgetOverseer();
    ForgetPowerSwaps();
    LetGo(g_daudArmsMesh);
    LetGo(g_daudSwordMesh);
    LetGo(g_daudBowMesh);
    g_daudArmsMesh = nullptr;
    g_daudSwordMesh = nullptr;
    g_daudBowMesh = nullptr;
    ReleaseMaterials(g_armsSwap);
    ReleaseMaterials(g_swordSwap);
    ReleaseMaterials(g_bowSwap);
    g_armsSwap = MeshSwap();
    g_swordSwap = MeshSwap();
    g_bowSwap = MeshSwap();
    if (g_daudLoadState != LoadState::Loading) g_daudLoadState = LoadState::Idle;
}

// Before CreateBody: if the wanted body differs from the built one, drop our components so CreateBody rebuilds.
static void RebuildBodyIfCharacterChanged(ADishonoredPlayerPawn* pawn) {
    if (!g_body || g_bodyPawn != pawn) return;
    int want = WantedBodyChar();
    if (want == g_builtChar) return;
    Log("Body Swap: rebuilding the body as %s", CharName(want));
    DetachBody();
    // As on a pawn change: the old components are purged by a later GC, which must not free our buffers.
    if (g_shadowBody && g_shadowBody->Animations) DetachNodeArrays(g_shadowBody->Animations, 0);
    DetachOurArrays(g_body);
    g_body = nullptr;
    g_head = nullptr;
    g_shadowBody = nullptr;
    g_bodyPawn = nullptr;
}

// Puts an alternative mesh (Daud's or the Overseer's) on comp, or the game's own back (alt null). alts lists every
// alternative the component can carry. Returns the mesh now on it.
static USkeletalMesh* ApplyMeshSwap(USkeletalMeshComponent* comp, USkeletalMesh* alt, std::initializer_list<USkeletalMesh*> alts,
                                    MeshSwap& s, const char* what) {
    if (!comp || !comp->SkeletalMesh) return nullptr;
    if (comp != s.comp) { ReleaseMaterials(s); s = MeshSwap(); s.comp = comp; }   // a new component starts out with the game's mesh
    USkeletalMesh* cur = comp->SkeletalMesh;
    // Build 200: two alternatives (Daud's, the Overseer's) - neither may be taken for the game's own mesh.
    bool curIsAlt = false;
    for (USkeletalMesh* a : alts) if (a && cur == a) curIsAlt = true;
    if (!curIsAlt) s.orig = cur;   // whatever the game gives it is "Corvo's"
    USkeletalMesh* want = alt ? alt : s.orig;
    auto* mats = reinterpret_cast<RawArray*>(&comp->Materials);
    if (want && cur != want) {
        if (!curIsAlt && want != s.orig) {
            // Per-component overrides were made for Corvo's mesh; left on, they would paint his textures on the other.
            // Held only when leaving the game's mesh (Daud -> Overseer: they are already held, the slots are empty).
            s.matCount = (mats->Data && mats->Num > 0) ? (mats->Num < 8 ? mats->Num : 8) : 0;
            for (int i = 0; i < s.matCount; ++i) {
                HoldMaterial(s, i, static_cast<UMaterialInterface**>(mats->Data)[i]);
                Log("  %s material override %d: %s", what, i, s.mats[i] ? s.mats[i]->GetFullName().c_str() : "none");
                if (s.mats[i]) comp->SetMaterial(i, nullptr);
            }
        }
        Log("Body Swap: %s %s -> %s", what, cur->GetName().c_str(), want->GetName().c_str());
        comp->SetSkeletalMesh(want, false);
        if (want == s.orig && s.matCount > 0) {
            for (int i = 0; i < s.matCount; ++i) if (s.mats[i]) comp->SetMaterial(i, s.mats[i]);
            ReleaseMaterials(s);
        }
        cur = want;
    } else if (curIsAlt && mats->Data && mats->Num > 0) {
        // The game set an override while an alternative mesh is on (e.g. blood on the blade): keep it off it.
        for (int i = 0; i < mats->Num && i < 8; ++i) {
            auto* m = static_cast<UMaterialInterface**>(mats->Data)[i];
            if (!m) continue;
            if (s.matCount < i + 1) { for (int k = s.matCount < 0 ? 0 : s.matCount; k <= i; ++k) s.mats[k] = nullptr; s.matCount = i + 1; }
            HoldMaterial(s, i, m);
            comp->SetMaterial(i, nullptr);
        }
    }
    return cur;
}

// The first-person mesh component of the first inventory item whose class name contains "what".
static USkeletalMeshComponent* FindItemMesh(ADishonoredPlayerPawn* pawn, const char* what) {
    if (!pawn || !pawn->m_pInventory) return nullptr;
    auto* slots = reinterpret_cast<RawArray*>(&pawn->m_pInventory->m_Slots);
    if (!slots->Data || slots->Num <= 0 || slots->Num > 64) return nullptr;
    for (int i = 0; i < slots->Num; ++i) {
        auto* item = *reinterpret_cast<UDishonoredInventoryItem**>(static_cast<uint8_t*>(slots->Data) + i * 12);
        if (item && item->Class && item->m_pPlayerMesh && item->Class->GetName().find(what) != std::string::npos) return item->m_pPlayerMesh;
    }
    return nullptr;
}

// The player's sword item (its first-person mesh component), if the pawn has one.
static USkeletalMeshComponent* FindSwordMesh(ADishonoredPlayerPawn* pawn) {
    if (!pawn || !pawn->m_pInventory) return nullptr;
    auto* slots = reinterpret_cast<RawArray*>(&pawn->m_pInventory->m_Slots);
    if (!slots->Data || slots->Num <= 0 || slots->Num > 64) return nullptr;
    for (int i = 0; i < slots->Num; ++i) {
        auto* item = *reinterpret_cast<UDishonoredInventoryItem**>(static_cast<uint8_t*>(slots->Data) + i * 12);
        if (!item || !item->Class || !item->m_pPlayerMesh) continue;
        if (item->Class->GetName().find("Sword") != std::string::npos) return item->m_pPlayerMesh;
    }
    return nullptr;
}

// ---- Power set recon (diagnostic): how the game wires Corvo's active powers, and what Daud's look like ----
static std::string ObjName(UObject* o) { return o ? o->GetFullName() : std::string("null"); }

static void DumpPowerComponent(const char* tag, UDishonoredActivePowerComponent* c) {
    if (!c) { Log("  %s: null", tag); return; }
    Log("  %s: %p %s arch=%s outer=%s", tag, c, ObjName(c).c_str(), ObjName(c->ObjectArchetype).c_str(), ObjName(c->Outer).c_str());
    Log("     name=%s ui=%d level=%d tweaks=%s ownerPawn=%p ownerPlayer=%p powersComp=%p flags=%08X%08X", c->m_PowerName.ToString().c_str(),
        static_cast<int>(c->m_eUISelection), c->m_CurrentLevel, ObjName(c->m_pPowerTweaks).c_str(), c->m_pOwnerPawn,
        c->m_pOwnerPlayerPawn, c->m_pOwnerPowersComponent, *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(c) + 0x0C),
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(c) + 0x08));
}

static void DumpPowers(ADishonoredPlayerPawn* pawn) {
    if (!pawn) return;
    Log("=== powers: pawn %s class %s size? tweaks=%s", ObjName(pawn).c_str(), ObjName(pawn->Class).c_str(), ObjName(pawn->m_pPawnTweaks).c_str());
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    for (int i = 0; arr->Data && i < arr->Num && i < 16; ++i) {
        char tag[16]; sprintf(tag, "active[%d]", i);
        DumpPowerComponent(tag, static_cast<UDishonoredActivePowerComponent**>(arr->Data)[i]);
    }
    Log("  typed: blink=%p possess=%p darkVision=%p swarm=%p", pawn->m_pBlinkPower, pawn->m_pPossessPower,
        pawn->m_pDarkVisionPower, pawn->m_pDevouringSwarmPower);
    if (UDishonoredPowersComponent* pc = pawn->m_pPowersComponent) {
        auto* pw = reinterpret_cast<RawArray*>(&pc->m_Powers);
        Log("  powersComponent %p %s: %d powers", pc, ObjName(pc).c_str(), pw->Num);
        for (int i = 0; pw->Data && i < pw->Num && i < 32; ++i) {
            auto& p = static_cast<FDishonoredPower*>(pw->Data)[i];
            Log("     [%d] %s ui=%d levels=%d current=%d", i, p.m_Name.ToString().c_str(), static_cast<int>(p.m_eUISelection),
                reinterpret_cast<RawArray*>(&p.m_Levels)->Num, p.m_CurrentLevel);
        }
    }
    auto* usable = reinterpret_cast<RawArray*>(&pawn->m_UsableActivePowerIndices);
    std::string idx;
    for (int i = 0; usable->Data && i < usable->Num && i < 16; ++i) idx += std::to_string(static_cast<int*>(usable->Data)[i]) + " ";
    Log("  usable active indices: %s (first acquired %d), mana %d/%d", idx.c_str(), pawn->m_iFirstAcquiredActivePower, pawn->m_Mana, pawn->m_ManaMax);
    if (pawn->m_pInventory) {
        auto* slots = reinterpret_cast<RawArray*>(&pawn->m_pInventory->m_Slots);
        for (int i = 0; slots->Data && i < slots->Num && i < 64; ++i) {
            auto* item = *reinterpret_cast<UDishonoredInventoryItem**>(static_cast<uint8_t*>(slots->Data) + i * 12);
            if (!item || !item->Class) continue;
            std::string cls = item->Class->GetName();
            Log("  slot %d: %s (%s) playerMesh=%s", i, cls.c_str(), ObjName(item).c_str(),
                item->m_pPlayerMesh && item->m_pPlayerMesh->SkeletalMesh ? ObjName(item->m_pPlayerMesh->SkeletalMesh).c_str() : "-");
            if (cls == "DisItemPowers") {
                auto* ip = static_cast<UDisItemPowers*>(item);
                Log("     current power %p slot %d tweaks %s", ip->m_pCurrentActivePower, ip->m_iPowerSlot, ObjName(ip->m_pPowersItemTweaks).c_str());
            }
        }
    }
}

static void DumpDaudPowers() {
    Log("=== Daud power data");
    const char* cdos[][2] = {
        { "DisDLC06ActivePowerComponent_Transversal", "Default__DisDLC06ActivePowerComponent_Transversal" },
        { "DisDLC06ActivePowerComponent_DarkVision", "Default__DisDLC06ActivePowerComponent_DarkVision" },
        { "DisDLC06ActivePowerComponent_SummonAssassin", "Default__DisDLC06ActivePowerComponent_SummonAssassin" },
        { "DisDLC07ActivePowerComponent_Pull", "Default__DisDLC07ActivePowerComponent_Pull" },
        { "DishonoredActivePowerComponent_Blink", "Default__DishonoredActivePowerComponent_Blink" } };
    for (auto& c : cdos) DumpPowerComponent(c[1], static_cast<UDishonoredActivePowerComponent*>(FindObjectByName(c[0], c[1])));
    auto* daud = static_cast<UDisDLC07Tweaks_PlayerPawn*>(FindObjectByName("DisDLC07Tweaks_PlayerPawn", "DLC07_Twk_Player_Daud"));
    Log("  Daud pawn tweaks %s", ObjName(daud).c_str());
    if (daud) {
        Log("     summon=%s transversal=%s darkVision=%s bendTime=%s pull=%s powersItem=%s", ObjName(daud->m_pSummonAssassinPowerTweaks).c_str(),
            ObjName(daud->m_pTransversalPowerTweaks).c_str(), ObjName(daud->m_pDarkVisionPowerTweaks).c_str(),
            ObjName(daud->m_pBendTimePowerTweaks).c_str(), ObjName(daud->m_pPullPowerTweaks).c_str(), ObjName(daud->m_pPowersItem).c_str());
        auto* pp = reinterpret_cast<RawArray*>(&daud->m_PassivePowers);
        for (int i = 0; pp->Data && i < pp->Num && i < 32; ++i) {
            auto& p = static_cast<FDishonoredPower*>(pp->Data)[i];
            Log("     passive [%d] %s ui=%d levels=%d", i, p.m_Name.ToString().c_str(), static_cast<int>(p.m_eUISelection),
                reinterpret_cast<RawArray*>(&p.m_Levels)->Num);
        }
    }
}

// Sword recon: every sword item tweak in memory (Corvo's and Daud's), what their player-specific part drives (arm
// anim sets, picker, anim states), and where the live sword item keeps its tweaks pointer.
static void DumpSwordTweaks(ADishonoredPlayerPawn* pawn) {
    Log("=== sword tweaks");
    std::vector<UObject*> tweaks;
    auto* objects = UObject::GObjObjects();
    for (int i = 0; i < objects->size(); ++i) {
        UObject* o = objects->at(i);
        if (!o || !o->Class) continue;
        std::string cn = o->Class->GetName();
        if (cn != "DisTweaks_WepSword" && cn != "DisTweaks_WepSwordOfAssassin" && cn != "DisDLC07Tweaks_WepSwordOfAssassin") continue;
        if (o->GetName().rfind("Default__", 0) == 0) continue;
        tweaks.push_back(o);
        auto* t = static_cast<UDisTweaks_InventoryItem*>(o);
        Log("  %p %s prefix=%s", o, ObjName(o).c_str(), t->m_ItemPrefix.ToString().c_str());
        auto* ps = t->m_pPlayerSpecificTweaks;
        if (!ps) continue;
        Log("     player: %s mesh=%s picker=%s default=%s tree=%s", ObjName(ps).c_str(), ObjName(ps->m_pEquippedMesh).c_str(),
            ps->m_AnimStatePickerName.ToString().c_str(), ps->m_DefaultAnimState.ToString().c_str(), ObjName(ps->m_pItemAnimTree).c_str());
        for (auto* arrp : { &ps->m_PawnItemAnimSets, &ps->m_ItemAnimSets }) {
            auto* a = reinterpret_cast<RawArray*>(arrp);
            std::string names;
            for (int k = 0; a->Data && k < a->Num && k < 16; ++k) names += ObjName(static_cast<UObject**>(a->Data)[k]) + "; ";
            Log("     %s: %s", arrp == &ps->m_PawnItemAnimSets ? "pawn anim sets" : "item anim sets", names.c_str());
        }
        auto* st = reinterpret_cast<RawArray*>(&ps->m_AnimStates_PawnSync);
        for (int k = 0; st->Data && k < st->Num && k < 24; ++k) {
            auto& s = reinterpret_cast<FDisAnimState*>(static_cast<uint8_t*>(st->Data))[k];
            std::string seqs;
            auto* cs = reinterpret_cast<RawArray*>(&s.m_CustomSequences);
            for (int q = 0; cs->Data && q < cs->Num && q < 8; ++q) {
                auto& c = static_cast<FDisAnimState_CustomSeq*>(cs->Data)[q];
                seqs += c.m_Alias.ToString() + "=" + c.m_Sequence.ToString() + " ";
            }
            Log("     sync state %s tree=%s %s", s.m_StateName.ToString().c_str(), s.m_AnimStateTree.ToString().c_str(), seqs.c_str());
        }
    }
    UDishonoredInventoryItem* sword = nullptr;
    if (pawn && pawn->m_pInventory) {
        auto* slots = reinterpret_cast<RawArray*>(&pawn->m_pInventory->m_Slots);
        for (int i = 0; slots->Data && i < slots->Num && i < 64; ++i) {
            auto* item = *reinterpret_cast<UDishonoredInventoryItem**>(static_cast<uint8_t*>(slots->Data) + i * 12);
            if (item && item->Class && item->Class->GetName().find("Sword") != std::string::npos) sword = item;
        }
    }
    if (!sword) { Log("  no sword item"); return; }
    Log("  sword item %s arch=%s", ObjName(sword).c_str(), ObjName(sword->ObjectArchetype).c_str());
    for (int off = 0x38; off < 0x200; off += 4) {
        UObject* p = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(sword) + off);
        for (UObject* t : tweaks) if (p == t) Log("  sword item +0x%X -> %s", off, ObjName(t).c_str());
    }
    if (USkeletalMeshComponent* m = sword->m_pPlayerMesh) {
        auto* sets = reinterpret_cast<RawArray*>(&m->AnimSets);
        std::string names;
        for (int k = 0; sets->Data && k < sets->Num && k < 16; ++k) names += ObjName(static_cast<UObject**>(sets->Data)[k]) + "; ";
        Log("  sword mesh comp anim sets: %s tree=%s", names.c_str(), ObjName(m->Animations).c_str());
    }
}

// Crossbow recon (build 179): Corvo's handheld crossbow vs Daud's wristbow - every crossbow item tweak in memory with
// its prefix, sockets, player flags and player-specific anim setup, plus where the live crossbow item keeps its
// tweaks pointer and what its first-person mesh component plays.
static void DumpCrossbowTweaks(ADishonoredPlayerPawn* pawn) {
    Log("=== crossbow tweaks");
    std::vector<UObject*> tweaks;
    auto* objects = UObject::GObjObjects();
    for (int i = 0; i < objects->size(); ++i) {
        UObject* o = objects->at(i);
        if (!o || !o->Class) continue;
        std::string cn = o->Class->GetName();
        if (cn.find("Tweaks_WepCrossbow") == std::string::npos) continue;
        if (o->GetName().rfind("Default__", 0) == 0) continue;
        tweaks.push_back(o);
        auto* t = static_cast<UDisTweaks_InventoryItem*>(o);
        std::string sockets;
        for (int k = 0; k < 4; ++k) sockets += t->m_Sockets[k].ToString() + " ";
        Log("  %p %s (%s) prefix=%s npcPrefix=%s sockets=[%s]", o, ObjName(o).c_str(), cn.c_str(), t->m_ItemPrefix.ToString().c_str(),
            t->m_ItemPrefix_NPCOnly.ToString().c_str(), sockets.c_str());
        auto* ps = t->m_pPlayerSpecificTweaks;
        if (!ps) continue;
        Log("     player: %s flags %08X mesh=%s picker=%s default=%s tree=%s", ObjName(ps).c_str(),
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(ps) + 0x114), ObjName(ps->m_pEquippedMesh).c_str(),
            ps->m_AnimStatePickerName.ToString().c_str(), ps->m_DefaultAnimState.ToString().c_str(), ObjName(ps->m_pItemAnimTree).c_str());
        for (auto* arrp : { &ps->m_PawnItemAnimSets, &ps->m_ItemAnimSets }) {
            auto* a = reinterpret_cast<RawArray*>(arrp);
            std::string names;
            for (int k = 0; a->Data && k < a->Num && k < 16; ++k) names += ObjName(static_cast<UObject**>(a->Data)[k]) + "; ";
            Log("     %s: %s", arrp == &ps->m_PawnItemAnimSets ? "pawn anim sets" : "item anim sets", names.c_str());
        }
        auto* st = reinterpret_cast<RawArray*>(&ps->m_AnimStates_PawnSync);
        for (int k = 0; st->Data && k < st->Num && k < 24; ++k) {
            auto& s = reinterpret_cast<FDisAnimState*>(static_cast<uint8_t*>(st->Data))[k];
            std::string seqs;
            auto* cs = reinterpret_cast<RawArray*>(&s.m_CustomSequences);
            for (int q = 0; cs->Data && q < cs->Num && q < 8; ++q) {
                auto& c = static_cast<FDisAnimState_CustomSeq*>(cs->Data)[q];
                seqs += c.m_Alias.ToString() + "=" + c.m_Sequence.ToString() + " ";
            }
            Log("     sync state %s tree=%s %s", s.m_StateName.ToString().c_str(), s.m_AnimStateTree.ToString().c_str(), seqs.c_str());
        }
    }
    UDishonoredInventoryItem* bow = nullptr;
    if (pawn && pawn->m_pInventory) {
        auto* slots = reinterpret_cast<RawArray*>(&pawn->m_pInventory->m_Slots);
        for (int i = 0; slots->Data && i < slots->Num && i < 64; ++i) {
            auto* item = *reinterpret_cast<UDishonoredInventoryItem**>(static_cast<uint8_t*>(slots->Data) + i * 12);
            if (item && item->Class && item->Class->GetName().find("Crossbow") != std::string::npos) bow = item;
        }
    }
    if (!bow) { Log("  no crossbow item"); return; }
    Log("  crossbow item %s (%s) arch=%s socket %d", ObjName(bow).c_str(), bow->Class->GetName().c_str(), ObjName(bow->ObjectArchetype).c_str(),
        static_cast<int>(bow->m_CurSocket));
    for (int off = 0x38; off < 0x200; off += 4) {
        UObject* p = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(bow) + off);
        for (UObject* t : tweaks) if (p == t) Log("  crossbow item +0x%X -> %s", off, ObjName(t).c_str());
    }
    if (USkeletalMeshComponent* m = bow->m_pPlayerMesh) {
        auto* sets = reinterpret_cast<RawArray*>(&m->AnimSets);
        std::string names;
        for (int k = 0; sets->Data && k < sets->Num && k < 16; ++k) names += ObjName(static_cast<UObject**>(sets->Data)[k]) + "; ";
        Log("  crossbow mesh comp: mesh %s anim sets: %s tree=%s", ObjName(m->SkeletalMesh).c_str(), names.c_str(), ObjName(m->Animations).c_str());
    }
}

static void DumpPowersOnce(ADishonoredPlayerPawn* pawn) {
    static ADishonoredPlayerPawn* dumpedFor = nullptr;
    static bool daudDumped = false;
    if (pawn != dumpedFor && g_bodyBuiltTick && GetTickCount() - g_bodyBuiltTick > 8000) { dumpedFor = pawn; DumpPowers(pawn); }
    if (!daudDumped && g_daudLoadState == LoadState::Loaded) { daudDumped = true; DumpDaudPowers(); DumpCrossbowTweaks(pawn); }
}

// ---- Power set. Each active power is a component in pawn->m_ActivePowers, instanced from the pawn's class templates
// (Default__DishonoredPlayerPawn.PowerBlink, ...). AddPower and the save go by m_PowerName, the journal matches its
// slots (config m_ActivePowerSlots) to components by m_eUISelection and reads the text by name (RPG.int), and the
// wheel holds at most 6 usable powers (OnActivePowerLevelChanged). Daud's set (user's choice; Pull dropped in build 172) swaps two
// of Corvo's six for Daud's own, with Daud's name and wheel slot so the journal shows his power and it levels up on
// its own: Blink -> Daud's Blink (Transversal), Windblast -> Pull, Devouring Swarm -> Summon Assassin. Corvo's
// originals are kept (rooted) and put back for Corvo's set; Daud's are kept (rooted) too while Corvo's set is on so
// both sets' levels go into every save (see HookedPowersGameSave).
struct PowerSwap {
    const char* corvoName;
    const char* daudClass;
    const char* daudName;                        // Daud's power name (his CDO default; RPG.int text key)
    int tweak;                                   // which of Daud's power tweaks: 0 transversal 1 void gaze 2 summon 3 pull
    uint8_t corvoUi, daudUi;                     // EeDisUISelectionType of each (journal slot types)
    UDishonoredActivePowerComponent* orig;       // Corvo's component (detached while Daud's is in)
    UDishonoredActivePowerComponent* daud;       // Daud's (transient package), in m_ActivePowers while his set is on
    void* daudPc;                                // powers component of the pawn it was made for
    void* pendingPc;                             // a level for it read from a save, for this powers component
    int pendingLevel;
    bool loggedMissing;
};
static PowerSwap g_powerSwaps[] = {
    { "Blink",          "DisDLC06ActivePowerComponent_Transversal",    "Transversal",    0, 2, 20 },
    { "DevouringSwarm", "DisDLC06ActivePowerComponent_SummonAssassin", "SummonAssassin", 2, 7, 21 },
};
constexpr int kPowerSwapCount = sizeof(g_powerSwaps) / sizeof(g_powerSwaps[0]);
static ADishonoredPlayerPawn* g_powerPawn = nullptr;
static UDisDLC07Tweaks_PlayerPawn* g_daudPawnTweaks = nullptr;

// Native vtable slots of UDishonoredActivePowerComponent (from its vftable @0x1167F60): IsActive, Cancel, GameSave.
constexpr int kVtIsActive = 0x160 / 4, kVtCancel = 0x170 / 4, kVtGameSave = 69;
using PowerBoolFn = unsigned(__thiscall*)(void*);
using PowerVoidFn = void(__thiscall*)(void*);
using GameSaveFn = void(__thiscall*)(void*, void*, int);

static void CancelPowerIfActive(UDishonoredActivePowerComponent* c) {
    if (!c) return;
    void** vt = *reinterpret_cast<void***>(c);
    if (reinterpret_cast<PowerBoolFn>(vt[kVtIsActive])(c)) {
        Log("  cancelling active power %s first", c->Class->GetName().c_str());
        reinterpret_cast<PowerVoidFn>(vt[kVtCancel])(c);
    }
}

// Saves: while one of ours is in, its per-component save data is written by Corvo's original instead, so a save
// made with Daud's powers loads cleanly into a pawn built from Corvo's templates (and with Daud's off again).
static GameSaveFn g_daudOrigSave[4];
template <int N> static void __fastcall DaudGameSave(void* self, void*, void* ar, int loc) {
    for (auto& s : g_powerSwaps) {
        if (s.daud == self && s.orig) {
            s.orig->m_CurrentLevel = s.daud->m_CurrentLevel;
            reinterpret_cast<GameSaveFn>((*reinterpret_cast<void***>(s.orig))[kVtGameSave])(s.orig, ar, loc);
            return;
        }
    }
    g_daudOrigSave[N](self, ar, loc);
}
static void* const kDaudSaveThunks[4] = { reinterpret_cast<void*>(&DaudGameSave<0>), reinterpret_cast<void*>(&DaudGameSave<1>),
                                          reinterpret_cast<void*>(&DaudGameSave<2>), reinterpret_cast<void*>(&DaudGameSave<3>) };

static void HookDaudSave(int i, UObject* daudComp) {
    void** vt = *reinterpret_cast<void***>(daudComp);
    if (vt[kVtGameSave] == kDaudSaveThunks[i]) return;
    g_daudOrigSave[i] = reinterpret_cast<GameSaveFn>(vt[kVtGameSave]);
    DWORD old;
    if (VirtualProtect(&vt[kVtGameSave], sizeof(void*), PAGE_READWRITE, &old)) {
        vt[kVtGameSave] = kDaudSaveThunks[i];
        VirtualProtect(&vt[kVtGameSave], sizeof(void*), old, &old);
        Log("  save redirect installed for %s (vtable %p, was %p)", daudComp->Class->GetName().c_str(), vt, g_daudOrigSave[i]);
    }
}

// Grabbed (and rooted) the moment his package loads: nothing in a Corvo level references Daud's tweaks, so the
// post-load GC purged them and the power set found nothing to swap in (build 154: "powers -> Daud" did nothing).
// Rooting the pawn tweaks keeps everything it references (his power tweaks, summoned assassin archetypes) alive.
static void GrabDaudTweaks() {
    if (!g_daudPawnTweaks)
        g_daudPawnTweaks = static_cast<UDisDLC07Tweaks_PlayerPawn*>(FindObjectByName("DisDLC07Tweaks_PlayerPawn", "DLC07_Twk_Player_Daud"));
    Protect(g_daudPawnTweaks);
    Log("Daud pawn tweaks: %s", ObjName(g_daudPawnTweaks).c_str());
}

static UDisTweaks_ActivePowerBase* DaudPowerTweaks(int which) {
    if (!g_daudPawnTweaks) return nullptr;
    Protect(g_daudPawnTweaks);
    switch (which) {
    case 0: return g_daudPawnTweaks->m_pTransversalPowerTweaks;
    case 1: return g_daudPawnTweaks->m_pDarkVisionPowerTweaks;
    case 2: return g_daudPawnTweaks->m_pSummonAssassinPowerTweaks;
    case 3: return g_daudPawnTweaks->m_pPullPowerTweaks;
    }
    return nullptr;
}

// Point the pawn's typed pointers and the equipped powers item at the replacement (when its type allows).
static void RetargetPowerPointers(ADishonoredPlayerPawn* pawn, UDishonoredActivePowerComponent* from, UDishonoredActivePowerComponent* to) {
    if (pawn->m_pBlinkPower == from && to->IsA(UDishonoredActivePowerComponent_Blink::StaticClass()))
        pawn->m_pBlinkPower = static_cast<UDishonoredActivePowerComponent_Blink*>(to);
    if (pawn->m_pDarkVisionPower == from && to->IsA(UDisActivePowerComponent_DarkVision::StaticClass()))
        pawn->m_pDarkVisionPower = static_cast<UDisActivePowerComponent_DarkVision*>(to);
    if (!pawn->m_pInventory) return;
    auto* slots = reinterpret_cast<RawArray*>(&pawn->m_pInventory->m_Slots);
    for (int i = 0; slots->Data && i < slots->Num && i < 64; ++i) {
        auto* item = *reinterpret_cast<UDishonoredInventoryItem**>(static_cast<uint8_t*>(slots->Data) + i * 12);
        if (item && item->Class && item->IsA(UDisItemPowers::StaticClass())) {
            auto* ip = static_cast<UDisItemPowers*>(item);
            if (ip->m_pCurrentActivePower == from) ip->m_pCurrentActivePower = to;
        }
    }
}

static void ForgetCorvoArmsMaterials();
static void SetPullTarget(void* pawn, void* pull);
static void ForgetBowItems();

static void ForgetPowerSwaps() {
    for (auto& s : g_powerSwaps) { LetGo(s.orig); LetGo(s.daud); s.orig = s.daud = nullptr; s.daudPc = nullptr; }
    LetGo(g_daudPawnTweaks);
    g_daudPawnTweaks = nullptr;
    SetPullTarget(nullptr, nullptr);
    ForgetBowItems();
    ForgetCorvoArmsMaterials();
}

// ---- Save-game safety for the power set (build 157). A save made with Daud's components in referenced them: the
// inventory saves the equipped power as an object reference (GameSave_Inventory @0xAAD2A0: ar << m_pCurrentActivePower)
// and our components lived under the pawn, so the level save carried them. Loading such a save rebuilt one without
// an owner and the "power equipped" event read its null owner pawn (UDisItemPowers::SetCurrentPower ->
// sub_BF30C0 +0x3F, crash 11:39). Now: our components live in the transient package (never saved), the equipped
// power is saved as Corvo's original, and SetCurrentPower repairs (and logs) any owner-less component it is handed.
static UObject* TransientPackage() {
    static UObject* pkg = nullptr;
    if (!pkg) pkg = FindObjectByName("Package", "Transient");
    return pkg;
}

static UDisItemPowers* FindItemPowers(ADishonoredPawn* pawn) {
    if (!pawn || !pawn->m_pInventory) return nullptr;
    auto* slots = reinterpret_cast<RawArray*>(&pawn->m_pInventory->m_Slots);
    for (int i = 0; slots->Data && i < slots->Num && i < 64; ++i) {
        auto* item = *reinterpret_cast<UDishonoredInventoryItem**>(static_cast<uint8_t*>(slots->Data) + i * 12);
        if (item && item->Class && item->IsA(UDisItemPowers::StaticClass())) return static_cast<UDisItemPowers*>(item);
    }
    return nullptr;
}

static UObject* FindOwnPower(ADishonoredPawn* pawn, int slot);

using GameSaveInventory_t = void(__thiscall*)(void*, void*, int);
static GameSaveInventory_t g_origGameSaveInventory = nullptr;
static void __fastcall HookedGameSaveInventory(ADishonoredPlayerPawn* pawn, void*, void* ar, int loc) {
    UDisItemPowers* ip = FindItemPowers(pawn);
    UDishonoredActivePowerComponent* ours = nullptr;
    if (ip && ip->m_pCurrentActivePower && ip->m_pCurrentActivePower->Outer != pawn) {
        // A Daud component (transient): save the pawn's own component of that slot instead.
        UDishonoredActivePowerComponent* own = nullptr;
        for (auto& s : g_powerSwaps) if (s.daud == ip->m_pCurrentActivePower && s.orig) own = s.orig;
        if (!own) {
            auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
            for (int i = 0; arr->Data && i < arr->Num; ++i)
                if (static_cast<UObject**>(arr->Data)[i] == ip->m_pCurrentActivePower)
                    own = static_cast<UDishonoredActivePowerComponent*>(FindOwnPower(pawn, i));
        }
        ours = ip->m_pCurrentActivePower;
        ip->m_pCurrentActivePower = own;   // null if not found: the load then simply equips nothing
        Log("save: equipped power written as %s", ObjName(own).c_str());
    }
    g_origGameSaveInventory(pawn, ar, loc);
    if (ours) ip->m_pCurrentActivePower = ours;
}

using SetCurrentPower_t = unsigned(__thiscall*)(void*, UDishonoredActivePowerComponent*, unsigned);
static SetCurrentPower_t g_origSetCurrentPower = nullptr;
static unsigned __fastcall HookedSetCurrentPower(UDisItemPowers* self, void*, UDishonoredActivePowerComponent* comp, unsigned flag) {
    if (comp && !comp->m_pOwnerPlayerPawn) {
        ADishonoredPlayerPawn* pawn = nullptr;
        if (comp->Outer && comp->Outer->IsA(ADishonoredPlayerPawn::StaticClass())) pawn = static_cast<ADishonoredPlayerPawn*>(comp->Outer);
        if (!pawn && self && self->m_pOwningInventory && self->m_pOwningInventory->m_pOwner &&
            self->m_pOwningInventory->m_pOwner->IsA(ADishonoredPlayerPawn::StaticClass()))
            pawn = static_cast<ADishonoredPlayerPawn*>(self->m_pOwningInventory->m_pOwner);
        Log("SetCurrentPower got owner-less %s (%s, outer %s); repairing with pawn %p", ObjName(comp).c_str(),
            comp->m_PowerName.ToString().c_str(), ObjName(comp->Outer).c_str(), pawn);
        if (!pawn) return 0;   // nothing safe to equip
        comp->m_pOwnerPawn = pawn;
        comp->m_pOwnerPlayerPawn = pawn;
        comp->m_pOwnerPowersComponent = pawn->m_pPowersComponent;
    }
    return g_origSetCurrentPower(self, comp, flag);
}

static void HookPowerSaveLoad() {
    const uint8_t scpExpect[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x53, 0x56, 0x57, 0x89, 0x4D, 0xFC };
    if (memcmp(reinterpret_cast<void*>(0x00C4BA70), scpExpect, sizeof(scpExpect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x00C4BA70), reinterpret_cast<void*>(&HookedSetCurrentPower),
                      reinterpret_cast<void**>(&g_origSetCurrentPower)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00C4BA70)) == MH_OK) Log("SetCurrentPower hooked");
    else Log("WARNING: could not hook SetCurrentPower");
    const uint8_t gsiExpect[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x83, 0x3D, 0x68, 0x19, 0x47, 0x01 };
    if (memcmp(reinterpret_cast<void*>(0x00AAD2A0), gsiExpect, sizeof(gsiExpect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x00AAD2A0), reinterpret_cast<void*>(&HookedGameSaveInventory),
                      reinterpret_cast<void**>(&g_origGameSaveInventory)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00AAD2A0)) == MH_OK) Log("GameSave_Inventory hooked");
    else Log("WARNING: could not hook GameSave_Inventory");
}

// Is comp actually attached to the pawn (in its Components array)? A component restored from an old save may not be.
static bool IsAttachedTo(ADishonoredPlayerPawn* pawn, UObject* comp) {
    auto* comps = reinterpret_cast<RawArray*>(&pawn->Components);
    for (int i = 0; comps->Data && i < comps->Num; ++i) if (static_cast<UObject**>(comps->Data)[i] == comp) return true;
    return false;
}

// Saves made while Better Powers: Blink was installed store the pawn's own PowerBlink as a Transversal; without that
// mod it loads back with Transversal's defaults (name "Transversal", wheel slot 20), which Corvo's journal shows as
// "undefined" and will not sell or equip (build 157 test). Transversal (and Daud's Void Gaze) are subclasses of
// Corvo's component classes, so convert such a pawn-owned leftover back in place: Corvo's class + vtables, and his
// name, wheel slot and tweaks from the Corvo class defaults. (Our own Daud components live in the transient package.)
struct LegacyFix { const char* daudClass; const char* corvoClass; uintptr_t vtMain, vtTweaks; };
static const LegacyFix kLegacyFixes[] = {
    { "DisDLC06ActivePowerComponent_Transversal", "DishonoredActivePowerComponent_Blink", 0x01168698, 0x0116867C },
    { "DisDLC06ActivePowerComponent_DarkVision",  "DisActivePowerComponent_DarkVision",   0x01168870, 0x01168850 },
};

static void FixLegacyPowerComponents(ADishonoredPlayerPawn* pawn, RawArray* arr) {
    for (int i = 0; i < arr->Num; ++i) {
        auto* c = static_cast<UDishonoredActivePowerComponent**>(arr->Data)[i];
        if (!c || c->Outer != pawn) continue;   // ours are in the transient package
        for (const LegacyFix& f : kLegacyFixes) {
            if (c->Class->GetName() != f.daudClass) continue;
            UClass* cls = UObject::FindClass((std::string("Class DishonoredGame.") + f.corvoClass).c_str());
            auto* cdo = static_cast<UDishonoredActivePowerComponent*>(
                FindObjectByName(f.corvoClass, (std::string("Default__") + f.corvoClass).c_str()));
            if (!cls || !cdo) { Log("legacy power fix: %s defaults not found", f.corvoClass); continue; }
            void** vt = *reinterpret_cast<void***>(c);
            void** vtTw = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(c) + 0x54);
            Log("legacy power fix: %s (name %s, ui %d, level %d, vtables %p/%p) -> %s", ObjName(c).c_str(),
                c->m_PowerName.ToString().c_str(), static_cast<int>(c->m_eUISelection), c->m_CurrentLevel, vt, vtTw, f.corvoClass);
            CancelPowerIfActive(c);
            pawn->DetachComponent(c);
            *reinterpret_cast<uintptr_t*>(c) = f.vtMain;
            *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(c) + 0x54) = f.vtTweaks;
            c->Class = cls;
            c->m_PowerName = cdo->m_PowerName;
            c->m_eUISelection = cdo->m_eUISelection;
            c->m_pPowerTweaks = cdo->m_pPowerTweaks;
            pawn->AttachComponent(c);
            Log("   now %s name %s ui %d tweaks %s", ObjName(c).c_str(), c->m_PowerName.ToString().c_str(),
                static_cast<int>(c->m_eUISelection), ObjName(c->m_pPowerTweaks).c_str());
        }
    }
}

// Daud's Void Gaze puts his own glove material (DLC06_PlyDaud.DLC06_Player_Daud_VG_inst) on the first-person arms and
// it stays after the power ends (build 157). While the arms are Corvo's, keep his material: remember the last
// non-Daud override per slot (rooted, since the game drops its reference when it swaps) and put it back.
static UMaterialInterface* g_corvoArmsMat[8];
static bool g_corvoArmsMatRooted[8];

static bool IsDaudMaterial(UObject* m) {
    for (int depth = 0; m && depth < 5; ++depth) {
        if (ObjName(m).find("PlyDaud") != std::string::npos) return true;
        if (!m->IsA(UMaterialInstance::StaticClass())) break;
        m = static_cast<UMaterialInstance*>(m)->Parent;
    }
    return false;
}

static void ForgetCorvoArmsMaterials() {
    for (int i = 0; i < 8; ++i) { if (g_corvoArmsMat[i] && !g_corvoArmsMatRooted[i]) LetGo(g_corvoArmsMat[i]); g_corvoArmsMat[i] = nullptr; }
}

static void KeepCorvoArmsMaterial(ADishonoredPlayerPawn* pawn) {
    USkeletalMeshComponent* arms = pawn ? pawn->Mesh : nullptr;
    static USkeletalMeshComponent* lastArms = nullptr;
    if (arms != lastArms) { ForgetCorvoArmsMaterials(); lastArms = arms; }
    if (!arms || !arms->SkeletalMesh || arms->SkeletalMesh == g_daudArmsMesh || arms->SkeletalMesh == g_ovsArmsMesh) return;   // Daud's / the Overseer's arms: their own materials are right
    auto* mats = reinterpret_cast<RawArray*>(&arms->Materials);
    for (int i = 0; mats->Data && i < mats->Num && i < 8; ++i) {
        auto* m = static_cast<UMaterialInterface**>(mats->Data)[i];
        if (!m) continue;
        if (IsDaudMaterial(m)) {
            if (g_corvoArmsMat[i]) {
                static UObject* lastLogged = nullptr;
                if (m != lastLogged) { lastLogged = m; Log("Corvo's arms got Daud's material %s; putting Corvo's back", ObjName(m).c_str()); }
                arms->SetMaterial(i, g_corvoArmsMat[i]);
            }
        } else if (m != g_corvoArmsMat[i]) {
            if (g_corvoArmsMat[i] && !g_corvoArmsMatRooted[i]) LetGo(g_corvoArmsMat[i]);
            g_corvoArmsMat[i] = m;
            g_corvoArmsMatRooted[i] = (*reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(m) + 0x08) & 0x4000ULL) != 0;
            KeepAlive(m);
        }
    }
}

// pawn vfunc +0x6F0 = OnActivePowerLevelChanged(index, level) (sub_AB5D20): keeps m_UsableActivePowerIndices (the
// wheel, max 6) in step - level > 0 adds the index, <= 0 removes it. Called after every swap since the two
// components in a slot can have different levels (e.g. Pull not bought yet where Windblast was).
static void PowerLevelChanged(ADishonoredPlayerPawn* pawn, int index, int level) {
    using Fn = void(__thiscall*)(void*, int, int);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(pawn))[0x6F0 / 4])(pawn, index, level);
}

static bool InPowerList(ADishonoredPlayerPawn* pawn, UObject* c) {
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    for (int i = 0; arr->Data && i < arr->Num; ++i) if (static_cast<UObject**>(arr->Data)[i] == c) return true;
    return false;
}

// ---- Journal slots: Corvo's journal shows the slot types in its config list; with Daud's set on, his three slots
// (copied from the DLC07 journal's config) replace Blink/Windblast/Devouring Swarm. Arrays are deep copies in the
// engine heap, so the engine may free them like its own.
static void CopyFString(FString& dst, const FString& src) {
    auto& s = reinterpret_cast<const RawArray&>(src);
    auto& d = reinterpret_cast<RawArray&>(dst);
    d.Data = nullptr; d.Num = d.Max = 0;
    if (!s.Data || s.Num <= 0) return;
    d.Data = ArrayAlloc(s.Num * 2);
    if (!d.Data) return;
    memcpy(d.Data, s.Data, s.Num * 2);
    d.Num = d.Max = s.Num;
}

static RawArray CopySlots(const RawArray& src) {
    RawArray out = { nullptr, 0, 0 };
    if (!src.Data || src.Num <= 0) return out;
    out.Data = ArrayAlloc(src.Num * sizeof(FDisJournalPowerSlot));
    if (!out.Data) return out;
    for (int i = 0; i < src.Num; ++i) {
        auto& s = static_cast<FDisJournalPowerSlot*>(src.Data)[i];
        auto& d = static_cast<FDisJournalPowerSlot*>(out.Data)[i];
        memcpy(&d, &s, sizeof(d));
        CopyFString(d.m_PowerId, s.m_PowerId);
        CopyFString(d.m_BackgroundImagePath, s.m_BackgroundImagePath);
    }
    out.Num = out.Max = src.Num;
    return out;
}

static RawArray g_corvoSlots = { nullptr, 0, 0 }, g_daudSlots = { nullptr, 0, 0 };   // templates (our copies)

static bool BuildSlotTemplates() {
    if (g_daudSlots.Data) return true;
    auto* corvo = static_cast<UDisGFxMoviePlayerJournal*>(FindObjectByName("DisGFxMoviePlayerJournal", "Default__DisGFxMoviePlayerJournal"));
    auto* dlc = static_cast<UDisGFxMoviePlayerJournal*>(FindObjectByName("DisDLC07MoviePlayerJournal", "Default__DisDLC07MoviePlayerJournal"));
    if (!corvo || !dlc) { Log("journal: defaults not found (corvo %p, DLC07 %p)", corvo, dlc); return false; }
    auto& cs = reinterpret_cast<RawArray&>(corvo->m_ActivePowerSlots);
    auto& ds = reinterpret_cast<RawArray&>(dlc->m_ActivePowerSlots);
    g_corvoSlots = CopySlots(cs);
    g_daudSlots = CopySlots(cs);
    for (int i = 0; i < g_daudSlots.Num; ++i) {
        auto& slot = static_cast<FDisJournalPowerSlot*>(g_daudSlots.Data)[i];
        for (auto& s : g_powerSwaps) {
            if (slot.m_SelectionType != s.corvoUi) continue;
            for (int k = 0; ds.Data && k < ds.Num; ++k) {
                auto& d = static_cast<FDisJournalPowerSlot*>(ds.Data)[k];
                if (d.m_SelectionType != s.daudUi) continue;
                slot.m_SelectionType = d.m_SelectionType;
                slot.m_Note = d.m_Note;
                CopyFString(slot.m_PowerId, d.m_PowerId);   // (leaks the copied Corvo string; tiny, once)
                CopyFString(slot.m_BackgroundImagePath, d.m_BackgroundImagePath);
                Log("journal: slot %d %s -> %s", i, s.corvoName, s.daudName);
            }
        }
    }
    return true;
}

static void ApplyJournalSlots(bool daud) {
    static int applied = -1;   // 0 Corvo, 1 Daud
    static DWORD lastCheck = 0;
    if (applied == (daud ? 1 : 0) && GetTickCount() - lastCheck < 2000) return;   // re-check now and then for new journals
    lastCheck = GetTickCount();
    if (!BuildSlotTemplates()) return;
    const RawArray& want = daud ? g_daudSlots : g_corvoSlots;
    auto* objects = UObject::GObjObjects();
    int changed = 0;
    for (int i = 0; i < objects->size(); ++i) {
        UObject* o = objects->at(i);
        if (!o || !o->Class || o->Class->GetName() != "DisGFxMoviePlayerJournal") continue;   // Corvo's journal (CDO + live)
        auto& arr = reinterpret_cast<RawArray&>(static_cast<UDisGFxMoviePlayerJournal*>(o)->m_ActivePowerSlots);
        bool hasDaud = false;
        for (int k = 0; arr.Data && k < arr.Num; ++k)
            for (auto& s : g_powerSwaps) if (static_cast<FDisJournalPowerSlot*>(arr.Data)[k].m_SelectionType == s.daudUi) hasDaud = true;
        if (hasDaud == daud) continue;
        arr = CopySlots(want);   // the old array is left to leak (it may be the config copy the engine owns)
        ++changed;
    }
    if (changed || applied != (daud ? 1 : 0)) Log("journal: %s power slots on %d journal object(s)", daud ? "Daud's" : "Corvo's", changed);
    applied = daud ? 1 : 0;
}

// ---- Daud's power icons (wheel: m_EquipmentIcons img:// paths; journal: background images) live in his DLC UI
// packages. Load them once, after his player package, and root the textures (nothing references them but paths).
static const wchar_t* kDaudUiPackages[] = { L"UI_EquipmentIcons_DLC06_SF", L"UI_EquipmentIcons_DLC07_SF",
                                            L"UI_Powers_Large_DLC06", L"UI_Powers_Large_DLC07", L"UI_Journal_DLC07_SF" };

// ---- Icons. Corvo's journal and wheel movies are Scaleform files with Corvo's six powers built in: the journal asks
// for blink / wind_blast / rat_swarm by id, and every image (journal grid icons Journal_I8x, the large picture
// UI_Powers_Large.*Big, the wheel's UI_EquipmentIcons.ic_pow_*) is loaded by path through
// FGFxTexture::InitTextureFromFile (0x985210 -> StaticLoadObject). With Daud's set on, three of those paths are sent
// to Daud's own images instead (build 160). The DLC07 journal's grid icons share Corvo's names, so their real
// path is found once their package has loaded.
struct IconRemap { const char* corvoPkg; const char* corvoName; const char* daudPath; const char* findDlc07JournalTex; };
static const IconRemap kIconRemaps[] = {
    { "UI_Journal",        "Journal_I86",      nullptr, "Journal_I84" },   // Blink -> Daud's Blink (Transversal)
    { "UI_Journal",        "Journal_I84",      nullptr, "Journal_I86" },   // Devouring Swarm -> Summon Assassin
    { "UI_Powers_Large",   "BlinkBig",         "UI_Powers_Large_DLC06.j_pow_ic_a_DLC06TransversalBig" },
    { "UI_Powers_Large",   "RatSwarmBig",      "UI_Powers_Large_DLC06.j_pow_ic_a_DLC06SummonAssassinBig" },
    { "UI_EquipmentIcons", "ic_pow_Blink",     "UI_EquipmentIcons_DLC06.ic_pow_DLC06_transversal" },
    { "UI_EquipmentIcons", "ic_pow_ratSwarm",  "UI_EquipmentIcons_DLC06.ic_pow_DLC06_summonAssassin" },
};
static std::string g_dlc07JournalPath[3];   // resolved paths of the DLC07 journal's grid icons (remaps 0-2)

static void ResolveDlc07JournalIcons() {
    auto* objects = UObject::GObjObjects();
    for (int i = 0; i < objects->size(); ++i) {
        UObject* o = objects->at(i);
        if (!o || !o->Class || o->Class->GetName() != "Texture2D") continue;
        // Skip objects awaiting the purge (RF_Unreachable): their Outer may already be freed (crash 21:47, build 202).
        if (*reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(o) + 0x08) & 0x0000000200000000ULL) continue;
        std::string full = o->GetFullName();   // "Texture2D Pkg.Name"
        if (full.find("DLC07") == std::string::npos) continue;
        for (int r = 0; r < 3; ++r) {
            if (!kIconRemaps[r].findDlc07JournalTex || o->GetName() != kIconRemaps[r].findDlc07JournalTex) continue;
            g_dlc07JournalPath[r] = full.substr(full.find(' ') + 1);
            KeepAlive(o);
            Log("icons: Daud journal icon for %s = %s", kIconRemaps[r].corvoName, g_dlc07JournalPath[r].c_str());
        }
    }
}

using InitTextureFromFile_t = bool(__thiscall*)(void*, const char*);
static InitTextureFromFile_t g_origInitTextureFromFile = nullptr;
static bool __fastcall HookedInitTextureFromFile(void* self, void*, const char* path) {
    static int logged = 0;
    if (path && logged < 400) {
        static std::vector<std::string> seen;
        std::string p(path);
        if (std::find(seen.begin(), seen.end(), p) == seen.end()) { seen.push_back(p); ++logged; Log("gfx image: %s", path); }
    }
    if (path && g_powerChar == 1) {
        const char* dot = strrchr(path, '.');
        const char* first = strchr(path, '.');
        if (dot && first) {
            std::string pkg(path, first - path), name(dot + 1);
            for (int r = 0; r < static_cast<int>(sizeof(kIconRemaps) / sizeof(kIconRemaps[0])); ++r) {
                const IconRemap& m = kIconRemaps[r];
                if (_stricmp(pkg.c_str(), m.corvoPkg) != 0 || _stricmp(name.c_str(), m.corvoName) != 0) continue;
                const char* to = m.daudPath ? m.daudPath : (r < 3 && m.findDlc07JournalTex && !g_dlc07JournalPath[r].empty() ? g_dlc07JournalPath[r].c_str() : nullptr);
                if (!to) break;
                if (g_origInitTextureFromFile(self, to)) { Log("icons: %s -> %s", path, to); return true; }
                Log("icons: %s -> %s failed to load, keeping Corvo's", path, to);
                break;
            }
        }
    }
    return g_origInitTextureFromFile(self, path);
}

static void HookIcons() {
    const uint8_t expect[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x2B, 0x73, 0xF4, 0x00, 0x64, 0xA1 };
    if (memcmp(reinterpret_cast<void*>(0x00985210), expect, sizeof(expect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x00985210), reinterpret_cast<void*>(&HookedInitTextureFromFile),
                      reinterpret_cast<void**>(&g_origInitTextureFromFile)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00985210)) == MH_OK) Log("InitTextureFromFile hooked (power icons)");
    else Log("WARNING: could not hook InitTextureFromFile");
}
static volatile LONG g_daudUiNext = 0;   // next package index to load; count = all done
static volatile LONG g_daudUiLoading = 0;

// Build 202 (crash 21:47 on startup): GetFullName walks an object's Outer chain. Right after the body was rebuilt as
// the Overseer, the old body's components were being purged a few objects per frame, and an anim node still in the
// object list had an Outer that was already freed - reading it crashed this callback. Objects awaiting the purge are
// marked unreachable: skip them before touching their Outer (their own name/class stay readable until they go).
static bool AwaitingPurge(UObject* o) {
    return (*reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(o) + 0x08) & 0x0000000200000000ULL) != 0;   // RF_Unreachable
}

static void __cdecl OnDaudUiLoaded(UObject* package, void*) {
    int idx = g_daudUiNext;
    Log("Daud UI package %s: %s", Narrow(kDaudUiPackages[idx]).c_str(), package ? "loaded" : "FAILED");
    int rooted = 0;
    auto* objects = UObject::GObjObjects();
    UClass* texClass = UTexture2D::StaticClass();
    for (int i = 0; i < objects->size(); ++i) {
        UObject* o = objects->at(i);
        if (!o || o->Class != texClass || AwaitingPurge(o)) continue;   // class first: no name built for anything else
        std::string n = o->GetFullName();
        if (n.find("UI_EquipmentIcons_DLC0") == std::string::npos && n.find("UI_Powers_Large_DLC0") == std::string::npos &&
            n.find("UI_Journal_DLC07") == std::string::npos) continue;
        if (o->Class->GetName() != "Texture2D") continue;
        KeepAlive(o);
        ++rooted;
    }
    Log("   rooted %d DLC UI textures", rooted);
    ResolveDlc07JournalIcons();
    InterlockedIncrement(&g_daudUiNext);
    InterlockedExchange(&g_daudUiLoading, 0);
}

static void LoadDaudUi() {
    if (g_daudUiLoading || g_daudUiNext >= static_cast<LONG>(sizeof(kDaudUiPackages) / sizeof(kDaudUiPackages[0])) || !LoadPackageAsync) return;
    InterlockedExchange(&g_daudUiLoading, 1);
    FString name(kDaudUiPackages[g_daudUiNext]);
    LoadPackageAsync(name, &OnDaudUiLoaded, nullptr, nullptr);
}

static UDishonoredActivePowerComponent* MakeDaudPower(ADishonoredPlayerPawn* pawn, PowerSwap& s, int si, UDishonoredActivePowerComponent* corvo) {
    UClass* cls = UObject::FindClass((std::string("Class DishonoredGame.") + s.daudClass).c_str());
    if (!cls) { if (!s.loggedMissing) { s.loggedMissing = true; Log("Power set: class %s not found", s.daudClass); } return nullptr; }
    auto* nc = static_cast<UDishonoredActivePowerComponent*>(
        StaticConstructObject(cls, TransientPackage() ? TransientPackage() : pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
    if (!nc) { Log("Power set: could not construct %s", s.daudClass); return nullptr; }
    // Name and wheel slot are the class defaults (Transversal/20, Pull/27, SummonAssassin/21). Level: from the save
    // if it had one; a new Daud's Blink starts at Corvo's Blink level (so you can still blink); the others unbought.
    bool fromSave = s.pendingPc && s.pendingPc == pawn->m_pPowersComponent;
    nc->m_eUISelection = static_cast<EeDisUISelectionType>(s.corvoUi);   // Corvo's journal/wheel look powers up by these slot types (build 160)
    if (fromSave) nc->m_CurrentLevel = s.pendingLevel;
    else if (si == 0 && corvo) nc->m_CurrentLevel = corvo->m_CurrentLevel;
    s.pendingPc = nullptr;
    s.daudPc = pawn->m_pPowersComponent;
    KeepAlive(nc);
    HookDaudSave(si, nc);
    Log("Power set: made %s (%s, ui %d, level %d%s)", s.daudName, nc->m_PowerName.ToString().c_str(),
        static_cast<int>(nc->m_eUISelection), nc->m_CurrentLevel, fromSave ? ", from the save" : "");
    return nc;
}

static void DumpPowerAnimDiag(ADishonoredPlayerPawn* pawn);
static RawArray* DaudAnimTable(int kind, int off);
static std::string AnimStateText(const FDisPawnAnimState& s);

// ---- Casting Daud's powers on Corvo (build 162). A cast plays the pawn anim state named by the power's tweaks
// (CastPull, CastSummonAssassin), whose sequences' anim notifies do the work. Corvo's pawn has neither state and his
// first-person arms lack DLC07_PlyDaud_Powers_as, so the cast did nothing but glow. Give his pawn the two states
// (Summon: Daud's own; Pull: Daud's uses a Pull-only anim tree node Corvo's arms do not have, so it is built on the
// standard CastPower tree with Daud's Pull_In/Loop/Out/Abort sequences), add the anim set to the arms, and rebuild
// the pawn's anim state maps (sub_AE0F90 = the PreBeginPlay_AnimStates builder; it resets and rebuilds).
static FDisPawnAnimState CopyAnimState(const FDisPawnAnimState& src) {
    FDisPawnAnimState out;
    memcpy(&out, &src, sizeof(out));
    auto& from = reinterpret_cast<const RawArray&>(src.m_CustomSequences);
    auto& to = reinterpret_cast<RawArray&>(out.m_CustomSequences);
    to.Data = nullptr; to.Num = to.Max = 0;
    if (from.Data && from.Num > 0) {
        to.Data = ArrayAlloc(from.Num * sizeof(FDisAnimState_CustomSeq));
        if (to.Data) { memcpy(to.Data, from.Data, from.Num * sizeof(FDisAnimState_CustomSeq)); to.Num = to.Max = from.Num; }
    }
    return out;
}

static bool HasAnimState(RawArray* arr, const char* name) {
    for (int i = 0; arr->Data && i < arr->Num; ++i)
        if (reinterpret_cast<FDisPawnAnimState*>(static_cast<uint8_t*>(arr->Data) + i * 0x34)->m_StateName.ToString() == name) return true;
    return false;
}

static void EnsureDaudPowerAnims(ADishonoredPlayerPawn* pawn) {
    static ADishonoredPlayerPawn* doneFor = nullptr;
    if (doneFor == pawn || !pawn->Mesh) return;
    auto* set = static_cast<UAnimSet*>(FindObjectByName("AnimSet", "DLC07_PlyDaud_Powers_as"));
    RawArray* daudSpecial = DaudAnimTable(2, 0x0A4);
    if (!set || !daudSpecial || !daudSpecial->Data) { Log("Daud power anims: not ready (set %p, Daud special table %p)", set, daudSpecial); doneFor = pawn; return; }
    doneFor = pawn;
    KeepAlive(set);

    // 1) The anim set on the first-person arms.
    auto* sets = reinterpret_cast<RawArray*>(&pawn->Mesh->AnimSets);
    bool hasSet = false;
    for (int i = 0; sets->Data && i < sets->Num; ++i) if (static_cast<UObject**>(sets->Data)[i] == set) hasSet = true;
    if (!hasSet) {
        if (sets->Max < sets->Num + 1 && !GrowArray(sets, 4, sets->Num + 4)) {
            Log("Daud power anims: could not grow the arms' anim sets");
            return;
        }
        static_cast<UObject**>(sets->Data)[sets->Num++] = set;
        pawn->Mesh->UpdateAnimations();
        Log("Daud power anims: %s added to the first-person arms", ObjName(set).c_str());
    }

    // 2) The cast states in Corvo's special anim state table (pawn +0xA0C).
    auto* special = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(pawn) + 0xA0C);
    const FDisPawnAnimState* summon = nullptr;
    for (int i = 0; i < daudSpecial->Num; ++i) {
        auto* s = reinterpret_cast<FDisPawnAnimState*>(static_cast<uint8_t*>(daudSpecial->Data) + i * 0x34);
        if (s->m_StateName.ToString() == "CastSummonAssassin") summon = s;
    }
    if (!summon) { Log("Daud power anims: Daud's CastSummonAssassin state not found"); return; }
    FDisPawnAnimState add[2];
    int count = 0;
    if (!HasAnimState(special, "CastSummonAssassin")) add[count++] = CopyAnimState(*summon);
    if (!HasAnimState(special, "CastPull")) {
        FDisPawnAnimState pull = CopyAnimState(*summon);
        pull.m_StateName = FName("CastPull");
        auto& cs = reinterpret_cast<RawArray&>(pull.m_CustomSequences);
        for (int q = 0; cs.Data && q < cs.Num; ++q) {
            auto& c = static_cast<FDisAnimState_CustomSeq*>(cs.Data)[q];
            std::string alias = c.m_Alias.ToString();
            const char* seq = alias == "In" ? "Powers_Cast_Pull_In" : alias == "Loop" ? "Powers_Cast_Pull_Loop"
                            : alias == "Out" ? "Powers_Cast_Pull_Out" : "Powers_Cast_Pull_Abort";
            c.m_Sequence = FName(seq);
        }
        add[count++] = pull;
    }
    if (count) {
        if (!GrowArray(special, 0x34, special->Num + count)) {
            Log("Daud power anims: could not grow the special anim state table");
            return;
        }
        for (int k = 0; k < count; ++k) {
            memcpy(static_cast<uint8_t*>(special->Data) + special->Num * 0x34, &add[k], 0x34);
            Log("Daud power anims: + %s", AnimStateText(add[k]).c_str());
            ++special->Num;
        }
        reinterpret_cast<void(__thiscall*)(void*)>(0x00AE0F90)(pawn);   // rebuild the anim state maps
        Log("Daud power anims: anim state maps rebuilt");
    }
}

// Daud's own CastPull tree has a DisDLC07AnimNodeBlendByPull node (OnBecomeRelevant @0xCF0CE0): when the Pull
// component's target (+0x9C object / +0xA0 IDisDLC07PullTargetInterface) answers yes to interface vfunc +0x24 (a
// living target) it plays the Lift child, else the object pull. Our CastPull state has no such node, so pick its
// sequences the same way every tick (build 167) - with the object set, a pulled NPC hung in the air (video 13-34-51).
static void UpdatePullSequences(ADishonoredPlayerPawn* pawn, UObject* pull) {
    if (!pawn || !pull) return;
    {   // diagnostic (build 168): every change of the Pull component's state block +0x88..+0xBC
        static uint32_t last[14] = {};
        uint32_t now[14];
        memcpy(now, reinterpret_cast<uint8_t*>(pull) + 0x88, sizeof(now));
        if (memcmp(&now[5], &last[5], 8) != 0) {   // log target changes only
            memcpy(last, now, sizeof(now));
            UObject* t = reinterpret_cast<UObject*>(now[(0x9C - 0x88) / 4]);
            Log("Pull state: lvl %d flags %X targetPS %08X acquiredPS %08X target %08X iface %08X tut %d/%d fade %.2f | target=%s",
                now[0], now[2], now[3], now[4], now[5], now[6], now[(0xB0 - 0x88) / 4], now[(0xB4 - 0x88) / 4],
                *reinterpret_cast<float*>(&now[(0xB8 - 0x88) / 4]), t ? ObjName(t).c_str() : "-");
        }
    }    void* iface = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pull) + 0xA0);
    UObject* target = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(pull) + 0x9C);
    static int lastLiving = -1;
    static ADishonoredPlayerPawn* lastPawn = nullptr;
    if (pawn != lastPawn) { lastPawn = pawn; lastLiving = -1; }
    bool living = false;
    if (target && iface) {
        living = false;   // build 170: the Lift set stopped Pull working on assassins; Daud's tree uses Pull_* for them too
    } else if (lastLiving >= 0) {
        return;   // no target (e.g. the cast just ended): keep the current set so a running Out is not swapped
    }
    auto* special = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(pawn) + 0xA0C);
    for (int i = 0; special->Data && i < special->Num; ++i) {
        auto* s = reinterpret_cast<FDisPawnAnimState*>(static_cast<uint8_t*>(special->Data) + i * 0x34);
        if (s->m_StateName.ToString() != "CastPull") continue;
        auto& cs = reinterpret_cast<RawArray&>(s->m_CustomSequences);
        if (lastLiving == (living ? 1 : 0)) return;
        for (int q = 0; cs.Data && q < cs.Num; ++q) {
            auto& c = static_cast<FDisAnimState_CustomSeq*>(cs.Data)[q];
            std::string alias = c.m_Alias.ToString();
            const char* seq = alias == "In"   ? (living ? "Powers_Cast_Pull_Lift_In" : "Powers_Cast_Pull_In")
                            : alias == "Loop" ? (living ? "Powers_Cast_Pull_Lift_Loop" : "Powers_Cast_Pull_Loop")
                            : alias == "Out"  ? (living ? "Powers_Cast_Pull_Lift_Out" : "Powers_Cast_Pull_Out")
                                              : (living ? "Powers_Cast_Pull_Lift_Abort" : "Powers_Cast_Pull_Abort");
            c.m_Sequence = FName(seq);
        }
        lastLiving = living ? 1 : 0;
        Log("Pull: target %s -> %s animations", ObjName(target).c_str(), living ? "lift (living)" : "object");
        return;
    }
}

// ---- Daud's blade draws like the Assassin's blade (build 162): the item settings' fast equip / unequip flags
// (DisTweaks_InventoryItem_PlayerSpecific +0x114 bits 1-2) pick Sword_Ready_Equip_Fast - a plain pull - instead of
// Corvo's flick-open Sword_Ready_Equip. Copy those bits from Daud's sword settings onto Corvo's while his blade is on.
static void ApplyBladeEquipStyle(bool daudBlade) {
    static UDisTweaks_InventoryItem* corvo = nullptr;
    static uint32_t corvoBits = 0;
    static int applied = -1;
    if (applied == (daudBlade ? 1 : 0)) return;
    if (!corvo) {
        corvo = static_cast<UDisTweaks_InventoryItem*>(FindObjectByName("DisTweaks_WepSword", "Twk_Inv_SwordCorvo"));
        if (!corvo || !corvo->m_pPlayerSpecificTweaks) { corvo = nullptr; return; }
        corvoBits = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(corvo->m_pPlayerSpecificTweaks) + 0x114);
        KeepAlive(corvo);
    }
    uint32_t& bits = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(corvo->m_pPlayerSpecificTweaks) + 0x114);
    if (daudBlade) {
        uint32_t daudBits = 0x6;   // fast equip + fast unequip unless Daud's settings say otherwise
        for (const char* n : { "DLC07_Inv_Sword_Daud", "Twk_Inv_SwordAssassin_Player" }) {
            auto* t = static_cast<UDisTweaks_InventoryItem*>(FindObjectByName("DisTweaks_WepSwordOfAssassin", n));
            if (t && t->m_pPlayerSpecificTweaks)
                Log("blade: %s player flags %08X", n, *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(t->m_pPlayerSpecificTweaks) + 0x114));
        }
        bits = (corvoBits & ~0x6u) | daudBits;
    } else {
        bits = corvoBits;
    }
    Log("blade: Corvo's sword player flags %08X -> %08X (%s draw)", corvoBits, bits, daudBlade ? "Daud's quick" : "Corvo's");
    applied = daudBlade ? 1 : 0;
}

static void ApplyPowerSet(ADishonoredPlayerPawn* pawn) {
    if (!pawn || !pawn->Class || pawn->Class->GetName() != "DishonoredPlayerPawn") return;   // not in Daud's own DLC
    if (pawn != g_powerPawn) { ForgetPowerSwaps(); g_powerPawn = pawn; }
    KeepCorvoArmsMaterial(pawn);
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    if (!arr->Data || arr->Num <= 0 || arr->Num > 16) return;
    FixLegacyPowerComponents(pawn, arr);
    bool wantDaud = g_powerChar == 1;
    if (g_daudLoadState == LoadState::Loaded) LoadDaudUi();
    static int lastWait = -1;
    int wait = !wantDaud ? 0 : g_daudLoadState != LoadState::Loaded ? 1 : !DaudPowerTweaks(0) ? 2 : 0;
    if (wait != lastWait) {
        lastWait = wait;
        if (wait) Log("Power set: waiting for %s", wait == 1 ? "Daud's package" : "Daud's power tweaks (not found)");
    }
    ApplyBladeEquipStyle(g_bladeChar != 0);   // Daud's and the Overseer's straight blades: quick draw, no flick
    if (wantDaud && !wait) EnsureDaudPowerAnims(pawn);
    for (int si = 0; si < kPowerSwapCount; ++si) {
        PowerSwap& s = g_powerSwaps[si];
        Protect(s.orig);
        Protect(s.daud);
        int idx = -1;
        for (int i = 0; i < arr->Num; ++i) {
            auto* c = static_cast<UDishonoredActivePowerComponent**>(arr->Data)[i];
            if (!c) continue;
            if (c == s.daud || c == s.orig) { idx = i; break; }
            std::string n = c->m_PowerName.ToString();
            if (n == s.corvoName || n == s.daudName) { idx = i; break; }
        }
        if (idx < 0) continue;
        auto*& slot = static_cast<UDishonoredActivePowerComponent**>(arr->Data)[idx];
        UDishonoredActivePowerComponent* cur = slot;
        // Daud's component exists whenever his set is on, or a save gave it a level (kept so saves keep it).
        bool pending = s.pendingPc && s.pendingPc == pawn->m_pPowersComponent;
        if (!s.daud && ((wantDaud && !wait) || pending)) s.daud = MakeDaudPower(pawn, s, si, cur != s.daud ? cur : nullptr);
        if (!s.daud) continue;
        if (wantDaud) {
            if (wait || cur == s.daud) continue;
            if (UDisTweaks_ActivePowerBase* t = DaudPowerTweaks(s.tweak)) s.daud->m_pPowerTweaks = t;
            Log("Power set: %s (level %d) -> %s (level %d) in slot %d", s.corvoName, cur->m_CurrentLevel, s.daudName,
                s.daud->m_CurrentLevel, idx);
            CancelPowerIfActive(cur);
            pawn->DetachComponent(cur);
            KeepAlive(cur);
            s.orig = cur;
            slot = s.daud;
            pawn->AttachComponent(s.daud);
            RetargetPowerPointers(pawn, cur, s.daud);
            PowerLevelChanged(pawn, idx, s.daud->m_CurrentLevel);
            Log("  in: tweaks=%s owner=%p", ObjName(s.daud->m_pPowerTweaks).c_str(), s.daud->m_pOwnerPlayerPawn);
            static ADishonoredPlayerPawn* diagFor = nullptr;
            if (si == kPowerSwapCount - 1 && diagFor != pawn) { diagFor = pawn; DumpPowerAnimDiag(pawn); }
        } else {
            if (cur != s.daud || !s.orig) continue;
            Log("Power set: %s (level %d) back to %s (level %d)", s.daudName, cur->m_CurrentLevel, s.corvoName, s.orig->m_CurrentLevel);
            CancelPowerIfActive(cur);
            pawn->DetachComponent(cur);
            slot = s.orig;
            pawn->AttachComponent(s.orig);
            RetargetPowerPointers(pawn, cur, s.orig);
            PowerLevelChanged(pawn, idx, s.orig->m_CurrentLevel);
            LetGo(s.orig);
            s.orig = nullptr;   // Daud's stays (rooted, detached) so its level goes into saves and comes back
        }
    }
}

// ---- Diagnostic (build 161): why Pull / Summon Assassin do nothing on Corvo, and how Daud's sword draw differs.
// A cast is pawn->RequestAction(tweaks->m_UsePowerAction): an action name + anim state names that the pawn must know.
struct AnimTable { const char* name; int pawnOff; int tweakKind; int tweakOff; };   // tweakKind 0 full, 1 upper, 2 special
static const AnimTable kAnimTables[] = {
    { "FullBody_Navigation",    0x980, 0, 0x0A8 }, { "FullBody_Assassination", 0x98C, 0, 0x0B4 },
    { "FullBody_Fatality",      0x998, 0, 0x0C0 }, { "UpperBody_Items",        0x9A4, 1, 0x0B0 },
    { "UpperBody_Items_Melee",  0x9B0, 1, 0x0BC }, { "UpperBody_Items_Ranged", 0x9BC, 1, 0x0C8 },
    { "LeftHand_Items",         0x9C8, 2, 0x0B0 }, { "FullBody_Misc",          0x9F4, 0, 0x09C },
    { "UpperBody_Misc",         0xA00, 1, 0x0A4 }, { "Special_Misc",           0xA0C, 2, 0x0A4 },
};

static std::string AnimStateText(const FDisPawnAnimState& s) {
    std::string t = s.m_StateName.ToString() + " [" + s.m_AnimStateTree.ToString() + "]";
    auto* cs = reinterpret_cast<const RawArray*>(&s.m_CustomSequences);
    for (int q = 0; cs->Data && q < cs->Num && q < 6; ++q) {
        auto& c = static_cast<FDisAnimState_CustomSeq*>(cs->Data)[q];
        t += " " + c.m_Alias.ToString() + "=" + c.m_Sequence.ToString();
    }
    if (s.m_bAddItemPrefix) t += " (+item prefix)";
    return t;
}

static RawArray* DaudAnimTable(int kind, int off) {
    if (!g_daudPawnTweaks || !g_daudPawnTweaks->m_pAnimationTweaks) return nullptr;
    UDisTweaks_Pawn_Animation* a = g_daudPawnTweaks->m_pAnimationTweaks;
    UObject* t = kind == 0 ? static_cast<UObject*>(a->m_pFullBodyTweaks) : kind == 1 ? static_cast<UObject*>(a->m_pUpperBodyTweaks)
                                                                                     : static_cast<UObject*>(a->m_pSpecialTweaks);
    return t ? reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(t) + off) : nullptr;
}

static void FindAnimState(ADishonoredPlayerPawn* pawn, const std::string& state) {
    bool found = false;
    for (const AnimTable& tb : kAnimTables) {
        auto* arr = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(pawn) + tb.pawnOff);
        for (int i = 0; arr->Data && i < arr->Num; ++i) {
            auto& s = *reinterpret_cast<FDisPawnAnimState*>(static_cast<uint8_t*>(arr->Data) + i * 0x34);
            if (s.m_StateName.ToString() == state) { Log("      Corvo %s: %s", tb.name, AnimStateText(s).c_str()); found = true; }
        }
        RawArray* d = DaudAnimTable(tb.tweakKind, tb.tweakOff);
        for (int i = 0; d && d->Data && i < d->Num; ++i) {
            auto& s = *reinterpret_cast<FDisPawnAnimState*>(static_cast<uint8_t*>(d->Data) + i * 0x34);
            if (s.m_StateName.ToString() == state) Log("      Daud  %s: %s", tb.name, AnimStateText(s).c_str());
        }
    }
    if (!found) Log("      -> MISSING on Corvo's pawn");
}

static void DumpPowerAnimDiag(ADishonoredPlayerPawn* pawn) {
    Log("=== power/sword anim diagnostic (Daud pawn tweaks %s, anim tweaks %s)", ObjName(g_daudPawnTweaks).c_str(),
        g_daudPawnTweaks ? ObjName(g_daudPawnTweaks->m_pAnimationTweaks).c_str() : "-");
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    for (int i = 0; arr->Data && i < arr->Num; ++i) {
        auto* c = static_cast<UDishonoredActivePowerComponent**>(arr->Data)[i];
        if (!c || !c->m_pPowerTweaks) continue;
        FPawnAction& a = c->m_pPowerTweaks->m_UsePowerAction;
        auto* st = reinterpret_cast<RawArray*>(&a.m_AnimStates);
        Log("  %s (%s): action %s, %d anim state(s)", c->m_PowerName.ToString().c_str(), c->Class->GetName().c_str(),
            a.m_ActionName.ToString().c_str(), st->Num);
        for (int k = 0; st->Data && k < st->Num; ++k) {
            std::string name = static_cast<FName*>(st->Data)[k].ToString();
            Log("    state %s", name.c_str());
            FindAnimState(pawn, name);
        }
    }
    // Sword draw / holster: every equip-ish state in both pawns' upper-body item tables.
    for (int pass = 0; pass < 2; ++pass) {
        RawArray* t = pass == 0 ? reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(pawn) + 0x9A4) : DaudAnimTable(1, 0x0B0);
        for (int i = 0; t && t->Data && i < t->Num; ++i) {
            auto& s = *reinterpret_cast<FDisPawnAnimState*>(static_cast<uint8_t*>(t->Data) + i * 0x34);
            std::string n = s.m_StateName.ToString();
            if (n.find("Equip") == std::string::npos) continue;
            Log("  %s UpperBody_Items: %s", pass == 0 ? "Corvo" : "Daud ", AnimStateText(s).c_str());
        }
    }
    auto* sets = reinterpret_cast<RawArray*>(&pawn->Mesh->AnimSets);
    std::string names;
    for (int k = 0; sets->Data && k < sets->Num && k < 64; ++k) names += static_cast<UObject**>(sets->Data)[k]->GetName() + " ";
    Log("  1P arms anim sets: %s", names.c_str());
}

// Saves: PowersComponent::GameSave (0xAD3A60) writes (name, level+1) for each m_ActivePowers entry. While it runs,
// put Corvo's originals back in their slots and append Daud's components, so both sets' levels are saved under
// their own names whichever set is on. Loading goes through AddPower by name: Corvo's land on the pawn's own
// components, Daud's (no component yet) are caught by HookedAddPower and handed to MakeDaudPower.
using PowersGameSave_t = void(__thiscall*)(void*, void*, int);
static PowersGameSave_t g_origPowersGameSave = nullptr;
static void __fastcall HookedPowersGameSave(UDishonoredPowersComponent* pc, void*, void* ar, int loc) {
    auto* pawn = g_powerPawn;
    if (!pawn || pawn->m_pPowersComponent != pc) { g_origPowersGameSave(pc, ar, loc); return; }
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    int oldNum = arr->Num;
    UDishonoredActivePowerComponent* swappedOut[kPowerSwapCount] = {};
    int swappedIdx[kPowerSwapCount];
    int extra = 0;
    for (auto& s : g_powerSwaps) if (s.daud) ++extra;
    if (extra && !GrowArray(arr, 4, oldNum + extra)) extra = 0;
    for (int si = 0; si < kPowerSwapCount; ++si) {
        PowerSwap& s = g_powerSwaps[si];
        swappedIdx[si] = -1;
        if (!s.daud) continue;
        for (int i = 0; i < oldNum; ++i) {
            auto*& c = static_cast<UDishonoredActivePowerComponent**>(arr->Data)[i];
            if (c == s.daud && s.orig) { swappedOut[si] = c; swappedIdx[si] = i; c = s.orig; }
        }
        if (extra) static_cast<UDishonoredActivePowerComponent**>(arr->Data)[arr->Num++] = s.daud;
    }
    std::string list;
    for (int i = 0; i < arr->Num; ++i) {
        auto* c = static_cast<UDishonoredActivePowerComponent**>(arr->Data)[i];
        list += c->m_PowerName.ToString() + "=" + std::to_string(c->m_CurrentLevel) + " ";
    }
    Log("save: powers %s", list.c_str());
    g_origPowersGameSave(pc, ar, loc);
    arr->Num = oldNum;
    for (int si = 0; si < kPowerSwapCount; ++si)
        if (swappedIdx[si] >= 0) static_cast<UDishonoredActivePowerComponent**>(arr->Data)[swappedIdx[si]] = swappedOut[si];
}

// AddPower (sub_ACE270, thiscall(FName name, int level, int flag)): loads and purchases. A level for one of Daud's
// power names that has no component on the pawn yet (a save being loaded) is kept for MakeDaudPower.
using AddPower_t = void(__thiscall*)(void*, int, int, int, int);
static AddPower_t g_origAddPower = nullptr;
static void __fastcall HookedAddPower(UDishonoredPowersComponent* pc, void*, int nameIdx, int nameNum, int level, int flag) {
    g_origAddPower(pc, nameIdx, nameNum, level, flag);
    if (nameNum != 0) return;
    for (auto& s : g_powerSwaps) {
        static int ids[kPowerSwapCount] = { -2, -2 };
        int si = static_cast<int>(&s - g_powerSwaps);
        if (ids[si] == -2) ids[si] = FName(s.daudName).FNameEntryId;
        if (nameIdx != ids[si]) continue;
        if (s.daud && s.daudPc == pc) {
            if (g_powerPawn && !InPowerList(g_powerPawn, s.daud)) s.daud->m_CurrentLevel = level;   // held (Corvo's set on)
        } else {
            s.pendingPc = pc;
            s.pendingLevel = level;
        }
        Log("AddPower %s level %d%s", s.daudName, level, s.daud && s.daudPc == pc ? "" : " (kept for when it is made)");
    }
}

// ---- The pawn's own save (ADishonoredPawn::GameSave @0xB55BE0) writes m_ActivePowers as object references and its
// load (@0xB5C0A0) rebuilds the list from them. Daud's components are transient, so a save made with them in the
// list held references the loader could not resolve: FLevelLoader::operator<<(UObject*&) asserted (crash 13:11,
// 0xA0CABE). Build 163: save Corvo's originals in those slots; make that loader branch return null instead of
// asserting (rescues the saves made with builds 159-162); refill any empty slot from the pawn's own power components.
using PawnSaveLoad_t = void(__thiscall*)(void*, void*, int);
static PawnSaveLoad_t g_origPawnGameSave = nullptr, g_origPawnGameLoad = nullptr;

static const char* const kOwnPowerNames[] = { "PowerBlink", "PowerBendTime", "PowerWindBlast", "PowerPossession", "PowerDarkVision", "PowerDevouringSwarm" };
static const char* const kOwnPowerClasses[] = { "DishonoredActivePowerComponent_Blink", "DishonoredActivePowerComponent_BendTime",
    "DishonoredActivePowerComponent_WindBlast", "DishonoredActivePowerComponent_Possess", "DisActivePowerComponent_DarkVision",
    "DishonoredActivePowerComponent_DevouringSwarm" };

// The pawn's own power component for a slot (a subobject named from its class template), or null.
static UObject* FindOwnPower(ADishonoredPawn* pawn, int slot) {
    if (slot < 0 || slot >= 6) return nullptr;
    auto* objects = UObject::GObjObjects();
    for (int i = 0; i < objects->size(); ++i) {
        UObject* o = objects->at(i);
        if (o && o->Outer == pawn && o->GetName() == kOwnPowerNames[slot]) return o;
    }
    return nullptr;
}

// Every entry of the list that is not the pawn's own (a transient Daud component) is saved as the pawn's own
// component for that slot - independent of the swap bookkeeping, which a level change resets before the autosave
// (the saves that broke in builds 157-162 were written that way).
static void __fastcall HookedPawnGameSave(ADishonoredPawn* pawn, void*, void* ar, int loc) {
    UObject* restore[16] = {};
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    bool player = pawn && pawn->IsA(ADishonoredPlayerPawn::StaticClass());
    for (int i = 0; player && arr->Data && i < arr->Num && i < 16; ++i) {
        auto*& c = static_cast<UObject**>(arr->Data)[i];
        if (!c || c->Outer == pawn) continue;
        UObject* own = nullptr;
        for (auto& s : g_powerSwaps) if (s.daud == c && s.orig) own = s.orig;
        if (!own) own = FindOwnPower(pawn, i);
        if (!own) { Log("save: WARNING power slot %d holds %s and Corvo's component was not found", i, ObjName(c).c_str()); continue; }
        restore[i] = c;
        c = own;
    }
    g_origPawnGameSave(pawn, ar, loc);
    for (int i = 0; arr->Data && i < arr->Num && i < 16; ++i)
        if (restore[i]) static_cast<UObject**>(arr->Data)[i] = restore[i];
}

// Old Body Swap saves (builds 159-162): a power slot's reference points at a transient Daud component that is not in
// the save's object table, followed by that component's inline save data (written in Corvo's original format by the
// GameSave redirect). The loader's bad-index branch (patched to jump to BadLoadRefStub) asks ResolveBadLoadRef for
// a stand-in: a throwaway object of Corvo's class for that slot, which reads and discards exactly those bytes; the
// load hook then puts the pawn's real component in the slot.
static ADishonoredPawn* g_loadingPawn = nullptr;

extern "C" UObject* __cdecl ResolveBadLoadRef() {
    ADishonoredPawn* pawn = g_loadingPawn;
    if (!pawn) { Log("load: unresolvable object reference outside a player power list (left empty)"); return nullptr; }
    int slot = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers)->Num;
    if (slot < 0 || slot >= 6) { Log("load: unresolvable power reference in slot %d (left empty)", slot); return nullptr; }
    UClass* cls = UObject::FindClass((std::string("Class DishonoredGame.") + kOwnPowerClasses[slot]).c_str());
    UObject* stand = cls ? StaticConstructObject(cls, TransientPackage(), 0, 0, 0, nullptr, nullptr, nullptr, nullptr) : nullptr;
    Log("load: power slot %d referenced an old Body Swap component; reading its data with a stand-in %s", slot, ObjName(stand).c_str());
    return stand;
}

extern "C" __attribute__((naked)) void BadLoadRefStub() {
    __asm__ volatile(
        "pushl %ecx\n\t"
        "pushl %edx\n\t"
        "call _ResolveBadLoadRef\n\t"
        "popl %edx\n\t"
        "popl %ecx\n\t"
        "testl %eax, %eax\n\t"
        "je 1f\n\t"
        "movl 8(%ebp), %ebx\n\t"      // out UObject*&
        "movl %eax, (%ebx)\n\t"
        "pushl $0x00A0CACF\n\t"       // continue as if found: flag check, ShouldLoadObject, inline load
        "ret\n\t"
        "1:\n\t"
        "popl %esi\n\t"
        "pushl $0x00A0CB18\n\t"       // the function's own null result path
        "ret\n\t");
}

static void __fastcall HookedPawnGameLoad(ADishonoredPawn* pawn, void*, void* ar, int loc) {
    bool player = pawn && pawn->IsA(ADishonoredPlayerPawn::StaticClass());
    if (player) g_loadingPawn = pawn;
    g_origPawnGameLoad(pawn, ar, loc);
    g_loadingPawn = nullptr;
    if (!player) return;
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    int empty = 0;
    for (int i = 0; arr->Data && i < arr->Num; ++i) {
        auto*& c = static_cast<UObject**>(arr->Data)[i];
        if (c && c->Outer != pawn) c = nullptr;   // a stand-in from ResolveBadLoadRef
        if (!c) ++empty;
    }
    if (!empty) return;
    // The pawn's own power components (instanced from its class templates) not already in the list.
    static const char* kOwn[] = { "PowerBlink", "PowerBendTime", "PowerWindBlast", "PowerPossession", "PowerDarkVision", "PowerDevouringSwarm" };
    std::vector<UObject*> spare;
    auto* objects = UObject::GObjObjects();
    for (const char* name : kOwn) {
        for (int i = 0; i < objects->size(); ++i) {
            UObject* o = objects->at(i);
            if (!o || o->Outer != pawn || o->GetName() != name) continue;
            bool listed = false;
            for (int k = 0; k < arr->Num; ++k) if (static_cast<UObject**>(arr->Data)[k] == o) listed = true;
            if (!listed) spare.push_back(o);
        }
    }
    size_t next = 0;
    for (int i = 0; i < arr->Num; ++i) {
        auto*& c = static_cast<UObject**>(arr->Data)[i];
        if (c) continue;
        for (size_t k = 0; i < 6 && k < spare.size(); ++k)
            if (spare[k] && spare[k]->GetName() == kOwnPowerNames[i]) { c = spare[k]; spare[k] = nullptr; }
        while (!c && next < spare.size()) c = spare[next++];
        Log("load: power slot %d was an unloadable reference (old Body Swap save); filled with %s", i, ObjName(c).c_str());
    }
    // Anything still empty is dropped so nothing walks into a null.
    int w = 0;
    for (int i = 0; i < arr->Num; ++i) if (static_cast<UObject**>(arr->Data)[i]) static_cast<UObject**>(arr->Data)[w++] = static_cast<UObject**>(arr->Data)[i];
    arr->Num = w;
}

static void HookPawnSaveLoad() {
    const uint8_t saveExpect[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x8B, 0x45, 0x0C, 0x53, 0x56, 0x8B };
    if (memcmp(reinterpret_cast<void*>(0x00B55BE0), saveExpect, sizeof(saveExpect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x00B55BE0), reinterpret_cast<void*>(&HookedPawnGameSave),
                      reinterpret_cast<void**>(&g_origPawnGameSave)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00B55BE0)) == MH_OK) Log("pawn GameSave hooked");
    else Log("WARNING: could not hook pawn GameSave");
    const uint8_t loadExpect[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x83, 0xEC, 0x14, 0x53, 0x56, 0x57 };
    if (memcmp(reinterpret_cast<void*>(0x00B5C0A0), loadExpect, sizeof(loadExpect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x00B5C0A0), reinterpret_cast<void*>(&HookedPawnGameLoad),
                      reinterpret_cast<void**>(&g_origPawnGameLoad)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00B5C0A0)) == MH_OK) Log("pawn GameLoad hooked");
    else Log("WARNING: could not hook pawn GameLoad");
    // FLevelLoader::operator<<(UObject*&): an index past the object table -> null result instead of the assert.
    // At 0xA0CAA5 (push Format_0 ... call appFailAssertFunc): pop esi; jmp 0xA0CB18 (the function's own null path).
    uint8_t* p = reinterpret_cast<uint8_t*>(0x00A0CAA5);
    const uint8_t expect[] = { 0x68, 0x34, 0x9F, 0xFB, 0x00 };
    if (memcmp(p, expect, sizeof(expect)) == 0) {
        DWORD old;
        if (VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) {
            p[0] = 0xE9;                                      // jmp BadLoadRefStub
            *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&BadLoadRefStub) - (0x00A0CAA5 + 5));
            VirtualProtect(p, 8, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 8);
            Log("level loader: unresolvable object references go to BadLoadRefStub");
        }
    } else Log("WARNING: level loader patch site not as expected");
}

// ---- Pull on Corvo (build 166). Daud's Pull code (anim notifies, anim nodes, the component) reaches the Pull
// component through the player pawn's DLC07 field m_pPullPower (+0x126C) - ADisDLC07PlayerPawn only; on Corvo's
// pawn that is past the end of the object (crash 13:29: Pull notify sub_CFE170 -> sub_CFD200 on garbage). Every
// load of [reg+0x126C] in Daud's DLC code is patched to call a generated stub: if the base register is Corvo's pawn
// it yields our Pull component, otherwise it does the original load (so Daud's own DLC is untouched).
static void* volatile g_pullPawn = nullptr;   // Corvo's pawn while our Pull component exists
static void* volatile g_pullComp = nullptr;   // our Pull component

static void SetPullTarget(void* pawn, void* pull) { g_pullComp = pull; g_pullPawn = pull ? pawn : nullptr; }

static void PatchPullFieldLoads() {
    static const uint32_t kSites[] = { 0xCE8000, 0xCF0B33, 0xCF0B4B, 0xCF0B70, 0xCF0CEE, 0xCF0E07, 0xCF6359,
                                       0xCF6D78, 0xCF70DF, 0xCFCEB4, 0xCFCEE5, 0xCFE179, 0xCFE1C9 };
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) { Log("WARNING: Pull patch: no stub memory"); return; }
    uint8_t* s = mem;
    int patched = 0;
    for (uint32_t va : kSites) {
        auto* p = reinterpret_cast<uint8_t*>(va);
        if (p[0] != 0x8B || (p[1] & 0xC0) != 0x80 || (p[1] & 7) == 4 || *reinterpret_cast<uint32_t*>(p + 2) != 0x126C) {
            Log("WARNING: Pull patch: unexpected bytes at %08X", va);
            continue;
        }
        int dest = (p[1] >> 3) & 7, base = p[1] & 7;
        uint8_t* stub = s;
        *s++ = 0x9C;                                                                   // pushfd
        *s++ = 0x3B; *s++ = static_cast<uint8_t>((base << 3) | 5);                     // cmp base, [g_pullPawn]
        *reinterpret_cast<uint32_t*>(s) = reinterpret_cast<uint32_t>(&g_pullPawn); s += 4;
        *s++ = 0x75; *s++ = 8;                                                         // jne original
        *s++ = 0x9D;                                                                   // popfd
        *s++ = 0x8B; *s++ = static_cast<uint8_t>((dest << 3) | 5);                     // mov dest, [g_pullComp]
        *reinterpret_cast<uint32_t*>(s) = reinterpret_cast<uint32_t>(&g_pullComp); s += 4;
        *s++ = 0xC3;                                                                   // ret
        *s++ = 0x9D;                                                                   // original: popfd
        memcpy(s, p, 6); s += 6;                                                       //   mov dest, [base+0x126C]
        *s++ = 0xC3;                                                                   //   ret
        DWORD old;
        if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) continue;
        p[0] = 0xE8;                                                                   // call stub
        *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(stub - (p + 5));
        p[5] = 0x90;
        VirtualProtect(p, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 6);
        ++patched;
    }
    Log("Pull: %d of %d m_pPullPower loads redirected for Corvo's pawn", patched, static_cast<int>(sizeof(kSites) / 4));
}

// ---- Journal text (build 169). Req_PowerDetails gets a power's name and descriptions from
// UDisGlobalEnums::GetLocalizedPowerDescription(uiSelection, ...) (0xC45700, cdecl) - by slot type, and Daud's
// components carry Corvo's slot types so Corvo's journal can find them. With Daud's set on, ask for his types.
using GetPowerText_t = void(__cdecl*)(unsigned char, void*, void*, void*);
static GetPowerText_t g_origGetPowerText = nullptr;
static void __cdecl HookedGetPowerText(unsigned char ui, void* a, void* b, void* c) {
    unsigned char asked = ui;
    if (g_powerChar == 1)
        for (auto& s : g_powerSwaps) if (ui == s.corvoUi && s.daud && g_powerPawn && InPowerList(g_powerPawn, s.daud)) { ui = s.daudUi; break; }
    g_origGetPowerText(ui, a, b, c);
    auto str = [](void* f) { auto* r = static_cast<RawArray*>(f); return (r && r->Data) ? Narrow(static_cast<wchar_t*>(r->Data)).substr(0, 40) : std::string(); };
    if (asked != ui) Log("power text: ui %d -> %d: '%s'", asked, ui, str(a).c_str());
}

// The power NAME (journal list "powerName", wheel item info, ...) comes from sub_C4C6B0(ui, FString& out) - also by
// slot type. Same mapping (build 171).
using GetPowerName_t = void(__cdecl*)(unsigned char, void*);
static GetPowerName_t g_origGetPowerName = nullptr;
static void __cdecl HookedGetPowerName(unsigned char ui, void* out) {
    if (g_powerChar == 1)
        for (auto& s : g_powerSwaps) if (ui == s.corvoUi && s.daud && g_powerPawn && InPowerList(g_powerPawn, s.daud)) { ui = s.daudUi; break; }
    g_origGetPowerName(ui, out);
}

static void HookPowerText() {    const uint8_t expect[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x9C, 0xD2, 0xF5, 0x00, 0x64, 0xA1 };
    if (memcmp(reinterpret_cast<void*>(0x00C45700), expect, sizeof(expect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x00C45700), reinterpret_cast<void*>(&HookedGetPowerText),
                      reinterpret_cast<void**>(&g_origGetPowerText)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00C45700)) == MH_OK) Log("GetLocalizedPowerDescription hooked (journal text)");
    else Log("WARNING: could not hook GetLocalizedPowerDescription");
    const uint8_t nameExpect[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x30, 0xD5, 0xF5, 0x00, 0x64, 0xA1 };
    if (memcmp(reinterpret_cast<void*>(0x00C4C6B0), nameExpect, sizeof(nameExpect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x00C4C6B0), reinterpret_cast<void*>(&HookedGetPowerName),
                      reinterpret_cast<void**>(&g_origGetPowerName)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00C4C6B0)) == MH_OK) Log("power name lookup hooked");
    else Log("WARNING: could not hook the power name lookup");
}

static void HookPowerLevels() {    const uint8_t pgsExpect[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x8B, 0x41, 0x48, 0x53, 0x8B, 0x59 };
    if (memcmp(reinterpret_cast<void*>(0x00AD3A60), pgsExpect, sizeof(pgsExpect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x00AD3A60), reinterpret_cast<void*>(&HookedPowersGameSave),
                      reinterpret_cast<void**>(&g_origPowersGameSave)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00AD3A60)) == MH_OK) Log("PowersComponent::GameSave hooked");
    else Log("WARNING: could not hook PowersComponent::GameSave");
    const uint8_t apExpect[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24, 0x8B, 0x45, 0x0C, 0x53, 0x56, 0x57 };
    if (memcmp(reinterpret_cast<void*>(0x00ACE270), apExpect, sizeof(apExpect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x00ACE270), reinterpret_cast<void*>(&HookedAddPower),
                      reinterpret_cast<void**>(&g_origAddPower)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00ACE270)) == MH_OK) Log("AddPower hooked");
    else Log("WARNING: could not hook AddPower");
}

// Diagnostic (build 156): the first-person hand looked like Daud's with only the power set on Daud. Log every
// change to the 1P arms' mesh / material overrides, with each material instance's parent chain.
static void WatchArmsMaterials(ADishonoredPlayerPawn* pawn) {
    USkeletalMeshComponent* arms = pawn ? pawn->Mesh : nullptr;
    if (!arms) return;
    static void* last[9] = {};
    auto* mats = reinterpret_cast<RawArray*>(&arms->Materials);
    void* now[9] = { arms->SkeletalMesh };
    for (int i = 0; mats->Data && i < mats->Num && i < 8; ++i) now[i + 1] = static_cast<void**>(mats->Data)[i];
    if (memcmp(now, last, sizeof(now)) == 0) return;
    memcpy(last, now, sizeof(now));
    Log("1P arms now: mesh %s, %d material override(s)", ObjName(arms->SkeletalMesh).c_str(), mats->Data ? mats->Num : 0);
    for (int i = 0; mats->Data && i < mats->Num && i < 8; ++i) {
        UObject* m = static_cast<UObject**>(mats->Data)[i];
        std::string chain = ObjName(m);
        for (int depth = 0; m && depth < 4 && m->IsA(UMaterialInstance::StaticClass()); ++depth) {
            m = static_cast<UMaterialInstance*>(m)->Parent;
            chain += " <- " + ObjName(m);
        }
        Log("   [%d] %s", i, chain.c_str());
    }
}
// ---- Crossbow -> Daud's wristbow (build 180). The crossbow item reads its settings through its tweaks pointer
// (+0x134, Twk_Inv_CrossbowCorvo): prefix Guns_, sockets LeftHandWpn/HolsterWpn, arm sets Ply_Guns_as/Ply_Crossbow_as.
// Daud's DLC07_Inv_WristbowDaud: prefix Wristbow_, socket Wristbow, arm set DLC06_PlyDaud_Wristbow_as, item set
// DLC06_Wristbow_Anim_as, same item tree. Point the item at Daud's settings while it is not in hand and let the game
// re-apply them (IDisTweaksInterface::ApplyTweakChanges @0xC82A30 -> ApplyTweakChanges_Derived: mesh, item anim
// sets, tree). Corvo's arms need the Wristbow socket (borrowed from Daud's arms mesh, same skeleton), the wristbow
// arm anim set and any Wristbow_* anim states Daud's pawn defines.
static UDisTweaks_InventoryItem* g_corvoBowTweaks = nullptr;
static UDisTweaks_InventoryItem* g_daudBowTweaks = nullptr;

static UDishonoredInventoryItem* FindItem(ADishonoredPlayerPawn* pawn, const char* what) {
    if (!pawn || !pawn->m_pInventory) return nullptr;
    auto* slots = reinterpret_cast<RawArray*>(&pawn->m_pInventory->m_Slots);
    for (int i = 0; slots->Data && i < slots->Num && i < 64; ++i) {
        auto* item = *reinterpret_cast<UDishonoredInventoryItem**>(static_cast<uint8_t*>(slots->Data) + i * 12);
        if (item && item->Class && item->Class->GetName().find(what) != std::string::npos) return item;
    }
    return nullptr;
}

static UDisTweaks_InventoryItem*& ItemTweaks(UDishonoredInventoryItem* item) {
    return *reinterpret_cast<UDisTweaks_InventoryItem**>(reinterpret_cast<uint8_t*>(item) + 0x134);
}

static void ApplyItemTweaks(UDishonoredInventoryItem* item) {
    reinterpret_cast<void(__thiscall*)(void*)>(0x00C82A30)(reinterpret_cast<uint8_t*>(item) + 0x40);   // its IDisTweaksInterface
}

static bool AddArmsAnimSet(ADishonoredPlayerPawn* pawn, const char* name) {
    auto* set = static_cast<UAnimSet*>(FindObjectByName("AnimSet", name));
    if (!set || !pawn->Mesh) return false;
    KeepAlive(set);
    auto* sets = reinterpret_cast<RawArray*>(&pawn->Mesh->AnimSets);
    for (int i = 0; sets->Data && i < sets->Num; ++i) if (static_cast<UObject**>(sets->Data)[i] == set) return true;
    if (sets->Max < sets->Num + 1 && !GrowArray(sets, 4, sets->Num + 4)) return false;
    static_cast<UObject**>(sets->Data)[sets->Num++] = set;
    pawn->Mesh->UpdateAnimations();
    Log("wristbow: %s added to the first-person arms", name);
    return true;
}

static void EnsureWristbowSocket(ADishonoredPlayerPawn* pawn) {
    USkeletalMesh* mesh = pawn->Mesh ? pawn->Mesh->SkeletalMesh : nullptr;
    if (!mesh || !g_daudArmsMesh || mesh == g_daudArmsMesh) return;
    auto* mine = reinterpret_cast<RawArray*>(&mesh->Sockets);
    for (int i = 0; mine->Data && i < mine->Num; ++i) {
        auto* sk = static_cast<USkeletalMeshSocket**>(mine->Data)[i];
        if (sk && sk->SocketName.ToString() == "Wristbow") return;
    }
    auto* his = reinterpret_cast<RawArray*>(&g_daudArmsMesh->Sockets);
    for (int i = 0; his->Data && i < his->Num; ++i) {
        auto* sk = static_cast<USkeletalMeshSocket**>(his->Data)[i];
        if (!sk || sk->SocketName.ToString() != "Wristbow") continue;
        if (mine->Max < mine->Num + 1 && !GrowArray(mine, 4, mine->Num + 4)) return;
        static_cast<USkeletalMeshSocket**>(mine->Data)[mine->Num++] = sk;
        KeepAlive(sk);
        Log("wristbow: socket Wristbow (bone %s) borrowed from Daud's arms for %s", sk->BoneName.ToString().c_str(), ObjName(mesh).c_str());
        return;
    }
    Log("wristbow: Daud's arms mesh has no Wristbow socket");
}

// Daud's pawn anim states that mention the wristbow, copied into the same table on Corvo's pawn, maps rebuilt.
static void EnsureWristbowStates(ADishonoredPlayerPawn* pawn) {
    static ADishonoredPlayerPawn* doneFor = nullptr;
    if (doneFor == pawn || !g_daudPawnTweaks) return;
    doneFor = pawn;
    int added = 0;
    for (const AnimTable& tb : kAnimTables) {
        RawArray* src = DaudAnimTable(tb.tweakKind, tb.tweakOff);
        auto* dst = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(pawn) + tb.pawnOff);
        for (int i = 0; src && src->Data && i < src->Num; ++i) {
            auto* st = reinterpret_cast<FDisPawnAnimState*>(static_cast<uint8_t*>(src->Data) + i * 0x34);
            std::string text = AnimStateText(*st);
            if (text.find("Wristbow") == std::string::npos) continue;
            std::string name = st->m_StateName.ToString();
            if (HasAnimState(dst, name.c_str())) continue;
            if (dst->Max < dst->Num + 1 && !GrowArray(dst, 0x34, dst->Num + 8)) continue;
            FDisPawnAnimState copy = CopyAnimState(*st);
            memcpy(static_cast<uint8_t*>(dst->Data) + dst->Num * 0x34, &copy, 0x34);
            ++dst->Num;
            ++added;
            Log("wristbow: + %s %s", tb.name, text.c_str());
        }
    }
    if (added) { reinterpret_cast<void(__thiscall*)(void*)>(0x00AE0F90)(pawn); Log("wristbow: %d anim states added, maps rebuilt", added); }
    else Log("wristbow: no wristbow-specific anim states needed");
}

// Build 181: the settings-pointer swap (build 180) left Corvo's fire contexts on a re-initialised item - neither bow
// fired and Corvo's came back sideways. Daud's DLC makes a different item class (UDisDLC06WepCrossbow) from his
// settings, so do the same: swap whole ITEMS in the crossbow's inventory slot. Daud's: the game's own add-item
// routine (sub_C19CD0: class from the settings, construct, init from settings, insert into the required slot).
// Corvo's original is kept aside (rooted) and put back with the insert routine (sub_C19380).
static UDishonoredInventoryItem* g_bowKept[2] = {};      // [0] Corvo's crossbow, [1] Daud's wristbow (not in the slot)
static ADishonoredPlayerPawn* g_bowPawn = nullptr;

static int CrossbowSlot(ADishonoredPlayerPawn* pawn) {
    if (!pawn || !pawn->m_pInventory) return -1;
    auto* slots = reinterpret_cast<RawArray*>(&pawn->m_pInventory->m_Slots);
    for (int i = 0; slots->Data && i < slots->Num && i < 64; ++i) {
        auto* item = *reinterpret_cast<UDishonoredInventoryItem**>(static_cast<uint8_t*>(slots->Data) + i * 12);
        if (item && item->Class && item->Class->GetName().find("Crossbow") != std::string::npos) return i;
    }
    return -1;
}

static UDishonoredInventoryItem*& SlotItem(ADishonoredPlayerPawn* pawn, int slot) {
    auto* slots = reinterpret_cast<RawArray*>(&pawn->m_pInventory->m_Slots);
    return *reinterpret_cast<UDishonoredInventoryItem**>(static_cast<uint8_t*>(slots->Data) + slot * 12);
}

static void ForgetBowItems() {
    for (auto*& it : g_bowKept) { LetGo(it); it = nullptr; }
    g_bowPawn = nullptr;
    // Found again next level (the wristbow's holder restriction lifted again). Corvo's settings are a campaign asset
    // the engine may root itself, so only forget that pointer; Daud's came from our package load.
    g_corvoBowTweaks = nullptr;
    LetGo(g_daudBowTweaks);
    g_daudBowTweaks = nullptr;
}

// Build 182: the insert (sub_C19380) refuses an item whose settings name m_pOnlyHeldByPawnType (+0xE0) when the
// inventory's owner lacks those pawn settings (IDisEngineTweaksInterface::HasTweaks) - the wristbow's names Daud's
// pawn, so every add returned null (build 181 retried it every frame). Only that insert reads it with HasTweaks.
static bool IsDaudBow(UDishonoredInventoryItem* item) {
    return item && ((g_daudBowTweaks && ItemTweaks(item) == g_daudBowTweaks) || item->Class->GetName() == "DisDLC06WepCrossbow");
}

static void ApplyCrossbowCharacter(ADishonoredPlayerPawn* pawn) {
    if (pawn != g_bowPawn) { ForgetBowItems(); g_bowPawn = pawn; }
    int slot = CrossbowSlot(pawn);
    if (slot < 0) return;
    UDishonoredInventoryItem* cur = SlotItem(pawn, slot);
    // Build 192: the socket was only added when the wristbow was made. Made with Daud's arms on (his mesh has the
    // Wristbow socket), then the arms switched to Corvo's: Skm_Player had none, so the wristbow had nowhere to attach -
    // invisible, and the fire animation played but nothing fired or sounded (user, log 19:23). Whenever the arms mesh
    // (or the arms component) changes while the wristbow is carried, give that mesh the socket and the arm set.
    if (IsDaudBow(cur) && g_daudLoadState == LoadState::Loaded && pawn->Mesh) {
        static USkeletalMeshComponent* compSeen = nullptr;
        static USkeletalMesh* meshSeen = nullptr;
        if (pawn->Mesh != compSeen || pawn->Mesh->SkeletalMesh != meshSeen) {
            compSeen = pawn->Mesh;
            meshSeen = pawn->Mesh->SkeletalMesh;
            EnsureWristbowSocket(pawn);
            AddArmsAnimSet(pawn, "DLC06_PlyDaud_Wristbow_as");
        }
    }
    if (cur == g_handItem[0] || cur == g_handItem[1]) return;   // only while it is put away
    if (!g_corvoBowTweaks) {
        g_corvoBowTweaks = static_cast<UDisTweaks_InventoryItem*>(FindObjectByName("DisTweaks_WepCrossbow", "Twk_Inv_CrossbowCorvo"));
        if (g_corvoBowTweaks) KeepAlive(g_corvoBowTweaks);
    }
    bool isDaud = IsDaudBow(cur);
    bool wantDaud = g_bowChar == 1;
    // A failed swap is not retried until the menu choice changes again.
    static int failedWant = -1, lastChoice = -1;
    if (lastChoice != g_bowChar) { lastChoice = g_bowChar; failedWant = -1; }
    if (wantDaud == isDaud || failedWant == (wantDaud ? 1 : 0)) return;
    if (wantDaud) {
        if (g_daudLoadState != LoadState::Loaded) return;
        if (!g_daudBowTweaks) {
            g_daudBowTweaks = static_cast<UDisTweaks_InventoryItem*>(FindObjectByName("DisDLC06Tweaks_WepCrossbow", "DLC07_Inv_WristbowDaud"));
            if (!g_daudBowTweaks) { static bool warned = false; if (!warned) { warned = true; Log("wristbow: DLC07_Inv_WristbowDaud not found"); } return; }
            KeepAlive(g_daudBowTweaks);
            auto*& onlyFor = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(g_daudBowTweaks) + 0xE0);   // m_pOnlyHeldByPawnType
            if (onlyFor) { Log("wristbow: lifted its holder restriction (%s) so Corvo can carry it", ObjName(onlyFor).c_str()); onlyFor = nullptr; }
        }
        EnsureWristbowSocket(pawn);
        AddArmsAnimSet(pawn, "DLC06_PlyDaud_Wristbow_as");
        EnsureWristbowStates(pawn);
    } else if (!g_corvoBowTweaks && !g_bowKept[0]) {
        return;
    }
    UDisTweaks_InventoryItem* want = wantDaud ? g_daudBowTweaks : g_corvoBowTweaks;
    uint8_t usage = *(reinterpret_cast<uint8_t*>(want) + 0x8D);   // m_RequiredEquipUsage
    // Take the current one out of the slot and keep it.
    LetGo(g_bowKept[isDaud ? 1 : 0]);
    g_bowKept[isDaud ? 1 : 0] = cur;
    KeepAlive(cur);
    SlotItem(pawn, slot) = nullptr;
    UDishonoredInventory* inv = pawn->m_pInventory;
    UDishonoredInventoryItem* in = g_bowKept[wantDaud ? 1 : 0];
    if (in) {
        unsigned ok = reinterpret_cast<unsigned(__thiscall*)(void*, void*, int, int, int, int)>(0x00C19380)(inv, in, 0, 0, 0, usage);
        if (ok) { LetGo(in); g_bowKept[wantDaud ? 1 : 0] = nullptr; }
        Log("wristbow: %s %s back into slot %d (%s)", ok ? "put" : "FAILED to put", ObjName(in).c_str(), slot, ok ? "ok" : "restoring");
        if (!ok) { SlotItem(pawn, slot) = cur; g_bowKept[isDaud ? 1 : 0] = nullptr; LetGo(cur); failedWant = wantDaud ? 1 : 0; }
    } else {
        auto* made = reinterpret_cast<UDishonoredInventoryItem*(__thiscall*)(void*, void*, int, int, int, int)>(0x00C19CD0)(inv, want, 0, 0, 0, usage);
        Log("wristbow: made %s (class %s) from %s in slot %d", ObjName(made).c_str(), made ? made->Class->GetName().c_str() : "-",
            ObjName(want).c_str(), slot);
        if (!made || SlotItem(pawn, slot) != made) {
            Log("wristbow: it did not land in the crossbow slot - restoring the old one (not retrying until the choice changes)");
            if (!SlotItem(pawn, slot)) SlotItem(pawn, slot) = cur;
            g_bowKept[isDaud ? 1 : 0] = nullptr;
            LetGo(cur);
            failedWant = wantDaud ? 1 : 0;
        }
    }
}

// Saves never reference Daud's wristbow: the player's save runs with Corvo's crossbow in the slot.
using PlayerGameSave_t = void(__thiscall*)(void*, void*, int);
static PlayerGameSave_t g_origPlayerGameSave = nullptr;
static void __fastcall HookedPlayerGameSave(ADishonoredPlayerPawn* pawn, void*, void* ar, int loc) {
    int slot = pawn == g_bowPawn ? CrossbowSlot(pawn) : -1;
    UDishonoredInventoryItem* daud = slot >= 0 ? SlotItem(pawn, slot) : nullptr;
    bool isDaud = IsDaudBow(daud);
    if (isDaud && !g_bowKept[0] && g_corvoBowTweaks) {
        // The wristbow came over a level change with no Corvo crossbow kept: make one for the save (and keep it).
        uint8_t usage = *(reinterpret_cast<uint8_t*>(g_corvoBowTweaks) + 0x8D);
        SlotItem(pawn, slot) = nullptr;
        auto* made = reinterpret_cast<UDishonoredInventoryItem*(__thiscall*)(void*, void*, int, int, int, int)>(0x00C19CD0)(
            pawn->m_pInventory, g_corvoBowTweaks, 0, 0, 0, usage);
        if (made && SlotItem(pawn, slot) == made) { g_bowKept[0] = made; KeepAlive(made); Log("save: made Corvo's crossbow to save in place of the wristbow"); }
        SlotItem(pawn, slot) = daud;
    }
    bool swapped = isDaud && g_bowKept[0];
    if (swapped) SlotItem(pawn, slot) = g_bowKept[0];
    g_origPlayerGameSave(pawn, ar, loc);
    if (swapped) SlotItem(pawn, slot) = daud;
}
static void HookPlayerSave() {
    const uint8_t expect[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x53, 0x56, 0x8B, 0x75, 0x08, 0x57 };
    if (memcmp(reinterpret_cast<void*>(0x00AB8770), expect, sizeof(expect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x00AB8770), reinterpret_cast<void*>(&HookedPlayerGameSave),
                      reinterpret_cast<void**>(&g_origPlayerGameSave)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00AB8770)) == MH_OK) Log("player GameSave hooked (crossbow settings)");
    else Log("WARNING: could not hook the player GameSave");
}

static void ApplyArmsCharacter(ADishonoredPlayerPawn* pawn) {    if (!pawn || !pawn->Mesh) return;
    if (!g_bodyBuiltTick && g_body) g_bodyBuiltTick = GetTickCount();
    bool needPackage = (g_armsChar == 1 && !g_daudArmsMesh) || (g_bladeChar == 1 && !g_daudSwordMesh) || (g_bowChar == 1 && !g_daudBowMesh) ||
                       (g_powerChar == 1 && g_daudLoadState != LoadState::Loaded);
    if (needPackage) {
        // Load only once our body has been in use a while: a second package load before that tore down the
        // first package's mesh data (tests 12-15).
        if (g_daudLoadState == LoadState::Idle && g_ovsLoadState != LoadState::Loading && LoadPackageAsync && g_body &&
            g_bodyAnimated && GetTickCount() - g_bodyBuiltTick > 3000) {
            Log("Body Swap: async loading %s", Narrow(kDaudPackage).c_str());
            g_daudLoadState = LoadState::Loading;
            g_daudLoadStartTick = GetTickCount();
            FString name(kDaudPackage);
            LoadPackageAsync(name, &OnDaudPackageLoaded, nullptr, nullptr);
        } else if (g_daudLoadState == LoadState::Loading && GetTickCount() - g_daudLoadStartTick > 30000) {
            Log("Body Swap: Daud package load timed out");
            g_daudLoadState = LoadState::Failed;
        }
    }
    Protect(g_daudArmsMesh);
    Protect(g_daudSwordMesh);
    Protect(g_daudBowMesh);
    // Overseer disguise (build 200): its package, on demand, one package load at a time (never alongside Daud's).
    bool needOvs = (g_bodyChar == 2 && !g_ovsBodyMesh) || (g_armsChar == 2 && !g_ovsArmsMesh) || (g_bladeChar == 2 && !g_ovsSwordMesh);
    if (needOvs) {
        if (g_ovsLoadState == LoadState::Idle && g_daudLoadState != LoadState::Loading && LoadPackageAsync && g_body &&
            g_bodyAnimated && GetTickCount() - g_bodyBuiltTick > 3000) {
            Log("Body Swap: async loading %s (Overseer disguise)", Narrow(kOvsPackage).c_str());
            g_ovsLoadState = LoadState::Loading;
            g_ovsLoadStartTick = GetTickCount();
            FString name(kOvsPackage);
            LoadPackageAsync(name, &OnOvsPackageLoaded, nullptr, nullptr);
        } else if (g_ovsLoadState == LoadState::Loading && GetTickCount() - g_ovsLoadStartTick > 45000) {
            Log("Body Swap: Overseer package load timed out");
            g_ovsLoadState = LoadState::Failed;
        }
    }
    for (USkeletalMesh* m : { g_ovsBodyMesh, g_ovsHeadMesh, g_ovsArmsMesh, g_ovsSwordMesh }) Protect(m);

    // Build 183: Daud's package finished loading mid-slide (log 16:29:46) and every swap landed at once - arms mesh,
    // sword, wristbow item, anim sets, anim state tables + map rebuilds, powers - and the slide never ended. Change
    // things only once the player has been on the ground in plain walking physics for a moment; slides, mantles,
    // climbs, jumps and falls wait.
    static DWORD settledSince = 0;
    bool settled = static_cast<int>(pawn->Physics) == 1 && !g_isSliding && !g_mantleWant && !g_isClimbing;
    if (!settled) { settledSince = 0; return; }
    if (!settledSince) settledSince = GetTickCount();
    if (GetTickCount() - settledSince < 300) return;

    DumpPowersOnce(pawn);
    USkeletalMesh* armsAlt = g_armsChar == 1 ? g_daudArmsMesh : g_armsChar == 2 ? g_ovsArmsMesh : nullptr;
    USkeletalMesh* arms = ApplyMeshSwap(pawn->Mesh, armsAlt, { g_daudArmsMesh, g_ovsArmsMesh }, g_armsSwap, "1P arms");
    if (arms && g_armsShadow && g_armsShadow->SkeletalMesh != arms) g_armsShadow->SetSkeletalMesh(arms, false);
    USkeletalMeshComponent* swordComp = FindSwordMesh(pawn);
    USkeletalMesh* swordAlt = g_bladeChar == 1 ? g_daudSwordMesh : g_bladeChar == 2 ? g_ovsSwordMesh : nullptr;
    USkeletalMesh* sword = ApplyMeshSwap(swordComp, swordAlt, { g_daudSwordMesh, g_ovsSwordMesh }, g_swordSwap, "sword");
    // Corvo's sword has its own anim tree (CorvoSword_at, PlySword_Anim_as) that flicks the folding blade open in
    // sync with the arms. Daud's sword item has none (one rigid bone), so hold his blade in its reference pose:
    // it just comes out with the hand. Put the tree back for Corvo's blade. The Overseer's sword is one rigid bone too.
    static USkeletalMeshComponent* refposed = nullptr;
    bool freeze = swordComp && sword && (sword == g_daudSwordMesh || sword == g_ovsSwordMesh);
    if (freeze && (refposed != swordComp || !swordComp->bForceRefpose)) {
        swordComp->bForceRefpose = 1;
        refposed = swordComp;
        Log("Body Swap: %s blade held in its reference pose (no fold-out animation)", sword == g_ovsSwordMesh ? "the Overseer's" : "Daud's");
    } else if (!freeze && refposed) {
        if (refposed == swordComp) { swordComp->bForceRefpose = 0; Log("Body Swap: sword animation back on"); }
        refposed = nullptr;
    }
    ApplyCrossbowCharacter(pawn);
    WatchArmsMaterials(pawn);
    ApplyPowerSet(pawn);
}

// Guard against test 14's crash: UpdateSkelPoseEnd reads LocalAtoms[0] unchecked, and the array was empty a few
// seconds after switching the body to its own tree. If it is empty (or the wrong size), give it a neutral pose
// from the engine heap so the engine's own code can take over, and log the state around it.
static void EnsureLocalAtoms(USkeletalMeshComponent* comp) {
    if (!comp || !g_bodyAnimated) return;
    auto* local = reinterpret_cast<RawArray*>(&comp->LocalAtoms);
    auto* space = reinterpret_cast<RawArray*>(&comp->SpaceBases);
    int bones = space->Num > 0 ? space->Num : 126;   // Corvo's body has 126 bones
    static int logged = 0;
    if (local->Num == bones && local->Data) return;
    if (logged++ < 20)
        Log("LocalAtoms %d (want %d), SpaceBases %d, parent %p, anims %p, hidden %d: refilling", local->Num, bones,
            space->Num, comp->ParentAnimComponent, comp->Animations, comp->HiddenGame);
    struct BoneAtom { float qx, qy, qz, qw, tx, ty, tz, scale; };
    static_assert(sizeof(BoneAtom) == 0x20, "FBoneAtom is 32 bytes");
    auto* atoms = static_cast<BoneAtom*>(EngineMalloc(sizeof(BoneAtom) * bones));
    if (!atoms) return;
    for (int i = 0; i < bones; ++i) atoms[i] = { 0, 0, 0, 1, 0, 0, 0, 1 };
    // The old buffer (if any) is leaked deliberately: freeing engine memory we didn't allocate is riskier.
    local->Data = atoms;
    local->Num = local->Max = bones;
}

// Find or load the NPC locomotion set, then switch the existing body from the 1P pose copy to its own tree.
static void UpdateBodyAnimation() {
    if (!g_body || g_bodyAnimated) return;
    if (!g_animSet) {
        switch (g_animLoadState) {
        case LoadState::Idle:
            g_animSet = static_cast<UAnimSet*>(FindObjectByName("AnimSet", kAnimSetName));
            if (g_animSet) {
                Log("anim set already in memory: %s", g_animSet->GetFullName().c_str());
                break;
            }
            Log("async loading %s for anim set %s", Narrow(kAnimPackage).c_str(), kAnimSetName);
            g_animLoadState = LoadState::Loading;
            g_animLoadStartTick = GetTickCount();
            {
                FString name(kAnimPackage);
                LoadPackageAsync(name, &OnAnimPackageLoaded, nullptr, nullptr);
            }
            return;
        case LoadState::Loading:
            if (GetTickCount() - g_animLoadStartTick > 30000) { Log("anim package timed out"); g_animLoadState = LoadState::Failed; }
            return;
        case LoadState::Loaded:
            g_animSet = static_cast<UAnimSet*>(FindObjectByName("AnimSet", kAnimSetName));
            Log("anim set after load: %s", g_animSet ? g_animSet->GetFullName().c_str() : "NOT FOUND");
            if (!g_animSet) { g_animLoadState = LoadState::Failed; return; }
            break;
        case LoadState::Failed:
            return;
        }
        KeepAlive(g_animSet);
    }
    g_body->SetParentAnimComponent(nullptr);
    g_bodyAnimated = SetupBodyAnimation(g_body);
    if (!g_bodyAnimated) {
        Log("anim tree setup failed, going back to the 1P pose copy");
        g_animLoadState = LoadState::Failed;
        if (g_bodyPawn && g_bodyPawn->Mesh) { g_body->SetParentAnimComponent(g_bodyPawn->Mesh); g_body->UpdateParentBoneMap(); }
    }
    ApplyArmHiding();
}

static void SyncTransform(ADishonoredPlayerPawn* pawn) {
    if (g_cfg.attachMode != 1 || !g_body || !pawn->Mesh) return;
    USkeletalMeshComponent* arms = pawn->Mesh;
    FVector t = arms->Translation;
    t.X -= g_armsShiftLocal.X; t.Y -= g_armsShiftLocal.Y; t.Z -= g_armsShiftLocal.Z;   // rigid camera moved the 1P arms, not the body
    // (Build 66 pinned the body to the standing arms Z: wrong. The game lowers the 1P arms - and the camera, which
    // rides them - after slides/jumps on purpose; pinning floated the body in front of the view.)
    t.X += g_cfg.offsetX; t.Y += g_cfg.offsetY; t.Z += g_cfg.offsetZ;
    // Ease the crouch offset in and out (~0.25 s).
    static DWORD last = 0;
    static float crouchBlend = 0.0f;
    DWORD now = GetTickCount();
    float dt = last ? Clamp((now - last) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    last = now;
    crouchBlend += Clamp((g_isCrouched ? 1.0f : 0.0f) - crouchBlend, -dt * 4.0f, dt * 4.0f);
    t.X += crouchBlend * g_cfg.crouchOffsetX;
    t.Z += crouchBlend * g_cfg.crouchOffsetZ;
    // The game drops the pawn by the full cylinder delta in one frame; let the body follow it over ~0.15 s.
    // (Planted-feet IK measures the root afterwards, so the feet stay on the floor during the catch-up.)
    // Ease normal crouch/stand changes; slides and other big drops (54.5) follow fast - easing those at the crouch
    // speed left the body hanging above the camera at slide start (video 18-39-19).
    // Down into a slide fast (the body hung above the camera otherwise); back up out of it gently, like the crouch.
    bool goingDown = g_crouchSink > g_smoothedSink;
    float sinkSpeed = (goingDown && (g_isSliding || g_crouchSink - g_smoothedSink > 30.0f)) ? 600.0f
                    : (!goingDown && g_smoothedSink - g_crouchSink > 15.0f) ? g_cfg.slideRecoverSpeed : g_cfg.crouchSmoothSpeed;
    g_smoothedSink += Clamp(g_crouchSink - g_smoothedSink, -dt * sinkSpeed, dt * sinkSpeed);
    t.Z += g_crouchSink - g_smoothedSink;
    t.Z += g_slideAmount * g_cfg.slideRaise;   // slide: hips just above the ground (was raised into the camera, video 17-19-35)
    t.X -= g_slideAmount * g_cfg.slideBack;
    // On a chain/ladder: shift the body sideways so the chain runs between the legs (user: move both legs left).
    t.Y -= g_climbAmount * g_cfg.climbSideShift;
    t.X += g_climbAmount * g_cfg.climbForwardShift;
    // Slide just ended: the game stands the pawn up at once while the slide eases out, and the camera passed through
    // the hips (video 18-16-03). While recovering, tuck the body down and back out of the view.
    {
        // Rising out of a slide: how far the body still is below the crouch (1 = slide depth, 0 = crouched). Moving
        // it back while it rises keeps the hips behind the camera, so it reads as one motion instead of the clip
        // guard hiding the legs (user).
        // Only after a slide (standing up from a normal crouch also has sink left and slid the feet back - user).
        static bool afterSlide = false;
        if (g_isSliding) afterSlide = true;
        float recover = 0.0f;
        if (afterSlide && !g_isSliding) {
            recover = Clamp((g_smoothedSink - g_crouchSink) / 30.0f, 0.0f, 1.0f);
            if (g_slideAmount > recover) recover = g_slideAmount;
            if (recover <= 0.0f) afterSlide = false;
        }
        t.Z -= recover * g_cfg.slideRecoverDown;
        t.X -= recover * g_cfg.slideRecoverBack;
    }
    t.Z += g_landDip;   // landing squat (both bodies; the planted feet bend the knees)
    // Camera bob/lean/mantle moves the view up to ~75 uu from the eyes (logged); the body stayed put and the camera
    // ended up inside the collar. Shift the visible body (not the shadow) with the camera's horizontal offset, in
    // player space, and hide the upper body when the camera dips below the eyes or the player is airborne.
    FVector bodyT = t;
    // Rigid camera: the fixed view sits where the game's camera is when looking fully down, up to ~25 uu ahead of
    // the level-look view, so the holstered arms ended up behind the camera (build 60). Move the visible body
    // (not the shadow) forward by the same amount; looking down that shift is ~0, so the feet stay put there.
    if (g_cfg.rigidCameraBodyFollow) {
        float yaw = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
        float cy = cosf(yaw), sy = sinf(yaw);
        bodyT.X += g_bodyFollowTarget.X * cy + g_bodyFollowTarget.Y * sy;
        bodyT.Y += -g_bodyFollowTarget.X * sy + g_bodyFollowTarget.Y * cy;
    }
    // Corner lean: the camera slides ~50 uu sideways and the body dropped out of view (video 14-55-18). Shift the
    // body's hips part of the way with it (CornerLeanHips); UpdateLean tilts the spine for the rest.
    if (g_cfg.cornerLean && !g_isSliding && !g_isClimbing) {
        static float hips = 0.0f;
        float want = (fabsf(g_cornerLeanRight) > 3.0f && !g_hitActive) ? g_cornerLeanRight * Clamp(g_cfg.cornerLeanHips, 0.0f, 1.0f) : 0.0f;
        hips += Clamp(want - hips, -dt * 200.0f, dt * 200.0f);
        bodyT.Y += hips;
        t.Y += hips;   // the shadow body and head shadow lean the same way
    }
    // Jumping: the camera dropped into the collar/shoulder pads looking down mid-jump, worst when running
    // (video 15-15-55). Ease the visible body back and down with the jump pose.
    bodyT.X -= g_jumpBodyW * g_cfg.jumpBodyBack;
    bodyT.Z -= g_jumpBodyW * g_cfg.jumpBodyDown;
    // View guard (UpdateVisibility): eased back just enough that no upper-body part is in view.
    bodyT.X -= g_viewGuardBack;
    // Sprinting leans the body forward and the collar/neck edged into view (user): ease the visible body back
    // while sprinting instead of hiding the upper body (that also hid the holstered arms).
    {
        static float sprintBack = 0.0f;
        FVector v = pawn->Velocity;
        float speed = sqrtf(v.X * v.X + v.Y * v.Y);
        float want = (!g_isCrouched && static_cast<int>(pawn->Physics) == 1)
            ? Clamp((speed - g_cfg.runBlendStart) / (g_cfg.runBlendEnd - g_cfg.runBlendStart + 1.0f), 0.0f, 1.0f) : 0.0f;
        sprintBack += Clamp(want - sprintBack, -dt * 4.0f, dt * 4.0f);
        bodyT.X -= sprintBack * g_cfg.sprintBodyBack;
        // Crouch-walking leans the chest forward into the bottom of the view (video 17-38-35): ease the visible body
        // back while moving crouched.
        static float crouchBack = 0.0f;
        float cw = (g_isCrouched && !g_isSliding) ? Clamp(speed / 150.0f, 0.0f, 1.0f) : 0.0f;
        crouchBack += Clamp(cw - crouchBack, -dt * 4.0f, dt * 4.0f);
        bodyT.X -= crouchBack * g_cfg.crouchMoveBack;
    }
    bool hideUpper = false;
    auto* pc = static_cast<APlayerController*>(pawn->Controller);
    if (pc && pc->PlayerCamera) {
        FVector cam = pc->PlayerCamera->CameraCache.POV.Location;
        cam.X += g_camCorrection.X; cam.Y += g_camCorrection.Y; cam.Z += g_camCorrection.Z;   // rigid camera
        float dx = cam.X - pawn->Location.X, dy = cam.Y - pawn->Location.Y;
        float dz = cam.Z - (pawn->Location.Z + pawn->EyeHeight);
        float yaw = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
        float fwd = dx * cosf(yaw) + dy * sinf(yaw);
        float right = -dx * sinf(yaw) + dy * cosf(yaw);
        float k = Clamp(g_cfg.cameraFollow, 0.0f, 1.0f);
        bodyT.X += Clamp(fwd * k, -g_cfg.cameraFollowMax, g_cfg.cameraFollowMax);
        bodyT.Y += Clamp(right * k, -g_cfg.cameraFollowMax, g_cfg.cameraFollowMax);
        bool standing = !g_isCrouched && g_crouchSink < 1.0f && fabsf(g_smoothedSink) < 0.5f;   // crouch lowers the camera on purpose
        hideUpper = standing && dz < -g_cfg.hideUpperDrop;
        // Stairs: the pawn steps up instantly but the camera eases up after it, so for a moment the camera sits low,
        // inside the chest. Lower the visible body with the camera (planted-feet IK keeps the feet on the step).
        // Dead zone: ordinary camera bob is a few uu; only a real dip (stairs) lowers the body.
        // Both ways: going down stairs the pawn drops instantly while the camera eases down after it (camera too high).
        // Measured against the pitch curve (g_stairDz), not the eyes: the camera always sits lower when looking
        // down, and that had lowered the body too. Sprinting up stairs the camera lags most (video 15-33-15).
        float sd = g_cfg.rigidCamera ? g_stairDz : dz;
        float dip = sd < -5.0f ? sd + 5.0f : (sd > 5.0f ? sd - 5.0f : 0.0f);
        float follow = g_cfg.rigidCamera ? Clamp(g_cfg.stairFollow, 0.0f, 1.0f) : Clamp(g_cfg.cameraFollowDown, 0.0f, 1.0f);
        float maxDip = g_cfg.rigidCamera ? g_cfg.stairFollowMax : g_cfg.cameraFollowMax;
        // Landing (build 188): the game dips its camera ~26 uu over a quarter second, and with the foot IK still
        // settling the pawn isn't "standing" yet, so the body stayed up and the camera sank into the chest/collar
        // (looking down). Follow the dip through the landing too, faded out with the landing hold (no pop).
        float landW = (!g_isCrouched && !g_isSliding) ? g_landHold * g_landHold * (3.0f - 2.0f * g_landHold) : 0.0f;
        float followW = standing ? 1.0f : landW;
        g_cameraDip = followW > 0.0f ? followW * Clamp(dip * follow, -maxDip, maxDip) : 0.0f;
        bodyT.Z += g_cameraDip;
        // Mantling/vaulting: the game pulls the camera back (and up) while the pawn stays; move the visible body back
        // with it so it stays under the view instead of being seen from behind or hidden (blinked - video 21-55-50).
        // Keep following for a moment after landing too: the camera is still offset when the pawn lands, and stopping
        // at once flashed the body into view at the end of quick mantles (video 22-04-16).
        static DWORD lastAir = 0;
        if (static_cast<int>(pawn->Physics) != 1) lastAir = GetTickCount();
        bool recentAir = lastAir && GetTickCount() - lastAir < 500;
        if (g_cfg.mantleFollow && recentAir && !g_isClimbing && !g_isSliding) {
            float back = Clamp(-g_cornerLeanFwd, 0.0f, 200.0f);
            static DWORD lastML = 0;
            if (back > 20.0f && GetTickCount() - lastML > 100) { lastML = GetTickCount(); Log("mantle follow: camera %.0f back, %.0f up", back, g_stairDz); }
            bodyT.X -= back;
            // The camera also drops far below its usual height while the pawn climbs the ledge (log: ~110 down);
            // follow it down too, or the body sat above the view (user: looking at the belt from behind).
            bodyT.Z += Clamp(g_stairDz, -200.0f, 0.0f);
        }
        // Clip guard push (UpdateVisibility): back and slightly down, away from the camera.
        // Eased (not applied in one frame): the instant push threw the legs out of view after a slide (video 21-03-45).
        static FVector applied = { 0, 0, 0 };
        {
            FVector target{ g_clipPush * g_clipDir.X, g_clipPush * g_clipDir.Y, g_clipPush * g_clipDir.Z };
            float step = dt * g_cfg.clipPushSpeed;
            applied.X += Clamp(target.X - applied.X, -step, step);
            applied.Y += Clamp(target.Y - applied.Y, -step, step);
            applied.Z += Clamp(target.Z - applied.Z, -step, step);
        }
        bodyT.X += applied.X;
        bodyT.Y += applied.Y;
        bodyT.Z += applied.Z;
        // Diagnostic: the first 0.6 s after a slide, every frame (what moves where).
        {
            static bool wasSliding = false;
            static DWORD endAt = 0;
            DWORD nowD = GetTickCount();
            if (wasSliding && !g_isSliding) endAt = nowD;
            wasSliding = g_isSliding;
            if (endAt && nowD - endAt < 600) {
                FName pel("Root_jnt");
                FVector p = g_body->GetBoneLocation(pel, 0);
                Log("slide end +%lums: view Z %.0f pelvis Z %.0f (dxy %.0f) pawn Z %.0f eye %.1f | sink %.1f/%.1f slide %.2f legW %.2f push %.0f applied (%.0f %.0f %.0f)",
                    nowD - endAt, g_viewLoc.Z, p.Z, sqrtf((p.X - g_viewLoc.X) * (p.X - g_viewLoc.X) + (p.Y - g_viewLoc.Y) * (p.Y - g_viewLoc.Y)),
                    pawn->Location.Z, pawn->EyeHeight, g_crouchSink, g_smoothedSink, g_slideAmount, g_slideLegW, g_clipPush,
                    applied.X, applied.Y, applied.Z);
            }
        }
    }
    if (static_cast<int>(pawn->Physics) != 1) hideUpper = true;   // not PHYS_Walking: jumping, falling, mantling
    if (!g_cfg.hideUpperWhenClipping) hideUpper = false;   // arms stay visible unless a hand holds something (user)
    if (g_isClimbing) hideUpper = true;   // except on ladders/chains: the coat and shoulders filled the view (1P hands grip)
    if (g_cfg.hideShouldersUp) hideUpper = true;   // shoulders up never visible: no clipping at all (the shadow keeps them)
    if (pc && g_cfg.lookUpHideDeg > 0.0f) {   // looking steeply up the camera sits at the back of the collar (video
        int p = pc->Rotation.Pitch & 0xFFFF;  // 14-47-15); the arms are out of view up there, so hide the upper body
        float deg = (p > 32768 ? p - 65536 : p) * (360.0f / 65536.0f);
        static bool lookingUp = false;   // small hysteresis so it doesn't flicker at the threshold
        lookingUp = deg > g_cfg.lookUpHideDeg - (lookingUp ? 5.0f : 0.0f);
        if (lookingUp) hideUpper = true;
        // Mid-jump, moving fast and looking down: the collar and the frozen stride arms filled the view.
        FVector v = pawn->Velocity;
        if (g_cfg.jumpHideUpper && !g_cfg.jumpArms && g_jumpAmount > 0.3f && deg < -20.0f && v.X * v.X + v.Y * v.Y > 250.0f * 250.0f)
            hideUpper = true;
        // ...and jumping backwards: the collar/neck came up under the camera (video 17-57-08).
        float yawR = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
        if (g_cfg.jumpHideUpper && !g_cfg.jumpArms && g_jumpAmount > 0.3f && v.X * cosf(yawR) + v.Y * sinf(yawR) < -50.0f) hideUpper = true;
    }
    if (g_smoothedSink > g_cfg.deepCrouchSink) hideUpper = true;   // very low crouch after a slide/under things (log: cylinder 33)
    if (g_swimAmount > 0.0f) hideUpper = true;   // swimming: the collar/hood ringed the underwater view (video 18-09-27)
    if (g_isSliding || g_slideAmount > 0.0f) hideUpper = true;   // sliding (incl. the blend in/out, which clipped): the reclined chest would fill the view; legs + 1P hands only
    if (g_isCrouched && g_viewPitchDeg > g_cfg.crouchHidePitch) hideUpper = true;   // crouched, looking ahead: chest top clipped the view
    g_hideUpperNow = hideUpper;
    if (g_cfg.hideUpperWhenClipping) {   // user: holstered arms stay visible while sprinting / looking up
        FVector v = pawn->Velocity;
        if (sqrtf(v.X * v.X + v.Y * v.Y) > g_cfg.hideUpperSpeed) hideUpper = true;   // sprinting
    }
    if (pc && g_cfg.hideUpperWhenClipping) {   // looking up: Rotation.Pitch is 0..65535, up = small positive values
        int pitch = pc->Rotation.Pitch & 0xFFFF;
        float deg = (pitch > 32768 ? pitch - 65536 : pitch) * (360.0f / 65536.0f);
        if (deg > g_cfg.hideUpperPitch) hideUpper = true;
    }
    static int upperHidden = -1;
    static USkeletalMeshComponent* upperComp = nullptr;
    if (g_body != upperComp) { upperComp = g_body; upperHidden = -1; }   // new body: apply the state again
    if ((hideUpper ? 1 : 0) != upperHidden) {
        FName spine("spine_3_jnt");
        if (spine.FNameEntryId >= 0) {
            if (false) {   // hiding is done by the eased BoneScale (UpdateCoat); HideBoneByName popped (user)
            } else {
                g_body->UnHideBoneByName(spine);
                FName neck("neck_jnt"), torso("Torso_jnt");   // unhiding the spine may unhide its children
                if (neck.FNameEntryId >= 0) g_body->HideBoneByName(neck, EPhysBodyOp::PBO_None);
                if (g_cfg.hideTorso && torso.FNameEntryId >= 0) g_body->HideBoneByName(torso, EPhysBodyOp::PBO_None);
            }
        }
        upperHidden = hideUpper ? 1 : 0;
    }
    g_body->SetTranslation(bodyT);
    g_body->SetRotation(arms->Rotation);
    {
        // Mantle: shrink the visible body away fast, grow it back smoothly (UpdateVisibility sets g_mantleWant).
        // Gone at once (shrinking over a few frames showed a tiny Corvo out in front - video 22-13-39), held while the
        // camera is pulled back, then grows back under the view once the camera has returned.
        if (g_mantleWant) g_mantleAmount = 1.0f;
        else g_mantleAmount = Clamp(g_mantleAmount - dt * g_cfg.reappearSpeed, 0.0f, 1.0f);
        g_body->SetScale(arms->Scale * (1.0f - 0.999f * g_mantleAmount));
    }
    if (g_shadowBody) {
        g_shadowBody->SetTranslation(t);
        g_shadowBodyT = t;
        g_shadowBody->SetRotation(arms->Rotation);
        g_shadowBody->SetScale(arms->Scale);
    }
    if (g_head) {
        // The head shadow is a separate rigid piece: with the spine lean (look up/down, corner lean) the shadow
        // body's head moved and the head shadow stayed behind (user). Each frame, nudge it by the gap between the
        // two head bones (both from the last pose), in actor space.
        static FVector headFix = { 0, 0, 0 };
        static USkeletalMeshComponent* fixFor = nullptr;
        if (fixFor != g_head) { fixFor = g_head; headFix = { 0, 0, 0 }; }
        FName headBone("head_jnt");
        if (g_shadowBody && headBone.FNameEntryId >= 0) {
            FVector want = g_shadowBody->GetBoneLocation(headBone, 0);
            FVector have = g_head->GetBoneLocation(headBone, 0);
            float ex = want.X - have.X, ey = want.Y - have.Y, ez = want.Z - have.Z;
            if (ex * ex + ey * ey + ez * ez < 150.0f * 150.0f) {   // ignore a garbage frame (spawn, teleport)
                float yaw = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
                float cy = cosf(yaw), sy = sinf(yaw);
                headFix.X = Clamp(headFix.X + 0.6f * (ex * cy + ey * sy), -80.0f, 80.0f);   // damped: no overshoot
                headFix.Y = Clamp(headFix.Y + 0.6f * (-ex * sy + ey * cy), -80.0f, 80.0f);
                headFix.Z = Clamp(headFix.Z + 0.6f * ez, -80.0f, 80.0f);
            }
        }
        t.X += headFix.X; t.Y += headFix.Y; t.Z += headFix.Z;
        g_head->SetTranslation(t);
        g_head->SetRotation(arms->Rotation);
        g_head->SetScale(arms->Scale);
    }
}

static void LogDebug(ADishonoredPlayerPawn* pawn) {
    Log("---- debug ----");
    Log("pawn=%p %s", pawn, pawn ? pawn->GetFullName().c_str() : "");
    if (!pawn) return;
    USkeletalMeshComponent* arms = pawn->Mesh;
    Log("pawn loc (%.1f %.1f %.1f) rot (%d %d %d)", pawn->Location.X, pawn->Location.Y, pawn->Location.Z,
        pawn->Rotation.Pitch, pawn->Rotation.Yaw, pawn->Rotation.Roll);
    if (arms) {
        Log("1P mesh %s skel=%s", arms->GetFullName().c_str(), arms->SkeletalMesh ? arms->SkeletalMesh->GetFullName().c_str() : "null");
        Log("1P mesh translation (%.1f %.1f %.1f) rot (%d %d %d) scale %.2f depth %d lod %d hidden %d",
            arms->Translation.X, arms->Translation.Y, arms->Translation.Z, arms->Rotation.Pitch, arms->Rotation.Yaw,
            arms->Rotation.Roll, arms->Scale, static_cast<int>(arms->DepthPriorityGroup), arms->ForcedLodModel, arms->HiddenGame);
        for (int i = 0; i < arms->AnimSets.size(); ++i)
            if (arms->AnimSets.at(i)) Log("  animset[%d] %s", i, arms->AnimSets.at(i)->GetFullName().c_str());
        if (arms->AnimTreeTemplate) Log("  animtree %s", arms->AnimTreeTemplate->GetFullName().c_str());
    }
    Log("body=%p state=%d source=%d", g_body, static_cast<int>(g_loadState), g_sourceIndex);
    if (g_body) {
        Log("body translation (%.1f %.1f %.1f) hidden %d depth %d parent %p", g_body->Translation.X,
            g_body->Translation.Y, g_body->Translation.Z, g_body->HiddenGame,
            static_cast<int>(g_body->DepthPriorityGroup), g_body->ParentAnimComponent);
    }
}

// Bone trace: compare the 1P mesh's pose with the body's, to see whether the 1P animations move the legs
// and whether SetParentAnimComponent copies them.
static DWORD g_traceUntil = 0;

static void TraceBones(ADishonoredPlayerPawn* pawn) {
    static DWORD last = 0;
    static int autoSamples = 0;
    static ADishonoredPlayerPawn* autoPawn = nullptr;
    DWORD now = GetTickCount();
    if (!pawn || !pawn->Mesh || now - last < 250) return;
    FVector v = pawn->Velocity;
    float speed = sqrtf(v.X * v.X + v.Y * v.Y);
    // Automatic: up to 40 samples per level while the player is moving. Manual: F11 for 8 s.
    if (pawn != autoPawn) { autoPawn = pawn; autoSamples = 0; }
    bool manual = now <= g_traceUntil;
    bool automatic = speed > 50.0f && autoSamples < 40;
    if (!manual && !automatic) return;
    if (automatic && !manual) ++autoSamples;
    last = now;
    static const char* bones[] = { "spine_2_jnt", "upper_leg_L_jnt", "lower_leg_L_jnt", "foot_L_jnt", "foot_R_jnt",
                                   "camera_jnt", "hand_R_jnt" };
    Log("trace: speed %.0f  crouched %d  phys %d", sqrtf(v.X * v.X + v.Y * v.Y), pawn->bIsCrouched,
        static_cast<int>(pawn->Physics));
    for (const char* b : bones) {
        FName n(b);
        if (n.FNameEntryId < 0) continue;
        FVector a = pawn->Mesh->GetBoneLocation(n, 1);
        if (g_body) {
            FVector c = g_body->GetBoneLocation(n, 1);
            Log("  %-16s 1P (%7.1f %7.1f %7.1f)   body (%7.1f %7.1f %7.1f)", b, a.X, a.Y, a.Z, c.X, c.Y, c.Z);
        } else {
            Log("  %-16s 1P (%7.1f %7.1f %7.1f)", b, a.X, a.Y, a.Z);
        }
    }
}

static void OnPlayerTick() {
    // (Build 201: the F6-F11 development keys - body on/off, arm hiding, attach mode, ini reload, leg axis, debug
    // dump - are gone: they clashed with players' bindings and nothing is meant to be adjusted in game.)
    ADishonoredPlayerPawn* pawn = GetPlayerPawn();

    // The pawn changed (level load, death, reload): the old component went with it, and the level
    // change frees the DLC mesh's render data even though the object pointer looks alive. Reload it
    // for every pawn (the async load takes ~0.5 s).
    static ADishonoredPlayerPawn* lastPawn = nullptr;
    if (pawn != lastPawn) {
        if (g_body || g_bodyMesh) Log("player pawn changed (%p -> %p): dropping body and mesh", lastPawn, pawn);
        // The old pawn (and our body on it) is purged by the next ordinary GC in the same world, e.g. a cutscene
        // that replaces the pawn (build 57 crash: IncrementalPurgeGarbage -> ExitProperties freeing our arrays).
        if (g_ourArrayCount) {
            if (g_shadowBody && g_shadowBody->Animations) DetachNodeArrays(g_shadowBody->Animations, 0);
            DetachOurArrays(g_body);
        }
        lastPawn = pawn;
        g_body = nullptr;
        g_head = nullptr;
        g_shadowBody = nullptr;
        g_itemShadow[0] = g_itemShadow[1] = nullptr;   // owned by the pawn; dropped with it
        g_armsShadow = nullptr;
        g_bodyPawn = nullptr;
        ReleaseLoadedAssets();
        g_bodyBuiltTick = 0;
        g_bodyMesh = nullptr;
        if (g_loadState != LoadState::Loading) g_loadState = LoadState::Idle;
        g_sourceIndex = 0;
        g_animSet = nullptr;
        g_crouchSet = nullptr;
        g_headMesh = nullptr; g_daudBodyMesh = nullptr; g_daudHeadMesh = nullptr; ForgetArmsSwap();
        if (g_animLoadState != LoadState::Loading) g_animLoadState = LoadState::Idle;
    }
    if (!g_cfg.enabled || !pawn) return;

    switch (g_loadState) {
    case LoadState::Idle:
        StartLoad();
        return;
    case LoadState::Loading:
        if (GetTickCount() - g_loadStartTick > 30000) {
            Log("package load timed out");
            g_loadState = LoadState::Failed;
        }
        return;
    case LoadState::Failed:
        // Try the next DLC package once; give up after the list.
        if (!g_bodyMesh && g_sourceIndex + 1 < static_cast<int>(sizeof(kSources) / sizeof(kSources[0]))) {
            ++g_sourceIndex;
            StartLoad();
        }
        return;
    case LoadState::Loaded:
        break;
    }

    if (!g_bodyMesh) {
        g_bodyMesh = FindLoadedMesh(kSources[g_sourceIndex].mesh);
        if (!g_bodyMesh) {
            Log("package loaded but mesh %s not found", kSources[g_sourceIndex].mesh);
            g_loadState = LoadState::Failed;
            return;
        }
        KeepAlive(g_bodyMesh);
        Log("found body mesh %s", g_bodyMesh->GetFullName().c_str());
        // The locomotion set ships in the same package: grab it now so the body is created with its own tree.
        // Loading a second map package tears down the first one's mesh data (tests 12-15).
        g_animSet = static_cast<UAnimSet*>(FindObjectByName("AnimSet", kAnimSetName));
        KeepAlive(g_animSet);
        Log("anim set: %s", g_animSet ? g_animSet->GetFullName().c_str() : "NOT FOUND (pose copy only)");
        if (!g_animSet) g_animLoadState = LoadState::Failed;
    }

    // Create the body as soon as the mesh is found: loading another package before the mesh is in use
    // tears down its lazily loaded data (GetStreamingTextureFactor assert, tests 12-13). It starts out
    // copying the 1P pose and switches to its own locomotion tree once the NPC anim set is available.
    ProtectAssets();
    RebuildBodyIfCharacterChanged(pawn);
    if (!g_body) CreateBody(pawn);
    ApplyArmsCharacter(pawn);
    ApplyArmsShift(pawn);
    UpdateHitState(pawn);
    // Posture first: on the frame a slide ends the game re-centres the taller cylinder (pawn up ~32) and
    // SyncTransform used last frame's sink, so the pelvis jumped into the camera for one frame (log 21:06).
    UpdatePosture(pawn);
    SyncTransform(pawn);
    TraceBones(pawn);
    UpdateBodyAnimation();
    UpdateLocomotionBlend(pawn);
    UpdateTurnInPlace(pawn);
    UpdateFootIK(pawn);
    UpdateSlide();
    UpdateClimb(pawn);
    UpdateJump(pawn);
    UpdateSwim(pawn);
    UpdateHands(GetPlayerPawn());
    UpdateCoat();
    UpdateLean(pawn);
    UpdateArmBlend();
    UpdateArmIK(pawn);
    UpdateArmsShadow(pawn);
    UpdateItemShadows(pawn);
    UpdateVisibility(pawn);
    DiagHeadShadow();
    // EnsureLocalAtoms(g_body);  // no longer needed since assets are rooted in the load callback (needs 16-byte alignment if revived)
}

// ---------------------------------------------------------------------------------------------
// Frame hook: UGameEngine::Tick (retail 0x632860). Dishonored's PlayerController has no script
// PlayerTick event, so we run once per engine frame, after the original tick, on the game thread.
// ---------------------------------------------------------------------------------------------
using GameEngineTick_t = void(__thiscall*)(void*, float);
static GameEngineTick_t g_origGameEngineTick = nullptr;
static DWORD g_pawnSeenTick = 0;
static ADishonoredPlayerPawn* g_pawnSeen = nullptr;

static void __fastcall HookedGameEngineTick(void* self, void* /*edx*/, float delta) {
    g_origGameEngineTick(self, delta);
    if (g_inHook) return;

    DWORD now = GetTickCount();

    // A new world means every component we made is gone (or going). Forget them without touching them.
    static void* lastWorld = nullptr;
    void* world = GetWorld();
    if (world != lastWorld) {
        Log("world changed %p -> %p: dropping body", lastWorld, world);
        lastWorld = world;
        g_body = nullptr;
        g_head = nullptr;
        g_shadowBody = nullptr;
        g_itemShadow[0] = g_itemShadow[1] = nullptr;   // owned by the pawn; dropped with it
        g_armsShadow = nullptr;
        g_bodyPawn = nullptr;
        ReleaseLoadedAssets();
        g_bodyMesh = nullptr;
        g_pawnSeen = nullptr;
        g_pawnSeenTick = now;
        g_bodyBuiltTick = 0;
        if (g_loadState != LoadState::Loading) g_loadState = LoadState::Idle;
        g_sourceIndex = 0;
        g_animSet = nullptr;
        g_crouchSet = nullptr;
        g_headMesh = nullptr; g_daudBodyMesh = nullptr; g_daudHeadMesh = nullptr; ForgetArmsSwap();
        if (g_animLoadState != LoadState::Loading) g_animLoadState = LoadState::Idle;
    }

    // Wait until a player pawn has existed for 3 s (level fully loaded, past the first frames).
    ADishonoredPlayerPawn* pawn = GetPlayerPawn();
    if (pawn != g_pawnSeen) {
        g_pawnSeen = pawn;
        g_pawnSeenTick = now;
        Log("player pawn is now %p", pawn);
    }
    if (pawn && now - g_pawnSeenTick < 3000) return;

    ++g_inHook;
    OnPlayerTick();
    --g_inHook;
}

// ---------------------------------------------------------------------------------------------
// GC hook: UObject::CollectGarbage(uint64 KeepFlags, UBOOL bPerformFullPurge) @0x47D750 (cdecl).
// Test 17 crashed in the full purge of a save load while our mesh and anim sequences were still rooted.
// When a full purge runs while the world is going away, let go of everything first; we reload per level.
// ---------------------------------------------------------------------------------------------
using CollectGarbage_t = void(__cdecl*)(uint64_t, uint32_t);
static CollectGarbage_t g_origCollectGarbage = nullptr;
void* g_assetsWorld = nullptr;   // the world our rooted assets were grabbed in

static void __cdecl HookedCollectGarbage(uint64_t keepFlags, uint32_t fullPurge) {
    if (fullPurge) {
        void* world = GetWorld();
        Log("full GC: world %p (assets from %p), mesh %p, anim set %p", world, g_assetsWorld, g_bodyMesh, g_animSet);
        if ((g_bodyMesh || g_animSet || g_ourArrayCount) && (!world || world != g_assetsWorld)) {
            Log("level is changing: releasing body assets before the purge");
            if (g_shadowBody && g_shadowBody->Animations) DetachNodeArrays(g_shadowBody->Animations, 0);
            DetachOurArrays(g_body);
            ReleaseLoadedAssets();
            g_bodyMesh = nullptr;
            g_animSet = nullptr;
            g_crouchSet = nullptr;
            g_headMesh = nullptr; g_daudBodyMesh = nullptr; g_daudHeadMesh = nullptr; ForgetArmsSwap();
        }
    }
    g_origCollectGarbage(keepFlags, fullPurge);
    // Ordinary GC: marking is done and the purge runs over the next frames. If our body was marked unreachable
    // (its pawn went away without us noticing), detach our arrays before the purge reaches it and forget it.
    if (!fullPurge && g_ourArrayCount) {
        auto unreachable = [](void* o) { return o && (*reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(o) + 0x8) & 0x0000000200000000ULL); };
        // Only the components themselves: the instanced AnimTree is replaced now and then (old one unreachable,
        // harmless) and reacting to that rebuilt the body while the old one stayed attached (build 58 log).
        if (unreachable(g_body) || unreachable(g_shadowBody)) {
            Log("GC marked our body unreachable (body %d, shadow %d): detaching arrays before the purge",
                unreachable(g_body) ? 1 : 0, unreachable(g_shadowBody) ? 1 : 0);
            if (g_shadowBody && g_shadowBody->Animations) DetachNodeArrays(g_shadowBody->Animations, 0);
            DetachOurArrays(g_body);
            g_body = nullptr;
            g_head = nullptr;
            g_shadowBody = nullptr;
            g_itemShadow[0] = g_itemShadow[1] = nullptr;   // owned by the pawn; dropped with it
        g_armsShadow = nullptr;
            g_bodyPawn = nullptr;
        }
    }
}

// UObject::StaticExit @0x4645D0 (cdecl, no args): the final purge on quit. It crashed freeing our arrays exactly
// like the level-change purge did, so detach them first here too.
using StaticExit_t = void(__cdecl*)();
static StaticExit_t g_origStaticExit = nullptr;

static void __cdecl HookedStaticExit() {
    Log("game is quitting: detaching body arrays before the final purge");
    if (g_shadowBody && g_shadowBody->Animations) DetachNodeArrays(g_shadowBody->Animations, 0);
            DetachOurArrays(g_body);
    ReleaseLoadedAssets();
    g_bodyMesh = nullptr;
    g_animSet = nullptr;
    g_crouchSet = nullptr;
    g_headMesh = nullptr; g_daudBodyMesh = nullptr; g_daudHeadMesh = nullptr; ForgetArmsSwap();
    g_body = nullptr;
    g_head = nullptr;
    g_shadowBody = nullptr;
    g_itemShadow[0] = g_itemShadow[1] = nullptr;   // owned by the pawn; dropped with it
        g_armsShadow = nullptr;
    g_origStaticExit();
}

// ---------------------------------------------------------------------------------------------
// Rigid camera
// ---------------------------------------------------------------------------------------------
// The first-person camera sits on the 1P mesh's camera_jnt, which the aim-offset animations swing in an arc with
// pitch (forward/down when looking down, back when looking up). The user wants the view to pivot in place.
// The arc is a fixed function of pitch, so it is learned while the player stands still (offset of the camera from
// the eyes, in view-yaw space, per 5-degree pitch bucket, standing and crouched separately) and the pitch-dependent
// part (offset at this pitch minus offset at level pitch) is removed from the view location. Bob, leans, stairs,
// crouch and mantles differ from the learned curve and pass through untouched.
// APlayerController::GetPlayerViewPoint @0x5E17A0 (thiscall, FVector& loc, FRotator& rot) feeds the renderer,
// audio and aiming, so everything stays consistent.
struct PitchCurve { float fwd[37], right[37], up[37], weight[37]; };
static PitchCurve g_pitchCurve[2];

using GetPlayerViewPoint_t = void(__fastcall*)(APlayerController*, void*, FVector*, FRotator*);
static GetPlayerViewPoint_t g_origGetPlayerViewPoint = nullptr;

static bool SamplePitchCurve(const PitchCurve& c, float deg, float& f, float& r, float& u) {
    float x = Clamp((deg + 90.0f) / 5.0f, 0.0f, 36.0f);
    int i = static_cast<int>(x);
    if (i >= 36) i = 35;
    float t = x - i;
    if (c.weight[i] > 0 && c.weight[i + 1] > 0) {
        f = c.fwd[i] + (c.fwd[i + 1] - c.fwd[i]) * t;
        r = c.right[i] + (c.right[i + 1] - c.right[i]) * t;
        u = c.up[i] + (c.up[i + 1] - c.up[i]) * t;
        return true;
    }
    int n = (t < 0.5f) ? i : i + 1;   // only one neighbour learned yet: use the nearer learned one
    if (c.weight[n] <= 0) n = (n == i) ? i + 1 : i;
    if (c.weight[n] <= 0) return false;
    f = c.fwd[n]; r = c.right[n]; u = c.up[n];
    return true;
}

static void LearnPitchCurve(PitchCurve& c, float deg, float f, float r, float u, float dt) {
    float x = Clamp((deg + 90.0f) / 5.0f, 0.0f, 36.0f);
    int i = static_cast<int>(x);
    if (i >= 36) i = 35;
    float t = x - i;
    for (int k = 0; k < 2; ++k) {
        int b = i + k;
        float w = k ? t : 1.0f - t;
        if (w < 0.25f) continue;
        float a = c.weight[b] <= 0 ? 1.0f : Clamp(dt * 4.0f * w, 0.0f, 1.0f);
        c.fwd[b] += (f - c.fwd[b]) * a;
        c.right[b] += (r - c.right[b]) * a;
        c.up[b] += (u - c.up[b]) * a;
        c.weight[b] = 1;
    }
}

static void __fastcall HookedGetPlayerViewPoint(APlayerController* pc, void* edx, FVector* loc, FRotator* rot) {
    g_origGetPlayerViewPoint(pc, edx, loc, rot);
    auto off = []() { g_camCorrection = { 0, 0, 0 }; g_camTarget = { 0, 0, 0 }; g_bodyFollowTarget = { 0, 0, 0 }; g_cornerLeanRight = g_cornerLeanFwd = g_stairDz = 0.0f; };
    if (!g_cfg.rigidCamera || !pc || !loc || !rot) { off(); return; }
    auto* pawn = static_cast<APawn*>(pc->Pawn);
    if (!pawn || pc->bCinematicMode || static_cast<APawn*>(g_bodyPawn) != pawn) { off(); return; }
    if (pc->PlayerCamera && pc->PlayerCamera->ViewTarget.Target &&
        pc->PlayerCamera->ViewTarget.Target != pawn && pc->PlayerCamera->ViewTarget.Target != pc) {
        off();
        return;
    }
    // The 1P arms are moved by the same correction (so weapons stay in front of the fixed camera, build 58 video).
    // If the game's camera rides the arms (camera_jnt), that shift is already in the view: take it out first.
    FVector raw = *loc;
    float rawDx = raw.X - pawn->Location.X, rawDy = raw.Y - pawn->Location.Y;
    float dz = raw.Z - (pawn->Location.Z + pawn->EyeHeight);
    {   // not the 1P view: measured without our arms shift, else switching off/on fed back every frame
        float k0 = g_camFollowsArms == 1 ? 1.0f : 0.0f;
        float vx = rawDx - k0 * g_armsShiftWorld.X, vy = rawDy - k0 * g_armsShiftWorld.Y, vz = dz - k0 * g_armsShiftWorld.Z;
        if (vx * vx + vy * vy + vz * vz > g_cfg.thirdPersonDist * g_cfg.thirdPersonDist) { off(); return; }
    }

    // Build 199: pitch and heading from the controller (the aim), not the camera's final rotation. Looking straight up
    // the camera's rotation (aim + walking bob) can't be split into a heading: its yaw/roll jumped every frame (log
    // 21:10: yaw 53920 -> 12484 -> 8212, roll +-30000), and every offset here - the pin, the look-up lock, the 1P arms
    // shift - was turned by that garbage heading: the view swung around and the arms floated above the head.
    int p = pc->Rotation.Pitch & 0xFFFF;
    float deg = (p > 32768 ? p - 65536 : p) * (360.0f / 65536.0f);
    float sdeg = Clamp(deg, -85.0f, 85.0f);   // the curve is noisy at the very top/bottom (build 58: sways looking up)
    float yaw = pc->Rotation.Yaw * (6.2831853f / 65536.0f);
    float cy = cosf(yaw), sy = sinf(yaw);
    FVector S = g_armsShiftWorld;
    float sF = S.X * cy + S.Y * sy, sR = -S.X * sy + S.Y * cy;
    float rawF = rawDx * cy + rawDy * sy, rawR = -rawDx * sy + rawDy * cy;
    PitchCurve& curve = g_pitchCurve[g_isCrouched ? 1 : 0];

    static DWORD lastTick = 0, stillSince = 0;
    static FVector lastPos = { 0, 0, 0 };
    DWORD now = GetTickCount();
    float dt = lastTick ? Clamp((now - lastTick) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    lastTick = now;
    FVector v = pawn->Velocity;
    float mv = fabsf(pawn->Location.X - lastPos.X) + fabsf(pawn->Location.Y - lastPos.Y) + fabsf(pawn->Location.Z - lastPos.Z);
    lastPos = pawn->Location;
    float speed2 = v.X * v.X + v.Y * v.Y;
    bool settled = static_cast<int>(pawn->Physics) == 1 && fabsf(v.Z) < 5.0f && !g_isSliding &&
                   fabsf(g_crouchSink - g_smoothedSink) < 0.5f;
    bool still = settled && speed2 < 25.0f && mv < 0.2f;
    if (!still) stillSince = now;
    float cf, cr, cu;
    bool known = SamplePitchCurve(curve, sdeg, cf, cr, cu);

    // Does the view follow the arms? Standing still with a known curve and a real shift, compare which
    // explanation fits: view = vanilla, or view = vanilla + arms shift. Decide after a few clear frames.
    static int votes0 = 0, votes1 = 0;
    float k = g_camFollowsArms == 1 ? 1.0f : 0.0f;
    bool shifted = sF * sF + sR * sR > 64.0f;
    if (known && shifted) {
        float e0 = fabsf(rawF - cf) + fabsf(rawR - cr);
        float e1 = fabsf(rawF - sF - cf) + fabsf(rawR - sR - cr);
        if (g_camFollowsArms < 0) {
            k = e1 < e0 ? 1.0f : 0.0f;   // best guess per frame until decided
            if (still && now - stillSince > 150) {
                if (e1 < e0 - 4.0f) ++votes1; else if (e0 < e1 - 4.0f) ++votes0;
                if (votes1 >= 15 || votes0 >= 15) {
                    g_camFollowsArms = votes1 > votes0 ? 1 : 0;
                    Log("rigid camera: the game's view %s the 1P arms (votes %d/%d)",
                        g_camFollowsArms ? "follows" : "does not follow", votes1, votes0);
                }
            }
        }
    }
    float fwd = rawF - k * sF, right = rawR - k * sR;   // the game's own (vanilla) camera offset
    float vanZ = raw.Z - k * S.Z;
    dz = vanZ - (pawn->Location.Z + pawn->EyeHeight);
    bool leaning = known ? fabsf(right - cr) > 4.0f : fabsf(right) > 15.0f;
    // Corner lean: how far the game's camera sits to the side (and forward) of where looking alone puts it.
    // UpdateLean tilts the spine so the upper body follows the camera round the corner.
    g_cornerLeanRight = known ? right - cr : 0.0f;
    g_cornerLeanFwd = known ? fwd - cf : 0.0f;
    // Height of the game's camera against where looking alone puts it: stairs (the pawn steps up at once, the
    // camera eases after it), landings, crouch easing. SyncTransform lowers/raises the visible body by it.
    g_stairDz = known ? dz - cu : 0.0f;
    bool trustworthy = g_camFollowsArms >= 0 || !shifted;
    // Learn fast while standing still; while walking only fill buckets not learned yet (walking bob made the
    // top of the curve noisy and the camera swayed looking up, build 58).
    if (g_cfg.rigidCameraLearn && !leaning && trustworthy && !shifted) {
        if (still && now - stillSince > 150) LearnPitchCurve(curve, deg, fwd, right, dz, dt);
        else if (!known && settled && speed2 < 300.0f * 300.0f) LearnPitchCurve(curve, deg, fwd, right, dz, dt * 0.25f);
    }

    // Horizontally the camera is pinned where the game puts it when looking fully down (in front of the chest):
    // pinned at the level-look spot, looking down showed the inside of the collar and shoulders (build 53 video).
    // Crouched uses its own curve once learned, the standing one until then.
    float lf = 0, lr = 0, lu = 0;
    auto pick = [&](const PitchCurve& c) {
        if (!SamplePitchCurve(c, sdeg, cf, cr, cu)) return false;
        int pivot = static_cast<int>(Clamp((g_cfg.rigidCameraPivot + 90.0f) / 5.0f, 0.0f, 36.0f) + 0.5f);
        int found = -1;
        for (int b = pivot; ; b += pivot < 18 ? 1 : -1) {
            if (c.weight[b] > 0) { found = b; break; }
            if (b == 18) break;
        }
        if (found < 0) return false;
        float tf, tr;
        if (!SamplePitchCurve(c, 0.0f, tf, tr, lu)) lu = cu;   // vertical reference: level pitch (if learned)
        lf = c.fwd[found]; lr = c.right[found];
        return true;
    };
    if (!pick(curve) && !pick(g_pitchCurve[0])) { off(); return; }
    // Build 196 (videos 20-52-13, 20-53-51): looking straight up, the pin (the looking-down spot, ~32 uu ahead of the
    // eyes - 63 uu ahead of where the game puts the camera at +85) kept the view on a point well in front of the head:
    // it swung around with every turn and step while running, and on ladders/chains it pushed the view through the
    // rungs. Looking up past 20 degrees, slide the pin back to the level-look spot (all the way by 60; nothing of the
    // body is in view up there); on ladders/chains (their own camera) let the game's camera be, eased in and out.
    {
        float lvF, lvR, lvU;
        if (SamplePitchCurve(g_pitchCurve[0], 0.0f, lvF, lvR, lvU) && sdeg > 20.0f) {
            float t = Clamp((sdeg - 20.0f) / 40.0f, 0.0f, 1.0f);
            t = t * t * (3.0f - 2.0f * t);
            lf += (lvF - lf) * t;
            lr += (lvR - lr) * t;
        }
    }
    lf += g_cfg.rigidCameraForward;
    float df = -(cf - lf), dr = -(cr - lr), du = g_cfg.rigidCameraVertical ? -(cu - lu) : 0.0f;
    static float pinW = 1.0f;
    {
        bool climbing = static_cast<int>(pawn->Physics) == 9 || g_isClimbing;   // PHYS_Ladder (log 20:53: 2 -> 9, Climb state)
        float before = pinW;
        pinW += Clamp((climbing ? 0.0f : 1.0f) - pinW, -dt * 4.0f, dt * 4.0f);
        if ((before >= 1.0f) != (pinW >= 1.0f)) Log("rigid camera: %s", pinW < 1.0f ? "easing out (ladder/chain: the game's climbing camera)" : "back on");
    }
    df *= pinW;
    dr *= pinW;
    du *= pinW;
    g_camTarget.X = df * cy - dr * sy;
    g_camTarget.Y = df * sy + dr * cy;
    g_camTarget.Z = du;
    // The visible body follows only up to the level-look amount: following the full look-up shift (~60 uu) put
    // the collar right back at the camera (video 14-47-15). Above level the arms are out of view anyway.
    {
        float bf = df, br = dr, lcf, lcr, lcu;
        if (sdeg > 0.0f && SamplePitchCurve(g_pitchCurve[0], 0.0f, lcf, lcr, lcu)) { bf = -(lcf - lf) * pinW; br = -(lcr - lr) * pinW; }
        g_bodyFollowTarget.X = bf * cy - br * sy;
        g_bodyFollowTarget.Y = bf * sy + br * cy;
    }
    // Final view = vanilla view + target; vanilla = raw minus the arms shift if the view rides the arms.
    loc->X = raw.X - k * S.X + g_camTarget.X;
    loc->Y = raw.Y - k * S.Y + g_camTarget.Y;
    loc->Z = vanZ + g_camTarget.Z;
    // Sliding: the game drops the view to hip height (it looked out of the crotch - user); lift it up the body.
    loc->Z += g_slideAmount * g_cfg.slideCameraUp;
    g_camCorrection.X = loc->X - raw.X;
    g_camCorrection.Y = loc->Y - raw.Y;
    g_camCorrection.Z = loc->Z - raw.Z;
    // The mod hides the body after the engine has drawn the frame, so on the first frame of a quick mantle the camera
    // was already pulled back while the body was still there (log 22:30: the step-up influence turns on the same
    // frame). Until the body is actually hidden, cancel the pull-back here (this runs while the frame is drawn):
    // the camera starts its mantle move one frame later, by which time the body is gone.
    if (g_cfg.mantleHide && pc->PlayerCamera && !g_isSliding &&
        pc->PlayerCamera->IsA(ADishonoredPlayerCamera::StaticClass())) {
        auto* cam = reinterpret_cast<ADishonoredPlayerCamera*>(pc->PlayerCamera);
        UDishonoredCameraInfluence* infl[2] = { cam->m_pStepUpMantleOffset_Influence, cam->m_pCrouchMantleOffset_Influence };
        bool on = false;
        for (auto* i : infl) if (i && (i->m_bActive || i->m_Weight > 0.01f)) on = true;
        // Held fully until the body is hidden, then eased out over a few frames (releasing it in one frame read as a
        // stutter - video 22-33-19).
        static float hold = 0.0f;
        static DWORD holdT = 0;
        DWORD nowH = GetTickCount();
        float dtH = holdT ? Clamp((nowH - holdT) / 1000.0f, 0.0f, 0.1f) : 0.0f;
        holdT = nowH;
        if (on && g_mantleAmount < 0.99f) hold = 1.0f;
        else hold = Clamp(hold - dtH * g_cfg.mantleCamRelease, 0.0f, 1.0f);
        if (hold > 0.0f && on) {
            float backBy = cf - fwd;           // + = vanilla camera behind its usual spot
            float downBy = cu - dz;            // + = vanilla camera below its usual height
            float e = hold * hold * (3.0f - 2.0f * hold);
            if (backBy > 0.0f) { loc->X += e * backBy * cy; loc->Y += e * backBy * sy; }
            if (downBy > 0.0f) loc->Z += e * downBy;
            static DWORD lastL = 0;
            if (GetTickCount() - lastL > 200) { lastL = GetTickCount(); Log("mantle: holding the camera (%.0f back, %.0f down), easing out", backBy, downBy); }
        }
    }
    // (Builds 184-187 held the camera up against the game's landing dip here. Holding it left the 1P arms sinking
    // below the view; lifting the arms with it fed back - the camera reads lower by the arms shift - and the hold ran
    // to ~138 uu then snapped back: a jolt on every landing (log 17:38, user). The game's landing is left alone now;
    // the visible body dips with it instead (SyncTransform, g_landHold).)

    // Looking steeply up (build 198, videos 20-52-13 / 21-02-00 / 21-06-35): walking or sprinting, the view jumped
    // sideways about once a step. The bob rides the 1P arms' camera_jnt, and the arms tilt with the view, so looking
    // up their up/down bob becomes a back/forth slide across the sky (the rigid pin passes the game's bob through).
    // Nothing of the body or arms is in view up there: from 30 to 60 degrees, blend the camera's horizontal position
    // to a fixed spot at the eyes (the level-look spot). The vertical bob stays - looking up it only moves the view
    // along its own axis.
    {
        float lvF, lvR, lvU;
        float t = Clamp((sdeg - 30.0f) / 30.0f, 0.0f, 1.0f) * pinW;
        if (g_cfg.lookUpSteady && t > 0.0f && SamplePitchCurve(g_pitchCurve[0], 0.0f, lvF, lvR, lvU)) {
            t = t * t * (3.0f - 2.0f * t);
            float rx = pawn->Location.X + lvF * cy - lvR * sy, ry = pawn->Location.Y + lvF * sy + lvR * cy;
            float beforeF = (loc->X - pawn->Location.X) * cy + (loc->Y - pawn->Location.Y) * sy;
            float beforeR = -(loc->X - pawn->Location.X) * sy + (loc->Y - pawn->Location.Y) * cy;
            loc->X += (rx - loc->X) * t;
            loc->Y += (ry - loc->Y) * t;
            static DWORD lastUpLog = 0;
            if (pawn->Velocity.X * pawn->Velocity.X + pawn->Velocity.Y * pawn->Velocity.Y > 50.0f * 50.0f && now - lastUpLog >= 100) {
                lastUpLog = now;
                Log("look-up steady: pitch %.0f aim yaw %d (camera yaw %d roll %d) | game cam fwd %.0f right %.0f up %.0f, arms shift fwd %.0f right %.0f | pinned was fwd %.0f right %.0f -> %.0f %.0f (x%.2f)",
                    deg, pc->Rotation.Yaw & 0xFFFF, rot->Yaw & 0xFFFF, rot->Roll, fwd, right, dz, sF, sR, beforeF, beforeR, lvF, lvR, t);
            }
        }
    }
    g_viewLoc = *loc;
    g_viewLocTick = GetTickCount();
    g_viewRot = *rot;
    g_viewFov = pc->PlayerCamera && pc->PlayerCamera->CameraCache.POV.FOV > 1.0f ? pc->PlayerCamera->CameraCache.POV.FOV : 90.0f;
}

// Per tick: move the 1P arms (pawn->Mesh) by the camera correction so the hands and weapons stay where the
// game draws them relative to the view. The game may or may not rewrite Translation itself; handle both.
static void ApplyArmsShift(ADishonoredPlayerPawn* pawn) {
    USkeletalMeshComponent* arms = pawn ? pawn->Mesh : nullptr;
    static USkeletalMeshComponent* lastArms = nullptr;
    static FVector setTo = { 0, 0, 0 };
    if (!arms) return;
    if (arms != lastArms) { lastArms = arms; g_armsShiftLocal = { 0, 0, 0 }; g_armsShiftWorld = { 0, 0, 0 }; setTo = arms->Translation; }
    // Per axis: the game eases the arms' Z after crouch/slide changes. Comparing all three axes together treated
    // that as "the game rewrote it" and baked our X/Y shift into the base, so the shift was added again after
    // every slide and the arms (and the camera riding them) crept ~140 uu away (video 14-42-27).
    FVector base = arms->Translation;
    if (fabsf(base.X - setTo.X) < 0.01f) base.X -= g_armsShiftLocal.X;
    if (fabsf(base.Y - setTo.Y) < 0.01f) base.Y -= g_armsShiftLocal.Y;
    if (fabsf(base.Z - setTo.Z) < 0.01f) base.Z -= g_armsShiftLocal.Z;
    bool ours = true;
    FVector c = (g_cfg.rigidCamera && g_cfg.rigidCameraArms) ? g_camTarget : FVector{ 0, 0, 0 };
    float yaw = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
    float cy = cosf(yaw), sy = sinf(yaw);
    g_armsShiftLocal = { c.X * cy + c.Y * sy, -c.X * sy + c.Y * cy, c.Z };
    g_armsShiftWorld = c;
    setTo = { base.X + g_armsShiftLocal.X, base.Y + g_armsShiftLocal.Y, base.Z + g_armsShiftLocal.Z };
    if (!ours || fabsf(setTo.X - arms->Translation.X) + fabsf(setTo.Y - arms->Translation.Y) + fabsf(setTo.Z - arms->Translation.Z) > 0.01f)
        arms->SetTranslation(setTo);
}
// The learned curve is saved next to the ini so the camera is rigid straight away on the next launch.
static void PitchCurvePath(char* out) {
    strcpy(out, g_iniPath);
    char* slash = strrchr(out, '\\');
    strcpy(slash ? slash + 1 : out, "CorvoBodyCamera.bin");
}

static void SavePitchCurve() {
    char path[MAX_PATH]; PitchCurvePath(path);
    if (FILE* f = fopen(path, "wb")) { fwrite(g_pitchCurve, sizeof(g_pitchCurve), 1, f); fclose(f); }
}

// The arc measured cleanly in build 57 (standing still, arms not shifted), every 10 degrees from -90 to +90.
// Learning during play fed back on itself once the 1P arms moved with the camera (the view rides the arms with a
// lag): the curve ran away to +90 uu forward (build 59 log). So the curve is fixed; crouched measured the same.
static void InitDefaultCurve() {
    static const float kFwd[19] = { 32, 32, 31, 30, 28, 25, 20, 15, 9, 7, 1, -7, -11, -16, -21, -25, -30, -31, -32 };
    static const float kUp[19] = { -40, -39, -35, -28, -24, -19, -14, -10, -8, -8, -7, -8, -10, -12, -14, -21, -30, -30, -33 };
    for (int s = 0; s < 2; ++s) {
        for (int b = 0; b < 37; ++b) {
            int i = b / 2;
            float t = (b & 1) ? 0.5f : 0.0f;
            int j = i < 18 ? i + 1 : 18;
            g_pitchCurve[s].fwd[b] = kFwd[i] + (kFwd[j] - kFwd[i]) * t;
            g_pitchCurve[s].up[b] = kUp[i] + (kUp[j] - kUp[i]) * t;
            g_pitchCurve[s].right[b] = 0.0f;
            g_pitchCurve[s].weight[b] = 1.0f;
        }
    }
}

static void LoadPitchCurve() {
    InitDefaultCurve();
    if (!g_cfg.rigidCameraLearn) return;
    char path[MAX_PATH]; PitchCurvePath(path);
    if (FILE* f = fopen(path, "rb")) {
        PitchCurve tmp[2];
        if (fread(tmp, sizeof(tmp), 1, f) == 1) { memcpy(g_pitchCurve, tmp, sizeof(tmp)); Log("loaded camera curve"); }
        fclose(f);
    }
}

static void LogPitchCurve() {
    if (g_cfg.rigidCameraLearn) SavePitchCurve();
    for (int s = 0; s < 2; ++s) {
        char line[1024]; int n = 0;
        for (int b = 0; b < 37; b += 2)
            n += snprintf(line + n, sizeof(line) - n, g_pitchCurve[s].weight[b] > 0 ? " %d:%.0f/%.0f" : " %d:-",
                          b * 5 - 90, g_pitchCurve[s].fwd[b], g_pitchCurve[s].up[b]);
        Log("rigid camera curve (%s, pitch:fwd/up):%s", s ? "crouched" : "standing", line);
    }
    Log("rigid camera correction now %.1f %.1f %.1f", g_camCorrection.X, g_camCorrection.Y, g_camCorrection.Z);
}

// ---------------------------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// Chain loading (compatibility with other dinput8.dll mods)
// ---------------------------------------------------------------------------------------------
// Many Dishonored DLL mods (Dishonored Mod Loader, Mission Stats, Daud's Blink...) are also dinput8.dll proxies,
// and only one file of that name can sit next to Dishonored.exe. CorvoBody keeps the name and loads the others:
// rename the other mod's dinput8.dll to anything starting with "dinput8_" (e.g. dinput8_modloader.dll) in the
// same folder. They load as if they were dinput8.dll. (The ChainLoad list is a baked setting now, empty.)
// The first chained DLL that exports DirectInput8Create also receives the game's call to it, so mods that do their
// work there keep working.
static HMODULE g_chainDinput = nullptr;   // chained DLL whose DirectInput8Create we forward to
static HMODULE g_chainMods[32];
static int g_chainModCount = 0;

// Body Swap plugin (Mods\plugins\BodySwap.dll) API. part: 0 body, 1 first-person arms, 2 sword, 3 power set, 4 crossbow;
// value: 0 Corvo, 1 Daud, 2 Daud's High Overseer disguise (body, arms and sword only).
extern "C" __declspec(dllexport) void __cdecl CorvoBody_SetCharacter(int part, int value) {
    int maxValue = part <= 2 ? 2 : 1;
    value = (value >= 0 && value <= maxValue) ? value : 0;
    InterlockedExchange(&g_charsSet, 1);
    if (part == 0) InterlockedExchange(&g_bodyChar, value);
    else if (part == 1) InterlockedExchange(&g_armsChar, value);
    else if (part == 2) InterlockedExchange(&g_bladeChar, value);
    else if (part == 3) InterlockedExchange(&g_powerChar, value);
    else if (part == 4) InterlockedExchange(&g_bowChar, value);
    Log("Body Swap: %s -> %s", part == 0 ? "body" : part == 1 ? "arms" : part == 2 ? "blade" : part == 3 ? "powers" : "crossbow",
        value == 2 ? "Overseer" : value ? "Daud" : "Corvo");
}

extern "C" __declspec(dllexport) int __cdecl CorvoBody_GetCharacter(int part) {
    return part == 0 ? static_cast<int>(g_bodyChar) : part == 1 ? static_cast<int>(g_armsChar) : part == 2 ? static_cast<int>(g_bladeChar) : part == 3 ? static_cast<int>(g_powerChar) : part == 4 ? static_cast<int>(g_bowChar) : 0;
}

// Extra exports some chained proxies provide, which their plugins look up with GetModuleHandle("dinput8.dll") -
// i.e. they find CorvoBody. Each is a jump to the same export of the chained DLL, resolved on first use.
// Dishonored Mod Loader: BetterPowersBlink.dll calls dinput8.dll!BlinkBootstrap_Register.
extern "C" void* g_fwdBlinkBootstrap = nullptr;
extern "C" void __cdecl ResolveBlinkBootstrap() {
    for (int i = 0; i < g_chainModCount && !g_fwdBlinkBootstrap; ++i)
        g_fwdBlinkBootstrap = reinterpret_cast<void*>(GetProcAddress(g_chainMods[i], "BlinkBootstrap_Register"));
    static bool logged = false;
    if (!logged) { logged = true; Log("BlinkBootstrap_Register forwarded: %s", g_fwdBlinkBootstrap ? "yes" : "no chained DLL exports it"); }
}
extern "C" __attribute__((used)) int __cdecl NoBlinkBootstrap() { return 0; }
// Naked: keeps the caller's stack and registers untouched whatever the calling convention.
extern "C" __declspec(dllexport) __attribute__((naked)) void BlinkBootstrap_Register() {
    __asm__ volatile(
        "cmpl $0, _g_fwdBlinkBootstrap\n\t"
        "jne 1f\n\t"
        "pushl %ecx\n\t"
        "pushl %edx\n\t"
        "call _ResolveBlinkBootstrap\n\t"
        "popl %edx\n\t"
        "popl %ecx\n\t"
        "cmpl $0, _g_fwdBlinkBootstrap\n\t"
        "jne 1f\n\t"
        "jmp _NoBlinkBootstrap\n"
        "1:\n\t"
        "jmp *_g_fwdBlinkBootstrap");
}

static void ChainLoadOtherMods(HMODULE self) {
    char dir[MAX_PATH];
    GetModuleFileNameA(self, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (!slash) return;
    *(slash + 1) = 0;
    char names[32][MAX_PATH];
    int count = 0;
    // 1) dinput8_*.dll next to the exe (alphabetical, as the filesystem returns them)
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%sdinput8_*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count < 32)
                snprintf(names[count++], MAX_PATH, "%s%s", dir, fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    // 2) the ChainLoad setting, a.dll;b.dll (relative to this folder, or full paths) - baked, empty since build 201
    char list[1024] = "";
    SettingString("ChainLoad", "", list, sizeof(list));
    for (char* tok = strtok(list, ";,"); tok && count < 32; tok = strtok(nullptr, ";,")) {
        while (*tok == ' ') ++tok;
        if (!*tok) continue;
        if (strchr(tok, ':')) snprintf(names[count++], MAX_PATH, "%s", tok);
        else snprintf(names[count++], MAX_PATH, "%s%s", dir, tok);
    }
    for (int i = 0; i < count; ++i) {
        HMODULE m = LoadLibraryA(names[i]);
        Log("chain-load %s: %s", names[i], m ? "loaded" : "FAILED");
        if (m && g_chainModCount < 32) g_chainMods[g_chainModCount++] = m;
        if (m && !g_chainDinput && GetProcAddress(m, "DirectInput8Create")) g_chainDinput = m;
    }
}

static HMODULE g_self = nullptr;
static volatile LONG g_chainDone = 0;
static void ChainLoadOnce() {   // from InitThread or the first DirectInput8Create, whichever comes first
    if (InterlockedExchange(&g_chainDone, 1) == 0 && SettingInt("ChainLoadEnabled", 1))
        ChainLoadOtherMods(g_self);
}

static DWORD WINAPI InitThread(LPVOID) {
    ChainLoadOnce();
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    gobjects = reinterpret_cast<TArray<UObject*>*>(base + addr::GObjObjects_rva);
    gnames = reinterpret_cast<TArray<FNameEntry*>*>(base + addr::GNames_rva);
    Log("CorvoBody starting. exe base %p", reinterpret_cast<void*>(base));
    if (base != 0x400000) {
        Log("unexpected image base, aborting (wrong Dishonored.exe version?)");
        return 0;
    }

    LoadSettings();
    LoadPitchCurve();

    LoadPackageAsync = reinterpret_cast<LoadPackageAsync_t>(
        FindPattern("55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 81 EC 8C 00 00 00 56 57"));
    Log("LoadPackageAsync at %p", reinterpret_cast<void*>(LoadPackageAsync));

    // Sanity check the prologue before hooking (push ebp; mov ebp,esp; push -1; push imm32).
    const uint8_t expect[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
    if (memcmp(reinterpret_cast<void*>(addr::GameEngineTick), expect, sizeof(expect)) != 0) {
        Log("UGameEngine::Tick prologue mismatch, aborting");
        return 0;
    }
    if (MH_Initialize() != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(addr::GameEngineTick), reinterpret_cast<void*>(&HookedGameEngineTick),
                      reinterpret_cast<void**>(&g_origGameEngineTick)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(addr::GameEngineTick)) != MH_OK) {
        Log("failed to hook UGameEngine::Tick");
        return 0;
    }
    const uint8_t gcExpect[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
    if (memcmp(reinterpret_cast<void*>(0x0047D750), gcExpect, sizeof(gcExpect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x0047D750), reinterpret_cast<void*>(&HookedCollectGarbage),
                      reinterpret_cast<void**>(&g_origCollectGarbage)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x0047D750)) == MH_OK) {
        Log("CollectGarbage hooked");
    } else {
        Log("WARNING: could not hook CollectGarbage");
    }
    const uint8_t exitExpect[] = { 0x8B, 0x0D, 0x14, 0xA9, 0x40, 0x01 };   // mov ecx, [GCallbackEvent]
    if (memcmp(reinterpret_cast<void*>(0x004645D0), exitExpect, sizeof(exitExpect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x004645D0), reinterpret_cast<void*>(&HookedStaticExit),
                      reinterpret_cast<void**>(&g_origStaticExit)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x004645D0)) == MH_OK) {
        Log("StaticExit hooked");
    } else {
        Log("WARNING: could not hook StaticExit");
    }
    const uint8_t gpvExpect[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x84, 0x03 };
    if (memcmp(reinterpret_cast<void*>(0x005E17A0), gpvExpect, sizeof(gpvExpect)) == 0 &&
        MH_CreateHook(reinterpret_cast<void*>(0x005E17A0), reinterpret_cast<void*>(&HookedGetPlayerViewPoint),
                      reinterpret_cast<void**>(&g_origGetPlayerViewPoint)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x005E17A0)) == MH_OK) {
        Log("GetPlayerViewPoint hooked (rigid camera)");
    } else {
        Log("WARNING: could not hook GetPlayerViewPoint");
    }
    HookPowerSaveLoad();
    HookPowerLevels();
    HookIcons();
    HookPawnSaveLoad();
    HookPlayerSave();
    HookPowerText();
    Log("UGameEngine::Tick hooked (settings fixed, no hotkeys)");
    return 0;
}

static void OpenLog(HMODULE self) {
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    snprintf(g_iniPath, sizeof(g_iniPath), "%sCorvoBody.ini", path);
    strncat(path, "CorvoBody.log", MAX_PATH - strlen(path) - 1);
    g_log = fopen(path, "w");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_self = inst;
        OpenLog(inst);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}

// ---------------------------------------------------------------------------------------------
// dinput8 proxy
// ---------------------------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid,
                                                                   LPVOID* out, LPUNKNOWN outer) {
    // A chained dinput8 mod gets the call (some do their setup there); guard against it calling back into us.
    ChainLoadOnce();
    static LONG depth = 0;
    if (g_chainDinput) {
        if (InterlockedIncrement(&depth) == 1) {
            using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
            auto chained = reinterpret_cast<Fn>(GetProcAddress(g_chainDinput, "DirectInput8Create"));
            HRESULT hr = chained ? chained(inst, version, riid, out, outer) : E_FAIL;
            InterlockedDecrement(&depth);
            if (chained) return hr;
        } else {
            InterlockedDecrement(&depth);   // re-entered from the chained mod: fall through to the real dinput8
        }
    }
    static HMODULE real = nullptr;
    if (!real) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        wcscat(sys, L"\\dinput8.dll");
        real = LoadLibraryW(sys);
        if (!real) return E_FAIL;
    }
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(real, "DirectInput8Create"));
    return fn ? fn(inst, version, riid, out, outer) : E_FAIL;
}

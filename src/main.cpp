// CorvoBody: first-person body awareness for Dishonored (2012), retail Steam exe.
//
// Loaded as a dinput8.dll proxy. Hooks UObject::ProcessEvent, and on the player's PlayerTick:
//   1. async-loads a DLC package that contains Corvo's body mesh (Knife of Dunwall / Brigmore Witches),
//   2. creates a SkeletalMeshComponent with that mesh on the player pawn,
//   3. drives it from the player's own first-person mesh (full-body skeleton) via SetParentAnimComponent.
// No hotkeys and no ini (build 201): the settings are fixed (baked_settings.h); Body Swap is chosen in Pause > Mods.

#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cmath>
#include <algorithm>
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
static LoadPackageAsync_t RawLoadPackageAsync = nullptr;

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
    bool quickTurn = true;         // Ultimate Back Turn: R3 / Left Alt: quick, smooth 180 degree turn
    float quickTurnTime = 0.18f;   // ...taking this long (s)
    int summonAssassinMax = 100;        // Summon Assassin: assassins allowed out at once (game: 1)
    bool crossbowSwapInHand = true;   // Body Swap: choosing the other crossbow while holding one swaps it in the hand - put away at once, swapped, the new one drawn (user: "at the moment you have to go into the weapon wheel, select something else, then select crossbow again")
    bool crossbowUpgradesCarryOver = true;   // Body Swap: the crossbow upgrades bought from Piero work on whichever bow is in use - Corvo's crossbow or Daud's wristbow (user 2026-10-12: "make it so the bow upgrades carry over to each other")
    bool crossbowAlwaysLoaded = true;   // crossbow / wristbow reload instantly after each shot (while there's ammo)
    float zoomHoldTime = 0.25f;    // R3 / Left Alt held this long (s): the game's Zoom instead, while held
    bool hideTorso = true;         // always hide the visible body's chest/collar (Torso_jnt) - not the shadow's
    bool hideUpperWhenClipping = false;  // hide the whole upper body (incl. arms) while airborne/sprinting/looking up/dipping
    bool hideShouldersUp = false;  // always hide spine_3 and everything above it (shoulders, arms, chest, neck) on the visible body
    float slideRaise = 5.0f;       // move the body up (+) / down (-) while sliding: hips just above the ground (user reference image)
    float slideBack = -10.0f;      // ...and back (+) / forward (-): camera over the hips looking down both legs (user: only a foot showed at 30)
    float deepCrouchSink = 35.0f;   // body sunk more than this (normal crouch 22.5, slide/low crouch 54.5): hide the torso
    bool lowCrouchHideLegs = true;  // ...and (not sliding) fold the legs away into the hips, e.g. crouching under tables
    bool takedownHideTorso = true;  // hide the visible torso during choke / assassination takedowns (the camera leans in)
    float hitCameraScale = 0.3f;    // how far the camera is thrown when hit (1 = the game's own, 0.3 = 70% less)
    float strafeHideTorsoSpeed = 40.0f;   // moving sideways faster than this (uu/s): hide the visible torso from the waist (20:12 clip: strafing, the collar rim round the collapsed neck bobbed into the bottom edge of the view every step); 0 = off
    float strafeHideTorsoPitch = -30.0f;  // ...only while looking above this pitch (looking further down, the chest is in view and the collar is behind the camera)
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
    bool slideArms = true;          // slide (hand free): a bracing pose - elbows down/back/out, forearms up and forward (00:35 clip: "arms straight out")
    // (user 01:20: right hand over the chest, left arm down to the floor pointing a little left)
    // (07:30 log: the hand-on-chest forearm sat 24 uu from the camera - the near guard hid that arm - and the floor hand was
    // under the bottom edge; user 07:35 "arms in view when holstered": both hands ahead of the camera, lower-left / lower-right)
    // (user 07:55: right arm raised a bit, left arm further out to the left)
    // (user 08:20: left hand open, flat on the ground as if keeping balance; right arm up a bit more - it clipped the chest)
    float slideElbowL[3] = { 0.30f, 0.55f, -0.78f };   // ...left elbow direction from the shoulder: forward, out, up (normalised) - down, out and a little forward (09:15: "couldn't see it")
    float slideForearmL[3] = { 0.35f, 0.45f, -0.82f }; // ...left forearm direction from the elbow: on down to the floor, out and forward
    bool slideLeftHandFlat = true;  // ...and that hand flat on the ground: palm down, fingers forward
    float slideLeftHandPalmSign = -1.0f;   // ...the bind-derived "palm" side goes down with +1; on Daud's 1P hand the thumb joints sit on the BACK side of the knuckle plane (09:55 log: thumb side +0.63, palm showed), so -1
    float handFlatCurl[4] = { -14.0f, -18.0f, -18.0f, -14.0f };   // braced flat hand: curl beyond the bind (negative = extend; the 1P bind is a slightly curled hand) at index/middle/ring/pinky
    float handFlatThumb = -12.0f;   // ...thumb joints (10:48 clip: the thumb tip sank into the floor; was -6)
    float slideLeftHandThumbUpDeg = 8.0f;   // ...and the braced hand rolls about its fingers so the thumb edge lifts off the floor (rests more on its outer edge)
    bool slideShowUpperArms = false; // slide: the body's own upper arms show (tried 08:55 for the forearm floating over the chest; 09:30 clip: read as "old arms without hands" - off)
    float slideElbowR[3] = { 0.78f, 0.40f, -0.36f };   // ...right elbow: forward, well OUT to the right, down - less down and more forward (11:05 clip: the thumb clipped the chest; "lift the arm up and move it forward")
    float slideForearmR[3] = { 0.64f, -0.50f, 0.64f }; // ...right forearm: in across to the chest, raised and forward (hand ~5 uu up, ~4 forward, 3 less across vs 09:55)
    bool slideHideFirstPersonArms = true;   // slide, both hands free: the game's own 1P arms are hidden (09:15 clip: a second, darker pair of hands under the view)
    float slideArmNearScale = 0.6f; // slide: the arm skin's near-camera limits scaled by this (the bracing hands are close by design)
    float slideCamSettle = 0.4f;    // slide -> crouch: the view height eases up into the crouch view over this long (s; user 10:30: "the camera jolts")
    float slideReclineLow = 32.0f;  // slide: extra lean-back at the base of the spine (spine_0), so the stomach and chest lie lower (user 07:35; 08:40 "a bit lower": 20 -> 32)
    float slideFootPoint = 35.0f;   // slide: the extended right foot's toes pointed (deg, as HoverToe; user 01:20)
    float slideBackTwist = 0.0f;    // slide: twist of the other (left, knee-up) leg about its length, as SlideFrontTwist
    float slideBackInward = -20.0f; // ...and its swing toward the middle (negative = out: user 01:20 "lean the left leg left a bit more")
    float slideTurnLag = 6.0f;      // slide: the body turns after the view, catching up at this rate (1/s; 0 = rigid) - Dishonored 2 style (user 00:52)
    float slideTurnLagMax = 45.0f;  // ...never more than this far behind (deg)
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
    float armSkinJumpBack = 10.0f;  // ...the new arms eased back (uu) in the air (their shoulders clipped the camera)
    float armSkinJumpDown = 5.0f;   // ...and down
    float armSkinJumpCentre = 1.0f;     // ...and moved across so they sit centred on the view (0 = off, 1 = fully; 17:32 clip)
    float armSkinJumpCentreMax = 30.0f; // ...by at most this much (uu)
    float armSkinShiftRight = 0.0f;     // ...moved sideways (uu, actor space). Was -6: the 20:03 "hands right of the model" gap was the
                                        // skin inheriting the shadow body's transform (UpdateArmSkin) - -6 was drawn as 6 UP, not left
    float armSkinSprintShiftRight = 0.0f;   // ...sprinting instead (was -2, same reason)
    float armSkinSprintDown = 10.0f;    // ...and this much lower while sprinting (the swing passed through the camera; 6 -> 10 20:19)
    float armSkinNearForearm = 24.0f;   // ...a forearm this close to the camera shrinks the arm too (20:03: bracer filled the view at 21)
    float armSkinNearForearmSprint = 28.0f; // ...while sprinting (20:10/20:19 logs: left forearm dips to ~26-28 every stride, in the lens)
    bool hideFirstPersonSprintHands = true;   // sprinting with a hand free: the game's own 1P forearm there hidden (19:58 clip)
    bool armSkinAlign = false;          // ...moved each frame so its engine-read shoulders meet the body's (19:46 clip: the readings
                                        // jump 40-60 uu frame to frame - the arms floated around; off, readings not used)
    bool armSkinOriginFix = true;       // ...placed with the mesh origins in the engine's order (19:22 clip: 9.5 right/high without)
    float armSkinSprintBack = 18.0f;    // ...eased back this much more than the body while sprinting (inside of the shoulders showed; 8 -> 12 -> 18)
    float armSkinStairUpCut = 1.0f;     // ...share of the body's rise with a high camera (stairs down) the arms DON'T follow (20:19 clip)
    bool fpArmSkin = true;          // Daud / Overseer bodies: forearms + hands drawn with the first-person arms model (user 2026-10-11)
    float armSkinNear = 18.0f;      // ...an arm whose upper arm or forearm comes this close (uu) to the camera shrinks away (sprint swing)
    float armSkinNearHold = 0.6f;   // ...and stays away until it has been clear this long (s), so it doesn't pulse per stride
    bool climbArms = false;         // ladders/chains: the body's upper arms reach to the first-person forearms (built 2026-10-11, parked by the user for later)
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
    bool climbHands = false;       // (OFF 19:2x: user reverted to the original climbing - grip legs, no climbing arms) ladders/chains: the first-person arms climb hand over hand (user 18:55: "the hands take turns pulling the player up the chain"). Each hand grips a point on the chain that stays put in the world - the climb carries it down past the face - lets go near the chest and reaches above the head while the other holds. Built procedurally: the game has no climbing animation for the body, its 1P hands sit still on a chain (log 18:54: 35 uu below the shoulder throughout)
    float climbChainForward = 16.0f; // ...the chain this far in front of the pawn (the body is moved ClimbBodyLeft so the chain runs between its legs: the hands grip there too)
    float climbGripTop = 4.0f;       // ...a hand takes a new grip this far above the eyes
    float climbGripBottom = -28.0f;  // ...and lets go once the climb has carried it down to here (below the eyes)
    float climbReachTime = 0.32f;    // ...seconds for a reach to the next grip (shorter when climbing fast)
    float climbHandSpread = 2.0f;    // ...each hand this far to its side of the chain
    float climbArmNearScale = 0.3f;  // ...the arm skin's near-camera guard limits scaled by this while climbing (the hands are at the face by design)
    bool selfTestCapture = false;    // (OFF for the 3.0 release; 20:1x: Present is hooked on the game's OWN device, found from inside SetTexture) DEVELOPMENT: the mod saves its own frames (CorvoBody\captures\*.bmp, half size): 3 a second while climbing (45 per session) and a 10-frame burst on the Home key - so the author can see the result without a clip. TURN OFF FOR RELEASE
    bool climbShadowArms = false;  // (OFF 19:2x: reverted) ladders/chains: the shadow's hands reach the game's own first-person climbing hands (hand over hand, fingers gripping) - the same reach as the weapon arms (user 18:45: "a chain climbing animation for the arms and legs"; the game has none for the body, only the 1P hands)
    bool climbLegCycle = false;    // (OFF 19:2x: reverted, see ClimbHands) ...and the legs climb: crossed at the ankles (the grip), the knees drawn up and pushed down again once per ClimbLegStride climbed - a rope-climb foot lock; the still grip (with its sway) when not moving
    float climbLegStride = 40.0f;  // uu climbed per leg cycle
    float climbLegBendHip = -85.0f;   // knees drawn up: thighs
    float climbLegBendKnee = 115.0f;  // ...knees
    float climbLegPushHip = -30.0f;   // legs pushed down (standing on the foot lock): thighs
    float climbLegPushKnee = 40.0f;   // ...knees
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
    bool weaponArmShadow = true;   // a second casting body keeps the weapon-side arm in the shadow (13:05: with shadows on, the arm the skin hides vanished from the shadow); 0 = g_shadowBody casts (one-armed with a weapon out)
    float castHoldAwayMs = 250.0f; // casting body: a busy hand whose 1P hand has been out of view this long (not looking up, not mid-draw) is treated as put away - its arm relaxes to the standing pose (14:45: blocking with the sword tucks the off-hand's item away)
    bool castLightEnv = true;      // casting body / head / blobs / item copies get their own DynamicLightEnvironment with bCastShadows (the 1P arms' environment they borrowed casts none): indoor lamps cast the player's shadow too, as they do for NPCs (user 15:37: "only the sun casts a player shadow")
    float castShoulderBlob = 0.0f;   // OFF (17:14 clip: at any size/orientation the blob read as a shoulder pad); kept for experiments.   // (17:07 clip: at 0.35 sunk into the joint the blob was entirely inside the silhouette - the notch is above and outside the joint)  // casting body: a shadow-only blob (the head mesh, this scale) at a raised arm's shoulder joint fills the notch the rig opens there (user 15:25, red marks); 0 = off
    float castShoulderBlobUp = 6.0f;  // ...its centre's offset from the joint: up (the notch is at the top of the shoulder)
    float castShoulderBlobOut = 5.0f; // ...out to the arm's side
    float castShoulderBlobFwd = 0.0f; // ...forward
    float castReachMax = 0.80f;    // (15:15 clip, front-lit wall: at 0.93 both upper arms still pointed at the wall and foreshortened into lumps - wide square shoulders; a properly bent elbow angles the upper arm down)    // casting body: the hand target is kept within this fraction of the arm's length from the shoulder, so the elbow always keeps a bend (15:00 clip: at full reach the IK locked the arm dead straight - a thin stick in the silhouette)
    bool castHolsterShoulders = true;   // the holstered shoulders stay in the shadow with weapons out: a second, never-rendered casting body in the plain (holstered) pose, forearms + hands folded to the elbow, merged into the casting body's shadow
    float castHolsterUpperArm = 0.8f;   // ...its upper arm's length/size on a raised side (1 = the whole holstered upper arm down to the elbow; smaller pulls it up into the shoulder)
    bool itemMeshCastShadow = true;     // the held items' models cast shadows even where their mesh turns shadow casting off per section (18:3x, user: no crossbow or grenade shadow under any light - first-person weapon models ship with section shadow casting off). The real first-person item component is kept from casting at the same time
    bool castOneShadow = true;          // the head shadow and the items-in-hand shadows are drawn as part of the casting body's own shadow (user 18:1x: under lamps the head and the items were darker where they overlapped the body - each cast its own shadow and the overlaps darkened twice; the sun draws everything into one shadow already)
    bool castHolsterShadowParent = true; // ...rendered as part of the casting body's own shadow (one shadow, no darker overlap under indoor lights)
    bool castArmRoll = true;         // casting body: the limb IK's hinge points the right way round (bInvertJointAxis per arm, learned off this body's own animated elbow while the IK is off, checked against the result). 17:28 log: the IK'd upper arm sat 167-180 deg from its rest rotation - a half turn about the arm's length: the skin between collar bone and upper arm collapses (the shoulder notch, the arm "floating below the shoulder"), and the wrist the same way
    int castArmRollInvert = -1;      // override for both arms: 0 / 1 (-1 = learned)
    float castClavicleFollow = 0.0f; // (off: 17:28 log "IK swing 0 deg" - the engine rewrites LocalAtoms after the controls, so the animated pose can't be read back; and the roll was the real fault) // casting body: the collar bone turns this fraction of the way toward the IK-raised upper arm, by the MEASURED rotation the IK added to the upper arm over the animation, in the collar bone's own frame (no guessed axis; settles at follow/(1+follow) = a third of the raise, the real shoulder's share); 0 = off
    float castClavicleFollowMax = 25.0f; // ...and never more than this many degrees
    float castClavicle = 0.0f;     // (off: even a trace added to the shoulder lump; the bent elbow is what makes the join natural)     // ...and its shoulder follows a raised arm: this fraction of the hand's elevation above hanging, as a forward shoulder roll (0 = off; 15:10 clip: 0.3 / 28 rolled the collar into a boxy hump seen from the front)
    float castClavicleMax = 10.0f; // ...capped (deg)
    float castElbowOut = 20.0f;    // ...its weapon arm's elbow pole: this far out to the side of the shoulder-hand midpoint (actor space; 14:34 clip: 55 stuck the elbow out like a wing)
    float castElbowDown = 55.0f;   // ...and this far below it (the elbow hangs under the arm, as a real one does)
    float castElbowBack = 40.0f;   // ...and this far behind it (was a fixed 10 until 18:2x: the pole then sat almost straight down-out from the shoulder, where the lowered sword hand bobs while walking - each time the hand crossed that line the bend plane flipped and the arm whipped half round, user: "the right arm's shadow looked like it was spinning"). Behind the shoulder no drawn 1P hand can reach it; with the hand out in front the elbow lands where it did
    float castElbowTurnRate = 720.0f; // ...and the bend plane turns at most this many degrees per second (a backstop against any remaining flip)
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
    bool jumpHands = true;          // ...hands up like claws (wrists back, fingers curled) and swaying with the body
    float jumpWristDeg = -20.0f;    // ...wrist bend (negative = cocked back; flip the sign if they fold forward)
    float jumpFingerCurlDeg = 1.0f; // ...finger bend: knuckles 80%, middle joints 140%, tips 80% (user: straighter than 18, then 9)
    float jumpThumbDeg = 0.0f;     // ...thumb bend (negative = away from the palm; +12 tucked the thumbs under, user)
    float jumpThumbSpreadDeg = -8.0f;// ...thumb swing (each step down brought it in: 25, 14, 7, -6; user: out more again, flatter hands)
    float jumpPalmSign = -1.0f;     // ...which side of the hand faces the camera (user 22:3x: +1 showed the palms)
    bool jumpHandsInView = true;    // ...hands held up in front of the view (arm IK), not hanging off the body
    float jumpHandFwd = 14.0f;      // ...where: ahead of the eyes (uu; user: pulled back toward the camera from 34, then 24)
    float jumpHandSide = 44.0f;     // ...out to each side (user: more apart than 38, 46; then closer than 54)
    float jumpLeftHandDown = 6.0f;  // ...the left hand this much lower than the right (user)
    bool armSkinHandoff = true;     // ...hand over to / take over from the game's 1P arm smoothly on draw / holster (user 22:57)
    float armSkinHandoffHide = 0.95f;   // ...a busy arm hides once the reach for the 1P hand is this complete (0..1)
    float armSkinLowerTime = 0.45f;     // ...after a holster the arm lowers from the 1P hand to the side over this long (s)
    float armSkinReachMaxPitch = 10.0f; // ...no handover reach looking up past this (deg) - 23:40: sleeves swept the top of the view
    float armSkinDrawWindow = 0.7f;     // ...a busy arm only takes over (1P hand off the bottom) this long into a draw (s)
    bool handMatchSkin = true;      // ...free hands take the 1P arms' own hand skeleton (bind positions + rotations) - 22:35
    float handFistCurl[4] = { 88.0f, 90.0f, 92.0f, 94.0f };   // ...sprint fist: curl (deg) at the knuckle (_0) on that bind,
    float handFistMid = 1.05f, handFistTip = 0.6f;             //    middle joint (_1) and tip joint (_2) x these (user 22:52: the
                                                               //    knuckles stayed straight - _0 is the first finger segment)
    float handFistThumb0 = 0.0f, handFistThumb1 = 50.0f;       // ...sprint fist thumb root curl / thumb tip curl (over the fingers)
    float handFistThumbTuck = -12.0f;                          // ...thumb root swung in across the fingers (local Z; -Z tucks)
    bool handRelaxedPose = true;    // ...free hands standing/walking: a relaxed pose (user 22:18: the power pose looked tense there)
    float handRelaxCurl[4] = { 3.0f, 5.0f, 7.0f, 9.0f };      // ...extra curl (deg) at the knuckle (_1) on the 1P arms' bind pose:
                                                              //    index, middle, ring, pinky; the middle joint (_2) gets 1.3x
                                                              //    (user 22:27: fingers can be straighter standing/walking)
    float handRelaxThumb0 = 0.0f, handRelaxThumb1 = 4.0f;      // ...thumb root / thumb tip extra curl
    bool jumpLeftPowerHand = true;  // ...the left hand takes the 1P "holding a power" finger pose (user 2026-10-11)
    float jumpArmRollSign = -1.0f;  // ...arm roll vs the elbow bend (flip if the elbows look broken / palms face up)
    float jumpHandUp = -80.0f;      // ...up (negative = below eye level; user: lower than -2, -10, -17, -22, then quite a bit lower than -38)
    float jumpArmYawDeg[2] = { 0.0f, 30.0f };    // ...per arm (L, R): whole arm turned around the shoulder, + = to the right
    float jumpForearmLift[2] = { 0.0f, 34.0f };  // ...per arm: hand raised (uu) so the forearm bends up at the elbow
    float jumpHandIn[2] = { 0.0f, 18.0f };       // ...per arm: hand pulled in toward the chest's middle (uu, user: right arm closer)
    float jumpHandBack[2] = { 0.0f, 4.0f };      // ...per arm: and back toward the body (uu)
    float jumpHandOutDeg = 12.0f;   // ...hands turned outward at the wrist (left to the left, right to the right; user: slightly)
    float jumpForearmRollDeg[2] = { 0.0f, 0.0f };      // (user: both hands like the unrolled left hand; mirrored rig - the left thumb side is bone +Y) // ...per arm: forearm + hand rolled about their length (R: thumb side to the camera)
    float jumpElbowForward = 1.2f;  // ...T-pose arms: upper arms angled this far forward from straight out (user: arms in more than 0.08; 11:05: hands just out of view at 0.6 - log x 1.06..1.6 - so further in)
    float jumpElbowDrop = 2.0f;     // ...upper arms angled down from level (~63 deg; 0.9 / ~42 deg read as a zombie pose, user)
    float jumpHandOpenFrac = 0.6f;  // ...the captured power pose's fingers opened by blending each joint this far toward its open bind pose (0 = the half-closed power pose, 1 = fully the bind). 11:29 clip: "half closed" in jumps and the right hand in slides; the flat left slide hand is separate
    float jumpCurlSignL = 1.0f;     // ...curl direction per hand (-1 if the fingers bend backwards)
    float jumpCurlSignR = 1.0f;
    float jumpSway = 0.0f;          // ...how much the arms move with falling, strafing and turning (0 = still; user: stationary)
    bool jumpHideTorso = true;      // hide the visible torso while airborne/vaulting/landing (camera clipped the coat)
    bool jumpPose = true;          // procedural jump/fall pose + landing squat
    float jumpHipDeg = -40.0f;      // lead hip in the air (same sign as the slide hips: negative = forward)
    float jumpKneeDeg = 60.0f;      // knee tuck in the air (trailing knee; the lead knee gets 70%)
    float jumpLegSpread = 10.0f;    // ...both thighs swung this far out from the middle (deg) so the knees never cross (10:55 clip; negative flips)
    bool jumpIdleLegs = false;      // ...the walk cycle gives way to the idle in the air (11:00; the user preferred the frozen stride's fluidity - off)
    float jumpKneeMax = 105.0f;     // ...total knee bend cap in the air (deg): the tuck only adds what the frozen stride leaves room for (11:10; a shin folded through its thigh)
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
    float armForwardSprintCut = 0.6f;
    float armStandSpreadDeg = -6.0f;  // standing / walking: the free arms rolled this far out from the body (user 10:15: the hands clipped the coat; +6 rolled them IN behind the coat - 10:20 screenshot)  // ...this share less while sprinting (20:03: 25 deg standing, ~10 sprinting - the swing clips)
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
static volatile LONG g_jumpChar = 0;    // Agility's power jump: 0 Corvo's double jump, 1 Daud's one continuous jump
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
        g_jumpChar = SettingInt("JumpCharacter", 0) == 1 ? 1 : 0;
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
    g_cfg.quickTurn = readBool("QuickTurn", g_cfg.quickTurn);
    g_cfg.summonAssassinMax = std::min(std::max(SettingInt("SummonAssassinMax", g_cfg.summonAssassinMax), 1), 1000);
    g_cfg.crossbowAlwaysLoaded = readBool("CrossbowAlwaysLoaded", g_cfg.crossbowAlwaysLoaded);
    g_cfg.crossbowUpgradesCarryOver = readBool("CrossbowUpgradesCarryOver", g_cfg.crossbowUpgradesCarryOver);
    g_cfg.zoomHoldTime = std::min(std::max(readFloat("ZoomHoldTime", g_cfg.zoomHoldTime), 0.1f), 2.0f);
    g_cfg.quickTurnTime = std::min(std::max(readFloat("QuickTurnTime", g_cfg.quickTurnTime), 0.05f), 1.0f);
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
    g_cfg.jumpHands = readBool("JumpHands", g_cfg.jumpHands);
    g_cfg.jumpWristDeg = readFloat("JumpWristDegrees", g_cfg.jumpWristDeg);
    g_cfg.jumpFingerCurlDeg = readFloat("JumpFingerCurlDegrees", g_cfg.jumpFingerCurlDeg);
    g_cfg.jumpThumbDeg = readFloat("JumpThumbDegrees", g_cfg.jumpThumbDeg);
    g_cfg.jumpCurlSignL = readFloat("JumpCurlSignL", g_cfg.jumpCurlSignL);
    g_cfg.jumpCurlSignR = readFloat("JumpCurlSignR", g_cfg.jumpCurlSignR);
    g_cfg.jumpSway = readFloat("JumpSway", g_cfg.jumpSway);
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
    g_cfg.weaponArmShadow = readBool("WeaponArmShadow", g_cfg.weaponArmShadow);
    g_cfg.castArmRoll = readBool("CastArmHinge", g_cfg.castArmRoll);
    g_cfg.castArmRollInvert = static_cast<int>(readFloat("CastArmHingeInvert", static_cast<float>(g_cfg.castArmRollInvert)));
    g_cfg.castHolsterShoulders = readBool("HolsteredShoulders", g_cfg.castHolsterShoulders);
    g_cfg.castHolsterUpperArm = readFloat("HolsteredShoulderUpperArm", g_cfg.castHolsterUpperArm);
    g_cfg.castHolsterShadowParent = readBool("HolsteredShoulderOneShadow", g_cfg.castHolsterShadowParent);
    g_cfg.castOneShadow = readBool("OneShadow", g_cfg.castOneShadow);
    g_cfg.itemMeshCastShadow = readBool("ItemModelsCastShadow", g_cfg.itemMeshCastShadow);
    if (!g_cfg.playerShadows) { g_cfg.shadowBody = false; g_cfg.headShadow = false; g_cfg.fpArmShadow = false; g_cfg.itemShadows = false; }
    // The first-person arm skin copies the shadow body's pose (unshrunk arms), so it is built even with player shadows
    // off - it just casts no shadow then (CreateShadowBody).
    if (g_cfg.fpArmSkin) g_cfg.shadowBody = true;
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
    g_cfg.strafeHideTorsoSpeed = readFloat("StrafeHideTorsoSpeed", g_cfg.strafeHideTorsoSpeed);
    g_cfg.strafeHideTorsoPitch = readFloat("StrafeHideTorsoPitch", g_cfg.strafeHideTorsoPitch);
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
    g_cfg.climbShadowArms = readBool("ClimbShadowArms", g_cfg.climbShadowArms);
    g_cfg.climbHands = readBool("ClimbHands", g_cfg.climbHands);
    g_cfg.climbChainForward = readFloat("ClimbChainForward", g_cfg.climbChainForward);
    g_cfg.climbGripTop = readFloat("ClimbGripTop", g_cfg.climbGripTop);
    g_cfg.climbGripBottom = readFloat("ClimbGripBottom", g_cfg.climbGripBottom);
    g_cfg.climbReachTime = readFloat("ClimbReachTime", g_cfg.climbReachTime);
    g_cfg.climbHandSpread = readFloat("ClimbHandSpread", g_cfg.climbHandSpread);
    g_cfg.climbArmNearScale = readFloat("ClimbArmNearScale", g_cfg.climbArmNearScale);
    g_cfg.selfTestCapture = readBool("SelfTestCapture", g_cfg.selfTestCapture);
    g_cfg.climbLegCycle = readBool("ClimbLegCycle", g_cfg.climbLegCycle);
    g_cfg.climbLegStride = readFloat("ClimbLegStride", g_cfg.climbLegStride);
    g_cfg.climbLegBendHip = readFloat("ClimbLegBendHip", g_cfg.climbLegBendHip);
    g_cfg.climbLegBendKnee = readFloat("ClimbLegBendKnee", g_cfg.climbLegBendKnee);
    g_cfg.climbLegPushHip = readFloat("ClimbLegPushHip", g_cfg.climbLegPushHip);
    g_cfg.climbLegPushKnee = readFloat("ClimbLegPushKnee", g_cfg.climbLegPushKnee);
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
static UObject* g_handItem[2] = { nullptr, nullptr };
static DWORD g_handsChangedTick = 0;     // UpdateHands: when the held items / their 1P models last changed (a draw, holster or switch starts)
static DWORD g_handSeqChangedTick = 0;   // ...when either held item's current animation last changed
static bool g_handsAllIdle = true;       // ...both held items are on an idle animation (or have none)   // per hand (0 left, 1 right): the held inventory item (UDishonoredInventoryItem)
static FVector g_shadowBodyT = { 0, 0, 0 };     // SyncTransform: the shadow body's Translation (actor space)
static FVector g_armsShadowOffset = { 0, 0, 0 };   // world offset from the 1P arms to the arm shadow copy (shoulders onto the body)
static USkeletalMeshComponent* g_armsShadow = nullptr;  // shadow-only copy of the first-person arms (pose copied from pawn->Mesh)
static USkeletalMeshComponent* g_armSkin = nullptr;   // UpdateArmSkin: the 1P arms model drawn on Daud's / the Overseer's body
static USkeletalMeshComponent* g_itemShadow[2] = { nullptr, nullptr };   // shadow-only copies of the held items' models
static bool g_bodyVisible = false;             // UpdateVisibility: first-person view (not a cutscene)
static int g_handKind[2] = { 0, 0 };          // per hand (0 left, 1 right): 0 empty, 1 sword, 2 pistol/crossbow, 3 power/other
static bool g_onePHandOnScreen[2] = { false, false };   // the game's 1P hand is in view (UpdateOnePHandOnScreen; handover)
static bool g_onePHandBelow[2] = { false, false };      // ...it is out of view under the bottom edge
static DWORD g_onePHandOffMs[2] = { 0, 0 };             // ...how long it has been out of view (ms; 0 while in view)
static DWORD g_handFreeMs[2] = { 0x7FFFFFFF, 0x7FFFFFFF };   // UpdateCoat: ms since that hand went busy -> free (0 while busy)
static bool g_skinArmShown[2] = { false, false };       // UpdateArmSkin: that side of the arm skin is drawn this frame
static bool g_handDrawing[2] = { false, false };        // UpdateHands: being drawn, its 1P model not shown yet (<= 600 ms)
static FVector g_skinOffWorld = { 0, 0, 0 };            // where the arm skin draws the shadow pose: shadow + this (UpdateArmSkin)
static float g_weaponRaise[2] = { 0, 0 };     // eased 0..1 per hand (0 left, 1 right): shadow arm raised for a weapon
static float g_crouchSink = 0.0f;              // set by UpdatePosture: how far crouching sank the body (uu)
static FVector g_viewLoc = { 0, 0, 0 };        // final view location from the GetPlayerViewPoint hook
static DWORD g_viewLocTick = 0;
static FRotator g_viewRot = { 0, 0, 0 };       // ...its rotation
static float g_viewFov = 90.0f;                // ...horizontal field of view (degrees)
static float g_viewGuardBack = 0.0f;           // view guard: uu the visible body is eased back so no upper-body part is in view
static float g_landHold = 0.0f;                // 1 -> 0 over 0.4 s after landing (UpdateJump)
static float g_slideLegW = 0.0f;              // UpdateSlide: weight of the slide leg pose (foot IK takes the rest)
static float g_slideYawOffDeg = 0.0f;         // SyncTransform: how far the body's yaw lags the view in a slide (deg, + = left of it)
static float g_slideCamW = 0.0f;              // UpdateSlide: weight of the slide camera raise (in/out at 6/s, matching the game's own camera move)
static float g_slideCamSettleOff = 0.0f;      // view hook: how far the slide -> crouch settle holds the view below the computed height (<= 0); the 1P arms follow it
static float g_upperAmount = 0.0f;            // eased 0..1: visible upper body (spine_3 up) hidden
static float g_torsoAmount = 0.0f;            // eased 0..1: visible torso from the waist up hidden
static float g_legHideAmount = 0.0f;          // eased 0..1: visible legs folded into the hips (crouching under tables)
static float g_hoverAmount = 0.0f;            // eased 0..1: floating leg pose (held in the air by Daud's Transversal)
static bool g_hitActive = false;              // UpdateHitState: just hit / stunned (hit reaction under way)
static FVector g_clipDir = { -0.8f, 0.0f, -0.6f };   // clip guard push direction (player space, unit)
static float g_clipPush = 0.0f;                // clip guard: uu the visible body is pushed back/down from the camera
static float g_climbAmount = 0.0f;            // UpdateClimb: eased 0..1 on a chain/ladder
// (14:33 log: "physics 0 -> 1" every frame, and the very first line too - the static starts at -1, so its memory is being
// zeroed before and between frames by something. The value lives here now, and PhysProbe after each per-frame step
// logs which step it changed under.)
static int g_physLast = -1, g_physLastSet = -1;
static float g_castS[2] = { 0.0f, 0.0f };
static float g_climbHandUp[2] = { 0.0f, 0.0f };
static float g_climbGrip[2] = { 0.0f, 0.0f };     // UpdateClimbHands: finger grip per hand (1 = closed round the chain), read by UpdateJumpHands
static bool g_climbHandsOn = false;              // UpdateClimbHands posed the arms this frame
static float g_climbVz = 0.0f;
static float g_climbLegPhase = 0.0f, g_climbLegMove = 0.0f;   // UpdateClimb: the legs' climbing cycle (reset when the climb ends)                   // UpdateClimb: measured climbing speed (uu/s, from the pawn's height - on a chain Velocity.Z reads 0)
static volatile LONG g_captureWant = 0;          // frame capture: 1 = save the next presented frame
static volatile LONG g_captureCount = 0;         // ...frames saved this session
static char g_captureTag[16] = "frame";   // UpdateCastingArmReach: how far each drawn 1P hand is above this body's shoulder (actor space), for the climb log      // UpdateCastingArmReach: this frame's casting-arm strength per hand (0 when its reach is off)
static float g_castHold[2] = { 1.0f, 1.0f };   // UpdateCastingArmReach: 1 = the hand holds its item; eases to 0 while the game keeps the 1P hand away (a block)
static void PhysProbe(const char* after) {
    if (g_physLastSet == -1 || g_physLast == g_physLastSet) return;
    static DWORD lastT = 0;
    if (GetTickCount() - lastT < 1000) return;
    lastT = GetTickCount();
    Log("probe: physLast is %d (UpdateClimb stored %d) after %s", g_physLast, g_physLastSet, after);
}
static const char* g_lastHideWhy = nullptr;    // UpdateVisibility: reason of the current hide (log filter)
static bool g_mantleWant = false;              // UpdateVisibility: camera pulled back by a mantle
static float g_mantleAmount = 0.0f;            // eased 0..1: visible body shrunk away for the mantle
static bool g_torsoHidden = false;             // UpdateLean: visible torso hidden from the waist up this frame
static float g_swimAmount = 0.0f;             // 0..1 eased swimming pose weight (UpdateSwim)
static float g_jumpAmount = 0.0f;             // 0..1 eased airborne pose weight (UpdateJump)
static float g_jumpSwayRaise = 0.0f;          // jump hands: extra arm raise (deg) from falling speed + drift
static float g_jumpSwayRoll = 0.0f;           // ...and sideways lag (deg) from strafing and turning
static float g_jumpFlex = 0.0f;               // ...slow finger flex (-1..1)
static bool g_armSkinOn = false;              // the first-person arm skin is drawn on the body (UpdateArmSkin)
static float g_sprintBackAmount = 0.0f;       // 0..1 eased sprint weight (SyncTransform; the arm skin eases back with it)
static float g_jumpBodyW = 0.0f;              // 0..1 weight of the jump's body back/down offset (held through the landing)
static float g_landDip = 0.0f;                 // uu (<= 0) both bodies drop on landing; foot IK bends the knees
static float g_zAbsorb = 0.0f;                 // SyncTransform: a pawn Z step after a slide, held back and released like the camera
static float g_absorbLastZ = 0.0f, g_absorbLastCyl = 0.0f;   // ...the pawn Z / cylinder height it last saw (shared with the view hook, which takes a step out in the same frame)
static DWORD g_absorbSlideEndAt = 0;           // ...GetTickCount at the slide's end (the absorb watches for a second after it)
static LONGLONG g_absorbQpc = 0;               // ...when that snapshot was taken (the view hook's velocity term)
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
static USkeletalMeshComponent* g_shoulderBlob[2] = { nullptr, nullptr };
static UDynamicLightEnvironmentComponent* g_castLightEnv = nullptr;   // the casters' own light environment: casts the composite shadow from the lights around the player, not just the sun (user 15:37)   // shadow-only "deltoid" at each shoulder joint of the casting body while that arm is raised (user 15:25: fill the gap)
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
// The Overseer 1P arms mesh has NO material of its own (package: Materials[0] = None) - the game supplies it at runtime in
// the DLC. Ours: DLC07_Player_DaudOverseer_inst from the same package (22:01: the arms didn't look like the disguise).
static UMaterialInterface* g_ovsArmsMat = nullptr;
static USkeletalMesh* g_ovsSwordMesh = nullptr;
static int g_builtChar = -1;                      // character the current body was built with
static USkeletalMeshComponent* g_shadowBody = nullptr;   // hidden full body (coat + arms): the arm skin's pose source (casts only if no g_shadowArms)
static USkeletalMeshComponent* ShadowCasterComp();
static USkeletalMeshComponent* g_holsterCast = nullptr;   // hidden, shadow-only body that stays in the HOLSTERED pose with weapons out; its forearms and hands are folded away, so it adds only the holstered shoulders + upper arms to the shadow (user 17:38: "keep the shoulder shadows from the holstered arm shadows always on")
static USkeletalMeshComponent* g_shadowArms = nullptr;   // hidden full body that is the SOLE shadow caster: same pose, but a weapon-side arm is posed to hold the weapon instead of shrunk (UpdateArmSkin shrinks g_shadowBody's so the visible skin arm hides)

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

// Async loading a package the engine's file cache doesn't know is a FATAL error: "Couldn't find file for package
// L_DLC07_Endgame_Script requested by async loading code" (user, 2026-10-11 19:39: Dishonored.exe started from a
// taskbar pin instead of through Steam - the DLC folders were never added, and every body/arms package is DLC).
// So ask the cache first (GPackageFileCache 0x13B6014, vtable slot 2 = FMapPackageFileCache::FindPackageFile
// 0x4940E0, checked against this exe); a package it can't find goes to the callback as a failed load instead.
static bool PackageAvailable(const wchar_t* package) {
    void* cache = *reinterpret_cast<void**>(0x013B6014);
    if (!cache || !package) return true;   // no cache yet: let the engine decide, as before
    struct RawString { wchar_t* data; int num, max; } out = { nullptr, 0, 0 };   // engine-allocated; left (a few bytes, rare)
    using FindPackageFile_t = unsigned int(__thiscall*)(void*, const wchar_t*, const void*, RawString*, const wchar_t*);
    auto find = reinterpret_cast<FindPackageFile_t>((*reinterpret_cast<void***>(cache))[2]);
    return find(cache, package, nullptr, &out, nullptr) != 0;
}

static void LoadPackageAsync(const FString& name, void(__cdecl* done)(UObject*, void*), void* user, const void* guid) {
    const wchar_t* pkg = *reinterpret_cast<const wchar_t* const*>(&name);   // FString: data pointer first (private in the SDK)
    if (!RawLoadPackageAsync || !PackageAvailable(pkg)) {
        Log("package %s not found by the engine - not loading it (DLC not mounted? start Dishonored through Steam)",
            pkg ? Narrow(pkg).c_str() : "?");
        done(nullptr, user);
        return;
    }
    RawLoadPackageAsync(name, done, user, guid);
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
    if (!RawLoadPackageAsync || g_sourceIndex >= static_cast<int>(sizeof(kSources) / sizeof(kSources[0]))) {
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
static OurArray g_ourArrays[1024];   // (256 until 18:1x: four bodies' trees now)
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
    if (g_ourArrayCount < 1024) g_ourArrays[g_ourArrayCount++] = { arr, data };
    else {
        static bool warned = false;
        if (!warned) { warned = true; Log("WARNING: array registry full (1024) - a buffer is untracked and a level change may crash"); }
    }
    return true;
}

// A body whose setup failed half way: empty the fields that still hold the buffers registered since 'mark' (the engine
// must never free our memory) and forget them, before the component is dropped (review 18:0x).
static void ForgetArraysSince(int mark) {
    if (mark < 0 || mark > g_ourArrayCount) return;
    for (int i = mark; i < g_ourArrayCount; ++i) {
        OurArray& a = g_ourArrays[i];
        if (a.field && a.field->Data == a.data) { a.field->Data = nullptr; a.field->Num = a.field->Max = 0; }
    }
    g_ourArrayCount = mark;
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
static void AddJumpHandControls(UAnimTree* tree, void* heads, int& count);

static void AddLegControls(UAnimTree* tree) {
    struct ListHead { int32_t nameIndex, nameNumber; USkelControlBase* head; int32_t drawY; };
    static_assert(sizeof(ListHead) == 16, "FSkelControlListHead is 16 bytes");
    ListHead heads[96] = {};   // legs, foot IK, coat, lean, arm IK, jump hands (fingers: 28)
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
    AddJumpHandControls(tree, heads, count);
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
static const char* kWristControlNames[2] = { "handHelperBone_L_jnt", "handHelperBone_R_jnt" };
static const char* kHelperFixNames[2] = { "handAttachment_L_jnt", "handAttachment_R_jnt" };   // ControlName of the helper-bone fix
// Corvo's coat cuffs (1P sleeve_X_jnt) ride the body's handAttachment_X bone, which a control places where the 1P sleeve
// bone sits: off the FOREARM (BCS_OtherBoneSpace lower_arm), (22,0,0), no rotation. ControlName sleeve_X (free name).
static const char* kCuffCarrierBones[2] = { "handAttachment_L_jnt", "handAttachment_R_jnt" };
static const char* kCuffControlNames[2] = { "sleeve_L_jnt", "sleeve_R_jnt" };
static const char* kLowerArmBones[2] = { "lower_arm_L_jnt", "lower_arm_R_jnt" };

// Jump hands (2026-10-10, user reference photo: both hands up, wrists cocked back, fingers curled like claws):
// additive bone-space bends on every finger and thumb joint. Finger joints run along local X and hinge on local Y
// (their Y axis lies along the knuckle line in the rest pose), like the forearms - so the curl is a pitch.
// [side][joint]: index/middle/ring/pinky 0-2, thumb 0-1.
static const char* kFingerBones[2][14] = {
    { "index_0_L_jnt", "index_1_L_jnt", "index_2_L_jnt", "middle_0_L_jnt", "middle_1_L_jnt", "middle_2_L_jnt",
      "ring_0_L_jnt", "ring_1_L_jnt", "ring_2_L_jnt", "pinky_0_L_jnt", "pinky_1_L_jnt", "pinky_2_L_jnt",
      "thumb_0_L_jnt", "thumb_1_L_jnt" },
    { "index_0_R_jnt", "index_1_R_jnt", "index_2_R_jnt", "middle_0_R_jnt", "middle_1_R_jnt", "middle_2_R_jnt",
      "ring_0_R_jnt", "ring_1_R_jnt", "ring_2_R_jnt", "pinky_0_R_jnt", "pinky_1_R_jnt", "pinky_2_R_jnt",
      "thumb_0_R_jnt", "thumb_1_R_jnt" } };

static const char* kUpperArmBones[2] = { "upper_arm_L_jnt", "upper_arm_R_jnt" };

static void AddJumpHandControls(UAnimTree* tree, void* heads, int& count) {
    struct ListHead { int32_t nameIndex, nameNumber; USkelControlBase* head; int32_t drawY; };
    auto* list = static_cast<ListHead*>(heads);
    int added = 0;
    for (const char* bone : kUpperArmBones) {   // jump hands: the whole arm is placed from the view
        FName name(bone);
        if (name.FNameEntryId < 0 || count >= 96) continue;
        auto* ctl = NewObject<USkelControlSingleBone>("Class Engine.SkelControlSingleBone", tree);
        if (!ctl) continue;
        ctl->ControlName = name;
        ctl->ControlStrength = ctl->StrengthTarget = 0.0f;
        ctl->bApplyRotation = 1;
        ctl->bAddRotation = 0;
        ctl->BoneRotationSpace = EBoneControlSpace::BCS_WorldSpace;
        list[count++] = { name.FNameEntryId, 0, ctl, 0 };
    }
    for (int side = 0; side < 2; ++side)
        for (const char* bone : kFingerBones[side]) {
            if (count >= 96) break;
            FName name(bone);
            if (name.FNameEntryId < 0) continue;
            auto* ctl = NewObject<USkelControlSingleBone>("Class Engine.SkelControlSingleBone", tree);
            if (!ctl) continue;
            ctl->ControlName = name;
            ctl->ControlStrength = ctl->StrengthTarget = 0.0f;
            ctl->bApplyRotation = 1;
            ctl->bAddRotation = 1;
            ctl->BoneRotationSpace = EBoneControlSpace::BCS_BoneSpace;
            list[count++] = { name.FNameEntryId, 0, ctl, 0 };
            ++added;
        }
    // The wrist twist bone (handHelperBone): the bodies' bind rolls it 24 deg against the 1P arms' - its verts (wrist,
    // Daud's wristbow) twisted. A control on that bone, named after handAttachment_X (FindSkelControl needs a free name).
    for (int side = 0; side < 2; ++side) {
        FName bone(kWristControlNames[side]), ctlName(kHelperFixNames[side]);
        if (bone.FNameEntryId < 0 || ctlName.FNameEntryId < 0 || count >= 96) continue;
        auto* ctl = NewObject<USkelControlSingleBone>("Class Engine.SkelControlSingleBone", tree);
        if (!ctl) continue;
        ctl->ControlName = ctlName;
        ctl->ControlStrength = ctl->StrengthTarget = 0.0f;
        list[count++] = { bone.FNameEntryId, 0, ctl, 0 };
        ++added;
    }
    for (int side = 0; side < 2; ++side) {   // the cuff carrier (see kCuffCarrierBones)
        FName bone(kCuffCarrierBones[side]), ctlName(kCuffControlNames[side]);
        if (bone.FNameEntryId < 0 || ctlName.FNameEntryId < 0 || count >= 96) continue;
        auto* ctl = NewObject<USkelControlSingleBone>("Class Engine.SkelControlSingleBone", tree);
        if (!ctl) continue;
        ctl->ControlName = ctlName;
        ctl->ControlStrength = ctl->StrengthTarget = 0.0f;
        list[count++] = { bone.FNameEntryId, 0, ctl, 0 };
        ++added;
    }
    Log("jump hands: %d finger controls", added);
}

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
        // Jump hands: the wrist bend runs after the IK on the same bone. Its ControlName is the (sibling)
        // handHelperBone's name so FindSkelControl can tell it from the IK; the list head decides the bone.
        FName wristName(kWristControlNames[i]);
        if (wristName.FNameEntryId >= 0) {
            if (auto* wrist = NewObject<USkelControlSingleBone>("Class Engine.SkelControlSingleBone", tree)) {
                wrist->ControlName = wristName;
                wrist->ControlStrength = wrist->StrengthTarget = 0.0f;
                wrist->bApplyRotation = 1;
                wrist->bAddRotation = 1;
                wrist->BoneRotationSpace = EBoneControlSpace::BCS_BoneSpace;
                ik->NextControl = wrist;
            }
        }
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
static const char* kSpineBones[3] = { "spine_1_jnt", "spine_2_jnt", "spine_0_jnt" };   // (spine_0: slide low recline only)

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

// Frame delta from the performance counter (seconds, clamped 0..0.1). GetTickCount steps every ~15.6 ms, so at 130 fps
// every other frame read dt = 0 and all the eases judder (slide get-up, user 01:20). Each caller keeps its own 'last'.
static float QpcDt(LONGLONG& last) {
    static LONGLONG freq = 0;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart ? f.QuadPart : 1; }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    float dt = last ? Clamp(static_cast<float>(static_cast<double>(c.QuadPart - last) / static_cast<double>(freq)), 0.0f, 0.1f) : 0.0f;
    last = c.QuadPart;
    return dt;
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
        // (slide work 00:35: the torso stays in slides - the camera is well above the chest now - so no slide clause, and
        // the deep-sink clause skips slides; the rising-sink clause still hides it for the get-up, as for crouch -> stand)
        bool slideNow = g_isSliding || g_slideAmount > 0.0f;
        // Strafing (20:12 clip, user: "the neck clipping in front of the bottom of the camera" moving right): the collar rim
        // round the collapsed neck bobs up into the bottom edge of the view with each sideways step. Facing forward the
        // torso is below the view anyway, so it goes (from the waist, as for looking up); looking down it stays.
        bool strafe = false;
        if (g_cfg.strafeHideTorsoSpeed > 0.0f && phys == 1 && !slideNow) {
            float yawR = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
            float lateral = -pawn->Velocity.X * sinf(yawR) + pawn->Velocity.Y * cosf(yawR);
            static bool was = false;
            strafe = fabsf(lateral) > g_cfg.strafeHideTorsoSpeed * (was ? 0.7f : 1.0f) && g_viewPitchDeg > g_cfg.strafeHideTorsoPitch - (was ? 5.0f : 0.0f);
            if (strafe != was) { was = strafe; Log("strafe: torso %s (sideways %.0f uu/s, pitch %.0f)", strafe ? "hidden" : "back", lateral, g_viewPitchDeg); }
        }
        // (10:45: a weapon slide keeps the game's low camera, which sat in the chest - torso hidden there, legs stay)
        bool want = takedown || hit || lookUp || strafe || g_swimAmount > 0.0f || g_climbAmount > 0.0f || (g_smoothedSink > g_cfg.deepCrouchSink && !slideNow) ||
                    (g_smoothedSink - g_crouchSink > 3.0f && g_slideLegW <= 0.0f) ||   // (not the slide get-up: the torso shows in slides now)
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
    static LONGLONG qpc1 = 0;
    float dt = QpcDt(qpc1);   // (frame-accurate; was GetTickCount steps)
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
    // Slide: the whole trunk lies back from the base of the spine as well (user 07:35: "stomach to chest lower to the
    // floor" - the spine_1/2 recline alone bent only the upper body back, the stomach stayed upright, log 07:26).
    int lowUnits = static_cast<int>(g_cfg.leanSign * g_slideAmount * g_cfg.slideReclineLow * 65536.0f / 360.0f);
    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
    for (USkeletalMeshComponent* comp : comps) {
        if (!comp) continue;
        for (const char* bone : kSpineBones) {
            auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(bone)));
            if (!ctl) continue;
            bool low = bone == kSpineBones[2];
            if (low) {
                ctl->BoneRotation = FRotator{ lowUnits, 0, 0 };
                ctl->BoneScale = 1.0f;
                ctl->ControlStrength = ctl->StrengthTarget = lowUnits ? 1.0f : 0.0f;
                continue;
            }
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

// 0..1 the slide bracing pose on a free hand's arm (the jump pose wins while airborne).
static float SlideArmAmount(int hand) {
    if (!g_cfg.slideArms) return 0.0f;
    bool busy = hand == 0 ? g_leftHandBusy : g_rightHandBusy;
    return busy ? 0.0f : g_slideAmount * (1.0f - g_jumpAmount);
}

struct Xf { float m[3][3]; float t[3]; };   // row i = axis i in world, t = origin
static Xf g_skinFrame = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { 0, 0, 0 } };   // UpdateArmSkin: shadow pose -> where the skin draws it
static bool g_skinFrameOk = false;                                                      // ...valid this frame
static bool BoneXf(USkeletalMeshComponent* comp, const char* bone, Xf& out);

// Jump hands, view-held: where each hand goes and how it should face. Fingers (bone X) forward, a little up and
// out; the bone Z the user wants away from the camera is jumpPalmSign x (forward/down/in) - with +1 the palms faced
// the camera, so the backs of the hands are bone -Z. Desired bone Y = Z x X.
struct JumpHandFrame { FVector target, X, Y, Z, F, R, U; };

static JumpHandFrame JumpHandAxes(int side, float yawOffDeg = 0.0f, bool bodyYaw = false) {
    auto comb = [](const FVector& a, float wa, const FVector& b, float wb, const FVector& c, float wc) {
        return FVector{ a.X * wa + b.X * wb + c.X * wc, a.Y * wa + b.Y * wb + c.Y * wc, a.Z * wa + b.Z * wb + c.Z * wc };
    };
    auto norm = [](FVector v) { float l = sqrtf(v.X * v.X + v.Y * v.Y + v.Z * v.Z); if (l > 1e-4f) { v.X /= l; v.Y /= l; v.Z /= l; } return v; };
    const float k = 6.2831853f / 65536.0f;
    // Level with the world, turned with the view's heading only (user: the arms keep their horizontal position while
    // jumping - looking down they stay where they are). Pitch is left out of the frame.
    float cp = 1.0f, sp = 0.0f;
    // (10:25 clip: looking straight down in a slide the view rotator's yaw/roll split swung 135 deg in a frame - the
    // slide arms belong to the body, so they take the pawn's heading (+ the turn lag), which is steady at any pitch)
    int yawUnits = (bodyYaw && g_bodyPawn) ? g_bodyPawn->Rotation.Yaw : g_viewRot.Yaw;
    float cy = cosf(yawUnits * k + yawOffDeg * 0.0174533f), sy = sinf(yawUnits * k + yawOffDeg * 0.0174533f);
    JumpHandFrame h;
    h.F = { cp * cy, cp * sy, sp }; h.R = { -sy, cy, 0.0f }; h.U = { -sp * cy, -sp * sy, cp };
    float sideSign = side == 0 ? -1.0f : 1.0f;
    float f = g_cfg.jumpHandFwd, r = sideSign * g_cfg.jumpHandSide + 0.8f * g_jumpSwayRoll;
    float u = g_cfg.jumpHandUp + 0.6f * g_jumpSwayRaise - (side == 0 ? g_cfg.jumpLeftHandDown : 0.0f);
    FVector off = comb(h.F, f, h.R, r, h.U, u);
    h.target = { g_viewLoc.X + off.X, g_viewLoc.Y + off.Y, g_viewLoc.Z + off.Z };
    // User photo (22:5x, own reference): forearms in from the sides almost level, hands just below eye level,
    // fingers pointing in toward each other and a little forward, backs of the hands up. Bone Z is the back-of-hand
    // side (22:35: palm sign -1 showed the backs), so Z ~ up and slightly toward the camera.
    // (2026-10-11, user: like the arms in Mirror's Edge - fingers more forward, less in toward each other)
    h.X = norm(comb(h.F, 0.85f, h.U, 0.1f, h.R, -0.45f * sideSign));
    float ps = -g_cfg.jumpPalmSign;   // +1 with the default -1
    FVector P = comb(h.F, -0.2f * ps, h.U, 1.0f * ps, h.R, 0.0f);
    float d = P.X * h.X.X + P.Y * h.X.Y + P.Z * h.X.Z;
    h.Z = norm(FVector{ P.X - h.X.X * d, P.Y - h.X.Y * d, P.Z - h.X.Z * d });
    h.Y = FVector{ h.Z.Y * h.X.Z - h.Z.Z * h.X.Y, h.Z.Z * h.X.X - h.Z.X * h.X.Z, h.Z.X * h.X.Y - h.Z.Y * h.X.X };
    return h;
}

// Jump hands: sway from the pawn's motion (eased) and the wrist / finger bends on both bodies.
static void UpdateJumpSway(ADishonoredPlayerPawn* pawn) {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    static LONGLONG qpc2 = 0;
    float dt = QpcDt(qpc2);   // (frame-accurate; was GetTickCount steps)
    last = now;
    static int lastYaw = 0;
    static float phase = 0.0f;
    float raise = 0.0f, roll = 0.0f;
    if (pawn && g_cfg.jumpHands && g_jumpAmount > 0.0f) {
        const FVector& v = pawn->Velocity;
        float yawR = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
        float lateral = -v.X * sinf(yawR) + v.Y * cosf(yawR);              // + = moving right
        int dyaw = static_cast<int16_t>((pawn->Rotation.Yaw - lastYaw) & 0xFFFF);
        float yawRate = dt > 0.0f ? dyaw * (360.0f / 65536.0f) / dt : 0.0f; // deg/s, + = turning right
        phase += dt * 2.3f;
        raise = Clamp(-v.Z * 0.015f, -12.0f, 20.0f) + 3.0f * sinf(phase);  // falling: arms lift
        roll = Clamp(-lateral * 0.02f, -12.0f, 12.0f) + Clamp(-yawRate * 0.03f, -10.0f, 10.0f);
        raise *= g_cfg.jumpSway;
        roll *= g_cfg.jumpSway;
    }
    if (pawn) lastYaw = pawn->Rotation.Yaw;
    float k = Clamp(dt * 6.0f, 0.0f, 1.0f);
    g_jumpSwayRaise += (raise - g_jumpSwayRaise) * k;
    g_jumpSwayRoll += (roll - g_jumpSwayRoll) * k;
    g_jumpFlex = sinf(phase * 1.3f);
}

// Reference-pose local rotation of a bone (quat x, y, z, w) from SkeletalMesh.RefSkeleton (TArray<FMeshBone>, native:
// FName, flags, then VJointPos = quat orientation + position). The element size isn't in the SDK, so it is found
// once by checking which stride puts two known bones' names where MatchRefBone says they are.
static int g_refStride = 0, g_refNameOff = 0, g_refQuatOff = 12;   // FMeshBone layout (found once, RefLocalQuat)

static bool RefLocalQuat(USkeletalMeshComponent* comp, const char* bone, float q[4]) {
    if (!comp || !comp->SkeletalMesh) return false;
    int& stride = g_refStride;
    auto* ref = reinterpret_cast<RawArray*>(&comp->SkeletalMesh->RefSkeleton);
    if (!ref->Data || ref->Num <= 0) return false;
    int& nameOff = g_refNameOff;
    int& quatOff = g_refQuatOff;
    if (!stride) {
        // Find the layout from the data: where two known bones' names sit (their indices from MatchRefBone) gives
        // the element size and the name's offset (23:06 log: none of the guessed sizes 40-64 matched).
        FName a("index_1_L_jnt"), b("pinky_2_R_jnt");
        int ia = comp->MatchRefBone(a), ib = comp->MatchRefBone(b);
        if (ia < 0 || ib < 0 || ia == ib) return false;
        auto* base = static_cast<uint8_t*>(ref->Data);
        // (16:16 log: 'element 0 bytes' - the old scan read a fixed Num*160 block, which wasn't all readable.) Try each
        // element size: bone ia's name at ia*st+off and bone ib's at ib*st+off, checking each read is safe first.
        auto at = [&](size_t o, int32_t& v) {
            if (IsBadReadPtr(base + o, 8)) return false;
            v = *reinterpret_cast<int32_t*>(base + o);
            return true;
        };
        for (int st = 24; st <= 200 && !stride; st += 4)
            for (int off = 0; off < st && off <= 16; off += 4) {
                int32_t va = 0, vb = 0;
                if (!at(static_cast<size_t>(ia) * st + off, va) || va != a.FNameEntryId) continue;
                if (!at(static_cast<size_t>(ib) * st + off, vb) || vb != b.FNameEntryId) continue;
                stride = st; nameOff = off; break;
            }
        if (stride) {   // the orientation quat: the first spot after the name where both bones hold unit quats
            auto unitAt = [&](int bone, int off) {
                const float* f = reinterpret_cast<const float*>(base + bone * stride + nameOff + off);
                float n = f[0] * f[0] + f[1] * f[1] + f[2] * f[2] + f[3] * f[3];
                return n > 0.98f && n < 1.02f;
            };
            quatOff = 0;
            for (int off : { 12, 8, 16, 20, 24 })
                if (off + 16 <= stride - nameOff && unitAt(ia, off) && unitAt(ib, off)) { quatOff = off; break; }
            if (!quatOff) stride = 0;
        }
        Log("jump hands: reference skeleton %d bones, element %d bytes, name at +%d, quat at +%d (bones %d/%d, names %d/%d)",
            ref->Num, stride, nameOff, quatOff, ia, ib, a.FNameEntryId, b.FNameEntryId);
        if (!stride && !IsBadReadPtr(base, 256)) {   // layout unknown: dump the start of the array for the next fix
            const int32_t* w = reinterpret_cast<const int32_t*>(base);
            Log("jump hands: RefSkeleton[0..] %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", w[0], w[1],
                w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15], w[16], w[17],
                w[18], w[19], w[20], w[21], w[22], w[23]);
        }
        if (!stride) stride = -1;
    }
    if (stride <= 0) return false;
    FName name(bone);
    int i = comp->MatchRefBone(name);
    if (i < 0 || i >= ref->Num) return false;
    const float* o = reinterpret_cast<const float*>(static_cast<uint8_t*>(ref->Data) + i * stride + nameOff + quatOff);
    for (int k = 0; k < 4; ++k) q[k] = o[k];
    return true;
}

// Reference-pose local position of a bone (VJointPos.Position, right after the orientation quat).
static bool RefLocalPos(USkeletalMeshComponent* comp, const char* bone, float pos[3]) {
    float q[4];
    if (!RefLocalQuat(comp, bone, q)) return false;   // also finds the layout
    auto* ref = reinterpret_cast<RawArray*>(&comp->SkeletalMesh->RefSkeleton);
    int i = comp->MatchRefBone(FName(bone));
    if (i < 0 || i >= ref->Num || g_refStride <= 0 || g_refQuatOff + 28 > g_refStride - g_refNameOff) return false;
    const float* o = reinterpret_cast<const float*>(static_cast<uint8_t*>(ref->Data) + i * g_refStride + g_refNameOff + g_refQuatOff + 16);
    for (int k = 0; k < 3; ++k) pos[k] = o[k];
    return true;
}

static void QuatMul(const float a[4], const float b[4], float out[4]) {   // a then b in the bone's frame: a * b
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}

static FRotator AxesToRotator(const FVector& X, const FVector& Y, const FVector& Z);

static FRotator QuatToRotator(const float q[4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    FVector X{ 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y) };
    FVector Y{ 2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x) };
    FVector Z{ 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y) };
    return AxesToRotator(X, Y, Z);
}

static FRotator AxesToRotator(const FVector& X, const FVector& Y, const FVector& Z) {   // FMatrix::Rotator
    auto dot = [](const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
    float pitch = atan2f(X.Z, sqrtf(X.X * X.X + X.Y * X.Y)), yaw = atan2f(X.Y, X.X);
    FVector sy0{ -sinf(yaw), cosf(yaw), 0.0f };   // Y of (pitch, yaw, 0)
    float roll = atan2f(dot(Z, sy0), dot(Y, sy0));
    const float toU = 32768.0f / 3.14159265f;
    return FRotator{ static_cast<int>(pitch * toU), static_cast<int>(yaw * toU), static_cast<int>(roll * toU) };
}

// Jump hands on the visible body, whole arm (22:53 clip: the hands twisted as the view pitched - the game's limb
// solver re-derives each forearm's roll every frame, and flipping its joint axis by measurement just oscillated).
// Placed by hand instead, from the view: the hand at a fixed spot and orientation in view space; the elbow found
// analytically (two-bone, bone lengths measured from the mesh) in the plane through shoulder and hand whose normal is
// the hand's Y, on the down/out side; then upper arm, forearm and hand get absolute world rotations that all share
// that Y - nothing twists, and the pose stays the same however the camera turns.
static bool FlatHandAxesLocal(int side, FVector& fingers, FVector& palm, FVector* thumbSide = nullptr);   // (defined with the hand bind, below)
static void PlaceJumpArm(USkeletalMeshComponent* comp, int side, float amount, float slideW = 0.0f) {
    auto* upperCtl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kUpperArmBones[side])));
    auto* lowerCtl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(side == 0 ? "lower_arm_L_jnt" : "lower_arm_R_jnt")));
    auto* handCtl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kWristControlNames[side])));
    auto* ik = static_cast<USkelControlLimb*>(comp->FindSkelControl(FName(kHandBones[side])));
    if (ik) { ik->ControlStrength = ik->StrengthTarget = 0.0f; ik->bInvertJointAxis = 0; }
    float w = g_cfg.jumpHandsInView ? amount : 0.0f;
    // Slide (00:35): the same construction with the slide's elbow / forearm directions; blended by weight if both apply.
    float sl = (w + slideW > 0.0f) ? slideW / (w + slideW) : 0.0f;
    if (slideW > w) w = slideW;
    if (upperCtl) upperCtl->ControlStrength = upperCtl->StrengthTarget = 0.0f;
    if (w <= 0.0f || !upperCtl || !lowerCtl || !handCtl) return;

    auto sub = [](const FVector& a, const FVector& b) { return FVector{ a.X - b.X, a.Y - b.Y, a.Z - b.Z }; };
    auto add = [](const FVector& a, const FVector& b) { return FVector{ a.X + b.X, a.Y + b.Y, a.Z + b.Z }; };
    auto mul = [](const FVector& a, float k) { return FVector{ a.X * k, a.Y * k, a.Z * k }; };
    auto dot = [](const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
    auto cross = [](const FVector& a, const FVector& b) { return FVector{ a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X }; };
    auto len = [&](const FVector& a) { return sqrtf(dot(a, a)); };
    auto norm = [&](const FVector& a) { float l = len(a); return l > 1e-4f ? mul(a, 1.0f / l) : a; };
    auto orth = [&](const FVector& v, const FVector& x) { return norm(sub(v, mul(x, dot(v, x)))); };

    JumpHandFrame h = JumpHandAxes(side, g_slideYawOffDeg * sl, sl > 0.5f);   // (slide: the arms belong to the lagging body - its heading, not the view's)
    FVector S = comp->GetBoneLocation(FName(kUpperArmBones[side]), 0);
    FVector Ecur = comp->GetBoneLocation(FName(side == 0 ? "lower_arm_L_jnt" : "lower_arm_R_jnt"), 0);
    FVector Hcur = comp->GetBoneLocation(FName(kHandBones[side]), 0);
    float L1 = len(sub(Ecur, S)), L2 = len(sub(Hcur, Ecur));
    if (L1 < 5.0f || L2 < 5.0f) return;

    FVector d = sub(h.target, S);
    float D = len(d);
    if (D < 1e-3f) return;
    FVector dir = mul(d, 1.0f / D);

    float sideSign = side == 0 ? -1.0f : 1.0f;
    // Pose (user, 2026-10-11, clip 16:08): upper arms straight out to the sides, level - almost a T-pose - and the
    // forearms bent 90 degrees at the elbows, pointing forward the way the camera faces. Built directly from the
    // shoulder with the arm's own bone lengths; the hand ends wherever the forearm does. (The per-arm yaw / in / lift
    // offsets of earlier requests don't apply to this pose.)
    (void)dir;
    FVector eJ = norm(add(mul(h.R, sideSign), mul(h.F, g_cfg.jumpElbowForward)));
    FVector fJ = h.F;
    const float* se = side == 0 ? g_cfg.slideElbowL : g_cfg.slideElbowR;
    const float* sf = side == 0 ? g_cfg.slideForearmL : g_cfg.slideForearmR;
    FVector eS = norm(add(add(mul(h.F, se[0]), mul(h.R, sideSign * se[1])), mul(h.U, se[2])));
    FVector fS = norm(add(add(mul(h.F, sf[0]), mul(h.R, sideSign * sf[1])), mul(h.U, sf[2])));
    FVector E = add(S, mul(norm(add(mul(eJ, 1.0f - sl), mul(eS, sl))), L1));
    FVector T = add(E, mul(norm(add(mul(fJ, 1.0f - sl), mul(fS, sl))), L2));
    dir = norm(sub(T, S));
    // Roll: the elbow's own bend axis (cross of upper arm and forearm), signed as the earlier rule had it
    // (JumpArmRollSign -1 -> Y = upper x fore), so the elbows fold naturally.
    FVector bend = cross(norm(sub(E, S)), norm(sub(T, E)));
    FVector Yarm = dot(bend, bend) > 1e-4f ? mul(norm(bend), -g_cfg.jumpArmRollSign) : mul(h.U, 1.0f);
    FVector Xu = norm(sub(E, S)), Xl = norm(sub(T, E));
    FVector Yu = orth(Yarm, Xu), Yl = orth(Yarm, Xl);
    // Straight wrists (user): the hand carries on along the forearm. Raised elbows tilt the forearm's roll, so the
    // hand turns part of the way (half) toward backs-up (bone Z = back of the hand) - a small turn, no bend.
    FVector Xh = Xl, Yh = Yl;
    {
        FVector Ydown = norm(cross(h.Z, Xh));   // Y that would put bone Z (back of the hand) up
        if (dot(Ydown, Yl) > 0.0f) Yh = orth(add(mul(Yl, 0.5f), mul(Ydown, 0.5f)), Xh);
    }
    // Slide, left hand (user 08:20): the open hand flat on the ground - fingers forward (a little out), back of the
    // hand (bone Z) up, so the wrist bends 90 degrees off the vertical forearm.
    // (09:50, user: "see the back of the hand as if flat on the floor, balancing") The hand bone's own axes are not
    // guessed any more: its finger direction and palm normal come from the 1P hand's bind pose (FlatHandAxesLocal),
    // and the rotation maps fingers -> forward (a little out), palm -> down.
    FVector fL, pL, tL;
    if (sl > 0.5f && side == 0 && g_cfg.slideLeftHandFlat && FlatHandAxesLocal(side, fL, pL, &tL)) {
        FVector Fw = norm(add(mul(h.F, 0.9f), mul(h.R, sideSign * 0.3f)));
        FVector Pw = orth(mul(h.U, -g_cfg.slideLeftHandPalmSign), Fw);   // palm down (PalmSign 1); -1 flips if the bind reads wrong
        FVector Cw = cross(Fw, Pw);
        pL = orth(pL, fL);
        FVector cL = cross(fL, pL);
        // (10:48 clip: the thumb tip sank into the floor) Roll the hand about its fingers so the thumb edge lifts a
        // little and it rests more on its outer edge. The thumb side is the bind's index-minus-pinky direction mapped
        // through the same rotation. Lifting that edge turns the hand's UP normal away from it (a plate lifted on its
        // right edge faces up-left) - and Pw here is whichever bind side maps up (PalmSign -1 on this rig: it IS the up
        // normal; 10:58 "clipping even more": the first cut tilted it the other way, as if it pointed down).
        if (g_cfg.slideLeftHandThumbUpDeg != 0.0f) {
            FVector tW = norm(add(add(mul(Fw, dot(fL, tL)), mul(Pw, dot(pL, tL))), mul(Cw, dot(cL, tL))));
            float a = g_cfg.slideLeftHandThumbUpDeg * 0.0174533f;
            float up = dot(Pw, h.U) >= 0.0f ? 1.0f : -1.0f;   // +1: Pw is the up normal (tilt away from the thumb); -1: the down normal (toward it)
            Pw = orth(add(mul(Pw, cosf(a)), mul(tW, -up * sinf(a))), Fw);
            Cw = cross(Fw, Pw);
        }
        // R = Fw (x) fL + Pw (x) pL + Cw (x) cL maps the local basis to the world one; bone X/Y in world = R e_x / R e_y
        Xh = norm(add(add(mul(Fw, fL.X), mul(Pw, pL.X)), mul(Cw, cL.X)));
        Yh = orth(add(add(mul(Fw, fL.Y), mul(Pw, pL.Y)), mul(Cw, cL.Y)), Xh);
    }
    // Per-arm forearm roll (user: right arm turned so the thumb side of the hand faces the camera): forearm and hand
    // turn together about their own length (Y -> Z), so the wrist doesn't twist. Bone -Y is the thumb side (rest pose).
    if (g_cfg.jumpForearmRollDeg[side] != 0.0f) {
        float a = g_cfg.jumpForearmRollDeg[side] * 0.0174533f, c = cosf(a), sn = sinf(a);
        Yl = norm(add(mul(Yl, c), mul(cross(Xl, Yl), sn)));
        Yh = norm(add(mul(Yh, c), mul(cross(Xh, Yh), sn)));
    }
    // Hands turned outward a little at the wrist, about the vertical (U x F = R: + turns right).
    if (g_cfg.jumpHandOutDeg != 0.0f) {
        float a = sideSign * g_cfg.jumpHandOutDeg * 0.0174533f, c = cosf(a), sn = sinf(a);
        auto turn = [&](const FVector& v) { return add(add(mul(v, c), mul(cross(h.U, v), sn)), mul(h.U, dot(h.U, v) * (1.0f - c))); };
        Xh = norm(turn(Xh));
        Yh = orth(turn(Yh), Xh);
    }
    upperCtl->BoneRotationSpace = lowerCtl->BoneRotationSpace = handCtl->BoneRotationSpace = EBoneControlSpace::BCS_WorldSpace;
    upperCtl->bAddRotation = lowerCtl->bAddRotation = handCtl->bAddRotation = 0;
    upperCtl->bApplyRotation = lowerCtl->bApplyRotation = handCtl->bApplyRotation = 1;
    upperCtl->BoneScale = lowerCtl->BoneScale = 1.0f;
    upperCtl->BoneRotation = AxesToRotator(Xu, Yu, cross(Xu, Yu));
    lowerCtl->BoneRotation = AxesToRotator(Xl, Yl, cross(Xl, Yl));
    handCtl->BoneRotation = AxesToRotator(Xh, Yh, cross(Xh, Yh));
    upperCtl->ControlStrength = upperCtl->StrengthTarget = w;
    lowerCtl->ControlStrength = lowerCtl->StrengthTarget = w;
    handCtl->ControlStrength = handCtl->StrengthTarget = w;
}

// While the jump pose holds a free hand up, the game's own first-person arm for that side is hidden (16:49 clip: its
// holstered jump animation brought a third, dark 1P right hand into view under ours). Back when the pose ends.
// Sprinting too (19:58 clip + log: the model put the body's left hand off the bottom-left, but a glove sat over the
// legs near the middle, fixed on screen - the game's holstered sprint animation brings its own 1P left hand up there;
// with the arm skin it looks just like ours. That was the "both arms shifted right" and the left arm through the
// camera). Only on the ground (PHYS_Walking), not climbing - mantles, ladders, pickups and doors keep their 1P hands.
static void HideFirstPersonJumpHands(ADishonoredPlayerPawn* pawn) {
    static USkeletalMeshComponent* arms = nullptr;
    static int hid[2] = { 0, 0 };
    USkeletalMeshComponent* now = pawn ? pawn->Mesh : nullptr;
    if (now != arms) { arms = now; hid[0] = hid[1] = 0; }
    if (!arms) return;
    static DWORD busySince[2] = { 0, 0 };   // (17:10: after holstering the sword stayed out - hidden mid-holster)
    bool busyNow[2] = { g_leftHandBusy, g_rightHandBusy };
    for (int side = 0; side < 2; ++side) if (busyNow[side]) busySince[side] = GetTickCount();
    for (int side = 0; side < 2; ++side) {
        bool settled = GetTickCount() - busySince[side] > 1000;
        bool jumping = g_cfg.jumpHands && g_cfg.jumpHandsInView && JumpArmAmount(side) > 0.05f;
        FVector v = pawn ? pawn->Velocity : FVector{ 0, 0, 0 };
        bool fast = g_sprintBackAmount > 0.3f || v.X * v.X + v.Y * v.Y > 450.0f * 450.0f;
        bool sprinting = g_cfg.hideFirstPersonSprintHands && g_armSkinOn && !busyNow[side] && pawn &&
                         static_cast<int>(pawn->Physics) == 1 && !g_isClimbing && fast;
        bool want = (jumping || sprinting) && g_bodyVisible && settled;
        // The whole 1P arm from the shoulder (20:12 clip: sprinting, a full sleeve + forearm + glove still swung up to
        // the top of the view - Daud's 1P arms have sleeves to the shoulder; hiding from the elbow left them). Applied
        // again every 0.25 s while wanted, in case the game resets bone visibility.
        // (21:55: no bone calls on the game's 1P arms any more. HideBone never hides in this game, and UnHideBone 0x7b4250
        // calls PhysicsAssetInstance::EnableCollisionBodiesBelow(true, bone) - it switched the 1P arm's collision on after
        // every sprint/jump. Only the state is tracked and logged.)
        if ((want ? 1 : 0) == hid[side]) continue;
        // NOTE (20:50, retail disassembly): Dishonored's USkeletalMeshComponent::HideBone (0x7bcdc0) returns at once for
        // PBO_None and otherwise only terms/disables the bone's physics body - it never hides anything visually. So
        // these calls have no effect; kept (logged as "requested") until a working 1P hide is needed.
        Log("1P %s arm hide %s (%s) - no visual effect in this game", side ? "right" : "left", want ? "requested" : "released",
            want ? (jumping ? "jump" : "sprint") : "back");
        hid[side] = want ? 1 : 0;
    }
}

// Left hand "holding a power" pose (user, 2026-10-11: when jumping, the left hand should look like it does holding a
// selected power). Taken from the game's own first-person arms while a power is held in the left hand, 1 s into the
// hold (the draw has settled): each left finger/thumb joint's rotation relative to its parent bone, from the 1P
// component-space pose (rows = axes, local = child x parent^T). The jump hands then set those outright on the
// left hand. Built in from the 2026-10-11 17:31 capture (log, Skm_Player), so it holds from the first jump; a new
// capture (each time a power is held) replaces it.
static FRotator g_powerHandRot[14] = {
    { 1548, -5751, 21508 }, { -5430, 0, 0 }, { -646, 0, 0 },     { 2614, -7714, 19587 }, { -6836, 0, 0 },
    { -2571, 0, 0 },        { 3368, -10155, 18581 }, { -9509, 0, 0 }, { -343, 0, 0 },   { 3468, -12242, 17414 },
    { -10889, 0, 0 },       { -6351, 0, 0 },  { -5731, -9320, -30206 }, { -6323, 0, 0 } };
static bool g_powerHandHave = true;
static unsigned g_powerHandVer = 1;   // bumped on each capture (the mirrored right hand is rebuilt from it)

// The right hand's version of the left power pose (user, 22:02: both hands in the power-holding pose). The rig is mirrored
// across the body: hand frames are reflected (Y), finger frames are reflected and turned 180 deg about Y (child offsets
// -X/-Z on the right). In quaternions, with P(q) = (-x, y, -z, w) and F = 180 deg about Y: a bone hanging off the hand
// (knuckles, thumb_0) maps as F*P(q) or P(q)*F^-1 depending on the storage convention, a bone off a finger bone as
// F*P(q)*F^-1. Checked on the recon psk (index_0, middle_0, thumb_0, index_1); the convention is picked at runtime from
// the body's own reference skeleton (index_0_L -> index_0_R).
static FRotator g_powerHandRotR[14];
static unsigned g_powerHandRVer = 0;
static int g_powerMirrorMode = 0;     // 0 unknown, 1 F*P(q), 2 P(q)*F^-1, -1 no match (right hand keeps the open pose)

static void RotatorToQuat(const FRotator& r, float q[4]) {   // inverse of QuatToRotator (rows = axes)
    const float k = 6.2831853f / 65536.0f;
    float sp = sinf(r.Pitch * k), cp = cosf(r.Pitch * k), sy = sinf(r.Yaw * k), cy = cosf(r.Yaw * k);
    float sr = sinf(r.Roll * k), cr = cosf(r.Roll * k);
    float M[3][3] = { { cp * cy, cp * sy, sp },
                      { sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp },
                      { -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp } };
    // column-convention matrix R[i][j] = M[j][i]
    float R00 = M[0][0], R11 = M[1][1], R22 = M[2][2];
    float R21 = M[1][2], R12 = M[2][1], R02 = M[2][0], R20 = M[0][2], R10 = M[0][1], R01 = M[1][0];
    float tr = R00 + R11 + R22;
    if (tr > 0.0f) {
        float t = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * t; q[0] = (R21 - R12) / t; q[1] = (R02 - R20) / t; q[2] = (R10 - R01) / t;
    } else if (R00 > R11 && R00 > R22) {
        float t = sqrtf(1.0f + R00 - R11 - R22) * 2.0f;
        q[3] = (R21 - R12) / t; q[0] = 0.25f * t; q[1] = (R01 + R10) / t; q[2] = (R02 + R20) / t;
    } else if (R11 > R22) {
        float t = sqrtf(1.0f + R11 - R00 - R22) * 2.0f;
        q[3] = (R02 - R20) / t; q[0] = (R01 + R10) / t; q[1] = 0.25f * t; q[2] = (R12 + R21) / t;
    } else {
        float t = sqrtf(1.0f + R22 - R00 - R11) * 2.0f;
        q[3] = (R10 - R01) / t; q[0] = (R02 + R20) / t; q[1] = (R12 + R21) / t; q[2] = 0.25f * t;
    }
}

static void QuatMul(const float a[4], const float b[4], float out[4]);
static FRotator QuatToRotator(const float q[4]);
static bool RefLocalQuat(USkeletalMeshComponent* comp, const char* bone, float q[4]);

static void MirrorFingerQuat(const float in[4], bool offHand, int mode, float out[4]) {
    static const float F[4] = { 0, 1, 0, 0 }, Fi[4] = { 0, -1, 0, 0 };
    float pq[4] = { -in[0], in[1], -in[2], in[3] }, t[4];
    if (!offHand) { QuatMul(F, pq, t); QuatMul(t, Fi, out); return; }
    if (mode == 1) QuatMul(F, pq, out);
    else QuatMul(pq, Fi, out);
}

static const FRotator* PowerHandRight(USkeletalMeshComponent* comp) {
    if (!g_powerHandHave) return nullptr;
    if (g_powerMirrorMode == 0 && comp) {
        float qL[4], qR[4], c[4];
        if (RefLocalQuat(comp, "index_0_L_jnt", qL) && RefLocalQuat(comp, "index_0_R_jnt", qR)) {
            float best = 0.0f;
            for (int mode = 1; mode <= 2; ++mode) {
                MirrorFingerQuat(qL, true, mode, c);
                float d = fabsf(c[0] * qR[0] + c[1] * qR[1] + c[2] * qR[2] + c[3] * qR[3]);
                if (d > best) { best = d; if (d > 0.99f) g_powerMirrorMode = mode; }
            }
            if (!g_powerMirrorMode) g_powerMirrorMode = -1;
            float pL[4], pR[4];   // phalanx check
            bool ph = RefLocalQuat(comp, "index_1_L_jnt", pL) && RefLocalQuat(comp, "index_1_R_jnt", pR);
            float pd = 0.0f;
            if (ph) { MirrorFingerQuat(pL, false, 1, c); pd = fabsf(c[0] * pR[0] + c[1] * pR[1] + c[2] * pR[2] + c[3] * pR[3]); }
            Log("power hand: right-hand mirror %s (knuckle match %.3f, phalanx match %.3f)",
                g_powerMirrorMode > 0 ? (g_powerMirrorMode == 1 ? "F*P(q)" : "P(q)*F^-1") : "NOT FOUND - right hand keeps the open pose", best, pd);
        }
    }
    if (g_powerMirrorMode <= 0) return nullptr;
    if (g_powerHandRVer != g_powerHandVer) {
        for (int j = 0; j < 14; ++j) {
            float q[4], m[4];
            RotatorToQuat(g_powerHandRot[j], q);
            bool offHand = (j < 12 && j % 3 == 0) || j == 12;   // index/middle/ring/pinky_0 and thumb_0 hang off the hand
            MirrorFingerQuat(q, offHand, g_powerMirrorMode, m);
            g_powerHandRotR[j] = QuatToRotator(m);
        }
        g_powerHandRVer = g_powerHandVer;
    }
    return g_powerHandRotR;
}

// Relaxed hand for standing/walking (user 22:18: the power pose there "looked very tense and a bit crippled"; fine in
// jumps and sprints). The body's own bind pose is a relaxed hand (phalanges bent 12-22 deg, thumb tip 30): every joint
// keeps it and curls a little more about its hinge (local Y), more toward the pinky - fingers stay together, loosely
// curled. Same curl sign on both hands (mirrored rig).
// 22:25 (user: the backs of the hands looked dipped in / concave): the _0 bones are the metacarpals - they run through
// the back of the hand to the knuckle - and they were curled, and the rest pose came from the BODY's bind while the hand
// drawn is the 1P arms mesh. The drawn finger stays undistorted only when its local rotation is the 1P mesh's own bind
// (that's why the captured power pose looks right). So: rest = the arm skin mesh's bind; the metacarpals keep it; the
// knuckle (_1) and middle joint (_2) curl.
static bool RelaxedFingerQuat(USkeletalMeshComponent* comp, int side, int j, float out[4]) {
    static const float kShare[3] = { 0.0f, 1.0f, 1.3f };   // metacarpal (_0, back of the hand), knuckle (_1), middle joint (_2)
    float rest[4];
    USkeletalMeshComponent* bindSrc = (g_armSkin && g_armSkin->SkeletalMesh) ? g_armSkin : comp;
    if (!RefLocalQuat(bindSrc, kFingerBones[side][j], rest) && !RefLocalQuat(comp, kFingerBones[side][j], rest)) return false;
    float deg = j < 12 ? g_cfg.handRelaxCurl[j / 3] * kShare[j % 3] : (j == 12 ? g_cfg.handRelaxThumb0 : g_cfg.handRelaxThumb1);
    float sign = side == 0 ? g_cfg.jumpCurlSignL : g_cfg.jumpCurlSignR;
    float h = sign * deg * 0.0087266f;   // half angle, radians
    float cu[4] = { 0.0f, sinf(h), 0.0f, cosf(h) };
    QuatMul(rest, cu, out);
    return true;
}

// The 1P arms' (arm skin mesh's) own hand skeleton, cached per mesh: [side][0..13] the finger bones, [side][14] the
// handHelperBone. The bodies' hand bones sit elsewhere (22:35, recon psk: metacarpals 4-5 uu and 10-28 deg off, finger
// bones 1-3 uu shorter, helper rolled 24 deg) - the 1P hand drawn on them was stretched: the backs caved in.
struct HandBind { USkeletalMesh* mesh = nullptr; bool ok[2][16]; float q[2][16][4]; float p[2][16][3]; };   // [15] sleeve_X
static HandBind g_handBind;
static const HandBind* SkinHandBind() {
    if (!g_armSkin || !g_armSkin->SkeletalMesh) return nullptr;
    if (g_handBind.mesh != g_armSkin->SkeletalMesh) {
        g_handBind.mesh = g_armSkin->SkeletalMesh;
        int good = 0;
        for (int side = 0; side < 2; ++side)
            for (int j = 0; j < 16; ++j) {
                const char* bone = j < 14 ? kFingerBones[side][j] : j == 14 ? kWristControlNames[side] : kCuffControlNames[side];
                float* q = g_handBind.q[side][j];
                float* pp = g_handBind.p[side][j];
                bool ok = RefLocalQuat(g_armSkin, bone, q) && RefLocalPos(g_armSkin, bone, pp);
                float len = ok ? sqrtf(pp[0] * pp[0] + pp[1] * pp[1] + pp[2] * pp[2]) : 0.0f;
                float qn = ok ? q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] : 0.0f;
                g_handBind.ok[side][j] = ok && len < 40.0f && qn > 0.98f && qn < 1.02f;   // (garbage guard)
                if (g_handBind.ok[side][j]) ++good;
            }
        const float* a = g_handBind.p[0][0];
        const float* b = g_handBind.p[0][9];
        const float* c = g_handBind.p[0][15];
        Log("hand bind: %s - %d/32 hand bones read; index_0_L at (%.2f %.2f %.2f), pinky_0_L at (%.2f %.2f %.2f), sleeve_L %s (%.2f %.2f %.2f)",
            g_armSkin->SkeletalMesh->GetName().c_str(), good, a[0], a[1], a[2], b[0], b[1], b[2],
            g_handBind.ok[0][15] ? "at" : "not found", c[0], c[1], c[2]);
    }
    return &g_handBind;
}

// rest (a bind local rotation) x curl about the hinge (local Y). The finger bones are the three segments: _0 starts AT
// the knuckle (~10 uu out from the wrist; the back of the hand is the hand bone), _1 the middle segment, _2 the tip
// (22:52: corrected - _0 had been taken for a metacarpal and never bent, so the fist's knuckles stayed straight).
// share = { knuckle _0, middle _1, tip _2 } x curl[finger]; thumb root / tip their own.
// The hand bone's finger direction and palm normal in its own frame, from the 1P hand's bind pose: fingers = toward the
// mean of the four knuckle joints; the plane through index and pinky knuckles gives the normal; the thumb joints sit
// on the palm side of that plane, which picks the sign. Logged once per mesh.
static bool FlatHandAxesLocal(int side, FVector& fingers, FVector& palm, FVector* thumbSide) {
    const HandBind* hb = SkinHandBind();
    if (!hb) return false;
    const int kn[4] = { 0, 3, 6, 9 };   // index_0, middle_0, ring_0, pinky_0
    for (int k = 0; k < 4; ++k) if (!hb->ok[side][kn[k]]) return false;
    if (!hb->ok[side][12] && !hb->ok[side][13]) return false;
    auto P = [&](int j) { return FVector{ hb->p[side][j][0], hb->p[side][j][1], hb->p[side][j][2] }; };
    auto sub = [](const FVector& a, const FVector& b) { return FVector{ a.X - b.X, a.Y - b.Y, a.Z - b.Z }; };
    auto dot = [](const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
    auto cross = [](const FVector& a, const FVector& b) { return FVector{ a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X }; };
    auto norm = [&](const FVector& a) { float l = sqrtf(dot(a, a)); return l > 1e-4f ? FVector{ a.X / l, a.Y / l, a.Z / l } : a; };
    FVector mean{ 0, 0, 0 };
    for (int k = 0; k < 4; ++k) { FVector q = P(kn[k]); mean.X += q.X * 0.25f; mean.Y += q.Y * 0.25f; mean.Z += q.Z * 0.25f; }
    fingers = norm(mean);
    FVector across = norm(sub(P(0), P(9)));
    FVector n = norm(cross(fingers, across));
    float d = 0.0f;
    int used = 0;
    for (int j = 12; j <= 13; ++j) if (hb->ok[side][j]) { d += dot(sub(P(j), mean), n); ++used; }
    if (!used || fabsf(d) < 0.05f) return false;
    palm = d > 0.0f ? n : FVector{ -n.X, -n.Y, -n.Z };
    if (thumbSide) { float k = dot(across, fingers); *thumbSide = norm(FVector{ across.X - fingers.X * k, across.Y - fingers.Y * k, across.Z - fingers.Z * k }); }
    static USkeletalMesh* loggedFor[2] = { nullptr, nullptr };
    if (loggedFor[side] != hb->mesh) {
        loggedFor[side] = hb->mesh;
        Log("flat hand (%s): bind fingers (%.2f %.2f %.2f) palm (%.2f %.2f %.2f) thumb side %.2f", side ? "right" : "left",
            fingers.X, fingers.Y, fingers.Z, palm.X, palm.Y, palm.Z, d);
    }
    return true;
}

static void QuatSlerp(const float a[4], const float b[4], float t, float out[4]);

// The power pose (captured from the game's 1P power hand) has the fingers half closed; open them by blending each
// finger joint toward its OPEN bind pose (the mesh's reference skeleton) by openFrac. A guessed local-Y hinge rotation
// only clenched them harder either way (12:18 and 12:26 clips: both signs tried), so blend toward a known-open pose
// instead - robust to the bone's axis convention. Thumb (j >= 12) left as the power pose has it.
static void OpenPowerFingers(float q[4], USkeletalMeshComponent* comp, int side, int j, float openFrac) {
    if (j >= 12 || openFrac <= 0.0f) return;
    float ref[4];
    if (!RefLocalQuat(comp, kFingerBones[side][j], ref)) return;
    float out[4];
    QuatSlerp(q, ref, Clamp(openFrac, 0.0f, 1.0f), out);
    for (int i = 0; i < 4; ++i) q[i] = out[i];
}

static void BindCurl(const float rest[4], int side, int j, const float curl[4], float thumb0, float thumb1, const float share[3],
                     float out[4], float thumbTuck = 0.0f) {
    float deg = j < 12 ? curl[j / 3] * share[j % 3] : (j == 12 ? thumb0 : thumb1);
    float sign = side == 0 ? g_cfg.jumpCurlSignL : g_cfg.jumpCurlSignR;
    float h = sign * deg * 0.0087266f;
    float cu[4] = { 0.0f, sinf(h), 0.0f, cosf(h) };
    if (j == 12 && thumbTuck != 0.0f) {   // thumb root: swing about local Z first (same sign on both hands - mirrored rig)
        float hz = thumbTuck * 0.0087266f, sp[4] = { 0.0f, 0.0f, sinf(hz), cosf(hz) }, t[4];
        QuatMul(rest, sp, t);
        QuatMul(t, cu, out);
        return;
    }
    QuatMul(rest, cu, out);
}

static void QuatSlerp(const float a[4], const float b[4], float t, float out[4]) {
    float bb[4] = { b[0], b[1], b[2], b[3] };
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) { for (float& v : bb) v = -v; d = -d; }
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        float th = acosf(d), sn = sinf(th);
        wa = sinf((1.0f - t) * th) / sn;
        wb = sinf(t * th) / sn;
    }
    float n = 0.0f;
    for (int i = 0; i < 4; ++i) { out[i] = wa * a[i] + wb * bb[i]; n += out[i] * out[i]; }
    n = sqrtf(n);
    if (n > 1e-6f) for (int i = 0; i < 4; ++i) out[i] /= n;
}

static void CapturePowerHand(ADishonoredPlayerPawn* pawn) {
    static DWORD heldSince = 0;
    static bool taken = false;
    UObject* item = g_handItem[0];
    bool power = pawn && pawn->Mesh && pawn->Mesh->SkeletalMesh && item && item->Class &&
                 item->Class->GetName().find("Powers") != std::string::npos;
    DWORD now = GetTickCount();
    if (!power) { heldSince = 0; taken = false; return; }
    if (!heldSince) heldSince = now;
    if (taken || now - heldSince < 1000) return;
    taken = true;
    USkeletalMeshComponent* m = pawn->Mesh;
    auto* bases = reinterpret_cast<RawArray*>(&m->SpaceBases);
    if (!bases->Data) return;
    auto rows = [&](int idx, FVector r[3]) {
        const float* a = reinterpret_cast<const float*>(static_cast<uint8_t*>(bases->Data) + idx * 32);
        float x = a[0], y = a[1], z = a[2], w = a[3];
        r[0] = { 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y) };
        r[1] = { 2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x) };
        r[2] = { 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y) };
    };
    auto dot = [](const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
    FRotator got[14];
    for (int j = 0; j < 14; ++j) {
        FName name(kFingerBones[0][j]);
        int idx = name.FNameEntryId >= 0 ? m->MatchRefBone(name) : -1;
        int par = idx >= 0 ? m->MatchRefBone(m->GetParentBone(name)) : -1;
        if (idx < 0 || par < 0 || idx >= bases->Num || par >= bases->Num) {
            Log("power hand: %s not found on the 1P arms - not taken", kFingerBones[0][j]);
            return;
        }
        FVector c[3], p[3], l[3];
        rows(idx, c);
        rows(par, p);
        for (int i = 0; i < 3; ++i) l[i] = { dot(c[i], p[0]), dot(c[i], p[1]), dot(c[i], p[2]) };
        got[j] = AxesToRotator(l[0], l[1], l[2]);
    }
    for (int j = 0; j < 14; ++j) g_powerHandRot[j] = got[j];
    ++g_powerHandVer;
    static bool logged = false;
    if (!logged) {   // the numbers (the built-in default above came from this line)
        logged = true;
        std::string s;
        char buf[48];
        for (int j = 0; j < 14; ++j) { snprintf(buf, sizeof buf, " {%d,%d,%d}", got[j].Pitch, got[j].Yaw, got[j].Roll); s += buf; }
        Log("power hand: left-hand pose taken from the 1P arms (%s):%s", m->SkeletalMesh->GetName().c_str(), s.c_str());
    }
    g_powerHandHave = true;
}

static void UpdateJumpHands() {
    // Fingers and thumbs (both bodies): a gentle open claw, thumbs forward (user reference photos 2026-10-10).
    static const float kJointShare[3] = { 0.8f, 1.4f, 0.8f };     // knuckle, middle, tip
    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
    auto units = [](float deg) { return static_cast<int>(deg * 65536.0f / 360.0f); };
    for (USkeletalMeshComponent* comp : comps) {
        if (!comp || comp == g_holsterCast) continue;   // (its forearms and hands are folded away: UpdateHolsterShoulders owns its arms)
        for (int side = 0; side < 2; ++side) {
            float amount = g_cfg.jumpHands ? JumpArmAmount(side) : 0.0f;
            float slideW = SlideArmAmount(side);
            float sign = side == 0 ? g_cfg.jumpCurlSignL : g_cfg.jumpCurlSignR;
            if (comp == g_body || comp == g_shadowBody || comp == g_shadowArms || comp == g_holsterCast) {
                // Both bodies: the shadow body carries the same pose (the 1P arm skin copies it, see UpdateArmSkin).
                PlaceJumpArm(comp, side, amount, slideW);   // also sets the hand's rotation
            } else if (auto* wrist = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kWristControlNames[side])))) {
                wrist->BoneRotationSpace = EBoneControlSpace::BCS_BoneSpace;   // shadow body: just the wrist bend
                wrist->bAddRotation = 1;
                wrist->BoneRotation = FRotator{ units(g_cfg.jumpWristDeg * amount), 0, 0 };
                wrist->ControlStrength = wrist->StrengthTarget = amount;
            }
            if ((comp == g_body || comp == g_shadowBody || comp == g_shadowArms || comp == g_holsterCast) && !(g_cfg.jumpHandsInView && amount > 0.0f) && slideW <= 0.0f) {
                if (auto* wrist = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kWristControlNames[side]))))
                    wrist->ControlStrength = wrist->StrengthTarget = 0.0f;
            }
            const HandBind* hb = (g_cfg.handMatchSkin && g_armSkinOn) ? SkinHandBind() : nullptr;
            bool handFree = !(side == 0 ? g_leftHandBusy : g_rightHandBusy);
            for (int j = 0; j < 14; ++j) {
                auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kFingerBones[side][j])));
                if (!ctl) continue;
                ctl->bApplyTranslation = 0;
                // Arm skin on, hand free: the bone takes the 1P arms' own skeleton - bind position, and a rotation built on
                // the 1P bind (relaxed standing/walking, a fist at a sprint, the captured power pose in the air; blended).
                if (hb && hb->ok[side][j] && handFree) {
                    const FRotator* pz = side == 0 ? (g_powerHandHave ? g_powerHandRot : nullptr) : PowerHandRight(comp);
                    bool flatHand = side == 0 && g_cfg.slideLeftHandFlat;   // (the braced left hand lies open and flat: relaxed fingers)
                    float wJ = (pz && g_cfg.jumpLeftPowerHand) ? (amount > slideW && !flatHand ? amount : flatHand ? amount : slideW) : 0.0f;   // (slide: fingers spread - user sketch)
                    float sw = Clamp(g_sprintBackAmount > g_climbGrip[side] ? g_sprintBackAmount : g_climbGrip[side], 0.0f, 1.0f);   // (climbing: the fist closes round the chain)
                    float wS = (1.0f - wJ) * sw, wR = (1.0f - wJ) * (1.0f - sw);
                    float qR[4], qF[4], qP[4] = { 0, 0, 0, 1 }, acc[4] = { 0, 0, 0, 0 };
                    // relaxed: knuckles straight, a little bend further out (as approved 22:45); fist: all three joints
                    const float relaxShare[3] = { 0.0f, 1.0f, 1.3f };
                    const float fistShare[3] = { 1.0f, g_cfg.handFistMid, g_cfg.handFistTip };
                    // Braced flat hand: the relaxed curl gives way to a straight, open hand (the bind itself is a little curled,
                    // so it extends past it - HandFlatCurl, negative) as the slide weight comes in.
                    float fw = flatHand ? slideW : 0.0f, fc = 1.0f - fw;
                    float rc[4];
                    for (int c4 = 0; c4 < 4; ++c4) rc[c4] = g_cfg.handRelaxCurl[c4] * fc + g_cfg.handFlatCurl[c4] * fw;
                    const float flatShare[3] = { 1.0f, 1.0f, 1.0f };   // (all three joints straighten)
                    BindCurl(hb->q[side][j], side, j, rc, g_cfg.handRelaxThumb0 * fc + g_cfg.handFlatThumb * fw,
                             g_cfg.handRelaxThumb1 * fc + g_cfg.handFlatThumb * fw, fw > 0.5f ? flatShare : relaxShare, qR);
                    BindCurl(hb->q[side][j], side, j, g_cfg.handFistCurl, g_cfg.handFistThumb0, g_cfg.handFistThumb1, fistShare, qF,
                             g_cfg.handFistThumbTuck);
                    if (wJ > 0.0f) { RotatorToQuat(pz[j], qP); OpenPowerFingers(qP, comp, side, j, g_cfg.jumpHandOpenFrac); }
                    auto addq = [&](const float* q, float w) {   // weighted sum, signs aligned to the relaxed pose
                        if (w <= 0.0f) return;
                        float d = q[0] * qR[0] + q[1] * qR[1] + q[2] * qR[2] + q[3] * qR[3];
                        float sg = d < 0.0f ? -1.0f : 1.0f;
                        for (int i = 0; i < 4; ++i) acc[i] += w * sg * q[i];
                    };
                    addq(qR, wR);
                    addq(qF, wS);
                    addq(qP, wJ);
                    float n = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2] + acc[3] * acc[3]);
                    if (n < 1e-4f) for (int i = 0; i < 4; ++i) acc[i] = qR[i]; else for (int i = 0; i < 4; ++i) acc[i] /= n;
                    ctl->BoneRotationSpace = EBoneControlSpace::BCS_ParentBoneSpace;
                    ctl->bAddRotation = 0;
                    ctl->bApplyRotation = 1;
                    ctl->BoneRotation = QuatToRotator(acc);
                    ctl->bApplyTranslation = 1;
                    ctl->bAddTranslation = 0;
                    ctl->BoneTranslationSpace = EBoneControlSpace::BCS_ParentBoneSpace;
                    ctl->BoneTranslation = FVector{ hb->p[side][j][0], hb->p[side][j][1], hb->p[side][j][2] };
                    ctl->ControlStrength = ctl->StrengthTarget = 1.0f;
                    continue;
                }
                // Free hands: the power-holding pose in the air (both hands, see above), a relaxed pose standing/walking
                // (RelaxedFingerQuat), the run animation's fists at a sprint - slerped between, so landing doesn't pop.
                const FRotator* pose = side == 0 ? (g_powerHandHave ? g_powerHandRot : nullptr) : PowerHandRight(comp);
                bool freeHand = !(side == 0 ? g_leftHandBusy : g_rightHandBusy);
                float jw = (pose && g_cfg.jumpLeftPowerHand) ? amount : 0.0f;
                float rw = (freeHand && g_cfg.handRelaxedPose) ? Clamp(1.0f - g_sprintBackAmount, 0.0f, 1.0f) * (1.0f - amount) : 0.0f;
                float qr[4], qp[4], qo[4];
                bool haveRelax = rw > 0.001f && RelaxedFingerQuat(comp, side, j, qr);
                if (!haveRelax) rw = 0.0f;
                if (jw + rw > 0.001f) {
                    if (jw > 0.001f) { RotatorToQuat(pose[j], qp); OpenPowerFingers(qp, comp, side, j, g_cfg.jumpHandOpenFrac); }
                    if (jw > 0.001f && haveRelax) QuatSlerp(qp, qr, rw / (jw + rw), qo);
                    else if (haveRelax) for (int i = 0; i < 4; ++i) qo[i] = qr[i];
                    else for (int i = 0; i < 4; ++i) qo[i] = qp[i];
                    ctl->BoneRotationSpace = EBoneControlSpace::BCS_ParentBoneSpace;
                    ctl->bAddRotation = 0;
                    ctl->BoneRotation = QuatToRotator(qo);
                    ctl->ControlStrength = ctl->StrengthTarget = jw > rw ? jw : rw;
                    continue;
                }
                float deg = j < 12 ? g_cfg.jumpFingerCurlDeg * kJointShare[j % 3] * (1.0f + 0.12f * g_jumpFlex * g_cfg.jumpSway)
                                   : g_cfg.jumpThumbDeg;
                // thumb root: yaw, the same sign on both hands (the rig is mirrored; rest-pose test: +Z swings the
                // thumb tip out from the palm on both, -Z tucks it under - the right hand had -Z)
                float spread = j == 12 ? g_cfg.jumpThumbSpreadDeg : 0.0f;
                // Sprinting clenched the hands into fists (user): an added curl rides on the run animation's fist.
                // Set each joint outright instead - reference pose (open hand) x spread (local Z) x curl (local Y),
                // in parent-bone space, blended in with the jump.
                float rest[4];
                // 17:03 diag: skin and shadow match the body exactly - the hook is the bind pose itself (phalanges bent
                // 12-22 deg each, thumb_1 30 deg). Middle and tip joints (and thumb_1) start straight; knuckles keep
                // the bind (it sets each finger's direction off the hand).
                bool straight = (j < 12 && j % 3 != 0) || j == 13;
                if (straight ? true : RefLocalQuat(comp, kFingerBones[side][j], rest)) {
                    if (straight) { rest[0] = rest[1] = rest[2] = 0.0f; rest[3] = 1.0f; }
                    const float h2 = 0.0087266f;   // half a degree in radians
                    float sp[4] = { 0, 0, sinf(spread * h2), cosf(spread * h2) };
                    float cu[4] = { 0, sinf(sign * deg * h2), 0, cosf(sign * deg * h2) };
                    float t[4], q[4];
                    QuatMul(rest, sp, t);
                    QuatMul(t, cu, q);
                    ctl->BoneRotationSpace = EBoneControlSpace::BCS_ParentBoneSpace;
                    ctl->bAddRotation = 0;
                    ctl->BoneRotation = QuatToRotator(q);
                    ctl->ControlStrength = ctl->StrengthTarget = amount;
                } else {
                    ctl->BoneRotationSpace = EBoneControlSpace::BCS_BoneSpace;
                    ctl->bAddRotation = 1;
                    ctl->BoneRotation = FRotator{ units(sign * deg * amount), units(spread * amount), 0 };
                    ctl->ControlStrength = ctl->StrengthTarget = amount > 0.0f ? 1.0f : 0.0f;
                }
            }
            // Cuff carrier: the body's handAttachment bone placed where the 1P sleeve bone is, off the forearm (any time the
            // skin is on - a busy arm is shrunk anyway). 22:45 user screenshot: mapped to the hand, Corvo's cuffs sat ~4 uu
            // too far down, through the backs of the hands.
            if (auto* cc = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kCuffControlNames[side])))) {
                const HandBind* hbc = (g_cfg.handMatchSkin && g_armSkinOn) ? SkinHandBind() : nullptr;
                bool on = hbc && hbc->ok[side][15];
                if (on) {
                    FName la(kLowerArmBones[side]);
                    cc->bApplyTranslation = 1;
                    cc->bAddTranslation = 0;
                    cc->BoneTranslationSpace = EBoneControlSpace::BCS_OtherBoneSpace;
                    cc->TranslationSpaceBoneName = la;
                    cc->BoneTranslation = FVector{ hbc->p[side][15][0], hbc->p[side][15][1], hbc->p[side][15][2] };
                    cc->bApplyRotation = 1;
                    cc->bAddRotation = 0;
                    cc->BoneRotationSpace = EBoneControlSpace::BCS_OtherBoneSpace;
                    cc->RotationSpaceBoneName = la;
                    cc->BoneRotation = QuatToRotator(hbc->q[side][15]);
                }
                cc->ControlStrength = cc->StrengthTarget = on ? 1.0f : 0.0f;
            }
            // The wrist twist bone takes the 1P arms' bind too (rotation + position), same conditions as the fingers.
            if (auto* hc = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kHelperFixNames[side])))) {
                bool on = hb && hb->ok[side][14] && handFree;
                if (on) {
                    hc->bApplyRotation = 1;
                    hc->bAddRotation = 0;
                    hc->BoneRotationSpace = EBoneControlSpace::BCS_ParentBoneSpace;
                    hc->BoneRotation = QuatToRotator(hb->q[side][14]);
                    hc->bApplyTranslation = 1;
                    hc->bAddTranslation = 0;
                    hc->BoneTranslationSpace = EBoneControlSpace::BCS_ParentBoneSpace;
                    hc->BoneTranslation = FVector{ hb->p[side][14][0], hb->p[side][14][1], hb->p[side][14][2] };
                }
                hc->ControlStrength = hc->StrengthTarget = on ? 1.0f : 0.0f;
            }
        }
    }
}

// Climbing arms (user, 2026-10-11): on ladders/chains the game animates only the first-person arms - forearms and
// hands (the 1P mesh has no upper arms). The visible body supplies the upper arms: each upper_arm points from its
// shoulder joint to where the 1P elbow is (1P lower_arm relative to the 1P camera_jnt, placed at the real view point -
// the arm-shadow trick), stretched so the two meet; the body's own forearm and hand are shrunk away, so the 1P forearm
// carries on from the body's elbow and does the gripping.
static void UpdateClimbArms(ADishonoredPlayerPawn* pawn) {
    if (!g_body || !pawn || !pawn->Mesh) return;
    float amount = g_cfg.climbArms ? g_climbAmount : 0.0f;
    static bool active = false;
    if (amount <= 0.0f && !active) return;
    Xf cam1P;
    bool ok = amount > 0.0f && BoneXf(pawn->Mesh, "camera_jnt", cam1P);
    auto sub = [](const FVector& a, const FVector& b) { return FVector{ a.X - b.X, a.Y - b.Y, a.Z - b.Z }; };
    auto dot = [](const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
    auto cross = [](const FVector& a, const FVector& b) { return FVector{ a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X }; };
    auto norm = [&](const FVector& a) { float l = sqrtf(dot(a, a)); return l > 1e-4f ? FVector{ a.X / l, a.Y / l, a.Z / l } : a; };
    float pyaw = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
    FVector right{ -sinf(pyaw), cosf(pyaw), 0.0f };
    static int logged = 0;
    for (int side = 0; side < 2; ++side) {
        const char* lower = side == 0 ? "lower_arm_L_jnt" : "lower_arm_R_jnt";
        auto* upperCtl = static_cast<USkelControlSingleBone*>(g_body->FindSkelControl(FName(kUpperArmBones[side])));
        auto* lowerCtl = static_cast<USkelControlSingleBone*>(g_body->FindSkelControl(FName(lower)));
        if (!upperCtl || !lowerCtl) continue;
        Xf elbow1P;
        if (!ok || !BoneXf(pawn->Mesh, lower, elbow1P)) {
            upperCtl->ControlStrength = upperCtl->StrengthTarget = 0.0f;
            upperCtl->BoneScale = 1.0f;
            continue;
        }
        FVector E{ g_viewLoc.X + elbow1P.t[0] - cam1P.t[0], g_viewLoc.Y + elbow1P.t[1] - cam1P.t[1],
                   g_viewLoc.Z + elbow1P.t[2] - cam1P.t[2] };
        FVector S = g_body->GetBoneLocation(FName(kUpperArmBones[side]), 0);
        FVector Ecur = g_body->GetBoneLocation(FName(lower), 0);
        FVector d = sub(E, S);
        float want = sqrtf(dot(d, d)), L1 = sqrtf(dot(sub(Ecur, S), sub(Ecur, S))) / (upperCtl->BoneScale > 0.01f ? upperCtl->BoneScale : 1.0f);
        if (want < 1.0f || L1 < 5.0f) continue;
        FVector X = norm(d);
        float sideSign = side == 0 ? -1.0f : 1.0f;
        FVector Y = norm(cross(X, FVector{ 0, 0, sideSign }));   // roll: level, across the arm (hidden under the sleeve)
        if (dot(Y, Y) < 0.5f) Y = norm(cross(X, right));
        FVector Z = cross(X, Y);
        upperCtl->BoneRotationSpace = EBoneControlSpace::BCS_WorldSpace;
        upperCtl->bAddRotation = 0;
        upperCtl->bApplyRotation = 1;
        upperCtl->BoneRotation = AxesToRotator(X, Y, Z);
        upperCtl->BoneScale = Clamp(want / L1, 0.6f, 1.6f);   // the upper arm reaches the 1P elbow
        upperCtl->ControlStrength = upperCtl->StrengthTarget = amount;
        lowerCtl->bApplyRotation = 0;
        lowerCtl->BoneScale = 0.001f;                          // the body's forearm + hand: the 1P ones show instead
        lowerCtl->ControlStrength = lowerCtl->StrengthTarget = amount;
        if (logged < 4 && amount > 0.99f) {
            ++logged;
            Log("climb arms: %s shoulder->1P elbow %.0f uu (upper arm %.0f, scale %.2f)", side ? "right" : "left", want, L1,
                upperCtl->BoneScale);
        }
    }
    active = amount > 0.0f;
    if (!active)
        for (int side = 0; side < 2; ++side)
            if (auto* upperCtl = static_cast<USkelControlSingleBone*>(g_body->FindSkelControl(FName(kUpperArmBones[side])))) {
                upperCtl->ControlStrength = upperCtl->StrengthTarget = 0.0f;
                upperCtl->BoneScale = 1.0f;
            }
}

// First-person arm skin (user, 2026-10-11: "use the texture from the arms used when holding weapons for the arms of
// the player model - for Daud and his Overseer disguise"). Those bodies have one material for the whole body (its
// texture is laid out for the body), so the 1P arms' texture can't simply go on. Instead the 1P arms model itself
// (Daud's / the disguise's: forearms and hands, the same 79-bone names) is drawn on the body: a component that copies
// the SHADOW body's pose (same controls, arms not shrunk - copying the visible body would copy the shrink below) and
// sits at the visible body's Translation/Rotation/Scale; the visible body's own forearms and hands shrink away. A side
// whose hand holds something keeps the game's 1P arm there instead (that copy side is hidden).

static USkeletalMesh* CorvoArmsMesh(ADishonoredPlayerPawn* pawn);
static bool XfInv(const Xf& a, Xf& o);
static Xf XfFromRotator(const FRotator& r, const FVector& loc);
static Xf XfMul(const Xf& a, const Xf& b);
static void XfDecompose(const Xf& x, FVector& loc, FRotator& rot, float& scale);
static Xf OwnerXf(ADishonoredPlayerPawn* pawn);
static bool BoneAtomXf(USkeletalMeshComponent* comp, const char* bone, Xf& out);

// Skin bones the body skeleton lacks are drawn in the skin's own reference pose (INDEX_NONE in ParentBoneMap). Corvo's 1P
// arms hang the coat cuffs on sleeve_L/R_jnt (292 verts each; 22 uu down the forearm, no rotation off it - the bodies' hand
// bone is at 25.2): mapped to the hand, each cuff sits at the wrist, ~3 uu nearer the fingers (22:01 clip: Corvo's sleeves
// far up the arm, bare forearms). Collarbone_L/R (10 verts) -> shoulder_L/R. Re-checked each second (a rebuilt map).
static void RemapUnmatchedSkinBones(USkeletalMeshComponent* skin, USkeletalMeshComponent* parent, bool log) {
    auto* map = reinterpret_cast<RawArray*>(&skin->ParentBoneMap);
    if (!skin->SkeletalMesh || !parent || !parent->SkeletalMesh || !map->Data || map->Num <= 0) return;
    // Sleeves go to the cuff carrier (handAttachment, placed off the forearm - see kCuffCarrierBones) when the pose source
    // has that control, else to the hand (the cuff then sits ~4 uu too far down, over the hand).
    static bool carrier = false;
    static USkeletalMeshComponent* carrierFor = nullptr;
    if (carrierFor != parent) { carrierFor = parent; carrier = parent->FindSkelControl(FName(kCuffControlNames[0])) != nullptr; }
    const char* kRemap[4][2] = { { "sleeve_L_jnt", carrier ? kCuffCarrierBones[0] : "hand_L_jnt" },
                                 { "sleeve_R_jnt", carrier ? kCuffCarrierBones[1] : "hand_R_jnt" },
                                 { "Collarbone_L_Jnt", "shoulder_L_jnt" }, { "Collarbone_R_Jnt", "shoulder_R_jnt" } };
    // Bone indices cached per mesh pair - checked every frame (cheap), so a rebuilt map is fixed before it is drawn.
    static USkeletalMesh* forSkin = nullptr;
    static USkeletalMesh* forParent = nullptr;
    static int idx[4][2];
    static bool forCarrier = false;
    if (forSkin != skin->SkeletalMesh || forParent != parent->SkeletalMesh || forCarrier != carrier) {
        forSkin = skin->SkeletalMesh;
        forParent = parent->SkeletalMesh;
        forCarrier = carrier;
        for (int i = 0; i < 4; ++i) {
            idx[i][0] = skin->MatchRefBone(FName(kRemap[i][0]));
            idx[i][1] = parent->MatchRefBone(FName(kRemap[i][1]));
        }
    }
    int* m = static_cast<int*>(map->Data);
    int done = 0;
    for (int i = 0; i < 4; ++i) {
        int si = idx[i][0], pi = idx[i][1];
        // sleeves (no namesake on the body): always the chosen target; collarbones only while unmatched
        if (si < 0 || si >= map->Num || pi < 0 || m[si] == pi || (i >= 2 && m[si] != -1)) continue;
        m[si] = pi;
        ++done;
    }
    static DWORD lastFixLog = 0;
    if (!log && done && GetTickCount() - lastFixLog > 2000) {
        lastFixLog = GetTickCount();
        Log("arm skin: bone map was rebuilt - %d sleeve/collarbone link(s) restored", done);
    }
    if (log) {
        int left = 0;
        for (int i = 0; i < map->Num; ++i) if (m[i] == -1) ++left;
        Log("arm skin: %d unmatched bone(s) mapped (sleeves -> %s, collarbones -> shoulders), %d still unmatched", done,
            carrier ? "cuff carriers on the forearms" : "hands", left);
    }
}

static void UpdateArmSkin(ADishonoredPlayerPawn* pawn) {
    // (16:29 log: no skin - Daud's 1P arms only load when the Arms option is Daud; now the Daud body loads them too.
    // User: every body, Corvo's as well.)
    USkeletalMesh* mesh = g_builtChar == 1 ? g_daudArmsMesh : g_builtChar == 2 ? g_ovsArmsMesh : CorvoArmsMesh(pawn);
    // 16:37 log: 'shadow body 00000000' - no shadow body that session, so the skin never came on. It now copies the
    // visible body's pose; the body's own forearms are hidden with HideBoneByName, which only stops them being drawn
    // (the pose - which the copy reads - keeps them), not with a BoneScale the copy would inherit.
    // (16:43 screenshot: the old forearms still showed - HideBoneByName is undone by the elbow controls' BoneScale.)
    // Back to: copy the shadow body's pose, shrink the visible body's forearms; the shadow body now always exists.
    bool want = g_cfg.fpArmSkin && mesh && pawn && g_body && g_shadowBody && g_bodyVisible;
    static DWORD lastWhy = 0;
    if (!want && g_cfg.fpArmSkin && GetTickCount() - lastWhy > 10000) {   // (16:32: still never switched on - say why)
        lastWhy = GetTickCount();
        Log("arm skin off: body char %d, mesh %s, body %p, shadow body %p, visible %d", g_builtChar,
            mesh ? mesh->GetName().c_str() : "none", g_body, g_shadowBody, g_bodyVisible ? 1 : 0);
    }
    g_skinFrameOk = false;
    g_skinArmShown[0] = g_skinArmShown[1] = false;
    if (!want) {
        if (g_armSkin && g_armSkinOn) g_armSkin->SetHidden(true);
        g_armSkinOn = false;   // (also when g_armSkin was dropped elsewhere - a stale 'on' kept the handover running)

        return;
    }
    // Owned by the pawn (like the arm shadow), dropped - not touched - on a new pawn/world (see g_armsShadow).
    if (g_armSkin && (g_armSkin->SkeletalMesh != mesh || g_armSkin->ParentAnimComponent != g_shadowBody)) {
        pawn->DetachComponent(g_armSkin);
        g_armSkin = nullptr;
        g_armSkinOn = false;
    }
    if (!g_armSkin) {
        auto* c = static_cast<USkeletalMeshComponent*>(
            StaticConstructObject(USkeletalMeshComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
        if (!c) return;
        c->SetSkeletalMesh(mesh, false);
        c->bUpdateSkelWhenNotRendered = 1;
        // 20:38 clip/log + retail disassembly: with a ParentAnimComponent and bTransformFromAnimParent (class default 1)
        // the engine takes the PARENT's LocalToWorld (UpdateTransform 0x757840 -> base SetParentToWorld 0x52a090 with
        // parent+0x60), ignores the owner's (SetParentToWorld 0x70d6b0 returns early), skips this mesh's own
        // Origin/RotOrigin and applies our Translation/Rotation/Scale in front of the shadow body's matrix
        // (CalcCurrentLocalToWorld 0x70d6d0) - in its raw mesh axes: our X drew as left, Y as down, Z as forward. Every
        // skin offset so far landed on the wrong axis. Off: placed from the owner + its own mesh origin (= skinFrame).
        const int inheritWas = c->bTransformFromAnimParent, reallyWas = c->m_bReallyInheritTransformFromAnimParent;
        c->bTransformFromAnimParent = 0;
        c->m_bReallyInheritTransformFromAnimParent = 0;
        if (g_body->LightEnvironment) c->SetLightEnvironment(g_body->LightEnvironment);
        c->CastShadow = 0;
        c->bCastDynamicShadow = 0;
        c->DepthPriorityGroup = g_body->DepthPriorityGroup;
        pawn->AttachComponent(c);
        c->SetParentAnimComponent(g_shadowBody);
        c->UpdateParentBoneMap();
        RemapUnmatchedSkinBones(c, g_shadowBody, true);
        g_armSkin = c;
        Log("arm skin: %s drawn on the %s body (%p) | transform from anim parent was %d (really %d), now 0",
            mesh->GetName().c_str(), CharName(g_builtChar), c, inheritWas, reallyWas);
    }
    if (!g_armSkinOn) { g_armSkin->SetHidden(false); g_armSkinOn = true; }
    {
        RemapUnmatchedSkinBones(g_armSkin, g_shadowBody, false);   // every frame (indices cached)
        auto* sm = reinterpret_cast<RawArray*>(&g_armSkin->Materials);
        UMaterialInterface* cur0 = (sm->Data && sm->Num > 0) ? static_cast<UMaterialInterface**>(sm->Data)[0] : nullptr;
        if (mesh == g_ovsArmsMesh && g_ovsArmsMat && cur0 != g_ovsArmsMat) {
            g_armSkin->SetMaterial(0, g_ovsArmsMat);
            Log("arm skin: Overseer arms material on");
        }
    }
    g_shadowBody->ForcedLodModel = 1;   // the pose source must update every bone (see CreateShadowBody)
    g_armSkin->ForcedLodModel = 1;
    // In the air the new arms' shoulders (the 1P model has full sleeves) clipped the camera: ease them back/down.
    float air = g_jumpAmount;
    FVector t = g_body->Translation;
    t.X -= g_cfg.armSkinJumpBack * air;
    t.Z -= g_cfg.armSkinJumpDown * air;
    // Jump: keep the arms centred on the view (17:32 clip: mid-jump both arms sat to the right of the camera - fine
    // standing and walking). The jump pose is built out from the shadow body's two upper-arm joints, and nothing keeps
    // those under the view sideways in the air. Measured each frame: the gap between the view and their midpoint (moved
    // to where the skin draws them), across the view's level heading; the skin moves across by it, eased, in the air.
    {
        static float lat = 0.0f;
        static DWORD lastC = 0, lastCLog = 0;
        DWORD nowC = GetTickCount();
        float dtC = lastC ? Clamp((nowC - lastC) / 1000.0f, 0.0f, 0.1f) : 0.0f;
        lastC = nowC;
        float pyawC = pawn->Rotation.Yaw * (6.2831853f / 65536.0f), cpy = cosf(pyawC), spy = sinf(pyawC);
        FVector dTc{ t.X - g_shadowBodyT.X, t.Y - g_shadowBodyT.Y, t.Z - g_shadowBodyT.Z };
        FVector offC{ dTc.X * cpy - dTc.Y * spy, dTc.X * spy + dTc.Y * cpy, dTc.Z };
        float vyaw = g_viewRot.Yaw * (6.2831853f / 65536.0f);
        FVector R{ -sinf(vyaw), cosf(vyaw), 0.0f };
        auto across = [&](const FVector& p) {   // + = right of the view
            return (p.X + offC.X - g_viewLoc.X) * R.X + (p.Y + offC.Y - g_viewLoc.Y) * R.Y;
        };
        FVector a = g_shadowBody->GetBoneLocation(FName(kUpperArmBones[0]), 0);
        FVector b = g_shadowBody->GetBoneLocation(FName(kUpperArmBones[1]), 0);
        float gap = across(FVector{ (a.X + b.X) * 0.5f, (a.Y + b.Y) * 0.5f, 0.0f });
        bool fresh = nowC - g_viewLocTick < 500 && fabsf(gap) < 100.0f;   // (ignore a garbage frame: spawn, teleport)
        lat += ((fresh ? gap : 0.0f) - lat) * Clamp(dtC * 12.0f, 0.0f, 1.0f);
        float c = Clamp(lat, -g_cfg.armSkinJumpCentreMax, g_cfg.armSkinJumpCentreMax);
        float s = -c * air * g_cfg.armSkinJumpCentre;   // along the view's right, world -> actor space
        t.X += s * (R.X * cpy + R.Y * spy);
        t.Y += s * (-R.X * spy + R.Y * cpy);
        if (air > 0.5f && nowC - lastCLog > 500) {
            lastCLog = nowC;
            Log("arm skin jump centre: arms %.1f uu right of the view (eased %.1f), moved %.1f | pelvis %.1f head %.1f",
                gap, lat, s, across(g_shadowBody->GetBoneLocation(FName("Root_jnt"), 0)),
                across(g_shadowBody->GetBoneLocation(FName("head_jnt"), 0)));
        }
    }
    // Placement (17:10 clip: the arms sat off the body's centre). The bones are copied in the body mesh's space, but
    // each mesh has its own RotOrigin/Origin (the 1P arms model's stands it up for the camera). World = bone x mesh
    // origin x component x owner, so the skin's component frame = inv(skin origin) x body origin x body component.
    // The engine applies Origin first, in the mesh's own frame, then RotOrigin (MODLOG build 89 diag: the 1P arms'
    // LocalToWorld sat ~72 uu LOWER for Origin (0, 87.5, 0) - not to the side). Composed the other way round
    // (FRotationTranslationMatrix(RotOrigin, Origin)), the 9.5 uu gap between the two meshes' Origins went to the
    // side instead of down: the skin drew 9.5 right and 9.5 high (user, 19:22 clip: both arms shifted right; the
    // inside of the shoulders showed while sprinting). ArmSkinOriginFix = the engine's order.
    // Sprinting: the new arms ease back a little more than the body (user: the inside of the shoulders showed).
    t.X -= g_cfg.armSkinSprintBack * g_sprintBackAmount * (1.0f - air);
    t.Z -= g_cfg.armSkinSprintDown * g_sprintBackAmount * (1.0f - air);
    // Down stairs the game's camera lags ~50 uu above (log 20:20 stairDz 52); SyncTransform lifts the visible body with
    // it (g_cameraDip > 0) so the legs stay in view - which lifted these arms straight into the camera (user: worst
    // running down stairs). The arms keep their own height.
    if (g_cameraDip > 0.0f) t.Z -= g_cameraDip * g_cfg.armSkinStairUpCut;
    t.Y += g_cfg.armSkinShiftRight + (g_cfg.armSkinSprintShiftRight - g_cfg.armSkinShiftRight) * g_sprintBackAmount;
    // Closed loop (19:34 clip: still shifted right, the left sleeve through the camera - yet by the model the arms sat
    // centred and never came within 25 uu of the camera). Where the skin should be: the shadow body's pose moved by the
    // gap between the two components' translations. Where it really is: the engine's own bone positions for the skin
    // (GetBoneMatrix reads the parent's pose through ParentBoneMap times the skin's LocalToWorld - what is drawn). Each
    // frame the two shoulder joints' midpoint is compared and the difference moves the skin (ArmSkinAlign), whatever the
    // reason for it (mesh origins, LocalToWorld conventions). Last frame's gap is used: both readings are last frame's.
    static FVector alignCorr = { 0, 0, 0 };   // actor space, added to the skin's placement
    static FVector alignPrevOff = { 0, 0, 0 };
    static USkeletalMesh* alignFor = nullptr;
    static bool alignHavePrev = false, alignReadOk = false;
    if (!g_cfg.armSkinAlign) { alignCorr = { 0, 0, 0 }; alignHavePrev = false; alignReadOk = false; }
    else {
        if (alignFor != mesh) { alignFor = mesh; alignCorr = { 0, 0, 0 }; alignHavePrev = false; }
        float py = pawn->Rotation.Yaw * (6.2831853f / 65536.0f), c = cosf(py), sn = sinf(py);
        FName sl("shoulder_L_jnt"), sr("shoulder_R_jnt");
        FVector kl = g_armSkin->GetBoneLocation(sl, 0), kr = g_armSkin->GetBoneLocation(sr, 0);
        FVector hl = g_shadowBody->GetBoneLocation(sl, 0), hr = g_shadowBody->GetBoneLocation(sr, 0);
        auto len = [](float x, float y, float z) { return sqrtf(x * x + y * y + z * z); };
        FVector raw{ (kl.X + kr.X - hl.X - hr.X) * 0.5f, (kl.Y + kr.Y - hl.Y - hr.Y) * 0.5f, (kl.Z + kr.Z - hl.Z - hr.Z) * 0.5f };
        FVector err{ raw.X - alignPrevOff.X, raw.Y - alignPrevOff.Y, raw.Z - alignPrevOff.Z };
        float e = len(err.X, err.Y, err.Z);
        bool apart = len(kl.X - kr.X, kl.Y - kr.Y, kl.Z - kr.Z) > 5.0f;   // a real pose (not one point for every bone)
        bool parentRead = len(raw.X, raw.Y, raw.Z) < 0.5f && len(alignPrevOff.X, alignPrevOff.Y, alignPrevOff.Z) > 3.0f;
        bool ok = alignHavePrev && apart && !parentRead && e < 60.0f;
        if (ok != alignReadOk) {
            Log("arm skin align: %s (skin shoulders %s, raw gap %.1f, error %.1f)", ok ? "reading the skin's bones" : "NOT usable",
                apart ? "apart" : "together", len(raw.X, raw.Y, raw.Z), e);
            alignReadOk = ok;
        }
        if (ok && g_cfg.armSkinAlign) {
            FVector el{ err.X * c + err.Y * sn, -err.X * sn + err.Y * c, err.Z };   // world -> actor space
            alignCorr.X = Clamp(alignCorr.X - el.X * 0.3f, -40.0f, 40.0f);
            alignCorr.Y = Clamp(alignCorr.Y - el.Y * 0.3f, -40.0f, 40.0f);
            alignCorr.Z = Clamp(alignCorr.Z - el.Z * 0.3f, -40.0f, 40.0f);
        }
        static DWORD lastA = 0;
        FVector v = pawn->Velocity;
        if ((air > 0.3f || v.X * v.X + v.Y * v.Y > 300.0f * 300.0f) && GetTickCount() - lastA > 500) {
            lastA = GetTickCount();
            FVector el{ err.X * c + err.Y * sn, -err.X * sn + err.Y * c, err.Z };
            Xf view = XfFromRotator(g_viewRot, g_viewLoc);
            auto inView = [&](const FVector& p, float& r, float& u, float& f) {
                FVector d{ p.X - g_viewLoc.X, p.Y - g_viewLoc.Y, p.Z - g_viewLoc.Z };
                f = d.X * view.m[0][0] + d.Y * view.m[0][1] + d.Z * view.m[0][2];
                r = d.X * view.m[1][0] + d.Y * view.m[1][1] + d.Z * view.m[1][2];
                u = d.X * view.m[2][0] + d.Y * view.m[2][1] + d.Z * view.m[2][2];
            };
            float lr, lu, lf, rr, ru, rf;
            inView(g_armSkin->GetBoneLocation(FName(kHandBones[0]), 0), lr, lu, lf);
            inView(g_armSkin->GetBoneLocation(FName(kHandBones[1]), 0), rr, ru, rf);
            Log("arm skin align: off fwd %.1f right %.1f up %.1f -> correction (%.1f %.1f %.1f) | skin hands in view (right/up/fwd): "
                "L %.0f %.0f %.0f  R %.0f %.0f %.0f | pitch %.0f speed %.0f air %.2f", el.X, el.Y, el.Z, alignCorr.X, alignCorr.Y,
                alignCorr.Z, lr, lu, lf, rr, ru, rf, g_viewPitchDeg, sqrtf(v.X * v.X + v.Y * v.Y), air);
        }
        FVector dTn{ t.X - g_shadowBodyT.X, t.Y - g_shadowBodyT.Y, t.Z - g_shadowBodyT.Z };   // this frame's gap, for next time
        alignPrevOff = { dTn.X * c - dTn.Y * sn, dTn.X * sn + dTn.Y * c, dTn.Z };
        alignHavePrev = true;
    }
    Xf skinFrame;           // pose (shadow body mesh space) -> world, exactly as the skin is placed this frame
    bool haveSkinFrame = false;
    {
        FVector tPlaced{ t.X + alignCorr.X, t.Y + alignCorr.Y, t.Z + alignCorr.Z };
        Xf cb = XfFromRotator(g_body->Rotation, tPlaced);
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) cb.m[i][j] *= g_body->Scale;
        USkeletalMesh* bm = g_body->SkeletalMesh;
        auto meshOrigin = [](USkeletalMesh* m) {
            if (!g_cfg.armSkinOriginFix) return XfFromRotator(m->RotOrigin, m->Origin);
            Xf o = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { m->Origin.X, m->Origin.Y, m->Origin.Z } };
            return XfMul(o, XfFromRotator(m->RotOrigin, FVector{ 0, 0, 0 }));   // Origin, then RotOrigin
        };
        Xf ob = meshOrigin(bm), os = meshOrigin(mesh), osInv;
        skinFrame = XfMul(XfMul(ob, cb), OwnerXf(pawn));
        haveSkinFrame = true;
        g_skinFrame = skinFrame;
        g_skinFrameOk = g_cfg.armSkinOriginFix && g_body->Scale > 0.5f;   // (mantle: body Scale ~0.001 - the inverse would blow up)
        static USkeletalMesh* loggedFor = nullptr;
        if (loggedFor != mesh) {
            loggedFor = mesh;
            Log("arm skin: body mesh origin (%.1f %.1f %.1f) rot (%d %d %d), arms mesh origin (%.1f %.1f %.1f) rot (%d %d %d)",
                bm->Origin.X, bm->Origin.Y, bm->Origin.Z, bm->RotOrigin.Pitch, bm->RotOrigin.Yaw, bm->RotOrigin.Roll,
                mesh->Origin.X, mesh->Origin.Y, mesh->Origin.Z, mesh->RotOrigin.Pitch, mesh->RotOrigin.Yaw, mesh->RotOrigin.Roll);
        }
        if (XfInv(os, osInv)) {
            Xf cs = XfMul(XfMul(osInv, ob), cb);
            FVector loc; FRotator rot; float sc;
            XfDecompose(cs, loc, rot, sc);
            static USkeletalMesh* loggedGap = nullptr;
            if (loggedGap != mesh) {   // expected with the fix: (0 0 -9.5) for Daud's arms on Daud's body
                loggedGap = mesh;
                Log("arm skin: placed at body + (%.1f %.1f %.1f) (origin fix %d)", loc.X - t.X, loc.Y - t.Y, loc.Z - t.Z,
                    g_cfg.armSkinOriginFix ? 1 : 0);
            }
            g_armSkin->SetTranslation(loc);
            g_armSkin->SetRotation(rot);
            g_armSkin->SetScale(sc);
        }
    }
    // The visible body's own forearms + hands shrink away (the skin's show). A busy hand: the game's 1P arm shows
    // there, so the skin must not - HideBoneByName on the skin did nothing (it copies the shadow body's pose; 16:53
    // clip: a glove floated beside the sword / wristbow). The SHADOW body's forearm on that side shrinks instead (the
    // skin copies that); its upper arm stays, and the 1P arm shadow casts that forearm when shadows are on.
    // 17:17 clip: (1) weapons out, the skin's upper sleeve on the busy side still reached up toward the 1P hand (the
    // shadow arm follows it by IK) - a second arm through the camera. A busy side now loses the skin's whole arm, from
    // the shoulder (the game's 1P arm is the arm there). (2) Sprinting and looking down, the run swing brought each
    // upper sleeve (the 1P model's are bulky) through the camera - big flat panels. Each free arm is measured: if its
    // upper arm (shoulder joint to elbow) passes within ArmSkinNear of the camera it shrinks away fast, and comes
    // back only once it has stayed clear for ArmSkinNearHold s (so it doesn't pulse with every stride).
    // Where the skin's bones are: the shadow body's (same component-space pose) moved by the gap between the two
    // components' translations (actor space, turned by the pawn's yaw). Shrunk arms are measured un-shrunk (the
    // shoulder scale shrinks them toward the shoulder joint; last frame's scale is known).
    static float hideAmt[2] = { 0.0f, 0.0f }, lastK[2] = { 1.0f, 1.0f };
    static DWORD clearSince[2] = { 0, 0 }, lastT = 0, lastLog = 0;
    static bool wasNear[2] = { false, false };            // trip log: rising edge of tooNear
    static DWORD lastTrip[2] = { 0, 0 };
    static float tripGap[2] = { 0.0f, 0.0f };             // ms between the last two trips (hold below)
    static float minU[2] = { 999.0f, 999.0f }, minF[2] = { 999.0f, 999.0f };   // closest since the last near-camera line
    DWORD nowT = GetTickCount();
    static LONGLONG qpc10 = 0;
    float dt = QpcDt(qpc10);   // (frame-accurate; was GetTickCount steps)
    lastT = nowT;
    float pyaw = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
    FVector dT{ t.X - g_shadowBodyT.X, t.Y - g_shadowBodyT.Y, t.Z - g_shadowBodyT.Z };
    FVector off{ dT.X * cosf(pyaw) - dT.Y * sinf(pyaw), dT.X * sinf(pyaw) + dT.Y * cosf(pyaw), dT.Z };
    g_skinOffWorld = off;   // (no longer read by the reach: UpdateArmSkinReach aims through g_skinFrame)
    auto segDist = [](const FVector& c, const FVector& a, const FVector& b) {
        FVector ab{ b.X - a.X, b.Y - a.Y, b.Z - a.Z }, ac{ c.X - a.X, c.Y - a.Y, c.Z - a.Z };
        float l2 = ab.X * ab.X + ab.Y * ab.Y + ab.Z * ab.Z;
        float u = l2 > 1e-4f ? Clamp((ac.X * ab.X + ac.Y * ab.Y + ac.Z * ab.Z) / l2, 0.0f, 1.0f) : 0.0f;
        FVector d{ ac.X - ab.X * u, ac.Y - ab.Y * u, ac.Z - ab.Z * u };
        return sqrtf(d.X * d.X + d.Y * d.Y + d.Z * d.Z);
    };
    bool busy[2] = { g_leftHandBusy, g_rightHandBusy };
    float nearD[2] = { -1.0f, -1.0f }, nearU[2] = { -1.0f, -1.0f }, nearF[2] = { -1.0f, -1.0f };
    // Where a skin bone is drawn: the shadow body's pose (this frame's SpaceBases) x the skin's own placement - not
    // GetBoneLocation on the shadow body, whose LocalToWorld trails while moving (20:25: guard said 28+ uu sprinting
    // while the forearm filled the view).
    auto skinBone = [&](const char* bone, FVector& out) {
        Xf a;
        if (haveSkinFrame && BoneAtomXf(g_shadowBody, bone, a)) {
            Xf w = XfMul(a, skinFrame);
            out = FVector{ w.t[0], w.t[1], w.t[2] };
            return true;
        }
        FVector p = g_shadowBody->GetBoneLocation(FName(bone), 0);
        out = FVector{ p.X + off.X, p.Y + off.Y, p.Z + off.Z };
        return false;
    };
    for (int side = 0; side < 2; ++side) {
        FName lower(side == 0 ? "lower_arm_L_jnt" : "lower_arm_R_jnt");
        FName shoulder(side == 0 ? "shoulder_L_jnt" : "shoulder_R_jnt");
        if (auto* ctl = static_cast<USkelControlSingleBone*>(g_body->FindSkelControl(lower))) {
            ctl->BoneScale = 0.001f;
            ctl->ControlStrength = ctl->StrengthTarget = 1.0f;
        }
        // The whole old arm, from the shoulder (user: old fingers showed by the armpits looking down).
        // Slide (08:50 clip, looking straight down): the skin's forearm + hand folded over the chest looked like a
        // floating sleeve - the skin has nothing above the elbow. A free arm's shoulder + upper arm grow back in with the
        // slide (the lower arm stays shrunk at the elbow, under the skin's forearm), so the sleeve joins the shoulder.
        if (auto* ctl = static_cast<USkelControlSingleBone*>(g_body->FindSkelControl(shoulder))) {
            float up = (g_cfg.slideShowUpperArms && !busy[side]) ? Clamp((g_slideAmount - 0.2f) / 0.3f, 0.0f, 1.0f) : 0.0f;
            ctl->BoneScale = 0.001f + 0.999f * up;
            ctl->ControlStrength = ctl->StrengthTarget = 1.0f;
        }
        bool tooNear = false;
        if (!busy[side] && lastK[side] > 0.005f && !(g_cfg.armSkinHandoff && g_weaponRaise[side] > 0.02f)) {   // (not mid-handover)   // 0.015 floor measured (1 - 0.985f*1 = 0.0149999857f failed >= 0.015f); busy 0.001 skipped
            // 19:34: measured on the skin itself when its bones read back (see the closed loop above) - the model's
            // positions said 25+ uu while the left sleeve went through the camera. Forearm included (E to hand).
            FVector S, U, E, H;
            skinBone(side == 0 ? "shoulder_L_jnt" : "shoulder_R_jnt", S);
            skinBone(kUpperArmBones[side], U);
            skinBone(side == 0 ? "lower_arm_L_jnt" : "lower_arm_R_jnt", E);
            skinBone(kHandBones[side], H);
            float inv = 1.0f / lastK[side];
            auto unshrink = [&](const FVector& p) {
                return FVector{ S.X + (p.X - S.X) * inv, S.Y + (p.Y - S.Y) * inv, S.Z + (p.Z - S.Z) * inv };
            };
            U = unshrink(U); E = unshrink(E); H = unshrink(H);
            float dU = segDist(g_viewLoc, U, E), dF = segDist(g_viewLoc, E, H);
            nearD[side] = dU < dF ? dU : dF;
            nearU[side] = dU; nearF[side] = dF;
            if (dU < minU[side]) minU[side] = dU;
            if (dF < minF[side]) minF[side] = dF;
            float limF = g_cfg.armSkinNearForearm + (g_cfg.armSkinNearForearmSprint - g_cfg.armSkinNearForearm) * g_sprintBackAmount;
            float limU = g_cfg.armSkinNear;
            float nearScale = 1.0f - (1.0f - g_cfg.slideArmNearScale) * g_slideAmount;   // (slide: the bracing hands are close by design)
            if (g_climbHandsOn) nearScale *= 1.0f - (1.0f - g_cfg.climbArmNearScale) * g_climbAmount;   // (climbing: hands on the chain at the face)
            limF *= nearScale; limU *= nearScale;
            tooNear = dU < limU || dF < limF;
            if (tooNear && !wasNear[side]) {   // the frame that hides the arm (was lost in the 500 ms throttle)
                Xf vw = XfFromRotator(g_viewRot, g_viewLoc);
                auto vs = [&](const FVector& p, float* o) {   // view space: forward, right, up
                    FVector d{ p.X - g_viewLoc.X, p.Y - g_viewLoc.Y, p.Z - g_viewLoc.Z };
                    for (int i = 0; i < 3; ++i) o[i] = d.X * vw.m[i][0] + d.Y * vw.m[i][1] + d.Z * vw.m[i][2];
                };
                float e3[3], h3[3];
                vs(E, e3); vs(H, h3);
                FVector pv = pawn->Velocity;
                if (lastTrip[side] && nowT - lastTrip[side] < 1500) tripGap[side] = static_cast<float>(nowT - lastTrip[side]);
                Log("arm skin near TRIP: %s %s (upper %.1f fore %.1f, limits %.0f / %.0f) | elbow view fwd/right/up %.0f %.0f %.0f, "
                    "hand %.0f %.0f %.0f | %.2f s since last trip | pitch %.0f fwd speed %.0f sprint %.2f air %.2f",
                    side ? "right" : "left", dU < limU ? "upper arm" : "forearm", dU, dF, limU, limF,
                    e3[0], e3[1], e3[2], h3[0], h3[1], h3[2], lastTrip[side] ? (nowT - lastTrip[side]) / 1000.0f : -1.0f,
                    g_viewPitchDeg, pv.X * cosf(pyaw) + pv.Y * sinf(pyaw), g_sprintBackAmount, air);
                lastTrip[side] = nowT;
            }
        }
        wasNear[side] = tooNear;
        if (tooNear || busy[side]) clearSince[side] = 0;
        else if (!clearSince[side]) clearSince[side] = nowT;
        // Trips that recur every stride: stay away for a whole measured trip interval instead of flashing back between strides.
        float holdMs = g_cfg.armSkinNearHold * 1000.0f;
        if (lastTrip[side] && nowT - lastTrip[side] < 1500 && tripGap[side] * 1.15f > holdMs) holdMs = tripGap[side] * 1.15f;
        bool hold = !busy[side] && (tooNear || (clearSince[side] && nowT - clearSince[side] < holdMs));
        if (hideAmt[side] <= 0.0f && !tooNear) hold = false;   // nothing to hold back
        if (g_cfg.armSkinHandoff && g_weaponRaise[side] > 0.02f) { hold = false; clearSince[side] = 0; }   // not mid-handover
        hideAmt[side] += Clamp((hold ? 1.0f : 0.0f) - hideAmt[side], -dt * g_cfg.reappearSpeed, dt * 20.0f);
        // Handover with the game's 1P arm (user 22:57: on holstering the arms "appeared out of nowhere"). While the
        // shadow arm's reach for the 1P hand (g_weaponRaise -> UpdateArmsShadow IK) is still blending - rising on a draw,
        // lowering after a holster - this arm stays shown and follows it, so it takes over from / hands over to the game's
        // arm right where that arm is. A busy arm hides once the reach is (nearly) complete.
        // One arm on screen at a time, decided by where the game's hand is (log 23:00: the sword holster runs while the
        // hand is still 'busy' and flips straight to Empty ~1 s later - hiding by reach amount left no arm in between).
        // Busy (or being drawn): the game's arm, unless its hand has gone off the bottom of the view (150 ms: attacks dip
        // out briefly) - then ours, already at that hand (UpdateArmSkinReach). Just freed: the game's arm while its hand is
        // still in view (1 s), then ours from that hand back to the side.
        bool hideBusy;
        bool busyNow = busy[side] || g_handDrawing[side];
        // (23:40 clip: holding a weapon and looking UP the 1P hand drops under the bottom edge - ours then reached past the
        // camera for it: big sleeves across the top of the view. Taking over is only for the start of a draw, and never
        // looking up.)
        static DWORD busySince[2] = { 0, 0 };
        if (!busyNow) busySince[side] = 0; else if (!busySince[side]) busySince[side] = nowT;
        bool drawWindow = busyNow && nowT - busySince[side] < static_cast<DWORD>(g_cfg.armSkinDrawWindow * 1000.0f);
        bool lookingUp = g_viewPitchDeg > g_cfg.armSkinReachMaxPitch;
        if (!g_cfg.armSkinHandoff || !g_cfg.shadowWeaponArms) hideBusy = busy[side];   // no reach: the old rule
        else if (busyNow) hideBusy = !(drawWindow && !lookingUp && g_onePHandBelow[side] && g_onePHandOffMs[side] > 150);
        else hideBusy = g_weaponRaise[side] > 0.02f && g_onePHandOnScreen[side] && g_handFreeMs[side] < 1000 &&
                        JumpArmAmount(side) <= 0.05f;
        float k = hideBusy ? 0.001f : 1.0f - 0.985f * hideAmt[side];
        g_skinArmShown[side] = !hideBusy && k > 0.05f;
        if (auto* ctl = static_cast<USkelControlSingleBone*>(g_shadowBody->FindSkelControl(shoulder))) {
            if (k < 0.999f) {
                ctl->BoneScale = k;
                ctl->ControlStrength = ctl->StrengthTarget = 1.0f;
            }
        }
        lastK[side] = k < 0.999f ? k : 1.0f;
    }
    // Diagnostic (19:46: still "not centred" sprinting/jumping, but the arms measured centred on the body): how the view
    // is turned/tilted against the pawn, and where each hand should be on screen by the model (shadow pose + gap),
    // x/y -1..1 (right/up) - compare with the clip.
    {
        static DWORD lastV = 0;
        FVector v = pawn->Velocity;
        if ((air > 0.3f || v.X * v.X + v.Y * v.Y > 300.0f * 300.0f) && nowT - lastV > 250) {
            lastV = nowT;
            Xf view = XfFromRotator(g_viewRot, g_viewLoc);
            float th = tanf(g_viewFov * 0.5f * 0.0174533f) * (4.0f / 3.0f);   // POV.FOV is the 4:3 FOV; 16:9 is wider (20:38 overlay)
            auto screen = [&](const FVector& p, float& x, float& y) {
                FVector d{ p.X - g_viewLoc.X, p.Y - g_viewLoc.Y, p.Z - g_viewLoc.Z };
                float f = d.X * view.m[0][0] + d.Y * view.m[0][1] + d.Z * view.m[0][2];
                float r = d.X * view.m[1][0] + d.Y * view.m[1][1] + d.Z * view.m[1][2];
                float u = d.X * view.m[2][0] + d.Y * view.m[2][1] + d.Z * view.m[2][2];
                if (f < 1.0f) { x = y = 9.0f; return; }   // behind the camera
                x = r / (f * th); y = u / (f * th * 0.5625f);
            };
            float lx, ly, rx, ry;
            FVector hl, hr;
            skinBone(kHandBones[0], hl);
            skinBone(kHandBones[1], hr);
            screen(hl, lx, ly);
            screen(hr, rx, ry);
            // How far the old reading (shadow body GetBoneLocation + gap) is from the drawn hand, and along the motion.
            FVector old = g_shadowBody->GetBoneLocation(FName(kHandBones[0]), 0);
            FVector dd{ old.X + off.X - hl.X, old.Y + off.Y - hl.Y, old.Z + off.Z - hl.Z };
            float sp = sqrtf(v.X * v.X + v.Y * v.Y);
            float along = sp > 1.0f ? (dd.X * v.X + dd.Y * v.Y) / sp : 0.0f;
            int dy = static_cast<int16_t>((g_viewRot.Yaw - pawn->Rotation.Yaw) & 0xFFFF);
            int roll = static_cast<int16_t>(g_viewRot.Roll & 0xFFFF);
            Log("arm skin view: camera turned %.1f deg right of the body, rolled %.1f, pitch %.0f | on screen: L hand %.2f %.2f  "
                "R hand %.2f %.2f | old reading off by %.1f uu (%.1f along the motion) | speed %.0f air %.2f",
                dy * (360.0f / 65536.0f), roll * (360.0f / 65536.0f), g_viewPitchDeg, lx, ly, rx, ry,
                sqrtf(dd.X * dd.X + dd.Y * dd.Y + dd.Z * dd.Z), along, sp, air);
        }
    }
    if ((nearD[0] >= 0.0f && nearD[0] < g_cfg.armSkinNear + 10.0f) || (nearD[1] >= 0.0f && nearD[1] < g_cfg.armSkinNear + 10.0f)) {
        if (nowT - lastLog > 500) {
            lastLog = nowT;
            FVector v = pawn->Velocity;
            Log("arm skin near camera: left upper %.0f fore %.0f, right upper %.0f fore %.0f (limits %.0f / %.0f) hidden %.2f/%.2f | "
                "min since last line L %.0f/%.0f R %.0f/%.0f | pitch %.0f speed %.0f sprint %.2f air %.2f stair lift %.0f",
                nearU[0], nearF[0], nearU[1], nearF[1], g_cfg.armSkinNear,
                g_cfg.armSkinNearForearm + (g_cfg.armSkinNearForearmSprint - g_cfg.armSkinNearForearm) * g_sprintBackAmount, hideAmt[0],
                hideAmt[1], minU[0], minF[0], minU[1], minF[1], g_viewPitchDeg, sqrtf(v.X * v.X + v.Y * v.Y), g_sprintBackAmount, air, g_cameraDip);
            minU[0] = minU[1] = minF[0] = minF[1] = 999.0f;
        }
    }
    // Placement check: the engine's own hand positions (parent pose x the skin's LocalToWorld = what is drawn) vs the
    // model (skinBone). With bTransformFromAnimParent off they should agree (standing ~0, moving ~one frame of motion).
    {
        static DWORD lastP = 0;
        if (haveSkinFrame && nowT - lastP > 2000) {
            lastP = nowT;
            float cp = cosf(pyaw), sn = sinf(pyaw), g[2][3];
            for (int s = 0; s < 2; ++s) {
                FVector m, e = g_armSkin->GetBoneLocation(FName(kHandBones[s]), 0);
                skinBone(kHandBones[s], m);
                FVector d{ e.X - m.X, e.Y - m.Y, e.Z - m.Z };
                g[s][0] = d.X * cp + d.Y * sn; g[s][1] = -d.X * sn + d.Y * cp; g[s][2] = d.Z;
            }
            USkeletalMeshComponent* a1 = pawn->Mesh;
            FVector v = pawn->Velocity;
            Log("arm skin placement check: engine - model (fwd/right/up) L hand %.1f %.1f %.1f, R hand %.1f %.1f %.1f | guard L %.0f/%.0f "
                "R %.0f/%.0f | skin T (%.1f %.1f %.1f) body T (%.1f %.1f %.1f) shadow T (%.1f %.1f %.1f) 1P T (%.1f %.1f %.1f) | "
                "inherit %d/%d fov %.1f speed %.0f", g[0][0], g[0][1], g[0][2], g[1][0], g[1][1], g[1][2], nearU[0], nearF[0], nearU[1],
                nearF[1], g_armSkin->Translation.X, g_armSkin->Translation.Y, g_armSkin->Translation.Z, g_body->Translation.X,
                g_body->Translation.Y, g_body->Translation.Z, g_shadowBodyT.X, g_shadowBodyT.Y, g_shadowBodyT.Z,
                a1 ? a1->Translation.X : 0.0f, a1 ? a1->Translation.Y : 0.0f, a1 ? a1->Translation.Z : 0.0f,
                g_armSkin->bTransformFromAnimParent, g_armSkin->m_bReallyInheritTransformFromAnimParent, g_viewFov,
                sqrtf(v.X * v.X + v.Y * v.Y));
        }
    }
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
        static LONGLONG qpc12 = 0;
        float dtC = QpcDt(qpc12);   // (frame-accurate; was GetTickCount steps)
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
        static bool wasBusyW[2] = { false, false };   // busy -> free edge per hand (post-holster hold below)
        static DWORD freeAtW[2] = { 0, 0 };
        DWORD nowW = GetTickCount();
        float dtW = lastW ? Clamp((nowW - lastW) / 1000.0f, 0.0f, 0.1f) : 0.0f;
        lastW = nowW;
        bool busy[2] = { g_leftHandBusy, g_rightHandBusy };
        for (int h = 0; h < 2; ++h)
        {
            // Lowering after a holster runs slower while the arm skin shows it (ArmSkinLowerTime): the drawn arm comes
            // down from where the game's arm was instead of popping in at the side (user 22:57).
            float fall = (g_armSkinOn && g_cfg.armSkinHandoff && g_cfg.armSkinLowerTime > 0.05f) ? 1.0f / g_cfg.armSkinLowerTime : 5.0f;
            // ...and only once the game's arm has left the view (it is still putting the item away): ours then starts at
            // that hand and comes back to the side. Only for 1 s after the holster and not under the jump pose - the game's
            // holstered sprint/jump animations bring a 1P hand into view too, which held the reach for the whole sprint/jump.
            if (wasBusyW[h] && !busy[h]) freeAtW[h] = nowW;
            wasBusyW[h] = busy[h];
            g_handFreeMs[h] = busy[h] ? 0u : freeAtW[h] ? nowW - freeAtW[h] : 0x7FFFFFFFu;
            if (g_armSkinOn && g_cfg.armSkinHandoff && !busy[h] && g_onePHandOnScreen[h] && g_handFreeMs[h] < 1000 &&
                JumpArmAmount(h) <= 0.05f) fall = 0.0f;
            g_weaponRaise[h] += Clamp(((busy[h] || g_handDrawing[h]) && g_cfg.shadowWeaponArms ? 1.0f : 0.0f) - g_weaponRaise[h],
                                      -dtW * fall, dtW * 5.0f);
        }
    }
    // Both bodies get the same pose controls; only the visible one hides its coat and busy arms. The shadow body
    // keeps them, so the shadow always has a coat and (fake) arms.
    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
    for (int ci = 0; ci < 4; ++ci) {
    USkeletalMeshComponent* comp = comps[ci];
    if (!comp) continue;
    bool isShadow = ci >= 1;
    // (14:08 clip: with the casting body's hand IK following the real arm, the fixed ShadowArmPose shoulder/elbow raise
    // on top of it hunched the shoulders - it is off for that body; UpdateCastingArmReach owns its weapon arm.)
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
            float weapon = (isShadow && comp != g_holsterCast && !g_cfg.armIK && !g_cfg.fpArmShadow && !(g_armSkinOn && g_cfg.armSkinHandoff)) ? g_weaponRaise[b == 6 ? 0 : 1] : 0.0f;   // (skin on: UpdateArmSkinReach poses it)
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
            // (the arm skin copies the shadow body's pose - it needs the swing too, or looking down the arms stayed out
            // of view, 17:10)
            bool forward = !hide && (!isShadow || g_armSkinOn) && g_cfg.armForwardDeg != 0.0f && down > 0.01f;
            float fwdDeg = forward ? g_cfg.leanSign * g_cfg.armForwardDeg * down * (1.0f - g_cfg.armForwardSprintCut * g_sprintBackAmount) : 0.0f;
            float rollDeg = spread ? side * g_cfg.armSpreadDeg * crouchAmount : 0.0f;
            // Standing / walking: a little away from the body (left arm left, right arm right) so the hands clear the coat.
            if (!hide && g_cfg.armStandSpreadDeg != 0.0f) { rollDeg += side * g_cfg.armStandSpreadDeg * (1.0f - crouchAmount); spread = true; }
            // Jumping/falling with that hand free: the arm floats up in front (user reference), back to the side on landing.
            float jumpArm = hide ? 0.0f : JumpArmAmount(b == 4 ? 0 : 1);
            if (jumpArm > 0.0f) {
                // ...and moves with the body (jump hands): lifts as you fall, trails sideways moves and turns.
                float sway = g_cfg.jumpHands ? 1.0f : 0.0f;
                fwdDeg += g_cfg.leanSign * (g_cfg.jumpArmRaise + sway * g_jumpSwayRaise) * jumpArm;
                rollDeg = rollDeg * (1.0f - jumpArm) + (-side * g_cfg.jumpArmIn + sway * g_jumpSwayRoll) * jumpArm;
                forward = true;
            }
            // Shadow body: an arm whose hand holds a weapon/power is raised forward like the 1P arm (the shadow
            // kept the idle arms-down pose with weapons out, user screenshot), and pulled in a little.
            float weapon = (isShadow && comp != g_holsterCast && !g_cfg.armIK && !g_cfg.fpArmShadow && !(g_armSkinOn && g_cfg.armSkinHandoff)) ? g_weaponRaise[b == 4 ? 0 : 1] : 0.0f;   // (skin on: UpdateArmSkinReach poses it)
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
    bool busy[3] = { false, false, false }, holstered[3] = { false, false, false }, unseen[3] = { false, false, false };
    int meshState[3] = { -1, -1, -1 };   // item's 1P mesh: -1 none, else attached*2 + visible
    std::string names[3];
    for (int usage = 1; usage <= 2; ++usage) {
        int slot = inv->m_EquipUsageInfo[usage].m_iEquippedSlot;
        if (slot < 0 || slot >= slots->Num || !slots->Data) continue;
        auto* item = *reinterpret_cast<UObject**>(static_cast<uint8_t*>(slots->Data) + slot * 12);
        if (!item || !item->Class) continue;
        names[usage] = item->Class->GetName();
        // An item still equipped but put away (UDishonoredInventoryItem::m_CurSocket = ItemSocket_Holstered) leaves the
        // hand free too (20:31 clip: sword holstered but still the equipped item - the body's right arm hid for a 1P arm
        // that wasn't there, so there was no right arm at all).
        auto* invItem = item->IsA(UDishonoredInventoryItem::StaticClass()) ? static_cast<UDishonoredInventoryItem*>(item) : nullptr;
        holstered[usage] = invItem && invItem->m_CurSocket == EItemSocket::ItemSocket_Holstered;
        // Equipped and "in hand" but its first-person model isn't showing (21:54 clip: the sword stayed the equipped item,
        // never holstered, while no 1P sword arm was drawn - the body's right arm hid for an arm that wasn't there).
        // Items without a 1P model (powers) can't be judged this way and keep the old rule.
        if (invItem && invItem->m_pPlayerMesh) {
            UPrimitiveComponent* pm = invItem->m_pPlayerMesh;
            meshState[usage] = (pm->bAttached ? 2 : 0) + (pm->HiddenGame ? 0 : 1);
            unseen[usage] = !pm->bAttached || pm->HiddenGame;
        }
        busy[usage] = names[usage].find("Empty") == std::string::npos && !holstered[usage] && !unseen[usage];
        g_handItem[usage == 2 ? 0 : 1] = busy[usage] ? item : nullptr;
    }
    for (int usage = 1; usage <= 2; ++usage) if (!busy[usage]) g_handItem[usage == 2 ? 0 : 1] = nullptr;
    // Quiet hands (18:4x, user: drew the crossbow, then could neither fire nor switch - Daud's package finished 0.1 s into
    // the crossbow's draw and the character swap replaced the 1P arms mesh mid-animation): what the hands hold, and the
    // newest animation of each held item (its anim state component's sequence history), so the swap can wait for idle.
    {
        static UObject* lastItem[3] = {};
        static int lastMesh[3] = { -2, -2, -2 };
        static bool lastBusy[3] = {};
        static std::string lastSeq[3];
        static DWORD lastSeqLog = 0;
        std::string seq[3];
        bool allIdle = true, changed = false;
        for (int usage = 1; usage <= 2; ++usage) {
            int slot = inv->m_EquipUsageInfo[usage].m_iEquippedSlot;
            UObject* item = (slot >= 0 && slot < slots->Num && slots->Data) ? *reinterpret_cast<UObject**>(static_cast<uint8_t*>(slots->Data) + slot * 12) : nullptr;
            if (item != lastItem[usage] || meshState[usage] != lastMesh[usage] || busy[usage] != lastBusy[usage]) changed = true;
            lastItem[usage] = item; lastMesh[usage] = meshState[usage]; lastBusy[usage] = busy[usage];
            if (!item || !busy[usage] || !item->IsA(UDishonoredInventoryItem::StaticClass())) continue;
            UDisAnimStateComponent* asc = static_cast<UDishonoredInventoryItem*>(item)->m_pAnimStateCompPlayer;
            if (!asc || IsBadReadPtr(asc, sizeof(UDisAnimStateComponent))) continue;
            int i = asc->m_iCurNewestSeqInHistory;
            if (i < 0 || i >= 12) continue;
            const FName& n = asc->m_AnimSeqHistory[i].m_SeqName;
            if (n.FNameEntryId <= 0 || !n.IsValid()) continue;
            seq[usage] = n.ToString();
            std::string low = seq[usage];
            for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (low.find("idle") == std::string::npos) allIdle = false;
        }
        if (changed) g_handsChangedTick = GetTickCount();
        if (seq[1] != lastSeq[1] || seq[2] != lastSeq[2]) {
            g_handSeqChangedTick = GetTickCount();
            if (GetTickCount() - lastSeqLog > 250) {
                lastSeqLog = GetTickCount();
                Log("hands anim: right %s, left %s%s", seq[1].empty() ? "-" : seq[1].c_str(), seq[2].empty() ? "-" : seq[2].c_str(), allIdle ? " (idle)" : "");
            }
            lastSeq[1] = seq[1]; lastSeq[2] = seq[2];
        }
        g_handsAllIdle = allIdle;
    }
    // What each hand holds, for the shadow arm pose: 1 sword, 2 gun (pistol/crossbow), 3 power or other.
    for (int usage = 1; usage <= 2; ++usage) {
        const std::string& n = names[usage];
        int kind = !busy[usage] ? 0 : n.find("Sword") != std::string::npos ? 1
                 : (n.find("Pistol") != std::string::npos || n.find("Crossbow") != std::string::npos) ? 2 : 3;
        g_handKind[usage == 2 ? 0 : 1] = kind;
    }
    if (busy[1] != g_rightHandBusy || busy[2] != g_leftHandBusy)
        Log("hands: right %s (%s%s%s, 1P mesh %d), left %s (%s%s%s, 1P mesh %d)", busy[1] ? "busy" : "free", names[1].c_str(),
            holstered[1] ? ", holstered" : "", unseen[1] ? ", not shown" : "", meshState[1], busy[2] ? "busy" : "free",
            names[2].c_str(), holstered[2] ? ", holstered" : "", unseen[2] ? ", not shown" : "", meshState[2]);
    g_rightHandBusy = busy[1];
    g_leftHandBusy = busy[2];
    static DWORD drawSince[3] = { 0, 0, 0 };   // (log 23:00: the sword turns busy ~240 ms into its draw)
    for (int usage = 1; usage <= 2; ++usage) {
        bool d = !busy[usage] && unseen[usage] && !holstered[usage] && !names[usage].empty() &&
                 names[usage].find("Empty") == std::string::npos;
        if (!d) drawSince[usage] = 0; else if (!drawSince[usage]) drawSince[usage] = GetTickCount();
        g_handDrawing[usage == 2 ? 0 : 1] = d && GetTickCount() - drawSince[usage] < 600;
    }
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
// A bone's pose in its mesh's own space: SpaceBases only (the FBoneAtom quat/translation/scale), no mesh origin and no
// LocalToWorld. (20:25: the hidden shadow body's LocalToWorld lags while moving - its GetBoneLocation trailed the
// drawn arm skin by tens of uu sprinting - so the skin's bones are built from this x our own placement instead.)
static bool BoneAtomXf(USkeletalMeshComponent* comp, const char* bone, Xf& out) {
    if (!comp || !comp->SkeletalMesh) return false;
    FName name(bone);
    if (name.FNameEntryId < 0) return false;
    int idx = comp->MatchRefBone(name);
    auto* bases = reinterpret_cast<RawArray*>(&comp->SpaceBases);
    if (idx < 0 || !bases->Data || idx >= bases->Num) return false;
    const float* a = reinterpret_cast<const float*>(static_cast<uint8_t*>(bases->Data) + idx * 32);
    float x = a[0], y = a[1], z = a[2], w = a[3], sc = a[7];
    if (!(sc > 0.0f && sc < 100.0f)) sc = 1.0f;
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;
    out.m[0][0] = (1.0f - (yy + zz)) * sc; out.m[0][1] = (xy + wz) * sc;          out.m[0][2] = (xz - wy) * sc;
    out.m[1][0] = (xy - wz) * sc;          out.m[1][1] = (1.0f - (xx + zz)) * sc; out.m[1][2] = (yz + wx) * sc;
    out.m[2][0] = (xz + wy) * sc;          out.m[2][1] = (yz - wx) * sc;          out.m[2][2] = (1.0f - (xx + yy)) * sc;
    out.t[0] = a[4]; out.t[1] = a[5]; out.t[2] = a[6];
    return true;
}

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
        // (21:55: HideBone is physics-only in this game and UnHideBone enables collision bodies - not called. The copy is
        // scaled to nothing when no hand is busy, below.)
        (void)bone;
        hidden[h] = want;
    }
}

// Is the game's own 1P hand in view? (handover: one arm on screen at a time.) Called right after UpdateHands so UpdateCoat
// and UpdateArmSkin read this frame's answer. Wrist, middle fingertip and mid-forearm are tested (fingers reach ~10 uu past
// the wrist) against the frame + 2 uu; once in view a hand stays "in" until 5 uu out (no edge flicker).
static void UpdateOnePHandOnScreen(ADishonoredPlayerPawn* pawn) {
    static DWORD offSince[2] = { 0, 0 };
    DWORD now = GetTickCount();
    USkeletalMeshComponent* arms = pawn ? pawn->Mesh : nullptr;
    bool ok = arms && arms->SkeletalMesh && now - g_viewLocTick < 500;   // (stale view: the rigid-camera hook bailed out)
    Xf vw = XfFromRotator(g_viewRot, g_viewLoc);
    Xf L1 = ok ? XfFromMatrix(arms->LocalToWorld) : vw;   // bone atom x LocalToWorld = GetBoneMatrix (LocalToWorld has the mesh origin)
    float tv = tanf(g_viewFov * 0.5f * 0.0174533f) * 0.75f;   // POV.FOV is the 4:3 horizontal FOV (Hor+): vertical half-tangent
    static float aspect = 16.0f / 9.0f;
    RECT rc;
    HWND wnd = GetActiveWindow();
    if (wnd && GetClientRect(wnd, &rc) && rc.bottom > 0 && rc.right > 0) aspect = static_cast<float>(rc.right) / rc.bottom;
    float th = tv * aspect;
    // (23:44 clip: two right hands in a draw - the 1P arms are drawn relative to the camera, not where their LocalToWorld
    // puts them; the reach log said 0.1 uu while the hands were a hand-width apart. Each 1P point is taken relative to the
    // 1P camera_jnt and put at the real view point - the arm-shadow trick, as UpdateClimbArms does.)
    Xf camA;
    bool haveCam = ok && BoneAtomXf(arms, "camera_jnt", camA);
    Xf camW = haveCam ? XfMul(camA, L1) : vw;
    auto vis = [&](const Xf& wx) {
        return haveCam ? FVector{ g_viewLoc.X + wx.t[0] - camW.t[0], g_viewLoc.Y + wx.t[1] - camW.t[1], g_viewLoc.Z + wx.t[2] - camW.t[2] }
                       : FVector{ wx.t[0], wx.t[1], wx.t[2] };
    };
    for (int s = 0; s < 2; ++s) {
        bool in = false, below = false;
        FVector pts[3];
        int n = 0;
        Xf a, w, e;
        if (ok && BoneAtomXf(arms, kHandBones[s], a)) {
            w = XfMul(a, L1);
            pts[n++] = vis(w);
            if (BoneAtomXf(arms, kFingerBones[s][5], a)) { e = XfMul(a, L1); pts[n++] = vis(e); }
            if (BoneAtomXf(arms, kLowerArmBones[s], a)) {
                e = XfMul(a, L1);
                FVector pw = vis(w), pe = vis(e);
                pts[n++] = FVector{ (pw.X + pe.X) * 0.5f, (pw.Y + pe.Y) * 0.5f, (pw.Z + pe.Z) * 0.5f };
            }
        }
        float m = g_onePHandOnScreen[s] ? 5.0f : 2.0f;
        for (int i = 0; i < n; ++i) {
            FVector d{ pts[i].X - g_viewLoc.X, pts[i].Y - g_viewLoc.Y, pts[i].Z - g_viewLoc.Z };
            float f = d.X * vw.m[0][0] + d.Y * vw.m[0][1] + d.Z * vw.m[0][2];
            float r = d.X * vw.m[1][0] + d.Y * vw.m[1][1] + d.Z * vw.m[1][2];
            float u = d.X * vw.m[2][0] + d.Y * vw.m[2][1] + d.Z * vw.m[2][2];
            if (f > 1.0f && fabsf(r) < f * th + m && fabsf(u) < f * tv + m) in = true;
            if (i == 0) below = u < 0.0f && (f <= 1.0f || -u > f * tv);   // the wrist: under the bottom edge (or behind the view)
        }
        if (in) offSince[s] = 0; else if (!offSince[s]) offSince[s] = now;
        g_onePHandOnScreen[s] = in;
        g_onePHandBelow[s] = !in && below;
        g_onePHandOffMs[s] = in ? 0 : now - offSince[s];
    }
}

static float g_armsFov = 0.0f;   // the camera's arms FOV (m_fCurFOV_Arms; diagnostic, HookedGetPlayerViewPoint)

// Handover (user 23:01: "make the unholstered arms look like the same arms as the holstered ones"): while the reach blends
// (g_weaponRaise) the shadow body's arm - which the arm skin draws - goes to the game's own 1P hand: position, rotation,
// elbow toward the 1P elbow. Aimed in the shadow body's component space through the frame the skin is drawn with
// (g_skinFrame; the placement check confirms it): world/actor-space targets go through the shadow body's own LocalToWorld.
// Runs after UpdateArmIK / UpdateArmsShadow / PlaceJumpArm, with or without the arm shadow (PlayerShadows=0 turns that off
// - the 23:01 build ran none of the reach).
static void UpdateArmSkinReach(ADishonoredPlayerPawn* pawn) {
    if (!g_shadowBody) return;
    USkeletalMeshComponent* arms = pawn ? pawn->Mesh : nullptr;
    Xf inv = g_skinFrame, L1 = g_skinFrame;
    bool on = g_armSkinOn && g_cfg.armSkinHandoff && g_skinFrameOk && arms && arms->SkeletalMesh && XfInv(g_skinFrame, inv);
    if (on) L1 = XfFromMatrix(arms->LocalToWorld);
    // Where the 1P arms are DRAWN: relative to their camera_jnt, at the real view point (see UpdateOnePHandOnScreen).
    Xf camA, camW = L1;
    bool haveCam = on && BoneAtomXf(arms, "camera_jnt", camA) && GetTickCount() - g_viewLocTick < 500;
    if (haveCam) camW = XfMul(camA, L1);
    auto drawn = [&](Xf wx) {
        if (haveCam) { wx.t[0] += g_viewLoc.X - camW.t[0]; wx.t[1] += g_viewLoc.Y - camW.t[1]; wx.t[2] += g_viewLoc.Z - camW.t[2]; }
        return wx;
    };
    for (int h = 0; h < 2; ++h) {
        auto* ik = static_cast<USkelControlLimb*>(g_shadowBody->FindSkelControl(FName(kHandBones[h])));
        if (!ik) continue;
        ik->JointTargetLocationSpace = EBoneControlSpace::BCS_ActorSpace;   // UpdateArmIK wrote the actor-space pole this frame
        if (!on) continue;
        // A hidden side gets no reach at all (user 23:44, "two sets of arms ... the playermodel arms behind the weapon holding
        // arms" the whole time a weapon was out): the side is hidden by shrinking the shoulder (UpdateArmSkin), and the limb
        // IK rebuilds upper arm / forearm / hand from the reference bone lengths - a full-size arm again, reaching the 1P hand
        // from behind it. (PlayerShadows=0 turns fpArmShadow off, so the old skip never ran.) The reach comes back the frame
        // the side shows (UpdateArmSkin runs first), already at the 1P hand.
        if (!g_skinArmShown[h]) {
            ik->ControlStrength = ik->StrengthTarget = 0.0f;
            if (auto* wrist = static_cast<USkelControlSingleBone*>(g_shadowBody->FindSkelControl(FName(kWristControlNames[h]))))
                if (JumpArmAmount(h) <= 0.0f) wrist->ControlStrength = wrist->StrengthTarget = 0.0f;
            continue;
        }
        float jump = JumpArmAmount(h);   // (0 for a busy hand) the jump pose owns a free arm
        float s = g_weaponRaise[h] * (1.0f - jump);
        if (g_viewPitchDeg > g_cfg.armSkinReachMaxPitch) s = 0.0f;   // looking up: no reach (the arm stays at the side, out of view)
        ik->ControlStrength = ik->StrengthTarget = s;
        Xf a1;
        if (s <= 0.0f || !BoneAtomXf(arms, kHandBones[h], a1)) continue;
        Xf w1raw = XfMul(a1, L1);
        Xf c1 = XfMul(drawn(w1raw), inv);   // the 1P hand as drawn -> shadow pose space
        ik->EffectorLocationSpace = EBoneControlSpace::BCS_ComponentSpace;
        ik->EffectorLocation = FVector{ c1.t[0], c1.t[1], c1.t[2] };
        {
            static DWORD lastReachLog = 0;
            if (g_armSkin && s > 0.9f && g_skinArmShown[h] && GetTickCount() - lastReachLog > 500) {
                lastReachLog = GetTickCount();
                FVector k = g_armSkin->GetBoneLocation(FName(kHandBones[h]), 0);
                Xf w1 = drawn(w1raw);
                float dx = k.X - w1.t[0], dy = k.Y - w1.t[1], dz = k.Z - w1.t[2];
                float ox = w1.t[0] - w1raw.t[0], oy = w1.t[1] - w1raw.t[1], oz = w1.t[2] - w1raw.t[2];
                Log("arm skin reach: %s skin hand - drawn 1P hand %.1f uu (last frame's target) | drawn vs LocalToWorld %.1f uu "
                    "(camera_jnt %s) | fov world %.1f arms %.1f", h ? "right" : "left", sqrtf(dx * dx + dy * dy + dz * dz),
                    sqrtf(ox * ox + oy * oy + oz * oz), haveCam ? "found" : "MISSING", g_viewFov, g_armsFov);
            }
        }
        Xf e1;
        if (BoneAtomXf(arms, kLowerArmBones[h], e1)) {   // elbow toward the 1P elbow: the forearm comes in at its angle
            Xf ce = XfMul(drawn(XfMul(e1, L1)), inv);
            ik->JointTargetLocationSpace = EBoneControlSpace::BCS_ComponentSpace;
            ik->JointTargetLocation = FVector{ ce.t[0], ce.t[1], ce.t[2] };
        }
        // The hand turned as the 1P hand is (the skin is the 1P arms model - same bind - so the drawn hands match).
        // The wrist control runs after the IK on hand_X; UpdateJumpHands zeroes it every frame when not jumping.
        if (jump <= 0.0f) {
            if (auto* wrist = static_cast<USkelControlSingleBone*>(g_shadowBody->FindSkelControl(FName(kWristControlNames[h])))) {
                FVector l; FRotator r; float sc;
                XfDecompose(c1, l, r, sc);
                wrist->BoneRotationSpace = EBoneControlSpace::BCS_ComponentSpace;
                wrist->bAddRotation = 0;
                wrist->bApplyRotation = 1;
                wrist->BoneRotation = r;
                wrist->ControlStrength = wrist->StrengthTarget = s;
            }
        }
    }
}

// A bone's component-space rotation (its SpaceBases atom's quaternion), x y z w.
static bool BoneQuat(USkeletalMeshComponent* comp, const char* bone, float q[4]) {
    if (!comp || !comp->SkeletalMesh) return false;
    FName name(bone);
    if (name.FNameEntryId < 0) return false;
    int idx = comp->MatchRefBone(name);
    auto* bases = reinterpret_cast<RawArray*>(&comp->SpaceBases);
    if (idx < 0 || !bases->Data || idx >= bases->Num) return false;
    const float* a = reinterpret_cast<const float*>(static_cast<uint8_t*>(bases->Data) + idx * 32);
    q[0] = a[0]; q[1] = a[1]; q[2] = a[2]; q[3] = a[3];
    return true;
}

// A bone's parent-space rotation as the animation left it this frame (its LocalAtoms atom's quaternion), x y z w.
static bool BoneAnimLocalQuat(USkeletalMeshComponent* comp, const char* bone, float q[4]) {
    if (!comp || !comp->SkeletalMesh) return false;
    FName name(bone);
    if (name.FNameEntryId < 0) return false;
    int idx = comp->MatchRefBone(name);
    auto* local = reinterpret_cast<RawArray*>(&comp->LocalAtoms);
    if (idx < 0 || !local->Data || idx >= local->Num) return false;
    const float* a = reinterpret_cast<const float*>(static_cast<uint8_t*>(local->Data) + idx * 32);
    q[0] = a[0]; q[1] = a[1]; q[2] = a[2]; q[3] = a[3];
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return n > 0.9f && n < 1.1f;
}

// Bones' space -> the mesh's (component) space: the mesh Origin, then its RotOrigin - the order the skin frame uses
// (ArmSkinOriginFix; the body's is Origin (0 97 0), RotOrigin yaw -90 / roll 90, so leaving it out puts a target a
// quarter turn and ~100 uu off). Bones -> world is MeshOriginXf x component local x owner.
static Xf MeshOriginXf(USkeletalMesh* m) {
    if (!g_cfg.armSkinOriginFix) return XfFromRotator(m->RotOrigin, m->Origin);
    Xf o = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { m->Origin.X, m->Origin.Y, m->Origin.Z } };
    return XfMul(o, XfFromRotator(m->RotOrigin, FVector{ 0, 0, 0 }));
}

// A node of this class in an anim tree (depth-first; only blend nodes have children, laid out as ChildAnim reads them).
static UAnimNode* FindAnimNodeOfClass(UAnimNode* node, UClass* cls, int depth) {
    if (!node || !cls || depth > 24) return nullptr;
    if (node->Class == cls) return node;
    if (!node->IsA(UAnimNodeBlendBase::StaticClass())) return nullptr;
    auto* children = reinterpret_cast<RawArray*>(&static_cast<UAnimNodeBlendBase*>(node)->Children);
    if (!children->Data) return nullptr;
    for (int i = 0; i < children->Num && i < 64; ++i) {
        UAnimNode* c = *reinterpret_cast<UAnimNode**>(static_cast<uint8_t*>(children->Data) + i * 0x50 + 8);
        if (UAnimNode* f = FindAnimNodeOfClass(c, cls, depth + 1)) return f;
    }
    return nullptr;
}

// The sword's melee-block context (a UDisItemContext_MeleeBlockPlayer in the item's m_ContextSlots_Parry_Player): its
// m_BlockState is StartBlock 0 (also the fresh value), Blocking 1, Block_Parry 2, Block_Cancel 3 - and stays 3 after a
// block (15:10 log: state 1 only while the user blocked; the 1P arms' tree has no block node). -1: no such context.
static int BlockContextState() {
    static int lastSt = -2;
    int st = -1;
    if (g_rightHandBusy && g_handItem[1]) {
        auto* item = static_cast<UDishonoredInventoryItem*>(g_handItem[1]);
        auto* arr = reinterpret_cast<RawArray*>(&item->m_ContextSlots_Parry_Player);
        if (arr->Data && arr->Num > 0 && arr->Num <= 32) {
            for (int i = 0; i < arr->Num; ++i) {
                auto* c = static_cast<UDisItemContext**>(arr->Data)[i];
                if (c && c->IsA(UDisItemContext_MeleeBlockPlayer::StaticClass())) { st = static_cast<int>(static_cast<UDisItemContext_MeleeBlockPlayer*>(c)->m_BlockState); break; }
            }
        }
    }
    if (st != lastSt) { lastSt = st; Log("block: context state %d", st); }
    return st;
}

// Is the player blocking with the sword? The 1P arms' tree has a DisAnimNodeBlendByMeleeBlockState (the game drives it
// Intro / Loop / Cancel / Parry while blocking); its total weight in the tree is the block. Found once per tree.
// Returns -1 when the node isn't there (then the caller falls back to the out-of-view rule).
static float BlockWeight(USkeletalMeshComponent* arms) {
    static UAnimNode* node = nullptr;
    static UAnimNode* forRoot = nullptr;
    static DWORD lastTry = 0;
    if (!arms || !arms->Animations) { node = nullptr; forRoot = nullptr; return -1.0f; }
    if (forRoot != arms->Animations || (!node && GetTickCount() - lastTry > 2000)) {
        forRoot = arms->Animations;
        lastTry = GetTickCount();
        node = FindAnimNodeOfClass(arms->Animations, UDisAnimNodeBlendByMeleeBlockState::StaticClass(), 0);
        Log("block node %s on the 1P arms' tree%s", node ? "found" : "not found", node ? "" : " (the out-of-view rule stands in for the off-hand)");
    }
    return node ? node->NodeTotalWeight : -1.0f;
}

// Casting body (13:48 clip: the shadow arm held the fixed ShadowArmPose while the 1P arm swung): its weapon-side hand
// follows the 1P hand as DRAWN - the same target UpdateArmSkinReach uses (1P hand relative to its camera_jnt, at the real
// view point) - into THIS body's own component frame, at the full weapon strength and never gated by the skin's show /
// look-up rules: this body is never drawn, so it cannot double the 1P arm. The elbow pole follows the 1P elbow and the
// wrist takes the 1P hand's rotation, so the item copy riding this hand (UpdateItemShadows) sits where the real item is.
// The IK's hinge (17:28 log; 17:4x review). UE3's limb solver builds each arm bone from two directions: BoneAxis along
// the limb, and JointAxis along the NORMAL of the plane the arm bends in (desired direction x joint-target direction),
// negated by bInvertJointAxis - the hinge. JointAxis Y is the hinge on this rig (MODLOG 15:44), but which way round it
// points is the rig's (and the rig is mirrored), and PlaceJumpArm clears bInvertJointAxis every frame. With the wrong
// sign each bone is turned half round about its length: the elbow still lands where it should, but the skin blended
// between the collar bone and a half-turned upper arm collapses to a line (the notch at the shoulder, the arm "floating
// below it"), and so does the wrist between a half-turned forearm and the hand. The 17:28 log has that signature: the
// IK'd upper arm 167-180 deg from its rest rotation (a half turn composed with any swing is a half turn).
// The sign is learned off this body's own animated arm while the IK has been off for a few frames (SpaceBases are the
// last evaluation's): the bend-plane normal (H-E) x (E-S) - the solver's normal for the same bend - in the upper arm's
// frame; the sign of its Y is the rig's. Then a check on the result: at full strength an upper arm still reading a half
// turn from rest for CHECK frames flips that arm's sign once (and back once more if that reads a half turn too).
struct ArmRoll {
    float acc[3], accLow[3], weight;   // hinge normal in the upper arm's / forearm's frame, bend-angle weighted
    float bendMax;
    int samples;
    bool learned, invert;              // from the animation
    int offFrames;                     // consecutive frames with the IK off and the body in first person
    int halfTurnFrames, flips;         // the result check
    bool checkOff;
    DWORD lastLog;
};
static ArmRoll g_armRoll[2] = {};
static void ResetArmRoll() { for (auto& r : g_armRoll) r = ArmRoll{}; }

// v in the frame whose axes are the rows of X.m (a bone atom: scaled rows, so normalised here).
static void InFrame(const Xf& X, const float v[3], float out[3]) {
    for (int i = 0; i < 3; ++i) {
        float n = sqrtf(X.m[i][0] * X.m[i][0] + X.m[i][1] * X.m[i][1] + X.m[i][2] * X.m[i][2]);
        out[i] = n > 1e-6f ? (v[0] * X.m[i][0] + v[1] * X.m[i][1] + v[2] * X.m[i][2]) / n : 0.0f;
    }
}

static bool Finite3(const float v[3]) {
    for (int i = 0; i < 3; ++i) if (!(v[i] == v[i]) || fabsf(v[i]) > 1e30f) return false;
    return true;
}

static void LearnArmHinge(int h) {
    ArmRoll& r = g_armRoll[h];
    Xf S, E, H;
    if (!BoneAtomXf(g_shadowArms, kUpperArmBones[h], S) || !BoneAtomXf(g_shadowArms, kLowerArmBones[h], E) ||
        !BoneAtomXf(g_shadowArms, kHandBones[h], H)) return;
    if (!Finite3(S.t) || !Finite3(E.t) || !Finite3(H.t)) return;
    float u[3] = { E.t[0] - S.t[0], E.t[1] - S.t[1], E.t[2] - S.t[2] }, f[3] = { H.t[0] - E.t[0], H.t[1] - E.t[1], H.t[2] - E.t[2] };
    float lu = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]), lf = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (lu < 5.0f || lf < 5.0f) return;
    float n[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };   // (H-E) x (E-S)
    float ln = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    float bend = atan2f(ln, u[0] * f[0] + u[1] * f[1] + u[2] * f[2]) * 57.29578f;   // 0 = straight arm
    if (!(bend >= 4.0f) || !(ln > 1e-3f)) return;   // too straight to tell the bend plane
    for (float& v : n) v /= ln;
    float a[3], b[3];
    InFrame(S, n, a);
    InFrame(E, n, b);
    if (!Finite3(a) || !Finite3(b)) return;
    for (int i = 0; i < 3; ++i) { r.acc[i] += a[i] * bend; r.accLow[i] += b[i] * bend; }
    r.weight += bend;
    if (r.weight > 20000.0f) { for (int i = 0; i < 3; ++i) { r.acc[i] *= 0.5f; r.accLow[i] *= 0.5f; } r.weight *= 0.5f; }
    r.samples++;
    if (bend > r.bendMax) r.bendMax = bend;
    float m = sqrtf(r.acc[0] * r.acc[0] + r.acc[1] * r.acc[1] + r.acc[2] * r.acc[2]);
    float share = m > 1e-3f ? fabsf(r.acc[1]) / m : 0.0f;
    if (r.samples >= 10 && share > 0.7f) {
        bool inv = r.acc[1] < 0.0f;
        if (!r.learned || inv != r.invert) Log("arm hinge %s: learned - hinge Y %s the bend normal -> bInvertJointAxis %d (%d samples, Y share %.2f)",
                                                h ? "right" : "left", inv ? "opposes" : "follows", inv ? 1 : 0, r.samples, share);
        if (!r.learned || inv != r.invert) { r.flips = 0; r.halfTurnFrames = 0; r.checkOff = false; }   // (review: a flip belongs to the answer it corrected)
        r.invert = inv;
        r.learned = true;
    }
    if (GetTickCount() - r.lastLog > 2000) {
        r.lastLog = GetTickCount();
        float ml = sqrtf(r.accLow[0] * r.accLow[0] + r.accLow[1] * r.accLow[1] + r.accLow[2] * r.accLow[2]);
        Log("arm hinge %s: elbow bent %.0f deg (max %.0f) | bend normal in the upper arm's frame (%.2f %.2f %.2f) forearm's (%.2f %.2f %.2f) | totals upper Y %+.2f forearm Y %+.2f of 1, %d samples%s",
            h ? "right" : "left", bend, r.bendMax, a[0], a[1], a[2], b[0], b[1], b[2], m > 1e-3f ? r.acc[1] / m : 0.0f,
            ml > 1e-3f ? r.accLow[1] / ml : 0.0f, r.samples, r.learned ? "" : " (not learned yet)");
    }
}

// The upper arm's rotation from its rest pose right now (from this body's atoms: parent^-1 x upper arm, against the
// reference skeleton's local rotation), in degrees; -1 if unreadable.
static float UpperArmFromRest(int h) {
    float qc[4], qu[4], ref[4];
    if (!BoneQuat(g_shadowArms, kArmBones[h], qc) || !BoneQuat(g_shadowArms, kUpperArmBones[h], qu) ||
        !RefLocalQuat(g_shadowArms, kUpperArmBones[h], ref)) return -1.0f;
    float qcInv[4] = { -qc[0], -qc[1], -qc[2], qc[3] }, refInv[4] = { -ref[0], -ref[1], -ref[2], ref[3] }, local[4], d[4];
    QuatMul(qcInv, qu, local);
    QuatMul(local, refInv, d);
    return 2.0f * acosf(Clamp(fabsf(d[3]), 0.0f, 1.0f)) * 57.29578f;
}

// One arm on the chain (UpdateClimbHands): built like PlaceJumpArm - world-space rotations for upper arm, forearm and hand
// from points, no limb IK (its roll can't flip). The elbow from the arm's own lengths, bent toward a pole down, out and
// back; the roll from the elbow's bend axis (JumpArmRollSign, as the jump hands); the hand turned so its palm faces the
// chain, fingers up along the forearm (bind-derived axes, FlatHandAxesLocal - the slide's flat hand uses them too).
static void PlaceClimbArm(USkeletalMeshComponent* comp, int side, float w, const FVector& T, const FVector& towardChain,
                          const FVector& F, const FVector& R, const FVector& U) {
    if (!comp || w <= 0.0f) return;
    auto* upperCtl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kUpperArmBones[side])));
    auto* lowerCtl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kLowerArmBones[side])));
    auto* handCtl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kWristControlNames[side])));
    if (!upperCtl || !lowerCtl || !handCtl) return;
    if (auto* ik = static_cast<USkelControlLimb*>(comp->FindSkelControl(FName(kHandBones[side])))) ik->ControlStrength = ik->StrengthTarget = 0.0f;
    auto sub = [](const FVector& a, const FVector& b) { return FVector{ a.X - b.X, a.Y - b.Y, a.Z - b.Z }; };
    auto add = [](const FVector& a, const FVector& b) { return FVector{ a.X + b.X, a.Y + b.Y, a.Z + b.Z }; };
    auto mul = [](const FVector& a, float k) { return FVector{ a.X * k, a.Y * k, a.Z * k }; };
    auto dot = [](const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
    auto cross = [](const FVector& a, const FVector& b) { return FVector{ a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X }; };
    auto len = [&](const FVector& a) { return sqrtf(dot(a, a)); };
    auto norm = [&](const FVector& a) { float l = len(a); return l > 1e-4f ? mul(a, 1.0f / l) : a; };
    auto orth = [&](const FVector& v, const FVector& x) { return norm(sub(v, mul(x, dot(v, x)))); };
    FVector S = comp->GetBoneLocation(FName(kUpperArmBones[side]), 0);
    FVector Ecur = comp->GetBoneLocation(FName(kLowerArmBones[side]), 0);
    FVector Hcur = comp->GetBoneLocation(FName(kHandBones[side]), 0);
    float L1 = len(sub(Ecur, S)), L2 = len(sub(Hcur, Ecur));
    if (L1 < 5.0f || L2 < 5.0f) return;
    FVector d = sub(T, S);
    float D = len(d);
    if (D < 1e-3f) return;
    FVector dir = mul(d, 1.0f / D);
    D = Clamp(D, fabsf(L1 - L2) + 1.0f, (L1 + L2) * 0.97f);
    float sideSign = side == 0 ? -1.0f : 1.0f;
    FVector pole = norm(add(add(mul(R, sideSign * 1.0f), mul(U, -0.35f)), mul(F, -0.1f)));   // out to the side, a little down and back (a down/out/back pole is near-opposite an overhead reach and flips - review 18:5x)
    FVector bendDir = orth(pole, dir);
    if (dot(bendDir, bendDir) < 0.5f) bendDir = orth(mul(R, sideSign), dir);
    float a = (L1 * L1 - L2 * L2 + D * D) / (2.0f * D);
    float hgt = sqrtf(L1 * L1 - a * a > 0.0f ? L1 * L1 - a * a : 0.0f);
    FVector E = add(add(S, mul(dir, a)), mul(bendDir, hgt));
    FVector Tc = add(S, mul(dir, D));
    FVector Xu = norm(sub(E, S)), Xl = norm(sub(Tc, E));
    FVector bend = cross(Xu, Xl);
    FVector Yarm = dot(bend, bend) > 1e-4f ? mul(norm(bend), -g_cfg.jumpArmRollSign) : mul(U, 1.0f);
    FVector Yu = orth(Yarm, Xu), Yl = orth(Yarm, Xl);
    FVector Xh = Xl, Yh = Yl;
    FVector fL, pL, tL;
    if (FlatHandAxesLocal(side, fL, pL, &tL)) {
        FVector Fw = norm(add(Xl, mul(towardChain, 0.35f)));                       // fingers on along the forearm, a little toward the chain
        FVector Pw = orth(mul(towardChain, g_cfg.slideLeftHandPalmSign), Fw);       // palm to the chain (the bind's "palm" side maps with the slide's sign)
        FVector Cw = cross(Fw, Pw);
        pL = orth(pL, fL);
        FVector cL = cross(fL, pL);
        Xh = norm(add(add(mul(Fw, fL.X), mul(Pw, pL.X)), mul(Cw, cL.X)));
        Yh = orth(add(add(mul(Fw, fL.Y), mul(Pw, pL.Y)), mul(Cw, cL.Y)), Xh);
        Yl = orth(add(mul(Yl, 0.5f), mul(Yh, 0.5f)), Xl);                          // the forearm turns half way with the hand (no wrist wring)
    }
    upperCtl->BoneRotationSpace = lowerCtl->BoneRotationSpace = handCtl->BoneRotationSpace = EBoneControlSpace::BCS_WorldSpace;
    upperCtl->bAddRotation = lowerCtl->bAddRotation = handCtl->bAddRotation = 0;
    upperCtl->bApplyRotation = lowerCtl->bApplyRotation = handCtl->bApplyRotation = 1;
    upperCtl->bApplyTranslation = lowerCtl->bApplyTranslation = handCtl->bApplyTranslation = 0;
    upperCtl->BoneScale = lowerCtl->BoneScale = 1.0f;
    upperCtl->BoneRotation = AxesToRotator(Xu, Yu, cross(Xu, Yu));
    lowerCtl->BoneRotation = AxesToRotator(Xl, Yl, cross(Xl, Yl));
    handCtl->BoneRotation = AxesToRotator(Xh, Yh, cross(Xh, Yh));
    upperCtl->ControlStrength = upperCtl->StrengthTarget = w;
    lowerCtl->ControlStrength = lowerCtl->StrengthTarget = w;
    handCtl->ControlStrength = handCtl->StrengthTarget = w;
}

// Hand over hand on a chain/ladder (user 18:55). Each hand holds a point on the chain fixed in the world, so the climb
// carries it down past the face: that is the pull. Once the holding hand is down at ClimbGripBottom (and the other hand
// holds) it lets go and reaches, on a small outward arc, to a new grip ClimbGripTop above the eyes; the fingers open on
// the way and close on the chain. Going down it runs the other way. Still: both hold where they are. Posed onto the
// arm skin's pose source (g_shadowBody: what the first-person arms draw) through the skin frame, and onto the casting
// body. Also asks for frames (SelfTestCapture) while climbing, and on the Home key.
static void UpdateClimbHands(ADishonoredPlayerPawn* pawn) {
    struct HandState { bool holding; FVector grip; FVector fromRel; float toRel; float t; float dur; };
    static HandState hs[2];
    static bool active = false;
    static LONGLONG qpcH = 0;
    float dt = QpcDt(qpcH);
    if (dt > 0.1f) dt = 0.1f;
    g_climbHandsOn = false;
    // ---- frame requests (development) ----
    {
        static DWORD nextShot = 0, burstUntil = 0;
        static bool homeWas = false;
        DWORD now = GetTickCount();
        bool home = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (home && !homeWas) { burstUntil = now + 5000; nextShot = 0; Log("capture: Home pressed - 10 frames over 5 s"); }
        homeWas = home;
        bool want = g_cfg.selfTestCapture && ((g_cfg.climbHands && g_climbAmount > 0.5f && g_captureCount < 45) || now < burstUntil);
        if (want && now >= nextShot && g_captureWant == 0) {
            strncpy(g_captureTag, now < burstUntil ? "home" : "climb", sizeof(g_captureTag) - 1);
            InterlockedExchange(&g_captureWant, 1);
            nextShot = now + (now < burstUntil ? 500 : 330);
        }
    }
    float w = g_cfg.climbHands ? g_climbAmount : 0.0f;
    if (!pawn || w <= 0.0f || GetTickCount() - g_viewLocTick > 500) {
        if (active) {   // let go: hand the forearm / wrist controls back (PlaceJumpArm only zeroes the upper arm)
            USkeletalMeshComponent* cs[2] = { g_shadowBody, g_shadowArms };
            for (USkeletalMeshComponent* c : cs) for (int h = 0; c && h < 2; ++h) {
                if (auto* l = static_cast<USkelControlSingleBone*>(c->FindSkelControl(FName(kLowerArmBones[h])))) l->ControlStrength = l->StrengthTarget = 0.0f;
                if (auto* wr = static_cast<USkelControlSingleBone*>(c->FindSkelControl(FName(kWristControlNames[h])))) wr->ControlStrength = wr->StrengthTarget = 0.0f;
            }
            Log("climb hands: let go");
        }
        active = false;
        g_climbGrip[0] = g_climbGrip[1] = 0.0f;
        return;
    }
    auto add = [](const FVector& a, const FVector& b) { return FVector{ a.X + b.X, a.Y + b.Y, a.Z + b.Z }; };
    auto sub = [](const FVector& a, const FVector& b) { return FVector{ a.X - b.X, a.Y - b.Y, a.Z - b.Z }; };
    auto mul = [](const FVector& a, float k) { return FVector{ a.X * k, a.Y * k, a.Z * k }; };
    float yaw = pawn->Rotation.Yaw * (6.2831853f / 65536.0f);
    FVector F{ cosf(yaw), sinf(yaw), 0.0f }, R{ -sinf(yaw), cosf(yaw), 0.0f }, U{ 0.0f, 0.0f, 1.0f };
    FVector P = pawn->Location;
    FVector C = add(add(P, mul(F, g_cfg.climbChainForward)), mul(R, -g_cfg.climbSideShift));   // the chain's line (x, y)
    float eyeZ = g_viewLoc.Z;
    float top = g_cfg.climbGripTop, bottom = g_cfg.climbGripBottom;
    if (top - bottom < 10.0f) bottom = top - 10.0f;
    auto gripAt = [&](int h, float z) { float sd = h ? 1.0f : -1.0f; return FVector{ C.X + R.X * sd * g_cfg.climbHandSpread, C.Y + R.Y * sd * g_cfg.climbHandSpread, z }; };
    if (!active) {   // the grab: staggered, one high, one lower
        hs[0] = { true, gripAt(0, eyeZ + top - 2.0f), FVector{ 0, 0, 0 }, 0.0f, 1.0f, 0.3f };
        hs[1] = { true, gripAt(1, eyeZ + (top + bottom) * 0.5f), FVector{ 0, 0, 0 }, 0.0f, 1.0f, 0.3f };
        active = true;
    }
    float speed = fabsf(g_climbVz);
    float span = top - bottom;
    float reachDur = Clamp(speed > 1.0f ? 0.45f * span / speed : g_cfg.climbReachTime, 0.12f, g_cfg.climbReachTime > 0.12f ? g_cfg.climbReachTime : 0.12f);
    for (int h = 0; h < 2; ++h) {
        HandState& m = hs[h];
        HandState& o = hs[1 - h];
        if (m.holding) {
            FVector g = gripAt(h, m.grip.Z);   // (the chain's line may drift with the pawn: the grip keeps its height)
            m.grip = g;
            float rel = m.grip.Z - eyeZ;
            bool low = rel < bottom && g_climbVz >= -5.0f, high = rel > top + 6.0f && g_climbVz <= 5.0f;
            if ((low || high) && o.holding) {
                m.holding = false;
                m.t = 0.0f;
                m.dur = reachDur;
                m.fromRel = sub(m.grip, FVector{ g_viewLoc.X, g_viewLoc.Y, eyeZ });
                m.toRel = low ? top : bottom + 6.0f;
            }
        } else {
            m.t += dt / (m.dur > 0.05f ? m.dur : 0.05f);
            if (m.t >= 1.0f) { m.holding = true; m.t = 1.0f; m.grip = gripAt(h, eyeZ + m.toRel); }
        }
    }
    FVector eye{ g_viewLoc.X, g_viewLoc.Y, eyeZ };
    Xf invSkin;
    bool skin = g_armSkinOn && g_skinFrameOk && g_shadowBody && XfInv(g_skinFrame, invSkin);
    Xf shadowL2W = skin ? XfMul(MeshOriginXf(g_shadowBody->SkeletalMesh), XfFromMatrix(g_shadowBody->LocalToWorld)) : Xf{};   // (bone space -> mesh origin -> component -> world, as GetBoneLocation)
    for (int h = 0; h < 2; ++h) {
        if (h ? g_rightHandBusy : g_leftHandBusy) { g_climbGrip[h] = 0.0f; continue; }   // (an item in hand: the game's arm)
        HandState& m = hs[h];
        float sd = h ? 1.0f : -1.0f;
        FVector T;
        if (m.holding) {
            T = m.grip;
            g_climbGrip[h] = 1.0f;
        } else {
            float u = Clamp(m.t, 0.0f, 1.0f);
            u = u * u * (3.0f - 2.0f * u);
            FVector a = add(eye, m.fromRel), b = gripAt(h, eyeZ + m.toRel);
            float arc = sinf(3.14159265f * u);
            T = add(add(a, mul(sub(b, a), u)), add(mul(R, sd * 5.0f * arc), mul(F, -4.0f * arc)));
            g_climbGrip[h] = Clamp(1.0f - 2.2f * arc, 0.0f, 1.0f);   // open mid-reach
        }
        FVector tc{ C.X - T.X, C.Y - T.Y, 0.0f };
        float tl = sqrtf(tc.X * tc.X + tc.Y * tc.Y);
        FVector toward = tl > 0.5f ? FVector{ tc.X / tl, tc.Y / tl, 0.0f } : F;
        if (skin) {   // where the skin draws the hand -> the shadow body's own world
            Xf tw = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { T.X, T.Y, T.Z } };
            Xf ts = XfMul(XfMul(tw, invSkin), shadowL2W);
            PlaceClimbArm(g_shadowBody, h, w, FVector{ ts.t[0], ts.t[1], ts.t[2] }, toward, F, R, U);
        }
        if (g_shadowArms) PlaceClimbArm(g_shadowArms, h, w, T, toward, F, R, U);
    }
    g_climbHandsOn = true;
    static DWORD lastLog = 0;
    if (GetTickCount() - lastLog > 250) {
        lastLog = GetTickCount();
        Log("climb hands: speed %.0f reach %.2f s | L %s %.0f  R %s %.0f (grip height vs eyes) | chain %.0f fwd %.0f left | skin %d",
            g_climbVz, reachDur, hs[0].holding ? "hold" : "reach", (hs[0].holding ? hs[0].grip.Z : eyeZ + hs[0].toRel) - eyeZ,
            hs[1].holding ? "hold" : "reach", (hs[1].holding ? hs[1].grip.Z : eyeZ + hs[1].toRel) - eyeZ,
            g_cfg.climbChainForward, g_cfg.climbSideShift, skin ? 1 : 0);
    }
}

// The holstered body's arms. A raised side: the forearm (and the hand under it) folded to the elbow, the upper arm scaled
// about the shoulder joint by CastHolsterUpperArm - the holstered shoulder and upper arm stay in the shadow beside the
// casting body's raised arm. A free side: the whole arm folded into the shoulder joint (18:0x, user: two sets of arm
// shadows when holstered) - the casting body draws that arm, and this body adds nothing there whatever poses it.
// Runs after UpdateJumpHands / UpdateCoat (they write the same controls) and before the frame's pose update.
static void UpdateHolsterShoulders() {
    if (!g_holsterCast) return;
    static int mode[2] = { -1, -1 };   // (for the log) 0 folded away, 1 holstered shoulder kept
    for (int h = 0; h < 2; ++h) {
        auto* upperCtl = static_cast<USkelControlSingleBone*>(g_holsterCast->FindSkelControl(FName(kUpperArmBones[h])));
        auto* lowerCtl = static_cast<USkelControlSingleBone*>(g_holsterCast->FindSkelControl(FName(kLowerArmBones[h])));
        if (!upperCtl || !lowerCtl) continue;
        upperCtl->bIgnoreWhenNotRendered = 0;
        lowerCtl->bIgnoreWhenNotRendered = 0;
        float s = g_castS[h];   // (the casting reach's own strength: when its reach is off the arm is down, so is this)
        bool keep = s > 0.02f;
        upperCtl->bApplyRotation = 0;
        upperCtl->bApplyTranslation = 0;
        upperCtl->BoneScale = keep ? 1.0f + (Clamp(g_cfg.castHolsterUpperArm, 0.05f, 1.0f) - 1.0f) * Clamp(s, 0.0f, 1.0f) : 0.001f;
        upperCtl->ControlStrength = upperCtl->StrengthTarget = 1.0f;
        lowerCtl->bApplyRotation = 0;
        lowerCtl->bApplyTranslation = 0;
        lowerCtl->BoneScale = 0.001f;
        lowerCtl->ControlStrength = lowerCtl->StrengthTarget = 1.0f;
        int m = keep ? 1 : 0;
        if (m != mode[h]) Log("holstered shoulders: %s arm %s", h ? "right" : "left", keep ? "raised - holstered shoulder kept, forearm folded" : "free - folded away (the casting body draws it)");
        mode[h] = m;
    }
}

static void UpdateCastingArmReach(ADishonoredPlayerPawn* pawn) {
    if (!g_shadowArms) return;
    USkeletalMeshComponent* arms = pawn ? pawn->Mesh : nullptr;
    static LONGLONG qpcC = 0;
    float dtC = QpcDt(qpcC);
    Xf frame, inv, L1;
    bool on = arms && arms->SkeletalMesh && g_shadowArms->SkeletalMesh && GetTickCount() - g_viewLocTick < 500 &&
              CopyFrameXf(pawn, g_shadowArms->Translation, g_shadowArms->Rotation, g_shadowArms->Scale, frame);
    // (14:02 log: the hand missed the drawn 1P hand by a steady 51 uu and sat twisted - the target was built without the
    // body mesh's own Origin / RotOrigin, which sit between the bones' space and the component's. The skin frame, which
    // lands its hand 0.1 uu off, is meshOrigin x component x owner; this is the same for the casting body.)
    if (on) { frame = XfMul(MeshOriginXf(g_shadowArms->SkeletalMesh), frame); on = XfInv(frame, inv); }
    Xf camA, camW;
    bool haveCam = false;
    if (on) {
        L1 = XfFromMatrix(arms->LocalToWorld);
        camW = L1;
        haveCam = BoneAtomXf(arms, "camera_jnt", camA);
        if (haveCam) camW = XfMul(camA, L1);
    }
    auto drawn = [&](Xf wx) {
        if (haveCam) { wx.t[0] += g_viewLoc.X - camW.t[0]; wx.t[1] += g_viewLoc.Y - camW.t[1]; wx.t[2] += g_viewLoc.Z - camW.t[2]; }
        return wx;
    };
    for (int h = 0; h < 2; ++h) {
        auto* ik = static_cast<USkelControlLimb*>(g_shadowArms->FindSkelControl(FName(kHandBones[h])));
        if (!ik) continue;
        ik->bIgnoreWhenNotRendered = 0;   // (see SyncTransform: this body is never rendered)
        ik->bAllowStretching = 0;         // (15:00 clip: stretching pushes the child bone out and the mesh between the joints thins - "thin wrists, thin shoulder joins"; a hand ~5 uu short is invisible in a shadow)
        auto* wrist = static_cast<USkelControlSingleBone*>(g_shadowArms->FindSkelControl(FName(kWristControlNames[h])));
        if (wrist) wrist->bIgnoreWhenNotRendered = 0;
        // Put away (14:45 clip: blocking with the sword - the game tucks the off-hand's item away and pulls that 1P hand out
        // of the view while the hand stays "busy"): after CastHoldAwayMs out of view the arm eases back to the standing
        // pose, and comes back when the hand does. Not while looking up (the hands drop out of the view then too) or
        // mid-draw. The item copy follows g_castHold (UpdateItemShadows).
        {
            // (15:05: the idle 1P SWORD hand sits below the bottom of the screen, so an out-of-view rule put the sword
            // arm to sleep whenever it was held still - the off-hand only, and by the game's own block state.)
            bool busyH = h ? g_rightHandBusy : g_leftHandBusy;
            bool away = false;
            if (h == 0 && busyH) {
                int bs = BlockContextState();
                float bw = static_cast<float>(bs);   // (for the log line)
                if (bs >= 0) away = bs == 1 || bs == 2;   // Blocking / Block_Parry
                else {
                    bool lookingUp = g_viewPitchDeg > g_cfg.armSkinReachMaxPitch;
                    away = !g_onePHandOnScreen[h] && !g_handDrawing[h] && !lookingUp &&
                           g_onePHandOffMs[h] > static_cast<DWORD>(g_cfg.castHoldAwayMs);
                }
                static DWORD lastAwayLog = 0;
                if (away && GetTickCount() - lastAwayLog > 1000) { lastAwayLog = GetTickCount(); Log("block: off-hand put away (block node weight %.2f)", bw); }
            }
            g_castHold[h] += Clamp((away ? 0.0f : 1.0f) - g_castHold[h], -dtC * 6.0f, dtC * 6.0f);
        }
        // Climbing (18:45): the game animates only the first-person hands on a chain/ladder - hand over hand. The
        // shadow's hands follow them like a weapon hand (same reach, elbow pole, wrist and finger grip), so the shadow
        // climbs in time with the game's own animation.
        float sW = on ? g_weaponRaise[h] * g_castHold[h] : 0.0f;
        float sC = (on && g_cfg.climbShadowArms && !g_cfg.climbHands) ? g_climbAmount : 0.0f;   // (ClimbHands poses it instead)
        float s = sW > sC ? sW : sC;
        g_castS[h] = sW;   // (UpdateHolsterShoulders: weapon raises only - an arm up on a chain gets no hanging holstered stub beside it)
        ik->ControlStrength = ik->StrengthTarget = s;
        if (g_shoulderBlob[h] && s <= 0.02f) g_shoulderBlob[h]->SetScale(0.0001f);   // (arm down: no blob)
        auto* clav = static_cast<USkelControlSingleBone*>(g_shadowArms->FindSkelControl(FName(kArmBones[h])));
        // (18:0x, user: two sets of arm shadows inside each other when holstered - this zeroed UpdateCoat's shoulder pose
        // (stand spread, jump raise) on the casting body only, a leftover of the collar-bone experiments; the holstered
        // body kept it. Only those experiments, when on, own this control.)
        if (clav && s <= 0.0f && (g_cfg.castClavicle > 0.0f || g_cfg.castClavicleFollow > 0.0f)) clav->ControlStrength = clav->StrengthTarget = 0.0f;
        {   // the hinge learner samples an arm the IK has left alone for a few frames, in first person, weapon fully down
            ArmRoll& r = g_armRoll[h];
            bool quiet = s <= 0.0f && g_weaponRaise[h] <= 0.0f && g_bodyVisible && JumpArmAmount(h) <= 0.0f && g_slideAmount <= 0.01f && g_climbAmount <= 0.0f;
            r.offFrames = quiet ? r.offFrames + 1 : 0;
            if (g_cfg.castArmRoll && r.offFrames >= 3) LearnArmHinge(h);
        }
        if (g_cfg.castArmRoll) {   // the hinge the right way round (see ArmRoll)
            ArmRoll& r = g_armRoll[h];
            // Before anything is learned: 1 (17:28 log - both arms read a half turn with 0, PlaceJumpArm's reset).
            bool inv = g_cfg.castArmRollInvert >= 0 ? g_cfg.castArmRollInvert != 0 : r.learned ? r.invert : true;
            if (r.flips & 1) inv = !inv;   // the result check overruled it
            ik->JointAxis = EAxis::AXIS_Y;
            ik->bInvertJointAxis = inv ? 1 : 0;
            float rest = UpperArmFromRest(h);
            if (!r.learned && !r.checkOff && g_cfg.castArmRollInvert < 0 && s >= 0.95f && rest >= 0.0f) {   // (only before a sign is learned: a bend-plane flip read as a half turn would otherwise flip a right answer)
                r.halfTurnFrames = rest > 155.0f ? r.halfTurnFrames + 1 : 0;
                if (r.halfTurnFrames >= 45) {
                    r.halfTurnFrames = 0;
                    if (r.flips < 2) {
                        r.flips++;
                        Log("arm hinge %s: the IK'd upper arm reads %.0f deg from rest (a half turn) - flipping its hinge (%d of 2)", h ? "right" : "left", rest, r.flips);
                    }
                    if (r.flips >= 2) { r.checkOff = true; Log("arm hinge %s: both ways read a half turn - the check stands down, back on the first answer", h ? "right" : "left"); }
                }
            } else if (s < 0.95f) r.halfTurnFrames = 0;
            static bool rigLogged[2] = { false, false };
            if (!rigLogged[h]) {   // the rig, once (review: the forearm shares this hinge only if its rest rotation is a pure Y turn; a -X bone would need bInvertBoneAxis)
                rigLogged[h] = true;
                float ql[4] = { 0, 0, 0, 1 }, pl[3] = { 0, 0, 0 }, ph[3] = { 0, 0, 0 };
                bool okq = RefLocalQuat(g_shadowArms, kLowerArmBones[h], ql);
                bool okp = RefLocalPos(g_shadowArms, kLowerArmBones[h], pl) && RefLocalPos(g_shadowArms, kHandBones[h], ph);
                Log("arm hinge %s rig: forearm rest rotation (%.3f %.3f %.3f %.3f)%s | upper arm runs (%.1f %.1f %.1f), forearm runs (%.1f %.1f %.1f) in their own frames%s",
                    h ? "right" : "left", ql[0], ql[1], ql[2], ql[3], okq ? (fabsf(ql[0]) < 0.05f && fabsf(ql[2]) < 0.05f ? " = a pure Y turn: the forearm shares the hinge" : " - NOT a pure Y turn: the forearm will be rolled") : " (unreadable)",
                    pl[0], pl[1], pl[2], ph[0], ph[1], ph[2], okp ? (pl[0] > 0.0f && ph[0] > 0.0f ? "" : " - a bone runs along -X: bInvertBoneAxis needed") : " (unreadable)");
            }
            static DWORD lastRollLog[2] = { 0, 0 };
            if (s >= 0.5f && GetTickCount() - lastRollLog[h] > 1000) {
                lastRollLog[h] = GetTickCount();
                Log("arm hinge %s: IK bInvertJointAxis %d (%s%s) | upper arm %.0f deg from rest (a half turn reads ~180)", h ? "right" : "left",
                    ik->bInvertJointAxis ? 1 : 0, g_cfg.castArmRollInvert >= 0 ? "ini override" : r.learned ? "learned off the animated arm" : "default 1",
                    (r.flips & 1) ? ", flipped by the result check" : "", rest);
            }
        }
        Xf a1;
        if (s <= 0.0f || !BoneAtomXf(arms, kHandBones[h], a1)) {
            if (wrist && s <= 0.0f && JumpArmAmount(h) <= 0.0f && SlideArmAmount(h) <= 0.0f) wrist->ControlStrength = wrist->StrengthTarget = 0.0f;
            continue;
        }
        Xf w1 = drawn(XfMul(a1, L1));
        // Keep an elbow: pull a target beyond CastReachMax of the arm's length back toward the shoulder (this body's
        // upper arm + forearm, measured off its own bones; constant now that nothing stretches them).
        if (g_cfg.castReachMax > 0.0f && g_cfg.castReachMax < 1.0f) {
            FVector S = g_shadowArms->GetBoneLocation(FName(kUpperArmBones[h]), 0);
            FVector E = g_shadowArms->GetBoneLocation(FName(kLowerArmBones[h]), 0);
            FVector Hn = g_shadowArms->GetBoneLocation(FName(kHandBones[h]), 0);
            float l1 = sqrtf((E.X - S.X) * (E.X - S.X) + (E.Y - S.Y) * (E.Y - S.Y) + (E.Z - S.Z) * (E.Z - S.Z));
            float l2 = sqrtf((Hn.X - E.X) * (Hn.X - E.X) + (Hn.Y - E.Y) * (Hn.Y - E.Y) + (Hn.Z - E.Z) * (Hn.Z - E.Z));
            float maxR = (l1 + l2) * g_cfg.castReachMax;
            float dx = w1.t[0] - S.X, dy = w1.t[1] - S.Y, dz = w1.t[2] - S.Z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (l1 > 5.0f && l2 > 5.0f && dist > maxR && dist > 1e-3f) {
                float k = maxR / dist;
                w1.t[0] = S.X + dx * k; w1.t[1] = S.Y + dy * k; w1.t[2] = S.Z + dz * k;
            }
        }
        Xf c1 = XfMul(w1, inv);   // the 1P hand as drawn -> this body's component space
        ik->EffectorLocationSpace = EBoneControlSpace::BCS_ComponentSpace;
        ik->EffectorLocation = FVector{ c1.t[0], c1.t[1], c1.t[2] };
        {
            static DWORD lastLog[2] = { 0, 0 };
            if (s > 0.5f && GetTickCount() - lastLog[h] > 500) {
                lastLog[h] = GetTickCount();
                FVector k = g_shadowArms->GetBoneLocation(FName(kHandBones[h]), 0);
                float dx = k.X - w1.t[0], dy = k.Y - w1.t[1], dz = k.Z - w1.t[2];
                Log("casting reach: %s strength %.2f (ik ignoreNR %d, body recentlyRendered %d) | casting hand - drawn 1P hand %.1f uu "
                    "| effector comp (%.0f %.0f %.0f) | camera_jnt %s", h ? "right" : "left", ik->ControlStrength,
                    ik->bIgnoreWhenNotRendered ? 1 : 0, g_shadowArms->bRecentlyRendered ? 1 : 0, sqrtf(dx * dx + dy * dy + dz * dz),
                    c1.t[0], c1.t[1], c1.t[2], haveCam ? "found" : "MISSING");
            }
        }
        // Elbow (14:27 clip: the arm's join to the shoulder looked "squeezed"): the 1P arms are built for the camera's
        // view - their elbows sit up beside the head, out of frame - so steering this body's elbow at the 1P elbow
        // folded the upper arm up toward the ear and it foreshortened into a stub at the shoulder in the silhouette.
        // A third-person elbow instead: out to the side and down from the shoulder-hand line, in actor space.
        {
            Xf own = OwnerXf(pawn), ownInv;
            if (XfInv(own, ownInv)) {
                FVector sw = g_shadowArms->GetBoneLocation(FName(kUpperArmBones[h]), 0);   // this body's shoulder joint (world)
                Xf sx = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { sw.X, sw.Y, sw.Z } };
                Xf hx = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { w1.t[0], w1.t[1], w1.t[2] } };
                Xf sa = XfMul(sx, ownInv), ha = XfMul(hx, ownInv);   // -> actor space (X forward, Y right, Z up)
                float side = h == 0 ? -1.0f : 1.0f;
                // The solver bends the arm in the plane through shoulder, hand and pole; its normal is (hand dir) x (pole
                // dir). A pole near the hand's line makes that normal swing round as the hand moves past it (18:15 clip:
                // the right arm spun while walking - the lowered sword hand bobs down-out from the shoulder, right where
                // the old pole sat; log: right upper arm 172-179 deg from rest for moments). So: the pole goes behind the
                // shoulder (CastElbowBack), out of any drawn hand's reach, and the plane may turn only so fast.
                g_climbHandUp[h] = ha.t[2] - sa.t[2];
                float D[3] = { ha.t[0] - sa.t[0], ha.t[1] - sa.t[1], ha.t[2] - sa.t[2] };
                float dl = sqrtf(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
                if (dl > 1.0f) { D[0] /= dl; D[1] /= dl; D[2] /= dl; } else { D[0] = 1.0f; D[1] = 0.0f; D[2] = 0.0f; }
                float P[3] = { -g_cfg.castElbowBack, side * g_cfg.castElbowOut, -g_cfg.castElbowDown };
                float pl = sqrtf(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
                if (pl < 1.0f) { P[0] = 0.0f; P[1] = side; P[2] = -1.0f; pl = sqrtf(2.0f); }
                for (float& v : P) v /= pl;
                float pd = P[0] * D[0] + P[1] * D[1] + P[2] * D[2];
                float B[3] = { P[0] - D[0] * pd, P[1] - D[1] * pd, P[2] - D[2] * pd };   // the bend direction the pole asks for
                float margin = sqrtf(B[0] * B[0] + B[1] * B[1] + B[2] * B[2]);           // sin(hand line, pole line): 0 = degenerate
                float N[3] = { D[1] * B[2] - D[2] * B[1], D[2] * B[0] - D[0] * B[2], D[0] * B[1] - D[1] * B[0] };   // plane normal
                float nl = sqrtf(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
                static float prevN[2][3];
                static bool havePrevN[2] = { false, false };
                static int clamps[2] = { 0, 0 };
                static float minMargin[2] = { 1.0f, 1.0f };
                if (margin < minMargin[h]) minMargin[h] = margin;
                if (nl > 1e-4f) {
                    for (float& v : N) v /= nl;
                    if (havePrevN[h] && g_cfg.castElbowTurnRate > 0.0f) {
                        // keep the previous normal's continuity: re-project it onto the plane normal to D, then turn it
                        // toward the new one by at most TurnRate x dt
                        float* q = prevN[h];
                        float qd = q[0] * D[0] + q[1] * D[1] + q[2] * D[2];
                        float Q[3] = { q[0] - D[0] * qd, q[1] - D[1] * qd, q[2] - D[2] * qd };
                        float ql = sqrtf(Q[0] * Q[0] + Q[1] * Q[1] + Q[2] * Q[2]);
                        if (ql > 1e-3f) {
                            for (float& v : Q) v /= ql;
                            float c = Clamp(Q[0] * N[0] + Q[1] * N[1] + Q[2] * N[2], -1.0f, 1.0f);
                            float ang = acosf(c);
                            float maxStep = g_cfg.castElbowTurnRate * 0.0174533f * (dtC > 0.0f ? (dtC < 0.1f ? dtC : 0.1f) : 0.016f);
                            if (ang > maxStep) {
                                // rotate Q about D (both are perpendicular to D) toward N by maxStep
                                float W[3] = { D[1] * Q[2] - D[2] * Q[1], D[2] * Q[0] - D[0] * Q[2], D[0] * Q[1] - D[1] * Q[0] };   // D x Q
                                float sgn = (W[0] * N[0] + W[1] * N[1] + W[2] * N[2]) >= 0.0f ? 1.0f : -1.0f;
                                float cs = cosf(maxStep), sn = sinf(maxStep) * sgn;
                                for (int i = 0; i < 3; ++i) N[i] = Q[i] * cs + W[i] * sn;
                                ++clamps[h];
                            }
                        }
                    }
                    prevN[h][0] = N[0]; prevN[h][1] = N[1]; prevN[h][2] = N[2];
                    havePrevN[h] = true;
                    // the bend direction for this normal: N x D (so that D x (N x D) ... = the plane the solver builds)
                    float Bd[3] = { N[1] * D[2] - N[2] * D[1], N[2] * D[0] - N[0] * D[2], N[0] * D[1] - N[1] * D[0] };
                    float reach = 0.5f * (dl > 1.0f ? dl : 1.0f);
                    ik->JointTargetLocationSpace = EBoneControlSpace::BCS_ActorSpace;
                    ik->JointTargetLocation = FVector{ sa.t[0] + D[0] * reach + Bd[0] * 50.0f,
                                                       sa.t[1] + D[1] * reach + Bd[1] * 50.0f,
                                                       sa.t[2] + D[2] * reach + Bd[2] * 50.0f };
                }
                static DWORD lastPoleLog[2] = { 0, 0 };
                if (GetTickCount() - lastPoleLog[h] > 1000) {
                    lastPoleLog[h] = GetTickCount();
                    Log("elbow plane %s: hand dir (%.2f %.2f %.2f) from the shoulder (actor) | pole margin now %.2f, lowest %.2f this second | plane turns clamped %d frames",
                        h ? "right" : "left", D[0], D[1], D[2], margin, minMargin[h], clamps[h]);
                    clamps[h] = 0;
                    minMargin[h] = 1.0f;
                }
                // The shoulder blob on this joint, grown with the arm's raise. (15:37 clip: two little heads floated at head
                // height - the head mesh is not pivoted at the neck: it shares the body's skeleton space, its geometry
                // ~150 uu above its pivot, which is how the head shadow sits at the body root. So the blob's pivot goes
                // down by the head's offset from the root, scaled, and the scaled head lands on the joint.)
                if (g_shoulderBlob[h] && s > 0.02f) {
                    float sc = g_cfg.castShoulderBlob * s * (g_shadowArms->Scale > 0.0f ? g_shadowArms->Scale : 1.0f);
                    FVector hw = g_shadowArms->GetBoneLocation(FName("head_jnt"), 0);
                    Xf hx2 = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { hw.X, hw.Y, hw.Z } };
                    Xf hac = XfMul(hx2, ownInv);   // the head bone in actor space
                    FVector rootA = g_shadowArms->Translation;   // the casting body's pivot (actor space)
                    float offX = hac.t[0] - rootA.X, offY = hac.t[1] - rootA.Y, offZ = hac.t[2] - rootA.Z + 8.0f;   // (+8: the head's centre above head_jnt)
                    // A dome, not a head (17:11 clip: "two heads at the shoulders"): the head's face points DOWN into the
                    // shoulder and its crown OUT to the arm's side, so from the front and the side the silhouette is the
                    // back of the skull - a deltoid - with the neck and chin inside the body. The head's offset from the
                    // mesh root goes through the same orientation so the centre still lands where it is aimed.
                    FVector ax{ 0.0f, 0.0f, -1.0f };   // the mesh's forward (face) -> down
                    FVector az{ 0.0f, side, 0.0f };    // its up (crown) -> out
                    FVector ay{ -side, 0.0f, 0.0f };   // Z x X
                    float ox = offX * ax.X + offY * ay.X + offZ * az.X;
                    float oy = offX * ax.Y + offY * ay.Y + offZ * az.Y;
                    float oz = offX * ax.Z + offY * ay.Z + offZ * az.Z;
                    g_shoulderBlob[h]->SetTranslation(FVector{ sa.t[0] - ox * sc + g_cfg.castShoulderBlobFwd,
                                                               sa.t[1] - oy * sc + side * g_cfg.castShoulderBlobOut,
                                                               sa.t[2] - oz * sc + g_cfg.castShoulderBlobUp });
                    g_shoulderBlob[h]->SetRotation(AxesToRotator(ax, ay, az));
                    g_shoulderBlob[h]->SetScale(sc);
                    static DWORD lastBlobLog[2] = { 0, 0 };
                    if (GetTickCount() - lastBlobLog[h] > 1000) {
                        lastBlobLog[h] = GetTickCount();
                        FVector bt = g_shoulderBlob[h]->Translation;
                        Log("blob %s: pivot (%.0f %.0f %.0f) joint (%.0f %.0f %.0f) head-root off (%.0f %.0f %.0f) scale %.2f -> centre (%.0f %.0f %.0f)",
                            h ? "right" : "left", bt.X, bt.Y, bt.Z, sa.t[0], sa.t[1], sa.t[2], offX, offY, offZ, sc,
                            bt.X + ox * sc, bt.Y + oy * sc, bt.Z + oz * sc);
                    }
                }
                // The shoulder follows the raised arm - by measurement (17:14: every guessed-axis roll was a shrug or a
                // hump, and the blobs read as shoulder pads). The upper arm's rotation relative to the collar bone now
                // (both bones' component atoms) against the rotation the animation gave it this frame (its LocalAtoms
                // atom; the bind pose if that is unreadable) is exactly what the IK swung the arm through, in the collar
                // bone's own frame; the collar bone turns CastClavicleFollow of that about the same axis, as a real
                // shoulder does (bone-space additive). The IK re-solves from the moved joint, and the measured swing
                // shrinks: it settles at follow/(1+follow) of the raise.
                float qc[4], qu[4], ref[4];
                bool refIsAnim = BoneAnimLocalQuat(g_shadowArms, kUpperArmBones[h], ref);
                if (clav && g_cfg.castClavicleFollow > 0.0f && s > 0.0f &&
                    BoneQuat(g_shadowArms, kArmBones[h], qc) && BoneQuat(g_shadowArms, kUpperArmBones[h], qu) &&
                    (refIsAnim || RefLocalQuat(g_shadowArms, kUpperArmBones[h], ref))) {
                    float qcInv[4] = { -qc[0], -qc[1], -qc[2], qc[3] }, refInv[4] = { -ref[0], -ref[1], -ref[2], ref[3] };
                    float local[4], delta[4], part[4];
                    QuatMul(qcInv, qu, local);      // the upper arm relative to the collar bone, now (comp = parent * local)
                    QuatMul(local, refInv, delta);  // ...less the animation's: the IK's swing, in the collar bone's frame
                    float ang = 2.0f * acosf(Clamp(fabsf(delta[3]), 0.0f, 1.0f)) * 57.29578f;
                    float t = Clamp(g_cfg.castClavicleFollow, 0.0f, 1.0f);
                    if (ang > 0.5f && t * ang > g_cfg.castClavicleFollowMax) t = g_cfg.castClavicleFollowMax / ang;
                    const float ident[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    QuatSlerp(ident, delta, t, part);
                    clav->bApplyRotation = 1;
                    clav->bAddRotation = 1;
                    clav->BoneRotationSpace = EBoneControlSpace::BCS_BoneSpace;
                    clav->BoneRotation = QuatToRotator(part);
                    clav->BoneScale = 1.0f;
                    clav->ControlStrength = clav->StrengthTarget = s;
                    static DWORD lastClavLog[2] = { 0, 0 };
                    if (GetTickCount() - lastClavLog[h] > 1000) {
                        lastClavLog[h] = GetTickCount();
                        float bind[4], animVsBind = -1.0f;
                        if (refIsAnim && RefLocalQuat(g_shadowArms, kUpperArmBones[h], bind)) {
                            float bindInv[4] = { -bind[0], -bind[1], -bind[2], bind[3] }, d2[4];
                            QuatMul(ref, bindInv, d2);
                            animVsBind = 2.0f * acosf(Clamp(fabsf(d2[3]), 0.0f, 1.0f)) * 57.29578f;
                        }
                        static bool parentLogged[2] = { false, false };
                        if (!parentLogged[h] && g_refStride > 0) {   // the skeleton's own word on who the upper arm hangs from
                            parentLogged[h] = true;
                            auto* rs = reinterpret_cast<RawArray*>(&g_shadowArms->SkeletalMesh->RefSkeleton);
                            int iu = g_shadowArms->MatchRefBone(FName(kUpperArmBones[h])), ic = g_shadowArms->MatchRefBone(FName(kArmBones[h]));
                            int parentOff = g_refNameOff + g_refQuatOff + 44;   // FMeshBone: ...VJointPos{quat, pos, length, size} then ParentIndex
                            int parent = -1;
                            if (rs->Data && iu >= 0 && parentOff + 4 <= g_refStride)
                                parent = *reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(rs->Data) + iu * g_refStride + parentOff);
                            Log("clavicle %s: %s is bone %d, its parent index reads %d; %s is bone %d (%s)", h ? "right" : "left",
                                kUpperArmBones[h], iu, parent, kArmBones[h], ic, parent == ic ? "the collar bone: good" : "NOT the collar bone, or the layout guess is off");
                        }
                        FRotator r = clav->BoneRotation;
                        Log("clavicle %s: IK swing %.0f deg over the %s pose (anim vs bind %.0f deg) -> collar bone turns %.0f deg: +(%d %d %d) pitch yaw roll, at %.2f",
                            h ? "right" : "left", ang, refIsAnim ? "animated" : "bind", animVsBind, ang * t, r.Pitch, r.Yaw, r.Roll, s);
                    }
                }
                if (clav && g_cfg.castClavicle > 0.0f) {
                    float dx = ha.t[0] - sa.t[0], dy = ha.t[1] - sa.t[1], dz = ha.t[2] - sa.t[2];
                    float horiz = sqrtf(dx * dx + dy * dy);
                    float elevDeg = atan2f(dz, horiz > 1.0f ? horiz : 1.0f) * 57.29578f;   // -90 hanging .. +90 straight up
                    float raiseDeg = Clamp(g_cfg.castClavicle * (elevDeg + 70.0f), 0.0f, g_cfg.castClavicleMax);
                    clav->bApplyRotation = 1;
                    clav->bAddRotation = 1;
                    clav->BoneRotationSpace = EBoneControlSpace::BCS_ActorSpace;
                    clav->BoneRotation = FRotator{ static_cast<int>(g_cfg.leanSign * raiseDeg * 65536.0f / 360.0f), 0, 0 };
                    clav->BoneScale = 1.0f;
                    clav->ControlStrength = clav->StrengthTarget = s;
                }
            }
        }
        if (wrist && JumpArmAmount(h) <= 0.0f) {   // (UpdateJumpHands zeroed it this frame; the jump pose owns a free arm)
            FVector l; FRotator r; float sc;
            XfDecompose(c1, l, r, sc);
            wrist->BoneRotationSpace = EBoneControlSpace::BCS_ComponentSpace;
            wrist->bAddRotation = 0;
            wrist->bApplyRotation = 1;
            wrist->BoneRotation = r;
            wrist->ControlStrength = wrist->StrengthTarget = s;
        }
        // Fingers (14:34 clip: the Heart "floated" in an open shadow hand - a busy hand got no finger pose): the real
        // hand's grip, joint by joint, from the 1P arms' finger bones (their parent-space rotation, as CapturePowerHand
        // reads the power pose) onto this body's fingers. A free hand is re-posed by UpdateJumpHands every frame.
        {
            auto* bases1 = reinterpret_cast<RawArray*>(&arms->SpaceBases);
            auto rows = [&](int idx, FVector r[3]) {
                const float* a = reinterpret_cast<const float*>(static_cast<uint8_t*>(bases1->Data) + idx * 32);
                float x = a[0], y = a[1], z = a[2], w = a[3];
                r[0] = { 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y) };
                r[1] = { 2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x) };
                r[2] = { 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y) };
            };
            auto dot3 = [](const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
            for (int j = 0; j < 14 && bases1->Data; ++j) {
                auto* fc = static_cast<USkelControlSingleBone*>(g_shadowArms->FindSkelControl(FName(kFingerBones[h][j])));
                if (!fc) continue;
                FName fn(kFingerBones[h][j]);
                int idx = fn.FNameEntryId >= 0 ? arms->MatchRefBone(fn) : -1;
                int par = idx >= 0 ? arms->MatchRefBone(arms->GetParentBone(fn)) : -1;
                if (idx < 0 || par < 0 || idx >= bases1->Num || par >= bases1->Num) continue;
                FVector c[3], pp[3], l[3];
                rows(idx, c);
                rows(par, pp);
                for (int i = 0; i < 3; ++i) l[i] = { dot3(c[i], pp[0]), dot3(c[i], pp[1]), dot3(c[i], pp[2]) };
                fc->BoneRotationSpace = EBoneControlSpace::BCS_ParentBoneSpace;
                fc->bAddRotation = 0;
                fc->bApplyRotation = 1;
                fc->bApplyTranslation = 0;
                fc->BoneRotation = AxesToRotator(l[0], l[1], l[2]);
                fc->ControlStrength = fc->StrengthTarget = s;
            }
        }
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

// A held item's model as the shadow copy will draw it: the source component's shadow flags, each LOD's per-section
// shadow-casting switches (FSkeletalMeshLODInfo::bEnableShadowCasting - read when a component's render proxy is made),
// and its materials (blend mode, world position offset: a masked/translucent or vertex-offset material casts
// differently). With ItemModelsCastShadow every section's switch is turned on (once per mesh), and the real 1P
// component - which must never cast from where it is drawn - gets CastShadow / bCastDynamicShadow off; both take effect
// at each component's next proxy rebuild, so the 1P item never starts casting.
static void PrepareItemMeshShadow(int h, USkeletalMeshComponent* src) {
    USkeletalMesh* m = src ? src->SkeletalMesh : nullptr;
    if (!m) return;
    static USkeletalMesh* done[16] = {};
    bool seen = false;
    for (USkeletalMesh* d : done) if (d == m) { seen = true; break; }
    auto* lods = reinterpret_cast<RawArray*>(&m->LODInfo);
    std::string flags;
    int off = 0, total = 0;
    for (int l = 0; lods->Data && l < lods->Num && l < 6; ++l) {
        auto* li = reinterpret_cast<FSkeletalMeshLODInfo*>(static_cast<uint8_t*>(lods->Data) + l * sizeof(FSkeletalMeshLODInfo));
        auto* sc = reinterpret_cast<RawArray*>(&li->bEnableShadowCasting);
        flags += " LOD" + std::to_string(l) + ":";
        for (int j = 0; sc->Data && j < sc->Num && j < 12; ++j) {
            uint32_t* v = static_cast<uint32_t*>(sc->Data) + j;
            flags += *v ? "1" : "0";
            ++total;
            if (!*v) { ++off; if (g_cfg.itemMeshCastShadow) *v = 1; }
        }
        if (!sc->Data || sc->Num <= 0) flags += "(none)";
    }
    std::string mats;
    auto* mm = reinterpret_cast<RawArray*>(&m->Materials);
    for (int i = 0; mm->Data && i < mm->Num && i < 6; ++i) {
        UMaterialInterface* mi = src->GetMaterial(i);
        if (!mi) { mats += " [none]"; continue; }
        UMaterial* base = mi->GetMaterial();
        int blend = base ? static_cast<int>(base->BlendMode) : -1;
        bool wpo = base && *reinterpret_cast<void**>(&base->WorldPositionOffset) != nullptr;
        mats += " [" + mi->GetName() + " blend " + std::to_string(blend) + (wpo ? " +WPO" : "") + "]";
    }
    if (!seen) {
        for (USkeletalMesh*& d : done) if (!d) { d = m; break; }
        Log("item model (%s hand) %s: 1P component CastShadow %d dynamic %d hidden %d | section shadow casting%s -> %d of %d were off%s | materials%s",
            h ? "right" : "left", m->GetName().c_str(), src->CastShadow ? 1 : 0, src->bCastDynamicShadow ? 1 : 0, src->HiddenGame ? 1 : 0,
            flags.c_str(), off, total, (off && g_cfg.itemMeshCastShadow) ? ", switched on" : "", mats.c_str());
    }
    if (g_cfg.itemMeshCastShadow && src->CastShadow && src->bCastDynamicShadow) {   // (only if it would cast: the sword's and crossbow's are CastShadow 0 already - leave the game's component alone)
        src->CastShadow = 0;   // (the real first-person item never casts: it is drawn at the camera)
        src->bCastDynamicShadow = 0;
    }
}

static void UpdateItemShadows(ADishonoredPlayerPawn* pawn) {
    static USkeletalMesh* lastMesh[2] = { nullptr, nullptr };
    for (int h = 0; h < 2; ++h) {
        USkeletalMeshComponent*& copy = g_itemShadow[h];
        auto* item = static_cast<UDishonoredInventoryItem*>(g_handItem[h]);
        USkeletalMeshComponent* src = item ? item->m_pPlayerMesh : nullptr;
        USkeletalMeshComponent* caster = g_shadowArms ? g_shadowArms : g_shadowBody;
        bool want = g_cfg.itemShadows && caster && pawn && pawn->Mesh && g_bodyVisible &&
                    src && src->SkeletalMesh && (h ? g_rightHandBusy : g_leftHandBusy) && g_castHold[h] > 0.5f;   // (put away: see UpdateCastingArmReach)
        if (!want) {
            if (copy) copy->SetScale(0.0001f);   // keep it (attached to the pawn), just cast nothing
            continue;
        }
        if (!copy) {
            copy = static_cast<USkeletalMeshComponent*>(
                StaticConstructObject(USkeletalMeshComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
            if (!copy) continue;
            copy->bUpdateSkelWhenNotRendered = 1;
            if (g_castLightEnv) copy->SetLightEnvironment(g_castLightEnv);
            else if (pawn->Mesh->LightEnvironment) copy->SetLightEnvironment(pawn->Mesh->LightEnvironment);
            copy->HiddenGame = 1;
            copy->CastShadow = 1;
            copy->bCastDynamicShadow = 1;
            copy->bCastHiddenShadow = 1;
            USkeletalMeshComponent* sp = g_cfg.castOneShadow ? ShadowCasterComp() : nullptr;
            if (sp) copy->SetShadowParent(sp);   // one shadow with the body (18:1x: darker where the item crossed it under lamps)
            pawn->AttachComponent(copy);
            copy->SetHidden(true);
            lastMesh[h] = nullptr;
            Log("item shadow component created for the %s hand (shadow parent %p)", h ? "right" : "left", sp);
        }
        {   // (user, clean V3.0 install: "when changing body the weapons' shadows disappear") A body change rebuilds the casting
            // body and its light environment, but this copy is kept - it stayed in the OLD casting body's shadow group (now
            // detached, so nothing drew it) and the old light environment. Follow the current ones.
            USkeletalMeshComponent* sp = g_cfg.castOneShadow ? ShadowCasterComp() : nullptr;
            ULightEnvironmentComponent* le = g_castLightEnv ? static_cast<ULightEnvironmentComponent*>(g_castLightEnv) : pawn->Mesh->LightEnvironment;
            if (copy->ShadowParent != sp || (le && copy->LightEnvironment != le)) {
                if (le && copy->LightEnvironment != le) copy->SetLightEnvironment(le);
                if (copy->ShadowParent != sp) copy->SetShadowParent(sp);
                Log("item shadow (%s hand): now in the current body's shadow (parent %p, light env %p)", h ? "right" : "left", sp, le);
            }
        }
        if (lastMesh[h] != src->SkeletalMesh) {
            PrepareItemMeshShadow(h, src);
            copy->ForcedLodModel = 1;   // (LOD0: a never-rendered component's LOD pick is meaningless)
            copy->SetSkeletalMesh(src->SkeletalMesh, false);
            // The item's own skeleton is animated (the blade's place comes from it); copy that pose, or the ref pose
            // left the sword shadow floating in the air (video 17-08-39).
            copy->SetParentAnimComponent(src);
            // (14:08 clip + log: the casting hand sat 0.2 uu on the real hand yet the sword copy was a streak beside it.
            // Same engine fact as the arm skin: with a ParentAnimComponent the copy is drawn from the PARENT's (the real
            // item's, 1P-frame) LocalToWorld with our Translation/Rotation applied in its raw mesh axes - which is what
            // "floated" every item shadow since build 82. Off: placed from the pawn + its own Translation/Rotation/Scale.)
            copy->bTransformFromAnimParent = 0;
            copy->m_bReallyInheritTransformFromAnimParent = 0;
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
        // The sword / pistol / crossbow / Heart copy rides the CASTING body's hand. (13:48 clip: attached to the game's
        // attachment bone it sat off the hand - on the BODY skeleton handAttachment_X is the coat-cuff carrier, moved off
        // the forearm by a control.) So: the item's pose RELATIVE TO THE 1P HAND, from the 1P arms themselves (attachment
        // bone x RelLoc/RelRot/RelScale, then into the 1P hand's frame - same component, the mesh origin cancels, no
        // LocalToWorld), put onto the casting body's hand, which the reach IK holds at the drawn 1P hand
        // (UpdateCastingArmReach) with the 1P hand's rotation: the copy sits where the real item is and follows the swing.
        FName boneName; boneName.FNameEntryId = found->boneIndex; boneName.InstanceNumber = found->boneNumber;
        Xf atomB1, atomH1, invH1, atomHc;
        bool ok = caster->SkeletalMesh && BoneAtomXf(pawn->Mesh, boneName.ToString().c_str(), atomB1) &&
                  BoneAtomXf(pawn->Mesh, kHandBones[h], atomH1) && XfInv(atomH1, invH1) && BoneAtomXf(caster, kHandBones[h], atomHc);
        if (!ok) { copy->SetScale(0.0001f); continue; }
        Xf rel = XfFromRotator(found->RelRot, found->RelLoc);
        float rs3[3] = { found->RelScale.X, found->RelScale.Y, found->RelScale.Z };
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) rel.m[i][j] *= (rs3[i] > 0.0f ? rs3[i] : 1.0f);
        Xf itemInHand = XfMul(XfMul(rel, atomB1), invH1);   // the item in the 1P hand's frame
        Xf boneComp = XfMul(atomHc, MeshOriginXf(caster->SkeletalMesh));   // (the skin frame's origin order; see MeshOriginXf)
        Xf compLocal = XfFromRotator(caster->Rotation, caster->Translation);   // casting body component -> actor
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) compLocal.m[i][j] *= caster->Scale;
        Xf itemActor = XfMul(XfMul(itemInHand, boneComp), compLocal);
        FVector loc; FRotator rot; float scale;
        XfDecompose(itemActor, loc, rot, scale);
        // Not the hand bone's scale: the IK's stretch scales the arm bones and the sword copy pulsed with it (14:16 clip,
        // "flickering"). The copy's size is the attachment's RelScale under the casting body's own scale.
        scale = caster->Scale * (rs3[0] > 0.0f ? rs3[0] : 1.0f);
        if (!(scale > 0.01f && scale < 100.0f) || !(fabsf(loc.X) < 500.0f && fabsf(loc.Y) < 500.0f && fabsf(loc.Z) < 500.0f)) {
            copy->SetScale(0.0001f);
            continue;
        }
        copy->SetTranslation(loc);
        copy->SetRotation(rot);
        copy->SetScale(scale);
        static DWORD lastPlaceLog[2] = { 0, 0 };
        if (GetTickCount() - lastPlaceLog[h] > 2000) {
            lastPlaceLog[h] = GetTickCount();
            Xf handActor = XfMul(boneComp, compLocal);
            float dx = loc.X - handActor.t[0], dy = loc.Y - handActor.t[1], dz = loc.Z - handActor.t[2];
            Log("item shadow (%s hand) %s: %.0f uu from the casting hand, scale %.2f, cast %d/%d/%d, bounds r %.0f, shadow parent %p",
                h ? "right" : "left", src->SkeletalMesh->GetName().c_str(), sqrtf(dx * dx + dy * dy + dz * dz), scale,
                copy->CastShadow ? 1 : 0, copy->bCastDynamicShadow ? 1 : 0, copy->bCastHiddenShadow ? 1 : 0, copy->Bounds.SphereRadius, copy->ShadowParent);
        }
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
    bool armsToo = pitch * 57.29578f > -25.0f && g_jumpAmount <= 0.0f && !(g_cfg.climbArms && g_climbAmount > 0.0f);
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
    static LONGLONG qpc3 = 0;
    float dt = QpcDt(qpc3);   // (frame-accurate; was GetTickCount steps)
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
        // The height tests alone also fired on plain jumps - running down stairs and jumping, high (power) jumps:
        // 'camera height offset 47/49' with the camera barely pulled back (back 1 / 0 / 7) - and the body shrank away
        // until landing (user, 2026-10-10). A real mantle pulls the camera back too (log: back 36); the game's mantle
        // state and its mantle camera influences (below) catch the rest. Height alone no longer counts.
        if (on && back <= 12.0f && !(back > (mantling ? 15.0f : g_cfg.mantleHideBack))) on = false;
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
    if (g_shadowArms) { if (firstPerson) pawn->AttachComponent(g_shadowArms); else pawn->DetachComponent(g_shadowArms); }
    if (g_holsterCast) { if (firstPerson) pawn->AttachComponent(g_holsterCast); else pawn->DetachComponent(g_holsterCast); }
    if (g_head) { if (firstPerson) pawn->AttachComponent(g_head); else pawn->DetachComponent(g_head); }
    for (int h = 0; h < 2; ++h) if (g_shoulderBlob[h]) { if (firstPerson) pawn->AttachComponent(g_shoulderBlob[h]); else pawn->DetachComponent(g_shoulderBlob[h]); }
    // (18:1x) the item copies join the casting body's shadow (OneShadow): they come and go with it
    for (int h = 0; h < 2; ++h) if (g_itemShadow[h]) { if (firstPerson) pawn->AttachComponent(g_itemShadow[h]); else pawn->DetachComponent(g_itemShadow[h]); }
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
        static LONGLONG qpc11 = 0;
        float dt = QpcDt(qpc11);   // (frame-accurate; was GetTickCount steps)
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
    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };   // same lift for the shadow body
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
    static LONGLONG qpc4 = 0;
    float dt = QpcDt(qpc4);   // (frame-accurate; was GetTickCount steps)
    last = now;
    // Get-up progress (0..1 after the slide ends; 1 = done), the ONE clock for the way out (user 01:20: the slide ->
    // crouch legs were janky - the legs faded over 0.6 s, the pose/raise/recline over a linear 0.4 s, the sink at 90
    // uu/s and the camera over 0.4 s, all stepping on GetTickCount): the amount now follows the same smoothstep as the
    // legs, and SyncTransform lifts the sink on it too. In at 6/s as before.
    static float getUp = 1.0f;
    if (g_isSliding) getUp = 0.0f;
    else if (getUp < 1.0f) getUp = Clamp(getUp + dt / (g_cfg.slideGetUpTime > 0.05f ? g_cfg.slideGetUpTime : 0.05f), 0.0f, 1.0f);
    if (g_isSliding) amount = Clamp(amount + dt * 6.0f, 0.0f, 1.0f);
    else { float ss = 1.0f - getUp * getUp * (3.0f - 2.0f * getUp); if (ss < amount) amount = ss; }
    g_slideAmount = amount;
    // The camera raise comes off at the rate the game lifts its own camera out of the slide (~42 uu in ~0.16 s, log
    // 00:29) - on the slow get-up curve the view went up and back down.
    static float camW = 0.0f;
    // (08:30: the game lifts its camera out of the slide on an exponential - 00:29 log: +3,3,2,2,3,1,2,2,2,1,1,1 uu
    // per 7.5 ms frame, ~0.1 s time constant - while ours came off linearly in 0.17 s: a ~10 uu dip and rise.)
    // (10:40: only holstered - with a weapon out the game's 1P arms are the arms, and shifting them to follow a raised
    // view fought the game's own easing of their height at the slide end: bounces. Weapon slides keep the game's camera.)
    bool raiseWant = g_isSliding;   // (10:50: every slide - the user wants the same height with a weapon out; the 1P arms are lifted with it)
    if (raiseWant) camW = Clamp(camW + dt * 6.0f, 0.0f, 1.0f);
    else camW += (0.0f - camW) * Clamp(dt * 10.0f, 0.0f, 1.0f);
    if (camW < 0.002f) camW = 0.0f;
    g_slideCamW = camW;
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
        // (14:33/14:44 logs: with the casting body added these two stayed at [2] - state[2] = want wrote a zero four bytes
        // past the array every frame, straight onto UpdateClimb's static (the "physics 0 -> 1" every frame), and setFor[2]
        // wrote a pointer past its end onto whatever followed. Moving the victim made the symptom vanish; this is the cause.)
        static USkeletalMeshComponent* setFor[4] = { nullptr, nullptr, nullptr, nullptr };
        static int state[4] = { -1, -1, -1, -1 };
        USkeletalMeshComponent* cs[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
        for (int ci = 0; ci < 4; ++ci) {
            if (!cs[ci]) continue;
            if (setFor[ci] != cs[ci]) { setFor[ci] = cs[ci]; state[ci] = -1; }
            int want = (g_isSliding || g_slideLegW > 0.55f || g_swimAmount > 0.0f || g_climbAmount > 0.0f ||
                        (g_cfg.jumpIdleLegs && g_jumpAmount > 0.2f)) ? 1 : 0;   // legs still until the pose is half faded (08:30: released at 0.15 the stepping snapped on at the end); swimming paddles instead; jumps tuck from the idle
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
            USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
            for (USkeletalMeshComponent* comp : comps) {
                if (!comp) continue;
                for (int i = 0; i < 4; ++i)
                    if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kLegBones[i])))) SetLegRotation(ctl, 0.0f, 0.0f);
                FName toe("foot_R_jnt");
                toe.InstanceNumber = 1;
                if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(toe))) SetLegRotation(ctl, 0.0f, 0.0f);
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
    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
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
            // The other (left) leg, now also straight out (user sketch 00:52): its own twist and swing.
            if (ctl && i == 0 && g_cfg.slideBackTwist != 0.0f)
                ctl->BoneRotation.Roll = static_cast<int>(g_cfg.slideBackTwist * 65536.0f / 360.0f);
            if (ctl && i == 0 && g_cfg.slideBackInward != 0.0f)
                ctl->BoneRotation.Yaw = static_cast<int>(g_cfg.slideBackInward * 65536.0f / 360.0f);
        }
        // The extended right foot: toes pointed (user 01:20), on the toe control the hover pose uses.
        FName toe("foot_R_jnt");
        toe.InstanceNumber = 1;
        if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(toe)))
            SetLegRotation(ctl, g_cfg.kneeSign * g_cfg.slideFootPoint, legW);
    }
}

// Chain/ladder climbing: no climb animation exists for NPCs, so alternate the legs procedurally (hip and knee
// SingleBone controls, bone-space pitch) at a rate driven by the vertical speed. The upper body (and its arms) is
// already hidden while not walking; the 1P hands do the gripping.
static void UpdateClimb(ADishonoredPlayerPawn* pawn) {
    static DWORD last = 0;
    static float amount = 0.0f, phase = 0.0f, moveBlend = 0.0f;
    static bool wrote = false;
    if (!pawn) return;
    int phys = static_cast<int>(pawn->Physics);
    static bool addrLogged = false;
    if (!addrLogged) { addrLogged = true; Log("probe: physLast at %p (UpdateClimb)", static_cast<void*>(&g_physLast)); }
    if (phys != g_physLast) { Log("physics %d -> %d", g_physLast, phys); g_physLast = phys; }
    g_physLastSet = g_physLast;
    g_isClimbing = phys == g_cfg.climbPhysics;
    DWORD now = GetTickCount();
    static LONGLONG qpc5 = 0;
    float dt = QpcDt(qpc5);   // (frame-accurate; was GetTickCount steps)
    last = now;
    // Grab/let go eased over ~0.25 s so the legs swing onto the chain instead of snapping.
    amount += Clamp((g_isClimbing ? 1.0f : 0.0f) - amount, -dt * 4.0f, dt * 4.0f);
    g_climbAmount = amount;
    {   // (18:54 log: Velocity.Z read 0 the whole climb - the chain moves the pawn without a velocity) speed from its height
        static float lastZ = 0.0f;
        static bool haveZ = false;
        float z = pawn->Location.Z;
        if (haveZ && dt > 0.0005f && g_isClimbing) {
            float v = Clamp((z - lastZ) / dt, -800.0f, 800.0f);
            g_climbVz += (v - g_climbVz) * Clamp(dt * 8.0f, 0.0f, 1.0f);
        } else if (!g_isClimbing) g_climbVz = 0.0f;
        lastZ = z; haveZ = true;
    }
    if (amount <= 0.0f && !g_isClimbing) {
        if (wrote) {   // hand the leg controls back cleanly
            USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
            for (USkeletalMeshComponent* comp : comps) {
                if (!comp) continue;
                for (int i = 0; i < 4; ++i)
                    if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kLegBones[i])))) SetLegRotation(ctl, 0.0f, 0.0f);
            }
            wrote = false;
        }
        phase = 0.0f;
        g_climbLegPhase = 0.0f;   // (each grab starts its cycle at the bottom - review)
        g_climbLegMove = 0.0f;
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
        // Climbing (18:45, user: a climbing animation for the legs too): the foot lock - the crossed legs draw the knees
        // up quickly (the hands hold), then push down slowly (standing on the lock while the hands reach), once per
        // ClimbLegStride climbed; run backwards going down. Eased in with the climbing speed; the still grip and its
        // sway when hanging on the chain.
        float& legPhase = g_climbLegPhase;
        float& legMove = g_climbLegMove;
        static DWORD lastClimbLog = 0;
        float bend = 0.0f;
        if (g_cfg.climbLegCycle) {
            float vzc = g_isClimbing ? g_climbVz : 0.0f, speedc = fabsf(vzc);   // (letting go: no more cycle - review)
            legMove += Clamp(Clamp((speedc - 8.0f) / 50.0f, 0.0f, 1.0f) - legMove, -dt * 3.0f, dt * 5.0f);
            float stride = g_cfg.climbLegStride > 5.0f ? g_cfg.climbLegStride : 5.0f;
            legPhase += (vzc >= 0.0f ? 1.0f : -1.0f) * speedc * dt / stride;   // in cycles
            legPhase -= floorf(legPhase);
            auto smooth = [](float x) { x = Clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); };
            auto bendAt = [&](float u) { u -= floorf(u); return u < 0.35f ? smooth(u / 0.35f) : 1.0f - smooth((u - 0.35f) / 0.65f); };
            float bL = bendAt(legPhase), bR = bendAt(legPhase - 0.06f);   // (the right a touch behind: not one rigid block)
            bend = bL;
            float cHL = g_cfg.climbLegPushHip + (g_cfg.climbLegBendHip - g_cfg.climbLegPushHip) * bL;
            float cKL = g_cfg.climbLegPushKnee + (g_cfg.climbLegBendKnee - g_cfg.climbLegPushKnee) * bL;
            float cHR = g_cfg.climbLegPushHip + (g_cfg.climbLegBendHip - g_cfg.climbLegPushHip) * bR;
            float cKR = g_cfg.climbLegPushKnee + (g_cfg.climbLegBendKnee - g_cfg.climbLegPushKnee) * bR;
            hL += (cHL - hL) * legMove; kL += (cKL - kL) * legMove;
            hR += (cHR - hR) * legMove; kR += (cKR - kR) * legMove;
            rL = g_cfg.climbCross - side[0] * (1.0f - legMove);           // (still crossed; the sideways sway fades while climbing)
            rR = -g_cfg.climbCross * 1.3f + side[1] * (1.0f - legMove);
        }
        if (g_isClimbing && (g_cfg.climbLegCycle || g_cfg.climbHands) && GetTickCount() - lastClimbLog > 250) {
            lastClimbLog = GetTickCount();
            Log("climb: speed %.0f (velocity z %.0f) | legs moving %.2f phase %.2f bend %.2f (hip %.0f knee %.0f) | hands %s, grip L %.2f R %.2f | busy L %d R %d",
                g_climbVz, pawn->Velocity.Z, legMove, legPhase, bend, hL, kL, g_climbHandsOn ? "climbing" : "off", g_climbGrip[0], g_climbGrip[1],
                g_leftHandBusy ? 1 : 0, g_rightHandBusy ? 1 : 0);
        }
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
    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
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
    static LONGLONG qpc6 = 0;
    float dt = QpcDt(qpc6);   // (frame-accurate; was GetTickCount steps)
    last = now;
    bool swimming = g_cfg.swimPose && static_cast<int>(pawn->Physics) == g_cfg.swimPhysics;
    amount += Clamp((swimming ? 1.0f : 0.0f) - amount, -dt * 4.0f, dt * 4.0f);
    g_swimAmount = amount;
    static int logged = -1;
    if ((swimming ? 1 : 0) != logged) { Log("swimming %s", swimming ? "started" : "ended"); logged = swimming ? 1 : 0; }
    if (amount <= 0.0f) {
        if (wrote) {
            USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
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
    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
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
    static LONGLONG qpc7 = 0;
    float dt = QpcDt(qpc7);   // (frame-accurate; was GetTickCount steps)
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
            USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
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
    // Knee cap (11:10): the tuck is additive on the frozen stride, so a knee already bent in the run cycle at take-off
    // folded through its thigh. From the drawn pose (hip / knee / foot positions, last frame) less what we added last
    // frame = the animation's own bend; the tuck gets only the room left under JumpKneeMax.
    static float addedKnee[2] = { 0.0f, 0.0f };
    if (g_body && g_cfg.jumpKneeMax > 0.0f) {
        static const char* kFt[2] = { "foot_L_jnt", "foot_R_jnt" };
        for (int sd = 0; sd < 2; ++sd) {
            FVector H = g_body->GetBoneLocation(FName(kLegBones[sd]), 0), K = g_body->GetBoneLocation(FName(kLegBones[2 + sd]), 0),
                    Fo = g_body->GetBoneLocation(FName(kFt[sd]), 0);
            FVector a{ H.X - K.X, H.Y - K.Y, H.Z - K.Z }, b{ Fo.X - K.X, Fo.Y - K.Y, Fo.Z - K.Z };
            float la = sqrtf(a.X * a.X + a.Y * a.Y + a.Z * a.Z), lb = sqrtf(b.X * b.X + b.Y * b.Y + b.Z * b.Z);
            if (la < 1.0f || lb < 1.0f) continue;
            float cosv = Clamp((a.X * b.X + a.Y * b.Y + a.Z * b.Z) / (la * lb), -1.0f, 1.0f);
            float flex = 180.0f - acosf(cosv) * 57.29578f;          // 0 = straight
            float animFlex = flex - addedKnee[sd];
            float allowed = Clamp(g_cfg.jumpKneeMax - animFlex, 0.0f, 180.0f);
            if (values[2 + sd] > allowed) values[2 + sd] = allowed;
            addedKnee[sd] = values[2 + sd] * w;
        }
    }
    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
    for (USkeletalMeshComponent* comp : comps) {
        if (!comp) continue;
        for (int i = 0; i < 4; ++i)
            if (auto* ctl = static_cast<USkelControlSingleBone*>(comp->FindSkelControl(FName(kLegBones[i])))) {
                SetLegRotation(ctl, values[i], w);
                // Hips: a little outward (bone-space yaw, same sense as the slide's inward swing - negative = out) so the
                // tucked knees never pass through each other as the body sways in the air (10:55 clip).
                if (i < 2 && g_cfg.jumpLegSpread != 0.0f) ctl->BoneRotation.Yaw = static_cast<int>(-g_cfg.jumpLegSpread * 65536.0f / 360.0f);
            }
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
    static LONGLONG qpc8 = 0;
    float dt = QpcDt(qpc8);   // (frame-accurate; was GetTickCount steps)
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
    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
    for (int ci = 0; ci < 4; ++ci) {
        if (!comps[ci]) continue;
        UAnimNode* outer = ChildAnim(ChildAnim(ChildAnim(comps[ci]->Animations, 0), 0), 1);   // per-bone R
        if (!outer || outer->Class != UAnimNodeBlendPerBone::StaticClass()) continue;
        UAnimNode* inner = ChildAnim(outer, 0);                                               // per-bone L
        float steady = Clamp(g_cfg.shadowArmSteady, 0.0f, 1.0f);
        bool raises = ci >= 1;   // (18:2x review: the holstered body too - its upper-arm stub beside a raised arm hangs still instead of swinging with the walk; a free side is folded away anyway)
        float r = raises ? g_weaponRaise[1] * steady : 0.0f;
        float l = raises ? g_weaponRaise[0] * steady : 0.0f;
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
    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
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

    USkeletalMeshComponent* comps[4] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast };
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

// The component whose shadow the head, items and holstered shoulders join (OneShadow): the casting body, or the
// shadow body while that one is the caster.
static USkeletalMeshComponent* ShadowCasterComp() {
    if (g_shadowArms) return g_shadowArms;
    if (g_shadowBody && g_shadowBody->CastShadow) return g_shadowBody;
    return nullptr;
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
    if (g_castLightEnv) head->SetLightEnvironment(g_castLightEnv);   // (CreateShadowArms runs first: the casters' own, shadow-casting environment)
    else if (pawn->Mesh && pawn->Mesh->LightEnvironment) head->SetLightEnvironment(pawn->Mesh->LightEnvironment);
    head->HiddenGame = 1;
    head->CastShadow = 1;
    head->bCastDynamicShadow = 1;
    head->bCastHiddenShadow = 1;
    if (g_cfg.castOneShadow) if (USkeletalMeshComponent* sp = ShadowCasterComp()) head->SetShadowParent(sp);   // one shadow with the body (18:1x)
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
    // Full detail always (16:46 clip: the arm skin, which copies this pose, stretched into spikes - never rendered,
    // the shadow body fell to its lowest LOD and stopped updating the hand/finger bones). ForcedLodModel 1 = LOD 0.
    shadow->ForcedLodModel = 1;
    if (pawn->Mesh && pawn->Mesh->LightEnvironment) shadow->SetLightEnvironment(pawn->Mesh->LightEnvironment);
    shadow->HiddenGame = 1;
    shadow->CastShadow = g_cfg.playerShadows ? 1 : 0;   // with shadows off it only serves as the arm skin's pose
    shadow->bCastDynamicShadow = g_cfg.playerShadows ? 1 : 0;
    shadow->bCastHiddenShadow = g_cfg.playerShadows ? 1 : 0;
    pawn->AttachComponent(shadow);
    int arrMark = g_ourArrayCount;
    if (!SetupBodyAnimation(shadow)) {
        Log("shadow body animation failed");
        if (shadow->Animations) DetachNodeArrays(shadow->Animations, 0);
        ForgetArraysSince(arrMark);
        pawn->DetachComponent(shadow);
        return;
    }
    shadow->SetHidden(true);
    g_shadowBody = shadow;
    Log("shadow body created: %p", shadow);
}

// Casting body (13:05, user: "the shadows from the unholstered arms aren't showing"): the arm skin copies g_shadowBody's
// pose, so UpdateArmSkin hides the visible skin arm on a weapon-drawn side by scaling g_shadowBody's shoulder to 0.001 -
// which also took that arm out of the shadow, as g_shadowBody was the caster. One control can't both hide the skin arm
// and cast it, so a second never-rendered body takes over the casting: it gets the same whole-body pose through the
// comps loops (holstered it is identical to g_shadowBody), is never touched by the skin's shrink, and on a busy side its
// arm is posed with ShadowArmPose (UpdateCoat) - the builds 72-81 weapon-arm shadow, driven from its own shoulder, no
// 1P-arm transform (the source of the old floating). g_shadowBody is demoted to a pose source only on success.
static void CreateShadowArms(ADishonoredPlayerPawn* pawn) {
    if (!g_cfg.shadowBody || !g_bodyAnimated || !pawn || !g_shadowBody) return;
    if (!g_cfg.playerShadows || !g_cfg.weaponArmShadow) return;   // nothing to cast / off: g_shadowBody keeps casting
    auto* a = static_cast<USkeletalMeshComponent*>(
        StaticConstructObject(USkeletalMeshComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
    if (!a) { Log("could not create the casting body; the shadow body stays the caster"); return; }
    a->SetSkeletalMesh(ActiveBodyMesh(), false);
    a->bUpdateSkelWhenNotRendered = 1;
    a->bIgnoreControllersWhenNotRendered = 0;   // never rendered: its controls must still run (see CreateShadowBody)
    a->ForcedLodModel = 1;
    if (pawn->Mesh && pawn->Mesh->LightEnvironment) a->SetLightEnvironment(pawn->Mesh->LightEnvironment);
    a->HiddenGame = 1;
    a->CastShadow = 1;
    a->bCastDynamicShadow = 1;
    a->bCastHiddenShadow = 1;
    pawn->AttachComponent(a);
    int arrMark = g_ourArrayCount;
    if (!SetupBodyAnimation(a)) {
        Log("casting body animation failed; the shadow body stays the caster");
        if (a->Animations) DetachNodeArrays(a->Animations, 0);
        ForgetArraysSince(arrMark);
        pawn->DetachComponent(a);
        return;
    }
    a->SetHidden(true);
    g_shadowArms = a;
    ResetArmRoll();
    // Light environment (15:37, user: only the sun casts the player's shadow): everything here borrowed the 1P arms'
    // DynamicLightEnvironment, and first-person arms cast no shadow - so indoor lamps lit the body but never shadowed
    // it; only the sun's whole-scene shadow showed. A character light environment of our own, with bCastShadows:
    // the engine composites the lights around the player into one shadow-casting light, as it does for NPCs.
    if (g_cfg.castLightEnv) {
        auto* le = static_cast<UDynamicLightEnvironmentComponent*>(
            StaticConstructObject(UDynamicLightEnvironmentComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
        if (le) {
            UDynamicLightEnvironmentComponent* ref = (pawn->Mesh && pawn->Mesh->LightEnvironment && pawn->Mesh->LightEnvironment->IsA(UDynamicLightEnvironmentComponent::StaticClass()))
                ? static_cast<UDynamicLightEnvironmentComponent*>(pawn->Mesh->LightEnvironment) : nullptr;
            if (ref) {
                Log("light env: 1P arms' - enabled %d dynamic %d castShadows %d compositeDyn %d synthDir %d synthSH %d character %d filter %d minAngle %.1f",
                    ref->bEnabled ? 1 : 0, ref->bDynamic ? 1 : 0, ref->bCastShadows ? 1 : 0, ref->bCompositeShadowsFromDynamicLights ? 1 : 0,
                    ref->bSynthesizeDirectionalLight ? 1 : 0, ref->bSynthesizeSHLight ? 1 : 0, ref->bIsCharacterLightEnvironment ? 1 : 0,
                    static_cast<int>(ref->ShadowFilterQuality), ref->MinShadowAngle);
                le->ShadowFilterQuality = ref->ShadowFilterQuality;
                le->MinShadowAngle = ref->MinShadowAngle;
                le->NumVolumeVisibilitySamples = ref->NumVolumeVisibilitySamples;
            }
            le->bEnabled = 1;
            le->bDynamic = 1;
            le->bCastShadows = 1;
            le->bCompositeShadowsFromDynamicLights = 1;
            le->bSynthesizeSHLight = 1;
            le->bIsCharacterLightEnvironment = 1;
            le->bForceNonCompositeDynamicLights = 0;
            pawn->AttachComponent(le);
            g_castLightEnv = le;
            a->SetLightEnvironment(le);
            Log("light env: ours created %p (castShadows 1, dynamic, composite) - on the casting body", le);
        } else Log("light env: could not create one; the casters keep the 1P arms' environment");
    }
    g_shadowBody->CastShadow = 0;   // demoted: pose source for the arm skin only (no double shadow)
    g_shadowBody->bCastDynamicShadow = 0;
    g_shadowBody->bCastHiddenShadow = 0;
    Log("casting body created: %p (sole shadow caster; the shadow body is the skin's pose source)", a);
    // The holstered shoulders (user 17:38: "keep the shoulder shadows from the holstered arm shadows always on"): a second
    // never-rendered casting body that gets every pose the casting body gets (legs, lean, slide, jump, coat) except the
    // weapon arm, so its shoulders are always the holstered ones. UpdateHolsterShoulders folds its forearms and hands away
    // on a raised side; its shadow joins the casting body's.
    if (g_cfg.castHolsterShoulders) {
        auto* hc = static_cast<USkeletalMeshComponent*>(
            StaticConstructObject(USkeletalMeshComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
        if (hc) {
            hc->SetSkeletalMesh(ActiveBodyMesh(), false);
            hc->bUpdateSkelWhenNotRendered = 1;
            hc->bIgnoreControllersWhenNotRendered = 0;
            hc->ForcedLodModel = 1;
            hc->SetLightEnvironment(a->LightEnvironment);
            hc->HiddenGame = 1;
            hc->CastShadow = 1;
            hc->bCastDynamicShadow = 1;
            hc->bCastHiddenShadow = 1;
            if (g_cfg.castHolsterShadowParent) hc->SetShadowParent(a);
            pawn->AttachComponent(hc);
            int hcMark = g_ourArrayCount;
            if (SetupBodyAnimation(hc)) {
                hc->SetHidden(true);
                g_holsterCast = hc;
                Log("holstered-shoulder body created: %p (shadow parent %p, upper arm %.2f) | array registry %d of 1024", hc, g_cfg.castHolsterShadowParent ? a : nullptr, g_cfg.castHolsterUpperArm, g_ourArrayCount);
            } else {
                if (hc->Animations) DetachNodeArrays(hc->Animations, 0);
                ForgetArraysSince(hcMark);
                pawn->DetachComponent(hc);
                Log("holstered-shoulder body: animation setup failed - off");
            }
        } else Log("holstered-shoulder body: could not create it - off");
    }
    // Shoulder blobs (user 15:25: "fill the gaps with a shadow"): when the IK raises an arm the rig's shoulder mass does
    // not follow and a notch opens between the torso and the upper arm. A never-rendered, shadow-only copy of the head
    // mesh, scaled to a deltoid, sits on that shoulder joint while the arm is up (placed in UpdateCastingArmReach).
    USkeletalMesh* blobMesh = (g_builtChar == 1 && g_daudHeadMesh) ? g_daudHeadMesh : (g_builtChar == 2 && g_ovsHeadMesh) ? g_ovsHeadMesh : g_headMesh;
    for (int h = 0; h < 2 && g_cfg.castShoulderBlob > 0.0f && blobMesh; ++h) {
        auto* b = static_cast<USkeletalMeshComponent*>(
            StaticConstructObject(USkeletalMeshComponent::StaticClass(), pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
        if (!b) break;
        b->SetSkeletalMesh(blobMesh, false);
        b->bUpdateSkelWhenNotRendered = 1;
        if (g_castLightEnv) b->SetLightEnvironment(g_castLightEnv);
        else if (pawn->Mesh && pawn->Mesh->LightEnvironment) b->SetLightEnvironment(pawn->Mesh->LightEnvironment);
        b->HiddenGame = 1;
        b->CastShadow = 1;
        b->bCastDynamicShadow = 1;
        b->bCastHiddenShadow = 1;
        pawn->AttachComponent(b);
        b->SetHidden(true);
        b->SetScale(0.0001f);
        g_shoulderBlob[h] = b;
    }
    if (g_shoulderBlob[0]) Log("shoulder blobs created (%s, scale %.2f)", blobMesh->GetName().c_str(), g_cfg.castShoulderBlob);
}

static void DetachBody() {
    if (g_head && g_bodyPawn) g_bodyPawn->DetachComponent(g_head);
    for (int h = 0; h < 2; ++h) if (g_shoulderBlob[h] && g_bodyPawn) g_bodyPawn->DetachComponent(g_shoulderBlob[h]);
    if (g_castLightEnv && g_bodyPawn) g_bodyPawn->DetachComponent(g_castLightEnv);
    if (g_shadowBody && g_bodyPawn) g_bodyPawn->DetachComponent(g_shadowBody);
    if (g_shadowArms && g_bodyPawn) g_bodyPawn->DetachComponent(g_shadowArms);
    if (g_holsterCast && g_bodyPawn) g_bodyPawn->DetachComponent(g_holsterCast);
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
    g_shoulderBlob[0] = g_shoulderBlob[1] = nullptr;
    g_castLightEnv = nullptr;
    g_shadowBody = nullptr;
    g_shadowArms = nullptr;
    g_holsterCast = nullptr;
    g_itemShadow[0] = g_itemShadow[1] = nullptr;   // owned by the pawn; dropped with it
        g_armsShadow = nullptr;
        g_armSkin = nullptr;
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
    CreateShadowArms(pawn);
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

// Corvo's first-person arms model: the game's own (kept in g_armsSwap.orig while Daud's/the disguise's are shown).
static USkeletalMesh* CorvoArmsMesh(ADishonoredPlayerPawn* pawn) {
    if (g_armsSwap.orig) return g_armsSwap.orig;
    if (g_armsChar == 0 && pawn && pawn->Mesh) return pawn->Mesh->SkeletalMesh;
    return nullptr;
}

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
        g_ovsArmsMat = static_cast<UMaterialInterface*>(FindObjectByName("MaterialInstanceConstant", "DLC07_Player_DaudOverseer_inst"));
        if (g_ovsArmsMat) Protect(g_ovsArmsMat);
        Log("Overseer arms material: %s", g_ovsArmsMat ? g_ovsArmsMat->GetFullName().c_str() : "NOT FOUND");
    }
    for (USkeletalMesh* m : { g_ovsBodyMesh, g_ovsHeadMesh, g_ovsArmsMesh, g_ovsSwordMesh }) Protect(m);
    auto name = [](USkeletalMesh* m) { return m ? m->GetFullName() : std::string("NOT FOUND"); };
    Log("Overseer meshes: body %s, head %s, arms %s, sword %s", name(g_ovsBodyMesh).c_str(), name(g_ovsHeadMesh).c_str(),
        name(g_ovsArmsMesh).c_str(), name(g_ovsSwordMesh).c_str());
    g_ovsLoadState = (g_ovsBodyMesh || g_ovsArmsMesh || g_ovsSwordMesh) ? LoadState::Loaded : LoadState::Failed;
}

static void ForgetOverseer() {
    for (USkeletalMesh** m : { &g_ovsBodyMesh, &g_ovsHeadMesh, &g_ovsArmsMesh, &g_ovsSwordMesh }) { LetGo(*m); *m = nullptr; }
    if (g_ovsArmsMat) { LetGo(g_ovsArmsMat); g_ovsArmsMat = nullptr; }
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
    if (g_shadowArms && g_shadowArms->Animations) DetachNodeArrays(g_shadowArms->Animations, 0);
    if (g_holsterCast && g_holsterCast->Animations) DetachNodeArrays(g_holsterCast->Animations, 0);
    DetachOurArrays(g_body);
    g_body = nullptr;
    g_head = nullptr;
    g_shoulderBlob[0] = g_shoulderBlob[1] = nullptr;
    g_castLightEnv = nullptr;
    g_shadowBody = nullptr;
    g_shadowArms = nullptr;
    g_holsterCast = nullptr;
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
            if (!m || m == g_ovsArmsMat) continue;   // (ours - the Overseer arms' own material, ApplyArmsCharacter)
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
// ---- Per-bow upgrades: DIAGNOSTICS (2026-10-12, user: Piero sells the active bow's upgrades; each bow upgraded on its
// own). Read-only: what each bow supports, what every bow-related upgrade does (host class, attributes), what the player
// owns, Piero's store list, and how a bow swap / a purchase changes the owned list and the attribute modifiers.
static std::string FStr(const void* fstring) {
    const RawArray* a = static_cast<const RawArray*>(fstring);
    return (a->Data && a->Num > 0) ? Narrow(static_cast<const wchar_t*>(a->Data)) : std::string();
}

static std::string UpgName(UObject* u) { return u ? u->GetName() : std::string("null"); }

static std::string DescribeUpgrade(UObject* u) {
    if (!u) return "null";
    uint8_t* b = reinterpret_cast<uint8_t*>(u);
    UObject* host = *reinterpret_cast<UObject**>(b + 0x90);
    std::string out = ObjName(u) + " | host " + (host ? host->GetName() : std::string("none")) + " | attrs";
    auto* at = reinterpret_cast<RawArray*>(b + 0x9C);
    for (int k = 0; at->Data && k < at->Num && k < 8; ++k) {
        const uint8_t* e = static_cast<const uint8_t*>(at->Data) + k * 12;
        const FName& n = *reinterpret_cast<const FName*>(e);
        char v[32]; snprintf(v, sizeof(v), "%g", *reinterpret_cast<const float*>(e + 8));
        out += " " + (n.IsValid() ? n.ToString() : std::string("?")) + "=" + v;
    }
    out += " | section " + std::to_string(static_cast<int>(b[0xD1]));
    if (UObject* parent = *reinterpret_cast<UObject**>(b + 0xE0)) out += " | parent " + parent->GetName();
    if (UObject* add = *reinterpret_cast<UObject**>(b + 0xE4)) out += " | adds-if-missing " + add->GetName();
    auto* rv = reinterpret_cast<RawArray*>(b + 0xA8);
    for (int k = 0; rv->Data && k < rv->Num && k < 6; ++k) out += " | reverts " + UpgName(static_cast<UObject**>(rv->Data)[k]);
    if (UObject* am = *reinterpret_cast<UObject**>(b + 0x94)) out += " | attach " + am->GetName();
    out += " | \"" + FStr(b + 0xB8) + "\"";
    return out;
}

static std::string OwnedUpgrades(ADishonoredPlayerPawn* pawn) {
    if (!pawn) return "-";
    auto* a = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(pawn) + 0xB50);
    std::string out = std::to_string(a->Data ? a->Num : 0) + ":";
    for (int k = 0; a->Data && k < a->Num && k < 64; ++k) out += " " + UpgName(static_cast<UObject**>(a->Data)[k]);
    return out;
}

static std::string AttrMods(UObject* attributes) {   // UDisAttributes::m_AttributesWithModifiers (+0x74, TArray<FName>)
    if (!attributes || IsBadReadPtr(attributes, 0x80)) return "-";
    auto* a = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(attributes) + 0x74);
    std::string out;
    for (int k = 0; a->Data && k < a->Num && k < 32; ++k) {
        const FName& n = static_cast<const FName*>(a->Data)[k];
        out += (k ? " " : "") + (n.IsValid() ? n.ToString() : std::string("?"));
    }
    return out.empty() ? std::string("none") : out;
}

static std::string ItemMods(UObject* item) { return item ? AttrMods(*reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(item) + 0xE4)) : std::string("-"); }
static std::string PawnMods(ADishonoredPlayerPawn* pawn) { return pawn ? AttrMods(*reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(pawn) + 0x77C)) : std::string("-"); }

static void DumpUpgradeData(ADishonoredPlayerPawn* pawn, const char* why) {
    Log("=== per-bow upgrades (%s)", why);
    struct Bow { const char* label; const char* cls; const char* name; } bows[] = {
        { "Corvo's crossbow", "DisTweaks_WepCrossbow", "Twk_Inv_CrossbowCorvo" },
        { "Daud's wristbow (DLC07)", "DisDLC06Tweaks_WepCrossbow", "DLC07_Inv_WristbowDaud" },
        { "Daud's wristbow (DLC06)", "DisDLC06Tweaks_WepCrossbow", "DLC06_Inv_WristbowDaud" } };
    for (const Bow& bw : bows) {
        UObject* t = FindObjectByName(bw.cls, bw.name);
        if (!t) { Log("  %s: tweaks %s not loaded", bw.label, bw.name); continue; }
        uint8_t* tb = reinterpret_cast<uint8_t*>(t);
        UObject* ps = *reinterpret_cast<UObject**>(tb + 0x104);
        UObject* fb = *reinterpret_cast<UObject**>(tb + 0x88);
        UObject* spawn = *reinterpret_cast<UObject**>(tb + 0x60);
        Log("  %s: %s | spawns %s | fallback %s | player-specific %s", bw.label, ObjName(t).c_str(), spawn ? spawn->GetName().c_str() : "-",
            ObjName(fb).c_str(), ObjName(ps).c_str());
        if (!ps) continue;
        auto* sup = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(ps) + 0x170);
        if (!sup->Data || sup->Num <= 0) Log("    supports no upgrades");
        for (int k = 0; sup->Data && k < sup->Num && k < 32; ++k) Log("    supports %s", DescribeUpgrade(static_cast<UObject**>(sup->Data)[k]).c_str());
    }
    // every upgrade object that sounds bow-related
    auto* objects = UObject::GObjObjects();
    int found = 0;
    for (int i = 0; i < objects->size(); ++i) {
        UObject* o = objects->at(i);
        if (!o || !o->Class || o->Class->GetName().find("Tweaks_Upgrade") == std::string::npos) continue;
        std::string n = o->GetName();
        if (n.rfind("Default__", 0) == 0) continue;
        std::string low = n;
        for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        bool bowish = low.find("crossbow") != std::string::npos || low.find("wristbow") != std::string::npos || low.find("quiver") != std::string::npos ||
                      low.find("sleep") != std::string::npos || low.find("bolt") != std::string::npos || low.find("arrow") != std::string::npos ||
                      low.find("dart") != std::string::npos;
        if (!bowish) continue;
        ++found;
        Log("  upgrade %s", DescribeUpgrade(o).c_str());
    }
    Log("  %d bow-related upgrade objects in memory", found);
    if (pawn) {
        UObject* pt = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(pawn) + 0x748);   // m_pPawnTweaks
        Log("  pawn tweaks %s: crossbow range upgrade %s", ObjName(pt).c_str(), pt ? UpgName(*reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(pt) + 0x1C4)).c_str() : "-");
    }
    if (UObject* dpt = FindObjectByName("DisDLC07Tweaks_PlayerPawn", "DLC07_Twk_Player_Daud"))
        Log("  Daud's pawn tweaks: crossbow range upgrade %s", UpgName(*reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(dpt) + 0x1C4)).c_str());
    Log("  owned %s", OwnedUpgrades(pawn).c_str());
    Log("  pawn modifiers: %s", PawnMods(pawn).c_str());
    // stores
    for (int i = 0; i < objects->size(); ++i) {
        UObject* o = objects->at(i);
        if (!o || !o->Class || o->Class->GetName() != "DisTweaks_Store") continue;
        if (o->GetName().rfind("Default__", 0) == 0) continue;
        auto* items = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(o) + 0xC4);
        Log("  store %s: %d entries", ObjName(o).c_str(), items->Data ? items->Num : 0);
        for (int k = 0; items->Data && k < items->Num && k < 64; ++k) {
            uint8_t* e = static_cast<uint8_t*>(items->Data) + k * 0x88;
            UObject* item = *reinterpret_cast<UObject**>(e + 0x5C);
            auto* pre = reinterpret_cast<RawArray*>(e + 0x34);
            std::string pres;
            for (int q = 0; pre->Data && q < pre->Num && q < 6; ++q) pres += " " + UpgName(static_cast<UObject**>(pre->Data)[q]);
            auto* cost = reinterpret_cast<RawArray*>(e + 0x28);
            int price = (cost->Data && cost->Num > 0) ? *static_cast<int*>(cost->Data) : -1;
            Log("    [%d] \"%s\" sub %d | item %s (%s) | needs%s | vis %d | cost %d", k, FStr(e).c_str(), *reinterpret_cast<int*>(e + 0x24),
                UpgName(item).c_str(), item && item->Class ? item->Class->GetName().c_str() : "-", pres.empty() ? " -" : pres.c_str(),
                *reinterpret_cast<int*>(e + 0x40), price);
        }
    }
}

// Once a second: what changed in the owned list (a purchase, a pickup, a load), with the bow in the slot at the time.
static int CrossbowSlot(ADishonoredPlayerPawn* pawn);
static UDishonoredInventoryItem*& SlotItem(ADishonoredPlayerPawn* pawn, int slot);
static void CarryBowUpgrades(ADishonoredPlayerPawn* pawn, UDishonoredInventoryItem* item, const char* why);
static void WatchOwnedUpgrades(ADishonoredPlayerPawn* pawn) {
    static ADishonoredPlayerPawn* forPawn = nullptr;
    static std::vector<UObject*> last;
    static DWORD lastCheck = 0;
    if (!pawn) return;
    DWORD now = GetTickCount();
    if (pawn == forPawn && now - lastCheck < 1000) return;
    lastCheck = now;
    auto* a = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(pawn) + 0xB50);
    std::vector<UObject*> cur;
    for (int k = 0; a->Data && k < a->Num && k < 256; ++k) cur.push_back(static_cast<UObject**>(a->Data)[k]);
    if (pawn != forPawn) {
        forPawn = pawn;
        last = cur;
        Log("upgrades: pawn %p owns %s", pawn, OwnedUpgrades(pawn).c_str());
        return;
    }
    if (cur == last) return;
    int slot = CrossbowSlot(pawn);
    UObject* bow = slot >= 0 ? SlotItem(pawn, slot) : nullptr;
    bool bowUpgradeGained = false;
    for (UObject* u : cur) if (std::find(last.begin(), last.end(), u) == last.end()) {
        Log("upgrades: GAINED %s | bow in slot %s | its modifiers: %s | pawn modifiers: %s", DescribeUpgrade(u).c_str(), ObjName(bow).c_str(), ItemMods(bow).c_str(), PawnMods(pawn).c_str());
        std::string n = UpgName(u);
        if (n.rfind("Twk_Upgrade_Crossbow_", 0) == 0) bowUpgradeGained = true;
    }
    if (bowUpgradeGained && bow) CarryBowUpgrades(pawn, static_cast<UDishonoredInventoryItem*>(bow), "bought");
    for (UObject* u : last) if (std::find(cur.begin(), cur.end(), u) == cur.end())
        Log("upgrades: LOST %s | bow in slot %s", UpgName(u).c_str(), ObjName(bow).c_str());
    last = cur;
}

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
    if (!daudDumped && g_daudLoadState == LoadState::Loaded) { daudDumped = true; DumpDaudPowers(); DumpCrossbowTweaks(pawn); DumpUpgradeData(pawn, "Daud's package loaded"); }
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
    // Without Better Powers: Blink, ForgetPowerSwaps lets go of the pawn tweaks on every new pawn (save load), but
    // Daud's package often stays loaded, so nothing grabbed them again: "waiting for Daud's power tweaks (not found)"
    // forever - his icons, Corvo's powers (2026-10-10, CorvoBody + loader only). Look them up again (every 2 s at most).
    if (!g_daudPawnTweaks && g_daudLoadState == LoadState::Loaded) {
        static DWORD lastTry = 0;
        DWORD now = GetTickCount();
        if (!lastTry || now - lastTry > 2000) {
            lastTry = now;
            if (FindObjectByName("DisDLC07Tweaks_PlayerPawn", "DLC07_Twk_Player_Daud")) GrabDaudTweaks();
        }
    }
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

static bool BetterPowersBlinkLoaded();
static void ForgetCorvoBlink();

static void ForgetPowerSwaps() {
    for (auto& s : g_powerSwaps) { LetGo(s.orig); LetGo(s.daud); s.orig = s.daud = nullptr; s.daudPc = nullptr; }
    // Our Corvo Blink and the mod's detached one reference the pawn: still rooted at a level change they kept the old
    // world alive ("World ... not cleaned up by garbage collection", 15:32). Let go with the other swaps.
    ForgetCorvoBlink();
    // Better Powers: Blink keeps a pointer to Daud's Transversal tweaks (DLC06_Pwr_Transversal, reached from these
    // pawn tweaks) and writes its baseline values through it. Letting go here freed Daud's package on every level
    // change and the reload made a new tweaks object, so the mod then wrote 1400/400 into whatever reused the old
    // memory and the next GC crashed (2026-10-08, 13:16). With that mod, keep Daud's tweaks rooted for the session:
    // the reload then finds the same objects.
    if (BetterPowersBlinkLoaded() && g_daudPawnTweaks) {
        Protect(g_daudPawnTweaks);
    } else {
        LetGo(g_daudPawnTweaks);
        g_daudPawnTweaks = nullptr;
    }
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

// Blink target clearance (0xBE8F20, only caller the targeting code at 0xBF5DDB). The game backs the target off from
// what the aim hit, along the view, in steps of half the player's bounding-sphere radius until a player-sized box fits
// (two SingleLineChecks, 0x64E7A0, trace flags 0x4060DF). Aimed at an NPC that lands up to a whole step further back
// than needed ("can never blink directly to an npc", 2026-10-09). Keep the game's answer, then search the last step
// for the closest spot that still fits - never closer than the game's own fit test allows (no landing inside anyone).
using BlinkClearanceFn = float(__thiscall*)(void* self, float hx, float hy, float hz, float dx, float dy, float dz);
static BlinkClearanceFn g_origBlinkClearance = nullptr;

static bool BlinkSpotFits(void* pawn, const float* p, const float* extent) {
    using LineCheckFn = int(__thiscall*)(void* world, void* hit, void* src, const float* end, const float* start,
                                        uint32_t flags, const float* extent, void* light);
    auto check = reinterpret_cast<LineCheckFn>(0x0064E7A0);
    for (float off : { 1.0f, -1.0f }) {   // the game's own two tests: from p+1 and from p-1 to p
        alignas(16) uint8_t hit[0x80] = {};
        *reinterpret_cast<int*>(hit + 0x24) = -1;
        *reinterpret_cast<int*>(hit + 0x44) = -1;
        float start[3] = { p[0] + off, p[1] + off, p[2] + off };
        if (!check(GetWorld(), hit, pawn, p, start, 0x4060DF, extent, nullptr)) return false;
    }
    return true;
}

// The targeting code's frame (its EBP when it calls 0xBE8F20) holds the aim trace's FCheckResult: Actor at EBP-0xB4,
// Location at EBP-0xB0. A naked stub keeps that EBP for the hook (diagnostics: what the aim hit).
extern "C" uintptr_t g_blinkCallerEbp = 0;
extern "C" void* g_blinkHookTarget = nullptr;
extern "C" __attribute__((naked)) void BlinkClearanceStub() {
    __asm__("movl %ebp, _g_blinkCallerEbp\n\t"
            "jmp *_g_blinkHookTarget");
}

static void LogBlinkAim(float back, float tight, float hx, float hy, float hz) {
    static int logged = 0;
    if (logged >= 60 || g_blinkCallerEbp < 0x10000) return;
    auto* actor = *reinterpret_cast<UObject**>(g_blinkCallerEbp - 0xB4);
    static UObject* last = reinterpret_cast<UObject*>(1);
    static float lastBack = -1.0f;
    if (actor == last && fabsf(back - lastBack) < 1.0f) return;   // only changes
    last = actor; lastBack = back; ++logged;
    std::string cls = actor && actor->Class ? actor->Class->GetName() : std::string("none");
    float dist = -1.0f;
    if (actor && actor->IsA(AActor::StaticClass())) {
        const FVector& l = static_cast<AActor*>(actor)->Location;
        dist = sqrtf((l.X - hx) * (l.X - hx) + (l.Y - hy) * (l.Y - hy));
    }
    Log("Blink aim: hit %s (%s), hit-to-actor %.0f, back-off %.0f -> %.0f", cls.c_str(), actor ? actor->GetName().c_str() : "-",
        dist, back, tight);
}

// Blink through NPCs, with "Pass Through Stumble" (2026-10-09: "lets just make it so we can blink through Npcs";
// enemies blinked through stagger - user named that Pass Through Stumble and wants it kept). The aim already passes through
// them, but the Blink move is stopped by their collision, so you end up short of (pushed back from) the NPC. While
// Blink is being aimed and for 1.5 s after (the move), living pawns don't block: AActor.bBlockActors (+0x128 bit 0)
// and their cylinder's BlockActors (+0x118 bit 0x40) are cleared and put back afterwards. Restored only for pawns
// still in the level's pawn list, so a pawn removed in between is never written.
struct GhostedPawn { APawn* pawn; bool actorBlock; UPrimitiveComponent* cyl; bool cylBlock; };
static std::vector<GhostedPawn> g_ghosted;
static ULONGLONG g_lastBlinkAim = 0;
static bool g_ghostActive = false;

static void GhostNpcs(APawn* player) {
    g_lastBlinkAim = GetTickCount64();
    if (g_ghostActive || !player || !player->WorldInfo) return;
    g_ghostActive = true;
    int n = 0;
    for (APawn* p = player->WorldInfo->PawnList; p && n < 2048; p = p->NextPawn, ++n) {
        if (p == player) continue;
        auto* flags = reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(p) + 0x128);
        UPrimitiveComponent* cyl = reinterpret_cast<UPrimitiveComponent*>(p->CylinderComponent);
        auto* cflags = cyl ? reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(cyl) + 0x118) : nullptr;
        GhostedPawn g{ p, (*flags & 1u) != 0, cyl, cflags && (*cflags & 0x40u) != 0 };
        if (!g.actorBlock && !g.cylBlock) continue;
        *flags &= ~1u;
        if (cflags) *cflags &= ~0x40u;
        g_ghosted.push_back(g);
    }
    static int logged = 0;
    if (logged++ < 10) Log("Blink through NPCs: %d pawn(s) not blocking (pawn list %d)", static_cast<int>(g_ghosted.size()), n);
}

static void RestoreNpcs(APawn* player, bool force) {
    if (!g_ghostActive || (!force && GetTickCount64() - g_lastBlinkAim < 1500)) return;
    if (player && player->WorldInfo) {
        int n = 0;
        for (APawn* p = player->WorldInfo->PawnList; p && n < 2048; p = p->NextPawn, ++n)
            for (const GhostedPawn& g : g_ghosted) {
                if (g.pawn != p) continue;
                if (g.actorBlock) *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(p) + 0x128) |= 1u;
                if (g.cylBlock && g.cyl == reinterpret_cast<UPrimitiveComponent*>(p->CylinderComponent))
                    *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(g.cyl) + 0x118) |= 0x40u;
            }
    }
    g_ghosted.clear();
    g_ghostActive = false;
}

static float __fastcall HookedBlinkClearance(void* self, void*, float hx, float hy, float hz, float dx, float dy, float dz) {
    float back = g_origBlinkClearance(self, hx, hy, hz, dx, dy, dz);
    float result = back;
    struct AimLog { float& r; float b, x, y, z; ~AimLog() { LogBlinkAim(b, r, x, y, z); } } aimLog{ result, back, hx, hy, hz };
    {
        auto* pawn = *reinterpret_cast<char**>(static_cast<char*>(self) + 0x80);
        auto* coll = pawn ? *reinterpret_cast<char**>(pawn + 0x208) : nullptr;   // CollisionComponent
        if (!coll) return back;
        GhostNpcs(reinterpret_cast<APawn*>(pawn));
        if (!(back > 0.0f)) return back;
        float step = *reinterpret_cast<float*>(coll + 0xE4) * 0.5f;              // Bounds.SphereRadius / 2
        float extent[3] = { *reinterpret_cast<float*>(coll + 0xD8), *reinterpret_cast<float*>(coll + 0xDC),
                            *reinterpret_cast<float*>(pawn + 0x290) };
        if (!(step > 0.0f) || back - step < -0.01f) return back;
        auto at = [&](float b, float* p) { p[0] = hx - dx * b; p[1] = hy - dy * b; p[2] = hz - dz * b; };
        float p[3];
        at(back, p);
        if (!BlinkSpotFits(pawn, p, extent)) return back;   // the game ran out of room: leave its answer alone
        float lo = back - step, hi = back;                      // lo failed (the game's previous try), hi fits
        for (int i = 0; i < 6; ++i) {
            float mid = 0.5f * (lo + hi);
            at(mid, p);
            (BlinkSpotFits(pawn, p, extent) ? hi : lo) = mid;
        }
        result = hi;
        return hi;
    }
}

static void HookBlinkClearance() {
    const uint8_t expect[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x84, 0x00, 0x00, 0x00 };
    if (memcmp(reinterpret_cast<void*>(0x00BE8F20), expect, sizeof(expect)) == 0 &&
        (g_blinkHookTarget = reinterpret_cast<void*>(&HookedBlinkClearance)) &&
        MH_CreateHook(reinterpret_cast<void*>(0x00BE8F20), reinterpret_cast<void*>(&BlinkClearanceStub),
                      reinterpret_cast<void**>(&g_origBlinkClearance)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(0x00BE8F20)) == MH_OK) Log("Blink clearance hooked");
    else Log("WARNING: could not hook Blink clearance");
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

// With Better Powers: Blink installed, the pawn's Transversal PowerBlink is that mod's own, live component (it swaps
// the PowerBlink template's class at startup), not a leftover: converting it back fought the mod's swap and left an
// object the garbage collector walked with the wrong layout (crashes 2026-10-08, GC at 0x465894 reading Better
// Powers' range values as object references). So the Transversal fix only runs without that mod.
static bool BetterPowersBlinkLoaded() {
    static int loaded = -1;
    if (loaded < 0) {
        loaded = GetModuleHandleA("BetterPowersBlink.dll") != nullptr;
        Log("Better Powers: Blink %s - legacy Transversal fix %s", loaded ? "loaded" : "not loaded", loaded ? "off" : "on");
    }
    return loaded == 1;
}

static void FixLegacyPowerComponents(ADishonoredPlayerPawn* pawn, RawArray* arr) {
    for (int i = 0; i < arr->Num; ++i) {
        auto* c = static_cast<UDishonoredActivePowerComponent**>(arr->Data)[i];
        if (!c || c->Outer != pawn) continue;   // ours are in the transient package
        for (const LegacyFix& f : kLegacyFixes) {
            if (c->Class->GetName() != f.daudClass) continue;
            if (f.corvoClass == kLegacyFixes[0].corvoClass && BetterPowersBlinkLoaded()) continue;
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
    if (g_daudUiLoading || g_daudUiNext >= static_cast<LONG>(sizeof(kDaudUiPackages) / sizeof(kDaudUiPackages[0])) || !RawLoadPackageAsync) return;
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

// ---- Corvo's own Blink with Better Powers: Blink installed (2026-10-08). That mod swaps the PowerBlink template's
// class to Daud's Transversal, so Corvo's Blink is a time-pausing Transversal even with Body Swap's Powers row on
// Corvo. Converting it in place crashed (the mod keeps using it), so with the Corvo set we put a separate, genuine
// Corvo Blink (transient package, from the class defaults: name Blink, Corvo's tweaks) in the slot and keep the mod's
// component detached and alive; with Daud's set (or the mod gone) the mod's component goes back first, so Daud's
// swap below takes it as "Corvo's original" as before. Saves write the pawn's own component (HookedPawnGameSave,
// HookedGameSaveInventory) and levels by name, so the mod's component gets this one's level.
static UDishonoredActivePowerComponent* g_corvoBlink = nullptr;   // ours (transient)
static UDishonoredActivePowerComponent* g_bpBlink = nullptr;      // the mod's (pawn's own), detached while ours is in

static void ForgetCorvoBlink() {
    LetGo(g_corvoBlink); LetGo(g_bpBlink);
    g_corvoBlink = g_bpBlink = nullptr;
}

static void SwapPowerSlot(ADishonoredPlayerPawn* pawn, int idx, UDishonoredActivePowerComponent* out, UDishonoredActivePowerComponent* in) {
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    in->m_CurrentLevel = out->m_CurrentLevel;
    CancelPowerIfActive(out);
    pawn->DetachComponent(out);
    static_cast<UDishonoredActivePowerComponent**>(arr->Data)[idx] = in;
    pawn->AttachComponent(in);
    RetargetPowerPointers(pawn, out, in);
    PowerLevelChanged(pawn, idx, in->m_CurrentLevel);
}

// Put the mod's Blink back (Daud's set, or the mod not loaded).
static void RestoreModBlink(ADishonoredPlayerPawn* pawn) {
    if (!g_corvoBlink || !g_bpBlink) return;
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    for (int i = 0; arr->Data && i < arr->Num; ++i) {
        if (static_cast<UDishonoredActivePowerComponent**>(arr->Data)[i] != g_corvoBlink) continue;
        Log("Corvo Blink: Better Powers' Blink back in slot %d (level %d)", i, g_corvoBlink->m_CurrentLevel);
        SwapPowerSlot(pawn, i, g_corvoBlink, g_bpBlink);
        LetGo(g_bpBlink);
        g_bpBlink = nullptr;
        return;
    }
}

// Corvo's set with the mod: replace its Transversal Blink with a genuine Corvo Blink.
static void InstallCorvoBlink(ADishonoredPlayerPawn* pawn) {
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    static UClass* transversal = nullptr;   // runs every tick: look the class up once (FindClass walks every object)
    if (!transversal) transversal = UObject::FindClass("Class DishonoredGame.DisDLC06ActivePowerComponent_Transversal");
    for (int i = 0; transversal && arr->Data && i < arr->Num; ++i) {
        auto* cur = static_cast<UDishonoredActivePowerComponent**>(arr->Data)[i];
        if (!cur || cur == g_corvoBlink || cur->Outer != pawn || cur->Class != transversal) continue;
        if (cur->m_PowerName.ToString() != "Blink") continue;   // the mod's (Corvo's name), not Daud's own
        if (!g_corvoBlink) {
            UClass* cls = UDishonoredActivePowerComponent_Blink::StaticClass();
            g_corvoBlink = static_cast<UDishonoredActivePowerComponent*>(
                StaticConstructObject(cls, TransientPackage() ? TransientPackage() : pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
            if (!g_corvoBlink) { Log("Corvo Blink: could not construct"); return; }
            KeepAlive(g_corvoBlink);
            Log("Corvo Blink: made %s (name %s, ui %d, tweaks %s)", ObjName(g_corvoBlink).c_str(),
                g_corvoBlink->m_PowerName.ToString().c_str(), static_cast<int>(g_corvoBlink->m_eUISelection),
                ObjName(g_corvoBlink->m_pPowerTweaks).c_str());
        }
        Log("Corvo Blink: in for Better Powers' Blink in slot %d (level %d)", i, cur->m_CurrentLevel);
        KeepAlive(cur);
        g_bpBlink = cur;
        SwapPowerSlot(pawn, i, cur, g_corvoBlink);
        return;
    }
}

// Better Powers: Blink puts its menu settings (range, horizontal/vertical range, cooldown, mana cost) on the tweaks
// of its own Blink component (the pawn's own PowerBlink). With Body Swap the Blink in use is ours (Corvo's genuine
// Blink, or Daud's Transversal with his tweaks), and the mod's provisioning of Daud's tweaks does not survive Body
// Swap's package handling ("baseline capture FAILED", 2026-10-08 15:17) - so copy those settings onto the tweaks of
// the Blink actually in the slot. Logged whenever something is copied.
static void SyncBlinkSettings(ADishonoredPlayerPawn* pawn, bool wantDaud) {
    UDishonoredActivePowerComponent* mod = g_bpBlink ? g_bpBlink : g_powerSwaps[0].orig;
    UDishonoredActivePowerComponent* active = wantDaud ? g_powerSwaps[0].daud : g_corvoBlink;
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    bool inSlot = false;
    for (int i = 0; active && arr->Data && i < arr->Num; ++i) inSlot |= static_cast<UObject**>(arr->Data)[i] == active;
    if (!mod || !active || !inSlot || !mod->m_pPowerTweaks || !active->m_pPowerTweaks) return;
    if (!mod->m_pPowerTweaks->IsA(UDisTweaks_Blink::StaticClass()) || !active->m_pPowerTweaks->IsA(UDisTweaks_Blink::StaticClass())) return;
    auto* src = static_cast<UDisTweaks_Blink*>(mod->m_pPowerTweaks);
    auto* dst = static_cast<UDisTweaks_Blink*>(active->m_pPowerTweaks);
    if (src == dst) return;
    bool changed = dst->m_ManaCost != src->m_ManaCost;
    for (int l = 0; l < 2; ++l) {
        const FPowerAttributes_Blink& a = src->m_Levels[l];
        const FPowerAttributes_Blink& b = dst->m_Levels[l];
        changed |= a.m_Distance != b.m_Distance || a.m_HorizDistance != b.m_HorizDistance ||
                   a.m_VertDistance != b.m_VertDistance || a.m_fCooldownTime != b.m_fCooldownTime;
    }
    if (!changed) return;
    static int logged = 0;
    if (logged++ < 20)
        Log("Blink settings from Better Powers (%s) -> %s: range %.0f/%.0f/%.0f -> %.0f/%.0f/%.0f (level 1), "
            "%.0f/%.0f/%.0f (level 2), cooldown %.2f, mana %d -> %d", ObjName(src).c_str(), ObjName(dst).c_str(),
            dst->m_Levels[0].m_Distance, dst->m_Levels[0].m_HorizDistance, dst->m_Levels[0].m_VertDistance,
            src->m_Levels[0].m_Distance, src->m_Levels[0].m_HorizDistance, src->m_Levels[0].m_VertDistance,
            src->m_Levels[1].m_Distance, src->m_Levels[1].m_HorizDistance, src->m_Levels[1].m_VertDistance,
            src->m_Levels[0].m_fCooldownTime, dst->m_ManaCost, src->m_ManaCost);
    dst->m_ManaCost = src->m_ManaCost;
    for (int l = 0; l < 2; ++l) {
        dst->m_Levels[l].m_Distance = src->m_Levels[l].m_Distance;
        dst->m_Levels[l].m_HorizDistance = src->m_Levels[l].m_HorizDistance;
        dst->m_Levels[l].m_VertDistance = src->m_Levels[l].m_VertDistance;
        dst->m_Levels[l].m_fCooldownTime = src->m_Levels[l].m_fCooldownTime;
    }
}

static void ApplyPowerSet(ADishonoredPlayerPawn* pawn) {
    if (!pawn || !pawn->Class || pawn->Class->GetName() != "DishonoredPlayerPawn") return;   // not in Daud's own DLC
    if (pawn != g_powerPawn) { ForgetPowerSwaps(); ForgetCorvoBlink(); g_powerPawn = pawn; }
    KeepCorvoArmsMaterial(pawn);
    auto* arr = reinterpret_cast<RawArray*>(&pawn->m_ActivePowers);
    if (!arr->Data || arr->Num <= 0 || arr->Num > 16) return;
    FixLegacyPowerComponents(pawn, arr);
    bool wantDaud = g_powerChar == 1;
    if (wantDaud || !BetterPowersBlinkLoaded()) RestoreModBlink(pawn);   // before Daud's swap reads the slot
    if (g_daudLoadState == LoadState::Loaded) LoadDaudUi();
    static int lastWait = -1;
    int wait = !wantDaud ? 0 : g_daudLoadState != LoadState::Loaded ? 1 : !DaudPowerTweaks(0) ? 2 : 0;
    if (wait != lastWait) {
        lastWait = wait;
        if (wait) Log("Power set: waiting for %s", wait == 1 ? "Daud's package" : "Daud's power tweaks (not found)");
    }
    ApplyBladeEquipStyle(g_bladeChar != 0);   // Daud's and the Overseer's straight blades: quick draw, no flick
    // Summon Assassin with (practically) unlimited range: its tweaks cap the targeting trace at 2000 units (~20 m).
    // Daud's tweaks stay loaded while his set is used, so setting it here sticks; the original is logged once.
    if (g_daudLoadState == LoadState::Loaded) {
        UObject* t = DaudPowerTweaks(2);
        if (t && t->IsA(UDisDLC06Tweaks_SummonAssassin::StaticClass())) {
            auto* summon = static_cast<UDisDLC06Tweaks_SummonAssassin*>(t);
            constexpr float kSummonRange = 1000000.0f;   // 10 km
            if (summon->m_fTargetingRange != kSummonRange) {
                Log("Summon Assassin: targeting range %.0f -> %.0f (%s)", summon->m_fTargetingRange, kSummonRange, ObjName(t).c_str());
                summon->m_fTargetingRange = kSummonRange;
            }
        }
    }
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
    if (!wantDaud && BetterPowersBlinkLoaded()) InstallCorvoBlink(pawn);   // after Daud's Blink went back out
    if (BetterPowersBlinkLoaded()) SyncBlinkSettings(pawn, wantDaud);
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

// The game's own "equip an item of this type" (the Kismet action DisSeqAct_EquipItemType, handled natively by
// ADishonoredPawn::OnEquipItemType): one action object per pawn, made from the class defaults (so its links exist),
// pointed at the item class each time. Secondary hand (usage 2), instantly or with the equip animation.
static UDisSeqAct_EquipItemType* g_equipAct = nullptr;
static ADishonoredPlayerPawn* g_equipActPawn = nullptr;
static int g_bowAutoSwitch = 0;          // 1: we put the drawn bow away to swap it - draw the new one once it's in
static DWORD g_bowAutoSwitchTick = 0;
static int g_bowAutoSwitchFailedFor = -1;   // the choice a stalled attempt was for (not retried until it changes)

static bool EquipItemType(ADishonoredPlayerPawn* pawn, UClass* cls, bool instantly) {
    if (!pawn || !cls) return false;
    if (g_equipActPawn != pawn) { if (g_equipAct) LetGo(g_equipAct); g_equipAct = nullptr; g_equipActPawn = pawn; }
    if (!g_equipAct) {
        UClass* actCls = UDisSeqAct_EquipItemType::StaticClass();
        if (!actCls) return false;
        g_equipAct = static_cast<UDisSeqAct_EquipItemType*>(StaticConstructObject(actCls, pawn, 0, 0, 0, nullptr, nullptr, nullptr, nullptr));
        if (!g_equipAct) return false;
        KeepAlive(g_equipAct);
    }
    g_equipAct->m_pItemTypeToEquip = cls;
    g_equipAct->m_PreferredUsage = EDisEquipUsage::EDisEquipUsage_Secondary;
    g_equipAct->m_bDoInstantly = instantly ? 1 : 0;
    g_equipAct->m_bAffectsAIIntention = 0;
    pawn->OnEquipItemType(g_equipAct);
    return true;
}

static void ForgetBowItems() {
    g_bowAutoSwitch = 0;
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

// ---- Bow upgrades carry over (user 2026-10-12). Piero sells Corvo's crossbow upgrades as always; whatever is owned works
// on whichever bow is in the slot.
//  - Corvo's three bow upgrades (accuracy, range, reload: host class DisWepCrossbow) can never reach the wristbow item - the
//    game finds the bow by EXACT class and the wristbow is a DisDLC06WepCrossbow. So the wristbow gets their DLC twins
//    (dlc06_*, identical attribute values, no attach mesh) put on its item with the item's own ApplyUpgrade (0xC122A0:
//    removes then applies, so repeating it is safe). That only takes upgrades the bow's tweaks list as supported, and the
//    wristbow ships listing only its reload one - the twins are added to its m_SupportedUpgrades (grown with the engine's
//    allocator). Nothing is added to the owned list: saves stay vanilla, Piero's "owned" stays Corvo's.
//  - Bolt capacity / combat sleep darts are player attributes (no host class): shared already.
//  - Piero's crossbow entries (and the bolts) list Corvo's crossbow tweaks as a prerequisite: while the wristbow is in the
//    slot that prerequisite points at the wristbow's tweaks instead (and back when Corvo's returns).
//  - A purchase made while the wristbow is in hand lands on no item: its twin goes onto the wristbow at once, and Corvo's
//    crossbow gets the owned upgrades re-applied whenever it comes back into the slot.
static const char* kBowUpgradeTwins[3][2] = {
    { "Twk_Upgrade_Crossbow_Accuracy", "dlc06_Upgrade_Wristbow_Acc" },
    { "Twk_Upgrade_Crossbow_Range", "dlc06_Upgrade_Wristbow_Range" },
    { "Twk_Upgrade_Crossbow_ReloadSpeed", "DLC06_Upgrd_Wristbow_ReloadSpeed" } };

static bool PawnOwnsUpgrade(ADishonoredPlayerPawn* pawn, UObject* upg) {
    if (!pawn || !upg) return false;
    auto* a = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(pawn) + 0xB50);   // m_Upgrades
    for (int k = 0; a->Data && k < a->Num && k < 256; ++k) if (static_cast<UObject**>(a->Data)[k] == upg) return true;
    return false;
}

static bool ItemApplyUpgradeOk() {   // the function's first bytes, once: push ebp / mov ebp,esp / mov eax,[ebp+8] / push esi / push edi
    static int ok = -1;
    if (ok < 0) {
        static const uint8_t want[8] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x56, 0x57 };
        ok = memcmp(reinterpret_cast<const void*>(0x00C122A0), want, sizeof(want)) == 0 ? 1 : 0;
        if (!ok) Log("WARNING: bow upgrades - the game's ApplyUpgrade isn't where expected; carry-over off");
    }
    return ok == 1;
}

static bool BowSupports(UObject* bowTweaks, UObject* upg, bool add) {
    if (!bowTweaks || !upg) return false;
    UObject* ps = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(bowTweaks) + 0x104);   // m_pPlayerSpecificTweaks
    if (!ps) return false;
    auto* sup = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(ps) + 0x170);           // m_SupportedUpgrades
    for (int k = 0; sup->Data && k < sup->Num && k < 64; ++k) if (static_cast<UObject**>(sup->Data)[k] == upg) return true;
    if (!add || sup->Num < 0 || sup->Num > 60) return false;
    if (sup->Max < sup->Num + 1 && !GrowArray(sup, 4, sup->Num + 4)) return false;
    static_cast<UObject**>(sup->Data)[sup->Num++] = upg;
    Log("bow upgrades: %s now takes %s", ObjName(ps).c_str(), upg->GetName().c_str());
    return true;
}

// The owned crossbow upgrades onto this bow item (Corvo's: the upgrades themselves; the wristbow: their twins).
static void CarryBowUpgrades(ADishonoredPlayerPawn* pawn, UDishonoredInventoryItem* item, const char* why) {
    if (!g_cfg.crossbowUpgradesCarryOver || !pawn || !item || !ItemApplyUpgradeOk()) return;
    bool daud = IsDaudBow(item);
    if (daud && !g_daudBowTweaks) return;
    std::string done;
    for (auto& tw : kBowUpgradeTwins) {
        UObject* corvoUpg = FindObjectByName("DisTweaks_Upgrade", tw[0]);
        if (!corvoUpg || !PawnOwnsUpgrade(pawn, corvoUpg)) continue;
        UObject* use = corvoUpg;
        if (daud) {
            use = FindObjectByName("DisTweaks_Upgrade", tw[1]);
            if (!use) { Log("bow upgrades: %s not loaded - %s can't carry over to the wristbow", tw[1], tw[0]); continue; }
            if (!BowSupports(g_daudBowTweaks, use, true)) { Log("bow upgrades: couldn't make the wristbow take %s", tw[1]); continue; }
        }
        int r = reinterpret_cast<int(__thiscall*)(void*, void*)>(0x00C122A0)(item, use);
        done += std::string(done.empty() ? "" : ", ") + use->GetName() + (r ? "" : " (refused)");
    }
    if (!done.empty()) Log("bow upgrades (%s): %s got %s | its modifiers now: %s", why, daud ? "the wristbow" : "Corvo's crossbow", done.c_str(), ItemMods(item).c_str());
}

// Piero's prerequisite follows the bow in the slot (once a second; the store object is cached per world).
static void PointStoreAtBow(ADishonoredPlayerPawn* pawn) {
    if (!g_cfg.crossbowUpgradesCarryOver || !pawn) return;
    static DWORD last = 0;
    static void* forWorld = nullptr;
    static UObject* store = nullptr;
    static DWORD lastFind = 0;
    DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    if (GetWorld() != forWorld) { forWorld = GetWorld(); store = nullptr; lastFind = 0; }
    if (!store && (!lastFind || now - lastFind > 10000)) {
        lastFind = now;
        store = FindObjectByName("DisTweaks_Store", "Twk_Store_Craftsman");
    }
    if (!store) return;
    int slot = CrossbowSlot(pawn);
    UDishonoredInventoryItem* bow = slot >= 0 ? SlotItem(pawn, slot) : nullptr;
    if (!bow || !g_corvoBowTweaks || !g_daudBowTweaks) return;
    bool daud = IsDaudBow(bow);
    UObject* want = daud ? static_cast<UObject*>(g_daudBowTweaks) : static_cast<UObject*>(g_corvoBowTweaks);
    UObject* other = daud ? static_cast<UObject*>(g_corvoBowTweaks) : static_cast<UObject*>(g_daudBowTweaks);
    auto* items = reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(store) + 0xC4);   // m_StoreItems
    int changed = 0;
    for (int k = 0; items->Data && k < items->Num && k < 128; ++k) {
        auto* pre = reinterpret_cast<RawArray*>(static_cast<uint8_t*>(items->Data) + k * 0x88 + 0x34);   // m_Prerequisites
        for (int q = 0; pre->Data && q < pre->Num && q < 16; ++q) {
            UObject*& o = static_cast<UObject**>(pre->Data)[q];
            if (o == other) { o = want; ++changed; }
        }
    }
    if (changed) Log("bow upgrades: Piero's store - %d entries now need %s", changed, daud ? "the wristbow" : "Corvo's crossbow");
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
    if (cur == g_handItem[0] || cur == g_handItem[1]) {   // only while it is put away...
        // ...but a new choice made while holding a bow (user): put it away at once, the swap below then happens and the new
        // bow is drawn (g_bowAutoSwitch). The empty hand first; if the bow is still in hand after 0.8 s, the powers hand;
        // after 3 s give up (not retried until the choice changes again).
        bool pendingDaud = g_bowChar == 1, curDaud = IsDaudBow(cur);
        if (pendingDaud == curDaud) { g_bowAutoSwitch = 0; g_bowAutoSwitchFailedFor = -1; }
        if (g_cfg.crossbowSwapInHand && pendingDaud != curDaud && (!pendingDaud || g_daudLoadState == LoadState::Loaded) &&
            g_bowAutoSwitchFailedFor != g_bowChar) {
            DWORD t = GetTickCount();
            static bool triedPowers = false;
            if (g_bowAutoSwitch == 0) {
                g_bowAutoSwitch = 1;
                g_bowAutoSwitchTick = t;
                triedPowers = false;
                bool ok = EquipItemType(pawn, UObject::FindClass("Class DishonoredGame.DishonoredItemEmpty"), true);
                Log("crossbow: %s chosen while holding %s - putting it away to swap (%s)", pendingDaud ? "Daud's wristbow" : "Corvo's crossbow",
                    ObjName(cur).c_str(), ok ? "ok" : "the equip action could not be made");
            } else if (!triedPowers && t - g_bowAutoSwitchTick > 800) {
                triedPowers = true;
                bool ok = EquipItemType(pawn, UObject::FindClass("Class DishonoredGame.DisItemPowers"), true);
                Log("crossbow: still in hand - equipping the powers hand instead (%s)", ok ? "ok" : "failed");
            } else if (t - g_bowAutoSwitchTick > 3000) {
                g_bowAutoSwitch = 0;
                g_bowAutoSwitchFailedFor = g_bowChar;
                Log("crossbow: the bow stayed in hand - swap it by holstering it yourself (not retried until the choice changes)");
            }
        }
        return;
    }
    if (g_bowAutoSwitch && GetTickCount() - g_bowAutoSwitchTick < 250) return;   // (a beat after putting it away before swapping)
    if (!g_corvoBowTweaks) {
        g_corvoBowTweaks = static_cast<UDisTweaks_InventoryItem*>(FindObjectByName("DisTweaks_WepCrossbow", "Twk_Inv_CrossbowCorvo"));
        if (g_corvoBowTweaks) KeepAlive(g_corvoBowTweaks);
    }
    bool isDaud = IsDaudBow(cur);
    bool wantDaud = g_bowChar == 1;
    // A failed swap is not retried until the menu choice changes again.
    static int failedWant = -1, lastChoice = -1;
    if (lastChoice != g_bowChar) { lastChoice = g_bowChar; failedWant = -1; }
    if (g_bowAutoSwitch && (wantDaud == isDaud || failedWant == (wantDaud ? 1 : 0))) {   // (put away for a swap that is no longer wanted)
        g_bowAutoSwitch = 0;
        if (cur && cur->Class) { bool ok = EquipItemType(pawn, cur->Class, false); Log("crossbow: no swap after all - drawing %s again (%s)", ObjName(cur).c_str(), ok ? "ok" : "failed"); }
    }
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
    Log("per-bow upgrades: swap %s -> %s | owned %s | outgoing bow modifiers: %s | kept %s modifiers: %s | pawn modifiers: %s",
        ObjName(cur).c_str(), wantDaud ? "Daud's wristbow" : "Corvo's crossbow", OwnedUpgrades(pawn).c_str(), ItemMods(cur).c_str(),
        ObjName(g_bowKept[wantDaud ? 1 : 0]).c_str(), ItemMods(g_bowKept[wantDaud ? 1 : 0]).c_str(), PawnMods(pawn).c_str());
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
    {
        UDishonoredInventoryItem* now = SlotItem(pawn, slot);
        Log("per-bow upgrades: after the swap the slot holds %s | its modifiers: %s | owned %s", ObjName(now).c_str(), ItemMods(now).c_str(), OwnedUpgrades(pawn).c_str());
        if (now && now != cur) CarryBowUpgrades(pawn, now, "bow swapped in");
        if (g_bowAutoSwitch) {
            g_bowAutoSwitch = 0;
            if (now && now->Class) {   // (the new bow - or the old one back if the swap failed: never leave the hand empty)
                bool ok = EquipItemType(pawn, now->Class, false);
                Log("crossbow: drawing %s (%s)", ObjName(now).c_str(), ok ? "ok" : "failed");
            }
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
    bool needPackage = (g_armsChar == 1 && !g_daudArmsMesh) || (g_bodyChar == 1 && g_cfg.fpArmSkin && !g_daudArmsMesh) || (g_bladeChar == 1 && !g_daudSwordMesh) || (g_bowChar == 1 && !g_daudBowMesh) ||
                       (g_powerChar == 1 && g_daudLoadState != LoadState::Loaded);
    if (needPackage) {
        // Load only once our body has been in use a while: a second package load before that tore down the
        // first package's mesh data (tests 12-15).
        if (g_daudLoadState == LoadState::Idle && g_ovsLoadState != LoadState::Loading && RawLoadPackageAsync && g_body &&
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
    bool needOvs = (g_bodyChar == 2 && !g_ovsBodyMesh) || (g_armsChar == 2 && !g_ovsArmsMesh) ||
                   (g_bodyChar == 2 && g_cfg.fpArmSkin && !g_ovsArmsMesh) || (g_bladeChar == 2 && !g_ovsSwordMesh);
    if (needOvs) {
        if (g_ovsLoadState == LoadState::Idle && g_daudLoadState != LoadState::Loading && RawLoadPackageAsync && g_body &&
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
    // ...and with quiet hands (18:4x: the swap replaced the 1P arms mesh and rebuilt the anim state maps 0.1 s into the
    // crossbow's draw - the draw never finished, so the crossbow could neither fire nor be put away). Whenever anything
    // structural is due (a new pawn, arms or sword component, a character choice, a package that just finished
    // loading), it waits until the held items have not changed for 1 s and both are on an idle animation (or the same
    // non-idle one for 3 s - a held pose). Routine upkeep after that runs every frame as before.
    {
        struct SwapSig { void* pawn; void* arms; void* sword; int armsChar, bladeChar, bowChar, powerChar, bodyChar, daud, ovs; };
        static SwapSig applied = {};
        SwapSig now = { pawn, pawn->Mesh, FindSwordMesh(pawn), g_armsChar, g_bladeChar, g_bowChar, g_powerChar, g_bodyChar,
                        static_cast<int>(g_daudLoadState), static_cast<int>(g_ovsLoadState) };
        if (memcmp(&now, &applied, sizeof(SwapSig)) != 0) {
            DWORD t = GetTickCount();
            bool stable = t - g_handsChangedTick >= 1000;
            bool quiet = stable && (g_handsAllIdle || t - g_handSeqChangedTick >= 3000);
            static DWORD waitLog = 0;
            if (!quiet) {
                if (t - waitLog > 1000) { waitLog = t; Log("Body Swap: waiting for the hands to be idle (%s)", !stable ? "an item just changed" : "an item is mid-animation"); }
                return;
            }
            applied = now;
            Log("Body Swap: hands idle - applying");
        }
    }

    DumpPowersOnce(pawn);
    USkeletalMesh* armsAlt = g_armsChar == 1 ? g_daudArmsMesh : g_armsChar == 2 ? g_ovsArmsMesh : nullptr;
    USkeletalMesh* arms = ApplyMeshSwap(pawn->Mesh, armsAlt, { g_daudArmsMesh, g_ovsArmsMesh }, g_armsSwap, "1P arms");
    if (g_ovsArmsMat) {   // the Overseer arms mesh has no material: give it its own; never leave it on another mesh
        auto* am = reinterpret_cast<RawArray*>(&pawn->Mesh->Materials);
        UMaterialInterface* cur0 = (am->Data && am->Num > 0) ? static_cast<UMaterialInterface**>(am->Data)[0] : nullptr;
        if (arms == g_ovsArmsMesh && cur0 != g_ovsArmsMat) { pawn->Mesh->SetMaterial(0, g_ovsArmsMat); Log("1P arms: Overseer material on"); }
        else if (arms != g_ovsArmsMesh && cur0 == g_ovsArmsMat) { pawn->Mesh->SetMaterial(0, nullptr); Log("1P arms: Overseer material off"); }
    }
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
    static LONGLONG qpc9 = 0;
    float dt = QpcDt(qpc9);   // (frame-accurate; was GetTickCount steps)
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
    // After a slide the body rises on the legs' get-up clock (g_slideLegW = 1 - smoothstep(get-up)), not at 90 uu/s:
    // one motion with the pose fade and the forward offset (user 01:20).
    {
        static bool getUpSink = false;
        static float slideSink = 54.5f;
        if (g_isSliding) { getUpSink = true; slideSink = g_smoothedSink; }
        else if (getUpSink) {
            if (g_slideLegW <= 0.0f) getUpSink = false;
            else g_smoothedSink = g_crouchSink + (slideSink - g_crouchSink) * g_slideLegW;
        }
    }
    t.Z += g_crouchSink - g_smoothedSink;
    t.Z += g_slideAmount * g_cfg.slideRaise;   // slide: hips just above the ground (was raised into the camera, video 17-19-35)
    // Forward under the camera with the slide (user 00:05: "legs further away from the camera, see more body" - SlideBack
    // -10 -> -30), and back out of it with the legs' get-up (g_slideLegW: the 0.6 s smoothstep) instead of the 0.4 s linear
    // fade plus SlideRecoverBack's one-frame step: that step swung the raised left knee up through the camera the frame
    // the slide ended (clip 23-59-16, 5.75 s). The knee is down by the time the hips come back past the camera.
    float slideFwd = g_slideAmount > g_slideLegW ? g_slideAmount : g_slideLegW;
    t.X -= slideFwd * g_cfg.slideBack;
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
    // After a slide the game moves the pawn up a step (6-8 uu, log 00:29) a frame after the cylinder grows, while its
    // camera eases: the body popped up under the view. Take the unexplained part of any pawn Z step in the second
    // after a slide and release it at the camera's rate (10/s).
    // (10:26 clip: the step is normally caught by the view hook now, in the frame it happens - this is the fallback.)
    {
        static bool wasSliding = false;
        float cyl = pawn->CylinderComponent ? pawn->CylinderComponent->CollisionHeight : 0.0f;
        if (wasSliding && !g_isSliding) g_absorbSlideEndAt = GetTickCount();
        wasSliding = g_isSliding;
        if (g_absorbLastZ != 0.0f && g_absorbSlideEndAt && GetTickCount() - g_absorbSlideEndAt < 1000 && static_cast<int>(pawn->Physics) == 1) {
            float dz = pawn->Location.Z - g_absorbLastZ - (cyl - g_absorbLastCyl) - pawn->Velocity.Z * dt;
            if (fabsf(dz) > 2.0f && fabsf(dz) < 40.0f) g_zAbsorb -= dz;
        }
        g_absorbLastZ = pawn->Location.Z; g_absorbLastCyl = cyl;
        QpcDt(g_absorbQpc);   // (snapshot time)
        g_zAbsorb += (0.0f - g_zAbsorb) * Clamp(dt * 10.0f, 0.0f, 1.0f);
        if (fabsf(g_zAbsorb) < 0.05f) g_zAbsorb = 0.0f;
        t.Z += g_zAbsorb;
    }
    // Slide turn lag (user 00:52, Dishonored 2 style): the body and legs follow the view round a little later. The
    // body's yaw chases the pawn's at SlideTurnLag per second while the slide pose holds; the difference is applied
    // to both bodies' rotation (the arm skin takes the body's rotation in UpdateArmSkin). Snaps back in step outside.
    {
        auto wrap = [](float a) { while (a > 180.0f) a -= 360.0f; while (a < -180.0f) a += 360.0f; return a; };
        static float lagYaw = 0.0f;
        static bool lagInit = false;
        float pawnYaw = static_cast<int16_t>(pawn->Rotation.Yaw & 0xFFFF) * (360.0f / 65536.0f);
        float slideW = g_slideAmount > g_slideLegW ? g_slideAmount : g_slideLegW;
        if (!lagInit || slideW <= 0.0f || g_cfg.slideTurnLag <= 0.0f) { lagYaw = pawnYaw; lagInit = true; }
        else lagYaw = wrap(lagYaw + wrap(pawnYaw - lagYaw) * Clamp(dt * g_cfg.slideTurnLag, 0.0f, 1.0f));
        g_slideYawOffDeg = Clamp(wrap(lagYaw - pawnYaw) * slideW, -g_cfg.slideTurnLagMax, g_cfg.slideTurnLagMax);
    }
    FRotator bodyRot = arms->Rotation;
    bodyRot.Yaw += static_cast<int>(g_slideYawOffDeg * 65536.0f / 360.0f);
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
        // (11:15: in the air too - the game keeps its sprint arm offset through a sprint jump, so with the pull-back
        // fading out at take-off the body and arm skin sat ~30 uu further forward than in a standing jump)
        int physS = static_cast<int>(pawn->Physics);
        float want = (!g_isCrouched && (physS == 1 || physS == 2))
            ? Clamp((speed - g_cfg.runBlendStart) / (g_cfg.runBlendEnd - g_cfg.runBlendStart + 1.0f), 0.0f, 1.0f) : 0.0f;
        sprintBack += Clamp(want - sprintBack, -dt * 4.0f, dt * 4.0f);
        bodyT.X -= sprintBack * g_cfg.sprintBodyBack;
        g_sprintBackAmount = sprintBack;
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
                Log("slide end +%lums: view Z %.0f pelvis Z %.0f (dxy %.0f) pawn Z %.0f eye %.1f | sink %.1f/%.1f slide %.2f legW %.2f push %.0f applied (%.0f %.0f %.0f) absorb %.1f",
                    nowD - endAt, g_viewLoc.Z, p.Z, sqrtf((p.X - g_viewLoc.X) * (p.X - g_viewLoc.X) + (p.Y - g_viewLoc.Y) * (p.Y - g_viewLoc.Y)),
                    pawn->Location.Z, pawn->EyeHeight, g_crouchSink, g_smoothedSink, g_slideAmount, g_slideLegW, g_clipPush,
                    applied.X, applied.Y, applied.Z, g_zAbsorb);
            }
        }
    }
    if (static_cast<int>(pawn->Physics) != 1 && !(g_isSliding || g_slideAmount > 0.0f)) hideUpper = true;   // not PHYS_Walking: jumping, falling, mantling (not the slide's physics 14 - user 00:52: the shirt was gone while sliding)
    if (!g_cfg.hideUpperWhenClipping) hideUpper = false;   // arms stay visible unless a hand holds something (user)
    // On ladders/chains the coat and shoulders filled the view (1P hands grip), so the upper body was hidden. With the
    // climbing arms (user, 2026-10-11) it stays: chest and neck are always shrunk anyway; the arms join the 1P forearms.
    if (g_isClimbing && !g_cfg.climbArms) hideUpper = true;
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
    bool slideNowU = g_isSliding || g_slideAmount > 0.0f;
    if (g_smoothedSink > g_cfg.deepCrouchSink && !slideNowU) hideUpper = true;   // very low crouch after a slide/under things (log: cylinder 33)
    if (g_swimAmount > 0.0f) hideUpper = true;   // swimming: the collar/hood ringed the underwater view (video 18-09-27)
    // (00:35: no slide clause any more - the upper body shows in slides, user "torso is missing"; the camera is above the chest)
    if (g_isCrouched && !slideNowU && g_viewPitchDeg > g_cfg.crouchHidePitch) hideUpper = true;   // crouched, looking ahead: chest top clipped the view
    g_hideUpperNow = hideUpper;
    if (g_cfg.hideUpperWhenClipping) {   // user: holstered arms stay visible while sprinting / looking up
        FVector v = pawn->Velocity;
        if (sqrtf(v.X * v.X + v.Y * v.Y) > g_cfg.hideUpperSpeed && !slideNowU) hideUpper = true;   // sprinting (a slide starts faster than this)
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
    g_body->SetRotation(bodyRot);
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
        g_shadowBody->SetRotation(bodyRot);
        g_shadowBody->SetScale(arms->Scale);
    }
    if (g_shadowArms) {
        g_shadowArms->SetTranslation(t);
        g_shadowArms->SetRotation(bodyRot);
        g_shadowArms->SetScale(arms->Scale);
        // (13:54 clip: the hand limb IK did nothing on this body while SingleBone controls on the same arm worked - and
        // the old "IK arms had no visible effect" note was from before the rendered arm skin made g_shadowBody count as
        // rendered.) A never-rendered component has its limb IK skipped (per-control bIgnoreWhenNotRendered / the
        // recently-rendered test); stamp it as just rendered every frame.
        if (pawn->WorldInfo) g_shadowArms->LastRenderTime = pawn->WorldInfo->TimeSeconds;
        g_shadowArms->bRecentlyRendered = 1;
    }
    if (g_holsterCast) {
        g_holsterCast->SetTranslation(t);
        g_holsterCast->SetRotation(bodyRot);
        g_holsterCast->SetScale(arms->Scale);
        if (pawn->WorldInfo) g_holsterCast->LastRenderTime = pawn->WorldInfo->TimeSeconds;
        g_holsterCast->bRecentlyRendered = 1;
    }
    if (g_head) {
        // The head shadow is a separate rigid piece: with the spine lean (look up/down, corner lean) the shadow
        // body's head moved and the head shadow stayed behind (user). Each frame, nudge it by the gap between the
        // two head bones (both from the last pose), in actor space.
        static FVector headFix = { 0, 0, 0 };
        static USkeletalMeshComponent* fixFor = nullptr;
        if (fixFor != g_head) { fixFor = g_head; headFix = { 0, 0, 0 }; }
        FName headBone("head_jnt");
        // (13:39 clip: the head bobbed on the body shadow - it chased g_shadowBody while g_shadowArms did the casting)
        USkeletalMeshComponent* headCaster = g_shadowArms ? g_shadowArms : g_shadowBody;
        if (headCaster && headBone.FNameEntryId >= 0) {
            FVector want = headCaster->GetBoneLocation(headBone, 0);
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

// ---------------------------------------------------------------------------------------------
// HD body textures (user 01:05: "upscale the player models to the same quality as the hands"). The body maps
// (Corvo 2048x1024, Daud / Overseer 2048x1024 colour + 1024x512 normal/spec) were AI-upscaled to 4096x2048 (Bria
// increase-resolution, content-preserving) and saved as DXT1 DDS with mips in CorvoBody\hd\<texture name>.dds next
// to this dll. Dishonored's packages / TFCs can't take bigger textures, so they go in at runtime, entirely at the D3D9
// level (the 07:14 run crashed walking the engine's render-resource structures for the texture behind a Texture2D;
// nothing of the engine is read any more): IDirect3DDevice9::SetTexture is hooked (vtable slot 65, taken from a
// throwaway device; MinHook). Every texture the renderer binds is classified once (per 5 s): a 2D DXT1 texture twice
// as wide as high and at least 32 wide has its ~32-wide mip decoded and compared with the same mip of each HD file
// (mean abs error < 25 = the same picture; the game's own small mip of that map); a match is bound as our texture
// from then on, created on the render thread from the file (D3DPOOL_MANAGED, survives resets). Logs "hd textures:".
static const char* kHdTexNames[] = { "Corvo_Body_d", "Corvo_Body_n", "Corvo_Body_s", "CorvoHood_d", "CorvoHood_n", "CorvoHood_s",
                                     "Daud_Body01_d", "Daud_Body01_n", "Daud_Body01_s", "OverseerBody_d", "OverseerBody_n", "OverseerBody_s" };
static const int kHdTexCount = sizeof(kHdTexNames) / sizeof(kHdTexNames[0]);
static const uint32_t kHdSmallW = 32, kHdSmallH = 16;   // the compare mip (all maps are 2:1)
static const uint32_t kHdMidW = 128, kHdMidH = 64;      // the verify mip: a candidate that has it must also CORRELATE with the file's
static const float kHdMinCorr = 0.6f;                    // (14:17 log: the pistol's 1024x512 took the hood's slot at error 14.5 - the 32x16 means of dark
                                                         // leather and the hood coat are alike; true matches at 128x64 correlate ~0.9+, other things < 0.5)
struct HdTexSlot {
    uint8_t* dds; uint32_t ddsSize; bool loadTried;          // the file (freed once the D3D texture exists)
    uint8_t small[kHdSmallW * kHdSmallH * 3]; bool haveSmall; // the file's 32x16 mip, decoded (game thread, at load)
    uint8_t mid[kHdMidW * kHdMidH * 3]; bool haveMid; float corr; // ...and its 128x64 mip; the last candidate's correlation with it
    IDirect3DTexture9* repl; bool replTried;                  // (render thread)
    IDirect3DBaseTexture9* orig; float mae, bestMae;          // the game's texture matched to this slot; best error seen
    UINT origW, origH; DWORD origLevels, lastVerify;          // its shape at the match, and when its picture was last re-checked
    unsigned swaps, dropped;
};
static HdTexSlot g_hdTex[kHdTexCount] = {};
typedef HRESULT(__stdcall* SetTextureFn)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
static SetTextureFn g_origSetTexture = nullptr;
static bool g_hdHooked = false;
static volatile LONG g_hdSetTextureCalls = 0, g_hdClassified = 0, g_hdCompared = 0;
static volatile LONG g_hdGen = 0;   // bumped by the game thread every 5 s: the render thread forgets its classifications
struct HdClass { IDirect3DBaseTexture9* ptr; int slot; };   // slot -1 = not a body map
static const int kHdClassSize = 16384;
static HdClass g_hdClass[kHdClassSize];

// DXT1 block rows -> RGB (w x h x 3, 8-bit); rows of blocks are 'pitch' bytes apart in the source.
static void HdDecodeDxt1(const uint8_t* src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t* out) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by) for (uint32_t bx = 0; bx < bw; ++bx) {
        const uint8_t* b = src + by * pitch + bx * 8;
        uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8), idx = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);
        int pal[4][3];
        auto unpack = [](uint32_t c, int* o) { o[0] = ((c >> 11) & 31) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; };
        unpack(c0, pal[0]); unpack(c1, pal[1]);
        for (int k = 0; k < 3; ++k) {
            if (c0 > c1) { pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3; pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3; }
            else { pal[2][k] = (pal[0][k] + pal[1][k]) / 2; pal[3][k] = 0; }
        }
        for (uint32_t py = 0; py < 4; ++py) for (uint32_t px = 0; px < 4; ++px) {
            uint32_t x = bx * 4 + px, y = by * 4 + py;
            if (x >= w || y >= h) continue;
            const int* c = pal[(idx >> (2 * (py * 4 + px))) & 3];
            uint8_t* o = out + (y * w + x) * 3;
            o[0] = static_cast<uint8_t>(c[0]); o[1] = static_cast<uint8_t>(c[1]); o[2] = static_cast<uint8_t>(c[2]);
        }
    }
}

// The file's mip of exactly kHdSmallW x kHdSmallH, decoded into s.small.
static void HdExtractSmall(HdTexSlot& s) {
    const uint32_t* h = reinterpret_cast<const uint32_t*>(s.dds + 4);
    uint32_t fw = h[3], fh = h[2], fm = h[6] ? h[6] : 1;
    const uint8_t* p = s.dds + 128;
    const uint8_t* end = s.dds + s.ddsSize;
    for (uint32_t m = 0; m < fm; ++m) {
        uint32_t bytes = ((fw + 3) / 4) * ((fh + 3) / 4) * 8;
        if (p + bytes > end) return;
        if (fw == kHdMidW && fh == kHdMidH) { HdDecodeDxt1(p, (fw / 4) * 8, fw, fh, s.mid); s.haveMid = true; }
        if (fw == kHdSmallW && fh == kHdSmallH) { HdDecodeDxt1(p, (fw / 4) * 8, fw, fh, s.small); s.haveSmall = true; return; }
        p += bytes; fw = fw > 1 ? fw / 2 : 1; fh = fh > 1 ? fh / 2 : 1;
    }
}

// DDS (DXT1 / DXT5, with mips) -> a managed D3D9 texture. Render thread only.
static IDirect3DTexture9* HdCreateFromDds(IDirect3DDevice9* dev, const uint8_t* data, uint32_t size) {
    if (!dev || !data || size < 128 || memcmp(data, "DDS ", 4) != 0) return nullptr;
    const uint32_t* h = reinterpret_cast<const uint32_t*>(data + 4);   // DDS_HEADER as dwords: size flags height width pitch depth mips ...
    uint32_t height = h[2], width = h[3], mips = h[6] ? h[6] : 1, fourcc = h[20];
    D3DFORMAT fmt;
    uint32_t blockBytes;
    if (fourcc == MAKEFOURCC('D', 'X', 'T', '1')) { fmt = D3DFMT_DXT1; blockBytes = 8; }
    else if (fourcc == MAKEFOURCC('D', 'X', 'T', '5')) { fmt = D3DFMT_DXT5; blockBytes = 16; }
    else return nullptr;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(dev->CreateTexture(width, height, mips, 0, fmt, D3DPOOL_MANAGED, &t, nullptr)) || !t) return nullptr;
    const uint8_t* p = data + 128;
    const uint8_t* end = data + size;
    uint32_t w = width, hh = height;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t bw = (w + 3) / 4, bh = (hh + 3) / 4, rowBytes = bw * blockBytes, levelBytes = rowBytes * bh;
        if (p + levelBytes > end) { t->Release(); return nullptr; }
        D3DLOCKED_RECT lr;
        if (FAILED(t->LockRect(m, &lr, nullptr, 0))) { t->Release(); return nullptr; }
        for (uint32_t r = 0; r < bh; ++r) memcpy(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch, p + r * rowBytes, rowBytes);
        t->UnlockRect(m);
        p += levelBytes;
        w = w > 1 ? w / 2 : 1;
        hh = hh > 1 ? hh / 2 : 1;
    }
    return t;
}

// Which body map is this (bound) texture, if any: shape first, then its ~32-wide mip against every file's (or one slot's
// when 'only' >= 0). Render thread.
static int HdClassify(IDirect3DBaseTexture9* tex, float& maeOut, int only = -1) {
    maeOut = -1.0f;
    if (tex->GetType() != D3DRTYPE_TEXTURE) return -1;
    auto* t = static_cast<IDirect3DTexture9*>(tex);
    DWORD levels = t->GetLevelCount();
    D3DSURFACE_DESC d = {};
    if (FAILED(t->GetLevelDesc(0, &d)) || d.Format != D3DFMT_DXT1 || d.Width != d.Height * 2 || d.Width < kHdSmallW || d.Width > 2048) return -1;
    DWORD lvl = 0;
    for (; lvl < levels; ++lvl) { if (FAILED(t->GetLevelDesc(lvl, &d))) return -1; if (d.Width <= kHdSmallW) break; }
    if (lvl >= levels || d.Width != kHdSmallW || d.Height != kHdSmallH) return -1;
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(lvl, &lr, nullptr, D3DLOCK_READONLY))) return -2;   // (couldn't read it now: retry, don't classify)
    uint8_t rgb[kHdSmallW * kHdSmallH * 3];
    HdDecodeDxt1(static_cast<const uint8_t*>(lr.pBits), lr.Pitch, kHdSmallW, kHdSmallH, rgb);
    t->UnlockRect(lvl);
    InterlockedIncrement(&g_hdCompared);
    int best = -1;
    float bestMae = 1e9f;
    for (int i = 0; i < kHdTexCount; ++i) {
        HdTexSlot& s = g_hdTex[i];
        if (!s.haveSmall || (only >= 0 && i != only)) continue;
        unsigned sum = 0;
        for (uint32_t k = 0; k < kHdSmallW * kHdSmallH * 3; ++k) sum += rgb[k] > s.small[k] ? rgb[k] - s.small[k] : s.small[k] - rgb[k];
        float mae = sum / static_cast<float>(kHdSmallW * kHdSmallH * 3);
        if (mae < s.bestMae) s.bestMae = mae;
        if (mae < bestMae) { bestMae = mae; best = i; }
    }
    maeOut = bestMae;
    if (!(bestMae < 15.0f)) return -1;   // (07:49 log: true matches 2-13, a look-alike NPC texture 17 - 25 let it through)
    // Verify by structure at 128x64 when the candidate has that level (a streamed-in stub smaller than that is taken on
    // the 32x16 match alone, as before): grey-level correlation with the file's mip, which the DXT1 / mip-filter
    // differences that put true matches at error 7-17 do not disturb.
    HdTexSlot& v = g_hdTex[best];
    if (v.haveMid) {
        DWORD lvlM = 0;
        D3DSURFACE_DESC dm = {};
        for (; lvlM < levels; ++lvlM) { if (FAILED(t->GetLevelDesc(lvlM, &dm))) return -1; if (dm.Width <= kHdMidW) break; }
        if (lvlM < levels && dm.Width == kHdMidW && dm.Height == kHdMidH) {
            D3DLOCKED_RECT lm;
            if (FAILED(t->LockRect(lvlM, &lm, nullptr, D3DLOCK_READONLY))) return -2;
            static uint8_t rgbM[kHdMidW * kHdMidH * 3];   // (render thread only; one classification at a time)
            HdDecodeDxt1(static_cast<const uint8_t*>(lm.pBits), lm.Pitch, kHdMidW, kHdMidH, rgbM);
            t->UnlockRect(lvlM);
            const uint32_t n = kHdMidW * kHdMidH;
            double sa = 0, sb = 0;
            for (uint32_t k = 0; k < n; ++k) { sa += rgbM[k * 3] + rgbM[k * 3 + 1] + rgbM[k * 3 + 2]; sb += v.mid[k * 3] + v.mid[k * 3 + 1] + v.mid[k * 3 + 2]; }
            double ma = sa / n, mb = sb / n, cov = 0, va = 0, vb = 0;
            for (uint32_t k = 0; k < n; ++k) {
                double a = rgbM[k * 3] + rgbM[k * 3 + 1] + rgbM[k * 3 + 2] - ma, b = v.mid[k * 3] + v.mid[k * 3 + 1] + v.mid[k * 3 + 2] - mb;
                cov += a * b; va += a * a; vb += b * b;
            }
            float corr = (va > 0 && vb > 0) ? static_cast<float>(cov / sqrt(va * vb)) : 0.0f;
            v.corr = corr;
            if (corr < kHdMinCorr) {
                static DWORD lastRej[kHdTexCount] = {};
                if (GetTickCount() - lastRej[best] > 2000) {
                    lastRej[best] = GetTickCount();
                    D3DSURFACE_DESC d0 = {};
                    t->GetLevelDesc(0, &d0);
                    Log("hd textures: %s: a %ux%u x%lu candidate matched at %.1f but its 128x64 correlation is %.2f - not ours",
                        kHdTexNames[best], d0.Width, d0.Height, static_cast<unsigned long>(levels), bestMae, corr);
                }
                return -1;
            }
        }
    }
    return best;
}

static void CaptureHookGameDevice(IDirect3DDevice9* dev);
static HRESULT __stdcall HookedSetTexture(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* tex) {
    InterlockedIncrement(&g_hdSetTextureCalls);
    {
        static volatile LONG checked = 0;
        if (g_cfg.selfTestCapture && self && InterlockedExchange(&checked, 1) == 0) CaptureHookGameDevice(self);
    }
    if (tex) {
        static LONG myGen = 0;
        LONG gen = g_hdGen;
        if (gen != myGen) {   // forget everything (a freed texture's address can be reused): classify afresh
            myGen = gen;
            memset(g_hdClass, 0, sizeof(g_hdClass));
            for (int i = 0; i < kHdTexCount; ++i) g_hdTex[i].orig = nullptr;
        }
        uintptr_t key = reinterpret_cast<uintptr_t>(tex) >> 4;
        int slot = -2;   // -2 unknown
        int at = -1;
        for (int probe = 0; probe < 8; ++probe) {
            int k = static_cast<int>((key + probe) & (kHdClassSize - 1));
            if (g_hdClass[k].ptr == tex) { slot = g_hdClass[k].slot; break; }
            if (!g_hdClass[k].ptr) { at = k; break; }
        }
        int tableAt = -1;
        if (slot == -2) {
            float mae;
            slot = HdClassify(tex, mae);
            InterlockedIncrement(&g_hdClassified);
            // (11:20: a transient lock failure must not classify the texture as 'not ours' for 5 s - the body flickered
            // to its original map at a slide end; -2 leaves the table empty so the next bind retries)
            if (slot == -2) { slot = -1; at = -1; }
            else if (at >= 0) { g_hdClass[at].ptr = tex; g_hdClass[at].slot = slot; tableAt = at; }
            if (slot >= 0) {
                HdTexSlot& s = g_hdTex[slot];
                D3DSURFACE_DESC d = {};
                auto* t = static_cast<IDirect3DTexture9*>(tex);
                t->GetLevelDesc(0, &d);
                s.orig = tex; s.mae = mae; s.origW = d.Width; s.origH = d.Height; s.origLevels = t->GetLevelCount(); s.lastVerify = GetTickCount();
            }
        } else if (slot >= 0) {
            // Still the same texture? A freed texture's address is reused at once on a loading screen (07:35: the loading
            // text went lavender - the body normal map on the font). Shape every time, the picture every 250 ms.
            HdTexSlot& s = g_hdTex[slot];
            auto* t = static_cast<IDirect3DTexture9*>(tex);
            D3DSURFACE_DESC d = {};
            bool same = tex->GetType() == D3DRTYPE_TEXTURE && t->GetLevelCount() == s.origLevels && SUCCEEDED(t->GetLevelDesc(0, &d)) &&
                        d.Width == s.origW && d.Height == s.origH && d.Format == D3DFMT_DXT1;
            DWORD now = GetTickCount();
            if (same && now - s.lastVerify > 250) {
                float mae;
                int r = HdClassify(tex, mae, slot);
                if (r != -2) { same = r == slot; s.lastVerify = now; if (same) s.mae = mae; }   // (-2: couldn't read it now - keep, retry)
            }
            if (!same) {
                for (int probe = 0; probe < 8; ++probe) {   // forget it in the table
                    int k = static_cast<int>((key + probe) & (kHdClassSize - 1));
                    if (g_hdClass[k].ptr == tex) { g_hdClass[k].slot = -1; break; }
                }
                if (s.orig == tex) s.orig = nullptr;
                ++s.dropped;
                slot = -1;
            }
        }
        (void)tableAt;
        if (slot >= 0) {
            HdTexSlot& s = g_hdTex[slot];
            if (!s.repl && !s.replTried && s.dds) {
                s.replTried = true;
                s.repl = HdCreateFromDds(self, s.dds, s.ddsSize);
                if (s.repl) { uint8_t* old = s.dds; s.dds = nullptr; free(old); }   // (the small mip is kept in s.small)
            }
            if (s.repl) { tex = s.repl; ++s.swaps; }
        }
    }
    return g_origSetTexture(self, stage, tex);
}

// ---- Frame capture (DEVELOPMENT, SelfTestCapture): the presented frame saved as a half-size 24-bit BMP in
// CorvoBody\captures next to this dll, so the author can look at the result himself. Requested from the game thread
// (g_captureWant), served on the render thread in Present (device or swap chain - whichever the game uses).
typedef HRESULT(__stdcall* DevPresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(__stdcall* ScPresentFn)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
static DevPresentFn g_origDevPresent = nullptr;
static ScPresentFn g_origScPresent = nullptr;
static void* g_hookedDevPresent = nullptr;     // the code address the throwaway device's Present hook patched
static void* g_hookedScPresent = nullptr;
static DevPresentFn g_origDevPresent2 = nullptr;   // the game's own device's Present, if it is different code
static ScPresentFn g_origScPresent2 = nullptr;
static volatile LONG g_presentSeen = 0;

static void CaptureFrame(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) { Log("capture: no back buffer"); return; }
    D3DSURFACE_DESC d = {};
    bb->GetDesc(&d);
    IDirect3DSurface9* src = bb;
    IDirect3DSurface9* rt = nullptr;
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {   // resolve first
        if (SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) && rt &&
            SUCCEEDED(dev->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE))) src = rt;
    }
    IDirect3DSurface9* sys = nullptr;
    bool ok = (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
              SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys &&
              SUCCEEDED(dev->GetRenderTargetData(src, sys));
    D3DLOCKED_RECT lr = {};
    if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        int w = static_cast<int>(d.Width) / 2, h = static_cast<int>(d.Height) / 2;
        int rowBytes = (w * 3 + 3) & ~3;
        char path[MAX_PATH];
        HMODULE self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&CaptureFrame), &self)) {
            GetModuleFileNameA(self, path, MAX_PATH);
            char* slash = strrchr(path, '\\');
            if (slash) {
                *(slash + 1) = 0;
                strncat(path, "CorvoBody\\captures", MAX_PATH - strlen(path) - 1);
                CreateDirectoryA(path, nullptr);
                SYSTEMTIME st; GetLocalTime(&st);
                char name[96];
                LONG n = InterlockedIncrement(&g_captureCount);
                snprintf(name, sizeof(name), "\\%02d%02d%02d_%03ld_%s.bmp", st.wHour, st.wMinute, st.wSecond, static_cast<long>(n), g_captureTag);
                strncat(path, name, MAX_PATH - strlen(path) - 1);
                HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    BITMAPFILEHEADER fh = {};
                    BITMAPINFOHEADER ih = {};
                    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24; ih.biCompression = BI_RGB;
                    ih.biSizeImage = rowBytes * h;
                    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + ih.biSizeImage;
                    DWORD wr = 0;
                    WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
                    WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
                    std::string row(rowBytes, '\0');
                    for (int y = h - 1; y >= 0; --y) {   // bottom-up
                        const uint8_t* srow = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y * 2) * lr.Pitch;
                        for (int x = 0; x < w; ++x) {
                            const uint8_t* px = srow + x * 8;   // every other pixel (BGRA)
                            row[x * 3 + 0] = static_cast<char>(px[0]); row[x * 3 + 1] = static_cast<char>(px[1]); row[x * 3 + 2] = static_cast<char>(px[2]);
                        }
                        WriteFile(f, row.data(), rowBytes, &wr, nullptr);
                    }
                    CloseHandle(f);
                    static DWORD lastCapLog = 0;
                    if (GetTickCount() - lastCapLog > 2000) { lastCapLog = GetTickCount(); Log("capture: saved %s (%dx%d)", path, w, h); }
                }
            }
        }
        sys->UnlockRect();
    } else Log("capture: could not read the frame (format %d, msaa %d)", static_cast<int>(d.Format), static_cast<int>(d.MultiSampleType));
    if (sys) sys->Release();
    if (rt) rt->Release();
    bb->Release();
}

static HRESULT __stdcall HookedDevPresent(IDirect3DDevice9* self, const RECT* a, const RECT* b, HWND c, const RGNDATA* e) {
    if (InterlockedExchange(&g_presentSeen, 1) == 0) Log("capture: the game presents through the device's Present");
    if (InterlockedCompareExchange(&g_captureWant, 0, 1) == 1) CaptureFrame(self);
    return g_origDevPresent(self, a, b, c, e);
}

static HRESULT __stdcall HookedDevPresent2(IDirect3DDevice9* self, const RECT* a, const RECT* b, HWND c, const RGNDATA* e) {
    if (InterlockedExchange(&g_presentSeen, 1) == 0) Log("capture: the game presents through its own device's Present");
    if (InterlockedCompareExchange(&g_captureWant, 0, 1) == 1) CaptureFrame(self);
    return g_origDevPresent2(self, a, b, c, e);
}

static HRESULT __stdcall HookedScPresent(IDirect3DSwapChain9* self, const RECT* a, const RECT* b, HWND c, const RGNDATA* e, DWORD fl) {
    if (InterlockedCompareExchange(&g_captureWant, 0, 1) == 1) {
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(self->GetDevice(&dev)) && dev) { CaptureFrame(dev); dev->Release(); }
    }
    return g_origScPresent(self, a, b, c, e, fl);
}

static HRESULT __stdcall HookedScPresent2(IDirect3DSwapChain9* self, const RECT* a, const RECT* b, HWND c, const RGNDATA* e, DWORD fl) {
    if (InterlockedExchange(&g_presentSeen, 1) == 0) Log("capture: the game presents through its own swap chain");
    if (InterlockedCompareExchange(&g_captureWant, 0, 1) == 1) {
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(self->GetDevice(&dev)) && dev) { CaptureFrame(dev); dev->Release(); }
    }
    return g_origScPresent2(self, a, b, c, e, fl);
}

// Once, from inside SetTexture (render thread, the GAME's device as 'self'): if its Present / swap-chain Present are
// different code from the throwaway device's (another device type), hook those too.
static void CaptureHookGameDevice(IDirect3DDevice9* dev) {
    void** vt = *reinterpret_cast<void***>(dev);
    Log("capture: game device Present %p (throwaway's %p)", vt[17], g_hookedDevPresent);
    if (vt[17] != g_hookedDevPresent &&
        MH_CreateHook(vt[17], reinterpret_cast<void*>(&HookedDevPresent2), reinterpret_cast<void**>(&g_origDevPresent2)) == MH_OK &&
        MH_EnableHook(vt[17]) == MH_OK) Log("capture: hooked the game device's own Present");
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        void** svt = *reinterpret_cast<void***>(sc);
        Log("capture: game swap chain Present %p (throwaway's %p)", svt[3], g_hookedScPresent);
        if (svt[3] != g_hookedScPresent &&
            MH_CreateHook(svt[3], reinterpret_cast<void*>(&HookedScPresent2), reinterpret_cast<void**>(&g_origScPresent2)) == MH_OK &&
            MH_EnableHook(svt[3]) == MH_OK) Log("capture: hooked the game swap chain's own Present");
        sc->Release();
    }
}

// SetTexture's address from a throwaway device (the vtable is d3d9.dll's, shared with the game's device).
static void HdInstallHook() {
    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    typedef IDirect3D9* (WINAPI* CreateFn)(UINT);
    auto create = d3d9 ? reinterpret_cast<CreateFn>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
    if (!create) { Log("hd textures: no Direct3DCreate9"); return; }
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) { Log("hd textures: Direct3DCreate9 failed"); return; }
    HWND wnd = CreateWindowExA(0, "STATIC", "CorvoBodyHd", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = pp.BackBufferHeight = 1;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
    if (FAILED(hr) || !dev) {
        Log("hd textures: dummy device failed (%08lx)", static_cast<unsigned long>(hr));
    } else {
        void** vt = *reinterpret_cast<void***>(dev);
        if (MH_CreateHook(vt[65], reinterpret_cast<void*>(&HookedSetTexture), reinterpret_cast<void**>(&g_origSetTexture)) == MH_OK &&
            MH_EnableHook(vt[65]) == MH_OK) {
            g_hdHooked = true;
            Log("hd textures: SetTexture hooked (fn %p)", vt[65]);
        } else Log("hd textures: SetTexture hook FAILED");
        if (g_cfg.selfTestCapture) {   // frame capture: device Present (17) and swap chain Present (3)
            if (MH_CreateHook(vt[17], reinterpret_cast<void*>(&HookedDevPresent), reinterpret_cast<void**>(&g_origDevPresent)) == MH_OK &&
                MH_EnableHook(vt[17]) == MH_OK) { g_hookedDevPresent = vt[17]; Log("capture: device Present hooked"); }
            IDirect3DSwapChain9* sc = nullptr;
            if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
                void** svt = *reinterpret_cast<void***>(sc);
                if (MH_CreateHook(svt[3], reinterpret_cast<void*>(&HookedScPresent), reinterpret_cast<void**>(&g_origScPresent)) == MH_OK &&
                    MH_EnableHook(svt[3]) == MH_OK) { g_hookedScPresent = svt[3]; Log("capture: swap chain Present hooked"); }
                sc->Release();
            }
        }
        dev->Release();
    }
    d3d->Release();
    if (wnd) DestroyWindow(wnd);
}

// CorvoBody\hd\<name>.dds next to this dll, read whole.
static void HdLoadDds(HdTexSlot& s, const char* name) {
    s.loadTried = true;
    s.bestMae = 1e9f;
    char path[MAX_PATH];
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&HdLoadDds), &self)) return;
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    *(slash + 1) = 0;
    strncat(path, "CorvoBody\\hd\\", MAX_PATH - strlen(path) - 1);
    strncat(path, name, MAX_PATH - strlen(path) - 1);
    strncat(path, ".dds", MAX_PATH - strlen(path) - 1);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("hd textures: no %s", path); return; }
    DWORD size = GetFileSize(f, nullptr), got = 0;
    uint8_t* buf = (size != INVALID_FILE_SIZE && size > 128) ? static_cast<uint8_t*>(malloc(size)) : nullptr;
    if (buf && ReadFile(f, buf, size, &got, nullptr) && got == size) {
        s.ddsSize = size;
        s.dds = buf;
        HdExtractSmall(s);
        Log("hd textures: loaded %s (%lu bytes, compare mip %s)", path, size, s.haveSmall ? "ok" : "MISSING");
        if (!s.haveSmall) { s.dds = nullptr; free(buf); }
    } else { free(buf); Log("hd textures: could not read %s", path); }
    CloseHandle(f);
}

static void HdTexProbe() {
    static DWORD lastSlow = 0;
    static bool loaded = false, hookTried = false;
    DWORD now = GetTickCount();
    if (!loaded) {
        loaded = true;
        for (int i = 0; i < kHdTexCount; ++i) HdLoadDds(g_hdTex[i], kHdTexNames[i]);
    }
    bool anyFile = false;
    for (int i = 0; i < kHdTexCount; ++i) if (g_hdTex[i].haveSmall) anyFile = true;
    if (!hookTried && (anyFile || g_cfg.selfTestCapture)) { hookTried = true; HdInstallHook(); }
    if (!g_hdHooked || now - lastSlow < 5000) return;
    lastSlow = now;
    LONG calls = InterlockedExchange(&g_hdSetTextureCalls, 0);
    LONG classified = InterlockedExchange(&g_hdClassified, 0), compared = InterlockedExchange(&g_hdCompared, 0);
    for (int i = 0; i < kHdTexCount; ++i) {
        HdTexSlot& s = g_hdTex[i];
        if (!s.haveSmall) continue;
        unsigned swaps = s.swaps, dropped = s.dropped;
        s.swaps = 0; s.dropped = 0;
        if (s.orig || swaps || dropped || s.bestMae < 60.0f)
            Log("hd textures: %s: game texture %p %ux%u x%lu (match error %.1f, best seen %.1f, corr %.2f) | swapped %u, dropped %u in 5 s | hd %s",
                kHdTexNames[i], static_cast<void*>(s.orig), s.origW, s.origH, static_cast<unsigned long>(s.origLevels), s.mae, s.bestMae, s.corr,
                swaps, dropped, s.repl ? "ready" : s.replTried ? "FAILED" : "loaded");
    }
    Log("hd textures: %ld SetTexture calls, %ld new textures classified, %ld compared in 5 s", calls, classified, compared);
    InterlockedIncrement(&g_hdGen);   // (after the log: the render thread forgets its table and re-matches)
}

// Holstered slide (09:15 clip, looking straight down): a second, darker pair of hands under the view - the game's own
// first-person arms (their slide / aim-offset pose puts them low in front). With both hands free the arm skin is the
// arms, so the 1P mesh is hidden for the slide (HideBone never hides in this game; the component can). Its skeleton
// keeps updating while hidden - the camera rides its camera_jnt.
static void UpdateSlideOnePArms(ADishonoredPlayerPawn* pawn) {
    static bool hidden = false;
    if (!pawn || !pawn->Mesh) { hidden = false; return; }
    bool want = g_cfg.slideHideFirstPersonArms && g_armSkinOn && g_slideAmount > 0.3f && !g_leftHandBusy && !g_rightHandBusy;
    want = want || (g_climbHandsOn && g_armSkinOn && g_climbAmount > 0.3f && !g_leftHandBusy && !g_rightHandBusy);   // (climbing: ours climb, the game's sit still under them)
    if (want) {
        pawn->Mesh->bUpdateSkelWhenNotRendered = 1;
        if (!pawn->Mesh->HiddenGame) pawn->Mesh->SetHidden(true);   // (every frame: the game may show it again)
        if (!hidden) Log("1P arms hidden for the slide");
    } else if (hidden) {
        pawn->Mesh->SetHidden(false);
        Log("1P arms shown again after the slide");
    }
    hidden = want;
}

// Slide diagnostic (slide work, 2026-10-12): every 100 ms through a slide and its get-up, where the body, legs and the
// arm skin are in VIEW space (forward / right / up from the camera, uu) - what the player can see, for tuning the
// body offsets, the camera raise and the arm pose against the clips.
static void DiagSlide(ADishonoredPlayerPawn* pawn) {
    static DWORD startAt = 0, lastLine = 0, lastActive = 0;
    static bool was = false;
    DWORD now = GetTickCount();
    if (g_isSliding && !was) startAt = now;
    was = g_isSliding;
    bool active = g_isSliding || g_slideAmount > 0.0f || g_slideLegW > 0.0f;
    if (active) lastActive = now;
    if (!g_body || !pawn || !startAt || (!active && now - lastActive > 300) || now - lastLine < 100) return;
    lastLine = now;
    Xf vw = XfFromRotator(g_viewRot, g_viewLoc);
    auto vs = [&](const FVector& p, float* o) {
        FVector d{ p.X - g_viewLoc.X, p.Y - g_viewLoc.Y, p.Z - g_viewLoc.Z };
        for (int i = 0; i < 3; ++i) o[i] = d.X * vw.m[i][0] + d.Y * vw.m[i][1] + d.Z * vw.m[i][2];
    };
    static const char* bodyBones[7] = { "Root_jnt", "lower_leg_L_jnt", "lower_leg_R_jnt", "foot_L_jnt", "foot_R_jnt", "spine_2_jnt", "neck_jnt" };
    // whose hands (09:15): the body's hand-to-elbow distance (0 = collapsed under the skin) and the game's 1P hands in view
    float bh[2] = { 0, 0 }, p1[2][3] = {};
    for (int sd = 0; sd < 2; ++sd) {
        FVector e = g_body->GetBoneLocation(FName(kLowerArmBones[sd]), 0), hnd = g_body->GetBoneLocation(FName(kHandBones[sd]), 0);
        bh[sd] = sqrtf((e.X - hnd.X) * (e.X - hnd.X) + (e.Y - hnd.Y) * (e.Y - hnd.Y) + (e.Z - hnd.Z) * (e.Z - hnd.Z));
        if (pawn->Mesh) vs(pawn->Mesh->GetBoneLocation(FName(kHandBones[sd]), 0), p1[sd]);
    }
    static const char* skinBones[6] = { "upper_arm_L_jnt", "upper_arm_R_jnt", "lower_arm_L_jnt", "lower_arm_R_jnt", "hand_L_jnt", "hand_R_jnt" };
    float b[7][3] = {}, k[6][3] = {};
    for (int i = 0; i < 7; ++i) vs(g_body->GetBoneLocation(FName(bodyBones[i]), 0), b[i]);
    if (g_armSkin) for (int i = 0; i < 6; ++i) vs(g_armSkin->GetBoneLocation(FName(skinBones[i]), 0), k[i]);
    FVector v = pawn->Velocity;
    Log("slide diag +%lums%s: amount %.2f legW %.2f sink %.1f/%.1f pitch %.0f speed %.0f | fwd/right/up: pelvis %.0f %.0f %.0f "
        "kneeL %.0f %.0f %.0f kneeR %.0f %.0f %.0f footL %.0f %.0f %.0f footR %.0f %.0f %.0f | skin shoulderL %.0f %.0f %.0f "
        "shoulderR %.0f %.0f %.0f elbowL %.0f %.0f %.0f elbowR %.0f %.0f %.0f handL %.0f %.0f %.0f handR %.0f %.0f %.0f | "
        "skin shown L%d R%d | 1P hand on screen L%d R%d | spine2 %.0f %.0f %.0f neck %.0f %.0f %.0f torso hidden %.2f upper %.2f | yaw lag %.0f"
        " | body hand-elbow L %.1f R %.1f | 1P hands raw L %.0f %.0f %.0f R %.0f %.0f %.0f | 1P mesh hidden %d",
        now - startAt, g_isSliding ? "" : " (over)", g_slideAmount, g_slideLegW, g_crouchSink, g_smoothedSink, g_viewPitchDeg,
        sqrtf(v.X * v.X + v.Y * v.Y), b[0][0], b[0][1], b[0][2], b[1][0], b[1][1], b[1][2], b[2][0], b[2][1], b[2][2],
        b[3][0], b[3][1], b[3][2], b[4][0], b[4][1], b[4][2], k[0][0], k[0][1], k[0][2], k[1][0], k[1][1], k[1][2],
        k[2][0], k[2][1], k[2][2], k[3][0], k[3][1], k[3][2], k[4][0], k[4][1], k[4][2], k[5][0], k[5][1], k[5][2],
        g_skinArmShown[0] ? 1 : 0, g_skinArmShown[1] ? 1 : 0, g_onePHandOnScreen[0] ? 1 : 0, g_onePHandOnScreen[1] ? 1 : 0,
        b[5][0], b[5][1], b[5][2], b[6][0], b[6][1], b[6][2], g_torsoAmount, g_upperAmount, g_slideYawOffDeg,
        bh[0], bh[1], p1[0][0], p1[0][1], p1[0][2], p1[1][0], p1[1][1], p1[1][2], pawn->Mesh ? (pawn->Mesh->HiddenGame ? 1 : 0) : -1);
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
            if (g_shadowArms && g_shadowArms->Animations) DetachNodeArrays(g_shadowArms->Animations, 0);
            if (g_holsterCast && g_holsterCast->Animations) DetachNodeArrays(g_holsterCast->Animations, 0);
            DetachOurArrays(g_body);
        }
        lastPawn = pawn;
        g_body = nullptr;
        g_head = nullptr;
        g_shoulderBlob[0] = g_shoulderBlob[1] = nullptr;
        g_castLightEnv = nullptr;
        g_shadowBody = nullptr;
        g_shadowArms = nullptr;
        g_holsterCast = nullptr;
        g_itemShadow[0] = g_itemShadow[1] = nullptr;   // owned by the pawn; dropped with it
        g_armsShadow = nullptr;
        g_armSkin = nullptr;
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
    WatchOwnedUpgrades(pawn);   // (per-bow upgrades: diagnostics + a purchase carried to the bow in hand)
    PointStoreAtBow(pawn);      // (Piero's crossbow entries follow the bow in the slot)
    ApplyArmsShift(pawn);
    UpdateHitState(pawn);
    // Posture first: on the frame a slide ends the game re-centres the taller cylinder (pawn up ~32) and
    // SyncTransform used last frame's sink, so the pelvis jumped into the camera for one frame (log 21:06).
    PhysProbe("frame start");
    UpdatePosture(pawn);
    PhysProbe("UpdatePosture");
    SyncTransform(pawn);
    PhysProbe("SyncTransform");
    TraceBones(pawn);
    PhysProbe("TraceBones");
    UpdateBodyAnimation();
    PhysProbe("UpdateBodyAnimation");
    UpdateLocomotionBlend(pawn);
    PhysProbe("UpdateLocomotionBlend");
    UpdateTurnInPlace(pawn);
    PhysProbe("UpdateTurnInPlace");
    UpdateFootIK(pawn);
    PhysProbe("UpdateFootIK");
    UpdateSlide();
    PhysProbe("UpdateSlide");
    UpdateClimb(pawn);
    PhysProbe("UpdateClimb");
    UpdateJump(pawn);
    PhysProbe("UpdateJump");
    UpdateSwim(pawn);
    PhysProbe("UpdateSwim");
    UpdateHands(GetPlayerPawn());
    PhysProbe("UpdateHands");
    UpdateOnePHandOnScreen(pawn);
    PhysProbe("UpdateOnePHandOnScreen");
    UpdateSlideOnePArms(pawn);
    PhysProbe("UpdateSlideOnePArms");
    UpdateJumpSway(pawn);
    PhysProbe("UpdateJumpSway");
    UpdateCoat();
    PhysProbe("UpdateCoat");
    CapturePowerHand(pawn);
    PhysProbe("CapturePowerHand");
    UpdateJumpHands();
    PhysProbe("UpdateJumpHands");
    UpdateClimbArms(pawn);
    PhysProbe("UpdateClimbArms");
    UpdateArmSkin(pawn);
    PhysProbe("UpdateArmSkin");
    HideFirstPersonJumpHands(pawn);
    PhysProbe("HideFirstPersonJumpHands");
    UpdateLean(pawn);
    PhysProbe("UpdateLean");
    UpdateArmBlend();
    PhysProbe("UpdateArmBlend");
    UpdateArmIK(pawn);
    PhysProbe("UpdateArmIK");
    UpdateArmsShadow(pawn);
    PhysProbe("UpdateArmsShadow");
    UpdateArmSkinReach(pawn);
    PhysProbe("UpdateArmSkinReach");
    UpdateCastingArmReach(pawn);
    UpdateHolsterShoulders();
    UpdateClimbHands(pawn);   // (after UpdateArmSkinReach / UpdateCastingArmReach: their wrist resets must not undo it)
    PhysProbe("UpdateCastingArmReach");
    UpdateItemShadows(pawn);
    PhysProbe("UpdateItemShadows");
    UpdateVisibility(pawn);
    PhysProbe("UpdateVisibility");
    DiagHeadShadow();
    PhysProbe("DiagHeadShadow");
    DiagSlide(pawn);
    PhysProbe("DiagSlide");
    HdTexProbe();
    PhysProbe("HdTexProbe");
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

// Ultimate Back Turn (2026-10-09): R3 (right stick click) or Left Alt - the game's Zoom on pad and keyboard
// (DishonoredInput.ini: XboxTypeS_RightThumbstick / LeftAlt -> GBA_Zoom). A tap turns the view 180 degrees, eased in
// and out over QuickTurnTime; held for ZoomHoldTime the game's Zoom comes on, and goes off again on release. The
// game's own Zoom from those two buttons is swallowed (ProcessEvent hook: Dis_Zoom while R3 / Left Alt is down) and
// called by us instead. Left Alt with Tab or F4 (Alt+Tab, Alt+F4) does neither.
// The controller is read with XInput (the game's own xinput1_3.dll); the turn is added to the player controller's
// yaw a little each frame, so the pawn follows as with stick input. Not while the game is paused (world time stops)
// or the game window isn't in front.
struct XPadState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
using XGetStateFn = DWORD(WINAPI*)(DWORD, XPadState*);
static XGetStateFn g_xGetState = nullptr;
static UFunction* g_disZoomFn = nullptr;
static int g_ourZoomCall = 0;

static void FindXInput() {
    static bool looked = false;
    if (looked) return;
    looked = true;
    for (const char* dll : { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" }) {
        HMODULE m = GetModuleHandleA(dll);
        if (!m) m = LoadLibraryA(dll);
        if (m && (g_xGetState = reinterpret_cast<XGetStateFn>(GetProcAddress(m, "XInputGetState")))) {
            Log("Ultimate Back Turn: R3 via %s, Left Alt", dll);
            return;
        }
    }
    Log("Ultimate Back Turn: no XInput - Left Alt only");
}

static bool PadR3Down() {
    if (!g_xGetState) return false;
    for (DWORD i = 0; i < 4; ++i) {
        XPadState st{};
        if (g_xGetState(i, &st) == 0 && (st.buttons & 0x0080)) return true;   // XINPUT_GAMEPAD_RIGHT_THUMB
    }
    return false;
}

static bool BackTurnButtonDown() {
    return g_cfg.quickTurn && ((GetAsyncKeyState(VK_LMENU) & 0x8000) || PadR3Down());
}

static void CallZoom(APlayerController* pc) {
    if (!pc || !g_disZoomFn || !pc->IsA(ADishonoredPlayerController::StaticClass())) return;
    ++g_ourZoomCall;
    static_cast<ADishonoredPlayerController*>(pc)->Dis_Zoom();
    --g_ourZoomCall;
}

static void QuickTurnTick(ADishonoredPlayerPawn* pawn) {
    FindXInput();
    if (!g_disZoomFn) g_disZoomFn = UFunction::FindFunction("Function DishonoredGame.DishonoredPlayerController.Dis_Zoom");
    static bool turning = false;
    static ULONGLONG lastMs = 0;
    static float elapsed = 0.0f, applied = 0.0f;
    static float lastTime = -1.0f, lastBend = -1.0f;
    ULONGLONG nowMs = GetTickCount64();
    float dt = lastMs ? (nowMs - lastMs) / 1000.0f : 0.0f;
    lastMs = nowMs;

    auto* pc = pawn ? static_cast<APlayerController*>(pawn->Controller) : nullptr;
    AWorldInfo* wi = pawn ? pawn->WorldInfo : nullptr;
    bool paused = true;
    if (wi) {
        paused = wi->TimeSeconds == lastTime && wi->BendTimeSeconds == lastBend;
        lastTime = wi->TimeSeconds; lastBend = wi->BendTimeSeconds;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool focused = pid == GetCurrentProcessId();

    // one press of R3 or Left Alt: tap -> turn, hold -> zoom while held
    bool alt = focused && (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
    bool down = g_cfg.quickTurn && (alt || PadR3Down());
    static bool wasDown = false, spoiled = false, zoomed = false;
    static ULONGLONG pressMs = 0, zoomMs = 0;
    static float baseFov = 0.0f;
    auto fov = [&]() { return pc && pc->PlayerCamera ? pc->PlayerCamera->CameraCache.POV.FOV : 0.0f; };
    if (down && !wasDown) {
        pressMs = nowMs; spoiled = !focused || paused || !pc; zoomed = false; baseFov = fov();
    }
    if (down) {
        if (alt && ((GetAsyncKeyState(VK_TAB) & 0x8000) || (GetAsyncKeyState(VK_F4) & 0x8000))) spoiled = true;
        if (!spoiled && !zoomed && nowMs - pressMs >= static_cast<ULONGLONG>(g_cfg.zoomHoldTime * 1000.0f)) {
            CallZoom(pc);
            zoomed = true; zoomMs = nowMs;
        }
    } else if (wasDown) {
        if (zoomed) {
            // still zoomed (narrower view than at the press, or the zoom only just started): zoom back out
            float f = fov();
            if ((baseFov > 0.0f && f > 0.0f && f < baseFov - 1.0f) || nowMs - zoomMs < 700) CallZoom(pc);
        } else if (!spoiled && !turning && pc && !paused && focused) {
            turning = true; elapsed = 0.0f; applied = 0.0f;
        }
        zoomed = false;
    }
    wasDown = down;
    if (!turning) return;
    if (!pc) { turning = false; return; }
    if (paused) return;                       // hold the turn while paused, finish it after
    elapsed += (std::min)(dt, 0.1f);
    float t = (std::min)(elapsed / g_cfg.quickTurnTime, 1.0f);
    float eased = t * t * (3.0f - 2.0f * t);   // smoothstep
    float want = eased * 32768.0f;             // 180 degrees in rotator units
    int step = static_cast<int>(want - applied);
    if (step) { pc->Rotation.Yaw += step; applied += static_cast<float>(step); }
    if (t >= 1.0f) turning = false;
}

// ProcessEvent: swallow the game's own Dis_Zoom while R3 / Left Alt is held (that press is ours, see above).
static void __fastcall HookedProcessEvent(UObject* obj, void*, UFunction* fn, void* parms, void* result) {
    {   // (diagnostic: per-bow upgrades) Piero's store - which calls come through here, and what the movie points at
        static UObject* storeCls = nullptr;
        static DWORD lastTry = 0;
        if (!storeCls && GetTickCount() - lastTry > 5000) { lastTry = GetTickCount(); storeCls = UObject::FindClass("Class DishonoredGame.DisGFxMoviePlayerStore"); }
        if (storeCls && fn && fn->Outer == storeCls && obj) {
            static UObject* dumpedFor = nullptr;
            static int logged = 0;
            std::string fname = fn->GetName();
            if (logged++ < 200) {
                int arg = (parms && (fname == "Req_BuyItem" || fname == "Req_ItemInfo")) ? *static_cast<int*>(parms) : -1;
                uint8_t* m = reinterpret_cast<uint8_t*>(obj);
                UObject* seq = *reinterpret_cast<UObject**>(m + 0x1B8);
                auto* shown = reinterpret_cast<RawArray*>(m + 0x1C4);
                Log("store: %s(%d) | movie tweaks %s, DLC tweaks %s | action %s: tweaks %s store %s | %d shown",
                    fname.c_str(), arg, ObjName(*reinterpret_cast<UObject**>(m + 0x1B0)).c_str(), ObjName(*reinterpret_cast<UObject**>(m + 0x1B4)).c_str(),
                    ObjName(seq).c_str(), seq ? ObjName(*reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(seq) + 0x128)).c_str() : "-",
                    seq ? ObjName(*reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(seq) + 0x12C)).c_str() : "-", shown->Data ? shown->Num : 0);
            }
            if (dumpedFor != obj) { dumpedFor = obj; DumpUpgradeData(static_cast<ADishonoredPlayerPawn*>(GetPlayerPawn()), "store opened"); }
        }
    }
    if (fn && fn == g_disZoomFn && !g_ourZoomCall && BackTurnButtonDown()) {
        static int logged = 0;
        if (logged++ < 5) Log("Ultimate Back Turn: game's Zoom on R3/Left Alt held back (hold to zoom)");
        return;
    }
    g_origProcessEvent(obj, fn, parms, result);
}

static void HookZoomButton() {
    if (MH_CreateHook(reinterpret_cast<void*>(addr::ProcessEvent), reinterpret_cast<void*>(&HookedProcessEvent),
                      reinterpret_cast<void**>(&g_origProcessEvent)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(addr::ProcessEvent)) == MH_OK) Log("ProcessEvent hooked (hold to zoom)");
    else Log("WARNING: could not hook ProcessEvent - R3/Left Alt zoom stays the game's");
}

// Always-loaded crossbow (2026-10-09): Corvo's crossbow and Daud's wristbow reload instantly after every shot - the
// next bolt is in as soon as one is fired, as long as there's ammo of that type left. Their player reload tweaks
// (DisTweaks_ReloadGunPlayer under the bow's pPlayerSpecific) get m_bDoNormalReloadInstantly and no reload time.
// The tweaks come and go with packages (Daud's with Body Swap), so look again every 5 s; already-set ones are skipped.
static void CrossbowInstantReload() {
    if (!g_cfg.crossbowAlwaysLoaded) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (last && now - last < 5000) return;
    last = now;
    UClass* cls = UDisTweaks_ReloadGunPlayer::StaticClass();
    if (!cls) return;
    auto* objects = UObject::GObjObjects();
    for (int i = 0; i < objects->size(); ++i) {
        UObject* obj = objects->at(i);
        if (!obj || obj->Class != cls) continue;
        auto* t = static_cast<UDisTweaks_ReloadGunPlayer*>(obj);
        if (t->m_bDoNormalReloadInstantly && t->m_fReloadTime == 0.0f) continue;
        std::string name = obj->GetFullName();
        if (name.find("rossbow") == std::string::npos && name.find("ristbow") == std::string::npos) continue;
        if (name.find("Default__") != std::string::npos) continue;
        Log("always-loaded bow: %s (reload %.2f s -> instant)", name.c_str(), t->m_fReloadTime);
        t->m_bDoNormalReloadInstantly = 1;
        t->m_fReloadTime = 0.0f;
    }
}

// Body Swap "Jump" (2026-10-09): Corvo's Agility jump is a double jump (StatePlayerMasterJump.m_JumpStyle from
// DishonoredPlayerState.ini = eDisJumpStyle_FixedNormalJump_FixedPowerJump); Daud's tweaks use one continuous power
// jump (DisDLC06Tweaks_PlayerPawn.m_PowerJumpStyle = ..._ContinuousPowerJump). Set m_JumpStyle (+0x59) on the jump
// state objects (and the class default) to match the switch: when it changes, and every 3 s for new pawns' states.
static void ApplyJumpStyle() {
    static DWORD last = 0;
    static LONG applied = -1;
    DWORD now = GetTickCount();
    LONG want = g_jumpChar;
    if (want == applied && last && now - last < 3000) return;
    last = now;
    static UClass* cls = nullptr;
    if (!cls) cls = UObject::FindClass("Class DishonoredGame.StatePlayerMasterJump");
    if (!cls) return;
    const uint8_t style = want ? 2 : 0;   // EeDisJumpStyle: 0 FixedPowerJump, 2 ContinuousPowerJump
    int changed = 0;
    auto* objects = UObject::GObjObjects();
    for (int i = 0; i < objects->size(); ++i) {
        UObject* obj = objects->at(i);
        if (!obj || obj->Class != cls) continue;
        auto* st = reinterpret_cast<uint8_t*>(obj) + 0x59;
        if (*st != style) { *st = style; ++changed; }
    }
    if (want != applied || changed) Log("jump: %s (%d jump state%s set)", want ? "Daud's continuous jump" : "Corvo's double jump",
                                        changed, changed == 1 ? "" : "s");
    applied = want;
}

// Summon Assassin without a limit (2026-10-09: "summon as many assassins as i want"): each level's m_MaxAssassins
// is 1 in DLC06_Pwr_Assassummon (one summon at a time). Every Summon Assassin tweaks object - Body Swap's and Daud's
// own in his DLCs - gets SummonAssassinMax (default 100) at both levels and the unlimited targeting range. Every 5 s,
// as the tweaks come and go with packages; already-set ones are skipped.
static void SummonAssassinUnlimited() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (last && now - last < 5000) return;
    last = now;
    UClass* cls = UDisDLC06Tweaks_SummonAssassin::StaticClass();
    if (!cls) return;
    const int maxAssassins = g_cfg.summonAssassinMax;
    const float castSpeed = 0.0f;   // the action's play-rate override stays the game's: 3.0 broke the summon (14:12)
    auto* objects = UObject::GObjObjects();
    for (int i = 0; i < objects->size(); ++i) {
        UObject* obj = objects->at(i);
        if (!obj || obj->Class != cls) continue;
        auto* t = static_cast<UDisDLC06Tweaks_SummonAssassin*>(obj);
        if (t->m_PowerLevels[0].m_MaxAssassins == maxAssassins && t->m_PowerLevels[1].m_MaxAssassins == maxAssassins &&
            t->m_fTargetingRange == 1000000.0f && t->m_UsePowerAction.m_fPlayRateOverride == castSpeed) continue;
        std::string name = obj->GetFullName();
        if (name.find("Default__") != std::string::npos) continue;
        Log("Summon Assassin: max assassins %d/%d -> %d, range %.0f -> 1000000, cast speed %.2f -> %.2f (%s)",
            t->m_PowerLevels[0].m_MaxAssassins, t->m_PowerLevels[1].m_MaxAssassins, maxAssassins, t->m_fTargetingRange,
            t->m_UsePowerAction.m_fPlayRateOverride, castSpeed, name.c_str());
        t->m_UsePowerAction.m_fPlayRateOverride = castSpeed;
        t->m_PowerLevels[0].m_MaxAssassins = maxAssassins;
        t->m_PowerLevels[1].m_MaxAssassins = maxAssassins;
        t->m_fTargetingRange = 1000000.0f;
    }
}


// Infinite mana for every power (2026-10-09: the Cheat Menu's "Infinite mana" still let Summon Assassin drain mana).
// The Mod Loader's infmana / infmana2 patches change the game's own mana writes (mov [esi+0A60h], eax at five sites;
// mov ecx, [esi+0A60h] at 0xAA93B8 / 0xAA93F8); Daud's powers take mana another way. While either patch is in (the
// bytes differ from the original), keep the player's m_Mana (+0xA60) at m_ManaMax (+0xA64).
static bool CheatInfiniteManaOn() {
    static const uintptr_t kSites[] = { 0xAA93B8, 0xAA93F8, 0xAA7948, 0xAB1A85, 0xAB614C, 0xABC961, 0xCF06B3 };
    static const uint8_t kRead[6] = { 0x8B, 0x8E, 0x60, 0x0A, 0x00, 0x00 }, kWrite[6] = { 0x89, 0x86, 0x60, 0x0A, 0x00, 0x00 };
    for (int i = 0; i < 7; ++i)
        if (memcmp(reinterpret_cast<void*>(kSites[i]), i < 2 ? kRead : kWrite, 6) != 0) return true;
    return false;
}

static void KeepManaFull(ADishonoredPlayerPawn* pawn) {
    if (!pawn) return;
    bool on = CheatInfiniteManaOn();
    static bool wasOn = false;
    if (on != wasOn) { wasOn = on; Log("infinite mana (Cheat Menu) %s: all powers free", on ? "on" : "off"); }
    if (!on) return;
    auto* mana = reinterpret_cast<int32_t*>(reinterpret_cast<char*>(pawn) + 0xA60);
    int32_t max = mana[1];
    if (max > 0 && mana[0] < max) mana[0] = max;
}

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
        g_shoulderBlob[0] = g_shoulderBlob[1] = nullptr;
        g_castLightEnv = nullptr;
        g_shadowBody = nullptr;
        g_shadowArms = nullptr;
        g_holsterCast = nullptr;
        g_itemShadow[0] = g_itemShadow[1] = nullptr;   // owned by the pawn; dropped with it
        g_armsShadow = nullptr;
        g_armSkin = nullptr;
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
        g_ghosted.clear(); g_ghostActive = false;   // that level's pawns are gone: nothing to put back
    }

    RestoreNpcs(GetPlayerPawn(), false);   // NPCs block again 1.5 s after the last Blink aim
    QuickTurnTick(GetPlayerPawn());
    CrossbowInstantReload();
    ApplyJumpStyle();
    SummonAssassinUnlimited();
    KeepManaFull(GetPlayerPawn());

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
            if (g_shadowArms && g_shadowArms->Animations) DetachNodeArrays(g_shadowArms->Animations, 0);
            if (g_holsterCast && g_holsterCast->Animations) DetachNodeArrays(g_holsterCast->Animations, 0);
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
        if (unreachable(g_body) || unreachable(g_shadowBody) || unreachable(g_shadowArms) || unreachable(g_holsterCast)) {
            Log("GC marked our body unreachable (body %d, shadow %d): detaching arrays before the purge",
                unreachable(g_body) ? 1 : 0, unreachable(g_shadowBody) ? 1 : 0);
            if (g_shadowBody && g_shadowBody->Animations) DetachNodeArrays(g_shadowBody->Animations, 0);
            if (g_shadowArms && g_shadowArms->Animations) DetachNodeArrays(g_shadowArms->Animations, 0);
            if (g_holsterCast && g_holsterCast->Animations) DetachNodeArrays(g_holsterCast->Animations, 0);
            DetachOurArrays(g_body);
            g_body = nullptr;
            g_head = nullptr;
            g_shoulderBlob[0] = g_shoulderBlob[1] = nullptr;
            g_castLightEnv = nullptr;
            g_shadowBody = nullptr;
            g_shadowArms = nullptr;
            g_holsterCast = nullptr;
            g_itemShadow[0] = g_itemShadow[1] = nullptr;   // owned by the pawn; dropped with it
        g_armsShadow = nullptr;
        g_armSkin = nullptr;
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
    if (g_shadowArms && g_shadowArms->Animations) DetachNodeArrays(g_shadowArms->Animations, 0);
    if (g_holsterCast && g_holsterCast->Animations) DetachNodeArrays(g_holsterCast->Animations, 0);
            DetachOurArrays(g_body);
    ReleaseLoadedAssets();
    g_bodyMesh = nullptr;
    g_animSet = nullptr;
    g_crouchSet = nullptr;
    g_headMesh = nullptr; g_daudBodyMesh = nullptr; g_daudHeadMesh = nullptr; ForgetArmsSwap();
    g_body = nullptr;
    g_head = nullptr;
    g_shoulderBlob[0] = g_shoulderBlob[1] = nullptr;
    g_castLightEnv = nullptr;
    g_shadowBody = nullptr;
    g_shadowArms = nullptr;
    g_holsterCast = nullptr;
    g_itemShadow[0] = g_itemShadow[1] = nullptr;   // owned by the pawn; dropped with it
        g_armsShadow = nullptr;
        g_armSkin = nullptr;
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
    if (pc && pc->PlayerCamera && pc->PlayerCamera->IsA(ADishonoredPlayerCamera::StaticClass()))
        g_armsFov = reinterpret_cast<ADishonoredPlayerCamera*>(pc->PlayerCamera)->m_fCurFOV_Arms;
    auto off = []() { g_camCorrection = { 0, 0, 0 }; g_camTarget = { 0, 0, 0 }; g_bodyFollowTarget = { 0, 0, 0 }; g_cornerLeanRight = g_cornerLeanFwd = g_stairDz = 0.0f; };
    if (!g_cfg.rigidCamera || !pc || !loc || !rot) { off(); return; }
    auto* pawn = static_cast<APawn*>(pc->Pawn);
    if (!pawn || pc->bCinematicMode || static_cast<APawn*>(g_bodyPawn) != pawn) { off(); return; }
    if (pc->PlayerCamera && pc->PlayerCamera->ViewTarget.Target &&
        pc->PlayerCamera->ViewTarget.Target != pawn && pc->PlayerCamera->ViewTarget.Target != pc) {
        off();
        return;
    }
    // Slide -> crouch with a weapon out (10:26 clip, "a flicker at the end of the slide"): the frame after a slide the
    // game steps the pawn up ~7 uu in one go. Its own 1P arms hide that - camera_jnt and the hands ease together in the
    // arms' animation, so nothing of the game's moves on screen - but the body, attached to the pawn, popped up with it
    // for one frame: the mod's per-frame hook runs after the frame has been drawn, so SyncTransform's absorb
    // (g_zAbsorb) only took hold a frame late, and the raised left knee flicked up at the camera (log 10:26:04: pelvis
    // 2018 -> 2025 -> 2019). This hook runs after the tick and before the draw: take the step out of the attached
    // meshes right here (ForceUpdate pushes the new transform to the renderer for this frame) and carry it in the
    // absorb, which SyncTransform then releases as before. Holstered, the arm skin rides along so the shoulders match.
    if (g_body && g_absorbSlideEndAt && GetTickCount() - g_absorbSlideEndAt < 1000 && g_absorbLastZ != 0.0f &&
        static_cast<int>(pawn->Physics) == 1) {
        float cyl = pawn->CylinderComponent ? pawn->CylinderComponent->CollisionHeight : 0.0f;
        float dtS = QpcDt(g_absorbQpc);
        float dz = pawn->Location.Z - g_absorbLastZ - (cyl - g_absorbLastCyl) - pawn->Velocity.Z * dtS;
        if (fabsf(dz) > 2.0f && fabsf(dz) < 40.0f) {
            g_zAbsorb -= dz;
            USkeletalMeshComponent* parts[6] = { g_body, g_shadowBody, g_shadowArms, g_holsterCast, g_armSkinOn ? g_armSkin : nullptr, g_head };
            int moved = 0;
            for (auto* c : parts) {
                if (!c || c->Owner != pawn || !c->bAttached) continue;
                FVector t = c->Translation;
                t.Z -= dz;
                c->SetTranslation(t);
                c->ForceUpdate(true);
                ++moved;
            }
            g_shadowBodyT.Z -= dz;
            Log("slide end: pawn Z stepped %.1f after the tick - %d attached meshes held in place before the draw (absorb %.1f)", dz, moved, g_zAbsorb);
        }
        g_absorbLastZ = pawn->Location.Z; g_absorbLastCyl = cyl;
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
    loc->Z += g_slideCamW * g_cfg.slideCameraUp;
    // Slide -> crouch (10:30 log: the view rose 25 uu in ~100 ms as the game lifted its camera out of the slide - a
    // jolt): for SlideCamSettle seconds after a slide the view height eases from where it was (smoothstep) to wherever
    // the game + the mod put it, so the climb into the crouch view is spread out instead of taken in six frames.
    {
        static LONGLONG endQpc = 0;
        static float endZ = 0.0f, lastOutZ = 0.0f;
        static bool wasSlide = false;
        static LONGLONG freq = 0;
        if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart ? f.QuadPart : 1; }
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        if (wasSlide && !g_isSliding && g_slideCamW > 0.5f && !g_leftHandBusy && !g_rightHandBusy) { endQpc = c.QuadPart; endZ = lastOutZ; }   // (holstered slides only: the eased view would part from the game's 1P arms)
        wasSlide = g_isSliding;
        float before = loc->Z;
        if (endQpc && g_cfg.slideCamSettle > 0.05f) {
            float u = static_cast<float>(static_cast<double>(c.QuadPart - endQpc) / static_cast<double>(freq)) / g_cfg.slideCamSettle;
            if (u < 1.0f) { float ss = u * u * (3.0f - 2.0f * u); loc->Z = endZ + (loc->Z - endZ) * ss; }
            else endQpc = 0;
        }
        g_slideCamSettleOff = loc->Z - before;   // (10:35 clip: with a weapon out the 1P arms bounced - they must ease with the view)
        lastOutZ = loc->Z;
    }
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
    // Z: ALWAYS take our shift back out. The game eases the arms' Z during slides, so a changed Z is not "the game
    // rewrote it" - with the 45 uu slide raise in the shift that test re-based our own lift every frame and the arms
    // (and the camera riding them) ran away upward until the 120 uu guard (10:10 clip, "floating"; log: camera 122 uu
    // from the eyes). If the game sets Z outright each frame this just nullifies the Z shift, which is safe.
    base.Z -= g_armsShiftLocal.Z;
    bool ours = true;
    FVector c = (g_cfg.rigidCamera && g_cfg.rigidCameraArms) ? g_camTarget : FVector{ 0, 0, 0 };
    // The slide camera raise too (10:05 clip: with a weapon out the 1P hands sat 45 uu under the lifted view and looked
    // small). The view hook takes the arms shift back out before adding the raise, so this moves only the arms.
    // (10:40: no slide terms here any more - the game eases the 1P arms' Z out of a slide itself, and any Z we add is
    // folded into that easing and fed back to the camera riding the arms: 10:35 bounce, 10:38 double bounce. Weapon
    // slides keep the game's camera height instead, see UpdateSlide.)
    c.Z += g_slideCamW * g_cfg.slideCameraUp;   // (the 1P arms rise with the slide camera: hands keep their size; the settle is holstered-only)
    c.Z = Clamp(c.Z, -60.0f, 60.0f);
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

// Body Swap plugin (Mods\plugins\BodySwap.dll) API. part: 0 body, 1 first-person arms, 2 sword, 3 power set, 4 crossbow,
// 5 jump (Agility's power jump);
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
    else if (part == 5) InterlockedExchange(&g_jumpChar, value);
    Log("Body Swap: %s -> %s", part == 0 ? "body" : part == 1 ? "arms" : part == 2 ? "blade" : part == 3 ? "powers" : part == 4 ? "crossbow" : "jump",
        value == 2 ? "Overseer" : value ? "Daud" : "Corvo");
}

extern "C" __declspec(dllexport) int __cdecl CorvoBody_GetCharacter(int part) {
    return part == 0 ? static_cast<int>(g_bodyChar) : part == 1 ? static_cast<int>(g_armsChar) : part == 2 ? static_cast<int>(g_bladeChar) : part == 3 ? static_cast<int>(g_powerChar) : part == 4 ? static_cast<int>(g_bowChar) : part == 5 ? static_cast<int>(g_jumpChar) : 0;
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

    RawLoadPackageAsync = reinterpret_cast<LoadPackageAsync_t>(
        FindPattern("55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 81 EC 8C 00 00 00 56 57"));
    Log("LoadPackageAsync at %p", reinterpret_cast<void*>(RawLoadPackageAsync));

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
    HookBlinkClearance();
    HookZoomButton();
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

// Title screen ("CorvoBody Version 3" under the Dishonored logo, release 3.0): the zip installs an edited
// DishonoredGame\CookedPCConsole\Dishonored_MainMenu.upk, which is larger than the game's own. The engine reads every
// package's size from DishonoredGame\PCConsoleTOC*.txt, so the entry must match the file on disk. Done here, in
// DllMain: DINPUT8.dll is a static import of Dishonored.exe, so this runs before the engine has read anything. Each TOC
// is only rewritten when its entry differs from the installed file (vanilla package -> vanilla size, so Steam's
// "verify files" and an uninstall stay consistent). Plain kernel32 file calls only (safe under the loader lock).
static void SyncTitleTocSize(HINSTANCE self) {
    char root[MAX_PATH];
    if (!GetModuleFileNameA(self, root, MAX_PATH)) return;
    for (int up = 0; up < 3; ++up) {   // ...\Binaries\Win32\dinput8.dll -> the game folder
        char* sl = strrchr(root, '\\');
        if (!sl) return;
        *sl = 0;
    }
    std::string base = std::string(root) + "\\DishonoredGame\\";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA((base + "CookedPCConsole\\Dishonored_MainMenu.upk").c_str(), GetFileExInfoStandard, &fa)) return;
    unsigned long long size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA((base + "PCConsoleTOC*.txt").c_str(), &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    const std::string key = "\\cookedpcconsole\\dishonored_mainmenu.upk 0";
    do {
        std::string path = base + fd.cFileName;
        HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD len = GetFileSize(f, nullptr), got = 0;
        std::string text(len, '\0');
        bool ok = len > 0 && len < 16u * 1024u * 1024u && ReadFile(f, &text[0], len, &got, nullptr) && got == len;
        CloseHandle(f);
        if (!ok) continue;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find("\r\n", pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            std::string low = line;
            for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            while (!low.empty() && (low.back() == ' ' || low.back() == '\t')) low.pop_back();
            if (low.size() >= key.size() && low.compare(low.size() - key.size(), key.size(), key) == 0) {
                size_t sp = line.find(' ');
                if (sp != std::string::npos) {
                    unsigned long long listed = strtoull(line.substr(0, sp).c_str(), nullptr, 10);
                    if (listed != size) {
                        std::string fixed = std::to_string(size) + line.substr(sp);
                        text = text.substr(0, pos) + fixed + text.substr(end);
                        std::string tmp = path + ".corvobody.tmp";
                        HANDLE w = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                        DWORD wr = 0;
                        bool wok = w != INVALID_HANDLE_VALUE && WriteFile(w, text.data(), static_cast<DWORD>(text.size()), &wr, nullptr) && wr == text.size();
                        if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
                        if (wok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
                            Log("title screen: %s lists Dishonored_MainMenu.upk as %llu bytes - set to %llu", fd.cFileName, listed, size);
                        else { DeleteFileA(tmp.c_str()); Log("title screen: could not update %s (%llu -> %llu)", fd.cFileName, listed, size); }
                    }
                }
                break;
            }
            pos = end + 2;
        }
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_self = inst;
        OpenLog(inst);
        SyncTitleTocSize(inst);
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

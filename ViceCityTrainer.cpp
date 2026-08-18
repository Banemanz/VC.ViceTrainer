/*
    Vice City Trainer - single-file Plugin-SDK source

    Target: Grand Theft Auto: Vice City (classic PC), Plugin-SDK snapshot around 2025-10-31.
    Baseline executable behavior: Vice City 1.0 EN IDB/function index supplied with the project.

    Controls:
      F7             Open/close trainer
      F6             Toggle airbreak
      Up/Down        Navigate
      Enter/Right    Select/toggle
      Backspace/Left Back/close submenu

    Airbreak:
      W/S            Forward/back
      A/D            Strafe left/right
      Space/LControl Up/down
      LShift         Fast move

    Notes:
      - Vehicle spawning follows VC's own VehicleCheat pattern: RequestModel(flag=1),
        LoadAllRequestedModels(false), construct, then CWorld::Add.
      - "Unlimited Ammo" keeps reserve ammo full but preserves normal reloads.
      - "No Reload" also keeps the clip full and clears reload/out-of-ammo states.

    VC 1.0 EN reverse-engineering anchors used while implementing this file:
      CStreaming::LoadAllRequestedModels(bool)  0x40B5F0
      CStreaming::RequestModel(int,int)         0x40E310
      FindPlayerPed()                          0x4BC120
      VehicleCheat(int)                        0x4AE8F0
      WeaponCheat3()                           0x4AEAD0
      CPed::GiveWeapon(...)                    0x4FFA30
      CWeapon::Reload()                        0x5CA3C0
*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "plugin.h"
#include "RenderWare.h"
#include "common.h"
#include "CFont.h"
#include "CSprite2d.h"
#include "CStreaming.h"
#include "CWorld.h"
#include "CPlayerPed.h"
#include "CWeapon.h"
#include "CWeaponInfo.h"
#include "CVehicle.h"
#include "CAutomobile.h"
#include "CBike.h"
#include "CBoat.h"
#include "CTimer.h"
#include "CPathFind.h"
#include "CPathNode.h"
#include "eVehicleModel.h"

using namespace plugin;

namespace vc_trainer {

constexpr float kBaseWidth  = 640.0f;
constexpr float kBaseHeight = 448.0f;
constexpr unsigned int kInfiniteAmmo = 99999u; // VC CPed::GiveWeapon clamps totals to 99999.

enum class Page {
    Main,
    Vehicles
};

enum class VehicleKind {
    Automobile,
    Bike,
    Boat
};

struct VehicleEntry {
    const char* name;
    int model;
    VehicleKind kind;
};

static const VehicleEntry kVehicles[] = {
    { "Infernus",      MODEL_INFERNUS, VehicleKind::Automobile },
    { "Cheetah",       MODEL_CHEETAH,  VehicleKind::Automobile },
    { "Banshee",       MODEL_BANSHEE,  VehicleKind::Automobile },
    { "Comet",         MODEL_COMET,    VehicleKind::Automobile },
    { "Deluxo",        MODEL_DELUXO,   VehicleKind::Automobile },
    { "Sabre Turbo",   MODEL_SABRETUR, VehicleKind::Automobile },
    { "Phoenix",       MODEL_PHEONIX,  VehicleKind::Automobile },
    { "Patriot",       MODEL_PATRIOT,  VehicleKind::Automobile },
    { "Hotring Racer", MODEL_HOTRING,  VehicleKind::Automobile },
    { "Police",        MODEL_POLICE,   VehicleKind::Automobile },
    { "Rhino",         MODEL_RHINO,    VehicleKind::Automobile },
    { "PCJ-600",       MODEL_PCJ600,   VehicleKind::Bike },
    { "Sanchez",       MODEL_SANCHEZ,  VehicleKind::Bike },
    { "Freeway",       MODEL_FREEWAY,  VehicleKind::Bike },
    { "Angel",         MODEL_ANGEL,    VehicleKind::Bike },
    { "Maverick",      MODEL_MAVERICK, VehicleKind::Automobile },
    { "Hunter",        MODEL_HUNTER,   VehicleKind::Automobile },
    { "Sparrow",       MODEL_SPARROW,  VehicleKind::Automobile },
    { "Sea Sparrow",   MODEL_SEASPAR,  VehicleKind::Automobile },
    { "Skimmer",       MODEL_SKIMMER,  VehicleKind::Automobile },
    { "Jetmax",        MODEL_JETMAX,   VehicleKind::Boat },
    { "Squalo",        MODEL_SQUALO,   VehicleKind::Boat },
    { "Speeder",       MODEL_SPEEDER,  VehicleKind::Boat },
    { "Predator",      MODEL_PREDATOR, VehicleKind::Boat },
};

constexpr int kVehicleCount = static_cast<int>(sizeof(kVehicles) / sizeof(kVehicles[0]));
constexpr int kMainCount = 13;

struct State {
    bool menuOpen = false;
    bool airbreak = false;
    bool godMode = false;
    bool vehicleGodMode = false;
    bool unlimitedAmmo = false;
    bool noReload = false;
    bool neverWanted = false;

    Page page = Page::Main;
    int mainSelection = 0;
    int vehicleSelection = 0;

    bool hasSavedPosition = false;
    CVector savedPosition = CVector(0.0f, 0.0f, 0.0f);

    char notification[128] = {};
    DWORD notificationUntil = 0;
};

static State g;

struct ProofSnapshot {
    CEntity* entity = nullptr;
    bool valid = false;
    bool bullet = false;
    bool fire = false;
    bool explosion = false;
    bool collision = false;
    bool melee = false;
    bool nonPlayer = false;
};

struct VehicleProofSnapshot : ProofSnapshot {
    bool tires = false;
};

struct AmmoSlotSnapshot {
    bool valid = false;
    eWeaponType type = WEAPONTYPE_UNARMED;
    unsigned int total = 0;
    unsigned int clip = 0;
    eWeaponState state = WEAPONSTATE_READY;
};

static ProofSnapshot gPlayerProof;
static VehicleProofSnapshot gVehicleProof;
static AmmoSlotSnapshot gAmmo[10];
static bool gKeyNow[256] = {};
static bool gKeyPrev[256] = {};
static bool gKeysInitialised = false;

// Uniform UI scale prevents 16:9 resolutions from stretching a 640-wide menu into
// something enormous.  Coordinates are still authored against VC's 640x448 space.
static float UiScale() {
    const float sx = static_cast<float>(RsGlobal.maximumWidth) / kBaseWidth;
    const float sy = static_cast<float>(RsGlobal.maximumHeight) / kBaseHeight;
    return std::min(sx, sy);
}

static float U(float v) {
    return v * UiScale();
}

static bool IsDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void UpdateKeys() {
    for (int i = 0; i < 256; ++i) {
        gKeyPrev[i] = gKeyNow[i];
        gKeyNow[i] = IsDown(i);
    }

    if (!gKeysInitialised) {
        std::memcpy(gKeyPrev, gKeyNow, sizeof(gKeyNow));
        gKeysInitialised = true;
    }
}

static bool Pressed(int vk) {
    const int key = vk & 0xFF;
    return gKeyNow[key] && !gKeyPrev[key];
}

static void Notify(const char* text) {
    std::strncpy(g.notification, text, sizeof(g.notification) - 1);
    g.notification[sizeof(g.notification) - 1] = '\0';
    g.notificationUntil = GetTickCount() + 1800;
}

static void CaptureProofs(CEntity* entity, ProofSnapshot& snap) {
    if (!entity)
        return;
    snap.entity = entity;
    snap.valid = true;
    snap.bullet = entity->bBulletProof;
    snap.fire = entity->bFireProof;
    snap.explosion = entity->bExplosionProof;
    snap.collision = entity->bCollisionProof;
    snap.melee = entity->bMeleeProof;
    snap.nonPlayer = entity->bImmuneToNonPlayerDamage;
}

static void ApplyProofs(CEntity* entity, bool enabled) {
    if (!entity)
        return;
    entity->bBulletProof = enabled;
    entity->bFireProof = enabled;
    entity->bExplosionProof = enabled;
    entity->bCollisionProof = enabled;
    entity->bMeleeProof = enabled;
    entity->bImmuneToNonPlayerDamage = enabled;
}

static void RestoreProofs(ProofSnapshot& snap) {
    if (!snap.valid || !snap.entity)
        return;
    CEntity* entity = snap.entity;
    entity->bBulletProof = snap.bullet;
    entity->bFireProof = snap.fire;
    entity->bExplosionProof = snap.explosion;
    entity->bCollisionProof = snap.collision;
    entity->bMeleeProof = snap.melee;
    entity->bImmuneToNonPlayerDamage = snap.nonPlayer;
    snap = ProofSnapshot();
}

static CPhysical* GetAirbreakEntity() {
    if (CVehicle* vehicle = FindPlayerVehicle())
        return vehicle;
    return FindPlayerPed();
}

static void ZeroVelocity(CPhysical* physical) {
    if (!physical)
        return;
    physical->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
    physical->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
}

static float HeadingRadians(CEntity* entity) {
    if (!entity)
        return 0.0f;
    const CVector& forward = entity->GetForward();
    return std::atan2(-forward.x, forward.y);
}

static float PlayerMaxHealth() {
    if (!CWorld::Players)
        return 100.0f;
    return static_cast<float>(std::max(100, static_cast<int>(CWorld::Players[CWorld::PlayerInFocus].m_nMaxHealth)));
}

static float PlayerMaxArmour() {
    if (!CWorld::Players)
        return 100.0f;
    return static_cast<float>(std::max(100, static_cast<int>(CWorld::Players[CWorld::PlayerInFocus].m_nMaxArmour)));
}

static void ApplyGodMode() {
    CPlayerPed* player = FindPlayerPed();
    if (!player)
        return;

    if (!gPlayerProof.valid || gPlayerProof.entity != player) {
        if (gPlayerProof.valid)
            RestoreProofs(gPlayerProof);
        CaptureProofs(player, gPlayerProof);
    }

    ApplyProofs(player, true);
    player->m_fHealth = std::max(player->m_fHealth, PlayerMaxHealth());
    player->m_fArmour = std::max(player->m_fArmour, PlayerMaxArmour());
}

static void DisableGodModeNow() {
    RestoreProofs(gPlayerProof);
}

static void RestoreVehicleProofs() {
    if (!gVehicleProof.valid || !gVehicleProof.entity)
        return;

    CVehicle* vehicle = static_cast<CVehicle*>(gVehicleProof.entity);
    vehicle->bBulletProof = gVehicleProof.bullet;
    vehicle->bFireProof = gVehicleProof.fire;
    vehicle->bExplosionProof = gVehicleProof.explosion;
    vehicle->bCollisionProof = gVehicleProof.collision;
    vehicle->bMeleeProof = gVehicleProof.melee;
    vehicle->bImmuneToNonPlayerDamage = gVehicleProof.nonPlayer;
    vehicle->bCarTiresInvulnerable = gVehicleProof.tires;
    gVehicleProof = VehicleProofSnapshot();
}

static void ApplyVehicleGodMode() {
    CVehicle* vehicle = FindPlayerVehicle();

    if (!vehicle) {
        // Leaving a still-existing vehicle should not leave it permanently trainer-proofed.
        RestoreVehicleProofs();
        return;
    }

    if (!gVehicleProof.valid || gVehicleProof.entity != vehicle) {
        RestoreVehicleProofs();
        CaptureProofs(vehicle, gVehicleProof);
        gVehicleProof.tires = vehicle->bCarTiresInvulnerable;
    }

    ApplyProofs(vehicle, true);
    vehicle->bCarTiresInvulnerable = true;
    vehicle->m_fHealth = std::max(vehicle->m_fHealth, 1000.0f);
    vehicle->ExtinguishCarFire();
}

static void DisableVehicleGodModeNow() {
    RestoreVehicleProofs();
}

static void HealPlayer() {
    CPlayerPed* player = FindPlayerPed();
    if (!player) {
        Notify("Player not ready");
        return;
    }

    player->m_fHealth = PlayerMaxHealth();
    player->m_fArmour = PlayerMaxArmour();
    Notify("Health + armour restored");
}

static void ClearWanted() {
    CPlayerPed* player = FindPlayerPed();
    if (!player) {
        Notify("Player not ready");
        return;
    }

    player->SetWantedLevel(0);
    Notify("Wanted level cleared");
}

static void RequestWeaponModels(eWeaponType type) {
    CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(type);
    if (!info)
        return;

    if (info->m_nModelId >= 0)
        CStreaming::RequestModel(info->m_nModelId, 1);
    if (info->m_nModel2Id >= 0)
        CStreaming::RequestModel(info->m_nModel2Id, 1);
}

static void ReleaseWeaponModels(eWeaponType type) {
    CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(type);
    if (!info)
        return;

    if (info->m_nModelId >= 0)
        CStreaming::SetModelIsDeletable(info->m_nModelId);
    if (info->m_nModel2Id >= 0)
        CStreaming::SetModelIsDeletable(info->m_nModel2Id);
}

static void GiveWeaponSet() {
    CPlayerPed* player = FindPlayerPed();
    if (!player) {
        Notify("Player not ready");
        return;
    }

    struct WeaponGrant { eWeaponType type; unsigned int ammo; };
    // Exact WeaponCheat3 quantities from VC 1.0 at 0x4AEAD0.
    static const WeaponGrant weapons[] = {
        { WEAPONTYPE_CHAINSAW,       0   },
        { WEAPONTYPE_GRENADE,        10  },
        { WEAPONTYPE_PYTHON,         40  },
        { WEAPONTYPE_SPAS12_SHOTGUN, 30  },
        { WEAPONTYPE_MP5,            100 },
        { WEAPONTYPE_M4,             150 },
        { WEAPONTYPE_LASERSCOPE,     21  },
        { WEAPONTYPE_MINIGUN,        500 }
    };

    for (const WeaponGrant& weapon : weapons)
        RequestWeaponModels(weapon.type);

    CStreaming::LoadAllRequestedModels(false);

    for (const WeaponGrant& weapon : weapons)
        player->GiveWeapon(weapon.type, weapon.ammo, true);

    player->SetCurrentWeapon(WEAPONTYPE_M4);

    for (const WeaponGrant& weapon : weapons)
        ReleaseWeaponModels(weapon.type);

    Notify("Weapon set granted");
}

static void SnapshotAmmoSlot(CPlayerPed* player, int slot) {
    CWeapon& weapon = player->m_aWeapons[slot];
    AmmoSlotSnapshot& snap = gAmmo[slot];
    if (snap.valid && snap.type == weapon.m_eWeaponType)
        return;

    snap.valid = true;
    snap.type = weapon.m_eWeaponType;
    snap.total = weapon.m_nAmmoTotal;
    snap.clip = weapon.m_nAmmoInClip;
    snap.state = weapon.m_eWeaponState;
}

static void RestoreAmmoCheats() {
    CPlayerPed* player = FindPlayerPed();
    if (player) {
        for (int i = 0; i < 10; ++i) {
            AmmoSlotSnapshot& snap = gAmmo[i];
            if (!snap.valid)
                continue;
            CWeapon& weapon = player->m_aWeapons[i];
            if (weapon.m_eWeaponType == snap.type) {
                weapon.m_nAmmoTotal = snap.total;
                weapon.m_nAmmoInClip = snap.clip;
                weapon.m_eWeaponState = snap.state;
            }
        }
    }
    for (AmmoSlotSnapshot& snap : gAmmo)
        snap = AmmoSlotSnapshot();
}

static void ApplyAmmoCheats() {
    CPlayerPed* player = FindPlayerPed();
    if (!player)
        return;

    for (int i = 0; i < 10; ++i) {
        CWeapon& weapon = player->m_aWeapons[i];
        if (weapon.m_eWeaponType == WEAPONTYPE_UNARMED)
            continue;

        SnapshotAmmoSlot(player, i);

        if (g.unlimitedAmmo || g.noReload)
            weapon.m_nAmmoTotal = kInfiniteAmmo;

        if (g.noReload) {
            CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weapon.m_eWeaponType);
            if (info && info->m_nAmountofAmmunition > 0)
                weapon.m_nAmmoInClip = info->m_nAmountofAmmunition;

            if (weapon.m_eWeaponState == WEAPONSTATE_RELOADING ||
                weapon.m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO) {
                weapon.m_eWeaponState = WEAPONSTATE_READY;
            }
        }
    }
}

static void RepairVehicle() {
    CVehicle* vehicle = FindPlayerVehicle();
    if (!vehicle) {
        Notify("No vehicle");
        return;
    }

    vehicle->m_fHealth = 1000.0f;
    vehicle->ExtinguishCarFire();
    vehicle->bEngineOn = true;

    const int appearance = vehicle->GetVehicleAppearance();
    if (appearance == VEHICLE_APPEARANCE_BIKE) {
        static_cast<CBike*>(vehicle)->Fix();
    } else if (appearance == VEHICLE_APPEARANCE_AUTOMOBILE ||
               appearance == VEHICLE_APPEARANCE_HELI ||
               appearance == VEHICLE_APPEARANCE_PLANE) {
        static_cast<CAutomobile*>(vehicle)->Fix();
    }

    if (g.vehicleGodMode)
        ApplyVehicleGodMode();

    Notify("Vehicle repaired");
}

static void FlipVehicle() {
    CVehicle* vehicle = FindPlayerVehicle();
    if (!vehicle) {
        Notify("No vehicle");
        return;
    }

    const CVector oldPos = vehicle->GetPosition();
    const float yaw = HeadingRadians(vehicle);

    vehicle->SetHeading(yaw);
    vehicle->SetPosition(oldPos.x, oldPos.y, oldPos.z + 0.25f);
    ZeroVelocity(vehicle);
    vehicle->UpdateRwFrame();

    Notify("Vehicle set upright");
}

static void SavePosition() {
    CEntity* entity = FindPlayerVehicle() ? static_cast<CEntity*>(FindPlayerVehicle())
                                          : static_cast<CEntity*>(FindPlayerPed());
    if (!entity) {
        Notify("Player not ready");
        return;
    }

    g.savedPosition = entity->GetPosition();
    g.hasSavedPosition = true;
    Notify("Position saved");
}

static void LoadPosition() {
    if (!g.hasSavedPosition) {
        Notify("No saved position");
        return;
    }

    CPhysical* entity = GetAirbreakEntity();
    if (!entity) {
        Notify("Player not ready");
        return;
    }

    ZeroVelocity(entity);
    entity->Teleport(g.savedPosition);
    Notify("Position loaded");
}

static CVehicle* ConstructVehicle(const VehicleEntry& entry) {
    // Trainer-created vehicles must not enter the normal random-traffic cleanup path.
    // VC's eVehicleCreatedBy value 4 is explicitly PERMANENT_VEHICLE.
    constexpr unsigned char createdBy = PERMANENT_VEHICLE;

    switch (entry.kind) {
    case VehicleKind::Bike:
        return new CBike(entry.model, createdBy);
    case VehicleKind::Boat:
        return new CBoat(entry.model, createdBy);
    case VehicleKind::Automobile:
    default:
        return new CAutomobile(entry.model, createdBy);
    }
}

static bool FindVehicleCheatSpawnPoint(CVector& out) {
    // This follows VC's own VehicleCheat(int) at 0x4AE8F0:
    //   FindPlayerCoors -> ThePaths.FindNodeClosestToCoors(..., 100.0f, false...)
    //   node XYZ / 8.0 -> Z + 4.0f.
    const CVector playerPos = FindPlayerCoors();
    const int node = ThePaths.FindNodeClosestToCoors(
        playerPos, 0, 100.0f, false, false, false, false);

    if (node >= 0) {
        out = ThePaths.nodes[node].GetPosition();
        out.z += 4.0f;
        return true;
    }

    // Defensive fallback for interiors / unusual path coverage.  Query from above the
    // player so the ground search cannot select a surface below an already-low origin.
    bool foundGround = false;
    const float groundZ = CWorld::FindGroundZFor3DCoord(
        playerPos.x, playerPos.y, playerPos.z + 25.0f, &foundGround);
    out = playerPos;
    out.z = foundGround ? groundZ + 4.0f : playerPos.z + 4.0f;
    return foundGround;
}

static void SpawnVehicle(const VehicleEntry& entry) {
    CPlayerPed* player = FindPlayerPed();
    if (!player) {
        Notify("Player not ready");
        return;
    }

    const bool wasAlreadyRequested = (CStreaming::ms_aInfoForModel[entry.model].m_nFlags & 1) != 0;

    CStreaming::RequestModel(entry.model, 1);
    CStreaming::LoadAllRequestedModels(false);

    if (CStreaming::ms_aInfoForModel[entry.model].m_nLoadState != LOADSTATE_LOADED) {
        if (!wasAlreadyRequested) {
            CStreaming::SetModelIsDeletable(entry.model);
            CStreaming::SetModelTxdIsDeletable(entry.model);
        }
        Notify("Vehicle model failed to load");
        return;
    }

    // Match the original VehicleCheat ordering: release our temporary streaming request
    // before creating the instance.  Calling these after CWorld::Add can race the next
    // streaming update against a just-created trainer vehicle.
    if (!wasAlreadyRequested) {
        CStreaming::SetModelIsDeletable(entry.model);
        CStreaming::SetModelTxdIsDeletable(entry.model);
    }

    CVector spawnPos;
    if (!FindVehicleCheatSpawnPoint(spawnPos)) {
        Notify("No safe vehicle spawn point");
        return;
    }

    CVehicle* vehicle = ConstructVehicle(entry);
    if (!vehicle) {
        Notify("Vehicle allocation failed");
        return;
    }

    CEntity* anchor = FindPlayerVehicle() ? static_cast<CEntity*>(FindPlayerVehicle())
                                          : static_cast<CEntity*>(player);
    vehicle->SetPosition(spawnPos.x, spawnPos.y, spawnPos.z);
    vehicle->SetHeading(HeadingRadians(anchor));
    vehicle->m_eDoorLock = DOORLOCK_UNLOCKED;
    vehicle->bEngineOn = true;

    // Plugin-SDK's current VC name is still unknown, but this is the executable's
    // flagsC bit 6: reVC identifies it as bStreamingDontDelete.  Combined with
    // PERMANENT_VEHICLE this keeps trainer spawns out of both streaming and random
    // traffic cleanup without pinning the vehicle model globally.
    vehicle->bEntUFlag23 = true;

    ZeroVelocity(vehicle);
    CWorld::Add(vehicle);

    char msg[96];
    std::snprintf(msg, sizeof(msg), "Spawned %s", entry.name);
    Notify(msg);
}

static void ToggleAirbreak() {
    g.airbreak = !g.airbreak;
    if (!g.airbreak)
        ZeroVelocity(GetAirbreakEntity());
    Notify(g.airbreak ? "Airbreak ON" : "Airbreak OFF");
}

static void ProcessAirbreak() {
    if (!g.airbreak)
        return;

    CPhysical* entity = GetAirbreakEntity();
    if (!entity)
        return;

    ZeroVelocity(entity);

    float move = 0.0f;
    float strafe = 0.0f;
    float vertical = 0.0f;

    if (IsDown('W')) move += 1.0f;
    if (IsDown('S')) move -= 1.0f;
    if (IsDown('D')) strafe += 1.0f;
    if (IsDown('A')) strafe -= 1.0f;
    if (IsDown(VK_SPACE)) vertical += 1.0f;
    if (IsDown(VK_LCONTROL) || IsDown(VK_RCONTROL)) vertical -= 1.0f;

    if (move == 0.0f && strafe == 0.0f && vertical == 0.0f)
        return;

    const float yaw = HeadingRadians(entity);
    const float s = std::sin(yaw);
    const float c = std::cos(yaw);

    // VC's matrix convention: horizontal forward = (-sin(yaw), cos(yaw)).
    const CVector forward(-s, c, 0.0f);
    const CVector right(c, s, 0.0f);

    const float timeStep = std::max(CTimer::ms_fTimeStep, 0.01f);
    const float speed = (IsDown(VK_LSHIFT) || IsDown(VK_RSHIFT) ? 1.25f : 0.35f) * timeStep;

    CVector pos = entity->GetPosition();
    pos.x += (forward.x * move + right.x * strafe) * speed;
    pos.y += (forward.y * move + right.y * strafe) * speed;
    pos.z += vertical * speed;

    // Use the game's own virtual Teleport path so sectors/RW state stay coherent.
    entity->Teleport(pos);
    ZeroVelocity(entity);
}

static const char* OnOff(bool value) {
    return value ? "ON" : "OFF";
}

static const char* MainLabel(int index) {
    switch (index) {
    case 0:  return "God Mode";
    case 1:  return "Heal + Armour";
    case 2:  return "Never Wanted";
    case 3:  return "Give Weapon Set";
    case 4:  return "Unlimited Ammo";
    case 5:  return "No Reload";
    case 6:  return "Vehicle God Mode";
    case 7:  return "Repair Vehicle";
    case 8:  return "Flip Vehicle";
    case 9:  return "Spawn Vehicle";
    case 10: return "Save Position";
    case 11: return "Load Position";
    case 12: return "Airbreak [F6]";
    default: return "";
    }
}

static const char* MainValue(int index) {
    switch (index) {
    case 0:  return OnOff(g.godMode);
    case 2:  return OnOff(g.neverWanted);
    case 4:  return OnOff(g.unlimitedAmmo);
    case 5:  return OnOff(g.noReload);
    case 6:  return OnOff(g.vehicleGodMode);
    case 9:  return ">";
    case 11: return g.hasSavedPosition ? "READY" : "EMPTY";
    case 12: return OnOff(g.airbreak);
    default: return nullptr;
    }
}

static bool AnyAmmoCheat() {
    return g.unlimitedAmmo || g.noReload;
}

static void ActivateMain(int index) {
    switch (index) {
    case 0:
        g.godMode = !g.godMode;
        if (!g.godMode)
            DisableGodModeNow();
        Notify(g.godMode ? "God Mode ON" : "God Mode OFF");
        break;
    case 1:
        HealPlayer();
        break;
    case 2:
        g.neverWanted = !g.neverWanted;
        if (g.neverWanted)
            ClearWanted();
        else
            Notify("Never Wanted OFF");
        break;
    case 3:
        GiveWeaponSet();
        break;
    case 4: {
        const bool wasActive = AnyAmmoCheat();
        g.unlimitedAmmo = !g.unlimitedAmmo;
        if (wasActive && !AnyAmmoCheat())
            RestoreAmmoCheats();
        Notify(g.unlimitedAmmo ? "Unlimited Ammo ON" : "Unlimited Ammo OFF");
        break;
    }
    case 5: {
        const bool wasActive = AnyAmmoCheat();
        g.noReload = !g.noReload;
        if (wasActive && !AnyAmmoCheat())
            RestoreAmmoCheats();
        Notify(g.noReload ? "No Reload ON" : "No Reload OFF");
        break;
    }
    case 6:
        g.vehicleGodMode = !g.vehicleGodMode;
        if (!g.vehicleGodMode)
            DisableVehicleGodModeNow();
        Notify(g.vehicleGodMode ? "Vehicle God Mode ON" : "Vehicle God Mode OFF");
        break;
    case 7:
        RepairVehicle();
        break;
    case 8:
        FlipVehicle();
        break;
    case 9:
        g.page = Page::Vehicles;
        break;
    case 10:
        SavePosition();
        break;
    case 11:
        LoadPosition();
        break;
    case 12:
        ToggleAirbreak();
        break;
    default:
        break;
    }
}

static void ProcessMenuInput() {
    if (Pressed(VK_F7)) {
        g.menuOpen = !g.menuOpen;
        if (g.menuOpen)
            g.page = Page::Main;
    }

    if (Pressed(VK_F6))
        ToggleAirbreak();

    if (!g.menuOpen)
        return;

    if (g.page == Page::Main) {
        if (Pressed(VK_UP))
            g.mainSelection = (g.mainSelection + kMainCount - 1) % kMainCount;
        if (Pressed(VK_DOWN))
            g.mainSelection = (g.mainSelection + 1) % kMainCount;
        if (Pressed(VK_RETURN) || Pressed(VK_RIGHT))
            ActivateMain(g.mainSelection);
        if (Pressed(VK_BACK) || Pressed(VK_LEFT))
            g.menuOpen = false;
    } else {
        if (Pressed(VK_UP))
            g.vehicleSelection = (g.vehicleSelection + kVehicleCount - 1) % kVehicleCount;
        if (Pressed(VK_DOWN))
            g.vehicleSelection = (g.vehicleSelection + 1) % kVehicleCount;
        if (Pressed(VK_RETURN) || Pressed(VK_RIGHT))
            SpawnVehicle(kVehicles[g.vehicleSelection]);
        if (Pressed(VK_BACK) || Pressed(VK_LEFT))
            g.page = Page::Main;
    }
}

static void ProcessCheats() {
    if (g.godMode)
        ApplyGodMode();
    if (g.vehicleGodMode)
        ApplyVehicleGodMode();
    if (g.neverWanted) {
        if (CPlayerPed* player = FindPlayerPed())
            player->SetWantedLevel(0);
    }
    if (AnyAmmoCheat())
        ApplyAmmoCheats();
    ProcessAirbreak();
}

static void DrawRect(float left, float top, float right, float bottom, const CRGBA& color) {
    CSprite2d::DrawRect(CRect(U(left), U(top), U(right), U(bottom)), color);
}

static void PrepareFont(const CRGBA& color, float scaleX, float scaleY, bool rightAligned = false) {
    CFont::SetBackgroundOff();
    CFont::SetCentreOff();
    CFont::SetJustifyOff();
    CFont::SetRightJustifyOff();
    if (rightAligned)
        CFont::SetRightJustifyOn();
    else
        CFont::SetJustifyOn();
    CFont::SetPropOn();
    CFont::SetFontStyle(FONT_STANDARD);
    CFont::SetDropShadowPosition(1);
    CFont::SetDropColor(CRGBA(0, 0, 0, 230));
    CFont::SetColor(color);
    CFont::SetScale(U(scaleX), U(scaleY));
}

static void DrawText(float x, float y, const char* text, const CRGBA& color,
                     float scaleX = 0.235f, float scaleY = 0.48f) {
    PrepareFont(color, scaleX, scaleY, false);
    CFont::PrintString(U(x), U(y), text);
}

static void DrawTextRight(float x, float y, const char* text, const CRGBA& color,
                          float scaleX = 0.225f, float scaleY = 0.46f) {
    PrepareFont(color, scaleX, scaleY, true);
    CFont::PrintString(U(x), U(y), text);
}

constexpr float kPanelWidth = 176.0f;
constexpr float kHeaderHeight = 28.0f;
constexpr float kRowTop = 30.0f;
constexpr float kRowHeight = 13.0f;
constexpr float kFooterHeight = 13.0f;

static void DrawHeader(const char* title, const char* hint) {
    DrawRect(0.0f, 0.0f, kPanelWidth, kHeaderHeight, CRGBA(8, 8, 12, 222));
    DrawRect(0.0f, 0.0f, kPanelWidth, 2.0f, CRGBA(255, 70, 180, 255));
    DrawRect(0.0f, kHeaderHeight - 2.0f, kPanelWidth, kHeaderHeight, CRGBA(50, 220, 235, 255));
    DrawText(6.0f, 6.0f, title, CRGBA(255, 255, 255, 255), 0.29f, 0.59f);
    if (hint && *hint)
        DrawTextRight(kPanelWidth - 6.0f, 8.0f, hint, CRGBA(150, 220, 232, 255), 0.18f, 0.37f);
}

static void DrawFooter(float top) {
    DrawRect(0.0f, top, kPanelWidth, top + kFooterHeight, CRGBA(8, 8, 12, 222));
    DrawText(5.0f, top + 3.0f, "ENTER SELECT   BACK   F7 CLOSE", CRGBA(170, 175, 185, 255), 0.165f, 0.34f);
}

static void DrawMainMenu() {
    const float bodyBottom = kRowTop + kRowHeight * kMainCount;
    DrawHeader("VICE TRAINER", "F6 AIR");
    DrawRect(0.0f, kHeaderHeight, kPanelWidth, bodyBottom, CRGBA(12, 12, 18, 205));

    for (int i = 0; i < kMainCount; ++i) {
        const float y = kRowTop + kRowHeight * i;
        const bool selected = i == g.mainSelection;
        if (selected)
            DrawRect(3.0f, y, kPanelWidth - 3.0f, y + kRowHeight - 1.0f, CRGBA(255, 70, 180, 118));

        DrawText(7.0f, y + 2.0f, MainLabel(i),
                 selected ? CRGBA(255, 255, 255, 255) : CRGBA(210, 215, 220, 255));

        if (const char* value = MainValue(i))
            DrawTextRight(kPanelWidth - 7.0f, y + 2.0f, value,
                          selected ? CRGBA(255, 255, 255, 255) : CRGBA(175, 215, 220, 255),
                          0.205f, 0.42f);
    }

    DrawFooter(bodyBottom);
}

static void DrawVehicleMenu() {
    constexpr int visibleRows = 12;
    const float bodyBottom = kRowTop + kRowHeight * visibleRows;
    DrawHeader("SPAWN VEHICLE", "ENTER");
    DrawRect(0.0f, kHeaderHeight, kPanelWidth, bodyBottom, CRGBA(12, 12, 18, 205));

    int first = g.vehicleSelection - visibleRows / 2;
    first = std::max(0, std::min(first, kVehicleCount - visibleRows));

    for (int row = 0; row < visibleRows; ++row) {
        const int index = first + row;
        if (index >= kVehicleCount)
            break;
        const float y = kRowTop + kRowHeight * row;
        const bool selected = index == g.vehicleSelection;
        if (selected)
            DrawRect(3.0f, y, kPanelWidth - 3.0f, y + kRowHeight - 1.0f, CRGBA(50, 220, 235, 115));

        DrawText(7.0f, y + 2.0f, kVehicles[index].name,
                 selected ? CRGBA(255, 255, 255, 255) : CRGBA(210, 215, 220, 255));
    }

    char count[32];
    std::snprintf(count, sizeof(count), "%d/%d", g.vehicleSelection + 1, kVehicleCount);
    DrawTextRight(kPanelWidth - 7.0f, bodyBottom - 10.5f, count, CRGBA(125, 175, 185, 255), 0.16f, 0.33f);
    DrawFooter(bodyBottom);
}

static float CurrentPanelBottom() {
    if (!g.menuOpen)
        return 0.0f;
    if (g.page == Page::Main)
        return kRowTop + kRowHeight * kMainCount + kFooterHeight;
    return kRowTop + kRowHeight * 12.0f + kFooterHeight;
}

static void DrawStatusOverlay() {
    float y = 0.0f;
    if (!g.menuOpen && g.airbreak) {
        DrawRect(0.0f, 0.0f, 79.0f, 12.0f, CRGBA(8, 8, 12, 200));
        DrawText(4.0f, 2.0f, "AIRBREAK ON [F6]", CRGBA(50, 220, 235, 255), 0.17f, 0.35f);
        y = 13.0f;
    } else if (g.menuOpen) {
        y = CurrentPanelBottom() + 1.0f;
    }

    if (g.notification[0] != '\0' && GetTickCount() < g.notificationUntil) {
        DrawRect(0.0f, y, kPanelWidth, y + 13.0f, CRGBA(8, 8, 12, 215));
        DrawText(5.0f, y + 3.0f, g.notification, CRGBA(255, 255, 255, 255), 0.185f, 0.38f);
    }
}

static void Draw() {
    if (g.menuOpen) {
        if (g.page == Page::Main)
            DrawMainMenu();
        else
            DrawVehicleMenu();
    }
    DrawStatusOverlay();
}

static void ResetRuntimeState() {
    g.menuOpen = false;
    g.airbreak = false;
    g.godMode = false;
    g.vehicleGodMode = false;
    g.unlimitedAmmo = false;
    g.noReload = false;
    g.neverWanted = false;
    g.page = Page::Main;
    gPlayerProof = ProofSnapshot();
    gVehicleProof = VehicleProofSnapshot();
    for (AmmoSlotSnapshot& snap : gAmmo)
        snap = AmmoSlotSnapshot();
}

static void Process() {
    UpdateKeys();
    ProcessMenuInput();
    ProcessCheats();
}

class ViceCityTrainerPlugin {
public:
    ViceCityTrainerPlugin() {
        Events::gameProcessEvent += [] { Process(); };
        Events::drawHudEvent += [] { Draw(); };

        Events::vehicleDtorEvent += [](CVehicle* vehicle) {
            if (gVehicleProof.valid && gVehicleProof.entity == vehicle)
                gVehicleProof = VehicleProofSnapshot();
        };
        Events::pedDtorEvent += [](CPed* ped) {
            if (gPlayerProof.valid && gPlayerProof.entity == ped)
                gPlayerProof = ProofSnapshot();
            for (AmmoSlotSnapshot& snap : gAmmo)
                snap = AmmoSlotSnapshot();
        };
        Events::restartGameEvent += [] { ResetRuntimeState(); };
    }
};

static ViceCityTrainerPlugin gPlugin;

} // namespace vc_trainer

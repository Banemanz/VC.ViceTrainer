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
      (shown on-screen whenever airbreak is active)

    Notes:
      - Vehicle streaming follows VC's VehicleCheat request pattern. Lifetime/placement
        follows VC's CREATE_CAR mission-entity path so trainer spawns do not get culled.
      - ViceTrainer.ini is generated beside the DLL only when missing and is never overwritten.
      - Airbreak toggle/movement keys and speeds are configurable in the INI.
      - Skin selector is split into Normal Peds, Cutscene Models, and Custom Skins.
      - Custom Skins are loose external DFF+TXD pairs loaded by the trainer itself.
      - External TXDs are fully loaded and made current before their DFF is read.
      - Custom DFFs use a genuinely unused normal-ped model ID; special/cutscene slots are never hijacked.
      - External skin weights/HAnim data are validated before VC normalises them in SetClump.
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
      CClumpModelInfo::SetClump(RpClump*)      0x541420
      CPedModelInfo::SetClump(RpClump*)        0x5665C0
      GetAnimHierarchyFromClump(RpClump*)      0x57F1E0
      ConvertPedNode2BoneTag(int)               0x405DE0
      CTxdStore::LoadTxd(int,char const*)      0x580CD0
*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "user32.lib")
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

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
#include "CTheScripts.h"
#include "CTxdStore.h"
#include "CModelInfo.h"
#include "CPedModelInfo.h"
#include "CPlayerInfo.h"
#include "CCarCtrl.h"
#include "CTheZones.h"
#include "CTimer.h"
#include "CPathFind.h"
#include "CPathNode.h"
#include "eVehicleModel.h"

using namespace plugin;

namespace vc_trainer {

constexpr float kBaseWidth  = 640.0f;
constexpr float kBaseHeight = 448.0f;
constexpr unsigned int kInfiniteAmmo = 99999u; // VC CPed::GiveWeapon clamps totals to 99999.
constexpr int kSpecialPedFirst = 109; // CStreaming::RequestSpecialChar(slot, ...) adds 0x6D.
constexpr int kSpecialPedLast  = 129; // 21 VC special-character slots total.

enum class Page {
    Main,
    Vehicles,
    SkinCategories,
    SkinList
};

enum class SkinCategory {
    NormalPeds,
    CutsceneModels,
    CustomSkins
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
constexpr int kMainCount = 14;

struct Config {
    int menuKey = VK_F7;
    int airbreakKey = VK_F6;
    int airForwardKey = 'W';
    int airBackKey = 'S';
    int airLeftKey = 'A';
    int airRightKey = 'D';
    int airUpKey = VK_SPACE;
    int airDownKey = VK_CONTROL;
    int airFastKey = VK_SHIFT;
    bool showAirbreakControls = true;
    float airbreakSpeed = 0.35f;
    float airbreakFastSpeed = 1.25f;
    bool includeNormalPeds = true;
    bool includeCutsceneModels = true;
    bool includeCustomSkins = true;
    std::string customSkinDirectory = "ViceTrainerSkins";
};

static Config gConfig;
static std::string gPluginDirectory;
static std::string gGameDirectory;
static std::string gIniPath;

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
    int skinCategorySelection = 0;
    int normalPedSelection = 0;
    int cutsceneSelection = 0;
    int customSkinSelection = 0;
    SkinCategory activeSkinCategory = SkinCategory::NormalPeds;

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

enum class SkinKind {
    DefaultPlayer,
    PedModel,
    ExternalDff
};

struct SkinEntry {
    SkinKind kind = SkinKind::DefaultPlayer;
    std::string name;
    std::string dffPath;
    std::string txdPath;
    int modelId = -1;
};

struct ExternalSkinRuntime {
    int modelId = -1;
    int txdSlot = -1;
    CPedModelInfo* modelInfo = nullptr;
    char txdName[20] = {};
};

static std::vector<SkinEntry> gNormalPedSkins;
static std::vector<SkinEntry> gCutsceneSkins;
static std::vector<SkinEntry> gCustomSkins;
static ExternalSkinRuntime gExternalSkin;
static char gSkinLoadError[96] = {};

static ProofSnapshot gPlayerProof;
static VehicleProofSnapshot gVehicleProof;
static AmmoSlotSnapshot gAmmo[10];
static bool gKeyNow[256] = {};
static bool gKeyPrev[256] = {};
static bool gKeysInitialised = false;

// VehicleCheat(int) is authoritative for ordinary automobile creation.  The SDK's
// vehicleCtorEvent fires synchronously from CVehicle::CVehicle, so while the native
// cheat is running we remember the exact instance it allocates.  After VehicleCheat
// returns (the object is fully constructed and already in CWorld), we can convert its
// lifetime from RANDOM_VEHICLE to the same mission-owned/locked semantics used by
// CREATE_CAR without guessing pool slots or scanning the world.
static bool gCaptureNativeVehicleCtor = false;
static CVehicle* gCapturedNativeVehicle = nullptr;

static void Notify(const char* text);


static std::string DirectoryName(const std::string& path) {
    const std::string::size_type slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

static std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty())
        return right;
    if (right.empty())
        return left;
    const char last = left[left.size() - 1];
    if (last == '\\' || last == '/')
        return left + right;
    return left + "\\" + right;
}

static bool IsAbsolutePath(const std::string& path) {
    return path.size() >= 2 && path[1] == ':' ||
           !path.empty() && (path[0] == '\\' || path[0] == '/');
}

static bool FileExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool DirectoryExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool EnsureDirectoryTree(const std::string& path) {
    if (path.empty() || DirectoryExists(path))
        return true;

    std::string partial;
    partial.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        const char ch = path[i];
        partial.push_back(ch);
        if (ch != '\\' && ch != '/')
            continue;

        // Skip bare drive roots (C:\) and the leading slash of rooted paths.
        if (partial.size() <= 3)
            continue;
        if (!DirectoryExists(partial))
            CreateDirectoryA(partial.c_str(), nullptr);
    }

    if (!DirectoryExists(path))
        CreateDirectoryA(path.c_str(), nullptr);
    return DirectoryExists(path);
}

static std::string ModuleDirectory(HMODULE module) {
    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
    if (length == 0 || length >= sizeof(path))
        return ".";
    return DirectoryName(path);
}

static std::string ResolvePluginPath(const std::string& path) {
    return IsAbsolutePath(path) ? path : JoinPath(gPluginDirectory, path);
}

static int ReadIniInt(const char* section, const char* key, const char* fallback) {
    char value[64] = {};
    GetPrivateProfileStringA(section, key, fallback, value, sizeof(value), gIniPath.c_str());
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 0);
    return end != value ? static_cast<int>(parsed) : std::strtol(fallback, nullptr, 0);
}

static float ReadIniFloat(const char* section, const char* key, const char* fallback) {
    char value[64] = {};
    GetPrivateProfileStringA(section, key, fallback, value, sizeof(value), gIniPath.c_str());
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    return end != value ? parsed : std::strtof(fallback, nullptr);
}

static void CreateDefaultIniIfMissing() {
    HANDLE file = CreateFileA(gIniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return; // Existing files are deliberately never rewritten.

    static const char defaults[] =
        "; ViceTrainer runtime configuration. Existing files are never overwritten.\r\n"
        "; Virtual-key values may be decimal or hexadecimal (0x76 = F7, 0x75 = F6).\r\n"
        "[Controls]\r\n"
        "MenuKey=0x76\r\n"
        "AirbreakKey=0x75\r\n"
        "AirForwardKey=0x57\r\n"
        "AirBackKey=0x53\r\n"
        "AirLeftKey=0x41\r\n"
        "AirRightKey=0x44\r\n"
        "AirUpKey=0x20\r\n"
        "AirDownKey=0x11\r\n"
        "AirFastKey=0x10\r\n"
        "ShowAirbreakControls=1\r\n"
        "AirbreakSpeed=0.35\r\n"
        "AirbreakFastSpeed=1.25\r\n"
        "\r\n"
        "[Skins]\r\n"
        "; Normal runtime ped models. Special-character slots are kept out of this page.\r\n"
        "IncludeNormalPeds=1\r\n"
        "; VC special-character slots 109-129. These are shown on their own page.\r\n"
        "IncludeCutsceneModels=1\r\n"
        "; Loose matching name.dff + name.txd pairs loaded directly by the trainer.\r\n"
        "IncludeCustomSkins=1\r\n"
        "; Relative paths are resolved from the trainer DLL directory. Subfolders are scanned too.\r\n"
        "CustomSkinDirectory=ViceTrainerSkins\r\n";

    DWORD written = 0;
    WriteFile(file, defaults, static_cast<DWORD>(sizeof(defaults) - 1), &written, nullptr);
    CloseHandle(file);
}

static void InitialiseConfig() {
    gGameDirectory = ModuleDirectory(nullptr);

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void*>(&gConfig), &mbi, sizeof(mbi)) != 0)
        gPluginDirectory = ModuleDirectory(static_cast<HMODULE>(mbi.AllocationBase));
    else
        gPluginDirectory = gGameDirectory;

    gIniPath = JoinPath(gPluginDirectory, "ViceTrainer.ini");
    CreateDefaultIniIfMissing();

    gConfig.menuKey = ReadIniInt("Controls", "MenuKey", "0x76") & 0xFF;
    gConfig.airbreakKey = ReadIniInt("Controls", "AirbreakKey", "0x75") & 0xFF;
    gConfig.airForwardKey = ReadIniInt("Controls", "AirForwardKey", "0x57") & 0xFF;
    gConfig.airBackKey = ReadIniInt("Controls", "AirBackKey", "0x53") & 0xFF;
    gConfig.airLeftKey = ReadIniInt("Controls", "AirLeftKey", "0x41") & 0xFF;
    gConfig.airRightKey = ReadIniInt("Controls", "AirRightKey", "0x44") & 0xFF;
    gConfig.airUpKey = ReadIniInt("Controls", "AirUpKey", "0x20") & 0xFF;
    gConfig.airDownKey = ReadIniInt("Controls", "AirDownKey", "0x11") & 0xFF;
    gConfig.airFastKey = ReadIniInt("Controls", "AirFastKey", "0x10") & 0xFF;
    if (gConfig.menuKey == 0) gConfig.menuKey = VK_F7;
    if (gConfig.airbreakKey == 0) gConfig.airbreakKey = VK_F6;
    if (gConfig.airForwardKey == 0) gConfig.airForwardKey = 'W';
    if (gConfig.airBackKey == 0) gConfig.airBackKey = 'S';
    if (gConfig.airLeftKey == 0) gConfig.airLeftKey = 'A';
    if (gConfig.airRightKey == 0) gConfig.airRightKey = 'D';
    if (gConfig.airUpKey == 0) gConfig.airUpKey = VK_SPACE;
    if (gConfig.airDownKey == 0) gConfig.airDownKey = VK_CONTROL;
    if (gConfig.airFastKey == 0) gConfig.airFastKey = VK_SHIFT;
    gConfig.showAirbreakControls = ReadIniInt("Controls", "ShowAirbreakControls", "1") != 0;
    gConfig.airbreakSpeed = std::max(0.01f, ReadIniFloat("Controls", "AirbreakSpeed", "0.35"));
    gConfig.airbreakFastSpeed = std::max(gConfig.airbreakSpeed,
        ReadIniFloat("Controls", "AirbreakFastSpeed", "1.25"));
    gConfig.includeNormalPeds = ReadIniInt("Skins", "IncludeNormalPeds", "1") != 0;
    gConfig.includeCutsceneModels = ReadIniInt("Skins", "IncludeCutsceneModels", "1") != 0;
    gConfig.includeCustomSkins = ReadIniInt("Skins", "IncludeCustomSkins", "1") != 0;

    // Keep old test-build INIs useful without rewriting them.  CustomSkinDirectory wins;
    // if it is absent, fall back to the earlier ExternalSkinDirectory key.
    char legacyDir[MAX_PATH] = {};
    GetPrivateProfileStringA("Skins", "ExternalSkinDirectory", "ViceTrainerSkins",
                             legacyDir, sizeof(legacyDir), gIniPath.c_str());
    char customDir[MAX_PATH] = {};
    GetPrivateProfileStringA("Skins", "CustomSkinDirectory", legacyDir[0] ? legacyDir : "ViceTrainerSkins",
                             customDir, sizeof(customDir), gIniPath.c_str());
    gConfig.customSkinDirectory = customDir[0] ? customDir : "ViceTrainerSkins";

    const std::string resolvedCustomDir = ResolvePluginPath(gConfig.customSkinDirectory);
    EnsureDirectoryTree(resolvedCustomDir);
}

static void FormatKeyName(int vk, char* out, size_t size) {
    if (!out || size == 0)
        return;
    out[0] = '\0';
    if (vk >= VK_F1 && vk <= VK_F24) {
        std::snprintf(out, size, "F%d", vk - VK_F1 + 1);
        return;
    }
    if (vk >= 'A' && vk <= 'Z' || vk >= '0' && vk <= '9') {
        out[0] = static_cast<char>(vk);
        if (size > 1)
            out[1] = '\0';
        return;
    }
    switch (vk) {
    case VK_SPACE: std::snprintf(out, size, "SPACE"); return;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: std::snprintf(out, size, "SHIFT"); return;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: std::snprintf(out, size, "CTRL"); return;
    case VK_MENU: case VK_LMENU: case VK_RMENU: std::snprintf(out, size, "ALT"); return;
    case VK_RETURN: std::snprintf(out, size, "ENTER"); return;
    case VK_BACK: std::snprintf(out, size, "BACK"); return;
    default: std::snprintf(out, size, "0x%02X", vk & 0xFF); return;
    }
}

static int PlayerBaseModelId() {
    static int cached = -2;
    if (cached != -2)
        return cached;

    int modelId = -1;
    CBaseModelInfo* info = CModelInfo::GetModelInfo("player", &modelId);
    cached = info ? modelId : 0;
    return cached;
}

static bool EnsurePedModelLoaded(int modelId) {
    if (modelId < 0 || modelId >= 6500)
        return false;
    CBaseModelInfo* info = CModelInfo::GetModelInfo(modelId);
    if (!info || info->m_nType != MODEL_INFO_PED)
        return false;
    if (info->GetRwObject())
        return true;

    const bool wasHeld = (CStreaming::ms_aInfoForModel[modelId].m_nFlags & 1) != 0;
    CStreaming::RequestModel(modelId, 1);
    CStreaming::LoadAllRequestedModels(false);
    const bool loaded = info->GetRwObject() != nullptr;

    if (!wasHeld) {
        CStreaming::SetModelIsDeletable(modelId);
        CStreaming::SetModelTxdIsDeletable(modelId);
    }
    return loaded;
}

static bool SwitchPlayerToModel(int modelId) {
    CPlayerPed* player = FindPlayerPed();
    if (!player || !EnsurePedModelLoaded(modelId))
        return false;
    player->SetModelIndex(modelId);
    player->UpdateRwFrame();
    return player->m_nModelIndex == modelId;
}

static void SetSkinLoadError(const char* text) {
    if (!text)
        text = "";
    std::strncpy(gSkinLoadError, text, sizeof(gSkinLoadError) - 1);
    gSkinLoadError[sizeof(gSkinLoadError) - 1] = '\0';
}

static int FindFreeExternalPedModelId() {
    // Do NOT borrow VC's special-character slots.  RequestSpecialModel owns those slots
    // and can replace their model-info contents at any time.  The previous build used a
    // special slot while leaving streaming state inconsistent, which could orphan our
    // RpClump when the game later reused that slot.
    //
    // VC reserves model IDs below 130 for peds and its PedModelStore has 130 objects.
    // Use a genuinely unused normal-ped ID only if the store still has capacity.  This
    // makes the trainer model a normal CModelInfo-owned object with normal shutdown.
    if (!CModelInfo::ms_pedModelStore || CModelInfo::ms_pedModelStore->m_nCount >= 130)
        return -1;

    for (int modelId = kSpecialPedFirst - 1; modelId > 0; --modelId) {
        if (CModelInfo::GetModelInfo(modelId) == nullptr)
            return modelId;
    }
    return -1;
}

static bool EnsureExternalModelInfo() {
    if (gExternalSkin.modelInfo && gExternalSkin.modelId > 0 &&
        gExternalSkin.modelId < kSpecialPedFirst)
        return true;

    const int modelId = FindFreeExternalPedModelId();
    if (modelId < 0) {
        SetSkinLoadError("No free VC ped model slot");
        return false;
    }

    CBaseModelInfo* baseRaw = CModelInfo::GetModelInfo(PlayerBaseModelId());
    if (!baseRaw || baseRaw->m_nType != MODEL_INFO_PED) {
        SetSkinLoadError("Player model info unavailable");
        return false;
    }

    CPedModelInfo* base = static_cast<CPedModelInfo*>(baseRaw);
    CPedModelInfo* external = CModelInfo::AddPedModel(modelId);
    if (!external) {
        SetSkinLoadError("Could not allocate VC ped model info");
        return false;
    }

    // Initialise the new model like a normal ped instead of borrowing a transient
    // cutscene/special model-info object.  The generic ped collision model is shared;
    // the skinned hit-collision model is deliberately left null so SetClump creates a
    // fresh one for the custom skeleton.
    std::strncpy(external->m_szName, "vctcustom", sizeof(external->m_szName) - 1);
    external->m_szName[sizeof(external->m_szName) - 1] = '\0';
    external->m_pClump = nullptr;
    external->m_nAnimFileIndex = base->m_nAnimFileIndex;
    external->m_pColModel = base->m_pColModel;
    external->m_bDoWeOwnTheColModel = false;
    external->m_nRefCount = 0;
    external->ClearTexDictionary();
    external->m_nAnigGroupId = base->m_nAnigGroupId;
    external->m_nPedType = base->m_nPedType;
    external->m_nPedStatType = base->m_nPedStatType;
    external->m_nCarsCanDriveMask = base->m_nCarsCanDriveMask;
    external->m_pHitColModel = nullptr;
    external->m_anPreferredRadioStations[0] = base->m_anPreferredRadioStations[0];
    external->m_anPreferredRadioStations[1] = base->m_anPreferredRadioStations[1];

    gExternalSkin.modelId = modelId;
    gExternalSkin.modelInfo = external;
    return true;
}

static bool SwitchPlayerAwayFromExternalModel() {
    CPlayerPed* player = FindPlayerPed();
    if (!player || gExternalSkin.modelId < 0 || player->m_nModelIndex != gExternalSkin.modelId)
        return true;

    if (!SwitchPlayerToModel(PlayerBaseModelId()))
        return false;

    CWorld::Players[CWorld::PlayerInFocus].SetPlayerSkin("");
    return true;
}

static bool ReleaseExternalSkinAssets(bool detachPlayer) {
    if (detachPlayer && !SwitchPlayerAwayFromExternalModel()) {
        SetSkinLoadError("Could not detach current custom skin");
        return false;
    }

    CPedModelInfo* modelInfo = gExternalSkin.modelInfo;
    if (modelInfo && modelInfo->m_pClump) {
        // CPedModelInfo::DeleteRwObject destroys the model clump, removes its TXD ref,
        // and frees the hit collision model.  This must happen while the RW Skin plugin
        // is still alive; leaving a skinned geometry for _rpClumpClose is too late.
        modelInfo->DeleteRwObject();
    }
    if (modelInfo)
        modelInfo->ClearTexDictionary();

    if (gExternalSkin.txdSlot >= 0 && CTxdStore::ms_pTxdPool &&
        CTxdStore::ms_pTxdPool->GetAt(gExternalSkin.txdSlot)) {
        CTxdStore::RemoveTxdSlot(gExternalSkin.txdSlot);
    }

    gExternalSkin.txdSlot = -1;
    gExternalSkin.txdName[0] = '\0';
    return true;
}

static bool PrepareForExternalReload() {
    if (!ReleaseExternalSkinAssets(true))
        return false;
    return EnsureExternalModelInfo();
}

static RpAtomic* CaptureFirstAtomic(RpAtomic* atomic, void* data) {
    if (!data)
        return nullptr;
    *static_cast<RpAtomic**>(data) = atomic;
    return nullptr; // stop after the first atomic, matching VC's IsClumpSkinned helper
}

static const RwUInt32* GetSkinVertexBoneIndicesVC(RpSkin* skin) {
    if (!skin)
        return nullptr;

    // VC 1.0 does not export/implement RpSkinGetVertexBoneIndices even though the
    // RenderWare header declares it.  SkinCreateSkinData (0x6495B0) stores the
    // per-vertex packed bone-index array pointer directly at RpSkin + 0x14;
    // RpSkinGetVertexBoneWeights immediately following it reads RpSkin + 0x18.
    // Read the field directly so this remains linkable against plugin_vc.lib.
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(skin);
    return *reinterpret_cast<const RwUInt32* const*>(raw + 0x14);
}

static RwInt32 GetHAnimNodeCountVC(const RpHAnimHierarchy* hierarchy) {
    if (!hierarchy)
        return 0;

    // VC 1.0 RenderWare layout, verified against RpHAnimIDGetIndex (0x646390):
    //   +0x04 = number of nodes
    //   +0x10 = RpHAnimNodeInfo table
    // Do not dereference RpHAnimHierarchy::pNodeInfo here: the SDK header layout is
    // not suitable for this direct runtime validation on Vice City's RW build.
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(hierarchy);
    return *reinterpret_cast<const RwInt32*>(raw + 0x04);
}

static const void* GetHAnimNodeInfoVC(const RpHAnimHierarchy* hierarchy) {
    if (!hierarchy)
        return nullptr;

    const unsigned char* raw = reinterpret_cast<const unsigned char*>(hierarchy);
    return *reinterpret_cast<void* const*>(raw + 0x10);
}

static const char* ValidateExternalPedClump(RpClump* clump) {
    if (!clump)
        return "DFF has no clump";

    RpAtomic* atomic = nullptr;
    RpClumpForAllAtomics(clump, CaptureFirstAtomic, &atomic);
    if (!atomic || !atomic->geometry)
        return "DFF has no ped geometry";

    RpSkin* skin = RpSkinGeometryGetSkin(atomic->geometry);
    if (!skin)
        return "DFF is not a skinned ped";

    // VC's CClumpModelInfo::SetClump calls GetAnimHierarchyFromClump and then assumes
    // it returned a valid hierarchy.  Validate that before handing an external DFF to
    // the original routine.
    RpHAnimHierarchy* hierarchy = plugin::CallAndReturn<RpHAnimHierarchy*, 0x57F1E0, RpClump*>(clump);
    if (!hierarchy)
        return "DFF has no HAnim hierarchy";

    const RwInt32 hierarchyNodes = GetHAnimNodeCountVC(hierarchy);
    if (hierarchyNodes <= 0 || hierarchyNodes > 128 || !GetHAnimNodeInfoVC(hierarchy))
        return "DFF has invalid HAnim hierarchy";

    // CPed::SetModelIndex -> RpAnimBlendClumpFillFrameArray asks the hierarchy for
    // every VC ped node 1..17.  RpHAnimIDGetIndex returning -1 is not checked by VC;
    // the game turns that -1 into a bogus frame pointer.  That is especially toxic to
    // physics/ragdoll mods which continuously consume the player's bone-frame array.
    for (int pedNode = 1; pedNode <= 17; ++pedNode) {
        const int boneTag = plugin::CallAndReturn<int, 0x405DE0, int>(pedNode);
        if (boneTag < 0 || RpHAnimIDGetIndex(hierarchy, boneTag) < 0)
            return "DFF is missing required VC ped bones";
    }

    const RwUInt32 numBones = RpSkinGetNumBones(skin);
    const RwInt32 numVertices = RpGeometryGetNumVertices(atomic->geometry);
    const RwMatrixWeights* weights = RpSkinGetVertexBoneWeights(skin);
    const RwUInt32* indices = GetSkinVertexBoneIndicesVC(skin);
    if (numBones == 0 || numBones > 128 || numBones > static_cast<RwUInt32>(hierarchyNodes))
        return "DFF has invalid skin bone count";
    if (numVertices <= 0 || !weights || !indices)
        return "DFF has invalid skin data";

    // VC normalises each vertex's four weights in-place in
    // CClumpModelInfo::SetClump.  A zero/NaN sum therefore turns into NaNs and can
    // poison animation/ragdoll bone matrices.  Reject those models before VC mutates
    // them, and also validate every weighted bone index.
    for (RwInt32 i = 0; i < numVertices; ++i) {
        const float w[4] = { weights[i].w0, weights[i].w1, weights[i].w2, weights[i].w3 };
        float sum = 0.0f;
        for (int j = 0; j < 4; ++j) {
            if (!std::isfinite(w[j]) || w[j] < 0.0f)
                return "DFF has invalid skin weights";
            sum += w[j];
        }
        if (!std::isfinite(sum) || sum <= 0.000001f)
            return "DFF has zero/invalid weight sum";

        const RwUInt32 packed = indices[i];
        for (int j = 0; j < 4; ++j) {
            if (w[j] <= 0.000001f)
                continue;
            const RwUInt32 bone = (packed >> (j * 8)) & 0xFFu;
            if (bone >= numBones)
                return "DFF references invalid skin bone";
        }
    }

    return nullptr;
}

static RpClump* LoadClumpFileSafe(const char* filename) {
    if (!filename || !*filename)
        return nullptr;

    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, filename);
    if (!stream)
        return nullptr;

    RpClump* clump = nullptr;
    if (RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr))
        clump = RpClumpStreamRead(stream);

    RwStreamClose(stream, nullptr);
    return clump;
}

static bool LoadExternalSkin(const SkinEntry& skin) {
    SetSkinLoadError("");

    CPlayerPed* player = FindPlayerPed();
    if (!player || !FileExists(skin.txdPath) || !FileExists(skin.dffPath)) {
        SetSkinLoadError("DFF/TXD pair is missing");
        return false;
    }
    if (!PrepareForExternalReload())
        return false;

    // Use a unique TXD-store name rather than a global hard-coded "vctskin" slot.
    // The model-info stores the resolved TXD index, so the external file's basename
    // does not need to equal the RenderWare dictionary slot name.
    std::snprintf(gExternalSkin.txdName, sizeof(gExternalSkin.txdName),
                  "vct%03X%08X", gExternalSkin.modelId & 0xFFF,
                  static_cast<unsigned int>(GetTickCount()));
    const int txdSlot = CTxdStore::AddTxdSlot(gExternalSkin.txdName);
    if (txdSlot < 0) {
        SetSkinLoadError("Could not allocate TXD slot");
        return false;
    }
    gExternalSkin.txdSlot = txdSlot;

    // RenderWare resolves material textures while RpClumpStreamRead is running.
    // Load the TXD completely first, make it current, and only then read the DFF.
    if (!CTxdStore::LoadTxd(txdSlot, skin.txdPath.c_str())) {
        SetSkinLoadError("TXD load failed");
        ReleaseExternalSkinAssets(false);
        return false;
    }

    gExternalSkin.modelInfo->ClearTexDictionary();
    gExternalSkin.modelInfo->SetTexDictionary(gExternalSkin.txdName);
    if (gExternalSkin.modelInfo->m_nTxdIndex != txdSlot) {
        SetSkinLoadError("TXD slot binding failed");
        ReleaseExternalSkinAssets(false);
        return false;
    }

    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(txdSlot);
    RpClump* clump = LoadClumpFileSafe(skin.dffPath.c_str());
    const char* validationError = ValidateExternalPedClump(clump);

    bool loaded = false;
    if (!validationError) {
        gExternalSkin.modelInfo->SetClump(clump);
        loaded = gExternalSkin.modelInfo->m_pClump == clump;
    } else if (clump) {
        RpClumpDestroy(clump);
    }
    CTxdStore::PopCurrentTxd();

    if (validationError) {
        SetSkinLoadError(validationError);
        ReleaseExternalSkinAssets(false);
        return false;
    }
    if (!loaded || !gExternalSkin.modelInfo->m_pClump) {
        SetSkinLoadError("VC rejected custom ped clump");
        ReleaseExternalSkinAssets(false);
        return false;
    }

    // This trainer model deliberately stays OUT of CStreaming.  It is a real model-info
    // object allocated from VC's PedModelStore, so model-info shutdown owns its lifetime.
    // Marking a loose DFF as a streamed model without valid CD/list bookkeeping is exactly
    // the sort of half-state that caused the previous special-slot implementation trouble.
    player->SetModelIndex(gExternalSkin.modelId);
    player->UpdateRwFrame();
    const bool switched = player->m_nModelIndex == gExternalSkin.modelId;
    if (switched) {
        CWorld::Players[CWorld::PlayerInFocus].DeletePlayerSkin();
    } else {
        SetSkinLoadError("Player model switch failed");
        ReleaseExternalSkinAssets(false);
    }
    return switched;
}

static std::string NameWithoutExtension(const char* filename) {
    std::string name = filename ? filename : "";
    const std::string::size_type dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name.resize(dot);
    return name;
}

static std::string ModelDisplayName(CBaseModelInfo* info, int modelId) {
    if (info && info->m_szName[0]) {
        size_t length = 0;
        while (length < sizeof(info->m_szName) && info->m_szName[length])
            ++length;
        return std::string(info->m_szName, length);
    }
    char text[32];
    std::snprintf(text, sizeof(text), "Model %d", modelId);
    return text;
}

static bool IsSpecialCharacterPedModel(int modelId) {
    // VC 1.0 CStreaming::RequestSpecialChar(slot, ...) adds 0x6D (109) to the
    // requested slot.  Vice City has 21 special-character ped slots, 109-129.
    // Keep these out of the normal-ped page because their contents are transient
    // mission/cutscene models rather than ordinary ambient ped models.
    return modelId >= kSpecialPedFirst && modelId <= kSpecialPedLast;
}

static void AddRuntimePedSkins() {
    const int baseId = PlayerBaseModelId();
    std::vector<SkinEntry> normal;
    std::vector<SkinEntry> cutscene;

    if (gConfig.includeNormalPeds) {
        SkinEntry player;
        player.kind = SkinKind::DefaultPlayer;
        player.modelId = baseId;
        player.name = "Tommy Vercetti";
        normal.push_back(player);
    }

    for (int modelId = 0; modelId < 6500; ++modelId) {
        if (modelId == baseId || modelId == gExternalSkin.modelId)
            continue;

        CBaseModelInfo* info = CModelInfo::GetModelInfo(modelId);
        if (!info || info->m_nType != MODEL_INFO_PED)
            continue;

        SkinEntry skin;
        skin.kind = SkinKind::PedModel;
        skin.modelId = modelId;
        skin.name = ModelDisplayName(info, modelId);

        if (IsSpecialCharacterPedModel(modelId)) {
            // Special slots are transient.  Only list one while it actually has a
            // resident RW model; otherwise the name/CD metadata may just be stale from
            // an earlier mission and RequestModel() would not mean "load this character".
            if (gConfig.includeCutsceneModels && info->GetRwObject())
                cutscene.push_back(skin);
        } else if (gConfig.includeNormalPeds) {
            normal.push_back(skin);
        }
    }

    // Keep Tommy first, then alphabetize the rest.  This also keeps extra IDE/plugin
    // ped models in Normal Peds as long as they are not using VC's special slots.
    if (normal.size() > 1) {
        std::sort(normal.begin() + 1, normal.end(), [](const SkinEntry& a, const SkinEntry& b) {
            const int byName = _stricmp(a.name.c_str(), b.name.c_str());
            return byName != 0 ? byName < 0 : a.modelId < b.modelId;
        });
    }
    std::sort(cutscene.begin(), cutscene.end(), [](const SkinEntry& a, const SkinEntry& b) {
        const int byName = _stricmp(a.name.c_str(), b.name.c_str());
        return byName != 0 ? byName < 0 : a.modelId < b.modelId;
    });

    gNormalPedSkins.swap(normal);
    gCutsceneSkins.swap(cutscene);
}

static bool HasFileExtension(const char* filename, const char* extension) {
    if (!filename || !extension)
        return false;
    const char* dot = std::strrchr(filename, '.');
    return dot && _stricmp(dot, extension) == 0;
}

static void ScanCustomSkinDirectory(const std::string& directory,
                                    const std::string& relativePrefix,
                                    std::vector<SkinEntry>& entries,
                                    int depth) {
    // Avoid pathological directory junction loops while still allowing organized skin packs.
    if (depth > 16)
        return;

    WIN32_FIND_DATAA data = {};
    HANDLE find = FindFirstFileA(JoinPath(directory, "*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return;

    do {
        const char* name = data.cFileName;
        if (!name || !*name || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
            continue;

        const std::string fullPath = JoinPath(directory, name);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                ScanCustomSkinDirectory(fullPath, relativePrefix + name + "\\", entries, depth + 1);
            }
            continue;
        }

        if (!HasFileExtension(name, ".dff"))
            continue;

        const std::string base = NameWithoutExtension(name);
        if (base.empty())
            continue;

        const std::string txd = JoinPath(directory, base + ".txd");
        if (!FileExists(txd))
            continue;

        SkinEntry skin;
        skin.kind = SkinKind::ExternalDff;
        skin.name = relativePrefix + base;
        skin.dffPath = fullPath;
        skin.txdPath = txd;
        entries.push_back(skin);
    } while (FindNextFileA(find, &data));

    FindClose(find);
}

static void AddCustomDffSkins() {
    gCustomSkins.clear();
    if (!gConfig.includeCustomSkins)
        return;

    const std::string directory = ResolvePluginPath(gConfig.customSkinDirectory);
    EnsureDirectoryTree(directory);

    std::vector<SkinEntry> entries;
    ScanCustomSkinDirectory(directory, std::string(), entries, 0);
    std::sort(entries.begin(), entries.end(), [](const SkinEntry& a, const SkinEntry& b) {
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    gCustomSkins.swap(entries);
}

static std::vector<SkinEntry>& SkinsForCategory(SkinCategory category) {
    switch (category) {
    case SkinCategory::NormalPeds:      return gNormalPedSkins;
    case SkinCategory::CutsceneModels: return gCutsceneSkins;
    case SkinCategory::CustomSkins:    return gCustomSkins;
    default:                           return gNormalPedSkins;
    }
}

static int& SkinSelectionForCategory(SkinCategory category) {
    switch (category) {
    case SkinCategory::NormalPeds:      return g.normalPedSelection;
    case SkinCategory::CutsceneModels: return g.cutsceneSelection;
    case SkinCategory::CustomSkins:    return g.customSkinSelection;
    default:                           return g.normalPedSelection;
    }
}

static const char* SkinCategoryTitle(SkinCategory category) {
    switch (category) {
    case SkinCategory::NormalPeds:      return "NORMAL PEDS";
    case SkinCategory::CutsceneModels: return "CUTSCENE MODELS";
    case SkinCategory::CustomSkins:    return "CUSTOM SKINS";
    default:                           return "SKINS";
    }
}

static void ClampSkinSelections() {
    const SkinCategory categories[] = {
        SkinCategory::NormalPeds,
        SkinCategory::CutsceneModels,
        SkinCategory::CustomSkins
    };
    for (SkinCategory category : categories) {
        std::vector<SkinEntry>& skins = SkinsForCategory(category);
        int& selection = SkinSelectionForCategory(category);
        if (skins.empty()) {
            selection = 0;
        } else {
            selection = std::max(0, std::min(selection, static_cast<int>(skins.size()) - 1));
        }
    }
}

static void RefreshSkinLists() {
    gNormalPedSkins.clear();
    gCutsceneSkins.clear();
    gCustomSkins.clear();
    AddRuntimePedSkins();
    AddCustomDffSkins();
    ClampSkinSelections();
}

static const char* SkinTag(SkinKind kind) {
    switch (kind) {
    case SkinKind::DefaultPlayer: return "DEFAULT";
    case SkinKind::PedModel:      return "PED";
    case SkinKind::ExternalDff:   return "CUSTOM";
    default:                      return "";
    }
}

static void ApplySkin(const SkinEntry& skin) {
    SetSkinLoadError("");
    CPlayerPed* player = FindPlayerPed();
    if (!player) {
        Notify("Player not ready");
        return;
    }

    bool success = false;
    switch (skin.kind) {
    case SkinKind::DefaultPlayer:
        success = SwitchPlayerToModel(PlayerBaseModelId());
        if (success) {
            CPlayerInfo& playerInfo = CWorld::Players[CWorld::PlayerInFocus];
            playerInfo.SetPlayerSkin("");
            success = playerInfo.m_pSkinTexture != nullptr;
            ReleaseExternalSkinAssets(true);
        }
        break;

    case SkinKind::PedModel:
        success = SwitchPlayerToModel(skin.modelId);
        if (success) {
            CWorld::Players[CWorld::PlayerInFocus].DeletePlayerSkin();
            ReleaseExternalSkinAssets(true);
        }
        break;

    case SkinKind::ExternalDff:
        success = LoadExternalSkin(skin);
        break;
    }

    char message[128];
    if (success) {
        std::snprintf(message, sizeof(message), "Skin: %s [%s]", skin.name.c_str(), SkinTag(skin.kind));
    } else if (gSkinLoadError[0] != '\0') {
        std::snprintf(message, sizeof(message), "Skin failed: %s", gSkinLoadError);
    } else {
        std::snprintf(message, sizeof(message), "Skin load failed: %s", skin.name.c_str());
    }
    Notify(message);
}

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
    } else if (appearance == VEHICLE_APPEARANCE_AUTOMOBILE) {
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
    // VC's CREATE_CAR opcode constructs script-owned vehicles with createdBy == 2.
    // CVehicle::CanBeDeleted() explicitly returns false for MISSION_VEHICLE, and the
    // opcode also sets the script-lock bit below before CWorld::Add.
    constexpr unsigned char createdBy = MISSION_VEHICLE;

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

static CVector GetVehicleSpawnCandidate() {
    CEntity* anchor = FindPlayerVehicle() ? static_cast<CEntity*>(FindPlayerVehicle())
                                          : static_cast<CEntity*>(FindPlayerPed());
    if (!anchor)
        return CVector(0.0f, 0.0f, 0.0f);

    const float yaw = HeadingRadians(anchor);
    const CVector forward(-std::sin(yaw), std::cos(yaw), 0.0f);
    CVector pos = anchor->GetPosition();

    // Put the new vehicle in front of the player rather than directly on top of them.
    pos.x += forward.x * 6.0f;
    pos.y += forward.y * 6.0f;
    return pos;
}

static bool ResolveVehicleSpawnPosition(CVehicle* vehicle, CVector& pos) {
    if (!vehicle)
        return false;

    // This is the important part of VC's CREATE_CAR implementation at 0x44A1F3:
    // resolve ground, then add CEntity::GetDistanceFromCentreOfMassToBaseOfModel().
    // That places the collision model's base on the surface instead of putting its
    // matrix origin at ground Z (the cause of the old half-underground spawns).
    bool foundGround = false;
    const float probeZ = pos.z + 30.0f;
    const float groundZ = CWorld::FindGroundZFor3DCoord(
        pos.x, pos.y, probeZ, &foundGround);

    if (!foundGround) {
        // Road-node fallback, using the same path query as VehicleCheat(int).
        const CVector playerPos = FindPlayerCoors();
        const int node = ThePaths.FindNodeClosestToCoors(
            playerPos, 0, 100.0f, false, false, false, false);
        if (node >= 0) {
            const CVector road = ThePaths.nodes[node].GetPosition();
            pos.x = road.x;
            pos.y = road.y;
            pos.z = road.z + vehicle->GetDistanceFromCentreOfMassToBaseOfModel() + 0.20f;
            return true;
        }

        // Last-resort placement: keep it above the player's current Z rather than
        // abandoning a fully constructed pooled vehicle before it reaches CWorld.
        pos.z += vehicle->GetDistanceFromCentreOfMassToBaseOfModel() + 2.0f;
        return true;
    }

    pos.z = groundZ + vehicle->GetDistanceFromCentreOfMassToBaseOfModel() + 0.20f;
    return true;
}

static void SpawnVehicle(const VehicleEntry& entry) {
    // For actual cars, delegate to VC 1.0's own VehicleCheat(int).  This is the
    // executable-authoritative construction/lifetime path and removes every trainer-side
    // allocator/status/autopilot variable from the equation.
    if (entry.kind == VehicleKind::Automobile) {
        // VC 1.0 EN VehicleCheat starts with: 53 56 57 B9 20 B2 94 00.
        // Guard the hardcoded call so a different executable revision falls through to
        // the mission-vehicle implementation instead of jumping into unrelated code.
        static const unsigned char kVehicleCheatSig[8] =
            { 0x53, 0x56, 0x57, 0xB9, 0x20, 0xB2, 0x94, 0x00 };
        if (std::memcmp(reinterpret_cast<const void*>(0x4AE8F0),
                        kVehicleCheatSig, sizeof(kVehicleCheatSig)) == 0) {
            using VehicleCheatFn = void (__cdecl *)(int);

            gCapturedNativeVehicle = nullptr;
            gCaptureNativeVehicleCtor = true;
            reinterpret_cast<VehicleCheatFn>(0x4AE8F0)(entry.model);
            gCaptureNativeVehicleCtor = false;

            // VehicleCheat deliberately creates RANDOM_VEHICLE (1).  That is fine for
            // the stock one-off cheat, but trainer spawns should remain until the user
            // or game explicitly destroys them.  Reclassify the *actual* instance the
            // native routine created and rebalance CCarCtrl's per-createdBy counters so
            // the destructor later decrements the same bucket we increment here.
            if (gCapturedNativeVehicle && gCapturedNativeVehicle->m_nModelIndex == entry.model) {
                CVehicle* vehicle = gCapturedNativeVehicle;
                CCarCtrl::UpdateCarCount(vehicle, true);
                vehicle->m_nCreatedBy = MISSION_VEHICLE;
                vehicle->bIsLocked = true;
                vehicle->b19 = true;
                vehicle->bRemoveFromWorld = false;
                vehicle->m_eDoorLock = DOORLOCK_UNLOCKED;
                vehicle->m_nState = 4; // STATUS_ABANDONED, exactly as VehicleCheat sets it.
                CCarCtrl::UpdateCarCount(vehicle, false);

                char msg[96];
                std::snprintf(msg, sizeof(msg), "Spawned %s [native+locked]", entry.name);
                Notify(msg);
            } else {
                // If this ever appears, the executable's native cheat ran but the ctor hook
                // did not identify its instance.  That is a useful revision/hook diagnostic.
                char msg[96];
                std::snprintf(msg, sizeof(msg), "Spawned %s [native/capture failed]", entry.name);
                Notify(msg);
            }
            return;
        }
    }

    CPlayerPed* player = FindPlayerPed();
    if (!player) {
        Notify("Player not ready");
        return;
    }

    const bool wasAlreadyRequested =
        (CStreaming::ms_aInfoForModel[entry.model].m_nFlags & 1) != 0;

    // Same temporary streaming request used by VC's own VehicleCheat(int).
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

    CVehicle* vehicle = ConstructVehicle(entry);
    if (!vehicle) {
        if (!wasAlreadyRequested) {
            CStreaming::SetModelIsDeletable(entry.model);
            CStreaming::SetModelTxdIsDeletable(entry.model);
        }
        Notify("Vehicle allocation failed");
        return;
    }

    CVector spawnPos = GetVehicleSpawnCandidate();
    ResolveVehicleSpawnPosition(vehicle, spawnPos);

    CEntity* anchor = FindPlayerVehicle() ? static_cast<CEntity*>(FindPlayerVehicle())
                                          : static_cast<CEntity*>(player);

    vehicle->SetPosition(spawnPos.x, spawnPos.y, spawnPos.z);
    vehicle->SetHeading(HeadingRadians(anchor));

    // CREATE_CAR clears the destination volume before the vehicle enters world sectors.
    CTheScripts::ClearSpaceForMissionEntity(spawnPos, vehicle);

    // Mirror VC's CREATE_CAR initialization (0x44A1F3) instead of inventing a
    // trainer-specific lifetime scheme.  These writes are made before CWorld::Add.
    vehicle->m_nState = 4;                 // STATUS_ABANDONED
    vehicle->bIsLocked = true;             // script-owned: normal car cleanup must not remove it
    vehicle->bRemoveFromWorld = false;
    vehicle->m_eDoorLock = DOORLOCK_UNLOCKED;
    vehicle->m_nZoneLevel =
        static_cast<unsigned char>(CTheZones::GetLevelFromPosition(&spawnPos));

    // CREATE_CAR sets the "owned by player" vehicle flag (CVehicle+0x1FB bit 2).
    // Plugin-SDK 10/31/2025 still calls that bit b19.
    vehicle->b19 = true;

    // Match the opcode's neutral autopilot state.  Leaving road/path state half-initialised
    // is a bad idea because the vehicle is processed immediately after CWorld::Add.
    vehicle->m_autoPilot.m_nCarMission = MISSION_NONE;
    vehicle->m_autoPilot.m_nAnimationId = TEMPACT_NONE;
    vehicle->m_autoPilot.m_nCurrentLane = 0;
    vehicle->m_autoPilot.m_nNextLane = 0;
    vehicle->m_autoPilot.m_nDrivingStyle = DRIVINGSTYLE_STOP_FOR_CARS;

    if (entry.kind == VehicleKind::Boat) {
        vehicle->m_autoPilot.m_fMaxTrafficSpeed = 20.0f;
        vehicle->m_autoPilot.m_nCruiseSpeed = 20;
    } else {
        vehicle->m_autoPilot.m_fMaxTrafficSpeed = 9.0f;
        vehicle->m_autoPilot.m_nCruiseSpeed = 9;
    }

    // CREATE_CAR turns the engine off initially.  It starts normally when entered.
    vehicle->bEngineOn = false;
    ZeroVelocity(vehicle);

    if (entry.kind == VehicleKind::Bike) {
        // CREATE_CAR sets CBike+0x484 bit 4 on script-created bikes.
        static_cast<CBike*>(vehicle)->m_nDamageFlags |= 0x10;
    }

    // VC's CREATE_CAR joins road vehicles to the road system before adding them to world sectors.
    if (entry.kind == VehicleKind::Automobile || entry.kind == VehicleKind::Bike)
        CCarCtrl::JoinCarWithRoadSystem(vehicle);

    CWorld::Add(vehicle);

    // Release only our temporary model request. The live entity now owns a model ref,
    // while the vehicle itself remains mission-owned and script-locked.
    if (!wasAlreadyRequested) {
        CStreaming::SetModelIsDeletable(entry.model);
        CStreaming::SetModelTxdIsDeletable(entry.model);
    }

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

    if (IsDown(gConfig.airForwardKey)) move += 1.0f;
    if (IsDown(gConfig.airBackKey)) move -= 1.0f;
    if (IsDown(gConfig.airRightKey)) strafe += 1.0f;
    if (IsDown(gConfig.airLeftKey)) strafe -= 1.0f;
    if (IsDown(gConfig.airUpKey)) vertical += 1.0f;
    if (IsDown(gConfig.airDownKey)) vertical -= 1.0f;

    if (move == 0.0f && strafe == 0.0f && vertical == 0.0f)
        return;

    const float yaw = HeadingRadians(entity);
    const float s = std::sin(yaw);
    const float c = std::cos(yaw);

    // VC's matrix convention: horizontal forward = (-sin(yaw), cos(yaw)).
    const CVector forward(-s, c, 0.0f);
    const CVector right(c, s, 0.0f);

    const float timeStep = std::max(CTimer::ms_fTimeStep, 0.01f);
    const float configuredSpeed = IsDown(gConfig.airFastKey)
        ? gConfig.airbreakFastSpeed : gConfig.airbreakSpeed;
    const float speed = configuredSpeed * timeStep;

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
    case 10: return "Skin Selector";
    case 11: return "Save Position";
    case 12: return "Load Position";
    case 13: return "Airbreak";
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
    case 10: return ">";
    case 12: return g.hasSavedPosition ? "READY" : "EMPTY";
    case 13: return OnOff(g.airbreak);
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
        RefreshSkinLists();
        g.skinCategorySelection = std::max(0, std::min(g.skinCategorySelection, 2));
        g.page = Page::SkinCategories;
        break;
    case 11:
        SavePosition();
        break;
    case 12:
        LoadPosition();
        break;
    case 13:
        ToggleAirbreak();
        break;
    default:
        break;
    }
}

static void ProcessMenuInput() {
    if (Pressed(gConfig.menuKey)) {
        g.menuOpen = !g.menuOpen;
        if (g.menuOpen)
            g.page = Page::Main;
    }

    if (Pressed(gConfig.airbreakKey))
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
        return;
    }

    if (g.page == Page::Vehicles) {
        if (Pressed(VK_UP))
            g.vehicleSelection = (g.vehicleSelection + kVehicleCount - 1) % kVehicleCount;
        if (Pressed(VK_DOWN))
            g.vehicleSelection = (g.vehicleSelection + 1) % kVehicleCount;
        if (Pressed(VK_RETURN) || Pressed(VK_RIGHT))
            SpawnVehicle(kVehicles[g.vehicleSelection]);
        if (Pressed(VK_BACK) || Pressed(VK_LEFT))
            g.page = Page::Main;
        return;
    }

    if (g.page == Page::SkinCategories) {
        constexpr int categoryCount = 4; // three pages + refresh
        if (Pressed(VK_UP))
            g.skinCategorySelection = (g.skinCategorySelection + categoryCount - 1) % categoryCount;
        if (Pressed(VK_DOWN))
            g.skinCategorySelection = (g.skinCategorySelection + 1) % categoryCount;
        if (Pressed(VK_RETURN) || Pressed(VK_RIGHT)) {
            if (g.skinCategorySelection == 3) {
                RefreshSkinLists();
                Notify("Skin lists refreshed");
            } else {
                g.activeSkinCategory = static_cast<SkinCategory>(g.skinCategorySelection);
                g.page = Page::SkinList;
            }
        }
        if (Pressed(VK_BACK) || Pressed(VK_LEFT))
            g.page = Page::Main;
        return;
    }

    if (g.page == Page::SkinList) {
        std::vector<SkinEntry>& skins = SkinsForCategory(g.activeSkinCategory);
        int& selection = SkinSelectionForCategory(g.activeSkinCategory);
        const int skinCount = static_cast<int>(skins.size());
        if (skinCount > 0) {
            if (Pressed(VK_UP))
                selection = (selection + skinCount - 1) % skinCount;
            if (Pressed(VK_DOWN))
                selection = (selection + 1) % skinCount;
            if (Pressed(VK_RETURN) || Pressed(VK_RIGHT))
                ApplySkin(skins[selection]);
        }
        if (Pressed(VK_BACK) || Pressed(VK_LEFT))
            g.page = Page::SkinCategories;
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
    char menuKey[16];
    FormatKeyName(gConfig.menuKey, menuKey, sizeof(menuKey));
    char text[96];
    std::snprintf(text, sizeof(text), "ENTER SELECT   BACK   %s CLOSE", menuKey);
    DrawText(5.0f, top + 3.0f, text, CRGBA(170, 175, 185, 255), 0.165f, 0.34f);
}

static void DrawMainMenu() {
    const float bodyBottom = kRowTop + kRowHeight * kMainCount;
    char airKey[16];
    FormatKeyName(gConfig.airbreakKey, airKey, sizeof(airKey));
    char airHint[32];
    std::snprintf(airHint, sizeof(airHint), "%s AIR", airKey);
    DrawHeader("VICE TRAINER", airHint);
    DrawRect(0.0f, kHeaderHeight, kPanelWidth, bodyBottom, CRGBA(12, 12, 18, 205));

    for (int i = 0; i < kMainCount; ++i) {
        const float y = kRowTop + kRowHeight * i;
        const bool selected = i == g.mainSelection;
        if (selected)
            DrawRect(3.0f, y, kPanelWidth - 3.0f, y + kRowHeight - 1.0f, CRGBA(255, 70, 180, 118));

        DrawText(7.0f, y + 2.0f, MainLabel(i),
                 selected ? CRGBA(255, 255, 255, 255) : CRGBA(210, 215, 220, 255));

        const char* value = MainValue(i);
        if (value)
            DrawTextRight(kPanelWidth - 7.0f, y + 2.0f, value,
                          selected ? CRGBA(255, 255, 255, 255) : CRGBA(175, 215, 220, 255),
                          0.205f, 0.42f);
    }

    DrawFooter(bodyBottom);
}

static void DrawVehicleMenu() {
    constexpr int visibleRows = 12;
    const float bodyBottom = kRowTop + kRowHeight * visibleRows;
    char count[32];
    std::snprintf(count, sizeof(count), "%d/%d", g.vehicleSelection + 1, kVehicleCount);
    DrawHeader("SPAWN VEHICLE", count);
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

    DrawFooter(bodyBottom);
}


static void DrawSkinCategories() {
    constexpr int rowCount = 4;
    const float bodyBottom = kRowTop + kRowHeight * rowCount;
    DrawHeader("SKIN SELECTOR", "PAGES");
    DrawRect(0.0f, kHeaderHeight, kPanelWidth, bodyBottom, CRGBA(12, 12, 18, 205));

    const char* labels[rowCount] = {
        "Normal Peds",
        "Cutscene Models",
        "Custom Skins",
        "Refresh Lists"
    };
    const int counts[rowCount] = {
        static_cast<int>(gNormalPedSkins.size()),
        static_cast<int>(gCutsceneSkins.size()),
        static_cast<int>(gCustomSkins.size()),
        -1
    };

    for (int i = 0; i < rowCount; ++i) {
        const float y = kRowTop + kRowHeight * i;
        const bool selected = i == g.skinCategorySelection;
        if (selected)
            DrawRect(3.0f, y, kPanelWidth - 3.0f, y + kRowHeight - 1.0f, CRGBA(50, 220, 235, 115));

        DrawText(7.0f, y + 2.0f, labels[i],
                 selected ? CRGBA(255, 255, 255, 255) : CRGBA(210, 215, 220, 255),
                 0.215f, 0.44f);
        if (counts[i] >= 0) {
            char count[24];
            std::snprintf(count, sizeof(count), "%d", counts[i]);
            DrawTextRight(kPanelWidth - 7.0f, y + 2.0f, count,
                          selected ? CRGBA(255, 255, 255, 255) : CRGBA(130, 185, 195, 255),
                          0.17f, 0.35f);
        } else {
            DrawTextRight(kPanelWidth - 7.0f, y + 2.0f, ">",
                          selected ? CRGBA(255, 255, 255, 255) : CRGBA(130, 185, 195, 255),
                          0.18f, 0.37f);
        }
    }

    DrawFooter(bodyBottom);
}

static int SkinListVisibleRows() {
    const int count = static_cast<int>(SkinsForCategory(g.activeSkinCategory).size());
    return std::max(1, std::min(12, count));
}

static void DrawSkinList() {
    std::vector<SkinEntry>& skins = SkinsForCategory(g.activeSkinCategory);
    int& selection = SkinSelectionForCategory(g.activeSkinCategory);
    const int skinCount = static_cast<int>(skins.size());
    const int visibleRows = SkinListVisibleRows();
    const float bodyBottom = kRowTop + kRowHeight * visibleRows;

    char count[32];
    if (skinCount > 0)
        std::snprintf(count, sizeof(count), "%d/%d", selection + 1, skinCount);
    else
        std::snprintf(count, sizeof(count), "EMPTY");
    DrawHeader(SkinCategoryTitle(g.activeSkinCategory), count);
    DrawRect(0.0f, kHeaderHeight, kPanelWidth, bodyBottom, CRGBA(12, 12, 18, 205));

    if (skinCount <= 0) {
        DrawText(7.0f, kRowTop + 2.0f,
                 g.activeSkinCategory == SkinCategory::CustomSkins ? "No DFF + TXD pairs found" : "No models available",
                 CRGBA(170, 175, 185, 255), 0.19f, 0.39f);
        DrawFooter(bodyBottom);
        return;
    }

    selection = std::max(0, std::min(selection, skinCount - 1));
    int first = selection - visibleRows / 2;
    first = std::max(0, std::min(first, std::max(0, skinCount - visibleRows)));

    for (int row = 0; row < visibleRows; ++row) {
        const int index = first + row;
        if (index >= skinCount)
            break;
        const float y = kRowTop + kRowHeight * row;
        const bool selected = index == selection;
        if (selected)
            DrawRect(3.0f, y, kPanelWidth - 3.0f, y + kRowHeight - 1.0f, CRGBA(50, 220, 235, 115));

        DrawText(7.0f, y + 2.0f, skins[index].name.c_str(),
                 selected ? CRGBA(255, 255, 255, 255) : CRGBA(210, 215, 220, 255),
                 0.205f, 0.42f);
    }

    DrawFooter(bodyBottom);
}

static float CurrentPanelBottom() {
    if (!g.menuOpen)
        return 0.0f;
    if (g.page == Page::Main)
        return kRowTop + kRowHeight * kMainCount + kFooterHeight;
    if (g.page == Page::Vehicles)
        return kRowTop + kRowHeight * 12.0f + kFooterHeight;
    if (g.page == Page::SkinCategories)
        return kRowTop + kRowHeight * 4.0f + kFooterHeight;
    return kRowTop + kRowHeight * static_cast<float>(SkinListVisibleRows()) + kFooterHeight;
}

static float DrawAirbreakControls(float y) {
    char airKey[16];
    FormatKeyName(gConfig.airbreakKey, airKey, sizeof(airKey));

    if (!gConfig.showAirbreakControls) {
        DrawRect(0.0f, y, 92.0f, y + 12.0f, CRGBA(8, 8, 12, 205));
        char line[48];
        std::snprintf(line, sizeof(line), "AIRBREAK ON [%s]", airKey);
        DrawText(4.0f, y + 2.0f, line, CRGBA(50, 220, 235, 255), 0.17f, 0.35f);
        return y + 13.0f;
    }

    char forwardKey[16], backKey[16], leftKey[16], rightKey[16];
    char upKey[16], downKey[16], fastKey[16];
    FormatKeyName(gConfig.airForwardKey, forwardKey, sizeof(forwardKey));
    FormatKeyName(gConfig.airBackKey, backKey, sizeof(backKey));
    FormatKeyName(gConfig.airLeftKey, leftKey, sizeof(leftKey));
    FormatKeyName(gConfig.airRightKey, rightKey, sizeof(rightKey));
    FormatKeyName(gConfig.airUpKey, upKey, sizeof(upKey));
    FormatKeyName(gConfig.airDownKey, downKey, sizeof(downKey));
    FormatKeyName(gConfig.airFastKey, fastKey, sizeof(fastKey));

    DrawRect(0.0f, y, kPanelWidth, y + 34.0f, CRGBA(8, 8, 12, 215));
    char line1[64], line2[96], line3[96];
    std::snprintf(line1, sizeof(line1), "AIRBREAK ON   %s TOGGLE", airKey);
    std::snprintf(line2, sizeof(line2), "%s/%s FWD-BACK   %s/%s STRAFE",
                  forwardKey, backKey, leftKey, rightKey);
    std::snprintf(line3, sizeof(line3), "%s/%s UP-DN   %s FAST", upKey, downKey, fastKey);
    DrawText(5.0f, y + 2.0f, line1, CRGBA(50, 220, 235, 255), 0.165f, 0.34f);
    DrawText(5.0f, y + 12.0f, line2, CRGBA(220, 225, 230, 255), 0.15f, 0.31f);
    DrawText(5.0f, y + 22.0f, line3, CRGBA(220, 225, 230, 255), 0.14f, 0.29f);
    return y + 35.0f;
}

static void DrawStatusOverlay() {
    float y = g.menuOpen ? CurrentPanelBottom() + 1.0f : 0.0f;

    if (g.airbreak)
        y = DrawAirbreakControls(y);

    if (g.notification[0] != '\0' && GetTickCount() < g.notificationUntil) {
        DrawRect(0.0f, y, kPanelWidth, y + 13.0f, CRGBA(8, 8, 12, 215));
        DrawText(5.0f, y + 3.0f, g.notification, CRGBA(255, 255, 255, 255), 0.185f, 0.38f);
    }
}

static void Draw() {
    if (g.menuOpen) {
        if (g.page == Page::Main)
            DrawMainMenu();
        else if (g.page == Page::Vehicles)
            DrawVehicleMenu();
        else if (g.page == Page::SkinCategories)
            DrawSkinCategories();
        else
            DrawSkinList();
    }
    DrawStatusOverlay();
}

static void ResetRuntimeState() {
    // A restart keeps RenderWare/model stores alive, so release the trainer-owned clump/TXD
    // here instead of merely forgetting their pointers.  The old build leaked ownership on restart.
    ReleaseExternalSkinAssets(true);

    g.menuOpen = false;
    g.airbreak = false;
    g.godMode = false;
    g.vehicleGodMode = false;
    g.unlimitedAmmo = false;
    g.noReload = false;
    g.neverWanted = false;
    g.page = Page::Main;
    g.skinCategorySelection = 0;
    g.normalPedSelection = 0;
    g.cutsceneSelection = 0;
    g.customSkinSelection = 0;
    g.activeSkinCategory = SkinCategory::NormalPeds;
    gNormalPedSkins.clear();
    gCutsceneSkins.clear();
    gCustomSkins.clear();
    gCaptureNativeVehicleCtor = false;
    gCapturedNativeVehicle = nullptr;
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
        InitialiseConfig();
        Events::gameProcessEvent += [] { Process(); };
        Events::drawHudEvent += [] { Draw(); };

        Events::vehicleCtorEvent += [](CVehicle* vehicle) {
            if (gCaptureNativeVehicleCtor)
                gCapturedNativeVehicle = vehicle;
        };
        Events::vehicleDtorEvent += [](CVehicle* vehicle) {
            if (gVehicleProof.valid && gVehicleProof.entity == vehicle)
                gVehicleProof = VehicleProofSnapshot();
            if (gCapturedNativeVehicle == vehicle)
                gCapturedNativeVehicle = nullptr;
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

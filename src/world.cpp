#include "world.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>
#include "gd_names.h"
#include "exe_ui.h"
#include "gameapi.h"
#include "hooks.h"
#include "speech.h"
#include "audio.h"
#include "core/message_builder.h"
#include "core/screen_clip.h"
#include "core/strings.h"
#include "log.h"
#include "msvc_string.h"
#include "settings.h"
#include "textcap.h"

namespace gd::world {
using namespace gd::names;
namespace {
// ---- instances captured from per-frame members ----
void* g_game_engine = nullptr;
void* g_controller = nullptr;
void* g_world = nullptr;
uint64_t g_engine_ticks = 0, g_controller_ticks = 0;

std::string caller_module(void* ret) {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m);
  if (!m) return "?";
  char path[MAX_PATH]; GetModuleFileNameA(m, path, MAX_PATH);
  const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
  return std::format("{}+{:#x}", base, (uintptr_t)ret - (uintptr_t)m);
}

typedef void (*Update_t)(void*, int);
Update_t GameEngineUpdate_orig, ControllerPlayerUpdate_orig;
void GameEngineUpdate(void* self, int dt) { g_game_engine = self; ++g_engine_ticks; GameEngineUpdate_orig(self, dt); }
void ControllerPlayerUpdate(void* self, int dt) { g_controller = self; ++g_controller_ticks; ControllerPlayerUpdate_orig(self, dt); }
typedef void (*WorldUpdate_t)(void*, const void*, const void*);
WorldUpdate_t WorldUpdate_orig;
void WorldUpdate(void* self, const void* spheres, const void* frusta) { g_world = self; WorldUpdate_orig(self, spheres, frusta); }

// ---- targeting instrumentation: who sets the combat enemy / sends actions, and with what ----
typedef void (*SetId_t)(void*, unsigned);
SetId_t SetCombatEnemy_hook_orig, FaceTarget_hook_orig;
typedef void (*ClearTarget_t)(void*);
ClearTarget_t ClearTarget_hook_orig;
typedef bool (*HandleJoystick_t)(void*, const void*, bool);
HandleJoystick_t HandleActionFromJoystick_hook_orig;
typedef bool (*HandleMouse_t)(void*, bool, bool, bool, bool, const void*, unsigned*, bool*);
HandleMouse_t HandleActionFromMouse_hook_orig;
unsigned g_last_enemy_logged = ~0u;
uint64_t g_c_joystick = 0, g_c_mouse = 0;
void SetCombatEnemy_hook(void* self, unsigned id) {
  if (id != g_last_enemy_logged) { g_last_enemy_logged = id; log::writef("target: SetCombatEnemy({}) <- {}", id, caller_module(_ReturnAddress())); }
  SetCombatEnemy_hook_orig(self, id);
}
void FaceTarget_hook(void* self, unsigned id) { log::writef("target: FaceTarget({}) <- {}", id, caller_module(_ReturnAddress())); FaceTarget_hook_orig(self, id); }
void ClearTarget_hook(void* self) {
  if (g_last_enemy_logged != 0) { g_last_enemy_logged = 0; log::writef("target: ClearTarget <- {}", caller_module(_ReturnAddress())); }
  ClearTarget_hook_orig(self);
}
std::string wv_text(const void* wv);
bool HandleActionFromJoystick_hook(void* self, const void* wv, bool b) {
  bool r = HandleActionFromJoystick_hook_orig(self, wv, b);
  if (++g_c_joystick % 60 == 1 || b) log::writef("target: HandleActionFromJoystick({}, {}) -> {} <- {} [#{}]", wv_text(wv), b, r, caller_module(_ReturnAddress()), g_c_joystick);
  return r;
}
bool HandleActionFromMouse_hook(void* self, bool a, bool b, bool c, bool d, const void* wv, unsigned* id, bool* flag) {
  unsigned before = id ? *id : 0;
  bool r = HandleActionFromMouse_hook_orig(self, a, b, c, d, wv, id, flag);
  if (++g_c_mouse % 60 == 1 || a || b)
    log::writef("target: HandleActionFromMouse({},{},{},{}, {}, id {}->{}, flag {}) -> {} <- {} [#{}]", a, b, c, d, wv_text(wv), before, id ? *id : 0,
                flag ? (int)*flag : -1, r, caller_module(_ReturnAddress()), g_c_mouse);
  return r;
}

struct alignas(16) Buf { unsigned char b[256]; };
struct MemVec { void* begin; void* end; void* cap; };  // mem::vector<T>: std-like {begin, end, cap} (read from Engine.dll)

// ---- conversation capture ----
// The UI fetches each displayed step's text through Conversation::GetText while the dialog is up, and a
// step's children through Conversation::GetSteps (private, but exported). Both are hooked; the per-frame
// fetches are the structured picture of the dialog.
std::mutex g_conv_mu;
ConvState g_conv_cur, g_conv_last;   // cur = being filled this frame; last = the latest complete frame
uint64_t g_conv_cur_frame = ~0ull;
typedef void (*ConvGetText_t)(void*, const void*, int, MsvcStringW*);
ConvGetText_t ConvGetText_hook_orig;
typedef void (*ConvGetSteps_t)(void*, void*, void*);
ConvGetSteps_t ConvGetSteps_hook_orig;
struct ConvApi {
  int (*GetType)(const void*) = nullptr;
  MsvcStringA* (*GetTypeTag)(const void*, MsvcStringA*) = nullptr;  // std::string by value -> hidden pointer
  bool (*IsAvailable)(const void*) = nullptr;
  bool (*IsUsed)(const void*) = nullptr;
  void* (*GetParent)(const void*) = nullptr;
} g_conv_api;
void conv_frame_begin(void* conv) {
  // Called under g_conv_mu. A new frame (per our tick counter) rotates cur -> last.
  uint64_t f = gd::hooks::frame();
  if (f != g_conv_cur_frame) {
    if (g_conv_cur_frame != ~0ull) g_conv_last = g_conv_cur;
    g_conv_cur = ConvState{}; g_conv_cur.frame = f; g_conv_cur_frame = f;
  }
  g_conv_cur.conversation = conv;
}
// Plain-data step readout under an SEH guard (no C++ objects with destructors may live in a __try frame).
struct StepRaw { int type; bool available, used; void* parent; char tag[64]; };
void read_step_raw(const void* step, StepRaw& r) {
  __try {
    if (g_conv_api.GetType) r.type = g_conv_api.GetType(step);
    if (g_conv_api.IsAvailable) r.available = g_conv_api.IsAvailable(step);
    if (g_conv_api.IsUsed) r.used = g_conv_api.IsUsed(step);
    if (g_conv_api.GetParent) r.parent = g_conv_api.GetParent(step);
    if (g_conv_api.GetTypeTag) {
      alignas(16) unsigned char sb[64] = {}; MsvcStringA* t = (MsvcStringA*)sb; t->capacity = 15;
      if (g_conv_api.GetTypeTag(step, t)) {
        std::string_view v = t->view();
        size_t n = v.size() < sizeof r.tag - 1 ? v.size() : sizeof r.tag - 1;
        memcpy(r.tag, v.data(), n); r.tag[n] = 0;
        if (t->capacity > 15 && t->u.ptr) free(t->u.ptr);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void ConvGetText_hook(void* self, const void* step, int gender, MsvcStringW* out) {
  ConvGetText_hook_orig(self, step, gender, out);
  StepRaw raw{-1, false, false, nullptr, {}};
  read_step_raw(step, raw);
  ConvStep s{(void*)step, raw.type, raw.tag, raw.available, raw.used, out ? textcap::speakable(out->view()) : std::string(), raw.parent};
  std::lock_guard lk(g_conv_mu);
  conv_frame_begin(self);
  for (ConvStep& e : g_conv_cur.steps) if (e.step == step) { e = s; return; }
  g_conv_cur.steps.push_back(s);
}
void ConvGetSteps_hook(void* self, void* parent, void* out_vec) {
  ConvGetSteps_hook_orig(self, parent, out_vec);
  std::lock_guard lk(g_conv_mu);
  conv_frame_begin(self);
  MemVec* v = (MemVec*)out_vec;
  g_conv_cur.children.clear(); g_conv_cur.children_of = parent;
  if (v && v->begin && v->end && (uintptr_t)v->end > (uintptr_t)v->begin && (uintptr_t)v->end - (uintptr_t)v->begin < (1u << 16)) {
    size_t n = (size_t)((char*)v->end - (char*)v->begin) / sizeof(void*);
    for (size_t i = 0; i < n; ++i) { void* c; memcpy(&c, (char*)v->begin + i * sizeof(void*), sizeof c); g_conv_cur.children.push_back(c); }
  }
}

std::vector<gd::hooks::Hook> g_hooks;

// ---- exports called directly (resolved once) ----
// Class-by-value returns (WorldCoords, Coords, Vec3) use the hidden return pointer as the 2nd argument.
struct Api {
  void* (*GetMainPlayer)(const void*) = nullptr;
  const MemVec* (*GetMarkerUIDs)(const void*) = nullptr;   // Player::GetMarkerUIDs -> mem::vector<UniqueId> (16 bytes each)
  void* (*GetCamera)(void*) = nullptr;
  void* (*Entity_GetCoords)(const void*, void*) = nullptr;
  void* (*Entity_GetRegion)(const void*) = nullptr;
  void* (*Character_GetFootCoords)(void*, void*, bool) = nullptr;
  double (*GetCurrentLife)(const void*) = nullptr;
  float (*GetLifeLimit)(const void*) = nullptr;
  unsigned (*GetCharLevel)(const void*) = nullptr;             // Character::GetCharLevel (the nameplate level)
  const int* (*GetClassification)(const void*) = nullptr;      // Monster::GetClassification -> enum const& (0 Common..5 SuperBoss)
  unsigned (*FindSkillId)(const void*, const char*) = nullptr; // SkillManager::FindSkillId(record path) -> live skill id (names a buff)
  float (*GetCurrentMana)(const void*) = nullptr;   // "energy" in the UI; float (GetCurrentLife is a double)
  float (*GetManaLimit)(const void*) = nullptr;
  const char16_t* (*GetPlayerName)(const void*) = nullptr;
  const MsvcString<char>* (*Region_GetName)(const void*) = nullptr;
  void* (*NavManager_Get)() = nullptr;
  bool (*IsPointOnPathMesh)(void*, const void*) = nullptr;
  int (*FindStraightMovePoint)(void*, const void*, const void*, void*) = nullptr;
  int (*FindClosestPointOnPathMesh)(void*, const void*, void*, float) = nullptr;
  // GAME::Player::FindPath(WorldVec3 const& dest, float, WorldVec3& out, float) const -> PathResult (enum int).
  // Member ABI maps to: self=rcx, dest=rdx, f1=xmm2, out=r9, f2=stack -- a plain fn ptr marshals it (dev only).
  int (*Player_FindPath)(void*, const void*, float, void*, float) = nullptr;
  // GAME::NavManager::FindPath(WorldVec3 const& from, WorldVec3 const& to, float radius, WorldVec3* p5,
  // uint* p6, float* p7, mem::vector<WorldVec3>* corridor, Vec3* p9, bool p10). All four scalar out-params are
  // null-checked in the body (Engine+0x10c2c0, read 2026-08-24); we pass only the corridor (the straight path).
  bool (*NavManager_FindPath)(void*, const void*, const void*, float, void*, void*, void*, void*, void*, bool) = nullptr;
  void* (*WorldVec3_ctor)(void*, void*, const void*) = nullptr;
  void* (*WorldVec3_GetWorldPosition)(const void*, void*) = nullptr;
  void* (*WorldVec3_GetRegion)(const void*) = nullptr;
  const Vec3* (*WorldVec3_GetRegionPosition)(const void*) = nullptr;
  bool (*WorldVec3_PutOnFloor)(void*) = nullptr;
  void* (*WorldCoords_GetRegionCoords)(const void*, void*) = nullptr;
  float (*GetCameraYaw)(const void*) = nullptr;
  // targeting / entities
  bool (*GetEntitiesInPriorFrameFrustum)(void*, void*) = nullptr;
  void (*Region_GetEntitiesInSphere)(void*, void*, const void*, bool, int) = nullptr;
  unsigned (*Object_GetObjectId)(const void*) = nullptr;
  const float* (*Entity_GetRegionBoundingBox)(const void*, bool) = nullptr;  // const ABBox&: {min, max} Vec3s (verified via /blocks dump)
  const char* (*Object_GetObjectName)(const void*) = nullptr;  // `char const*` per the export (not a std::string)
  const char* (*Resource_GetFileName)(const void*) = nullptr;  // Resource::GetFileName -- a map icon's custom symbol texture path
  const void* (*Object_GetRTTIClassInfo)(const void*) = nullptr;
  const void* (*Entity_StaticClassInfo)() = nullptr;
  const void* (*Character_StaticClassInfo)() = nullptr;
  const void* (*Monster_StaticClassInfo)() = nullptr;
  const void* (*Npc_StaticClassInfo)() = nullptr;
  const void* (*Player_StaticClassInfo)() = nullptr;
  int (*SkillActivated_GetTargetType)(const void*) = nullptr;    // SkillTargetType {Default=0,Point=1,Object=2,Target=3}; reads this+0x5c0, never overridden
  const void* (*SkillActivated_StaticClassInfo)() = nullptr;     // is-a guard: passives/modifiers are Skill but not SkillActivated
  void (*GetCurrentAttackTarget)(void*, unsigned*, void*, unsigned*) = nullptr;
  void* (*GetFactionManager)(void*) = nullptr;
  bool (*FactionManager_IsFoe)(void*, unsigned, unsigned, bool) = nullptr;  // by object ids
  void (*ItemAction)(void*, bool, bool, const void*, const void*) = nullptr;   // ControllerPlayer: (no_walk, unused, WorldVec3 const&, Item const*)
  void (*InteractAction)(void*, bool, bool, const void*, const void*) = nullptr;   // ControllerPlayer: same shape, FixedActor const* (doors, ladders, chests, shrines)
  void (*SetCommandRepeated)(void*, bool) = nullptr;                           // ControllerPlayer +0x430; ItemAction no-ops while it is set
  bool (*Character_IsAlive)(const void*) = nullptr;   // virtual; the base body is the Monster implementation (only Player overrides)
  const MsvcStringA* (*Engine_GetAreaNameTag)(const void*) = nullptr;   // std::string const& (a localization tag)
  // labels: basic_string<unsigned short> by value -> hidden return pointer (2nd arg)
  MsvcStringW* (*Monster_GetGameDescription)(const void*, MsvcStringW*, bool, bool) = nullptr;
  MsvcStringW* (*Npc_GetRolloverDescription)(const void*, MsvcStringW*) = nullptr;
  MsvcStringW* (*Player_GetRolloverDescription)(const void*, MsvcStringW*) = nullptr;
  MsvcStringW* (*Item_GetGameDescription)(const void*, MsvcStringW*, bool, bool) = nullptr;
  void (*SetCombatEnemy)(void*, unsigned) = nullptr;
  unsigned (*GetCombatEnemy)(const void*) = nullptr;
  void (*ClearTarget)(void*) = nullptr;
  void (*FaceTarget)(void*, unsigned) = nullptr;
  void** Object_vftable = nullptr;                       // to find GetRTTIClassInfo's slot (virtual dispatch)
  // Review classification = the Interact key's own filter (docs: re_interact_key.md): FixedActor/Item that
  // say IsOfInterest() (virtual; slot found in each class's vftable), Npc with a conversation.
  const void* (*FixedActor_StaticClassInfo)() = nullptr;
  const void* (*Actor_StaticClassInfo)() = nullptr;   // Actor declares the virtual GetGameDescription every label comes from
  const void* (*ItemNote_StaticClassInfo)() = nullptr;   // lore notes: J must stay a click for them (the toast + codex entry come from the exe click path)
  const void* (*Item_StaticClassInfo)() = nullptr;
  bool (*FixedActor_IsOfInterest)(const void*) = nullptr;
  bool (*Item_IsOfInterest)(const void*) = nullptr;
  void** FixedActor_vftable = nullptr;
  void** Monster_vftable = nullptr;       // to find the GetGameDescription slot (same slot for every Actor)
  void** Item_vftable = nullptr;
  bool (*Npc_HasConversation)(const void*) = nullptr;
  const void* (*Destructible_StaticClassInfo)() = nullptr;   // breakables (barrels, crates, quest targets): the B group
  bool (*Destructible_IsBroken)(const void*) = nullptr;
  const void* (*StaticShrine_StaticClassInfo)() = nullptr;   // devotion shrines: the sonar tells ruined from restored
  bool (*StaticShrine_IsCleansed)(const void*) = nullptr;
  bool (*Destructible_IsTargetable)(const void*) = nullptr;
  void* (*Project)(const void*, void*, const void*, const void*) = nullptr;  // WorldCamera::Project: hidden Vec2 return
  void* (*GetRayThroughImagePoint)(const void*, void*, const void*, const void*) = nullptr;  // WorldCamera: hidden WorldRay return {Region*, Vec3 origin, pad, Vec3 dir}
  void (*World_GetIntersection)(const void*, const void*, void*, int, bool, float, bool) = nullptr;  // (ray, WorldIntersection& out, PhysicsSurface, skip_entities, max_dist, bool)
  void (*World_GetAllIntersections)(const void*, const void*, void*, bool, float) = nullptr;   // (ray, mem::vector<Entity*>&, bool, max_dist)
  void (*SetZoom)(void*, float) = nullptr;               // GameCamera::SetZoom(value in the camera's zoom range)
  void (*ResetZoom)(void*) = nullptr;
  void (*SetCameraYaw)(void*, float) = nullptr;          // GameCamera::SetCameraYaw (radians)
  void* (*Viewport_ctor)(void*, int, int, int, int) = nullptr;
  // world structure (chunks and portals; dev dumps for docs/rooms.md)
  int (*World_GetNumRegions)(const void*) = nullptr;
  void* (*World_GetRegion)(void*, int) = nullptr;
  const int* (*Region_GetOffsetFromWorld)(const void*) = nullptr;   // const IntVec3&
  bool (*Region_IsUnderground)(const void*) = nullptr;
  bool (*Region_IsLevelLoaded)(const void*) = nullptr;
  void (*Region_BackgroundLoadLevel)(void*, bool) = nullptr;   // request the level stream (guarded/idempotent); off-map dungeons never load by proximity
  bool (*Region_IsLoadingFinished)(const void*) = nullptr;     // level attached and the loading byte (+0x90) clear
  void* (*Engine_GetResourceLoader)(void*) = nullptr;          // the engine's async asset loader (textures, meshes, effects)
  bool (*ResourceLoader_IsIdle)(void*) = nullptr;              // zero-timeout wait on its work event: nothing queued
  bool (*IsGameTimePaused)() = nullptr;
  void (*Character_StopMoving)(void*, bool, bool) = nullptr;
  void (*PauseGameTime)() = nullptr;
  void (*UnpauseGameTime)() = nullptr;
  int (*Region_GetNumPortals)(const void*) = nullptr;
  void* (*Region_GetPortal)(const void*, int) = nullptr;
  int (*Region_GetWorldIndex)(const void*) = nullptr;
  const float* (*Region_GetBoundingBox)(const void*) = nullptr;      // const ABBox&: min Vec3, max Vec3
  void* (*Portal_GetConnectedRegion)(const void*) = nullptr;
  void* (*Portal_GetChokePoint)(const void*, void*) = nullptr;       // WorldCoords by value: hidden pointer
  bool (*Portal_GetIsOpen)(const void*) = nullptr;
  // authoring dev routes
  void* (*World_GetRegionContainingXZ)(const void*, void*, float, float) = nullptr;
  void (*Entity_SetCoords)(void*, const void*) = nullptr;                 // protected, exported -- a raw field write, see teleport()
  void (*Character_TeleportToLocation)(void*, const void*) = nullptr;     // the game's own teleport (Game.dll): World::SetCoords + nav reset
  void* (*Region_GetFogOfWar)(void*, bool) = nullptr;
  void (*FogOfWar_AddVisibility)(void*, const Vec3*, int) = nullptr;
  bool (*FogOfWar_IsInFog)(const void*, const Vec3*) = nullptr;
  // Painted damage sectors (docs/hazards.md): the exact chain TickManager::Tick uses once a second.
  void* (*Level_GetSectorLayers)(void*) = nullptr;
  void* (*SectorLayers_GetTargetId)(void*, void*, int, int, int) = nullptr;   // UniqueId by hidden pointer; (layer, x, z) region-relative ints
  void* (*SectorDataManager_GetSectorData)(const void*, unsigned, const void*) = nullptr;   // (layer, UniqueId const&) -> SectorData*
  void** gEngine = nullptr;   // GAME::gEngine (Engine.dll data): the SectorDataManager is embedded at Engine+8
  bool loaded = false;
} g_api;

template <class F> F fn(const char* dll, const char* name) {
  HMODULE m = GetModuleHandleA(dll);
  return m ? (F)GetProcAddress(m, name) : nullptr;
}
void load_api() {
  if (g_api.loaded) return;
  g_api.loaded = true;
#define LOAD(field, ID) \
  g_api.field = fn<decltype(g_api.field)>(ID##_DLL, ID); \
  if (!g_api.field) log::writef("world: export {} not found", #ID)
  LOAD(GetMainPlayer, GameEngine_GetMainPlayer);
  LOAD(GetMarkerUIDs, Player_GetMarkerUIDs);
  LOAD(GetCamera, GameEngine_GetCamera);
  LOAD(Entity_GetCoords, Entity_GetCoords);
  LOAD(Entity_GetRegion, Entity_GetRegion);
  LOAD(Character_GetFootCoords, Character_GetFootCoords);
  LOAD(GetCurrentLife, Character_GetCurrentLife);
  LOAD(GetLifeLimit, Character_GetLifeLimit);
  LOAD(GetCharLevel, Character_GetCharLevel);
  LOAD(GetClassification, Monster_GetClassification);
  LOAD(FindSkillId, SkillManager_FindSkillId);
  LOAD(GetCurrentMana, Character_GetCurrentMana);
  LOAD(GetManaLimit, Character_GetManaLimit);
  LOAD(GetPlayerName, Player_GetPlayerName);
  LOAD(Region_GetName, Region_GetName);
  LOAD(NavManager_Get, NavManager_Get);
  LOAD(IsPointOnPathMesh, NavManager_IsPointOnPathMesh);
  LOAD(FindStraightMovePoint, NavManager_FindStraightMovePoint);
  LOAD(FindClosestPointOnPathMesh, NavManager_FindClosestPointOnPathMesh);
  LOAD(NavManager_FindPath, NavManager_FindPath);
  LOAD(WorldVec3_ctor, WorldVec3_ctor);
  LOAD(WorldVec3_GetWorldPosition, WorldVec3_GetWorldPosition);
  LOAD(WorldVec3_GetRegion, WorldVec3_GetRegion);
  LOAD(WorldVec3_GetRegionPosition, WorldVec3_GetRegionPosition);
  LOAD(WorldVec3_PutOnFloor, WorldVec3_PutOnFloor);
  LOAD(Player_FindPath, Player_FindPath);
  LOAD(WorldCoords_GetRegionCoords, WorldCoords_GetRegionCoords);
  LOAD(GetCameraYaw, WorldCamera_GetCameraYaw);
  LOAD(GetEntitiesInPriorFrameFrustum, Engine_GetEntitiesInPriorFrameFrustum);
  LOAD(Region_GetEntitiesInSphere, Region_GetEntitiesInSphere);
  LOAD(Object_GetObjectId, Object_GetObjectId);
  LOAD(Entity_GetRegionBoundingBox, Entity_GetRegionBoundingBox);
  LOAD(Object_GetObjectName, Object_GetObjectName);
  LOAD(Resource_GetFileName, Resource_GetFileName);
  LOAD(Object_GetRTTIClassInfo, Object_GetRTTIClassInfo);
  LOAD(Entity_StaticClassInfo, Entity_GetStaticClassInfo);
  LOAD(Character_StaticClassInfo, Character_GetStaticClassInfo);
  LOAD(Monster_StaticClassInfo, Monster_GetStaticClassInfo);
  LOAD(Npc_StaticClassInfo, Npc_GetStaticClassInfo);
  LOAD(Player_StaticClassInfo, Player_GetStaticClassInfo);
  LOAD(SkillActivated_GetTargetType, SkillActivated_GetTargetType);
  LOAD(SkillActivated_StaticClassInfo, SkillActivated_GetStaticClassInfo);
  LOAD(GetCurrentAttackTarget, Character_GetCurrentAttackTarget);
  LOAD(GetFactionManager, GameEngine_GetFactionManager);
  LOAD(FactionManager_IsFoe, FactionManager_IsFoe);
  LOAD(ItemAction, ControllerPlayer_ItemAction);
  LOAD(InteractAction, ControllerPlayer_InteractAction);
  LOAD(SetCommandRepeated, ControllerPlayer_SetCommandRepeated);
  LOAD(Character_IsAlive, Character_IsAlive);
  LOAD(Engine_GetAreaNameTag, Engine_GetAreaNameTag);
  LOAD(Monster_GetGameDescription, Monster_GetGameDescription);
  LOAD(Npc_GetRolloverDescription, Npc_GetRolloverDescription);
  LOAD(Player_GetRolloverDescription, Player_GetRolloverDescription);
  LOAD(Item_GetGameDescription, Item_GetGameDescription);
  LOAD(SetCombatEnemy, ControllerPlayer_SetCombatEnemy);
  LOAD(GetCombatEnemy, ControllerPlayer_GetCombatEnemy);
  LOAD(ClearTarget, ControllerPlayer_ClearTarget);
  LOAD(FaceTarget, ControllerPlayer_FaceTarget);
  LOAD(Object_vftable, Object_vftable);
  LOAD(FixedActor_StaticClassInfo, FixedActor_GetStaticClassInfo);
  LOAD(Actor_StaticClassInfo, Actor_GetStaticClassInfo);
  LOAD(ItemNote_StaticClassInfo, ItemNote_GetStaticClassInfo);
  LOAD(Item_StaticClassInfo, Item_GetStaticClassInfo);
  LOAD(FixedActor_IsOfInterest, FixedActor_IsOfInterest);
  LOAD(Item_IsOfInterest, Item_IsOfInterest);
  LOAD(FixedActor_vftable, FixedActor_vftable);
  LOAD(Monster_vftable, Monster_vftable);
  LOAD(Item_vftable, Item_vftable);
  LOAD(Npc_HasConversation, Npc_HasConversation);
  LOAD(Destructible_StaticClassInfo, Destructible_GetStaticClassInfo);
  LOAD(Destructible_IsBroken, Destructible_IsBroken);
  LOAD(StaticShrine_StaticClassInfo, StaticShrine_GetStaticClassInfo);
  LOAD(StaticShrine_IsCleansed, StaticShrine_IsCleansed);
  LOAD(Destructible_IsTargetable, Destructible_IsTargetable);
  LOAD(Project, WorldCamera_Project);
  LOAD(GetRayThroughImagePoint, WorldCamera_GetRayThroughImagePoint);
  LOAD(World_GetIntersection, World_GetIntersection);
  LOAD(World_GetAllIntersections, World_GetAllIntersections);
  LOAD(SetZoom, GameCamera_SetZoom);
  LOAD(ResetZoom, GameCamera_ResetZoom);
  LOAD(SetCameraYaw, GameCamera_SetCameraYaw);
  LOAD(Viewport_ctor, Viewport_ctor);
  LOAD(World_GetNumRegions, World_GetNumRegions);
  LOAD(World_GetRegion, World_GetRegion);
  LOAD(Region_GetOffsetFromWorld, Region_GetOffsetFromWorld);
  LOAD(Region_IsUnderground, Region_IsUnderground);
  LOAD(Region_IsLevelLoaded, Region_IsLevelLoaded);
  LOAD(Region_BackgroundLoadLevel, Region_BackgroundLoadLevel);
  LOAD(Region_IsLoadingFinished, Region_IsLoadingFinished);
  LOAD(Engine_GetResourceLoader, Engine_GetResourceLoader);
  LOAD(ResourceLoader_IsIdle, ResourceLoader_IsIdle);
  LOAD(IsGameTimePaused, IsGameTimePaused);
  g_api.Character_StopMoving = fn<decltype(g_api.Character_StopMoving)>("Game.dll", "?StopMoving@Character@GAME@@QEAAX_N0@Z");
  LOAD(PauseGameTime, PauseGameTime);
  LOAD(UnpauseGameTime, UnpauseGameTime);
  LOAD(Region_GetNumPortals, Region_GetNumPortals);
  LOAD(Region_GetPortal, Region_GetPortal);
  LOAD(Region_GetWorldIndex, Region_GetWorldIndex);
  LOAD(Region_GetBoundingBox, Region_GetBoundingBox);
  LOAD(Portal_GetConnectedRegion, Portal_GetConnectedRegion);
  LOAD(Portal_GetChokePoint, Portal_GetChokePoint);
  LOAD(Portal_GetIsOpen, Portal_GetIsOpen);
  LOAD(World_GetRegionContainingXZ, World_GetRegionContainingXZ);
  LOAD(Entity_SetCoords, Entity_SetCoords);
  LOAD(Character_TeleportToLocation, Character_TeleportToLocation);
  LOAD(Region_GetFogOfWar, Region_GetFogOfWar);
  LOAD(FogOfWar_AddVisibility, FogOfWar_AddVisibility);
  LOAD(FogOfWar_IsInFog, FogOfWar_IsInFog);
  LOAD(Level_GetSectorLayers, Level_GetSectorLayers);
  LOAD(SectorLayers_GetTargetId, SectorLayers_GetTargetId);
  LOAD(SectorDataManager_GetSectorData, SectorDataManager_GetSectorData);
  LOAD(gEngine, gEngine);
#undef LOAD
}

void* player() { return g_game_engine && g_api.GetMainPlayer ? g_api.GetMainPlayer(g_game_engine) : nullptr; }

// Layouts measured 2026-08-21 (see CLAUDE.md): WorldCoords = Region* (8) + origin Vec3 (12) + 3x3 axes;
// Coords = 3x3 axes then origin (offset 36); WorldVec3 = Region* + Vec3.
constexpr size_t kWorldCoordsOriginOffset = 8;
constexpr size_t kCoordsOriginOffset = 36;

// WorldVec3::GetWorldPosition dereferences the Region* first thing (Engine.dll+0x22bdf7 reads region+0x3c): a
// WorldVec3 built from a freed entity's coords carried a Region* of 1 and took the game down (2026-09-20 18:51).
// The callers now validate the region (entity_world_vec) and this is the last line: a fault here is a zero
// position, not a crash. POD only inside the __try.
bool get_world_pos_guarded(const Buf* wv, Buf* out) {
  __try { g_api.WorldVec3_GetWorldPosition(wv, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
Vec3 world_pos_of(const Buf& wv) {
  Vec3 v{};
  if (!g_api.WorldVec3_GetWorldPosition) return v;
  Buf o{};
  if (!get_world_pos_guarded(&wv, &o)) {
    static int logged = 0;
    void* region; memcpy(&region, wv.b, sizeof region);
    if (logged++ < 5) log::writef("world: GetWorldPosition faulted (region {})", region);
    return v;
  }
  memcpy(&v, o.b, sizeof v);
  return v;
}
std::string wv_text(const void* wv) {
  if (!wv) return "null";
  Buf b{}; memcpy(b.b, wv, 32);
  void* region; memcpy(&region, b.b, sizeof region);
  Vec3 r; memcpy(&r, b.b + 8, sizeof r);
  // GetWorldPosition on a WorldVec3 without a region hung the game thread once (2026-08-21): report the
  // region-relative part only.
  if (!region) return std::format("(no region; {:.1f}, {:.1f}, {:.1f})", r.x, r.y, r.z);
  Vec3 p = world_pos_of(b);
  return std::format("({:.1f}, {:.1f}, {:.1f})", p.x, p.y, p.z);
}

// Any entity's position as a WorldVec3 built through the game's own ctor from its WorldCoords.
bool get_coords_guarded(const void* entity, Buf* wc) {   // POD only: the entity may be freed (a picked-up item)
  __try { g_api.Entity_GetCoords(entity, wc); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool in_game_image(const void* p);   // defined below (rtti_of's image check)
// A Region* read out of an entity's coords is trusted only if it looks like a live Region: a real address whose
// vtable lies in the game's images. GetCoords on a freed entity (reached through a stale id -> pointer) does not
// fault, it returns garbage -- a "region" of 1 was the 2026-09-20 crash.
bool plausible_region(const void* region) {
  if ((uintptr_t)region < 0x10000 || IsBadReadPtr(region, sizeof(void*))) return false;
  void* vt; memcpy(&vt, region, sizeof vt);
  return in_game_image(vt);
}
bool entity_world_vec(const void* entity, Buf& out_wv, void** out_region = nullptr) {
  if (!entity || !g_api.Entity_GetCoords || !g_api.WorldVec3_ctor) return false;
  Buf wc{};
  if (!get_coords_guarded(entity, &wc)) return false;
  void* region; memcpy(&region, wc.b, sizeof region);
  if (!region) return false;
  if (!plausible_region(region)) {
    static int logged = 0;
    if (logged++ < 5) log::writef("world: entity {} has an implausible region {} (freed?), position refused", entity, region);
    return false;
  }
  Vec3 origin; memcpy(&origin, wc.b + kWorldCoordsOriginOffset, sizeof origin);
  memset(&out_wv, 0, sizeof out_wv);
  g_api.WorldVec3_ctor(&out_wv, region, &origin);
  if (out_region) *out_region = region;
  return true;
}
// The player's feet.
bool player_world_vec(Buf& out_wv, void** out_region = nullptr) {
  void* p = player();
  if (!p || !g_api.Character_GetFootCoords || !g_api.WorldCoords_GetRegionCoords || !g_api.WorldVec3_ctor || !g_api.Entity_GetRegion) return false;
  Buf wc{}, coords{};
  g_api.Character_GetFootCoords(p, &wc, false);
  g_api.WorldCoords_GetRegionCoords(&wc, &coords);
  void* region = g_api.Entity_GetRegion(p);
  if (!region) return false;
  Vec3 origin; memcpy(&origin, coords.b + kCoordsOriginOffset, sizeof origin);
  memset(&out_wv, 0, sizeof out_wv);
  g_api.WorldVec3_ctor(&out_wv, region, &origin);
  if (out_region) *out_region = region;
  return true;
}

// The floor height at a world xz, through the player's region (WorldVec3::PutOnFloor from the player's y);
// fallback_y when the probe is unavailable. project_point lifts the point by 1.0 itself.
float floor_y_at(float x, float z, float fallback_y) {
  Buf base; void* region = nullptr;
  if (!player_world_vec(base, &region) || !g_api.WorldVec3_PutOnFloor) return fallback_y;
  Vec3 wb = world_pos_of(base);
  const Vec3* rb = g_api.WorldVec3_GetRegionPosition(&base);
  Vec3 rel{x - wb.x + rb->x, rb->y, z - wb.z + rb->z};
  Buf wv{};
  g_api.WorldVec3_ctor(&wv, region, &rel);
  if (!g_api.WorldVec3_PutOnFloor(&wv)) return fallback_y;
  Vec3 f; memcpy(&f, wv.b + 8, sizeof f);
  return f.y - rb->y + wb.y;
}
// The object's dynamic RTTI_ClassInfo: the exported Object::GetRTTIClassInfo is the BASE implementation, so
// dispatch through the vtable instead -- its slot is where Object's own vftable holds that export.
// RTTI_ClassInfo (measured 2026-08-21): +0 vptr, +8 const char* name ("Player", "Monster", ...).
int g_rtti_slot = -1;
// A game object's vtable, and the slot it dispatches to, must lie inside the game's own images (the exe,
// Engine.dll, Game.dll). A freed object whose memory was reused carries garbage there; calling through it ran
// arbitrary bytes and corrupted the heap (the cast tracker, VM report 2026-09-14). Every class lookup goes
// through here, so the guard covers every feature that inspects objects.
struct ImageRange { uintptr_t lo = 0, hi = 0; };
const ImageRange* game_images() {
  static ImageRange r[3]; static bool init = false;
  if (!init) {
    init = true;
    HMODULE mods[3] = {GetModuleHandleW(nullptr), GetModuleHandleA("Engine.dll"), GetModuleHandleA("Game.dll")};
    for (int i = 0; i < 3; ++i) {
      if (!mods[i]) continue;
      auto* dos = (const IMAGE_DOS_HEADER*)mods[i];
      auto* nt = (const IMAGE_NT_HEADERS64*)((const char*)mods[i] + dos->e_lfanew);
      r[i].lo = (uintptr_t)mods[i]; r[i].hi = r[i].lo + nt->OptionalHeader.SizeOfImage;
    }
  }
  return r;
}
bool in_game_image(const void* p) {
  const ImageRange* r = game_images();
  uintptr_t a = (uintptr_t)p;
  for (int i = 0; i < 3; ++i) if (r[i].lo && a >= r[i].lo && a < r[i].hi) return true;
  return false;
}
const void* rtti_of(const void* obj) {
  if (!obj || !g_api.Object_GetRTTIClassInfo) return nullptr;
  if (g_rtti_slot == -1) {
    g_rtti_slot = -2;
    if (g_api.Object_vftable)
      for (int i = 0; i < 64; ++i)
        if (g_api.Object_vftable[i] == (void*)g_api.Object_GetRTTIClassInfo) { g_rtti_slot = i; break; }
    log::writef("world: Object::GetRTTIClassInfo vtable slot = {}", g_rtti_slot);
  }
  if (IsBadReadPtr(obj, sizeof(void*))) return nullptr;
  void** vt; memcpy(&vt, obj, sizeof vt);
  if (!in_game_image(vt) || IsBadReadPtr(vt, (g_rtti_slot < 0 ? 1 : g_rtti_slot + 1) * sizeof(void*))) return nullptr;
  if (g_rtti_slot < 0) return g_api.Object_GetRTTIClassInfo(obj);
  if (!in_game_image(vt[g_rtti_slot])) return nullptr;
  return ((const void* (*)(const void*))vt[g_rtti_slot])(obj);
}
std::string rtti_name(const void* ci) {
  if (!ci) return "?";
  const char* n; memcpy(&n, (const char*)ci + 8, sizeof n);
  return n && !IsBadStringPtrA(n, 64) ? std::string(n) : std::string("?");
}
std::string class_name(const void* obj) {
  const void* ci = rtti_of(obj);
  if (!ci) return "?";
  const char* n; memcpy(&n, (const char*)ci + 8, sizeof n);
  return n && !IsBadStringPtrA(n, 64) ? std::string(n) : std::string("?");
}

// One object's readout, taken under a structured-exception guard: the sphere query returns every Object
// in range, not only Entities, so any of these calls may fault on a foreign object (that killed the game
// once). Plain data only -- no C++ objects with destructors may live in a __try frame.
struct EntityRaw { unsigned id; const void* ci; Vec3 pos; bool has_pos; char name[160]; };
bool read_entity(void* e, EntityRaw& r) {
  __try {
    r.id = g_api.Object_GetObjectId ? g_api.Object_GetObjectId(e) : 0;
    r.ci = rtti_of(e);  // dynamic class (vtable dispatch), not the exported base implementation
    const char* name = g_api.Object_GetObjectName ? g_api.Object_GetObjectName(e) : nullptr;
    r.name[0] = 0;
    if (name && !IsBadStringPtrA(name, 512)) { strncpy_s(r.name, name, sizeof r.name - 1); }
    Buf wv;
    r.has_pos = entity_world_vec(e, wv);
    if (r.has_pos) r.pos = world_pos_of(wv);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
}  // namespace
const void* object_rtti(const void* obj) { return rtti_of(obj); }
std::string object_class_name(const void* obj) { return class_name(obj); }
namespace { bool is_kind_of(const void* ci, const void* base); }
bool object_is_a(const void* obj, const void* class_info) { return obj && class_info && is_kind_of(rtti_of(obj), class_info); }

bool install() {
  load_api();
  g_hooks = {GD_HOOK(GameEngine_Update, GameEngineUpdate), GD_HOOK(ControllerPlayer_Update, ControllerPlayerUpdate), GD_HOOK(World_Update, WorldUpdate),
             GD_HOOK(ControllerPlayer_SetCombatEnemy, SetCombatEnemy_hook), GD_HOOK(ControllerPlayer_FaceTarget, FaceTarget_hook),
             GD_HOOK(ControllerPlayer_ClearTarget, ClearTarget_hook), GD_HOOK(ControllerPlayer_HandleActionFromJoystick, HandleActionFromJoystick_hook),
             GD_HOOK(ControllerPlayer_HandleActionFromMouse, HandleActionFromMouse_hook),
             GD_HOOK(Conversation_GetText, ConvGetText_hook), GD_HOOK(Conversation_GetSteps, ConvGetSteps_hook)};
  g_conv_api.GetType = fn<decltype(g_conv_api.GetType)>(ConversationStep_GetType_DLL, ConversationStep_GetType);
  g_conv_api.GetTypeTag = fn<decltype(g_conv_api.GetTypeTag)>(ConversationStep_GetTypeTag_DLL, ConversationStep_GetTypeTag);
  g_conv_api.IsAvailable = fn<decltype(g_conv_api.IsAvailable)>(ConversationStep_IsAvailable_DLL, ConversationStep_IsAvailable);
  g_conv_api.IsUsed = fn<decltype(g_conv_api.IsUsed)>(ConversationStep_IsUsed_DLL, ConversationStep_IsUsed);
  g_conv_api.GetParent = fn<decltype(g_conv_api.GetParent)>(ConversationStep_GetParent_DLL, ConversationStep_GetParent);
  return gd::hooks::attach_hooks(g_hooks) == 0;
}
void remove() { gd::hooks::detach_hooks(g_hooks); g_game_engine = nullptr; g_controller = nullptr; g_world = nullptr; }

// Not gated on the controller: ControllerPlayer::Update stops while the game is paused (alt-tab pauses single
// player; a hot reload in the world leaves it paused, /pause?set=0), and the world is still there.
bool in_world() { return g_game_engine && player() != nullptr; }
void* game_engine() { return g_game_engine; }
void* controller() { return g_controller; }

bool player_position(Vec3& p) {
  Buf wv;
  if (!player_world_vec(wv)) return false;
  p = world_pos_of(wv);
  return true;
}
std::string player_name() {
  void* p = player();
  const char16_t* n = p && g_api.GetPlayerName ? g_api.GetPlayerName(p) : nullptr;
  return n ? log::utf8(n) : std::string();
}
unsigned player_id() {
  void* p = player();
  return p && g_api.Object_GetObjectId ? g_api.Object_GetObjectId(p) : 0;
}
static bool read_area_tag(const void* eng, char* out, size_t cap) {   // POD only (SEH)
  __try {
    const MsvcStringA* s = g_api.Engine_GetAreaNameTag(eng);
    if (!s || s->size == 0 || s->size >= cap) return false;
    memcpy(out, s->data(), s->size); out[s->size] = 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
namespace { bool is_kind_of(const void* ci, const void* base); }
bool object_is_note(const void* obj) {
  if (!obj) return false;
  load_api();
  return g_api.ItemNote_StaticClassInfo && is_kind_of(rtti_of(obj), g_api.ItemNote_StaticClassInfo());
}
std::string area_name() {
  load_api();
  void* eng = gd::hooks::engine_object();
  char tag[256];
  if (!eng || !g_api.Engine_GetAreaNameTag || !read_area_tag(eng, tag, sizeof tag)) return {};
  std::string text = gd::hooks::localize(tag);
  return text.empty() ? std::string(tag) : text;
}
std::string region_name() {
  void* p = player();
  void* r = p && g_api.Entity_GetRegion ? g_api.Entity_GetRegion(p) : nullptr;
  if (!r || !g_api.Region_GetName) return {};
  const MsvcString<char>* s = g_api.Region_GetName(r);
  return s ? std::string(s->view()) : std::string();
}
double life() { void* p = player(); return p && g_api.GetCurrentLife ? g_api.GetCurrentLife(p) : 0.0; }
bool player_alive() { void* p = player(); return p && g_api.Character_IsAlive && g_api.Character_IsAlive(p); }
float life_max() { void* p = player(); return p && g_api.GetLifeLimit ? g_api.GetLifeLimit(p) : 0.0f; }
float energy() { void* p = player(); return p && g_api.GetCurrentMana ? g_api.GetCurrentMana(p) : 0.0f; }
float energy_max() { void* p = player(); return p && g_api.GetManaLimit ? g_api.GetManaLimit(p) : 0.0f; }
float camera_yaw() {
  void* cam = g_game_engine && g_api.GetCamera ? g_api.GetCamera(g_game_engine) : nullptr;
  return cam && g_api.GetCameraYaw ? g_api.GetCameraYaw(cam) : 0.0f;
}

// Snap `world_point` to the floor (PutOnFloor) and test it against the path mesh. `floored`, when non-null, is
// filled with the snapped world point regardless of the result -- so a caller can carry the floor height
// forward and keep a straight ray hugging the terrain instead of holding a flat y (see free_distance).
bool navmesh_probe(const Vec3& world_point, Vec3* floored) {
  void* nav = g_api.NavManager_Get ? g_api.NavManager_Get() : nullptr;
  Buf base; void* region = nullptr;
  if (!nav || !g_api.IsPointOnPathMesh || !player_world_vec(base, &region)) return false;
  // region-relative = world - (world_of_base - regionpos_of_base)
  Vec3 wb = world_pos_of(base);
  const Vec3* rb = g_api.WorldVec3_GetRegionPosition(&base);
  Vec3 rel{world_point.x - wb.x + rb->x, world_point.y - wb.y + rb->y, world_point.z - wb.z + rb->z};
  Buf wv{};
  g_api.WorldVec3_ctor(&wv, region, &rel);
  if (g_api.WorldVec3_PutOnFloor) g_api.WorldVec3_PutOnFloor(&wv);
  bool ok = g_api.IsPointOnPathMesh(nav, &wv);
  if (floored) *floored = world_pos_of(wv);
  return ok;
}
bool on_navmesh(const Vec3& world_point) { return navmesh_probe(world_point, nullptr); }

static void* region_containing_xz(void* from, float wx, float wz);   // defined with the teleport helpers below
// ---- painted damage sectors (docs/hazards.md) ----
// The engine's per-cell "sector" layers carry a damage layer (index 7, DamageSectorData): TickManager::Tick reads
// it once a second at each character's region-relative position (truncated ints) and applies
// rate * max life straight through CombatManager::ApplyDamage -- no resistance (Aether Act3 Boss 0.12, Aether01
// 0.15, Aether02 0.30, poisons 0.02..0.20). This is that lookup, for any world point: region containing the
// point -> its Level -> SectorLayers -> UniqueId at (x, z) -> SectorData (+0x58 rate, +0x5c type). Cheap (pointer
// hops), no caching. Region+0x68 = Level (CLAUDE.md). SEH-guarded: an unloaded chunk has no layers.
namespace {
constexpr int kDamageLayer = 7;
constexpr size_t kRegionLevelOffset = 0x68;
constexpr size_t kSectorDataRate = 0x58, kSectorDataType = 0x5c;
struct HazardHit { bool painted = false; float rate = 0; int type = 0; };
HazardHit seh_hazard(void* region, int rx, int rz) {
  HazardHit h;
  __try {
    void* level = *(void**)((char*)region + kRegionLevelOffset);
    if (!level) return h;
    void* layers = g_api.Level_GetSectorLayers(level);
    if (!layers) return h;
    alignas(16) unsigned char uid[16] = {};
    g_api.SectorLayers_GetTargetId(layers, uid, kDamageLayer, rx, rz);
    bool any = false; for (unsigned char c : uid) any |= c != 0;
    if (!any) return h;
    void* engine = g_api.gEngine ? *g_api.gEngine : nullptr;
    if (!engine) return h;
    const void* mgr = (const char*)engine + 8;
    const char* sd = (const char*)g_api.SectorDataManager_GetSectorData(mgr, kDamageLayer, uid);
    if (!sd) return h;
    h.painted = true; h.rate = *(const float*)(sd + kSectorDataRate); h.type = *(const int*)(sd + kSectorDataType);
  } __except (EXCEPTION_EXECUTE_HANDLER) { h = HazardHit{}; }
  return h;
}
}  // namespace
bool hazard_at(const Vec3& world_point, float* rate, int* type) {
  if (!g_api.Level_GetSectorLayers || !g_api.SectorLayers_GetTargetId || !g_api.SectorDataManager_GetSectorData || !g_api.Region_GetOffsetFromWorld) return false;
  void* p = player();
  void* region = p && g_api.Entity_GetRegion ? g_api.Entity_GetRegion(p) : nullptr;
  if (!region) return false;
  // The player's chunk covers [offset, offset + 128) in x and z; a point outside it belongs to a neighbour.
  const int* off = g_api.Region_GetOffsetFromWorld(region);
  if (!off) return false;
  float rx = world_point.x - (float)off[0], rz = world_point.z - (float)off[2];
  if (rx < 0 || rx >= 128.0f || rz < 0 || rz >= 128.0f) {
    void* other = region_containing_xz(region, world_point.x, world_point.z);
    if (!other) return false;
    const int* o2 = g_api.Region_GetOffsetFromWorld(other);
    if (!o2) return false;
    region = other; rx = world_point.x - (float)o2[0]; rz = world_point.z - (float)o2[2];
  }
  HazardHit h = seh_hazard(region, (int)rx, (int)rz);   // truncation toward zero, like the tick's cvttss2si
  if (rate) *rate = h.rate;
  if (type) *type = h.type;
  return h.painted;
}

// dev only: run the game's own pathfinder (Player::FindPath) from the player to a world point. Returns the raw
// PathResult enum (calibrate live) and, in out_world, the reachable endpoint the pathfinder resolved. Used to
// tell whether a bake-islanded room is actually connected to its region's main body via the runtime navmesh
// (stairs/ramps the flat bake drops), and where -- so exits can be added for real connections (2026-08-24).
namespace {
int seh_find_path(void* p, const void* dest, float f1, void* out, float f2) {
  __try { return g_api.Player_FindPath(p, dest, f1, out, f2); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return -1000; }
}
}
int find_path(const Vec3& dest_world, float f1, float f2, Vec3* out_world) {
  load_api();
  void* p = player();
  Buf base; void* region = nullptr;
  if (!p || !g_api.Player_FindPath || !g_api.WorldVec3_ctor || !player_world_vec(base, &region)) return -1001;
  Vec3 wb = world_pos_of(base);
  const Vec3* rb = g_api.WorldVec3_GetRegionPosition(&base);
  Vec3 rel{dest_world.x - wb.x + rb->x, dest_world.y - wb.y + rb->y, dest_world.z - wb.z + rb->z};
  Buf dest{}; g_api.WorldVec3_ctor(&dest, region, &rel);
  if (g_api.WorldVec3_PutOnFloor) g_api.WorldVec3_PutOnFloor(&dest);
  Buf out = dest;   // valid WorldVec3 (same region) in case FindPath leaves it untouched
  int result = seh_find_path(p, &dest, f1, &out, f2);
  if (out_world) *out_world = world_pos_of(out);
  return result;
}

// The game's own navmesh path from the player to a world point, as a polyline in absolute world coords
// (NavManager::FindPath, Engine.dll). The mem::vector<WorldVec3> 7th arg is the straight-path corridor -- all
// four scalar out-params are null-checked in the body, so we pass only the corridor. Used to decide whether a
// nearby room is a DIRECT exit (the route to it does not detour through a third room). On-demand (V / room
// change), never per frame. Fills `out` with the path points; returns false (and clears `out`) on any failure.
namespace {
constexpr float kNavSnapRadius = 4.0f;   // how far off-mesh from/to may be and still snap (gates the found point)
bool seh_nav_find_path(void* nav, const void* from, const void* to, void* corridor) {
  __try { return g_api.NavManager_FindPath(nav, from, to, kNavSnapRadius, nullptr, nullptr, nullptr, corridor, nullptr, false); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
bool find_path_corridor(const Vec3& dest_world, std::vector<Vec3>& out) {
  out.clear();
  load_api();
  void* nav = g_api.NavManager_Get ? g_api.NavManager_Get() : nullptr;
  Buf base; void* region = nullptr;
  if (!nav || !g_api.NavManager_FindPath || !g_api.WorldVec3_ctor || !g_api.WorldVec3_GetRegionPosition ||
      !player_world_vec(base, &region))
    return false;
  Vec3 wb = world_pos_of(base);
  const Vec3* rb = g_api.WorldVec3_GetRegionPosition(&base);
  Vec3 rel{dest_world.x - wb.x + rb->x, dest_world.y - wb.y + rb->y, dest_world.z - wb.z + rb->z};
  Buf dest{}; g_api.WorldVec3_ctor(&dest, region, &rel);
  if (g_api.WorldVec3_PutOnFloor) g_api.WorldVec3_PutOnFloor(&dest);
  // Reuse one game-allocated corridor vector across calls (game thread only): reset its size to 0 (POD
  // WorldVec3, no dtors) so the game refills from empty and reuses capacity. The buffer is intentionally never
  // freed -- a single small leak on unload, the same pattern as GetEntitiesInSphere.
  static MemVec corridor{};
  corridor.end = corridor.begin;   // size 0, keep capacity
  if (!seh_nav_find_path(nav, &base, &dest, &corridor)) return false;
  if (!(corridor.begin && corridor.end && (uintptr_t)corridor.end > (uintptr_t)corridor.begin &&
        (uintptr_t)corridor.end - (uintptr_t)corridor.begin < (1u << 20)))
    return false;
  constexpr size_t kStride = 0x18;   // sizeof(WorldVec3): Region*(8) + Vec3(12) + pad(4)
  size_t n = (size_t)((char*)corridor.end - (char*)corridor.begin) / kStride;
  for (size_t i = 0; i < n && i < 4096; ++i) {
    Buf wv{}; memcpy(wv.b, (char*)corridor.begin + i * kStride, kStride);
    out.push_back(world_pos_of(wv));
  }
  return !out.empty();
}

// ---- dev dumps of the world structure (chunks = engine Regions; tools/gdmap reads the same data offline) ----
std::string region_label(const void* r) {
  if (!r) return "null";
  const MsvcString<char>* s = g_api.Region_GetName ? g_api.Region_GetName(r) : nullptr;
  return s ? std::string(s->view()) : std::format("{}", r);
}
// POD-only SEH helpers (MSVC C2712: no __try in functions with C++ objects).
struct RegionInfo { void* region; int index; int offset[3]; bool underground, loaded; int portals; float bbox[6]; };
static bool read_region_info(void* world, int i, RegionInfo* out) {
  __try {
    void* r = g_api.World_GetRegion(world, i);
    if (!r) return false;
    out->region = r;
    out->index = g_api.Region_GetWorldIndex ? g_api.Region_GetWorldIndex(r) : -1;
    const int* off = g_api.Region_GetOffsetFromWorld ? g_api.Region_GetOffsetFromWorld(r) : nullptr;
    for (int k = 0; k < 3; ++k) out->offset[k] = off ? off[k] : 0;
    // NOT Region::IsUnderground: it calls Region::LoadLevel (disassembled 2026-08-22) -- over all chunks
    // that loads the whole world. GetBoundingBox is unverified, so it is only read for loaded chunks.
    out->underground = false;
    out->loaded = g_api.Region_IsLevelLoaded ? g_api.Region_IsLevelLoaded(r) : false;
    out->portals = g_api.Region_GetNumPortals ? g_api.Region_GetNumPortals(r) : -1;
    const float* bb = out->loaded && g_api.Region_GetBoundingBox ? g_api.Region_GetBoundingBox(r) : nullptr;
    for (int k = 0; k < 6; ++k) out->bbox[k] = bb ? bb[k] : 0.f;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct PortalInfo { void* portal; void* other; bool open; void* choke_region; Vec3 choke; };
static bool read_portal_info(void* region, int i, PortalInfo* out) {
  __try {
    void* portal = g_api.Region_GetPortal(region, i);
    if (!portal) return false;
    out->portal = portal;
    out->other = g_api.Portal_GetConnectedRegion ? g_api.Portal_GetConnectedRegion(portal) : nullptr;
    out->open = g_api.Portal_GetIsOpen ? g_api.Portal_GetIsOpen(portal) : false;
    Buf wc{};
    if (g_api.Portal_GetChokePoint) g_api.Portal_GetChokePoint(portal, &wc);
    memcpy(&out->choke_region, wc.b, sizeof out->choke_region);
    memcpy(&out->choke, wc.b + kWorldCoordsOriginOffset, sizeof out->choke);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// /settle: is the world quiet enough to photograph? (2026-09-15, for tools/shots.py -- replaces a flat 1.6 s sleep
// per sample). idle = the engine's ResourceLoader has nothing queued (ResourceLoader::IsIdle, a zero-timeout wait
// on its work event -- the engine's own WaitForIdle spins on the same test); loading = chunks within `radius` of the
// player whose level is attached but whose loading byte is still set (Region::IsLoadingFinished false);
// unloaded_near = chunks in that radius with no level at all (reported, not waited on: a far dungeon nearby in XZ
// never streams); state = the exe's app state (10 = loading screen); ticks = GameEngine::Update count.
// SEH-guarded raw scan (no C++ objects with destructors): counts chunks near (px, pz) by load state.
static void settle_scan_raw(void* world, float px, float pz, float radius, int* loading, int* unloaded, int* nearby) {
  int n = g_api.World_GetNumRegions(world);
  for (int i = 0; i < n; ++i) {
    __try {
      void* r = g_api.World_GetRegion(world, i);
      if (!r) continue;
      const int* off = g_api.Region_GetOffsetFromWorld(r);
      if (!off) continue;
      // chunk footprint [off, off+128) vs the player, in XZ
      float dx = (float)off[0] - px; if (dx < px - ((float)off[0] + 128.f)) dx = px - ((float)off[0] + 128.f); if (dx < 0.f) dx = 0.f;
      float dz = (float)off[2] - pz; if (dz < pz - ((float)off[2] + 128.f)) dz = pz - ((float)off[2] + 128.f); if (dz < 0.f) dz = 0.f;
      if (dx * dx + dz * dz > radius * radius) continue;
      ++*nearby;
      bool loaded = g_api.Region_IsLevelLoaded ? g_api.Region_IsLevelLoaded(r) : false;
      if (!loaded) { ++*unloaded; continue; }
      if (!g_api.Region_IsLoadingFinished(r)) ++*loading;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ++*loading;   // an unreadable region counts as not settled
    }
  }
}

std::string settle_status(float radius) {
  load_api();
  int idle = -1;
  if (g_api.gEngine && *g_api.gEngine && g_api.Engine_GetResourceLoader && g_api.ResourceLoader_IsIdle) {
    void* rl = g_api.Engine_GetResourceLoader(*g_api.gEngine);
    if (rl) idle = g_api.ResourceLoader_IsIdle(rl) ? 1 : 0;
  }
  int loading = 0, unloaded = 0, nearby = 0;
  Vec3 p{};
  bool have = player_position(p);
  if (have && g_world && g_api.World_GetNumRegions && g_api.World_GetRegion && g_api.Region_GetOffsetFromWorld && g_api.Region_IsLoadingFinished)
    settle_scan_raw(g_world, p.x, p.z, radius, &loading, &unloaded, &nearby);
  return std::format("idle={} loading={} unloaded_near={} nearby={} state={} ticks={} paused={}\n", idle, loading, unloaded, nearby,
                     exe_ui::app_state(), g_engine_ticks, g_api.IsGameTimePaused ? (int)g_api.IsGameTimePaused() : -1);
}

std::string regions_dump(int max) {
  load_api();
  if (!g_world || !g_api.World_GetNumRegions || !g_api.World_GetRegion) return "no world\n";
  std::string out;
  int n = g_api.World_GetNumRegions(g_world);
  out += std::format("{} regions (world={}), showing up to {}\n", n, g_world, max);
  for (int i = 0; i < n && i < max; ++i) {
    RegionInfo ri{};
    log::writef("regions_dump: reading region {}", i);
    if (!read_region_info(g_world, i, &ri)) { out += std::format("{}: null or faulted\n", i); continue; }
    out += std::format("{:4d} {:28s} idx={} offset=({}, {}, {}) loaded={} portals={} bbox=({:.0f},{:.0f},{:.0f})..({:.0f},{:.0f},{:.0f})\n", i, region_label(ri.region),
                       ri.index, ri.offset[0], ri.offset[1], ri.offset[2], ri.loaded, ri.portals, ri.bbox[0], ri.bbox[1], ri.bbox[2], ri.bbox[3], ri.bbox[4], ri.bbox[5]);
  }
  return out;
}
// SEH-guarded raw read (no C++ objects with destructors here). Returns the real UID count (>=0),
// copying up to max_uids*4 ints into buf; negative = fault/implausible.
static int read_marker_uids_raw(void* p, int* buf, int max_uids) {
  __try {
    const MemVec* v = g_api.GetMarkerUIDs(p);
    if (!v || !v->begin || !v->end || (uintptr_t)v->end < (uintptr_t)v->begin) return 0;
    size_t bytes = (char*)v->end - (char*)v->begin;
    if (bytes > (1u << 20)) return -2;
    int n = (int)(bytes / 16);   // UniqueId = 4 ints (matches the WorldMapWindow icon uid[4])
    int take = n < max_uids ? n : max_uids;
    memcpy(buf, v->begin, (size_t)take * 16);
    return n;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return -3; }
}
std::string markers_dump() {
  load_api();
  void* p = player();
  if (!p || !g_api.GetMarkerUIDs) return "no player / export\n";
  constexpr int kMax = 512;
  static int buf[kMax * 4];
  int n = read_marker_uids_raw(p, buf, kMax);
  if (n < 0) return std::format("read failed ({})\n", n);
  std::string out = std::format("Player::GetMarkerUIDs: {} marker UIDs\n", n);
  int shown = n < kMax ? n : kMax;
  for (int i = 0; i < shown; ++i)
    out += std::format("  [{}] {} {} {} {}\n", i, buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2], buf[i * 4 + 3]);
  return out;
}
std::string portals_dump() {
  load_api();
  void* p = player();
  void* region = p && g_api.Entity_GetRegion ? g_api.Entity_GetRegion(p) : nullptr;
  if (!region || !g_api.Region_GetNumPortals || !g_api.Region_GetPortal) return "no region\n";
  std::string out;
  int n = g_api.Region_GetNumPortals(region);
  out += std::format("region {} ({}): {} portals\n", region_label(region), region, n);
  for (int i = 0; i < n && i < 256; ++i) {
    PortalInfo pi{};
    if (!read_portal_info(region, i, &pi)) { out += std::format("  {}: null or faulted\n", i); continue; }
    std::string world;
    if (pi.choke_region && g_api.WorldVec3_ctor) { Buf wv{}; g_api.WorldVec3_ctor(&wv, pi.choke_region, &pi.choke); world = wv_text(&wv); }
    out += std::format("  {}: -> {} open={} choke region={} rel=({:.1f}, {:.1f}, {:.1f}) world={}\n", i, region_label(pi.other), pi.open, region_label(pi.choke_region),
                       pi.choke.x, pi.choke.y, pi.choke.z, world);
  }
  return out;
}
// ---- authoring dev routes (docs/rooms.md M4): teleport, batch projection, fog reveal ----
// World::GetRegionContainingXZ(world, from, x, z) takes x,z RELATIVE TO `from` (the disassembly adds them to
// from's GetOffsetFromWorld before the search; read 2026-08-22 after a shots run resolved every room north
// of the spawn to the wrong chunk). Our callers think in world coordinates.
static void* region_containing_xz(void* from, float wx, float wz) {
  if (!g_api.World_GetRegionContainingXZ || !g_world || !from) return nullptr;
  const int* off = g_api.Region_GetOffsetFromWorld ? g_api.Region_GetOffsetFromWorld(from) : nullptr;
  return g_api.World_GetRegionContainingXZ(g_world, from, wx - (off ? (float)off[0] : 0.f), wz - (off ? (float)off[2] : 0.f));
}
namespace { bool project_point(const Vec3& world_point, float& x, float& y); }   // defined with the cursor lock below
bool world_vec3_at(const Vec3& w, void* out) {
  load_api();
  Buf base; void* region = nullptr;
  if (!out || !g_api.WorldVec3_ctor || !player_world_vec(base, &region)) return false;
  void* target = g_api.World_GetRegionContainingXZ && g_world ? region_containing_xz(region, w.x, w.z) : nullptr;
  if (!target) target = region;
  const int* off = g_api.Region_GetOffsetFromWorld ? g_api.Region_GetOffsetFromWorld(target) : nullptr;
  Vec3 rel{w.x - (off ? (float)off[0] : 0.f), w.y - (off ? (float)off[1] : 0.f), w.z - (off ? (float)off[2] : 0.f)};
  g_api.WorldVec3_ctor(out, target, &rel);
  return true;
}
std::string teleport(float x, float z, bool check_only) {
  load_api();
  void* p = player();
  Buf base; void* region = nullptr;
  if (!p || !g_api.Character_TeleportToLocation || !g_api.Entity_GetCoords || !player_world_vec(base, &region)) return "no player or exports\n";
  // Target chunk: the one containing (x, z), reached from the player's chunk; the chunk's frame is world minus
  // its offset (verified 2026-08-22), so region-relative = world - GetOffsetFromWorld().
  void* target = g_api.World_GetRegionContainingXZ && g_world ? region_containing_xz(region, x, z) : nullptr;
  if (!target) return std::format("no chunk contains ({:.1f}, {:.1f})\n", x, z);
  // Into an unloaded chunk SetCoords crashes the game (2026-08-22, the first far room of a region run):
  // the caller hops closer and waits until the chunk's level is streamed in.
  bool loaded = g_api.Region_IsLevelLoaded ? g_api.Region_IsLevelLoaded(target) : false;
  // An off-map dungeon (Burial Cave at world x 1184+) is outside every proximity stream: request the load
  // and let the caller poll check=1 until loaded (Region::BackgroundLoadLevel is guarded, repeat calls ok).
  if (!loaded && g_api.Region_BackgroundLoadLevel) g_api.Region_BackgroundLoadLevel(target, false);
  if (check_only || !loaded) return std::format("target chunk {} loaded={}{}\n", region_label(target), loaded, loaded ? "" : " (refused)");
  const int* off = g_api.Region_GetOffsetFromWorld ? g_api.Region_GetOffsetFromWorld(target) : nullptr;
  Vec3 wb = world_pos_of(base);
  // PutOnFloor casts DOWN from (y + a small lift): starting under the target's floor finds nothing, the entity
  // lands off the navmesh, and the controller snaps it back to its last valid spot (2026-08-22, the spawn at
  // 7.8 reached from a chunk at 1.7). Try rising start heights and keep the first landing the navmesh accepts.
  Vec3 floored{}; bool found = false; float used_lift = 0.f;
  for (float lift : {0.f, 5.f, 15.f, 40.f, 80.f}) {
    Vec3 rel{x - (off ? (float)off[0] : 0.f), wb.y + lift - (off ? (float)off[1] : 0.f), z - (off ? (float)off[2] : 0.f)};
    Buf wv{};
    g_api.WorldVec3_ctor(&wv, target, &rel);
    if (g_api.WorldVec3_PutOnFloor) g_api.WorldVec3_PutOnFloor(&wv);
    Vec3 cand; memcpy(&cand, wv.b + 8, sizeof cand);
    Vec3 cand_world{cand.x + (off ? (float)off[0] : 0.f), cand.y + (off ? (float)off[1] : 0.f), cand.z + (off ? (float)off[2] : 0.f)};
    if (on_navmesh(cand_world)) { floored = cand; found = true; used_lift = lift; break; }
  }
  if (!found) return std::format("no navmesh floor at ({:.1f}, {:.1f}) in {} (refused)\n", x, z, region_label(target));
  Buf wc{};
  g_api.Entity_GetCoords(p, &wc);                       // keep the axes, replace region + origin
  memcpy(wc.b, &target, sizeof target);
  memcpy(wc.b + kWorldCoordsOriginOffset, &floored, sizeof floored);
  // NOT Entity::SetCoords: that is a 0x40-byte field write + OnMoved() and nothing else (Engine.dll 0x7ea00, read
  // 2026-08-22). The level bookkeeping (Level::RemoveEntity from the old level, GuaranteedGetLevel + add to the
  // new one, OnAddToLevel) lives in World::SetCoords, and the player's per-frame update
  // (Engine::UpdateForcedEntitiesInPlayerLoadSphere -> Entity::Update -> ... -> ControllerPlayer::Update) only
  // reaches an entity registered in the region being iterated: a raw SetCoords stalled the controller (WASD dead)
  // and, once the stale level was torn down with the controller object, crashed the exe's mouse handler in
  // SetMoveCommand. Character::TeleportToLocation is the game's path: ControllerCharacter::Teleport ->
  // World::SetCoords (axes kept from the character, only region + origin used) + NavManager::ResetObject.
  g_api.Character_TeleportToLocation(p, &wc);
  Vec3 now; player_position(now);
  return std::format("teleported to ({:.1f}, {:.1f}, {:.1f}) in {}; on_navmesh={} lift={}\n", now.x, now.y, now.z, region_label(target), on_navmesh(now), used_lift);
}
// The game pauses single player when it believes its window lost focus; a hot reload in the world does exactly
// that (the foreground fake is gone between unload and re-inject) and the pause outlives the re-inject:
// ControllerPlayer::Update stops, in_world() turns false, the mod says "unsupported screen" (2026-08-22).
std::string set_paused(int want) {
  load_api();
  if (want == 1 && g_api.PauseGameTime) g_api.PauseGameTime();
  if (want == 0 && g_api.UnpauseGameTime) g_api.UnpauseGameTime();
  return std::format("paused={}\n", g_api.IsGameTimePaused ? g_api.IsGameTimePaused() : false);
}
std::string project_points(const std::vector<Vec3>& pts) {
  load_api();
  std::string out;
  RECT rc{};
  HWND w = FindWindowA("Grim Dawn", nullptr);
  if (!w || !GetClientRect(w, &rc)) return "no window\n";
  for (Vec3 p : pts) {
    // ground points: put on the floor through the player's region first (project_point lifts by 1.0)
    float y = floor_y_at(p.x, p.z, p.y);
    float sx, sy;
    bool ok = project_point(Vec3{p.x, y, p.z}, sx, sy);
    bool visible = ok && sx >= 0 && sy >= 0 && sx < (float)rc.right && sy < (float)rc.bottom;
    out += std::format("{:.2f},{:.2f} -> {:.1f},{:.1f} {}\n", p.x, p.z, sx, sy, visible ? "visible" : "off");
  }
  return out;
}
std::string fog_reveal(float x, float z, int radius) {
  load_api();
  void* p = player();
  void* region = p && g_api.Entity_GetRegion ? g_api.Entity_GetRegion(p) : nullptr;
  if (!region || !g_api.Region_GetFogOfWar || !g_api.FogOfWar_AddVisibility) return "no region or exports\n";
  void* target = g_api.World_GetRegionContainingXZ && g_world ? region_containing_xz(region, x, z) : region;
  if (!target) target = region;
  void* fow = g_api.Region_GetFogOfWar(target, false);
  if (!fow) return "no fog of war object\n";
  const int* off = g_api.Region_GetOffsetFromWorld ? g_api.Region_GetOffsetFromWorld(target) : nullptr;
  Vec3 rel{x - (off ? (float)off[0] : 0.f), 0.f, z - (off ? (float)off[2] : 0.f)};
  bool before = g_api.FogOfWar_IsInFog ? g_api.FogOfWar_IsInFog(fow, &rel) : false;
  g_api.FogOfWar_AddVisibility(fow, &rel, radius);
  bool after = g_api.FogOfWar_IsInFog ? g_api.FogOfWar_IsInFog(fow, &rel) : false;
  return std::format("fog at ({:.1f}, {:.1f}) in {}: in_fog before={} after={} (radius {})\n", x, z, region_label(target), before, after, radius);
}

std::string navprobe(float x0, float z0, float x1, float z1, float step) {
  load_api();
  Vec3 p;
  if (!player_position(p) || step <= 0.05f) return "no player or bad step\n";
  int cols = (int)((x1 - x0) / step) + 1, rows = (int)((z1 - z0) / step) + 1;
  if (cols <= 0 || rows <= 0 || (long long)cols * rows > 400000) return "bad or too large range\n";
  std::string out = std::format("navprobe x0={} z0={} x1={} z1={} step={} cols={} rows={} player=({:.1f},{:.1f},{:.1f}) region={}\n", x0, z0, x1, z1, step, cols, rows, p.x, p.y, p.z, region_name());
  for (int r = 0; r < rows; ++r) {
    std::string line; line.reserve(cols + 1);
    for (int c = 0; c < cols; ++c) line += on_navmesh(Vec3{x0 + c * step, p.y, z0 + r * step}) ? '#' : '.';
    out += line; out += '\n';
  }
  return out;
}

// Free walkable distance along a straight horizontal ray. The ray HUGS THE TERRAIN: each sample's start y is
// the floor height snapped at the previous sample, not a flat player-height y. PutOnFloor only searches a fixed
// window above the sample's start (World::PutOnFloor raises y by a constant, then casts down), so a flat-y ray
// loses the floor once a slope climbs past that window and reports a phantom wall on any real incline; carrying
// the floor forward keeps every step within one slope-delta of the true floor. `follow=false` restores the old
// flat-y behaviour (kept for the A/B probe, /wallcmp).
static float free_distance_from(const Vec3& p, float dir_x, float dir_z, float max_dist, float step, bool follow) {
  float y = p.y;
  for (float d = step; d <= max_dist + 1e-4f; d += step) {
    Vec3 q{p.x + dir_x * d, y, p.z + dir_z * d}, floored;
    if (!navmesh_probe(q, &floored)) return d - step;
    if (follow) y = floored.y;
  }
  return max_dist;
}
float free_distance_ex(float dir_x, float dir_z, float max_dist, float step, bool follow) {
  Vec3 p;
  if (!player_position(p) || step <= 0) return 0;
  return free_distance_from(p, dir_x, dir_z, max_dist, step, follow);
}
float free_distance(float dir_x, float dir_z, float max_dist, float step) {
  return free_distance_ex(dir_x, dir_z, max_dist, step, true);
}
// A lane of a rectangle probe: the same terrain-following ray, started `lateral` units beside the player. The
// lane's own start is tested first (a lane beginning inside the wall the player is hugging is not a route and
// must read 0), then it walks like the centre ray.
float free_distance_lane(float dir_x, float dir_z, float lateral, float max_dist, float step) {
  Vec3 p;
  if (!player_position(p) || step <= 0) return 0;
  if (lateral == 0.0f) return free_distance_from(p, dir_x, dir_z, max_dist, step, true);
  Vec3 start{p.x - dir_z * lateral, p.y, p.z + dir_x * lateral}, floored;
  if (!navmesh_probe(start, &floored)) return 0;
  start.y = floored.y;
  return free_distance_from(start, dir_x, dir_z, max_dist, step, true);
}

// Free distance along a straight ray by the game's navmesh RAYCAST (NavManager::FindStraightMovePoint =
// findNearestPoly(from) + dtNavMeshQuery::raycast): it walks the polygon graph portal to portal and stops at the
// first edge with no neighbour -- a wall, a ledge, a hole, a fence -- and reports exactly where. Replaces the
// IsPointOnPathMesh point walk for the wall tones (2026-09-03). That call is findNearestPoly + "a polygon was
// found" (Engine+0x150570 tests the status bit and a nonzero ref, nothing else), and Detour's findNearestPoly
// gathers candidates by BOUNDING-BOX overlap and returns the nearest without bounding its distance -- so a
// point inside a hole still "finds" the polygon around it and reads walkable whenever that polygon's box covers
// the hole. Live case: Burrwitch Outskirts (-459.5, -951.5), a rock pile at (-460.6, -953.6) carved a ~6 u hole
// the pathfinder refused (a 1.5 u request north snapped back to the player's feet) while the point probe read
// every cell of it walkable, so north stayed silent and WASD north did nothing. Holes narrower than the
// surrounding polygons (the checkerboard caps them at 8 u) were invisible to the old probe; the raycast sees
// polygon edges, not boxes. Lanes (`lateral` != 0) are gated by FindClosestPointOnPathMesh, the containment test
// IsPointOnPathMesh should have been: a lane start whose closest mesh point is not itself is inside a wall and
// reads 0. Results: 1 = ok, 3 = start off the mesh (reads 0); any other code reads 0 and is logged once.
namespace {
int seh_straight_move(void* nav, const void* from, const void* to, void* out) {
  __try { return g_api.FindStraightMovePoint(nav, from, to, out); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return -1000; }
}
int seh_closest_point(void* nav, const void* from, void* out, float radius) {
  __try { return g_api.FindClosestPointOnPathMesh(nav, from, out, radius); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return -1000; }
}
// A WorldVec3 (in the player's region) for an absolute world point, given the player's own WorldVec3 as the base.
void world_vec_at(const Vec3& world_point, const Buf& base, void* region, Buf& out) {
  Vec3 wb = world_pos_of(base);
  const Vec3* rb = g_api.WorldVec3_GetRegionPosition(&base);
  Vec3 rel{world_point.x - wb.x + rb->x, world_point.y - wb.y + rb->y, world_point.z - wb.z + rb->z};
  memset(&out, 0, sizeof out);
  g_api.WorldVec3_ctor(&out, region, &rel);
}
constexpr float kLaneStartTol = 0.2f;   // a lane start farther than this from its closest mesh point is in a wall
}
// Containment on the path mesh: the point, floored, must be within kLaneStartTol of its closest mesh point (the
// lane gate of free_distance_ray; IsPointOnPathMesh is a bounding-box test and reads holes as walkable).
bool mesh_contains(const Vec3& world_point) {
  void* nav = g_api.NavManager_Get ? g_api.NavManager_Get() : nullptr;
  Buf base; void* region = nullptr;
  if (!nav || !g_api.FindClosestPointOnPathMesh || !g_api.WorldVec3_ctor || !player_world_vec(base, &region)) return false;
  Buf from; world_vec_at(world_point, base, region, from);
  if (g_api.WorldVec3_PutOnFloor) g_api.WorldVec3_PutOnFloor(&from);
  Buf closest = from;
  if (seh_closest_point(nav, &from, &closest, 1.0f) != 1) return false;
  Vec3 c = world_pos_of(closest), s = world_pos_of(from);
  float dx = c.x - s.x, dz = c.z - s.z;
  return dx * dx + dz * dz <= kLaneStartTol * kLaneStartTol;
}
float free_distance_ray(float dir_x, float dir_z, float lateral, float max_dist, Vec3* hit_world) {
  void* nav = g_api.NavManager_Get ? g_api.NavManager_Get() : nullptr;
  Buf base; void* region = nullptr;
  if (!nav || !g_api.FindStraightMovePoint || !g_api.WorldVec3_ctor || !g_api.WorldVec3_GetRegionPosition ||
      max_dist <= 0 || !player_world_vec(base, &region))
    return 0;
  Vec3 p = world_pos_of(base);
  Vec3 s{p.x - dir_z * lateral, p.y, p.z + dir_x * lateral};
  Buf from = base;
  if (lateral != 0.0f) {
    world_vec_at(s, base, region, from);
    if (g_api.WorldVec3_PutOnFloor) g_api.WorldVec3_PutOnFloor(&from);
    if (g_api.FindClosestPointOnPathMesh) {   // containment gate: the closest mesh point must be the start itself
      Buf closest = from;
      if (seh_closest_point(nav, &from, &closest, 1.0f) != 1) return 0;
      Vec3 c = world_pos_of(closest);
      float ddx = c.x - s.x, ddz = c.z - s.z;
      if (ddx * ddx + ddz * ddz > kLaneStartTol * kLaneStartTol) return 0;
    }
    s = world_pos_of(from);
  }
  Vec3 e{s.x + dir_x * max_dist, s.y, s.z + dir_z * max_dist};
  Buf to; world_vec_at(e, base, region, to);
  Buf out = from;
  int r = seh_straight_move(nav, &from, &to, &out);
  if (r != 1) {
    if (r != 3) { static int logged = 0; if (logged++ < 5) log::writef("world: FindStraightMovePoint returned {}", r); }
    return 0;
  }
  Vec3 o = world_pos_of(out);
  if (hit_world) *hit_world = o;
  float ddx = o.x - s.x, ddz = o.z - s.z;
  float d = std::sqrt(ddx * ddx + ddz * ddz);
  return d > max_dist ? max_dist : d;
}

// dev: the vertical window PutOnFloor+IsPointOnPathMesh accepts at the player's feet -- sweep on_navmesh at
// (x, foot_y + dy, z) and report the dy range that still reads on-mesh. How far a flat-y ray can be off before
// it loses the floor.
std::string nav_vwindow(float span, float step) {
  Vec3 p; if (!player_position(p)) return "no player\n";
  if (step < 0.05f) step = 0.5f;
  float lo = 1e9f, hi = -1e9f;
  std::string hits;
  for (float dy = -span; dy <= span + 1e-4f; dy += step) {
    bool ok = on_navmesh(Vec3{p.x, p.y + dy, p.z});
    if (ok) { lo = std::min(lo, dy); hi = std::max(hi, dy); }
  }
  if (hi < lo) return std::format("foot=({:.2f},{:.2f},{:.2f}) NO on-mesh dy in +-{:.0f}\n", p.x, p.y, p.z, span);
  return std::format("foot=({:.2f},{:.2f},{:.2f}) on-mesh dy in [{:+.1f}, {:+.1f}] (window {:.1f} up, {:.1f} down)\n",
                     p.x, p.y, p.z, lo, hi, hi, -lo);
}

// dev: A/B the flat-y vs terrain-following ray in `dirs` directions around the compass. Prints per-direction
// flat and follow free distance and flags where they diverge (the phantom-wall spots the fix cures).
std::string wall_compare(int dirs, float max_dist, float step) {
  Vec3 p; if (!player_position(p)) return "no player\n";
  if (dirs < 4) dirs = 16;
  float yaw = camera_yaw();
  std::string out = std::format("at ({:.1f},{:.1f},{:.1f}) yaw={:.3f} region '{}' max={:.0f} step={:.2f}\n",
                                p.x, p.y, p.z, yaw, region_name(), max_dist, step);
  int diffs = 0;
  for (int i = 0; i < dirs; ++i) {
    float a = 6.2831853f * i / dirs;
    float dx = std::sin(a), dz = std::cos(a);   // bearing 0 = +z, clockwise; absolute (not camera-relative)
    float flat = free_distance_ex(dx, dz, max_dist, step, false);
    float follow = free_distance_ex(dx, dz, max_dist, step, true);
    bool diff = std::fabs(flat - follow) > step * 0.5f;
    if (diff) ++diffs;
    out += std::format("  {:3.0f} deg (dx={:+.2f},dz={:+.2f}): flat={:5.1f} follow={:5.1f}{}\n",
                       a * 57.2958f, dx, dz, flat, follow, diff ? "   <-- DIFF" : "");
  }
  out += std::format("{} of {} directions differ\n", diffs, dirs);
  return out;
}

// Dev: the layout of RTTI_ClassInfo beyond the name -- is there a parent pointer? Dumps the static infos.
std::string classinfo_dump() {
  std::string out;
  struct { const char* n; const void* (*f)(); } infos[] = {{"Entity", g_api.Entity_StaticClassInfo}, {"Character", g_api.Character_StaticClassInfo}, {"Monster", g_api.Monster_StaticClassInfo},
                                                         {"Npc", g_api.Npc_StaticClassInfo}, {"Player", g_api.Player_StaticClassInfo}};
  for (auto& i : infos) {
    const void* ci = i.f ? i.f() : nullptr;
    out += std::format("{} ci={} name='{}'", i.n, ci, rtti_name(ci));
    for (size_t off = 0x10; off <= 0x30 && ci; off += 8) {
      const void* q; memcpy(&q, (const char*)ci + off, sizeof q);
      out += std::format(" +{:#x}={}", off, q);
      if (q && !IsBadReadPtr(q, 16)) { std::string n = rtti_name(q); if (n != "?") out += std::format("('{}')", n); }
    }
    out += '\n';
  }
  return out;
}
std::string debug_dump() {
  load_api();
  std::string s = std::format("game_engine={} controller={} world={} engine_ticks={} controller_ticks={} nav={} engine={} paused={}\n", g_game_engine, g_controller, g_world,
                              g_engine_ticks, g_controller_ticks, g_api.NavManager_Get ? g_api.NavManager_Get() : nullptr, gd::hooks::engine_object(),
                              g_api.IsGameTimePaused ? g_api.IsGameTimePaused() : false);
  void* p = player();
  s += std::format("player={} name='{}' region='{}' area='{}' life={:.1f}/{:.1f} camera_yaw={:.4f}\n", p, player_name(), region_name(), area_name(), life(), life_max(), camera_yaw());
  if (!p) return s;
  // NEVER class_name()/rtti_of() a Region: the Object::GetRTTIClassInfo slot is 0 here, which on a Region's
  // vtable is the virtual destructor -- doing so destroyed the live region and crashed the render (2026-08-25).
  { Buf b; void* region = nullptr;
    if (player_world_vec(b, &region) && region) s += std::format("region={}\n", region); }
  Vec3 w;
  if (player_position(w)) s += std::format("player world=({:.2f}, {:.2f}, {:.2f}) on_navmesh={} id={} class={}\n", w.x, w.y, w.z, on_navmesh(w),
                                           g_api.Object_GetObjectId ? g_api.Object_GetObjectId(p) : 0, class_name(p));
  const char* names[] = {"+x", "-x", "+z", "-z"};
  const float dirs[][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (int i = 0; i < 4; ++i) s += std::format("free {}: {:.1f}\n", names[i], free_distance(dirs[i][0], dirs[i][1], 15.0f, 0.5f));
  for (int i = 0; i < 4; ++i) s += std::format("ray {}: {:.1f}\n", names[i], free_distance_ray(dirs[i][0], dirs[i][1], 0.0f, 15.0f, nullptr));
  // RTTI_ClassInfo layout discovery: the player's class info vs the exported static infos, word by word.
  if (g_api.Object_GetRTTIClassInfo) {
    const void* ci = g_api.Object_GetRTTIClassInfo(p);
    s += std::format("rtti: player={} Player::static={} Character::static={} Entity::static={}\n", ci, g_api.Player_StaticClassInfo ? g_api.Player_StaticClassInfo() : nullptr,
                     g_api.Character_StaticClassInfo ? g_api.Character_StaticClassInfo() : nullptr, g_api.Entity_StaticClassInfo ? g_api.Entity_StaticClassInfo() : nullptr);
    for (const void* info : {ci, g_api.Player_StaticClassInfo ? g_api.Player_StaticClassInfo() : nullptr}) {
      if (!info) continue;
      s += std::format("  {}:", info);
      for (int i = 0; i < 8; ++i) {
        uint64_t w; memcpy(&w, (const char*)info + i * 8, 8);
        const char* str = (const char*)w;
        bool text = w > 0x10000 && !IsBadStringPtrA(str, 64) && str[0] > 0x20 && str[0] < 0x7f;
        s += text ? std::format(" '{}'", std::string(str).substr(0, 40)) : std::format(" {:#x}", w);
      }
      s += "\n";
    }
  }
  return s;
}

// ---- targeting ----
namespace { std::string entity_label(void* e, const void* ci, const std::string& cls); }
namespace { bool is_of_interest(const void* e, const void* ci); bool is_kind_of(const void* ci, const void* base); int vt_slot(void** vt, const void* fn); }
namespace {
// Frames since the render preload last stamped the entity (Entity::InRenderPreLoadFrustum: entity+0x17c vs
// gEngine+0x640, read 2026-08-25); -1 without an engine, -2 on a fault.
long long render_age(const void* e) {
  const void* eng = gd::hooks::engine_object();
  if (!eng) return -1;
  __try { return (long long)*(const unsigned*)((const char*)eng + 0x640) - (long long)*(const unsigned*)((const char*)e + 0x17c); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}
}
std::string entities_dump(float max_dist, bool frustum) {
  load_api();
  Vec3 me; if (!player_position(me)) return "no player\n";
  alignas(16) unsigned char vb[64] = {};
  MemVec* v = (MemVec*)vb;
  bool ok = false;
  std::string how;
  // Source 1: the player's region asked for everything in a sphere around the player. Read from
  // Engine.dll (tools/dll_dis.py, 2026-08-21): Region::GetEntitiesInSphere forwards to World:: over all
  // regions, Sphere = {Vec3 centre, float radius} in the calling region's coordinates, and mem::vector is
  // std-like {begin, end, cap} (appended to). EntityListType 0 for now.
  Buf base; void* region = nullptr;
  if (!frustum && g_api.Region_GetEntitiesInSphere && player_world_vec(base, &region)) {
    const Vec3* rp = g_api.WorldVec3_GetRegionPosition(&base);
    struct { Vec3 c; float r; } sphere{*rp, max_dist};
    g_api.Region_GetEntitiesInSphere(region, v, &sphere, false, 0);
    ok = true; how = "Region::GetEntitiesInSphere";
  } else if (void* eng = gd::hooks::engine_object(); eng && g_api.GetEntitiesInPriorFrameFrustum) {
    ok = g_api.GetEntitiesInPriorFrameFrustum(eng, v);
    how = "Engine::GetEntitiesInPriorFrameFrustum";
  } else return "no entity source\n";
  // mem::vector layout is not exported; the raw words tell which of {begin,end,cap} / {data,size,cap} it is.
  uint64_t raw[4]; memcpy(raw, vb, sizeof raw);
  log::writef("entities: {} raw vector words {:#x} {:#x} {:#x} {:#x}", how, raw[0], raw[1], raw[2], raw[3]);
  size_t n = 0;
  if (v->begin && v->end && (uintptr_t)v->end > (uintptr_t)v->begin && (uintptr_t)v->end - (uintptr_t)v->begin < (1u << 20))
    n = (size_t)((char*)v->end - (char*)v->begin) / sizeof(void*);  // {begin, end, cap}
  struct Row { float d; std::string text; };
  std::vector<Row> rows;
  size_t faulted = 0;
  for (size_t i = 0; i < n && i < 4096; ++i) {
    void* e; memcpy(&e, (char*)v->begin + i * sizeof(void*), sizeof e);
    if (!e) continue;
    EntityRaw r{};
    if (!read_entity(e, r)) { ++faulted; log::writef("entities: #{} {} faulted while being read", i, e); continue; }
    if (!r.has_pos) continue;
    float d = std::sqrt((r.pos.x - me.x) * (r.pos.x - me.x) + (r.pos.z - me.z) * (r.pos.z - me.z));
    if (d > max_dist) continue;
    std::string cls = rtti_name(r.ci);
    // Classification readout for the review cursor's object group (FixedActor / Item + IsOfInterest).
    std::string kind;
    if (g_api.FixedActor_StaticClassInfo && is_kind_of(r.ci, g_api.FixedActor_StaticClassInfo())) kind = " [FixedActor";
    else if (g_api.Item_StaticClassInfo && is_kind_of(r.ci, g_api.Item_StaticClassInfo())) kind = " [Item";
    if (!kind.empty()) kind += is_of_interest(e, r.ci) ? " interest]" : " no-interest]";
    if (cls == "Monster" && g_api.Character_IsAlive && !g_api.Character_IsAlive(e)) kind += " [dead]";   // a corpse: not an enemy
    if (gameapi::entity_hidden(e)) kind += " [hidden]";   // Entity visibility 0: a collected placed quest item, never listed
    else if (g_api.Item_StaticClassInfo && is_kind_of(r.ci, g_api.Item_StaticClassInfo()) && !gameapi::item_passes_loot_filter(e)) kind += " [filtered]";
    // Render stamp: Entity::InRenderPreLoadFrustum() == (entity+0x17c == gEngine+0x640); the render preload
    // stamps every entity it considers with the frame counter, so age = frames since the renderer last saw it.
    long long age = render_age(e);
    rows.push_back({d, std::format("{:6.1f}  dy={:+5.1f} age={:<6} id={:<8} {:<12} label='{}' at ({:.1f}, {:.1f}, {:.1f}) {} '{}'{}", d, r.pos.y - me.y, age, r.id, cls, entity_label(e, r.ci, cls),
                                   r.pos.x, r.pos.y, r.pos.z, e, r.name, kind)});
  }
  if (faulted) log::writef("entities: {} objects faulted while being read (skipped)", faulted);
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
  std::string s = std::format("ok={} source={} returned={} within {:.0f}: {}\n", ok, how, n, max_dist, rows.size());
  for (auto& r : rows) s += r.text + "\n";
  return s;  // the vector's storage is leaked on purpose (dev route; the game's allocator owns it)
}

// ---- the aerial map's icons (map_markers) ----
// MinimapGameNugget (0xA0, read live 2026-09-11, docs/map-icons.md): +0x08 type, +0x10 the icon's own name
// (basic_string<u16>: the localized AreaDescription of a point of interest, the hero's name; empty on the
// class-driven icons), +0x50 the custom symbol texture (type 14 only; a Resource, so GetFileName names it),
// +0x58 Region*, +0x60 region-relative position. The type is the value each class's AppendDetailMapData writes
// (Game.dll static RE): 0 hero, 1 party member, 2 Npc, 3 teleporter/riftgate, 4 StaticRespawner, 5 FixedItemShrine,
// 6 AreaOfInterest, 7 NpcMerchant, 8 hero Monster, 10 NpcSkillReallocator, 11 NpcEnchanter (inventor), 12 the grave,
// 13 NpcCaravan, 14 record-driven custom symbol (mapNuggetType = Custom: barricades), 15 NpcCrafter, 16 StaticShrine,
// 17 boss Monster, 18 NpcTransmuter. Spoken with the game's own rollover words (tagMapSymbol*) where it has them.
namespace {
std::string nugget_type_name(int t) {
  auto tag = [](const char* tg) { return gd::hooks::localize(tg); };
  switch (t) {
    case 0: return tag("tagMapSymbolHero");
    case 1: return tag("tagMapSymbolGroup");
    case 2: return tag("tagMapSymbolNPC");
    case 3: return tag("tagMapSymbolRiftgate");
    case 4: return tag("tagMapSymbolRespawn");
    case 5: case 16: return tag("tagMapSymbolShrine");
    case 6: return tag("tagMapSymbolAOI");
    case 7: return tag("tagMapSymbolVendor");
    case 8: return std::string(gd::strings::kHeroMonster);
    case 10: return tag("tagMapSymbolWitch");
    case 11: return tag("tagMapSymbolInventor");
    case 12: return std::string(gd::strings::kYourGrave);
    case 13: return tag("tagMapSymbolSmuggler");
    case 14: return std::string(gd::strings::kObstacle);
    case 15: return tag("tagMapSymbolSmith");
    case 17: return std::string(gd::strings::kBossMonster);
    case 18: return std::string(gd::strings::kIllusionist);
    default: return std::string(gd::strings::kMapMarker);
  }
}
// POD read of one nugget under SEH (no C++ objects inside __try): type, the name as UTF-16 (truncated to the buffer),
// and the custom symbol texture pointer.
struct NugFields { int type; char16_t name[128]; size_t name_len; const void* texture; };
bool read_nugget_fields(const void* nug, NugFields& f) {
  __try {
    const char* b = (const char*)nug;
    f.type = *(const int*)(b + 0x08);
    const MsvcStringW* s = (const MsvcStringW*)(b + 0x10);
    const char16_t* src = s->capacity >= 8 ? s->u.ptr : s->u.buf;
    size_t n = s->size < 127 ? s->size : 127;
    f.name_len = 0;
    if (src && s->size < 4096) { for (size_t i = 0; i < n; ++i) f.name[i] = src[i]; f.name_len = n; }
    f.texture = *(const void* const*)(b + 0x50);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const char* texture_file_name(const void* tex) {
  if (!tex || !g_api.Resource_GetFileName) return nullptr;
  __try { return g_api.Resource_GetFileName(tex); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
std::string narrow_u16(const char16_t* s, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) { char16_t c = s[i]; out += c < 0x80 ? (char)c : '?'; }
  return out;
}
// The custom symbol's kind from its texture path: every shipped custom symbol is a mapsymbol_dynamicobstacle*
// (barricades, rubble walls, the Burrwitch bridge debris) -> "obstacle"; anything else reads by its file stem; an
// unreadable texture name is an unknown "marker", never an obstacle.
std::string custom_symbol_name(const void* tex, std::string* path_out) {
  const char* fn = texture_file_name(tex);
  if (!fn) return std::string(gd::strings::kMapMarker);
  std::string path(fn);
  if (path_out) *path_out = path;
  if (path.find("dynamicobstacle") != std::string::npos) return std::string(gd::strings::kObstacle);
  size_t slash = path.find_last_of("/\\"), dot = path.find_last_of('.');
  std::string stem = path.substr(slash == std::string::npos ? 0 : slash + 1, dot == std::string::npos || dot < slash ? std::string::npos : dot - (slash == std::string::npos ? 0 : slash + 1));
  return stem.empty() ? std::string(gd::strings::kMapMarker) : stem;
}
}  // namespace

std::vector<MapMarker> map_markers() {
  load_api();
  std::vector<MapMarker> out;
  Vec3 me;
  if (!player_position(me)) return out;
  void* begin = nullptr;
  size_t count = 0;
  if (!gd::exe_ui::aerial_nugget_span(begin, count)) return out;   // the aerial map is not open / not populated

  // First pass: read the nuggets (type, own name, symbol, world position), dropping the hero marker.
  struct Raw { int type; std::string name; std::string symbol; Vec3 pos; float dist; };
  std::vector<Raw> raws;
  float maxd = 0;
  for (size_t i = 0; i < count; ++i) {
    char* nug = (char*)begin + i * 0xA0;
    NugFields f{};
    if (!read_nugget_fields(nug, f) || f.type == 0) continue;   // 0 = the hero (player) marker
    Vec3 pos;
    if (!world_point(nug + 0x58, pos)) continue;
    float d = std::sqrt((pos.x - me.x) * (pos.x - me.x) + (pos.z - me.z) * (pos.z - me.z));
    std::string name = f.name_len ? gd::strings::strip_markup(narrow_u16(f.name, f.name_len)) : std::string();   // "{^b}..." colour codes
    std::string symbol;
    if (name.empty() && f.type == 14) name = custom_symbol_name(f.texture, &symbol);
    raws.push_back({f.type, std::move(name), std::move(symbol), pos, d});
    if (d > maxd) maxd = d;
  }

  // Index nearby entities so each icon can be named (the nugget carries no id/name; the icon sits on its
  // entity). Use the region sphere query (entities_dump's primary source), sized to reach the icons.
  struct Ent { Vec3 pos; unsigned id; std::string label; };
  std::vector<Ent> ents;
  {
    Buf base;
    void* region = nullptr;
    if (g_api.Region_GetEntitiesInSphere && g_api.WorldVec3_GetRegionPosition && player_world_vec(base, &region)) {
      float radius = maxd + 15.0f;
      if (radius < 60.0f) radius = 60.0f;
      if (radius > 220.0f) radius = 220.0f;
      const Vec3* rp = g_api.WorldVec3_GetRegionPosition(&base);
      struct { Vec3 c; float r; } sphere{*rp, radius};
      alignas(16) unsigned char vb[64] = {};
      MemVec* v = (MemVec*)vb;
      g_api.Region_GetEntitiesInSphere(region, v, &sphere, false, 0);
      size_t n = 0;
      if (v->begin && v->end && (uintptr_t)v->end > (uintptr_t)v->begin && (uintptr_t)v->end - (uintptr_t)v->begin < (1u << 20))
        n = (size_t)((char*)v->end - (char*)v->begin) / sizeof(void*);
      for (size_t i = 0; i < n && i < 4096; ++i) {
        void* e;
        memcpy(&e, (char*)v->begin + i * sizeof(void*), sizeof e);
        if (!e) continue;
        EntityRaw r{};
        if (!read_entity(e, r) || !r.has_pos) continue;
        std::string lbl = entity_label(e, r.ci, rtti_name(r.ci));
        if (!lbl.empty()) ents.push_back({r.pos, r.id, std::move(lbl)});
      }
    }
  }

  for (const Raw& rw : raws) {
    const Vec3& pos = rw.pos;
    MapMarker m{};
    m.type = rw.type;
    m.symbol = rw.symbol;
    m.pos = pos;
    m.dist = rw.dist;
    float best = 4.0f;
    const Ent* he = nullptr;
    for (const Ent& e : ents) {
      float d = std::sqrt((e.pos.x - pos.x) * (e.pos.x - pos.x) + (e.pos.z - pos.z) * (e.pos.z - pos.z));
      if (d < best) { best = d; he = &e; }
    }
    // Label: the icon's own name (points of interest carry theirs), else the entity the icon sits on (a merchant's or
    // NPC's name), else the kind in the game's own rollover words.
    if (!rw.name.empty()) m.label = rw.name;
    else if (he) m.label = he->label;
    else m.label = nugget_type_name(rw.type);
    if (he) m.id = he->id;
    m.quest = rw.type == 6;   // a point of interest: the quest-bound ones exist only while their task is active
    if (m.label.empty()) m.label = std::string(gd::strings::kMapMarker);
    out.push_back(std::move(m));
  }
  std::sort(out.begin(), out.end(), [](const MapMarker& a, const MapMarker& b) { return a.dist < b.dist; });
  return out;
}

std::string map_markers_dump() {
  std::vector<MapMarker> ms = map_markers();
  std::string s = std::format("{} map markers\n", ms.size());
  for (const MapMarker& m : ms)
    s += std::format("  {:6.1f}  type={:<3} {:<14} '{}' id={} at ({:.1f},{:.1f},{:.1f}){}{}\n", m.dist, m.type, nugget_type_name(m.type), m.label, m.id, m.pos.x, m.pos.y, m.pos.z, m.quest ? " [poi]" : "", m.symbol.empty() ? "" : " symbol=" + m.symbol);
  return s;
}
bool set_target(unsigned id) {
  if (!g_controller || !g_api.SetCombatEnemy) return false;
  g_api.SetCombatEnemy(g_controller, id);
  if (g_api.FaceTarget) g_api.FaceTarget(g_controller, id);
  return true;
}
void clear_target() { if (g_controller && g_api.ClearTarget) g_api.ClearTarget(g_controller); }
unsigned current_target() { return g_controller && g_api.GetCombatEnemy ? g_api.GetCombatEnemy(g_controller) : 0; }
std::string target_dump() {
  std::string s = std::format("combat_enemy={}\n", current_target());
  void* p = player();
  if (p && g_api.GetCurrentAttackTarget) {
    unsigned id = 0, extra = 0; Buf wv{};
    g_api.GetCurrentAttackTarget(p, &id, &wv, &extra);
    s += std::format("attack_target id={} at {} extra={}\n", id, wv_text(&wv), extra);
  }
  return s;
}

// ---- the virtual cursor ----
// A target of ours is the cursor parked over the entity on screen each frame (the exe re-resolves its
// combat enemy/ally from the cursor every frame). The entity is re-found by id in a sphere query around the
// player on every tick, so a despawned target simply unlocks.
namespace {
unsigned g_locked_id = 0;
void* g_locked_entity = nullptr;
uint64_t g_lock_frames = 0;
// Grace (2026-09-20, the user's kiting report): find_entity is a 40 u sphere query around the player, so a target
// that ran (or was kited) out of it for a moment was unlocked at once, and the next enemy key started over from
// the nearest thing. Now the lock survives kLockGraceMs unseen (cursor override off, "too far away" on the keys)
// and resumes when the id is found again; a death is still immediate once the corpse is in range.
ULONGLONG g_lock_lost_ms = 0;   // GetTickCount64 of the first frame the id was not found, 0 while found
constexpr ULONGLONG kLockGraceMs = 5000;

void* find_entity(unsigned id) {
  Buf base; void* region = nullptr;
  if (!g_api.Region_GetEntitiesInSphere || !player_world_vec(base, &region)) return nullptr;
  alignas(16) unsigned char vb[64] = {};
  MemVec* v = (MemVec*)vb;
  const Vec3* rp = g_api.WorldVec3_GetRegionPosition(&base);
  struct { Vec3 c; float r; } sphere{*rp, 40.0f};
  g_api.Region_GetEntitiesInSphere(region, v, &sphere, false, 0);
  if (!(v->begin && v->end && (uintptr_t)v->end > (uintptr_t)v->begin && (uintptr_t)v->end - (uintptr_t)v->begin < (1u << 20))) return nullptr;
  size_t n = (size_t)((char*)v->end - (char*)v->begin) / sizeof(void*);
  for (size_t i = 0; i < n && i < 4096; ++i) {
    void* e; memcpy(&e, (char*)v->begin + i * sizeof(void*), sizeof e);
    if (e && g_api.Object_GetObjectId && g_api.Object_GetObjectId(e) == id) return e;
  }
  return nullptr;
}
struct BBoxRaw { float v[6]; bool ok; };
bool read_bbox(void* e, BBoxRaw& b);
bool project(void* entity, float& x, float& y) {
  void* cam = g_game_engine && g_api.GetCamera ? g_api.GetCamera(g_game_engine) : nullptr;
  if (!cam || !g_api.Project || !g_api.Viewport_ctor) return false;
  Buf wv; if (!entity_world_vec(entity, wv)) return false;
  { void* rgn = nullptr; memcpy(&rgn, wv.b, sizeof rgn); if (!rgn) return false; }   // WorldCamera::Project derefs the region (a Lua-spawned item had none)
  // Park the cursor at the centre of the entity's bounding box: mid-body for a character, the item itself for
  // loot lying on the ground (a fixed +1.0 lift put the cursor a metre above ground items, the click landed on
  // the ground and "attack here" fired instead of a pickup -- the user, 2026-08-22). The box is region-relative
  // like the position; a box that is missing or implausible falls back to the chest-height lift.
  // ABBox = {centre xyz, half-extents xyz} (measured 2026-08-22: a vertical beam standing at y 14 reports centre
  // y 18.6, extent 4.6), in the entity's region frame like its position.
  Vec3 pos; memcpy(&pos, wv.b + 8, sizeof pos);
  BBoxRaw bb{};
  Vec3 at{pos.x, pos.y + 1.0f, pos.z};
  if (read_bbox(entity, bb) && bb.ok) {
    float dx = bb.v[0] - pos.x, dy = bb.v[1] - pos.y, dz = bb.v[2] - pos.z;
    if (dx > -3.0f && dx < 3.0f && dz > -3.0f && dz < 3.0f && dy > -1.0f && dy < 3.0f) at = Vec3{bb.v[0], bb.v[1] < pos.y + 0.1f ? pos.y + 0.1f : bb.v[1], bb.v[2]};
  }
  memcpy(wv.b + 8, &at, sizeof at);
  RECT rc{};
  HWND w = FindWindowA("Grim Dawn", nullptr);
  if (!w || !GetClientRect(w, &rc)) return false;
  Buf vp{};
  g_api.Viewport_ctor(&vp, 0, 0, rc.right, rc.bottom);
  alignas(16) float out[4] = {};
  g_api.Project(cam, out, &wv, &vp);
  x = out[0]; y = out[1];
  return true;
}
}  // namespace

// Camera line of sight to an entity, the sighted player's own primitive (static RE of the exe's cursor pick at
// exe+0x30c49, 2026-08-25): the ray the camera sends through the entity's screen point
// (WorldCamera::GetRayThroughImagePoint), cast with World::GetIntersection (surface 0, entities skipped, max +inf,
// last bool true as the exe passes it) -- if level geometry stops it short of the entity, the entity is hidden
// behind a wall / under a floor. World::GetAllIntersections lists the entities the ray crosses on the way
// (the meshes the renderer would fade). WorldIntersection = {float dist +0, Region* +8, Vec3 point +0x10,
// Entity* +0x20, int surface +0x28}, 0x30 bytes; surface 100 = nothing hit.
namespace {
bool seh_world_intersection(const void* ray, void* hit, bool skip_entities) {
  __try { g_api.World_GetIntersection(g_world, ray, hit, 0, skip_entities, std::numeric_limits<float>::infinity(), true); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_all_intersections(const void* ray, void* vec, float max_dist) {
  __try { g_api.World_GetAllIntersections(g_world, ray, vec, false, max_dist); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
std::string los_dump(unsigned id) {
  load_api();
  void* cam = g_game_engine && g_api.GetCamera ? g_api.GetCamera(g_game_engine) : nullptr;
  if (!cam || !g_world || !g_api.Project || !g_api.Viewport_ctor || !g_api.GetRayThroughImagePoint || !g_api.World_GetIntersection) return "los: api missing\n";
  void* e = find_entity(id);
  if (!e) return "entity not found near the player\n";
  float sx, sy;
  if (!project(e, sx, sy)) return "cannot project\n";
  RECT rc{};
  HWND w = FindWindowA("Grim Dawn", nullptr);
  if (!w || !GetClientRect(w, &rc)) return "no window\n";
  Buf vp{};
  g_api.Viewport_ctor(&vp, 0, 0, rc.right, rc.bottom);
  // Image points are viewport FRACTIONS (0..1): the exe's pick divides the cursor by the viewport's width/height
  // before this call (exe+0x30b8d..0x30ba5), and Camera::GetImagePoint takes them that way.
  alignas(16) float pt[2] = {sx / (float)rc.right, sy / (float)rc.bottom};
  Buf ray{};
  g_api.GetRayThroughImagePoint(cam, &ray, pt, &vp);
  void* ray_region = nullptr; memcpy(&ray_region, ray.b, sizeof ray_region);
  Vec3 origin_rel, dir; memcpy(&origin_rel, ray.b + 8, sizeof origin_rel); memcpy(&dir, ray.b + 0x18, sizeof dir);
  // The entity's position in world coords and its distance from the ray origin (the bbox-centre aim point
  // project() used differs by at most the box's half-height; the 0.5 slack below covers it).
  Buf wv; if (!entity_world_vec(e, wv)) return "no entity position\n";
  Vec3 target = world_pos_of(wv);
  Buf ow{}; memcpy(ow.b, &ray_region, sizeof ray_region); memcpy(ow.b + 8, &origin_rel, sizeof origin_rel);
  Vec3 origin = world_pos_of(ow);
  float tdist = std::sqrt((target.x - origin.x) * (target.x - origin.x) + (target.y - origin.y) * (target.y - origin.y) + (target.z - origin.z) * (target.z - origin.z));
  std::string s = std::format("los id={} screen=({:.0f},{:.0f}) ray origin=({:.1f},{:.1f},{:.1f}) dir=({:.3f},{:.3f},{:.3f}) region={} target=({:.1f},{:.1f},{:.1f}) tdist={:.1f}\n",
                              id, sx, sy, origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, ray_region, target.x, target.y, target.z, tdist);
  for (int pass = 0; pass < 2; ++pass) {
    alignas(16) unsigned char hit[0x40] = {};
    if (!seh_world_intersection(&ray, hit, pass == 0)) { s += "  GetIntersection faulted\n"; continue; }
    float d; memcpy(&d, hit, sizeof d);
    void* hr; memcpy(&hr, hit + 8, sizeof hr);
    Vec3 p; memcpy(&p, hit + 0x10, sizeof p);
    void* he; memcpy(&he, hit + 0x20, sizeof he);
    int surf; memcpy(&surf, hit + 0x28, sizeof surf);
    std::string hit_desc;
    if (he) { EntityRaw r{}; if (read_entity(he, r)) hit_desc = std::format(" entity id={} {} '{}'", r.id, rtti_name(r.ci), r.name); }
    s += std::format("  {}: dist={:.2f} surface={} region={} point=({:.1f},{:.1f},{:.1f}){} -> {}\n", pass == 0 ? "geometry only" : "with entities",
                     d, surf, hr, p.x, p.y, p.z, hit_desc, d < tdist - 0.5f ? "BLOCKED before the target" : "clear to the target");
  }
  if (g_api.World_GetAllIntersections) {
    alignas(16) unsigned char vb[64] = {};
    MemVec* v = (MemVec*)vb;
    if (!seh_all_intersections(&ray, v, tdist + 0.5f)) { s += "  GetAllIntersections faulted\n"; return s; }
    size_t n = 0;
    if (v->begin && v->end && (uintptr_t)v->end > (uintptr_t)v->begin && (uintptr_t)v->end - (uintptr_t)v->begin < (1u << 20))
      n = (size_t)((char*)v->end - (char*)v->begin) / sizeof(void*);
    s += std::format("  entities crossed within {:.1f}: {}\n", tdist + 0.5f, n);
    for (size_t i = 0; i < n && i < 64; ++i) {
      void* ce; memcpy(&ce, (char*)v->begin + i * sizeof(void*), sizeof ce);
      EntityRaw r{}; if (!ce || !read_entity(ce, r)) continue;
      s += std::format("    id={} {} '{}' at ({:.1f},{:.1f},{:.1f})\n", r.id, rtti_name(r.ci), r.name, r.pos.x, r.pos.y, r.pos.z);
    }
  }
  return s;
}

// A bare world point (a room exit) projected through the player's region like on_navmesh does.
namespace {
bool g_point_locked = false;
Vec3 g_locked_point;
// The free cursor (Shift+WASD): a bare world point that overrides the lock's projection while it is set. The lock
// (g_locked_id / g_point_locked) is left as it was, so the reviewed thing stays reviewed and resumes as the cursor
// when the free point is dropped (any landing, unlock_target).
bool g_free_cursor = false;
Vec3 g_free_point;
gd::core::XZ g_free_heading{};   // the polar line's direction, kept while the cursor sits on the player
Vec3 g_lock_last_pos;            // the locked entity's position the last frame it was found
bool g_lock_last_pos_ok = false;
gd::core::CursorMode g_cursor_mode = gd::core::CursorMode::Grid;
bool g_cursor_mode_loaded = false;
constexpr const char* kCursorModeKey = "cursor.polar";
bool project_point(const Vec3& world_point, float& x, float& y) {
  void* cam = g_game_engine && g_api.GetCamera ? g_api.GetCamera(g_game_engine) : nullptr;
  Buf base; void* region = nullptr;
  if (!cam || !g_api.Project || !g_api.Viewport_ctor || !g_api.WorldVec3_ctor || !player_world_vec(base, &region)) return false;
  Vec3 wb = world_pos_of(base);
  const Vec3* rb = g_api.WorldVec3_GetRegionPosition(&base);
  Vec3 rel{world_point.x - wb.x + rb->x, world_point.y - wb.y + rb->y + 1.0f, world_point.z - wb.z + rb->z};
  Buf wv{};
  g_api.WorldVec3_ctor(&wv, region, &rel);
  RECT rc{};
  HWND w = FindWindowA("Grim Dawn", nullptr);
  if (!w || !GetClientRect(w, &rc)) return false;
  Buf vp{};
  g_api.Viewport_ctor(&vp, 0, 0, rc.right, rc.bottom);
  alignas(16) float out[4] = {};
  g_api.Project(cam, out, &wv, &vp);
  x = out[0]; y = out[1];
  return true;
}
}  // namespace

bool lock_target(unsigned id) {
  void* e = find_entity(id);
  if (!e) return false;
  g_point_locked = false; g_free_cursor = false;
  g_locked_id = id; g_locked_entity = e; g_lock_frames = 0; g_lock_lost_ms = 0; g_lock_last_pos_ok = false;
  return true;
}
bool lock_point(const Vec3& world_point) {
  if (!in_world()) return false;
  g_locked_id = 0; g_locked_entity = nullptr; g_free_cursor = false;
  g_point_locked = true; g_locked_point = world_point;
  return true;
}
void unlock_target() {
  if (g_locked_id || g_point_locked || g_free_cursor) gd::hooks::set_cursor_override(false, 0, 0);
  g_locked_id = 0; g_locked_entity = nullptr; g_point_locked = false; g_free_cursor = false; g_lock_lost_ms = 0;
}
unsigned locked_target() { return g_locked_id; }
std::string lock_dump() {
  std::string s = g_free_cursor ? std::format("free cursor at {:.2f},{:.2f},{:.2f}\n", g_free_point.x, g_free_point.y, g_free_point.z) : "";
  if (!g_locked_id) return s + (g_point_locked ? "locked point\n" : "no lock\n");
  if (!g_lock_lost_ms) return s + std::format("locked id={} found\n", g_locked_id);
  return s + std::format("locked id={} NOT FOUND for {} ms (grace {} ms)\n", g_locked_id, GetTickCount64() - g_lock_lost_ms, kLockGraceMs);
}
// ---- the free cursor ----
namespace {
void screen_axes(float& fx, float& fz, float& rx, float& rz);   // defined with the review cursor below
void load_cursor_mode() {
  if (g_cursor_mode_loaded) return;
  g_cursor_mode_loaded = true;
  g_cursor_mode = gd::settings::get_bool(kCursorModeKey, false) ? gd::core::CursorMode::Polar : gd::core::CursorMode::Grid;
}
}  // namespace
bool free_cursor_step(gd::core::CursorKey key) {
  if (!in_world()) return false;
  Vec3 me;
  if (!player_position(me)) return false;
  load_cursor_mode();
  if (!g_free_cursor) {   // seed from wherever the cursor is now
    Vec3 seed = me;
    if (g_point_locked) seed = g_locked_point;
    else if (g_locked_id) { Vec3 p; if (entity_position(g_locked_id, p)) seed = p; }
    g_free_point = seed; g_free_cursor = true; g_free_heading = {};
  }
  float fx, fz, rx, rz;
  screen_axes(fx, fz, rx, rz);
  gd::core::XZ n = gd::core::step_cursor(g_cursor_mode, key, {me.x, me.z}, {g_free_point.x, g_free_point.z}, {fx, fz}, {rx, rz}, &g_free_heading);
  g_free_point = Vec3{n.x, floor_y_at(n.x, n.z, g_free_point.y), n.z};
  return true;
}
bool free_cursor_active() { return g_free_cursor; }
gd::core::CursorMode cursor_mode() { load_cursor_mode(); return g_cursor_mode; }
std::string toggle_cursor_mode() {
  load_cursor_mode();
  g_cursor_mode = g_cursor_mode == gd::core::CursorMode::Grid ? gd::core::CursorMode::Polar : gd::core::CursorMode::Grid;
  gd::settings::set_bool(kCursorModeKey, g_cursor_mode == gd::core::CursorMode::Polar);
  gd::core::MessageBuilder m;
  gd::strings::push_cursor_mode(m, g_cursor_mode == gd::core::CursorMode::Polar);
  return m.build();
}
std::string free_cursor_dump() {
  load_cursor_mode();
  std::string s = std::format("mode={} active={}\n", g_cursor_mode == gd::core::CursorMode::Polar ? "polar" : "grid", g_free_cursor);
  if (!g_free_cursor) return s;
  Vec3 me; float x = 0, y = 0;
  bool ok = project_point(g_free_point, x, y);
  s += std::format("point {:.2f},{:.2f},{:.2f}", g_free_point.x, g_free_point.y, g_free_point.z);
  if (player_position(me)) {
    float dx = g_free_point.x - me.x, dz = g_free_point.z - me.z;
    s += std::format(" dist={:.2f} clock={}", std::sqrt(dx * dx + dz * dz), clock_hour(g_free_point));
  }
  s += ok ? std::format(" screen={:.0f},{:.0f}\n", x, y) : " (projection failed)\n";
  return s;
}
void tick() {
  if (g_free_cursor) {
    if (!in_world()) { unlock_target(); return; }
    float x, y; RECT rc{};
    HWND w = FindWindowA("Grim Dawn", nullptr);
    bool visible = project_point(g_free_point, x, y) && w && GetClientRect(w, &rc) && x >= 0 && y >= 0 && x < (float)rc.right && y < (float)rc.bottom;
    gd::hooks::set_cursor_override(visible, x, y);
    return;
  }
  if (g_point_locked) {
    if (!in_world()) { unlock_target(); return; }
    float x, y; RECT rc{};
    HWND w = FindWindowA("Grim Dawn", nullptr);
    bool visible = project_point(g_locked_point, x, y) && w && GetClientRect(w, &rc) && x >= 0 && y >= 0 && x < (float)rc.right && y < (float)rc.bottom;
    gd::hooks::set_cursor_override(visible, x, y);
    return;
  }
  if (!g_locked_id) return;
  if (!in_world()) { unlock_target(); return; }
  // Re-find EVERY frame: the id is the identity, the pointer is only valid for the frame it was resolved in. A
  // 30-frame cadence let a picked-up item's freed object reach WorldCamera::Project for up to 29 frames (the rare
  // "not responding after a pickup" crash, 2026-08-30; the log showed the item's tooltip vanish, then the cursor
  // override jump to the window centre off a garbage projection). find_entity is the game's own sphere query
  // and already runs per frame elsewhere in this file.
  g_locked_entity = find_entity(g_locked_id);
  // A target that is gone -- out of the query sphere, despawned, or dead -- leaves the cursor WHERE IT WAS
  // (2026-09-20, the user's call): the last position it was seen at becomes a point lock, so J / I and the cursor
  // keys keep working from that spot instead of everything snapping to "no target". While unseen within the grace
  // the entity lock is kept (it resumes if the id turns up again) and the stale point is what the cursor shows.
  if (!g_locked_entity) {
    ULONGLONG now = GetTickCount64();
    if (!g_lock_lost_ms) g_lock_lost_ms = now;
    if (now - g_lock_lost_ms > kLockGraceMs) { if (g_lock_last_pos_ok) lock_point(g_lock_last_pos); else unlock_target(); return; }
    if (!g_lock_last_pos_ok) { gd::hooks::set_cursor_override(false, 0, 0); return; }
    float x, y; RECT rc{};
    HWND w = FindWindowA("Grim Dawn", nullptr);
    bool visible = project_point(g_lock_last_pos, x, y) && w && GetClientRect(w, &rc) && x >= 0 && y >= 0 && x < (float)rc.right && y < (float)rc.bottom;
    gd::hooks::set_cursor_override(visible, x, y);
    return;
  }
  g_lock_lost_ms = 0;
  ++g_lock_frames;
  { Buf wv; if (entity_world_vec(g_locked_entity, wv)) { g_lock_last_pos = world_pos_of(wv); g_lock_last_pos_ok = true; } }
  // A locked Monster that died is a corpse: the cursor stays on the spot as a point lock (the next enemy key still
  // enters at the nearest living one, since the corpse is no longer in the enemy scan).
  {
    EntityRaw r{};
    if (g_api.Character_IsAlive && read_entity(g_locked_entity, r) && rtti_name(r.ci) == "Monster" && !g_api.Character_IsAlive(g_locked_entity)) {
      if (g_lock_last_pos_ok) lock_point(g_lock_last_pos); else unlock_target();
      return;
    }
  }
  float x, y;
  RECT rc{};
  HWND w = FindWindowA("Grim Dawn", nullptr);
  bool visible = project(g_locked_entity, x, y) && w && GetClientRect(w, &rc) && x >= 0 && y >= 0 && x < (float)rc.right && y < (float)rc.bottom;
  // Only park the cursor on something the camera shows; off-window the override would only confuse the
  // game's hover (the lock itself stays, the readouts say "distant" / "too far away").
  gd::hooks::set_cursor_override(visible, x, y);
}
bool entity_screen_pos(unsigned id, float& x, float& y) {
  void* e = find_entity(id);
  return e && project(e, x, y);
}
// ---- blockage classification ----
namespace {
struct Nearby { void* e; unsigned id; Vec3 pos; float radius; float height; float miny, maxy; std::string cls; std::string name; };
std::vector<Nearby> g_nearby;
ULONGLONG g_nearby_at = 0;
constexpr float kNearbyRadius = 14.0f;
constexpr ULONGLONG kNearbyRefreshMs = 200;
constexpr float kWallHeight = 1.6f;   // a blocker shorter than this (units ~ metres) is an obstacle the character walks round

bool read_bbox(void* e, BBoxRaw& b) {
  __try {
    const float* bb = g_api.Entity_GetRegionBoundingBox ? g_api.Entity_GetRegionBoundingBox(e, false) : nullptr;
    if (!bb || IsBadReadPtr(bb, 24)) { b.ok = false; return true; }
    memcpy(b.v, bb, sizeof b.v); b.ok = true;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { b.ok = false; return false; }
}
void refresh_nearby() {
  ULONGLONG now = GetTickCount64();
  if (now - g_nearby_at < kNearbyRefreshMs) return;
  g_nearby_at = now;
  g_nearby.clear();
  Buf base; void* region = nullptr;
  if (!g_api.Region_GetEntitiesInSphere || !player_world_vec(base, &region)) return;
  alignas(16) unsigned char vb[64] = {};
  MemVec* v = (MemVec*)vb;
  const Vec3* rp = g_api.WorldVec3_GetRegionPosition(&base);
  struct { Vec3 c; float r; } sphere{*rp, kNearbyRadius};
  g_api.Region_GetEntitiesInSphere(region, v, &sphere, false, 0);
  if (!(v->begin && v->end && (uintptr_t)v->end > (uintptr_t)v->begin && (uintptr_t)v->end - (uintptr_t)v->begin < (1u << 20))) return;
  size_t n = (size_t)((char*)v->end - (char*)v->begin) / sizeof(void*);
  for (size_t i = 0; i < n && i < 4096; ++i) {
    void* e; memcpy(&e, (char*)v->begin + i * sizeof(void*), sizeof e);
    if (!e) continue;
    EntityRaw r{};
    if (!read_entity(e, r) || !r.has_pos) continue;
    std::string cls = rtti_name(r.ci);
    std::string name = r.name;
    if (cls == "Player" || cls == "PlayerSpawnPoint" || name.find("records/fx/") == 0 || name.find("records/triggervolumes/") == 0) continue;
    BBoxRaw bb{};
    if (!read_bbox(e, bb) || !bb.ok) continue;
    // ABBox (measured 2026-08-21): centre Vec3 then half-extents Vec3. Lights report a cube of their radius.
    if (name.find("light") != std::string::npos) continue;
    float hx = bb.v[3], hy = bb.v[4], hz = bb.v[5];
    g_nearby.push_back({e, r.id, r.pos, std::sqrt(hx * hx + hz * hz), hy * 2.0f, bb.v[1] - hy, bb.v[1] + hy, cls, name});
  }
}
const Nearby* blocker_at(const Vec3& stop) {
  refresh_nearby();
  // Render boxes of big decorations (a 20-unit rock) cover everything; among the entities whose footprint
  // reaches the stop, name the SMALLEST one -- it is the likeliest actual blocker.
  const Nearby* best = nullptr;
  for (const Nearby& nb : g_nearby) {
    float dx = nb.pos.x - stop.x, dz = nb.pos.z - stop.z;
    float d = std::sqrt(dx * dx + dz * dz) - nb.radius;  // distance to the footprint's edge
    if (d < 1.0f && (!best || nb.radius < best->radius)) best = &nb;
  }
  return best;
}
}  // namespace

// Obstacle = walkable navmesh exists a little beyond the stop along the probe direction (the character
// walks round it); wall = nothing walkable behind it within kBeyond. Tested at two distances so a thin
// post and a fat rock both read as obstacles.
constexpr float kBeyond1 = 2.0f, kBeyond2 = 4.0f;
BlockKind classify_block(const Vec3& stop, float dir_x, float dir_z) {
  Vec3 b1{stop.x + dir_x * kBeyond1, stop.y, stop.z + dir_z * kBeyond1};
  Vec3 b2{stop.x + dir_x * kBeyond2, stop.y, stop.z + dir_z * kBeyond2};
  return on_navmesh(b1) || on_navmesh(b2) ? BlockKind::Obstacle : BlockKind::Wall;
}
std::string blocks_dump() {
  Vec3 p; if (!player_position(p)) return "no player\n";
  refresh_nearby();
  std::string s = std::format("nearby={} (radius {:.0f})\n", g_nearby.size(), kNearbyRadius);
  const char* names[] = {"+x", "-x", "+z", "-z"};
  const float dirs[][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (int i = 0; i < 4; ++i) {
    float d = free_distance(dirs[i][0], dirs[i][1], 15.0f, 0.5f);
    Vec3 stop{p.x + dirs[i][0] * (d + 0.25f), p.y, p.z + dirs[i][1] * (d + 0.25f)};
    const Nearby* nb = d >= 15.0f ? nullptr : blocker_at(stop);
    const char* kind = d >= 15.0f ? "open" : classify_block(stop, dirs[i][0], dirs[i][1]) == BlockKind::Obstacle ? "OBSTACLE" : "WALL";
    s += std::format("{}: free {:.1f} {}{}", names[i], d, kind, nb ? "" : d >= 15.0f ? "" : " (no entity: level geometry)");
    if (nb) s += std::format("  <- {} h={:.2f} r={:.2f} y[{:.1f}..{:.1f}] '{}'", nb->cls, nb->height, nb->radius, nb->miny, nb->maxy, nb->name);
    s += "\n";
  }
  s += "nearest entities with boxes:\n";
  std::vector<const Nearby*> v; for (auto& nb : g_nearby) v.push_back(&nb);
  std::sort(v.begin(), v.end(), [&](const Nearby* a, const Nearby* b) { return std::hypot(a->pos.x - p.x, a->pos.z - p.z) < std::hypot(b->pos.x - p.x, b->pos.z - p.z); });
  for (size_t i = 0; i < v.size() && i < 12; ++i) {
    BBoxRaw bb{}; read_bbox(v[i]->e, bb);
    s += std::format("  {:5.1f} {:<10} at ({:.1f}, {:.1f}, {:.1f}) raw[{:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f}] '{}'\n", std::hypot(v[i]->pos.x - p.x, v[i]->pos.z - p.z), v[i]->cls,
                     v[i]->pos.x, v[i]->pos.y, v[i]->pos.z, bb.v[0], bb.v[1], bb.v[2], bb.v[3], bb.v[4], bb.v[5], v[i]->name);
  }
  return s;
}

// ---- labels ----
namespace {
// The u16 result lands in a 32-byte SSO string the callee constructs; heap storage (capacity > 7) is the
// game's CRT allocation (same MSVC CRT) and is freed here.
// GetGameDescription(bool, bool) is declared virtual on Actor (Engine.dll), so one vtable slot -- found where
// Monster's exported implementation sits in Monster's vftable -- labels every Actor the way the game's hover
// does: chests ("Rotting Corpse"), doors, shrines, barrels ("Crate"), items, monsters. Npc/Player keep their
// rollover text (the name); anything not an Actor has no label (2026-08-22; containers were "fixed item container").
int game_description_slot() {
  static int slot = -2;
  if (slot == -2) {
    slot = vt_slot(g_api.Monster_vftable, (const void*)g_api.Monster_GetGameDescription);
    log::writef("world: GetGameDescription slot: {}", slot);
  }
  return slot;
}
bool read_label(void* e, const void* ci, const std::string& cls, char16_t* out, size_t cap) {
  __try {
    alignas(16) unsigned char sb[64] = {};
    MsvcStringW* s = (MsvcStringW*)sb;
    s->capacity = 7;
    MsvcStringW* r = nullptr;
    if (cls == "Npc" && g_api.Npc_GetRolloverDescription) r = g_api.Npc_GetRolloverDescription(e, s);
    else if (cls == "Player" && g_api.Player_GetRolloverDescription) r = g_api.Player_GetRolloverDescription(e, s);
    else if (cls == "Monster" && g_api.Monster_GetGameDescription) r = g_api.Monster_GetGameDescription(e, s, false, false);
    else if (cls == "Item" && g_api.Item_GetGameDescription) r = g_api.Item_GetGameDescription(e, s, false, false);
    else if (ci && g_api.Actor_StaticClassInfo && is_kind_of(ci, g_api.Actor_StaticClassInfo()) && game_description_slot() >= 0) {
      void** vt; memcpy(&vt, e, sizeof vt);
      r = ((MsvcStringW* (*)(const void*, MsvcStringW*, bool, bool))vt[game_description_slot()])(e, s, false, false);
    }
    if (!r) return false;
    std::u16string_view v = s->view();
    size_t n = v.size() < cap - 1 ? v.size() : cap - 1;
    memcpy(out, v.data(), n * sizeof(char16_t)); out[n] = 0;
    if (s->capacity > 7 && s->u.ptr) free(s->u.ptr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string entity_label(void* e, const void* ci, const std::string& cls) {
  char16_t buf[512];
  if (!read_label(e, ci, cls, buf, 512)) return {};
  return textcap::speakable(std::u16string_view(buf));
}
}  // namespace

bool is_foe(unsigned id) {
  void* fm = g_game_engine && g_api.GetFactionManager ? g_api.GetFactionManager(g_game_engine) : nullptr;
  unsigned pid = player_id();
  return fm && g_api.FactionManager_IsFoe && pid && id && id != pid && g_api.FactionManager_IsFoe(fm, pid, id, false);
}
std::string label_of(unsigned id) {
  void* e = find_entity(id);
  if (!e) return {};
  EntityRaw r{};
  if (!read_entity(e, r)) return {};
  return entity_label(e, r.ci, rtti_name(r.ci));
}

// ---- the review cursor ----
namespace {
unsigned g_reviewed_id = 0;
// Screen-up in world xz (measured against WASD, see in_game.cpp): (-sin yaw, -cos yaw).
void screen_axes(float& fx, float& fz, float& rx, float& rz) {
  float yaw = camera_yaw();
  fx = -std::sin(yaw); fz = -std::cos(yaw); rx = std::cos(yaw); rz = -std::sin(yaw);
}
// RTTI_ClassInfo: +0 vptr, +8 name, +0x10 parent (measured live 2026-08-22: Monster/Npc/Player -> Character
// -> Actor, Entity -> Object). is-a = walk the parents.
bool is_kind_of(const void* ci, const void* base) {
  for (int depth = 0; ci && depth < 16; ++depth) {
    if (ci == base) return true;
    if (IsBadReadPtr(ci, 0x18)) return false;
    memcpy(&ci, (const char*)ci + 0x10, sizeof ci);
  }
  return false;
}
// The slot of a class's IsOfInterest in its own vftable (the export is that class's implementation).
int vt_slot(void** vt, const void* fn) {
  if (!vt || !fn) return -1;
  for (int i = 0; i < 160; ++i) if (vt[i] == fn) return i;
  return -1;
}
bool call_bool_slot(const void* obj, int slot, bool* out) {
  __try { void** vt; memcpy(&vt, obj, sizeof vt); *out = ((bool (*)(const void*))vt[slot])(obj); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool call_bool_fn(const void* obj, bool (*fn)(const void*), bool* out) {
  __try { *out = fn(obj); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool call_int_fn(const void* obj, int (*fn)(const void*), int* out) {
  __try { *out = fn(obj); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// What the game's Interact key would consider: a FixedActor (door, chest, shrine, lever ...) or an Item
// whose IsOfInterest() says so -- virtual, so dispatched through the object's vtable at the slot the class's
// exported implementation occupies in that class's vftable.
bool is_of_interest(const void* e, const void* ci) {
  static int fa_slot = -2, item_slot = -2;
  if (fa_slot == -2) {
    fa_slot = vt_slot(g_api.FixedActor_vftable, (const void*)g_api.FixedActor_IsOfInterest);
    item_slot = vt_slot(g_api.Item_vftable, (const void*)g_api.Item_IsOfInterest);
    log::writef("world: IsOfInterest slots: FixedActor {} Item {}", fa_slot, item_slot);
  }
  bool v = false;
  if (g_api.FixedActor_StaticClassInfo && is_kind_of(ci, g_api.FixedActor_StaticClassInfo())) return fa_slot >= 0 && call_bool_slot(e, fa_slot, &v) && v;
  if (g_api.Item_StaticClassInfo && is_kind_of(ci, g_api.Item_StaticClassInfo())) return item_slot >= 0 && call_bool_slot(e, item_slot, &v) && v;
  return false;
}
bool is_loot(const void* ci, const std::string& cls) {
  return (g_api.Item_StaticClassInfo && is_kind_of(ci, g_api.Item_StaticClassInfo())) || cls == "FixedItemContainer";
}
// An unbroken, targetable breakable (barrels, crates, the quest's "destroy N of X" targets); a broken one is debris.
bool is_live_destructible(const void* e, const void* ci) {
  if (!g_api.Destructible_StaticClassInfo || !is_kind_of(ci, g_api.Destructible_StaticClassInfo())) return false;
  bool broken = false, targetable = true;
  if (g_api.Destructible_IsBroken && (!call_bool_fn(e, g_api.Destructible_IsBroken, &broken) || broken)) return false;
  if (g_api.Destructible_IsTargetable && (!call_bool_fn(e, g_api.Destructible_IsTargetable, &targetable) || !targetable)) return false;
  return true;
}
bool npc_has_conversation(const void* e) { bool v = false; return g_api.Npc_HasConversation && call_bool_fn(e, g_api.Npc_HasConversation, &v) && v; }
bool is_a(const void* ci, const void* (*sf)()) { return sf && is_kind_of(ci, sf()); }
// DungeonEntrance exports no GetStaticClassInfo: walk the RTTI parent chain comparing NAMES instead.
bool is_named_kind(const void* ci, const char* name) {
  for (int depth = 0; ci && depth < 16; ++depth) {
    if (IsBadReadPtr(ci, 0x18)) return false;
    const char* n = nullptr;
    memcpy(&n, (const char*)ci + 8, sizeof n);
    if (n && !IsBadStringPtrA(n, 64) && strcmp(n, name) == 0) return true;
    memcpy(&ci, (const char*)ci + 0x10, sizeof ci);
  }
  return false;
}
bool in_group(const void* e, const void* ci, const std::string& cls, ScanGroup g) {
  // is-a checks, never exact class-name compares: Kerrick the merchant is an NpcMerchant, not an Npc, and
  // was in no group at all (found on the real save 2026-08-23). Subclasses must inherit their group.
  switch (g) {
    case ScanGroup::Enemies: return is_a(ci, g_api.Monster_StaticClassInfo);   // the scan filters IsFoe + alive after
    // N = the important non-loot things (decided 2026-08-22: one key for the one-offs -- people you can talk to,
    // rifts, shrines, doors, levers), M = loot only (items on the ground, containers), B = flavour NPCs.
    // A subclassed Npc (NpcMerchant Kerrick -- HasConversation is FALSE for merchants, the vendor window is
    // not a conversation) is specialized BECAUSE it does something important: N regardless.
    // Every DungeonEntrance is in N whatever IsOfInterest says (2026-09-04, Hanneffy Mine): the one-way exit shaft the
    // player dropped in through is locked = not of interest, yet the sonar's transition cue pings it -- N must be able
    // to name what the ear hears. scan() marks such an entrance "locked".
    case ScanGroup::Neutrals: return (is_a(ci, g_api.Npc_StaticClassInfo) && (npc_has_conversation(e) || cls != "Npc")) || (is_of_interest(e, ci) && !is_loot(ci, cls)) ||
                                     is_named_kind(ci, "DungeonEntrance");
    // B = flavour NPCs + breakables (2026-08-25: a quest asks for destructibles; no sound for them yet).
    case ScanGroup::Bystanders: return (is_a(ci, g_api.Npc_StaticClassInfo) && cls == "Npc" && !npc_has_conversation(e)) || is_live_destructible(e, ci);
    case ScanGroup::Objects:
    // Loot (the sonar sweep's group too): an Item on the ground or a container the Interact key would open.
    case ScanGroup::Loot: return is_of_interest(e, ci) && is_loot(ci, cls);
    // Transitions = dungeon entrances/exits (locked or not: the way out of a cave is still the way out).
    case ScanGroup::Transitions: return is_named_kind(ci, "DungeonEntrance");
    case ScanGroup::Destructibles: return is_live_destructible(e, ci);   // sonar: breakables only (no flavour NPCs)
    case ScanGroup::Shrines: return g_api.StaticShrine_StaticClassInfo && is_kind_of(ci, g_api.StaticShrine_StaticClassInfo());
    // Sonar: the rest of the N group -- people you can talk to (quest NPCs, merchants) and the objects the Interact
    // key would use, minus the kinds with a cue of their own (dungeon entrances, shrines; loot and breakables are
    // not in N). The user (2026-08-28): NPCs count, Harmond and Kerrick were silent.
    case ScanGroup::Interactables:
      return in_group(e, ci, cls, ScanGroup::Neutrals) && !is_named_kind(ci, "DungeonEntrance") &&
             !(g_api.StaticShrine_StaticClassInfo && is_kind_of(ci, g_api.StaticShrine_StaticClassInfo()));
    // C = player characters: yourself today, party members in multiplayer (the sphere query returns the main player too).
    case ScanGroup::Players: return is_a(ci, g_api.Player_StaticClassInfo);
    default: break;
  }
  return false;
}
}  // namespace

// A skill's aiming, for the quickbar readout (docs/skills-targeting.md). SkillActivated::GetTargetType is a
// field at this+0x5c0; its RUNTIME values were read off the game (2026-08-23, /hotbar over known skills) and
// are NOT the DBR targetingMode enum order: 1 = self / buff (Overguard, potions, Field Command), 2 = offensive
// (Weapon Attack, Cadence, Blitz, Forcewave, War Cry -- the game aims these at your target), 3 = a ground
// point (Evade, Move To); 0 = passive / not applicable. A player-centred AoE (War Cry) is a 2 too, split off
// as "around you" by its concrete RTTI class name (Skill_AttackRadius). Passives / modifiers are Skill but
// not SkillActivated -> None.
SkillAim skill_aim(const void* skill_obj) {
  if (!skill_obj || !g_api.SkillActivated_StaticClassInfo || !g_api.SkillActivated_GetTargetType) return SkillAim::None;
  if (rtti_name(rtti_of(skill_obj)) == "Skill_PetAttack") return SkillAim::AtTarget;   // "Pet Attack": not a SkillActivated, aims at the cursor (docs/pets.md)
  if (!is_kind_of(rtti_of(skill_obj), g_api.SkillActivated_StaticClassInfo())) return SkillAim::None;
  int tt = 0;
  if (!call_int_fn(skill_obj, g_api.SkillActivated_GetTargetType, &tt)) return SkillAim::None;
  switch (tt) {
    case 1: return SkillAim::SelfCast;                                                            // self / buff: no aiming
    case 2: return class_name(skill_obj).find("Radius") != std::string::npos ? SkillAim::AroundYou : SkillAim::AtTarget;
    case 3: return SkillAim::AtPoint;                                                             // a ground location: movement, placed AoE
    default: return SkillAim::None;                                                               // 0 = passive / not applicable
  }
}

int skill_target_type(const void* skill_obj) {
  if (!skill_obj || !g_api.SkillActivated_StaticClassInfo || !g_api.SkillActivated_GetTargetType) return -1;
  if (!is_kind_of(rtti_of(skill_obj), g_api.SkillActivated_StaticClassInfo())) return -1;
  int tt = 0;
  return call_int_fn(skill_obj, g_api.SkillActivated_GetTargetType, &tt) ? tt : -1;
}

int clock_hour(const Vec3& p) {
  Vec3 me; if (!player_position(me)) return 12;
  float fx, fz, rx, rz; screen_axes(fx, fz, rx, rz);
  float dx = p.x - me.x, dz = p.z - me.z;
  float ahead = dx * fx + dz * fz, right = dx * rx + dz * rz;
  float ang = std::atan2(right, ahead);  // 0 = ahead, +pi/2 = right
  int hour = (int)std::lround(ang / (2.0f * 3.14159265f) * 12.0f);
  hour = ((hour % 12) + 12) % 12;
  return hour == 0 ? 12 : hour;
}

static std::vector<ScanItem> (*g_exit_provider)() = nullptr;
void set_exit_provider(std::vector<ScanItem> (*provider)()) { g_exit_provider = provider; }
static Vec3 g_reviewed_point;   // the position of a reviewed point item (set on landing)
static std::string g_reviewed_label;

// Enemy nameplate stats off a Monster* (health fraction, char level, MonsterClassification). SEH-guarded and
// POD-only (no C++ objects) so it can wrap the raw game reads. The caller must have confirmed e is a Monster
// (GetClassification reads Monster+0x4ef4).
bool read_enemy_stats(void* e, int& level, int& classification, float& pct) {
  level = 0; classification = -1; pct = 0.0f;
  __try {
    if (g_api.GetCharLevel) level = (int)g_api.GetCharLevel(e);
    if (g_api.GetClassification) { const int* c = g_api.GetClassification(e); if (c) classification = *c; }
    double cur = g_api.GetCurrentLife ? g_api.GetCurrentLife(e) : 0.0;
    float mx = g_api.GetLifeLimit ? g_api.GetLifeLimit(e) : 0.0f;
    pct = mx > 0.0f ? (float)(cur / mx) : 0.0f;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Walk a Character's active buff/debuff list into POD record-path strings. The entry holds NO skill id
// (SkillBuffTransfer+0x48 is the CASTER, confirmed live 2026-08-25); the identity is the record path at +0x00.
// Chain: Character+0x850 SkillManager; *(SM+0x390) SkillServices (nullable); *(SVC+0x8) = std::list sentinel;
// node+0x10 = value (stride 0xA0), value+0x00 = std::string<char> record. Returns how many records were written.
constexpr int kBuffRecLen = 160;
int read_buff_records(void* character, char (*recs)[kBuffRecLen], int max) {
  int count = 0;
  __try {
    void* svc = *(void**)((char*)character + 0x850 + 0x390);
    if (!svc) return 0;
    void* head = *(void**)((char*)svc + 0x8);
    if (!head) return 0;
    void* n = *(void**)head;   // head->next
    for (int guard = 0; n && n != head && guard < 512 && count < max; ++guard, n = *(void**)n) {
      const MsvcStringA* s = (const MsvcStringA*)((char*)n + 0x10);   // record at value+0x00
      size_t len = s->size < (size_t)kBuffRecLen - 1 ? s->size : (size_t)kBuffRecLen - 1;
      const char* d = s->data();
      if (len && !IsBadReadPtr((void*)d, len)) { memcpy(recs[count], d, len); recs[count][len] = 0; }
      else recs[count][0] = 0;
      ++count;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return count;
}
// The live skill id for a buff record, via the owner's own SkillManager (the record is registered there when
// the buff is applied). SEH-guarded; 0 when not found.
unsigned find_skill_by_record(void* character, const char* record) {
  if (!character || !record || !record[0] || !g_api.FindSkillId) return 0;
  unsigned id = 0;
  __try { id = g_api.FindSkillId((char*)character + 0x850, record); } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
  return id;
}

namespace { bool g_show_all_items = false; }
bool show_all_items() { return g_show_all_items; }
void toggle_show_all_items() {
  g_show_all_items = !g_show_all_items;
  exe_ui::set_show_all_items(g_show_all_items);
  speech::speak(std::string(g_show_all_items ? gd::strings::kShowingAllItems : gd::strings::kLootFilterOn), true);
}
// The game's Alt handler clears the byte on every Alt release, so the latch re-asserts it each frame.
void show_all_tick() { if (g_show_all_items) exe_ui::set_show_all_items(true); }

std::vector<ScanItem> scan(ScanGroup group, float radius) {
  std::vector<ScanItem> out;
  Vec3 me; Buf base; void* region = nullptr;
  if (!player_position(me)) return out;
  if (group == ScanGroup::Pets) {   // the game's own pet list (docs/pets.md); note = the stance word
    for (const gameapi::PetInfo& p : gameapi::pets()) {
      ScanItem it; it.id = p.id; it.cls = "Pet"; it.label = p.label; it.pos = p.pos; it.note = gameapi::pet_stance_name(p.stance);
      it.dist = std::sqrt((p.pos.x - me.x) * (p.pos.x - me.x) + (p.pos.z - me.z) * (p.pos.z - me.z));
      out.push_back(it);
    }
    std::sort(out.begin(), out.end(), [](const ScanItem& a, const ScanItem& b) { return a.dist < b.dist; });
    return out;
  }
  if (group == ScanGroup::Exits) {
    if (g_exit_provider) out = g_exit_provider();
    for (ScanItem& it : out) it.dist = std::sqrt((it.pos.x - me.x) * (it.pos.x - me.x) + (it.pos.z - me.z) * (it.pos.z - me.z));
    std::sort(out.begin(), out.end(), [](const ScanItem& a, const ScanItem& b) { return a.dist < b.dist; });
    return out;
  }
  if (!g_api.Region_GetEntitiesInSphere || !player_world_vec(base, &region)) return out;
  alignas(16) unsigned char vb[64] = {};
  MemVec* v = (MemVec*)vb;
  const Vec3* rp = g_api.WorldVec3_GetRegionPosition(&base);
  struct { Vec3 c; float r; } sphere{*rp, radius};
  g_api.Region_GetEntitiesInSphere(region, v, &sphere, false, 0);
  if (!(v->begin && v->end && (uintptr_t)v->end > (uintptr_t)v->begin && (uintptr_t)v->end - (uintptr_t)v->begin < (1u << 20))) return out;
  size_t n = (size_t)((char*)v->end - (char*)v->begin) / sizeof(void*);
  for (size_t i = 0; i < n && i < 4096; ++i) {
    void* e; memcpy(&e, (char*)v->begin + i * sizeof(void*), sizeof e);
    if (!e) continue;
    EntityRaw r{};
    if (!read_entity(e, r) || !r.has_pos) continue;
    std::string cls = rtti_name(r.ci), record = r.name;
    if (!in_group(e, r.ci, cls, group)) continue;
    float d = std::sqrt((r.pos.x - me.x) * (r.pos.x - me.x) + (r.pos.z - me.z) * (r.pos.z - me.z));
    if (d > radius) continue;
    // What the game does not show, we do not list: a placed quest item the player already collected stays in the
    // world with visibility 0 (docs/loot-filter.md) -- the "Strange Key you already have" bug.
    if (gameapi::entity_hidden(e)) continue;
    // Ground items obey the player's loot filter like their floating labels do, unless O latched "show all".
    if ((group == ScanGroup::Loot || group == ScanGroup::Objects) && !g_show_all_items && g_api.Item_StaticClassInfo && is_kind_of(r.ci, g_api.Item_StaticClassInfo()) &&
        !gameapi::item_passes_loot_filter(e))
      continue;
    // Enemies are Monsters the game's faction manager calls foes of the player (guards are Monsters too).
    int lvl = 0, cls_i = -1;
    if (group == ScanGroup::Enemies) {
      void* fm = g_game_engine && g_api.GetFactionManager ? g_api.GetFactionManager(g_game_engine) : nullptr;
      void* p = player();
      unsigned pid = p && g_api.Object_GetObjectId ? g_api.Object_GetObjectId(p) : 0;
      if (fm && g_api.FactionManager_IsFoe && pid && !g_api.FactionManager_IsFoe(fm, pid, r.id, false)) continue;
      // Corpses stay Monsters (and foes) until the game reaps them: only the living are enemies (2026-08-22).
      if (g_api.Character_IsAlive && !g_api.Character_IsAlive(e)) continue;
      float pct; read_enemy_stats(e, lvl, cls_i, pct);   // level + rarity for the readout (pct unused here)
    }
    std::string label = entity_label(e, r.ci, cls);  // an unlabelled object is read by its class name (cycle_review)
    std::string note;
    if (is_named_kind(r.ci, "DungeonEntrance")) {   // a one-way exit shaft: no name, and the game says not usable
      if (label.empty()) label = std::string(gd::strings::kEntrance);
      if (!is_of_interest(e, r.ci)) note = std::string(gd::strings::kLocked);
    }
    if (group == ScanGroup::Players && r.id == player_id()) note = std::string(gd::strings::kYou);
    out.push_back({r.id, cls, label, record, r.pos, d, note, lvl, cls_i});
  }
  std::sort(out.begin(), out.end(), [](const ScanItem& a, const ScanItem& b) { return a.dist < b.dist; });
  return out;
}

static std::string_view group_label(ScanGroup g) {
  switch (g) {
    case ScanGroup::Enemies: return gd::strings::kEnemies;
    case ScanGroup::Neutrals: return gd::strings::kNeutrals;
    case ScanGroup::Bystanders: return gd::strings::kBystanders;
    case ScanGroup::Exits: return gd::strings::kExits;
    case ScanGroup::Loot: return gd::strings::kLoot;
    case ScanGroup::Transitions: return gd::strings::kTransitions;
    case ScanGroup::Pets: return gd::strings::kPets;
    case ScanGroup::Players: return gd::strings::kCharacters;
    default: return gd::strings::kLoot;
  }
}

// The shared landing: pick an item from an already-scanned, nearest-first list (continuing from the current
// review target when present, else entering at the nearest / farthest by direction), lock the cursor on it and
// speak "label, N away, H o'clock, i of n" -- with "level N <rarity>" folded into an enemy's label.
static std::string land_on(std::vector<ScanItem>& items, ScanGroup group, int dir, bool nearest) {
  gd::core::MessageBuilder m;
  if (items.empty()) { unlock_target(); g_reviewed_id = 0; gd::strings::push_nothing_nearby(m, group_label(group)); return m.build(); }
  int idx = -1;
  if (!nearest) for (size_t i = 0; i < items.size(); ++i) if (items[i].id == g_reviewed_id) { idx = (int)i; break; }
  int count = (int)items.size();
  idx = idx < 0 ? (dir >= 0 ? 0 : count - 1) : ((idx + dir) % count + count) % count;
  const ScanItem& it = items[(size_t)idx];
  g_reviewed_id = it.id;
  g_reviewed_label = it.label.empty() ? it.cls : it.label;
  if (is_point_id(it.id)) { g_reviewed_point = it.pos; lock_point(it.pos); }
  else lock_target(it.id);
  ping_reviewed();  // every landing plays the route ping, like wotr
  std::string label = it.label.empty() ? it.cls : it.label;
  if (group == ScanGroup::Enemies && it.classification >= 0) {   // "walking undead level 5 hero"
    gd::core::MessageBuilder em; gd::strings::push_enemy_label(em, label, it.level, it.classification); label = em.build();
  }
  // Your own character: no distance or bearing to yourself ("claude, you, 1 of 1"); the lock parks the cursor on you,
  // which is how a cursor-placed skill (Inquisitor Seal, a totem) goes under your own feet.
  if (group == ScanGroup::Players && it.id == player_id()) gd::strings::push_scan_self(m, label, idx + 1, count);
  else gd::strings::push_scan_item(m, label, it.dist, clock_hour(it.pos), idx + 1, count, !on_screen(it.id), it.note);
  return m.build();
}

std::string cycle_review(ScanGroup group, int dir, bool nearest) {
  std::vector<ScanItem> items = scan(group);
  return land_on(items, group, dir, nearest);
}

std::string cycle_highest_classification(int dir) {
  std::vector<ScanItem> items = scan(ScanGroup::Enemies);
  int top = -1;
  for (const ScanItem& it : items) top = std::max(top, it.classification);
  if (top > 0) std::erase_if(items, [top](const ScanItem& it) { return it.classification != top; });
  return land_on(items, ScanGroup::Enemies, dir, false);
}
unsigned reviewed_id() { return g_reviewed_id; }
bool shrine_restored(unsigned id) {
  void* e = gameapi::object_by_id(id);
  EntityRaw r{};
  if (!e || !g_api.StaticShrine_StaticClassInfo || !g_api.StaticShrine_IsCleansed || !read_entity(e, r) || !is_kind_of(r.ci, g_api.StaticShrine_StaticClassInfo())) return false;
  bool v = false;
  return call_bool_fn(e, g_api.StaticShrine_IsCleansed, &v) && v;
}

// ---- status effects and the target inspector ----
std::vector<std::string> enemy_effects(unsigned id) {
  std::vector<std::string> out;
  void* e = gameapi::object_by_id(id);
  if (!e) return out;
  static char recs[64][kBuffRecLen];
  int n = read_buff_records(e, recs, 64);
  for (int i = 0; i < n; ++i) {
    std::string name = gameapi::skill_name_by_id(find_skill_by_record(e, recs[i]));
    if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
  }
  return out;
}

// Name a single buff by its record on the given owner's SkillManager (combat.cpp: the victim owns the entry).
std::string buff_name(unsigned owner_id, const char* record) {
  void* e = gameapi::object_by_id(owner_id);
  if (!e) return {};
  return gameapi::skill_name_by_id(find_skill_by_record(e, record));
}

bool enemy_vitals(unsigned id, float& pct, int& level, int& classification) {
  void* e = gameapi::object_by_id(id);
  if (!e) return false;
  EntityRaw r{};
  if (!read_entity(e, r) || rtti_name(r.ci) != "Monster") return false;   // only a Monster has GetClassification
  return read_enemy_stats(e, level, classification, pct);
}

std::string inspect_target() {
  if (!in_world()) return {};
  void* ctrl = controller();
  unsigned id = ctrl && g_api.GetCombatEnemy ? g_api.GetCombatEnemy(ctrl) : 0;
  if (!id) return {};                                  // nothing targeted -> silent
  void* e = gameapi::object_by_id(id);
  if (!e) return {};
  EntityRaw r{};
  if (!read_entity(e, r) || rtti_name(r.ci) != "Monster") return {};       // not an enemy -> silent
  if (g_api.Character_IsAlive && !g_api.Character_IsAlive(e)) return {};    // a corpse -> silent
  int level = 0, classification = -1; float pct = 0.0f;
  if (!read_enemy_stats(e, level, classification, pct)) return {};
  std::vector<std::string> fx = enemy_effects(id);
  gd::core::MessageBuilder m;
  gd::strings::push_target_inspect(m, (int)std::lround(pct * 100.0f), fx);
  return m.build();
}

bool entity_position(unsigned id, Vec3& out) {
  void* e = gameapi::object_by_id(id);
  Buf wv;
  if (!e || !entity_world_vec(e, wv)) return false;
  out = world_pos_of(wv);
  return true;
}

// Diagnostic: copy each buff entry's record path (+0x00) and full 0xA0 bytes (POD/SEH), so effects_dump can
// show the record and probe every 4-byte field for the one that resolves to a named skill.
struct BuffRaw { char record[256]; unsigned char bytes[0xA0]; };
int read_buff_raw(void* character, BuffRaw* out, int max) {
  int count = 0;
  __try {
    void* svc = *(void**)((char*)character + 0x850 + 0x390);
    if (!svc) return 0;
    void* head = *(void**)((char*)svc + 0x8);
    if (!head) return 0;
    void* n = *(void**)head;
    for (int g = 0; n && n != head && g < 512 && count < max; ++g, n = *(void**)n) {
      const char* val = (const char*)n + 0x10;
      memcpy(out[count].bytes, val, 0xA0);
      const MsvcStringA* s = (const MsvcStringA*)val;   // record path at val+0x00
      size_t len = s->size < 255 ? s->size : 255;
      const char* d = s->data();
      if (len && !IsBadReadPtr((void*)d, len)) { memcpy(out[count].record, d, len); out[count].record[len] = 0; }
      else out[count].record[0] = 0;
      ++count;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return count;
}

std::string effects_dump(unsigned id) {
  void* e = gameapi::object_by_id(id);
  if (!e) return "no such object\n";
  BuffRaw raw[32];
  int n = read_buff_raw(e, raw, 32);
  std::string out = std::format("buffs on id {}: {}\n", id, n);
  for (int i = 0; i < n; ++i) {
    unsigned sid = find_skill_by_record(e, raw[i].record);
    out += std::format("  record='{}' skillId={} name='{}'\n", raw[i].record, sid, gameapi::skill_name_by_id(sid));
  }
  return out;
}

// The one pan/gain rule for positioned sounds and voices (wotr's sonar): pan by the ear-frame bearing of the
// point from the player, gain ref/(ref+dist) with ref = 10 ft in units, never below 0.15.
static float g_ping_ref = 10.0f, g_ping_floor = 0.2f;   // a cue 10 units away plays at half; nothing in reach drops under a fifth
void ear_frame(const Vec3& p, float& pan, float& gain, float* ahead) {
  Vec3 me; pan = 0.0f; gain = 1.0f;
  if (ahead) *ahead = 1.0f;
  if (!player_position(me)) return;
  float fx, fz, rx, rz; screen_axes(fx, fz, rx, rz);
  float dx = p.x - me.x, dz = p.z - me.z;
  float dist = std::sqrt(dx * dx + dz * dz);
  float right = dx * rx + dz * rz;
  float fwd = dx * fx + dz * fz;
  pan = dist > 0.01f ? right / dist : 0.0f;
  if (ahead) *ahead = dist > 0.01f ? std::clamp(fwd / dist, -1.0f, 1.0f) : 1.0f;
  // One curve for every positioned cue. wotr's 10 ft reference (4.5 units) and its 40 % sonar channel were
  // tuned for its corridors; at Grim Dawn's scale they were "way too quiet" (the user, 2026-08-22).
  gain = g_ping_ref / (g_ping_ref + dist);
  if (gain < g_ping_floor) gain = g_ping_floor;
}
void set_ping_rolloff(float ref, float floor) { if (ref > 0.1f) g_ping_ref = ref; if (floor >= 0.0f && floor <= 1.0f) g_ping_floor = floor; }
std::string ping_rolloff() { return std::format("ref={:.1f} floor={:.2f}", g_ping_ref, g_ping_floor); }
float rear_shelf_db(float ahead) { return ahead < 0.0f ? 10.0f * ahead : 0.0f; }   // -10 dB * how far behind
static float g_voice_near = 9.0f, g_voice_far = 32.0f, g_voice_floor = 0.4f;
float voice_gain(float dist) {
  if (dist <= g_voice_near) return 1.0f;
  if (dist >= g_voice_far) return g_voice_floor;
  return 1.0f - (1.0f - g_voice_floor) * (dist - g_voice_near) / (g_voice_far - g_voice_near);
}
void set_voice_rolloff(float near_d, float far_d, float floor_g) {  // (near/far are windows.h macros)
  if (near_d >= 0) g_voice_near = near_d;
  if (far_d > g_voice_near) g_voice_far = far_d;
  if (floor_g >= 0 && floor_g <= 1) g_voice_floor = floor_g;
}
std::string voice_rolloff() { return std::format("near={:.1f} far={:.1f} floor={:.2f}", g_voice_near, g_voice_far, g_voice_floor); }
// A WorldVec3 (Region* + region-relative Vec3) to world space; false without a region (GetWorldPosition on a
// region-less WorldVec3 hung the game thread once).
bool world_point(const void* worldvec3, Vec3& out) {
  if (!worldvec3) return false;
  Buf b{}; memcpy(b.b, worldvec3, 32);
  void* region; memcpy(&region, b.b, sizeof region);
  if (!region) return false;
  out = world_pos_of(b);
  return true;
}

// The route kind for the reviewed target ("straight" / "path" / "unreachable") and its world position, or ""
// when nothing is reviewed (or the target's position can't be read this frame). No sound -- reping_tick
// compares this frame to frame and ping_reviewed turns it into a played cue.
// Route kind from the player to an arbitrary target point: every half unit of the straight line on the
// navmesh = straight walk; the target's own spot walkable but the line not = path around; the target off the
// mesh = unreachable (a cliff, water, inside a wall). Shared by the review cursor and the follow target.
static std::string route_kind_to(const Vec3& me, const Vec3& target) {
  float dx = target.x - me.x, dz = target.z - me.z;
  float dist = std::sqrt(dx * dx + dz * dz);
  const char* kind = "unreachable";
  Vec3 near_target{target.x - (dist > 0.5f ? dx / dist * 0.5f : 0), target.y, target.z - (dist > 0.5f ? dz / dist * 0.5f : 0)};
  if (on_navmesh(near_target) || on_navmesh(target)) {
    bool straight = true;
    float y = me.y;   // hug the terrain (PutOnFloor's ~4.8u down-window loses a rise otherwise; see free_distance)
    for (float d = 0.5f; d < dist - 0.5f; d += 0.5f) {
      Vec3 s{me.x + dx / dist * d, y, me.z + dz / dist * d}, floored;
      if (!navmesh_probe(s, &floored)) { straight = false; break; }
      y = floored.y;
    }
    kind = straight ? "straight" : "path";
  }
  return kind;
}
static std::string reviewed_route(Vec3& me, Vec3& target) {
  if (!g_reviewed_id) return {};
  if (!player_position(me)) return {};
  if (is_point_id(g_reviewed_id)) {
    target = g_reviewed_point;
  } else {
    void* e = find_entity(g_reviewed_id);
    Buf wv;
    if (!e || !entity_world_vec(e, wv)) return {};
    target = world_pos_of(wv);
  }
  return route_kind_to(me, target);
}

static std::string g_last_ping_kind;   // the kind reping_tick last sounded, for g_last_ping_id
static unsigned g_last_ping_id = 0;
static void play_ping(const std::string& kind, const Vec3& target) {
  float pan, vol, ahead; ear_frame(target, pan, vol, &ahead);
  gd::audio::play_sample(gd::audio::module_dir() + "assets\\audio\\review_" + kind + ".wav", vol, pan, rear_shelf_db(ahead));
  g_last_ping_kind = kind;
  g_last_ping_id = g_reviewed_id;
}

std::string ping_reviewed() {
  Vec3 me, target;
  std::string kind = reviewed_route(me, target);
  if (kind.empty()) return {};
  play_ping(kind, target);
  return kind;
}

// ---- the follow target (the quest-following key ') ----
// A destination the player chose from the map picker: an entity id (re-resolved each ping so it tracks a
// moving NPC) or a fixed world point. Independent of the review cursor. The ' key plays the route ping
// toward it and speaks "label, N away, H o'clock" (plus a note when it can't be reached directly).
static bool g_follow_active = false;
static unsigned g_follow_id = 0;   // 0 = a fixed point (g_follow_pos)
static Vec3 g_follow_pos{};
static std::string g_follow_label;

void set_follow_target(unsigned id, const Vec3& pos, const std::string& label) {
  g_follow_active = true;
  g_follow_id = id;
  g_follow_pos = pos;
  g_follow_label = label;
}
void clear_follow_target() { g_follow_active = false; g_follow_id = 0; g_follow_label.clear(); }
bool has_follow_target() { return g_follow_active; }
std::string follow_target_label() { return g_follow_label; }

bool reviewed_destination(TravelTarget& out) {
  if (!g_reviewed_id) return false;
  out = {}; out.label = g_reviewed_label;
  if (is_point_id(g_reviewed_id)) { out.pos = g_reviewed_point; return true; }
  out.id = g_reviewed_id;
  out.enemy = is_foe(out.id);
  return entity_position(out.id, out.pos);
}
bool followed_destination(TravelTarget& out) {
  if (!g_follow_active) return false;
  out = {}; out.label = g_follow_label; out.pos = g_follow_pos;
  if (g_follow_id && !is_point_id(g_follow_id)) {
    out.id = g_follow_id; out.enemy = is_foe(out.id);
    if (!entity_position(out.id, out.pos)) return false;
  }
  return true;
}
bool game_paused() { return g_api.IsGameTimePaused && g_api.IsGameTimePaused(); }
// Short world-space commands use the same controller entry point as WASD. The travel layer supplies
// successive corners from the navmesh corridor; it never holds a synthetic key or changes movement mode.
bool movement_step(const Vec3& destination) {
  if (!g_controller || !HandleActionFromJoystick_hook_orig || !g_api.Character_StopMoving) return false;
  Buf wv{};
  if (!world_vec3_at(destination, wv.b)) return false;
  __try { return HandleActionFromJoystick_hook_orig(g_controller, wv.b, false); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void stop_movement() {
  void* p = player();
  if (!p || !g_api.Character_StopMoving) return;
  __try { g_api.Character_StopMoving(p, true, false); }
  __except (EXCEPTION_EXECUTE_HANDLER) { log::write("travel: stop movement faulted"); }
}

std::string follow_ping() {
  if (!g_follow_active) return {};
  Vec3 me;
  if (!player_position(me)) return {};
  Vec3 target = g_follow_pos;
  if (g_follow_id && !is_point_id(g_follow_id)) {   // a live entity: track its current position, else keep the last known
    void* e = find_entity(g_follow_id);
    Buf wv;
    if (e && entity_world_vec(e, wv)) { target = world_pos_of(wv); g_follow_pos = target; }
  }
  std::string kind = route_kind_to(me, target);
  float pan, vol, ahead;
  ear_frame(target, pan, vol, &ahead);
  gd::audio::play_sample(gd::audio::module_dir() + "assets\\audio\\review_" + kind + ".wav", vol, pan, rear_shelf_db(ahead));
  float dx = target.x - me.x, dz = target.z - me.z;
  float dist = std::sqrt(dx * dx + dz * dz);
  int hour = clock_hour(target);
  gd::core::MessageBuilder m;
  m.list_item().fragment(g_follow_label);   // label must be a list item for "label, N away" (see push_scan_item)
  if (kind == "unreachable") m.list_item().fragment(gd::strings::kBlocked);
  gd::strings::push_distance_bearing(m, dist, hour);
  return m.build();
}

std::string probe_timing(int iters) {   // dev: time one reviewed_route() call (the navmesh line probe)
  if (iters < 1) iters = 1;
  Vec3 me, target;
  std::string kind = reviewed_route(me, target);  // warm + report the shape
  if (kind.empty()) return "no target under review\n";
  float dx = target.x - me.x, dz = target.z - me.z;
  float dist = std::sqrt(dx * dx + dz * dz);
  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  for (int i = 0; i < iters; ++i) { Vec3 a, b; reviewed_route(a, b); }
  QueryPerformanceCounter(&t1);
  double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)freq.QuadPart / iters;
  int steps = dist > 1.0f ? (int)((dist - 0.5f) / 0.5f) : 0;
  return std::format("kind={} dist={:.1f} line_steps~{} iters={} avg={:.1f} us/call ({:.3f} ms)\n",
                     kind, dist, steps, iters, us, us / 1000.0);
}

void reping_tick() {
  // Every frame -- the probe is 36-44 us at close range, ~2.7 us per half-unit step, so ~0.25 ms even at the
  // 40-unit scan radius (walltones already runs ~80 such probes per frame). Any throttle was audible as lag.
  if (!g_reviewed_id) { g_last_ping_kind.clear(); g_last_ping_id = 0; return; }
  // Exits (point ids) ping only on the landing (V), never automatically: their position is the corridor's
  // entry point, which depends on where the player stands, so re-pinging as the route kind flips while
  // walking read as noise (2026-08-25, the user).
  if (is_point_id(g_reviewed_id)) return;
  Vec3 me, target;
  std::string kind = reviewed_route(me, target);
  if (kind.empty()) return;
  if (g_reviewed_id == g_last_ping_id && kind == g_last_ping_kind) return;  // route unchanged since the last ping
  play_ping(kind, target);
}

bool on_screen(unsigned id) {
  float x, y;
  if (is_point_id(id) ? !project_point(g_reviewed_point, x, y) : !entity_screen_pos(id, x, y)) return false;
  RECT rc{};
  HWND w = FindWindowA("Grim Dawn", nullptr);
  if (!w || !GetClientRect(w, &rc)) return false;
  return x >= 0 && y >= 0 && x < (float)rc.right && y < (float)rc.bottom;
}
// J on a reviewed ITEM lying on the ground is not a click (static RE 2026-08-22, docs/re_pickup.md): the sighted
// player clicks the item's floating name label, which the game draws only when the loot filter shows it, and a
// click on the item's model resolves nothing ("attack here"). The label click ends in
// ControllerPlayer::ItemAction(false, false, pos, item) -- the range test, the navmesh approach point and the
// MoveToItem -> PickupItem chain are all inside -- so that is what J issues for an item, on the press. It
// silently does nothing while the controller's IsCommandRepeated byte is set (our mouse hold leaves it on).
static bool call_item_action(void* ctrl, const void* wv, const void* item) {   // POD only: SEH cannot unwind C++ objects
  __try {
    if (g_api.SetCommandRepeated) g_api.SetCommandRepeated(ctrl, false);
    g_api.ItemAction(ctrl, false, false, wv, item);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool pickup_locked_item() {
  if (!g_locked_id || !g_controller || !g_api.ItemAction || !g_api.Item_StaticClassInfo) return false;
  g_locked_entity = find_entity(g_locked_id);   // the key edge may land on a frame the tick has not re-resolved yet
  if (!g_locked_entity) return false;
  EntityRaw r{};
  if (!read_entity(g_locked_entity, r) || !is_kind_of(r.ci, g_api.Item_StaticClassInfo())) return false;
  // NOT lore notes: a direct ItemAction consumes a note without the pickup toast or the codex entry (measured
  // 2026-08-23 -- the exe's click path owns those), and the plain click resolves notes fine. Click them.
  if (g_api.ItemNote_StaticClassInfo && is_kind_of(r.ci, g_api.ItemNote_StaticClassInfo())) return false;
  Buf wv;
  if (!entity_world_vec(g_locked_entity, wv)) return false;
  if (!call_item_action(g_controller, wv.b, g_locked_entity)) { log::write("world: ItemAction faulted"); return false; }
  log::writef("world: ItemAction on {} '{}'", g_locked_id, label_of(g_locked_id));
  return true;
}
// J on a reviewed FIXED ACTOR of interest (door, ladder / dungeon entrance, chest, lever, shrine) is the call the
// exe's click makes once it has resolved the actor: ControllerPlayer::InteractAction(false, false, pos, actor) ->
// DefaultRequestInteractableAction (walks into the record's own interact range, then UseFixedItem;
// docs/re_pickup.md). Issued directly because the click at the parked point does NOT always resolve the actor: a
// ladder's clickable body stands up the wall above its floor position, and the pick ray at that point hit the
// ground (Flooded Cellar 2026-08-30, "Rickety Ladder": every HandleActionFromMouse resolved id 0, the player
// could not leave). Npcs keep the click (mid-body parks fine); Destructibles are not "of interest" and keep the
// attack click.
static bool call_interact_action(void* ctrl, const void* wv, const void* actor) {   // POD only: SEH cannot unwind C++ objects
  __try {
    if (g_api.SetCommandRepeated) g_api.SetCommandRepeated(ctrl, false);
    g_api.InteractAction(ctrl, false, false, wv, actor);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool interact_locked_actor() {
  if (!g_locked_id || !g_controller || !g_api.InteractAction || !g_api.FixedActor_StaticClassInfo) return false;
  g_locked_entity = find_entity(g_locked_id);
  if (!g_locked_entity) return false;
  EntityRaw r{};
  if (!read_entity(g_locked_entity, r) || !is_kind_of(r.ci, g_api.FixedActor_StaticClassInfo()) || !is_of_interest(g_locked_entity, r.ci)) return false;
  Buf wv;
  if (!entity_world_vec(g_locked_entity, wv)) return false;
  if (!call_interact_action(g_controller, wv.b, g_locked_entity)) { log::write("world: InteractAction faulted"); return false; }
  log::writef("world: InteractAction on {} '{}'", g_locked_id, label_of(g_locked_id));
  return true;
}
// ---- where an injected mouse transition lands ----
// The exe keeps its own "left / right button held" bytes (world screen +0x88 / +0x89) and, while either is set, its
// per-frame WASD routine (exe+0x2c2b5) issues no move command at all: a held button's repeated command outranks the
// keyboard. Its mouse handler ignores events outside the client area, so a transition delivered off-window is
// simply lost and the byte sticks. Seen 2026-09-01: J held on a locked enemy that died; the lock went away, the
// release (and the next press) fell back to the real cursor at (-86, 193), and WASD stayed dead until the cursor came
// back on screen. Every transition we inject therefore lands inside the window: a press only where there is a point
// to press (the virtual cursor while the camera shows it, else the real cursor while it is in the window), a
// release at the virtual cursor, or -- when its target has left the window -- where the player-to-target line
// leaves the window (the user's rule), so the release still goes out in the target's direction.
namespace {
constexpr float kEdgeMargin = 4.0f;   // pixels inside the client edge, so a clipped point is unambiguously in the window
bool client_size(float& w, float& h) {
  RECT rc{};
  HWND win = FindWindowA("Grim Dawn", nullptr);
  if (!win || !GetClientRect(win, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
  w = (float)rc.right; h = (float)rc.bottom;
  return true;
}
bool inside_window(float x, float y, float w, float h) { return x >= 0 && y >= 0 && x < w && y < h; }
// The virtual cursor's target projected to the screen, on or off the window: the locked entity or point.
bool virtual_cursor_pos(float& x, float& y) {
  if (g_free_cursor) return project_point(g_free_point, x, y);
  if (g_point_locked) return project_point(g_locked_point, x, y);
  if (g_locked_id) { void* e = find_entity(g_locked_id); return e && project(e, x, y); }
  return false;
}
bool player_screen_pos(float& x, float& y) {
  Buf base;
  return player_world_vec(base) && project_point(world_pos_of(base), x, y);
}
// Where a press lands; false when there is nothing on screen to press.
// over_hud: the point is on the game's HUD (exe_ui::point_over_hud) -- the UI would take the click (a hotslot, a
// menu button), the world would never see it, so it is not a press point (2026-09-04).
// A locked target whose own point is not pressable -- on the HUD, or off the window (at max zoom the window's south
// edge is 13 units out against 21 north: the camera sits south of the player looking down at 46 degrees, the same
// geometry a sighted player has) -- is aimed at BY DIRECTION, as a sighted player does: the press lands on the
// player-to-target line where it leaves the aimable area (the window inset by kEdgeMargin, then backed off the HUD).
// The game fires point-aimed skills that way, searches near the point for an enemy, and walks / attacks a weapon
// attack toward it (docs/skills-targeting.md); only the charges refuse without an entity, natively. The screen ray is
// taken through a reference point a few units out (always on screen and in front of the camera; a far target behind
// the camera would project mirrored), and the target's own projection is used only when it lies that way (2026-09-17).
constexpr float kAimRefUnits = 6.0f;
constexpr int kAimSteps = 40;
constexpr float kAimMinT = 0.15f;
bool aim_along_line(float w, float h, float& x, float& y) {
  Vec3 target;
  if (g_free_cursor) target = g_free_point;
  else if (g_point_locked) target = g_locked_point;
  else if (!g_locked_id || !entity_position(g_locked_id, target)) return false;
  Buf base;
  if (!player_world_vec(base)) return false;
  Vec3 me = world_pos_of(base);
  float dx = target.x - me.x, dz = target.z - me.z, d = std::sqrt(dx * dx + dz * dz);
  if (d < 0.5f) return false;
  float r = d < kAimRefUnits ? d : kAimRefUnits;
  Vec3 ref{me.x + dx / d * r, me.y, me.z + dz / d * r};
  float px, py, rx, ry;
  if (!player_screen_pos(px, py) || !inside_window(px, py, w, h) || !project_point(ref, rx, ry)) return false;
  float sx = rx - px, sy = ry - py, len = std::sqrt(sx * sx + sy * sy);
  if (len < 1.0f) return false;
  float ex, ey;
  float tx, ty;
  bool own = virtual_cursor_pos(tx, ty) && inside_window(tx, ty, w, h) && (tx - px) * sx + (ty - py) * sy > 0;
  if (own) { ex = tx; ey = ty; }
  else if (!gd::core::clip_toward(px, py, px + sx / len * 4000.f, py + sy / len * 4000.f, w, h, kEdgeMargin, ex, ey)) return false;
  return gd::core::back_off_rects(px, py, ex, ey, gd::exe_ui::hud_rects(), kAimSteps, kAimMinT, x, y);
}
bool press_point(float& x, float& y, bool* over_hud = nullptr) {
  float w, h;
  if (over_hud) *over_hud = false;
  if (!client_size(w, h)) return false;
  if (g_locked_id || g_point_locked || g_free_cursor) {
    bool shown = virtual_cursor_pos(x, y) && inside_window(x, y, w, h);
    bool on_hud = shown && gd::exe_ui::point_over_hud(x, y);
    if (shown && !on_hud) return true;
    if (aim_along_line(w, h, x, y)) return true;
    if (over_hud) *over_hud = on_hud;
    return false;
  }
  bool ok = gd::hooks::real_cursor_in_window(x, y);
  if (ok && gd::exe_ui::point_over_hud(x, y)) { if (over_hud) *over_hud = true; return false; }
  return ok;
}
// A release must reach the world's mouse handler (the exe clears its held byte on the first event with both buttons
// up, and the UI would swallow an event on the HUD like it swallows one off the window): keep it off the HUD.
void avoid_hud(float& x, float& y, float w, float h) {
  if (!gd::exe_ui::point_over_hud(x, y)) return;
  float px, py;
  if (player_screen_pos(px, py) && inside_window(px, py, w, h) && !gd::exe_ui::point_over_hud(px, py)) { x = px; y = py; return; }
  x = w / 2; y = h * 0.3f;
}
// Where a release lands: always inside the window (see above).
void release_point(float& x, float& y) {
  float w, h;
  if (!client_size(w, h)) { x = 0; y = 0; return; }
  float px, py;
  bool have_player = player_screen_pos(px, py) && inside_window(px, py, w, h);
  float tx, ty;
  if (virtual_cursor_pos(tx, ty)) {
    if (inside_window(tx, ty, w, h)) { x = tx; y = ty; avoid_hud(x, y, w, h); return; }
    if (have_player && gd::core::clip_toward(px, py, tx, ty, w, h, kEdgeMargin, x, y)) { avoid_hud(x, y, w, h); return; }
  }
  if (have_player) { x = px; y = py; avoid_hud(x, y, w, h); return; }   // the target is gone (a dead lock is released before we get here)
  x = w / 2; y = h / 2;
}
void release_hold(int button) {
  if (!gd::hooks::mouse_held(button)) return;
  float x, y;
  release_point(x, y);
  gd::hooks::set_mouse_hold(button, false, x, y);
}
}  // namespace

void mouse_key(int button, bool held) {
  static bool key_down[3] = {};
  bool& prev = key_down[button == 2 ? 2 : 1];
  bool edge = held && !prev;
  prev = held;
  static bool pickup_held = false;   // J went down on an item: the hold that follows must not become a click
  if (!held) {
    if (button == 1) pickup_held = false;
    release_hold(button);
    return;
  }
  float x, y;
  bool over_hud = false;
  if (!press_point(x, y, &over_hud)) {
    // Nothing is locked and the real cursor is off the window or on the HUD, or a locked target could not even be
    // aimed at by direction (projection failed, or the whole line to it is under the HUD): nothing to press. A hold in
    // progress ends here, on screen.
    if (edge) gd::speech::speak(over_hud ? gd::strings::kBehindInterface : (g_locked_id || g_point_locked || g_free_cursor) ? gd::strings::kTooFarAway : gd::strings::kNoTarget, true);
    release_hold(button);
    return;
  }
  if (button == 1) {
    if (edge && g_locked_id && !is_point_id(g_locked_id) && (pickup_locked_item() || interact_locked_actor())) pickup_held = true;
    if (pickup_held) { release_hold(button); return; }
  }
  gd::hooks::set_mouse_hold(button, true, x, y);
}

// ---- camera lock ----
// Decided 2026-08-22 (zoom does not change what the player hears, so nothing is lost): the camera sits at the
// far end of its zoom range and at yaw 0 (axis-aligned, "north" up), re-applied whenever the game drifts
// them. GameCamera layout (Game.dll, read from SetZoom/ResetZoom): zoom range +0x590..+0x594, current
// fraction +0x584; SetZoom(value) clamps into the range.
namespace {
constexpr size_t kCam_ZoomMin = 0x590, kCam_ZoomMax = 0x594, kCam_ZoomT = 0x584;
bool read_cam(const void* cam, size_t off, float* out) {
  __try { *out = *(const float*)((const char*)cam + off); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float cam_float(void* cam, size_t off) { float v = 0; return read_cam(cam, off, &v) ? v : 0; }
}  // namespace
void pin_camera() {
  void* cam = g_game_engine && g_api.GetCamera ? g_api.GetCamera(g_game_engine) : nullptr;
  if (!cam) return;
  if (g_api.SetZoom && cam_float(cam, kCam_ZoomT) < 0.999f) g_api.SetZoom(cam, cam_float(cam, kCam_ZoomMax));
  if (g_api.SetCameraYaw && g_api.GetCameraYaw && std::fabs(g_api.GetCameraYaw(cam)) > 0.001f) g_api.SetCameraYaw(cam, 0.0f);
}

// ---- conversations ----
ConvState conversation_state() {
  std::lock_guard lk(g_conv_mu);
  // The frame being filled is more current than the last complete one once it has any text.
  return g_conv_cur.steps.empty() ? g_conv_last : g_conv_cur;
}
bool in_conversation() {
  std::lock_guard lk(g_conv_mu);
  uint64_t f = gd::hooks::frame();
  const ConvState& s = g_conv_cur.steps.empty() ? g_conv_last : g_conv_cur;
  return !s.steps.empty() && f - s.frame <= 3;
}
std::string conversation_dump() {
  ConvState s = conversation_state();
  std::string out = std::format("conversation={} frame={} now={} in_conversation={} steps={} children_of={} children={}\n", s.conversation, s.frame, gd::hooks::frame(),
                                in_conversation(), s.steps.size(), s.children_of, s.children.size());
  for (const ConvStep& st : s.steps)
    out += std::format("  step={} type={} tag='{}' avail={} used={} parent={} text='{}'\n", st.step, st.type, st.type_tag, st.available, st.used, st.parent,
                       st.text.size() > 90 ? st.text.substr(0, 90) + "..." : st.text);
  for (void* c : s.children) out += std::format("  child {}\n", c);
  return out;
}

std::string project_dump(unsigned id) {
  float x = 0, y = 0;
  void* e = find_entity(id);
  if (!e) return std::format("id {} not found near the player\n", id);
  Vec3 p; Buf wv; if (entity_world_vec(e, wv)) p = world_pos_of(wv);
  bool ok = project(e, x, y);
  return std::format("id {} at world ({:.1f}, {:.1f}, {:.1f}) -> screen ({:.0f}, {:.0f}) ok={} locked={}\n", id, p.x, p.y, p.z, x, y, ok, g_locked_id);
}
}  // namespace gd::world

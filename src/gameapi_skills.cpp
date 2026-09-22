// Skills, masteries and the character sheet: SkillManager / Skill / Character / ControllerCharacter exports
// (docs/ingame-ui-survey.md; the 2026-08-22 readout of the exe's skills window and character sheet).
#include "gameapi.h"
#include "gameapi_internal.h"
#include <format>
#include <set>
#include <unordered_map>
#include "core/message_builder.h"
#include "core/strings.h"

namespace gd::gameapi {
using namespace gd::names;
using namespace gd::gameapi::detail;
namespace {
struct Api {
  const void* (*GetSkillManager)(const void*) = nullptr;
  const MemVec* (*SM_GetSkillList)(const void*) = nullptr;
  const MemVec* (*SM_GetUISkillList)(const void*) = nullptr;
  void (*SM_GetSkillMasteryIds)(const void*, MemVec*) = nullptr;
  void (*SM_IncrementSkill)(void*, unsigned, unsigned) = nullptr;
  void (*SM_RecalculateSkills)(void*) = nullptr;
  bool (*SM_UseReclamationPoints)(void*, int) = nullptr;
  unsigned (*SM_GetCurrentSkillReclamationCost)(const void*) = nullptr;
  unsigned (*SM_GetNumMasteryPoints)(const void*) = nullptr;
  unsigned (*GetDefaultSkillId)(const void*, int) = nullptr;   // GetDefaultSkillId(DefaultSkill): 0 = left mouse basic attack, 1 = right mouse
  const MemVec* (*SM_GetItemSkillList)(const void*) = nullptr; // mem::vector<Skill*>: ALL item skills (incl. loose bag components)
  unsigned (*FindItemSkillIdByItemId)(const void*, unsigned) = nullptr;   // an item's granted skill (equipped-only sourcing)
  unsigned (*Skill_GetSkillLevel)(const void*) = nullptr;
  unsigned (*Skill_GetMaxLevel)(const void*) = nullptr;
  unsigned (*Skill_GetUltimateLevel)(const void*) = nullptr;
  unsigned (*Skill_GetMasteryId)(const void*) = nullptr;
  unsigned (*Skill_GetMasteryLevel)(const void*) = nullptr;
  unsigned (*Skill_GetMasteryLevelRequirement)(const void*) = nullptr;
  const MemVec* (*Skill_GetModifiers)(const void*) = nullptr;   // a base skill's Skill_Modifier ids (Skill+0x140)
  const MemVec* (*Skill_GetSecondarySkills)(const void*) = nullptr;   // a base skill's SkillSecondary ids (Skill+0x170): the pet modifiers
  const MemVec* (*Skill_GetBaseSkills)(const void*) = nullptr;   // a sub-skill's base skill ids (Skill+0x1b8): the forward link, both kinds
  bool (*Skill_IsBaseSkillEnabled)(const void*) = nullptr;   // the game's own learn gate: no base skills, or one of them learned
  bool (*Skill_IsSecondary)(const void*) = nullptr;   // byte Skill+0xad; live TRUE on the base summon, FALSE on its pet modifiers -- not "is a sub-skill"
  bool (*Skill_IsLocked)(const void*) = nullptr;
  bool (*Skill_IsSkillTheMasterySkill)(const void*) = nullptr;
  bool (*Skill_IsSkillModifier)(const void*) = nullptr;
  bool (*Skill_IsItemSkillAuto)(void*) = nullptr;   // an auto-triggered item skill (proc); not assignable
  bool (*Skill_IsSkillEnabled)(const void*) = nullptr;                        // virtual
  MsvcStringW* (*Skill_CreateUISkillName)(const void*, MsvcStringW*, bool) = nullptr;   // virtual, u16 by value
  const MsvcStringA* (*Skill_GetDisplayNameTag)(const void*) = nullptr;
  const void* (*Skill_GetSkillProfile)(const void*) = nullptr;               // virtual
  unsigned (*Profile_GetSkillTier)(const void*) = nullptr;
  void (*Skill_IncrementSkillLevel)(void*, unsigned) = nullptr;              // virtual
  bool (*Skill_DecrementSkillLevel)(void*, unsigned) = nullptr;              // virtual
  void (*Skill_SetSkillLevel)(void*, unsigned) = nullptr;                    // virtual
  void** Skill_vftable = nullptr;
  unsigned (*GetSkillPoints)(const void*) = nullptr;
  void (*SubtractSkillPoint)(void*) = nullptr;
  void (*AddSkillPoints)(void*, unsigned) = nullptr;
  void (*GetSkillMasteries)(const void*, MemVec*) = nullptr;
  unsigned (*GetSkillMasteriesAllowed)(const void*) = nullptr;
  void (*GenerateUISkillText)(const void*, MemVec*, const void*, bool, bool, int, int, bool) = nullptr;
  unsigned (*Object_GetObjectId)(const void*) = nullptr;
  // sheet
  unsigned (*GetCharLevel)(const void*) = nullptr;
  unsigned (*GetExperiencePoints)(const void*) = nullptr;
  unsigned (*GetNextLevelExperience)(const void*) = nullptr;
  unsigned (*GetModifierPoints)(const void*) = nullptr;
  float (*GetTotalCharAttribute)(const void*, int) = nullptr;
  void (*GetAllDefenseAttributes)(const void*, void*) = nullptr;
  void* (*Acc_ctor)(void*) = nullptr;
  void (*Acc_dtor)(void*) = nullptr;
  void (*Acc_Clear)(void*) = nullptr;
  float (*Acc_GetTotalDefenseType)(void*, int) = nullptr;
  float (*DesignerCalculateOffensiveAbility)(void*, float) = nullptr;
  float (*DesignerCalculateDefensiveAbility)(void*, float) = nullptr;
  void (*CalculateDps)(const void*, float*, unsigned) = nullptr;
  MsvcStringW* (*GetClassNameA)(const void*, MsvcStringW*) = nullptr;
  unsigned (*GetDevotionPoints)(const void*) = nullptr;
  unsigned (*GetCurrentLifeInt)(const void*) = nullptr;
  float (*GetCurrentMana)(const void*) = nullptr;
  // attribute points
  void (*Inc_Strength)(void*) = nullptr;
  void (*Inc_Dexterity)(void*) = nullptr;
  void (*Inc_Intelligence)(void*) = nullptr;
  void (*Inc_Life)(void*, int) = nullptr;
  void (*Inc_Mana)(void*) = nullptr;
  float (*StrengthLifeIncrement)(const void*) = nullptr;
  float (*DexterityLifeIncrement)(const void*) = nullptr;
  float (*IntelligenceLifeIncrement)(const void*) = nullptr;
  // dev
  void (*CharacterExperienceOutbound)(void*, unsigned, unsigned) = nullptr;
  void (*DisplaySkillReallocationWindow)(void*) = nullptr;   // the spirit guide's own open-in-reclaim-mode path
  bool loaded = false;
} g;
int g_s_enabled = -1, g_s_name = -1, g_s_profile = -1, g_s_inc = -1, g_s_dec = -1, g_s_set = -1;
constexpr int kSlotReleasePets = 0x80 / 8;   // Skill vtable +0x80 (the exe calls it right before IncrementSkillLevel)

void load_skills() {
  if (g.loaded) return;
  g.loaded = true;
  GAPI_LOAD(g, GetSkillManager, Character_GetSkillManager);
  GAPI_LOAD(g, SM_GetSkillList, SkillManager_GetSkillList);
  GAPI_LOAD(g, SM_GetUISkillList, SkillManager_GetUISkillList);
  GAPI_LOAD(g, SM_GetSkillMasteryIds, SkillManager_GetSkillMasteryIds);
  GAPI_LOAD(g, SM_IncrementSkill, SkillManager_IncrementSkill);
  GAPI_LOAD(g, SM_RecalculateSkills, SkillManager_RecalculateSkills);
  GAPI_LOAD(g, SM_UseReclamationPoints, SkillManager_UseReclamationPoints);
  GAPI_LOAD(g, SM_GetCurrentSkillReclamationCost, SkillManager_GetCurrentSkillReclamationCost);
  GAPI_LOAD(g, SM_GetNumMasteryPoints, SkillManager_GetNumMasteryPoints);
  GAPI_LOAD(g, GetDefaultSkillId, SkillManager_GetDefaultSkillId);
  GAPI_LOAD(g, SM_GetItemSkillList, SkillManager_GetItemSkillList);
  GAPI_LOAD(g, FindItemSkillIdByItemId, SkillManager_FindItemSkillIdByItemId);
  GAPI_LOAD(g, Skill_GetSkillLevel, Skill_GetSkillLevel);
  GAPI_LOAD(g, Skill_GetMaxLevel, Skill_GetMaxLevel);
  GAPI_LOAD(g, Skill_GetUltimateLevel, Skill_GetUltimateLevel);
  GAPI_LOAD(g, Skill_GetMasteryId, Skill_GetMasteryId);
  GAPI_LOAD(g, Skill_GetMasteryLevel, Skill_GetMasteryLevel);
  GAPI_LOAD(g, Skill_GetMasteryLevelRequirement, Skill_GetMasteryLevelRequirement);
  GAPI_LOAD(g, Skill_GetModifiers, Skill_GetModifiers);
  GAPI_LOAD(g, Skill_GetSecondarySkills, Skill_GetSecondarySkills);
  GAPI_LOAD(g, Skill_GetBaseSkills, Skill_GetBaseSkills);
  GAPI_LOAD(g, Skill_IsBaseSkillEnabled, Skill_IsBaseSkillEnabled);
  GAPI_LOAD(g, Skill_IsSecondary, Skill_IsSecondary);
  GAPI_LOAD(g, Skill_IsLocked, Skill_IsLocked);
  GAPI_LOAD(g, Skill_IsSkillTheMasterySkill, Skill_IsSkillTheMasterySkill);
  GAPI_LOAD(g, Skill_IsSkillModifier, Skill_IsSkillModifier);
  GAPI_LOAD(g, Skill_IsItemSkillAuto, Skill_IsItemSkillAuto);
  GAPI_LOAD(g, Skill_IsSkillEnabled, Skill_IsSkillEnabled);
  GAPI_LOAD(g, Skill_CreateUISkillName, Skill_CreateUISkillName);
  GAPI_LOAD(g, Skill_GetDisplayNameTag, Skill_GetDisplayNameTag);
  GAPI_LOAD(g, Skill_GetSkillProfile, Skill_GetSkillProfile);
  GAPI_LOAD(g, Profile_GetSkillTier, SkillProfile_GetSkillTier);
  GAPI_LOAD(g, Skill_IncrementSkillLevel, Skill_IncrementSkillLevel);
  GAPI_LOAD(g, Skill_DecrementSkillLevel, Skill_DecrementSkillLevel);
  GAPI_LOAD(g, Skill_SetSkillLevel, Skill_SetSkillLevel);
  GAPI_LOAD(g, Skill_vftable, Skill_vftable);
  GAPI_LOAD(g, GetSkillPoints, Character_GetSkillPoints);
  GAPI_LOAD(g, SubtractSkillPoint, Character_SubtractSkillPoint);
  GAPI_LOAD(g, AddSkillPoints, Character_AddSkillPoints);
  GAPI_LOAD(g, GetSkillMasteries, Character_GetSkillMasteries);
  GAPI_LOAD(g, GetSkillMasteriesAllowed, Character_GetSkillMasteriesAllowed);
  GAPI_LOAD(g, GenerateUISkillText, GameEngine_GenerateUISkillText);
  GAPI_LOAD(g, Object_GetObjectId, Object_GetObjectId);
  GAPI_LOAD(g, GetCharLevel, Character_GetCharLevel);
  GAPI_LOAD(g, GetExperiencePoints, Character_GetExperiencePoints);
  GAPI_LOAD(g, GetNextLevelExperience, Character_GetNextLevelExperience);
  GAPI_LOAD(g, GetModifierPoints, Character_GetModifierPoints);
  GAPI_LOAD(g, GetTotalCharAttribute, Character_GetTotalCharAttribute);
  GAPI_LOAD(g, GetAllDefenseAttributes, Character_GetAllDefenseAttributes);
  GAPI_LOAD(g, Acc_ctor, CombatAttributeAccumulator_ctor);
  GAPI_LOAD(g, Acc_dtor, CombatAttributeAccumulator_dtor);
  GAPI_LOAD(g, Acc_Clear, CombatAttributeAccumulator_Clear);
  GAPI_LOAD(g, Acc_GetTotalDefenseType, CombatAttributeAccumulator_GetTotalDefenseType);
  GAPI_LOAD(g, DesignerCalculateOffensiveAbility, Character_DesignerCalculateOffensiveAbility);
  GAPI_LOAD(g, DesignerCalculateDefensiveAbility, Character_DesignerCalculateDefensiveAbility);
  GAPI_LOAD(g, CalculateDps, Player_CalculateDps);
  GAPI_LOAD(g, GetClassNameA, Player_GetClassNameA);
  GAPI_LOAD(g, GetDevotionPoints, Character_GetDevotionPoints);
  GAPI_LOAD(g, GetCurrentLifeInt, Character_GetCurrentLifeInt);
  GAPI_LOAD(g, GetCurrentMana, Character_GetCurrentMana);
  GAPI_LOAD(g, Inc_Strength, ControllerCharacter_IncrementCharacterStrength);
  GAPI_LOAD(g, Inc_Dexterity, ControllerCharacter_IncrementCharacterDexterity);
  GAPI_LOAD(g, Inc_Intelligence, ControllerCharacter_IncrementCharacterIntelligence);
  GAPI_LOAD(g, Inc_Life, ControllerCharacter_IncrementCharacterLife);
  GAPI_LOAD(g, Inc_Mana, ControllerCharacter_IncrementCharacterMana);
  GAPI_LOAD(g, StrengthLifeIncrement, Character_GetStrengthLifeIncrement);
  GAPI_LOAD(g, DexterityLifeIncrement, Character_GetDexterityLifeIncrement);
  GAPI_LOAD(g, IntelligenceLifeIncrement, Character_GetIntelligenceLifeIncrement);
  GAPI_LOAD(g, CharacterExperienceOutbound, GameEngine_CharacterExperienceOutbound);
  GAPI_LOAD(g, DisplaySkillReallocationWindow, GameEngine_DisplaySkillReallocationWindow);
  g_s_enabled = vslot(g.Skill_vftable, (const void*)g.Skill_IsSkillEnabled);
  g_s_name = vslot(g.Skill_vftable, (const void*)g.Skill_CreateUISkillName);
  g_s_profile = vslot(g.Skill_vftable, (const void*)g.Skill_GetSkillProfile);
  g_s_inc = vslot(g.Skill_vftable, (const void*)g.Skill_IncrementSkillLevel);
  g_s_dec = vslot(g.Skill_vftable, (const void*)g.Skill_DecrementSkillLevel);
  g_s_set = vslot(g.Skill_vftable, (const void*)g.Skill_SetSkillLevel);
  log::writef("gameapi: Skill slots enabled={} name={} profile={} inc={} dec={}", g_s_enabled, g_s_name, g_s_profile, g_s_inc, g_s_dec);
}
const void* skill_manager() { load_skills(); void* p = player(); return p && g.GetSkillManager ? g.GetSkillManager(p) : nullptr; }

SkillInfo read_skill(void* s) {
  SkillInfo i{s};
  guarded("skill readout", [&] {
    i.id = g.Object_GetObjectId ? g.Object_GetObjectId(s) : 0;
    i.record = object_record(s);
    if (auto f = (MsvcStringW * (*)(const void*, MsvcStringW*, bool))vfn(s, g_s_name)) { MsvcStringW n; init_u16(n); f(s, &n, false); i.name = take_u16(n); }
    if (i.name.empty() && g.Skill_GetDisplayNameTag) i.name = localize(a_text(g.Skill_GetDisplayNameTag(s)));
    i.level = g.Skill_GetSkillLevel ? g.Skill_GetSkillLevel(s) : 0;
    i.max_level = g.Skill_GetMaxLevel ? g.Skill_GetMaxLevel(s) : 0;
    i.ultimate_level = g.Skill_GetUltimateLevel ? g.Skill_GetUltimateLevel(s) : 0;
    i.mastery_id = g.Skill_GetMasteryId ? g.Skill_GetMasteryId(s) : 0;
    i.mastery_level = g.Skill_GetMasteryLevel ? g.Skill_GetMasteryLevel(s) : 0;
    i.mastery_req = g.Skill_GetMasteryLevelRequirement ? g.Skill_GetMasteryLevelRequirement(s) : 0;
    // The base a sub-skill enhances: Skill::GetBaseSkills (the forward link the game's own learn gate walks) --
    // filled for Skill_Modifier AND SkillSecondary sub-skills (the Occultist's pet modifiers: Storm Spirit is a
    // SkillSecondary_PetModifier, 2026-09-13). skills() adds the reverse of GetModifiers/GetSecondarySkills as a
    // fallback. Skill::GetModifiedSkillId is a different (transform/replace) relationship and reads 0 for tree skills.
    if (g.Skill_GetBaseSkills) { std::vector<unsigned> bases = vec_items<unsigned>(g.Skill_GetBaseSkills(s), 8); if (!bases.empty()) i.modified_skill_id = bases[0]; }
    i.locked = g.Skill_IsLocked ? g.Skill_IsLocked(s) : false;
    i.is_mastery = g.Skill_IsSkillTheMasterySkill ? g.Skill_IsSkillTheMasterySkill(s) : false;
    // "modifier" to the player = a tree sub-skill of another skill: the Skill_Modifier class OR anything with a base
    // skill (the pet modifiers). NOT Skill::IsSecondary: live it reads true on Summon Familiar and false on Storm Spirit.
    i.modifier = (g.Skill_IsSkillModifier && g.Skill_IsSkillModifier(s)) || i.modified_skill_id != 0;
    i.item_auto = g.Skill_IsItemSkillAuto ? g.Skill_IsItemSkillAuto(s) : false;
    if (auto f = (bool (*)(const void*))vfn(s, g_s_enabled)) i.enabled = f(s);
    if (auto f = (const void* (*)(const void*))vfn(s, g_s_profile)) { const void* prof = f(s); if (prof && g.Profile_GetSkillTier) i.tier = g.Profile_GetSkillTier(prof); }
  });
  return i;
}
// Fallback for a sub-skill whose GetBaseSkills read empty: reverse the base skills' own lists. A base skill keeps
// its Skill_Modifier ids in GetModifiers and its SkillSecondary ids (pet modifiers) in GetSecondarySkills.
void link_bases(std::vector<SkillInfo>& out) {
  if (!g.Skill_GetModifiers && !g.Skill_GetSecondarySkills) return;
  std::unordered_map<unsigned, unsigned> base_of;   // sub-skill id -> base skill id
  for (const SkillInfo& s : out) {
    std::vector<unsigned> subs;
    guarded("GetModifiers", [&] {
      if (g.Skill_GetModifiers) if (const MemVec* v = g.Skill_GetModifiers(s.p)) subs = vec_items<unsigned>(v, 64);
      if (g.Skill_GetSecondarySkills) if (const MemVec* v = g.Skill_GetSecondarySkills(s.p)) for (unsigned m : vec_items<unsigned>(v, 64)) subs.push_back(m);
    });
    for (unsigned m : subs) base_of[m] = s.id;
  }
  for (SkillInfo& s : out) { if (s.modified_skill_id) continue; auto it = base_of.find(s.id); if (it != base_of.end()) s.modified_skill_id = it->second; }
}
}  // namespace

std::vector<SkillInfo> skills() {
  std::vector<SkillInfo> out;
  const void* sm = skill_manager();
  if (!sm || !g.SM_GetUISkillList) return out;
  std::vector<unsigned> ids;
  guarded("GetUISkillList", [&] { ids = vec_items<unsigned>(g.SM_GetUISkillList(sm), 1024); });
  for (unsigned id : ids) { void* s = object_by_id(id); if (s) out.push_back(read_skill(s)); }
  if (out.empty() && g.SM_GetSkillList) {  // the full list when the UI list is empty
    std::vector<void*> ptrs;
    guarded("GetSkillList", [&] { ptrs = vec_items<void*>(g.SM_GetSkillList(sm), 1024); });
    for (void* s : ptrs) if (s) out.push_back(read_skill(s));
  }
  link_bases(out);
  return out;
}
// A buff/debuff entry names itself by its skill id (SkillBuffTransfer+0x48): resolve the live Skill object and
// take its UI name. Name-only (no full read_skill), for the status-effect readouts (src/world.cpp, src/combat.cpp).
std::string skill_name_by_id(unsigned skill_id) {
  load_skills();
  void* s = object_by_id(skill_id);
  if (!s) return {};
  // SAFETY: only call the Skill vtable methods on an actual skill. object_by_id can return any object for a
  // stray id, and dispatching CreateUISkillName through a non-Skill vtable slot crashed/hung the game. A skill's
  // record lives under records/skills/...; Object::GetObjectName is a safe base call on any object.
  std::string rec = object_record(s);
  if (rec.find("skills/") == std::string::npos && rec.find("skills\\") == std::string::npos) return {};
  std::string name;
  guarded("skill name", [&] {
    if (auto f = (MsvcStringW * (*)(const void*, MsvcStringW*, bool))vfn(s, g_s_name)) { MsvcStringW n; init_u16(n); f(s, &n, false); name = take_u16(n); }
    if (name.empty() && g.Skill_GetDisplayNameTag) name = localize(a_text(g.Skill_GetDisplayNameTag(s)));
  });
  return name;
}
// The base skill a sub-skill (modifier or secondary) enhances, or 0: Skill::GetBaseSkills first, else the reverse
// of the base skills' GetModifiers / GetSecondarySkills. On-demand (a key press), so the one-pass scan is fine.
unsigned modifier_base_id(const void* skill) {
  load_skills();
  if (!skill) return 0;
  unsigned base = 0;
  if (g.Skill_GetBaseSkills) guarded("GetBaseSkills", [&] { std::vector<unsigned> b = vec_items<unsigned>(g.Skill_GetBaseSkills(skill), 8); if (!b.empty()) base = b[0]; });
  if (base) return base;
  const void* sm = skill_manager();
  if (!sm || !g.SM_GetSkillList || !g.Object_GetObjectId) return 0;
  unsigned my = 0; guarded("obj id", [&] { my = g.Object_GetObjectId(skill); });
  if (!my) return 0;
  std::vector<void*> ptrs;
  guarded("GetSkillList", [&] { ptrs = vec_items<void*>(g.SM_GetSkillList(sm), 1024); });
  for (void* s : ptrs) {
    if (!s) continue;
    std::vector<unsigned> subs;
    guarded("GetModifiers", [&] {
      if (g.Skill_GetModifiers) if (const MemVec* v = g.Skill_GetModifiers(s)) subs = vec_items<unsigned>(v, 64);
      if (g.Skill_GetSecondarySkills) if (const MemVec* v = g.Skill_GetSecondarySkills(s)) for (unsigned m : vec_items<unsigned>(v, 64)) subs.push_back(m);
    });
    for (unsigned m : subs) if (m == my) { guarded("obj id", [&] { base = g.Object_GetObjectId(s); }); break; }
    if (base) break;
  }
  return base;
}
unsigned skill_level(const void* skill) { load_skills(); unsigned n = 0; if (skill && g.Skill_GetSkillLevel) guarded("GetSkillLevel", [&] { n = g.Skill_GetSkillLevel(skill); }); return n; }
unsigned skill_points() { load_skills(); void* p = player(); unsigned n = 0; if (p && g.GetSkillPoints) guarded("GetSkillPoints", [&] { n = g.GetSkillPoints(p); }); return n; }
unsigned experience() { load_skills(); void* p = player(); unsigned n = 0; if (p && g.GetExperiencePoints) guarded("GetExperiencePoints", [&] { n = g.GetExperiencePoints(p); }); return n; }
// The character's current default skill for a role (0 = left mouse basic attack, 1 = right mouse), via the
// game's own SkillManager::GetDefaultSkillId -- computed live from the equipped weapon and skills, so it is
// always the correct instance to put back on a mouse button. Never cache the returned id.
unsigned default_skill_id(int role) {
  const void* sm = skill_manager();
  unsigned id = 0;
  if (sm && g.GetDefaultSkillId) guarded("GetDefaultSkillId", [&] { id = g.GetDefaultSkillId(sm, role); });
  return id;
}
// Everything that could go on a hotbar slot: the UI skill list plus item-granted skills, deduped by id, each
// as a SkillInfo (with names). The caller filters to what is actually activatable (world::skill_aim != None).
std::vector<SkillInfo> assignable_skills() {
  std::vector<SkillInfo> out;
  std::set<unsigned> seen;
  for (const SkillInfo& s : skills()) if (s.id && seen.insert(s.id).second) out.push_back(s);
  // Item-granted skills (GetItemSkillList returns them all, including multi-skill items -- a Chilled Steel
  // grants both Ice Spike and Chill Aura). read_skill fills item_auto (Skill::IsItemSkillAuto); the caller
  // drops the auto ones -- Ice Spike is a chance-on-attack PROC (not player-assignable) while Chill Aura is a
  // real toggle you can slot.
  for (unsigned id : item_skill_ids()) {
    if (!id || !seen.insert(id).second) continue;
    if (void* s = object_by_id(id)) out.push_back(read_skill(s));
  }
  return out;
}
// Skills granted by equipped items (SkillManager::GetItemSkillList -> mem::vector<Skill*>), as object ids.
std::vector<unsigned> item_skill_ids() {
  const void* sm = skill_manager();
  std::vector<unsigned> out;
  if (!sm || !g.SM_GetItemSkillList) return out;
  std::vector<void*> ptrs;
  guarded("GetItemSkillList", [&] { ptrs = vec_items<void*>(g.SM_GetItemSkillList(sm), 256); });
  for (void* s : ptrs) if (s && g.Object_GetObjectId) { unsigned id = 0; guarded("item skill id", [&] { id = g.Object_GetObjectId(s); }); if (id) out.push_back(id); }
  return out;
}
unsigned masteries_allowed() { load_skills(); void* p = player(); unsigned n = 0; if (p && g.GetSkillMasteriesAllowed) guarded("GetSkillMasteriesAllowed", [&] { n = g.GetSkillMasteriesAllowed(p); }); return n; }
std::vector<unsigned> mastery_ids() {
  std::vector<unsigned> out;
  load_skills(); void* p = player();
  if (!p || !g.GetSkillMasteries) return out;
  VecBuffer<unsigned> buf(64);
  guarded("GetSkillMasteries", [&] { g.GetSkillMasteries(p, buf.vec()); });
  return buf.take("GetSkillMasteries");
}
// The ten masteries use tagSkillClassName01..10 / tagSkillClassDescription01..10 and enumeration N =
// class{N+1:02}. 07-08 are Ashes of Malmouth, 09 is Forgotten Gods, and 10 is Fangs of Asterkarn. Earlier layers
// ship placeholder "?" text for unavailable masteries, so those entries are skipped.
std::vector<MasteryChoice> mastery_choices() {
  std::vector<MasteryChoice> out;
  for (int i = 0; i < 10; ++i) {
    MasteryChoice c{i, localize(std::format("tagSkillClassName{:02}", i + 1)), localize(std::format("tagSkillClassDescription{:02}", i + 1))};
    if (c.name.empty() || c.name == "?") continue;
    out.push_back(std::move(c));
  }
  return out;
}
const SkillInfo* mastery_skill(const std::vector<SkillInfo>& list, int enumeration) {
  std::string tail = std::format("/playerclass{:02}/_classtraining_class{:02}.dbr", enumeration + 1, enumeration + 1);
  for (const SkillInfo& s : list) if (s.is_mastery && s.record.size() >= tail.size() && s.record.compare(s.record.size() - tail.size(), tail.size(), tail) == 0) return &s;
  return nullptr;
}
std::string dump_item_skills() {
  std::string out = "item skills (GetItemSkillList); auto=proc/chance -> excluded from the palette:\n";
  for (unsigned id : item_skill_ids()) { void* s = object_by_id(id); SkillInfo i = s ? read_skill(s) : SkillInfo{}; out += std::format("  id={} '{}' auto={} {}\n", id, i.name, i.item_auto, i.record); }
  return out.size() > 60 ? out : out + "  (none)\n";
}
std::vector<std::string> skill_tooltip(const void* skill) {
  load_skills();
  std::vector<std::string> out;
  if (!skill || !g.GenerateUISkillText) return out;
  TextLineBuffer buf;
  alignas(16) unsigned char reasons[64] = {};   // SkillReasons: ~14 bools, never null-checked by the builder
  guarded("GenerateUISkillText", [&] { g.GenerateUISkillText(skill, buf.vec(), reasons, false, false, 0, 0x31, true); });
  for (TextLine& l : buf.take("skill text")) out.push_back(std::move(l.text));
  return out;
}
// Raising: IncrementSkillLevel(n) (= AddSkillLevel: level += n clamped to the profile's cap, recalc, the owner's
// level-changed notify) then DecrementSkillLevel(n) (= max(level - n, 0), the same notify; at 0 the owner's "skill
// removed" vt+0xc8). Lowering a learned skill to 0 therefore restores through SetSkillLevel(cur), whose > 0 path is
// the owner's "skill added" vt+0xc0 -- the pair the game itself uses around a level reaching / leaving 0; lowering to
// a level above 0 is the symmetric dec / inc. (Game.dll 0x46d480 SetSkillLevel, 0x46d520 Decrement, 0x47eed0 Add.)
std::vector<std::string> skill_tooltip_at(const void* skill, unsigned level) {
  load_skills();
  if (!skill) return {};
  unsigned cur = g.Skill_GetSkillLevel ? g.Skill_GetSkillLevel(skill) : 0;
  auto inc = (void (*)(void*, unsigned))vfn(skill, g_s_inc);
  auto dec = (void (*)(void*, unsigned))vfn(skill, g_s_dec);
  auto set = (void (*)(void*, unsigned))vfn(skill, g_s_set);
  if (level != cur && (!inc || !dec || !set)) return {};
  // Separate guards: a fault inside the text builder must not skip the restore.
  if (level > cur) guarded("skill_tooltip_at inc", [&] { inc((void*)skill, level - cur); });
  else if (level < cur) guarded("skill_tooltip_at dec", [&] { dec((void*)skill, cur - level); });
  std::vector<std::string> out = skill_tooltip(skill);
  if (level > cur) guarded("skill_tooltip_at dec", [&] { dec((void*)skill, level - cur); });
  else if (level < cur) guarded("skill_tooltip_at restore", [&] { if (level == 0) set((void*)skill, cur); else inc((void*)skill, cur - level); });
  if (g.Skill_GetSkillLevel && g.Skill_GetSkillLevel(skill) != cur) log::writef("gameapi: skill_tooltip_at left {} at level {} (was {})", object_record(skill), g.Skill_GetSkillLevel(skill), cur);
  return out;
}
// Dev: the whole skill list (SkillManager::GetSkillList, every mastery's tree whether chosen or not) grouped by the
// mastery enumeration parsed off the record path (records/skills/playerclassNN/...), each skill with the game's text at
// level 0 and at GetMaxLevel. Text lines are tab-indented so the parser (tools/gen_masteries_doc.py) never confuses them
// with the field lines.
std::string dump_masteries(std::string (*aim)(const void* skill)) {
  load_skills();
  const void* sm = skill_manager();
  std::string out;
  if (!sm || !g.SM_GetSkillList) return "no skill manager\n";
  std::vector<void*> ptrs;
  guarded("GetSkillList", [&] { ptrs = vec_items<void*>(g.SM_GetSkillList(sm), 2048); });
  std::vector<SkillInfo> list;
  for (void* s : ptrs) if (s) list.push_back(read_skill(s));
  link_bases(list);
  for (const MasteryChoice& c : mastery_choices()) out += std::format("mastery {} name={}\n", c.enumeration, c.name);
  for (const SkillInfo& s : list) {
    size_t at = s.record.find("playerclass");
    if (at == std::string::npos || at + 13 > s.record.size()) continue;
    int nn = atoi(s.record.substr(at + 11, 2).c_str());
    if (nn < 1) continue;
    std::string base;
    if (s.modified_skill_id) { void* b = object_by_id(s.modified_skill_id); if (b) base = object_record(b); }
    out += std::format("skill record={}\nname={}\nenum={} max={} ult={} req={} tier={} modifier={} base={} mastery_skill={} level={}\n",
                       s.record, s.name, nn - 1, s.max_level, s.ultimate_level, s.mastery_req, s.tier, (int)s.modifier, base, (int)s.is_mastery, s.level);
    if (aim) out += std::format("aim={}\n", aim(s.p));
    for (unsigned lvl : {0u, s.max_level}) {
      out += std::format("@level {}\n", lvl);
      for (const std::string& l : skill_tooltip_at(s.p, lvl)) out += "\t" + l + "\n";
    }
    out += "end\n";
  }
  return out;
}
// Whether the character can put a point into this skill right now, and if not, a spoken reason. Replicates the
// game's own skill-icon gate (the SkillReasons builder exe+0x2492b0): points>0, below max, and either the
// mastery skill (with a free mastery slot when committing a new one) or a non-mastery whose mastery bar has
// reached its GetMasteryLevelRequirement and whose base skill (for a sub-skill) is learned -- that last test is the
// game's own Skill::IsBaseSkillEnabled (walks GetBaseSkills; true when there is none), so it covers Skill_Modifier
// and SkillSecondary sub-skills alike. "" = allowed.
std::string can_learn_skill(const void* skill) {
  load_skills(); void* p = player();
  if (!skill || !p) return std::string(strings::kCannot);
  std::string reason;
  guarded("can_learn_skill", [&] {
    if (g.GetSkillPoints && g.GetSkillPoints(p) == 0) { reason = std::string(strings::kNoPoints); return; }
    unsigned lvl = g.Skill_GetSkillLevel ? g.Skill_GetSkillLevel(skill) : 0;
    unsigned max = g.Skill_GetMaxLevel ? g.Skill_GetMaxLevel(skill) : 0;
    if (max && lvl >= max) { reason = std::string(strings::kAtMaximum); return; }
    // The mastery ("class training") skill has req 0 and no base, so it passes the checks below and is always
    // learnable (raising the bar); choosing a NEW class is a separate flow (build_select / skills_set_pane).
    unsigned mlvl = g.Skill_GetMasteryLevel ? g.Skill_GetMasteryLevel(skill) : 0;
    unsigned mreq = g.Skill_GetMasteryLevelRequirement ? g.Skill_GetMasteryLevelRequirement(skill) : 0;
    if (mlvl < mreq) { reason = std::format("{} {}", strings::kRequiresMastery, mreq); return; }
    if (g.Skill_IsBaseSkillEnabled && !g.Skill_IsBaseSkillEnabled(skill)) {   // a sub-skill needs its base skill learned
      unsigned base = modifier_base_id(skill);
      void* bs = base ? object_by_id(base) : nullptr;
      std::string bname = bs ? read_skill(bs).name : std::string();
      reason = bname.empty() ? std::string(strings::kRequirementsNotMet) : std::format("{} {}", strings::kRequires, bname);
    }
  });
  return reason;
}
unsigned reclaim_cost() {
  const void* sm = skill_manager();
  unsigned n = 0;
  if (sm && g.SM_GetCurrentSkillReclamationCost) guarded("reclaim cost", [&] { n = g.SM_GetCurrentSkillReclamationCost(sm); });
  return n;
}
// Why a point can't be reclaimed right now (spirit-guide mode assumed), or "" if it can. Replicates the game's
// reclaim gate, which lives in the skills window's icon-enable pass (SkillReasons builder exe+0x2492b0, consumed by
// the pane update exe+0x247bf6..0x247cde), NOT in Game.dll: the exe's "-" click only refuses a mastery at level 1
// and trusts Skill::DecrementSkillLevel, which validates nothing (level > 0 -> subtract, return true). So a direct
// DecrementSkillLevel orphans modifiers / hosted powers / a mastery's dependants -- every reclaim path must pass here.
//   byte 0xb  level 0                                        -> nothing to reclaim
//   handler   mastery at level 1 (the class stays)           -> tagDecreaseMasteryError
//   byte 0xd  mastery: a learned skill of it needs the bar  -> "<skill> needs mastery N"
//   byte 0xa  level 1 with modifiers still holding points   -> remove points from its modifiers first, <names>
//   byte 7    level 1 hosting a celestial power             -> detach its celestial power first, <power>
//   byte 8    reclamation cost > money                       -> not enough iron bits
// Not replicated: the mastery also needs Engine::IsExpansion1Loaded (the icon stays grey without the expansion).
std::string can_reclaim_skill(const void* skill) {
  load_skills(); void* p = player();
  if (!skill || !p) return std::string(strings::kCannot);
  std::string reason;
  guarded("can_reclaim_skill", [&] {
    unsigned lvl = g.Skill_GetSkillLevel ? g.Skill_GetSkillLevel(skill) : 0;
    if (lvl == 0) { reason = std::string(strings::kNothingToReclaim); return; }
    bool mastery = g.Skill_IsSkillTheMasterySkill && g.Skill_IsSkillTheMasterySkill(skill);
    if (mastery && lvl <= 1) { reason = localize("tagDecreaseMasteryError"); return; }
    unsigned id = object_id(skill);
    if (mastery) {
      // Lowering the bar by one must not drop it under any learned skill's requirement (byte 0xd walks the pane's
      // skills, i.e. this mastery's tree). SkillInfo::mastery_id is the mastery SKILL's object id.
      for (const SkillInfo& s : skills()) {
        if (s.is_mastery || s.mastery_id != id || s.level == 0 || s.mastery_req < lvl) continue;
        core::MessageBuilder m;
        m.fragment(s.name.empty() ? s.record : s.name).fragment(strings::kRequiresMastery).fragment(std::format("{}", s.mastery_req));
        reason = m.build();
        return;
      }
    } else if (lvl == 1) {
      core::MessageBuilder m; bool any = false;
      for (const SkillInfo& s : skills()) {
        if (s.modified_skill_id != id || s.level == 0) continue;
        if (!any) m.fragment(strings::kRemoveModifiersFirst);
        any = true;
        m.list_item().fragment(s.name.empty() ? s.record : s.name);
      }
      if (any) { reason = m.build(); return; }
      if (unsigned power = hosted_power_id(skill)) {
        core::MessageBuilder pm; pm.fragment(strings::kDetachPowerFirst);
        std::string pname = skill_name_by_id(power);
        if (!pname.empty()) pm.list_item().fragment(pname);
        reason = pm.build();
        return;
      }
    }
    if (reclaim_cost() > money()) { reason = std::string(strings::kNotEnoughBits); return; }
  });
  return reason;
}
// The skills window's own "+" (exe+0x248505): points left, below max, ReleasePets, IncrementSkillLevel(1),
// SubtractSkillPoint. Gated by can_learn_skill so requirements (mastery rank, modifier base) are respected.
bool learn_skill(const void* skill) {
  load_skills(); void* p = player();
  if (!skill || !p || !g.GetSkillPoints || !g.SubtractSkillPoint) return false;
  if (!can_learn_skill(skill).empty()) return false;
  bool ok = false;
  guarded("learn skill", [&] {
    if (g.GetSkillPoints(p) == 0) return;
    if (g.Skill_GetMaxLevel && g.Skill_GetSkillLevel && g.Skill_GetSkillLevel(skill) >= g.Skill_GetMaxLevel(skill)) return;
    auto inc = (void (*)(void*, unsigned))vfn(skill, g_s_inc);
    if (!inc) return;
    if (auto release = (void (*)(void*))vfn(skill, kSlotReleasePets)) release((void*)skill);
    inc((void*)skill, 1);
    g.SubtractSkillPoint(p);
    ok = true;
  });
  log::writef("gameapi: learn skill {} ok={}", skill, ok);
  return ok;
}
// The window's reallocation "-" (exe+0x248459): never the mastery's last point; DecrementSkillLevel(1), then
// UseReclamationPoints(1) (undone on refusal), ReleasePets, AddSkillPoints(1). Gated by can_reclaim_skill, because
// the game's own gate is the greyed icon, which this direct path does not see.
bool refund_skill(const void* skill) {
  load_skills(); void* p = player(); const void* sm = skill_manager();
  if (!skill || !p || !sm || !g.SM_UseReclamationPoints || !g.AddSkillPoints) return false;
  if (!can_reclaim_skill(skill).empty()) return false;
  bool ok = false;
  guarded("refund skill", [&] {
    if (g.Skill_IsSkillTheMasterySkill && g.Skill_IsSkillTheMasterySkill(skill) && g.Skill_GetSkillLevel && g.Skill_GetSkillLevel(skill) <= 1) return;
    if (g.Skill_GetSkillLevel && g.Skill_GetSkillLevel(skill) == 0) return;
    auto dec = (bool (*)(void*, unsigned))vfn(skill, g_s_dec);
    auto inc = (void (*)(void*, unsigned))vfn(skill, g_s_inc);
    if (!dec || !inc) return;
    if (!dec((void*)skill, 1)) return;
    if (!g.SM_UseReclamationPoints((void*)sm, 1)) { inc((void*)skill, 1); return; }
    if (auto release = (void (*)(void*))vfn(skill, kSlotReleasePets)) release((void*)skill);
    g.AddSkillPoints(p, 1);
    ok = true;
  });
  log::writef("gameapi: refund skill {} ok={}", skill, ok);
  return ok;
}
// Dev: open the skills window in spirit-guide reclaim mode (GameEngine::DisplaySkillReallocationWindow, the exact
// path an NpcSkillReallocator uses). Lets reclaim be tested without walking to a guide. Game thread.
bool dev_open_skill_reclaim() {
  load_skills(); void* e = engine();
  if (!e || !g.DisplaySkillReallocationWindow) return false;
  bool ok = guarded("DisplaySkillReallocationWindow", [&] { g.DisplaySkillReallocationWindow(e); });
  log::writef("gameapi: dev open skill reclaim ok={}", ok);
  return ok;
}
bool dev_add_experience(unsigned xp) {
  load_skills(); void* e = engine(); void* p = player();
  if (!e || !p || !g.CharacterExperienceOutbound) return false;
  bool ok = guarded("CharacterExperienceOutbound", [&] { g.CharacterExperienceOutbound(e, object_id(p), xp); });
  log::writef("gameapi: dev experience {} ok={}", xp, ok);
  return ok;
}
std::string dump_skills() {
  std::string out = std::format("skill points {} masteries allowed {} mastery ids:", skill_points(), masteries_allowed());
  for (unsigned m : mastery_ids()) out += std::format(" {}", m);
  out += "\n";
  for (const MasteryChoice& c : mastery_choices()) out += std::format("  mastery {} '{}'\n", c.enumeration, c.name);
  for (const SkillInfo& s : skills())
    out += std::format("  skill {} id={} '{}' lvl {}/{} (ult {}) mastery={} mlvl={} req={} modifies={} tier={} locked={} mastery_skill={} enabled={} modifier={} {}\n", s.p, s.id, s.name, s.level, s.max_level, s.ultimate_level, s.mastery_id, s.mastery_level, s.mastery_req, s.modified_skill_id, s.tier, s.locked, s.is_mastery, s.enabled, s.modifier, s.record);
  return out;
}

// ---- the character sheet ----
// The first stat tab's rows (exe+0x13d870): level and class, the attributes (CharAttributeType 4 health, 5
// energy, 1 physique, 2 cunning, 3 spirit), offensive / defensive ability, DPS, and the ten resistances by
// defense type. Resistances use the character's own defense accumulator (the exe adds the skill manager's and
// bio's contributions and the reductions on top; first pass).
unsigned attribute_points() { load_skills(); void* p = player(); unsigned n = 0; if (p && g.GetModifierPoints) guarded("GetModifierPoints", [&] { n = g.GetModifierPoints(p); }); return n; }
std::vector<Stat> character_sheet() {
  load_skills();
  std::vector<Stat> out;
  void* p = player();
  if (!p) return out;
  // Truncate, never round: attributes are fractional floats and the equip gate compares the raw value against a
  // whole-number requirement, so 391.7 must read "391" (a rounded "392" let a 392-Physique shield refuse, 2026-09-11).
  auto num = [](double v) { return std::format("{}", (long long)v); };
  guarded("sheet", [&] {
    std::string cls;
    if (g.GetClassNameA) { MsvcStringW s; init_u16(s); g.GetClassNameA(p, &s); cls = take_u16(s); }
    out.push_back({std::string(strings::kLevel), g.GetCharLevel ? num(g.GetCharLevel(p)) : std::string()});
    out.push_back({std::string(strings::kClass), cls.empty() ? std::string(strings::kNoClass) : cls});
    if (g.GetExperiencePoints && g.GetNextLevelExperience) out.push_back({std::string(strings::kExperience), std::format("{} of {}", g.GetExperiencePoints(p), g.GetNextLevelExperience(p))});
    if (g.GetModifierPoints) out.push_back({std::string(strings::kAttributePoints), num(g.GetModifierPoints(p))});
    out.push_back({std::string(strings::kSkillPoints), num(g.GetSkillPoints ? g.GetSkillPoints(p) : 0)});
    if (g.GetDevotionPoints) out.push_back({std::string(strings::kDevotionPoints), num(g.GetDevotionPoints(p))});
    out.push_back({std::string(strings::kAffinities), affinities_text()});   // one row: "Ascendant 3, Chaos 1" (docs/devotion.md)
    if (g.GetTotalCharAttribute) {
      if (g.GetCurrentLifeInt) out.push_back({localize("tagCharAttributeName04"), std::format("{} of {}", g.GetCurrentLifeInt(p), (int)g.GetTotalCharAttribute(p, 4)), 0, localize("tagCharAttributeDescription04")});
      if (g.GetCurrentMana) out.push_back({localize("tagCharAttributeName05"), std::format("{:.0f} of {:.0f}", g.GetCurrentMana(p), g.GetTotalCharAttribute(p, 5)), 0, localize("tagCharAttributeDescription05")});
      out.push_back({localize("tagCharAttributeName02"), num(g.GetTotalCharAttribute(p, 1)), 1, localize("tagCharAttributeDescription02")});   // Physique
      out.push_back({localize("tagCharAttributeName01"), num(g.GetTotalCharAttribute(p, 2)), 2, localize("tagCharAttributeDescription01")});   // Cunning
      out.push_back({localize("tagCharAttributeName03"), num(g.GetTotalCharAttribute(p, 3)), 3, localize("tagCharAttributeDescription03")});   // Spirit
    }
    if (g.DesignerCalculateOffensiveAbility) out.push_back({localize("tagCharStatsOA"), num(g.DesignerCalculateOffensiveAbility(p, 0.0f)), 0, localize("tagCharStatsOADescription")});
    if (g.DesignerCalculateDefensiveAbility) out.push_back({localize("tagCharStatsDA"), num(g.DesignerCalculateDefensiveAbility(p, 0.0f)), 0, localize("tagCharStatsDADescription")});
    if (g.CalculateDps) { float dps = 0; g.CalculateDps(p, &dps, 0); out.push_back({std::string(strings::kDps), num(dps), 0, localize("tagCharStatsDPSDescription")}); }
    if (g.GetAllDefenseAttributes && g.Acc_ctor && g.Acc_dtor && g.Acc_GetTotalDefenseType) {
      struct R { const char* tag; int type; } rows[] = {{"tagStatsResistance01", 6}, {"tagStatsResistance03", 5}, {"tagStatsResistance02", 8}, {"tagStatsResistance04", 7},
                                                      {"tagStatsResistance05", 4}, {"tagStatsResistance06", 15}, {"tagStatsResistance07", 9}, {"tagStatsResistance08", 11},
                                                      {"tagStatsResistance09", 2}, {"tagStatsResistance10", 10}};
      alignas(16) unsigned char acc[1024] = {};
      g.Acc_ctor(acc);
      g.GetAllDefenseAttributes(p, acc);
      for (const R& r : rows) out.push_back({localize(r.tag), std::format("{:.0f} {}", g.Acc_GetTotalDefenseType(acc, r.type), strings::kPercent), 0, localize(std::string(r.tag) + "Desc")});
      g.Acc_dtor(acc);
    }
  });
  return out;
}
// The sheet's "+" buttons (exe+0x141090): through the controller, with the life / energy increments.
bool spend_attribute_point(int which) {
  load_skills(); void* p = player(); void* c = controller();
  if (!p || !c || !g.GetModifierPoints || !g.Inc_Life) return false;
  bool ok = false;
  guarded("spend attribute point", [&] {
    if (g.GetModifierPoints(p) == 0) return;
    if (which == 1 && g.Inc_Strength && g.StrengthLifeIncrement) { g.Inc_Strength(c); g.Inc_Life(c, (int)g.StrengthLifeIncrement(p)); ok = true; }
    else if (which == 2 && g.Inc_Dexterity && g.DexterityLifeIncrement) { g.Inc_Dexterity(c); g.Inc_Life(c, (int)g.DexterityLifeIncrement(p)); ok = true; }
    else if (which == 3 && g.Inc_Intelligence && g.IntelligenceLifeIncrement && g.Inc_Mana) { g.Inc_Intelligence(c); g.Inc_Life(c, (int)g.IntelligenceLifeIncrement(p)); g.Inc_Mana(c); ok = true; }
  });
  log::writef("gameapi: spend attribute {} ok={}", which, ok);
  return ok;
}
std::string dump_sheet() {
  std::string out;
  for (const Stat& s : character_sheet()) out += std::format("  {}: {}\n", s.label, s.value);
  return out.empty() ? "no sheet\n" : out;
}
}  // namespace gd::gameapi

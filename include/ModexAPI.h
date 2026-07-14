#pragma once

// ModexAPI.h - Public API for Modex (Mod Explorer Menu)
//
// This is the distributable header for other SKSE plugins to consume.
// Copy this file into your project and include it to interact with Modex.
//
// Usage:
//   1. Include this header in your plugin.
//   2. After SKSE sends kPostLoad, call ModexAPI::GetModexInterface001().
//   3. Use the returned interface pointer to interact with Modex.
//
// Note:
//   See Commands.h for implementations. Most methods defer to the games main
//   thread since Modex typically calls them from the UI thread. This is important
//   to consider when listening for events called from this API.
//
// Interface code based on https://github.com/adamhynek/higgs
// Notable Mention: https://github.com/Nightfallstorm/DescriptionFramework

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace ModexAPI
{
	constexpr const auto ModexPluginName = "Modex";

	// Publicly available enums for querying Modex's cached form data.
	enum class CacheType : uint32_t
	{
		kItem = 0,
		kNPC,
		kObject,
		kCell,
		kOutfit,
		kKit,
		kSpell,
	};

	// Property types for querying form data via GetFormProperty().
	// These mirror Modex's internal PropertyType enum for API stability.
	enum class PropertyType : uint32_t
	{
		kNone = 0,
		kPlugin = 19,
		kFormID = 20,
		kName = 21,
		kEditorID = 22,
		kFormType = 23,
		kWeaponDamage = 25,
		kWeaponSpeed = 26,
		kWeaponType = 27,
		kWeaponSkill = 28,
		kWeaponCriticalDamage = 29,
		kWeaponDamagePerSecond = 30,
		kWeaponRange = 31,
		kWeaponStagger = 32,
		kArmorRating = 34,
		kArmorSlot = 35,
		kArmorType = 36,
		kPlayable = 37,
		kEnchanted = 38,
		kGoldValue = 39,
		kCarryWeight = 40,
		kReferenceID = 41,
		kUnique = 42,
		kEssential = 43,
		kDisabled = 45,
		kHealth = 46,
		kMagicka = 47,
		kStamina = 48,
		kRace = 49,
		kGender = 50,
		kClass = 51,
		kLevel = 52,
		kDefaultOutfit = 53,
		kSleepOutfit = 54,
		kKeywordList = 56,
		kFactionList = 58,
		kSpellList = 60,
		kSpellType = 64,
		kSpellCastType = 65,
		kSpellDelivery = 66,
		kTomeSpell = 67,
		kTomeSkill = 68,
		kOutfitItems = 72,
		kPersistent = 73,
		kDeleted = 74,
		kSpellSkill = 83,      // School of magic for spells
		kKnownByTarget = 84,   // True if Modex's current table target knows this spell
		kKitTags = 85,         // Comma-separated tag string on a kit entry
		kKitGoldValue = 86     // Precomputed total gold value of a kit's items
	};

	// Options for configuring a selector UI before opening. Shared shape between the form
	// and kit selectors — both consume the same fields. Use the alias `KitSelectorOptions`
	// when configuring the kit selector for readability.
	struct SelectorOptions
	{
		bool singleSelect{ false };       // Replace selection instead of accumulating
		bool showTotalCost{ false };      // Display gold cost tally in action pane
		bool requireTotalCost{ false };   // Enforce player can afford total cost
		int maxCost{ 0 };                 // Maximum total gold value (0 = unlimited)
		int maxCount{ 0 };                // Maximum number of selections (0 = unlimited)
		float costMultiplier{ 1.0f };     // Scale factor for gold costs
		const char* title{ nullptr };     // Custom window title (nullptr = default)
	};

	using FormSelectorOptions = SelectorOptions;  // existing name; kept for ABI compatibility
	using KitSelectorOptions  = SelectorOptions;  // readability alias for kit selector calls

	// Lightweight form entry returned by GetCachedForms().
	// String pointers are valid for the lifetime of the Modex cache (until game exit).
	struct FormEntry
	{
		uint32_t  formID;
		uint32_t  refID;
		RE::FormType formType;
		const char* name;
		const char* editorID;
		const char* plugin;
	};

	// Kit entry returned by GetCachedKits().
	// String pointers are valid for the lifetime of the Modex kit cache (until game exit).
	struct KitEntry
	{
		const char* key;          // Unique kit identifier (relative path)
		const char* name;         // Display name (file stem)
		const char* collection;   // Collection/category grouping
		uint32_t    itemCount;    // Number of items in the kit
		uint32_t    spellCount;   // Number of spells in the kit
		int32_t     goldValue;    // Total gold value of all items (spells have no gold value)
	};

	// A message used to fetch Modex's interface.
	struct ModexMessage
	{
		enum : uint32_t
		{
			kMessage_QueryInterface = 'MODX' // 0x4d4f4458
		};

		void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
	};

	// Returns an IModexInterface001 object compatible with the API shown below.
	// This should only be called after SKSE sends kPostLoad to your plugin.
	struct IModexInterface001;
	IModexInterface001* GetModexInterface001();

	struct IModexInterface001
	{
		/// Returns true if Modex has finished caching form data.
		virtual bool IsDataReady() = 0;

		/// Force reload user settings from JSON
		virtual void UpdateSettings() = 0;

		/// Add the specified FormID/EditorID to the player's inventory directly.
		virtual void AddItemToPlayer(uint32_t a_formID, uint32_t a_amount = 1) = 0;

		/// Add the specified FormID/EditorID to the reference FormID inventory.
		virtual void AddItemToInventory(uint32_t a_formID, uint32_t a_targetReference, uint32_t a_amount = 1) = 0;

		/// Add & Resolve a LeveledList FormID/EditorID to the target reference.
		virtual void AddLeveledListToInventory(uint32_t a_formID, uint32_t a_targetReference, uint16_t a_amount = 1) = 0;

		/// Incrementally remove all items from the specified reference inventory.
		virtual void RemoveAllItems(uint32_t a_targetReference) = 0;

		/// Opens the virtual Modex container used when showing results in-game.
		virtual void OpenModexContainer() = 0;

		/// Opens the reference FormID's inventory. Internally handled invalid arguments passed.
		virtual void OpenInventory(uint32_t a_targetReference) = 0;

		/// Returns the number of cached forms in the given category.
		virtual unsigned int GetCachedFormCount(CacheType a_type) = 0;

		/// Returns true if the given form ID exists in the specified cache.
		virtual bool IsFormCached(uint32_t a_formID, CacheType a_type) = 0;

		/// Remove the specified EditorID from the target reference's inventory.
		virtual void RemoveItem(uint32_t a_formID, uint32_t a_targetReference, uint32_t a_amount = 1) = 0;

		/// Spawn the specified EditorID at the player's location.
		virtual void PlaceAtMe(uint32_t a_formID, uint32_t a_count = 1, bool a_persistent = true, bool a_disabled = false) = 0;

		/// Teleport the player to the specified NPC reference.
		virtual void TeleportPlayerToNPC(uint32_t a_refID) = 0;

		/// Teleport the specified NPC reference to the player.
		virtual void TeleportNPCToPlayer(uint32_t a_refID) = 0;

		/// Teleport the player to the specified cell by editor ID.
		virtual void CenterOnCell(const char* a_cellEditorID) = 0;

		/// Kill the actor at the specified reference.
		virtual void KillActor(uint32_t a_refID) = 0;

		/// Resurrect the actor at the specified reference.
		virtual void ResurrectActor(uint32_t a_refID) = 0;

		/// Disable the specified reference.
		virtual void DisableReference(uint32_t a_refID) = 0;

		/// Enable the specified reference.
		virtual void EnableReference(uint32_t a_refID, bool a_resetInventory = false) = 0;

		/// Opens the Modex menu. No-op if already open.
		virtual void OpenMenu() = 0;

		/// Closes the Modex menu. No-op if already closed.
		virtual void CloseMenu() = 0;

		/// Returns true if the Modex menu is currently open.
		virtual bool IsMenuOpen() = 0;

		/// Opens the Form Selector UI, allowing the user to search and select forms.
		/// The callback is invoked when the user confirms their selection.
		/// @param a_type     The category of forms to display (Item, NPC, Cell, etc).
		/// @param a_callback Called with an array of selected FormIDs and the count.
		///                   The pointer is valid only for the duration of the callback.
		virtual void OpenFormSelector(CacheType a_type, void (*a_callback)(const uint32_t* a_formIDs, uint32_t a_count)) = 0;

		/// Copies cached form entries into a user-provided buffer.
		/// @param a_type      The cache category to read from.
		/// @param a_outBuffer Pointer to a FormEntry array to fill. Pass nullptr to query count only.
		/// @param a_maxCount  Maximum number of entries to write into a_outBuffer.
		/// @return            The total number of forms in the cache (regardless of a_maxCount).
		virtual uint32_t GetCachedForms(CacheType a_type, FormEntry* a_outBuffer, uint32_t a_maxCount) = 0;

		/// Query a property value for a specific cached form.
		/// @param a_formID   The base FormID to look up.
		/// @param a_type     The cache category to search in.
		/// @param a_property The property to retrieve.
		/// @param a_outBuffer Buffer to write the result string into.
		/// @param a_bufferSize Size of the output buffer in bytes.
		/// @return           True if the form was found and the property was written.
		virtual bool GetFormProperty(uint32_t a_formID, CacheType a_type, PropertyType a_property, char* a_outBuffer, uint32_t a_bufferSize) = 0;

		/// Opens the Form Selector UI with custom options.
		/// @param a_type     The category of forms to display.
		/// @param a_options  Configuration options for the selector behavior.
		/// @param a_callback Called with an array of selected FormIDs and the count.
		virtual void OpenFormSelector(CacheType a_type, const FormSelectorOptions& a_options, void (*a_callback)(const uint32_t* a_formIDs, uint32_t a_count)) = 0;

		/// Resolve an outfit's items and add them to the target reference's inventory.
		/// @param a_outfitFormID The FormID of the BGSOutfit to resolve.
		/// @param a_targetReference The reference to receive the items.
		/// @param a_level Level override for leveled list resolution (0 = use target's level).
		virtual void AddOutfitToInventory(uint32_t a_outfitFormID, uint32_t a_targetReference, uint16_t a_level = 0) = 0;

		/// Resolve an outfit's items, add them to the target, and equip them.
		/// @param a_outfitFormID The FormID of the BGSOutfit to resolve and equip.
		/// @param a_targetReference The actor reference to equip on.
		/// @param a_level Level override for leveled list resolution (0 = use target's level).
		virtual void EquipOutfit(uint32_t a_outfitFormID, uint32_t a_targetReference, uint16_t a_level = 0) = 0;

		/// Set the default outfit on the target actor's base record.
		/// @param a_outfitFormID The FormID of the BGSOutfit to assign.
		/// @param a_targetReference The actor reference whose base record to modify.
		virtual void SetDefaultOutfit(uint32_t a_outfitFormID, uint32_t a_targetReference) = 0;

		/// Set the sleep outfit on the target actor's base record.
		/// @param a_outfitFormID The FormID of the BGSOutfit to assign.
		/// @param a_targetReference The actor reference whose base record to modify.
		virtual void SetSleepOutfit(uint32_t a_outfitFormID, uint32_t a_targetReference) = 0;

		/// Opens the Kit Selector UI, allowing the user to browse and select equipment kits.
		/// The callback is invoked when the user confirms their selection.
		/// @param a_callback Called with an array of kit key strings and the count.
		///                   The pointers are valid only for the duration of the callback.
		virtual void OpenKitSelector(void (*a_callback)(const char* const* a_kitKeys, uint32_t a_count)) = 0;

		/// Opens the Kit Selector UI with custom options.
		/// @param a_options  Configuration options for the selector behavior.
		/// @param a_callback Called with an array of kit key strings and the count.
		virtual void OpenKitSelector(const KitSelectorOptions& a_options, void (*a_callback)(const char* const* a_kitKeys, uint32_t a_count)) = 0;

		/// Copies cached kit entries into a user-provided buffer.
		/// @param a_outBuffer Pointer to a KitEntry array to fill. Pass nullptr to query count only.
		/// @param a_maxCount  Maximum number of entries to write into a_outBuffer.
		/// @return            The total number of kits in the cache (regardless of a_maxCount).
		virtual uint32_t GetCachedKits(KitEntry* a_outBuffer, uint32_t a_maxCount) = 0;

		/// Teach a spell to the actor at the specified reference.
		/// No-op if the actor already knows the spell.
		/// @param a_spellFormID FormID of the SpellItem to teach.
		/// @param a_targetReference Actor reference to teach the spell to.
		virtual void AddSpellToActor(uint32_t a_spellFormID, uint32_t a_targetReference) = 0;

		/// Remove a spell from the actor at the specified reference.
		/// @param a_spellFormID FormID of the SpellItem to remove.
		/// @param a_targetReference Actor reference to remove the spell from.
		virtual void RemoveSpellFromActor(uint32_t a_spellFormID, uint32_t a_targetReference) = 0;
	};

}  // namespace ModexAPI

extern ModexAPI::IModexInterface001* g_ModexInterface;

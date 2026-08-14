#pragma once

// The LINKED GAME's configs as the editor's own.
//
// Gameplay content is not files, it is config SECTIONS: the Spawn tool lists
// every section with a $spawn line, the server-entity factory builds an entity
// from its class - all of it read from pSettings. Out of the box pSettings
// comes from the editor's bundled configs, so the author placing "actors with
// logic" saw the SDK's stock roster instead of what their game really has.
//
// Activate() extracts configs\** of the linked install (archives + loose, the
// game's own precedence) into a cache folder under the editor's _appdata_,
// repoints $game_config$ there and rebuilds pSettings from the game's
// system.ltx. The extraction is fingerprinted, so a relink to the same install
// is a no-op. The FACTORY reload is the caller's job: XrECore cannot see
// g_SEFactoryManager, and entity statics have to be rebuilt right after the
// swap (see the hook in UIMainForm).
namespace EditorGameConfigs
{
	// true when pSettings now describes the linked game (fresh or cached).
	// false + reason when there is no linked game or the extraction failed -
	// the editor then keeps its own configs, which still work.
	ECORE_API bool		Activate			(xr_string& err);

	// fingerprint of what is active; empty = the editor's bundled configs.
	// The caller compares this across frames to notice a project/link change.
	ECORE_API LPCSTR	ActiveFingerprint	();
}

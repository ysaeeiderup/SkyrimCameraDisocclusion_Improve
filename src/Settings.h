#pragma once

// Plugin-level user-tunable settings, read once at SKSE init from
// Data\SKSE\Plugins\skyrimcameradisocclusion.ini. Defaults are hardcoded
// here; missing/malformed INI silently falls back to the default.
//
// Inline globals (C++17) so each setting is one definition, accessed by
// name everywhere. Mutable in form but written only by Settings::Load();
// after that point they're effectively read-only.
namespace Settings
{
	// Vertical offset added to the player's position before computing the
	// ray fan target. Higher = aim toward head/torso (default ~150 ≈ chest
	// height for a standard human race).
	inline float PlayerHeightOffset = 150.0f;

	// Half-width / half-height of the elliptical ray fan around the
	// camera→player segment. The fan biases horizontally because walls
	// and columns are the most common occluders that need clipping.
	inline float FanRadiusH = 20.0f;
	inline float FanRadiusV = 6.0f;

	// How long the outdoor strip stays active after the most recent ray hit
	// (seconds). Brief — the strip turns off shortly after you stop looking
	// through walls so occluders snap back to normal.
	inline float StripLingerSeconds = 0.3f;

	// How long a hit object persists in the clip-distance set (seconds).
	// Longer than StripLingerSeconds so the visual clip doesn't twitch
	// across single-frame ray misses.
	inline float HitLingerSeconds = 2.0f;

	// Read INI values into the inline globals. Idempotent.
	void Load();
}

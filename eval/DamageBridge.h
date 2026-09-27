/*
 * DamageBridge.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * Bridges shadow-evaluation calls into the engine's script layer. The
 * architecture doc targets scripts/damage/damageCalculator.lua; in VCMI 1.7.5
 * that script is not shipped (damage calc is C++ via battle::DamageCalculator),
 * so the bridge resolves whatever damage script the active mod set registers
 * through scripting::Service, calls it via scripting::Context::callGlobal with
 * a namespaced JsonNode self-table, and falls back to the native C++ estimate
 * when no script is present.
 *
 * Each TBB worker gets its own headless scripting::Pool so Lua state mutexes
 * never serialize the arena.
 */
#pragma once

#include "../StdInc.h"

VCMI_LIB_NAMESPACE_BEGIN
class JsonNode;
class Environment;
namespace scripting { class Context; class Pool; class Script; }
VCMI_LIB_NAMESPACE_END

namespace omniai
{

class DamageBridge
{
public:
	explicit DamageBridge(const Environment * env);

	/// True when a script registered under the damage-calculator name exists.
	bool scriptAvailable() const;

	/// Call the damage script. Params arrive in Lua as the script's self
	/// table; all OmniAI keys are namespaced "ai_" so they cannot shadow
	/// script methods. Returns empty JsonNode when no script is loaded.
	JsonNode callDamage(const JsonNode & params);

	/// C++ fallback estimate: expected damage of attackerStats on
	/// defenderStats using the standard Heroes III attack/defence formula.
	/// attackerStats/defenderStats carry keys: attack, defense, minDamage,
	/// maxDamage, count, isRanged.
	double estimateDamage(const JsonNode & attackerStats, const JsonNode & defenderStats) const;

private:
	const Environment * env_;
	const scripting::Script * script_ = nullptr;

	// One scripting pool per OS thread. thread_local keeps worker contexts
	// fully independent so the lua_State mutex never contends the arena.
	std::shared_ptr<scripting::Context> threadContext();
};

}

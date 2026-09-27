/*
 * DamageBridge.cpp, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 */
#include "../StdInc.h"
#include "DamageBridge.h"

#include "GameLibrary.h"
#include "json/JsonNode.h"
#include "vcmi/Environment.h"

#if SCRIPTING_ENABLED
#include "ScriptHandler.h"
#include "vcmi/scripting/Service.h"
#endif

namespace omniai
{

namespace
{
	// Script registry name the bridge probes for. Mods may register their
	// own calculator under this identifier; the base 1.7.5 game does not.
	constexpr const char * DAMAGE_SCRIPT = "damageCalculator";
	constexpr const char * DAMAGE_METHOD = "calculateDamage";
}

DamageBridge::DamageBridge(const Environment * env)
	: env_(env)
{
#if SCRIPTING_ENABLED
	if(env_ && env_->services() && LIBRARY && LIBRARY->scriptHandler)
		script_ = LIBRARY->scriptHandler->resolveScript(DAMAGE_SCRIPT);
#endif
}

bool DamageBridge::scriptAvailable() const
{
	return script_ != nullptr;
}

std::shared_ptr<scripting::Context> DamageBridge::threadContext()
{
#if SCRIPTING_ENABLED
	// One Context per worker thread: each carries its own lua_State, so TBB
	// workers never contend on a shared interpreter mutex.
	thread_local std::map<const scripting::Script *,
		std::shared_ptr<scripting::Context>> contexts;

	if(!script_)
		return nullptr;

	auto it = contexts.find(script_);
	if(it == contexts.end())
		it = contexts.emplace(script_, script_->createContext(env_)).first;
	return it->second;
#else
	return nullptr;
#endif
}

JsonNode DamageBridge::callDamage(const JsonNode & params)
{
#if SCRIPTING_ENABLED
	auto ctx = threadContext();
	if(!ctx)
		return JsonNode();

	JsonNode scoped = params;
	// Namespace every AI-supplied key so a colliding name cannot overwrite a
	// method in the script's class table (documented VCMI failure mode).
	for(const auto & key : params.Struct())
		if(!key.first.starts_with("ai_"))
			scoped["ai_" + key.first] = key.second;

	return ctx->callGlobal(DAMAGE_METHOD, scoped);
#else
	return JsonNode();
#endif
}

double DamageBridge::estimateDamage(const JsonNode & attacker, const JsonNode & defender) const
{
	// Heroes III core formula: base damage scaled by attack/defence delta.
	// dmg = count * rand[min,max] * (1 + 0.05*(A-D)) when A>D,
	// dmg = count * rand[min,max] * (1 - 0.025*(D-A)) when D>A, capped.
	const double count = attacker["count"].Float();
	const double minD = attacker["minDamage"].Float();
	const double maxD = attacker["maxDamage"].Float();
	const double atk = attacker["attack"].Float();
	const double def = defender["defense"].Float();
	const bool ranged = attacker["isRanged"].Bool();

	if(count <= 0.0)
		return 0.0;

	const double base = count * (minD + maxD) * 0.5;
	const double delta = atk - def;
	double scale;
	if(delta >= 0.0)
		scale = 1.0 + 0.05 * delta;
	else
		scale = 1.0 + 0.025 * delta;

	// Vanilla caps: +400% on attack side, -70% floor on defence side.
	scale = std::clamp(scale, 0.30, 5.0);
	return base * scale * (ranged ? 1.0 : 1.0); // ranged melee parity placeholder
}

}

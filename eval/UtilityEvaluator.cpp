/*
 * UtilityEvaluator.cpp, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 */
#include "../StdInc.h"
#include "UtilityEvaluator.h"
#include "ShadowDAG.h"
#include "DamageBridge.h"
#include "../memory/LearningStore.h"

#include "bonuses/CBonusSystemNode.h"
#include "bonuses/IBonusBearer.h"
#include "mapObjects/CGObjectInstance.h"
#include "mapObjects/CGHeroInstance.h"
#include "mapObjects/army/CArmedInstance.h"
#include "mapObjects/MiscObjects.h"
#include "mapObjects/CGResource.h"
#include "mapObjects/army/CCreatureSet.h"
#include "json/JsonNode.h"

namespace omniai
{

UtilityEvaluator::UtilityEvaluator(HandleRegistry & registry,
	const DifficultyProfile & profile, const Environment * env)
	: registry_(registry)
	, profile_(profile)
	, shadowOwner_(std::make_unique<ShadowEvaluator>())
	, bridgeOwner_(env ? std::make_unique<DamageBridge>(env) : nullptr)
{
	shadow_ = shadowOwner_.get();
	bridge_ = bridgeOwner_.get();
}

UtilityEvaluator::~UtilityEvaluator() = default;

uint64_t UtilityEvaluator::cacheKey(uint64_t objectHandle, uint64_t armyHash)
{
	return objectHandle ^ (armyHash * 0x9E3779B97F4A7C15ull);
}

double UtilityEvaluator::intrinsicWorth(const CGObjectInstance & obj) const
{
	// Static worth: resource piles and artifacts carry a known baseline;
	// guarded objects inherit the guard's strength as a negative offset which
	// the caller folds into riskCost. Kept deliberately small; the DAG delta
	// is where omni-compat content earns its value.
	switch(obj.ID)
	{
		case Obj::RESOURCE:            return 60.0;
		case Obj::ARTIFACT:            return 120.0;
		case Obj::MINE:                return 300.0;
		case Obj::PRISON:              return 800.0;
		case Obj::TOWN:                return 1000.0;
		case Obj::RANDOM_DWELLING:
		case Obj::RANDOM_DWELLING_LVL:
		case Obj::RANDOM_DWELLING_FACTION:
		case Obj::CREATURE_GENERATOR1:
		case Obj::CREATURE_GENERATOR2:
		case Obj::CREATURE_GENERATOR3:
		case Obj::CREATURE_GENERATOR4: return 250.0;
		default:                       return 25.0;
	}
}

double UtilityEvaluator::scarcityOf(int32_t resource) const
{
	if(resource < 0 || resource >= int32_t(scarcity_.size()))
		return 1.0;
	return scarcity_[resource];
}

UtilityBreakdown UtilityEvaluator::evaluate(const CGObjectInstance & obj,
	const CGHeroInstance & hero, double travelCost, bool withShadow) const
{
	UtilityBreakdown out;
	out.intrinsicValue = intrinsicWorth(obj);

	// What the object produces matters as much as that it produces. A flat
	// worth per object type meant a sawmill and a gold mine scored the same
	// while the build queue starved for want of wood.
	if(const auto * mine = dynamic_cast<const CGMine *>(&obj))
		out.intrinsicValue *= scarcityOf(mine->producedResource.getNum());
	else if(const auto * pile = dynamic_cast<const CGResource *>(&obj))
		out.intrinsicValue *= scarcityOf(pile->resourceID().getNum());
	// Owned objects (our towns/heroes) are revisit targets at best; an
	// unweighted intrinsic makes a own TOWN (1000) outrank every real
	// objective on the map. Downweight, not drop, so defense can still win.
	if(obj.tempOwner == hero.tempOwner)
		out.intrinsicValue *= 0.15;

	// Learned layer (no-ops when the store is disabled): per-type worth
	// drift from recorded outcomes, and one-shot objects a previous play
	// already exhausted.
	const auto & learn = LearningStore::instance();
	out.intrinsicValue *= 1.0 + learn.worthAdjust(obj.ID.getNum());
	if(learn.isDepleted(obj.id.getNum()))
		out.intrinsicValue *= 0.1;
	out.riskCost = travelCost;
	// An object on another map level is only reachable through a gate, which
	// is itself a same-level objective. The straight-line travel cost ignores
	// the level gap entirely, so a town sitting directly "below" the hero out
	// of reach outscored every real target and stayed the top pick all game.
	// Push cross-level destinations below anything the hero can walk to now.
	if(obj.visitablePos().z != hero.pos.z)
		out.riskCost += 1e5;

	// Army composition hash: creature type ids folded with counts. Two armies
	// with identical composition reuse each other's memo entries.
	uint64_t armyHash = 1469598103934665603ull;
	for(const auto & [slot, stack] : hero.Slots())
	{
		if(!stack)
			continue;
		armyHash ^= uint64_t(stack->getId().getNum());
		armyHash *= 1099511628211ull;
		armyHash ^= uint64_t(stack->getCount());
	}

	// If the object carries a bonus node (artifact, shrine, modded entity),
	// measure it on the shadow graph. Guarded by profile gate for Easy.
	// Heroes are excluded on purpose, and for two reasons. artifactDelta
	// ignores its hero argument and simply sums the source node's own
	// bonuses, so on a hero it measures that hero's own stats, which is not
	// a thing we would gain by walking over to it. And a hero's bonus tree
	// is the most volatile one on the map: the server relinks it on every
	// level up, artifact pickup and movement change, in the middle of our
	// turn, which is what the crash at day 156 was standing on.
	if(withShadow)
		out.dagDelta = shadowDelta(obj, hero, armyHash);

	// Armed danger: monsters, and any armed object owned by an enemy
	// player (enemy heroes, enemy town garrisons, map garrisons).
	// Strength is CCreatureSet::getArmyStrength (sum of creature AI
	// values), so it scales with modded creatures automatically. Beyond
	// the learned danger threshold the target is effectively
	// unreachable; near it adds a scaled risk.
	const bool isMonster = obj.ID == Obj::MONSTER || obj.ID == Obj::RANDOM_MONSTER;
	const bool isEnemyArmed = obj.tempOwner.isValidPlayer()
		&& obj.tempOwner != hero.tempOwner;
	if(isMonster || isEnemyArmed)
	{
		if(const auto * guard = dynamic_cast<const CArmedInstance *>(&obj))
		{
			const double heroStr = double(hero.getArmyStrength());
			const double guardStr = double(guard->getArmyStrength());
			const double threshold = learn.dangerThreshold();
			if(heroStr <= 0 || guardStr >= heroStr * threshold)
				out.riskCost += 1e7; // suicide - never selected
			else if(guardStr >= heroStr * threshold * 0.55)
				out.riskCost += guardStr; // contested fight - scaled penalty
		}
	}

	out.final = profile_.alphaBase * out.intrinsicValue
		+ profile_.betaDag * out.dagDelta
		- profile_.gammaRisk * out.riskCost;
	return out;
}

double UtilityEvaluator::shadowDelta(const CGObjectInstance & obj,
	const CGHeroInstance & hero, uint64_t armyHash) const
{
	// Heroes are excluded on purpose, and for two reasons. artifactDelta
	// ignores its hero argument and simply sums the source node's own
	// bonuses, so on a hero it measures that hero's own stats, which is not
	// something we would gain by walking over to it. And a hero's bonus tree
	// is the most volatile one on the map: the engine relinks it on every
	// level up, artifact pickup and movement change.
	const IBonusBearer * bearer = dynamic_cast<const IBonusBearer *>(&obj);
	if(!bearer || !profile_.useShadowEval)
		return 0.0;
	if(dynamic_cast<const CGHeroInstance *>(&obj))
		return 0.0;

	const uint64_t key = cacheKey(registry_.acquire(obj.id.getNum()).pack(), armyHash);
	typename tbb::concurrent_hash_map<uint64_t, double>::const_accessor acc;
	if(memo_.find(acc, key))
		return acc->second;

	// Non-const view needed for attachToSource; the shadow graph never
	// writes back into the engine node.
	auto * node = const_cast<CBonusSystemNode *>(
		dynamic_cast<const CBonusSystemNode *>(&obj));
	double delta = 0.0;
	if(node && shadow_)
		delta = shadow_->artifactDelta(*node, hero);
	memo_.emplace(key, delta);
	return delta;
}

double UtilityEvaluator::shadowTerm(const CGObjectInstance & obj,
	const CGHeroInstance & hero) const
{
	uint64_t armyHash = 1469598103934665603ull;
	for(const auto & [slot, stack] : hero.Slots())
	{
		if(!stack)
			continue;
		armyHash ^= uint64_t(stack->getId().getNum());
		armyHash *= 1099511628211ull;
		armyHash ^= uint64_t(stack->getCount());
	}
	return profile_.betaDag * shadowDelta(obj, hero, armyHash);
}

double UtilityEvaluator::artifactDelta(const CArtifactInstance & artifact,
	const CGHeroInstance & hero) const
{
	auto * node = const_cast<CBonusSystemNode *>(
		dynamic_cast<const CBonusSystemNode *>(&artifact));
	if(!node || !shadow_)
		return 0.0;
	return shadow_->artifactDelta(*node, hero);
}

double UtilityEvaluator::bonusNodeDelta(CBonusSystemNode & node,
	const CGHeroInstance & hero) const
{
	return shadow_ ? shadow_->artifactDelta(node, hero) : 0.0;
}

void UtilityEvaluator::clearCache()
{
	memo_.clear();
}

}

/*
 * ShadowDAG.cpp, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 */
#include "../StdInc.h"
#include "ShadowDAG.h"

#include "bonuses/CBonusSystemNode.h"
#include "bonuses/IBonusBearer.h"
#include "bonuses/BonusCache.h"
#include "bonuses/BonusSelector.h"
#include "bonuses/BonusEnum.h"
#include "mapObjects/CGHeroInstance.h"

namespace omniai
{

ShadowDAG::ShadowDAG()
	: root_(std::make_shared<CBonusSystemNode>(BonusNodeType::HERO, true))
{
}

ShadowDAG::~ShadowDAG()
{
	detachAll();
}

void ShadowDAG::attachSource(CBonusSystemNode & sourceNode)
{
	// X.attachToSource(Y) makes X inherit Y's bonuses: the shadow root must
	// attach to the source, not the reverse. Verified against
	// CBonusSystemNode::attachToSource: it writes only parentsToInherit on
	// the child (this root) and bumps the atomic globalCounter. The parent
	// (live artifact node) is touched only via newRedDescendant under
	// !isHypothetic() - our root IS hypothetic, so the live graph is never
	// mutated and the same source may feed multiple roots concurrently.
	// Remaining contract: the live node must outlive the eval (guaranteed
	// while we evaluate during our own synchronous turn).
	root_->attachToSource(sourceNode);
	attached_.push_back(&sourceNode);
}

void ShadowDAG::detachSource(CBonusSystemNode & sourceNode)
{
	root_->detachFromSource(sourceNode);
	std::erase(attached_, &sourceNode);
}

void ShadowDAG::detachAll()
{
	for(auto * node : attached_)
		root_->detachFromSource(*node);
	attached_.clear();
}

int64_t ShadowDAG::valOf(const CSelector & selector, int64_t base) const
{
	return root_->valOfBonuses(selector, {}, static_cast<int>(base));
}

double ShadowDAG::combatWorth() const
{
	// UnitBonusValuesProxy and BonusCacheBase are lib-internal (no DLL_LINKAGE),
	// so the import lib cannot resolve them. Rebuild the same selector table
	// over IBonusBearer::valOfBonuses, which is exported.
	static const CSelector selMelee = Selector::effectRange()(BonusLimitEffect::NO_LIMIT)
		.Or(Selector::effectRange()(BonusLimitEffect::ONLY_MELEE_FIGHT));
	static const CSelector selRanged = Selector::effectRange()(BonusLimitEffect::NO_LIMIT)
		.Or(Selector::effectRange()(BonusLimitEffect::ONLY_DISTANCE_FIGHT));
	static const CSelector selMinDamage =
		Selector::typeSubtype(BonusType::CREATURE_DAMAGE, BonusCustomSubtype::creatureDamageBoth)
		.Or(Selector::typeSubtype(BonusType::CREATURE_DAMAGE, BonusCustomSubtype::creatureDamageMin));
	static const CSelector selMaxDamage =
		Selector::typeSubtype(BonusType::CREATURE_DAMAGE, BonusCustomSubtype::creatureDamageBoth)
		.Or(Selector::typeSubtype(BonusType::CREATURE_DAMAGE, BonusCustomSubtype::creatureDamageMax));
	static const CSelector selAttack =
		Selector::typeSubtype(BonusType::PRIMARY_SKILL, BonusSubtypeID(PrimarySkill::ATTACK));
	static const CSelector selDefence =
		Selector::typeSubtype(BonusType::PRIMARY_SKILL, BonusSubtypeID(PrimarySkill::DEFENSE));
	static const CSelector selExtraAttack = Selector::type()(BonusType::ADDITIONAL_ATTACK);
	static const CSelector selHealth = Selector::type()(BonusType::STACK_HEALTH);

	auto val = [this](const CSelector & s) {
		return root_->valOfBonuses(s, {}, 0);
	};

	const double melee = val(selMinDamage.And(selMelee)) + val(selMaxDamage.And(selMelee));
	const double ranged = val(selMinDamage.And(selRanged)) + val(selMaxDamage.And(selRanged));
	const double attack = val(selAttack.And(selMelee)) + val(selAttack.And(selRanged));
	const double defence = val(selDefence.And(selMelee)) + val(selDefence.And(selRanged));
	const double health = val(selHealth);
	const double attacks = val(selExtraAttack.And(selMelee)) + val(selExtraAttack.And(selRanged));

	// Weighted aggregate: damage output dominates, defence and health scale
	// survivability, extra attacks multiply effective damage.
	return 0.5 * (melee + ranged) + 2.0 * attack + 1.5 * defence
		+ 0.1 * health + 5.0 * attacks;
}

double ShadowEvaluator::artifactDelta(CBonusSystemNode & artifactNode, const CGHeroInstance & hero)
{
	std::lock_guard walk(graphWalk_);
	ShadowDAG dag;
	const double baseline = dag.combatWorth();
	dag.attachSource(artifactNode);
	const double augmented = dag.combatWorth();
	dag.detachSource(artifactNode);
	return augmented - baseline;
}

double ShadowEvaluator::statDelta(CBonusSystemNode & artifactNode, const CSelector & selector, int64_t base)
{
	std::lock_guard walk(graphWalk_);
	ShadowDAG dag;
	const int64_t baseline = dag.valOf(selector, base);
	dag.attachSource(artifactNode);
	const int64_t augmented = dag.valOf(selector, base);
	dag.detachSource(artifactNode);
	return static_cast<double>(augmented - baseline);
}

}

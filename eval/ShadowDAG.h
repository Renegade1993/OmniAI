/*
 * ShadowDAG.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * Shadow bonus-graph evaluation. Instead of mutating the live hero node, the
 * evaluator hangs the candidate modded object (artifact, spell, skill) on a
 * hypothetical CBonusSystemNode and measures the aggregate stat delta through
 * the DAG's own limiter/propagator machinery. Unknown modded bonuses get
 * real values; hardcoded heuristics are never consulted.
 */
#pragma once

#include "../StdInc.h"

VCMI_LIB_NAMESPACE_BEGIN
class CBonusSystemNode;
class IBonusBearer;
class CSelector;
class CGHeroInstance;
class CArtifactInstance;
VCMI_LIB_NAMESPACE_END

namespace omniai
{

/// Owns one isolated hypothetical bonus node plus the sources attached to it.
/// Not thread-safe by itself; each TBB worker gets its own instance.
class ShadowDAG
{
public:
	ShadowDAG();
	~ShadowDAG();

	ShadowDAG(const ShadowDAG &) = delete;
	ShadowDAG & operator=(const ShadowDAG &) = delete;

	/// Attach a live engine node (e.g. an artifact's bonus node) as a bonus
	/// source feeding the hypothetical root. The live graph is not modified:
	/// attachToSource records the source relationship on OUR node only.
	void attachSource(CBonusSystemNode & sourceNode);
	void detachSource(CBonusSystemNode & sourceNode);
	void detachAll();

	/// Read the hypothetical node's aggregated value for a selector, e.g.
	/// valOfBonuses(BonusType::PRIMARY_SKILL, PrimarySkill::ATTACK).
	int64_t valOf(const CSelector & selector, int64_t base = 0) const;

	/// Aggregate combat-worth proxy across the standard combat stats the AI
	/// scores on: attack, defense, damage, health, speed, morale, luck.
	double combatWorth() const;

	CBonusSystemNode * root() { return root_.get(); }

private:
	std::shared_ptr<CBonusSystemNode> root_;
	std::vector<CBonusSystemNode *> attached_;
};

/// Measures the combat-worth delta a candidate object produces when its bonus
/// node feeds the hero's army. Positive = army gets stronger.
class ShadowEvaluator
{
public:
	/// artifactNode: the modded object's bonus-system node (from
	/// IBonusBearer / CArtifactInstance). hero: live hero whose army receives
	/// the hypothetical bonuses. Returns augmented minus baseline worth.
	double artifactDelta(CBonusSystemNode & artifactNode, const CGHeroInstance & hero);

	/// Same measurement restricted to a stat selector (cheaper).
	double statDelta(CBonusSystemNode & artifactNode, const CSelector & selector, int64_t base = 0);

private:
	/// The hypothetical root is per-call and private to the caller, but what
	/// it reads through is not: valOfBonuses walks from the root into the
	/// live source node and on up ITS ancestors, and CBonusSystemNode caches
	/// the aggregate inside each node, rebuilding it whenever the global
	/// tree counter moves. Two workers arriving at a shared ancestor with a
	/// stale cache both rebuild it in place, which is a torn BonusList and a
	/// null read. Measured: soak 2 died at day 156 with TBB workers 0 and 1
	/// faulting on 0x0 in the same microsecond, directly after the server
	/// applied SetSecSkill for a hero that had just won a fight. One walker
	/// at a time. The memo in UtilityEvaluator keeps the cost off the hot
	/// path.
	std::mutex graphWalk_;
};

}

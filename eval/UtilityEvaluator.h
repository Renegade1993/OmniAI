/*
 * UtilityEvaluator.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * Scores candidate map actions: U = alpha*V + beta*DeltaDag - gamma*R.
 * Expensive shadow evaluations are memoized in a concurrent hash map keyed on
 * the object's handle + the hero's composition hash, so repeated IDDFS passes
 * reuse Lua/DAG results instead of re-entering the script VM.
 */
#pragma once

#include "../StdInc.h"

#include <array>
#include "../memory/GenerationalHandle.h"
#include "DifficultyMatrix.h"

VCMI_LIB_NAMESPACE_BEGIN
class CGHeroInstance;
class CGObjectInstance;
class CArtifactInstance;
class CBonusSystemNode;
class Environment;
VCMI_LIB_NAMESPACE_END

namespace omniai
{

class ShadowEvaluator;
class DamageBridge;
class HandleRegistry;

struct UtilityBreakdown
{
	double intrinsicValue = 0.0;   // V: known static worth (resources, XP...)
	double dagDelta = 0.0;         // DeltaDag: shadow-graph combat delta
	double riskCost = 0.0;         // R: distance + threat + movement spent
	double final = 0.0;            // weighted sum
};

class UtilityEvaluator
{
public:
	UtilityEvaluator(HandleRegistry & registry, const DifficultyProfile & profile,
		const Environment * env = nullptr);
	~UtilityEvaluator(); // out-of-line: unique_ptr over incomplete types

	/// Score a map object as seen by hero.
	///
	/// withShadow=false leaves out the shadow-DAG term and, with it, every
	/// read of the live bonus graph. That is the form the parallel pass
	/// uses: walking engine bonus nodes from several threads while the
	/// engine relinks them on its own is what crashed two soaks. The DAG
	/// term is added afterwards by shadowTerm(), on one thread, for the
	/// handful of candidates that are actually in contention.
	UtilityBreakdown evaluate(const CGObjectInstance & obj,
		const CGHeroInstance & hero,
		double travelCost,
		bool withShadow = true) const;

	/// The weighted shadow-DAG contribution on its own, already multiplied
	/// by the difficulty profile's beta. Call from ONE thread only.
	double shadowTerm(const CGObjectInstance & obj,
		const CGHeroInstance & hero) const;

	/// Combat-worth delta of a specific artifact on a specific hero.
	double artifactDelta(const CArtifactInstance & artifact,
		const CGHeroInstance & hero) const;

	/// Direct access for callers that already hold the bonus node.
	double bonusNodeDelta(CBonusSystemNode & node,
		const CGHeroInstance & hero) const;

	void clearCache();

	/// Per-resource multiplier applied to mines and resource piles, one entry
	/// per GameResID. Set once a turn from the turn thread, before scoring.
	/// A sawmill when we have no wood should not score the same as a gold
	/// mine when we are sitting on sixteen thousand gold.
	void setScarcity(const std::array<double, 7> & mult) { scarcity_ = mult; }

private:
	double intrinsicWorth(const CGObjectInstance & obj) const;
	double scarcityOf(int32_t resource) const;
	double shadowDelta(const CGObjectInstance & obj, const CGHeroInstance & hero,
		uint64_t armyHash) const;
	static uint64_t cacheKey(uint64_t objectHandle, uint64_t armyHash);

	HandleRegistry & registry_;
	const DifficultyProfile & profile_;
	mutable ShadowEvaluator * shadow_ = nullptr; // owned below
	std::unique_ptr<ShadowEvaluator> shadowOwner_;
	mutable DamageBridge * bridge_ = nullptr;
	std::unique_ptr<DamageBridge> bridgeOwner_;

	std::array<double, 7> scarcity_ = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

	// objectHandle ^ armyHash -> cached delta
	mutable tbb::concurrent_hash_map<uint64_t, double> memo_;
};

}

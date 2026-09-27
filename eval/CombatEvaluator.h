/*
 * CombatEvaluator.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * An expected-outcome estimate for an adventure-map fight, built to replace
 * the bare getArmyStrength() ratios that gate attack, hold, sally, retreat
 * and siege decisions. The scalar answer a strength ratio gives is "who is
 * bigger"; what a decision needs is "who wins, and by how much". This models
 * the fight instead of comparing sizes:
 *
 *   - each stack contributes its own count, damage, hit points and attack
 *     versus the defender's average defence, so creature tiers and count
 *     both matter (a few high-tier stacks are not the same as many weak ones
 *     even at equal strength value);
 *   - hero attack and defence shift every friendly stack's stats, and spell
 *     power plus mana add a per-round magic output;
 *   - the side with the faster stacks strikes first and shooters fire without
 *     taking a retaliation;
 *   - the exchange is iterated round by round with attrition (a side taking
 *     casualties deals less damage back), so the answer is an outcome, not a
 *     ratio.
 *
 * The verdict carries the winner, the fraction of the winner's army expected
 * to survive, and a damage-weighted power ratio for callers that still want
 * a continuous number rather than a yes or no.
 */
#pragma once

#include "../StdInc.h"

VCMI_LIB_NAMESPACE_BEGIN
class CArmedInstance;
class CGHeroInstance;
VCMI_LIB_NAMESPACE_END

namespace omniai
{

struct CombatVerdict
{
	/// True when the attacking side is expected to win.
	bool win = false;
	/// Fraction of the WINNER's army expected to survive, 0 to 1. Near 1 is a
	/// rout in the winner's favour; near 0 is a pyrrhic result either way.
	double margin = 0.0;
	/// Damage-weighted power of the attacker over the defender. Above 1
	/// favours the attacker; use it where a continuous number reads better
	/// than the boolean.
	double ratio = 0.0;
};

class CombatEvaluator
{
public:
	/// A simplified battle profile for one side of a fight.
	struct Side
	{
		struct Stack
		{
			double count = 0;
			double hp = 0;       // hit points per creature
			double atk = 0;      // attack skill, hero included
			double def = 0;      // defence skill, hero included
			double dmg = 0;      // average damage per creature per attack
			double speed = 0;    // initiative, decides first strike
			bool shooter = false;
			bool flyer = false;  // crosses town walls
		};
		std::vector<Stack> stacks;
		double magicDps = 0;     // hero spell output per round
		double towerDps = 0;     // arrow tower/keep output per round, siege only
		double topSpeed = 0;
		/// Defender side only: the town's fort level (1 fort, 2 citadel, 3
		/// castle). The attacker's ground stacks are discounted against it
		/// the way VCMI's own CCreatureSet::getArmyStrength(fortLevel) does
		/// for its AIs: shooters by 1/fort, melee by 1/fort^2, flyers not.
		int fortLevel = 0;
	};

	/// Build the profile for an armed object (hero, map guard, town garrison).
	/// hero may be null for a neutral stack or a garrison with no defender
	/// hero. fortLevel applies the siege defence discount to a town defender.
	static Side profile(const CArmedInstance * army,
		const CGHeroInstance * hero, int fortLevel = 0);

	/// Merge additional armed objects into one defender profile - a town's
	/// garrison plus its visiting heroes fight as one side in a siege.
	static void absorb(Side & into, const CArmedInstance * army,
		const CGHeroInstance * hero, int fortLevel = 0);

	/// Estimate the outcome of atk marching on def.
	static CombatVerdict estimate(const Side & atk, const Side & def);
};

}

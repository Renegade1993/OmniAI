/*
 * CombatEvaluator.cpp, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 */
#include "../StdInc.h"
#include "CombatEvaluator.h"

#include "mapObjects/army/CStackInstance.h"
#include "mapObjects/CGHeroInstance.h"
#include "mapObjects/CGObjectInstance.h"
#include "CCreatureHandler.h"

using namespace omniai;

namespace
{
	// The standard Heroes III attack-versus-defence damage factor. Attack above
	// the defender's defence multiplies damage by 5% a point (capped near
	// 400%), defence above the attacker's reduces it by 2.5% a point (floored
	// near 30%).
	double damageFactor(double atk, double def)
	{
		if(atk >= def)
			return 1.0 + 0.05 * std::min(atk - def, 60.0);
		return std::max(0.30, 1.0 - 0.025 * std::min(def - atk, 28.0));
	}
}

void CombatEvaluator::absorb(Side & into, const CArmedInstance * army,
	const CGHeroInstance * hero, int fortLevel)
{
	if(!army)
		return;
	// Primary skills bend every stack's attack and defense, and they include
	// artifact bonuses, so an equipped hero is already counted. Secondary
	// skills are the part that dominates a big fight: expert Offense sharpens
	// every melee swing 30%, expert Archery every shot 50%, expert Armorer
	// makes the whole line 15% harder to kill. At 200k of army these are the
	// difference between the raw stack total and the real outcome.
	auto lvl = [hero](const SecondarySkill & sk)
	{ return hero ? hero->getSecSkillLevel(sk) : 0; };
	static const double MELEE[] = {1.0, 1.10, 1.20, 1.30};
	static const double SHOOT[] = {1.0, 1.10, 1.25, 1.50};
	static const double ARMOR[] = {1.0, 1.05, 1.10, 1.18};
	static const double SORC[]  = {1.0, 1.05, 1.10, 1.15};
	const double meleeMul = MELEE[std::min(3, int(lvl(SecondarySkill::OFFENCE)))];
	const double shootMul = SHOOT[std::min(3, int(lvl(SecondarySkill::ARCHERY)))];
	const double armorMul = ARMOR[std::min(3, int(lvl(SecondarySkill::ARMORER)))];
	const double sorcMul  = SORC [std::min(3, int(lvl(SecondarySkill::SORCERY)))];

	const double heroAtk = hero ? double(hero->getPrimSkillLevel(PrimarySkill::ATTACK)) : 0.0;
	const double heroDef = hero ? double(hero->getPrimSkillLevel(PrimarySkill::DEFENSE)) : 0.0;
	for(const auto & entry : army->Slots())
	{
		const auto * stack = entry.second.get();
		if(!stack || !stack->getType())
			continue;
		const auto * cre = stack->getCreature();
		if(!cre)
			continue;
		Side::Stack s;
		s.count = double(stack->getCount());
		// Armorer is folded into durability: a harder-to-kill stack is more
		// effective hit points.
		s.hp = double(std::max(1, cre->getBaseHitPoints())) * armorMul;
		s.atk = double(cre->getBaseAttack()) + heroAtk;
		s.def = double(cre->getBaseDefense()) + heroDef;
		s.dmg = 0.5 * double(cre->getBaseDamageMin() + cre->getBaseDamageMax());
		s.speed = double(cre->getBaseSpeed());
		s.shooter = cre->getBaseShots() > 0;
		s.flyer = cre->hasBonusOfType(BonusType::FLYING);
		s.dmg *= s.shooter ? shootMul : meleeMul;
		// A besieged defender behind walls is harder to hit and hits softer at
		// range; getArmyStrength uses the same fort discount, so mirror it.
		if(fortLevel > 0 && s.def > 0)
			s.def += double(fortLevel);
		into.stacks.push_back(s);
		into.topSpeed = std::max(into.topSpeed, s.speed);
	}
	// Hero magic is a damage source of its own. A day of Implosion or
	// Armageddon on a 100k fight is real output, not a garnish: spell power
	// scales the per-cast hit, Sorcery multiplies it, and the mana pool bounds
	// how long it can keep casting. Weighted up from the old flat trickle so
	// a genuine caster changes the verdict at scale.
	if(hero && hero->hasSpellbook())
	{
		const double power = double(hero->getPrimSkillLevel(PrimarySkill::SPELL_POWER));
		const double manaFrac = hero->manaLimit() > 0
			? std::min(1.0, double(hero->mana) / double(hero->manaLimit())) : 0.0;
		into.magicDps += power * 60.0 * manaFrac * sorcMul;
	}
}

CombatEvaluator::Side CombatEvaluator::profile(const CArmedInstance * army,
	const CGHeroInstance * hero, int fortLevel)
{
	Side s;
	absorb(s, army, hero, fortLevel);
	return s;
}

CombatVerdict CombatEvaluator::estimate(const Side & atk, const Side & def)
{
	CombatVerdict v;
	if(atk.stacks.empty())
		return v;             // nothing to fight with
	if(def.stacks.empty())
	{
		v.win = true;          // unguarded
		v.margin = 1.0;
		v.ratio = 1e9;
		return v;
	}

	// Weighted average defence each side presents to the other's attacks.
	auto avgDef = [](const Side & s)
	{
		double num = 0, den = 0;
		for(const auto & st : s.stacks){ num += st.def * st.count; den += st.count; }
		return den > 0 ? num / den : 0.0;
	};
	const double defAvg = avgDef(def);
	const double atkAvg = avgDef(atk);

	// Full-strength durability and damage output for each side. wallFort is
	// the fort level this side attacks INTO (0 in the open): VCMI's own AI
	// valuation of an army against walls (CCreatureSet::getArmyStrength)
	// divides a ground shooter's power by the fort level and a ground melee
	// stack's by its square, flyers untouched. The old +fort defence bump
	// alone read a Castle siege as a rout: R3_duel r05 "attacker keeps 84%"
	// where the garrison by itself killed a tenth of the attacker.
	auto full = [](const Side & s, double oppAvgDef, int wallFort)
	{
		double hp = 0, dps = s.magicDps + s.towerDps;
		for(const auto & st : s.stacks)
		{
			hp += st.count * st.hp;
			double d = st.count * st.dmg * damageFactor(st.atk, oppAvgDef);
			if(wallFort > 0 && !st.flyer)
				d /= st.shooter ? double(wallFort) : double(wallFort * wallFort);
			dps += d;
		}
		return std::pair<double, double>{hp, dps};
	};
	const auto a = full(atk, defAvg, def.fortLevel);
	const auto b = full(def, atkAvg, 0);
	const double hpA = a.first, dpsA = a.second;
	const double hpB = b.first, dpsB = b.second;

	v.ratio = dpsB > 0 ? dpsA / dpsB : (dpsA > 0 ? 1e9 : 1.0);
	if(hpA <= 0 || hpB <= 0)
	{
		v.win = hpA > 0;
		v.margin = v.win ? 1.0 : 0.0;
		return v;
	}

	// Round-by-round exchange. The faster side strikes first and a side that
	// is losing creatures deals proportionally less damage back, which is what
	// separates this from a plain strength ratio.
	const bool aFaster = atk.topSpeed >= def.topSpeed;
	double hA = hpA, hB = hpB;
	// The faster side gets one unanswered hit before the exchange starts.
	if(aFaster) hB -= dpsA; else hA -= dpsB;
	for(int round = 0; hA > 0 && hB > 0 && round < 60; ++round)
	{
		const double curA = dpsA * std::max(0.0, hA) / hpA;
		const double curB = dpsB * std::max(0.0, hB) / hpB;
		if(aFaster)
		{
			hB -= curA;
			if(hB > 0) hA -= curB;
		}
		else
		{
			hA -= curB;
			if(hA > 0) hB -= curA;
		}
	}

	if(hA > 0 && hB <= 0)      { v.win = true;  v.margin = std::min(1.0, hA / hpA); }
	else if(hB > 0 && hA <= 0) { v.win = false; v.margin = std::min(1.0, hB / hpB); }
	else                       { v.win = hA >= hB; v.margin = std::min(1.0, std::max(0.0, std::max(hA, hB)) / (hA >= hB ? hpA : hpB)); }
	return v;
}

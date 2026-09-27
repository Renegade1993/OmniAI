/*
 * DifficultyMatrix.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * Difficulty-scaled decision weighting from the Round 2 architecture doc:
 *     U(a) = alpha * V(a)  +  beta * DeltaDag(a)  -  gamma * R(a)
 * where V = static resource value, DeltaDag = combat delta measured on the
 * shadow bonus graph, R = risk/pathing cost. alpha, beta, gamma scale with
 * difficulty, as do IDDFS depth and TBB concurrency.
 */
#pragma once

#include <cstdint>

namespace omniai
{

struct DifficultyProfile
{
	const char * name;
	uint32_t iddfsDepthTurns;   // how many turns ahead the search plans
	double alphaBase;           // weight of intrinsic object value
	double betaDag;             // weight of shadow-DAG synergy delta
	double gammaRisk;           // risk aversion factor
	uint32_t maxThreads;        // TBB arena concurrency cap (0 = uncapped)
	bool useShadowEval;         // false on Easy: skip expensive DAG/Lua work
};

inline constexpr DifficultyProfile DIFFICULTY_TIERS[] = {
	// Mirrored on EMapDifficulty 0..4 (EASY..IMPOSSIBLE, MapDifficulty.h).
	{ "Easy",       1, 1.0, 0.0, 2.5, 1, false },
	{ "Normal",     3, 1.0, 0.5, 1.5, 2, true  },
	{ "Hard",       5, 0.8, 1.2, 1.0, 4, true  },
	{ "Expert",     7, 0.5, 2.0, 0.8, 0, true  }, // 0 threads = uncapped
	{ "Impossible", 9, 0.4, 2.5, 0.6, 0, true  },
};

inline constexpr uint32_t DIFFICULTY_TIER_COUNT = 5;

/// Map a VCMI difficulty index (EMapDifficulty 0..4) onto a profile.
/// Out-of-range values clamp.
inline const DifficultyProfile & profileFor(int difficulty)
{
	if(difficulty < 0)
		difficulty = 0;
	if(difficulty > 4)
		difficulty = 4;
	return DIFFICULTY_TIERS[difficulty];
}

}

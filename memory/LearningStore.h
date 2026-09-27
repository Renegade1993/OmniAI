/*
 * LearningStore.h, part of the OmniAI VCMI plugin
 *
 * Optional procedural-learning memory. When enabled (settings key
 * ai.omniaiLearningMode "learn", or with that key absent any of the old
 * gates: ai.omniaiLearning true, OMNIAI_LEARNING set, or the
 * omniai.learning submod active), the store persists a small
 * JSON document at <userData>/OmniAI/memory.json (or in the folder
 * setDir() names) so the AI improves across repeated plays of the same
 * map and across maps in general:
 *
 *   - per-object-type worth adjustments (what paid off, what did not)
 *   - a self-tuned danger threshold (win/loss vs predicted strength
 *     ratio, replaces the hardcoded 1.1x guard comparison)
 *   - per-map depleted markers for one-shot structures that stay on
 *     the map after their reward is gone (windmills, gardens, shrines)
 *
 * Everything is learned through live engine state only - no mod
 * content is read directly, so the memory stays mod-agnostic.
 *
 * Controls for the in-game config panel (all under settings "ai"):
 *   omniaiLearningMode  "learn" | "pause" | "off". Pause reads the memory
 *                       and never writes it; off ignores it. Absent: the
 *                       old gates decide between learn and off.
 *   omniaiResetBrain    an integer stamp (e.g. the time the Reset Brain
 *                       button was pressed). A stamp higher than the one
 *                       stored in memory.json wipes the memory at the next
 *                       game start, keeping memory.json.bak.
 */
#pragma once

#include "../StdInc.h"

#include "json/JsonNode.h"

VCMI_LIB_NAMESPACE_BEGIN
class IGameInfoCallback;
VCMI_LIB_NAMESPACE_END

namespace omniai
{

class LearningStore
{
public:
	static LearningStore & instance();

	/// Read the enable flag, resolve the map key from the callback's
	/// header, and load memory.json from the store's folder if present.
	void init(const IGameInfoCallback * cb);
	/// The folder memory.json lives in, set before init(). OmniAI.cpp passes
	/// its omniDir(), which follows OMNIAI_DIR, so the memory goes wherever
	/// the decision logs go. Empty means <userData>/OmniAI, as before.
	void setDir(std::string dir) { dir_ = std::move(dir); }
	/// The memory file init() resolved; empty while learning is off.
	const std::string & storePath() const { return path_; }

	bool enabled() const { return enabled_; }
	/// Pause mode: the memory is read, never written.
	bool readOnly() const { return readOnly_; }

	/// Multiplicative worth adjustment in [-0.5, +0.5] for an Obj:: id.
	double worthAdjust(int32_t objType) const;

	/// Self-tuned ratio of heroStrength the AI demands before engaging a
	/// guard. Starts at 1.1, drifts with observed battle outcomes.
	double dangerThreshold() const;

	/// True when a previous game marked this instance as exhausted.
	bool isDepleted(int32_t instanceId) const;

	/// Outcome feedback for an object visit.
	void recordVisit(int32_t objType, bool positive);

	/// Mark an instance as exhausted for this and future plays of the map.
	void markDepleted(int32_t instanceId);

	/// Battle outcome vs predicted strength ratio (hero/guard).
	void recordBattle(double predictedRatio, bool won);

	/// Write dirty state to disk.
	void flush();

private:
	LearningStore() = default;

	JsonNode & section(const char * which); // "global" or the map section
	const JsonNode & section(const char * which) const;

	bool enabled_ = false;
	bool readOnly_ = false;
	bool dirty_ = false;
	std::string mapKey_;
	std::string path_;
	std::string dir_;
	JsonNode store_;
};

}

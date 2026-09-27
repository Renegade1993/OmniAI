/*
 * LearningStore.cpp, part of the OmniAI VCMI plugin
 */
#include "../StdInc.h"
#include "LearningStore.h"

#include "CConfigHandler.h"
#include "GameLibrary.h"
#include "callback/IGameInfoCallback.h"
#include "json/JsonNode.h"
#include "mapping/CMapHeader.h"
#include "modding/CModHandler.h"

#include <filesystem>
#include <fstream>

#include <shlobj.h>
#include <knownfolders.h>

namespace omniai
{

namespace
{
	constexpr int STORE_VERSION = 1;

	/// Mod ids that switch learning on when active in the launcher.
	///
	/// "omniai.learning" is the current one: a submod of the OmniAI mod, so it
	/// appears nested under OmniAI in the mod manager the way hota.cove sits
	/// under hota. VCMI composes a submod's id as parent + '.' + folder name
	/// (ModManager.cpp), which is where that spelling comes from.
	///
	/// "omniai-learning" was the original flat stub mod and is still accepted,
	/// because an install that predates the restructure would otherwise turn
	/// learning off silently on upgrade. Losing a trained memory.json to a
	/// rename is not a failure anyone would think to look for.
	const char * const LEARNING_MOD_IDS[] = { "omniai.learning", "omniai-learning" };

	bool learningModActive()
	{
		// LIBRARY is null in the standalone test executable, which has no mod
		// handler at all. Dereferencing it there crashed the test run once.
		if(!LIBRARY || !LIBRARY->modh)
			return false;

		const auto & active = LIBRARY->modh->getActiveMods();
		for(const char * id : LEARNING_MOD_IDS)
			if(vstd::contains(active, std::string(id)))
				return true;
		return false;
	}

	bool flagEnabled()
	{
		// Any one of three gates turns learning on: the environment variable,
		// the launcher mod toggle, or the settings key. They are checked
		// cheapest first.
		if(std::getenv("OMNIAI_LEARNING"))
			return true;

		if(learningModActive())
			return true;

		const JsonNode & flag = settings["ai"]["omniaiLearning"];
		return flag.getType() == JsonNode::JsonType::DATA_BOOL && flag.Bool();
	}

	/// "<Documents>\My Games\vcmi" - resolved through KnownFolders instead
	/// of VCMIDirs::userDataPath(): vcmi's lib does not export the boost
	/// filesystem internals that path->string conversion pulls in, so
	/// touching the returned bfs::path here cannot link.
	enum class Mode { Off, Learn, Pause };

	/// The config panel's explicit choice wins; without one, the old gates
	/// decide between learning and off. Benchmarks set neither, so they
	/// stay off exactly as before.
	Mode resolveMode()
	{
		const JsonNode & m = settings["ai"]["omniaiLearningMode"];
		if(m.getType() == JsonNode::JsonType::DATA_STRING)
		{
			if(m.String() == "learn")
				return Mode::Learn;
			if(m.String() == "pause")
				return Mode::Pause;
			if(m.String() == "off")
				return Mode::Off;
		}
		return flagEnabled() ? Mode::Learn : Mode::Off;
	}

	std::string userDataDir()
	{
		PWSTR docs = nullptr;
		if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)))
		{
			const std::filesystem::path p = std::filesystem::path(docs) / "My Games" / "vcmi";
			CoTaskMemFree(docs);
			return p.string();
		}
		return {};
	}

	/// The folder memory.json lives in: the one set through setDir (OmniAI's
	/// omniDir(), which follows OMNIAI_DIR), else <userData>\OmniAI.
	std::string storeFolder(const std::string & set)
	{
		if(!set.empty())
			return set;
		const std::string base = userDataDir();
		return base.empty() ? base : (std::filesystem::path(base) / "OmniAI").string();
	}
}

LearningStore & LearningStore::instance()
{
	static LearningStore inst;
	return inst;
}

void LearningStore::init(const IGameInfoCallback * cb)
{
	const Mode mode = resolveMode();
	enabled_ = mode != Mode::Off;
	readOnly_ = mode == Mode::Pause;
	dirty_ = false;
	mapKey_.clear();
	path_.clear();
	store_ = JsonNode();

	// Reset Brain, in every learning mode: a nonzero ai.omniaiResetBrain is
	// a one-time request. The lobby panel's checkbox writes 1 (the generic
	// settings binding, InterfaceObjectConfigurable), so the request is
	// cleared back to 0 here once served; ticking the box again asks again.
	// The old file is kept as memory.json.bak in case it was a misclick.
	// With several OmniAI players in one game the first init serves it and
	// the rest read 0.
	{
		const JsonNode & reset = settings["ai"]["omniaiResetBrain"];
		const bool asked = (reset.isNumber() && reset.Integer() != 0)
			|| (reset.getType() == JsonNode::JsonType::DATA_BOOL && reset.Bool());
		const std::string folder = asked ? storeFolder(dir_) : std::string();
		if(asked && !folder.empty())
		{
			const std::string file = (std::filesystem::path(folder) / "memory.json").string();
			std::error_code rec;
			if(std::filesystem::exists(file, rec))
			{
				std::filesystem::copy_file(file, file + ".bak",
					std::filesystem::copy_options::overwrite_existing, rec);
				std::filesystem::remove(file, rec);
			}
			Settings flag = settings.write["ai"]["omniaiResetBrain"];
			flag->Integer() = 0;
			logAi->info("OmniAI: brain reset on request; previous memory kept as %s.bak", file);
		}
	}

	if(!enabled_)
		return;

	if(cb && cb->getMapHeader())
	{
		const auto & mh = *cb->getMapHeader();
		const auto sz = cb->getMapSize();
		mapKey_ = mh.name.toString() + "|"
			+ std::to_string(sz.x) + "x" + std::to_string(sz.y) + "x" + std::to_string(sz.z);
	}
	if(mapKey_.empty())
		mapKey_ = "unknown";

	const std::string folder = storeFolder(dir_);
	if(folder.empty())
	{
		logAi->warn("OmniAI: cannot resolve user data dir; learning disabled");
		enabled_ = false;
		return;
	}
	path_ = (std::filesystem::path(folder) / "memory.json").string();

	std::error_code ec;
	if(std::filesystem::exists(path_, ec))
	{
		std::ifstream in(path_, std::ios::binary);
		std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if(!text.empty())
		{
			try
			{
				store_ = JsonNode(text.data(), text.size(), "memory.json");
			}
			catch(...)
			{
				logAi->warn("OmniAI: learning store %s unreadable; starting fresh", path_);
				store_ = JsonNode();
			}
		}
	}
	if(store_["version"].Integer() != STORE_VERSION)
	{
		store_["version"] = JsonNode(int32_t(STORE_VERSION));
		dirty_ = true;
	}

	logAi->info("OmniAI: learning %s, store %s, map key '%s'",
		readOnly_ ? "paused (read only)" : "enabled", path_, mapKey_);
}

JsonNode & LearningStore::section(const char * which)
{
	if(std::strcmp(which, "global") == 0)
		return store_["global"];
	return store_["maps"][mapKey_];
}

const JsonNode & LearningStore::section(const char * which) const
{
	static const JsonNode empty;
	if(std::strcmp(which, "global") == 0)
		return store_["global"];
	if(store_["maps"].getType() != JsonNode::JsonType::DATA_STRUCT)
		return empty;
	return store_["maps"][mapKey_];
}

double LearningStore::worthAdjust(int32_t objType) const
{
	if(!enabled_)
		return 0.0;
	const auto key = std::to_string(objType);
	double adj = section("global")["worth"][key].Float()
		+ section("map")["worth"][key].Float();
	return std::clamp(adj, -0.5, 0.5);
}

double LearningStore::dangerThreshold() const
{
	if(!enabled_)
		return 1.1;
	const double d = section("global")["danger"].Float();
	return d > 0.0 ? d : 1.1;
}

bool LearningStore::isDepleted(int32_t instanceId) const
{
	if(!enabled_)
		return false;
	const auto & depleted = section("map")["depleted"];
	if(depleted.getType() != JsonNode::JsonType::DATA_VECTOR)
		return false;
	for(const auto & e : depleted.Vector())
		if(e.Integer() == instanceId)
			return true;
	return false;
}

void LearningStore::recordVisit(int32_t objType, bool positive)
{
	if(!enabled_)
		return;
	const auto key = std::to_string(objType);
	// Map-local evidence dominates the global signal.
	for(const char * which : {"map", "global"})
	{
		auto & w = section(which)["worth"][key];
		const double next = w.Float() + (positive ? 0.02 : -0.05);
		w = JsonNode(std::clamp(next, -0.5, 0.5));
	}
	dirty_ = true;
}

void LearningStore::markDepleted(int32_t instanceId)
{
	if(!enabled_ || isDepleted(instanceId))
		return;
	section("map")["depleted"].Vector().push_back(JsonNode(int32_t(instanceId)));
	dirty_ = true;
}

void LearningStore::recordBattle(double predictedRatio, bool won)
{
	if(!enabled_ || predictedRatio <= 0.0)
		return;
	auto & danger = section("global")["danger"];
	double d = danger.Float() > 0.0 ? danger.Float() : 1.1;
	// We predicted a win (ratio above threshold) and lost -> demand more.
	// We won below the threshold -> engage earlier next time.
	if(!won && predictedRatio >= d)
		d *= 1.08;
	else if(won && predictedRatio < d * 0.9)
		d *= 0.95;
	danger = JsonNode(std::clamp(d, 0.7, 2.0));
	dirty_ = true;
}

void LearningStore::flush()
{
	if(!enabled_ || !dirty_ || readOnly_)
		return;
	std::error_code ec;
	std::filesystem::create_directories(
		std::filesystem::path(path_).parent_path(), ec);
	std::ofstream out(path_, std::ios::binary | std::ios::trunc);
	if(!out)
	{
		logAi->warn("OmniAI: cannot write learning store %s", path_);
		return;
	}
	out << store_.toCompactString();
	out.flush();
	dirty_ = false;
}

}

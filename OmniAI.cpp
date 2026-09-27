/*
 * OmniAI.cpp, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 */
#include "StdInc.h"
#include <chrono>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <regex>
#include <map>
#include <set>
// shlobj.h must precede knownfolders.h: the latter uses GUID and EXTERN_C
// without declaring them, so on its own it produces a hundred redefinition
// errors that say nothing about the real cause.
#include <shlobj.h>
#include <knownfolders.h>
#include "OmniAI.h"

#include "memory/GenerationalHandle.h"
#include "memory/QSBRReclaimer.h"
#include "memory/LearningStore.h"
#include "eval/DifficultyMatrix.h"
#include "eval/UtilityEvaluator.h"
#include "net/ActionDispatcher.h"
#include "net/PendingActionQueue.h"

#include "callback/CCallback.h"
#include "vcmi/Environment.h"
#include "vcmi/Services.h"
#include "CStack.h"
#include "StartInfo.h"
#include "battle/BattleAction.h"
#include "battle/BattleStateInfoForRetreat.h"
#include "battle/CPlayerBattleCallback.h"
#include "CRandomGenerator.h"
#include "constants/StringConstants.h"
#include "CConfigHandler.h"
#include "json/JsonNode.h"
#include "mapObjects/CGHeroInstance.h"
#include "mapObjects/CGObjectInstance.h"
#include "mapObjects/CGTownInstance.h"
#include "spells/CSpellHandler.h"
#include "mapObjects/CGDwelling.h"
#include "mapObjects/CGMarket.h"
#include "mapObjects/MiscObjects.h"
#ifdef OMNIAI_IN_TREE
#include "VCMIDirs.h"
#endif
#include "mapObjects/CQuest.h"
#include "mapObjects/IMarket.h"
#include "entities/building/CBuilding.h"
#include "entities/artifact/ArtifactUtils.h"
#include "gameState/UpgradeInfo.h"
#include "entities/ResourceTypeHandler.h"
#include "entities/building/TownFortifications.h"
#include "entities/faction/CTown.h"
#include "mapping/CMapHeader.h"
#include "mapping/TerrainTile.h"
#include "networkPacks/PacksForClient.h"
#include "networkPacks/PacksForClientBattle.h"
#include "networkPacks/PacksForServer.h"
#include "gameState/CGameState.h"
#include "gameState/GameStatistics.h"
#include "CPlayerState.h"
#include "pathfinder/PathfinderCache.h"
#include "pathfinder/CGPathNode.h"
#include "pathfinder/PathfinderOptions.h"
#include "logging/VisualLogger.h"

// DMB Dev's isolation check (dmb_isolation.py, September 26th) reads this from
// the DLL's bytes: every file OmniAI writes (decision logs, the PAUSE file,
// the learning memory) follows OMNIAI_DIR. Exported so it is never dropped.
extern "C" __declspec(dllexport) const char dmbIsolationMarker[] = "DMB-ISOLATION-1";

namespace
{
	/// One JSON object, built field by field, for the decision events
	/// (decisions-<colour>.jsonl). Strings are escaped; numbers are printed
	/// with six significant digits.
	class JsonObj
	{
	public:
		JsonObj & str(const char * k, const std::string & v)
		{
			key(k);
			s_ += '"';
			for(const unsigned char c : v)
			{
				switch(c)
				{
				case '"': s_ += "\\\""; break;
				case '\\': s_ += "\\\\"; break;
				case '\n': s_ += "\\n"; break;
				case '\r': s_ += "\\r"; break;
				case '\t': s_ += "\\t"; break;
				default:
					if(c < 0x20)
					{
						char b[8];
						std::snprintf(b, sizeof(b), "\\u%04x", unsigned(c));
						s_ += b;
					}
					else
						s_ += char(c);
				}
			}
			s_ += '"';
			return *this;
		}
		JsonObj & num(const char * k, double v)
		{
			key(k);
			char b[32];
			std::snprintf(b, sizeof(b), "%.6g", v);
			s_ += b;
			return *this;
		}
		JsonObj & integer(const char * k, int64_t v) { key(k); s_ += std::to_string(v); return *this; }
		JsonObj & boolean(const char * k, bool v) { key(k); s_ += v ? "true" : "false"; return *this; }
		JsonObj & pos(const char * k, const int3 & q)
		{
			key(k);
			s_ += "[" + std::to_string(q.x) + "," + std::to_string(q.y) + "," + std::to_string(q.z) + "]";
			return *this;
		}
		JsonObj & raw(const char * k, const std::string & json) { key(k); s_ += json; return *this; }
		std::string done() const { return s_ + "}"; }
	private:
		void key(const char * k)
		{
			if(!first_)
				s_ += ',';
			first_ = false;
			s_ += '"';
			s_ += k;
			s_ += "\":";
		}
		std::string s_ = "{";
		bool first_ = true;
	};

	/// The OmniAI folder under the VCMI user directory, resolved as LearningStore
	/// uses, because VCMI_lib does not export the boost filesystem internals
	/// that VCMIDirs::userDataPath() would drag in.
	std::string omniDir()
	{
		// The watch copy (C:\VCMI\watch, September 25th) keeps its logs and
		// its PAUSE file apart from a benchmark running beside it:
		// watch_match.py sets OMNIAI_DIR to a folder inside that copy.
		if(const char * own = std::getenv("OMNIAI_DIR"); own && *own)
			return own;
#ifdef OMNIAI_IN_TREE
		// Built with the engine (as DMB builds it): the engine's own user folder,
		// which its dirs.json decides. DMB's client also hands this folder over as
		// OMNIAI_DIR; this covers a server something else started.
		return (VCMIDirs::get().userDataPath() / "OmniAI").string();
#else
		PWSTR docs = nullptr;
		if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)))
		{
			const std::filesystem::path d =
				std::filesystem::path(docs) / "My Games" / "vcmi" / "OmniAI";
			CoTaskMemFree(docs);
			return d.string();
		}
		return {};
#endif
	}

	/// Of the heroes a tavern offers, the one bringing the most army. A hire
	/// is 2500 gold for a body AND its starting stacks, and those stacks vary
	/// by 2-3x between offers: Nullkiller scores every offer by its army cost
	/// (RecruitHeroBehavior.cpp:110), and we used to take whoever was listed
	/// first. A hero of the town's own faction fights beside that town's
	/// creatures without the mixed-faction morale penalty, the same preference
	/// Nullkiller carries at 1.5x, applied more gently here.
	const CGHeroInstance * bestHireOffer(const std::vector<const CGHeroInstance *> & offers,
		FactionID faction)
	{
		const CGHeroInstance * pick = nullptr;
		double pickScore = -1.0;
		for(const CGHeroInstance * offer : offers)
		{
			if(!offer)
				continue;
			double score = double(offer->getArmyStrength());
			if(offer->getFactionID() == faction)
				score *= 1.25;
			if(score > pickScore)
			{
				pickScore = score;
				pick = offer;
			}
		}
		return pick ? pick : (offers.empty() ? nullptr : offers.front());
	}
}

namespace omniai
{
/**
 * Plain-text record of what the AI decided, and why.
 *
 * The engine's own log runs to megabytes of network tracing per game and says
 * nothing about reasoning. This is the other half: one line per decision,
 * readable end to end. Without it, working out why the AI played badly means
 * inferring it from which packs it happened to send, which is how the first
 * two playtests had to be diagnosed.
 *
 * Truncated when a game starts, so a run is never read against the tail of
 * the one before it.
 */
class DecisionLog
{
public:
	// One file per player, not one per process. An AI-only game loads this
	// same plugin once per side, and a single shared file meant whichever
	// side started second truncated the other's log away.
	void start(const std::string & who, const std::string & tag)
	{
		std::lock_guard lock(mutex_);
		const std::string dir = omniDir();
		if(dir.empty())
			return;
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		file_.close();
		file_.open(dir + "\\decisions-" + tag + ".log", std::ios::out | std::ios::trunc);
		if(file_)
			file_ << "=== OmniAI decision log, " << who << " ===\n" << std::flush;
		// The structured twin (September 25th, for the live observer and
		// MapGen's dashboard): one JSON object per line.
		json_.close();
		json_.open(dir + "\\decisions-" + tag + ".jsonl", std::ios::out | std::ios::trunc);
		player_ = tag;
	}

	void setDay(int day) { std::lock_guard lock(mutex_); day_ = day; }
	void setRound(int round) { std::lock_guard lock(mutex_); round_ = round; }
	/// Draw each hero's intent on the adventure map (VCMI's visual log,
	/// channel "omni.<colour>"): a watched game only.
	void setVisual(bool on) { std::lock_guard lock(mutex_); visual_ = on; }
	void setHero(const std::string & hero) { std::lock_guard lock(mutex_); hero_ = hero; }

	/// A structured event. The caller's object gets t, day, p and hero
	/// (when one is acting) in front of its own fields.
	void event(const std::string & type, const std::string & fields)
	{
		std::lock_guard lock(mutex_);
		writeEvent(type, fields);
	}

	void line(const std::string & text)
	{
		std::lock_guard lock(mutex_);
		if(file_)
			file_ << text << "\n" << std::flush;
		derive(text);
		if(tap_)
			tap_(text);
	}

	void detail(const std::string & text) { line("      " + text); }

	// Every line also goes to this, when set: the spectator narration.
	// It must only queue, never log or send, since it runs under the lock
	// and on whatever thread wrote the line.
	void setTap(std::function<void(const std::string &)> tap)
	{
		std::lock_guard lock(mutex_);
		tap_ = std::move(tap);
	}

private:
	void writeEvent(const std::string & type, const std::string & fields)
	{
		if(!json_)
			return;
		JsonObj head;
		head.integer("n", ++seq_).str("t", type).integer("day", day_).str("p", player_);
		if(round_ > 0)
			head.integer("round", round_);
		if(!hero_.empty())
			head.str("hero", hero_);
		std::string out = head.done();
		out.pop_back();                       // reopen the object
		if(fields.size() > 2)
			out += "," + fields.substr(1);    // the caller's fields, without its brace
		else
			out += "}";
		json_ << out << "\n" << std::flush;
	}

	static std::string ruleOf(const std::string & verb)
	{
		static const std::map<std::string, std::string> rules = {
			{"RESUPPLY", "resupply"}, {"PRESS", "capital-press"}, {"TASK", "registry"},
			{"RE-ENTER", "deferred-guard"}, {"DEFER", "deferred-guard"}, {"CROSSING", "crossing"},
			{"SUMMON", "crossing"}, {"SHIPYARD", "crossing"}, {"HUNT", "hunt"}, {"RAID", "raid"},
			{"GRIND", "grind"}, {"SCOUT", "scout"}, {"DEEP", "scout"}, {"EXPLORE", "scout"},
			{"SALLY", "sally"}, {"HOME", "home"}, {"MARCH", "conquest"}, {"SIEGE", "conquest"},
			{"DEFEND", "defend"}, {"HOLD", "defend"}, {"EVACUATE", "defend"}, {"LAST", "defend"}, {"ABANDON", "defend"},
			{"ESCAPE", "escape"}, {"RETREAT", "escape"}, {"MOVE", "scoring"}, {"COLLECT", "collect"},
			{"PROBE", "probe"}, {"MINE", "mine-run"}, {"SUPPLY", "supply"}, {"GARRISON", "post"},
			{"TO", "post"}, {"POOL", "pool"}, {"PRIZE", "prize"}, {"DOOR", "post"}, {"WALLS", "defend"} };
		const auto it = rules.find(verb);
		return it == rules.end() ? "other" : it->second;
	}

	struct VisHero
	{
		int3 pos = int3(-1, -1, -1);
		int3 target = int3(-1, -1, -1);
		std::string order;
		std::vector<std::pair<int3, std::string>> marks;
	};

	static bool firstInt3(const std::string & text, int3 & out)
	{
		static const std::regex rx("\\((\\d+) (\\d+) (\\d+)\\)");
		std::smatch m;
		if(!std::regex_search(text, m, rx))
			return false;
		out = int3(std::atoi(m[1].str().c_str()), std::atoi(m[2].str().c_str()), std::atoi(m[3].str().c_str()));
		return true;
	}

	void mark(const int3 & at, const std::string & label)
	{
		if(hero_.empty())
			return;
		auto & marks = vis_[hero_].marks;
		if(marks.size() < 12)
			marks.emplace_back(at, label.size() > 40 ? label.substr(0, 37) + "..." : label);
		if(visual_)
			redraw();
	}

	/// The whole channel again: each hero's name where it stands, a line to
	/// its current target with the order at the target, and the ranked and
	/// skipped candidates of its latest scoring pass. Each call replaces the
	/// channel's contents (VisualLogger::updateWithLock).
	void redraw()
	{
		if(!logVisual)
			return;
		logVisual->updateWithLock("omni." + player_, [this](IVisualLogBuilder & b)
		{
			for(const auto & kv : vis_)
			{
				const VisHero & v = kv.second;
				if(v.pos.z < 0)
					continue;
				b.addText(v.pos, kv.first, ColorRGBA(0, 0, 0, 200));
				if(v.target.z >= 0)
				{
					b.addLine(v.pos, v.target);
					b.addText(v.target, kv.first + ": " + v.order, ColorRGBA(20, 60, 130, 210));
				}
				for(const auto & mk : v.marks)
					b.addText(mk.first, mk.second, mk.second.rfind("x ", 0) == 0
						? ColorRGBA(130, 20, 20, 190) : ColorRGBA(60, 60, 60, 180));
			}
		});
	}

	/// The first "(x y z)" in a line, as a JSON array, or empty.
	static std::string firstPos(const std::string & text)
	{
		static const std::regex rx("\\((\\d+) (\\d+) (\\d+)\\)");
		std::smatch m;
		if(!std::regex_search(text, m, rx))
			return {};
		return "[" + m[1].str() + "," + m[2].str() + "," + m[3].str() + "]";
	}

	/// Events for lines whose text is already regular: orders, walk vetoes,
	/// skipped candidates, the ranked candidates, builds, savings, recruits,
	/// threats, a new commander, the pause.
	void derive(const std::string & text)
	{
		if(!json_ || text.size() < 4)
			return;
		const size_t lead = text.find_first_not_of(' ');
		if(lead == std::string::npos)
			return;
		const std::string body = text.substr(lead);
		size_t end = 0;
		while(end < body.size() && (std::isupper(static_cast<unsigned char>(body[end])) || body[end] == '-'))
			++end;
		const std::string verb = body.substr(0, end);
		static const std::set<std::string> scans = { "GRINDSCAN", "HUNTSCAN", "GROW", "ECON", "TRUTH",
			"ROSTER", "BUILDSCAN", "MAP", "HERO", "DAY" };
		const std::string where = firstPos(body);
		if(lead == 2 && verb == "HERO")
		{
			// "HERO <name> (<role>) at (x y z), N MP": where this hero stands.
			const size_t open = body.find(" (");
			int3 at;
			if(open != std::string::npos && firstInt3(body, at))
			{
				vis_[body.substr(5, open - 5)].pos = at;
				if(visual_)
					redraw();
			}
			return;
		}
		if(lead == 3 && verb.size() >= 3 && !scans.count(verb))
		{
			if(verb == "FENCED" || verb == "HELD")
			{
				JsonObj o;
				o.str("kind", verb == "FENCED" ? "fenced" : "held");
				if(!where.empty())
					o.raw("tile", where);
				o.str("text", body);
				writeEvent("veto", o.done());
				return;
			}
			JsonObj o;
			o.str("verb", verb).str("rule", ruleOf(verb));
			if(!hero_.empty())
			{
				const auto hv = vis_.find(hero_);
				if(hv != vis_.end() && hv->second.pos.z >= 0)
					o.pos("from", hv->second.pos);
			}
			if(!where.empty())
				o.raw("target_pos", where);
			o.str("text", body);
			writeEvent("order", o.done());
			if(!hero_.empty())
			{
				VisHero & v = vis_[hero_];
				int3 to;
				v.target = firstInt3(body, to) ? to : int3(-1, -1, -1);
				v.order = body.size() > 60 ? body.substr(0, 57) + "..." : body;
				if(visual_)
					redraw();
			}
			return;
		}
		if(lead == 3 && body.rfind("best ", 0) == 0)
		{
			static const std::regex rx("best (.+) at \\((\\d+) (\\d+) (\\d+)\\)\\s+score ([-\\d.e+]+)");
			std::smatch m;
			if(std::regex_search(body, m, rx))
			{
				writeEvent("candidate", JsonObj().integer("rank", 1).str("name", m[1].str())
					.raw("pos", "[" + m[2].str() + "," + m[3].str() + "," + m[4].str() + "]")
					.num("score", std::atof(m[5].str().c_str())).done());
				mark(int3(std::atoi(m[2].str().c_str()), std::atoi(m[3].str().c_str()), std::atoi(m[4].str().c_str())),
					"#1 " + m[1].str() + " " + std::to_string(int(std::atof(m[5].str().c_str()))));
			}
			rank_ = 1;
			return;
		}
		if(lead == 8)
		{
			static const std::regex rx("^(.+) at \\((\\d+) (\\d+) (\\d+)\\)\\s+score ([-\\d.e+]+)");
			std::smatch m;
			if(std::regex_search(body, m, rx))
			{
				writeEvent("candidate", JsonObj().integer("rank", ++rank_).str("name", m[1].str())
					.raw("pos", "[" + m[2].str() + "," + m[3].str() + "," + m[4].str() + "]")
					.num("score", std::atof(m[5].str().c_str())).done());
				mark(int3(std::atoi(m[2].str().c_str()), std::atoi(m[3].str().c_str()), std::atoi(m[4].str().c_str())),
					"#" + std::to_string(rank_) + " " + m[1].str() + " " + std::to_string(int(std::atof(m[5].str().c_str()))));
			}
			return;
		}
		if(lead == 6 && body.rfind("skip ", 0) == 0)
		{
			const size_t colon = body.find(": ");
			std::string name = body.substr(5, colon == std::string::npos ? std::string::npos : colon - 5);
			const size_t at = name.find(" at (");
			if(at != std::string::npos)
				name = name.substr(0, at);
			const std::string why = colon == std::string::npos ? "" : body.substr(colon + 2);
			std::string kind = "other";
			if(why.rfind("no route", 0) == 0) kind = "no-route";
			else if(why.find("guarded by") != std::string::npos) kind = "guarded";
			else if(why.find("leash") != std::string::npos) kind = "leashed";
			else if(why.find("fence") != std::string::npos || why.find("FENCED") != std::string::npos) kind = "fenced";
			else if(why.find("reserved") != std::string::npos || why.find("targeted") != std::string::npos) kind = "reserved";
			JsonObj o;
			o.str("name", name);
			if(!where.empty())
				o.raw("pos", where);
			o.str("why", kind).str("text", why);
			writeEvent("skip", o.done());
			int3 spot;
			if(firstInt3(body, spot))
				mark(spot, "x " + name + ": " + kind);
			return;
		}
		if(lead == 6 && body.rfind("stopping short of", 0) == 0)
		{
			JsonObj o;
			o.str("kind", "stop-short");
			if(!where.empty())
				o.raw("tile", where);
			o.str("text", body);
			writeEvent("veto", o.done());
			return;
		}
		if(lead == 6 && body.rfind("saving ", 0) == 0)
		{
			writeEvent("save", JsonObj().str("text", body).done());
			return;
		}
		if(lead == 2)
		{
			if(verb == "BUILD" || verb == "RECRUIT" || verb == "THREAT" || verb == "COMMANDER"
				|| verb == "PAUSED" || verb == "DEPOSIT" || verb == "CHEST" || verb == "SUPPLY")
			{
				JsonObj o;
				if(!where.empty())
					o.raw("pos", where);
				o.str("text", body);
				std::string type = verb == "BUILD" ? "build" : verb == "RECRUIT" ? "recruit"
					: verb == "THREAT" ? "threat" : verb == "COMMANDER" ? "commander"
					: verb == "PAUSED" ? "paused" : verb == "DEPOSIT" ? "deposit"
					: verb == "CHEST" ? "chest" : "handover";
				writeEvent(type, o.done());
			}
			else if(body.find(" candidates scored") != std::string::npos)
			{
				JsonObj o;
				if(!where.empty())
					o.raw("pos", where);
				o.str("text", body);
				writeEvent("scoring", o.done());
				if(!hero_.empty())
					vis_[hero_].marks.clear();   // a fresh ranking for this hero
			}
		}
	}

	std::mutex mutex_;
	std::ofstream file_;
	std::ofstream json_;
	std::function<void(const std::string &)> tap_;
	int day_ = 0;
	int rank_ = 0;
	std::string player_;
	std::string hero_;
	bool visual_ = false;
	std::map<std::string, VisHero> vis_;
	int round_ = 0;
	int64_t seq_ = 0;
};
}

OmniAI::OmniAI()
{
	registry_ = std::make_unique<omniai::HandleRegistry>();
	actionQueue_ = std::make_unique<omniai::PendingActionQueue>();
	decisionLog_ = std::make_unique<omniai::DecisionLog>();
}

OmniAI::~OmniAI()
{
	{
		std::lock_guard lock(taskMutex_);
		stopping_ = true;
	}
	taskCv_.notify_all();
	for(auto & w : workers_)
		if(w.joinable())
			w.join();
}

/**
 * Queue work for the AI's own thread.
 *
 * Nothing that sends a request to the server may run on the thread that
 * delivered the callback, and the reason is specific rather than stylistic.
 * Requests now block until the server confirms them, and the way the engine
 * implements that wait is:
 *
 *     auto gsUnlocker = vstd::makeUnlockSharedGuard(CGameState::mutex);
 *     waitingRequest.waitWhileContains(requestID);
 *
 * It RELEASES the game state lock while waiting, so the network thread can
 * deliver the reply, then takes it back. Which means the caller has to be
 * holding that lock, in shared mode, or the engine unlocks a mutex the thread
 * does not own. That is undefined behavior, and it is what killed the client
 * on the first build request of playtest 2: the wait started and the process
 * was gone a millisecond later.
 *
 * So the worker takes the lock for the duration of each task, exactly as
 * Nullkiller does for every one of its async actions (AIGateway.cpp:1626).
 *
 * One worker rather than a pool, deliberately. Tasks have to run in the order
 * they were queued, or a turn could start before the query that granted it
 * has been answered.
 */
void OmniAI::executeAsync(const char * what, std::function<void()> fn)
{
	{
		std::lock_guard lock(taskMutex_);
		if(stopping_)
			return;
		tasks_.emplace_back(what, std::move(fn));
		// A pool, not one thread, and the reason is deadlock rather than
		// throughput. A turn is long and blocks on every request. If the
		// server asks a question mid-move, say a recruitment window opening
		// as a hero walks into a dwelling, the answer cannot wait behind the
		// turn: the turn is waiting on the move and the move is waiting on
		// the answer. Separate threads break the cycle, and shared locks on
		// the game state do not block each other, so they really do run at
		// the same time.
		if(workers_.empty())
			for(int i = 0; i < 4; ++i)
				workers_.emplace_back(&OmniAI::asyncWorker, this);
	}
	taskCv_.notify_one();
}

void OmniAI::asyncWorker()
{
	for(;;)
	{
		std::pair<const char *, std::function<void()>> task;
		{
			std::unique_lock lock(taskMutex_);
			taskCv_.wait(lock, [this]{ return stopping_ || !tasks_.empty(); });
			if(stopping_ && tasks_.empty())
				return;
			task = std::move(tasks_.front());
			tasks_.pop_front();
		}

		// Held for the whole task. See executeAsync above for why.
		std::shared_lock<std::shared_mutex> gsLock(CGameState::mutex);

		// A throw crossing a thread boundary terminates the process, so
		// nothing is allowed out.
		try
		{
			task.second();
		}
		catch(const std::exception & e)
		{
			logAi->error("OmniAI: %s failed: %s", task.first, e.what());
		}
		catch(...)
		{
			logAi->error("OmniAI: %s failed with an unknown exception", task.first);
		}
	}
}

void OmniAI::initGameInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CCallback> CB)
{
	cb = CB;
	cbc = CB; // CAdventureAI::battleStart asserts this; adventure cb doubles as battle cb
	env = ENV;
	human = false;

	// Block until the server has actually applied each request.
	//
	// This defaults to false, and leaving it false is why the AI could only
	// ever act once per hero per turn. Every moveHero and recruitCreatures
	// returned immediately, before the server had moved anything, so the
	// hero's position and remaining movement were stale the instant we looked
	// at them again. There was no safe way to decide what to do next, so the
	// turn simply ended. A playtest over 30 game days produced 31 move orders
	// and 225 tiles walked, about 40 percent of the movement available.
	//
	// Nullkiller sets the same flag for the same reason (AIGateway.cpp:593).
	cb->waitTillRealize = true;
	if(const auto pid = cb->getPlayerID())
		playerID = *pid;
	else
		logAi->error("OmniAI: no playerID in initGameInterface; callbacks will be ignored");

	profile_ = &omniai::profileFor(difficultyIndex());
	decisionLog_->start(
		std::string("player ") + playerID.toString() + ", difficulty " + profile_->name,
		playerID.toString());
	// Narration for a person watching (K's ask, September 24th: a way to see
	// the AI's moves AND its reasons live). The client turns spectator mode
	// on by itself for any AI-only game that is not headless, so the
	// benchmark harness, which runs headless, never narrates.
	// OMNIAI_NARRATE forces it on for a test.
	{
		// The first event of a game (MapGen's dashboard finds and draws the
		// map from it, and a new start line tells a live follower a new game
		// began): the map as the harness named it, its size, every seat.
		const int3 sz = cb->getMapSize();
		std::string seats = "[";
		if(const StartInfo * si = cb->getStartInfo())
			for(const auto & pi : si->playerInfos)
			{
				if(seats.size() > 1)
					seats += ",";
				const JsonNode & ai = settings["ai"]["playerAIOverrides"][pi.first.toString()];
				seats += JsonObj().str("p", pi.first.toString())
					.str("ai", ai.isString() ? ai.String() : std::string())
					.boolean("human", pi.second.isControlledByHuman()).done();
			}
		seats += "]";
		char when[32] = {};
		const std::time_t now = std::time(nullptr);
		std::tm tmv{};
		localtime_s(&tmv, &now);
		std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
		decisionLog_->event("start", JsonObj()
			.str("map", settings["session"]["testmap"].isString() ? settings["session"]["testmap"].String() : std::string())
			.integer("w", sz.x).integer("h", sz.y).integer("levels", sz.z)
			.raw("seats", seats).str("build", std::string(__DATE__) + " " + __TIME__)
			.str("difficulty", profile_ ? profile_->name : "").str("time", when).done());
	}
	narrate_ = settings["session"]["spectate"].Bool() || std::getenv("OMNIAI_NARRATE") != nullptr;
	// A chat line in a game with no player window crashes the client: the
	// engine's GameChatHandler::onNewGameMessageReceived reads
	// GAME->interface()->cb without a check, and a headless AI-only game has
	// no interface (verify_r7y, September 25th: "Attempt to read from
	// 0x00000000000000C0" four seconds in, on the first [OmniAI] line). The
	// chat is for a person at a window, so a headless game gets the pause,
	// the events and the overlay channel, and no chat.
	chat_ = narrate_ && !settings["session"]["headless"].Bool();
	if(narrate_)
	{
		if(chat_)
			decisionLog_->setTap([this](const std::string & text) { queueNarration(text); });
		// The live overlay: the client draws the chosen visual-log channel over
		// the adventure map (MapView -> MapOverlayLogVisualizer). Selecting it
		// here saves typing "/vslog omni.<colour>" in the game console, which
		// still switches channels (Nullkiller publishes "<colour>.danger.*").
		decisionLog_->setVisual(true);
		if(logVisual)
			logVisual->setKey("omni." + playerID.toString());
	}
	omniai::LearningStore::instance().setDir(omniDir());
	omniai::LearningStore::instance().init(cb.get());
	omniai::QSBRReclaimer::instance().setParticipants(
		profile_->maxThreads ? profile_->maxThreads : uint32_t(std::thread::hardware_concurrency()));

	dispatcher_ = std::make_unique<omniai::ActionDispatcher>(cb, *actionQueue_);
	evaluator_ = std::make_unique<omniai::UtilityEvaluator>(*registry_, *profile_, ENV.get());

	// Teleports: only the deterministic channels. Two-way monoliths and
	// single-exit one-way monoliths have one possible exit, so the forced
	// dialog lands where the pathfinder planned; random-exit monoliths and
	// whirlpools stay off because the protocol has no "decline" answer
	// (any invalid index rolls a random exit, HeroMovementController.cpp:79).
	PathfinderOptions opts(*cb);
	opts.useTeleportTwoWay = true;
	opts.useTeleportOneWay = true;
	opts.useTeleportOneWayRandom = false;
	opts.useTeleportWhirlpool = false;
	pathCache_ = std::make_unique<PathfinderCache>(cb.get(), opts);

	// Per-game world state: the enemy-bearing hint and the deferred route
	// guards belong to this map, not the last one. The world-object records
	// and the task list built on them are the same - a stale guard position
	// or a carried-over delivery assignment from a finished game would
	// mislead the new one.
	enemyBeaconValid_ = false;
	enemyStartValid_ = false;
	foeEverSeen_ = false;
	// Where the enemy lives is declared in the map header: every player's
	// start town is public scenario information, the same thing a human
	// sees looking at the map layout before fog covers it. Seed the hunt
	// beacon from it so the press knows which side is theirs from day 1
	// instead of waiting for a hero to surface inside sight range - the
	// Nullkiller digests show its main marching on our capital by day 2,
	// and this is the parity version of that knowledge. First enemy
	// declared wins; a real sighting overwrites it.
	if(const CMapHeader * mh = cb->getMapHeader())
		for(int p = 0; p < int(mh->players.size()); ++p)
		{
			const PlayerInfo & pi = mh->players[p];
			if(PlayerColor(p) == playerID || !pi.hasMainTown
				|| !pi.posOfMainTown.isValid()
				|| cb->getPlayerRelations(playerID, PlayerColor(p)) != PlayerRelations::ENEMIES)
				continue;
			enemyBeacon_ = pi.posOfMainTown;
			enemyBeaconValid_ = true;
			// Kept apart from the beacon, which sightings overwrite: the raid
			// needs where their capital is, not where their hero was.
			enemyStart_ = pi.posOfMainTown;
			enemyStartValid_ = true;
			decisionLog_->line("  enemy start declared at "
				+ enemyBeacon_.toString() + " - hunt beacon seeded");
			break;
		}
	deferredTasks_.clear();
	regTasks_.clear();
	worldObjs_.clear();
	truthBuilt_.clear();
	buildReserve_.clear();
	regenDay_ = -1;
	secondFieldId_ = -1;
	freshlyCaptured_.clear();

	// Say where the tier came from. A match run at the wrong tier and a match
	// run at the right one look identical without this line.
	{
		const JsonNode & bySlot = settings["ai"]["omniaiDifficultyBySlot"][playerID.toString()];
		const JsonNode & pinned = settings["ai"]["omniaiDifficulty"];
		const bool isPinned = (bySlot.isNumber() && bySlot.Integer() >= 0 && bySlot.Integer() <= 4)
			|| (pinned.isNumber() && pinned.Integer() >= 0 && pinned.Integer() <= 4);
		decisionLog_->line(std::string("  difficulty profile '")
			+ profile_->name + "' ("
			+ (isPinned ? "pinned by ai.omniaiDifficulty" : "from the scenario")
			+ ")");
	}
	logAi->info("OmniAI initialized for player %d, difficulty profile '%s'",
		int(playerID.getNum()), profile_->name);
}

int OmniAI::difficultyIndex() const
{
	// Our own tier, when one is pinned. Both AIs read StartInfo.difficulty,
	// Nullkiller to load its per-difficulty settings and we to pick a
	// decision profile, so on one map they are otherwise always the same
	// tier and a match between two different tiers cannot be set up at all.
	//
	// This pins OUR profile and nothing else. The engine-side cheats
	// (starting resources, config/difficulty.json, CGameState::initDifficulty)
	// still come from the scenario and still apply to every AI player
	// equally, so both sides start with the same pile and only the quality of
	// play differs. That is the comparison worth making.
	//
	// -1 or absent means follow the scenario, which is the old behaviour.
	// Per slot first: ai.omniaiDifficultyBySlot.<colour> (0..4, -1 unset) is
	// what the config panel's per-slot override writes. A separate key,
	// because the settings schema declares omniaiDifficulty as a number and
	// an object there would be reset on load. Then the all-slots pin.
	const JsonNode & bySlot = settings["ai"]["omniaiDifficultyBySlot"][playerID.toString()];
	if(bySlot.isNumber() && bySlot.Integer() >= 0 && bySlot.Integer() <= 4)
		return static_cast<int>(bySlot.Integer());
	const JsonNode & pinned = settings["ai"]["omniaiDifficulty"];
	if(pinned.isNumber())
	{
		const int want = static_cast<int>(pinned.Integer());
		if(want >= 0 && want <= 4)
			return want;
	}

	// The scenario's difficulty selection (EMapDifficulty 0..4) drives both
	// the engine-side cheat config (config/difficulty.json applies AI
	// resources/bonuses automatically) and our decision-profile tier.
	if(cb && cb->getStartInfo())
		return static_cast<int>(cb->getStartInfo()->difficulty);
	return 1; // Normal
}

void OmniAI::snapshotRegistry()
{
	registry_->clear();
	if(!cb)
		return;
	// Owned objects are always candidates (defend a town, reposition a hero);
	// getAllVisitableObjs returns every visible visitable map object (the
	// actual targets), FoW-tracked seen set stays as a supplement.
	for(const CGObjectInstance * obj : cb->getMyObjects())
		if(obj)
			registry_->acquire(obj->id.getNum());
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
		if(obj)
			registry_->acquire(obj->id.getNum());
	std::vector<int32_t> seen;
	{
		std::lock_guard lock(seenMutex_);
		seen.assign(seenObjs_.begin(), seenObjs_.end());
	}
	for(const int32_t id : seen)
		if(cb->getObj(ObjectInstanceID(id), false)) // drop stale entries
			registry_->acquire(id);
		else
		{
			std::lock_guard lock(seenMutex_);
			seenObjs_.erase(id);
		}
}

std::string OmniAI::getBattleAIName() const
{
	return "BattleAI";
}

void OmniAI::maybeEndTurn()
{
	// endTurn is a server request and therefore blocks, so it cannot go out
	// on whichever thread happened to call this. requestRealized in
	// particular runs on the network thread and is itself what wakes a
	// blocked request, so sending from there stops the whole client.
	if(!endTurnPending_ || !cb)
		return;

	// Not while a fight is on. The server refuses every pack from a player
	// holding an open CBattleQuery, and this retry fires from
	// requestRealized, which is where that refusal arrives, so each refusal
	// sends another. Measured: 2113 EndTurn packs across 298 player-turns
	// and 3466 query complaints in a 149 day run. battleEnd calls back in
	// here, so the turn closes as soon as the fight does.
	//
	// Bounded, because holding it forever would hang the turn and take the
	// game with it, which is the failure this retry exists to prevent. After
	// 30 seconds it goes out and takes the refusal.
	// Same hold for an open query as for an open battle: the server refuses
	// every non-QueryReply pack while one is up, so endTurn goes out rejected.
	// A leaked count self-clears on the same 30s bound.
	if(battlesActive_.load() > 0 || openQueries_.load() > 0)
	{
		const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		int64_t since = endTurnHeldSince_.load();
		if(since == 0)
		{
			endTurnHeldSince_.compare_exchange_strong(since, now);
			return;
		}
		if(now - since < 30LL * 1000 * 1000 * 1000)
			return;
	}
	endTurnHeldSince_.store(0);

	executeAsync("endTurn", [this]
	{
		if(endTurnPending_ && cb)
			cb->endTurn();
	});
}

void OmniAI::answerQuery(QueryID queryID, int selection)
{
	// QueryID(-1) marks a non-real query (Nullkiller convention): no reply.
	if(queryID != QueryID(-1))
		cb->selectionMade(selection, queryID);
	// One open query closed. Release the movement/endTurn gate. Floored at
	// zero - a reply to a query that never incremented (a stray -1 or an
	// engine auto-resolve) must not drive the count negative.
	if(openQueries_.fetch_sub(1) <= 1)
		openQueries_.store(0);
	// If our earlier endTurn was dropped because this query was open,
	// answering it frees the pipeline - retry now.
	maybeEndTurn();
}

void OmniAI::yourTurn(QueryID queryID)
{
	// A fresh turn means the pipeline is clear except the turn-start query
	// itself; setting the count also self-heals any value leaked across the
	// boundary by a query that resolved without our reply.
	openQueries_.store(1);
	// Answering the query is itself a server request, so even that cannot
	// happen here. Both steps go to the worker, in order.
	executeAsync("yourTurn", [this, queryID]
	{
		answerQuery(queryID, 0);
		runTurn();
		endTurnPending_ = true;
		maybeEndTurn();
	});
}

void OmniAI::runTurn()
{
	if(!cb)
		return;

	{
		std::ostringstream o;
		decisionLog_->setDay(cb->getDate(Date::DAY));
		decisionLog_->setHero("");
		o << "\nDAY " << cb->getDate(Date::DAY)
		  << "  gold " << cb->getResourceAmount(EGameResID::GOLD)
		  << "  wood " << cb->getResourceAmount(EGameResID::WOOD)
		  << "  ore " << cb->getResourceAmount(EGameResID::ORE)
		  << "  heroes " << cb->getHeroesInfo().size()
		  << "  towns " << cb->getTownsInfo().size();
		// Our own army, in the same units the THREAT lines report theirs in.
		// Without this the one question duel-v2 actually poses cannot be
		// answered from the log: their Oidana is at 9457 on day 3 and we
		// fight at odds 0.22, and nothing recorded what we were carrying or
		// when the gap opened. Best hero and total, because a column split
		// four ways loses to a hero that is not.
		{
			uint64_t best = 0, total = 0;
			for(const CGHeroInstance * h : cb->getHeroesInfo())
			{
				if(!h)
					continue;
				const uint64_t a = uint64_t(h->getArmyStrength());
				total += a;
				best = std::max(best, a);
			}
			o << "  army " << best << "/" << total;
		}
		// Growth inputs, ours against theirs: our mine count is the income
		// engine, the strongest enemy hero's army is their compounding curve.
		// The decomposition the siege gap needs - do they out-earn us, out-
		// recruit us, or simply out-tempo us to the same pool.
		int mines[7] = {};
		{
			for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
				if(obj && obj->ID == Obj::MINE && obj->tempOwner == playerID)
				{
					const auto * m = dynamic_cast<const CGMine *>(obj);
					const int r = m ? m->producedResource.getNum() : -1;
					if(r >= 0 && r < 7)
						++mines[r];
				}
			uint64_t foe = 0;
			for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
			{
				const auto * h = dynamic_cast<const CGHeroInstance *>(obj);
				if(h && h->tempOwner.isValidPlayer() && h->tempOwner != playerID)
					foe = std::max(foe, uint64_t(h->getArmyStrength()));
			}
			o << "  mines " << (mines[0]+mines[1]+mines[2]+mines[3]+mines[4]+mines[5]+mines[6])
			  << "  foe " << foe;
		}
		decisionLog_->line(o.str());

		// GROWTH DECOMPOSITION, both sides, once a day. The divergence report
		// needs each side's income proxies, creature tier mix and hero/town
		// counts to name the input that bends our curve flat while theirs
		// climbs. mines order is w,o,mercury,sulfur,crystal,gems.
		{
			const CGHeroInstance * fieldHero = nullptr;
			uint64_t best = 0;
			for(const CGHeroInstance * h : cb->getHeroesInfo())
				if(h && uint64_t(h->getArmyStrength()) > best)
				{
					best = uint64_t(h->getArmyStrength());
					fieldHero = h;
				}
			int eTowns = 0, eHeroes = 0;
			const CGHeroInstance * foeHero = nullptr;
			uint64_t foeArmy = 0;
			std::ostringstream foes;
			for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
			{
				if(!obj)
					continue;
				if(obj->ID == Obj::TOWN && obj->tempOwner.isValidPlayer()
					&& obj->tempOwner != playerID)
					++eTowns;
				const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
				if(e && e->tempOwner.isValidPlayer() && e->tempOwner != playerID)
				{
					++eHeroes;
					if(uint64_t(e->getArmyStrength()) > foeArmy)
					{
						foeArmy = uint64_t(e->getArmyStrength());
						foeHero = e;
					}
					// Every visible enemy hero, not just the biggest: their
					// roster shape (one main vs many carriers) is half the
					// comparison K wants, and a day-3 stack appearing from
					// fog says when the blitz actually started moving.
					foes << " " << e->getNameTranslated() << "="
						 << uint64_t(e->getArmyStrength())
						 << "@" << e->visitablePos().toString();
				}
			}
			// Adventure-spell instrumentation: does our roster ever learn the
			// tempo spells (Town Portal, Dimension Door, Fly)? Nullkiller's
			// AdventureSpellCast goal casts them; we walk. If our heroes
			// never know them, the fix is upstream (mage guilds, spell
			// priority), not casting.
			int advSpellHeroes = 0;
			int bestSpellLevel = 0;
			for(const CGHeroInstance * h : cb->getHeroesInfo())
			{
				if(!h)
					continue;
				if(h->maxSpellLevel() > bestSpellLevel)
					bestSpellLevel = h->maxSpellLevel();
				if(!h->hasSpellbook())
					continue;
				const auto & known = h->getSpellsInSpellbook();
				if(known.count(SpellID(SpellID::TOWN_PORTAL))
					|| known.count(SpellID(SpellID::DIMENSION_DOOR))
					|| known.count(SpellID(SpellID::FLY)))
					++advSpellHeroes;
			}
			std::ostringstream g;
			g << "   GROW d" << cb->getDate(Date::DAY)
			  << " field=" << (fieldHero ? armyTiers(fieldHero) : std::string("-"))
			  << " | foe=" << foeArmy
			  << " tier=" << (foeHero ? armyTiers(foeHero) : std::string("-"))
			  << " eTowns=" << eTowns << " eHeroes=" << eHeroes
			  << " | advSpells=" << advSpellHeroes
			  << " maxSpellLvl=" << bestSpellLevel
			  << " | minesW/O/M/S/C/G=" << mines[0] << "," << mines[2]
			  << "," << mines[1] << "," << mines[3] << "," << mines[4]
			  << "," << mines[5]
			  << " | foes:" << foes.str();
			decisionLog_->line(g.str());

			// ECON, once a day: where the gold comes from and what the
			// roster can service. Town income and gold mines are the income
			// side; the roster columns answer the hire question - is the
			// pool short because gold is short, or because coverage demand
			// (ready tasks waiting) outruns the bodies we field. That split
			// is what decides whether the fix is "earn more" or "hire sooner".
			{
				int townInc = 0;
				size_t townsHeld = 0;
				for(const CGTownInstance * t : cb->getTownsInfo())
					if(t && t->tempOwner == playerID)
					{
						++townsHeld;
						townInc += t->dailyIncome()[EGameResID::GOLD];
					}
				const int gold = cb->getResourceAmount(EGameResID::GOLD);
				const int heroes = int(cb->getHeroesInfo().size());
				int ready = 0, defNeed = 0;
				for(const auto & t : regTasks_)
				{
					if(t.kind == RegTask::Kind::Capture || t.kind == RegTask::Kind::Hold)
						++defNeed;
					else if(t.ready)
						++ready;
				}
				const int wanted = int(std::min<size_t>(6,
					1 + townsHeld + size_t((ready + 1) / 2) + size_t(defNeed)));
				std::ostringstream e;
				e << "   ECON d" << cb->getDate(Date::DAY)
				  << " gold=" << gold
				  << " townInc=" << townInc << "/d"
				  << " goldMines=" << mines[6]
				  << " | heroes=" << heroes << "/" << wanted
				  << " afford=" << (gold >= 2 * GameConstants::HERO_GOLD_COST ? 1 : 0)
				  << " | tasks=" << regTasks_.size()
				  << " ready=" << ready
				  << " covNeed=" << (heroes > 0 ? (ready + heroes - 1) / heroes : ready);
				decisionLog_->line(e.str());
			}
			const CGHeroInstance * grinder = fieldHeroId_ >= 0
				? cb->getHero(ObjectInstanceID(fieldHeroId_)) : nullptr;
			logGrindScan(grinder ? grinder : fieldHero);
		}
		logTruth();
	}
	// How much of the map is off the fog, once a week. The standing
	// question is whether finding an enemy town is achievable at all
	// inside the ~26 days a duel-v2 game lasts, and two cheap fixes for
	// it are already measured and rejected. A rate beats another guess.
	{
		const int3 size = cb->getMapSize();
		int seen = 0, total = 0;
		for(int z = 0; z < size.z; ++z)
			for(int y = 0; y < size.y; ++y)
				for(int x = 0; x < size.x; ++x)
				{
					++total;
					if(cb->isVisible(int3(x, y, z)))
						++seen;
				}
		const int week = cb->getDate(Date::DAY) / 7;
		if(week != lastSeenReportWeek_)
		{
			lastSeenReportWeek_ = week;
			std::ostringstream m;
			m << "  MAP seen " << seen << " of " << total << " ("
			  << (total > 0 ? seen * 100 / total : 0) << "%), size "
			  << size.x << "x" << size.y << "x" << size.z;
			decisionLog_->line(m.str());
		}

		// Frontier-dry counter: fog flat while no hostile town is known.
		// The pocket chores keep a field hero busy without ever reaching
		// the scout fallback, so the dry case has to be measured at the
		// map level, not at the fallback's log site.
		if(prevSeenCount_ >= 0 && seen == prevSeenCount_
			&& !knowsHostileTown())
			++frontierDryDays_;
		else
			frontierDryDays_ = 0;
		prevSeenCount_ = seen;
	}

	// Towns build first, before anyone moves. Building does not depend on a
	// hero being present, and doing it up front means the day's one building
	// is never lost to a hero wandering off and the turn ending.
	//
	// Hiring runs ahead of building, not after it: measured on Twins, a town
	// that can afford a building every day drains gold below the hire reserve
	// before the check ever runs, which is how a side with 10k gold banked
	// fields two heroes for a month. The reserve inside hireHeroIfNone still
	// keeps 2500 back for the army, so a hire cannot eat the recruit money.
	updateResourceScarcity();
	// The hire gate reads the threat map, and the enemy has moved since the
	// last round of yesterday.
	assessThreats();
	// Below the roster floor a hire is a burst, not a single purchase:
	// Nullkiller fields three heroes by day 1-2 and the pickup capacity
	// is the game on a small map. Looping the hire before the build pass
	// spends the day-1 treasury on bodies instead of walls - m007 showed
	// the single hire + build ordering ending day 1 at 2 heroes and gold
	// never reaching the hire line again all game.
	while(cb->getHeroesInfo().size() < 3 && hireHeroIfNone())
		;
	hireHeroIfNone();
	// Troops before the hall in the first three days when contact comes
	// early (R8a, September 25th). Nullkiller buys every creature on day 1
	// and its hall on day 2; we bought the Town Hall on day 1 with the last
	// 2500 gold in all 30 games of R7t_duel ("cannot afford Archer" right
	// after it), and its one merged hero of 10-14k met our 7-10k, split
	// across three heroes and a garrison, at the gate on day 2-3 in six of
	// them. contactSoon's rule in recruitFromDwelling only frees gold saved
	// toward a build; the build itself came first in this pass. The hall
	// now waits for the gold left after the troops, a few days.
	if(contactSoon() && cb->getDate(Date::DAY) <= 3)
		recruitInAllTowns();
	tradeForBlockedBuilding();
	buildInAllTowns();
	tradeForWhatIsMissing();
	recruitInAllTowns();
	decisionLog_->detail("finished with towns, moving on to heroes");

	// A hero keeps going until its movement is spent, not until it has
	// reached one thing.
	//
	// This used to score once per hero and dispatch a single path, which left
	// the hero standing still for the rest of the day with movement in hand.
	// Measured over a 30 day playtest: 1.03 move orders per day and 7.5 tiles
	// walked per day, against the 15 to 20 tiles a starting hero can cover on
	// open ground. Picking up one object per day is most of what made the AI
	// look asleep.
	//
	// Each round re-scores from scratch, because reaching an object changes
	// the picture: the object is consumed, fog lifts, new candidates appear.
	// The loop stops when no hero spent any movement, which covers a hero
	// that is boxed in as well as one that is simply finished.
	constexpr int MAX_ROUNDS = 16;   // bound, so a pathological cycle of two
	                                 // equally scored targets cannot hang a turn

	// Cleared once per turn, not per round: the point is to stop a hero
	// choosing the same target twice in one day.
	targetedThisTurn_.clear();
	collectedBy_.clear();
	frontierTriedThisTurn_.clear();
	stalledThisTurn_.clear();
	heldThisTurn_.clear();

	for(int round = 0; round < MAX_ROUNDS; ++round)
	{
		decisionLog_->setRound(round + 1);
		currentRound_ = round + 1;
		decisionLog_->detail("round " + std::to_string(round + 1) + ": taking stock");
		snapshotRegistry();
		assessThreats();
		consolidateTownDefence();
		assessPosture();
		assignHeroRoles();
		refreshTasks();

		// Standing evaluations, every round instead of once at dawn: gold
		// picked up mid-turn and the weekly pool convert the same day, the
		// way NK re-runs "Buy army" at every owned town every cycle. Both
		// calls early-out when nothing changed.
		hireHeroIfNone();
		recruitInAllTowns();

		// Work from ids, never from the pointers getHeroesInfo handed back.
		//
		// Moving can kill the hero doing the moving: walk onto a guard, lose
		// the battle, and the CGHeroInstance is destroyed before the call
		// returns, because requests are synchronous now. Holding the pointer
		// across the move and then reading its movement points is a read of
		// freed memory, and it would only bite on the turn the AI lost a
		// fight, which is exactly when nobody is looking at the debugger.
		// Re-resolving by id makes a dead hero simply come back null.
		std::vector<ObjectInstanceID> heroIds;
		for(const CGHeroInstance * h : cb->getHeroesInfo())
			if(h)
				heroIds.push_back(h->id);

		bool progressed = false;
		for(const ObjectInstanceID & id : heroIds)
		{
			const CGHeroInstance * hero = cb->getHero(id);
			if(!hero || hero->movementPointsRemaining() <= 0)
				continue;
			if(stalledThisTurn_.count(hero->id.getNum()))
				continue;   // already proved it cannot act today

			const int before = hero->movementPointsRemaining();
			const int3 posBefore = hero->visitablePos();
			waitWhilePaused(hero->getNameTranslated());
			// Who the next orders belong to. Many order lines do not name
			// their hero; this line lets a reader (and the observer page,
			// .tmp/omni/observer/observer_panel.py) put each one on a hero.
			decisionLog_->line("  HERO " + hero->getNameTranslated() + " (" + roleOf(hero) + ") at "
				+ posBefore.toString() + ", " + std::to_string(before) + " MP");
			decisionLog_->setHero(hero->getNameTranslated());
			{
				std::string stacks = "[";
				for(const auto & st : hero->Slots())
					if(st.second && st.second->getCreature())
					{
						if(stacks.size() > 1)
							stacks += ",";
						stacks += "[" + JsonObj().str("c", st.second->getCreature()->getNameSingularTranslated()).done()
							.substr(5, std::string::npos);
						stacks.pop_back();
						stacks += "," + std::to_string(st.second->getCount()) + "]";
					}
				stacks += "]";
				decisionLog_->event("hero", JsonObj().integer("id", hero->id.getNum())
					.str("role", roleOf(hero)).pos("pos", posBefore).integer("mp", before)
					.integer("level", int(hero->level))
					.integer("att", hero->getPrimSkillLevel(PrimarySkill::ATTACK))
					.integer("def", hero->getPrimSkillLevel(PrimarySkill::DEFENSE))
					.integer("pow", hero->getPrimSkillLevel(PrimarySkill::SPELL_POWER))
					.integer("kno", hero->getPrimSkillLevel(PrimarySkill::KNOWLEDGE))
					.integer("army", int64_t(hero->getArmyStrength()))
					.raw("stacks", stacks).done());
			}
			if(narrate_)
			{
				std::lock_guard lock(narrateMutex_);
				narrateHero_ = hero->getNameTranslated();
			}
			moveBestHero(hero);
			flushNarration();

			// Re-resolve before touching it again.
			hero = cb->getHero(id);
			if(!hero)
				continue;   // died on the way; the loop carries on without it

			// Requests are synchronous (waitTillRealize), so the count is
			// current here. Spending nothing means this hero has nowhere
			// useful left to go.
			if(hero->movementPointsRemaining() < before)
			{
				// Movement spent and the hero is on the tile it started on.
				// The order resolved as something other than a step, most
				// often a blocking visit onto a tile one of our own heroes
				// occupies. Re-issuing it does the same thing until the day
				// is gone, so this hero is finished for the turn.
				if(hero->visitablePos() == posBefore)
				{
					stalledThisTurn_.insert(hero->id.getNum());
					decisionLog_->detail(hero->getNameTranslated()
						+ " spent movement without leaving "
						+ posBefore.toString() + ", done for the turn");
					continue;
				}
				progressed = true;
			}
		}

		if(!progressed)
		{
			decisionLog_->detail("nobody moved, turn is done");
			break;
		}
	}

	flushNarration();
	// Turn boundary: sweep anything retired this round even if not every
	// worker announced a quiescent state.
	omniai::QSBRReclaimer::instance().purge();
}

void OmniAI::queueNarration(const std::string & text)
{
	// What a watcher needs: the orders (three spaces, then an upper-case
	// verb) and the day's events (two spaces: builds, fights, threats,
	// a new commander, a chest). Scans, standing lines, fences and the
	// attribution line stay in the log.
	const size_t lead = text.find_first_not_of(' ');
	if(lead == std::string::npos || (lead != 2 && lead != 3))
		return;
	const std::string body = text.substr(lead);
	size_t end = 0;
	while(end < body.size() && (std::isupper(static_cast<unsigned char>(body[end])) || body[end] == '-'))
		++end;
	const std::string verb = body.substr(0, end);
	if(verb.size() < 3 || verb == "HERO")
		return;
	static const std::set<std::string> quiet = { "GRINDSCAN", "HUNTSCAN", "GROW", "ECON", "TRUTH",
		"ROSTER", "BUILDSCAN", "FENCED", "HELD", "MAP" };
	if(quiet.count(verb))
		return;
	static const std::set<std::string> events = { "BUILD", "FIGHT", "THREAT", "COMMANDER", "CHEST" };
	if(lead == 2 && !events.count(verb))
		return;
	std::lock_guard lock(narrateMutex_);
	std::string msg = (lead == 3 && !narrateHero_.empty()) ? narrateHero_ + ": " + body : body;
	if(msg.size() > 140)
		msg = msg.substr(0, 137) + "...";
	narrateQueue_.push_back(std::move(msg));
	while(narrateQueue_.size() > 40)
		narrateQueue_.pop_front();
}

void OmniAI::waitWhilePaused(const std::string & where)
{
	// A watching aid (K, September 25th: he may sit in on games). While
	// Documents\My Games\vcmi\OmniAI\PAUSE exists, OmniAI holds before
	// its next hero action, so a person can read the observer page and the
	// map before the move is made; watch_match.py toggles the file from its
	// console. Only in a watched game (the same switch as the narration), so a
	// file left behind can never stall a benchmark, and never more than ten
	// minutes at a stretch.
	if(!narrate_)
		return;
	const std::string dir = omniDir();
	if(dir.empty())
		return;
	const std::filesystem::path flag = std::filesystem::path(dir) / "PAUSE";
	std::error_code ec;
	if(!std::filesystem::exists(flag, ec))
		return;
	decisionLog_->line("  PAUSED before " + where + "'s move (delete the PAUSE file to go on)");
	flushNarration();
	const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(10);
	while(std::filesystem::exists(flag, ec) && std::chrono::steady_clock::now() < until)
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

void OmniAI::flushNarration()
{
	// Sent from the turn loop only: a request must leave from the AI's own
	// thread (see executeAsync). A few lines per move is what a person reads
	// while watching; the rest is in the decision log and on the observer
	// page.
	if(!chat_ || !cb)
		return;
	std::deque<std::string> out;
	{
		std::lock_guard lock(narrateMutex_);
		out.swap(narrateQueue_);
	}
	int sent = 0;
	for(const std::string & m : out)
	{
		if(sent++ >= 6)
			break;
		cb->sendMessage("[OmniAI] " + m);
	}
}

void OmniAI::logTruth()
{
	if(!cb)
		return;
	// The base interface exposes the whole game state; CCallback only hides
	// the accessor. Read here and logged, never handed to a decision.
	const CGameState & gs = static_cast<const IGameInfoCallback &>(*cb).gameState();
	const int day = cb->getDate(Date::DAY);
	for(const auto & entry : gs.players)
	{
		const PlayerColor color = entry.first;
		const PlayerState & ps = entry.second;
		if(!color.isValidPlayer() || ps.status != EPlayerStatus::INGAME)
			continue;
		const auto heroes = ps.getHeroes();
		const auto towns = ps.getTowns();
		if(heroes.empty() && towns.empty())
			continue;
		uint64_t heroArmy = 0, garrison = 0, bestArmy = 0;
		int64_t exp = 0;
		const CGHeroInstance * best = nullptr;
		int maxLevel = 0;
		for(const CGHeroInstance * h : heroes)
		{
			if(!h)
				continue;
			const uint64_t a = uint64_t(h->getArmyStrength());
			heroArmy += a;
			exp += h->exp;
			maxLevel = std::max(maxLevel, int(h->level));
			if(!best || a > bestArmy)
			{
				best = h;
				bestArmy = a;
			}
		}
		int buildings = 0, forts = 0, manned = 0;
		std::ostringstream fresh;
		for(const CGTownInstance * t : towns)
		{
			if(!t)
				continue;
			garrison += uint64_t(t->getArmyStrength());
			// A hero inside holds the town even with no garrison troops: the
			// empty-capital count needs both (K_duel's "garrison 0" days
			// included towns with a Nullkiller hero standing in them).
			if(t->getGarrisonHero() || t->getVisitingHero())
				++manned;
			buildings += int(t->getBuildings().size());
			forts += int(t->fortLevel());
			// Name what went up since the last pass. The first sighting of a
			// town only seeds the record, so a captured town's whole list is
			// not reported as one day's construction.
			auto & seen = truthBuilt_[t->id.getNum()];
			const bool first = seen.empty();
			for(const BuildingID & b : t->getBuildings())
				if(seen.insert(b.getNum()).second && !first)
					fresh << " " << t->getNameTranslated() << "+" << b.getNum();
		}
		int mines = 0, goldMines = 0;
		for(const auto & m : Statistic::getNumMines(&gs, &ps))
		{
			mines += m.second;
			if(m.first == EGameResID::GOLD)
				goldMines += m.second;
		}
		std::ostringstream o;
		o << "   TRUTH d" << day << " " << color.toString()
		  << (color == playerID ? "*" : "")
		  << " gold=" << ps.resources[EGameResID::GOLD]
		  << " wood=" << ps.resources[EGameResID::WOOD]
		  << " ore=" << ps.resources[EGameResID::ORE]
		  << " inc=" << Statistic::getIncome(&gs, &ps) << "/d"
		  << " towns=" << towns.size()
		  << " bld=" << buildings << " fort=" << forts
		  << " heroes=" << heroes.size()
		  << " dw=" << Statistic::getNumberOfDwellings(&ps)
		  << " mines=" << mines << "(g" << goldMines << ")"
		  << " army=" << heroArmy << "+" << garrison
		  << " exp=" << exp << " maxL=" << maxLevel;
		if(best)
			o << " best=" << best->getNameTranslated() << ":" << bestArmy
			  << "@L" << int(best->level)
			  << " a" << best->getPrimSkillLevel(PrimarySkill::ATTACK)
			  << "/d" << best->getPrimSkillLevel(PrimarySkill::DEFENSE)
			  << "/p" << best->getPrimSkillLevel(PrimarySkill::SPELL_POWER)
			  << "/k" << best->getPrimSkillLevel(PrimarySkill::KNOWLEDGE)
			  << " " << best->visitablePos().toString();
		// After the best-hero block so the parsers' "maxL=N best=" shape holds.
		o << " manned=" << manned;
		if(!fresh.str().empty())
			o << " built:" << fresh.str();
		decisionLog_->line(o.str());
		{
			JsonObj tr;
			tr.str("player", color.toString()).boolean("us", color == playerID)
				.integer("gold", ps.resources[EGameResID::GOLD])
				.integer("wood", ps.resources[EGameResID::WOOD])
				.integer("ore", ps.resources[EGameResID::ORE])
				.integer("income", Statistic::getIncome(&gs, &ps))
				.integer("towns", int(towns.size())).integer("buildings", buildings).integer("fort", forts)
				.integer("heroes", int(heroes.size())).integer("dwellings", Statistic::getNumberOfDwellings(&ps))
				.integer("mines", mines).integer("army_field", int64_t(heroArmy)).integer("army_towns", int64_t(garrison))
				.integer("exp", exp).integer("max_level", maxLevel);
			if(best)
				tr.str("best", best->getNameTranslated()).integer("best_army", int64_t(bestArmy))
					.integer("best_level", int(best->level)).pos("best_pos", best->visitablePos());
			std::string hs = "[", ts = "[";
			for(const CGHeroInstance * h : heroes)
				if(h)
				{
					if(hs.size() > 1)
						hs += ",";
					hs += JsonObj().str("name", h->getNameTranslated()).pos("pos", h->visitablePos())
						.integer("army", int64_t(h->getArmyStrength())).integer("level", int(h->level)).done();
				}
			for(const CGTownInstance * tw : towns)
				if(tw)
				{
					if(ts.size() > 1)
						ts += ",";
					ts += JsonObj().str("name", tw->getNameTranslated()).pos("pos", tw->visitablePos()).done();
				}
			tr.raw("heroes_list", hs + "]").raw("towns_list", ts + "]");
			decisionLog_->event("truth", tr.done());
		}
		// Every one of our heroes, not only the best army: R7l3c had level
		// 6-10 support heroes beside a level-2 army hero and nothing said
		// what their attack and defense were, which the commander
		// promotion (pickFieldHero) is decided on.
		if(color == playerID)
		{
			std::ostringstream r;
			r << "   ROSTER d" << day;
			for(const CGHeroInstance * h : heroes)
				if(h)
					r << " | " << h->getNameTranslated() << " L" << int(h->level)
					  << " " << h->getPrimSkillLevel(PrimarySkill::ATTACK)
					  << "/" << h->getPrimSkillLevel(PrimarySkill::DEFENSE)
					  << "/" << h->getPrimSkillLevel(PrimarySkill::SPELL_POWER)
					  << "/" << h->getPrimSkillLevel(PrimarySkill::KNOWLEDGE)
					  << " " << uint64_t(h->getArmyStrength()) << " " << roleOf(h);
			decisionLog_->line(r.str());
		}
	}
}

std::string OmniAI::roleOf(const CGHeroInstance * hero) const
{
	if(!hero)
		return "-";
	const int32_t id = hero->id.getNum();
	if(id == fieldHeroId_)
		return "field";
	if(id == secondFieldId_)
		return "second";
	if(id == collectorHeroId_)
		return "collector";
	return "support";
}

std::array<bool, 7> OmniAI::blockedBuildingResources() const
{
	std::array<bool, 7> wanted{};
	if(!cb)
		return wanted;
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;
		const auto * ct = town->getTown();
		if(!ct)
			continue;
		for(const auto & entry : ct->buildings)
		{
			if(!entry.second
				|| entry.second->mode != CBuilding::BUILD_NORMAL
				|| town->hasBuilt(entry.first))
				continue;
			if(cb->canBuildStructure(town, entry.first)
				!= EBuildingState::NO_RESOURCES)
				continue;
			for(int r = 0; r < 7; ++r)
			{
				if(entry.second->resources[GameResID(r)]
					> cb->getResourceAmount(GameResID(r)))
				{
					wanted[r] = true;
				}
			}
		}
	}
	return wanted;
}

void OmniAI::updateResourceScarcity()
{
	if(!cb || !evaluator_)
		return;

	// How much of each resource counts as "enough for now". Below it, things
	// that produce that resource are worth more; at or above it they are
	// worth their face value and no more. Gold is set where it is because a
	// hero that cannot pay for a week's recruits is poor whatever else it
	// owns, and the rare four because nothing early needs many of them.
	static constexpr int ENOUGH[] = {
		20,    // wood
		10,    // mercury
		20,    // ore
		10,    // sulfur
		10,    // crystal
		10,    // gems
		10000  // gold
	};
	// At nothing at all a producer is worth this much more than face value.
	constexpr double AT_ZERO = 4.0;

	std::array<double, 7> mult{};
	std::ostringstream o;
	o << "  short of:";
	bool anyShort = false;
	for(int i = 0; i < 7; ++i)
	{
		const int have = cb->getResourceAmount(GameResID(i));
		const double ratio = std::min(1.0, double(have) / double(ENOUGH[i]));
		mult[i] = 1.0 + (AT_ZERO - 1.0) * (1.0 - ratio);
		if(mult[i] > 1.5)
		{
			anyShort = true;
			o << " " << GameConstants::RESOURCE_NAMES[i] << "(" << have << ")";
		}
	}

	// Identity on top of quantity: a resource a building is waiting on RIGHT
	// NOW earns a further spike even when the raw ratio looks mild (8 ore
	// against a 10-ore ENOUGH bar reads "75% there", nowhere near AT_ZERO,
	// while it is still the one thing standing between the town and its
	// next building). grabScarceMine has carried this distinction for mines
	// since the mercury-vs-ore fix ("nothing in the build list is made of
	// mercury"); the general scoring pass that prices resource PILES never
	// did, so a wood pile and a gem pile scored identically while the fort
	// waited on wood alone. Stacks onto the quantity term rather than
	// replacing it, so a blocked resource we also have none of tops out
	// near 10x face value and a blocked-but-partly-stocked one still reads
	// well above an unrelated resource at the same quantity.
	constexpr double BLOCKED_BUILDING_MULT = 2.5;
	const std::array<bool, 7> blocked = blockedBuildingResources();
	for(int i = 0; i < 7; ++i)
	{
		if(!blocked[i])
			continue;
		mult[i] *= BLOCKED_BUILDING_MULT;
		anyShort = true;
		o << " " << GameConstants::RESOURCE_NAMES[i] << "[blocked]";
	}

	scarcity_ = mult;
	evaluator_->setScarcity(mult);
	if(anyShort)
		decisionLog_->detail(o.str());
}

void OmniAI::tradeForBlockedBuilding()
{
	// Buy what the best building blocked on wood, ore or a rare lacks, when
	// surplus alone can pay. H_duel r01 held 141 wood, 204 ore and 74625
	// idle gold on day 47 while every tier 5-7 dwelling waited on mercury,
	// sulfur, crystal or gems and we owned no rare mine. At one marketplace
	// a rare costs 20 wood or 5000 gold (IMarket::getOffer), dear, but idle
	// resources buy nothing at all. Pays with wood and ore above what the
	// building itself needs plus a kept 20 (provisional, unmeasured), then
	// with surplusGold(); never touches the planner's reserve, never sells a
	// rare. Runs before the build pass so the building can go up today.
	if(!cb)
		return;
	const CGTownInstance * mkt = nullptr;
	for(const CGTownInstance * t : cb->getTownsInfo())
		if(t && t->tempOwner == playerID && t->hasBuilt(BuildingID::MARKETPLACE))
		{
			mkt = t;
			break;
		}
	if(!mkt)
		return;
	const IMarket * market = mkt;
	const TResources have = cb->getResourceAmount();
	constexpr int KEEP_BASIC = 20;
	const GameResID sellers[] = { GameResID(EGameResID::WOOD), GameResID(EGameResID::ORE),
		GameResID(EGameResID::GOLD) };

	struct Trade { GameResID from, to; int amount; };
	// What a surplus gold piece is worth to us: face value while the surplus
	// is small, less as it piles up with nothing to buy. surplusGold() is
	// already past the planner's saving, every creature on sale and a hire,
	// and 10000 is the ENOUGH mark updateResourceScarcity uses for gold.
	// R3_duel r05: 50315 gold idle on day 40 while every tier 5-7 dwelling
	// waited on sulfur, crystal and gems; at one marketplace a rare costs
	// 5000 gold, so pricing the gold at face value refused every trade.
	const double goldUnit = std::min(1.0, 10000.0 / double(std::max(1, surplusGold())));
	const CGTownInstance * bestTown = nullptr;
	BuildingID bestId = BuildingID::NONE;
	double bestRatio = 0.0;
	std::vector<Trade> bestTrades;

	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID || !town->getTown())
			continue;
		for(const auto & entry : town->getTown()->buildings)
		{
			const CBuilding * b = entry.second.get();
			if(!b || b->mode != CBuilding::BUILD_NORMAL || town->hasBuilt(entry.first)
				|| town->forbiddenBuildings.count(entry.first))
				continue;
			if(cb->canBuildStructure(town, entry.first) != EBuildingState::NO_RESOURCES)
				continue;
			TResources shortfall = b->resources - have;
			shortfall.positive();
			if(shortfall[EGameResID::GOLD] > 0)
				continue;   // gold is the planner's saving, not a trade
			TResources spare = have;
			for(int r = 0; r < GameConstants::RESOURCE_QUANTITY; ++r)
				spare[GameResID(r)] = 0;
			spare[EGameResID::WOOD] = std::max(0, int(have[EGameResID::WOOD])
				- int(b->resources[EGameResID::WOOD]) - KEEP_BASIC);
			spare[EGameResID::ORE] = std::max(0, int(have[EGameResID::ORE])
				- int(b->resources[EGameResID::ORE]) - KEEP_BASIC);
			spare[EGameResID::GOLD] = std::max(0, surplusGold()
				- int(b->resources[EGameResID::GOLD]));
			std::vector<Trade> trades;
			double paid = 0.0;   // gold-equivalent given away, at market prices
			bool feasible = true;
			for(int r = 0; r < GameConstants::RESOURCE_QUANTITY && feasible; ++r)
			{
				const GameResID want(r);
				int remaining = shortfall[want];
				for(const GameResID & from : sellers)
				{
					if(remaining <= 0)
						break;
					if(from == want)
						continue;
					int give = 0, get = 0;
					if(!market->getOffer(from.getNum(), want.getNum(), give, get,
						EMarketMode::RESOURCE_RESOURCE) || give <= 0 || get <= 0)
						continue;
					const int lotsNeeded = (remaining + get - 1) / get;
					const int lots = std::min(lotsNeeded, int(spare[from]) / give);
					if(lots <= 0)
						continue;
					trades.push_back({from, want, lots * give});
					spare[from] -= lots * give;
					remaining -= lots * get;
					// What the sold units were worth to us: idle wood or ore
					// only what the market would pay for it in gold (about
					// 25 a unit at one marketplace), surplus gold face value.
					// Pricing idle wood at its 250 base price made a Castle's
					// 6 ore read as 15000 gold paid, and nothing ever traded.
					double unit = goldUnit;
					if(from != GameResID(EGameResID::GOLD))
					{
						int g1 = 0, g2 = 0;
						if(market->getOffer(from.getNum(), GameResID(EGameResID::GOLD).getNum(),
							g1, g2, EMarketMode::RESOURCE_RESOURCE) && g1 > 0)
							unit = double(g2) / double(g1);
					}
					paid += double(lots * give) * unit;
				}
				if(remaining > 0)
					feasible = false;
			}
			if(!feasible || trades.empty())
				continue;
			const double value = buildingValue(town, entry.first)
				+ fortGrowthValue(town, entry.first);
			const double ratio = value / std::max(1.0, paid);
			if(ratio > bestRatio)
			{
				bestRatio = ratio;
				bestTown = town;
				bestId = entry.first;
				bestTrades = trades;
			}
		}
	}
	// Worth doing only when the building repays what leaves the purse; the
	// resources were idle, so parity is enough.
	if(!bestTown || bestRatio < 1.0)
		return;
	for(const Trade & tr : bestTrades)
	{
		decisionLog_->line("  TRADE " + std::to_string(tr.amount) + " "
			+ GameConstants::RESOURCE_NAMES[tr.from.getNum()] + " for "
			+ GameConstants::RESOURCE_NAMES[tr.to.getNum()] + " toward building "
			+ std::to_string(bestId.getNum()) + " in " + bestTown->getNameTranslated());
		cb->trade(mkt->id, EMarketMode::RESOURCE_RESOURCE, tr.from, tr.to, tr.amount);
	}
}

void OmniAI::tradeForWhatIsMissing()
{
	if(!cb)
		return;

	// Wood and ore are what every early building is made of, and gold is the
	// one thing this AI reliably accumulates. A marketplace rate is poor, and
	// it is still better than a stalled build queue: measured, the AI reached
	// day 6, ran out of wood, stopped building for the rest of the game, and
	// finished with 16198 gold it had no way to spend.
	constexpr int GOLD_TO_KEEP = 4000;   // recruitment comes first
	constexpr int MIN_SELL = 500;
	constexpr int MAX_SELL = 50000;      // one turn cannot empty the treasury
	constexpr int ENOUGH = 15;           // stop once a build is affordable again

	const int gold = cb->getResourceAmount(EGameResID::GOLD);
	const int spare = gold - GOLD_TO_KEEP - buildReserveTotal();
	if(spare < MIN_SELL)
		return;

	// Scale with how rich we are. The marketplace rate for gold is poor by
	// design, so a flat thousand a turn buys nothing: a measured run sat on
	// 21350 gold and 4 wood at day 20, still trading a thousand at a time.
	const int wantToSell = std::min(MAX_SELL, std::max(MIN_SELL, spare / 2));

	// A mine we already own delivers the resource without paying the
	// marketplace's poor rate, so trading gold for it is redundant. Only
	// consider a resource we have no mine income for at all - otherwise the
	// answer to a shortfall is grabScarceMine, not the treasury. Measured on
	// Twins: 71 trades burned 207500 gold for 83 wood and ore at 2500:1.
	std::array<bool, 7> mined{};
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		if(!obj || obj->ID != Obj::MINE || obj->tempOwner != playerID)
			continue;
		const auto * mine = dynamic_cast<const CGMine *>(obj);
		if(!mine)
			continue;
		const int res = mine->producedResource.getNum();
		if(res >= 0 && res < int(mined.size()))
			mined[res] = true;
	}

	const EGameResID wanted[] = { EGameResID::WOOD, EGameResID::ORE };
	EGameResID target = EGameResID::WOOD;
	int lowest = ENOUGH;
	for(const EGameResID r : wanted)
	{
		const int have = cb->getResourceAmount(r);
		if(have < lowest)
		{
			lowest = have;
			target = r;
		}
	}
	if(lowest >= ENOUGH)
		return;
	// The scarcest is already being mined: wait for it, do not buy it. If it
	// is the other resource we cannot mine, trade for that one instead.
	if(mined[target.getNum()])
	{
		const EGameResID other =
			(target == EGameResID::WOOD) ? EGameResID::ORE : EGameResID::WOOD;
		const int otherHave = cb->getResourceAmount(other);
		if(otherHave < ENOUGH && !mined[other.getNum()])
		{
			target = other;
			lowest = otherHave;
		}
		else
		{
			decisionLog_->detail(std::string("no trade: ")
				+ (target == EGameResID::WOOD ? "wood" : "ore")
				+ " already has a mine feeding it, gold is better spent on troops");
			return;
		}
	}

	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || !town->hasBuilt(BuildingID::MARKETPLACE))
			continue;

		// The server refuses any amount that is not a whole number of base
		// quantities, and refuses it silently as far as the AI is concerned:
		// 309 trades in one match, every one rejected, wood still at 1.
		// CGTownInstance is an IMarket, so ask it for the rate rather than
		// assuming one.
		const IMarket * market = town;
		int b1 = 0;   // gold we must give
		int b2 = 0;   // units we get back for it
		if(!market->getOffer(GameResID(EGameResID::GOLD).getNum(),
			GameResID(target).getNum(), b1, b2, EMarketMode::RESOURCE_RESOURCE)
			|| b1 <= 0 || b2 <= 0)
			continue;

		const int lots = wantToSell / b1;
		if(lots <= 0)
			continue;               // cannot afford one whole lot yet
		const int sell = lots * b1;

		std::ostringstream o;
		o << "  TRADE " << sell << " gold for " << (lots * b2) << " "
		  << (target == EGameResID::WOOD ? "wood" : "ore")
		  << " at " << town->getNameTranslated()
		  << " (rate " << b1 << ":" << b2 << ", have " << lowest
		  << ", gold " << gold << ")";
		decisionLog_->line(o.str());
		cb->trade(town->id, EMarketMode::RESOURCE_RESOURCE,
			GameResID(EGameResID::GOLD), GameResID(target), sell);
		return; // one trade a turn is plenty
	}
}

void OmniAI::assignHeroRoles()
{
	// Recomputed every round rather than assigned once, because the answer
	// moves: a home hero fed by its town all week can become the stronger of
	// the two, and at that point it is the one that should be going out.
	//
	// Two guards on "strongest wins", both from measured failures. The
	// incumbent keeps the job while it lives unless plainly outclassed, so a
	// field hero that comes home to collect is not demoted mid-errand. And a
	// hero standing in a town we own is not a candidate for a fresh pick: on
	// hiring day the fresh recruit had just been bought the week's growth,
	// outclassed the actual field hero, and took its job, so the courier went
	// to war while the field hero tried to walk home through fog it could
	// not cross and died on day 15.
	const CGHeroInstance * incumbent = nullptr;
	if(cb && fieldHeroId_ >= 0)
		incumbent = cb->getHero(ObjectInstanceID(fieldHeroId_));
	fieldHeroId_ = -1;
	if(!cb)
		return;

	auto inOurTown = [this](const CGHeroInstance * h)
	{
		for(const CGTownInstance * town : cb->getTownsInfo())
		{
			if(!town || town->tempOwner != playerID)
				continue;
			if(town->getVisitingHero() == h || town->getGarrisonHero() == h)
				return true;
		}
		return false;
	};

	uint64_t best = 0;
	const CGHeroInstance * pick = nullptr;
	for(const CGHeroInstance * h : cb->getHeroesInfo())
	{
		if(!h || h == incumbent || inOurTown(h))
			continue;
		if(!pick || h->getArmyStrength() > best)
		{
			best = h->getArmyStrength();
			pick = h;
		}
	}
	if(!pick)
	{
		// Every hero is standing in a town. The role still has to be
		// somebody's; take the strongest on the board.
		for(const CGHeroInstance * h : cb->getHeroesInfo())
		{
			if(!h || h == incumbent)
				continue;
			if(!pick || h->getArmyStrength() > best)
			{
				best = h->getArmyStrength();
				pick = h;
			}
		}
	}

	// Plainly stronger means half again, not a troop or two: below that the
	// roles would swap back and forth as the weekly buy moves the balance.
	if(incumbent
		&& (!pick || double(pick->getArmyStrength())
			< double(incumbent->getArmyStrength()) * 1.5))
		pick = incumbent;

	// The fighting should be done by the hero whose own attack and defense
	// multiply an army most, and the army comes to it: the couriers already
	// walk every spare stack to the field hero, the old one included. Picked
	// by army alone, R6z3c mg72s7 had a level-10 hero on day 21 running
	// errands while the level-1 field hero held the army (a level-5 one in
	// the other game); Nullkiller's main was level 11-14. Clearly better is
	// 10% more hero quality (below), about four points of attack and defense
	// together, so the role does not swap on one level-up. A town's only
	// holder keeps its post.
	// The measure is the engine's getHeroStrength (fighting strength times
	// the caster's power and knowledge term) without its mana scaling, so a
	// commander that has just cast its mana away does not lose the role for
	// a day. R7l3c's level 6-10 support heroes gained mostly power and
	// knowledge, which attack and defense alone never saw.
	auto quality = [](const CGHeroInstance * h)
	{
		double q = h->getFightingStrength();
		if(h->hasSpellbook())
		{
			bool combat = false;
			for(const SpellID & sp : h->getSpellsInSpellbook())
				if(sp.toSpell() && sp.toSpell()->isCombat())
				{
					combat = true;
					break;
				}
			if(combat)
				q *= std::sqrt((1.0 + 0.05 * h->getPrimSkillLevel(PrimarySkill::KNOWLEDGE))
					* (1.0 + 0.05 * h->getPrimSkillLevel(PrimarySkill::SPELL_POWER)));
		}
		return q;
	};
	if(pick)
	{
		const CGHeroInstance * commander = nullptr;
		double bar = quality(pick) * 1.10;
		for(const CGHeroInstance * h : cb->getHeroesInfo())
		{
			if(!h || h == pick || isSoleHolder(h))
				continue;
			if(quality(h) > bar)
			{
				bar = quality(h);
				commander = h;
			}
		}
		if(commander)
		{
			if(decisionLog_ && commander != incumbent)
				decisionLog_->line("  COMMANDER " + commander->getNameTranslated()
					+ " (level " + std::to_string(commander->level) + ", army "
					+ std::to_string(uint64_t(commander->getArmyStrength()))
					+ ") takes the field from " + pick->getNameTranslated()
					+ " (level " + std::to_string(pick->level) + ", army "
					+ std::to_string(uint64_t(pick->getArmyStrength())) + ")");
			pick = commander;
		}
	}

	if(pick)
		fieldHeroId_ = pick->id.getNum();

	// The burner is the cheapest spare pair of boots: not the field hero,
	// and not the only hero holding a town right now, because a post that
	// empties to go collecting is the failure this whole split exists to
	// prevent. Everything else on the roster couriers.
	collectorHeroId_ = -1;
	const CGHeroInstance * weakest = nullptr;
	for(const CGHeroInstance * h : cb->getHeroesInfo())
	{
		if(!h || h->id.getNum() == fieldHeroId_ || isSoleHolder(h))
			continue;
		if(!weakest || h->getArmyStrength() < weakest->getArmyStrength())
			weakest = h;
	}
	if(weakest)
		collectorHeroId_ = weakest->id.getNum();

	// A second field-capable hero, when the roster is deep enough to spare
	// one: the second-strongest hero that is not already the field hero or
	// the collector and is not the only defender of a town. On an open map
	// one hero cannot hold a captured town and keep pressing at the same
	// time, so the raider takes the captures the field hero cannot reach and
	// holds a front. Below four heroes there is nobody free to be it.
	secondFieldId_ = -1;
	// Only while we are ahead. A second fighter keeps its own army, and two
	// halves lose to one whole: J3 MapGen 72x72 s17 day 25 had our 41k spread
	// over 8 heroes (best 17.9k, a second fighter near it) against
	// Nullkiller's 33.5k main of 62k. Behind or level, every support hero
	// couriers to the one field hero instead (Nullkiller gathers onto its
	// main, GatherArmyBehavior).
	if(cb->getHeroesInfo().size() >= 4 && attacking_)
	{
		const CGHeroInstance * second = nullptr;
		uint64_t secondBest = 0;
		for(const CGHeroInstance * h : cb->getHeroesInfo())
		{
			if(!h || h->id.getNum() == fieldHeroId_
				|| h->id.getNum() == collectorHeroId_ || isSoleHolder(h))
				continue;
			if(!second || h->getArmyStrength() > secondBest)
			{
				secondBest = h->getArmyStrength();
				second = h;
			}
		}
		if(second)
			secondFieldId_ = second->id.getNum();
	}
}

bool OmniAI::isSoleHolder(const CGHeroInstance * hero) const
{
	if(!cb || !hero)
		return false;
	// In one of our towns, and no other non-field hero shares the post.
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;
		if(town->getVisitingHero() != hero && town->getGarrisonHero() != hero)
			continue;
		bool shared = false;
		for(const CGHeroInstance * other : cb->getHeroesInfo())
		{
			if(!other || other == hero || other->id.getNum() == fieldHeroId_)
				continue;
			if(town->getVisitingHero() == other || town->getGarrisonHero() == other)
				shared = true;
		}
		if(!shared)
			return true;
	}
	return false;
}

// A support hero can be spared to hold a new post when it is not the only
// wall a town has: either it holds nothing, or the town it holds repels the
// threat on it with the garrison alone and does not need the hero.
bool OmniAI::defenderCanBeSpared(const CGHeroInstance * hero) const
{
	if(!cb || !hero || hero->id.getNum() == fieldHeroId_
		|| hero->id.getNum() == collectorHeroId_)
		return false;
	const CGTownInstance * held = nullptr;
	for(const CGTownInstance * t : cb->getTownsInfo())
		if(t && t->tempOwner == playerID
			&& (t->getVisitingHero() == hero || t->getGarrisonHero() == hero))
			held = t;
	if(!held)
		return true;   // a free support hero
	const CGHeroInstance * foe = strongestThreatNear(held);
	return !foe || defenseHolds(nullptr, held, foe, hero);
}

// A capture only converts if a defender can be seated on the taken town.
// That is a support hero that can be spared, or the gold plus a town to buy
// one from - so a Capture task does not fire into a town we could not hold
// the day after.
bool OmniAI::defenderAvailable() const
{
	if(!cb)
		return false;
	for(const CGHeroInstance * h : cb->getHeroesInfo())
		if(defenderCanBeSpared(h))
			return true;
	return cb->getResourceAmount(EGameResID::GOLD) >= GameConstants::HERO_GOLD_COST
		&& !cb->getTownsInfo().empty();
}

double OmniAI::townDefenseStrength(const CGTownInstance * town) const
{
	if(!town)
		return 0.0;
	double defense = double(town->getArmyStrength());
	if(const CGHeroInstance * g = town->getGarrisonHero())
		defense += double(g->getArmyStrength());
	if(const CGHeroInstance * v = town->getVisitingHero())
		if(v->tempOwner != playerID)
			defense += double(v->getArmyStrength());
	return defense;
}

omniai::CombatVerdict OmniAI::estimateFight(const CGHeroInstance * hero,
	const CArmedInstance * defender, const CGHeroInstance * defHero, int fortLevel) const
{
	omniai::CombatEvaluator::Side atk =
		omniai::CombatEvaluator::profile(hero, hero, 0);
	omniai::CombatEvaluator::Side def =
		omniai::CombatEvaluator::profile(defender, defHero, fortLevel);
	return omniai::CombatEvaluator::estimate(atk, def);
}

// The army that actually shows up to a fight the field hero starts: its own
// stacks plus what deliverable couriers hand over, all fighting under the
// field hero's stats. Mirrors conquestStrength so a siege we commit to is
// the siege we can win.
omniai::CombatEvaluator::Side OmniAI::fieldArmyProfile(const CGHeroInstance * field) const
{
	using CE = omniai::CombatEvaluator;
	CE::Side atk = CE::profile(field, field, 0);
	if(!cb)
		return atk;
	for(const CGHeroInstance * h : cb->getHeroesInfo())
	{
		if(!h || h == field || h->tempOwner != playerID)
			continue;
		if(h->id.getNum() == collectorHeroId_ || isSoleHolder(h))
			continue;
		auto hp = pathCache_ ? pathCache_->getPathsInfo(h) : nullptr;
		if(!hp)
			continue;
		if(stepsToward(const_cast<CPathsInfo *>(hp.get()), h, field->visitablePos()).empty())
			continue;
		CE::absorb(atk, h, field, 0);
	}
	return atk;
}

double OmniAI::townTowerDps(const CGTownInstance * town) const
{
	// Arrow towers were priced only as a +0-3 flat defense bump on every
	// defending creature (the siege discount CombatEvaluator::absorb already
	// applies) - the guaranteed per-round damage the towers themselves deal
	// was not modeled at all, so a siege against a built-up Castle read as
	// barely harder than one against an empty Fort. Engine ground truth
	// (CGTownInstance::getTowerDamageRange/getKeepDamageRange, verified
	// against DamageCalculator.cpp and BattleInfo.cpp's siege setup, not the
	// research note's approximate figures): each tower rolls
	// baseDamage+extraDamage*townLevel to 2x that, once a round,
	// unconditional on attacker/defender stats. Central (keep) is
	// 10+2/building; the two side towers are 6+1/building each; only the
	// towers the current fortification level actually grants take part.
	// Expected value (the 1.5x factor) folds into towerDps, the same
	// per-round-unconditional bucket magicDps already uses for a hero's
	// spell output, so it feeds the same estimate() math for free. Shared
	// by estimateSiege (are THEIR walls worth attacking) and defenseHolds
	// (do OUR walls hold) - a town's own towers fire for whichever side is
	// defending it, so both call sites need the identical number.
	if(!town)
		return 0.0;
	const auto fortHealth = town->fortificationsLevel();
	const int townLevel = town->getTownLevel();
	double towerDps = 0.0;
	if(fortHealth.citadelHealth != 0)
		towerDps += 1.5 * (10 + 2 * townLevel);
	if(fortHealth.upperTowerHealth != 0)
		towerDps += 1.5 * (6 + townLevel);
	if(fortHealth.lowerTowerHealth != 0)
		towerDps += 1.5 * (6 + townLevel);
	return towerDps;
}

omniai::CombatVerdict OmniAI::estimateSiege(const CGHeroInstance * hero,
	const CGTownInstance * town, bool withCouriers) const
{
	using CE = omniai::CombatEvaluator;
	// Couriers only for a plan: R6h_duel r01 day 29 the fence priced the
	// siege of Kanan (a Castle with Sylvia, 16896, inside) with three
	// support heroes 10-14 steps away counted in (126 + 4150 + 8432), called
	// it a win for the field hero's 33731, and the field hero died alone at
	// the walls - from a 47k to 25k lead on day 28 to a lost game.
	CE::Side atk = withCouriers ? fieldArmyProfile(hero) : CE::profile(hero, hero, 0);
	CE::Side def;
	if(!town)
		return CE::estimate(atk, def);
	const int fort = int(town->fortLevel());
	const CGHeroInstance * garrisonHero = town->getGarrisonHero();
	// The garrison troops fight under the garrison hero's command, so they
	// take its stats; the hero's own carried army and a hostile visitor's
	// each add their own stacks on top.
	CE::absorb(def, town, garrisonHero, fort);
	if(garrisonHero)
		CE::absorb(def, garrisonHero, garrisonHero, fort);
	if(const CGHeroInstance * v = town->getVisitingHero())
		if(v->tempOwner != playerID)
			CE::absorb(def, v, v, fort);
	def.towerDps = townTowerDps(town);
	def.fortLevel = fort;

	// Calibration of our own sieges, measured on the nine R6y_duel sieges
	// tagged "[siege attack fort 3]": the towers took about 12% of our army
	// even where the model saw no loss at all (three near-empty Castles:
	// predicted 1.00, kept 0.94, 0.79, 0.94), and the rest ran about three
	// times the predicted loss (0.92 predicted, 0.08 kept; 0.84, 0.43; 0.68,
	// 0.05; one predicted win lost outright). So a Castle costs 0.12 plus
	// three times the model's loss. No attack on a fort of level 1-2 has
	// been logged yet; those take the field-fight factor (1.6) until they
	// have been. The floor keeps a costly win apart from a loss.
	// Below a Castle, a town with an enemy hero in it is a hero fight and
	// takes the hero-fight factor (2.8, combatVerdict's), not the neutral
	// one: R7p_duel r24 day 5 attacked our own lost capital (fort 0) with
	// Nullkiller's Adelaide inside at "keeps 25%" and lost everything
	// (odds 0.81), the raw model having read keeps 53%.
	const CGHeroInstance * visitor = town->getVisitingHero();
	const bool heroInside = garrisonHero || (visitor && visitor->tempOwner != playerID);
	omniai::CombatVerdict v = CE::estimate(atk, def);
	if(v.win)
	{
		const double loss = 1.0 - v.margin;
		const double real = fort >= 3 ? 0.12 + 3.0 * loss
			: (heroInside ? 2.8 : 1.6) * loss;
		v.margin = std::max(0.05, 1.0 - real);
	}
	return v;
}

bool OmniAI::lastKnownTownOf(const CGTownInstance * town) const
{
	// R6x r02 and R6y r17, the two duel-v2 wins of September 24th, were both
	// won by taking the enemy's only town: seven days later the owner is out
	// of the game. As one more producer the town had to clear the usual
	// siege margin, and at the corrected Castle price few do; as the last
	// town it is the game. K's "capital first" in its attacking sense.
	if(!town || !town->tempOwner.isValidPlayer() || town->tempOwner == playerID)
		return false;
	int known = 0;
	for(const auto & kv : worldObjs_)
		if(kv.second.kind == WorldObj::Kind::Town
			&& kv.second.owner == town->tempOwner.getNum())
			++known;
	return known <= 1;
}

double OmniAI::conquestStrength(const CGHeroInstance * field) const
{
	if(!cb || !field)
		return 0.0;
	// The field hero's own army, plus what couriers that can reach it right
	// now are carrying, discounted for the stack they keep and the walk.
	double total = double(field->getArmyStrength());
	for(const CGHeroInstance * h : cb->getHeroesInfo())
	{
		if(!h || h == field || h->tempOwner != playerID)
			continue;
		if(h->id.getNum() == collectorHeroId_ || isSoleHolder(h))
			continue;   // the burner loops and a sole holder keeps its post
		auto hp = pathCache_ ? pathCache_->getPathsInfo(h) : nullptr;
		if(!hp)
			continue;
		if(stepsToward(const_cast<CPathsInfo *>(hp.get()), h, field->visitablePos()).empty())
			continue;   // cannot hand over; its strength stays home
		total += double(h->getArmyStrength()) * 0.7;
	}
	return total;
}

// How far a hero sees, and the reveal a frontier tile has to promise before
// it is worth a day's movement. Both match exploreFrontier, which is where
// they were measured.
static constexpr int SCOUT_SIGHT = 4;
static constexpr int SCOUT_WORTH_A_DAY = 6;

// How many move DECISIONS the field hero will spend walking at one frontier
// tile before picking again. Decisions and not game days: runTurn loops
// rounds while heroes still have movement (OmniAI.cpp:462), so moveBestHero
// runs several times a day and this budget is spent faster than the word
// turn suggests. Commitment is the point, so it is not small; the bound
// exists so a target behind a wall cannot own the hero for the game.
static constexpr int SCOUT_BUDGET = 8;

// How many move decisions a committed resupply march gets. The walk to a
// sealed town is a multi-day trip, so it is longer than the scout budget;
// progress is also self-checking, since a round that cannot get any closer
// drops the march without spending the rest of the budget.
static constexpr int RESUPPLY_BUDGET = 40;

bool OmniAI::unseenCentroid(int z, int3 & out) const
{
	if(!cb)
		return false;
	const int3 sizes = cb->getMapSize();
	int64_t sx = 0, sy = 0, n = 0;
	for(int x = 0; x < sizes.x; ++x)
		for(int y = 0; y < sizes.y; ++y)
		{
			const int3 q(x, y, z);
			if(!cb->isVisible(q))
			{
				sx += x; sy += y; ++n;
			}
		}
	if(n == 0)
		return false;
	out = int3(int(sx / n), int(sy / n), z);
	return true;
}

bool OmniAI::frontierLeft(const CGHeroInstance * hero, CPathsInfo * paths) const
{
	// Flat fog is not an exhausted frontier when the hero was the one not
	// moving: the frontier-dry tripwire sent a stuck field hero home, home
	// was blocked, and the hero never moved again, which kept the fog flat.
	// Ask the map instead: is any tile this hero reaches still worth a day?
	if(!cb || !hero || !paths)
		return false;
	const int3 sizes = cb->getMapSize();
	const int z = hero->visitablePos().z;
	for(int y = 0; y < sizes.y; ++y)
		for(int x = 0; x < sizes.x; ++x)
		{
			const int3 p(x, y, z);
			const CGPathNode * n = paths->getNode(p, EPathfindingLayer::LAND);
			if(!n || !n->reachable() || n->accessible != EPathAccessibility::ACCESSIBLE)
				continue;
			if(revealFrom(p) >= SCOUT_WORTH_A_DAY)
				return true;
		}
	return false;
}

int OmniAI::revealFrom(const int3 & p) const
{
	if(!cb)
		return 0;
	int reveal = 0;
	for(int dx = -SCOUT_SIGHT; dx <= SCOUT_SIGHT; ++dx)
		for(int dy = -SCOUT_SIGHT; dy <= SCOUT_SIGHT; ++dy)
		{
			const int3 q(p.x + dx, p.y + dy, p.z);
			if(cb->isInTheMap(q) && !cb->isVisible(q))
				++reveal;
		}
	return reveal;
}

bool OmniAI::scoutMarch(const CGHeroInstance * hero, CPathsInfo * paths)
{
	if(!cb || !hero || !paths || !scoutTargetSet_)
		return false;

	auto drop = [&](const char * why) {
		scoutTargetSet_ = false;
		scoutTurnsLeft_ = 0;
		decisionLog_->detail(std::string("scout target dropped: ") + why);
		return false;
	};

	if(hero->visitablePos() == scoutTarget_)
		return drop("arrived");
	if(--scoutTurnsLeft_ <= 0)
		return drop("budget spent");
	// Somebody else's movement may have lifted the fog this tile was for.
	if(revealFrom(scoutTarget_) < SCOUT_WORTH_A_DAY)
		return drop("nothing left to uncover there");

	// A hostile hero stronger than us may not have been anywhere near when
	// this target was first picked - exploreFrontier's own danger discount
	// only judges the choice at selection time, and a multi-day march does
	// not re-check the ground it is crossing on later days. This is the
	// original mechanism the danger-aware scouting fix started from: a
	// field hero mid multi-turn "SCOUT ON" walk was caught by an enemy
	// hero that had grown large nearby in the meantime (DEVELOPMENT_LOG.md).
	// Same radius exploreFrontier's dangerDiscount already uses, so a
	// target does not get picked under one rule and then kept under a
	// looser one for the rest of its budget.
	const double ourStrength = double(hero->getArmyStrength());
	const int3 hp = hero->visitablePos();
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
		if(!e || !e->tempOwner.isValidPlayer() || e->tempOwner == playerID
			|| double(e->getArmyStrength()) <= ourStrength)
			continue;
		const int3 ep = e->visitablePos();
		if(ep.z != hp.z)
			continue;
		const double dist = std::sqrt(double(
			(hp.x - ep.x) * (hp.x - ep.x) + (hp.y - ep.y) * (hp.y - ep.y)));
		if(dist < 18.0)
			return drop("a stronger enemy hero closed in nearby");
	}

	std::vector<int3> steps = stepsToward(paths, hero, scoutTarget_);
	if(steps.empty())
		return drop("no path");

	std::ostringstream o;
	o << "   SCOUT ON toward " << scoutTarget_.toString() << ", "
	  << steps.size() << " steps, " << scoutTurnsLeft_ << " turns left";
	decisionLog_->line(o.str());
	return walk(hero, steps);
}

bool OmniAI::knowsHostileTown() const
{
	if(!cb)
		return false;
	// Neutral counts. The question this answers is whether there is a gate
	// anywhere worth walking to, and an unowned town is one.
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * town = dynamic_cast<const CGTownInstance *>(obj);
		if(town && town->tempOwner != playerID)
			return true;
	}
	return false;
}

// The other half of the offense lever. When every reachable frontier is
// cleared and still no hostile town is in sight, the enemy is sealed off and
// the field hero has been clearing its own pocket's last fog while their
// army compounds unseen. A field hero strong enough to take a capital should
// push the seal toward the bulk of the unseen instead - on a two-player map
// that mass is the enemy's side. pressToward treats a beatable choke guard
// as a legal target, so the press either opens the pocket or stalls at the
// boundary, and once the enemy capital is revealed conquestMarch takes over.
//
// The floor keeps a fresh hero home: below it the seal guard the enemy is
// hiding behind is usually too strong to beat, and pressing out just feeds
// the field hero to their scouting stack. Measured on duel-v2 the seal guard
// and early enemy heroes run in the low thousands, so 9000 is the point
// where the hero is plausibly the strongest thing it can meet this week.
bool OmniAI::huntTheEnemy(const CGHeroInstance * hero, CPathsInfo * paths)
{
	if(!cb || !hero || !paths)
		return false;
	constexpr double HUNT_MIN_ARMY = 9000.0;
	if(double(hero->getArmyStrength()) < HUNT_MIN_ARMY)
		return false;
	// On a map small enough for the leash (one town, 48 tiles or less) the
	// press can only walk to the leash's edge, which on duel-v2 is where the
	// level-6 guards stand, and it walked there on day 2 of 4 of the 8 Q_duel
	// games so far while Nullkiller's main was two days out. Wait for the
	// posture instead: attacking_ is what releases the leash anyway.
	if(!attacking_ && leashBinds(hero))
		return false;

	int3 target;
	std::string label;
	const int3 from = hero->visitablePos();
	if(enemyBeaconValid_)
	{
		// We have seen where the enemy lives: press through the last-seen
		// enemy hero and keep going in that direction to the far edge, which
		// on a two-player map is where their capital sits. The press picks the
		// closest reachable tile toward it, so it routes through the choke the
		// enemy uses instead of clearing our own fog.
		const int3 sizes = cb->getMapSize();
		const double dx = double(enemyBeacon_.x - from.x);
		const double dy = double(enemyBeacon_.y - from.y);
		const double len = std::sqrt(dx * dx + dy * dy);
		// Standing on the beacon gives no direction at all; and the old
		// "overshoot then clamp each axis" collapsed to a map corner
		// whenever the ray exited one edge first - Twins produced
		// (96 -7 1) -> clamped to a corner, and the press marched a 259k
		// stack at nothing. Take the true ray-box exit instead: the point
		// where the ray through the beacon leaves the map.
		if(len < 4.0)
		{
			if(!unseenCentroid(from.z, target))
				return false;
			label = "   HUNT toward the unseen bulk";
		}
		else
		{
			// Ray-box exit in unit direction: the first axis boundary the
			// ray through the beacon crosses. Exit = from + u * t.
			const double ux = dx / len, uy = dy / len;
			double t = std::numeric_limits<double>::max();
			if(ux > 0.0)
				t = std::min(t, (sizes.x - 1 - from.x) / ux);
			else if(ux < 0.0)
				t = std::min(t, -from.x / ux);
			if(uy > 0.0)
				t = std::min(t, (sizes.y - 1 - from.y) / uy);
			else if(uy < 0.0)
				t = std::min(t, -from.y / uy);
			if(t == std::numeric_limits<double>::max())
				t = 0.0;   // degenerate: hero already on a boundary row
			target = int3(
				std::clamp(int(from.x + ux * t), 0, sizes.x - 1),
				std::clamp(int(from.y + uy * t), 0, sizes.y - 1),
				std::clamp(from.z, 0, sizes.z - 1));
			label = "   HUNT the enemy's side past " + enemyBeacon_.toString();
		}
	}
	else
	{
		if(!unseenCentroid(from.z, target))
			return false;
		label = "   HUNT toward the unseen bulk";
	}
	// Saturation: the press keeps picking the same boundary tile with no
	// guard registered, day after day. The seal is not going to open, and
	// camping it is what left the pocket's town empty when the enemy came
	// through on 108x108_s23. Park the stack on the town instead; the
	// counter resets the moment a different tile or a blocker shows up.
	if(sealedPressHold(hero, paths))
		return true;
	const CArmedInstance * blocker = nullptr;
	int3 tile;
	const bool moved = pressToward(hero, paths, target,
		label + " at " + target.toString(), &blocker, &tile);
	// The route is sealed by a guard we cannot beat today. Register it as a
	// deferred task - "wait until the field army clears it" - and let the
	// hero go back to compounding. The task re-enters the moment the
	// evaluator says we win, which is the growth-parity mechanism working
	// toward a concrete purpose rather than an abstract rate.
	if(blocker)
		deferGuard(blocker);
	notePress(hero, blocker, moved, tile);
	return moved;
}

// Support-hero counterpart to the field hero's huntTheEnemy. Nullkiller
// fields dedicated scouts (its digest shows Gwenneth/Rissa sweeping the
// map while the main marches on our capital), and the consequence of us
// never scouting is concrete: m008 never found Claxton while Christian
// marched on Castellatus from day 2 - the enemy's capital was simply
// never in our object set, so conquestMarch had nothing to target. A
// support hero with no task and no courier load walks toward the unseen
// bulk instead of standing in a doorway. One hero holds the job per day
// (deepScoutDay_/deepScouts_) so the whole support pool does not walk
// the same centroid; walk()'s fencing keeps it out of known fights.
bool OmniAI::deepScout(const CGHeroInstance * hero, CPathsInfo * paths)
{
	if(!cb || !hero || !paths)
		return false;
	// The job is finding their capital. Once one is known, scouting for
	// its own sake is movement a collector or holder could use - except on
	// a big map still mostly unseen, where the towns and mines worth taking
	// are out there too (scoutingDue).
	const bool bigUnseen = scoutingDue();
	if(knowsHostileTown() && !bigUnseen)
		return false;
	if(hero->id.getNum() == fieldHeroId_ || hero->id.getNum() == secondFieldId_)
		return false;   // fighters scout through their own press
	// A hero seated on a town is holding a post, not idle: walking it off
	// leaves the walls unmanned, and the one-hero-holds-one-town pattern
	// is what keeps a taken town taken.
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(town && town->tempOwner == playerID
			&& (town->getVisitingHero() == hero || town->getGarrisonHero() == hero))
			return false;
	}
	if(isSoleHolder(hero))
		return false;
	const int today = cb->getDate(Date::DAY);
	if(today != deepScoutDay_)
	{
		deepScoutDay_ = today;
		deepScouts_.clear();
	}
	const int32_t me = hero->id.getNum();
	const bool already = deepScouts_.count(me) != 0;
	// Two scouts a day on a big map still mostly unseen, each in its own
	// sector; one otherwise.
	if(!already && deepScouts_.size() >= size_t(bigUnseen ? 2 : 1))
		return false;   // today's scouts are already out
	// Aim at the enemy's side when we know where it is: the seeded beacon
	// is their start town, the way a human scouts toward the other corner.
	// The unseen centroid is the fallback for maps with no declared start.
	int3 target;
	const char * why;
	// On a map with a lot of water in sight, the crossing waits on one
	// question: does the land route seekCrossing still hopes for exist? That
	// route leaves what we have seen at crossingLookAt_, so look there first.
	// The R7g Islands game waited 33 days for the coastline to be seen by
	// chance before its crossing was due; Nullkiller was at sea on day 31.
	// A tenth of the seen tiles being water keeps this off land maps with a
	// lake, where the same tile is only the way toward the enemy.
	if(crossingLookDay_ >= today - 1 && crossingLookAt_.z == hero->visitablePos().z
		&& cb->isInTheMap(crossingLookAt_) && !cb->isVisible(crossingLookAt_)
		&& seenWaterShare_ >= 0.10)
	{
		target = crossingLookAt_;
		why = "where a land route across would leave what we have seen, at";
	}
	else if(enemyBeaconValid_ && !bigUnseen)
	{
		target = enemyBeacon_;
		why = "the enemy's side at";
	}
	else
	{
		// A big map, mostly unseen: scout by sector round our main town, not
		// toward the centroid of everything unseen, which always lies toward
		// the middle of the map. R7l3c mg72s7 blue: the neutral town 31 tiles
		// due west of our start was never seen; field hero and scout both
		// went north-west for the centroid (33 33 0), and Nullkiller, which
		// sees the map, owned the town on day 7.
		int sector = -1;
		if(bigUnseen)
		{
			std::set<int> taken;
			for(const auto & kv : deepScouts_)
				if(kv.first != me && kv.second >= 0)
					taken.insert(kv.second);
			const int forced = already ? deepScouts_[me] : -1;
			if(!scoutSector(hero->visitablePos().z, taken, forced, target, sector))
				sector = -1;
		}
		if(sector >= 0)
		{
			deepScouts_[me] = sector;
			why = "unseen ground round home at";
		}
		else
		{
			if(!unseenCentroid(hero->visitablePos().z, target))
				return false;
			why = "the unseen bulk at";
		}
	}
	std::vector<int3> steps = stepsToward(paths, hero, target);
	// A target in fog has no route, so this used to return empty-handed
	// most days: seven scouting orders in the whole of R6z3c's first 72x72
	// game, and 10% of the map seen by week four. Walk the route through
	// the fog instead, the way the mine probe does.
	if(steps.empty())
	{
		int3 frontier;
		if(fogFrontierToward(hero, paths, target, frontier))
			steps = stepsToward(paths, hero, frontier);
	}
	if(steps.empty())
		return false;
	if(!deepScouts_.count(me))
		deepScouts_[me] = -1;
	decisionLog_->line(std::string("   DEEP SCOUT toward ") + why + " "
		+ target.toString() + ", " + std::to_string(steps.size()) + " steps");
	return walk(hero, steps);
}

bool OmniAI::scoutSector(int z, const std::set<int> & taken, int forced,
	int3 & out, int & sector) const
{
	if(!cb)
		return false;
	// Round the first town we own; a player with no town has no home to
	// scout round.
	int3 origin(-1, -1, -1);
	for(const CGTownInstance * town : cb->getTownsInfo())
		if(town && town->tempOwner == playerID && town->visitablePos().z == z)
		{
			origin = town->visitablePos();
			break;
		}
	if(origin.z < 0)
		return false;
	constexpr int NEAR_TILES = 6, FAR_TILES = 36, WORTH = 12;
	constexpr double PI = 3.14159265358979323846;
	long cnt[8] = {0}, sx[8] = {0}, sy[8] = {0}, sd[8] = {0};
	const int3 size = cb->getMapSize();
	for(int y = std::max(0, origin.y - FAR_TILES); y <= std::min(size.y - 1, origin.y + FAR_TILES); ++y)
		for(int x = std::max(0, origin.x - FAR_TILES); x <= std::min(size.x - 1, origin.x + FAR_TILES); ++x)
		{
			const int dx = x - origin.x, dy = y - origin.y;
			const int d = std::max(std::abs(dx), std::abs(dy));
			if(d < NEAR_TILES || cb->isVisible(int3(x, y, z)))
				continue;
			const int k = int(std::floor((std::atan2(double(dy), double(dx)) + PI) / (PI / 4.0))) & 7;
			++cnt[k];
			sx[k] += x;
			sy[k] += y;
			sd[k] += d;
		}
	double best = 0.0;
	sector = -1;
	for(int k = 0; k < 8; ++k)
	{
		if(cnt[k] < WORTH || (forced >= 0 && k != forced) || (forced < 0 && taken.count(k)))
			continue;
		// Unseen tiles per tile of distance: a near patch of fog beats a
		// larger one far away.
		const double score = double(cnt[k]) / (double(sd[k]) / double(cnt[k]));
		if(score > best)
		{
			best = score;
			sector = k;
		}
	}
	if(sector < 0)
		return false;
	out = int3(int(sx[sector] / cnt[sector]), int(sy[sector] / cnt[sector]), z);
	return true;
}

bool OmniAI::scoutingDue() const
{
	// K's map-size note (September 24th): on a big map the neutral towns and
	// the mines are spread out and have to be found. R6z3c's first 72x72
	// game saw 8% of the map by week two and 10% by week four; all three of
	// the map's neutral towns lay outside it, and Nullkiller, which sees the
	// whole map, took every one (5 towns and 11000 a day by day 36). Half
	// the map is the bar, and 48 is the size the leash already treats as a
	// knife fight, where there is nothing between the two starts to find.
	if(!cb)
		return false;
	const int3 size = cb->getMapSize();
	if(std::max(size.x, size.y) <= 48)
		return false;
	const int today = cb->getDate(Date::DAY);
	if(seenFractionDay_ != today)
	{
		seenFractionDay_ = today;
		long seen = 0, total = 0, water = 0;
		for(int z = 0; z < size.z; ++z)
			for(int y = 0; y < size.y; ++y)
				for(int x = 0; x < size.x; ++x)
				{
					++total;
					const int3 q(x, y, z);
					if(cb->isVisible(q))
					{
						++seen;
						const TerrainTile * tile = cb->getTile(q, false);
						if(tile && tile->isWater())
							++water;
					}
				}
		seenFraction_ = total > 0 ? double(seen) / double(total) : 1.0;
		seenWaterShare_ = seen > 0 ? double(water) / double(seen) : 0.0;
	}
	return seenFraction_ < 0.5;
}

void OmniAI::notePress(const CGHeroInstance * hero,
	const CArmedInstance * blocker, bool moved, const int3 & tile)
{
	// A guard on the route is the press working as designed - waiting for
	// the army is the plan, not the stall saturation is for.
	if(blocker || !moved)
	{
		pressSatDays_ = 0;
		return;
	}
	// Only days spent AT the seal count: a hero still days away is
	// travelling, not camping.
	const int3 hp = hero->visitablePos();
	const int gap = std::max(std::abs(hp.x - tile.x), std::abs(hp.y - tile.y));
	if(pressSatHero_ == hero->id.getNum() && tile == pressSatTile_
		&& gap <= 4)
		++pressSatDays_;
	else
	{
		pressSatHero_ = hero->id.getNum();
		pressSatTile_ = tile;
		pressSatDays_ = gap <= 4 ? 1 : 0;
	}
}

bool OmniAI::sealedPressHold(const CGHeroInstance * hero, CPathsInfo * paths)
{
	constexpr int PRESS_SAT_DAYS = 5;
	if(pressSatHero_ != hero->id.getNum() || pressSatDays_ < PRESS_SAT_DAYS)
		return false;
	if(pressSatDays_ == PRESS_SAT_DAYS)
		decisionLog_->line("   POCKET SEALED after " + std::to_string(pressSatDays_)
			+ " days pressing (" + pressSatTile_.toString()
			+ ") - holding the town");
	return pocketHold(hero, paths);
}

void OmniAI::deferGuard(const CArmedInstance * guard)
{
	if(!guard)
		return;
	// Only a wandering monster holds its tile. An enemy hero standing beside
	// a mine or dwelling was being registered as that object's guard
	// (L_duel r01 day 19: "DEFER route guard at (9 14 0) (strength 12367)"
	// was Nullkiller's Sorsha), fencing the ground it stood on long after it
	// walked away. Enemy heroes are the reach checks' business.
	if(guard->ID != Obj::MONSTER)
		return;
	const int32_t id = guard->id.getNum();
	for(const DeferredTask & t : deferredTasks_)
		if(t.objId == id)
			return;
	DeferredTask t;
	t.objId = id;
	t.pos = guard->visitablePos();
	t.str = uint64_t(guard->getArmyStrength());
	t.day = cb ? cb->getDate(Date::DAY) : -1;
	deferredTasks_.push_back(t);
	decisionLog_->line("   DEFER route guard at " + t.pos.toString()
		+ " (strength " + std::to_string(t.str)
		+ ") - re-enters when our army beats it");
}

bool OmniAI::isDeferredTile(const int3 & pos) const
{
	// The guard's tile and its eight neighbours: stepping next to a stack
	// we declined to fight is the same fight one tile later.
	for(const DeferredTask & t : deferredTasks_)
	{
		if(t.pos.z != pos.z)
			continue;
		if(std::max(std::abs(t.pos.x - pos.x), std::abs(t.pos.y - pos.y)) <= 1)
			return true;
	}
	return false;
}

bool OmniAI::runDeferredGuards(const CGHeroInstance * hero, CPathsInfo * paths)
{
	if(!cb || !hero || !paths || deferredTasks_.empty())
		return false;
	// Of the beatable deferred guards, take the nearest: clearing a far wall
	// for a cheap prize pulls the field hero off the press for no gain. The
	// task list grows as more guarded valuables register, so least cost is
	// what keeps re-entry profitable instead of a detour.
	int pick = -1;
	std::vector<int3> pickSteps;
	const CArmedInstance * pickObj = nullptr;
	for(size_t i = 0; i < deferredTasks_.size(); ++i)
	{
		DeferredTask & t = deferredTasks_[i];
		const auto * obj = dynamic_cast<const CArmedInstance *>(
			cb->getObj(ObjectInstanceID(t.objId), false));
		if(!obj || (obj->tempOwner.isValidPlayer() && obj->tempOwner == playerID))
		{
			// The wall is gone - the fight happened, or the guard wandered
			// off the choke and the route opened on its own. Drop the task.
			deferredTasks_.erase(deferredTasks_.begin() + int(i));
			--i;
			continue;
		}
		if(!expectedToWin(hero, obj, commitMargin(hero)))
			continue;
		std::vector<int3> steps = stepsToward(paths, hero, obj->visitablePos());
		if(steps.empty())
			continue;
		if(pick < 0 || steps.size() < pickSteps.size())
		{
			pick = int(i);
			pickSteps = steps;
			pickObj = obj;
		}
	}
	if(pick < 0)
		return false;
	decisionLog_->line("   RE-ENTER route guard at "
		+ pickObj->visitablePos().toString() + ", army beats "
		+ std::to_string(uint64_t(pickObj->getArmyStrength())));
	deferredTasks_.erase(deferredTasks_.begin() + pick);
	return walk(hero, pickSteps);
}

bool OmniAI::huntWeakEnemyHero(const CGHeroInstance * hero, CPathsInfo * paths)
{
	if(!cb || !hero || !paths)
		return false;

	// The other half of grindNearestFight's "we are ahead, spend the
	// advantage" gate, aimed at a higher-value target: a neutral guard only
	// ever grants XP, but a visible enemy hero killed here stops outright -
	// no growth, no consolidation, no chance to become the stack that
	// catches us later. This is the traced cause of a fast duel-v2 loss
	// (DEVELOPMENT_LOG.md, the day-8 log): a 351-strength enemy hero was
	// visible on day 4 against our 8017 and nothing hunted it; by day 6 it
	// had grown to 10054 and started winning fights it had no business
	// winning two days earlier. Same near-free-kill floor grindNearestFight
	// already uses - a hero fight carries more hidden variance (spells,
	// artifacts) than a plain neutral stack, so this is not the place to
	// get greedy about the margin.
	//
	// No longer gated on the global posture (attacking_). K_duel, 16 games:
	// on 82 of 436 days an enemy hero at half our best hero's army or less
	// was in sight, 55 of those sightings within 15 tiles, and this fired 4
	// times. Those are Nullkiller's mine-flaggers (it held 13-15 of duel-v2's
	// 16 mines by day 19-28, ours included). Whether THIS kill is safe is a
	// local question: reachable today, won at the hero-fight bar, and not
	// ending inside the reach of a hero that beats us.
	const CGHeroInstance * best = nullptr;
	std::vector<int3> bestSteps;
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
		if(!e || !e->tempOwner.isValidPlayer() || e->tempOwner == playerID)
			continue;
		// 0.75 here is 0.90 in effect: expectedToWin adds the hero-fight
		// cushion (+0.15). At 0.90 the bar was 1.05 and this never fired.
		if(!expectedToWin(hero, e, 0.75))
			continue;
		CGPath probe;
		if(!paths->getPath(probe, e->visitablePos()) || probe.nodes.empty()
			|| probe.nodes.front().turns != 0)
			continue;   // a chase we cannot finish today ends beside it, not on it
		if(withinEnemyReach(e->visitablePos(), hero))
			continue;   // a stronger hero would take the winner the same night
		if(!leashAllows(hero, e->visitablePos()))
			continue;
		std::vector<int3> steps = stepsToward(paths, hero, e->visitablePos());
		if(steps.empty())
			continue;
		if(!best || steps.size() < bestSteps.size())
		{
			best = e;
			bestSteps = std::move(steps);
		}
	}
	if(!best)
		return false;
	decisionLog_->line("   HUNT (weak) " + best->getNameTranslated() + " at "
		+ best->visitablePos().toString() + ", "
		+ std::to_string(uint64_t(best->getArmyStrength())) + " against our "
		+ std::to_string(uint64_t(hero->getArmyStrength())) + ", denying it the growth");
	// The kill is the priced destination: fencing the last step at the
	// generic commit margin would park the hunter beside its quarry.
	return walk(hero, bestSteps, true);
}

bool OmniAI::pressCapital(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// SPEC press when ahead (queue, September 24th), unbuilt until R6x.
	// R6v3 r03: on day 11 our field hero killed Nullkiller's main in front
	// of our town; for the next nine days its strongest known hero was 2665
	// against our 8300-9700, and the field hero spent them on a guarded
	// dwelling and scouting ("no town to take is known"), because
	// conquestMarch only sees a town already out of fog. By day 20 its
	// Capitol paid 4000 a day to our 2000, by day 36 its army stood at 60.7k
	// to our 8.7k, and the game was lost on day 61. The capital's position
	// is public (the map header's start town, enemyStart_).
	//
	// Ahead means the posture's own test, the field army at 1.3 times the
	// strongest enemy hero projected from every sighting, with one change:
	// after the last known enemy hero dies, "none known" counts as ahead,
	// provided we have seen one at all this game. The garrison is priced
	// when the town comes into view; conquestMarch and the walk's siege
	// fence decide the fight itself.
	constexpr double ADVANTAGE = 1.3;   // assessPosture's
	if(!cb || !hero || !paths || !enemyStartValid_ || !foeEverSeen_ || !decisionLog_)
		return false;
	if(knowsHostileTown())
		return false;
	// A threat the town repels without us is no reason to stay: any enemy
	// hero within 12 tiles marks a town threatened, and R6x r01 held its
	// 40.6k field hero home through days 28-36 over a 312-strength scout.
	// Only a town that would fall without the field hero keeps it, the same
	// test defendThreatenedTown uses ("holds without the field hero").
	for(const CGTownInstance * t : cb->getTownsInfo())
	{
		if(!t || t->tempOwner != playerID || !threatenedTowns_.count(t->id.getNum()))
			continue;
		const CGHeroInstance * foe = strongestThreatNear(t);
		if(foe && !defenseHolds(nullptr, t, foe, hero))
			return false;
	}
	const int3 hp = hero->visitablePos();
	if(enemyStart_.z != hp.z)
		return false;
	const int today = cb->getDate(Date::DAY);
	double theirs = 0.0;
	for(const auto & kv : foeSeen_)
		theirs = std::max(theirs, kv.second.str
			* std::pow(1.045, double(std::max(0, today - kv.second.day))));
	const double ours = double(hero->getArmyStrength());
	if(ours < ADVANTAGE * theirs)
		return false;
	std::ostringstream why;
	why << "   PRESS toward their capital at " << enemyStart_.toString()
		<< ": our " << uint64_t(ours) << " against their strongest known "
		<< uint64_t(theirs);
	int3 frontier;
	if(fogFrontierToward(hero, paths, enemyStart_, frontier))
	{
		std::vector<int3> steps = stepsToward(paths, hero, frontier);
		if(!steps.empty())
		{
			decisionLog_->line(why.str() + ", " + std::to_string(steps.size())
				+ " steps to the fog edge at (" + frontier.toString() + ")");
			if(walk(hero, steps, false, true))
				return true;
		}
	}
	return pressToward(hero, paths, enemyStart_, why.str(), nullptr, nullptr, true);
}

bool OmniAI::raidEnemyCapital(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// Nullkiller empties its capital. K_duel, 16 games: its town garrison was
	// 0 on 275 of 435 days, and on 50 of those days our best hero stood
	// nearer that town than Nullkiller's own main did (`.tmp/omni/
	// nk_empty_town.py`). A town with no troops and no hero is captured by
	// walking in (CGTownInstance::onHeroVisit: no armedGarrison and no
	// visitor -> onTownCaptured). Nothing of ours ever tried: the small-map
	// leash held the field hero within 10 tiles of home, and conquestMarch
	// only sees a town already out of fog. So when their main was seen today
	// or yesterday far enough from its capital that we arrive first, walk at
	// the declared start. Once the town is in sight conquestMarch prices the
	// real garrison and takes over; the escape rule still pulls us out of a
	// stronger hero's reach on the way.
	if(!cb || !hero || !paths || !enemyStartValid_ || !threatenedTowns_.empty())
		return false;
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
		if(const auto * t = dynamic_cast<const CGTownInstance *>(obj))
			if(t->anchorPos() == enemyStart_)
				return false;   // in sight: conquestMarch has the real numbers
	const int today = cb->getDate(Date::DAY);
	// Their main is the strongest hero we have ever seen of theirs, not the
	// strongest seen lately: a scout in sight says nothing about a main that
	// may be standing at home.
	// With no town left the game ends in seven days whatever we do; their
	// capital is worth the race even when their main could get home first,
	// or when we have not seen it. R3_duel r03: our 9510 field hero spent
	// days 4-10 escaping from the hero sitting in our old town and lost on
	// the clock.
	const bool homeless = cb->getTownsInfo().empty();
	const FoeSighting * main = nullptr;
	for(const auto & kv : foeSeen_)
		if(!main || kv.second.str > main->str)
			main = &kv.second;
	if(!homeless && (!main || main->day < today - 1))
		return false;   // their main not seen since yesterday: it may be home
	auto cheb = [](const int3 & a, const int3 & b)
	{ return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); };
	const int3 hp = hero->visitablePos();
	if(hp.z != enemyStart_.z)
		return false;
	const int ourReach = std::max(1, hero->movementPointsLimit(true) / 100 + 1);
	const int ourDays = (cheb(hp, enemyStart_) + ourReach - 1) / ourReach;
	int mainDays = 99;
	if(main)
	{
		const int mainReach = std::max(1, main->reach);
		// A sighting from yesterday has already had a day to walk home.
		mainDays = (cheb(main->pos, enemyStart_) + mainReach - 1) / mainReach
			- (today - main->day);
	}
	if(ourDays >= mainDays && !homeless)
		return false;
	const CArmedInstance * blocker = nullptr;
	int3 tile;
	const bool moved = pressToward(hero, paths, enemyStart_,
		"   RAID toward their capital at " + enemyStart_.toString() + " ("
			+ std::to_string(ourDays) + " days for us, " + std::to_string(mainDays)
			+ " for their main)", &blocker, &tile, true);
	if(blocker)
		deferGuard(blocker);
	return moved;
}

bool OmniAI::grindNearestFight(const CGHeroInstance * hero, CPathsInfo * paths)
{
	if(!cb || !hero || !paths)
		return false;

	// Experience is the stat multiplier we never earned. September 24th,
	// J3 on MapGen 72x72: our best hero was level 1 on day 40 while
	// Nullkiller's reached 7-16 (Elleshar defense 12). Battle experience is
	// the hit points of what is killed plus 500 per hero
	// (BattleResultProcessor.cpp:223-241), and Nullkiller's own fights run
	// at 3-28x odds. So grind the way it does: only fights the calibrated
	// verdict calls near-free (0.94 survivors on a 36x36 map, the square law
	// at 3x odds; less on a bigger map, see grindSurvivor),
	// only targets this turn's movement reaches so there is no detour, and
	// the most experience first rather than the nearest. The old ungated
	// grind was measured to bleed the army, but under the uncalibrated
	// evaluator, which called 0.9 on fights that cost 30-50%; the posture
	// gate it needed then kept every hero at level 1.
	const double GRIND_SURVIVOR = grindSurvivor();
	const CArmedInstance * best = nullptr;
	std::vector<int3> bestSteps;
	double bestXp = 0.0;
	for(const auto & kv : worldObjs_)
	{
		const WorldObj & w = kv.second;
		if(w.kind != WorldObj::Kind::Guard)
			continue;
		const auto * obj = dynamic_cast<const CArmedInstance *>(
			cb->getObj(ObjectInstanceID(w.objId), false));
		if(!obj || (obj->tempOwner.isValidPlayer() && obj->tempOwner == playerID))
			continue;
		if(!expectedToWin(hero, obj, std::max(GRIND_SURVIVOR, commitMargin(hero))))
			continue;
		CGPath path;
		if(!paths->getPath(path, obj->visitablePos()) || path.nodes.empty()
			|| path.nodes.front().turns != 0)
			continue;   // not reachable today: no multi-day detour for a grind
		std::vector<int3> steps = stepsToward(paths, hero, obj->visitablePos());
		if(steps.empty() || safePrefix(hero, steps, true) != steps.size())
			continue;
		// Reached today, so the fight's tile is where the move ends. Asked
		// here as well as in walk(): the refused pick was re-chosen every
		// round, 10-45 "leashed" GRINDs a game in K_duel.
		if(!leashAllows(hero, obj->visitablePos()))
			continue;
		double xp = 0.0;
		for(const auto & st : obj->Slots())
			if(st.second && st.second->getCreature())
				xp += double(st.second->getCount())
					* std::max(1.0, double(st.second->getCreature()->getMaxHealth()));
		if(!best || xp > bestXp)
		{
			best = obj;
			bestXp = xp;
			bestSteps = std::move(steps);
		}
	}
	if(!best)
		return false;
	decisionLog_->line("   GRIND " + std::string(best->getObjectName())
		+ " at " + best->visitablePos().toString() + " for about "
		+ std::to_string(int(bestXp)) + " experience");
	return walk(hero, bestSteps, true);
}

void OmniAI::logGrindScan(const CGHeroInstance * hero)
{
	// The audit pass (audit_spread.py, September 24th) put our best hero at
	// level 1-2 in 14 of 16 R6n games while Nullkiller's reached 6-10, and
	// half of its experience came on days it beat one of our heroes. The
	// grind logs only the fights it takes, so "nothing qualified" and "the
	// chain never asked" read the same from outside. This says which.
	if(!cb || !hero || !pathCache_ || !decisionLog_)
		return;
	auto shared = pathCache_->getPathsInfo(hero);
	if(!shared)
		return;
	auto * paths = const_cast<CPathsInfo *>(shared.get());
	// grindNearestFight's bar: grindSurvivor(), or the voluntary-fight bar when that is higher
	const double GRIND_SURVIVOR = std::max(grindSurvivor(), commitMargin(hero));
	int known = 0, noRoute = 0, cheapToday = 0, cheapLater = 0, costly = 0, losing = 0;
	int fenced = 0, leashed = 0;
	double bestCostly = -1.0, cheapLaterXp = 0.0, cheapTodayXp = 0.0;
	std::string costlyNote, laterNote;
	for(const auto & kv : worldObjs_)
	{
		const WorldObj & w = kv.second;
		if(w.kind != WorldObj::Kind::Guard)
			continue;
		const auto * obj = dynamic_cast<const CArmedInstance *>(
			cb->getObj(ObjectInstanceID(w.objId), false));
		if(!obj || (obj->tempOwner.isValidPlayer() && obj->tempOwner == playerID))
			continue;
		++known;
		CGPath path;
		if(!paths->getPath(path, obj->visitablePos()) || path.nodes.empty())
		{
			++noRoute;
			continue;
		}
		const int turns = path.nodes.front().turns;
		double xp = 0.0;
		for(const auto & st : obj->Slots())
			if(st.second && st.second->getCreature())
				xp += double(st.second->getCount())
					* std::max(1.0, double(st.second->getCreature()->getMaxHealth()));
		const omniai::CombatVerdict v = combatVerdict(hero, obj);
		std::ostringstream note;
		note << obj->getObjectName() << " " << obj->visitablePos().toString()
			 << " " << turns << "t " << int(xp) << "xp";
		if(!v.win)
		{
			++losing;
			continue;
		}
		if(v.margin < GRIND_SURVIVOR)
		{
			++costly;
			if(v.margin > bestCostly)
			{
				bestCostly = v.margin;
				costlyNote = note.str();
			}
			continue;
		}
		if(turns != 0)
		{
			++cheapLater;
			if(xp > cheapLaterXp)
			{
				cheapLaterXp = xp;
				laterNote = note.str();
			}
			continue;
		}
		const std::vector<int3> steps = stepsToward(paths, hero, obj->visitablePos());
		if(steps.empty() || safePrefix(hero, steps, true) != steps.size())
		{
			++fenced;
			continue;
		}
		if(!leashAllows(hero, obj->visitablePos()))
		{
			++leashed;
			continue;
		}
		++cheapToday;
		cheapTodayXp = std::max(cheapTodayXp, xp);
	}
	std::ostringstream o;
	o << "   GRINDSCAN d" << cb->getDate(Date::DAY) << " " << hero->getNameTranslated()
	  << " army " << hero->getArmyStrength() << " L" << hero->level
	  << " exp " << hero->exp
	  << ": known " << known << " noRoute " << noRoute
	  << " | cheap today " << cheapToday << " (best " << int(cheapTodayXp) << "xp)"
	  << " fenced " << fenced << " leashed " << leashed
	  << " | cheap later " << cheapLater;
	if(!laterNote.empty())
		o << " (" << laterNote << ")";
	o << " | costly " << costly;
	if(!costlyNote.empty())
		o << " (best keeps " << int(bestCostly * 100.0 + 0.5) << "%: " << costlyNote << ")";
	o << " | losing " << losing;
	decisionLog_->line(o.str());

	// HUNTSCAN: the same question for enemy heroes. R6q_duel: on 180 of 651
	// hero-days a visible enemy hero stood at half our best hero's army or
	// less, 40 of them within 8 tiles, and HUNT (weak) fired 3 times in 21
	// games. Each filter huntWeakEnemyHero applies, counted in its order.
	int foes = 0, weakHalf = 0, lose = 0, notToday = 0, inReach = 0, leashedH = 0, open = 0;
	std::string openNote;
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
		if(!e || !e->tempOwner.isValidPlayer() || e->tempOwner == playerID)
			continue;
		++foes;
		if(2 * uint64_t(e->getArmyStrength()) <= uint64_t(hero->getArmyStrength()))
			++weakHalf;
		if(!expectedToWin(hero, e, 0.75))
		{
			++lose;
			continue;
		}
		CGPath probe;
		if(!paths->getPath(probe, e->visitablePos()) || probe.nodes.empty()
			|| probe.nodes.front().turns != 0)
		{
			++notToday;
			continue;
		}
		if(withinEnemyReach(e->visitablePos(), hero))
		{
			++inReach;
			continue;
		}
		if(!leashAllows(hero, e->visitablePos()))
		{
			++leashedH;
			continue;
		}
		++open;
		if(openNote.empty())
			openNote = e->getNameTranslated() + " " + std::to_string(uint64_t(e->getArmyStrength()))
				+ " at " + e->visitablePos().toString();
	}
	if(foes > 0)
	{
		std::ostringstream h;
		h << "   HUNTSCAN d" << cb->getDate(Date::DAY) << " " << hero->getNameTranslated()
		  << " army " << hero->getArmyStrength() << ": enemy heroes " << foes
		  << " (at half ours or less " << weakHalf << ") | would lose " << lose
		  << " | not today " << notToday << " | in a stronger hero's reach " << inReach
		  << " | leashed " << leashedH << " | huntable " << open;
		if(!openNote.empty())
			h << " (" << openNote << ")";
		decisionLog_->line(h.str());
	}
}

// ---------------------------------------------------------------------------
// The registry: the world model of actionable tasks. Each turn it rebuilds
// the list from what the map has shown us and hands every ready task to the
// hero that can reach it cheapest and is fit for it. Three task kinds cover
// the standing needs the per-hero chains were doing ad hoc:
//   Guard    - a route guard, deferred until the evaluator clears it
//   Deliver  - carry a town's recruit pool out to the field hero, on cadence
//   Capture  - take a neutral/enemy town whose garrison the evaluator clears
// ---------------------------------------------------------------------------

double OmniAI::nominalWorth(WorldObj::Kind kind) const
{
	// Flat, kind-only worth - no distance, no guard, no scarcity. Deliberately
	// the same numbers the route-collection pass already used ad hoc, now
	// the one place either of them reads. Bank and Town are excluded (0):
	// a captured bank's real prize is too variable to flatten into one
	// number here, and a town's value runs through townBusinessValue and
	// the dedicated Capture task, so counting it again here would double it.
	switch(kind)
	{
		case WorldObj::Kind::Pile:     return 300;
		// A mine pays every day for the rest of the game; it ranked below an
		// artifact (700), a visit (650) and a dwelling (600), so the support
		// heroes took those first and the mines near home waited a week
		// (R6i_duel r01).
		case WorldObj::Kind::Mine:     return 1000;
		case WorldObj::Kind::Dwelling: return 600;
		case WorldObj::Kind::Pickup:   return 700;
		case WorldObj::Kind::Visit:    return 650;
		default:                       return 0;
	}
}

double OmniAI::valueHorizonDays() const
{
	// How long an investment is counted to pay: a mine's output, a dwelling's
	// recruits, a hall's income. It was a flat 28 days everywhere, measured
	// against duel-v2, where games last about a month. On the 72x72
	// three-class map (R6z3c, September 24th) the same 28 days priced a
	// guarded mine below the fight for it and left the Fort waiting 20 days
	// for ore, while Nullkiller took three neutral towns and reached 11000 a
	// day. K's note the same afternoon: map size should shift pacing and the
	// Capitol-versus-tempo trade, read from the map rather than assumed.
	// Scaled with the map's linear size (the distances a game has to cover),
	// 28 days at duel-v2's 36x36, capped at four times that. Levels count as
	// area. The scale is a first estimate, to be checked against observed
	// game lengths by map size.
	if(!cb)
		return 28.0;
	const int3 size = cb->getMapSize();
	const double area = double(size.x) * double(size.y) * double(std::max(1, size.z));
	return std::clamp(28.0 * std::sqrt(area / (36.0 * 36.0)), 28.0, 112.0);
}

bool OmniAI::contactSoon() const
{
	// K's map-size note, the tempo side of it: on a small map the enemy's
	// main is at the door in the first week or two (R7p_duel: six of the
	// first twenty games over by day 14, Keyvan beside our field hero on day
	// 3), so troops bought now beat a hall finished four days sooner.
	if(!cb)
		return false;
	if(valueHorizonDays() <= 28.0)
		return true;
	const int today = cb->getDate(Date::DAY);
	for(const auto & kv : foeSeen_)
	{
		const FoeSighting & f = kv.second;
		const int ago = today - f.day;
		if(ago < 0 || ago > 2)
			continue;
		const int reach = std::max(1, f.reach);
		for(const CGTownInstance * town : cb->getTownsInfo())
		{
			if(!town || town->tempOwner != playerID || town->visitablePos().z != f.pos.z)
				continue;
			const int3 tp = town->visitablePos();
			const int d = std::max(std::abs(tp.x - f.pos.x), std::abs(tp.y - f.pos.y));
			if(d <= reach * (2 + ago))
				return true;
		}
	}
	return false;
}

double OmniAI::grindSurvivor() const
{
	// 0.94 was set on duel-v2's 36x36, where Nullkiller's main arrives in the
	// second week and every creature a grind costs is one fewer in that
	// fight. On a bigger map first contact is further off and the weekly
	// growth has replaced the loss before it. R6z3c's 72x72 games kept the
	// field hero at level 1-2 for 38-50 days because every guard in reach
	// cost 6-16% (Yeti 90-94%, Flesh Golems 88-94%, Wadjets 84-88%), while
	// Nullkiller's reached level 4 by day 10. Each step of the map's linear
	// scale (valueHorizonDays, 1 at 36x36, 2 at 72x72) lowers the bar 0.06,
	// down to 0.82. commitMargin still raises it when a stronger enemy hero
	// is near or was seen near one of our towns.
	const double scale = valueHorizonDays() / 28.0;
	return std::max(0.82, 0.94 - 0.06 * (scale - 1.0));
}

double OmniAI::guardedPrizeWorth(const CGObjectInstance * obj) const
{
	// buildingValue's horizons and rates, so a prize argues in the same
	// units as the army the fight would cost: four weeks of a dwelling's
	// recruits on top of what stands in it now, 28 days of a mine's output
	// with wood, ore and the rest at eight times a gold piece.
	const double HORIZON_DAYS = valueHorizonDays();
	const double WEEKS_AHEAD = HORIZON_DAYS / 7.0;
	constexpr double ARMY_PER_GOLD = 1.2;
	constexpr double RARE_MULTIPLE = 8.0;
	if(const auto * dw = dynamic_cast<const CGDwelling *>(obj))
	{
		double worth = double(recruitsOnOffer(obj));
		for(const auto & level : dw->creatures)
		{
			if(level.second.empty())
				continue;
			const auto * cre = level.second.front().toCreature();
			if(cre)
				worth += double(std::max(1, cre->getGrowth()))
					* double(std::max(0, cre->getAIValue())) * WEEKS_AHEAD;
		}
		return worth;
	}
	if(const auto * mine = dynamic_cast<const CGMine *>(obj))
	{
		const bool gold = mine->producedResource == GameResID(EGameResID::GOLD);
		return double(mine->producedQuantity) * HORIZON_DAYS
			* (gold ? ARMY_PER_GOLD : ARMY_PER_GOLD * RARE_MULTIPLE);
	}
	return 0.0;
}

void OmniAI::refreshWorldModel()
{
	if(!cb)
		return;
	const int today = cb->getDate(Date::DAY);

	// A reference hero for measuring a tile's guard - the field hero if we
	// have one, else whoever is on the board.
	const CGHeroInstance * ref = nullptr;
	if(fieldHeroId_ >= 0)
		ref = cb->getHero(ObjectInstanceID(fieldHeroId_));
	if(!ref && !cb->getHeroesInfo().empty())
		ref = cb->getHeroesInfo().front();

	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		if(!obj || !obj->isVisitable())
			continue;
		const int32_t id = obj->id.getNum();
		WorldObj & w = worldObjs_[id];
		// A town whose owner just became ours across a refresh was captured.
		// Mark it so the capturer can leave a holding garrison before it
		// marches on, instead of handing an empty town back to whoever walks
		// in next.
		const int32_t newOwner = obj->tempOwner.isValidPlayer()
			? obj->tempOwner.getNum() : -1;
		// Only a town the model already knew under another owner was taken.
		// A record created this refresh defaults to owner -1, so without the
		// lastSeen check the start town read as captured on day 1 and spent
		// its first week out of the delivery pool and leaking tripwire stacks.
		if(obj->ID.getNum() == Obj::TOWN && w.lastSeen >= 0
			&& w.owner != playerID.getNum() && newOwner == playerID.getNum())
			freshlyCaptured_[id] = today;
		// Stale entry: a town we lost again drops out, and one held past its
		// holding window goes back to feeding the pool.
		if(obj->ID.getNum() == Obj::TOWN && freshlyCaptured_.count(id))
		{
			if(newOwner != playerID.getNum() || today - freshlyCaptured_[id] > 7)
				freshlyCaptured_.erase(id);
		}
		w.objId = id;
		w.pos = obj->visitablePos();
		w.owner = newOwner;
		w.lastSeen = today;

		switch(obj->ID.getNum())
		{
			case Obj::TOWN: w.kind = WorldObj::Kind::Town; break;
			case Obj::MINE:
			case Obj::ABANDONED_MINE: w.kind = WorldObj::Kind::Mine; break;
			case Obj::RESOURCE: w.kind = WorldObj::Kind::Pile; break;
			case Obj::CREATURE_BANK: w.kind = WorldObj::Kind::Bank; break;
			case Obj::CREATURE_GENERATOR1:
			case Obj::CREATURE_GENERATOR2:
			case Obj::CREATURE_GENERATOR3:
			case Obj::CREATURE_GENERATOR4: w.kind = WorldObj::Kind::Dwelling; break;
			case Obj::MONSTER:
			case Obj::RANDOM_MONSTER: w.kind = WorldObj::Kind::Guard; break;
			case Obj::SUBTERRANEAN_GATE: w.kind = WorldObj::Kind::Portal; break;
			// Valuable one-shot pickups - an artifact, a chest, a scroll.
			case Obj::ARTIFACT:
			case Obj::TREASURE_CHEST:
			case Obj::SEA_CHEST:
			case Obj::SPELL_SCROLL:
			case Obj::FLOTSAM:
			case Obj::SHIPWRECK_SURVIVOR:
			case Obj::WARRIORS_TOMB: w.kind = WorldObj::Kind::Pickup; break;
			// A beneficial building visit - a shrine teaches a spell, a witch
			// hut a skill, a scholar a stat. Worth a stop when already close.
			case Obj::SHRINE_OF_MAGIC_INCANTATION:
			case Obj::SHRINE_OF_MAGIC_GESTURE:
			case Obj::SHRINE_OF_MAGIC_THOUGHT:
			case Obj::WITCH_HUT:
			case Obj::SCHOLAR:
			// Permanent hero growth: a stat booster or a free level. This is
			// the tempo lever, a hero that visits these keeps pace with the
			// enemy main's stats and one that does not falls behind for good.
			case Obj::ARENA:
			case Obj::SCHOOL_OF_WAR:
			case Obj::SCHOOL_OF_MAGIC:
			case Obj::MERCENARY_CAMP:
			case Obj::MARLETTO_TOWER:
			case Obj::GARDEN_OF_REVELATION:
			case Obj::LIBRARY_OF_ENLIGHTENMENT:
			case Obj::STAR_AXIS:
			case Obj::PILLAR_OF_FIRE:
			case Obj::LEARNING_STONE:
			case Obj::TREE_OF_KNOWLEDGE:
			case Obj::UNIVERSITY:
			case Obj::FOUNTAIN_OF_FORTUNE:
			case Obj::FOUNTAIN_OF_YOUTH:
			// A keymaster tent is a permanent key, not a consumable - the
			// first hero to visit one opens every border gate of that
			// colour for the rest of the game. Nullkiller quests these
			// out deliberately (CompleteQuest.cpp routes a hero to the
			// tent matching the gate's colour); scoring it as a Visit
			// gets the same object picked up whenever one is in reach,
			// which is what turns a sealed-pocket stall into an opened
			// door on maps like Twins and UQ.
			case Obj::KEYMASTER: w.kind = WorldObj::Kind::Visit; break;
			default: w.kind = WorldObj::Kind::Other; break;
		}
		// A tent we already keyed stays visitable but grants nothing -
		// without the filter it reads as a fresh Visit forever and heroes
		// keep re-stopping at it.
		if(const auto * tent = dynamic_cast<const CGKeymasterTent *>(obj))
			w.reward = tent->wasVisited(playerID) ? 0.0
				: nominalWorth(w.kind);
		else
			w.reward = nominalWorth(w.kind);

		// The prereq: whatever has to be beaten before the object is safe.
		// An armed object (monster stack, town garrison) carries its own
		// strength; an unguarded pile or mine leans on a separate guard
		// stack standing its tile, which guardStrengthAt finds.
		// A mine is an armed object with an empty garrison, so reading only
		// its own army called a mine behind a monster unguarded and handed it
		// to a support hero as a free pickup. Take the stronger of the two.
		uint64_t own = 0;
		if(const auto * armed = dynamic_cast<const CArmedInstance *>(obj))
			own = uint64_t(armed->getArmyStrength());
		w.guardStr = std::max<uint64_t>(own, ref ? guardStrengthAt(w.pos, ref) : 0);
	}
}

void OmniAI::refreshTasks()
{
	if(!cb)
		return;
	const int today = cb->getDate(Date::DAY);
	if(today == regenDay_)
		return;
	regenDay_ = today;
	regTasks_.clear();
	refreshWorldModel();

	const CGHeroInstance * field = (fieldHeroId_ >= 0)
		? cb->getHero(ObjectInstanceID(fieldHeroId_)) : nullptr;
	RegTask bestPrize;
	double bestPrizeValue = 0.0;
	std::string bestPrizeNote;

	// A guarded prize that is too strong today is a deferred task, not a dead
	// end: register the guard so it re-enters the moment the field army
	// crosses it, then whatever it was standing over becomes collectable.
	// This is the deferred-prereq mechanism generalized off the route-guard
	// seed - any valuable behind a wall, not just the one blocking the press.
	for(const auto & kv : worldObjs_)
	{
		const WorldObj & w = kv.second;
		if(w.kind != WorldObj::Kind::Mine && w.kind != WorldObj::Kind::Dwelling
			&& w.kind != WorldObj::Kind::Bank)
			continue;
		if(w.owner == playerID.getNum() || w.guardStr == 0)
			continue;
		const CGObjectInstance * obj = cb->getObj(ObjectInstanceID(w.objId), false);
		if(!obj)
			continue;
		const CArmedInstance * guard = strongestHostileAt(w.pos, field);
		if(!guard)
			continue;
		if(field && expectedToWin(field, guard, commitMargin(field)))
		{
			// Beatable now. This used to say "take it" and then assign it
			// to nobody: the collectors cannot fight it, and the field hero
			// met it only if the scoring pass happened to rank it first.
			// R6n_duel r02: blue's collector walked at the guarded Barracks
			// (33 20) on five days and stopped one step short each time;
			// the field hero chose it on day 17, and Nullkiller's Loynis
			// had flagged it on day 13. Nullkiller holds 3-6 external
			// dwellings a game and we held 0 on day 7 in all 16 R6n games.
			// Now a ready task for the field hero, when the fight costs no
			// more army than the prize is worth. Banks stay with the
			// scoring pass: their prize is not priced here.
			if(w.kind == WorldObj::Kind::Bank || !pathCache_)
				continue;
			const omniai::CombatVerdict v = combatVerdict(field, guard);
			const double cost = (1.0 - v.margin) * double(field->getArmyStrength());
			const double worth = guardedPrizeWorth(obj);
			if(!v.win || cost > worth || !leashAllows(field, guard->visitablePos()))
				continue;
			auto fp = pathCache_->getPathsInfo(field);
			CGPath route;
			if(!fp || !const_cast<CPathsInfo *>(fp.get())->getPath(route, guard->visitablePos())
				|| route.nodes.empty())
				continue;
			const double value = (worth - cost) / double(1 + route.nodes.front().turns);
			if(value > bestPrizeValue)
			{
				bestPrizeValue = value;
				bestPrize.kind = RegTask::Kind::Guard;
				bestPrize.objId = guard->id.getNum();
				bestPrize.pos = guard->visitablePos();
				bestPrize.str = uint64_t(guard->getArmyStrength());
				bestPrize.ready = true;
				bestPrize.heroId = fieldHeroId_;
				std::ostringstream o;
				o << "   PRIZE " << obj->getObjectName() << " at " << obj->visitablePos().toString()
				  << ": worth " << int(worth) << ", the fight costs " << int(cost)
				  << " (keeps " << int(v.margin * 100.0 + 0.5) << "%), "
				  << int(route.nodes.front().turns) << " day(s) off";
				bestPrizeNote = o.str();
			}
			continue;
		}
		deferGuard(guard);
	}
	// One at a time: the field hero takes the best, the rest wait a day.
	if(bestPrize.objId >= 0)
	{
		regTasks_.push_back(bestPrize);
		decisionLog_->line(bestPrizeNote);
	}

	// Route guards we have deferred: ready the moment the field army beats
	// them. The guard task IS the growth target - this is where it re-enters.
	for(const DeferredTask & d : deferredTasks_)
	{
		RegTask t;
		t.kind = RegTask::Kind::Guard;
		t.objId = d.objId;
		t.pos = d.pos;
		t.str = d.str;
		const auto * obj = dynamic_cast<const CArmedInstance *>(
			cb->getObj(ObjectInstanceID(d.objId), false));
		t.ready = obj && field && expectedToWin(field, obj, commitMargin(field));
		if(field)
			t.heroId = fieldHeroId_;
		regTasks_.push_back(t);
	}

	// Support-hero work - deliveries and pickups - gets a real assignment,
	// not first-come-per-loop: build every candidate task, order by worth,
	// and hand each to its nearest still-free support hero. A high-value
	// town's courier is not lost to a cheap pile that happened to come first,
	// and each hero carries at most one task.
	struct SupportCand { RegTask task; double worth; };
	std::vector<SupportCand> support;

	// Garrison deliveries: a town holding a worthwhile pool feeds the field
	// hero. Off while a town is threatened - a courier off carrying troops is
	// a post left open, and the pool is better as standing defense.
	if(field && threatenedTowns_.empty())
	{
		for(const CGTownInstance * town : cb->getTownsInfo())
		{
			if(!town || town->tempOwner != playerID)
				continue;
			// A just-captured town keeps its own recruits as a standing
			// garrison while it is fresh, so it is not drained back to
			// empty the week the enemy comes back for it.
			if(freshlyCaptured_.count(town->id.getNum()))
				continue;
			const uint64_t pool = townBusinessValue(town, field);
			if(pool < 1000)
				continue;
			RegTask t;
			t.kind = RegTask::Kind::Deliver;
			t.objId = town->id.getNum();
			t.pos = town->visitablePos();
			t.str = pool;
			t.ready = true;
			support.push_back({t, double(pool)});
		}
	}

	// Route collection: a free valuable is a pickup for a support hero, worth
	// its measured guard-free reward. Pickups and beneficial visits join the
	// piles/mines/dwellings - an artifact or a shrine is worth a stop, a loose
	// pile less so.
	if(field)
	{
		for(const auto & kv : worldObjs_)
		{
			const WorldObj & w = kv.second;
			const double worth = nominalWorth(w.kind);
			if(worth <= 0)
				continue;   // Bank/Town/Guard/Portal/Other are not a pickup here
			if(w.owner == playerID.getNum() || w.guardStr != 0)
				continue;   // ours, or still behind a guard
			const CGObjectInstance * obj =
				cb->getObj(ObjectInstanceID(w.objId), false);
			if(!obj)
				continue;
			RegTask t;
			t.kind = RegTask::Kind::Collect;
			t.objId = w.objId;
			t.pos = w.pos;
			t.ready = true;
			support.push_back({t, worth});
		}
	}

	// Highest worth first, each to its nearest free support hero.
	std::sort(support.begin(), support.end(),
		[](const SupportCand & a, const SupportCand & b){ return a.worth > b.worth; });
	std::set<int32_t> busyHeroes;
	constexpr long COLLECT_RADIUS = 15;
	for(const SupportCand & c : support)
	{
		const CGHeroInstance * runner = nullptr;
		long bestD = std::numeric_limits<long>::max();
		for(const CGHeroInstance * h : cb->getHeroesInfo())
		{
			if(!h || h->id.getNum() == fieldHeroId_
				|| h->id.getNum() == collectorHeroId_
				|| busyHeroes.count(h->id.getNum()))
				continue;
			const int3 hp = h->visitablePos();
			const long d = std::abs(hp.x - c.task.pos.x) + std::abs(hp.y - c.task.pos.y);
			if(d < bestD)
			{
				bestD = d;
				runner = h;
			}
		}
		if(!runner)
			continue;
		// Deliveries justify a longer walk; a pickup only makes sense local.
		if(c.task.kind == RegTask::Kind::Collect
			&& (bestD > COLLECT_RADIUS || isSoleHolder(runner)))
			continue;
		RegTask t = c.task;
		t.heroId = runner->id.getNum();
		busyHeroes.insert(t.heroId);
		regTasks_.push_back(t);
	}

	// Captures: a town whose garrison the field army clears is a growth
	// input, not just a win target - each taken town is another producer
	// feeding the weekly deliveries. Field hero takes it; on open maps a
	// second field-capable hero can too, but duel-v2 runs thin on heroes.
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * town = dynamic_cast<const CGTownInstance *>(obj);
		if(!town || town->tempOwner == playerID)
			continue;
		RegTask t;
		t.kind = RegTask::Kind::Capture;
		t.objId = town->id.getNum();
		t.pos = town->visitablePos();
		t.str = uint64_t(townDefenseStrength(town));
		// A siege commits the army, so the bar scales with how much the
		// committed hero is risking. It goes to whichever field-capable hero
		// can reach it cheapest (the main field hero or the second).
		const CGHeroInstance * second = (secondFieldId_ >= 0)
			? cb->getHero(ObjectInstanceID(secondFieldId_)) : nullptr;
		const CGHeroInstance * cap = field;
		if(second && field)
		{
			const int3 fp = field->visitablePos(), sp = second->visitablePos();
			const long fd = std::abs(fp.x - t.pos.x) + std::abs(fp.y - t.pos.y);
			const long sd = std::abs(sp.x - t.pos.x) + std::abs(sp.y - t.pos.y);
			if(sd < fd)
				cap = second;
		}
		else if(!field)
			cap = second;
		const omniai::CombatVerdict v = cap
			? estimateSiege(cap, town) : omniai::CombatVerdict{};
		// A capture is not done when the flag flips: it is done when a
		// defender is seated. Gate readiness on a defender being spareable or
		// hirable, so the field hero does not commit its army to a town that
		// gets walked back out from under it the next day.
		//
		// And not when the route is fenced: Gladeroot came back "ready" 127
		// times over days 12-31 on s29 because the siege eval cleared while
		// a 25.5k stack kept the route deferred - the hero walked, the fence
		// truncated it, and the task re-picked next round. A fenced route
		// parks the task as not-ready; the deferred guard's own Guard task
		// is what re-arms it the day the army beats the blocker.
		bool routeFenced = false;
		if(cap)
		{
			auto cpaths = pathCache_ ? pathCache_->getPathsInfo(cap) : nullptr;
			CGPath probe;
			if(cpaths && cpaths->getPath(probe, t.pos))
			{
				for(const auto & nd : probe.nodes)
				{
					if(isDeferredTile(nd.coord))
					{
						routeFenced = true;
						break;
					}
				}
			}
		}
		t.ready = cap && !routeFenced && v.win
			&& v.margin >= (lastKnownTownOf(town) ? DECISIVE_MARGIN : commitMargin(cap, 0.25))
			&& defenderAvailable();
		if(routeFenced)
			decisionLog_->detail("capture of " + town->getNameTranslated()
				+ " parked: route crosses a deferred fight");
		if(t.ready)
			t.heroId = cap->id.getNum();
		regTasks_.push_back(t);
	}

	// Hold: a just-captured town that is ours but has no hero seated on it
	// gets a defender routed out to live there. The capturer moves on; the
	// holder is what turns the take into a kept producer. Highest-worthest
	// exposed town to the nearest hero that can be spared.
	for(const auto & kv : freshlyCaptured_)
	{
		const CGTownInstance * town = dynamic_cast<const CGTownInstance *>(
			cb->getObj(ObjectInstanceID(kv.first), false));
		if(!town || town->tempOwner != playerID)
			continue;
		if(town->getGarrisonHero() || town->getVisitingHero())
			continue;   // a defender is already seated
		if(cb->getTownsInfo().size() <= 1)
			continue;   // our only town is held by its own defense logic
		RegTask t;
		t.kind = RegTask::Kind::Hold;
		t.objId = town->id.getNum();
		t.pos = town->visitablePos();
		t.str = uint64_t(town->getArmyStrength());
		t.ready = true;
		// Nearest support hero that can be spared takes it.
		const CGHeroInstance * runner = nullptr;
		long bestD = std::numeric_limits<long>::max();
		for(const CGHeroInstance * h : cb->getHeroesInfo())
		{
			if(!defenderCanBeSpared(h))
				continue;
			const int3 hp = h->visitablePos();
			const long d = std::abs(hp.x - t.pos.x) + std::abs(hp.y - t.pos.y);
			if(d < bestD)
			{
				bestD = d;
				runner = h;
			}
		}
		if(runner)
			t.heroId = runner->id.getNum();
		regTasks_.push_back(t);
	}
}

const OmniAI::RegTask * OmniAI::assignedTask(const CGHeroInstance * hero) const
{
	if(!hero)
		return nullptr;
	for(const RegTask & t : regTasks_)
		if(t.heroId == hero->id.getNum() && t.ready)
			return &t;
	return nullptr;
}

bool OmniAI::runRegistryTask(const CGHeroInstance * hero, CPathsInfo * paths)
{
	if(!cb || !hero || !paths)
		return false;
	const RegTask * t = assignedTask(hero);
	if(!t)
		return false;

	switch(t->kind)
	{
		case RegTask::Kind::Guard:
		{
			// The evaluator cleared it - walk onto the guard and open the route.
			const auto * obj = dynamic_cast<const CArmedInstance *>(
				cb->getObj(ObjectInstanceID(t->objId), false));
			if(!obj)
				return false;
			std::vector<int3> steps = stepsToward(paths, hero, obj->visitablePos());
			if(steps.empty())
				return false;
			decisionLog_->line("   TASK guard " + std::string(obj->getObjectName())
				+ " at " + obj->visitablePos().toString() + ", "
				+ std::to_string(steps.size()) + " steps");
			return walk(hero, steps);
		}
		case RegTask::Kind::Deliver:
		{
			const auto * town = dynamic_cast<const CGTownInstance *>(
				cb->getObj(ObjectInstanceID(t->objId), false));
			const CGHeroInstance * field = cb->getHero(ObjectInstanceID(fieldHeroId_));
			if(!town || !field)
				return false;
			const int3 hp = hero->visitablePos();
			// Standing in the source town: load the recruit pool into us.
			if(town->visitablePos() == hp || town->getVisitingHero() == hero
				|| town->getGarrisonHero() == hero)
				collectGarrison(town, hero);
			// Carrying a worthwhile load means deliver; empty means fetch. A
			// load the hand-over would not move (the field hero's cast-offs)
			// counts as empty, or the courier meets the field hero daily for
			// nothing.
			const bool loaded = hero->getArmyStrength() >= 800
				&& (wouldHandOver(hero, field) || carriesArtifacts(hero));
			// Town Portal makes the long leg free. Loaded: teleport to the
			// town nearest the field hero and walk the last stretch. Empty:
			// teleport straight to the source town to load. A courier that
			// used to burn a week on the road spends a turn.
			if(hero->getSpellsInSpellbook().count(SpellID(SpellID::TOWN_PORTAL)))
			{
				const CGTownInstance * portTown = nullptr;
				if(!loaded)
					portTown = town;
				else
				{
					const int3 fp = field->visitablePos();
					long bestD = std::numeric_limits<long>::max();
					for(const CGTownInstance * t2 : cb->getTownsInfo())
					{
						if(!t2 || t2->tempOwner != playerID)
							continue;
						const long d = std::abs(t2->visitablePos().x - fp.x)
							+ std::abs(t2->visitablePos().y - fp.y);
						if(d < bestD)
						{
							bestD = d;
							portTown = t2;
						}
					}
				}
				if(portTown && castTownPortalTo(hero, portTown))
					return true;
			}
			const int3 dest = loaded ? field->visitablePos() : town->visitablePos();
			std::vector<int3> steps = stepsToward(paths, hero, dest);
			if(steps.empty())
				return false;
			decisionLog_->line("   TASK deliver " + town->getNameTranslated()
				+ (loaded ? " -> field" : " -> pickup") + ", carrying "
				+ std::to_string(uint64_t(hero->getArmyStrength())));
			return walk(hero, steps);
		}
		case RegTask::Kind::Capture:
		{
			const auto * town = dynamic_cast<const CGTownInstance *>(
				cb->getObj(ObjectInstanceID(t->objId), false));
			if(!town)
				return false;
			std::vector<int3> steps = stepsToward(paths, hero, town->visitablePos());
			if(steps.empty())
				return false;
			decisionLog_->line("   TASK capture " + town->getNameTranslated()
				+ " - evaluator clears the garrison");
			return walk(hero, steps);
		}
		case RegTask::Kind::Hold:
		{
			const auto * town = dynamic_cast<const CGTownInstance *>(
				cb->getObj(ObjectInstanceID(t->objId), false));
			if(!town || town->tempOwner != playerID)
				return false;
			// Seat the defender: on the walls it is the town's army, not a
			// post beside it. A hero in the doorway blocks other visits, so
			// it steps into the garrison slot.
			if(town->getVisitingHero() == hero)
			{
				cb->swapGarrisonHero(town);
				return true;
			}
			if(town->getGarrisonHero() == hero)
				return true;
			std::vector<int3> steps = stepsToward(paths, hero, town->visitablePos());
			if(steps.empty())
				return false;
			decisionLog_->line("   TASK hold " + town->getNameTranslated()
				+ " - routing a defender to live on it");
			return walk(hero, steps);
		}
		case RegTask::Kind::Collect:
			// A pickup is a fallback, not a job that outranks a delivery or a
			// held post - handled by runCollectTask late in the chain, not
			// here where it would preempt the hero's real work.
			return false;
	}
	return false;
}

bool OmniAI::runCollectTask(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// Idle-hero pickup: only fires for a hero whose assigned task is a Collect
	// and who has reached the end of the chain with nothing better. Keeps
	// route collection in the assignment layer without letting it preempt a
	// delivery or a held post.
	if(!cb || !hero || !paths)
		return false;
	const RegTask * t = assignedTask(hero);
	if(!t || t->kind != RegTask::Kind::Collect)
		return false;
	const CGObjectInstance * obj =
		cb->getObj(ObjectInstanceID(t->objId), false);
	if(!obj)
		return false;
	std::vector<int3> steps = stepsToward(paths, hero, obj->visitablePos());
	if(steps.empty() || safePrefix(hero, steps) == 0)
		return false;
	decisionLog_->line("   TASK collect " + obj->visitablePos().toString());
	return walk(hero, steps);
}

bool OmniAI::conquestMarch(const CGHeroInstance * hero, CPathsInfo * paths)
{
	if(!cb || !hero || !paths)
		return false;

	const double attack = conquestStrength(hero);

	// Siege fights uphill through towers and walls, so the evaluator has to
	// call it a win and we need enough army left over to hold the captured
	// town, not just to breach it. The bar rises with the army we are
	// committing - a 200k siege cannot be a coin flip - and with the length
	// of the march: the eval is taken today but the fight lands on arrival,
	// and the defender reinforces while we walk. Gladeroot's garrison grew
	// 0 -> 8700 across a multi-day approach on s7, and the human playtest's
	// tan siege bounced under strength the same way. Each turn of travel is
	// priced as another slice of their production meeting us at the walls.
	auto marginNeeded = [&](const CGPath & probe) -> double
	{
		constexpr double DRIFT_PER_TURN = 0.05;
		const int turns = probe.nodes.empty()
			? 0 : int(probe.nodes.front().turns);
		return std::max(0.20, commitMargin(hero, 0.25)) + DRIFT_PER_TURN * turns;
	};
	auto takeable = [&](const CGTownInstance * town) -> bool
	{
		if(!town || town->tempOwner == playerID)
			return false;
		CGPath probe;
		if(!paths->getPath(probe, town->visitablePos()))
			return false;
		// Couriers can only matter to a march that takes days; a town in
		// reach today is fought with what the hero carries.
		const bool days = !probe.nodes.empty() && probe.nodes.front().turns > 0;
		const omniai::CombatVerdict v = estimateSiege(hero, town, days);
		return v.win && v.margin >= (lastKnownTownOf(town)
			? DECISIVE_MARGIN : marginNeeded(probe));
	};

	// Keep the current target while it is still a win. Retargeting every
	// turn is how a hero paces between two towns it never reaches.
	const CGTownInstance * target = nullptr;
	if(conquestTargetId_ >= 0)
	{
		target = dynamic_cast<const CGTownInstance *>(
			cb->getObj(ObjectInstanceID(conquestTargetId_), false));
		if(target && target->tempOwner != playerID)
		{
			CGPath probe;
			const bool pathed = paths->getPath(probe, target->visitablePos());
			const bool days = !pathed
				|| (!probe.nodes.empty() && probe.nodes.front().turns > 0);
			const omniai::CombatVerdict v = estimateSiege(hero, target, days);
			if(pathed)
			{
				if(!v.win || v.margin < (lastKnownTownOf(target)
					? DECISIVE_MARGIN : marginNeeded(probe)))
				{
					target = nullptr;
					conquestTargetId_ = -1;
				}
			}
			else
			{
				// The flood does not reach the town tile today, so the old
				// takeable() check dropped the committed target, the hero
				// wandered on explore/scout for the morning, and the march
				// re-committed on leftover movement a round later. Measured
				// on s7 norm: a 25 tile march crawled for days while their
				// main walked into our capital. The army is committed and
				// the siege still wins, so press the boundary the way the
				// weakest-hostile press below does.
				if(v.win && v.margin >= std::max(0.20, commitMargin(hero, 0.25))
					&& pressToward(hero, paths, target->visitablePos(),
						"   SIEGE PRESS toward " + target->getNameTranslated()
							+ " at " + target->visitablePos().toString(),
						nullptr, nullptr, true))
					return true;
				target = nullptr;
				conquestTargetId_ = -1;
			}
		}
		else
		{
			target = nullptr;
			conquestTargetId_ = -1;
		}
	}
	if(!target)
	{
		// Nearest takeable town first, weakest breaks ties. Weakest alone
		// flipped the target between two empty towns every day as enemy
		// heroes stepped in and out of them: R6n_duel r11 days 53-56, our
		// 112811 field hero marched on Middleheim, Gateway, Middleheim,
		// Gateway, 20+ tiles each way, and was caught in the open by a 57k
		// hero with both towns empty. Nearest keeps the march on the town it
		// is already closing on.
		int nearestTurns = std::numeric_limits<int>::max();
		double weakest = std::numeric_limits<double>::max();
		double weakestAny = std::numeric_limits<double>::max();
		const CGTownInstance * weakAny = nullptr;
		for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
		{
			const auto * town = dynamic_cast<const CGTownInstance *>(obj);
			if(!town || town->tempOwner == playerID)
				continue;
			const double def = townDefenseStrength(town);
			if(def < weakestAny)
			{
				weakestAny = def;
				weakAny = town;
			}
			if(!takeable(town))
				continue;
			CGPath route;
			const int turns = paths->getPath(route, town->visitablePos()) && !route.nodes.empty()
				? int(route.nodes.front().turns) : std::numeric_limits<int>::max();
			if(turns < nearestTurns || (turns == nearestTurns && def < weakest))
			{
				nearestTurns = turns;
				weakest = def;
				target = town;
			}
		}
		if(target)
			conquestTargetId_ = target->id.getNum();
		else if(weakAny)
		{
			// Which gate actually keeps us off the weakest hostile town:
			// the siege margin, or the same reach problem that seals our
			// own capital.
			CGPath probe;
			const bool pathable = paths->getPath(probe, weakAny->visitablePos());
			decisionLog_->detail("no takeable town: weakest is "
				+ weakAny->getNameTranslated() + " at "
				+ std::to_string(uint64_t(weakestAny)) + " vs our "
				+ std::to_string(uint64_t(attack)) + " (needs "
				+ std::to_string(uint64_t(weakestAny * 1.3)) + "), "
				+ (pathable ? "path exists" : "no path reaches it"));
			// The army is there and the map is not. Measured on Twins: the
			// enemy capital emptied to 0 defence on day 36, needed 0, and
			// the line above said `no path reaches it`, so a town we could
			// have walked into sat there for the rest of the game. Press at
			// the boundary the way resupplyMarch presses toward our own
			// sealed town: closest reachable tile wins, sight gain breaks
			// ties, and a guard we can beat is a legal target because
			// stepping into its zone of control starts the fight that opens
			// the way.
			//
			// Strict on purpose. The evaluator must already call the siege a
			// win, so this fires only when the prize is genuinely takeable
			// and the map is the only thing in the way, which is rare enough
			// that it cannot pin the field hero at a boundary the way an
			// always-armed resupply march did.
			// Across water the press only ever reaches the shore: on Emerald
			// Isles (R6y_water) the field hero "SIEGE PRESSED" toward Sancras
			// and stood on the same coast tiles every day, because this ran
			// ahead of the crossing and returned first. seekCrossing does
			// nothing when a land route exists, even one through fog.
			if(!pathable && seekCrossing(hero, paths))
				return true;
			const omniai::CombatVerdict v = estimateSiege(hero, weakAny);
			if(!pathable && v.win && v.margin >= (lastKnownTownOf(weakAny)
					? DECISIVE_MARGIN : std::max(0.20, commitMargin(hero, 0.25)))
				&& pressToward(hero, paths, weakAny->visitablePos(),
					"   SIEGE PRESS toward " + weakAny->getNameTranslated()
						+ " at " + weakAny->visitablePos().toString(),
					nullptr, nullptr, true))
				return true;
		}
	}
	if(!target)
		return false;

	std::vector<int3> steps = stepsToward(paths, hero, target->visitablePos());
	if(steps.empty())
	{
		conquestTargetId_ = -1;
		return false;
	}
	std::ostringstream o;
	o << "   MARCH on " << target->getNameTranslated() << ", garrison "
	  << uint64_t(townDefenseStrength(target)) << " vs our " << uint64_t(attack);
	decisionLog_->line(o.str());
	targetedThisTurn_.insert(target->id.getNum());
	// The town is the fight this walk exists for. The small-map leash keeps
	// the field hero near its only town; a town the siege evaluator says we
	// take is worth more than that guard, so it does not veto the march.
	return walk(hero, steps, true, true);
}

bool OmniAI::resupplyMarch(const CGHeroInstance * hero, CPathsInfo * paths)
{
	if(!cb || !hero || !paths || resupplyTownId_ < 0)
		return false;

	const CGTownInstance * town = nullptr;
	for(const CGTownInstance * t : cb->getTownsInfo())
		if(t && t->id.getNum() == resupplyTownId_)
			town = t;

	// A drop that means "this town is not reachable from here" puts the
	// town on a cooldown. Without it runHomeErrand re-arms the same town
	// on the very next round, the budget never expires, and the press
	// walks the field hero back to the same boundary tile every morning
	// for the rest of the game. A path opening is a success and carries
	// no cooldown: the plain home errand takes over from there.
	constexpr int RESUPPLY_COOLDOWN_DAYS = 20;
	auto drop = [&](const char * why, bool cooldown = true) {
		if(cooldown && resupplyTownId_ >= 0)
			resupplyCooldown_[resupplyTownId_] =
				cb->getDate(Date::DAY) + RESUPPLY_COOLDOWN_DAYS;
		resupplyTownId_ = -1;
		resupplyTurnsLeft_ = 0;
		decisionLog_->detail(std::string("resupply march dropped: ") + why);
		return false;
	};

	if(!town || town->tempOwner != playerID)
		return drop("town is gone");
	if(--resupplyTurnsLeft_ <= 0)
		return drop("budget spent");

	// The march exists because no path reaches the town. The moment one
	// does - a guard died, a monster drifted off the corridor - the plain
	// home errand does the collecting and the march is done.
	const int3 gate = town->visitablePos();
	{
		CGPath probe;
		if(paths->getPath(probe, gate))
			return drop("a path opened", false);
	}

	// Stand on the reachable tile that best approaches the town.
	if(!pressToward(hero, paths, gate,
		"   RESUPPLY toward " + town->getNameTranslated()
			+ " at " + gate.toString()))
		return drop("nothing reachable gets closer or uncovers anything");
	return true;
}

// The reachable tile that best approaches a destination the pathfinder
// cannot reach: closest along the land route that may exist (fog open,
// what we have seen as it is), a lateral step only where it uncovers new
// ground, and any guard we can beat is a legal target -
// stepping into the zone of control starts the fight that opens the
// pocket. The fight margin is deliberately looser than the danger
// threshold: whatever sits behind the seal is worth more than the hero,
// and the alternative to a long-odds fight is starving at the boundary
// forever. Returns false when nothing reachable helps.
bool OmniAI::scanSeal(const CGHeroInstance * hero, CPathsInfo * paths,
	const int3 & dest, int3 & bestTile, const CArmedInstance ** blocker)
{
	// The seal fight only needs the win, not a comfortable one: whatever sits
	// behind the seal is worth more than the hero, and the alternative to a
	// costly breach is starving at the boundary forever. The evaluator's
	// expected outcome replaces the strength-ratio margin.
	// Raised from 0.10 on September 24th: a seal fight that keeps a tenth of
	// the army opens the pocket for the enemy's intact stack, not for us.
	// Twice on day 1 of duel-v2 the hunt stepped into the level-6 guard on the
	// road east and paid 63-100% of the field army. Same bar exploreFrontier's
	// break-out fights already use.
	constexpr double SEAL_SURVIVOR = 0.50;
	const int3 hp = hero->visitablePos();
	const int3 sizes = cb->getMapSize();
	// Closeness is measured along the land route that may still exist
	// (crossingField: fog open, seen ground as it is), not in a straight
	// line. R7h3c mg72s7 r01: the only way out of our start zone ran past a
	// Sprites stack at (16 6 0), which the seal bar beats, and the hunt
	// pressed toward (11 13 0) instead for days, a walled corner a few tiles
	// nearer the enemy's corner as the crow flies. Map coverage stayed at 8%
	// to day 21. A wall we have seen lengthens the route, so a dead end
	// stops winning once it is looked at. Where not even the fog leaves a
	// land route (an island), the straight line stays in use.
	const std::vector<int> route = crossingField(dest, false);
	auto routeAt = [&](const int3 & q) { return route[size_t(q.y) * size_t(sizes.x) + size_t(q.x)]; };
	const bool byRoute = dest.z == hp.z && routeAt(hp) >= 0;
	const int heroDist = byRoute ? routeAt(hp)
		: std::max(std::abs(dest.x - hp.x), std::abs(dest.y - hp.y));

	long bestScore = 0;
	bool found = false;
	// The strongest unbeatable guard on a tile that would carry us closer to
	// dest: the wall that, once we can beat it, opens the route. Reported so
	// the press can defer it instead of abandoning the push.
	const CArmedInstance * bestBlocker = nullptr;
	long bestBlockerScore = LONG_MIN;
	// A tile one of our own heroes stands on is a meeting, not a step: the
	// walk resolves as an exchange and the hero spends its movement where it
	// stands. R6z3c's first 72x72 game: the field hero pressed home onto its
	// own courier in a one-tile corridor every morning from day 7 to day 28.
	std::set<int3> ours;
	for(const CGHeroInstance * h : cb->getHeroesInfo())
		if(h && h != hero)
			ours.insert(h->visitablePos());
	for(int y = 0; y < sizes.y; ++y)
		for(int x = 0; x < sizes.x; ++x)
		{
			const int3 p(x, y, dest.z);
			if(p == hp || ours.count(p))
				continue;
			const CGPathNode * n = paths->getNode(p, EPathfindingLayer::LAND);
			if(!n || !n->reachable())
				continue;
			const int d = byRoute ? routeAt(p)
				: std::max(std::abs(x - dest.x), std::abs(y - dest.y));
			if(d < 0 || d > heroDist)
				continue;
			// Any tile in reach of a hostile we cannot beat is a wall, not
			// just tiles the pathfinder flags GUARDED: a guard's own tile
			// reads as visitable (the object IS the visit) and used to pass
			// straight through here - m002's SALLY dispatched 5 steps whose
			// exempt last step landed ON a 5750 stack and fought at 0.02.
			// strongestHostileAt already covers the tile and its neighbours.
			const CArmedInstance * g = strongestHostileAt(p, hero);
			if(g && !expectedToWin(hero, g, SEAL_SURVIVOR))
			{
				const long bs = long(heroDist - d) * 1000 + revealFrom(p);
				if(bs > bestBlockerScore)
				{
					bestBlockerScore = bs;
					bestBlocker = g;
				}
				continue;
			}
			// Distance dominates, sight gain breaks the tie: the closest
			// boundary tile wins, and among equals the one that sees most
			// into the sealed region. A beatable guard outranks either -
			// fighting it is what actually opens the pocket.
			const long score = long(heroDist - d) * 1000 + revealFrom(p)
				+ (n->accessible == EPathAccessibility::GUARDED ? 500 : 0);
			if(score > bestScore)
			{
				bestScore = score;
				bestTile = p;
				found = true;
			}
		}
	if(blocker)
		*blocker = bestBlocker;
	return found;
}

bool OmniAI::pressToward(const CGHeroInstance * hero, CPathsInfo * paths,
	const int3 & dest, const std::string & label,
	const CArmedInstance ** blocker, int3 * pressTile, bool ignoreLeash)
{
	int3 bestTile;
	if(!scanSeal(hero, paths, dest, bestTile, blocker))
		return false;
	if(pressTile)
		*pressTile = bestTile;

	std::vector<int3> steps = stepsToward(paths, hero, bestTile);
	if(steps.empty())
		return false;

	decisionLog_->line(label + ", " + std::to_string(steps.size())
		+ " steps toward (" + bestTile.toString() + ")");
	// The seal fight is the point of the press: scanSeal picked this tile
	// at its own loose margin, so the last step is a priced destination.
	return walk(hero, steps, true, ignoreLeash);
}

bool OmniAI::sallyOut(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// A hero inside the same pocket as a town presses at the seal toward a
	// hero sealed outside. The inside hero's strength - the town pool's,
	// once loaded - takes the choke guard that keeps the outside hero weak,
	// and a carrier that gets out both delivers and opens the pocket behind
	// it. The field hero inside the pocket sallies too: it presses toward
	// whoever is sealed out, which is how the pocket opens outward.
	if(!cb || !hero || !paths)
		return false;

	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;

		// This hero has to be inside the pocket to help: in the town, or
		// its own paths reach the town that a sealed-out hero cannot.
		const bool inside = town->getVisitingHero() == hero
			|| town->getGarrisonHero() == hero;
		if(!inside)
		{
			CGPath mine;
			if(!paths->getPath(mine, town->visitablePos()))
				continue;   // outside the pocket too - cannot help
		}

		// Find a hero sealed out of this town to press toward. A hero
		// standing AT the town is not sealed out - getPath answers a
		// degenerate query about it and would call it sealed forever,
		// which is how two heroes at the same town sallied toward each
		// other in a 1392-day standstill.
		const CGHeroInstance * target = nullptr;
		for(const CGHeroInstance * h : cb->getHeroesInfo())
		{
			if(!h || h == hero || h->tempOwner != playerID)
				continue;
			if(town->getVisitingHero() == h || town->getGarrisonHero() == h
				|| h->visitablePos() == town->visitablePos())
				continue;   // h is at the town - inside, not sealed out
			auto hp = pathCache_ ? pathCache_->getPathsInfo(h) : nullptr;
			if(!hp)
				continue;
			CGPath probe;
			if(const_cast<CPathsInfo *>(hp.get())->getPath(probe, town->visitablePos()))
				continue;   // h can reach the town - not sealed out
			target = h;
			break;
		}
		if(!target)
			continue;   // no sealed-out hero - the pocket is not our problem

		const double ours0 = double(std::max<uint64_t>(1, hero->getArmyStrength()));
		const double refStr = double(std::max<uint64_t>(1, target->getArmyStrength()));

		// Same seal-camping pathology as the field hero's press, on the
		// inside hero: s23-swap logged SALLY to the same sealed-out hero
		// for days while the town sat empty. If this hero has been on the
		// same boundary long enough, pocketHold parks it instead.
		if(sealedPressHold(hero, paths))
			return true;

		// Is there anywhere to press toward at all? Standing at the seal
		// with the pool IS the sally when nothing closer qualifies - the
		// pocket's edge is where it opens first, and falling through to
		// holdTheFort would walk the carrier home, deposit the pool, and
		// collect it again tomorrow, churning one stack forever.
		// The pool at home compounds weekly; the saddlebag does not. A
		// carrier parked at a closed seal with a grown pool behind it is
		// half a delivery - Twins logged one holding at the choke with
		// 3075 while the garrison it left stood at 16685, and it never
		// walked back for the rest. When the town's standing stack is
		// worth at least half of what this hero already carries, spend a
		// turn reloading instead of holding at the seal. Checked before
		// the press so both the stalled-press and parked-at-seal cases
		// top up.
		auto townPool = [&](const CGTownInstance * t) -> uint64_t
		{
			uint64_t pool = 0;
			for(const auto & entry : t->Slots())
				if(entry.second && entry.second->getCreature())
					pool += uint64_t(entry.second->getCount())
						* uint64_t(std::max(0, entry.second->getCreature()->getAIValue()));
			return pool;
		};
		if(!inside && double(townPool(town)) >= ours0 * 0.5)
		{
			std::vector<int3> home =
				stepsToward(paths, hero, town->visitablePos());
			if(!home.empty())
			{
				decisionLog_->line("   SALLY reloading at "
					+ town->getNameTranslated() + " - pool "
					+ std::to_string(uint64_t(townPool(town)))
					+ " against " + std::to_string(uint64_t(ours0))
					+ " in the saddle");
				return walk(hero, home);
			}
		}

		int3 sealTile;
		if(!scanSeal(hero, paths, target->visitablePos(), sealTile))
		{
			// Two carriers holding the same seal apart are one army that
			// cannot open it twice (R7l3c Twins r01, days 4-14). The weaker
			// walks to a stronger support hero it reaches today, the meeting
			// pools the armies (heroExchangeStarted), and the pooled one
			// prices the seal fight tomorrow.
			{
				const CGHeroInstance * join = nullptr;
				std::vector<int3> joinSteps;
				for(const CGHeroInstance * other : cb->getHeroesInfo())
				{
					if(!other || other == hero || other->id.getNum() == fieldHeroId_
						|| other->getArmyStrength() <= hero->getArmyStrength()
						|| other->visitablePos().z != hero->visitablePos().z)
						continue;
					CGPath probe;
					if(!paths->getPath(probe, other->visitablePos()) || probe.nodes.empty()
						|| probe.nodes.front().turns != 0)
						continue;
					std::vector<int3> st = stepsToward(paths, hero, other->visitablePos());
					if(st.empty())
						continue;
					if(!join || st.size() < joinSteps.size())
					{
						join = other;
						joinSteps = std::move(st);
					}
				}
				if(join)
				{
					decisionLog_->line("   POOL " + hero->getNameTranslated() + " ("
						+ std::to_string(uint64_t(ours0)) + ") joins "
						+ join->getNameTranslated() + " ("
						+ std::to_string(uint64_t(join->getArmyStrength()))
						+ ") at the seal toward " + target->getNameTranslated());
					if(walk(hero, joinSteps, true))
						return true;
				}
			}
			if(ours0 >= refStr * 0.5)
			{
				decisionLog_->line("   SALLY holding at the seal toward "
					+ target->getNameTranslated() + ", carrying "
					+ std::to_string(uint64_t(ours0)));
				// Holding in place still counts toward saturation: the
				// hero's own tile is the endpoint it keeps choosing.
				notePress(hero, nullptr, true, hero->visitablePos());
				return true;
			}
			continue;
		}

		// Standing in it: the pool is the cargo, and it is also what makes
		// the choke guard beatable. Count hero + garrison together before
		// committing - loading it makes this hero the strongest on the
		// board, which assignHeroRoles notices, so a carrier that breaks
		// out becomes the field hero.
		double ours = ours0;
		if(inside)
		{
			uint64_t pool = 0;
			for(const auto & entry : town->Slots())
				if(entry.second && entry.second->getCreature())
					pool += uint64_t(entry.second->getCount())
						* uint64_t(std::max(0, entry.second->getCreature()->getAIValue()));
			if(ours + double(pool) < refStr * 0.5)
				continue;   // even loaded, not worth leaving the post over
			collectGarrison(town, hero);
			ours = double(std::max<uint64_t>(1, hero->getArmyStrength()));
		}
		else if(ours < refStr * 0.5)
			continue;   // a passer-by carries only what it has

		const CArmedInstance * sallyBlocker = nullptr;
		int3 sallyTile;
		const bool sallyMoved = pressToward(hero, paths, target->visitablePos(),
			"   SALLY to " + target->getNameTranslated()
				+ " at " + target->visitablePos().toString(),
			&sallyBlocker, &sallyTile);
		// The seal guard keeping the outside hero out is the same kind of
		// deferred target the field press registers - waiting for the army
		// is the plan, so it belongs on the same list.
		if(sallyBlocker)
			deferGuard(sallyBlocker);
		notePress(hero, sallyBlocker, sallyMoved, sallyTile);
		if(sallyMoved)
			return true;
	}
	return false;
}

bool OmniAI::retreatMove(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// The scored pass declined every candidate because its endpoint sits in
	// a stronger enemy's reach. Taking the best one anyway is a walk toward
	// the pickup that hands the enemy first move: the logs read "nowhere
	// safe to stop" and a defended loss within a day in 8 of 9 cases. A
	// turn spent retreating costs the pursuer its own movement instead.
	if(!cb || !hero || !paths)
		return false;

	// Same bar as withinEnemyReach: only heroes that would beat us count.
	const double ours = double(hero->getArmyStrength());
	std::vector<int3> threats;
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
		if(!e || !e->tempOwner.isValidPlayer() || e->tempOwner == playerID)
			continue;
		if(e->visitablePos().z != hero->visitablePos().z)
			continue;
		if(double(e->getArmyStrength()) < ours)
			continue;
		threats.push_back(e->visitablePos());
	}
	if(threats.empty())
		return false;

	auto cheb = [](const int3 & a, const int3 & b)
	{
		return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
	};
	const int3 hp = hero->visitablePos();
	int homeMind = INT_MAX;
	for(const int3 & t : threats)
		homeMind = std::min(homeMind, cheb(hp, t));

	// Where "away" points: the nearest town we own is walls, a garrison
	// and a siege the pursuer has to pay for, so equal-safety ties pull
	// toward it rather than across the map at random.
	int3 anchor = hp;
	int anchorDist = INT_MAX;
	for(const CGTownInstance * t : cb->getTownsInfo())
	{
		if(!t || t->tempOwner != playerID || t->visitablePos().z != hp.z)
			continue;
		const int d = cheb(hp, t->visitablePos());
		if(d < anchorDist)
		{
			anchorDist = d;
			anchor = t->visitablePos();
		}
	}

	// Standing still is a candidate too - only a real gain justifies the
	// walk, and a tile that ends next to a guard we cannot beat is not a
	// retreat, it is a different way to lose the hero.
	const long homeScore = long(homeMind) * 1000 - long(cheb(hp, anchor)) * 10;
	const int3 sizes = cb->getMapSize();
	int3 bestTile;
	long bestScore = LONG_MIN;
	int bestMind = 0;
	bool found = false;
	for(int y = 0; y < sizes.y; ++y)
		for(int x = 0; x < sizes.x; ++x)
		{
			const int3 p(x, y, hp.z);
			if(p == hp)
				continue;
			const CGPathNode * n = paths->getNode(p, EPathfindingLayer::LAND);
			if(!n || !n->reachable() || n->turns != 0)
				continue;
			if(isDeferredTile(p))
				continue;
			const CArmedInstance * g = strongestHostileAt(p, hero);
			if(g && !expectedToWin(hero, g, commitMargin(hero)))
				continue;
			int mind = INT_MAX;
			for(const int3 & t : threats)
				mind = std::min(mind, cheb(p, t));
			const long score = long(mind) * 1000
				- long(cheb(p, anchor)) * 10 + revealFrom(p);
			if(score > bestScore)
			{
				bestScore = score;
				bestTile = p;
				bestMind = mind;
				found = true;
			}
		}
	if(!found || bestScore <= homeScore)
		return false;

	std::vector<int3> steps = stepsToward(paths, hero, bestTile);
	if(steps.empty())
		return false;
	decisionLog_->line("   RETREAT to " + bestTile.toString()
		+ ", threat distance " + std::to_string(homeMind)
		+ " -> " + std::to_string(bestMind));
	// Fleeing a hero that beats us is not the wander the leash exists to
	// stop; L_duel r01 day 19 the leash refused exactly this.
	return walk(hero, steps, false, true);
}

void OmniAI::consolidateTownDefence()
{
	// VCMI 1.7.5 fights a town's VISITING hero outside the walls whenever a
	// garrison hero is also inside (CGTownInstance::isBattleOutsideTown:
	// defendingHero && getGarrisonHero() && defendingHero != getGarrisonHero()),
	// and the visitor defends first. No walls, no towers, no garrison troops:
	// the visitor's own army alone. siegeOnUs and every HOLD assumed the two
	// heroes and the walls fight together. N_duel r01 days 24-26: a fresh hire
	// stood in the door of a castle-walled town with a holder in the garrison,
	// was killed outside by Valeska each day (228, 126 and 62 army), and was
	// re-hired the next morning. So in a threatened town with two of our heroes,
	// the stronger takes the other's army and the garrison slot; the weaker is
	// left at the door with its fastest stack, to leave (escapeStrongerHero
	// no longer counts it as sheltered) or to cost the attacker a fight.
	if(!cb)
		return;
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID
			|| !threatenedTowns_.count(town->id.getNum()))
			continue;
		const CGHeroInstance * vis = town->getVisitingHero();
		const CGHeroInstance * gar = town->getGarrisonHero();
		if(vis && gar && vis->tempOwner == playerID && gar->tempOwner == playerID)
		{
			const bool visStronger = vis->getArmyStrength() > gar->getArmyStrength();
			const CGHeroInstance * strong = visStronger ? vis : gar;
			const CGHeroInstance * weak = visStronger ? gar : vis;
			if(weak->stacksCount() > 1 || visStronger)
			{
				if(weak->stacksCount() > 1)
					handOver(weak, strong);
				if(visStronger)
					cb->swapGarrisonHero(town);
				decisionLog_->line("  WALLS " + town->getNameTranslated() + ": "
					+ strong->getNameTranslated() + " holds the garrison with "
					+ std::to_string(uint64_t(strong->getArmyStrength())) + ", "
					+ weak->getNameTranslated() + " at the door would fight outside alone");
			}
		}

		// The door belongs to the field hero when it can come home today.
		// R3_duel r09 day 31: Yemapel held the garrison, the collector Kaite
		// (115) stood in the door, and Caitlin (19640) stood one tile
		// outside, unable to enter; Leyla (58233) killed Kaite and Caitlin
		// separately, with 40k of our army split across the wall. A visitor
		// with the garrison slot free steps into it; with both slots taken
		// the visitor (the one left with one stack after the hand-over)
		// walks out.
		vis = town->getVisitingHero();
		gar = town->getGarrisonHero();
		const CGHeroInstance * field = fieldHeroId_ >= 0
			? cb->getHero(ObjectInstanceID(fieldHeroId_)) : nullptr;
		if(!field || !vis || vis == field || gar == field || vis->tempOwner != playerID
			|| field->getArmyStrength() <= vis->getArmyStrength())
			continue;
		auto fp = pathCache_ ? pathCache_->getPathsInfo(field) : nullptr;
		CGPath probe;
		if(!fp || !const_cast<CPathsInfo *>(fp.get())->getPath(probe, town->visitablePos())
			|| probe.nodes.empty() || probe.nodes.front().turns != 0)
			continue;   // it cannot get in today anyway
		if(!gar)
		{
			decisionLog_->line("  DOOR " + town->getNameTranslated() + ": "
				+ vis->getNameTranslated() + " steps into the garrison so "
				+ field->getNameTranslated() + " can come in");
			cb->swapGarrisonHero(town);
		}
		else if(auto vp = pathCache_->getPathsInfo(vis))
		{
			CPathsInfo * vpi = const_cast<CPathsInfo *>(vp.get());
			// The field hero needs the door, not the visitor's troops: they
			// stay with the garrison hero, and the WALLS hand-over passes them
			// on once the field hero is inside.
			if(vis->stacksCount() > 1)
				handOver(vis, gar);
			if(retreatMove(vis, vpi))
				decisionLog_->line("  DOOR " + town->getNameTranslated() + ": "
					+ vis->getNameTranslated() + " leaves so "
					+ field->getNameTranslated() + " can come in");
			// Cornered: no tile farther from the attacker than the town, so
			// retreatMove stays and the door stays shut. R7t_duel r20 day 2
			// and r04 day 3, blue's town in the map's corner: the field hero
			// (4824, 9308) ended its day one tile outside ("spent movement
			// without leaving"), and the attacker killed the door hero and
			// the garrison hero in two fights and took the town. Any tile
			// out of the town frees the door.
			else if(const CGHeroInstance * foe = strongestThreatNear(town))
			{
				if(evacuateTown(vis, foe->visitablePos(), vpi, false))
					decisionLog_->line("  DOOR " + town->getNameTranslated() + ": "
						+ vis->getNameTranslated() + " steps aside so "
						+ field->getNameTranslated() + " can come in");
			}
		}
	}
}

bool OmniAI::escapeStrongerHero(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// Measured over 58 duel-v2 deaths of our field hero in fights Nullkiller
	// started (saved logs, September 24th): 25 happened in the open, and in
	// 20 of those the killer was already in sight at the start of our turn,
	// typically 2-12 tiles off. The field hero then spent the turn on a
	// march, a press or a pickup, or stood still, and was attacked where it
	// ended. Supports and collectors die the same way (45 of the 56 fights
	// Nullkiller started in the fog runs). Only the scoring pass ever asked
	// whether its END tile was inside a stronger hero's reach; nothing asked
	// whether the tile the hero already stands on is. A hero in a town is
	// left to defendThreatenedTown (walls, garrison, the rout check).
	if(!cb || !hero || !paths)
		return false;
	const int3 hp = hero->visitablePos();
	for(const CGTownInstance * t : cb->getTownsInfo())
		if(t && t->tempOwner == playerID && t->visitablePos() == hp)
		{
			// Behind the walls only as the hero who defends them: the garrison
			// hero, or a visitor with the garrison slot empty. A visitor in
			// front of a garrison hero is fought OUTSIDE, alone
			// (CGTownInstance::isBattleOutsideTown), so it is in the open.
			const CGHeroInstance * gh = t->getGarrisonHero();
			if(!gh || gh == hero)
				return false;
		}

	// Danger is the evaluator's call, not raw strength: a hero we beat with a
	// quarter of our army to spare is not one Nullkiller attacks with, and
	// fleeing it would be the camping this project keeps having to undo.
	// expectedToWin adds the +0.15 hero cushion, so 0.10 here asks for 0.25.
	constexpr double SAFE_SURVIVOR = 0.10;
	const CGHeroInstance * danger = nullptr;
	int dangerDist = 0;
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
		if(!e || !e->tempOwner.isValidPlayer() || e->tempOwner == playerID)
			continue;
		const int3 ep = e->visitablePos();
		if(ep.z != hp.z)
			continue;
		const int dist = std::max(std::abs(ep.x - hp.x), std::abs(ep.y - hp.y));
		// Its own movement allowance in straight tiles, the reach walk()
		// already uses for support heroes; generous on diagonals on purpose.
		if(dist > e->movementPointsLimit(true) / 100 + 1)
			continue;
		if(expectedToWin(hero, e, SAFE_SURVIVOR))
			continue;
		if(!danger || e->getArmyStrength() > danger->getArmyStrength())
		{
			danger = e;
			dangerDist = dist;
		}
	}
	if(!danger)
		return false;

	const std::string who = hero->getNameTranslated() + " (" + roleOf(hero) + ")";
	const std::string why = " from " + danger->getNameTranslated() + " ("
		+ std::to_string(uint64_t(danger->getArmyStrength())) + " vs our "
		+ std::to_string(uint64_t(hero->getArmyStrength())) + ", "
		+ std::to_string(dangerDist) + " tiles)";

	// Shelter: a town of ours reached today puts walls, towers and the
	// garrison between us and the attacker, and the recruit pass buys into
	// whoever stands in it. A door another of our heroes already holds
	// resolves as a meeting, not an entry, so it does not count.
	const CGTownInstance * shelter = nullptr;
	std::vector<int3> shelterSteps;
	bool shelterHandIn = false;
	for(const CGTownInstance * t : cb->getTownsInfo())
	{
		if(!t || t->tempOwner != playerID)
			continue;
		const CGHeroInstance * door = t->getVisitingHero();
		// A door our field hero holds is still the place for any other hero
		// to go: the step resolves as a meeting, and heroExchangeStarted
		// hands the carrier's army to the field hero inside the walls. L_duel
		// r01 days 19-20: four supports carrying 15.7k stood outside the door
		// Ken held, "nowhere better to stand", and died one at a time.
		// handOver leaves the carrier its fastest stack, so one stack left
		// means there is nothing more to hand in.
		const bool handIn = door && door != hero
			&& door->id.getNum() == fieldHeroId_ && hero->id.getNum() != fieldHeroId_
			&& hero->stacksCount() > 1;
		if(door && door != hero && !handIn)
			continue;
		// A hideout that falls is a trap for anyone but the field hero, whose
		// army is the town's defence. R6h_duel r04 day 2: two collectors
		// (80 and 126) escaped into Highcastle, which had no walls and no
		// garrison, and Lysander (10879) took the town with both inside.
		// Running on keeps the hero and costs Nullkiller its 500 experience.
		if(hero->id.getNum() != fieldHeroId_ && !handIn
			&& !defenseHolds(hero, t, danger))
			continue;
		CGPath probe;
		if(!paths->getPath(probe, t->visitablePos()) || probe.nodes.empty()
			|| probe.nodes.front().turns != 0)
			continue;
		std::vector<int3> steps = stepsToward(paths, hero, t->visitablePos());
		if(steps.empty() || safePrefix(hero, steps) != steps.size())
			continue;
		if(!shelter || steps.size() < shelterSteps.size())
		{
			shelter = t;
			shelterSteps = std::move(steps);
			shelterHandIn = handIn;
		}
	}
	// The hero's turn ends where the escape leaves it, so no later round
	// marches it back out; but only if it actually moved (the leash or a
	// fence can still refuse the walk), otherwise the normal chain runs.
	const ObjectInstanceID id = hero->id;
	const int3 before = hp;
	auto moved = [&]()
	{
		const CGHeroInstance * h = cb->getHero(id);
		return !h || h->visitablePos() != before;
	};
	if(shelter)
	{
		decisionLog_->line("   ESCAPE " + who + why + (shelterHandIn
			? ": handing the army in at " : ": into ") + shelter->getNameTranslated()
			+ ", " + std::to_string(shelterSteps.size()) + " steps");
		walk(hero, shelterSteps);
		if(moved())
		{
			stalledThisTurn_.insert(id.getNum());
			return true;
		}
	}
	else if(retreatMove(hero, paths) && moved())
	{
		decisionLog_->line("   ESCAPE " + who + why + ": no town in reach today, retreated");
		stalledThisTurn_.insert(id.getNum());
		return true;
	}
	decisionLog_->detail("in danger" + why + " and nowhere better to stand");
	return false;
}

bool OmniAI::supplyRun(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// Carry the week out to whoever is doing the fighting. The field hero's
	// army is otherwise capped at whatever it was holding when it left, while
	// growth piles up in a town it is nowhere near.
	//
	// Not while a town needs defending. The garrison job outranks the courier
	// job, because a town that falls while its defender is away carrying
	// troops is a failure we already fixed once today from the other side.
	if(!threatenedTowns_.empty())
		return false;
	if(fieldHeroId_ < 0 || hero->id.getNum() == fieldHeroId_)
		return false;

	// With three or more heroes the jobs split: one holds the town and does
	// not leave it, another carries. A hero that is the only non-field hero
	// standing in one of our towns is the holder, and a holder gone courier
	// is a post left open. With only two heroes the support hero still
	// doubles up, because otherwise the field hero never gets fed at all.
	if(cb->getHeroesInfo().size() > 2)
	{
		for(const CGTownInstance * town : cb->getTownsInfo())
		{
			if(!town || town->tempOwner != playerID)
				continue;
			if(town->getVisitingHero() != hero && town->getGarrisonHero() != hero)
				continue;
			bool shared = false;
			for(const CGHeroInstance * other : cb->getHeroesInfo())
			{
				if(!other || other == hero || other->id.getNum() == fieldHeroId_)
					continue;
				if(town->getVisitingHero() == other || town->getGarrisonHero() == other)
				{
					shared = true;
					break;
				}
			}
			if(!shared)
				return false;
		}
	}

	const CGHeroInstance * field = cb->getHero(ObjectInstanceID(fieldHeroId_));
	if(!field)
		return false;

	// Intercept where the field hero is going, not where it stands: a
	// courier aimed at its current tile chases a march forever. If a
	// conquest target is held, the field hero is walking to that town,
	// so meet it there. (Nullkiller routes the delivery TO the main
	// hero's path; this is the cheaper version of the same rendezvous.)
	int3 meetAt = field->visitablePos();
	const CGObjectInstance * marchDest = conquestTargetId_ >= 0
		? cb->getObj(ObjectInstanceID(conquestTargetId_), false) : nullptr;
	if(marchDest)
		meetAt = marchDest->visitablePos();
	std::vector<int3> steps = stepsToward(paths, hero, meetAt);
	if(steps.empty())
		return false;

	// Load the cargo before deciding the trip is worth it. A courier standing
	// in a town carries the week's recruits, not the few troops it walked in
	// with - only the field hero was pulling the garrison on a visit, so the
	// pool sat there while the field hero starved. Supply runs only happen
	// when no town is threatened, so emptying the garrison for delivery is
	// safe here.
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;
		if(town->getVisitingHero() == hero || town->getGarrisonHero() == hero)
			collectGarrison(town, hero);
	}

	// Worth the walk, on an absolute load - a week's recruits are worth a
	// courier trip however big the field hero has grown, because they
	// compound. The old bar asked the courier to already carry a fifth of
	// the field army, which no weekly pool can match once the field hero is
	// strong: delivery died exactly when the army plateaued. That was the
	// whole measured divergence.
	constexpr double CARRY_FLOOR = 1200.0;
	const double ours = double(hero->getArmyStrength());
	const double theirs = double(std::max<uint64_t>(1, field->getArmyStrength()));
	// An artifact is cargo whatever the army: the hand-over moves it onto the
	// field hero, where its attack or defense multiplies the whole army.
	if(ours < CARRY_FLOOR && ours < theirs * 0.10 && !carriesArtifacts(hero))
		return false;
	// A load the hand-over would not move is not cargo: stacks that fit
	// nowhere and are worth less than all of the field hero's. Before the
	// hand-over worked this never came up; with it, a courier left holding
	// the field hero's cast-offs would walk out to meet it every day.
	if(!wouldHandOver(hero, field) && !carriesArtifacts(hero))
		return false;

	std::ostringstream o;
	o << "   SUPPLY RUN to " << field->getNameTranslated() << " at "
	  << field->visitablePos().toString() << ", carrying " << uint64_t(ours)
	  << " against their " << uint64_t(theirs) << ", " << steps.size() << " steps";
	decisionLog_->line(o.str());
	return walk(hero, steps);
}

bool OmniAI::probeTowardMine(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// A mine we can see but cannot path to near home is a mine whose
	// approach is still fog: the pathfinder does not route through
	// unexplored tiles. R6l_duel r02 (blue, duel-v2): the sawmill at (31 1 0)
	// and the ore pits at (29 7 0) and (24 1 0) sit 3-8 tiles from the town
	// in a straight line, behind walls, on a ~17-step route through fog;
	// every pickup skipped them as "no route ... turns 255, tile visible to
	// us: yes" until day 6-7. Nullkiller, with the map revealed, paths there
	// on day 1. Walking at the nearest one uncovers the approach.
	if(!cb || !hero || !paths)
		return false;
	constexpr int NEAR_HOME = 12;
	const int3 hp = hero->visitablePos();
	const CGObjectInstance * best = nullptr;
	int bestD = INT_MAX;
	// The plan first: each hero its own mine. Two heroes used to probe the
	// same mine on the same day (R7t_duel r03 day 2, the Sawmill at (6 1 0)),
	// each choosing the nearest in a straight line for itself.
	planMineProbes();
	std::set<int32_t> plannedForOthers;
	for(const auto & kv : minePlan_)
		if(kv.first != hero->id.getNum())
			plannedForOthers.insert(kv.second);
	const auto mine = minePlan_.find(hero->id.getNum());
	if(mine != minePlan_.end())
		best = cb->getObj(ObjectInstanceID(mine->second), false);
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		if(best)
			break;
		if(!obj || obj->ID != Obj::MINE || obj->tempOwner == playerID
			|| plannedForOthers.count(obj->id.getNum()))
			continue;
		const int3 op = obj->visitablePos();
		if(op.z != hp.z)
			continue;
		const int d = std::max(std::abs(op.x - hp.x), std::abs(op.y - hp.y));
		if(d > NEAR_HOME || guardStrengthAt(op, hero) > 0)
			continue;   // a guarded mine is the field hero's fight, not a scouting job
		CGPath probe;
		if(paths->getPath(probe, op))
			continue;   // reachable: the pickups and the mine run take it
		if(d < bestD)
		{
			bestD = d;
			best = obj;
		}
	}
	if(!best)
		return false;
	// Walk the route through the fog, not at the tile nearest the mine in a
	// straight line. On a maze that tile is a dead end: duel-v2's ore pit
	// four tiles from the blue gate is 27 steps by its only route, round
	// through the map centre, and the old probe walked to the same dead end
	// (31 9 0) on eight days of R6n_duel r02 while the mine waited until
	// day 13. Simulated on the map, the frontier walk knows the whole route
	// after about 15 tiles.
	int3 frontier;
	if(fogFrontierToward(hero, paths, best->visitablePos(), frontier))
	{
		std::vector<int3> steps = stepsToward(paths, hero, frontier);
		if(!steps.empty())
		{
			decisionLog_->line("   PROBE toward " + std::string(best->getObjectName())
				+ " at " + best->visitablePos().toString() + ", route through fog, "
				+ std::to_string(steps.size()) + " steps to the fog edge at "
				+ frontier.toString());
			if(walk(hero, steps))
				return true;
		}
	}
	return pressToward(hero, paths, best->visitablePos(),
		"   PROBE toward " + std::string(best->getObjectName()) + " at "
			+ best->visitablePos().toString() + ", its approach is still fog");
}

void OmniAI::planMineProbes()
{
	// Nullkiller flags about one and a half mines a day from day 3 of
	// duel-v2 with or without its map reveal (R6n_fog: 7.5 on day 7; R6n_duel
	// 7.0), and we flagged our first on day 4-6 (week1_profile.py over R7p and
	// R7t). Our week one was fog probes and pickups, each hero choosing its
	// own nearest mine by straight line. This pairs heroes with mines by the
	// route through the fog (crossingField: fog open, seen ground as it is),
	// shortest pair first, one mine per hero, within about two days' walk.
	if(!cb)
		return;
	const int today = cb->getDate(Date::DAY);
	if(minePlanDay_ == today && minePlanRound_ == currentRound_)
		return;
	const bool firstOfDay = minePlanDay_ != today;
	minePlanDay_ = today;
	minePlanRound_ = currentRound_;
	minePlan_.clear();
	constexpr int MAX_ROUTE = 30;
	const int3 size = cb->getMapSize();
	std::set<const CGHeroInstance *> garrisoned;
	for(const CGTownInstance * town : cb->getTownsInfo())
		if(town && town->getGarrisonHero())
			garrisoned.insert(town->getGarrisonHero());
	std::vector<const CGHeroInstance *> heroes;
	for(const CGHeroInstance * h : cb->getHeroesInfo())
		if(h && h->tempOwner == playerID && h->id.getNum() != fieldHeroId_
			&& h->id.getNum() != secondFieldId_ && !garrisoned.count(h))
			heroes.push_back(h);
	if(heroes.empty())
		return;
	struct Pair { int len; int32_t hero; int32_t mine; };
	std::vector<Pair> pairs;
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		if(!obj || obj->ID != Obj::MINE || obj->tempOwner == playerID)
			continue;
		const int3 op = obj->visitablePos();
		std::vector<int> field;
		for(const CGHeroInstance * h : heroes)
		{
			const int3 hp = h->visitablePos();
			if(hp.z != op.z || std::max(std::abs(op.x - hp.x), std::abs(op.y - hp.y)) > MAX_ROUTE)
				continue;
			if(guardStrengthAt(op, h) > 0)
				continue;   // a guarded mine is the field hero's fight (PRIZE)
			auto shared = pathCache_ ? pathCache_->getPathsInfo(h) : nullptr;
			if(!shared)
				continue;
			CGPath probe;
			if(const_cast<CPathsInfo *>(shared.get())->getPath(probe, op))
				continue;   // reachable already: the pickups and the mine run take it
			if(field.empty())
				field = crossingField(op, false);
			const int len = field[size_t(hp.y) * size_t(size.x) + size_t(hp.x)];
			if(len < 0 || len > MAX_ROUTE)
				continue;
			pairs.push_back({len, h->id.getNum(), obj->id.getNum()});
		}
	}
	std::sort(pairs.begin(), pairs.end(), [](const Pair & a, const Pair & b) { return a.len < b.len; });
	std::set<int32_t> heroTaken, mineTaken;
	std::ostringstream o;
	std::string pairsJson = "[";
	for(const Pair & pr : pairs)
	{
		if(heroTaken.count(pr.hero) || mineTaken.count(pr.mine))
			continue;
		minePlan_[pr.hero] = pr.mine;
		heroTaken.insert(pr.hero);
		mineTaken.insert(pr.mine);
		const CGHeroInstance * h = cb->getHero(ObjectInstanceID(pr.hero));
		const CGObjectInstance * m = cb->getObj(ObjectInstanceID(pr.mine), false);
		if(!h || !m)
			continue;
		o << (o.tellp() > 0 ? "; " : "") << h->getNameTranslated() << " -> " << m->getObjectName()
		  << " " << m->visitablePos().toString() << " (" << pr.len << " by route)";
		if(pairsJson.size() > 1)
			pairsJson += ",";
		pairsJson += JsonObj().str("hero", h->getNameTranslated()).pos("from", h->visitablePos())
			.str("mine", m->getObjectName()).pos("mine_pos", m->visitablePos()).integer("route", pr.len).done();
	}
	if(firstOfDay && !minePlan_.empty())
	{
		decisionLog_->line("  MINE PLAN " + o.str());
		decisionLog_->event("mine_plan", JsonObj().raw("pairs", pairsJson + "]").done());
	}
}

std::vector<int> OmniAI::crossingField(const int3 & target, bool water) const
{
	const int3 size = cb->getMapSize();
	const int width = size.x, height = size.y, z = target.z;
	std::vector<int> dist(size_t(width) * size_t(height), -1);
	auto index = [width](const int3 & q) { return q.y * width + q.x; };
	// Is an unseen tile beside known water? For the land-only field such a
	// tile is taken for more sea: the fog past the visible ring round an
	// island would otherwise read as a land bridge to anywhere.
	auto besideWater = [&](const int3 & q)
	{
		for(int dx = -1; dx <= 1; ++dx)
			for(int dy = -1; dy <= 1; ++dy)
			{
				const int3 r(q.x + dx, q.y + dy, q.z);
				if((!dx && !dy) || !cb->isInTheMap(r) || !cb->isVisible(r))
					continue;
				const TerrainTile * t = cb->getTile(r, false);
				if(t && t->isWater())
					return true;
			}
		return false;
	};
	auto open = [&](const int3 & q)
	{
		if(!cb->isVisible(q))
			return water || !besideWater(q);
		const TerrainTile * tile = cb->getTile(q, false);
		if(!tile)
			return true;
		if(tile->isWater() && !water)
			return false;
		// A visitable object's tile ends a walk and the next day goes on
		// from it; an obstacle or an object's body does not open at all.
		return !tile->blocked() || tile->visitable();
	};
	std::deque<int3> queue;
	dist[index(target)] = 0;
	queue.push_back(target);
	while(!queue.empty())
	{
		const int3 q = queue.front();
		queue.pop_front();
		for(int dx = -1; dx <= 1; ++dx)
			for(int dy = -1; dy <= 1; ++dy)
			{
				if(!dx && !dy)
					continue;
				const int3 r(q.x + dx, q.y + dy, z);
				if(r.x < 0 || r.y < 0 || r.x >= width || r.y >= height)
					continue;
				if(dist[index(r)] >= 0 || !open(r))
					continue;
				dist[index(r)] = dist[index(q)] + 1;
				queue.push_back(r);
			}
	}
	return dist;
}

bool OmniAI::seekCrossing(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// OmniAI never used to get a boat. It rode one only when the pathfinder
	// happened to route through a free boat already on our shore (VCMI's
	// gameConfig pathfinder.useBoat); it never bought one, never cast Summon
	// Boat, never built the town Shipyard. A start on its own island (the
	// MapGen Islands layouts, stock maps such as Thousand Islands) could
	// never meet the enemy. VCMI's pathfinder treats unexplored tiles as
	// blocked on water as on land, so the way across is found the way the
	// mine probe finds a way through fog: optimistic step counts from the
	// target, fog open, and the reachable tile nearest by them.
	if(!cb || !hero || !paths || !decisionLog_)
		return false;
	const int3 hp = hero->visitablePos();

	// Where to: the nearest hostile town we know of, else the start town
	// the map declares for the enemy.
	int3 target(-1, -1, -1);
	int best = INT_MAX;
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		if(!obj || obj->ID != Obj::TOWN || !obj->tempOwner.isValidPlayer()
			|| cb->getPlayerRelations(playerID, obj->tempOwner) != PlayerRelations::ENEMIES)
			continue;
		const int3 op = obj->visitablePos();
		const int d = std::max(std::abs(op.x - hp.x), std::abs(op.y - hp.y));
		if(op.z == hp.z && d < best)
		{
			best = d;
			target = op;
		}
	}
	if(!target.isValid() && enemyStartValid_)
		target = enemyStart_;
	if(!target.isValid() || target.z != hp.z)
		return false;
	CGPath direct;
	if(paths->getPath(direct, target))
		return false;   // a route exists; the march and the press take it

	const std::vector<int> wet = crossingField(target, true);
	const int3 size = cb->getMapSize();
	auto index = [&](const int3 & q) { return q.y * size.x + q.x; };
	if(wet[index(hp)] < 0)
		return false;   // nothing across the sea either
	// Is the crossing due now? Only when no land route can exist even
	// through the fog, fog beside known water counted as more sea: a land
	// map's lake is walked round, unexplored land may still hold a way, and
	// a fully explored island has none. (A land-only field with all fog
	// counted open read every unseen stretch of coast as a land bridge, and
	// the R6z water test never crossed.)
	const std::vector<int> dry = crossingField(target, false);
	if(dry[index(hp)] >= 0)
	{
		// Once a day, say where the land route that keeps us ashore runs:
		// down its step counts to the first unseen tile, the place to look.
		const int today = cb->getDate(Date::DAY);
		if(crossingNoteDay_ != today && !cb->isVisible(target))
		{
			crossingNoteDay_ = today;
			int3 q = hp;
			for(int guard = 0; guard < size.x * size.y && q != target && cb->isVisible(q); ++guard)
			{
				int3 next = q;
				for(int dx = -1; dx <= 1; ++dx)
					for(int dy = -1; dy <= 1; ++dy)
					{
						const int3 r(q.x + dx, q.y + dy, q.z);
						if((!dx && !dy) || !cb->isInTheMap(r))
							continue;
						if(dry[index(r)] >= 0 && dry[index(r)] < dry[index(next)])
							next = r;
					}
				if(next == q)
					break;
				q = next;
			}
			decisionLog_->detail("crossing to " + target.toString() + " not due: a land route of "
				+ std::to_string(dry[index(hp)]) + " steps may exist (by sea "
				+ std::to_string(wet[index(hp)]) + "); it first leaves what we have seen at "
				+ q.toString());
			if(!cb->isVisible(q))
			{
				crossingLookAt_ = q;
				crossingLookDay_ = today;
			}
		}
		return false;
	}

	// Progress: the tile this hero reaches, on foot or by a boat the
	// pathfinder can already use, that is fewest steps from the target.
	int3 bestTile(-1, -1, -1);
	int bestD = wet[index(hp)];
	uint8_t bestTurns = 255;
	for(int y = 0; y < size.y; ++y)
		for(int x = 0; x < size.x; ++x)
		{
			const int3 q(x, y, hp.z);
			const int d = wet[index(q)];
			if(d < 0 || d > bestD || !cb->isVisible(q))
				continue;
			const CGPathNode * n = paths->getNode(q, EPathfindingLayer::LAND);
			if(!n || !n->reachable())
				n = paths->getNode(q, EPathfindingLayer::SAIL);
			if(!n || !n->reachable())
				continue;
			if(d < bestD || (d == bestD && n->turns < bestTurns))
			{
				bestD = d;
				bestTile = q;
				bestTurns = n->turns;
			}
		}
	if(bestTile.isValid() && bestTile != hp)
	{
		std::vector<int3> steps = stepsToward(paths, hero, bestTile);
		if(!steps.empty())
		{
			decisionLog_->line("   CROSSING toward " + target.toString() + ": "
				+ std::to_string(steps.size()) + " steps to (" + bestTile.toString()
				+ "), " + std::to_string(bestD) + " from the target"
				+ (hero->inBoat() ? ", at sea" : ""));
			// The leash keeps a field hero near home on a small map; the
			// crossing is how this map is played at all.
			if(walk(hero, steps, false, true))
				return true;
		}
	}

	// Stuck at the water's edge with no boat the pathfinder can use.
	const int today = cb->getDate(Date::DAY);
	if(hero->inBoat() || boatAskedDay_ == today)
		return false;

	// Summon Boat from where the hero stands, if it is on the shore.
	if(hero->hasSpellbook() && hero->bestLocation().isValid())
	{
		const spells::Spell * spell = LIBRARY->spells()->getById(SpellID(SpellID::SUMMON_BOAT));
		if(spell && hero->getSpellsInSpellbook().count(SpellID(SpellID::SUMMON_BOAT))
			&& hero->canCastThisSpell(spell) && hero->mana >= hero->getSpellCost(spell))
		{
			boatAskedDay_ = today;
			decisionLog_->line("   SUMMON BOAT " + hero->getNameTranslated() + " at "
				+ hp.toString() + " toward " + target.toString());
			cb->castSpell(hero, SpellID(SpellID::SUMMON_BOAT));
			return true;
		}
	}

	// A shipyard on this landmass: one of our towns with the Shipyard
	// built, or a Shipyard object nobody hostile holds. The boat appears on
	// the water beside it, where the pathfinder finds it next round.
	const TResources purse = cb->getResourceAmount();
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		if(!obj || (obj->ID != Obj::TOWN && obj->ID != Obj::SHIPYARD))
			continue;
		if(obj->tempOwner.isValidPlayer() && obj->tempOwner != playerID
			&& cb->getPlayerRelations(playerID, obj->tempOwner) == PlayerRelations::ENEMIES)
			continue;
		if(obj->ID == Obj::TOWN && obj->tempOwner != playerID)
			continue;
		const auto * yard = dynamic_cast<const IShipyard *>(obj);
		if(!yard || yard->shipyardStatus() != IBoatGenerator::GOOD)
			continue;
		CGPath toYard;
		if(obj->visitablePos() != hp && !paths->getPath(toYard, obj->visitablePos()))
			continue;   // on another landmass
		TResources cost;
		yard->getBoatCost(cost);
		if(!purse.canAfford(cost))
			continue;
		boatAskedDay_ = today;
		decisionLog_->line("   BUILD BOAT at " + obj->getObjectName() + " "
			+ obj->visitablePos().toString() + " for " + hero->getNameTranslated()
			+ ", toward " + target.toString());
		cb->buildBoat(yard);
		return true;
	}

	// None: one of our coastal towns on this landmass raises its Shipyard.
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID || town->hasBuilt(BuildingID::SHIPYARD))
			continue;
		CGPath toTown;
		if(town->visitablePos() != hp && !paths->getPath(toTown, town->visitablePos()))
			continue;
		if(cb->canBuildStructure(town, BuildingID::SHIPYARD) != EBuildingState::ALLOWED)
			continue;
		boatAskedDay_ = today;
		decisionLog_->line("   SHIPYARD raised in " + town->getNameTranslated()
			+ ": the way to " + target.toString() + " is across water");
		cb->buildBuilding(town, BuildingID(BuildingID::SHIPYARD));
		return true;
	}
	decisionLog_->detail("crossing to " + target.toString()
		+ " needs a boat and none can be had today");
	return false;
}

bool OmniAI::fogFrontierToward(const CGHeroInstance * hero, CPathsInfo * paths,
	const int3 & target, int3 & frontier) const
{
	// VCMI's pathfinder treats unexplored tiles as blocked
	// (PathfinderUtil::evaluateAccessibility), so a target behind fog has no
	// route at all and "the reachable tile nearest to it" is a straight-line
	// guess. Search instead as if unexplored ground were open and explored
	// ground were exactly what the pathfinder says, then stop at the edge of
	// what is known. Each walk there uncovers the next stretch and the next
	// call re-plans on it.
	if(!cb || !hero || !paths)
		return false;
	const int3 start = hero->visitablePos();
	if(start.z != target.z)
		return false;
	const int3 size = cb->getMapSize();
	const int width = size.x, height = size.y, z = start.z;
	auto index = [width](const int3 & q) { return q.y * width + q.x; };
	auto open = [&](const int3 & q)
	{
		if(q == target || !cb->isVisible(q))
			return true;
		const CGPathNode * n = paths->getNode(q, EPathfindingLayer::LAND);
		return n && (n->accessible == EPathAccessibility::ACCESSIBLE
			|| n->accessible == EPathAccessibility::VISITABLE
			|| n->accessible == EPathAccessibility::GUARDED);
	};
	std::vector<int> prev(size_t(width) * size_t(height), -2);
	std::deque<int3> queue;
	prev[index(start)] = -1;
	queue.push_back(start);
	bool found = false;
	while(!queue.empty() && !found)
	{
		const int3 q = queue.front();
		queue.pop_front();
		for(int dx = -1; dx <= 1 && !found; ++dx)
			for(int dy = -1; dy <= 1; ++dy)
			{
				if(!dx && !dy)
					continue;
				const int3 r(q.x + dx, q.y + dy, z);
				if(r.x < 0 || r.y < 0 || r.x >= width || r.y >= height)
					continue;
				if(prev[index(r)] != -2 || !open(r))
					continue;
				prev[index(r)] = index(q);
				if(r == target)
				{
					found = true;
					break;
				}
				queue.push_back(r);
			}
	}
	if(!found)
		return false;
	std::vector<int3> route;
	for(int i = index(target); i >= 0; i = prev[i])
		route.emplace_back(i % width, i / width, z);
	std::reverse(route.begin(), route.end());
	frontier = int3(-1, -1, -1);
	for(size_t i = 1; i < route.size(); ++i)
	{
		if(!cb->isVisible(route[i]))
			break;
		const CGPathNode * n = paths->getNode(route[i], EPathfindingLayer::LAND);
		if(n && n->reachable())
			frontier = route[i];
	}
	return frontier.isValid() && frontier != start;
}

bool OmniAI::grabScarceMine(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// Short enough that the build queue is feeling it. updateResourceScarcity
	// runs 1.0 at plenty up to 4.0 at nothing (higher still when the same
	// resource is also blocking a building), so 2.0 is a low bar against
	// that range, not a strict one.
	constexpr double REALLY_SHORT = 2.0;

	if(!cb || !hero || !paths)
		return false;

	// What a building we cannot afford is actually short of. Scarcity
	// alone sent the hero after mercury: the Alchemist's Lab was ONE step
	// away and the Ore Pit seventeen, so 2.80/1 beat 4.00/17 four times
	// over ten days while the fort that gates every dwelling in the town
	// waited on ore. ENOUGH treats all seven resources as equally worth
	// having and only weights how short of them we are; it has no idea
	// that nothing in the build list is made of mercury.
	const std::array<bool, 7> wantedByABlockedBuilding = blockedBuildingResources();
	bool anyWanted = false;
	for(bool w : wantedByABlockedBuilding)
		anyWanted = anyWanted || w;

	const double ours = double(std::max<uint64_t>(1, hero->getArmyStrength()));
	const CGObjectInstance * best = nullptr;
	std::vector<int3> bestSteps;
	double bestWorth = 0.0;
	double bestScarcity = 0.0;

	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		if(!obj || obj->ID != Obj::MINE || obj->tempOwner == playerID)
			continue;
		const auto * mine = dynamic_cast<const CGMine *>(obj);
		if(!mine)
			continue;
		const int res = mine->producedResource.getNum();
		// An unguarded mine this hero reaches today is income for the rest of
		// the game for part of one day's walk, whatever the build list wants
		// this morning. R6i_duel r01: the sawmill and both ore pits 4-8
		// tiles from home stood unflagged for six days because the blocked
		// building of the day was waiting on gold; Nullkiller flags 5-7
		// mines by day 7, we flagged 1-2.
		bool cheapToday = false;
		if(guardStrengthAt(obj->visitablePos(), hero) == 0)
		{
			CGPath today;
			cheapToday = paths->getPath(today, obj->visitablePos())
				&& !today.nodes.empty() && today.nodes.front().turns == 0;
		}
		if(!cheapToday)
		{
			if(res < 0 || res >= int(scarcity_.size())
				|| scarcity_[res] < REALLY_SHORT)
				continue;   // a gold mine while the queue starves for wood is not this
			// When something in a town is waiting on a resource, that resource
			// is the job. Distance stops deciding it.
			if(anyWanted && !wantedByABlockedBuilding[res])
				continue;
		}
		// A guarded mine is priced like a PRIZE task: the calibrated fight
		// against what the mine is worth, scaled by how short we are of what
		// it makes. The old bar (guard under 0.8 of our strength, 1.25:1
		// odds) sent R6v r01's field hero at the Cerberi guarding an
		// Alchemist's Lab on day 3 at a predicted 0.77: it kept 0.54, and
		// Nullkiller's first hero took the weakened army and the town on
		// day 4.
		if(guardStrengthAt(obj->visitablePos(), hero) > 0)
		{
			const CArmedInstance * guard = strongestHostileAt(obj->visitablePos(), hero);
			if(!guard)
				continue;
			const omniai::CombatVerdict v = combatVerdict(hero, guard);
			const double shortOf = (res >= 0 && res < int(scarcity_.size()))
				? std::max(1.0, scarcity_[res]) : 1.0;
			if(!v.win || v.margin < commitMargin(hero)
				|| (1.0 - v.margin) * ours > guardedPrizeWorth(obj) * shortOf)
				continue;   // that is a fight, not a mine
		}
		std::vector<int3> steps = stepsToward(paths, hero, obj->visitablePos());
		if(steps.empty() || safePrefix(hero, steps) == 0)
			continue;   // no route, or its first step is a fight walk() refuses
		// The leash judges where today's walk ends, not the mine: a route
		// around a wall can leave a mine four tiles from home unreachable
		// inside the leash (K_duel r01, Ore Pit (6 7 0), days 4-11).
		if(!leashAllows(hero, hero->convertToVisitablePos(steps.back())))
			continue;
		// Nearest wins among things we are equally short of, and being
		// shorter breaks the tie.
		const double worth = std::max(1.0, (res >= 0 && res < int(scarcity_.size())) ? scarcity_[res] : 1.0)
			/ double(steps.size());
		if(worth > bestWorth)
		{
			bestWorth = worth;
			bestScarcity = scarcity_[res];
			best = obj;
			bestSteps = std::move(steps);
		}
	}
	if(!best)
		return false;

	std::ostringstream o;
	o << "   MINE RUN to " << best->getObjectName() << " at "
	  << best->visitablePos().toString() << ", " << bestSteps.size()
	  << " steps, " << (anyWanted ? "wanted, " : "")
	  << "scarcity " << std::fixed << std::setprecision(2)
	  << bestScarcity;
	decisionLog_->line(o.str());
	targetedThisTurn_.insert(best->id.getNum());
	return walk(hero, bestSteps);
}
bool OmniAI::holdTheFort(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// The home hero's entire job. Stand in a town we own, or walk to the
	// nearest one and stand in that. No exploring, no objects, no fights it
	// goes looking for.
	//
	// Standing still is the point rather than a failure to act on it:
	// recruitInAllTowns buys into whoever is in the town, collectGarrison
	// hands over anything bought while nobody was home, and a town with a
	// defender in it does not fall to a hero walking past. Its army grows
	// every week without it going anywhere.
	//
	// How many of OUR other heroes are already standing in this town. Two
	// heroes holding one post while a second town stands open is a wasted
	// hero, so open posts win and shared ones are only a fallback.
	auto occupants = [this, hero](const CGTownInstance * town)
	{
		int n = 0;
		for(const CGHeroInstance * other : cb->getHeroesInfo())
		{
			if(!other || other == hero)
				continue;
			if(town->getVisitingHero() == other || town->getGarrisonHero() == other)
				++n;
		}
		return n;
	};

	const CGTownInstance * best = nullptr;
	std::vector<int3> bestSteps;
	const CGTownInstance * shared = nullptr;
	std::vector<int3> sharedSteps;
	const CGTownInstance * current = nullptr;

	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;
		if(town->getVisitingHero() == hero || town->getGarrisonHero() == hero)
		{
			current = town;
			if(occupants(town) == 0)
			{
				// The visitor slot is the town's only door. A holder
				// parked on it walls the doorway for everyone else: any
				// hero's walk home resolves as a blocking hero-visit that
				// spends the step and moves nobody, so the field hero can
				// never actually reach the pool inside. Step the holder
				// into the garrison, where it defends the town without
				// standing in the way. The swap folds the town's garrison
				// stacks onto the hero (CGameHandler.cpp:2588-2597), so the
				// pool moves with it; collectGarrison pulls a garrisoned
				// holder's army as readily as the town's own slots.
				if(town->getVisitingHero() == hero)
					cb->swapGarrisonHero(town);
				// Park what it is carrying where the field hero can reach
				// it. A holder is not going anywhere with an army, and the
				// garrison defends this town just as well: the only thing
				// that changes is that the pool becomes collectable.
				// Safe from ping-pong because recruitInAllTowns now hands
				// the garrison back only to the field hero.
				//
				// NEVER strip a holder carrying more than the hero we
				// expect to fight with. assignHeroRoles will not pick a
				// hero standing in one of our towns as a fresh field hero,
				// deliberately, so the strongest hero on the board is often
				// a holder; taking its army off it and leaving that in a
				// garrison the weaker field hero has to walk back for is
				// the same mistake pointing the other way. Measured without
				// this guard: 10 of 13 duel-v2 games ended on day 3, where
				// the baseline's worst was day 4.
				const CGHeroInstance * field = fieldHeroId_ >= 0
					? cb->getHero(ObjectInstanceID(fieldHeroId_)) : nullptr;
				if(!field || hero->getArmyStrength() <= field->getArmyStrength())
					depositArmy(hero, town);
				decisionLog_->line("   GARRISON " + town->getNameTranslated()
					+ ", " + hero->getNameTranslated() + " is holding it");
				return true;
			}
			// Somebody else already holds this one, and we are standing in
			// its only door. That is not a post: with the garrison slot taken
			// the visitor slot is the entrance, and a second hero parked on it
			// turns every other hero's walk in into a blocking visit. C_duel
			// r02 (blue) kept its field hero one tile outside for 80 days
			// that way while Nullkiller grew to 406k. Step off and do
			// something else.
			if(town->getVisitingHero() == hero && town->getGarrisonHero()
				&& town->getGarrisonHero() != hero)
				return false;
			// In the garrison slot with a visitor beside us: the garrison
			// slot IS the post, whoever is passing through the door.
			if(town->getGarrisonHero() == hero)
			{
				decisionLog_->line("   GARRISON " + town->getNameTranslated()
					+ ", " + hero->getNameTranslated() + " is holding it");
				return true;
			}
			continue;
		}
		std::vector<int3> steps = stepsToward(paths, hero, town->visitablePos());
		if(steps.empty())
			continue;
		// A frontier town that was just taken or sits under a threat is the
		// one a standing hero is actually for: a safe interior capital holds
		// on its own garrison, so the holder belongs on the exposed one.
		const bool exposed = freshlyCaptured_.count(town->id.getNum())
			|| threatenedTowns_.count(town->id.getNum());
		if(occupants(town) == 0)
		{
			const bool bestExposed = best
				&& (freshlyCaptured_.count(best->id.getNum())
					|| threatenedTowns_.count(best->id.getNum()));
			if(!best || (exposed && !bestExposed)
				|| (exposed == bestExposed && steps.size() < bestSteps.size()))
			{
				best = town;
				bestSteps = std::move(steps);
			}
		}
		else if(!shared || steps.size() < sharedSteps.size())
		{
			shared = town;
			sharedSteps = std::move(steps);
		}
	}

	if(!best)
	{
		// No open post to take. A post somebody already holds has one free
		// slot left, the visitor slot, and that slot is the town's door: a
		// hero parked there blocks every other hero walking in (the 80-day
		// field-hero deadlock above). A spare hero with no open post is a
		// spare pair of boots, so it falls through to the pickup chain.
		(void)shared;
		(void)sharedSteps;
		return false;
	}

	// A weak holder committed to a multi-day walk toward an exposed town is
	// exactly how one gets caught in the open before arriving - the enemy
	// moves too, and the path above has no idea a hostile hero might reach
	// the same ground first. Measured (DEVELOPMENT_LOG.md, this build's own
	// trace): a holder walking 8 steps toward a threatened town was
	// intercepted and destroyed before it got there, at odds of 0.01,
	// while our field hero was untouched elsewhere. Only correct the
	// exposed choice when it is not reachable THIS turn and a safe
	// alternative exists - an exposed post reachable now is still the
	// right call (multi-day exposure in the open is the specific failure,
	// not the exposed town itself), and a hero with no safe option left
	// has nowhere better to go anyway. getCost(), not steps.size(): a path
	// node's cost is real movement points, the same unit
	// movementPointsRemaining() reports; step count is tile count and
	// compares nothing meaningful against it.
	const bool bestExposed = freshlyCaptured_.count(best->id.getNum())
		|| threatenedTowns_.count(best->id.getNum());
	if(bestExposed)
	{
		const CGPathNode * n = paths->getNode(best->visitablePos(), EPathfindingLayer::LAND);
		const bool reachesThisTurn = n
			&& double(n->getCost()) <= double(hero->movementPointsRemaining());
		if(!reachesThisTurn)
		{
			const CGTownInstance * safe = nullptr;
			std::vector<int3> safeSteps;
			for(const CGTownInstance * town : cb->getTownsInfo())
			{
				if(!town || town->tempOwner != playerID || town == best)
					continue;
				if(freshlyCaptured_.count(town->id.getNum())
					|| threatenedTowns_.count(town->id.getNum()))
					continue;   // still exposed, not the safe fallback we want
				if(occupants(town) != 0)
					continue;   // already held
				std::vector<int3> steps = stepsToward(paths, hero, town->visitablePos());
				if(steps.empty())
					continue;
				if(!safe || steps.size() < safeSteps.size())
				{
					safe = town;
					safeSteps = std::move(steps);
				}
			}
			if(safe)
			{
				decisionLog_->detail(best->getNameTranslated()
					+ " is exposed and too far to reach this turn; heading to "
					+ safe->getNameTranslated() + " instead, out of the open");
				best = safe;
				bestSteps = std::move(safeSteps);
			}
		}
	}

	std::ostringstream o;
	o << "   TO POST at " << best->getNameTranslated() << ", "
	  << bestSteps.size() << " steps";
	decisionLog_->line(o.str());
	targetedThisTurn_.insert(best->id.getNum());
	return walk(hero, bestSteps);
}

bool OmniAI::pocketHold(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// Sealed-pocket defense: the seal did not open, so the field stack
	// belongs on the town - the enemy that comes through meets the whole
	// army instead of an empty shell. Garrison in place when home (the
	// doorway stays free for couriers), walk home when out, decline when
	// no owned town is reachable. Unlike holdTheFort the army stays on
	// the hero: a parked field stack that gets sieged fights behind the
	// walls, and weekly recruits merge into it there.
	if(!cb || !hero || !paths)
		return false;
	const CGTownInstance * best = nullptr;
	std::vector<int3> bestSteps;
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;
		if(town->getGarrisonHero() == hero)
			return true;   // already parked
		if(town->getVisitingHero() == hero
			|| hero->visitablePos() == town->visitablePos())
		{
			if(town->getVisitingHero() == hero)
				cb->swapGarrisonHero(town);
			decisionLog_->line("   POCKET HOLD garrisoned in "
				+ town->getNameTranslated());
			return true;
		}
		// Another of our heroes already stands in the door: walking onto it is
		// a hero exchange, not an entry, and the stall guard then ends the
		// turn. Doing that every morning is the 80-day deadlock.
		if(const CGHeroInstance * door = town->getVisitingHero())
			if(door != hero && door->tempOwner == playerID)
				continue;
		std::vector<int3> steps = stepsToward(paths, hero, town->visitablePos());
		if(!steps.empty() && (!best || steps.size() < bestSteps.size()))
		{
			best = town;
			bestSteps = std::move(steps);
		}
	}
	if(!best)
		return false;
	decisionLog_->line("   POCKET HOLD to " + best->getNameTranslated()
		+ ", " + std::to_string(bestSteps.size()) + " steps");
	targetedThisTurn_.insert(best->id.getNum());
	return walk(hero, bestSteps);
}

bool OmniAI::holderMayCollect(const CGHeroInstance * hero) const
{
	// A holder stands in a town so the town is not empty. That is worth a
	// hero while an enemy is near and worth little otherwise: Nullkiller runs
	// every hero it owns on pickups (its digests show three heroes chaining
	// resources, mines and chests from day 1) and flags ~10 mines by day 10
	// on duel-v2 against our 3-4 with one collector (TRUTH, F_duel r01). So
	// a holder whose town is not threatened, and still has a garrison once
	// it steps out, runs the pickup circuit. The threat scan runs first
	// every round, and defendThreatenedTown brings it back.
	if(!cb || !hero || !threatenedTowns_.empty())
		return false;
	if(hero->id.getNum() == fieldHeroId_ || hero->id.getNum() == secondFieldId_)
		return false;
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;
		if(town->getGarrisonHero() != hero && town->getVisitingHero() != hero)
			continue;
		if(town->stacksCount() == 0)
			return false;   // leaving would hand the town to the first scout
	}
	return true;
}

bool OmniAI::collectorCircuit(const CGHeroInstance * hero, CPathsInfo * paths)
{
	if(!cb || !hero || !paths)
		return false;

	const int week = cb->getDate(Date::DAY) / 7;
	const double ours = double(std::max<uint64_t>(1, hero->getArmyStrength()));
	const double danger = omniai::LearningStore::instance().dangerThreshold();

	// Carrying an artifact and the field hero is within this turn's reach:
	// meet it first. The meeting is a hero exchange, and handOver moves the
	// artifact onto the hero whose army it multiplies.
	if(carriesArtifacts(hero) && fieldHeroId_ >= 0)
		if(const CGHeroInstance * field = cb->getHero(ObjectInstanceID(fieldHeroId_)))
		{
			CGPath toField;
			if(paths->getPath(toField, field->visitablePos()) && !toField.nodes.empty()
				&& toField.nodes.front().turns == 0)
			{
				std::vector<int3> steps = stepsToward(paths, hero, field->visitablePos());
				if(!steps.empty() && safePrefix(hero, steps, true) == steps.size())
				{
					decisionLog_->line("   COURIER artifacts to " + field->getNameTranslated()
						+ ", " + std::to_string(steps.size()) + " steps");
					return walk(hero, steps, true);
				}
			}
		}

	// A trading post is only worth the stop while its fixed rate beats the
	// best marketplace rate we own, and only while we hold surplus to sell.
	int bestMarket = 0;
	for(const CGTownInstance * town : cb->getTownsInfo())
		if(town && town->tempOwner == playerID)
			bestMarket = std::max(bestMarket, town->getMarketEfficiency());
	const bool haveSurplus =
		cb->getResourceAmount(EGameResID::WOOD)
			+ cb->getResourceAmount(EGameResID::ORE) > 80;

	// Nearest-next over the week's pickups: windmills, wheels and gardens
	// hand out free resources, dwellings restock creatures that ride home
	// for upgrading, and the post converts surplus at the better rate.
	const CGObjectInstance * best = nullptr;
	std::vector<int3> bestSteps;
	bool bestUrgent = false;
	const std::array<bool, 7> blockedRes = blockedBuildingResources();
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		if(!obj || !obj->isVisitable())
			continue;

		const int oid = obj->ID.getNum();
		// Weekly producers, and the loose piles lying around. The piles were
		// missing entirely: a human watching a game noticed blue sidestepping
		// resource heaps it walked straight past, and the reason is this list.
		// The field hero can target a pile through the scoring pass - old logs
		// show MOVE toward Wood and MOVE toward Ore - but that pass now runs
		// between zero and three times in a whole game, so in practice nobody
		// picks them up at all. A one-shot pile disappears once taken, so the
		// weekly visit marker below costs nothing on them.
		bool collectible = oid == Obj::WINDMILL
			|| oid == Obj::WATER_WHEEL
			|| oid == Obj::MYSTICAL_GARDEN
			|| oid == Obj::REFUGEE_CAMP
			|| oid == Obj::RESOURCE
			|| oid == Obj::TREASURE_CHEST
			|| oid == Obj::CAMPFIRE
			|| oid == Obj::FLOTSAM
			|| oid == Obj::SEA_CHEST
			|| oid == Obj::SHIPWRECK_SURVIVOR;
		if(!collectible)
			if(const auto * dw = dynamic_cast<const CGDwelling *>(obj))
				collectible = dwellingOffersRecruits(dw, hero);
		// A mine is income for the rest of the game, and flagging it is one
		// visit. TRUTH on duel-v2: Nullkiller holds 11 mines by day 12 and we
		// hold 0-1, because our only mine taker is the field hero's scarce-
		// resource run. The burner walks past them on its circuit anyway.
		if(!collectible && oid == Obj::MINE && obj->tempOwner != playerID)
			collectible = true;
		// Loose artifacts: the pickup equips them on the collector, and the
		// next meeting with the field hero moves them across (handOver).
		if(!collectible && oid == Obj::ARTIFACT)
			collectible = true;
		if(!collectible && haveSurplus
			&& (oid == Obj::TRADING_POST || oid == Obj::TRADING_POST_SNOW))
		{
			const auto * post = dynamic_cast<const CGMarket *>(obj);
			collectible = post && post->getMarketEfficiency() > bestMarket;
		}
		if(!collectible)
			continue;

		{
			std::lock_guard lock(visitedMutex_);
			const auto it = objectVisitWeek_.find(obj->id.getNum());
			if(it != objectVisitWeek_.end() && it->second == week)
				continue;   // taken already this week
		}
		// Another of our heroes is already walking to it. The same hero may
		// carry on: a walk cut short at the fog's edge used to cost the
		// target for the day, and 6% of COLLECT orders in R6n_duel ended
		// that way before the hero turned to something else.
		if(targetedThisTurn_.count(obj->id.getNum()))
		{
			const auto by = collectedBy_.find(obj->id.getNum());
			if(by == collectedBy_.end() || by->second != hero->id.getNum())
				continue;
		}

		// Cheap pickups stop being cheap the moment somebody has to fight
		// for them, and this hero is nobody's fighter.
		if(double(guardStrengthAt(obj->visitablePos(), hero)) >= ours * danger)
			continue;
		if(withinEnemyReach(obj->visitablePos(), hero))
			continue;

		std::vector<int3> steps = stepsToward(paths, hero, obj->visitablePos());
		if(steps.empty() || safePrefix(hero, steps) == 0)
			continue;   // no route, or walk() would fence it at the first step
		// A mine producing what a building is blocked on outranks nearness:
		// income of the one resource the town is waiting for.
		bool urgent = false;
		if(oid == Obj::MINE)
			if(const auto * mine = dynamic_cast<const CGMine *>(obj))
			{
				const int r = mine->producedResource.getNum();
				urgent = r >= 0 && r < int(blockedRes.size()) && blockedRes[r];
			}
		if(!best || (urgent && !bestUrgent)
			|| (urgent == bestUrgent && steps.size() < bestSteps.size()))
		{
			best = obj;
			bestSteps = std::move(steps);
			bestUrgent = urgent;
		}
	}

	int stacks = 0;
	for(const auto & entry : hero->Slots())
		if(entry.second && entry.second->getType())
			++stacks;
	const bool loaded = stacks >= 2;
	const bool bagsFull = stacks >= 7;

	if(best && !bagsFull)
	{
		std::ostringstream o;
		o << "   COLLECT " << best->getObjectName() << " at "
		  << best->visitablePos().toString() << ", " << bestSteps.size() << " steps";
		decisionLog_->line(o.str());
		targetedThisTurn_.insert(best->id.getNum());
		collectedBy_[best->id.getNum()] = hero->id.getNum();
		return walk(hero, bestSteps);
	}

	// Nothing left this week, or no room to carry more: run the takings home
	// to the garrison that hands them on to the field hero.
	if(!loaded)
		return false;

	const CGTownInstance * home = nullptr;
	std::vector<int3> homeSteps;
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;
		if(town->getVisitingHero() == hero || town->getGarrisonHero() == hero)
		{
			home = town;
			break;
		}
		std::vector<int3> steps = stepsToward(paths, hero, town->visitablePos());
		if(steps.empty())
			continue;
		if(!home || steps.size() < homeSteps.size())
		{
			home = town;
			homeSteps = std::move(steps);
		}
	}
	if(!home)
		return false;   // loaded with nowhere to put it; hold a post

	if(homeSteps.empty())
	{
		// Standing in the town already: hand everything but the keep-stack
		// to the garrison.
		depositArmy(hero, home);
		return true;
	}
	std::ostringstream o;
	o << "   DEPOSIT RUN to " << home->getNameTranslated()
	  << ", " << homeSteps.size() << " steps";
	decisionLog_->line(o.str());
	return walk(hero, homeSteps);
}

size_t OmniAI::safePrefix(const CGHeroInstance * hero, const std::vector<int3> & steps,
	bool finalFightOk) const
{
	// The steps walk() would actually send: it stops before a deferred tile
	// and before any hostile the hero is not expected to beat. Callers that
	// pick targets ask this first; otherwise a pick walk() refuses is picked
	// again every round (the collector aimed at one fenced gem pile for six
	// straight days on C_duel r06 and moved nowhere).
	// Steps are hero ANCHOR positions (convertFromVisitablePos, what moveHero
	// wants), one tile east of the tile the hero stands on; every check here
	// is about the tile, so convert back. See walk() for the measured cost.
	const size_t n = steps.size();
	const size_t fenceUpTo = finalFightOk && n ? n - 1 : n;
	for(size_t i = 0; i < n; ++i)
	{
		const int3 tile = hero->convertToVisitablePos(steps[i]);
		if(isDeferredTile(tile))
			return i;
		if(i >= fenceUpTo)
		{
			// The exempt last step is a priced fight only if something we can
			// see is there to price; beside fog it may be an unseen one.
			if(!strongestHostileOnStep(tile, hero) && bordersFog(tile))
				return i;
			break;
		}
		if(bordersFog(tile))
			return i;
		const CArmedInstance * hostile = strongestHostileOnStep(tile, hero);
		if(hostile && !expectedToWin(hero, hostile, commitMargin(hero)))
			return i;
	}
	return n;
}

bool OmniAI::leashBinds(const CGHeroInstance * hero) const
{
	// The conditions under which leashAllows can refuse anything at all.
	if(!cb || !hero || hero->id.getNum() != fieldHeroId_ || attacking_)
		return false;
	const int3 sizes = cb->getMapSize();
	return std::max(sizes.x, sizes.y) <= 48 && cb->getTownsInfo().size() == 1;
}

bool OmniAI::leashAllows(const CGHeroInstance * hero, const int3 & end) const
{
	// Field-hero leash. On a knife-fight map - one a fast enemy crosses in
	// about two days - while we hold exactly one town, the field hero's end
	// position belongs inside a day's answer of the walls. Every duel-v2
	// loss ended the same way: their main appears at ~11 tiles (the edge of
	// sight range) and sieges before our hero can walk home. A move that
	// closes on the anchor is always allowed, so a defender heading home is
	// never fenced by it, and the leash releases once we are clearly ahead
	// or a second town exists to retreat to. Asked when a target is chosen
	// as well as when it is walked: refusing only at walk time let the
	// scoring pass pick the same out-of-leash Ore Pit as its first move for
	// 17 straight days on duel-v2 and move nowhere.
	if(!cb || !hero || hero->id.getNum() != fieldHeroId_ || attacking_)
		return true;
	const int3 sizes = cb->getMapSize();
	const auto & towns = cb->getTownsInfo();
	if(std::max(sizes.x, sizes.y) > 48 || towns.size() > 1)
		return true;
	const CGTownInstance * home = nullptr;
	for(const CGTownInstance * t : towns)
		if(t && t->tempOwner == playerID)
			home = t;
	if(!home)
		return true;
	// One day's march home, not a fixed 10. The rule's own reason is that
	// their main shows up at the edge of sight and sieges the next day, so
	// the field hero must be able to walk home in one turn: that distance is
	// the hero's movement allowance in straight tiles. The old constant 10
	// was never measured, sat exactly at duel-v2's level-6 guards, and put
	// four of our eight mines on that map (13-27 tiles out) out of bounds
	// for the only hero that can clear their guards; Nullkiller held 13-15
	// of the 16 by day 19-28.
	const int LEASH = std::max(10, hero->movementPointsLimit(true) / 100);
	const int3 ap = home->visitablePos();
	const auto cheb = [&](const int3 & p)
	{
		return std::max(std::abs(p.x - ap.x), std::abs(p.y - ap.y));
	};
	// Walking steps, not a straight line. duel-v2 is a maze: in 25 of 62
	// town losses across R6n_duel, R6n_fog and R6q_duel our best hero stood
	// more than a day's walk from the town at the dawn before it fell, and
	// in five of them it was 2-5 tiles away in a straight line and 26-29
	// steps away on foot (R6n r16, R6q r06: the near ore pit behind the
	// wall). R6q r13 lost a Castle it held at army parity on day 45 with the
	// field hero 13 tiles out in a straight line, 18 by the road, ending the
	// DEFEND walk two tiles short. A step on this field is straight (100)
	// or diagonal (141) movement; 120 per step is the middle, so the budget
	// is what one day actually walks.
	if(leashFieldDay_ != cb->getDate(Date::DAY) || leashFieldHome_ != ap)
	{
		leashField_ = crossingField(ap, false);
		leashFieldDay_ = cb->getDate(Date::DAY);
		leashFieldHome_ = ap;
	}
	const auto walking = [&](const int3 & p)
	{
		if(p.z != ap.z || !cb->isInTheMap(p))
			return -1;
		return leashField_[size_t(p.y) * size_t(sizes.x) + size_t(p.x)];
	};
	const int stepsEnd = walking(end);
	const int stepsNow = walking(hero->visitablePos());
	if(stepsEnd < 0 || stepsNow < 0)
		return cheb(end) <= LEASH || cheb(end) <= cheb(hero->visitablePos());
	const int STEPS = std::max(8, hero->movementPointsLimit(true) / 120);
	if(stepsEnd <= STEPS || stepsEnd <= stepsNow)
		return true;

	// Past a day's walk the question is a race, not a radius: may the hero
	// stand there only while every enemy hero that could take the town
	// without it needs longer to reach the walls than it needs to get home.
	// A fixed radius of a day's walk shuts the field hero out of every
	// guarded dwelling on duel-v2 (16-26 steps from the gates) and out of
	// the mines behind the maze, whatever the enemy is doing. Enemy
	// positions come from the sightings assessPosture keeps (strength
	// projected 4.5% a day, as there); each day since a sighting is a day
	// it may have spent walking at us. With no sighting at all, the start
	// the map declares for the enemy stands in for its main.
	const int today = cb->getDate(Date::DAY);
	const int ourDays = (stepsEnd + STEPS - 1) / STEPS;
	uint64_t holds = uint64_t(home->getArmyStrength());
	if(const CGHeroInstance * gh = home->getGarrisonHero())
		if(gh != hero)
			holds += uint64_t(gh->getArmyStrength());
	int theirDays = INT_MAX;
	bool anyThreat = false;
	for(const auto & kv : foeSeen_)
	{
		const FoeSighting & f = kv.second;
		const double projected = f.str * std::pow(1.045, double(std::max(0, today - f.day)));
		if(projected <= double(holds))
			continue;   // the garrison holds against it without us
		anyThreat = true;
		int steps = walking(f.pos);
		if(steps < 0)
			steps = cheb(f.pos);
		const int reach = std::max(1, f.reach);
		theirDays = std::min(theirDays, (steps + reach - 1) / reach - (today - f.day));
	}
	if(!anyThreat && enemyStartValid_)
	{
		int steps = walking(enemyStart_);
		if(steps < 0)
			steps = cheb(enemyStart_);
		theirDays = (steps + STEPS - 1) / STEPS - (today - 1);
	}
	return ourDays < theirDays;
}

bool OmniAI::walk(const CGHeroInstance * hero, const std::vector<int3> & steps,
	bool finalFightOk, bool ignoreLeash)
{
	if(!cb || !hero || !dispatcher_ || steps.empty())
		return false;

	// A garrisoned hero cannot walk, so step it out first. This used to
	// happen at the top of every hero's turn, which left the town holder
	// standing on the town's entrance tile: every other hero's walk home
	// then resolved as a blocking hero-to-hero visit, spending the step
	// cost and moving nobody. Measured on duel-v2, ten identical HOME
	// orders a day for the whole game.
	//
	// Garrison -> visitor moves no army (CGameHandler.cpp:2606-2621). It
	// is the other direction that merges the town's army onto the hero,
	// which is why the pool has to be put back after a visitor steps in.
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(town && town->tempOwner == playerID && town->getGarrisonHero() == hero)
		{
			cb->swapGarrisonHero(town);
			break;
		}
	}

	// Field-hero leash. On a knife-fight map - one a fast enemy crosses in
	// about two days - while we hold exactly one town, the field hero's end
	// position belongs inside a day's answer of the walls. Every duel-v2
	// loss ends the same way: their main appears at ~11 tiles (the edge of
	// sight range) and sieges before our hero can walk home, so the town
	// falls at 0.01-0.02 on a garrison-only fight while the field stack
	// stands eight tiles out. Refuse endpoints outside the leash; a move
	// that closes on the anchor is always allowed, so a defender heading
	// home is never fenced by it, and the leash releases entirely once we
	// are clearly ahead (the press is then the point) or when a second
	// town exists to retreat to.
	// Every step is a hero ANCHOR position (convertFromVisitablePos, which is
	// what moveHero takes), one tile east of the tile the hero will stand on.
	// Until September 24th the fence, the deferred-tile check, the leash and
	// the reach check below all read the anchor as the tile. K_duel r01 day
	// 19: the field hero's first step home was the open tile (10 12 0), its
	// anchor (11 12 0) sat beside a deferred guard at (12 13 0), the walk was
	// "FENCED ... deferred fight" at every attempt, and Nullkiller's main
	// killed the hero where it stood. The same shift let real guard zones one
	// tile west of a step through unpriced.
	const auto tileOf = [hero](const int3 & anchor) { return hero->convertToVisitablePos(anchor); };
	if(!ignoreLeash && !leashAllows(hero, tileOf(steps.back())))
	{
		decisionLog_->detail("   leashed - field hero stays near its only town");
		return false;
	}

	// An open query blocks every pack except QueryReply, so a MoveHero sent
	// now is held behind it until it times out - the dominant fishy request
	// on the wire. Wait briefly for the answer to land. The wait is bounded:
	// a leaked count costs a few seconds of latency, never a hang, and the
	// worker pool (4 threads) answers the query on a different thread.
	for(int i = 0; i < 60 && openQueries_.load() > 0; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// A hostile tile on the way is a fight whether the walk meant it or
	// not: stepping onto a monster or an enemy object starts combat at
	// whatever the odds happen to be. The scored pass prices only its own
	// destination, so supply runs, collects and home errands have been
	// walking into deferred guards - s17-72x72 day 25 lost an 11.7k stack
	// to a 31.9k one it had declined to fight a line earlier. Walk the
	// safe prefix instead; finalFightOk leaves the last step alone for
	// callers whose destination IS the priced fight.
	size_t n = steps.size();
	const size_t fenceUpTo = finalFightOk && n ? n - 1 : n;
	for(size_t i = 0; i < n; ++i)
	{
		// A deferred tile stays fenced all day even for callers that priced
		// their destination: the defer decision said "not today", and a
		// press or march does not get to overrule it by naming the tile.
		// runDeferredGuards erases the task before walking, so its own
		// re-entry is never caught here.
		const int3 tile = tileOf(steps[i]);
		if(isDeferredTile(tile))
		{
			decisionLog_->line("   FENCED at " + tile.toString()
				+ ": deferred fight, not today");
			n = i;
			break;
		}
		if(i >= fenceUpTo)
		{
			// finalFightOk exempts a destination the caller priced. With no
			// visible hostile on it there was nothing to price, and fog beside
			// it may hide the guard: Q_duel r02 day 2 (Cyclopes, "exp -0.43",
			// 9402 lost) and r04 day 2 (Unicorns, "exp 0.43", 9207 of 11202).
			// A tile one of our own heroes stands on is a meeting: the
			// walker stays beside it and steps into nothing unknown.
			bool friendly = false;
			for(const CGHeroInstance * h : cb->getHeroesInfo())
				if(h && h != hero && h->visitablePos() == tile)
					friendly = true;
			if(!friendly && !strongestHostileOnStep(tile, hero) && bordersFog(tile))
			{
				decisionLog_->detail("stopping short of " + tile.toString()
					+ ": nothing seen there to price, and fog beside it");
				n = i;
			}
			break;
		}
		// The same meeting exemption on the last step of an ordinary walk: a
		// courier walking onto the field hero stays beside it, so fog next
		// to the field hero's tile is not ground the courier enters.
		bool meeting = false;
		if(i + 1 == n)
			for(const CGHeroInstance * h : cb->getHeroesInfo())
				if(h && h != hero && h->visitablePos() == tile)
					meeting = true;
		if(!meeting && bordersFog(tile))
		{
			decisionLog_->detail("stopping short of " + tile.toString()
				+ ": unseen ground beside it could hide a guard");
			n = i;
			break;
		}
		const CArmedInstance * hostile = strongestHostileOnStep(tile, hero);
		if(!hostile || expectedToWin(hero, hostile, commitMargin(hero)))
			continue;
		decisionLog_->line("   FENCED at " + tile.toString()
			+ ": hostile strength "
			+ std::to_string(uint64_t(hostile->getArmyStrength()))
			+ " is more than this walk is for");
		n = i;
		break;
	}
	if(!n)
		return false;

	// Support heroes do not end a move inside a stronger enemy hero's reach.
	// They carry little and die a lot: across 10 fog-on duel-v2 games 45 of
	// the 56 fights Nullkiller started on us killed a collector or a support
	// hero, each one re-hired at 2500 gold (up to 11 hires in one game, the
	// fort, City Hall and Capitol together) and each kill Nullkiller's XP.
	// Reach is the enemy's own movement allowance in straight tiles, not a
	// fixed radius. A move that ends on one of our towns, or leaves the hero
	// less exposed than where it stands, is still allowed: running home is
	// the point. The field hero keeps its own end-tile check in the scoring
	// pass, since gating it here reintroduces the camping the leash and the
	// press-saturation rules exist to stop.
	if(hero->id.getNum() != fieldHeroId_ && !finalFightOk)
	{
		const double ours = double(hero->getArmyStrength());
		struct Threat { int3 pos; int reach; };
		std::vector<Threat> threats;
		std::set<int32_t> visibleFoes;
		for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
		{
			const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
			if(!e || !e->tempOwner.isValidPlayer() || e->tempOwner == playerID)
				continue;
			visibleFoes.insert(e->id.getNum());
			if(double(e->getArmyStrength()) <= ours)
				continue;
			threats.push_back({e->visitablePos(), e->movementPointsLimit(true) / 100 + 1});
		}
		// A strong enemy seen yesterday and not today is still out there. J3
		// MapGen s17 blue lost seven heroes in three days to one 10.6k hero
		// that was out of sight when each of them moved. Counted for one day,
		// with the reach it could have covered in the day it was unseen
		// (double); older sightings are too vague to freeze a hero on.
		const int today = cb->getDate(Date::DAY);
		for(const auto & kv : foeSeen_)
		{
			if(visibleFoes.count(kv.first) || kv.second.day < today - 1)
				continue;
			if(kv.second.str <= ours)
				continue;
			threats.push_back({kv.second.pos, kv.second.reach * (1 + today - kv.second.day)});
		}
		if(!threats.empty())
		{
			// How far inside the nearest threat's reach a tile sits; <= 0 is safe.
			auto exposure = [&](const int3 & p)
			{
				int worst = INT_MIN;
				for(const Threat & t : threats)
				{
					if(t.pos.z != p.z)
						continue;
					const int d = std::max(std::abs(t.pos.x - p.x), std::abs(t.pos.y - p.y));
					worst = std::max(worst, t.reach - d);
				}
				return worst;
			};
			auto ourTownAt = [&](const int3 & p)
			{
				for(const CGTownInstance * town : cb->getTownsInfo())
					if(town && town->tempOwner == playerID && town->visitablePos() == p)
						return true;
				return false;
			};
			const int here = exposure(hero->visitablePos());
			size_t safe = n;
			while(safe > 0)
			{
				const int3 end = tileOf(steps[safe - 1]);
				const int e = exposure(end);
				if(e <= 0 || ourTownAt(end) || e < here)
					break;
				--safe;
			}
			if(safe < n)
			{
				decisionLog_->line("   HELD " + hero->getNameTranslated() + " ("
					+ roleOf(hero) + ") short of " + tileOf(steps[n - 1]).toString()
					+ ": a stronger enemy hero reaches it, "
					+ std::to_string(safe) + " of " + std::to_string(n) + " steps kept");
				heldThisTurn_.insert(hero->id.getNum());
				n = safe;
				if(!n)
					return false;
			}
		}
	}

	++movesDispatched_;
	dispatcher_->moveHero(hero,
		std::vector<int3>(steps.begin(), steps.begin() + n), false);
	return true;
}
void OmniAI::depositArmy(const CGHeroInstance * hero, const CGTownInstance * town)
{
	if(!cb || !hero || !town)
		return;

	// Every stack but the fastest goes into the garrison: a hero's next-day
	// movement is capped by its slowest creature, and couriers live on
	// movement. (Measured rule, now also the research's: a scout carries
	// exactly one fast unit.)
	std::vector<std::pair<SlotID, int32_t>> carried;
	for(const auto & entry : hero->Slots())
	{
		if(!entry.second || !entry.second->getType())
			continue;
		const auto * cre = entry.second->getCreature();
		if(!cre)
			continue;
		carried.emplace_back(entry.first, cre->getBaseSpeed());
	}
	if(carried.empty())
		return;
	std::sort(carried.begin(), carried.end(),
		[](const auto & l, const auto & r){ return l.second > r.second; });
	const SlotID fastest = carried.front().first;
	carried.erase(carried.begin());   // leave the fastest behind

	// One stack at a time: bulkMoveArmy moves the whole army and kept one
	// creature of the named slot, so the courier walked off with a single
	// creature of a slow stack instead of its fastest (see handOver).
	int moved = 0;
	for(const auto & entry : carried)
		if(moveOneStack(hero, town, entry.first))
			++moved;
	// ... as one creature of it (R8b): the same speed, and the rest waits in
	// the pool for the field hero instead of riding with a courier.
	if(moveOneStack(hero, town, fastest, true))
		++moved;
	if(moved)
		decisionLog_->line("  DEPOSIT " + std::to_string(moved)
			+ " stack(s) in " + town->getNameTranslated());
}

void OmniAI::leaveHoldingGarrison(const CGHeroInstance * hero, const CGTownInstance * town)
{
	if(!cb || !hero || !town)
		return;
	// A taken town with no wall-troops is a free walk-in for the first enemy
	// scout that wanders by. Leave the weakest stack as a tripwire so the
	// town is not empty; the town's own recruits build the real defense
	// while the fresh-capture window keeps it out of the courier pool. A
	// bigger deposit just donates a third of the field army to a wall the
	// enemy's main takes anyway - measured shorter, weaker games.
	std::vector<std::pair<SlotID, uint64_t>> carried;
	for(const auto & entry : hero->Slots())
	{
		if(!entry.second || !entry.second->getType())
			continue;
		const auto * cre = entry.second->getCreature();
		if(!cre)
			continue;
		carried.emplace_back(entry.first,
			uint64_t(entry.second->getCount())
			* uint64_t(std::max(0, cre->getAIValue())));
	}
	if(carried.size() <= 1)
		return;   // nothing to spare
	std::sort(carried.begin(), carried.end(),
		[](const auto & l, const auto & r){ return l.second < r.second; });
	// The weakest stack alone. This was cb->bulkMoveArmy, which moved the
	// WHOLE army into the empty garrison and left the hero one creature of
	// its weakest stack: Adelaide walked out of Whitemoon with 154 of 14168
	// (R6x r04 day 14), Victoria out of Kildare with 198 of 10799 three
	// times (R6y r01). The measurement above was taken with that bug in.
	if(!moveOneStack(hero, town, carried.front().first))
		return;
	decisionLog_->line("  HOLD GARRISON left a stack holding "
		+ town->getNameTranslated() + ", " + hero->getNameTranslated()
		+ " walks on with " + std::to_string(uint64_t(hero->getArmyStrength())));
}

bool OmniAI::hireHeroIfNone()
{
	if(!cb)
		return false;

	// The roster is demand-driven, not a fixed count. Demand is what the map
	// asks us to service this week: the field hero, one defender per town we
	// hold, bodies for the live task queue (each support hero clears about
	// two concurrent task streams - a pickup run, a delivery leg, a guard it
	// is waiting out), plus any capture sitting on a holder that is not yet
	// seated. Measured on Twins the queue is nearly empty, so this correctly
	// reads "small" there - the roster gap on that map is gold, not demand,
	// and over-hiring would just buy heroes it cannot feed. The 'wait until
	// area lets the extra hero earn' rule lands here: as reachable work and
	// towns grow, wanted rises on its own.
	size_t townsHeld = 0;
	for(const CGTownInstance * town : cb->getTownsInfo())
		if(town && town->tempOwner == playerID)
			++townsHeld;
	int readyStreams = 0, defNeed = 0;
	for(const auto & t : regTasks_)
	{
		// A Capture or a Hold wants a hero seated on it: the capture needs a
		// defender standing there before it counts as held, the hold is one
		// already routed. Each is a body the roster has to supply.
		if(t.kind == RegTask::Kind::Capture || t.kind == RegTask::Kind::Hold)
			++defNeed;
		else if(t.ready)
			++readyStreams;
	}
	const int workers = (readyStreams + 1) / 2;
	// Floor of 3, not the demand formula's 2 on a one-town map: the NK
	// trace shows a third hero hired on days 1-3 in every match sampled,
	// and it earns its keep as a pickup-chain scout even with no pending
	// captures. Our demand-driven floor left the roster at 2 for weeks.
	const size_t wanted = std::min<size_t>(6,
		std::max<size_t>(3, 1 + townsHeld + size_t(workers) + size_t(defNeed)));
	// Past the roster we need, a hire is still the way to turn idle gold
	// into army: 2500 buys a hero AND its starting stacks, which the supply
	// run then walks to the field hero. Nullkiller hires whenever an offer's
	// army is worth more than half the price and gold pressure is low
	// (RecruitHeroBehavior.cpp:133). Only from true surplus, up to the
	// engine's 8-hero map cap, and only for an offer that clears that bar.
	bool surplusHire = false;
	if(cb->getHeroesInfo().size() >= wanted)
	{
		if(cb->getHeroesInfo().size() >= 8
			|| surplusGold() < GameConstants::HERO_GOLD_COST)
			return false;
		surplusHire = true;
	}

	// Losing the last hero used to be terminal. showTavernWindow is the
	// only place that hires, and it fires when a hero visits a tavern,
	// which is not something a side with no hero can arrange. Measured:
	// blue lost its hero around day 100 of a 476 day soak and spent the
	// remaining 375 days sitting in its town with 910k gold and an empty
	// map in front of it.
	// A second hero is worth buying, and not at the price of the army. The
	// first one is different: with none at all nothing else matters, so it
	// spends down to the last coin if it has to.
	const bool desperate = cb->getHeroesInfo().empty();
	// Not into a threat. A hire appears at a town, and with an enemy hero
	// near it the new hero is killed at the door or the first time it walks
	// out: N_duel r01 re-hired on days 24, 25 and 26 and Valeska killed each
	// one the day it arrived. Across K_duel and R3_duel (26 games) 53% of
	// Nullkiller's experience came on days it killed one of our support or
	// collector heroes (161 kills, 6.2 a game) while we hired 7-8 heroes a
	// game against its 3. Only the last-hero emergency hires into a threat.
	if(!desperate && !threatenedTowns_.empty())
	{
		decisionLog_->detail("hire held: an enemy hero is near one of our towns");
		return false;
	}
	// Below the floor the hire is an emergency purchase, not a budget
	// line: no reserve at all. The half reserve this used to carry
	// (need 3750) priced heroes 2-3 out of the entire opening week on
	// duel-v2 - every log showed day-1 starts at 750-1760 gold and the
	// third hero landing day 5-20 or never, while Nullkiller fields 3
	// by day 1-2. The building check below is already skipped under the
	// floor for the same reason.
	//
	// Below the floor the hire still leaves the building savings alone,
	// though. Our supports die 4.6 times a game (R6n_duel, 73 in 16), so
	// the roster sat below 3 most of the game and every re-hire took the
	// gold the City Hall was waiting for: "saving 5000 for building 12,
	// have 2000", then "hiring hero to hold the towns", then the saving
	// starts over (R6n r05, r08). City Hall landed on day 16 (median, 36
	// R6n games) against Nullkiller's day 8, and our income sat at 1000 a
	// day while its reached 2000; that is about 8000 gold by day 16.
	// Nullkiller does not hire while it is saving (gold pressure,
	// RecruitHeroBehavior). Only a side with no hero at all spends it.
	const bool belowFloor = cb->getHeroesInfo().size() < 3;
	const int reserve = desperate ? 0
		: belowFloor ? buildReserveTotal()
		: GameConstants::HERO_GOLD_COST + buildReserveTotal();
	const int gold = cb->getResourceAmount(EGameResID::GOLD);
	if(gold < GameConstants::HERO_GOLD_COST + reserve)
	{
		// Logged so the roster constraint is measurable: how many hires gold
		// declined is the difference between a 2-hero ceiling and a frontier
		// worth defending.
		decisionLog_->detail("hire declined on gold: have "
			+ std::to_string(gold) + ", need "
			+ std::to_string(GameConstants::HERO_GOLD_COST + reserve));
		return false;
	}
	// And do not take the gold a building is waiting for it. Measured on
	// duel-v2, day 22: 6280 gold, 25 wood and 48 ore at dawn, the hire took
	// 2500, and the FORT at 5000 came back NO_RESOURCES with the wood and
	// ore sitting right there. Every dwelling above level 2 is blocked
	// behind that fort, and the same game hired on days 1, 12, 15, 22, 24
	// and 25, fifteen thousand gold into holders.
	//
	// hireHeroIfNone runs before buildInAllTowns deliberately, because a
	// town that can afford a building every day otherwise drains the
	// treasury below the hire check before it runs. That ordering is right
	// and this is its other edge.
	//
	// Only yields when something is actually waiting today, and the gold
	// gets spent either way: the only question is on which. Below the
	// roster floor the check is skipped outright - a week-1 town can
	// always build something cheap, which is exactly what held every
	// third-hero hire back to day 12+ on the drive2 logs.
	if(!desperate && !belowFloor)
	{
		const int afterHire = gold - GameConstants::HERO_GOLD_COST;
		for(const CGTownInstance * town : cb->getTownsInfo())
		{
			if(!town || town->tempOwner != playerID)
				continue;
			const auto * ct = town->getTown();
			if(!ct)
				continue;
			for(const auto & entry : ct->buildings)
			{
				if(!entry.second
					|| entry.second->mode != CBuilding::BUILD_NORMAL
					|| town->hasBuilt(entry.first))
					continue;
				if(cb->canBuildStructure(town, entry.first)
					!= EBuildingState::ALLOWED)
					continue;
				const int cost = entry.second->resources[EGameResID::GOLD];
				if(cost <= gold && cost > afterHire)
				{
					decisionLog_->detail("hire held back: "
						+ town->getNameTranslated() + " can raise building "
						+ std::to_string(entry.first.getNum()) + " today for "
						+ std::to_string(cost) + " and the hire would leave "
						+ std::to_string(afterHire));
					return false;
				}
			}
		}
	}

	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || !town->hasBuilt(BuildingID::TAVERN))
			continue;
		const auto heroes = cb->getAvailableHeroes(town);
		if(heroes.empty())
		{
			decisionLog_->detail("tavern in " + town->getNameTranslated()
				+ " has nobody on offer");
			continue;
		}
		// The recruit needs the visitor slot: the server refuses the hire
		// while a hero stands in it ("There is visiting hero - no place!"),
		// which used to make every hiring day a silent refusal once the
		// holder settled in as the visitor. Ours can step into an empty
		// garrison to make room; a garrison hero on top, or a hostile
		// visitor, and this tavern cannot hire today at all.
		if(const CGHeroInstance * visitor = town->getVisitingHero())
		{
			if(visitor->tempOwner != playerID)
				continue;
			if(!town->getGarrisonHero())
			{
				cb->swapGarrisonHero(town);
				if(town->getVisitingHero())
					continue;   // swap refused (no room to merge); slot still taken
			}
			else if(belowFloor || desperate)
			{
				// The hero hired this morning is standing in the doorway
				// and the garrison is already taken, so the swap cannot
				// make room - and that is what capped the roster at one
				// hire a day, m008/m009's third hero landing day 5+ while
				// Nullkiller's third is out day 1-2. Under the roster
				// floor a fresh hire has a whole day's movement to spend,
				// so step it out one tile and free the slot.
				const int3 tp = town->visitablePos();
				for(int dx = -1; dx <= 1 && town->getVisitingHero(); ++dx)
					for(int dy = -1; dy <= 1 && town->getVisitingHero(); ++dy)
					{
						if(!dx && !dy)
							continue;
						const int3 q(tp.x + dx, tp.y + dy, tp.z);
						if(!cb->isInTheMap(q))
							continue;
						const TerrainTile * tt = cb->getTile(q, false);
						if(!tt || tt->blocked())
							continue;
						bool occupied = false;
						for(const auto & oid : tt->visitableObjects)
							if(dynamic_cast<const CGHeroInstance *>(
								cb->getObj(oid, false)))
								occupied = true;
						if(occupied)
							continue;
						dispatcher_->moveHero(visitor, {q}, false);
					}
				if(town->getVisitingHero())
					continue;   // boxed in - slot stays taken today
			}
			else
				continue;
		}
		const CGHeroInstance * pickHero = bestHireOffer(heroes, town->getFactionID());
		if(surplusHire && (!pickHero
			|| pickHero->getArmyCost() < GameConstants::HERO_GOLD_COST / 2))
			continue;   // not worth the price as an army purchase
		decisionLog_->line(std::string(desperate
			? "  no heroes left, hiring one in "
			: surplusHire ? "  SURPLUS HIRE for its army, in "
			: "  hiring hero to hold the towns, in ")
			+ town->getNameTranslated() + ": " + pickHero->getNameTranslated()
			+ " with army " + std::to_string(uint64_t(pickHero->getArmyStrength()))
			+ " of " + std::to_string(heroes.size()) + " on offer");
		cb->recruitHero(town, pickHero);
		return true;
	}
	// Reaching here under roster means every tavern check above refused or
	// no owned town has a tavern at all, and until now that case printed
	// nothing - a 3774 day Unholy Quest soak sat at heroes 1/2 with
	// afford=1 and zero explanation, because Armitage's tavern is FORBIDDEN
	// by the scenario. Say which world we are in: a tavern exists somewhere
	// (the per-town refusals above carry the reason), a town could raise
	// one (roster is blocked on the build pass, not the hire), or no owned
	// town can ever build one (hiring is dead; the roster only grows by
	// taking a town that has one).
	bool anyTavern = false;
	std::string canRaise;
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;
		if(town->hasBuilt(BuildingID::TAVERN))
		{
			anyTavern = true;
			break;
		}
		const auto * ct = town->getTown();
		if(!ct)
			continue;
		const auto it = ct->buildings.find(BuildingID::TAVERN);
		if(it == ct->buildings.end() || !it->second
			|| it->second->mode != CBuilding::BUILD_NORMAL)
			continue;
		const EBuildingState st = cb->canBuildStructure(town, BuildingID::TAVERN);
		if(st == EBuildingState::ALLOWED || st == EBuildingState::PREREQUIRES
			|| st == EBuildingState::MISSING_BASE || st == EBuildingState::NO_RESOURCES)
			canRaise = town->getNameTranslated();
	}
	if(!anyTavern)
		decisionLog_->detail(canRaise.empty()
			? "no tavern anywhere and no owned town can build one - hiring is dead, roster grows only by capture"
			: "no tavern anywhere; " + canRaise
				+ " could raise one - roster is waiting on the build pass, not the hire");
	if(desperate)
		decisionLog_->detail("no heroes left and nowhere to hire one");
	return false;
}

void OmniAI::assessPosture()
{
	// How far ahead we want to be before marching. Well above parity, partly
	// because a fight is not a certainty and partly because the count below
	// only sees what is out of fog.
	constexpr double ADVANTAGE = 1.3;

	if(!cb)
		return;

	uint64_t ourBest = 0;
	for(const CGHeroInstance * h : cb->getHeroesInfo())
		if(h)
			ourBest = std::max<uint64_t>(ourBest, h->getArmyStrength());

	uint64_t theirBest = 0;
	const int today = cb->getDate(Date::DAY);
	{
		std::lock_guard lock(seenMutex_);
		for(const int32_t id : removedIds_)
			foeSeen_.erase(id);   // killed or gone: stop projecting it
		removedIds_.clear();
	}
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		if(!obj || !obj->tempOwner.isValidPlayer() || obj->tempOwner == playerID)
			continue;
		if(const auto * armed = dynamic_cast<const CArmedInstance *>(obj))
			theirBest = std::max<uint64_t>(theirBest, armed->getArmyStrength());
		if(const auto * h = dynamic_cast<const CGHeroInstance *>(obj))
		{
			const auto prev = foeSeen_.find(h->id.getNum());
			const bool moved = prev != foeSeen_.end()
				&& (prev->second.moved || prev->second.pos != h->visitablePos());
			foeSeen_[h->id.getNum()] = { double(h->getArmyStrength()), today,
				h->visitablePos(), h->movementPointsLimit(true) / 100 + 1, moved };
			foeEverSeen_ = true;
		}
	}
	// An enemy out of sight has not stopped growing. Remember every enemy
	// hero's last-seen army and project it forward at 4.5% a day, the median
	// growth TRUTH measured for Nullkiller on duel-v2 (day 5 to day 21, 2.0x
	// to 2.2x across the September 24th spreads). Without this, nothing in
	// sight read as no enemy at all, and a stale sighting can only ever make
	// the posture more careful, never less.
	constexpr double FOE_GROWTH_PER_DAY = 1.045;
	for(const auto & kv : foeSeen_)
	{
		const double projected = kv.second.str
			* std::pow(FOE_GROWTH_PER_DAY, double(std::max(0, today - kv.second.day)));
		theirBest = std::max<uint64_t>(theirBest, uint64_t(projected));
	}

	// Nothing of theirs in sight means there is nothing to march on, so the
	// posture is moot and developing is the honest answer.
	const bool wasAttacking = attacking_;
	attacking_ = theirBest > 0 && double(ourBest) >= double(theirBest) * ADVANTAGE;

	// Reported, not acted on. The flip-flopping this line exposed is what
	// showed the global view was the wrong one to steer by: four changes of
	// posture in a match while our army barely moved, driven entirely by
	// their hero walking in and out of the fog.
	if(attacking_ != wasAttacking)
	{
		std::ostringstream o;
		o << "  balance of power: " << (attacking_ ? "ahead" : "behind")
		  << ", our best hero " << ourBest
		  << " against their strongest " << theirBest;
		decisionLog_->detail(o.str());
	}
}

double OmniAI::strategicBonus(const CGObjectInstance * obj) const
{
	// A town is the only object on the map that ends the game, so it
	// outranks anything else we could walk to. Their mines and dwellings
	// come next: taking one counts twice, once for what it gives us and once
	// for what it stops giving them.
	constexpr double THEIR_TOWN = 900.0;
	constexpr double THEIR_PROPERTY = 350.0;

	if(!obj)
		return 0.0;
	if(!obj->tempOwner.isValidPlayer() || obj->tempOwner == playerID)
		return 0.0;

	// Not gated on the overall balance of power, and that is deliberate.
	// Whether we are winning the war does not decide where one hero walks;
	// whether THIS can be taken does, and that is already answered per
	// object. The danger check refuses a town whose garrison beats us, the
	// guarded-object veto refuses guards we cannot beat, and the reach check
	// refuses to end a turn beside a stronger hero. A weakly held town is
	// worth taking whether or not their hero is strong somewhere else.
	const double base = obj->ID == Obj::TOWN ? THEIR_TOWN : THEIR_PROPERTY;

	// Early-vs-late weighting. The research note is explicit: early game
	// leans on economy, and by month 2 the weight should move toward the
	// enemy's towns and property specifically - this only ever adds to the
	// enemy-focus term, on purpose, rather than also discounting resources
	// on the other side of the ledger (those already carry
	// updateResourceScarcity's own multiplier, and stacking a late-game
	// discount on top risks starving a real, CURRENT shortage over a
	// calendar assumption). Ramps to double weight by day 56 (~month 2, the
	// research's own marker) and holds there - not unbounded, so a very
	// long game does not send this to infinity.
	constexpr int PHASE_RAMP_DAYS = 56;
	constexpr double LATE_GAME_BOOST = 1.0;   // +100% at the ramp's end
	const int day = cb ? cb->getDate(Date::DAY) : 0;
	const double phase = std::min(1.0, double(day) / double(PHASE_RAMP_DAYS));
	return base * (1.0 + LATE_GAME_BOOST * phase);
}

void OmniAI::assessThreats()
{
	threatenedTowns_.clear();
	if(!cb)
		return;

	// A town falls to whoever walks into it, and losing the last one loses
	// the game. Until this existed the AI did not so much as notice an enemy
	// hero beside its capital.
	//
	// A radius rather than a path length, on purpose. The question is "can
	// that hero be here soon", the enemy's movement allowance and paths are
	// not ours to compute, and a straight-line bound errs toward caution,
	// which is the right direction to err in when the alternative is losing
	// the game.
	constexpr int THREAT_RADIUS = 12;

	std::vector<const CGHeroInstance *> enemies;
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * h = dynamic_cast<const CGHeroInstance *>(obj);
		if(h && h->tempOwner.isValidPlayer() && h->tempOwner != playerID)
			enemies.push_back(h);
	}
	if(enemies.empty())
		return;

	// Every enemy hero we can see is a waypoint toward the pocket it walked
	// out of. Keep the strongest one's position as the beacon the hunt
	// presses toward - their main stack rides the route from their capital.
	const CGHeroInstance * deepest = nullptr;
	for(const CGHeroInstance * e : enemies)
		if(!deepest || e->getArmyStrength() > deepest->getArmyStrength())
			deepest = e;
	if(deepest)
	{
		enemyBeacon_ = deepest->visitablePos();
		enemyBeaconValid_ = true;
	}

	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town)
			continue;
		const int3 tp = town->visitablePos();
		for(const CGHeroInstance * e : enemies)
		{
			const int3 ep = e->visitablePos();
			if(ep.z != tp.z)
				continue;
			const int dist = std::max(std::abs(ep.x - tp.x), std::abs(ep.y - tp.y));
			if(dist > THREAT_RADIUS)
				continue;
			const double strength = double(e->getArmyStrength());
			auto & worst = threatenedTowns_[town->id.getNum()];
			if(strength > worst)
			{
				worst = strength;
				std::ostringstream o;
				o << "  THREAT: " << e->getNameTranslated() << " is " << dist
				  << " tiles from " << town->getNameTranslated()
				  << ", strength " << strength;
				decisionLog_->line(o.str());
			}
		}
	}

	// Early warning, on a map bigger than the knife-fight size. The radius
	// above is under a day of enemy movement, and on a 72x72 map the field
	// hero is often two days out: in five of the seven R7 big-map games the
	// last town fell with our army hero 25-39 tiles from home (R7l3c
	// mg72s17 blue, day 21: Nullkiller's 21k Golwyn took the capital, held
	// by support heroes with 972, while our 15k field hero was ~35 tiles
	// west), after the couriers had carried the whole pool out. So a town is
	// also threatened by an enemy hero seen moving in the last two days that
	// could get there no later than a day after our field hero could, and
	// that would beat what stands in it. That stops the couriers and sends
	// the field hero home (defendThreatenedTown). Duel-v2 keeps the old rule:
	// its leash already keeps the field hero within reach of home.
	{
		const int3 msize = cb->getMapSize();
		const CGHeroInstance * field = fieldHeroId_ >= 0
			? cb->getHero(ObjectInstanceID(fieldHeroId_)) : nullptr;
		const int today = cb->getDate(Date::DAY);
		auto cheb = [](const int3 & a, const int3 & b)
		{
			return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
		};
		if(std::max(msize.x, msize.y) > 48)
			for(const CGTownInstance * town : cb->getTownsInfo())
			{
				if(!town || town->tempOwner != playerID)
					continue;
				const int3 tp = town->visitablePos();
				int fieldDays = 0;
				if(field && field->visitablePos().z == tp.z)
				{
					const int fr = std::max(1, int(field->movementPointsLimit(true) / 100) + 1);
					fieldDays = (cheb(field->visitablePos(), tp) + fr - 1) / fr;
				}
				// Walls count for half again, the same credit the posture
				// check gives a walled garrison.
				const double holds = double(town->getArmyStrength())
					* (town->fortLevel() != CGTownInstance::NONE ? 1.5 : 1.0);
				for(const auto & kv : foeSeen_)
				{
					const FoeSighting & f = kv.second;
					const int ago = today - f.day;
					if(!f.moved || ago < 0 || ago > 2 || f.pos.z != tp.z)
						continue;
					const int reach = std::max(1, f.reach);
					const int d = cheb(f.pos, tp);
					if(d <= THREAT_RADIUS && ago == 0)
						continue;   // the radius above has it
					const int foeDays = std::max(0, (d + reach - 1) / reach - ago);
					if(foeDays > fieldDays + 1)
						continue;
					const double str = f.str * std::pow(1.045, double(ago));
					if(str <= holds)
						continue;
					// Worth the field hero's walk home only against a real
					// army: a raider under a third of ours that takes an
					// empty town is retaken inside the week's grace, and a
					// recall for every passing scout would pin the field
					// hero at home.
					if(field && str < 0.35 * double(field->getArmyStrength()))
						continue;
					auto & worst = threatenedTowns_[town->id.getNum()];
					if(str > worst)
					{
						worst = str;
						std::ostringstream o;
						o << "  THREAT (early): a hero seen " << ago << " day(s) ago " << d
						  << " tiles from " << town->getNameTranslated() << " can be there in "
						  << foeDays << " day(s), our field hero needs " << fieldDays
						  << ", strength " << uint64_t(str) << " against " << uint64_t(holds);
						decisionLog_->line(o.str());
					}
				}
			}
	}

	// Which of those would fall. Any enemy hero within the radius marks a
	// town threatened, a 312-strength scout included (R6x r01, days 28-36),
	// and the build planner answered every such mark by dropping its saving
	// and freeing it for recruits. Only a town the siege model says falls
	// to its strongest visitor, defended by what stands in it now, is in
	// danger.
	dangerTowns_.clear();
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || !threatenedTowns_.count(town->id.getNum()))
			continue;
		const CGHeroInstance * foe = strongestThreatNear(town);
		if(foe && !defenseHolds(nullptr, town, foe))
			dangerTowns_.insert(town->id.getNum());
	}
}

bool OmniAI::betterDefenderAvailable(const CGHeroInstance * me,
	const CGTownInstance * town) const
{
	if(!cb || !me || !town)
		return false;
	const uint64_t mine = uint64_t(me->getArmyStrength());
	for(const CGHeroInstance * h : cb->getHeroesInfo())
	{
		if(!h || h == me || h->tempOwner != playerID)
			continue;
		const uint64_t theirs = uint64_t(h->getArmyStrength());
		if(theirs < mine
			|| (theirs == mine && h->id.getNum() > me->id.getNum()))
			continue;
		auto hp = pathCache_ ? pathCache_->getPathsInfo(h) : nullptr;
		if(!hp)
			continue;
		CGPath probe;
		if(const_cast<CPathsInfo *>(hp.get())->getPath(probe, town->visitablePos()))
			return true;
	}
	return false;
}

bool OmniAI::defendThreatenedTown(const CGHeroInstance * hero, CPathsInfo * paths)
{
	const int3 hp = hero->visitablePos();

	const CGTownInstance * best = nullptr;
	std::vector<int3> bestSteps;

	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town)
			continue;
		const auto threat = threatenedTowns_.find(town->id.getNum());
		if(threat == threatenedTowns_.end())
			continue;

		// Defend only what can be defended. Walls favour whoever is behind
		// them, hence the discount, but standing in a town in front of an
		// army four times your size is not defence. Measured: the hero died
		// on day 5 doing exactly that, and a dead hero loses the town anyway
		// AND the hero.
		// Only when there is somewhere else worth being. With one town left
		// there is nothing to preserve the hero FOR: lose it and the game is
		// lost a week later whichever way the hero runs. Measured, this rule
		// without the last-town exception declined to defend 8 times in a
		// match and turned a day 25 defeat into a day 12 one.
		constexpr double WALLS_ARE_WORTH = 0.7;

		// The garrison and the walls belong to a hero that is STANDING IN
		// the town. A hero eight tiles away fights wherever the attacker
		// meets it, in the open, on its own, and crediting it a garrison it
		// has not reached yet is what sends it there. Measured on duel-v2:
		// one game logged three DEFEND orders and three fights lost at odds
		// 0.50, 0.44 and 0.35, and those odds are the marching hero's own
		// army against theirs with nothing added. Thirty runs of the
		// best-defender rule changed nothing (median 6 against 6, day-4
		// collapses 9 against 10) because this accounting meant the weak
		// hero always cleared the bar and never stood down.
		const bool inside = town->visitablePos() == hp;
		const double ours = inside
			? double(hero->getArmyStrength()) + double(town->getArmyStrength())
			: double(hero->getArmyStrength());
		const double bar = inside
			? threat->second * WALLS_ARE_WORTH
			: threat->second;

		const bool lastTown = cb->getTownsInfo().size() <= 1;

		// Standing in the threatened town, hold-vs-leave is the evaluator's
		// call: does the garrison + our hero + the walls repel the attacker?
		// A hero that stays in a town it loses dies with the walls; one that
		// leaves keeps the army compounding. Only when it is not the last
		// town - losing the last one ends the game whichever way we stand.
		if(inside)
		{
			const CGHeroInstance * foe = strongestThreatNear(town);
			// A support hero in an unwalled town that nothing we can put in it
			// today would hold (R8a, September 25th). The field hero has its
			// own last-town rule below; everyone else held and died. R7t_duel
			// r20, r04, r10, r26: a holder in the garrison and a collector at
			// the door, 700-2900 between them, killed one after the other by
			// a 10-14k hero on day 2-3, and the town fell the same turn. The
			// town goes either way; leaving keeps the heroes and the troops
			// for the retake. Only when even the field hero, if it can get in
			// today, would not make a fight of it: 1.4 times everything, the
			// bar the field hero's own rule uses.
			if(foe && hero->id.getNum() != fieldHeroId_
				&& town->fortLevel() == CGTownInstance::NONE)
			{
				double everything = double(town->getArmyStrength());
				for(const CGHeroInstance * h : {town->getVisitingHero(), town->getGarrisonHero()})
					if(h && h->tempOwner == playerID)
						everything += double(h->getArmyStrength());
				const CGHeroInstance * field = fieldHeroId_ >= 0
					? cb->getHero(ObjectInstanceID(fieldHeroId_)) : nullptr;
				bool fieldComes = false;
				if(field && town->getVisitingHero() != field && town->getGarrisonHero() != field)
				{
					auto fpi = pathCache_ ? pathCache_->getPathsInfo(field) : nullptr;
					CGPath probe;
					fieldComes = fpi
						&& const_cast<CPathsInfo *>(fpi.get())->getPath(probe, town->visitablePos())
						&& !probe.nodes.empty() && probe.nodes.front().turns == 0;
					if(fieldComes)
						everything += double(field->getArmyStrength());
				}
				if(double(foe->getArmyStrength()) >= 1.4 * everything)
				{
					decisionLog_->line("   ABANDON " + town->getNameTranslated() + ": "
						+ foe->getNameTranslated() + " (" + std::to_string(uint64_t(foe->getArmyStrength()))
						+ ") against everything we can put in it today ("
						+ std::to_string(uint64_t(everything)) + (fieldComes ? ", field hero included" : "")
						+ "); " + hero->getNameTranslated() + " takes the garrison out");
					if(evacuateTown(hero, foe->visitablePos(), paths))
						return true;
				}
			}
			// Behind walls the siege model reads holds as routs (see the
			// last-town case below), so only an unwalled town is given up
			// on its say-so.
			if(!lastTown && foe && town->fortLevel() == CGTownInstance::NONE
				&& !defenseHolds(hero, town, foe)
				&& evacuateTown(hero, foe->visitablePos(), paths))
				return true;
			// The last town, and the walls, the garrison and this hero
			// together are routed: staying donates the army along with the
			// town. H_duel r01, day ~31: the field hero (25982) held our only
			// town against Dury (~42k), lost everything, and the town fell
			// anyway. Leaving with the garrison keeps the army for the retake
			// inside the 7-day grace. Only for a clear rout (the enemy keeps
			// half its army or more), because the raw evaluator this reads
			// runs optimistic for the attacker. The September 21st last-town
			// rule, heroes outside marching home rather than declining, is
			// untouched.
			// Only behind no walls. The siege model prices walls as +fort
			// defense and towers as a per-round trickle, and it reads real
			// sieges as routs: R3_duel r05 day 40 it called Fiur (68061, L9)
			// "keeps 84%" against Caitlin's 49324 plus a 7592 garrison in a
			// Castle, the field hero left, and the garrison alone then killed
			// 6771 of Fiur's army before falling. Town and game lost.
			if(lastTown && foe && hero->id.getNum() == fieldHeroId_
				&& town->fortLevel() == CGTownInstance::NONE)
			{
				const omniai::CombatVerdict v = siegeOnUs(hero, town, foe);
				// And only when the attacker is plainly stronger on the raw
				// count as well. The model read "keeps 63-85%" at parity in
				// four early losses of R7p_duel (r07: ours 13250 against
				// 12213, r15 11905 against 12300, r17 11065 against 12914,
				// r24 6694 against 8303, all on days 3-4, all unwalled); the
				// field hero left each time and the game was over by day 14.
				// The rule's own case, H_duel r01, was 25982 against ~42k.
				const double ourSide = double(hero->getArmyStrength())
					+ double(town->getArmyStrength());
				const bool plainlyStronger = double(foe->getArmyStrength()) >= 1.4 * ourSide;
				if(v.win && v.margin >= 0.5 && plainlyStronger)
				{
					// ratio is their damage output over ours; 1e9 means the
					// model saw no defender at all (L_duel r01 day 19 read
					// "keeps 100%" with a 13316 field hero inside, unexplained).
					std::ostringstream diag;
					diag << " [ratio " << std::setprecision(3) << v.ratio
					     << ", ours " << uint64_t(hero->getArmyStrength())
					     << "+" << uint64_t(town->getArmyStrength())
					     << (town->getGarrisonHero() ? " gh " + town->getGarrisonHero()->getNameTranslated() : std::string())
					     << (town->getVisitingHero() ? " vh " + town->getVisitingHero()->getNameTranslated() : std::string())
					     << ", theirs " << uint64_t(foe->getArmyStrength())
					     << ", fort " << int(town->fortLevel()) << "]";
					decisionLog_->line("   LAST TOWN routed: " + foe->getNameTranslated()
						+ " keeps " + std::to_string(int(v.margin * 100))
						+ "% against walls, garrison and " + hero->getNameTranslated()
						+ "; leaving with the army" + diag.str());
					if(evacuateTown(hero, foe->visitablePos(), paths))
						return true;
				}
			}
			// The hold trap: a field hero parked in a town the garrison and
			// walls already repel is a turn not spent pressing, and the
			// enemy main compounds in the meantime. If the town holds with
			// our hero out of the line, let it go.
			if(hero->id.getNum() == fieldHeroId_ && foe
				&& defenseHolds(nullptr, town, foe, hero))
			{
				decisionLog_->detail(town->getNameTranslated()
					+ " holds without the field hero; letting it press");
				return false;
			}
		}
		else
		{
			// Not standing in it. If the garrison and walls already repel
			// the threat with nobody added, the town is not really at risk
			// and marching a hero to it wastes the march.
			const CGHeroInstance * foe = strongestThreatNear(town);
			if(foe && defenseHolds(nullptr, town, foe))
			{
				decisionLog_->detail(town->getNameTranslated()
					+ " repels the threat with its own garrison; no defender needed");
				continue;
			}
			if(!lastTown && ours < bar)
			{
				decisionLog_->detail(town->getNameTranslated()
					+ " is threatened by more than we can beat, and it is not our"
					  " last town; staying out of its way");
				continue;
			}
		}

		// Already home. Normally hold it: sitting in a threatened town is
		// the point, and recruitInAllTowns buys into this hero every turn it
		// stays, so the garrison grows while the enemy decides.
		if(town->visitablePos() == hp)
		{
			// Unless the hero alone beats the attacker and can reach it this
			// turn. A loiterer that weak is a target, not a reason to park a
			// whole field army: measured, a 44097 hero held ~28 days against a
			// ~3.7k loiterer while the enemy's main compounded to 87k. Only
			// commit to a kill the evaluator calls a win with army to spare -
			// chasing a foe beyond our movement would leave the town open.
			// 0.75 here is 0.90 with the hero-fight cushion, the bar
			// huntWeakEnemyHero uses. At 0.30 (0.45 effective) the swat
			// took 10 fights in the saved logs: the 4 predicted at 0.48-0.72
			// were all lost with the whole field army (R6l r01 day 8, then
			// the town), and wins predicted at 0.77-0.89 cost 52-64%. Every
			// swat that paid was predicted at 0.95 or better.
			constexpr double SWAT_SURVIVOR = 0.75;
			const CGHeroInstance * foe = nullptr;
			for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
			{
				const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
				if(!e || !e->tempOwner.isValidPlayer()
					|| e->tempOwner == playerID)
					continue;
				const int3 ep = e->visitablePos();
				if(ep.z != hp.z)
					continue;
				const int dist =
					std::max(std::abs(ep.x - hp.x), std::abs(ep.y - hp.y));
				if(dist > 12)   // same straight-line bound the threat scan uses
					continue;
				if(!foe || e->getArmyStrength() > foe->getArmyStrength())
					foe = e;
			}
			if(foe && expectedToWin(hero, foe, SWAT_SURVIVOR))
			{
				std::vector<int3> steps =
					stepsToward(paths, hero, foe->visitablePos());
				// Reachable this turn: kill it now. Otherwise, when we
				// clearly outclass it, still walk toward it - a 44k hero
				// parked in the doorway while a 3.7k loiterer keeps its
				// distance is the hold trap, and hunting the threat down
				// removes it so the press can resume. Only the field hero
				// hunts; a support hero holding the post stays.
				const bool field = hero->id.getNum() == fieldHeroId_;
				const bool outclass = double(hero->getArmyStrength())
					>= double(foe->getArmyStrength()) * 2.5;
				if(!steps.empty()
					&& (int(steps.size()) <= hero->movementPointsRemaining()
						|| (field && outclass)))
				{
					decisionLog_->line(std::string(
						int(steps.size()) <= hero->movementPointsRemaining()
							? "   SWAT " : "   HUNT ") + foe->getNameTranslated()
						+ " loitering at " + foe->visitablePos().toString()
						+ ", ours " + std::to_string(uint64_t(hero->getArmyStrength()))
						+ " vs " + std::to_string(uint64_t(foe->getArmyStrength())));
					// The loiterer is the priced target, not a stray guard.
					return walk(hero, steps, true);
				}
			}
			decisionLog_->line("   HOLD " + town->getNameTranslated()
				+ ", an enemy hero is close");
			return true;
		}

		// Whoever is NEAREST used to answer this, and every hero answers it
		// independently, so on a one-town map the closest hero walked in
		// first whatever it was carrying. Measured on duel-v2: on day 2 our
		// best hero held 6662 against their Golwyn at 6258, a fight we could
		// have taken, and what actually happened was two fights lost at odds
		// 0.51 and 0.45. Those odds put about 2800 on the hero that fought,
		// which was the other one. We lost the 6662 hero between day 2 and
		// day 3 and the town on day 4.
		//
		// The last-town rule above is right that there is nothing to
		// preserve a hero FOR once the last town is going. It is right about
		// the hero that can fight. Feeding a weaker hero to an attacker that
		// beats it donates the hero and loses the town anyway, so the local
		// question is not "is this worth defending" but "am I the one who
		// should be standing here".
		//
		// Only when this hero would LOSE. A hero that clears the bar is the
		// right defender whoever else exists, and deferring to a stronger
		// one twenty tiles away would leave the town open for the walk.
		if(ours < bar && betterDefenderAvailable(hero, town))
		{
			decisionLog_->detail(town->getNameTranslated()
				+ " is more than I can hold and a stronger defender can reach"
				  " it; staying out of the way");
			continue;
		}

		CGPath path;
		if(!paths->getPath(path, town->visitablePos()))
			continue;

		std::vector<int3> steps;
		steps.reserve(path.nodes.size());
		for(auto it = path.nodes.rbegin(); it != path.nodes.rend(); ++it)
		{
			if(it->coord == hp)
				continue;
			if(it->isTeleportAction() || it->turns != 0)
				break;
			steps.push_back(hero->convertFromVisitablePos(it->coord));
		}
		if(steps.empty())
			continue;
		// A route walk() would refuse at the first step is not a defence: the
		// order goes out, nobody moves, and the hero stands where the attacker
		// finds it (K_duel r01 day 19, every round). Leave it to the escape.
		if(safePrefix(hero, steps) == 0)
		{
			decisionLog_->detail("route to " + town->getNameTranslated()
				+ " is fenced at the first step; not a defence");
			continue;
		}
		if(!best || steps.size() < bestSteps.size())
		{
			best = town;
			bestSteps = std::move(steps);
		}
	}

	if(!best)
		return false;

	// Town Portal where a march cannot win the race. A threatened town
	// reached on foot is days of walking while the enemy closes; the hero
	// that teleports onto it arrives this turn, on the walls, which is the
	// whole reason to build the guild that teaches the spell. Save the mana
	// when we are already close enough to make it on foot.
	if(int(bestSteps.size()) > hero->movementPointsRemaining()
		&& castTownPortalTo(hero, best))
	{
		targetedThisTurn_.insert(best->id.getNum());
		return true;
	}

	std::ostringstream o;
	o << "   DEFEND " << best->getNameTranslated() << " at "
	  << best->visitablePos().toString() << ", " << bestSteps.size() << " steps";
	decisionLog_->line(o.str());
	targetedThisTurn_.insert(best->id.getNum());
	return walk(hero, bestSteps);
}

void OmniAI::moveBestHero(const CGHeroInstance * hero)
{
	if(hero->movementPointsRemaining() <= 0)
		return;

	// A garrisoned hero cannot walk, and stepping every one of them out
	// here is what put the town holder on the entrance tile and blocked
	// the way in for everybody else. walk() does it at the moment a move
	// is actually ordered instead, so a hero that is going nowhere stays
	// in the garrison slot where it defends the town and blocks nobody.

	// Defence before acquisition. Everything below this decides which nice
	// thing to go and collect, and none of it is worth a capital.
	auto homePaths = pathCache_ ? pathCache_->getPathsInfo(hero) : nullptr;

	// Every hero answers a threat first, the support hero included: it used
	// to sit out a siege because holdTheFort told it to stand in the nearest
	// town, which was not always the one being attacked.
	if(!threatenedTowns_.empty() && homePaths
		&& defendThreatenedTown(hero, const_cast<CPathsInfo *>(homePaths.get())))
		return;

	// Last stand: a field hero with no town left has seven days, and running
	// from their hero only spends them. A town it can take (its own lost one,
	// if the evaluator says so) or their capital comes before the escape.
	if(homePaths && hero->id.getNum() == fieldHeroId_ && cb->getTownsInfo().empty())
	{
		CPathsInfo * fp = const_cast<CPathsInfo *>(homePaths.get());
		if(conquestMarch(hero, fp) || raidEnemyCapital(hero, fp))
			return;
	}

	// Then every hero standing in the open inside a stronger enemy hero's
	// reach gets out of it: into a town it reaches today, else back out of
	// range. Everything below picks something to walk to, and none of it
	// asks whether the hero survives the enemy's next turn where it ends.
	if(homePaths && escapeStrongerHero(hero, const_cast<CPathsInfo *>(homePaths.get())))
		return;

	// A hero standing in a town we just captured leaves a holding garrison
	// before it moves on, so the walls are not a free walk-in for the next
	// scout to wander by. Only while the garrison is still thin; once troops
	// are on the wall the deposit stops re-firing.
	if(!freshlyCaptured_.empty())
	{
		for(const CGTownInstance * town : cb->getTownsInfo())
		{
			if(!town || town->tempOwner != playerID)
				continue;
			if(!freshlyCaptured_.count(town->id.getNum()))
				continue;
			if(town->getVisitingHero() != hero && town->getGarrisonHero() != hero)
				continue;
			if(double(town->getArmyStrength()) < double(hero->getArmyStrength()) / 3)
				leaveHoldingGarrison(hero, town);
			break;
		}
	}

	// Anyone who is not the field hero holds a town and does nothing else.
	// Narrow on purpose: a second hero with ambitions is a second hero that
	// dies, and the one job it has here is the one the field hero cannot do
	// while it is on the far side of the map.
	if(fieldHeroId_ >= 0 && hero->id.getNum() != fieldHeroId_ && homePaths)
	{
		CPathsInfo * hp = const_cast<CPathsInfo *>(homePaths.get());
		// A move walk() already cut short of a stronger enemy this turn:
		// the same chain would reissue it every round. Hold a town instead.
		if(heldThisTurn_.count(hero->id.getNum()))
		{
			holdTheFort(hero, hp);
			return;
		}
		// The burner runs its pickup circuit and otherwise stands in a town;
		// it is not worth feeding to a chase. Everyone else carries when the
		// load is worth it and holds a post otherwise. While the field hero
		// is sealed out, though, a hero in the pocket does the one thing it
		// cannot: press the seal from inside and carry the pool through it.
		if(hero->id.getNum() == collectorHeroId_)
		{
			if(sallyOut(hero, hp) || collectorCircuit(hero, hp)
				|| probeTowardMine(hero, hp)
				|| deepScout(hero, hp) || holdTheFort(hero, hp))
				return;
		}
		else if(hero->id.getNum() == secondFieldId_)
		{
			// The second fighter: it takes the captures and route guards the
			// main field hero cannot reach, and otherwise holds a post. It
			// does not courier. This is how a taken town gets held while the
			// press keeps going on an open map.
			if(runRegistryTask(hero, hp) || sallyOut(hero, hp)
				|| holdTheFort(hero, hp))
				return;
		}
		else if(runRegistryTask(hero, hp) || sallyOut(hero, hp)
			|| supplyRun(hero, hp)
			// Big map, mostly unseen: the day's scout goes out ahead of the
			// errands, after the couriers, so a loaded courier keeps its job.
			|| (scoutingDue() && deepScout(hero, hp))
			|| runCollectTask(hero, hp) || probeTowardMine(hero, hp) || deepScout(hero, hp)
			|| (holderMayCollect(hero) && collectorCircuit(hero, hp))
			|| holdTheFort(hero, hp))
			return;
	}

	// The field hero's real job. While a conquest target is held the march
	// comes before shopping, because the burner's deposits would otherwise
	// keep pulling it home forever; with no target held, a home errand gets
	// its turn first so the army stocks up before it commits.
	if(hero->id.getNum() == fieldHeroId_ && homePaths)
	{
		CPathsInfo * fp = const_cast<CPathsInfo *>(homePaths.get());
		// Sealed-pocket tripwire that does not depend on the scout fallback
		// being reached: fog flat for 3 days with no hostile town known
		// means the reachable content is spent, whatever chores filled the
		// days. Park the stack behind walls before a breaker finds it in
		// the open. Declines to the normal chain while anything is still
		// being revealed.
		// An island that is fully explored is not a sealed pocket: the way
		// out is a boat, not a garrison.
		if(frontierDryDays_ >= 3 && conquestTargetId_ < 0 && seekCrossing(hero, fp))
			return;
		if(frontierDryDays_ >= 3 && conquestTargetId_ < 0
			&& !frontierLeft(hero, fp) && pocketHold(hero, fp))
			return;
		if(conquestTargetId_ >= 0)
		{
			if(conquestMarch(hero, fp))
				return;
			// Target dropped this round; fall through to normal logic.
		}
		// A held scout target beats the home errand, which is the whole
		// difference between exploring and pacing. Measured on Twins with
		// the mandate but no commitment: 53 EXPLORE orders against 52 HOME
		// errands, so the hero stepped toward the fog and was pulled back
		// the next morning, every morning. It scouted 68 times in a 53 day
		// game and never uncovered a town to march on. A conquest target
		// still outranks a scout target, and the budget in scoutMarch
		// keeps the errand from being starved for good.
		// A committed resupply march outranks a held scout target: the
		// garrison pool is the field hero's army growth, and fog or a guard
		// sealing the way home does not open by scouting somewhere else.
		// A field hero inside a sealed pocket sallies toward whoever is
		// sealed out - it has to get through the choke to compound, and a
		// hero that keeps collecting the pocket's weekly dribble instead
		// never does.
		else
		{
			// Reserved first look for the scoring pass (Design B, gated). It
			// may take the day ahead of housekeeping only for a target worth
			// pre-empting for: a one-shot pile that pays once is never that.
			// The ungated first-look this replaces measured a day-1 crystal
			// pile at 162 points outranking the sawmill that gated the fort,
			// and four of six games over inside a week. It is once a day, on
			// the field hero's first call - running it every round starves
			// the housekeeping chains several times over.
			constexpr double RESERVED_LOOK_WORTH = 300.0;
			const int today = cb->getDate(Date::DAY);
			if(today != reservedLookDay_)
			{
				reservedLookDay_ = today;
				if(scoringPassMove(hero, RESERVED_LOOK_WORTH))
					return;
			}
			// seekCrossing sits ahead of every order that walks at the
			// enemy: on Emerald Isles (R6z_water) the hunt and the siege
			// press each reached the same shore tile and ended the turn
			// there, so the crossing never ran. It does nothing while any
			// land route exists, fog included.
			if(resupplyMarch(hero, fp)
				|| pressCapital(hero, fp)
				|| runRegistryTask(hero, fp)
				|| runDeferredGuards(hero, fp)
				|| seekCrossing(hero, fp)
				|| huntWeakEnemyHero(hero, fp)
				|| raidEnemyCapital(hero, fp)
				|| grindNearestFight(hero, fp)
				|| scoutMarch(hero, fp)
				|| sallyOut(hero, fp)
				|| runHomeErrand(hero, fp)
				|| conquestMarch(hero, fp))
				return;
		}

		// Scouting mandate. exploreFrontier has only ever run as a fallback:
		// after the scoring pass, and then only when the best object was one
		// we had already been to. So a field hero with any fresh pickup in
		// reach never scouted. Fog is solid to the pathfinder and an unseen
		// town is not in getAllVisitableObjs, so conquestMarch cannot target
		// a capital nobody has looked at, and six Twins runs measured four
		// that never marched at all. When no town we can see is anyone's but
		// ours, collecting cannot win the game and finding one can.
		// A mine we are short of outranks the mandate. The mandate returns
		// before the scoring pass and spends the day on fog, so a Sawmill
		// scored at 1537, the best object on the duel-v2 map, was skipped
		// every single day with "no step is reachable this turn" while wood
		// sat at zero and the build queue stopped.
		if(grabScarceMine(hero, fp) || probeTowardMine(hero, fp))
			return;

		if(!knowsHostileTown())
		{
			// Strong enough to matter but nothing to march on: the enemy is
			// sealed off and the win is finding them, so press the seal toward
			// their side before spending the day on our own last fog.
			if(huntTheEnemy(hero, fp))
				return;
			if(exploreFrontier(hero, fp))
			{
				decisionLog_->line("   SCOUT, no town to take is known");
				return;
			}
			decisionLog_->detail("no town known and nothing left to reveal");
		}
		// Nothing on this landmass leads to the enemy: sail.
		if(seekCrossing(hero, fp))
			return;
	}
	else if(homePaths && runHomeErrand(hero, const_cast<CPathsInfo *>(homePaths.get())))
		return;

	// Everything with a claim on the day has declined. The scoring pass is
	// the universal fallback: best reachable object, or explore when there
	// is none. Ungated here - the reserved look above already had its turn.
	scoringPassMove(hero, 0.0);
}

bool OmniAI::scoringPassMove(const CGHeroInstance * hero, double minScoreToTake)
{
	const auto packed = registry_->snapshotPacked();
	decisionLog_->detail("scoring " + std::to_string(packed.size()) + " known objects");

	// Score candidates in the arena; declare quiescence on exit so retired
	// shadow objects can be reclaimed.
	omniai::QuiescentGuard quiescent;

	tbb::task_arena arena(profile_->maxThreads > 0
		? int(profile_->maxThreads) : tbb::task_arena::automatic);

	struct Scored { omniai::GenerationalHandle h; double score; };
	tbb::concurrent_vector<Scored> results;

	arena.execute([&]{
		tbb::parallel_for(tbb::blocked_range<size_t>(0, packed.size()),
			[&](const tbb::blocked_range<size_t> & r){
				for(size_t i = r.begin(); i < r.end(); ++i)
				{
					const auto h = omniai::GenerationalHandle::unpack(packed[i]);
					if(!registry_->validate(h))
						continue; // stale: object died after snapshot
					const int64_t objId = registry_->resolve(h);
					const CGObjectInstance * obj = objId >= 0
						? cb->getObj(ObjectInstanceID(int32_t(objId)), false)
						: nullptr;
					if(!obj || !obj->isVisitable())
						continue;
					const int3 delta = obj->visitablePos() - hero->pos;
					const double travel = std::sqrt(double(
						delta.x*delta.x + delta.y*delta.y));
					// One unscoreable object must not cost the whole turn.
					// A throw here escapes the parallel_for, unwinds the
					// entire scoring pass, and the hero then does nothing
					// all day. Skip the object, say which one, carry on.
					try
					{
						// No shadow DAG on this thread. See UtilityEvaluator.h.
						const auto b = evaluator_->evaluate(*obj, *hero, travel, false);
						results.push_back({h, b.final});
					}
					catch(const std::exception & e)
					{
						logAi->warn("OmniAI: could not score object %d: %s",
							int(objId), e.what());
					}
				}
			});
	});

	// Walk candidates best-first until one yields a legal path. The top
	// scorer is often unreachable (across water, walled off, occupied
	// tile), and getPath is a cheap lookup into the precomputed paths -
	// so iterate rather than idling the hero on a single best.
	std::vector<Scored> ranked(results.begin(), results.end());
	auto byScore = [](const Scored & a, const Scored & b){ return a.score > b.score; };
	std::sort(ranked.begin(), ranked.end(), byScore);

	// Second pass, this thread only, and only for candidates with a real
	// chance of winning. The shadow-DAG term is the expensive half of the
	// score and the only part that reads the live bonus graph, so running it
	// here instead of in the arena takes the concurrent readers from every
	// worker times every object down to one thread times a couple of dozen
	// objects. The engine can still relink a node underneath us, but the
	// window is a fraction of what it was.
	//
	// Skipped outright while one of our heroes is in a fight. That is when
	// the engine rebuilds bonus tables hardest, and this is the only part of
	// the score that reads them. Waiting for the battle instead was tried
	// and deadlocked: the handlers that END a battle are queued on the same
	// four-worker pool that runs the turn, so a worker parked on a battle
	// starves the answer that would finish it. It froze the game twice for
	// the full 60 second deadline in one 240 second soak. Losing the DAG
	// term for one round costs a little accuracy and nothing else.
	constexpr size_t DEEP_SCORED = 24;
	if(battlesActive_.load() == 0)
	{
		const size_t deep = std::min<size_t>(ranked.size(), DEEP_SCORED);
		for(size_t i = 0; i < deep; ++i)
		{
			const int64_t oid = registry_->resolve(ranked[i].h);
			const CGObjectInstance * cand = oid >= 0
				? cb->getObj(ObjectInstanceID(int32_t(oid)), false) : nullptr;
			if(!cand)
				continue;
			try
			{
				ranked[i].score += evaluator_->shadowTerm(*cand, *hero);
				// What the fight for it costs, and what it hands over.
				ranked[i].score += fightValue(cand, hero);
				// And whether we are in any position to go and take it.
				ranked[i].score += strategicBonus(cand);
			}
			catch(const std::exception & e)
			{
				logAi->warn("OmniAI: shadow term failed for object %d: %s",
					int(oid), e.what());
			}
		}
		std::sort(ranked.begin(), ranked.begin() + deep, byScore);
	}

	{
		std::ostringstream o;
		o << "  " << hero->getNameTranslated() << " at " << hero->visitablePos().toString()
		  << ", " << hero->movementPointsRemaining() << " movement, "
		  << ranked.size() << " candidates scored";
		decisionLog_->line(o.str());
		// The top few explain the choice; the whole list would be unreadable.
		for(size_t i = 0; i < ranked.size() && i < 5; ++i)
		{
			const int64_t oid = registry_->resolve(ranked[i].h);
			const CGObjectInstance * cand = oid >= 0
				? cb->getObj(ObjectInstanceID(int32_t(oid)), false) : nullptr;
			std::ostringstream d;
			d << (i ? "        " : "   best ")
			  << (cand ? cand->getObjectName() : "?") << " at "
			  << (cand ? cand->visitablePos().toString() : "?")
			  << "  score " << ranked[i].score;
			decisionLog_->line(d.str());
		}
	}

	auto paths = pathCache_ ? pathCache_->getPathsInfo(hero) : nullptr;
	if(!paths)
	{
		decisionLog_->detail("no path data for this hero");
		return false;
	}

	// How much of the world is actually open to this hero. The scored list
	// counts everything we have ever seen; this counts what the pathfinder
	// will route to, which after a few turns of fog is a very different
	// number. An array lookup per candidate, no paths constructed.
	int reachableCandidates = 0;
	for(const auto & s : ranked)
	{
		const int64_t oid = registry_->resolve(s.h);
		const CGObjectInstance * o = oid >= 0
			? cb->getObj(ObjectInstanceID(int32_t(oid)), false) : nullptr;
		if(!o)
			continue;
		const CGPathNode * n = const_cast<CPathsInfo *>(paths.get())
			->getNode(o->visitablePos(), EPathfindingLayer::LAND);
		if(n && n->reachable())
			++reachableCandidates;
	}

	int attempts = 0;
	bool gridDumped = false;

	// The first candidate that yields a legal batch is held here rather
	// than sent. Whether it is worth the day gets decided below, and a
	// fallback that comes up empty still needs these steps to fall back on.
	const CGObjectInstance * bestObj = nullptr;
	int64_t bestId = -1;
	double bestScore = 0.0;
	std::vector<int3> bestSteps;

	// The best move that ends somewhere a stronger hero could reach. Held in
	// case nothing safe turns up, because standing still beside that hero is
	// no safer than moving.
	bool haveFallback = false;
	const CGObjectInstance * fallbackObj = nullptr;
	int64_t fallbackId = -1;
	double fallbackScore = 0.0;
	std::vector<int3> fallbackSteps;

	for(const auto & s : ranked)
	{
		if(attempts++ >= 32)
			break; // bound getPath calls per hero per turn

		const int64_t objId = registry_->resolve(s.h);
		const CGObjectInstance * obj = objId >= 0
			? cb->getObj(ObjectInstanceID(int32_t(objId)), false)
			: nullptr;
		if(!obj)
			continue;

		// Already walked toward this one today. Without this a hero can
		// oscillate: it takes a mine, the mine still scores positively
		// because owned objects keep a small share of their worth, and it is
		// the nearest thing again from where the hero now stands. The hero
		// spends its whole day pacing between two objects. Anything skipped
		// here is available again tomorrow.
		if(targetedThisTurn_.count(objId))
			continue;

		// moveHero is a neighbor-step protocol: the server validates
		// every path element against areNeighbours. CGPath stores nodes
		// DESTINATION-FIRST (nodes[0]=target, nodes.back()=hero tile),
		// so iterate in reverse and skip the hero's own tile. turns>0
		// nodes are out of this turn's MP - sending them would fail
		// mid-pack. Teleport actions cannot appear: the pathfinder was
		// built with all useTeleport* options off.
		CGPath path;
		if(!paths->getPath(path, obj->visitablePos()))
		{
			if(!gridDumped)
			{
				gridDumped = true;
				dumpPathGrid(hero, const_cast<CPathsInfo *>(paths.get()));
			}
			if(attempts <= 3)
			{
				// Say WHY there is no route. "unreachable" and "reachable but
				// the search never got there" look identical from outside and
				// have completely different causes.
				// getNode(coord) hides the truth: when the land node is not
				// reachable it silently returns the SAIL node instead, which
				// for a land tile is always NOT_SET. Ask for the land layer
				// directly or the diagnosis is meaningless.
				const CGPathNode * n = const_cast<CPathsInfo *>(paths.get())
					->getNode(obj->visitablePos(), EPathfindingLayer::LAND);
				std::ostringstream o;
				o << "skip " << obj->getObjectName() << " at "
				  << obj->visitablePos().toString() << ": no route ("
				  << (n ? (n->reachable() ? "tile reachable" : "tile NOT reachable")
				        : "no node")
				  << ", accessibility " << (n ? int(n->accessible) : -1)
				  << ", turns " << (n ? n->turns : -1)
				  << ", tile visible to us: " << (cb->isVisible(obj->visitablePos()) ? "yes" : "no")
				  << ")";
				decisionLog_->detail(o.str());
			}
			continue;
		}
		if(path.nodes.size() <= 1)
		{
			if(attempts <= 3)
				decisionLog_->detail(std::string("skip ") + obj->getObjectName()
					+ ": already standing on it");
			continue;
		}

		std::vector<int3> steps;
		steps.reserve(path.nodes.size() - 1);
		// A step onto a hostile tile is an attack, whatever the destination
		// was scored for - Ivor lost a 12k stack on day 9 of 72x72_s7 when a
		// scored pickup's route crossed a guard he had no business fighting.
		// The destination's own guard is already priced in; this checks the
		// tiles on the way there. A fight that fails the commit margin makes
		// the route unsafe, not just unwise.
		const CArmedInstance * transitGuard = nullptr;
		bool deferredRoute = false;
		for(auto it = path.nodes.rbegin(); it != path.nodes.rend(); ++it)
		{
			if(it->coord == hero->visitablePos())
				continue; // current tile - not a step
			if(it->isTeleportAction() || it->turns != 0)
				break;
			// A deferred fight fences its tile for the day to every move,
			// scored pickups included: a route that crosses it cannot be
			// taken no matter what the destination is worth.
			if(isDeferredTile(it->coord))
			{
				deferredRoute = true;
				break;
			}
			if(!transitGuard)
			{
				const TerrainTile * tt = cb->getTile(it->coord, false);
				if(tt)
					for(const auto & oid : tt->visitableObjects)
					{
						const CGObjectInstance * obj2 = cb->getObj(oid, false);
						const auto * armed = obj2
							? dynamic_cast<const CArmedInstance *>(obj2) : nullptr;
						if(!armed)
							continue;
						if(obj2->ID == Obj::MONSTER
							|| (obj2->tempOwner.isValidPlayer()
								&& obj2->tempOwner != hero->tempOwner))
						{
							transitGuard = armed;
							break;
						}
					}
			}
			steps.push_back(hero->convertFromVisitablePos(it->coord));
		}
		if(deferredRoute)
		{
			if(attempts <= 3)
				decisionLog_->detail(std::string("skip ") + obj->getObjectName()
					+ ": route crosses a deferred fight");
			continue;
		}
		if(transitGuard && !expectedToWin(hero, transitGuard, commitMargin(hero)))
		{
			if(attempts <= 3)
				decisionLog_->detail(std::string("skip ") + obj->getObjectName()
					+ ": route crosses a guard of "
					+ std::to_string(uint64_t(transitGuard->getArmyStrength()))
					+ " we cannot cleanly beat");
			continue;
		}
		// steps are anchor positions; the leash and the reach are about tiles.
		const int3 endTile = steps.empty() ? hero->visitablePos()
			: hero->convertToVisitablePos(steps.back());
		if(!steps.empty() && !leashAllows(hero, endTile))
		{
			if(attempts <= 3)
				decisionLog_->detail(std::string("skip ") + obj->getObjectName()
					+ ": ends outside the leash");
			continue;
		}
		if(!steps.empty())
		{
			// Where this move ENDS is what matters, not where it was aimed.
			// A path that stops in a stronger hero's reach hands it the
			// first move, and the first move decides these games.
			const bool exposed = withinEnemyReach(endTile, hero);
			if(exposed && !haveFallback)
			{
				haveFallback = true;
				fallbackObj = obj;
				fallbackId = objId;
				fallbackScore = s.score;
				fallbackSteps = steps;
				continue;   // keep looking for somewhere safe to stop
			}
			if(exposed)
				continue;

			bestObj = obj;
			bestId = objId;
			bestScore = s.score;
			bestSteps = std::move(steps);
			break;
		}
		if(attempts <= 3)
			decisionLog_->detail(std::string("skip ") + obj->getObjectName()
				+ ": route exists but no step is reachable this turn ("
				+ std::to_string(path.nodes.size()) + " nodes)");
	}

	// Everything scored was either unreachable or already walked toward.
	// The visible world is a bubble: fogged tiles read BLOCKED to the
	// pathfinder, so a target one tile past the fog edge cannot be routed
	// to at all. The only way to grow the bubble is to stand at its edge -
	// walk toward the cheapest reachable tile that touches unrevealed
	// ground, and the next round has new objects to score.
	if(!bestObj && haveFallback)
	{
		// Buying distance first: the advance-anyway fallback walked into
		// the enemy's first move and lost the hero inside a day in 8 of 9
		// logged cases. When retreat gains nothing the fallback below is
		// still the least-bad option - the enemy catches us either way, so
		// the pickup at least comes first.
		if(retreatMove(hero, const_cast<CPathsInfo *>(paths.get())))
			return true;
		decisionLog_->detail("nowhere safe to stop; taking the best move anyway");
		bestObj = fallbackObj;
		bestId = fallbackId;
		bestScore = fallbackScore;
		bestSteps = std::move(fallbackSteps);
	}

	if(!bestObj)
	{
		// A gated reserved look found nothing reachable; housekeeping still
		// gets its turn rather than spending the day on the fallback.
		if(minScoreToTake > 0.0)
			return false;
		exploreFrontier(hero, const_cast<CPathsInfo *>(paths.get()));
		return true;
	}

	// Reserved-look gate (Design B): a target below the worth bar is not
	// reason enough to pre-empt the housekeeping chains, so the call falls
	// through to them as if the look had not run. The ungated bottom call
	// passes 0 and takes whatever is best, exactly as before.
	if(minScoreToTake > 0.0 && bestScore <= minScoreToTake)
		return false;

	// The pacing stall, and why this is no longer simply "walk to the best
	// thing you can reach". Measured over a 7196 game-day soak: 13939 of
	// 13950 move orders went to five objects. 7046 of them went to the
	// hero's own town and 5460 to a single Mercury pile it never once
	// reached, because the morning walk home undid the previous evening's
	// walk out. The explore fallback ran once in twenty game years, since
	// the town was always pathable and that alone counted as having moved.
	//
	// So a lap of the same circuit now has to beat the alternatives instead
	// of pre-empting them. A target is a lap when we own it, when a hero
	// has already stood on it, or when a previous game recorded it spent.
	// A dwelling holding stock we can pay for and find room for is never a
	// lap; that is the shop being open, and it is how the army grows.
	const bool ours = bestObj->tempOwner == hero->tempOwner;
	bool stoodHereBefore = false;
	{
		std::lock_guard lock(visitedMutex_);
		stoodHereBefore = visitedObjs_.count(bestId) != 0;
	}
	const bool spent = omniai::LearningStore::instance().isDepleted(int32_t(bestId));
	// Somewhere with stock we can buy, or a garrison holding troops this hero
	// could pick up, is not a lap. It is the errand the hero exists to run.
	//
	// Unless it is the only place we can get to. A dwelling regrows every
	// week, so it excuses a revisit every week, forever, and two of them
	// beside each other will hold a hero between them for a whole game:
	// measured at 341 of 350 position samples on two tiles over 139 days,
	// with explore firing 19 times in all of it while every other candidate
	// read "no route". Fog is a wall to the pathfinder and only exploring
	// takes it down, so a hero that never explores never earns a third
	// option.
	//
	// An errand is a thing you do INSTEAD of something else. With nothing
	// else reachable it is not an errand, it is pacing.
	constexpr int ENOUGH_CHOICE = 3;
	const bool anywhereElse = reachableCandidates >= ENOUGH_CHOICE;
	const bool shopOpen = anywhereElse
		&& (dwellingOffersRecruits(dynamic_cast<const CGDwelling *>(bestObj), hero)
			|| garrisonWaitingFor(dynamic_cast<const CGTownInstance *>(bestObj), hero));
	if(!anywhereElse)
		decisionLog_->detail("only " + std::to_string(reachableCandidates)
			+ " reachable target(s); a shop is not an errand when it is the only door");

	// No "unless something outranks it" clause here, and the 476 day soak
	// is why. Once the map around the hero is picked clean its own town is
	// the highest scoring object left on it - a town carries a fat bonus
	// node and the 0.15 owned multiplier does not cancel that - so the
	// clause switched itself off exactly when it was needed and the hero
	// spent 350 days walking between its town and one artifact eight tiles
	// away, arriving at neither. Whether there is anywhere better to go is
	// not a question the score can answer; exploreFrontier answers it, and
	// if the answer is no we still take the lap below.
	if((ours || stoodHereBefore || spent) && !shopOpen)
	{
		// A lap is not worth pre-empting housekeeping for. The reserved look
		// only takes the day for a fresh target that clears the bar.
		if(minScoreToTake > 0.0)
			return false;
		decisionLog_->detail(std::string("been to ") + bestObj->getObjectName()
			+ " already; looking for new ground or a way out first");
		if(exploreFrontier(hero, const_cast<CPathsInfo *>(paths.get())))
			return true;
		// Nothing left to reveal and nothing beatable in the way. Another
		// lap still beats standing still.
	}

	std::ostringstream mo;
	mo << "   MOVE" << (minScoreToTake > 0.0 ? " (reserved)" : "")
	   << " toward " << bestObj->getObjectName() << " at "
	   << bestObj->visitablePos().toString() << ", " << bestSteps.size()
	   << " steps, score " << bestScore;
	decisionLog_->line(mo.str());
	targetedThisTurn_.insert(bestId);
	return walk(hero, bestSteps);
}

std::vector<int3> OmniAI::stepsToward(CPathsInfo * paths,
	const CGHeroInstance * hero, const int3 & target) const
{
	// CGPath is DESTINATION-FIRST (nodes[0] is the target, nodes.back() the
	// hero's own tile), so walk it backwards, skip the tile we are standing
	// on, and stop at the first node this turn's movement cannot reach:
	// moveHero validates every element and a node with turns != 0 fails
	// mid-pack. Teleport nodes are also a stop: the hero halts at the
	// monolith entrance and the forced dialog carries it through.
	std::vector<int3> steps;
	CGPath path;
	if(!paths || !paths->getPath(path, target))
		return steps;

	const int3 hp = hero->visitablePos();
	steps.reserve(path.nodes.size());
	for(auto it = path.nodes.rbegin(); it != path.nodes.rend(); ++it)
	{
		if(it->coord == hp)
			continue;
		if(it->isTeleportAction() || it->turns != 0)
			break;
		steps.push_back(hero->convertFromVisitablePos(it->coord));
	}
	return steps;
}

uint64_t OmniAI::townBusinessValue(const CGTownInstance * town,
	const CGHeroInstance * hero) const
{
	// What a trip home would actually add to this hero's army, in the same
	// units the danger check uses, so it can be weighed against what the
	// hero already has.
	if(!town || !hero || !cb)
		return 0;

	uint64_t value = 0;

	// Stock we can pay for and carry.
	for(const auto & level : town->creatures)
	{
		if(level.first == 0 || level.second.empty())
			continue;
		const CreatureID creID = level.second.back();
		const auto * cre = creID.toCreature();
		if(!cre || !hero->getSlotFor(creID).validSlot())
			continue;
		const int cost = int(cb->getResourceAmount() / cre->getFullRecruitCost());
		const int affordable = std::min<int>(int(level.first), cost);
		if(affordable > 0)
			value += uint64_t(affordable) * uint64_t(std::max(0, cre->getAIValue()));
	}

	// Troops already bought and standing in the garrison waiting for a lift.
	// A stack with no free slot still counts for what swapping it in for one
	// of the hero's weaker stacks would add (collectGarrison does the swap):
	// counting it as nothing is what left a 17.6k pool idle beside a 7.9k
	// field hero on J3 MapGen s17.
	std::vector<uint64_t> noRoom, heroStacks;
	for(const auto & entry : town->Slots())
	{
		if(!entry.second || !entry.second->getType())
			continue;
		const auto * cre = entry.second->getCreature();
		if(!cre)
			continue;
		const uint64_t v = uint64_t(entry.second->getCount())
			* uint64_t(std::max(0, cre->getAIValue()));
		if(hero->getSlotFor(cre->getId()).validSlot())
			value += v;
		else
			noRoom.push_back(v);
	}
	for(const auto & entry : hero->Slots())
		if(entry.second && entry.second->getCreature())
			heroStacks.push_back(uint64_t(entry.second->getCount())
				* uint64_t(std::max(0, entry.second->getCreature()->getAIValue())));
	std::sort(noRoom.rbegin(), noRoom.rend());
	std::sort(heroStacks.begin(), heroStacks.end());
	for(size_t i = 0; i < noRoom.size() && i < heroStacks.size(); ++i)
		if(noRoom[i] > heroStacks[i])
			value += noRoom[i] - heroStacks[i];
	return value;
}

bool OmniAI::runHomeErrand(const CGHeroInstance * hero, CPathsInfo * paths)
{
	// Enough of the hero's own strength to be worth the walk. Below this the
	// trip costs more movement than the troops are worth and the hero should
	// stay out and keep taking ground.
	constexpr double WORTH_THE_TRIP = 0.15;

	const uint64_t ours = std::max<uint64_t>(1, hero->getArmyStrength());
	const int3 hp = hero->visitablePos();

	const CGTownInstance * best = nullptr;
	std::vector<int3> bestSteps;
	uint64_t bestValue = 0;
	uint64_t topValue = 0;
	bool topHadPath = false;
	const CGTownInstance * topTown = nullptr;

	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;

		const uint64_t value = townBusinessValue(town, hero);
		if(value > topValue)
		{
			topValue = value;
			topTown = town;
			CGPath probe;
			topHadPath = paths && paths->getPath(probe, town->visitablePos());
		}
		if(double(value) < double(ours) * WORTH_THE_TRIP)
			continue;

		// Already standing in it. The buying pass at the top of the turn has
		// this covered, and walking to where we are is not a plan.
		if(town->visitablePos() == hp)
			return false;

		// The visitor slot IS the town's entrance tile, so a hero of ours
		// parked there blocks the gate. The engine resolves the walk as a
		// blocking hero-to-hero visit: we stay where we are and the step
		// cost is deducted anyway, so the round loop counts it as progress
		// and orders the same walk again. Measured on duel-v2: ten identical
		// HOME orders in one day, three days running, and a field hero that
		// did nothing from day 1 to the end of the game while our own holder
		// stood in the doorway.
		if(const CGHeroInstance * doorman = town->getVisitingHero())
		{
			if(doorman != hero)
			{
				decisionLog_->detail("home errand skips "
					+ town->getNameTranslated() + ": "
					+ doorman->getNameTranslated() + " is in the doorway");
				continue;
			}
		}

		std::vector<int3> steps = stepsToward(paths, hero, town->visitablePos());
		if(steps.empty())
			continue;
		if(!best || value > bestValue)
		{
			best = town;
			bestValue = value;
			bestSteps = std::move(steps);
		}
	}

	if(!best)
	{
		if(topValue > 0 && topTown)
		{
			decisionLog_->detail("home errand declined: richest pool worth "
				+ std::to_string(topValue) + " against our " + std::to_string(ours)
				+ " at " + topTown->getNameTranslated() + " "
				+ topTown->visitablePos().toString()
				+ (topHadPath ? "" : ", and no path reaches it"));
			if(!topHadPath)
			{
				// The pool is worth the walk and the way is sealed - commit
				// to the march instead of forgetting this town exists again
				// next round.
				//
				// Only if this town is not already on cooldown from a march
				// that just gave up on it. Re-arming here every round is what
				// made RESUPPLY_BUDGET meaningless and cost the field hero 32
				// straight days on Twins.
				const int32_t tid = topTown->id.getNum();
				const auto cool = resupplyCooldown_.find(tid);
				if(cool != resupplyCooldown_.end()
					&& cb->getDate(Date::DAY) < cool->second)
				{
					decisionLog_->detail("resupply march not re-armed for "
						+ topTown->getNameTranslated() + ": cooling off until day "
						+ std::to_string(cool->second));
					return false;
				}
				resupplyTownId_ = tid;
				resupplyTurnsLeft_ = RESUPPLY_BUDGET;
				return resupplyMarch(hero, paths);
			}
		}
		return false;
	}

	std::ostringstream o;
	o << "   HOME to " << best->getNameTranslated() << " at "
	  << best->visitablePos().toString() << ", worth " << bestValue
	  << " against our " << ours << ", " << bestSteps.size() << " steps";
	decisionLog_->line(o.str());
	targetedThisTurn_.insert(best->id.getNum());
	return walk(hero, bestSteps);
}

bool OmniAI::withinEnemyReach(const int3 & tile, const CGHeroInstance * hero) const
{
	// A day's travel for a hero on open ground. Deliberately generous: being
	// wrong in this direction costs a detour, being wrong the other way costs
	// the hero and usually the game.
	constexpr int REACH = 12;

	// Only heroes that would actually beat us. Walking near a weaker one is
	// how a map gets taken, not how a game gets lost.
	constexpr double THEY_WIN_AT = 1.0;

	if(!cb)
		return false;
	const double ours = double(hero->getArmyStrength());

	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
		if(!e || !e->tempOwner.isValidPlayer() || e->tempOwner == playerID)
			continue;
		if(double(e->getArmyStrength()) < ours * THEY_WIN_AT)
			continue;
		const int3 ep = e->visitablePos();
		if(ep.z != tile.z)
			continue;
		if(std::max(std::abs(ep.x - tile.x), std::abs(ep.y - tile.y)) <= REACH)
			return true;
	}
	return false;
}

bool OmniAI::garrisonWaitingFor(const CGTownInstance * town,
	const CGHeroInstance * hero) const
{
	// Troops bought into the garrison while nobody was home are worth a trip
	// back, and only if this hero can actually take them: a full army with no
	// matching stack would walk home for nothing.
	if(!town || !hero)
		return false;
	for(const auto & entry : town->Slots())
	{
		if(!entry.second || !entry.second->getType())
			continue;
		if(hero->getSlotFor(entry.second->getCreature()->getId()).validSlot())
			return true;
	}
	return false;
}

bool OmniAI::dwellingOffersRecruits(const CGDwelling * dwelling, const CArmedInstance * dst)
{
	if(!dwelling || !dst || !cb)
		return false;
	for(const auto & level : dwelling->creatures)
	{
		if(level.first == 0 || level.second.empty())
			continue;
		const CreatureID creID = level.second.back();
		const auto * cre = creID.toCreature();
		if(!cre)
			continue;
		// getSlotFor gives back the hero's existing stack of this creature
		// or a free slot, and an invalid slot when the army is full of
		// seven other things. recruitFromDwelling gives up on that level
		// for the same reason, so counting it as stock on offer is what
		// made the town look open every single morning.
		if(!dst->getSlotFor(creID).validSlot())
			continue;
		if(int(cb->getResourceAmount() / cre->getFullRecruitCost()) > 0)
			return true;
	}
	return false;
}

bool OmniAI::guardsItself(const CGObjectInstance * obj, const CGHeroInstance * hero) const
{
	// A neutral object whose own army fights whoever visits it: creature
	// banks, unowned dwellings with guards, neutral towns and garrisons,
	// guarded artifacts, resources and Pandora's boxes. None of them is a
	// MONSTER and none has a player owner, so no fence or danger check ever
	// saw them. K_duel: seven collectors and supports died walking into a
	// Monastery's 9 Monks or a Training Grounds' 6 Cavaliers ("priced none",
	// odds 0.01-0.11). N_duel r02 and r04: the field hero fought an Imp
	// Cache's 200 imps and lost 6197 and 6595 of its army. A prison is a
	// neutral hero with an army and is freed, not fought.
	if(!obj || !hero || obj->tempOwner.isValidPlayer() || obj->tempOwner == hero->tempOwner)
		return false;
	if(obj->ID == Obj::MONSTER || obj->ID == Obj::PRISON
		|| dynamic_cast<const CGHeroInstance *>(obj))
		return false;
	const auto * armed = dynamic_cast<const CArmedInstance *>(obj);
	return armed && armed->stacksCount() > 0;
}

const CArmedInstance * OmniAI::strongestHostileAt(const int3 & tile,
	const CGHeroInstance * hero) const
{
	// hero may be null: refreshTasks passes the field hero, which is gone
	// once it dies. R6f_duel r03 day 10, no heroes left: the null
	// dereference below killed the AI thread mid-turn and the game hung
	// until the harness deadline. Our own colour stands in for it.
	// Strongest hostile army on this tile or standing next to it. A guard
	// owns the tiles around itself, so the thing defending an object is
	// almost never on the object.
	const CArmedInstance * worst = nullptr;
	uint64_t worstStr = 0;
	if(!cb)
		return nullptr;
	for(int dx = -1; dx <= 1; ++dx)
		for(int dy = -1; dy <= 1; ++dy)
		{
			const int3 q(tile.x + dx, tile.y + dy, tile.z);
			if(!cb->isInTheMap(q))
				continue;
			const TerrainTile * t = cb->getTile(q, false);
			if(!t)
				continue;
			for(const auto & oid : t->visitableObjects)
			{
				const CGObjectInstance * obj = cb->getObj(oid, false);
				const auto * armed = obj
					? dynamic_cast<const CArmedInstance *>(obj) : nullptr;
				if(!armed)
					continue;
				const bool hostile = obj->ID == Obj::MONSTER ||
					(obj->tempOwner.isValidPlayer() && obj->tempOwner != (hero ? hero->tempOwner : playerID))
					|| (dx == 0 && dy == 0 && guardsItself(obj, hero));
				const uint64_t str = uint64_t(armed->getArmyStrength());
				if(hostile && str > worstStr)
				{
					worstStr = str;
					worst = armed;
				}
			}
		}
	return worst;
}

bool OmniAI::bordersFog(const int3 & tile) const
{
	// A monster standing in fog guards the tiles beside it unseen: the path
	// was planned against what we could see, the server stops the walk the
	// moment it enters the hidden stack's zone, and the fight is on at
	// whatever odds. N_duel r03 day 2: the hunt press ended at (11 12 0)
	// beside Nightmares hidden at (12 13 0), "exp -0.64", 10865 lost, game
	// over on day 3. Vision recedes the fog along every tile walked, so
	// stopping one step short and re-planning next round costs a round, not
	// a day.
	if(!cb)
		return false;
	for(int dx = -1; dx <= 1; ++dx)
		for(int dy = -1; dy <= 1; ++dy)
		{
			const int3 q(tile.x + dx, tile.y + dy, tile.z);
			if(cb->isInTheMap(q) && !cb->isVisible(q))
				return true;
		}
	return false;
}

const CArmedInstance * OmniAI::strongestHostileOnStep(const int3 & tile,
	const CGHeroInstance * hero) const
{
	// Only monsters guard the tiles around them (CGameState::guardingCreatures
	// checks Obj::MONSTER alone); an enemy hero or town is fought only by
	// stepping onto it. strongestHostileAt folds every neighbour in for every
	// owner, which is right for "is this end tile guarded" and wrong for a
	// step on the way: the walk fence cut DEFEND orders short beside the
	// enemy hero standing near the route home, and the field hero died where
	// it stood (A_duel r10 day 27, C_duel r09 day 3, K_duel r03 day 31: each
	// "FENCED ... hostile strength" named the killer, one tile off the path).
	const CArmedInstance * worst = nullptr;
	uint64_t worstStr = 0;
	if(!cb)
		return nullptr;
	for(int dx = -1; dx <= 1; ++dx)
		for(int dy = -1; dy <= 1; ++dy)
		{
			const int3 q(tile.x + dx, tile.y + dy, tile.z);
			if(!cb->isInTheMap(q))
				continue;
			const TerrainTile * t = cb->getTile(q, false);
			if(!t)
				continue;
			const bool onTile = dx == 0 && dy == 0;
			for(const auto & oid : t->visitableObjects)
			{
				const CGObjectInstance * obj = cb->getObj(oid, false);
				const auto * armed = obj
					? dynamic_cast<const CArmedInstance *>(obj) : nullptr;
				if(!armed)
					continue;
				const bool hostile = obj->ID == Obj::MONSTER
					|| (onTile && obj->tempOwner.isValidPlayer()
						&& obj->tempOwner != (hero ? hero->tempOwner : playerID))
					|| (onTile && guardsItself(obj, hero));
				const uint64_t str = uint64_t(armed->getArmyStrength());
				if(hostile && str > worstStr)
				{
					worstStr = str;
					worst = armed;
				}
			}
		}
	return worst;
}

uint64_t OmniAI::guardStrengthAt(const int3 & tile, const CGHeroInstance * hero) const
{
	const CArmedInstance * worst = strongestHostileAt(tile, hero);
	return worst ? uint64_t(worst->getArmyStrength()) : 0;
}

// The evaluator's verdict on `hero` attacking `target`. A town goes through
// the siege path so its walls count; an enemy hero is its own commander;
// anything else is a neutral guard stack with no hero behind it.
omniai::CombatVerdict OmniAI::combatVerdict(const CGHeroInstance * hero,
	const CArmedInstance * target) const
{
	if(const auto * town = dynamic_cast<const CGTownInstance *>(target))
		return estimateSiege(hero, town, false);   // the fight at hand
	const auto * defHero = dynamic_cast<const CGHeroInstance *>(target);
	omniai::CombatVerdict v = estimateFight(hero, target, defHero, 0);

	// Calibration cap. Measured on September 24th over 24 attacks that logged
	// both a prediction and real casualties: won fights kept about 0.1 less
	// army than predicted, and 8 fights predicted as wins (exp 0.52-1.00, odds
	// 0.66-1.23) were lost outright - the round-by-round model does not see
	// neutral stack splitting, blinding, retaliation or spells. The Lanchester
	// square law on the engine's own strength numbers (army strength times the
	// H3 hero factor, CGHeroInstance::getTotalStrength) tracked the same fights
	// better: sqrt(1 - 1/R^2) predicts 0.57 at R 1.22 (real 0.65), 0.75 at
	// 1.53 (0.85), 0.91 at 2.36 (0.77), and a loss at 0.98 where the model
	// said 0.87 and the fight cost 63% of the army. The verdict is the more
	// pessimistic of the two. Sieges keep the model alone: walls and towers
	// already make it the pessimistic one there.
	if(hero && target)
	{
		const double ours = double(hero->getTotalStrength());
		const double theirs = defHero ? double(defHero->getTotalStrength())
			: double(target->getArmyStrength());
		if(ours > 0 && theirs > 0)
		{
			const double r = ours / theirs;
			if(r <= 1.0)
			{
				if(v.win)
				{
					v.win = false;
					v.margin = std::sqrt(std::max(0.0, 1.0 - r * r));
				}
			}
			else if(v.win)
				v.margin = std::min(v.margin, std::sqrt(1.0 - 1.0 / (r * r)));
		}
	}

	// Loss calibration, after the cap above. Measured on September 24th over
	// every attack in the R6i-R6q duel-v2 and R6n_fog logs that logged a
	// prediction and real casualties (`.tmp/omni/evalcal.py`, least squares
	// on realized = 1 - K x predicted loss): neutral stacks cost 1.6 times
	// the predicted share of our army (167 won fights, median ratio 1.73);
	// enemy heroes cost 2.8 times (27 won, median 2.59, and 3 more predicted
	// wins were lost outright). The R6v r01 field hero took Ice Elementals
	// at a predicted 0.89 and kept 0.74. Every voluntary-fight bar (grind,
	// commit margin, hunt, prize price) reads this margin, so the bars keep
	// their meaning and the numbers they read stop flattering us. A small
	// floor keeps a costly win distinguishable from a loss: the multiplier
	// overshoots on the few fights predicted at half the army or worse.
	if(v.win && hero && target)
	{
		const double k = defHero ? 2.8 : 1.6;
		v.margin = std::max(0.05, 1.0 - k * (1.0 - v.margin));
	}
	return v;
}

// Whether `hero` is expected to beat `target` with at least minSurvivor of
// its army left. The floor is what turns a bare win/lose answer into a
// fight worth taking: a seal we only just break still opens the pocket, so
// the bar is low; a pickup we barely survive is not worth the army.
double OmniAI::commitMargin(const CGHeroInstance * hero, double base) const
{
	// The bar rises with the army being risked: the model read that is mostly
	// right at 20k is a coin flip it cannot afford at 200k, so a bigger field
	// force needs a bigger predicted cushion before a commit.
	//
	// Voluntary fights start at 0.60, raised from 0.25 on September 24th:
	// 0.25 let a walk or a pickup take a fight that cost 75% of the army, and
	// F_duel r01's field hero bled from 15k to 4.8k in three such "won"
	// fights against shooter stacks (one predicted at 0.41, 55% lost) before
	// Nullkiller's main arrived. With the square-law cap in combatVerdict a
	// 0.60 bar means roughly 1.25x strength odds or better; Nullkiller's own
	// fights run at 3-28x. Sieges pass 0.25 (the old base): taking a town is
	// the decisive fight and its price already includes walls and towers.
	const double army = hero ? double(hero->getArmyStrength()) : 0.0;
	double margin = std::min(0.85, base + army / 250000.0);
	if(!cb || !hero)
		return margin;

	// Fleet-in-being term. A voluntary fight that leaves the stack weaker
	// than a hostile hero already inside interception range is a loss
	// postponed one day: s7-72x72 took a 0.58-odds pickup while a 26k
	// stack stood adjacent. Inside ~40 tiles (two long marches) a
	// stronger enemy ramps the required margin toward near-certainty.
	// The comparison runs in fight value: AI value over-credits a mixed
	// low-tier swarm's utility premiums and would read parity where a
	// consolidated stack is actually far stronger.
	const int3 hp = hero->visitablePos();
	const double oursFv = double(armyFightValue(hero));
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
		if(!e || !e->tempOwner.isValidPlayer() || e->tempOwner == playerID)
			continue;
		const int3 ep = e->visitablePos();
		if(ep.z != hp.z)
			continue;
		const int dist = std::max(std::abs(ep.x - hp.x), std::abs(ep.y - hp.y));
		if(dist <= 40 && double(armyFightValue(e)) > oursFv)
			margin = std::min(0.95, margin + 0.40 * (40.0 - dist) / 40.0);
	}

	// The same for a stronger enemy hero out of sight since yesterday or
	// the day before that could be at one of our towns by now: out of the
	// fog it is still walking. Three early losses on September 24th had the
	// same shape (R6v3 r01, R6y r04, R6z r05): a voluntary fight cost the
	// field hero 15-45% of its army while Nullkiller's first hero, seen
	// beside our unwalled town a day earlier, was just out of sight, and the
	// town fell to it within two days at even odds. Strengths projected as
	// assessPosture does; reach is the sighting's own daily allowance.
	{
		const int today = cb->getDate(Date::DAY);
		const double oursStr = double(hero->getArmyStrength());
		for(const auto & kv : foeSeen_)
		{
			const FoeSighting & f = kv.second;
			const int ago = today - f.day;
			if(ago < 1 || ago > 2)
				continue;   // today's sightings are the visible loop above
			if(f.str * std::pow(1.045, double(ago)) <= oursStr)
				continue;
			for(const CGTownInstance * town : cb->getTownsInfo())
			{
				if(!town || town->tempOwner != playerID || town->visitablePos().z != f.pos.z)
					continue;
				const int3 tp = town->visitablePos();
				const int d = std::max(std::abs(tp.x - f.pos.x), std::abs(tp.y - f.pos.y));
				if(d <= std::max(1, f.reach) * (ago + 1))
					margin = 0.95;
			}
		}
	}

	// Saturday/Sunday is the last turn before the weekly growth lands in
	// every town. A marginal voluntary fight taken today is often free
	// after the wave, so wait for it rather than bleed for a pickup.
	const int dow = cb->getDate(Date::DAY_OF_WEEK);
	if(dow >= 6)
		margin = std::min(0.95, margin + 0.10);

	// Distance from resupply. The research framing is specifically about
	// discretionary fights (a chest, a relic, a mine) rather than the march
	// itself: a loss taken next to a town is replaced from the dwelling by
	// next week, the same loss six marches out is a standing weakness for as
	// long as it takes to walk home - real exposure if a stronger enemy is
	// loose elsewhere on the map. Deliberately calibrated lighter than the
	// fleet-in-being term above (0.12 max here against that term's 0.40):
	// this project has already spent real sessions re-learning that a field
	// hero which grows more cautious the farther it presses tends to
	// reintroduce sealed-pocket camping (pocketHold, sealedPressHold and the
	// walk() fence all exist to stop exactly that), so this is sized to
	// nudge, not to override the guards already built for the opposite
	// failure. Straight-line tiles to the nearest owned town, the same unit
	// the fleet-in-being term above already uses - not a pathfinding query,
	// since this runs from walk()'s per-step fence check, far too hot a
	// path for a fresh route lookup on every call. No owned town at all
	// (turn 1) leaves this a no-op rather than a division by nothing.
	constexpr double SUPPLY_MARGIN_MAX = 0.12;
	constexpr double SUPPLY_TILES_FOR_MAX = 60.0;   // ~3 long marches
	long nearestTownDist = -1;
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;
		const int3 tp = town->visitablePos();
		if(tp.z != hp.z)
			continue;
		const long d = std::max(std::abs(tp.x - hp.x), std::abs(tp.y - hp.y));
		if(nearestTownDist < 0 || d < nearestTownDist)
			nearestTownDist = d;
	}
	if(nearestTownDist > 0)
		margin = std::min(0.95, margin + SUPPLY_MARGIN_MAX
			* std::min(1.0, double(nearestTownDist) / SUPPLY_TILES_FOR_MAX));

	return margin;
}

bool OmniAI::expectedToWin(const CGHeroInstance * hero,
	const CArmedInstance * target, double minSurvivor) const
{
	if(!target)
		return true;
	// A hero fight is not a neutral-stack fight: the defender brings a
	// spellbook, secondary skills and artifacts the model prices only in
	// aggregate, and one Slow/Implosion turn swings far more than the
	// flat magicDps term carries. m009 logged a fight we attacked at 4.32
	// in our favour with expectation 1.00 and lost outright - the model
	// never saw the mage. Demand a wider cushion against heroes.
	if(dynamic_cast<const CGHeroInstance *>(target))
		minSurvivor = std::min(0.95, minSurvivor + 0.15);
	const omniai::CombatVerdict v = combatVerdict(hero, target);
	return v.win && v.margin >= minSurvivor;
}

uint64_t OmniAI::armyFightValue(const CArmedInstance * armed) const
{
	uint64_t total = 0;
	if(!armed)
		return total;
	for(const auto & entry : armed->Slots())
	{
		const auto * cre = (entry.second && entry.second->getType())
			? entry.second->getCreature() : nullptr;
		if(cre)
			total += uint64_t(entry.second->getCount())
				* uint64_t(std::max(0, cre->getFightValue()));
	}
	return total;
}

const CGHeroInstance * OmniAI::strongestThreatNear(const CGTownInstance * town) const
{
	if(!cb || !town)
		return nullptr;
	constexpr int THREAT_RADIUS = 12;
	const int3 tp = town->visitablePos();
	const CGHeroInstance * strongest = nullptr;
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
		if(!e || !e->tempOwner.isValidPlayer() || e->tempOwner == playerID)
			continue;
		const int3 ep = e->visitablePos();
		if(ep.z != tp.z)
			continue;
		if(std::max(std::abs(ep.x - tp.x), std::abs(ep.y - tp.y)) > THREAT_RADIUS)
			continue;
		if(!strongest || e->getArmyStrength() > strongest->getArmyStrength())
			strongest = e;
	}
	return strongest;
}

// Teleport a hero straight onto a town. Town Portal is the one spell that
// makes a march instant: a defender reaches a threatened town the same
// turn instead of arriving days late, and a courier's delivery leg stops
// costing a week. Nullkiller's AdventureSpellCast goal uses it; this is
// our copy of the cast - pos is the destination town's visitable tile.
bool OmniAI::castTownPortalTo(const CGHeroInstance * hero,
	const CGTownInstance * town)
{
	if(!cb || !hero || !town || !hero->hasSpellbook())
		return false;
	if(!hero->getSpellsInSpellbook().count(SpellID(SpellID::TOWN_PORTAL)))
		return false;
	const spells::Spell * spell = LIBRARY->spells()->getById(SpellID(SpellID::TOWN_PORTAL));
	if(!spell || !hero->canCastThisSpell(spell))
		return false;
	if(hero->mana < hero->getSpellCost(spell))
		return false;
	cb->castSpell(hero, SpellID(SpellID::TOWN_PORTAL), town->visitablePos());
	decisionLog_->line("   TOWN PORTAL " + std::string(hero->getNameTranslated())
		+ " -> " + town->getNameTranslated());
	return true;
}

bool OmniAI::defenseHolds(const CGHeroInstance * ourHero,
	const CGTownInstance * town, const CGHeroInstance * enemyHero,
	const CGHeroInstance * excluded) const
{
	if(!enemyHero)
		return true;
	return !siegeOnUs(ourHero, town, enemyHero, excluded).win;
}

omniai::CombatVerdict OmniAI::siegeOnUs(const CGHeroInstance * ourHero,
	const CGTownInstance * town, const CGHeroInstance * enemyHero,
	const CGHeroInstance * excluded) const
{
	if(!enemyHero)
		return omniai::CombatVerdict{};
	using CE = omniai::CombatEvaluator;
	// The enemy sieges: they are the attacker, our garrison + our hero + the
	// walls are the defender. We hold if their attack is expected to fail.
	CE::Side atk = CE::profile(enemyHero, enemyHero, 0);
	CE::Side def;
	const int fort = town ? int(town->fortLevel()) : 0;
	// The excluded hero is left out wherever it stands. A garrison hero's
	// army IS the garrison (the town's own slots merge into it), so leaving
	// it out leaves the walls: R6f_duel r01 day 18 asked "does Gateway hold
	// without the field hero" while the field hero sat in the garrison slot,
	// got yes from its own army, walked out, and the town fell to Traugutt.
	const CGHeroInstance * gh = town ? town->getGarrisonHero() : nullptr;
	if(gh && gh == excluded)
		gh = nullptr;
	if(ourHero == excluded)
		ourHero = nullptr;
	CE::absorb(def, town, gh ? gh : ourHero, fort);
	if(gh)
		CE::absorb(def, gh, gh, fort);
	if(ourHero && ourHero != gh)
		CE::absorb(def, ourHero, ourHero, fort);
	// A defender already routed to the town counts too: the threat on a
	// frontier town is days out, and the Hold task's runner arrives on a
	// comparable clock. Counting only the seated force is what made a town
	// read as lost while a defender was a day away.
	if(town)
	{
		for(const RegTask & t : regTasks_)
		{
			if(t.kind != RegTask::Kind::Hold || t.objId != town->id.getNum()
				|| t.heroId < 0)
				continue;
			const CGHeroInstance * inbound = cb->getHero(ObjectInstanceID(t.heroId));
			if(inbound && inbound != ourHero && inbound != gh && inbound != excluded)
				CE::absorb(def, inbound, inbound, fort);
		}
	}
	// Same tower math estimateSiege uses when WE are the besieger - a
	// town's towers fire for whichever side owns it, so evaluating our own
	// defense without them was the asymmetric half of the gap: this read
	// our walls as weaker than they actually are, which is the wrong
	// direction to be wrong in right before deciding whether to evacuate.
	def.towerDps = townTowerDps(town);
	// A visitor in front of a garrison hero fights outside with none of
	// this (isBattleOutsideTown); consolidateTownDefence keeps that layout
	// out of threatened towns, so the walls are counted here.
	def.fortLevel = fort;
	return CE::estimate(atk, def);
}

bool OmniAI::evacuateTown(const CGHeroInstance * hero,
	const int3 & threatPos, CPathsInfo * paths, bool takeGarrison)
{
	if(!cb || !hero || !paths)
		return false;
	const int3 hp = hero->visitablePos();
	const int3 sizes = cb->getMapSize();

	// The army leaves with the hero or it does not leave at all: troops
	// standing in a doomed garrison die with the walls they could not
	// hold, which is the 0.00-defense donation the evacuate/sally matrix
	// calls out. Pull them onto the departing hero first.
	for(const CGTownInstance * here : cb->getTownsInfo())
		if(takeGarrison && here && here->tempOwner == playerID && here->visitablePos() == hp)
		{
			collectGarrison(here, hero);
			break;
		}

	// Prefer another town we still own - the hero lands where it can defend
	// or resupply next - else just put ground between us and the threat.
	const CGTownInstance * safeTown = nullptr;
	int safeDist = -1;
	for(const CGTownInstance * t : cb->getTownsInfo())
	{
		if(!t || t->tempOwner != playerID || t->visitablePos() == hp)
			continue;
		CGPath probe;
		if(!paths->getPath(probe, t->visitablePos()))
			continue;
		const int d = std::abs(t->visitablePos().x - threatPos.x)
			+ std::abs(t->visitablePos().y - threatPos.y);
		if(d > safeDist)
		{
			safeDist = d;
			safeTown = t;
		}
	}
	if(safeTown)
	{
		std::vector<int3> steps = stepsToward(paths, hero, safeTown->visitablePos());
		if(!steps.empty())
		{
			decisionLog_->line("   EVACUATE to " + safeTown->getNameTranslated()
				+ " - cannot hold here, taking the army");
			return walk(hero, steps, false, true);
		}
	}
	// No friendly town in reach: retire to the reachable tile that opens the
	// most distance from the attacker, never into a guard.
	int3 bestTile;
	long bestScore = -1;
	for(int y = 0; y < sizes.y; ++y)
		for(int x = 0; x < sizes.x; ++x)
		{
			const int3 p(x, y, hp.z);
			if(p == hp)
				continue;
			const CGPathNode * n = paths->getNode(p, EPathfindingLayer::LAND);
			if(!n || !n->reachable()
				|| n->accessible == EPathAccessibility::GUARDED)
				continue;
			const long d = std::abs(x - threatPos.x) + std::abs(y - threatPos.y);
			if(d > bestScore)
			{
				bestScore = d;
				bestTile = p;
			}
		}
	if(bestScore < 0)
		return false;
	std::vector<int3> steps = stepsToward(paths, hero, bestTile);
	if(steps.empty())
		return false;
	decisionLog_->line("   EVACUATE - retreating from a town we cannot hold");
	return walk(hero, steps, false, true);
}

std::string OmniAI::armyTiers(const CCreatureSet * army) const
{
	if(!army)
		return "-";
	int t[8] = {};
	for(const auto & e : army->Slots())
	{
		if(!e.second || !e.second->getCreature())
			continue;
		const int l = int(e.second->getCreature()->getLevel());
		if(l >= 1 && l <= 7)
			t[l] += int(e.second->getCount());
	}
	std::ostringstream o;
	o << t[1];
	for(int i = 2; i <= 7; ++i)
		o << "/" << t[i];
	return o.str();
}

uint64_t OmniAI::recruitsOnOffer(const CGObjectInstance * obj) const
{
	// The army a dwelling would actually hand over, which is what taking it
	// is for. A flat worth per object type cannot tell a Halberdier hut from
	// a Behemoth lair.
	const auto * dw = dynamic_cast<const CGDwelling *>(obj);
	if(!dw)
		return 0;
	uint64_t total = 0;
	for(const auto & level : dw->creatures)
	{
		if(level.first == 0 || level.second.empty())
			continue;
		const auto * cre = level.second.back().toCreature();
		if(cre)
			total += uint64_t(level.first) * uint64_t(std::max(0, cre->getAIValue()));
	}
	return total;
}

double OmniAI::fightValue(const CGObjectInstance * obj, const CGHeroInstance * hero) const
{
	// Roughly what winning costs in army. A fight near your own strength is
	// close to mutual destruction and a small one is nearly free, so a
	// fraction of the guard is the simplest honest model, and it is in the
	// same units as everything else here.
	constexpr double COST_OF_WINNING = 0.6;
	// How far ahead the recruits are worth counting.
	constexpr double WEEKS_AHEAD = 2.0;
	// Scaled into the range an intrinsic worth lives in, so this argues with
	// the rest of the score rather than shouting over it.
	constexpr double SCALE = 300.0;
	constexpr double LIMIT = 600.0;

	if(!obj)
		return 0.0;

	const uint64_t guard = guardStrengthAt(obj->visitablePos(), hero);
	const uint64_t buys = recruitsOnOffer(obj);
	if(!guard && !buys)
		return 0.0;

	const double ours = double(std::max<uint64_t>(1, hero->getArmyStrength()));

	// A fight we lose buys nothing. The evaluator already refuses these, but
	// only when the object IS the monster: a guarded dwelling is a dwelling,
	// so its veto never fires for one. Without this the recruits term happily
	// argues for walking into a guard half again our strength, which is the
	// opposite of what it was written for.
	if(double(guard) >= ours * omniai::LearningStore::instance().dangerThreshold())
	{
		decisionLog_->detail(std::string("skip ") + obj->getObjectName()
			+ ": guarded by " + std::to_string(guard)
			+ " against our " + std::to_string(uint64_t(ours)));
		return -1e7;
	}

	// What clearing this guard would open up, not just what the object it
	// stands on hands over - a guard is frequently the one thing sealing a
	// whole pocket, and beating one used to score exactly like beating an
	// equally strong guard over empty ground either way. Zero for an
	// unguarded object: nothing was sealed, so nothing was opened by
	// taking it.
	const double pocket = guard ? pocketValueBehind(obj->visitablePos(), hero) : 0.0;
	const double net = (double(buys) * WEEKS_AHEAD + pocket - double(guard) * COST_OF_WINNING) / ours;
	return std::clamp(net * SCALE, -LIMIT, LIMIT);
}

double OmniAI::pocketValueBehind(const int3 & guardPos, const CGHeroInstance * hero) const
{
	// Bounded proxy for "what does clearing this guard open up": known
	// objects within POCKET_RADIUS that the pathfinder cannot reach this
	// turn. Not a real what-if-this-guard-were-gone reachability recompute -
	// that would mean a second pathfinding pass per candidate. Ground near a
	// guard that is unreachable by every OTHER known route is very likely
	// sealed by exactly this guard, which is why unreachable is the filter
	// rather than distance alone: a far pile behind its OWN separate guard
	// must not get credited to this one. Priced at half nominal and folded
	// into fightValue's existing clamp, so a misattributed pocket costs at
	// most a fraction of one candidate's score, not a runaway bonus.
	constexpr int POCKET_RADIUS = 8;
	constexpr double POCKET_SHARE = 0.5;

	if(!cb || !hero)
		return 0.0;
	auto paths = pathCache_ ? pathCache_->getPathsInfo(hero) : nullptr;
	if(!paths)
		return 0.0;
	CPathsInfo * mutablePaths = const_cast<CPathsInfo *>(paths.get());

	double sum = 0.0;
	for(const auto & kv : worldObjs_)
	{
		const WorldObj & w = kv.second;
		if(w.reward <= 0 || w.owner == playerID.getNum() || w.pos.z != guardPos.z)
			continue;
		const int dist = std::max(std::abs(w.pos.x - guardPos.x),
			std::abs(w.pos.y - guardPos.y));
		if(dist == 0 || dist > POCKET_RADIUS)
			continue;
		const CGPathNode * n = mutablePaths->getNode(w.pos, EPathfindingLayer::LAND);
		if(n && n->reachable())
			continue;   // reachable some other way already - not this guard's pocket
		sum += w.reward;
	}
	return sum * POCKET_SHARE;
}

bool OmniAI::worthTheGuard(const int3 & tile, const CGHeroInstance * hero) const
{
	// A guarded tile commits the hero to a fight on the guard's terms next
	// turn. The node does not say which monster owns the zone of control,
	// but a guard is by definition adjacent, so the eight neighbours find it.
	//
	// Tighter than the desperation bound the armed fallback uses, and on
	// purpose: this is exploration. If the fight is not clearly ours there is
	// no reason to walk into it for the sake of some fog, and the armed
	// fallback is still free to take it later on its own looser terms.
	constexpr double CLEARLY_OURS = 0.8;

	const double heroStrength = double(hero->getArmyStrength());
	for(int dx = -1; dx <= 1; ++dx)
		for(int dy = -1; dy <= 1; ++dy)
		{
			if(!dx && !dy)
				continue;
			const int3 q(tile.x + dx, tile.y + dy, tile.z);
			if(!cb->isInTheMap(q))
				continue;
			const TerrainTile * t = cb->getTile(q, false);
			if(!t)
				continue;
			for(const auto & oid : t->visitableObjects)
			{
				const CGObjectInstance * obj = cb->getObj(oid, false);
				const auto * armed = obj
					? dynamic_cast<const CArmedInstance *>(obj) : nullptr;
				if(!armed)
					continue;
				const bool hostile = obj->ID == Obj::MONSTER ||
					(obj->tempOwner.isValidPlayer() && obj->tempOwner != (hero ? hero->tempOwner : playerID));
				if(!hostile)
					continue;
				if(double(armed->getArmyStrength()) > heroStrength * CLEARLY_OURS)
					return false;
			}
		}
	return true;
}

bool OmniAI::exploreFrontier(const CGHeroInstance * hero, CPathsInfo * paths)
{
	const int3 hp = hero->visitablePos();
	const int3 sizes = cb->getMapSize();

	// Fog to lift and a guard to kill are both found in one walk of the map
	// now. They used to be three separate walks, each reachable only when
	// the one before it came up empty, and that is why the armed fallback
	// never ran: a pocket with two fogged tiles left in it always produced
	// a frontier tile, so the search never got as far as looking for a way
	// to fight out.
	constexpr int SIGHT = 4;

	// A frontier tile worth less than this is not worth a day's movement.
	// The ground around the hero is already known and what seals the pocket
	// is a guard, not fog. Measured: the stall pocket offered frontier tiles
	// worth two revealed tiles apiece, enough to keep an unconditional
	// explore shuffling in place forever.
	constexpr int WORTH_A_DAY = 6;

	// The evaluator's suicide verdict has proven conservative - a Harpy pack
	// it rated certain death was beaten at even odds - so the desperation
	// bound is looser than the scoring one. There is still a bound, because
	// a dead hero opens nothing.
	constexpr double DESPERATION = 1.6;

	// Direction, not just quantity. Commitment alone produced a tidy crawl:
	// of 45 held targets in one measured game, 18 were dropped for "nothing
	// left to uncover there", meaning they had been aimed at the near lip of
	// the unknown and the walk itself uncovered them. That is what happens
	// when reveal count is the only term, because reachability is bounded by
	// fog and the reachable set only ever touches the edge of it.
	//
	// Weighting by progress toward the bulk of what we have never seen turns
	// the crawl into a push. The centroid costs one map scan, is computed
	// from our own fog and nothing else, and on a two-player map the mass of
	// the unknown is where the enemy lives.
	constexpr double DIRECTION_WEIGHT = 1.0;
	int3 unseen;
	const bool haveDirection = unseenCentroid(hp.z, unseen);
	const double heroToUnseen = haveDirection
		? std::sqrt(double((hp.x - unseen.x) * (hp.x - unseen.x)
			+ (hp.y - unseen.y) * (hp.y - unseen.y)))
		: 0.0;

	// Known hostile heroes, gathered once: frontier tiles near one of these
	// get discounted below. Measured cause of a fast duel-v2 loss (day 7,
	// s30 of the 9-fix spread): the scout target was picked purely on fog
	// revealed and direction, with no term at all for a hostile hero already
	// on the board nearby - Cuthbert (8017 army, well ahead as of day 4)
	// scouted straight through the area an enemy hero was building up in and
	// was gone by day 8. Only heroes actually stronger than us count as a
	// reason to flinch, same test commitMargin's fleet-in-being term
	// already uses, so a weak scout hero we can still fight past does not
	// avoid ground for no reason.
	struct FrontierDanger { int3 pos; double strength; };
	std::vector<FrontierDanger> dangers;
	const double ourStrength = double(hero->getArmyStrength());
	for(const CGObjectInstance * obj : cb->getAllVisitableObjs())
	{
		const auto * e = dynamic_cast<const CGHeroInstance *>(obj);
		if(e && e->tempOwner.isValidPlayer() && e->tempOwner != playerID
			&& double(e->getArmyStrength()) > ourStrength)
			dangers.push_back({e->visitablePos(), double(e->getArmyStrength())});
	}
	constexpr double DANGER_RADIUS = 18.0;   // a few turns of hero movement
	constexpr double DANGER_WEIGHT = 0.7;    // up to 70% off at zero distance,
	                                          // never fully zeroed - nowhere to
	                                          // explore is worse than somewhere risky
	auto dangerDiscount = [&](const int3 & p) -> double
	{
		double worst = 0.0;
		for(const auto & d : dangers)
		{
			if(d.pos.z != p.z)
				continue;
			const double dist = std::sqrt(double(
				(p.x - d.pos.x) * (p.x - d.pos.x) + (p.y - d.pos.y) * (p.y - d.pos.y)));
			if(dist < DANGER_RADIUS)
				worst = std::max(worst, (DANGER_RADIUS - dist) / DANGER_RADIUS);
		}
		return 1.0 - DANGER_WEIGHT * worst;
	};

	const CGPathNode * openBest = nullptr;
	int openReveal = 0;
	double openScore = -1.0;
	float openCost = std::numeric_limits<float>::max();

	const CGPathNode * guardedBest = nullptr;
	int guardedReveal = 0;
	double guardedScore = -1.0;
	float guardedCost = std::numeric_limits<float>::max();

	// Nearest beatable hostile, not weakest. See the note at the choice
	// below for the 6253 orders and 309 battles that changed this.
	const double beatableLimit = double(hero->getArmyStrength()) * DESPERATION;
	// A voluntary fight has to leave an army behind: breaking the seal at
	// 42% survivors (36x36_s23, day 3) put the enemy's intact stack through
	// the breach we paid for. The same guard is cheaper next week after
	// recruits, so a fight that would cost half the army waits.
	constexpr double BREAKOUT_SURVIVOR = 0.5;
	const CGPathNode * fight = nullptr;
	double fightStrength = 0.0;
	float fightCost = std::numeric_limits<float>::max();

	for(int z = 0; z < sizes.z; ++z)
		for(int x = 0; x < sizes.x; ++x)
			for(int y = 0; y < sizes.y; ++y)
			{
				const int3 p(x, y, z);
				const CGPathNode * n = paths->getNode(p, EPathfindingLayer::LAND);
				if(!n || !n->reachable() || frontierTriedThisTurn_.count(p))
					continue;
				if(!leashAllows(hero, p))
					continue;   // walk() would refuse it; do not spend the pick

				const bool open = n->accessible == EPathAccessibility::ACCESSIBLE;
				const bool guarded = n->accessible == EPathAccessibility::GUARDED;
				if(guarded && !worthTheGuard(p, hero))
					continue;
				if(open || guarded)
				{
					// How much unseen ground standing here would open up.
					// Guarded tiles count: stepping into a zone of control is
					// legal, it just ends the turn there and commits the hero
					// to the fight next turn.
					int reveal = 0;
					for(int dx = -SIGHT; dx <= SIGHT; ++dx)
						for(int dy = -SIGHT; dy <= SIGHT; ++dy)
						{
							const int3 q(x + dx, y + dy, z);
							if(cb->isInTheMap(q) && !cb->isVisible(q))
								++reveal;
						}
					if(reveal == 0)
						continue;
					// Ground gained on the unknown, as a fraction of how far
					// the hero stands from it now. 1 means standing on it,
					// 0 means no closer, negative means walking away.
					double gain = 0.0;
					if(haveDirection && heroToUnseen > 1.0)
					{
						const double d = std::sqrt(double(
							(x - unseen.x) * (x - unseen.x)
							+ (y - unseen.y) * (y - unseen.y)));
						gain = (heroToUnseen - d) / heroToUnseen;
						gain = std::max(-1.0, std::min(1.0, gain));
					}
					const double score =
						double(reveal) * (1.0 + DIRECTION_WEIGHT * gain)
						* dangerDiscount(p);

					const CGPathNode *& bestNode = open ? openBest : guardedBest;
					int & bestReveal = open ? openReveal : guardedReveal;
					double & bestScore = open ? openScore : guardedScore;
					float & bestCost = open ? openCost : guardedCost;
					if(score > bestScore ||
						(score == bestScore && n->getCost() < bestCost))
					{
						bestNode = n;
						bestReveal = reveal;
						bestScore = score;
						bestCost = n->getCost();
					}
					continue;
				}

				// A monster stands on its own tile, which reads BLOCKVIS;
				// enemy heroes, town garrisons and map garrisons read
				// BLOCKVIS or VISITABLE. Weakest first.
				if(n->accessible != EPathAccessibility::VISITABLE &&
					n->accessible != EPathAccessibility::BLOCKVIS)
					continue;
				const TerrainTile * tile = cb->getTile(p, false);
				if(!tile)
					continue;
				for(const auto & oid : tile->visitableObjects)
				{
					const CGObjectInstance * obj = cb->getObj(oid, false);
					const auto * armed = obj
						? dynamic_cast<const CArmedInstance *>(obj) : nullptr;
					if(!armed)
						continue;
					const bool hostile = obj->ID == Obj::MONSTER ||
						(obj->tempOwner.isValidPlayer() && obj->tempOwner != hero->tempOwner);
					if(!hostile)
						continue;

					const double str = double(armed->getArmyStrength());
					if(str > beatableLimit)
						continue; // hopeless; a dead hero opens nothing
					if(!expectedToWin(hero, armed, BREAKOUT_SURVIVOR))
						continue; // wins but guts the army - wait for recruits
					if(!fight || n->getCost() < fightCost)
					{
						fight = n;
						fightCost = n->getCost();
						fightStrength = str;
					}
				}
			}

	// Open ground beats guarded ground at equal promise, because stepping
	// into a zone of control hands the guard the initiative.
	const CGPathNode * frontier = openBest ? openBest : guardedBest;
	const int frontierReveal = openBest ? openReveal : guardedReveal;
	// Real exploration first, then the fight that unseals the pocket, then
	// exploration at any size at all rather than standing still.
	//
	// The fight is chosen by distance, not by weakness. Weakest was right
	// while this was a last resort out of a sealed pocket, where the only
	// question was whether the fight could be survived. It runs on every
	// refused lap now, and then the question is which fight can be reached:
	// over 4925 game days the weakest rule produced 6253 attack orders and
	// 309 battles, because the weakest hostile on a mirror map is the enemy
	// hero, and it walks away every turn.
	const CGPathNode * target = nullptr;
	bool attacking = false;
	// Only a tile that clears WORTH_A_DAY on its own is worth holding across
	// turns. The last branch below is "anything rather than stand still",
	// and committing to one of those put the selection and the commitment in
	// direct contradiction: scoutMarch drops a target the moment it uncovers
	// less than WORTH_A_DAY, so every such target was dropped on the next
	// call. Measured in one run: 590 drops for "nothing left to uncover
	// there" against 29 arrivals.
	bool worthCommitting = false;
	if(frontier && frontierReveal >= WORTH_A_DAY)
	{
		target = frontier;
		worthCommitting = true;
	}
	else if(fight)
	{
		target = fight;
		attacking = true;
	}
	else if(frontier)
		target = frontier;

	if(!target || target->coord == hp)
		return false;

	CGPath path;
	if(!paths->getPath(path, target->coord))
		return false;

	std::vector<int3> steps;
	steps.reserve(path.nodes.size() - 1);
	for(auto it = path.nodes.rbegin(); it != path.nodes.rend(); ++it)
	{
		if(it->coord == hp)
			continue;
		if(it->isTeleportAction() || it->turns != 0)
			break;
		steps.push_back(hero->convertFromVisitablePos(it->coord));
	}
	if(steps.empty())
		return false;

	std::ostringstream o;
	if(attacking)
		o << "   ATTACK to break out at " << target->coord.toString()
		  << " (guard strength " << fightStrength << " against our "
		  << hero->getArmyStrength() << "), " << steps.size() << " steps";
	else
		o << "   EXPLORE toward fog edge at " << target->coord.toString()
		  << " (reveals ~" << frontierReveal << " tiles), " << steps.size() << " steps";
	decisionLog_->line(o.str());
	frontierTriedThisTurn_.insert(target->coord);
	// Commit to real exploration, never to a break-out attack (that is this
	// turn's business and resolves itself) and never to a consolation tile.
	if(worthCommitting)
	{
		scoutTarget_ = target->coord;
		scoutTargetSet_ = true;
		scoutTurnsLeft_ = SCOUT_BUDGET;
	}
	// Break-out attacks are picked at the desperation bound, looser than
	// the fence's commitMargin - the last step is the priced fight.
	return walk(hero, steps, attacking);
}

void OmniAI::dumpPathGrid(const CGHeroInstance * hero, CPathsInfo * paths)
{
	const int3 hp = hero->visitablePos();
	decisionLog_->line("   path grid (~ fog, # wall, . open, v visit, g guarded, "
		"b blockvis, f flyable; CAPS = path reaches, @ = hero):");
	for(int y = hp.y - 9; y <= hp.y + 9; ++y)
	{
		std::string row;
		for(int x = hp.x - 9; x <= hp.x + 9; ++x)
		{
			const int3 p(x, y, hp.z);
			char c = ' ';
			if(cb->isInTheMap(p))
			{
				if(x == hp.x && y == hp.y)
					c = '@';
				else if(!cb->isVisible(p))
					c = '~';
				else if(const CGPathNode * n = paths->getNode(p, EPathfindingLayer::LAND))
				{
					switch(n->accessible)
					{
						case EPathAccessibility::ACCESSIBLE: c = '.'; break;
						case EPathAccessibility::VISITABLE:  c = 'v'; break;
						case EPathAccessibility::GUARDED:    c = 'g'; break;
						case EPathAccessibility::BLOCKVIS:   c = 'b'; break;
						case EPathAccessibility::FLYABLE:    c = 'f'; break;
						case EPathAccessibility::BLOCKED:    c = '#'; break;
						default: c = '?';
					}
					if(n->reachable())
						c = char(std::toupper(static_cast<unsigned char>(c)));
				}
			}
			row += c;
		}
		decisionLog_->line("   |" + row + "|");
	}
}

void OmniAI::tileRevealed(const FowTilesType &pos)
{
	if(!cb)
		return;
	if(pathCache_)
		pathCache_->invalidatePaths();
	std::lock_guard lock(seenMutex_);
	for(const int3 & p : pos)
		for(const CGObjectInstance * obj : cb->getVisitableObjs(p, false))
			if(obj)
				seenObjs_.insert(obj->id.getNum());
}

void OmniAI::newObject(const CGObjectInstance * obj)
{
	if(!obj)
		return;
	if(pathCache_)
		pathCache_->invalidatePaths();
	std::lock_guard lock(seenMutex_);
	seenObjs_.insert(obj->id.getNum());
}

void OmniAI::heroMoved(const TryMoveHero & details, bool verbose)
{
	// Cached paths anchor at the hero's old position (the cache only keys on
	// the bonus-tree version, not coordinates), so drop them on every move.
	if(pathCache_)
		pathCache_->invalidatePaths();

	// The server moved our hero, so the oldest Move pack still waiting on
	// its PackageApplied realized - receipts go missing whenever a visit
	// or a battle interrupts the walk, which is where the "Move timed out"
	// warnings come from. Genuine refusals produce no heroMoved and still
	// sweep at 30s. Only our moves count: an enemy's visible move would
	// clear a pending of ours that never happened.
	if(actionQueue_ && details.result != TryMoveHero::FAILED)
	{
		const CGHeroInstance * h = cb ? cb->getHero(details.id) : nullptr;
		if(h && h->tempOwner == playerID)
			actionQueue_->resolveOldest(omniai::ActionKind::Move);
	}
}

void OmniAI::objectRemoved(const CGObjectInstance *obj, const PlayerColor & initiator)
{
	if(!obj)
		return;
	if(pathCache_)
		pathCache_->invalidatePaths();
	registry_->retire(obj->id.getNum());
	std::lock_guard lock(seenMutex_);
	seenObjs_.erase(obj->id.getNum());
	removedIds_.push_back(obj->id.getNum());
}

void OmniAI::requestSent(const CPackForServer *pack, int requestID)
{
	if(dispatcher_)
		dispatcher_->onRequestSent(pack, requestID);
}

void OmniAI::requestRealized(PackageApplied *pa)
{
	if(dispatcher_ && pa)
		dispatcher_->onRequestRealized(*pa);
	// Any receipt can trail a query reply that just unblocked the pipeline;
	// if an endTurn was dropped behind a query, this is the retry point.
	maybeEndTurn();
}

void OmniAI::playerStartsTurn(PlayerColor player)
{
	// A leaked count would make every moveBestHero sit out the full wait
	// deadline, which is a worse failure than the race it guards against.
	// No battle of ours survives a turn boundary, so this is always safe.
	battlesActive_.store(0);
	endTurnHeldSince_.store(0);

	if(player == playerID && evaluator_)
		evaluator_->clearCache();
}

void OmniAI::playerEndsTurn(PlayerColor player)
{
	if(player == playerID)
	{
		endTurnPending_ = false;
		endTurnHeldSince_.store(0);
		actionQueue_->sweepExpired(std::chrono::seconds(30));
		omniai::LearningStore::instance().flush();
	}
}

void OmniAI::heroVisit(const CGHeroInstance * visitor, const CGObjectInstance * visitedObj, bool start)
{
	if(!start || !visitedObj || !visitor || visitor->tempOwner != playerID)
		return;
	lastVisitor_.store(visitor->id.getNum());

	// Recorded before the learning-store gate, because this set has to work
	// with learning switched off: it is what stops the hero walking the
	// same three objects for twenty game years.
	{
		std::lock_guard lock(visitedMutex_);
		visitedObjs_.insert(visitedObj->id.getNum());
		objectVisitWeek_[visitedObj->id.getNum()] =
			cb ? cb->getDate(Date::DAY) / 7 : 0;
	}

	auto & learn = omniai::LearningStore::instance();
	if(!learn.enabled())
		return;

	// A visit we chose counts as weak-positive evidence for the type.
	learn.recordVisit(visitedObj->ID.getNum(), true);

	// One-shot structures: the reward is gone for this hero once taken,
	// and the object stays on the map looking visitable. Mark them so the
	// next game skips them for any hero (per-hero once-only in engine
	// terms: witch hut skill, shrine spell, scholar, learning stones).
	switch(visitedObj->ID.toEnum())
	{
		case Obj::WITCH_HUT:
		case Obj::SCHOLAR:
		case Obj::SHRINE_OF_MAGIC_INCANTATION:
		case Obj::SHRINE_OF_MAGIC_GESTURE:
		case Obj::SHRINE_OF_MAGIC_THOUGHT:
		case Obj::LEARNING_STONE:
		case Obj::TREE_OF_KNOWLEDGE:
		case Obj::WINDMILL:
		case Obj::WATER_WHEEL:
		case Obj::MYSTICAL_GARDEN:
			learn.markDepleted(visitedObj->id.getNum());
			break;
		default:
			break;
	}
}

void OmniAI::battleStart(const BattleID & battleID, const CCreatureSet * army1,
	const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1,
	const CGHeroInstance * hero2, BattleSide side, bool replayAllowed)
{
	// Capture the predicted strength ratio before delegating; battleEnd
	// pairs it with the outcome for the learning threshold.
	battlesActive_.fetch_add(1);
	if(army1 && army2)
	{
		const double mine = side == BattleSide::LEFT_SIDE
			? double(army1->getArmyStrength()) : double(army2->getArmyStrength());
		const double theirs = side == BattleSide::LEFT_SIDE
			? double(army2->getArmyStrength()) : double(army1->getArmyStrength());
		if(theirs > 0.0)
		{
			// The evaluator's own pre-fight call, so the log can answer
			// "was the fight misjudged" instead of only "was it lopsided".
			// Enemy heroes are the armed object; a neutral guard resolves
			// to whatever armed thing sits on the battle tile.
			const CGHeroInstance * ours =
				(hero1 && hero1->tempOwner == playerID) ? hero1
				: (hero2 && hero2->tempOwner == playerID) ? hero2 : nullptr;
			const CGHeroInstance * foeHero =
				(hero1 && hero1->tempOwner.isValidPlayer()
					&& hero1->tempOwner != playerID) ? hero1
				: (hero2 && hero2->tempOwner.isValidPlayer()
					&& hero2->tempOwner != playerID) ? hero2 : nullptr;
			double margin = -2.0;
			std::string priced;
			// A siege we start is priced as a siege: the town, its walls and
			// the garrison that merges into the defending hero. Priced against
			// the garrison hero alone, R6x r02's capture of Dunwall read keep
			// 0.70 and kept 0.24, and the siege model had no record to be
			// checked against.
			const CGTownInstance * attackedTown = nullptr;
			if(ours && hero1 == ours)
				for(const CGObjectInstance * o : cb->getVisitableObjs(tile, false))
					if(const auto * t = dynamic_cast<const CGTownInstance *>(o))
						if(t->tempOwner != playerID)
							attackedTown = t;
			if(ours)
			{
				const CArmedInstance * foeObj = attackedTown
					? static_cast<const CArmedInstance *>(attackedTown)
					: foeHero ? static_cast<const CArmedInstance *>(foeHero)
					: strongestHostileAt(tile, ours);
				if(foeObj)
				{
					const auto v = combatVerdict(ours, foeObj);
					margin = v.win ? v.margin : -v.margin;
					// Which object the verdict priced, so a flawless-win call on a
					// lost fight can be traced: twice now (m009 at 4.32, C_duel r01
					// at 0.66) the evaluator returned exp 1.00, which only its
					// empty-defender branch produces.
					priced = std::string(foeObj->getObjectName()) + ":"
						+ std::to_string(foeObj->stacksCount()) + "st";
				}
				else
					priced = "none";
			}
			// The army that actually fought, from the engine's own battle start.
			const CCreatureSet * foeArmy = side == BattleSide::LEFT_SIDE ? army2 : army1;
			std::ostringstream foeList;
			bool firstFoe = true;
			for(const auto & st : foeArmy->Slots())
				if(st.second && st.second->getCreature())
				{
					foeList << (firstFoe ? "" : ",") << st.second->getCount()
						<< "x" << st.second->getCreature()->getNameSingularTranslated();
					firstFoe = false;
				}
			PendingBattle pb{ mine / theirs, side,
				hero1 && hero1->tempOwner == playerID, margin };
			pb.who = ours ? ours->getNameTranslated() + "/" + roleOf(ours)
				: std::string("garrison");
			if(foeHero)
				pb.who += " vs " + foeHero->getNameTranslated();
			pb.who += " [priced " + priced + "; fought " + foeList.str() + "]";
			if(attackedTown)
			{
				// K's note on the siege price (September 24th): the walls deal
				// no damage. They hold back whatever cannot fly, which then
				// stands in the towers' fire for the rounds the breach takes,
				// so the price should depend on how much of our army walks.
				// Logged first, to fit against: the share of our army's value
				// that cannot fly.
				const CCreatureSet * ourArmy = side == BattleSide::LEFT_SIDE ? army1 : army2;
				uint64_t all = 0, ground = 0;
				for(const auto & st : ourArmy->Slots())
					if(st.second && st.second->getCreature())
					{
						const auto * cre = st.second->getCreature();
						const uint64_t v = uint64_t(st.second->getCount())
							* uint64_t(std::max(0, cre->getAIValue()));
						all += v;
						if(!cre->hasBonusOfType(BonusType::FLYING))
							ground += v;
					}
				pb.who += " [siege attack fort " + std::to_string(int(attackedTown->fortLevel()))
					+ ", ground " + std::to_string(all ? int(ground * 100 / all) : 0) + "%]";
			}
			// A siege on one of our towns: record what the siege model says
			// the attacker keeps, so its wall pricing can be checked against
			// the "they lost" figure battleEnd prints. R3_duel r05 read
			// "keeps 84%" where the garrison alone killed a tenth of the
			// attacker; one case, so this collects more.
			const bool weDefend = !(hero1 && hero1->tempOwner == playerID);
			if(weDefend && foeHero)
			{
				const CGTownInstance * siegeTown = nullptr;
				for(const CGTownInstance * t : cb->getTownsInfo())
					if(t && t->tempOwner == playerID
						&& ((ours && ours->getVisitedTown() == t)
							|| static_cast<const CCreatureSet *>(t) == (side == BattleSide::LEFT_SIDE ? army1 : army2)))
					{
						siegeTown = t;
						break;
					}
				if(siegeTown)
				{
					const bool outside = siegeTown->isBattleOutsideTown(ours);
					const omniai::CombatVerdict sv = siegeOnUs(ours, siegeTown, foeHero);
					std::ostringstream so;
					so << " [siege fort " << int(siegeTown->fortLevel())
					   << (outside ? " fought OUTSIDE" : "")
					   << ", model: attacker " << (sv.win ? "keeps " : "loses, we keep ")
					   << int(sv.margin * 100) << "%]";
					pb.who += so.str();
				}
			}
			pb.ours = mine;
			pb.theirs = theirs;
			pendingBattleRatio_[battleID] = pb;
		}
	}
	CAdventureAI::battleStart(battleID, army1, army2, tile, hero1, hero2, side, replayAllowed);
}

void OmniAI::battleEnd(const BattleID & battleID, const BattleResult * br, QueryID queryID)
{
	const auto it = pendingBattleRatio_.find(battleID);
	if(it != pendingBattleRatio_.end())
	{
		const bool won = br && br->winner == it->second.side;

		// Say what we expected and what happened. Without this, "does it lose
		// fights it should win" cannot be answered from any log we keep: the
		// client log records hundreds of battles and no winner in any form
		// worth grepping, and this number was already being computed and
		// then handed to a learning store that is switched off by default.
		std::ostringstream o;
		o << "  FIGHT " << (won ? "won" : "LOST") << " at odds "
		  << std::fixed << std::setprecision(2) << it->second.ratio
		  << " in our favor " << (it->second.attacker ? "[we attacked]" : "[we defended]");
		if(it->second.margin > -2.0)
			o << " exp " << std::setprecision(2) << it->second.margin;
		// What the fight cost each side, in AI value, from the engine's own
		// casualty list: a won fight that halves the army is how attrition
		// loses games, and the FIGHT line alone could not show it.
		if(br)
		{
			auto lost = [&](BattleSide s) -> double
			{
				double v = 0;
				for(const auto & c : br->casualties[s])
					if(const auto * cre = c.first.toCreature())
						v += double(c.second) * double(std::max(0, cre->getAIValue()));
				return v;
			};
			const BattleSide us = it->second.side;
			const BattleSide them = us == BattleSide::ATTACKER
				? BattleSide::DEFENDER : BattleSide::ATTACKER;
			o << std::setprecision(0) << " | " << it->second.who
			  << " lost " << lost(us) << "/" << it->second.ours
			  << ", they lost " << lost(them) << "/" << it->second.theirs;
		}
		if(!won && (it->second.ratio > 1.0 || it->second.margin > 0.0))
			o << "  <- lost a fight we were favored in";
		decisionLog_->line(o.str());
		{
			double lostUs = -1, lostThem = -1;
			if(br)
			{
				auto lostOf = [&](BattleSide s) -> double
				{
					double v = 0;
					for(const auto & c : br->casualties[s])
						if(const auto * cre = c.first.toCreature())
							v += double(c.second) * double(std::max(0, cre->getAIValue()));
					return v;
				};
				const BattleSide us = it->second.side;
				lostUs = lostOf(us);
				lostThem = lostOf(us == BattleSide::ATTACKER ? BattleSide::DEFENDER : BattleSide::ATTACKER);
			}
			JsonObj f;
			f.str("result", won ? "won" : "lost").str("we", it->second.attacker ? "attacked" : "defended")
				.num("odds", it->second.ratio);
			if(it->second.margin > -2.0)
				f.num("expected_keep", it->second.margin);
			f.str("who", it->second.who).num("had", it->second.ours).num("they_had", it->second.theirs);
			if(lostUs >= 0)
				f.num("lost", lostUs).num("they_lost", lostThem);
			decisionLog_->event("fight", f.done());
		}

		omniai::LearningStore::instance().recordBattle(it->second.ratio, won);
		pendingBattleRatio_.erase(it);
	}
	CAdventureAI::battleEnd(battleID, br, queryID);
	if(battlesActive_.fetch_sub(1) <= 0)
		battlesActive_.store(0); // never let a stray end drive it negative

	// The turn may have been waiting on this fight to close.
	maybeEndTurn();
}

void OmniAI::heroGotLevel(const CGHeroInstance *hero, PrimarySkill pskill,
	std::vector<SecondarySkill> &skills, QueryID queryID)
{
	openQueries_.fetch_add(1);
	// The engine's own offer logic (GameRandomizer::rollSecondarySkillForLevelup)
	// already guarantees Wisdom and a magic school surface periodically, so by
	// the time skills[] reaches here it is usually just two live choices, not
	// a judgment call across all ~28 skills - this only needs to break the tie
	// the engine leaves open, not rank the whole tree. Preferring a skill the
	// hero already holds over a brand new one was a pure coin flip before:
	// deepening Basic to Advanced/Expert is almost always at least as strong
	// as the same level in a skill starting from zero, and it does not spend
	// one of the hero's 8 skill slots the way a new skill does.
	int pick = skills.empty() ? 0
		: CRandomGenerator::getDefault().nextInt((int)skills.size() - 1);
	if(hero && skills.size() > 1)
	{
		std::vector<int> known;
		for(int i = 0; i < int(skills.size()); ++i)
			if(hero->getSecSkillLevel(skills[i]) > 0)
				known.push_back(i);
		if(!known.empty())
			pick = known[CRandomGenerator::getDefault().nextInt((int)known.size() - 1)];
	}
	const std::string name = hero ? hero->getNameTranslated() : "hero";
	executeAsync("heroGotLevel", [this, queryID, pick, name, n = skills.size()]
	{
		decisionLog_->detail(
			name + " levelled up, chose skill option " + std::to_string(pick)
			+ " of " + std::to_string(n));
		answerQuery(queryID, pick);
	});
}

void OmniAI::commanderGotLevel(const CCommanderInstance * commander,
	std::vector<ui32> skills, QueryID queryID)
{
	openQueries_.fetch_add(1);
	const int pick = skills.empty()
		? 0
		: CRandomGenerator::getDefault().nextInt((int)skills.size() - 1);
	executeAsync("commanderGotLevel", [this, queryID, pick]{ answerQuery(queryID, pick); });
}

void OmniAI::showBlockingDialog(const std::string &text, const std::vector<Component> &components,
	QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept)
{
	openQueries_.fetch_add(1);
	// Nullkiller semantics: selection dialogs answer the LAST component
	// (1-indexed), yes/no dialogs answer 1 (the AI walked there on
	// purpose), pure info dialogs answer 0.
	//
	// One override ahead of that default: a treasure chest's reward choice
	// (config/objects/rewardablePickable.json - gold or experience, gold
	// listed first at every value tier) always presented as [gold,
	// experience], so "last component" picked experience every time -
	// backwards from the week-1 rule to bank gold over levels early. Only
	// fires when the dialog genuinely offers that choice (a RESOURCE:GOLD
	// component and an EXPERIENCE component both present), so every other
	// selection dialog - skills, spells, artifacts - keeps the existing
	// last-component default untouched. Does not attempt the research
	// note's finer exception (take experience when a specific hero is one
	// chest from a named power-spike skill); that needs per-hero skill
	// lookahead this does not have yet.
	int answer = -1;
	if(selection && !components.empty())
	{
		int goldIdx = -1, expIdx = -1;
		for(int i = 0; i < int(components.size()); ++i)
		{
			if(components[i].type == ComponentType::RESOURCE
				&& components[i].subType.as<GameResID>() == GameResID::GOLD)
				goldIdx = i;
			else if(components[i].type == ComponentType::EXPERIENCE)
				expIdx = i;
		}
		if(goldIdx >= 0 && expIdx >= 0)
		{
			answer = goldIdx + 1;   // components are answered 1-indexed
			// Experience when a fighter opens it and gold is not what binds
			// us. Gold only buys soldiers that exist and buildings we can
			// plan; past that it sits in the purse. C_duel r02 held 7-11k gold
			// idle from day 21 to 84 with wood the binding shortage, while
			// Nullkiller, which always takes the experience, levelled its
			// main to 5-6 and ours stayed level 1. "Covered" means the purse
			// already holds the planner's saving target, every creature on
			// sale in our towns and a spare hire.
			const CGHeroInstance * v = cb
				? cb->getHero(ObjectInstanceID(lastVisitor_.load())) : nullptr;
			// Any of our heroes, since R7j. Collectors open most chests: the
			// R7g Islands game opened 12 and took gold every time, none of
			// them by the field hero, and ended day 21 with a best hero of
			// level 2 against Nullkiller's 12 on day 11. A collector that
			// levels is a candidate commander (pickFieldHero), so the
			// experience reaches the army either way.
			if(v && v->tempOwner == playerID)
			{
				// Only the planner's next building counts against it now. The
				// old bar added every creature on sale and a spare hire, which
				// the purse almost never covers, so across K_duel and R3_duel
				// (24 games) the field hero took experience twice while
				// Nullkiller's main takes it every time and sits at level 3-5
				// by day 14 against our 1. Experience is the commander's
				// attack and defence on every stack, for the rest of the game.
				const int need = buildReserveCached_.load();
				const int gold = cb->getResourceAmount(EGameResID::GOLD);
				if(gold >= need)
					answer = expIdx + 1;
				decisionLog_->line(std::string("  CHEST ") + v->getNameTranslated() + " takes "
					+ (answer == expIdx + 1 ? "experience" : "gold") + " (gold "
					+ std::to_string(gold) + " against " + std::to_string(need) + " needed)");
			}
		}
	}
	if(answer < 0)
		answer = (selection && !components.empty())
			? (int)components.size()
			: (cancel ? 1 : 0);
	executeAsync("blockingDialog", [this, askID, answer]{ answerQuery(askID, answer); });
}

void OmniAI::showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel,
	TTeleportExitsList exits, bool impassable, QueryID askID)
{
	openQueries_.fetch_add(1);
	// No "decline" answer exists: 0 picks exits[0], any invalid index
	// rolls a RANDOM exit (HeroMovementController.cpp:79). This handler
	// should be unreachable anyway - pathfinding never routes through
	// teleports - so prefer the deterministic first exit over a random
	// one if it somehow fires.
	const int answer = exits.empty() ? -1 : 0;
	executeAsync("teleportDialog", [this, askID, answer]{ answerQuery(askID, answer); });
}

void OmniAI::showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down,
	bool removableUnits, QueryID queryID, const MetaString & customTitle)
{
	openQueries_.fetch_add(1);
	executeAsync("garrisonDialog", [this, queryID]{ answerQuery(queryID, 0); });
}

void OmniAI::showMapObjectSelectDialog(QueryID askID, const Component & icon,
	const MetaString & title, const MetaString & description,
	const std::vector<ObjectInstanceID> & objects)
{
	openQueries_.fetch_add(1);
	// Answers are object instance ids, not list indices (Nullkiller replies
	// selectedObject.getNum()).
	const int answer = objects.empty() ? 0 : objects.front().getNum();
	executeAsync("objectSelect", [this, askID, answer]{ answerQuery(askID, answer); });
}

double OmniAI::buildingValue(const CGTownInstance * town, const BuildingID & id) const
{
	// Everything is expressed in army value, the same units the danger check
	// and the home errand use, so a building can be argued against a stack of
	// troops and against another building.
	//
	// Horizons and rates. These are the honest guesses in here and they are
	// labelled as such: how far ahead a purchase is worth counting, and what
	// a gold piece is worth once it has become soldiers. The gold rate comes
	// from creature costs in the shipped data, where a Pikeman is 80 of army
	// for 60 gold and a Griffin 351 for 200, so somewhere above one.
	const double HORIZON_DAYS = valueHorizonDays();
	const double WEEKS_AHEAD = HORIZON_DAYS / 7.0;
	constexpr double ARMY_PER_GOLD = 1.2;
	// A unit of wood or ore is worth more than a gold piece because it gates
	// building outright rather than merely paying for it.
	constexpr double RARE_MULTIPLE = 8.0;

	if(!town || !cb)
		return 0.0;
	const auto * t = town->getTown();
	if(!t)
		return 0.0;
	const auto found = t->buildings.find(id);
	if(found == t->buildings.end() || !found->second)
		return 0.0;
	const CBuilding * b = found->second.get();

	// Only things a player can actually choose to raise. The grail needs an
	// artifact we do not have, automatic buildings appear on their own, and
	// special ones cannot be bought at all. canBuildStructure says ALLOWED
	// for the grail regardless, which cost a 101 day match every one of its
	// build turns.
	if(b->mode != CBuilding::BUILD_NORMAL)
		return 0.0;

	// A floor, not a valuation. Forts, citadels, mage guilds and blacksmiths
	// have no income, no creatures and no market, so everything below scores
	// them at nothing, and a town that can only build things worth zero stops
	// building. Fortifications are genuinely not valued yet; this keeps
	// development moving without pretending otherwise.
	//
	// Removing this floor was tried on 2026-09-21 and reverted. It did stop
	// the AI buying things it had never valued, and a duel-v2 game then
	// raised ONE building all game and held its 10 wood and 10 ore to the
	// end. It also killed the chain: a fort scores zero on its own merits
	// and every dwelling sits behind one, so the build queue printed "no
	// building possible" every day from day 2 with gold in hand. Crediting
	// a prerequisite with a share of what it unlocks was tried next and
	// recovered most of the Twins collapse it caused (growth median x1.18
	// to x11.49) without reaching the x19.51 of the build that had neither.
	// The scarce-mine run addresses the same starvation at its source, so
	// neither change is load-bearing now. SID-20260921-4c7e21.
	double value = 50.0;

	// Income, straight from the building's own data - net of what the
	// building it replaces already produces. A Town Hall's 1000 a day
	// supersedes the Village Hall's 500, so raising it gains 500, not 1000;
	// crediting the gross figure doubled the hall chain's worth (the Capitol
	// read 134400 against a real gain of half that) and pushed it ahead of
	// the army its gold would otherwise buy.
	const CBuilding * replaced = nullptr;
	if(b->upgrade != BuildingID::NONE && town->hasBuilt(b->upgrade))
	{
		const auto base = t->buildings.find(b->upgrade);
		if(base != t->buildings.end() && base->second)
			replaced = base->second.get();
	}
	for(int r = 0; r < GameConstants::RESOURCE_QUANTITY; ++r)
	{
		const int amount = b->produce[GameResID(r)]
			- (replaced ? replaced->produce[GameResID(r)] : 0);
		if(amount <= 0)
			continue;
		const double perUnit = (r == GameResID(EGameResID::GOLD).getNum())
			? ARMY_PER_GOLD : ARMY_PER_GOLD * RARE_MULTIPLE;
		value += double(amount) * HORIZON_DAYS * perUnit;
	}

	// A dwelling is worth the army it will produce, so a Behemoth lair and a
	// Halberdier hut stop being the same building.
	for(int level = 0; level < GameConstants::CREATURES_PER_TOWN; ++level)
	{
		for(int up = 0; up < 3; ++up)
		{
			if(BuildingID::getDwellingFromLevel(level, up) != id)
				continue;
			if(level >= int(t->creatures.size()) || t->creatures[level].empty())
				continue;
			const int which = std::min<int>(up, int(t->creatures[level].size()) - 1);
			const auto * cre = t->creatures[level][which].toCreature();
			if(!cre)
				continue;
			// The engine reports no growth for a dwelling that does not exist
			// yet (CGTownInstance::getGrowthInfo: "no dwelling"), so an
			// unbuilt dwelling read as 1 creature a week: a Griffin Tower at
			// a seventh of its worth, the Barracks at a quarter. The planner
			// then never raised the Barracks, which gates every Castle
			// dwelling from level 3 up: in 32 K and R3 games our town built
			// it zero times, ended with tier 1-4 armies, and banked up to
			// 74k gold with nothing to buy (R6h r02), while Nullkiller built
			// its levels 3-6. Unbuilt: the creature's own base growth.
			int growth = town->creatureGrowth(level);
			if(growth <= 0)
				growth = cre->getGrowth();
			growth = std::max(1, growth);
			double worth = double(growth) * double(std::max(0, cre->getAIValue()))
				* WEEKS_AHEAD;
			// An upgrade only buys the difference, not the whole stack.
			if(up > 0 && !t->creatures[level].empty())
			{
				const auto * base = t->creatures[level][0].toCreature();
				if(base)
					worth = double(growth) * double(std::max(0,
						cre->getAIValue() - base->getAIValue())) * WEEKS_AHEAD;
			}
			value += worth;
		}
	}

	// A market is worth exactly what it unblocks, and only while something is
	// blocked. Built early today because a run finished holding sixteen
	// thousand gold and two wood, with the build queue stopped since day six.
	if(!b->marketModes.empty())
	{
		const int wood = cb->getResourceAmount(EGameResID::WOOD);
		const int ore = cb->getResourceAmount(EGameResID::ORE);
		const int gold = cb->getResourceAmount(EGameResID::GOLD);
		const int scarcest = std::min(wood, ore);
		if(scarcest < 15 && gold > 5000)
			value += double(gold - 4000) * 0.25 * ARMY_PER_GOLD;
	}

	// A mage guild is worth the spells a visiting hero walks away with. Town
	// Portal is the single biggest tempo lever in the game - it teleports a
	// courier's delivery leg, snaps a defender home to a threatened town, and
	// puts the field hero on a captured frontier - and the spell probe shows
	// our heroes know zero adventure-movement spells precisely because no
	// town ever builds the guild that teaches them. Score it when a hero we
	// own would actually visit and learn from it, higher for the levels that
	// teach the tempo spells.
	{
		const int num = int(id.getNum());
		if(num >= int(BuildingID::MAGES_GUILD_1)
			&& num <= int(BuildingID::MAGES_GUILD_5))
		{
			const int guildLevel = num - int(BuildingID::MAGES_GUILD_1) + 1;
			// Count every hero, not only spellbook carriers: a hero that
			// walks into a town with a guild gets a book and the guild's
			// spells in the same visit, so gating the value on already
			// owning a book is what left the first guild at the floor price
			// and stalled the chain before it could teach anything.
			int learners = 0;
			for(const CGHeroInstance * h : cb->getHeroesInfo())
				if(h)
					++learners;
			// The tempo spells live at guild level 3 and up, and Town Portal
			// alone repays the build: a courier that teleports its delivery
			// leg and a defender that snaps home are worth more than the
			// extra dwelling this gold would buy. Score it to climb, not to
			// sit at level 1 - each level toward 3 is worth more, and a hero
			// that already knows Town Portal makes the whole chain moot.
			if(learners > 0)
			{
				bool portalKnown = false;
				int bestSpellLevel = 0;
				for(const CGHeroInstance * h : cb->getHeroesInfo())
				{
					if(!h)
						continue;
					if(int(h->maxSpellLevel()) > bestSpellLevel)
						bestSpellLevel = int(h->maxSpellLevel());
					if(h->hasSpellbook()
						&& h->getSpellsInSpellbook().count(SpellID(SpellID::TOWN_PORTAL)))
						portalKnown = true;
				}
				// A hero that can already learn a level-4 spell is the reason
				// to push the guild to 4 now rather than let it idle at 3 -
				// that is the level Town Portal lives at.
				const double wise = bestSpellLevel >= 4 ? 1.8 : 1.0;
				// The field hero is the learner that matters: its spells are
				// cast in the fights that decide the game. Every other hero
				// counts a quarter. Counting all of them in full priced a
				// level-3 guild at 45000 with six heroes (BUILDSCAN, R7l3c:
				// guild levels x10-16 against dwellings x1.3-3), so the gold
				// went to guilds while the level-7 dwelling sat at x1.3.
				const double weightedLearners = 1.0 + 0.25 * double(std::max(0, learners - 1));
				if(!portalKnown)
					value += wise * weightedLearners * (1500.0 + double(guildLevel) * 2000.0);
			}
		}
	}

	// The tavern. The only building that undoes a dead hero, so what not
	// having it costs is the army that dies with him and everything that
	// army would have taken. A 49 day match spent 36 of those days logging
	// "no heroes left and nowhere to hire one" while holding 3100 gold.
	if(id == BuildingID::TAVERN && !town->hasBuilt(BuildingID::TAVERN))
	{
		const auto heroes = cb->getHeroesInfo();
		if(heroes.size() <= 1)
		{
			uint64_t exposed = 0;
			for(const CGHeroInstance * h : heroes)
				if(h)
					exposed += h->getArmyStrength();
			value += double(std::max<uint64_t>(exposed, 2000));
		}
	}

	return value;
}

double OmniAI::fortGrowthValue(const CGTownInstance * town, const BuildingID & id) const
{
	// Citadel and Castle each add half of every dwelling's base growth, the
	// H3 rule VCMI ships in its town config. buildingValue prices them at the
	// 50 floor because they produce nothing directly, which left the fort
	// line unwanted in every run: fort=0 all game on duel-v2 while Nullkiller
	// stood at a Castle by day 14 (TRUTH, A_duel_fog r01).
	if(!town || (id != BuildingID::CITADEL && id != BuildingID::CASTLE))
		return 0.0;
	const auto * t = town->getTown();
	if(!t)
		return 0.0;
	const double WEEKS_AHEAD = valueHorizonDays() / 7.0;   // buildingValue's horizon
	double weekly = 0.0;
	for(int level = 0; level < GameConstants::CREATURES_PER_TOWN; ++level)
	{
		if(level >= int(t->creatures.size()) || t->creatures[level].empty())
			continue;
		if(!town->hasBuilt(BuildingID::getDwellingFromLevel(level, 0)))
			continue;
		const auto * cre = t->creatures[level][0].toCreature();
		if(cre)
			weekly += double(cre->getGrowth()) * double(std::max(0, cre->getAIValue()));
	}
	return 0.5 * weekly * WEEKS_AHEAD;
}

int OmniAI::surplusGold() const
{
	// Gold nothing already planned will spend: the build planner's saving
	// target, every creature on sale in our towns, and one hire kept back.
	// H_duel r01 banked 74625 gold by day 47 (21050 at day 28) with the
	// Capitol built and nothing left in the shops, while Nullkiller spent
	// down to ~16k and grew 30k -> 90k in twelve days.
	if(!cb)
		return 0;
	int need = buildReserveTotal() + GameConstants::HERO_GOLD_COST;
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town || town->tempOwner != playerID)
			continue;
		for(const auto & level : town->creatures)
		{
			if(level.first == 0 || level.second.empty())
				continue;
			if(const auto * cre = level.second.back().toCreature())
				need += int(level.first) * cre->getFullRecruitCost()[EGameResID::GOLD];
		}
	}
	return cb->getResourceAmount(EGameResID::GOLD) - need;
}

void OmniAI::upgradeArmy(const CArmedInstance * army)
{
	// Port of Nullkiller's makePossibleUpgrades (AIGateway.cpp:808), paid
	// from surplus only. An upgrade is the best army-per-gold on offer once
	// the shops are empty: Pikeman to Halberdier is +35 army for 15 gold,
	// Griffin to Royal Griffin +97 for 40. We had never upgraded a stack.
	if(!cb || !army)
		return;
	for(int i = 0; i < GameConstants::ARMY_SIZE; ++i)
	{
		const CStackInstance * s = army->getStackPtr(SlotID(i));
		if(!s || !s->getCreature())
			continue;
		UpgradeInfo info(s->getId());
		cb->fillUpgradeInfo(army, SlotID(i), info);
		if(!info.hasUpgrades())
			continue;
		CreatureID best = CreatureID::NONE;
		int bestValue = s->getCreature()->getAIValue();
		for(const CreatureID & up : info.getAvailableUpgrades())
			if(up.toCreature() && up.toCreature()->getAIValue() > bestValue)
			{
				bestValue = up.toCreature()->getAIValue();
				best = up;
			}
		if(best == CreatureID::NONE)
			continue;
		const TResources cost = info.getUpgradeCostsFor(best) * s->getCount();
		TResources free = cb->getResourceAmount();
		free[EGameResID::GOLD] = std::max(0, surplusGold());
		if(!free.canAfford(cost))
			continue;
		decisionLog_->line("  UPGRADE " + std::to_string(s->getCount()) + " "
			+ s->getCreature()->getNamePluralTranslated() + " to "
			+ best.toCreature()->getNamePluralTranslated() + " for "
			+ std::to_string(cost[EGameResID::GOLD]) + " gold");
		cb->upgradeCreature(army, SlotID(i), best);
	}
}

int OmniAI::buildReserveTotal() const
{
	// A town that came under threat since dawn spends its savings on the
	// wall now; the threat scan runs every round, the build pass once a day.
	int total = 0;
	for(const auto & kv : buildReserve_)
		if(!dangerTowns_.count(kv.first))
			total += kv.second;
	return total;
}

void OmniAI::buildInTown(const CGTownInstance * town)
{
	if(!cb || !town)
		return;

	// The build order used to be a hand-written priority list. It is gone,
	// and the reason is worth keeping: a list is one person's answer to
	// "what should we pay for first", frozen at the moment it was written.
	// Two separate failures today were things sitting far enough down it to
	// be unreachable. The marketplace never got built, so a run finished
	// with sixteen thousand gold and two wood and a build queue stopped
	// since day six. The tavern never got built, so a hero died on day 25
	// and the AI spent 36 further days logging that it had nowhere to hire
	// another while holding enough gold for three.
	//
	// buildingValue() asks the question instead of answering it in advance.

	// Every building the engine says we may raise, scored against each
	// other. No order, so nothing can sit behind a wall of dwelling
	// upgrades and never be reached, which is what happened to the
	// marketplace and then again to the tavern.
	const auto * t = town->getTown();
	if(!t)
		return;

	// What this pass will not spend. With no hero on the map the only
	// purchase that matters is the one that gets us another, so nothing
	// except the tavern may eat the fare.
	const bool heroless = cb->getHeroesInfo().empty();
	const int gold = cb->getResourceAmount(EGameResID::GOLD);
	const TResources purse = cb->getResourceAmount();
	const int32_t townKey = town->id.getNum();
	// A threatened town needs soldiers on the wall this week, not a payback
	// next month: no saving, and only steps that add walls or growth.
	const bool threatened = dangerTowns_.count(townKey) != 0;
	// Recomputed below every dawn, except under a threat. buildReserveTotal
	// already leaves a threatened town out, so the gold is free for the wall
	// today; wiping the saving as well meant it was gone on the morning the
	// threat passed, and the dawn hire runs before this pass. R6v r01 saved
	// 5000 for the City Hall, lost the saving to a two-day threat, re-hired
	// on day 8 with no reserve, and built the City Hall on day 14.
	if(!threatened)
		buildReserve_.erase(townKey);

	// The rate the gold would otherwise turn into army: the best AI value per
	// gold among creatures on sale here, or the Pikeman-class rate (80 army
	// for 60 gold, shipped creature data) when the shop is empty. A path must
	// beat that rate over the valuation horizon to be worth its gold.
	double armyRate = 80.0 / 60.0;
	for(const auto & level : town->creatures)
	{
		if(level.first == 0 || level.second.empty())
			continue;
		const auto * cre = level.second.back().toCreature();
		const int each = cre ? cre->getFullRecruitCost()[EGameResID::GOLD] : 0;
		if(each > 0)
			armyRate = std::max(armyRate, double(std::max(0, cre->getAIValue())) / double(each));
	}
	// Ranking prices for non-gold resources. Provisional (the common H3 rule
	// of thumb, not measured): wood and ore 100 gold, the rare four 200.
	auto goldEquivalent = [](const TResources & r) -> double
	{
		return double(r[EGameResID::GOLD])
			+ 100.0 * double(r[EGameResID::WOOD] + r[EGameResID::ORE])
			+ 200.0 * double(r[EGameResID::MERCURY] + r[EGameResID::SULFUR]
				+ r[EGameResID::CRYSTAL] + r[EGameResID::GEMS]);
	};
	auto isDwelling = [](const BuildingID & id)
	{
		for(int level = 0; level < GameConstants::CREATURES_PER_TOWN; ++level)
			for(int up = 0; up < 3; ++up)
				if(BuildingID::getDwellingFromLevel(level, up) == id)
					return true;
		return false;
	};
	auto directValue = [&](const BuildingID & id)
	{
		return buildingValue(town, id) + fortGrowthValue(town, id);
	};
	// A step can be taken today, or needs only gold to be taken: anything
	// short of wood, ore or a rare is not fixed by saving, and a reserve held
	// for it would freeze recruitment for as long as the mine is missing.
	auto goldOnlyShort = [&](const CBuilding * b)
	{
		for(int r = 0; r < GameConstants::RESOURCE_QUANTITY; ++r)
			if(r != GameResID(EGameResID::GOLD).getNum()
				&& b->resources[GameResID(r)] > purse[GameResID(r)])
				return false;
		return true;
	};

	// The planner. Every unbuilt building is a target reached by a path: the
	// building plus whatever it still requires. A path is priced as a whole,
	// so a marketplace is worth what the City Hall behind it is worth, not
	// the 50-point floor it scored on its own, and the choice is value per
	// gold across whole paths rather than the best building allowed today.
	struct Plan
	{
		BuildingID target = BuildingID::NONE, next = BuildingID::NONE;
		double value = 0, cost = 0, roi = 0;
		bool nextAllowed = false, income = false;
	};
	Plan best;
	int lastBlocked = -1;
	// BUILDSCAN, once a week per town: every candidate path with its worth,
	// cost and ratio, the ones dropped as "worth more as soldiers" included.
	// K's tier-7 target (September 24th): our towns have never raised a
	// level-7 dwelling in any duel-v2 game, and the planner only ever logged
	// what it built, so nothing said whether one was even in the running.
	struct ScanRow { BuildingID id; double value, cost, roi; bool asSoldiers; };
	std::vector<ScanRow> scan;
	for(const auto & entry : t->buildings)
	{
		const BuildingID id = entry.first;
		const CBuilding * b = entry.second.get();
		if(!b || b->mode != CBuilding::BUILD_NORMAL || town->hasBuilt(id)
			|| town->forbiddenBuildings.count(id))
			continue;
		const EBuildingState state = cb->canBuildStructure(town, id);
		if(state == EBuildingState::CANT_BUILD_TODAY)
			return;   // this town already built today
		if(state != EBuildingState::ALLOWED && state != EBuildingState::NO_RESOURCES
			&& state != EBuildingState::PREREQUIRES)
		{
			lastBlocked = int(state);
			continue;
		}
		if(heroless && id != BuildingID::TAVERN)
			continue;   // with no hero left only the tavern matters

		std::vector<BuildingID> path{id};
		bool dead = false;
		town->genBuildingRequirements(id, true).morph(
			[&](const BuildingID & dep) -> CBuilding::TRequired::Variant
		{
			if(!town->hasBuilt(dep))
			{
				if(town->forbiddenBuildings.count(dep) || !t->buildings.count(dep))
					dead = true;
				else if(std::find(path.begin(), path.end(), dep) == path.end())
					path.push_back(dep);
			}
			return dep;
		});
		if(dead)
			continue;

		TResources cost;
		double value = 0;
		bool income = false;
		for(const BuildingID & p : path)
		{
			const CBuilding * pb = t->buildings.at(p).get();
			cost += pb->resources;
			value += directValue(p);
			if(pb->produce[EGameResID::GOLD] > 0)
				income = true;
		}
		const double gc = goldEquivalent(cost);
		scan.push_back({ id, value, gc, gc > 0 ? value / gc : 0.0,
			gc <= 0 || value <= gc * armyRate });
		if(gc <= 0 || value <= gc * armyRate)
			continue;   // its gold is worth more as soldiers

		// The step to take now: a member whose own requirements are met.
		BuildingID next = BuildingID::NONE;
		bool nextAllowed = false;
		double nextVal = -1;
		int nextRank = -1;
		for(const BuildingID & p : path)
		{
			const EBuildingState ps = cb->canBuildStructure(town, p);
			const CBuilding * pb = t->buildings.at(p).get();
			const bool allowed = ps == EBuildingState::ALLOWED;
			if(!allowed && !(ps == EBuildingState::NO_RESOURCES && goldOnlyShort(pb)))
				continue;
			if(threatened && !allowed)
				continue;
			if(threatened && p != BuildingID::FORT && p != BuildingID::CITADEL
				&& p != BuildingID::CASTLE && !isDwelling(p))
				continue;
			const double pv = directValue(p);
			// An income step short of nothing but gold comes first, ahead of
			// any other step buildable today: the saving branch below holds
			// the gold for it, and a hall a day late costs its income for
			// that day. R7l3c Twins r01 built Tavern, Mage Guild, Marketplace
			// and only then the Town Hall on day 7, because a gold-short
			// Town Hall lost to whatever else was buildable (Nullkiller had
			// 2000 a day by day 7 on the mirrored side, we had 500).
			const bool incomeStep = pb->produce[EGameResID::GOLD] > 0;
			const int rank = incomeStep ? 2 : (allowed ? 1 : 0);
			if(next == BuildingID::NONE || rank > nextRank
				|| (rank == nextRank && ((allowed && !nextAllowed)
					|| (allowed == nextAllowed && pv > nextVal))))
			{
				next = p;
				nextAllowed = allowed;
				nextVal = pv;
				nextRank = rank;
			}
		}
		if(next == BuildingID::NONE)
			continue;
		const double roi = value / gc;
		if(roi > best.roi)
			best = { id, next, value, gc, roi, nextAllowed, income };
	}

	{
		const int week = cb->getDate(Date::DAY) / 7;
		auto seen = buildScanWeek_.find(townKey);
		if(!scan.empty() && (seen == buildScanWeek_.end() || seen->second != week))
		{
			buildScanWeek_[townKey] = week;
			std::sort(scan.begin(), scan.end(), [](const ScanRow & a, const ScanRow & b)
				{ return a.roi > b.roi; });
			std::ostringstream o;
			o << "   BUILDSCAN " << town->getNameTranslated() << " (soldiers "
			  << std::fixed << std::setprecision(2) << armyRate << " per gold):";
			for(const auto & r : scan)
				o << " " << r.id.getNum() << "=x" << std::setprecision(1) << r.roi
				  << (r.asSoldiers ? "(soldiers)" : "");
			decisionLog_->line(o.str());
		}
	}
	if(best.next != BuildingID::NONE && best.nextAllowed)
	{
		std::ostringstream o;
		o << "  BUILD " << town->getNameTranslated() << ": building "
		  << best.next.getNum();
		if(best.next != best.target)
			o << " toward " << best.target.getNum();
		o << ", path worth " << int(best.value) << " for " << int(best.cost)
		  << " (x" << std::fixed << std::setprecision(1) << best.roi << ")"
		  << "  (gold " << gold << ")";
		decisionLog_->line(o.str());
		cb->buildBuilding(town, best.next);
		return; // one per day, so stop here
	}
	if(best.next != BuildingID::NONE)
	{
		// Short only of gold. Nullkiller saves toward income buildings and
		// buys army with what is left (BuildingBehavior.cpp:79-90, the
		// SaveResources lock read by getFreeResources); do the same, and
		// never while the town is threatened.
		const int need = t->buildings.at(best.next)->resources[EGameResID::GOLD];
		if(best.income && !threatened)
		{
			buildReserve_[townKey] = need;
			decisionLog_->detail("saving " + std::to_string(need) + " for building "
				+ std::to_string(best.next.getNum()) + " toward "
				+ std::to_string(best.target.getNum()) + " (path x"
				+ std::to_string(int(best.roi * 10) / 10.0).substr(0, 4)
				+ "), have " + std::to_string(gold));
		}
		return;
	}
	// Nothing in the whole list was allowed. Worth saying, because a town
	// that quietly stops growing looks identical to one that is finished.
	std::ostringstream o;
	o << "no building possible in " << town->getNameTranslated()
	  << " (last refusal code " << lastBlocked
	  << ", gold " << cb->getResourceAmount(EGameResID::GOLD)
	  << " wood " << cb->getResourceAmount(EGameResID::WOOD)
	  << " ore " << cb->getResourceAmount(EGameResID::ORE) << ")";
	decisionLog_->detail(o.str());
	// And WHICH buildings are blocked, and by what. The refusal code above
	// is whichever refusal came last while iterating, so it cannot answer
	// "why has this town not built a dwelling in 27 days". Reported on the
	// first stalled day of each week, because a stalled town would otherwise
	// print this every day, and a fixed day-of-week gate misses a town that
	// stalls on day 4 and is building again by day 8.
	const int stallWeek = cb->getDate(Date::DAY) / 7;
	if(stallWeek != lastStallReportWeek_)
	{
		int listed = 0;
		for(const auto & blocked : t->buildings)
		{
			if(listed >= 10)
				break;
			const BuildingID bid = blocked.first;
			if(town->hasBuilt(bid) || !blocked.second
				|| blocked.second->mode != CBuilding::BUILD_NORMAL)
				continue;
			const EBuildingState st = cb->canBuildStructure(town, bid);
			if(st == EBuildingState::ALLOWED)
				continue;
			std::ostringstream b;
			b << "    blocked " << bid.getNum() << " state " << int(st);
			// WHICH resource is short, not just that one is. Holding wood for
			// a fort that was actually waiting on gold cost a measurement and
			// thirteen building-days on 2026-09-21; state 6 alone does not say.
			if(st == EBuildingState::NO_RESOURCES)
			{
				b << " shortfall";
				for(int r = 0; r < GameConstants::RESOURCE_QUANTITY; ++r)
				{
					const int need = blocked.second->resources[GameResID(r)];
					const int have = cb->getResourceAmount(GameResID(r));
					if(need > have)
						b << " " << GameConstants::RESOURCE_NAMES[r] << " "
						  << (need - have);
				}
			}
			if(st == EBuildingState::PREREQUIRES)
			{
				b << " missing";
				town->genBuildingRequirements(bid, true).morph(
					[&](const BuildingID & dep) -> CBuilding::TRequired::Variant
				{
					if(!town->hasBuilt(dep))
						b << " " << dep.getNum();
					return dep;
				});
			}
			decisionLog_->detail(b.str());
			++listed;
		}
		lastStallReportWeek_ = stallWeek;
	}
}

void OmniAI::buildInAllTowns()
{
	if(!cb)
		return;
	for(const CGTownInstance * town : cb->getTownsInfo())
		if(town)
			buildInTown(town);
	buildReserveCached_.store(buildReserveTotal());
}

void OmniAI::recruitFromDwelling(const CGDwelling * dwelling, const CArmedInstance * dst,
	int goldReserve)
{
	if(!dwelling || !dst)
		return;

	// Port of Nullkiller::recruitCreatures, level order reversed: highest
	// tier first, not lowest. Per level, merge a duplicate stack if the
	// destination has no free slot, then buy the affordable count of the
	// last (usually upgraded) alternative. Gold is read fresh every
	// iteration (below), so a purchase earlier in this same call correctly
	// shrinks what a later one can afford - which was the problem with
	// buying ascending: a week's stock of tier 1 and 2 could exhaust a
	// tight budget before the loop ever reached tier 6 or 7, even though
	// the top tiers are usually the better buy per gold spent. When gold
	// covers the whole week's stock at every level the order is a no-op;
	// it only changes anything exactly when gold is the binding limit.
	for(int i = int(dwelling->creatures.size()) - 1; i >= 0; --i)
	{
		if(dwelling->creatures[i].second.empty())
			continue;

		int count = int(dwelling->creatures[i].first);
		const CreatureID creID = dwelling->creatures[i].second.back();

		if(!dst->getSlotFor(creID).validSlot())
		{
			for(const auto & stack : dst->Slots())
			{
				if(!stack.second->getType())
					continue;
				const auto duplicatingSlot = dst->getSlotFor(stack.second->getCreature());
				if(duplicatingSlot != stack.first)
				{
					cb->mergeStacks(dst, dst, stack.first, duplicatingSlot);
					break;
				}
			}
			if(!dst->getSlotFor(creID).validSlot())
				continue;
		}

		const int available = count;
		// Spend what is on top of the reserve, not everything. See the
		// garrison call site: a town full of troops and no hero to carry
		// them is how sixty game days got thrown away. The build planner's
		// savings sit on top: gold held for the next income step is not
		// recruitment money (Nullkiller's getFreeResources rule).
		// Army before savings when contact is close (contactSoon): on
		// duel-v2 the saving for the City Hall held the whole week-two
		// growth unbought from day 8 to 11 (R7p_duel r01), and Nullkiller's
		// army went from 14.5k to 22.7k that week while ours fell to 10.5k.
		// On a big map with no enemy near, the savings still come first.
		const int reserve = goldReserve + (contactSoon() ? 0 : buildReserveTotal());
		TResources spendable = cb->getResourceAmount();
		if(reserve > 0)
			spendable[EGameResID::GOLD] =
				std::max(0, int(spendable[EGameResID::GOLD]) - reserve);
		vstd::amin(count, int(spendable / creID.toCreature()->getFullRecruitCost()));
		if(count > 0)
		{
			std::ostringstream o;
			o << "  RECRUIT " << count << " of " << available << " "
			  << creID.toCreature()->getNameSingularTranslated();
			decisionLog_->line(o.str());
			cb->recruitCreatures(dwelling, dst, creID, count, i);
		}
		else if(available > 0)
			decisionLog_->detail(
				std::string("cannot afford ") + creID.toCreature()->getNameSingularTranslated());
	}
}

void OmniAI::showRecruitmentDialog(const CGDwelling *dwelling, const CArmedInstance *dst,
	int level, QueryID queryID)
{
	openQueries_.fetch_add(1);
	// Ids, not pointers: the work happens on another thread, by which time
	// anything captured by pointer may already have been freed.
	const ObjectInstanceID dwellId = dwelling ? dwelling->id : ObjectInstanceID();
	const ObjectInstanceID dstId = dst ? dst->id : ObjectInstanceID();
	executeAsync("recruitDialog", [this, dwellId, dstId, queryID]
	{
		const auto * d = dynamic_cast<const CGDwelling *>(cb->getObj(dwellId, false));
		const auto * t = dynamic_cast<const CArmedInstance *>(cb->getObj(dstId, false));
		if(d && t)
			recruitFromDwelling(d, t);
		answerQuery(queryID, 0);
	});
}

void OmniAI::heroVisitsTown(const CGHeroInstance *hero, const CGTownInstance *town)
{
	// A player opens the town screen and buys what the garrison offers.
	// Towns are CGDwelling subclasses carrying their grown creatures in
	// creatures[], so the same recruit-all-affordable pass applies.
	if(!hero || !town || hero->tempOwner != playerID || town->tempOwner != playerID)
		return;
	const ObjectInstanceID townId = town->id;
	const ObjectInstanceID heroId = hero->id;
	executeAsync("visitTown", [this, townId, heroId]
	{
		const auto * t = dynamic_cast<const CGTownInstance *>(cb->getObj(townId, false));
		const CGHeroInstance * h = cb->getHero(heroId);
		if(!t || !h)
			return;
		// The turn thread may already have walked this hero back out of the
		// town while this task sat in the queue. The server checks exactly
		// this and refuses the pack (CGameHandler.cpp:2426), so asking is
		// free and sending blind was costing us half our recruitment.
		if(h != t->getVisitingHero() && h != t->getGarrisonHero())
		{
			decisionLog_->detail(t->getNameTranslated()
				+ ": hero left before the shop opened, leaving the stock");
			return;
		}
		decisionLog_->line(
			"  visiting own town " + t->getNameTranslated() + ", buying what it offers");
		// The garrison pool is the reason the field hero walked here, and
		// it may leave again this same day - waiting for tomorrow's
		// recruitInAllTowns would leave the pool parked under a hero that
		// is already gone.
		if(h->id.getNum() == fieldHeroId_ || cb->getHeroesInfo().size() <= 1)
			collectGarrison(t, h);
		recruitFromDwelling(t, h);
		if(h->id.getNum() == fieldHeroId_ || h->id.getNum() == secondFieldId_)
			upgradeArmy(h);
	});
}

void OmniAI::collectGarrison(const CGTownInstance * town, const CGHeroInstance * hero)
{
	if(!cb || !town || !hero)
		return;

	// Troops standing in a town do not take mines or fight heroes. Anything
	// bought into the garrison while nobody was home belongs in the army of
	// whoever is home now.
	std::vector<SlotID> slots;
	for(const auto & entry : town->Slots())
		if(entry.second && entry.second->getType())
			slots.push_back(entry.first);

	int moved = 0;
	for(const SlotID & slot : slots)
	{
		// Re-resolve: every move rewrites the garrison underneath us.
		const auto & garrison = town->Slots();
		const auto it = garrison.find(slot);
		if(it == garrison.end() || !it->second || !it->second->getType())
			continue;
		const CreatureID creID = it->second->getCreature()->getId();
		// Asking first keeps a full hero from generating refused packs.
		if(!hero->getSlotFor(creID).validSlot())
			continue;
		if(moveOneStack(town, hero, slot))
			++moved;
	}

	// Seven slots, full of whatever the hires and pickups brought: a garrison
	// stack of a type the hero does not carry cannot be added at all, so a
	// strong pool sat unused beside a weak field army. J3 MapGen s17 blue,
	// day 20: 17.6k idle in the garrison while the field hero carried 7.9k
	// and Nullkiller's 10.6k main killed our heroes one by one. Swap instead:
	// the strongest garrison stack that does not fit takes the slot of the
	// hero's weakest stack of another type, when it is worth more.
	auto stackValue = [](const CStackInstance * s) -> uint64_t
	{
		const auto * c = (s && s->getType()) ? s->getCreature() : nullptr;
		return c ? uint64_t(s->getCount()) * uint64_t(std::max(0, c->getAIValue())) : 0;
	};
	for(int guard = 0; guard < 7; ++guard)
	{
		SlotID bestTown; uint64_t bestVal = 0; const CCreature * bestCre = nullptr;
		for(const auto & e : town->Slots())
		{
			const auto * cre = (e.second && e.second->getType()) ? e.second->getCreature() : nullptr;
			if(!cre || hero->getSlotFor(cre->getId()).validSlot())
				continue;
			const uint64_t v = stackValue(e.second.get());
			if(v > bestVal)
			{
				bestVal = v;
				bestTown = e.first;
				bestCre = cre;
			}
		}
		if(!bestCre)
			break;
		SlotID weakHero; uint64_t weakVal = std::numeric_limits<uint64_t>::max();
		for(const auto & e : hero->Slots())
		{
			const auto * cre = (e.second && e.second->getType()) ? e.second->getCreature() : nullptr;
			if(!cre || cre->getId() == bestCre->getId())
				continue;
			const uint64_t v = stackValue(e.second.get());
			if(v < weakVal)
			{
				weakVal = v;
				weakHero = e.first;
			}
		}
		if(!weakHero.validSlot() || bestVal <= weakVal)
			break;
		cb->swapCreatures(town, hero, bestTown, weakHero);
		++moved;
	}

	if(moved)
		decisionLog_->line("  collected " + std::to_string(moved)
			+ " garrison stack(s) in " + town->getNameTranslated()
			+ " into " + hero->getNameTranslated());

	// The holder is parked in the garrison slot to keep the doorway open,
	// and swapping it there folds the town's garrison stacks onto it. So
	// the pool may be riding on the holder rather than sitting in the
	// slots. Pull that too - it is the whole reason this hero walked home.
	if(const CGHeroInstance * holder = town->getGarrisonHero())
		if(holder != hero && holder->tempOwner == playerID)
			handOver(holder, hero);
}

void OmniAI::recruitInAllTowns()
{
	if(!cb)
		return;

	// Same pass as heroVisitsTown, run from the turn thread at the top of
	// the day instead of from an event callback. Nothing can move the hero
	// out from under it here, so the pack is never refused, and stock that
	// the queued path missed yesterday gets bought today.
	for(const CGTownInstance * town : cb->getTownsInfo())
	{
		if(!town)
			continue;

		const CGHeroInstance * buyer = town->getVisitingHero();
		if(!buyer)
			buyer = town->getGarrisonHero();
		if(buyer && buyer->tempOwner != playerID)
			continue;

		// Only the hero that has to fight draws the pool. Handing the
		// garrison to whoever happened to be standing there, every turn, is
		// how a holder's army grew while the field hero stayed at half the
		// board's strength. Measured on duel-v2, day 2 of one game:
		// `army 4599/9057`, nine thousand owned and four and a half on the
		// hero that has to take something, against their 10951 concentrated
		// on one. Pooled, that is a fight. Split, it is a rout, and every
		// defence and movement rule tried today was downstream of it.
		//
		// The garrison is not a dead end. townDefenseStrength counts it, so
		// the pool defends the town it sits in, and townBusinessValue counts
		// it too, so a fat garrison is exactly what pulls the field hero
		// home to collect it.
		const bool drawsThePool = buyer
			&& (buyer->id.getNum() == fieldHeroId_
				|| cb->getHeroesInfo().size() <= 1);

		if(buyer && drawsThePool)
		{
			// Anything sitting in the garrison belongs in the army of
			// whoever is standing here, where it can walk.
			collectGarrison(town, buyer);
			decisionLog_->detail("buying what " + town->getNameTranslated()
				+ " offers for " + buyer->getNameTranslated());
			recruitFromDwelling(town, buyer);
			upgradeArmy(buyer);
			continue;
		}
		if(buyer)
		{
			// The server refuses a town-as-destination recruit while any
			// hero stands in it (CGameHandler.cpp:2426), so a garrison buy
			// with a holder home was silently buying nothing: the RECRUIT
			// lines printed, the packs were refused, and the gold piled up
			// unspent. The purchase goes onto the holder instead and is
			// parked in the garrison in the same pass. This is the pooling
			// rule rather than a new way to carry: the stack is inventory
			// for the field hero, and townDefenseStrength still counts it
			// where it sits, so the strongest holder depositing is the
			// intended outcome rather than the mistake the guard in
			// holdTheFort was written against.
			decisionLog_->detail("buying what " + town->getNameTranslated()
				+ " offers through " + buyer->getNameTranslated()
				+ " into the garrison");
			recruitFromDwelling(town, buyer);
			// This pass now runs every round, so a courier standing in town
			// with a loaded Deliver task would get its cargo dumped right
			// back into the pool it collected. Keep it aboard.
			const RegTask * run = assignedTask(buyer);
			if(!run || run->kind != RegTask::Kind::Deliver)
				depositArmy(buyer, town);
			continue;
		}

		// Nobody home. Buy into the garrison anyway rather than leave the
		// week's growth in the dwelling: dwelling stock is capped by weekly
		// growth and does not accumulate, gold does, and a hero that comes
		// home to defend collects the lot on arrival. The server permits the
		// town as its own destination precisely when no hero is in it
		// (CGameHandler.cpp:2423-2427).
		// Leave the price of a hero alone. Troops in a garrison cannot take
		// a mine, cannot fight for a town they are not in, and cannot be
		// collected by a hero we can no longer afford to hire.
		decisionLog_->detail("nobody in " + town->getNameTranslated()
			+ ", buying into the garrison above the hero reserve");
		recruitFromDwelling(town, town, GameConstants::HERO_GOLD_COST);
		upgradeArmy(town);
	}
}

void OmniAI::showMarketWindow(const IMarket * market, const CGHeroInstance * visitor,
	QueryID queryID)
{
	openQueries_.fetch_add(1);
	executeAsync("marketWindow", [this, market, visitor, queryID]
	{
		// A trading post's fixed rate beats our own marketplaces until we own
		// about six of them. When the burner stops at one, sell the surplus
		// it is passing for gold rather than closing the window on the better
		// rate. Everything else closes as before.
		const auto * post = dynamic_cast<const CGMarket *>(market);
		if(post && visitor && visitor->tempOwner == playerID
			&& visitor->id.getNum() == collectorHeroId_ && cb)
		{
			int bestMarket = 0;
			for(const CGTownInstance * town : cb->getTownsInfo())
				if(town && town->tempOwner == playerID)
					bestMarket = std::max(bestMarket, town->getMarketEfficiency());
			if(post->getMarketEfficiency() > bestMarket)
			{
				// Keep double what "enough for now" is, sell the rest.
				static const EGameResID SURPLUS_RES[] = {
					EGameResID::WOOD, EGameResID::ORE, EGameResID::MERCURY,
					EGameResID::SULFUR, EGameResID::CRYSTAL, EGameResID::GEMS };
				static const int KEEP[] = {40, 40, 20, 20, 20, 20};
				std::vector<TradeItemSell> sell;
				std::vector<TradeItemBuy> buy;
				std::vector<ui32> amounts;
				for(size_t i = 0; i < sizeof(SURPLUS_RES) / sizeof(SURPLUS_RES[0]); ++i)
				{
					const int surplus = cb->getResourceAmount(SURPLUS_RES[i]) - KEEP[i];
					if(surplus <= 0)
						continue;
					sell.push_back(TradeItemSell(GameResID(SURPLUS_RES[i])));
					buy.push_back(TradeItemBuy(GameResID(EGameResID::GOLD)));
					amounts.push_back(ui32(surplus));
				}
				if(!sell.empty())
				{
					decisionLog_->line("  TRADE at " + post->getObjectName()
						+ ": surplus into gold");
					cb->trade(post->id, EMarketMode::RESOURCE_RESOURCE,
						sell, buy, amounts, visitor);
				}
			}
		}
		answerQuery(queryID, 0);
	});
}

void OmniAI::showUniversityWindow(const IMarket *market, const CGHeroInstance *visitor,
	QueryID queryID)
{
	openQueries_.fetch_add(1);
	executeAsync("universityWindow", [this, queryID]{ answerQuery(queryID, 0); });
}

void OmniAI::showTavernWindow(const CGObjectInstance * object, const CGHeroInstance * visitor,
	QueryID queryID)
{
	openQueries_.fetch_add(1);
	// Hire the first available hero when we can afford it - a second hero
	// doubles the per-turn decision loop. Cost check keeps the pack legal
	// when broke; the server re-validates anyway.
	const ObjectInstanceID tavernId = object ? object->id : ObjectInstanceID();
	const FactionID visitorFaction = visitor ? visitor->getFactionID() : FactionID::NONE;
	executeAsync("tavern", [this, tavernId, queryID, visitorFaction]
	{
		const CGObjectInstance * tavern = cb->getObj(tavernId, false);
		if(tavern)
		{
			const auto heroes = cb->getAvailableHeroes(tavern);
			const int gold = cb->getResourceAmount(EGameResID::GOLD);
			if(heroes.empty())
				decisionLog_->detail("tavern had no heroes on offer");
			else if(gold < GameConstants::HERO_GOLD_COST)
				decisionLog_->detail(
					"tavern: cannot afford a hero, " + std::to_string(gold) + " gold");
			else
			{
				const CGHeroInstance * pick = bestHireOffer(heroes, visitorFaction);
				decisionLog_->line("  hiring a second hero at the tavern: "
					+ pick->getNameTranslated() + " with army "
					+ std::to_string(uint64_t(pick->getArmyStrength())));
				cb->recruitHero(tavern, pick);
			}
		}
		answerQuery(queryID, 0);
	});
}

void OmniAI::heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2,
	QueryID queryID)
{
	openQueries_.fetch_add(1);
	executeAsync("heroExchange", [this, hero1, hero2, queryID]
	{
		// Two of our heroes are standing together. Until now this answered
		// "dismiss" and nothing moved between them, so a support hero could
		// walk a week of recruits across the map, meet the field hero, and
		// hand over nothing.
		const CGHeroInstance * a = cb->getHero(hero1);
		const CGHeroInstance * b = cb->getHero(hero2);
		if(a && b && a->tempOwner == playerID && b->tempOwner == playerID)
		{
			const CGHeroInstance * field =
				(a->id.getNum() == fieldHeroId_) ? a
				: (b->id.getNum() == fieldHeroId_) ? b : nullptr;
			const CGHeroInstance * carrier = (field == a) ? b : (field == b) ? a : nullptr;
			if(field && carrier)
				handOver(carrier, field);
			else if(!field)
			{
				// Neither is the field hero: pool onto the stronger. Two
				// couriers held a seal apart for eleven days in R7l3c Twins
				// r01 (4641 and 2317 against a 3762 guard) while the field
				// hero sat outside with 3828; a meeting used to move nothing.
				const bool aStronger = a->getArmyStrength() >= b->getArmyStrength();
				handOver(aStronger ? b : a, aStronger ? a : b);
			}
		}
		answerQuery(queryID, 0);
	});
}

void OmniAI::handOver(const CGHeroInstance * from, const CGHeroInstance * to)
{
	if(!cb || !from || !to || from == to)
		return;

	auto stackValue = [](const CStackInstance * s) -> uint64_t
	{
		const auto * c = (s && s->getType()) ? s->getCreature() : nullptr;
		return c ? uint64_t(s->getCount()) * uint64_t(std::max(0, c->getAIValue())) : 0;
	};

	// Every move here is one stack. This used cb->bulkMoveArmy, which moves
	// the source's WHOLE army wherever a stack fits; its slot argument only
	// names the stack that keeps one creature when everything would leave
	// (CGameHandler::bulkMoveArmy). As a one-slot move it ping-ponged the two
	// armies: the swap loop ran all 16 rounds, an even number, so nothing
	// ended up moving. 45 of the 66 hand-overs in R6z3c's first 72x72 game
	// did that; the field hero sat at 20313 for eleven days while a courier
	// carrying 11989 met it every morning in its only corridor home.
	const ObjectInstanceID fromId = from->id;
	const ObjectInstanceID toId = to->id;
	const uint64_t armyBefore = to->getArmyStrength();
	int moved = 0;

	// The carrier keeps its fastest stack for the road home: movement is
	// capped by the slowest creature carried. A carrier with one stack hands
	// it over less one creature.
	SlotID keep;
	int keepSpeed = -1;
	std::vector<SlotID> carried;
	for(const auto & e : from->Slots())
	{
		if(!e.second || !e.second->getCreature())
			continue;
		carried.push_back(e.first);
		const int speed = e.second->getCreature()->getBaseSpeed();
		if(speed > keepSpeed)
		{
			keepSpeed = speed;
			keep = e.first;
		}
	}
	if(carried.empty())
		return;
	const bool lone = carried.size() == 1;
	for(const SlotID & slot : carried)
	{
		if(!lone && slot == keep)
			continue;
		from = cb->getHero(fromId);
		to = cb->getHero(toId);
		if(!from || !to)
			return;
		if(moveOneStack(from, to, slot, lone))
			++moved;
	}
	// Into a fighter, the kept stack goes as well, less one creature (R8b,
	// September 25th). A hero's speed is its slowest creature's, so one
	// creature of the fastest stack moves exactly as fast as the whole
	// stack, and the rest belongs on the hero that fights. On day 2 of
	// R7t_duel our field hero held two thirds of our army (medians 7660 and
	// 7430) while the other heroes carried 2300-2800 and the garrison about
	// 1000; Nullkiller's best hero held 96% of its army, and a total equal
	// to ours became a main stack 1.3-1.6 times our field hero's at our gate.
	const bool toFighter = toId.getNum() == fieldHeroId_ || toId.getNum() == secondFieldId_;
	if(!lone && toFighter && keep.validSlot())
	{
		from = cb->getHero(fromId);
		to = cb->getHero(toId);
		if(from && to && moveOneStack(from, to, keep, true))
			++moved;
	}

	// The leak: a carrier stack that does not fit (the field hero's seven
	// slots are full of other types) used to ride home again, so the pool
	// never fully moved. Swap instead - if the carrier's stack is worth more
	// than the field hero's weakest, the two trade slots. Each swap raises
	// the field hero's total, so the loop ends with it holding its
	// strongest seven.
	for(int guard = 0; guard < 7; ++guard)
	{
		from = cb->getHero(fromId);
		to = cb->getHero(toId);
		if(!from || !to)
			return;
		SlotID bestFrom; uint64_t bestVal = 0; const CCreature * bestCre = nullptr;
		for(const auto & e : from->Slots())
		{
			if(!e.second || !e.second->getType())
				continue;
			const auto * cre = e.second->getCreature();
			if(!cre || to->getSlotFor(cre->getId()).validSlot())
				continue;   // already fits - not a swap candidate
			const uint64_t v = stackValue(e.second.get());
			if(v > bestVal)
			{
				bestVal = v;
				bestFrom = e.first;
				bestCre = cre;
			}
		}
		if(!bestCre)
			break;

		// The field hero's weakest stack of a different type, which is the
		// slot the better stack would take.
		SlotID weakTo; uint64_t weakVal = std::numeric_limits<uint64_t>::max();
		const CCreature * weakCre = nullptr;
		for(const auto & e : to->Slots())
		{
			if(!e.second || !e.second->getType())
				continue;
			const auto * cre = e.second->getCreature();
			if(!cre || cre->getId() == bestCre->getId())
				continue;
			const uint64_t v = stackValue(e.second.get());
			if(v < weakVal)
			{
				weakVal = v;
				weakTo = e.first;
				weakCre = cre;
			}
		}
		if(!weakCre || !weakTo.validSlot() || bestVal <= weakVal)
			break;
		cb->swapCreatures(from, to, bestFrom, weakTo);
		++moved;
	}

	const CGHeroInstance * f = cb->getHero(fromId);
	const CGHeroInstance * t = cb->getHero(toId);

	if(moved && f && t)
		decisionLog_->line("  SUPPLY " + std::to_string(moved) + " stack(s) from "
			+ f->getNameTranslated() + " to " + t->getNameTranslated() + ", army "
			+ std::to_string(armyBefore) + " -> " + std::to_string(uint64_t(t->getArmyStrength())));

	// Artifacts go to the hero that fights. TRUTH, September 24th: our best
	// hero sat at attack 1-2 all game while Nullkiller's main carried +8-10
	// attack from artifacts by day 7 (Orrin a11 at level 1) - a 1.3-1.5x
	// multiplier on every creature it owns. The engine equips each one into
	// a free slot on the receiver, else its backpack, and skips anything
	// that cannot be removed (CGameHandler::bulkMoveArtifacts).
	if(f && t && (t->id.getNum() == fieldHeroId_ || t->id.getNum() == secondFieldId_)
		&& carriesArtifacts(f))
	{
		cb->bulkMoveArtifacts(f->id, t->id, false, true, true);
		decisionLog_->line("  ARTIFACTS from " + f->getNameTranslated() + " to "
			+ t->getNameTranslated());
	}
}

bool OmniAI::wouldHandOver(const CGHeroInstance * from, const CGHeroInstance * to) const
{
	if(!from || !to || from == to)
		return false;
	auto stackValue = [](const CStackInstance * s) -> uint64_t
	{
		const auto * c = (s && s->getType()) ? s->getCreature() : nullptr;
		return c ? uint64_t(s->getCount()) * uint64_t(std::max(0, c->getAIValue())) : 0;
	};
	SlotID keep;
	int keepSpeed = -1, stacks = 0;
	for(const auto & e : from->Slots())
	{
		if(!e.second || !e.second->getCreature())
			continue;
		++stacks;
		if(e.second->getCreature()->getBaseSpeed() > keepSpeed)
		{
			keepSpeed = e.second->getCreature()->getBaseSpeed();
			keep = e.first;
		}
	}
	for(const auto & e : from->Slots())
	{
		if(!e.second || !e.second->getCreature())
			continue;
		const CCreature * cre = e.second->getCreature();
		if(to->getSlotFor(cre->getId()).validSlot())
		{
			if(stacks > 1 ? e.first != keep : e.second->getCount() > 1)
				return true;
			continue;
		}
		const uint64_t v = stackValue(e.second.get());
		for(const auto & f : to->Slots())
			if(f.second && f.second->getCreature()
				&& f.second->getCreature()->getId() != cre->getId()
				&& stackValue(f.second.get()) < v)
				return true;
	}
	return false;
}

bool OmniAI::moveOneStack(const CArmedInstance * src, const CArmedInstance * dst,
	SlotID slot, bool keepOne)
{
	if(!cb || !src || !dst || src == dst || !slot.validSlot())
		return false;
	const auto & slots = src->Slots();
	const auto it = slots.find(slot);
	if(it == slots.end() || !it->second || !it->second->getCreature())
		return false;
	const CreatureID creature = it->second->getCreature()->getId();
	const int count = it->second->getCount();
	const SlotID target = dst->getSlotFor(creature);   // same type, else first free
	if(!target.validSlot())
		return false;
	if(keepOne)
	{
		if(count <= 1)
			return false;
		// splitStack takes the count the target slot ends up holding.
		const int have = dst->slotEmpty(target) ? 0 : dst->getStackCount(target);
		cb->splitStack(src, dst, slot, target, have + count - 1);
	}
	else if(dst->slotEmpty(target))
		cb->swapCreatures(src, dst, slot, target);   // into the empty slot
	else
		cb->mergeStacks(src, dst, slot, target);
	return true;
}

bool OmniAI::carriesArtifacts(const CGHeroInstance * hero) const
{
	if(!hero)
		return false;
	if(!hero->artifactsInBackpack.empty())
		return true;
	for(const auto & worn : hero->artifactsWorn)
	{
		const auto * art = worn.second.getArt();
		// The spellbook and the war machines ride with every hero and move
		// with nobody; they are not what a courier trip is for.
		if(art && ArtifactUtils::isArtRemovable(worn)
			&& worn.first != ArtifactPosition::SPELLBOOK
			&& worn.first != ArtifactPosition::MACH1 && worn.first != ArtifactPosition::MACH2
			&& worn.first != ArtifactPosition::MACH3 && worn.first != ArtifactPosition::MACH4)
			return true;
	}
	return false;
}

std::optional<BattleAction> OmniAI::makeSurrenderRetreatDecision(const BattleID & battleID,
	const BattleStateInfoForRetreat & battleState)
{
	// Nullkiller (AIGateway::makeSurrenderRetreatDecision, the AI this is
	// measured against) has real logic behind this hook. Ours has always
	// returned nullopt, so every fight that goes worse than the pre-battle
	// estimate got fought to the last stack instead of cut - a fight that
	// looked fine walking in but ran bad (an unlucky roll, a spell, a
	// misjudged guard) had no way out. ATTRITION is the largest loss class
	// measured; this is the one mid-battle safety net that class was
	// missing entirely.
	//
	// Deliberately conservative for this first pass: isLastTurnBeforeDie is
	// the engine's own projection that this side does not survive the
	// opponent's next round, not a ratio this AI is guessing at, so it
	// cannot misfire on a fight that is merely progressing normally (both
	// sides always look worse mid-battle than they did pre-battle, which is
	// exactly why a raw strength-ratio trigger risks bailing on fights that
	// were actually being won). Losing an empty-handed retreat still beats
	// losing the whole stack for nothing, and it saves the hero itself -
	// its experience, artifacts and skills - which a wipe does not. A wider
	// "cut it early" trigger is a plausible follow-up but needs match
	// evidence to calibrate without costing winnable fights.
	if(!battleState.canFlee)
		return std::nullopt;
	if(battleState.ourHero && battleState.ourHero->patrol.patrolling)
		return std::nullopt;   // nowhere to flee to; same exemption Nullkiller carries
	if(!battleState.isLastTurnBeforeDie)
		return std::nullopt;

	std::ostringstream o;
	o << "   RETREAT mid-battle: about to be wiped, our strength "
	  << battleState.getOurStrength() << " vs enemy " << battleState.getEnemyStrength();
	decisionLog_->line(o.str());
	return BattleAction::makeRetreat(battleState.ourSide);
}

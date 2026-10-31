// funk-midi as a CLAP plugin: funk's patterns (funk's docs/dev/patterns.md) behind
// projects/clap/plugin.cpp.
//
// `pat = <a Pattern Int>` loops in Live mode; an expression of type Pattern Int plays once,
// for one cycle, and the evaluation returns once its notes are queued. As a clip, either
// renders one cycle, which the clip repeats. A cycle is 2000 script ms: 4 beats, a bar of
// 4/4. Values are note + 128 * velocity + 16384 * (channel - 1), as funk's lib/midi.fk
// makes them; velocity 0 plays as 100.
//
// `sig = <a g.Sig>` plays on the plugin's stereo output from the next block, as in
// funk-midi --audio; `voices f` is d.poly of kVoices voices, which every note the plugin
// sends plays too (funk_voices, on the audio thread). The kernels are lib/kernels.fk,
// compiled into funkdsp.so, which the build puts in the bundle's Resources (beside the
// .clap on Linux). graph.fk's imports use glue.c, linked in (funk_glue), so no C compiler runs.
//
// Each instance has its own funk session (libfunk.h), on funk's own thread, with its own
// player and voices. Every funk call is queued to that thread from the instance's evaluator
// thread, which sends the MIDI, or from the host's main thread at activation.

#include "language.hpp"
#ifdef FUNK_HIGHLIGHT
#include "highlight.hpp"
#endif

#include "funk_lib.h" // kFunkLib: funk's lib modules, as {file name, source}

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <dlfcn.h>
#include <libgen.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "graph.h"
#include "libfunk.h"
#include "midi_clock.h"
extern const funk_glue_entry funk_glue_table[]; // glue.c, from glue.fk by funk emit-c --glue
extern const int funk_glue_count;
}

namespace {

const double kCycleMs = 2000.;
const double kWindowMs = 10.;
const int kCap = 4096; // events per window, as in kStep
const size_t kHeapLimit = (size_t)512 << 20, kStackSize = (size_t)256 << 20;
const char* const kStep = "step buf a b = p.writeEvents (plusPtr nullPtr buf) 4096 pat 2000000 a b";
const char* const kStepOnce = "stepOnce buf a b = p.writeEvents (plusPtr nullPtr buf) 4096 once 2000000 a b";

const int kVoices = 8;
const uint32_t kChannels = 2;

const char* const kDefaultCode =
	"-- Ctrl+Return (Cmd+Return on macOS) evaluates the paragraph around the caret, Ctrl+. stops.\n"
	"-- To hear it, evaluate `sig` first, the instrument, then a pattern, its notes.\n"
	"-- With Live on, `pat` loops; an expression of type Pattern Int plays once.\n"
	"-- A cycle is one bar. Values: note + 128 * velocity + 16384 * (channel - 1).\n"
	"-- `sig` plays on the plugin's output; `voices f` plays each note it sends.\n"
	"\n"
	"sig = voices (\\gate freq -> d.adsr 0.01 0.2 0.5 0.3 gate\n"
	"                            * d.saw freq * 0.2)\n"
	"\n"
	"pat = p.stack [p.fastcat [p.atom 48, p.silence, p.atom 55, p.silence],\n"
	"               m.vel 70 (p.euclid 3 8 72)]\n"
	"\n"
	"p.fastcat [p.atom 60, p.atom 64, p.atom 67, p.atom 72]\n";

// kFunkLib's directory, named by a hash of its files and kept across loads: graph.fk's
// glue is cached by its compiler arguments, which name this directory, so a fresh
// directory per load compiled the glue again on every load. "" if it cannot be made. Its
// files are written once per process: instances would race on one temporary name.
std::string LibDir()
{
	static std::once_flag once;
	static std::string dir;
	std::call_once(once, [] {
		uint64_t h = 14695981039346656037u; // FNV-1a
		for (const auto& m : kFunkLib)
			for (const char* p : {m.name, "", m.source, ""})
				for (const char* c = p;; c++) {
					h = (h ^ (unsigned char)*c) * 1099511628211u;
					if (!*c) break;
				}
		const char *x = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
		const std::string base = x && *x ? x : std::string(home && *home ? home : "/tmp") + "/.cache";
		char name[32];
		snprintf(name, sizeof name, "%016llx", (unsigned long long)h);
		const std::string d = base + "/funk/plugin/" + name;
		for (const std::string& part : {base, base + "/funk", base + "/funk/plugin", d})
			if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return;
		for (const auto& m : kFunkLib) {
			const std::string path = d + "/" + m.name;
			if (access(path.c_str(), F_OK) == 0) continue; // same content: the directory is named by it
			const std::string tmp = path + "." + std::to_string(getpid());
			FILE* f = fopen(tmp.c_str(), "w");
			if (!f) return;
			const bool written = fputs(m.source, f) >= 0;
			if (fclose(f) != 0 || !written || rename(tmp.c_str(), path.c_str()) != 0) {
				unlink(tmp.c_str());
				return;
			}
		}
		dir = d;
	});
	return dir;
}

class Session {
public:
	Session()
	{
		dir_ = LibDir();
		if (dir_.empty()) return;
		Start start{this, {}};
		std::future<bool> ok = start.started.get_future();
		thread_ = std::thread([this, &start] {
			// A runaway pattern meets a limit, as a runtime error, before it takes the host's memory.
			// graph.h, for glue compiled if the linked glue does not match.
			cc_args_[0] = "-I";
			cc_args_[1] = dir_.c_str();
			const funk_config config{kHeapLimit, 0, kStackSize, cc_args_, 2};
			funk_ = funk_new(&config);
			if (!funk_) {
				start.started.set_value(false);
				return;
			}
			funk_run(funk_, &Session::Main, &start);
			funk_free(funk_);
		});
		ok_ = ok.get();
	}

	~Session()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			quit_ = true;
		}
		cv_.notify_one();
		if (thread_.joinable()) thread_.join();
	}

	bool ok() const { return ok_; }
	funk* handle() const { return funk_; }
	bool graphs() const { return graphs_; } // graph.fk's imports compiled: audio is possible

	// Runs fn on funk's thread, with what funk writes meanwhile appended to out. A Stop
	// (midi_clock_cancelled, on the evaluator thread) interrupts it; returns whether one did.
	// Other threads pass stoppable false: the clock belongs to the evaluator thread.
	bool call(const std::function<void()>& fn, std::string* out, bool stoppable = true)
	{
		std::packaged_task<void()> task([this, &fn, out] {
			out_ = out;
			fn();
			out_ = nullptr;
		});
		std::future<void> done = task.get_future();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			jobs_.push_back(std::move(task));
		}
		cv_.notify_one();
		bool interrupted = false;
		while (done.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
			if (stoppable && midi_clock_cancelled()) {
				funk_interrupt(funk_); // again on each poll: a call that starts afterwards clears it
				interrupted = true;
			}
		}
		return interrupted;
	}

	// On funk's thread: evaluates src, its value appended to out.
	bool eval(const std::string& src, std::string& out)
	{
		char* shown = nullptr;
		const bool ok = funk_eval(funk_, src.c_str(), "<eval>", &shown);
		if (shown) out += std::string(shown) + "\n";
		free(shown);
		return ok;
	}

private:
	struct Start {
		Session* session;
		std::promise<bool> started;
	};

	static int Main(funk* f, void* arg)
	{
		auto* start = static_cast<Start*>(arg);
		Session& s = *start->session;
		std::promise<bool>* started = &start->started;
		char* shown = nullptr;
		// graph.fk's imports: the plugin's own functions, which it does not export
#define PROVIDE(fn) funk_provide(f, #fn, (void*)fn);
		PROVIDE(funk_graph_new) PROVIDE(funk_graph_add) PROVIDE(funk_graph_control) PROVIDE(funk_graph_name)
		PROVIDE(funk_graph_output) PROVIDE(funk_graph_memory) PROVIDE(funk_player_new) PROVIDE(funk_player_send) PROVIDE(funk_player_frames)
		PROVIDE(funk_player_play) PROVIDE(funk_player_render_channels) PROVIDE(funk_player_free)
#undef PROVIDE
		funk_glue(f, funk_glue_table, funk_glue_count); // graph.fk's imports: no C compiler needed
		funk_module_dir(f, s.dir_.c_str());
		funk_output(f, &Session::Output, &s);
		bool ok = funk_eval(f, "import pattern as p", "<setup>", &shown) && funk_eval(f, "import midi as m", "<setup>", &shown);
		// If the linked glue does not match and no C compiler runs, graph.fk's imports fail:
		// MIDI still plays, the output is silent.
		s.graphs_ = ok && funk_eval(f, "import graph as g", "<setup>", &shown) && funk_eval(f, "import dsp as d", "<setup>", &shown);
		if (ok && !s.graphs_) fputs("funk-midi: no audio: graph.fk's foreign imports did not compile\n", stderr);
		started->set_value(ok);
		if (!ok) return 1;
		for (;;) {
			std::packaged_task<void()> task;
			{
				std::unique_lock<std::mutex> lock(s.mutex_);
				s.cv_.wait(lock, [&s] { return s.quit_ || !s.jobs_.empty(); });
				if (s.jobs_.empty()) return 0;
				task = std::move(s.jobs_.front());
				s.jobs_.pop_front();
			}
			task();
		}
	}

	static void Output(void* ctx, int stream, const char* text, size_t n)
	{
		(void)stream;
		auto* s = static_cast<Session*>(ctx);
		if (s->out_) s->out_->append(text, n);
		else fwrite(text, 1, n, stderr);
	}

	std::string dir_;
	const char* cc_args_[2] = {};
	funk* funk_ = nullptr;
	bool ok_ = false, graphs_ = false;
	std::string* out_ = nullptr; // funk's thread only
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<std::packaged_task<void()>> jobs_;
	bool quit_ = false;
	std::thread thread_;
};

// A paragraph line that defines name, of three letters.
bool Defines(const std::string& code, const char* name)
{
	for (size_t at = 0; at < code.size(); at = code.find('\n', at) + 1) {
		if (code.compare(at, 3, name) == 0 && at + 3 < code.size() && strchr(" =\t", code[at + 3])) return true;
		if (code.find('\n', at) == std::string::npos) break;
	}
	return false;
}

// --- audio ------------------------------------------------------------------

// funkdsp.so: in the bundle's Resources on macOS, beside the .clap elsewhere.
std::string KernelsPath()
{
	Dl_info info;
	if (!dladdr((void*)&kLanguage, &info) || !info.dli_fname) return "funkdsp.so";
	std::string path = info.dli_fname;
	std::string dir = dirname(&path[0]);
#ifdef __APPLE__
	return dir + "/../Resources/funkdsp.so";
#else
	return dir + "/funkdsp.so";
#endif
}

// code as the body of `once`, indented, so that its lines continue the definition
std::string AsOnce(const std::string& code)
{
	std::string s = "once = (\n  ";
	for (char c : code) {
		s += c;
		if (c == '\n') s += "  ";
	}
	return s + "\n  ) :: p.Pattern Int";
}

class FunkLanguage : public Language {
public:
	~FunkLanguage() override
	{
		funk_voices_free(audio_.voices);
		funk_player_free(audio_.player); // the host deactivated: the audio thread is done with it
		if (audio_.kernels) dlclose(audio_.kernels);
	}

	bool ok() const { return session_.ok(); }

	void setLive(bool live) override { live_ = live; }

	void eval(const std::string& code, std::string& out) override
	{
		Session& s = session_;
		// An expression of type Pattern Int becomes `once`; anything else fails here quietly.
		std::string ignored;
		bool once = false;
		if (s.call([&] { once = s.eval(AsOnce(code), ignored) && s.eval(kStepOnce, ignored); }, &ignored)) {
			out += "stopped\n"; // a pattern whose evaluation does not end
			return;
		}
		bool pat = false;
		if (!once) {
			const bool interrupted = s.call([&] {
				if (!s.eval(code, out)) return;
				if (Defines(code, "sig")) playSig(out);
				if (!Defines(code, "pat")) return;
				// Compiled again, so that it names the new pat; as a clip, pat plays once.
				pat = s.eval(kStep, out) && (live_ || (s.eval("once = pat", out) && s.eval(kStepOnce, out)));
				if (!pat) out += "funk-midi: pat is not playing: it must be a Pattern Int\n";
			}, &out);
			if (interrupted) out += "stopped\n";
		}
		// Compiling may outlast the lookahead: the notes start where they still play on time.
		midi_clock_catch_up();
		const double start = midi_clock_ms();
		if (pat && live_) {
			if (!looping_) grid_ = start;
			looping_ = true;
		} else if (once || pat) {
			// Its cycle is queued now. While pat loops, messages before grid_ are sent: it starts there at the earliest.
			const double from = looping_ ? std::max(start, grid_) : start;
			double last = from;
			for (double a = 0.; a < kCycleMs && !midi_clock_cancelled(); a += kWindowMs) {
				if (!query("stepOnce", a, a + kWindowMs, from, &last, out)) break;
			}
			// Live, the evaluation returns once these are sent, as other languages' do.
			double t;
			while (live_ && !midi_clock_cancelled() && !pending_.empty() && pending_.top().ms <= last && nextWake(&t)) {
				midi_clock_wait_until(t);
				if (midi_clock_cancelled()) break;
				resume(t, out);
			}
		}
	}

	// While pat loops, its next window; else the next message.
	bool nextWake(double* ms) override
	{
		if (looping_) *ms = grid_;
		else if (!pending_.empty()) *ms = pending_.top().ms;
		else return false;
		return true;
	}

	void resume(double ms, std::string& out) override
	{
		if (looping_ && grid_ <= ms) {
			if (!query("step", grid_, grid_ + kWindowMs, 0., nullptr, out)) looping_ = false;
			grid_ += kWindowMs;
		}
		// In time order: while pat loops, only what precedes its next window.
		while (!pending_.empty() && (looping_ ? pending_.top().ms < grid_ : pending_.top().ms <= ms)) {
			midi_clock_set_ms(pending_.top().ms);
			midi_out_send(nullptr, pending_.top().msg, 3);
			pending_.pop();
		}
	}

	void stopVoices() override
	{
		looping_ = false;
		pending_ = {};
	}

	// False only if funk did not start; without audio, MIDI still plays and the output is silent.
	bool audioActivate(double rate, uint32_t maxFrames) override
	{
		if (!session_.ok()) return false;
		if (!session_.graphs()) return true;
		if (!audio_.kernels) {
			const std::string path = KernelsPath();
			audio_.kernels = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
			if (!audio_.kernels) {
				fprintf(stderr, "funk-midi: no audio: cannot load the kernels: %s\n", dlerror());
				return true;
			}
		}
		if (!audio_.player) {
			audio_.player = funk_player_new();
			audio_.voices = audio_.player ? funk_voices_new(audio_.player, kVoices, true) : nullptr;
			if (!audio_.voices) return false;
		}
		// A new rate needs graphs built for it: audioPlayer is defined again, and sig played.
		char src[256];
		snprintf(src, sizeof src, "audioPlayer = g.Player (plusPtr nullPtr %ld) [plusPtr nullPtr %ld] %.17g %u",
		         (long)audio_.player, (long)audio_.kernels, rate, maxFrames);
		bool ok = false;
		std::string out;
		session_.call([&] {
			char voices[256];
			snprintf(voices, sizeof voices,
			         "voices :: (g.Sig -> g.Sig -> g.Sig) -> g.Sig\nvoices f = d.poly %d f\n"
			         "voicesOn :: Int -> (g.Sig -> g.Sig -> g.Sig) -> g.Sig\nvoicesOn c f = d.polyOn c %d f",
			         kVoices, kVoices);
			ok = session_.eval(src, out) && session_.eval(voices, out);
			audio_.ready = ok;
			if (ok && audio_.haveSig) playSig(out);
		}, &out, false);
		if (!out.empty()) fputs(out.c_str(), stderr);
		return ok;
	}

	// The audio thread: a note at its frame, on its channel's voices or the shared ones.
	void audioNote(uint32_t offset, const uint8_t* data) override
	{
		if (!audio_.voices) return;
		const int64_t frame = funk_player_frames(audio_.player) + offset;
		if ((data[0] & 0xF0) == 0x90 && data[2]) funk_voices_on(audio_.voices, frame, INT64_MAX, data[0] & 15, data[1], data[2]);
		else funk_voices_off(audio_.voices, frame, data[0] & 15, data[1]);
	}

	void audioRender(float* const* out, uint32_t channels, uint32_t frames) override
	{
		if (audio_.player) funk_player_render_channels(audio_.player, out, (int)channels, (int)frames);
		else Language::audioRender(out, channels, frames);
	}

private:
	// On funk's thread: compiled again after sig changes, so it names the new sig.
	void playSig(std::string& out)
	{
		audio_.haveSig = true;
		if (!audio_.ready) return;
		long arg = 0, r = 0;
		if (!session_.eval("playsig :: Int ->> Int\nplaysig u = do { g.play audioPlayer sig ; 0 }", out) ||
		    !funk_call(session_.handle(), "playsig", 1, &arg, &r))
			out += "funk-midi: sig is not playing: it must be a g.Sig of funkdsp.so's kernels\n";
	}

	struct Msg {
		double ms;
		uint64_t seq;
		unsigned char msg[3];
		// later first out of the heap; at one time, note-offs first, so a note that ends
		// where the next begins is released first
		bool operator>(const Msg& o) const
		{
			if (ms != o.ms) return ms > o.ms;
			const bool on = (msg[0] & 0xf0) == 0x90, oon = (o.msg[0] & 0xf0) == 0x90;
			if (on != oon) return on;
			return seq > o.seq;
		}
	};

	// Queues the notes of step (or stepOnce) for [a, b) of its own ms, at origin + time.
	// last, if given, gets the latest message time. False if the pattern failed, which out reports.
	bool query(const char* step, double a, double b, double origin, double* last, std::string& out)
	{
		long args[3] = {(long)buf_, std::lround(a * 1000), std::lround(b * 1000)}, n = 0;
		bool ok = false;
		session_.call([&] { ok = funk_call(session_.handle(), step, 3, args, &n); }, &out);
		if (!ok) {
			out += "funk-midi: the pattern failed; it stops until it is evaluated again\n";
			return false;
		}
		if (n > kCap) out += "funk-midi: " + std::to_string(n) + " events in one window; played " + std::to_string(kCap) + "\n";
		// A clip's note-offs end 1 ms before its bar: at the bar they would grow the clip by one,
		// and within half a frame of it the clip player drops them as it loops.
		const double end = origin + kCycleMs - 1.;
		for (long i = 0; i < n && i < kCap; i++) {
			const double at = origin + buf_[3 * i] / 1000.;
			double off = at + std::max(buf_[3 * i + 1] / 1000., 0.001);
			if (!live_) off = std::min(off, end);
			const long v = buf_[3 * i + 2];
			const unsigned char key = v & 127, vel = (v >> 7) & 127, ch = (v >> 14) & 15;
			pending_.push({at, seq_++, {(unsigned char)(0x90 | ch), key, vel ? vel : (unsigned char)100}});
			pending_.push({off, seq_++, {(unsigned char)(0x80 | ch), key, 0}});
			if (last) *last = std::max(*last, off);
		}
		return true;
	}

	Session session_;
	// The player, built at the first activation; voices and player are the audio thread's
	// from then on. The rest is funk's thread's.
	struct {
		funk_player* player = nullptr;
		funk_voices* voices = nullptr;
		void* kernels = nullptr;
		bool ready = false;   // audioPlayer and playsig are defined
		bool haveSig = false; // sig is defined, and plays once ready
	} audio_;
	bool live_ = false, looping_ = false;
	double grid_ = 0.; // pat's next window, in script ms
	uint64_t seq_ = 0;
	std::priority_queue<Msg, std::vector<Msg>, std::greater<Msg>> pending_;
	long buf_[3 * kCap];
};

} // namespace

const LanguageInfo kLanguage = {
	"com.github.shakfu.midi-langs.funk-midi",
	"funk-midi",
	"funk patterns, live",
	"MidiLangsFunkMidiEditor",
	"FUNK_MIDI_CLAP_UDP_PORT",
	57170,
	{EvalSyntax::kParagraphs, "--", "\"'"},
	kDefaultCode,
	[]() -> std::unique_ptr<Language> {
		auto lang = std::make_unique<FunkLanguage>();
		if (!lang->ok()) return nullptr;
		return lang;
	},
	kChannels,
#ifdef FUNK_HIGHLIGHT
	FunkHighlight,
#else
	nullptr,
#endif
};

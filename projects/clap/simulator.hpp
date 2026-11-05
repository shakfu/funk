#pragma once

// Simulator mode: a thread that evaluates steps from a script file in turn, as a performer
// would, and logs each step, the plugin's output and the audio thread's block timings to a file.
// Enabled per process by environment variables named from LanguageInfo::simEnv; see
// docs/dev/simulator.md.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

struct SimConfig {
	std::string script;     // <P>: the steps file
	std::string log;        // <P>_LOG, default <script>.log
	bool random = false;    // <P>_ORDER: rotate (default) or random
	double interval = 16.;  // <P>_INTERVAL: seconds from one step's start to the next
	uint64_t seed = 0;      // <P>_SEED, default from the clock
	std::string lineComment; // the language's, from EvalSyntax
};

// False if <prefix> is unset or empty. Unparsable values keep the defaults; error says which.
bool SimConfigFromEnv(const char* prefix, SimConfig* config, std::string* error);

struct SimStep {
	std::string name;
	std::vector<std::string> paragraphs;
};

// A script: paragraphs before the first "--- name" line are the preamble, evaluated before the
// first step and again whenever it changes; each "--- name" line starts a step. Paragraphs are
// separated by blank lines and evaluated one at a time, as the editor's Ctrl+Return would.
// Paragraphs of only lineComment lines are skipped.
struct SimScript {
	std::vector<std::string> preamble;
	std::vector<SimStep> steps;
};

SimScript ParseSimScript(const std::string& text, const char* lineComment);

// Step indices: in turn from start, or uniformly at random with no step twice in a row.
class SimOrder {
public:
	SimOrder(bool random, uint64_t seed, size_t start) : random_(random), rng_(seed), next_(start) {}
	size_t next(size_t count);

private:
	bool random_;
	std::mt19937_64 rng_;
	size_t next_;
	size_t last_ = SIZE_MAX;
};

// The audio thread's block timings: one writer, readers on other threads.
struct BlockStats {
	std::atomic<uint64_t> blocks{0};
	std::atomic<uint64_t> over{0};    // blocks whose process() took longer than the block lasts
	std::atomic<uint64_t> half{0};    // longer than half of it
	std::atomic<uint64_t> maxNs{0};   // the longest process(), since the reader last reset it
	std::atomic<uint64_t> budgetNs{0}; // the last block's duration
	void add(uint64_t ns, uint64_t budget);
};

class Simulator {
public:
	struct Host {
		// Queues code on the evaluator; the future is ready when it has run.
		std::function<std::future<std::string>(const std::string& code)> eval;
		std::function<bool()> live;
		BlockStats* stats;
	};

	Simulator(SimConfig config, int instance, Host host);
	~Simulator();

	// The plugin's output, which goes to the log as "out" lines.
	void output(const std::string& text);

private:
	void loop();
	bool sleep(double seconds); // false once quitting
	bool evalAll(const std::vector<std::string>& paragraphs);
	void line(const char* kind, const std::string& text);
	void audio(const char* kind, bool always);

	SimConfig config_;
	int instance_;
	Host host_;
	FILE* file_ = nullptr;
	SimOrder order_;
	std::string preamble_;
	uint64_t seenOver_ = 0, seenHalf_ = 0, seenBlocks_ = 0;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool quit_ = false;
	std::thread thread_;
};

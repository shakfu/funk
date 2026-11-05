#include "simulator.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {

// One lock for every instance's writes: instances may share a log file.
std::mutex gFileMutex;

bool Blank(const std::string& line)
{
	return line.find_first_not_of(" \t\r") == std::string::npos;
}

std::string Env(const std::string& name)
{
	const char* v = getenv(name.c_str());
	return v ? v : "";
}

std::string Timestamp()
{
	const auto now = std::chrono::system_clock::now();
	const time_t t = std::chrono::system_clock::to_time_t(now);
	const long ms = (long)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
	tm local{};
	localtime_r(&t, &local);
	char buf[40];
	const size_t n = strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &local);
	snprintf(buf + n, sizeof buf - n, ".%03ld", ms);
	return buf;
}

} // namespace

bool SimConfigFromEnv(const char* prefix, SimConfig* config, std::string* error)
{
	if (!prefix) return false;
	const std::string p = prefix;
	config->script = Env(p);
	if (config->script.empty()) return false;
	config->log = Env(p + "_LOG");
	if (config->log.empty()) config->log = config->script + ".log";
	const std::string order = Env(p + "_ORDER");
	if (order == "random") config->random = true;
	else if (!order.empty() && order != "rotate") *error += p + "_ORDER: \"" + order + "\" is not rotate or random; rotating\n";
	const std::string interval = Env(p + "_INTERVAL");
	if (!interval.empty()) {
		char* end = nullptr;
		const double s = strtod(interval.c_str(), &end);
		if (*end == '\0' && s >= 0.1) config->interval = s;
		else *error += p + "_INTERVAL: \"" + interval + "\" is not a number of seconds >= 0.1\n";
	}
	const std::string seed = Env(p + "_SEED");
	char* end = nullptr;
	config->seed = seed.empty() ? 0 : strtoull(seed.c_str(), &end, 10);
	if (seed.empty() || *end != '\0') {
		if (!seed.empty()) *error += p + "_SEED: \"" + seed + "\" is not an unsigned integer\n";
		config->seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
	}
	return true;
}

SimScript ParseSimScript(const std::string& text, const char* lineComment)
{
	SimScript script;
	std::vector<std::string>* paragraphs = &script.preamble;
	std::string paragraph;
	bool code = false; // the paragraph has a line that is not a comment
	auto flush = [&] {
		while (!paragraph.empty() && paragraph.back() == '\n') paragraph.pop_back();
		if (code) paragraphs->push_back(paragraph);
		paragraph.clear();
		code = false;
	};
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		if (line.compare(0, 3, "---") == 0 && (line.size() == 3 || line[3] == ' ')) {
			flush();
			const size_t at = line.find_first_not_of(' ', 3);
			std::string name = at == std::string::npos ? "" : line.substr(at);
			while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
			if (name.empty()) name = "step " + std::to_string(script.steps.size() + 1);
			script.steps.push_back({name, {}});
			paragraphs = &script.steps.back().paragraphs;
		} else if (Blank(line)) {
			flush();
		} else {
			const size_t at = line.find_first_not_of(" \t");
			code |= !lineComment || !*lineComment || line.compare(at, strlen(lineComment), lineComment) != 0;
			paragraph += line + "\n";
		}
	}
	flush();
	return script;
}

size_t SimOrder::next(size_t count)
{
	if (count == 0) return 0;
	size_t i;
	if (!random_) {
		i = next_ % count;
		next_ = i + 1;
	} else if (count == 1) {
		i = 0;
	} else {
		// one of the others: [0, count - 1), shifted past the last
		i = std::uniform_int_distribution<size_t>(0, count - 2)(rng_);
		if (last_ < count && i >= last_) ++i;
	}
	last_ = i;
	return i;
}

void BlockStats::add(uint64_t ns, uint64_t budget)
{
	blocks.fetch_add(1, std::memory_order_relaxed);
	if (ns > budget) over.fetch_add(1, std::memory_order_relaxed);
	if (2 * ns > budget) half.fetch_add(1, std::memory_order_relaxed);
	uint64_t m = maxNs.load(std::memory_order_relaxed);
	while (ns > m && !maxNs.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
	budgetNs.store(budget, std::memory_order_relaxed);
}

Simulator::Simulator(SimConfig config, int instance, Host host)
	: config_(std::move(config)), instance_(instance), host_(std::move(host)),
	  order_(config_.random, config_.seed + (uint64_t)instance, (size_t)(instance - 1))
{
	file_ = fopen(config_.log.c_str(), "a");
	if (!file_) {
		fprintf(stderr, "simulator: cannot open %s: %s\n", config_.log.c_str(), strerror(errno));
		return;
	}
	char start[256];
	snprintf(start, sizeof start, "script %s, order %s, interval %g s, seed %llu", config_.script.c_str(),
	         config_.random ? "random" : "rotate", config_.interval, (unsigned long long)config_.seed);
	line("start", start);
	thread_ = std::thread([this] { loop(); });
}

Simulator::~Simulator()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable()) thread_.join();
	if (!file_) return;
	audio("audio", true);
	line("end", "");
	fclose(file_);
}

void Simulator::output(const std::string& text)
{
	if (!file_ || text.empty()) return;
	std::istringstream in(text);
	std::string l;
	while (std::getline(in, l)) line("out", l);
}

void Simulator::line(const char* kind, const std::string& text)
{
	std::lock_guard<std::mutex> lock(gFileMutex);
	fprintf(file_, "%s #%d %-5s %s\n", Timestamp().c_str(), instance_, kind, text.c_str());
	fflush(file_);
}

// Block timings since the last report; logged if any block took over half its time, or always.
void Simulator::audio(const char* kind, bool always)
{
	BlockStats& s = *host_.stats;
	const uint64_t blocks = s.blocks.load(), over = s.over.load(), half = s.half.load();
	if (!always && half == seenHalf_) return;
	const uint64_t maxNs = s.maxNs.exchange(0), budget = s.budgetNs.load();
	char text[192];
	snprintf(text, sizeof text, "blocks %llu, over budget %llu, over half %llu, longest %.0f us of %.0f us",
	         (unsigned long long)(blocks - seenBlocks_), (unsigned long long)(over - seenOver_),
	         (unsigned long long)(half - seenHalf_), maxNs / 1e3, budget / 1e3);
	line(kind, text);
	seenBlocks_ = blocks, seenOver_ = over, seenHalf_ = half;
}

bool Simulator::sleep(double seconds)
{
	std::unique_lock<std::mutex> lock(mutex_);
	return !cv_.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return quit_; });
}

// Evaluates paragraphs in order, reporting slow blocks meanwhile; false once quitting.
bool Simulator::evalAll(const std::vector<std::string>& paragraphs)
{
	for (const std::string& code : paragraphs) {
		const auto started = std::chrono::steady_clock::now();
		std::future<std::string> done = host_.eval(code);
		bool warned = false;
		while (done.wait_for(std::chrono::milliseconds(250)) != std::future_status::ready) {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (quit_) return false;
			}
			audio("slow", false);
			const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			if (!warned && s > 10.) {
				line("busy", "evaluation still running after 10 s");
				warned = true;
			}
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		const std::string out = done.get();
		const bool failed = out.find("error") != std::string::npos || out.find("not playing") != std::string::npos;
		char text[64];
		snprintf(text, sizeof text, "%.1f ms%s", ms, failed ? ", failed" : "");
		line("done", text);
	}
	return true;
}

void Simulator::loop()
{
	line("wait", "for the host's first block");
	while (host_.stats->blocks.load() == 0) {
		if (!sleep(0.1)) return;
	}
	size_t count = 0;
	for (;;) {
		const auto started = std::chrono::steady_clock::now();
		std::ifstream in(config_.script);
		std::stringstream text;
		if (in.is_open()) text << in.rdbuf();
		if (!in.is_open()) {
			line("error", "cannot read " + config_.script);
		} else {
			const SimScript script = ParseSimScript(text.str(), config_.lineComment.c_str());
			std::string preamble;
			for (const std::string& p : script.preamble) preamble += p + "\n\n";
			if (preamble != preamble_) {
				preamble_ = preamble;
				line("step", "preamble (" + std::to_string(script.preamble.size()) + " paragraphs)");
				if (!evalAll(script.preamble)) return;
			}
			if (script.steps.empty()) {
				line("error", config_.script + " has no \"--- name\" steps");
			} else {
				const size_t i = order_.next(script.steps.size());
				const SimStep& step = script.steps[i];
				char text[64];
				snprintf(text, sizeof text, "%zu: step %zu of %zu, %s: ", ++count, i + 1, script.steps.size(),
				         host_.live() ? "live" : "clip");
				line("step", text + step.name);
				if (!evalAll(step.paragraphs)) return;
			}
		}
		audio("audio", true);
		// The next step starts an interval after this one started, reporting slow blocks meanwhile.
		for (;;) {
			const double left = config_.interval - std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			if (left <= 0.) break;
			if (!sleep(std::min(left, 0.25))) return;
			audio("slow", false);
		}
	}
}

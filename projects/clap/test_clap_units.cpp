// Unit tests for the CLAP framework's timeline and eval-code selection.

#include "editor_logic.hpp"
#include "simulator.hpp"
#include "timeline.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void check(bool ok, const std::string& what)
{
	if (!ok) {
		printf("FAIL: %s\n", what.c_str());
		++gFailures;
	}
}

void checkEq(const std::string& got, const std::string& want, const std::string& what)
{
	check(got == want, what + ": got \"" + got + "\", want \"" + want + "\"");
}

// --- SelectEvalCode -----------------------------------------------------

const EvalSyntax kLisp = {EvalSyntax::kBrackets, ";", "\""};
const EvalSyntax kLua = {EvalSyntax::kParagraphs, "--", "\"'"};

std::string At(const std::string& text, int caret, const EvalSyntax& syntax)
{
	return SelectEvalCode(text, caret, caret, caret, syntax);
}

void testSelection()
{
	checkEq(SelectEvalCode("1 2 +\n3 4 *\n", 2, 9, 9, kLisp), "2 +\n3 4", "selection wins");
	checkEq(SelectEvalCode("1 2 +\n3 4 *\n", 9, 2, 2, kLua), "2 +\n3 4", "reversed selection");
	checkEq(SelectEvalCode("1 2 +\n", 5, 6, 6, kLisp), "", "selection of only a newline");
	checkEq(SelectEvalCode("1 2 +", -5, 99, 0, kLisp), "1 2 +", "out-of-range offsets clamp");
}

void testBrackets()
{
	const std::string lines = "1 2 +\n3 4 *\n5 6 -";
	checkEq(At(lines, 0, kLisp), "1 2 +", "caret line");
	checkEq(At(lines, 11, kLisp), "3 4 *", "caret at end of line");
	checkEq(At(lines, 17, kLisp), "5 6 -", "caret at end of text");
	checkEq(At("a\n   \nb", 4, kLisp), "a", "blank line uses the line above");
	checkEq(At("\n\n", 1, kLisp), "", "blank code is empty");

	const std::string block = "(define (f)\n  (note c4))\n";
	for (int caret : {0, 14, (int)block.size()}) {
		checkEq(At(block, caret, kLisp), block.substr(0, block.size() - 1), "multi-line group, caret " + std::to_string(caret));
	}
	checkEq(At("1 (2\n3) +\n4", 11, kLisp), "4", "line after a group");
	checkEq(At("(1 2 +) 3 *\n(4)", 2, kLisp), "(1 2 +) 3 *", "one-line group is the line");
	checkEq(At("1 ; (\n2 \")\" 3\n4", 7, kLisp), "2 \")\" 3", "brackets in comments and strings ignored");
	checkEq(At("(\n1 2 +", 3, kLisp), "1 2 +", "unbalanced group falls back to the line");
}

void testParagraphs()
{
	const std::string text = "a\nb\n\nc\nd\n";
	checkEq(At(text, 2, kLua), "a\nb", "paragraph around the caret");
	checkEq(At(text, 6, kLua), "c\nd", "second paragraph");
	checkEq(At(text, 4, kLua), "a\nb", "blank line uses the paragraph above");
	checkEq(At(text, (int)text.size(), kLua), "c\nd", "caret after the final newline");
	const std::string voice = "spawn(function()\n  play(c4)\nend)\n\nn(e4)";
	checkEq(At(voice, 20, kLua), "spawn(function()\n  play(c4)\nend)", "lua function block");
	checkEq(At(voice, (int)voice.size(), kLua), "n(e4)", "last paragraph without a newline");
}

// --- timeline -----------------------------------------------------------

struct Out {
	long long frame; // absolute
	uint8_t status, key;
};

const double k120At48k = 120. / 60. / 48000.; // beats per frame
const uint32_t kBlock = 512;

MidiEvent Ev(double beat, uint8_t status, uint8_t key, uint8_t vel)
{
	return {beat, {status, key, vel}, 3};
}

// Renders blocks from beat `start`, collecting events at absolute frames from frame `base`.
void RenderClip(ClipPlayer& player, const Clip* clip, bool playing, double start, int blocks, long long base, std::vector<Out>& out)
{
	for (int b = 0; b < blocks; ++b) {
		BlockTime t{start + b * kBlock * k120At48k, k120At48k, kBlock};
		player.render(clip, playing, t, [&](uint32_t offset, const uint8_t* data, uint8_t) {
			out.push_back({base + b * (long long)kBlock + offset, data[0], data[1]});
		});
	}
}

void testMakeClip()
{
	Clip c = MakeClip({Ev(1., 0x80, 60, 0), Ev(0., 0x90, 60, 100), Ev(1., 0x90, 62, 100)}, 4.);
	check(c.length == 4., "clip length rounds up to a bar");
	check(c.events[0].beat == 0. && c.events[1].data[0] == 0x80 && c.events[2].data[1] == 62,
	      "events sorted by beat, send order kept at equal beats");
	check(MakeClip({Ev(4., 0x80, 60, 0)}, 4.).length == 8., "an event on a bar line starts another bar");
	check(MakeClip({Ev(2.9, 0x80, 60, 0)}, 3.).length == 3., "bar length follows the time signature");
}

void testClipRoundTrip()
{
	Clip c = MakeClip({Ev(0., 0x90, 60, 100), Ev(1.5, 0x80, 60, 0), {2., {0xC0, 5, 0}, 2}}, 4.);
	std::string data = "xx";
	WriteClip(data, c);
	size_t pos = 2;
	Clip back;
	check(ReadClip(data, pos, back) && pos == data.size(), "clip reads back");
	check(back.length == c.length && back.events.size() == 3 && back.events[1].beat == 1.5
	          && back.events[2].size == 2 && back.events[2].data[1] == 5,
	      "clip round trip keeps events");
	pos = 2;
	check(!ReadClip(data.substr(0, data.size() - 1), pos, back), "truncated clip is rejected");
	std::string empty;
	WriteClip(empty, Clip());
	pos = 0;
	check(ReadClip(empty, pos, back) && back.events.empty(), "empty clip round trip");
}

void testClipPlayback()
{
	const Clip c = MakeClip({Ev(0., 0x90, 60, 100), Ev(1., 0x80, 60, 0)}, 4.);
	ClipPlayer player;
	std::vector<Out> out;
	// 2 loops of 4 beats: 192000 frames = 375 blocks.
	RenderClip(player, &c, true, 0., 375, 0, out);
	check(out.size() == 4, "two loops give two note pairs, got " + std::to_string(out.size()));
	if (out.size() == 4) {
		check(out[0].frame == 0 && out[0].status == 0x90, "note on at frame 0");
		check(out[1].frame == 24000 && out[1].status == 0x80, "note off one beat later, on a block boundary");
		check(out[2].frame == 96000 && out[3].frame == 120000, "the clip loops after its length");
	}

	// Transport stops with the note held: released at once.
	ClipPlayer stopped;
	out.clear();
	RenderClip(stopped, &c, true, 0., 10, 0, out);
	RenderClip(stopped, &c, false, 10 * kBlock * k120At48k, 1, 10 * kBlock, out);
	check(out.size() == 2 && out[1].status == 0x80 && out[1].frame == 10 * kBlock, "stop releases the held note");

	// The host jumps into the middle of the held note: release, and no note-on until the next loop.
	ClipPlayer jumped;
	out.clear();
	RenderClip(jumped, &c, true, 0., 10, 0, out);
	RenderClip(jumped, &c, true, 0.5, 1, 100000, out);
	check(out.size() == 2 && out[1].status == 0x80 && out[1].frame == 100000, "a jump releases the held note");

	// A new clip releases the old clip's notes.
	const Clip other = MakeClip({Ev(2., 0x90, 64, 100), Ev(3., 0x80, 64, 0)}, 4.);
	ClipPlayer swapped;
	out.clear();
	RenderClip(swapped, &c, true, 0., 10, 0, out);
	RenderClip(swapped, &other, true, 10 * kBlock * k120At48k, 1, 10 * kBlock, out);
	check(out.size() == 2 && out[1].status == 0x80 && out[1].key == 60, "a new clip releases held notes");

	// Pre-roll: negative positions play the end of the clip's previous pass.
	const Clip tail = MakeClip({Ev(3.5, 0x90, 67, 100)}, 4.);
	ClipPlayer preroll;
	out.clear();
	RenderClip(preroll, &tail, true, -1., 47, 0, out);
	check(out.size() == 1 && out[0].frame == 12000, "pre-roll plays the clip's previous pass");
}

void testLivePlayback()
{
	SpscRing<LiveEvent, 16> queue;
	LivePlayer player;
	queue.push({Ev(0.25, 0x90, 60, 100), 1});
	queue.push({Ev(0.0, 0x90, 61, 100), 0}); // from before a stop
	queue.push({Ev(0.0, 0x90, 62, 100), 1}); // late and out of order
	queue.push({Ev(1.0, 0x80, 60, 0), 1});
	std::vector<Out> out;
	auto emit = [&](uint32_t offset, const uint8_t* data, uint8_t) { out.push_back({offset, data[0], data[1]}); };
	// The block at beat 0.24 covers beats [0.24, 0.2613).
	player.render(queue, 1, {0.24, k120At48k, kBlock}, emit);
	check(out.size() == 2, "live block plays due events, got " + std::to_string(out.size()));
	if (out.size() == 2) {
		check(out[0].key == 60 && out[0].frame == 240, "live event at its frame");
		check(out[1].key == 62 && out[1].frame == 240, "late event keeps the output sorted");
	}
	check(queue.front() && queue.front()->event.beat == 1.0, "future events stay queued");
	out.clear();
	player.stop(emit);
	check(out.size() == 2 && out[0].status == 0x80, "stop releases live notes");
}

} // namespace

// --- REAPER's hwnd_info hook --------------------------------------------

std::vector<std::string> gRegistered;

int fakeRegister(const char* name, void*)
{
	gRegistered.push_back(name);
	return 1;
}

void testReaperTextFields()
{
	const ReaperInfo reaper = {0, nullptr, fakeRegister, nullptr};
	int a = 0, b = 0, windowA = 0, windowB = 0, other = 0;
	bool active = true;
	check(ReaperHwndInfo(&windowA, 0) == 0, "no editor: not known");
	ReaperAddTextFields(nullptr, &a, [](const void*) { return true; }); // another host
	check(gRegistered.empty() && ReaperHwndInfo(&other, 0) == 0, "not REAPER: nothing registered");
	ReaperRemoveTextFields(&a);
	ReaperAddTextFields(&reaper, &a, [&](const void* w) { return w == &windowA && active; });
	ReaperAddTextFields(&reaper, &b, [&](const void* w) { return w == &windowB; });
	check(gRegistered.size() == 1 && gRegistered[0] == "hwnd_info", "one hook for two editors");
	check(ReaperHwndInfo(&windowA, 0) == 1 && ReaperHwndInfo(&windowA, 1) == 1, "active field");
	check(ReaperHwndInfo(&windowA, 2) == 0, "unknown query");
	check(ReaperHwndInfo(&other, 0) == 0, "another window");
	active = false;
	check(ReaperHwndInfo(&windowA, 0) == 0, "field not active: REAPER keeps Space");
	ReaperRemoveTextFields(&a);
	check(gRegistered.size() == 1 && ReaperHwndInfo(&windowB, 0) == 1, "the other editor keeps the hook");
	ReaperRemoveTextFields(&b);
	check(gRegistered.size() == 2 && gRegistered[1] == "-hwnd_info", "the last editor removes it");
	ReaperRemoveTextFields(&b);
	check(gRegistered.size() == 2, "removing twice is harmless");
}

// --- simulator -------------------------------------------------------------

void testSimScript()
{
	const SimScript s = ParseSimScript("-- about\n  -- the set\n\nsig = a\n\n  \n-- k\nk = 1\n--- bass\npat = b\n\nx\n---\n\n--- empty name \n---no space\n1\n", "--");
	check(s.preamble.size() == 2 && s.preamble[0] == "sig = a", "preamble paragraphs");
	check(s.steps.size() == 3, "three steps: got " + std::to_string(s.steps.size()));
	if (s.steps.size() == 3) {
		checkEq(s.steps[0].name, "bass", "step name");
		check(s.steps[0].paragraphs.size() == 2 && s.steps[0].paragraphs[1] == "x", "a step's paragraphs");
		checkEq(s.steps[1].name, "step 2", "an unnamed step is numbered");
		check(s.steps[1].paragraphs.empty(), "a step may be empty");
		checkEq(s.steps[2].name, "empty name", "trailing spaces trimmed");
		check(s.steps[2].paragraphs.size() == 1 && s.steps[2].paragraphs[0] == "---no space\n1",
		      "---word is a comment line, not a step");
	}
	check(s.preamble.size() == 2 && s.preamble[1] == "-- k\nk = 1", "a comment paragraph is skipped, a commented one kept");
	check(ParseSimScript("", "--").steps.empty(), "empty script");
	check(ParseSimScript("-- x\n", nullptr).preamble.size() == 1, "no comment syntax: every paragraph is code");
}

void testSimOrder()
{
	SimOrder rotate(false, 0, 1);
	std::string got;
	for (int i = 0; i < 5; ++i) got += std::to_string(rotate.next(3));
	checkEq(got, "12012", "rotation from the instance's offset");
	checkEq(std::to_string(rotate.next(2)), "1", "rotation when the script shrinks");
	SimOrder random(true, 42, 0);
	size_t last = random.next(4);
	int seen[4] = {0, 0, 0, 0};
	bool repeated = false;
	for (int i = 0; i < 400; ++i) {
		const size_t k = random.next(4);
		repeated |= k == last;
		seen[k < 4 ? k : 0]++;
		last = k;
	}
	check(!repeated, "random: no step twice in a row");
	check(seen[0] > 50 && seen[1] > 50 && seen[2] > 50 && seen[3] > 50, "random: every step chosen");
	SimOrder one(true, 1, 0);
	check(one.next(1) == 0 && one.next(1) == 0, "random of one step");
	SimOrder a(true, 7, 0), b(true, 7, 0);
	bool same = true;
	for (int i = 0; i < 20; ++i) same &= a.next(5) == b.next(5);
	check(same, "random: a seed repeats its order");
}

void testSimConfig()
{
	SimConfig c;
	std::string error;
	unsetenv("T_SIM");
	check(!SimConfigFromEnv("T_SIM", &c, &error), "unset: no simulator");
	check(!SimConfigFromEnv(nullptr, &c, &error), "no prefix: no simulator");
	setenv("T_SIM", "/x/set.fk", 1);
	check(SimConfigFromEnv("T_SIM", &c, &error) && error.empty(), "set");
	check(c.log == "/x/set.fk.log" && !c.random && c.interval == 16., "defaults");
	setenv("T_SIM_ORDER", "random", 1);
	setenv("T_SIM_INTERVAL", "2.5", 1);
	setenv("T_SIM_SEED", "9", 1);
	setenv("T_SIM_LOG", "/y.log", 1);
	c = SimConfig();
	check(SimConfigFromEnv("T_SIM", &c, &error) && error.empty(), "every variable");
	check(c.random && c.interval == 2.5 && c.seed == 9 && c.log == "/y.log", "values");
	setenv("T_SIM_ORDER", "shuffle", 1);
	setenv("T_SIM_INTERVAL", "0", 1);
	setenv("T_SIM_SEED", "x", 1);
	c = SimConfig();
	check(SimConfigFromEnv("T_SIM", &c, &error), "bad values still enable it");
	check(!c.random && c.interval == 16., "bad values keep the defaults");
	check(error.find("_ORDER") != std::string::npos && error.find("_INTERVAL") != std::string::npos
	          && error.find("_SEED") != std::string::npos, "bad values reported: " + error);
}

void testBlockStats()
{
	BlockStats s;
	s.add(100, 1000);
	s.add(600, 1000);
	s.add(1500, 1000);
	check(s.blocks == 3 && s.half == 2 && s.over == 1 && s.maxNs == 1500 && s.budgetNs == 1000, "block counts");
}

int main()
{
	testSimScript();
	testSimOrder();
	testSimConfig();
	testBlockStats();
	testReaperTextFields();
	testSelection();
	testBrackets();
	testParagraphs();
	testMakeClip();
	testClipRoundTrip();
	testClipPlayback();
	testLivePlayback();
	printf("%d failure(s)\n", gFailures);
	return gFailures ? 1 : 0;
}

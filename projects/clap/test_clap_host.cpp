// Headless CLAP host: loads a midi-langs plugin and checks evaluation over UDP, clip and live
// playback at exact frames, stop, state, and the one-instance rule.
// Usage: test_clap_host <plugin binary> <cases file> <udp port>
// The plugin's UDP port variable must be set to <udp port>, which turns UDP on at init.
//
// The cases file holds the language's code for each check, as "[name]" lines followed by code:
//   expr, expr_out   an expression and text its reply contains
//   error            code that fails
//   note             one note: key 60, velocity 100, 500 ms
//   voices           two concurrent voices from time 0: key 60 for 500 ms, key 64 for 1000 ms
//   forever          a loop of notes that never ends
//   busy             optional: a loop that neither sends nor sleeps, for languages whose Stop
//                    interrupts any code
//   live_voice       a voice, or a loop, that plays key 72 forever

#include <clap/clap.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

int gFailures = 0;
int gPort = 0;
const double kRate = 48000.;
const uint32_t kBlock = 512;
const long long kBeatFrames = 24000; // at 120 BPM
uint32_t gAudioChannels = 0;         // of the plugin's output port; 0 for a note effect

void check(bool ok, const std::string& what)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
	if (!ok) ++gFailures;
}

std::map<std::string, std::string> ReadCases(const char* path)
{
	std::map<std::string, std::string> cases;
	std::ifstream in(path);
	std::string line, name;
	while (std::getline(in, line)) {
		if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
			name = line.substr(1, line.size() - 2);
			cases[name];
		} else if (!name.empty()) {
			cases[name] += line + "\n";
		}
	}
	for (auto& [k, v] : cases) {
		while (!v.empty() && v.back() == '\n') v.pop_back();
	}
	return cases;
}

const clap_plugin_entry_t* LoadEntry(const char* path)
{
#ifdef _WIN32
	HMODULE lib = LoadLibraryA(path);
	return lib ? (const clap_plugin_entry_t*)GetProcAddress(lib, "clap_entry") : nullptr;
#else
	void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!lib) fprintf(stderr, "dlopen: %s\n", dlerror());
	return lib ? (const clap_plugin_entry_t*)dlsym(lib, "clap_entry") : nullptr;
#endif
}

const clap_host_t kHost = {
	CLAP_VERSION_INIT,
	nullptr,
	"test_clap_host",
	"midi-langs",
	"",
	"1.0",
	[](const clap_host_t*, const char*) -> const void* { return nullptr; },
	[](const clap_host_t*) {},
	[](const clap_host_t*) {},
	[](const clap_host_t*) {},
};

// --- UDP ------------------------------------------------------------------

struct Udp {
	Udp()
	{
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		addr.sin_family = AF_INET;
		addr.sin_port = htons((uint16_t)gPort);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	}
	~Udp()
	{
#ifdef _WIN32
		closesocket(fd);
#else
		close(fd);
#endif
	}

	void send(const std::string& code) { sendto(fd, code.data(), (int)code.size(), 0, (sockaddr*)&addr, sizeof(addr)); }

	// The reply, or "" if none arrives within ms.
	std::string receive(int ms)
	{
#ifdef _WIN32
		DWORD timeout = ms;
#else
		timeval timeout{ms / 1000, (ms % 1000) * 1000};
#endif
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
		char buf[65536];
		int n = (int)recv(fd, buf, sizeof(buf), 0);
		return n > 0 ? std::string(buf, n) : std::string();
	}

#ifdef _WIN32
	SOCKET fd;
#else
	int fd;
#endif
	sockaddr_in addr{};
};

std::string Eval(const std::string& code)
{
	Udp udp;
	udp.send(code);
	return udp.receive(15000);
}

// --- processing -------------------------------------------------------------

struct Event {
	long long frame; // absolute: block start + offset
	uint8_t data[3];
};

struct Events {
	std::vector<Event> list;
	std::vector<float> audio; // the first channel's output, with an audio port
	long long blockStart = 0;
	bool sorted = true;
	uint32_t last = 0;
};

const clap_output_events_t* MakeOut(Events& events)
{
	static clap_output_events_t out;
	out.ctx = &events;
	out.try_push = [](const clap_output_events_t* o, const clap_event_header_t* h) {
		auto* ev = static_cast<Events*>(o->ctx);
		if (h->type != CLAP_EVENT_MIDI || h->space_id != CLAP_CORE_EVENT_SPACE_ID) return false;
		const auto* midi = reinterpret_cast<const clap_event_midi_t*>(h);
		if (h->time < ev->last || h->time >= kBlock) ev->sorted = false;
		// The checks are about notes; a language may add program and control changes.
		const uint8_t status = midi->data[0] & 0xF0;
		if (status != 0x80 && status != 0x90) return true;
		ev->last = h->time;
		ev->list.push_back({ev->blockStart + h->time, {midi->data[0], midi->data[1], midi->data[2]}});
		return true;
	};
	out.ctx = &events;
	return &out;
}

const clap_input_events_t kNoInput = {
	nullptr,
	[](const clap_input_events_t*) -> uint32_t { return 0; },
	[](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* { return nullptr; },
};

// Events for a block's input, in time order.
struct Input {
	std::vector<const clap_event_header_t*> list;
};

const clap_input_events_t* MakeIn(const Input& input)
{
	static clap_input_events_t in;
	in.ctx = const_cast<Input*>(&input);
	in.size = [](const clap_input_events_t* i) { return (uint32_t)static_cast<Input*>(i->ctx)->list.size(); };
	in.get = [](const clap_input_events_t* i, uint32_t k) { return static_cast<Input*>(i->ctx)->list[k]; };
	return &in;
}

// Processes one block. beat < 0 means no transport; playing selects the transport's play flag.
void Process(const clap_plugin_t* plugin, Events& events, double beat, bool playing, const Input* input = nullptr)
{
	clap_event_transport_t tr{};
	tr.header.size = sizeof(tr);
	tr.header.type = CLAP_EVENT_TRANSPORT;
	tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE
	           | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
	tr.song_pos_beats = (clap_beattime)std::llround(beat * CLAP_BEATTIME_FACTOR);
	tr.tempo = 120.;
	tr.tsig_num = 4;
	tr.tsig_denom = 4;
	clap_process_t process{};
	process.frames_count = kBlock;
	process.steady_time = -1;
	process.transport = beat < 0 ? nullptr : &tr;
	process.in_events = input ? MakeIn(*input) : &kNoInput;
	process.out_events = MakeOut(events);
	std::vector<float> channels(gAudioChannels * kBlock);
	std::vector<float*> data;
	for (uint32_t c = 0; c < gAudioChannels; ++c) data.push_back(&channels[c * kBlock]);
	clap_audio_buffer_t port{};
	port.data32 = data.data();
	port.channel_count = gAudioChannels;
	if (gAudioChannels) {
		process.audio_outputs = &port;
		process.audio_outputs_count = 1;
	}
	events.last = 0;
	plugin->process(plugin, &process);
	events.audio.insert(events.audio.end(), channels.begin(), channels.begin() + (gAudioChannels ? kBlock : 0));
	events.blockStart += kBlock;
}

// Renders `blocks` blocks with the transport playing from `beat`.
Events Play(const clap_plugin_t* plugin, double beat, int blocks)
{
	Events events;
	for (int b = 0; b < blocks; ++b) Process(plugin, events, beat + b * kBlock / (double)kBeatFrames, true);
	return events;
}

bool Is(const Event& e, long long frame, uint8_t status, uint8_t key)
{
	return e.frame == frame && e.data[0] == status && e.data[1] == key;
}

std::string Describe(const Events& events)
{
	std::ostringstream s;
	for (const Event& e : events.list) {
		char buf[48];
		snprintf(buf, sizeof(buf), " %02x %d @%lld", e.data[0], e.data[1], e.frame);
		s << buf;
	}
	return s.str();
}

// --- state ------------------------------------------------------------------

struct Buffer {
	std::string data;
	size_t pos = 0;
};

bool SaveState(const clap_plugin_t* plugin, std::string& data)
{
	auto* state = (const clap_plugin_state_t*)plugin->get_extension(plugin, CLAP_EXT_STATE);
	Buffer buf;
	clap_ostream_t os{&buf, [](const clap_ostream_t* s, const void* p, uint64_t n) -> int64_t {
		((Buffer*)s->ctx)->data.append((const char*)p, n);
		return (int64_t)n;
	}};
	bool ok = state && state->save(plugin, &os);
	data = buf.data;
	return ok;
}

bool LoadState(const clap_plugin_t* plugin, const std::string& data)
{
	auto* state = (const clap_plugin_state_t*)plugin->get_extension(plugin, CLAP_EXT_STATE);
	Buffer buf{data, 0};
	clap_istream_t is{&buf, [](const clap_istream_t* s, void* p, uint64_t n) -> int64_t {
		Buffer* b = (Buffer*)s->ctx;
		size_t k = std::min<size_t>(n, b->data.size() - b->pos);
		memcpy(p, b->data.data() + b->pos, k);
		b->pos += k;
		return (int64_t)k;
	}};
	return state && state->load(plugin, &is);
}

// Version 1, flags, empty text, empty clip.
std::string EmptyState(char flags)
{
	std::string s;
	s += '\x01';
	s += flags;
	s.append(8, '\0');                // text length
	s.append(8, '\0');                // event count
	s.append(8, '\0');                // clip length 0.0
	return s;
}

void SetOffline(const clap_plugin_t* plugin, bool offline)
{
	auto* render = (const clap_plugin_render_t*)plugin->get_extension(plugin, CLAP_EXT_RENDER);
	check(render && render->set(plugin, offline ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME),
	      offline ? "render mode offline" : "render mode realtime");
}

// --- checks -------------------------------------------------------------------

void CheckDescriptor(const clap_plugin_factory_t* factory)
{
	const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
	bool noteEffect = false, instrument = false;
	for (const char* const* f = desc->features; f && *f; ++f) {
		noteEffect |= !strcmp(*f, CLAP_PLUGIN_FEATURE_NOTE_EFFECT);
		instrument |= !strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT);
	}
	check(noteEffect, "descriptor lists the note-effect feature");
	if (instrument) printf("instrument: audio checks on\n");
	check(desc->version && desc->version[0], "descriptor has a version");
}

void CheckPorts(const clap_plugin_t* plugin)
{
	// A note effect has no audio ports; an instrument one main output.
	auto* audio = (const clap_plugin_audio_ports_t*)plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS);
	check(audio && audio->count(plugin, true) == 0 && audio->count(plugin, false) <= 1, "no audio input, one output at most");
	clap_audio_port_info_t port{};
	if (audio && audio->count(plugin, false) == 1) {
		check(audio->get(plugin, 0, false, &port) && (port.flags & CLAP_AUDIO_PORT_IS_MAIN) && port.channel_count > 0,
		      "the audio output is a main port");
		gAudioChannels = port.channel_count;
	}
	// One note output; an instrument also takes notes, as CLAP events or MIDI.
	auto* notes = (const clap_plugin_note_ports_t*)plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS);
	clap_note_port_info_t info{};
	const uint32_t inputs = gAudioChannels ? 1 : 0;
	check(notes && notes->count(plugin, true) == inputs && notes->count(plugin, false) == 1,
	      "one note output, and a note input if an instrument");
	check(notes && notes->get(plugin, 0, false, &info) && info.supported_dialects == CLAP_NOTE_DIALECT_MIDI,
	      "the note output speaks MIDI");
	if (inputs)
		check(notes->get(plugin, 0, true, &info) && (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP)
		          && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI),
		      "the note input takes CLAP notes and MIDI");
}

clap_event_note_t Note(uint16_t type, uint32_t time, int16_t key, double velocity)
{
	clap_event_note_t e{};
	e.header.size = sizeof(e);
	e.header.time = time;
	e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	e.header.type = type;
	e.note_id = -1;
	e.port_index = 0;
	e.channel = 0;
	e.key = key;
	e.velocity = velocity;
	return e;
}

clap_event_midi_t Midi(uint32_t time, uint8_t status, uint8_t key, uint8_t vel)
{
	clap_event_midi_t e{};
	e.header.size = sizeof(e);
	e.header.time = time;
	e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	e.header.type = CLAP_EVENT_MIDI;
	e.data[0] = status;
	e.data[1] = key;
	e.data[2] = vel;
	return e;
}

// The index of the first sample from `from` whose being nonzero is `nonzero`, or -1.
long long FirstWhere(const std::vector<float>& audio, size_t from, bool nonzero)
{
	for (size_t i = from; i < audio.size(); ++i)
		if ((audio[i] != 0.f) == nonzero) return (long long)i;
	return -1;
}

// The plugin's notes play its own voices too, from the frames they are sent at.
void CheckAudio(const clap_plugin_t* plugin, std::map<std::string, std::string>& cases)
{
	if (!gAudioChannels || !cases.count("audio")) {
		printf("SKIP: notes play the plugin's voices (no audio port or no [audio] case)\n");
		return;
	}
	std::string reply = Eval(cases["audio"]);
	check(reply.find("error") == std::string::npos, "the [audio] case evaluates: " + reply);
	Events silent = Play(plugin, 0., 4);
	check(FirstWhere(silent.audio, 0, true) < 0, "silent without notes");

	// The host's notes play the voices from their frames: a CLAP note, ended by MIDI, then the reverse.
	Events in;
	const clap_event_note_t noteOn = Note(CLAP_EVENT_NOTE_ON, 100, 69, 1.0), noteOff = Note(CLAP_EVENT_NOTE_OFF, 30, 60, 0.);
	const clap_event_midi_t midiOff = Midi(50, 0x80, 69, 0), midiOn = Midi(10, 0x90, 60, 64);
	const Input blocks[4] = {{{&noteOn.header}}, {{&midiOff.header}}, {{&midiOn.header}}, {{&noteOff.header}}};
	for (const Input& b : blocks) Process(plugin, in, -1., false, &b);
	const long long a = FirstWhere(in.audio, 0, true), b = FirstWhere(in.audio, (size_t)std::max(a, 0LL), false);
	const long long c = FirstWhere(in.audio, (size_t)std::max(b, 0LL), true), d = FirstWhere(in.audio, (size_t)std::max(c, 0LL), false);
	check(a == 100 && in.audio[100] == 1.f && b == kBlock + 50,
	      "a CLAP note-on sounds from its frame, at its velocity, to a MIDI note-off: " + std::to_string(a) + " to " + std::to_string(b));
	check(c == 2 * kBlock + 10 && in.audio[c] == 64.f / 127.f && d == 3 * kBlock + 30,
	      "a MIDI note-on sounds to a CLAP note-off: " + std::to_string(c) + " to " + std::to_string(d));
	reply = Eval(cases["note"]);
	check(reply.find("clip: ") != std::string::npos, "note becomes a clip: " + reply);
	Events ev = Play(plugin, 0., (int)(2 * kBeatFrames / kBlock));
	const long long on = FirstWhere(ev.audio, 0, true), off = on < 0 ? -1 : FirstWhere(ev.audio, (size_t)on, false);
	check(on == 0 && off == kBeatFrames,
	      "the note sounds from frame 0 to its note-off at 24000: " + std::to_string(on) + " to " + std::to_string(off));

	// In one block, a host's note at frame 300 and the clip's at frame 0 play in time order:
	// the voices take events in order, so the clip's, if second, would start at 300.
	Events mixed;
	const clap_event_note_t hostOn = Note(CLAP_EVENT_NOTE_ON, 300, 69, 1.0), hostOff = Note(CLAP_EVENT_NOTE_OFF, 0, 69, 0.);
	const Input first{{&hostOn.header}}, second{{&hostOff.header}};
	Process(plugin, mixed, 0., true, &first);
	Process(plugin, mixed, kBlock / (double)kBeatFrames, true, &second);
	const float clip = 100.f / 127.f;
	check(mixed.list.size() == 1 && Is(mixed.list[0], 0, 0x90, 60) && mixed.audio[0] == clip
	          && std::fabs(mixed.audio[300] - (clip + 1.f)) < 1e-6f && mixed.audio[kBlock] == clip,
	      "the host's and the clip's notes in one block sound in time order: " + std::to_string(mixed.audio[0]) + ", "
	          + std::to_string(mixed.audio[300]) + ", " + std::to_string(mixed.audio[kBlock]));
	Events released; // a stopped transport releases the clip's held note
	Process(plugin, released, 2 * kBlock / (double)kBeatFrames, false);
}

void CheckEval(std::map<std::string, std::string>& cases)
{
	std::string reply = Eval(cases["expr"]);
	check(reply.find(cases["expr_out"]) != std::string::npos, "expression reply contains its value: " + reply);
	reply = Eval(cases["error"]);
	check(reply.find("error") != std::string::npos, "error reply: " + reply);
}

void CheckClip(const clap_plugin_t* plugin, std::map<std::string, std::string>& cases)
{
	std::string reply = Eval(cases["note"]);
	check(reply.find("clip: ") != std::string::npos && reply.find(", 4 beats") != std::string::npos,
	      "note becomes a one-bar clip: " + reply);
	// Two bars: the clip loops after 4 beats.
	Events ev = Play(plugin, 0., (int)(8 * kBeatFrames / kBlock));
	check(ev.sorted, "events are sorted within each block");
	check(ev.list.size() == 4 && Is(ev.list[0], 0, 0x90, 60) && ev.list[0].data[2] == 100
	          && Is(ev.list[1], kBeatFrames, 0x80, 60) && Is(ev.list[2], 4 * kBeatFrames, 0x90, 60)
	          && Is(ev.list[3], 5 * kBeatFrames, 0x80, 60),
	      "clip plays on frames 0, 24000 and loops at 96000:" + Describe(ev));

	// Tempo follows the host: the transport position, not the frame count, places events.
	Events late = Play(plugin, 2., 2);
	check(late.list.empty(), "nothing plays between the note and the loop:" + Describe(late));

	reply = Eval(cases["voices"]);
	check(reply.find("clip: ") != std::string::npos && reply.find(", 4 beats") != std::string::npos,
	      "voices become one clip: " + reply);
	ev = Play(plugin, 0., (int)(2 * kBeatFrames / kBlock) + 2);
	check(ev.list.size() == 4 && Is(ev.list[0], 0, 0x90, 60) && Is(ev.list[1], 0, 0x90, 64)
	          && Is(ev.list[2], kBeatFrames, 0x80, 60) && Is(ev.list[3], 2 * kBeatFrames, 0x80, 64),
	      "voices play concurrently from frame 0:" + Describe(ev));

	// Stopping the transport with a note held releases it; a stopped transport plays nothing.
	ev = Play(plugin, 0., 4);
	Events stopped;
	Process(plugin, stopped, 4 * kBlock / (double)kBeatFrames, false);
	Process(plugin, stopped, 4 * kBlock / (double)kBeatFrames, false);
	check(stopped.list.size() == 2 && Is(stopped.list[0], 0, 0x80, 60) && Is(stopped.list[1], 0, 0x80, 64),
	      "transport stop releases held notes:" + Describe(stopped));

	reply = Eval(cases["forever"]);
	check(reply.find("stopped") != std::string::npos, "a clip that never ends is cut off: " + reply);
}

void CheckStop(const clap_plugin_t* plugin, std::map<std::string, std::string>& cases)
{
	if (!cases.count("busy")) {
		plugin->reset(plugin);
		Play(plugin, 0., 1);
		printf("SKIP: stop ends a busy evaluation (no [busy] case)\n");
		return;
	}
	Udp udp;
	udp.send(cases["busy"]);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	plugin->reset(plugin); // what the editor's Stop does
	std::string reply = udp.receive(5000);
	check(reply.find("stopped") != std::string::npos, "stop ends a busy evaluation: " + reply);
	Events ev = Play(plugin, 0., 4);
	check(ev.list.empty(), "stop silences the clip:" + Describe(ev));
}

void CheckState(const clap_plugin_t* plugin, std::map<std::string, std::string>& cases, std::string& saved)
{
	const std::string reply = Eval(cases["note"]);
	check(reply.find("clip: ") != std::string::npos, "evaluation after a stop makes a clip: " + reply);
	Play(plugin, 0., 1); // the audio thread takes the new clip
	check(SaveState(plugin, saved) && saved.size() > 10, "state save");
	check(saved[1] == 1, "state saves UDP on, wrap on, clip mode");
	check(LoadState(plugin, saved), "state load");
	std::string again;
	check(SaveState(plugin, again) && again == saved, "state round trip");
	check(!LoadState(plugin, saved.substr(0, saved.size() - 3)), "truncated state is rejected");
}

void CheckLive(const clap_plugin_t* plugin, std::map<std::string, std::string>& cases)
{
	check(LoadState(plugin, EmptyState(1 | 4)), "load state with Live on");
	SetOffline(plugin, true);
	Events released; // switching mode releases the clip's held note
	Process(plugin, released, -1., false);

	// The evaluation waits for the frames it plays at; the reply comes once the note-off is queued.
	Udp udp;
	udp.send(cases["note"]);
	Events ev;
	std::string reply;
	for (int b = 0; b < 400 && reply.empty(); ++b) {
		Process(plugin, ev, -1., false);
		if (b % 8 == 7) reply = udp.receive(1);
	}
	for (int b = 0; b < 100; ++b) Process(plugin, ev, -1., false);
	check(reply == "ok\n", "live evaluation replies after the note: " + reply);
	check(ev.list.size() == 2 && ev.list[0].data[0] == 0x90 && ev.list[1].data[0] == 0x80
	          && ev.list[1].frame - ev.list[0].frame == kBeatFrames,
	      "live note lasts one beat to the frame:" + Describe(ev));

	// A voice that never ends plays until stopped, then its note is released. In a language
	// without voices it is a loop, which replies only once stopped, so the reply is not awaited.
	Udp liveVoice;
	liveVoice.send(cases["live_voice"]);
	// The datagram may reach the plugin after rendering starts: wait for the first note. By
	// time, not blocks: offline blocks render as fast as the CPU allows, so under load the
	// datagram could come after any number of them.
	Events voice;
	const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (voice.list.empty() && std::chrono::steady_clock::now() < give_up) Process(plugin, voice, -1., false);
	for (int b = 0; b < 200; ++b) Process(plugin, voice, -1., false);
	const size_t played = voice.list.size();
	check(played >= 6, "live voice keeps playing:" + Describe(voice));
	const bool held = !voice.list.empty() && voice.list.back().data[0] == 0x90;
	plugin->reset(plugin);
	Events after;
	for (int b = 0; b < 100; ++b) Process(plugin, after, -1., false);
	check(after.list.size() == (held ? 1u : 0u) && (!held || after.list[0].data[0] == 0x80),
	      "stop ends the voice and releases its note:" + Describe(after));
	SetOffline(plugin, false);

	// In real time the evaluator runs ahead by the lookahead, so events still land on their frames.
	Udp rt;
	rt.send(cases["note"]);
	Events paced;
	auto due = std::chrono::steady_clock::now();
	for (int b = 0; b < (int)(1.5 * kBeatFrames / kBlock); ++b) {
		Process(plugin, paced, -1., false);
		due += std::chrono::microseconds((long long)(kBlock * 1e6 / kRate));
		std::this_thread::sleep_until(due);
	}
	check(rt.receive(1000) == "ok\n", "real-time live evaluation replies");
	check(paced.list.size() == 2 && paced.list[1].frame - paced.list[0].frame == kBeatFrames,
	      "real-time live note lasts one beat to the frame:" + Describe(paced));
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: test_clap_host <plugin> <cases file> <udp port>\n");
		return 2;
	}
	auto cases = ReadCases(argv[2]);
	for (const char* name : {"expr", "expr_out", "error", "note", "voices", "forever", "live_voice"}) {
		if (!cases.count(name)) {
			fprintf(stderr, "%s: no [%s] case\n", argv[2], name);
			return 2;
		}
	}
	gPort = atoi(argv[3]);
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	const clap_plugin_entry_t* entry = LoadEntry(argv[1]);
	check(entry != nullptr, "clap_entry exported");
	if (!entry) return 1;
	check(entry->init(argv[1]), "entry init");
	auto* factory = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	check(factory && factory->get_plugin_count(factory) == 1, "factory has one plugin");
	CheckDescriptor(factory);
	const char* id = factory->get_plugin_descriptor(factory, 0)->id;

	const clap_plugin_t* plugin = factory->create_plugin(factory, &kHost, id);
	check(plugin != nullptr, "create plugin");
	check(factory->create_plugin(factory, &kHost, id) == nullptr, "second instance refused");
	check(plugin->init(plugin), "plugin init");
	CheckPorts(plugin);
	check(plugin->activate(plugin, kRate, 32, 4096), "activate");
	check(plugin->start_processing(plugin), "start processing");

	CheckEval(cases);
	CheckAudio(plugin, cases);
	CheckClip(plugin, cases);
	CheckStop(plugin, cases);
	std::string saved;
	CheckState(plugin, cases, saved);
	CheckLive(plugin, cases);

	plugin->stop_processing(plugin);
	plugin->deactivate(plugin);
	plugin->destroy(plugin);

	// A new instance restores the saved clip without evaluating anything.
	const clap_plugin_t* again = factory->create_plugin(factory, &kHost, id);
	check(again != nullptr && again->init(again), "new instance after destroy");
	if (again) {
		again->activate(again, kRate, 32, 4096);
		check(LoadState(again, saved), "new instance loads the saved state");
		Events ev = Play(again, 0., (int)(kBeatFrames / kBlock) + 1);
		check(ev.list.size() == 2 && Is(ev.list[0], 0, 0x90, 60) && Is(ev.list[1], kBeatFrames, 0x80, 60),
		      "the saved clip plays in a new instance:" + Describe(ev));
		again->deactivate(again);
		again->destroy(again);
	}

	entry->deinit();
	printf("%d failure(s)\n", gFailures);
	return gFailures ? 1 : 0;
}

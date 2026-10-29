// Minimal CLAP host for trying a midi-langs plugin: opens its editor in a window, runs its
// process() in real time with the transport playing at 120 BPM, and sends its MIDI to a port.
// A plugin with an audio output plays through the default device (miniaudio), whose callback
// then runs process(). Adapted from sapf's examples/clap/dev_host.cpp. Close the window to quit.

#include "midi_open.h"
#include "miniaudio.h"

#include <clap/clap.h>
#include <pugl/pugl.h>
#include <pugl/stub.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

double gRate = 48000.; // the audio device's, when there is one
const uint32_t kBlock = 256;
const double kTempo = 120.;

struct Timer {
	clap_id id;
	std::chrono::milliseconds period;
	std::chrono::steady_clock::time_point due;
};

const clap_plugin_t* gPlugin = nullptr;
libremidi_midi_out_handle* gMidiOut = nullptr;
std::vector<Timer> gTimers;
std::atomic<bool> gCallbackRequested{false};
std::atomic<bool> gRunning{true};

const clap_host_timer_support_t kTimerSupport = {
	[](const clap_host_t*, uint32_t ms, clap_id* id) {
		static clap_id next = 0;
		*id = next++;
		gTimers.push_back({*id, std::chrono::milliseconds(ms), std::chrono::steady_clock::now()});
		return true;
	},
	[](const clap_host_t*, clap_id id) {
		for (auto it = gTimers.begin(); it != gTimers.end(); ++it) {
			if (it->id == id) {
				gTimers.erase(it);
				return true;
			}
		}
		return false;
	},
};

const clap_host_t kHost = {
	CLAP_VERSION_INIT,
	nullptr,
	"midi-langs dev host",
	"midi-langs",
	"https://github.com/shakfu/midi-langs",
	"1.0",
	[](const clap_host_t*, const char* id) -> const void* {
		return strcmp(id, CLAP_EXT_TIMER_SUPPORT) ? nullptr : &kTimerSupport;
	},
	[](const clap_host_t*) {},                             // request_restart
	[](const clap_host_t*) {},                             // request_process: always processing
	[](const clap_host_t*) { gCallbackRequested = true; }, // request_callback
};

const clap_plugin_entry_t* LoadEntry(const char* path)
{
#ifdef _WIN32
	HMODULE lib = LoadLibraryA(path);
	return lib ? (const clap_plugin_entry_t*)GetProcAddress(lib, "clap_entry") : nullptr;
#else
	void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!lib) fprintf(stderr, "%s\n", dlerror());
	return lib ? (const clap_plugin_entry_t*)dlsym(lib, "clap_entry") : nullptr;
#endif
}

const char* WindowApi()
{
#if defined(__APPLE__)
	return CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
	return CLAP_WINDOW_API_WIN32;
#else
	return CLAP_WINDOW_API_X11;
#endif
}

// CLAP gui sizes are points on macOS and pixels elsewhere; pugl sizes are pixels.
double HostUnitsToPixels(PuglView* view)
{
#ifdef __APPLE__
	return puglGetScaleFactor(view);
#else
	(void)view;
	return 1.0;
#endif
}

// --- MIDI out -----------------------------------------------------------------

std::vector<libremidi_midi_out_port*> gPorts;

void ListPorts()
{
	libremidi_observer_configuration conf;
	libremidi_api_configuration api;
	libremidi_midi_observer_handle* observer = nullptr;
	libremidi_midi_observer_configuration_init(&conf);
	libremidi_midi_api_configuration_init(&api);
	api.api = midi_backend_api();
	if (libremidi_midi_observer_new(&conf, &api, &observer) != 0) return;
	libremidi_midi_observer_enumerate_output_ports(observer, nullptr, [](void*, const libremidi_midi_out_port* port) {
		libremidi_midi_out_port* copy = nullptr;
		libremidi_midi_out_port_clone(port, &copy);
		gPorts.push_back(copy);
	});
	libremidi_midi_observer_free(observer);
}

// port >= 0: that port; otherwise the first port, or a virtual port named virtualName if there is none.
bool OpenMidiOut(int port, const char* virtualName)
{
	ListPorts();
	libremidi_midi_configuration conf;
	libremidi_api_configuration api;
	libremidi_midi_configuration_init(&conf);
	libremidi_midi_api_configuration_init(&api);
	api.api = midi_backend_api();
	char label[256] = "";
	if (port >= (int)gPorts.size()) {
		fprintf(stderr, "no MIDI port %d (%zu available)\n", port, gPorts.size());
		return false;
	}
	if (virtualName || gPorts.empty()) {
		conf.virtual_port = true;
		conf.port_name = virtualName ? virtualName : "midi-langs-dev";
		snprintf(label, sizeof(label), "virtual port %s", conf.port_name);
	} else {
		conf.out_port = gPorts[port < 0 ? 0 : port];
		midi_port_label(conf.out_port, label, sizeof(label));
	}
	if (midi_out_open(&conf, &api, &gMidiOut) != 0) {
		fprintf(stderr, "cannot open MIDI output: %s\n", midi_out_last_error());
		return false;
	}
	printf("MIDI out: %s\n", label);
	return true;
}

// --- processing -----------------------------------------------------------------

const clap_input_events_t kNoInput = {
	nullptr,
	[](const clap_input_events_t*) -> uint32_t { return 0; },
	[](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* { return nullptr; },
};

// Sends each event when its block is processed: timing is within one block (5 ms).
const clap_output_events_t kMidiOutput = {
	nullptr,
	[](const clap_output_events_t*, const clap_event_header_t* h) {
		if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_MIDI) return false;
		const auto* midi = reinterpret_cast<const clap_event_midi_t*>(h);
		const uint8_t status = midi->data[0] & 0xF0;
		const size_t size = (status == 0xC0 || status == 0xD0) ? 2 : 3;
		libremidi_midi_out_send_message(gMidiOut, midi->data, size);
		return true;
	},
};

double gBeat = 0.;

// One block of frames <= kBlock, into out[0..channels) if out is given.
void Process(uint32_t frames, float* const* out, uint32_t channels)
{
	clap_event_transport_t tr{};
		tr.header.size = sizeof(tr);
		tr.header.type = CLAP_EVENT_TRANSPORT;
		tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE
		           | CLAP_TRANSPORT_IS_PLAYING;
	tr.song_pos_beats = (clap_beattime)std::llround(gBeat * CLAP_BEATTIME_FACTOR);
	tr.tempo = kTempo;
	tr.tsig_num = 4;
	tr.tsig_denom = 4;
	clap_audio_buffer_t port{};
	port.data32 = const_cast<float**>(out);
	port.channel_count = channels;
	clap_process_t process{};
	process.frames_count = frames;
	process.steady_time = -1;
	process.transport = &tr;
	process.in_events = &kNoInput;
	process.out_events = &kMidiOutput;
	if (out) {
		process.audio_outputs = &port;
		process.audio_outputs_count = 1;
	}
	gPlugin->process(gPlugin, &process);
	gBeat += frames * kTempo / 60. / gRate;
}

// Without an audio output: process() on a timer.
void ProcessLoop()
{
	gPlugin->start_processing(gPlugin); // CLAP: on the audio thread
	const auto period = std::chrono::duration<double>(kBlock / gRate);
	auto due = std::chrono::steady_clock::now();
	while (gRunning) {
		Process(kBlock, nullptr, 0);
		due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
		std::this_thread::sleep_until(due);
	}
	gPlugin->stop_processing(gPlugin);
}

// With one: process() from the device's callback, in blocks of at most kBlock frames.
const uint32_t kMaxChannels = 8;
uint32_t gChannels = 0; // the plugin's output port's
bool gProcessing = false;
std::atomic<float> gPeak{0.f}; // since --meter last printed it

void OnAudio(ma_device* device, void* output, const void*, ma_uint32 frames)
{
	if (!gProcessing) gProcessing = gPlugin->start_processing(gPlugin); // CLAP: on the audio thread
	static float buf[kMaxChannels][kBlock];
	float* chans[kMaxChannels];
	for (uint32_t c = 0; c < gChannels; ++c) chans[c] = buf[c];
	float* o = static_cast<float*>(output);
	const uint32_t devChannels = device->playback.channels;
	for (ma_uint32 done = 0; done < frames;) {
		const uint32_t m = std::min<uint32_t>(frames - done, kBlock);
		Process(m, chans, gChannels);
		float peak = gPeak.load(std::memory_order_relaxed);
		for (uint32_t i = 0; i < m; ++i)
			for (uint32_t c = 0; c < devChannels; ++c) {
				const float x = buf[c % gChannels][i];
				o[(done + i) * devChannels + c] = x;
				peak = std::max(peak, std::fabs(x));
			}
		gPeak.store(peak, std::memory_order_relaxed);
		done += m;
	}
}

struct Window {
	const clap_plugin_gui_t* gui = nullptr;
	bool open = true;
};

PuglStatus OnEvent(PuglView* view, const PuglEvent* event)
{
	auto* win = static_cast<Window*>(puglGetHandle(view));
	if (event->type == PUGL_CLOSE) {
		win->open = false;
	} else if (event->type == PUGL_CONFIGURE && win->gui) {
		const double scale = HostUnitsToPixels(view);
		uint32_t w = (uint32_t)(event->configure.width / scale), h = (uint32_t)(event->configure.height / scale);
		if (win->gui->adjust_size(gPlugin, &w, &h)) win->gui->set_size(gPlugin, w, h);
	}
	return PUGL_SUCCESS;
}

void Usage(const char* prog)
{
	fprintf(stderr,
	        "Usage: %s PLUGIN.clap [options]\n"
	        "Opens the plugin's editor and plays it at 120 BPM.\n\n"
	        "Options:\n"
	        "  -l, --list          List MIDI output ports\n"
	        "  -p, --port N        Send to MIDI port N (0-based)\n"
	        "  --virtual NAME      Send to a new virtual port NAME\n"
	        "  --meter             Print the audio output's peak every second\n\n"
	        "By default it sends to the first MIDI port, or a virtual port\n"
	        "named midi-langs-dev if there is none.\n",
	        prog);
}

} // namespace

int main(int argc, char** argv)
{
	const char* path = nullptr;
	const char* virtualName = nullptr;
	int port = -1;
	bool meter = false;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			Usage(argv[0]);
			return 0;
		} else if (arg == "-l" || arg == "--list") {
			ListPorts();
			char label[256];
			for (size_t p = 0; p < gPorts.size(); ++p) {
				midi_port_label(gPorts[p], label, sizeof(label));
				printf("%zu: %s\n", p, label);
			}
			return 0;
		} else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
			port = atoi(argv[++i]);
		} else if (arg == "--meter") {
			meter = true;
		} else if (arg == "--virtual" && i + 1 < argc) {
			virtualName = argv[++i];
		} else if (!path && arg[0] != '-') {
			path = argv[i];
		} else {
			Usage(argv[0]);
			return 2;
		}
	}
	if (!path) {
		Usage(argv[0]);
		return 2;
	}
	if (!OpenMidiOut(port, virtualName)) return 1;

	const clap_plugin_entry_t* entry = LoadEntry(path);
	if (!entry || !entry->init(path)) {
		fprintf(stderr, "cannot load CLAP plugin %s\n", path);
		return 1;
	}
	auto* factory = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	gPlugin = factory->create_plugin(factory, &kHost, factory->get_plugin_descriptor(factory, 0)->id);
	if (!gPlugin || !gPlugin->init(gPlugin)) {
		fprintf(stderr, "cannot create plugin\n");
		return 1;
	}
	// An audio output plays through the default device, which then runs process().
	auto* ports = (const clap_plugin_audio_ports_t*)gPlugin->get_extension(gPlugin, CLAP_EXT_AUDIO_PORTS);
	clap_audio_port_info_t info{};
	ma_device device;
	bool audio = false;
	if (ports && ports->count(gPlugin, false) > 0 && ports->get(gPlugin, 0, false, &info) && info.channel_count > 0) {
		gChannels = std::min(info.channel_count, kMaxChannels);
		ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
		cfg.playback.format = ma_format_f32;
		cfg.playback.channels = gChannels;
		cfg.sampleRate = (ma_uint32)gRate;
		cfg.dataCallback = OnAudio;
		audio = ma_device_init(nullptr, &cfg, &device) == MA_SUCCESS;
		if (audio) {
			gRate = device.sampleRate;
			printf("audio out: %s, %u channels at %.0f Hz\n", device.playback.name, device.playback.channels, gRate);
		} else {
			fprintf(stderr, "cannot open the audio device: no audio\n");
		}
	}
	if (!gPlugin->activate(gPlugin, gRate, 1, kBlock)) {
		fprintf(stderr, "cannot activate plugin\n");
		return 1;
	}
	std::thread processing;
	if (audio && ma_device_start(&device) != MA_SUCCESS) {
		fprintf(stderr, "cannot start the audio device: no audio\n");
		ma_device_uninit(&device);
		audio = false;
	}
	if (!audio) processing = std::thread(ProcessLoop);

	auto* gui = (const clap_plugin_gui_t*)gPlugin->get_extension(gPlugin, CLAP_EXT_GUI);
	uint32_t w = 720, h = 480;
	if (gui) gui->get_size(gPlugin, &w, &h);

	Window win; // win.gui stays null until the editor exists, so early CONFIGUREs are not forwarded

	PuglWorld* world = puglNewWorld(PUGL_PROGRAM, 0);
	puglSetWorldString(world, PUGL_CLASS_NAME, "MidiLangsDevHost");
	PuglView* view = puglNewView(world);
	puglSetHandle(view, &win);
	puglSetBackend(view, puglStubBackend());
	puglSetEventFunc(view, OnEvent);
	puglSetViewString(view, PUGL_WINDOW_TITLE, gPlugin->desc->name);
	puglSetViewHint(view, PUGL_RESIZABLE, 1);
	puglSetSizeHint(view, PUGL_DEFAULT_SIZE, w, h);
	if (puglRealize(view) != PUGL_SUCCESS) {
		fprintf(stderr, "cannot open window\n");
		gRunning = false;
		if (processing.joinable()) processing.join();
		if (audio) ma_device_uninit(&device);
		return 1;
	}
	const double scale = HostUnitsToPixels(view);
	puglSetSizeHint(view, PUGL_CURRENT_SIZE, (unsigned)(w * scale), (unsigned)(h * scale));
	puglShow(view, PUGL_SHOW_RAISE);

	clap_window_t parent{WindowApi(), {}};
#if defined(__APPLE__)
	parent.cocoa = (void*)puglGetNativeView(view);
#elif defined(_WIN32)
	parent.win32 = (void*)puglGetNativeView(view);
#else
	parent.x11 = (clap_xwnd)puglGetNativeView(view);
#endif
	if (gui && gui->create(gPlugin, WindowApi(), false) && gui->set_parent(gPlugin, &parent)) {
		gui->show(gPlugin);
		win.gui = gui;
	} else {
		fprintf(stderr, "plugin editor failed to open\n");
	}

	auto metered = std::chrono::steady_clock::now();
	while (win.open) {
		puglUpdate(world, 0.005);
		const auto now = std::chrono::steady_clock::now();
		if (meter && audio && now - metered >= std::chrono::seconds(1)) {
			metered = now;
			printf("peak %.4f\n", gPeak.exchange(0.f));
			fflush(stdout);
		}
		auto* timers = (const clap_plugin_timer_support_t*)gPlugin->get_extension(gPlugin, CLAP_EXT_TIMER_SUPPORT);
		for (size_t i = 0; i < gTimers.size(); ++i) { // on_timer may unregister timers
			if (now < gTimers[i].due) continue;
			gTimers[i].due = now + gTimers[i].period;
			if (timers) timers->on_timer(gPlugin, gTimers[i].id);
		}
		if (gCallbackRequested.exchange(false)) gPlugin->on_main_thread(gPlugin);
	}

	if (win.gui) win.gui->destroy(gPlugin);
	puglFreeView(view);
	puglFreeWorld(world);
	gRunning = false;
	if (processing.joinable()) processing.join();
	if (audio) {
		ma_device_uninit(&device); // the callback has returned
		if (gProcessing) gPlugin->stop_processing(gPlugin);
	}
	gPlugin->deactivate(gPlugin);
	gPlugin->destroy(gPlugin);
	entry->deinit();
	// All notes off on every channel, in case the plugin was stopped mid-note.
	for (uint8_t ch = 0; ch < 16; ++ch) {
		const uint8_t off[3] = {(uint8_t)(0xB0 | ch), 123, 0};
		libremidi_midi_out_send_message(gMidiOut, off, 3);
	}
	libremidi_midi_out_free(gMidiOut);
	for (auto* p : gPorts) libremidi_midi_out_port_free(p);
	return 0;
}

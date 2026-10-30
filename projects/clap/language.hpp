#pragma once

// One interpreter behind a CLAP plugin. Each language links plugin.cpp with an adapter that
// defines kLanguage. All Language calls happen on the plugin's evaluator thread, after a
// midi_sink (midi_clock.h) is installed, so the interpreter's sends and sleeps use script time.

#include "editor_logic.hpp"

#include <cstdint>
#include <memory>
#include <string>

class Language {
public:
	virtual ~Language() = default;
	// Before each eval: whether it runs live or as a clip, which must end. A language whose
	// code never ends by design, such as funk-midi's patterns, bounds a clip itself.
	virtual void setLive(bool live) { (void)live; }
	// Runs code, appending to out what it prints, its result, and any error.
	virtual void eval(const std::string& code, std::string& out) = 0;
	// Earliest wake time, in script ms, of a voice waiting in script time; false if none waits.
	virtual bool nextWake(double* ms) = 0;
	// Resumes the voices due at or before ms, appending their output to out.
	virtual void resume(double ms, std::string& out) = 0;
	virtual void stopVoices() = 0;
};

// An instrument's audio, for a language that renders as well as sends notes. activate and
// deactivate run on the host's main thread, note and render on its audio thread; neither
// runs on the evaluator thread.
struct AudioOutput {
	uint32_t channels; // of the one output port
	bool (*activate)(double rate, uint32_t maxFrames);
	void (*deactivate)();
	// Each note-on or note-off of the block, offset frames into it, in time order, before
	// render: the ones the plugin sends and the ones the host sends to its note input.
	void (*note)(uint32_t offset, const uint8_t* data);
	void (*render)(float* const* out, uint32_t channels, uint32_t frames);
};

struct LanguageInfo {
	const char* id;          // CLAP plugin id
	const char* name;        // e.g. "lua-midi"
	const char* description;
	const char* windowClass; // pugl window class, unique per plugin binary
	const char* udpPortEnv;  // turns UDP on at load and sets the port
	uint16_t udpPort;
	EvalSyntax syntax;
	const char* defaultCode;
	std::unique_ptr<Language> (*create)(); // returns nullptr if the interpreter cannot start
	const AudioOutput* audio;              // nullptr: a note effect, without audio
};

extern const LanguageInfo kLanguage;

#pragma once

// An interpreter behind a CLAP plugin, one Language per plugin instance. Each language links
// plugin.cpp with an adapter that defines kLanguage. Language calls but the audio ones happen on
// the instance's evaluator thread, after a midi_sink (midi_clock.h) is installed, so the
// interpreter's sends and sleeps use script time.

#include "editor_logic.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

	// An instrument's audio, if LanguageInfo::audioChannels is not 0. audioActivate and
	// audioDeactivate run on the host's main thread, audioNote and audioRender on its audio
	// thread; none runs on the evaluator thread.
	virtual bool audioActivate(double rate, uint32_t maxFrames) { (void)rate, (void)maxFrames; return true; }
	virtual void audioDeactivate() {}
	// Each note-on or note-off of the block, offset frames into it, in time order, before
	// audioRender: the ones the plugin sends and the ones the host sends to its note input.
	virtual void audioNote(uint32_t offset, const uint8_t* data) { (void)offset, (void)data; }
	virtual void audioRender(float* const* out, uint32_t channels, uint32_t frames)
	{
		for (uint32_t c = 0; c < channels; c++)
			for (uint32_t i = 0; i < frames; i++) out[c][i] = 0.f;
	}
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
	uint32_t audioChannels;                // of the one output port; 0: a note effect, without audio
	// The editor's syntax colours: a Token per byte of text, on the host's main thread; nullptr
	// for plain text.
	void (*highlight)(const std::string& text, std::vector<uint8_t>& tokens);
};

extern const LanguageInfo kLanguage;

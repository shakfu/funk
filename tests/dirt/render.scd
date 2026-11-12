// render.scd -- SuperDirt's events rendered by scsynth in non-real time,
// for tests/dirt/oracle.py. Run: sclang render.scd CASES OUTDIR. CASES
// evaluates to [seed, [[name, wav, frames, rate, channels, [[time, event],
// ...], seconds, core, synth], ...]], synth "" for a sample. Each case
// starts 16 orbits as DirtOrbit.initNodeTree does, each with SuperDirt's
// own GlobalDirtEffects; then each event, of the sample or the synth, runs
// DirtEvent's own orderTimeSpan, calcTimeSpan, finaliseParameters and
// playSynths (DirtEvent.sc), with SuperDirt's modules, in an event made as
// DirtOrbit and DirtSoundLibrary make it. A stand-in server writes what
// they send into a Score, which scsynth renders with SuperDirt's
// SynthDefs, its random generator seeded by RandSeed. A core case, and
// those after it, have dirt_delay as SuperDirt makes it without sc3-plugins:
// core-synths-global.scd, its plugin check false. Writes OUTDIR/NAME.wav,
// float, 2 channels.
(
var args = thisProcess.argv, given = args[0].load, outDir = args[1], seed = given[0], cases = given[1];
var rate = 44100, top = 900, dirt, seeder, core = false;
// without the SuperDirt quark, exit 2: oracle.py skips
if(\SuperDirt.asClass.isNil) { "render.scd: no SuperDirt class".postln; 2.exit };
// SuperDirt.new loads its SynthDefs and modules, and needs no server for
// it. Its classes go by symbol: a missing class would stop the file compiling.
dirt = \SuperDirt.asClass.new(2, Server.default);
seeder = SynthDef(\oracle_seed, { RandSeed.ir(1, \seed.ir(0)) });
fork { try {
	var c = Condition.new;
	cases.do { |k|
		var name = k[0], wav = k[1], frames = k[2], fileRate = k[3], chans = k[4], events = k[5], secs = k[6], synth = k[8];
		var score = [], t = 0, nodes = 1000, server, orbits, defs;
		if(k[7] and: { core.not }) {
			var path = \SuperDirt.asClass.filenameSymbol.asString.dirname.dirname +/+ "synths/core-synths-global.scd";
			(dirt: dirt).use { File.readAllString(path).replace("\\SwitchDelay.asClass.notNil", "false").interpret };
			core = true
		};
		// a server that adds each message to the score at time t
		server = (know: true, sendMsg: { |self ...msg| score = score.add([t, msg]) },
			makeBundle: { |self, latency, func| func.value }, nextNodeID: { nodes = nodes + 1 });
		// 16 orbits, as DirtOrbit makes them: each orbit's group at the head
		// of the dirt group, its effects after it, so the last orbit runs
		// first. IdentityDictionaries: an Event's own group method would hide the key.
		orbits = (0..15).collect { |o|
			var g = server.nextNodeID, dry = 16 + (6 * o), effect = dry + 2;
			var fx = [
				\GlobalDirtEffect.asClass.new(\dirt_delay, [\delaytime, \delayfeedback, \delaySend, \delayAmp, \lock, \cps], 2),
				\GlobalDirtEffect.asClass.new(\dirt_reverb, [\size, \room, \dry], 2),
				\GlobalDirtEffect.asClass.new(\dirt_leslie, [\leslie, \lrate, \lsize], 2),
				\GlobalDirtEffect.asClass.new(\dirt_rms, [\rmsReplyRate, \rmsPeakLag], 2).alwaysRun_(true),
				\GlobalDirtEffect.asClass.new(\dirt_monitor, [\limitertype], 2).alwaysRun_(true)
			];
			server.sendMsg(\g_new, g, 0, top);
			fx.reverseDo { |x| x.play(Group.basicNew(server, g), 0, dry, effect, o) };
			IdentityDictionary(know: true).putPairs([globalEffects: fx, group: g,
				dirt: IdentityDictionary(know: true).put(\group, top),
				synthBus: Bus(\audio, dry + 4, 2, Server.default), dryBus: Bus(\audio, dry, 2, Server.default)])
		};
		events.do { |te|
			// DirtOrbit.makeDefaultParentEvent's values (DirtOrbit.sc), then
			// DirtSoundLibrary.makeEventForBuffer's, then the case's
			var orbit = orbits[te[1][\orbit] ? 0];
			var parent = (cps: 1.0, offset: 0.0, begin: 0.0, end: 1.0, speed: 1.0, pan: 0.5, gain: 1.0, overgain: 0.0,
				cut: 0.0, unit: \r, n: \none, octave: 5, midinote: #{ ~note ? ~n + (~octave * 12) },
				freq: #{ ~midinote.value.midicps }, dur: 1.0, delta: #{ ~dur.value }, latency: 0.0, lag: 0.0,
				length: 1.0, loop: 1.0, dry: 0.0, lock: 0, amp: 0.4, fadeTime: 0.001, dirt: dirt,
				out: orbit.synthBus.index, numChannels: 2, server: server);
			// DirtSoundLibrary's event for the buffer, or getEvent's for a SynthDef
			var proto = if(synth.isEmpty) {
				(buffer: 0, unitDuration: { frames / fileRate * 60.midicps / ~freq.value }, hash: 1, note: 0,
					instrument: format("dirt_sample_%_2", chans).asSymbol)
			} { (instrument: synth.asSymbol, hash: 1) };
			var ev = te[1].copy.parent_(parent).proto_(proto), e = \DirtEvent.asClass.new(orbit, dirt.modules, ev);
			// DirtEvent.splitName: a sound without ":NUMBER" has n 0
			if(synth.notEmpty and: { te[1][\n].isNil }) { ev[\n] = 0.0 };
			t = te[0];
			e.slotPut(\server, server);
			ev.use {
				e.orderTimeSpan;
				e.calcTimeSpan;
				e.finaliseParameters;
				// DirtEvent.play drops an event shorter than minSustain, 8 frames
				if(~sustain >= (8 / rate)) { e.playSynths }
			}
		};
		defs = score.select { |m| [\s_new, 9].includes(m[1][0]) }.collect { |m| m[1][1].asSymbol }.asSet.asArray.sort;
		score = [[0.0, [\b_allocRead, 0, wav]], [0.0, [\d_recv, seeder.asBytes]]]
			++ defs.collect { |d| [0.0, [\d_recv, SynthDescLib.global[d].def.asBytes]] }
			++ [[0.0, [\g_new, top, 0, 0]], [0.0, [\s_new, \oracle_seed, -1, 0, 0, \seed, seed]]]
			++ score ++ [[secs, [\c_set, 0, 0]]];
		Score.recordNRT(score, outDir +/+ name ++ ".osc", outDir +/+ name ++ ".wav", nil, rate, "WAV", "float",
			// memSize: 16 orbits' delay lines need some 30 MB, over scsynth's default 8 MB
			ServerOptions.new.numOutputBusChannels_(2).numInputBusChannels_(0).memSize_(8192 * 64), "", secs, { c.unhang });
		c.hang;
	};
	0.exit;
} { |error| error.reportError; 1.exit } };
)

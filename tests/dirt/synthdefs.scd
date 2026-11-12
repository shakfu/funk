// synthdefs.scd -- SuperDirt's synths as funk data: lib/synthdefs.fk.
// Run: sclang synthdefs.scd OUT NAME... Loads SuperDirt (its extra synths
// need sc3-plugins), then writes each named SynthDef's UGen graph as
// scsynth runs it: its controls, by index, with defaults; and its UGens
// in order, each with its class, rate, special index, number of outputs
// and inputs, a constant or an earlier UGen's output.
(
var args = thisProcess.argv, out = args[0], names = args[1..], f, num, rate;
if(\SuperDirt.asClass.isNil) { "synthdefs.scd: no SuperDirt class".postln; 2.exit };
// SuperDirt's SynthDefs draw constants from sclang's generator as they
// are built (superhammond's phases, superfm's presets): seeded, as
// render.scd seeds it, they are the same each run
thisThread.randSeed = 1234;
\SuperDirt.asClass.new(2, Server.default);
// a float32 as funk reads it: every digit of the double it widens to
num = { |x| var y = Float.from32Bits(x.asFloat.as32Bits), s = y.asStringPrec(17);
	case
	{ y == inf } { "(1.0 / 0.0)" }
	{ y == -inf } { "(-1.0 / 0.0)" }
	{ if(s.includes($.).not and: { s.includes($e).not }) { s = s ++ ".0" }; if(y < 0) { "(" ++ s ++ ")" } { s } } };
try {
rate = { |r| (scalar: 0, control: 1, audio: 2, demand: 3)[r] };
f = File(out, "w");
f.write("module synthdefs where\n\n");
f.write("-- synthdefs.fk -- SuperDirt's synths, written by tests/dirt/synthdefs.scd from\n");
f.write("-- their SynthDefs; do not edit. lib/scsynth.fk builds them. Written by:\n");
f.write("--   sclang tests/dirt/synthdefs.scd lib/synthdefs.fk " ++ names.join(" ") ++ "\n\n");
f.write("-- an input: a constant, or UGen i's output k\n");
f.write("data In = K Float | U Int Int\n\n");
f.write("-- a UGen: its class, rate (0 scalar, 1 control, 2 audio, 3 demand),\n");
f.write("-- special index, number of outputs, and inputs\n");
f.write("data UGen = UGen String Int Int Int [In]\n\n");
f.write("-- a SynthDef: its controls by index, each a name and default, and its UGens in order\n");
f.write("data Def = Def [(String, Float)] [UGen]\n\n");
f.write("synthDefs :: [(String, Def)]\nsynthDefs = [\n");
names.do { |name, n|
	var def = (SynthDescLib.global[name.asSymbol] ?? { Error("no SynthDef " ++ name).throw }).def, controls = [];
	def.allControlNames.do { |c|
		var d = c.defaultValue.asArray;
		d.do { |v, k| controls = controls.add([c.name.asString ++ if(d.size > 1) { k.asString } { "" }, c.index + k, v]) }
	};
	controls = controls.sort { |a, b| a[1] < b[1] };
	f.write("  (\"" ++ name ++ "\", Def [" ++ controls.collect { |c| "(\"" ++ c[0] ++ "\", " ++ num.(c[2]) ++ ")" }.join(", ") ++ "]\n    [");
	f.write(def.children.collect { |u|
		var ins = (u.inputs ? []).collect { |i|
			case
			{ i.isKindOf(OutputProxy) } { "U " ++ i.source.synthIndex ++ " " ++ i.outputIndex }
			{ i.isKindOf(UGen) } { "U " ++ i.synthIndex ++ " 0" }
			{ "K " ++ num.(i) }
		};
		"UGen \"" ++ u.class.name ++ "\" " ++ rate.(u.rate) ++ " " ++ u.specialIndex ++ " " ++ u.numOutputs ++ " [" ++ ins.join(", ") ++ "]"
	}.join(",\n     "));
	f.write("])" ++ if(n < (names.size - 1)) { "," } { "" } ++ "\n");
};
f.write("  ]\n");
f.close;
0.exit;
} { |error| error.reportError; 1.exit };
)

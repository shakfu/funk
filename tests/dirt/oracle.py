"""oracle.py -- funk's sample and synth events against scsynth's (docs/dev/dirt-sound.md).

Writes two test samples, renders each case with SuperDirt's SynthDefs in
scsynth (render.scd) and with funk's event layer (play.fk), each with 16
orbits of SuperDirt's global effects, the random generator seeded with
SEED, and compares every frame of both channels. Events start LEAD in,
after the effects' first block has paused them. A case passes within
TOLERANCE, or FFT_TOLERANCE for the FFT effects; a negative control,
funk's output a frame late, must fail it,
so the bound separates a right render from a wrong one. Exits 77, ctest's
skip, if sclang lacks the SuperDirt quark.

With $ORACLE_SET=patches, the cases are tests/tidal/ports.fk's patches
instead, each event's sample from Dirt-Samples as SuperDirt picks it.

Usage: python3 -I oracle.py SCLANG FUNK KERNELS OUTDIR -- FUNKARGS...
"""
import math
import os
import struct
import subprocess
import sys

RATE = 44100
TOLERANCE = 1e-6  # absolute, at samples of up to 0.4
# scsynth's macOS FFT is vDSP's, whose float rounding funk cannot match: a
# bin it rounds across a step of SC_Complex.h's polar table differs by 1e-3
# (docs/dev/dirt-sound.md, difference 13)
FFT_TOLERANCE = 2e-5
FFT_CASES = ["freeze", "freezeOff", "comb", "smear", "scram", "binshift", "hbrick", "lbrick", "conformer", "enhance"]
SEED = 1234
LEAD = 256 / RATE
TAIL = 0.03  # past a case's seconds: dirt_monitor's Limiter is 881 frames late

# name, sample, seconds, controls: Tidal's, as funk sends them to SuperDirt;
# or a list of (seconds, controls), one per event; then True to render with
# dirt_delay's CombL, as SuperDirt makes it without sc3-plugins
CASES = [
    ("plain", "mono", 0.2, {}),
    ("speed", "mono", 0.2, {"speed": 2.0, "pan": 0.2}),
    ("backwards", "mono", 0.2, {"speed": -1.0}),
    ("part", "mono", 0.2, {"begin": 0.25, "end": 0.75}),
    ("accelerate", "mono", 0.2, {"accelerate": 1.0}),
    ("note", "mono", 0.3, {"note": 3.0, "speed": 0.73}),
    ("loop", "mono", 0.3, {"loop": 2.0}),
    ("sustain", "mono", 0.2, {"sustain": 0.05}),
    ("legato", "mono", 0.2, {"legato": 0.5, "delta": 0.2}),
    # cps 2.5: play.fk's cycle is a whole number of microseconds
    ("unitc", "mono", 0.5, {"unitc": 1.0, "cps": 2.5, "speed": 1.0}),
    # unit "s": the span's length in seconds (DirtEvent.sc), the 0.11 s sample looping
    ("units", "mono", 0.4, {"units": 1.0, "begin": 0.1, "end": 0.4}),
    ("unitsLoop", "stereo", 0.5, {"units": 1.0, "end": 0.2, "loop": 2.0, "speed": 1.5}),
    ("unitsSustain", "mono", 0.4, {"units": 1.0, "end": 0.3, "sustain": 0.12}),
    ("fade", "mono", 0.2, {"begin": 0.1, "sustain": 0.08, "fadeTime": 0.01}),
    ("gain", "mono", 0.2, {"gain": 1.2, "amp": 0.5}),
    ("stereo", "stereo", 0.2, {"pan": 0.8}),
    ("stereoSpeed", "stereo", 0.2, {"speed": 1.37, "pan": 0.35}),
    # the second cuts the first; it starts 2205 frames in, off the 64-frame grid
    ("cut", "mono", 0.2, [(0.0, {"cut": 1.0}), (0.05, {"cut": 1.0, "speed": 1.5})]),
    ("cutOther", "mono", 0.2, [(0.0, {"cut": 2.0}), (0.03, {"cut": 1.0})]),
    # SuperDirt's core modules (core-modules.scd)
    ("lpf", "mono", 0.2, {"cutoff": 800.0, "resonance": 0.3}),
    ("lpfResonant", "mono", 0.2, {"cutoff": 3000.0, "resonance": 0.9}),
    ("hpf", "mono", 0.2, {"hcutoff": 1200.0, "hresonance": 0.5}),
    ("bpf", "mono", 0.2, {"bandf": 1500.0, "bandq": 4.0}),
    ("bpfNoQ", "mono", 0.2, {"bandf": 400.0}),
    ("shape", "mono", 0.2, {"shape": 0.6}),
    ("crush", "mono", 0.2, {"crush": 4.0}),
    ("coarse", "mono", 0.2, {"coarse": 5.0}),
    ("vowel", "mono", 0.2, {"vowel": "a", "resonance": 0.4}),
    ("vowelO", "stereo", 0.2, {"vowel": "o"}),
    ("tremolo", "mono", 0.2, {"tremolorate": 30.0, "tremolodepth": 0.8}),
    ("phaser", "mono", 0.2, {"phaserrate": 20.0, "phaserdepth": 0.7}),
    ("envelope", "mono", 0.2, {"attack": 0.01, "hold": 0.02, "release": 0.03, "curve": -3.0}),
    # curve 0, as nil sends it: linear, longer than a block
    ("envelopeLinear", "mono", 0.2, {"attack": 0.02, "release": 0.05}),
    ("grenvelo", "mono", 0.2, {"tilt": 0.3, "plat": 0.2, "curve": 2.0}),
    ("pshift", "mono", 0.2, {"psrate": 1.5}),
    ("pshiftDown", "stereo", 0.2, {"psrate": 0.7, "speed": 0.8}),
    # a 24-bit AIFF, at a third rate (S7)
    ("aiff", "aiff", 0.2, {"speed": 1.3, "pan": 0.3}),
    ("aiffLoop", "aiff", 0.3, {"begin": 0.2, "end": 0.6, "loop": 2.0, "cutoff": 900.0}),
    ("chain", "mono", 0.2, {"cutoff": 2000.0, "shape": 0.3, "crush": 6.0, "hcutoff": 200.0, "tremolorate": 7.0}),
    # default-effects-extra.scd's modules
    ("waveloss", "mono", 0.2, {"waveloss": 40.0}),
    ("squiz", "mono", 0.2, {"squiz": 2.5}),
    ("fshift", "mono", 0.2, {"fshift": 150.0}),
    ("fshiftNote", "stereo", 0.2, {"fshift": 20.0, "fshiftnote": 0.5, "note": 3.0}),
    ("triode", "mono", 0.2, {"triode": 0.5}),
    ("krush", "mono", 0.2, {"krush": 0.6, "kcutoff": 2000.0}),
    ("krushDefault", "stereo", 0.2, {"krush": 0.3}),
    ("octer", "mono", 0.2, {"octer": 0.6, "octersub": 0.5, "octersubsub": 0.3}),
    ("ring", "mono", 0.2, {"ring": 0.7, "ringf": 300.0, "ringdf": 200.0}),
    ("distort", "mono", 0.2, {"distort": 0.5}),
    ("spectralDelay", "mono", 0.3, {"tsdelay": 0.6, "xsdelay": 5.0}),
    ("spectralDelayX", "stereo", 0.3, {"xsdelay": 11.0}),
    ("freeze", "mono", 0.3, {"freeze": 1.0}),
    ("freezeOff", "stereo", 0.3, {"freeze": 0.0}),
    ("comb", "mono", 0.3, {"comb": 0.3}),
    ("smear", "mono", 0.3, {"smear": 0.5}),
    ("scram", "mono", 0.3, {"scram": 0.4}),
    ("binshift", "mono", 0.3, {"binshift": 0.3}),
    ("hbrick", "mono", 0.3, {"hbrick": 0.4}),
    ("lbrick", "stereo", 0.3, {"lbrick": 0.6}),
    ("conformer", "mono", 0.3, {"real": 0.3, "imag": 0.4}),
    ("enhance", "mono", 0.3, {"enhance": 0.5}),
    ("djf", "mono", 0.2, {"djf": 0.3}),
    ("djfHigh", "stereo", 0.2, {"djf": 0.8}),
    # SuperDirt's global effects (core-synths-global.scd)
    ("room", "mono", 0.6, {"room": 0.4, "size": 0.6}),
    ("roomDry", "stereo", 0.6, {"room": 0.3, "size": 0.9, "dry": 0.5}),
    ("delay", "mono", 0.5, {"delay": 0.6, "delaytime": 0.05, "delayfeedback": 0.5}),
    ("delayLock", "mono", 0.6, {"delay": 0.5, "delaytime": 0.5, "lock": 1.0, "cps": 2.5}),
    ("delayMove", "mono", 0.5, [(0.0, {"delay": 0.5, "delaytime": 0.05, "delayfeedback": 0.3}),
                                (0.1, {"delay": 0.5, "delaytime": 0.08, "delayfeedback": 0.3})]),
    ("leslie", "stereo", 0.4, {"leslie": 0.8, "lrate": 6.7, "lsize": 0.3}),
    ("leslieFast", "mono", 0.4, [(0.0, {"leslie": 0.5, "lrate": 0.7}), (0.15, {"leslie": 0.5, "lrate": 9.0, "lsize": 0.5})]),
    ("limiter", "mono", 0.2, {"gain": 1.6, "amp": 1.0}),
    ("softclip", "mono", 0.2, {"gain": 1.5, "limitertype": 2.0}),
    ("orbit", "mono", 0.4, [(0.0, {"orbit": 1.0, "room": 0.3}), (0.05, {"room": 0.5, "speed": 1.5})]),
    ("pshiftRoom", "mono", 0.4, {"psrate": 1.5, "psdisp": 0.01, "room": 0.3}),
    # the reverb pauses after 4 s of silence; the second event resumes it
    ("pause", "mono", 4.7, [(0.0, {"room": 0.3}), (4.5, {"room": 0.2})]),
    # SuperDirt's synths (default-synths.scd): a sound not in SAMPLES
    ("dirac", "dirac", 0.1, {"pan": 0.3}),
    ("imp", "imp", 0.3, {"note": 5.0, "sustain": 0.2}),
    ("impAccelerate", "imp", 0.3, {"n": 2.0, "accelerate": 1.5, "begin": 0.2, "speed": 1.3}),
    ("psin", "psin", 0.3, {"note": -7.0, "sustain": 0.25, "modfreq": 30.0}),
    ("gabor", "gabor", 0.1, {"note": 12.0, "pan": 0.8}),
    ("gaborLong", "gabor", 0.2, {"sustain": 0.1, "width": 0.2}),
    ("cyclo", "cyclo", 0.3, {"note": 2.0, "cycle": 3.0, "sustain": 0.2, "ratio": 1.5}),
    # default-synths-extra.scd's
    ("supergong", "supergong", 0.3, {"note": -5.0, "sustain": 0.25, "voice": 0.5, "decay": 0.7}),
    ("superkick", "superkick", 0.3, {"n": 2.0, "sustain": 0.25, "pitch1": 1.3, "decay": 0.8}),
    ("superhat", "superhat", 0.2, {"n": 3.0, "sustain": 0.15, "accelerate": 0.5}),
    ("superclap", "superclap", 0.3, {"n": 1.0, "sustain": 0.25, "delay": 1.5, "pitch1": 0.9}),
    ("supersnare", "supersnare", 0.3, {"n": 4.0, "sustain": 0.25, "decay": 0.6}),
    ("super808", "super808", 0.3, {"n": 2.0, "sustain": 0.25, "voice": 0.4, "rate": 1.5}),
    ("supersiren", "supersiren", 0.3, {"note": 3.0, "sustain": 0.25, "accelerate": 0.3}),
    ("superfork", "superfork", 0.3, {"note": 7.0, "sustain": 0.2, "pan": 0.6}),
    ("soskick", "soskick", 0.3, {"sustain": 0.25, "pitch1": 40.0, "voice": 1.5, "pitch2": 0.3, "speed": 0.4}),
    ("soshats", "soshats", 0.2, {"sustain": 0.15, "resonance": 0.6}),
    ("sostoms", "sostoms", 0.3, {"sustain": 0.25, "voice": 0.8}),
    ("sossnare", "sossnare", 0.3, {"sustain": 0.25, "voice": 0.5, "resonance": 0.3, "semitone": 0.6}),
    ("supermandolin", "supermandolin", 0.3, {"note": 4.0, "sustain": 0.25, "detune": 0.4}),
    ("superzow", "superzow", 0.3, {"note": -3.0, "sustain": 0.25, "slide": 2.0, "decay": 0.3}),
    ("superstatic", "superstatic", 0.2, {"note": -12.0, "sustain": 0.15}),
    ("supertron", "supertron", 0.3, {"note": -12.0, "sustain": 0.25, "voice": 0.5, "detune": 0.3}),
    ("superreese", "superreese", 0.3, {"note": -12.0, "sustain": 0.25, "voice": 0.7, "detune": 0.5}),
    ("superhoover", "superhoover", 0.3, {"note": -5.0, "sustain": 0.25, "slide": 0.5, "decay": 0.2}),
    ("supervibe", "supervibe", 0.3, {"note": 5.0, "sustain": 0.25, "velocity": 0.8, "decay": 0.5}),
    ("supersquare", "supersquare", 0.3, {"note": -5.0, "sustain": 0.25, "voice": 0.3, "resonance": 0.4}),
    ("supercomparator", "supercomparator", 0.3, {"note": -5.0, "sustain": 0.25, "voice": 0.8, "resonance": 0.3}),
    ("superhammond", "superhammond", 0.3, {"note": 2.0, "sustain": 0.25, "voice": 3.0, "perc": 0.5, "decay": 0.3}),
    ("supersaw", "supersaw", 0.3, {"note": -5.0, "sustain": 0.25, "voice": 0.6, "resonance": 0.3}),
    ("supernoise", "supernoise", 0.3, {"note": -5.0, "sustain": 0.25, "voice": 0.3, "resonance": 0.5, "slide": 0.5}),
    ("supernoiseBrown", "supernoise", 0.3, {"note": 2.0, "sustain": 0.25, "voice": 0.8, "accelerate": 0.3}),
    ("superwavemechanics", "superwavemechanics", 0.3, {"note": -5.0, "sustain": 0.25, "voice": 0.5, "resonance": 0.3, "detune": 0.5}),
    ("superprimes", "superprimes", 0.3, {"note": -5.0, "sustain": 0.25, "voice": 0.5, "detune": 0.3, "rate": 1.5}),
    ("supergrind", "supergrind", 0.3, {"note": -5.0, "sustain": 0.25, "voice": 0.5, "detune": 0.3, "rate": 2.0}),
    ("superpwm", "superpwm", 0.3, {"note": -5.0, "sustain": 0.25, "voice": 0.4, "decay": 0.2}),
    ("superfm", "superfm", 0.3, {"note": -5.0, "sustain": 0.25, "voice": 2.0, "lfodepth": 0.1, "lfofreq": 5.0}),
    ("superfmUser", "superfm", 0.3, {"note": 3.0, "sustain": 0.25, "voice": 0.0, "amp1": 1.0, "amp2": 0.0, "mod11": 0.5, "mod12": 0.78, "ratio2": 2.0, "feedback": 1.0}),
    ("superhex", "superhex", 0.3, {"note": 5.0, "sustain": 0.25, "rate": 2.0}),
    ("superhexAccelerate", "superhex", 0.3, {"note": -7.0, "sustain": 0.25, "accelerate": 0.5, "pan": 0.7}),
    ("superchip", "superchip", 0.3, {"note": 5.0, "sustain": 0.25, "voice": 0.3}),
    ("superchipSlide", "superchip", 0.3, {"note": -7.0, "sustain": 0.25, "slide": 0.5, "rate": 2.0, "accelerate": 0.3}),
    ("superpiano", "superpiano", 0.3, {"note": 5.0, "sustain": 0.25}),
    ("superpianoLow", "superpiano", 0.3, {"note": -26.0, "sustain": 0.25, "velocity": 0.4, "muffle": 0.3, "pan": 0.2}),
    ("superpianoHigh", "superpiano", 0.3, {"note": 38.0, "sustain": 0.25, "stereo": 0.7, "detune": 0.6, "accelerate": 0.4}),
    ("supertronFeedback", "supertron", 0.4, {"note": -17.0, "sustain": 0.35, "voice": 1.0, "detune": 1.0}),
    # last: render.scd keeps the core delay for the cases after a core one
    ("combDelay", "mono", 0.5, {"delay": 0.6, "delaytime": 0.05, "delayfeedback": 0.5}, True),
    ("combDelayMove", "mono", 0.5, [(0.0, {"delay": 0.5, "delaytime": 0.05, "delayfeedback": 0.3}),
                                    (0.1, {"delay": 0.5, "delaytime": 0.08, "delayfeedback": 0.8})], True),
]

# each case as (name, sample, seconds, events, core); $ORACLE_CASES, names
# separated by commas, runs only those; $ORACLE_SHARD, "I/N", every Nth from
# the Ith, so that ctest runs the shards in parallel
SHARD, SHARDS = map(int, os.environ.get("ORACLE_SHARD", "0/1").split("/"))


def select(cases):
    return [(c[0], c[1], c[2], c[3], len(c) > 4 and c[4]) for i, c in enumerate(cases)
            if (not os.environ.get("ORACLE_CASES") or c[0] in os.environ["ORACLE_CASES"].split(",")) and i % SHARDS == SHARD]


# $ORACLE_SET=patches: tests/tidal/ports.fk's patches (tests/dirt/patches.fk),
# with Dirt-Samples from $DIRT_SAMPLES or SuperCollider's downloaded quarks.
# A case's sample "*" takes each event's s and n.
PATCH_TAIL = 1.0  # seconds past the last cycle, for the tails to ring
DIRT_SAMPLES = [os.path.expanduser(p) for p in ("~/Library/Application Support/SuperCollider/downloaded-quarks/Dirt-Samples",
                                                "~/.local/share/SuperCollider/downloaded-quarks/Dirt-Samples")]


def dirt_samples():
    for d in [os.environ.get("DIRT_SAMPLES", "")] + DIRT_SAMPLES:
        if d and os.path.isdir(d):
            return d
    return None


def patch_cases(funk, funk_args, dirt):
    """The patches' events as cases; events of a sound with no folder are
    dropped, as SuperDirt drops a sound it does not find."""
    r = subprocess.run([funk, "run", "tests/dirt/patches.fk"] + funk_args + ["-M", "tests/tidal"], capture_output=True, text=True, timeout=600)
    if r.returncode:
        sys.exit("patches.fk failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    by, dropped = {}, {}
    for line in r.stdout.splitlines():
        head, *pairs = line.split("\t")
        _, name, t, delta, cycle = head.split(" ")
        cs = {"cps": 0.5625, "delta": float(delta)}
        for kv in pairs:
            k, v = kv.split("=", 1)
            cs[k] = v[1:] if v[0] == "s" else float(v[1:])
        if not sound_files(dirt, cs["s"]):
            dropped[cs["s"]] = dropped.get(cs["s"], 0) + 1
            continue
        by.setdefault(name, []).append((float(t), cs))
    for s, k in sorted(dropped.items()):
        print("dropped %d events of %s: no such sample folder" % (k, s))
    return [("patch-" + n, "*", max(t for t, c in es) + PATCH_TAIL, sorted(es, key=lambda e: e[0])) for n, es in by.items()]


def sound_files(dirt, folder):
    """A folder's sound files as DirtSoundLibrary.loadSoundFileFolder loads them: sorted, by extension."""
    d = os.path.join(dirt, folder)
    if not os.path.isdir(d):
        return []
    return [os.path.join(d, f) for f in sorted(os.listdir(d)) if not f.startswith(".")
            and os.path.splitext(f)[1][1:].lower() in ("wav", "aif", "aiff", "aifc")]


def sound_info(path):
    """A WAV or AIFF file's frames, rate and channels."""
    b = open(path, "rb").read()
    if b[:4] == b"RIFF":
        at, chans, rate, align, size = 12, 0, 0, 1, 0
        while at + 8 <= len(b):
            tag, n = b[at:at + 4], struct.unpack("<I", b[at + 4:at + 8])[0]
            if tag == b"fmt ":
                chans, rate, _, align = struct.unpack("<HIIH", b[at + 10:at + 22])
            elif tag == b"data":
                size = min(n, len(b) - at - 8)
            at += 8 + n + (n & 1)
        return size // align, rate, chans
    at = 12
    while at + 8 <= len(b):
        tag, n = b[at:at + 4], struct.unpack(">I", b[at + 4:at + 8])[0]
        if tag == b"COMM":
            chans, frames = struct.unpack(">hI", b[at + 8:at + 14])
            e, m = struct.unpack(">HQ", b[at + 16:at + 26])
            return frames, round(m * 2.0 ** ((e & 0x7FFF) - 16383 - 63)), chans
        at += 8 + n + (n & 1)
    sys.exit("no sound in " + path)


def layout(case, samples, dirt):
    """A case's buffers for render.scd, each event's buffer, and for play.fk
    the samples' folder, the sample names it loads and each event's sound."""
    name, s, secs, cs, core = case
    es = events(cs)
    if s != "*":
        own = s if s in SAMPLES else "mono"
        return ([(os.path.join(samples, own, sample_file(own)),) + SAMPLES[own]], [0] * len(es), samples,
                list(SAMPLES), [""] * len(es))
    buffers, at, picks, names = [], {}, [], []
    for t, c in es:
        files = sound_files(dirt, c["s"])
        path = files[int(c.get("n", 0.0)) % len(files)]  # getEvent: wrapAt(index.asInteger)
        if path not in at:
            at[path] = len(buffers)
            buffers.append((path,) + sound_info(path))
        picks.append(at[path])
        key = "%s:%d" % (c["s"], int(c.get("n", 0.0)))
        if key not in names:
            names.append(key)
    return buffers, picks, dirt, names, [c["s"] for t, c in es]

# a vowel is sent to funk as its index in VOWELS; play.fk maps it back
VOWELS = "aeiou"


def events(controls):
    return controls if isinstance(controls, list) else [(0.0, controls)]

# the samples: frames, file rate, channels; any other sound is a synth
SAMPLES = {"mono": (5000, 44100, 1), "stereo": (4000, 48000, 2), "aiff": (4500, 22050, 2)}
# the samples written as 24-bit AIFF, a.aif; the others are 16-bit WAV, a.wav
AIFF = ["aiff"]


def signal(i, c):
    """A test signal: two sines, and a little of an LCG's noise."""
    noise = ((i * 1103515245 + 12345 + 977 * c) % 65536) / 65536.0 - 0.5
    return 0.5 * math.sin(2 * math.pi * i * (0.013 + 0.004 * c)) + 0.3 * math.sin(2 * math.pi * i * 0.0711) + 0.05 * noise


def write_wav(path, frames, rate, chans):
    data = b"".join(struct.pack("<h", max(-32768, min(32767, round(32767 * signal(i, c)))))
                    for i in range(frames) for c in range(chans))
    fmt = struct.pack("<HHIIHH", 1, chans, rate, rate * 2 * chans, 2 * chans, 16)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data)


def write_aiff(path, frames, rate, chans):
    data = b"".join(struct.pack(">i", max(-8388608, min(8388607, round(8388607 * signal(i, c)))))[1:]
                    for i in range(frames) for c in range(chans))
    e = rate.bit_length() - 1  # rate as an 80-bit extended
    comm = struct.pack(">hIh", chans, frames, 24) + struct.pack(">HQ", 16383 + e, rate << (63 - e))
    ssnd = struct.pack(">II", 0, 0) + data
    body = b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm + b"SSND" + struct.pack(">I", len(ssnd)) + ssnd
    with open(path, "wb") as f:
        f.write(b"FORM" + struct.pack(">I", len(body)) + body)


def sample_file(name):
    return "a.aif" if name in AIFF else "a.wav"


def read_float_wav(path):
    """Both channels of scsynth's float WAV."""
    b = open(path, "rb").read()
    at, chans, data = 12, 0, b""
    while at + 8 <= len(b):
        tag, size = b[at:at + 4], struct.unpack("<I", b[at + 4:at + 8])[0]
        body = b[at + 8:at + 8 + size]
        if tag == b"fmt ":
            chans = struct.unpack("<H", body[2:4])[0]
        elif tag == b"data":
            data = body
        at += 8 + size + (size & 1)
    xs = struct.unpack("<%df" % (len(data) // 4), data)
    return [xs[c::chans] for c in range(chans)]


def sc_time(t):
    """t for scsynth: a quarter frame after funk's frame, round(t * RATE). An
    OSC time tag is whole 2^-32 s, and scsynth truncates its offset into a
    block, so a time on a frame can start a frame early."""
    return (round(t * RATE) + 0.25) / RATE


def sc_event(controls):
    """A case's controls as an sclang event literal."""
    parts = []
    for k, v in controls.items():
        if k == "unitc":
            parts.append("unit: \\c")
        elif k == "units":
            parts.append("unit: \\s")
        elif isinstance(v, str):
            parts.append("%s: \\%s" % (k, v))
        else:
            parts.append("%s: %r" % (k, v))
    return "(" + ", ".join(parts) + ")"


def funk_cases(controls):
    """A case's controls as funk's: unit "c" is play.fk's unitc, "s" its units, a vowel its index;
    an event's s is its own field."""
    return "[" + ", ".join("(%s, %r)" % (f'"{k}"', float(VOWELS.index(v) if isinstance(v, str) else v)) for k, v in controls.items() if k != "s") + "]"


def read_funk(text, outdir):
    """play.fk's renders, by case name, and each one's CPU nanoseconds."""
    out = {}
    for line in text.splitlines():
        if line.startswith("case "):
            name, cpu = line[5:].rsplit(" ", 1)
            if not cpu.isdigit():
                sys.exit("funk %s case %s" % (cpu, name))
            out[name] = (read_float_wav(os.path.join(outdir, name + ".funk.wav")), int(cpu))
    return out


def worst(sc, fk, shift):
    """The largest difference over both channels, funk's output delayed by
    shift frames, over the frames funk rendered: scsynth's run on to a block's end."""
    err = 0.0
    for c in range(2):
        f = [0.0] * shift + list(fk[c])
        for i in range(len(fk[c])):
            a = sc[c][i] if i < len(sc[c]) else 0.0
            b = f[i] if i < len(f) else 0.0
            err = max(err, abs(a - b))
    return err


def main():
    sep = sys.argv.index("--")
    sclang, funk, kernels, outdir = sys.argv[1:sep]
    funk_args = sys.argv[sep + 1:]
    os.makedirs(outdir, exist_ok=True)
    samples = os.path.join(outdir, "samples")
    for name, (frames, rate, chans) in SAMPLES.items():
        os.makedirs(os.path.join(samples, name), exist_ok=True)
        (write_aiff if name in AIFF else write_wav)(os.path.join(samples, name, sample_file(name)), frames, rate, chans)
    dirt = None
    if os.environ.get("ORACLE_SET") == "patches":
        dirt = dirt_samples()
        if not dirt:
            print("skipped: no Dirt-Samples ($DIRT_SAMPLES)")
            sys.exit(77)
        cases = select(patch_cases(funk, funk_args, dirt))
    else:
        cases = select(CASES)
    layouts = {c[0]: layout(c, os.path.abspath(samples), dirt) for c in cases}
    with open(os.path.join(outdir, "cases.scd"), "w") as f:
        f.write("[%d, [\n" % SEED)
        for name, s, secs, cs, core in cases:
            buffers, picks, d, names, sounds = layouts[name]
            f.write('  ["%s", [%s], [%s], %r, %s, "%s"],\n' % (name,
                ", ".join('["%s", %d, %d, %d]' % (os.path.abspath(p), fr, ra, ch) for p, fr, ra, ch in buffers),
                ", ".join("[%r, %d, %s]" % (sc_time(LEAD + t), b, sc_event(c)) for (t, c), b in zip(events(cs), picks)), LEAD + secs + TAIL,
                "true" if core else "false", "" if s in SAMPLES or s == "*" else s))
        f.write("]]\n")
    with open(os.path.join(outdir, "cases.fk"), "w") as f:
        f.write("module cases where\n\nseed :: Int\nseed = %d\n\n" % SEED)
        f.write("cases :: [(String, String, Float, [(Float, String, [(String, Float)])], Bool, String, [String])]\ncases = [\n")
        f.write(",\n".join('  ("%s", "%s", %r, [%s], %s, "%s", [%s])' % (n, s, LEAD + secs + TAIL,
                            ", ".join('(%r, "%s", %s)' % (LEAD + float(t), snd, funk_cases(c)) for (t, c), snd in zip(events(cs), layouts[n][4])),
                            "True" if core else "False", layouts[n][2], ", ".join('"%s"' % k for k in layouts[n][3]))
                            for n, s, secs, cs, core in cases))
        f.write("]\n")
    r = subprocess.run([sclang, os.path.abspath("tests/dirt/render.scd"), os.path.abspath(os.path.join(outdir, "cases.scd")),
                        os.path.abspath(outdir)], capture_output=True, text=True, timeout=300)
    if r.returncode == 2:
        print("skipped: sclang has no SuperDirt quark")
        sys.exit(77)
    missing = [n for n, s, secs, cs, core in cases if not os.path.exists(os.path.join(outdir, n + ".wav"))]
    if missing:
        sys.exit("sclang rendered no %s:\n%s%s" % (", ".join(missing), r.stdout[-3000:], r.stderr[-3000:]))
    env = dict(os.environ, KERNELS=kernels, OUT=os.path.abspath(outdir))
    r = subprocess.run([funk, "run", "-M", outdir] + funk_args + ["tests/dirt/play.fk"], capture_output=True, text=True, env=env, timeout=600)
    if r.returncode:
        sys.exit("funk failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    got = read_funk(r.stdout, outdir)
    failed = 0
    for name, s, secs, cs, core in cases:
        sc = read_float_wav(os.path.join(outdir, name + ".wav"))
        err, wrong = worst(sc, got[name][0], 0), worst(sc, got[name][0], 1)
        bound = FFT_TOLERANCE if name in FFT_CASES else TOLERANCE
        ok = err <= bound and wrong > bound
        failed += not ok
        print("%-14s %s  error %.3g, a frame late %.3g" % (name, "ok  " if ok else "FAIL", err, wrong))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

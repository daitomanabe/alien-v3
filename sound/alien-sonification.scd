// ALIEN ecosystem sonification
// Receives OSC from alien_server (see source/Server/Main.cpp):
//   /alien/stats      i numCells, i numFreeCells, i numParticles, f totalEnergy, f tps, i timestep
//   /alien/lineage    i id, i numCreatures, i sumCells, f energy, f muscleDelta, f attackDelta, f mutationsDelta, f avgGenerations, i colorBitset
//   /alien/attacks    i totalCount
//   /alien/attack     f x, f y            (world coordinates)
//   /alien/detonation f x, f y, f radius
//
// Sound model:
//   lineage  -> one drone voice per slot (max 8): population = level, generations = pitch,
//               muscle activity = tremolo/shimmer, predation = grit, mutation = detune
//   attack   -> short percussive tick, x = pan, y = register
//   detonation -> low impact
//   stats    -> master low-pass follows total energy; cell count sets reverb size feel

(
// alien_server address: subscribing there makes the server stream OSC back to
// this sclang's port (works through one-way tailnet sharing).
// ~alienServerPort may be preset (e.g. by test-capture.scd argv) to listen to
// another garden: 12000 main / 12020 rain / 12030 islands.
~alienServerAddr = NetAddr(~alienServerHost ? "100.70.183.86", ~alienServerPort ? 12000);

s.options.numInputBusChannels = 0;
if((~quietMode ? false).not, {
	s.options.sampleRate = 44100;
});
// quiet capture never writes to bus 0, so any bootable device stays silent;
// leaving device and rate unset lets the boot follow the current output setup

s.waitForBoot({
	var numSlots = 8;

	// ---------- SynthDefs ----------
	SynthDef(\lineageDrone, {
		|out = 0, freq = 110, amp = 0, pan = 0, shimmer = 0, grit = 0, detune = 0, bright = 1, gate = 1|
		var env, tremolo, saw1, saw2, noise, sig;
		env = EnvGen.kr(Env.asr(3, 1, 4), gate, doneAction: 2);
		tremolo = SinOsc.kr(shimmer.linexp(0, 1, 0.15, 9), Rand(0, 6.28)).range(1 - (shimmer * 0.6), 1);
		saw1 = Saw.ar(freq * (1 + (detune * 0.012 * SinOsc.kr(0.13, Rand(0, 6.28)))));
		saw2 = Saw.ar(freq * (1 + (detune * -0.017 * SinOsc.kr(0.11, Rand(0, 6.28)))) * 0.5);
		noise = PinkNoise.ar(grit * 0.7);
		sig = (saw1 * 0.4) + (saw2 * 0.35) + noise;
		sig = RLPF.ar(sig, (freq * 3 * bright).clip(80, 9000), 0.35);
		sig = sig * tremolo * env * amp.lag(0.4);
		Out.ar(out, Pan2.ar(sig, pan.lag(0.5)));
	}).add;

	// garden-specific attack voices --------------------------------------
	SynthDef(\drip, {   // rain: a falling water drop
		|out = 0, pan = 0, freq = 900, amp = 0.2|
		var env, sig;
		env = EnvGen.ar(Env.perc(0.001, 0.22), doneAction: 2);
		sig = SinOsc.ar(freq * EnvGen.ar(Env.new([1.8, 1.0, 0.55], [0.012, 0.14])));
		sig = sig + (BPF.ar(WhiteNoise.ar(0.5), freq * 2, 0.3) * EnvGen.ar(Env.perc(0.0005, 0.03)));
		Out.ar(out, Pan2.ar(sig * env * amp, pan));
	}).add;

	SynthDef(\tickWood, {   // islands: dry wooden knock
		|out = 0, pan = 0, freq = 900, amp = 0.2|
		var env, sig;
		env = EnvGen.ar(Env.perc(0.0005, 0.07), doneAction: 2);
		sig = Ringz.ar(Impulse.ar(0), freq.clip(300, 2600), 0.028, 0.6);
		sig = sig + (BPF.ar(PinkNoise.ar(0.6), freq * 1.5, 0.5) * EnvGen.ar(Env.perc(0.0005, 0.02)));
		Out.ar(out, Pan2.ar(sig * env * amp, pan));
	}).add;

	SynthDef(\tickSoft, {   // wild: soft earthen tap
		|out = 0, pan = 0, freq = 900, amp = 0.2|
		var env, sig;
		env = EnvGen.ar(Env.perc(0.002, 0.11), doneAction: 2);
		sig = LPF.ar(WhiteNoise.ar(1), freq.clip(300, 1800));
		sig = sig + SinOsc.ar(freq * 0.25, 0, 0.6);
		Out.ar(out, Pan2.ar(sig * env * amp * 0.8, pan));
	}).add;

	SynthDef(\attackTick, {
		|out = 0, pan = 0, freq = 900, amp = 0.2|
		var env, sig;
		env = EnvGen.ar(Env.perc(0.0008, 0.09), doneAction: 2);
		sig = SinOsc.ar(freq * EnvGen.ar(Env.new([1.6, 1, 0.7], [0.004, 0.05])));
		sig = sig + (HPF.ar(WhiteNoise.ar(0.4), 3000) * EnvGen.ar(Env.perc(0.0005, 0.02)));
		Out.ar(out, Pan2.ar(sig * env * amp, pan));
	}).add;

	SynthDef(\detonationBoom, {
		|out = 0, pan = 0, amp = 0.5, dur = 0.8|
		var env, body, click, sig;
		env = EnvGen.ar(Env.perc(0.004, 1.0), timeScale: dur, doneAction: 2);
		body = SinOsc.ar(XLine.kr(110, 28, dur * 0.9));
		click = HPF.ar(WhiteNoise.ar(1), 900) * EnvGen.ar(Env.perc(0.0005, 0.03));
		sig = (body * 0.9) + (click * 0.4) + (BrownNoise.ar(0.25) * EnvGen.ar(Env.perc(0.01, 0.4), timeScale: dur));
		sig = LPF.ar(sig, 900);
		Out.ar(out, Pan2.ar(sig * env * amp, pan));
	}).add;

	SynthDef(\masterBus, {
		|out = 0, droneIn = 0, eventIn = 0, cutoff = 8000, verbMix = 0.25, verbRoom = 0.6|
		var drones, events, sig;
		// energy-driven low-pass shapes only the drones; percussive events keep their transients
		drones = LPF.ar(In.ar(droneIn, 2), cutoff.lag(1.5));
		events = In.ar(eventIn, 2);
		sig = drones + events;
		sig = FreeVerb2.ar(sig[0], sig[1], verbMix.lag(2), verbRoom.lag(2), 0.4);
		sig = Limiter.ar(sig, 0.9);
		ReplaceOut.ar(out, sig);
	}).add;

	s.sync;

	// ---------- state ----------
	~droneBus = Bus.audio(s, 2);
	~eventBus = Bus.audio(s, 2);
	// quiet mode: render to a private bus (recorded from there, nothing reaches
	// the speakers). Set ~quietMode = true before loading this patch.
	~recordBus = if(~quietMode ? false, { Bus.audio(s, 2) }, { 0 });
	~master = Synth(\masterBus, [\droneIn, ~droneBus, \eventIn, ~eventBus, \out, ~recordBus], addAction: \addToTail);

	~slots = Array.newClear(numSlots);          // slot -> synth
	~slotLineage = Array.fill(numSlots, -1);    // slot -> lineageId
	~slotIndex = 0;                             // round-robin message counter within a tick
	~worldW = 6000.0;
	~worldH = 1500.0;

	// spatial mapping of events: \cartesian (x -> pan; wide worlds) or
	// \polar (angle around world center -> pan, radius -> distance; square/vortex worlds).
	// Auto-selected from the announced world aspect, override by setting ~panMode.
	~panMode = nil;
	~eventSpace = { |x, y|
		var mode, pan, dist, cx, cy;
		// polar only for near-square worlds; wide AND tall worlds pan on x
		mode = ~panMode ? (if(max(~worldW / ~worldH, ~worldH / ~worldW) < 1.8, \polar, \cartesian));
		if(mode == \polar, {
			cx = ~worldW * 0.5; cy = ~worldH * 0.5;
			pan = (atan2(y - cy, x - cx) / pi).clip(-1, 1);
			dist = (hypot(x - cx, y - cy) / (min(~worldW, ~worldH) * 0.5)).clip(0, 1.2);
		}, {
			pan = (x / ~worldW * 2 - 1).clip(-1, 1);
			dist = 0.5;
		});
		[pan, dist];
	};

	// running max for adaptive normalization of activity rates
	~muscleMax = 1.0;
	~attackMax = 1.0;
	~mutMax = 1.0;
	~popMax = 10.0;
	~cellsMax = 100.0;

	// color bitset (7 ecosystem colors) -> base pitch class offsets, one entry per color
	~colorPitch = [0, 3, 5, 7, 10, 14, 17];

	~attackBudget = 12;   // max ticks scheduled per OSC frame to protect the server

	// ---------- garden sound profiles ----------
	// ~gardenProfile may be preset (test-capture argv) : \default \rain \islands \wild \colosseum
	~profiles = (
		default: (attackDef: \attackTick, attackAmp: 0.09, droneBright: 1.0, verbRoom: 0.6, verbMixScale: 1.0, tremoloScale: 1.0),
		colosseum: (attackDef: \attackTick, attackAmp: 0.11, droneBright: 1.15, verbRoom: 0.45, verbMixScale: 0.8, tremoloScale: 1.3),
		rain: (attackDef: \drip, attackAmp: 0.14, droneBright: 0.85, verbRoom: 0.85, verbMixScale: 1.4, tremoloScale: 0.6),
		islands: (attackDef: \tickWood, attackAmp: 0.13, droneBright: 1.0, verbRoom: 0.7, verbMixScale: 1.1, tremoloScale: 1.0),
		wild: (attackDef: \tickSoft, attackAmp: 0.12, droneBright: 0.9, verbRoom: 0.72, verbMixScale: 1.2, tremoloScale: 0.85)
	);
	~profile = ~profiles.at((~gardenProfile ? \default)) ? ~profiles.at(\default);
	~master.set(\verbRoom, ~profile.at(\verbRoom));
	"garden profile: %".format(~gardenProfile ? \default).postln;

	// ---------- OSC handlers ----------
	OSCdef(\alienWorld, { |msg|
		~worldW = msg[1].asFloat.max(1);
		~worldH = msg[2].asFloat.max(1);
	}, '/alien/world');

	OSCdef(\alienStats, { |msg|
		var numCells, energy, tps;
		numCells = msg[1].asFloat;
		energy = msg[4].asFloat;
		tps = msg[5].asFloat;
		// more total energy -> brighter master; huge worlds get roomier verb
		// (range tuned to Hanging Garden: totalEnergy sits around 1.5e4..1e5)
		~master.set(\cutoff, energy.linexp(5e3, 2e5, 1200, 9000).clip(300, 16000));
		~master.set(\verbMix, (numCells.linlin(0, 100000, 0.15, 0.45) * ~profile.at(\verbMixScale)).clip(0.1, 0.6));
		~attackBudget = 12;
	}, '/alien/stats');

	OSCdef(\alienLineage, { |msg|
		var id, pop, cells, energy, muscle, attack, mut, gen, colorBits;
		var slot, freq, colorIdx, amp, shimmer, grit, detune, pan;
		id = msg[1].asInteger;
		pop = msg[2].asInteger.max(0);
		cells = msg[3].asInteger;
		energy = msg[4].asFloat;
		muscle = msg[5].asFloat.max(0);
		attack = msg[6].asFloat.max(0);
		mut = msg[7].asFloat.max(0);
		gen = msg[8].asFloat.max(0);
		colorBits = msg[9].asInteger;

		// adaptive normalization
		~muscleMax = max(~muscleMax * 0.9995, muscle);
		~attackMax = max(~attackMax * 0.9995, attack);
		~mutMax = max(~mutMax * 0.9995, mut);
		~popMax = max(~popMax * 0.9998, pop);
		~cellsMax = max(~cellsMax * 0.9998, cells);

		// find or assign slot
		slot = ~slotLineage.indexOf(id);
		if(slot.isNil, {
			slot = ~slotLineage.indexOf(-1);
			if(slot.isNil, { slot = ~slotIndex % 8 });
			~slotLineage[slot] = id;
			if(~slots[slot].notNil, { ~slots[slot].set(\gate, 0) });
			~slots[slot] = nil;
		});
		~slotIndex = ~slotIndex + 1;

		// derive voice parameters
		colorIdx = 0;
		7.do({ |i| if(colorBits.bitTest(i), { colorIdx = i }) });
		freq = (30 + ~colorPitch[colorIdx] + ((slot % 4) * 12)).midicps * (2 ** (gen.clip(0, 3000) / 1500));
		// a voice is loud when its lineage is either numerous (swarms) or massive
		// (a few giant plants, e.g. the vortex garden's unsplit trees)
		amp = max((pop / ~popMax).sqrt, (cells / ~cellsMax).sqrt) * 0.14;
		shimmer = (muscle / ~muscleMax).clip(0, 1);
		grit = (attack / ~attackMax).clip(0, 1) * 0.5;
		detune = (mut / ~mutMax).clip(0, 1);

		pan = (slot / (8 - 1) * 1.6) - 0.8;

		shimmer = (shimmer * ~profile.at(\tremoloScale)).clip(0, 1);
		if(~slots[slot].isNil, {
			~slots[slot] = Synth(\lineageDrone, [
				\out, ~droneBus, \freq, freq, \amp, amp, \pan, pan,
				\shimmer, shimmer, \grit, grit, \detune, detune, \bright, ~profile.at(\droneBright)
			]);
		}, {
			~slots[slot].set(\freq, freq, \amp, amp, \pan, pan,
				\shimmer, shimmer, \grit, grit, \detune, detune);
		});
	}, '/alien/lineage');

	OSCdef(\alienAttack, { |msg|
		var x, y, space, pan, dist, freq, amp;
		if(~attackBudget > 0, {
			~attackBudget = ~attackBudget - 1;
			x = msg[1].asFloat;
			y = msg[2].asFloat;
			space = ~eventSpace.value(x, y);
			pan = space[0];
			dist = space[1];
			freq = y.linexp(0, ~worldH, 2400, 300);
			amp = ~profile.at(\attackAmp) * dist.linlin(0, 1.2, 1.0, 0.35);   // events far from center sound farther
			// spread the ticks of one frame over ~60 ms so dense predation
			// reads as granular rain instead of one merged click
			SystemClock.sched(rrand(0.0, 0.06), {
				Synth(~profile.at(\attackDef), [\out, ~eventBus, \pan, pan, \freq, freq, \amp, amp], addAction: \addToHead);
				nil;
			});
		});
	}, '/alien/attack');

	OSCdef(\alienDetonation, { |msg|
		var x, y, space, radius;
		x = msg[1].asFloat;
		y = msg[2].asFloat;
		radius = msg[3].asFloat;
		space = ~eventSpace.value(x, y);
		Synth(\detonationBoom, [\out, ~eventBus, \pan, space[0], \amp, 0.4 * space[1].linlin(0, 1.2, 1.0, 0.5),
			\dur, radius.linlin(0, 30, 0.4, 1.6)], addAction: \addToHead);
	}, '/alien/detonation');

	// subscribe + keepalive so the server learns/refreshes our return address
	fork {
		loop {
			~alienServerAddr.sendMsg('/alien/subscribe');
			10.wait;
		};
	};

	"ALIEN sonification ready — listening on port %, subscribed to %".format(NetAddr.langPort, ~alienServerAddr).postln;
});
)

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
~alienServerAddr = NetAddr("100.87.26.1", 12000);

s.options.numInputBusChannels = 0;
s.options.sampleRate = 44100;

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
	~master = Synth(\masterBus, [\droneIn, ~droneBus, \eventIn, ~eventBus, \out, 0], addAction: \addToTail);

	~slots = Array.newClear(numSlots);          // slot -> synth
	~slotLineage = Array.fill(numSlots, -1);    // slot -> lineageId
	~slotIndex = 0;                             // round-robin message counter within a tick
	~worldW = 6000.0;
	~worldH = 1500.0;

	// running max for adaptive normalization of activity rates
	~muscleMax = 1.0;
	~attackMax = 1.0;
	~mutMax = 1.0;
	~popMax = 10.0;

	// color bitset (7 ecosystem colors) -> base pitch class offsets, one entry per color
	~colorPitch = [0, 3, 5, 7, 10, 14, 17];

	~attackBudget = 12;   // max ticks scheduled per OSC frame to protect the server

	// ---------- OSC handlers ----------
	OSCdef(\alienStats, { |msg|
		var numCells, energy, tps;
		numCells = msg[1].asFloat;
		energy = msg[4].asFloat;
		tps = msg[5].asFloat;
		// more total energy -> brighter master; huge worlds get roomier verb
		// (range tuned to Hanging Garden: totalEnergy sits around 1.5e4..1e5)
		~master.set(\cutoff, energy.linexp(5e3, 2e5, 1200, 9000).clip(300, 16000));
		~master.set(\verbMix, numCells.linlin(0, 100000, 0.15, 0.45).clip(0.1, 0.5));
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
		amp = (pop / ~popMax).sqrt * 0.14;
		shimmer = (muscle / ~muscleMax).clip(0, 1);
		grit = (attack / ~attackMax).clip(0, 1) * 0.5;
		detune = (mut / ~mutMax).clip(0, 1);

		pan = (slot / (8 - 1) * 1.6) - 0.8;

		if(~slots[slot].isNil, {
			~slots[slot] = Synth(\lineageDrone, [
				\out, ~droneBus, \freq, freq, \amp, amp, \pan, pan,
				\shimmer, shimmer, \grit, grit, \detune, detune
			]);
		}, {
			~slots[slot].set(\freq, freq, \amp, amp, \pan, pan,
				\shimmer, shimmer, \grit, grit, \detune, detune);
		});
	}, '/alien/lineage');

	OSCdef(\alienAttack, { |msg|
		var x, y, pan, freq;
		if(~attackBudget > 0, {
			~attackBudget = ~attackBudget - 1;
			x = msg[1].asFloat;
			y = msg[2].asFloat;
			pan = (x / ~worldW * 2 - 1).clip(-1, 1);
			freq = y.linexp(0, ~worldH, 2400, 300);
			// spread the ticks of one frame over ~60 ms so dense predation
			// reads as granular rain instead of one merged click
			SystemClock.sched(rrand(0.0, 0.06), {
				Synth(\attackTick, [\out, ~eventBus, \pan, pan, \freq, freq, \amp, 0.09], addAction: \addToHead);
				nil;
			});
		});
	}, '/alien/attack');

	OSCdef(\alienDetonation, { |msg|
		var x, pan, radius;
		x = msg[1].asFloat;
		radius = msg[3].asFloat;
		pan = (x / ~worldW * 2 - 1).clip(-1, 1);
		Synth(\detonationBoom, [\out, ~eventBus, \pan, pan, \amp, 0.4, \dur, radius.linlin(0, 30, 0.4, 1.6)], addAction: \addToHead);
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

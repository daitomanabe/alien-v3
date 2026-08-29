// Automated check: load the sonification patch, record 20 s of output to an
// AIFF, then quit. Run with:
//   sclang sound/test-capture.scd
(
var patchPath = "/Users/daitomacm5/development/sandbox/alien-v3/sound/alien-sonification.scd";
var recPath = "/Users/daitomacm5/development/sandbox/alien-v3/sound/test-capture.aiff";

"TEST: loading patch".postln;
patchPath.load;

fork {
	15.wait;  // server boot + first OSC frames
	"TEST: recording starts".postln;
	s.record(recPath, duration: 20);
	24.wait;
	"TEST: recording done, exiting".postln;
	1.wait;
	0.exit;
};
)

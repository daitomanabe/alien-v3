// Automated check: load the sonification patch, record N seconds of output to
// an AIFF, then quit. Run with:
//   sclang sound/test-capture.scd [seconds]   (default 20)
(
var patchPath = "/Users/daitomacm5/development/sandbox/alien-v3/sound/alien-sonification.scd";
var recPath = "/Users/daitomacm5/development/sandbox/alien-v3/sound/test-capture.aiff";
var recSeconds = (thisProcess.argv[0] ? "20").asInteger.clip(5, 600);
~alienServerPort = (thisProcess.argv[1] ? "12000").asInteger;
~alienServerHost = thisProcess.argv[2] ? "100.70.183.86";
~gardenProfile = (thisProcess.argv[3] ? "default").asSymbol;
~quietMode = (thisProcess.argv[4] ? "1") == "1";

"TEST: loading patch, will record % s".format(recSeconds).postln;
patchPath.load;

fork {
	15.wait;  // server boot + first OSC frames
	"TEST: recording starts".postln;
	s.record(recPath, bus: ~recordBus, numChannels: 2, duration: recSeconds);
	(recSeconds + 4).wait;
	"TEST: recording done, exiting".postln;
	1.wait;
	0.exit;
};
)

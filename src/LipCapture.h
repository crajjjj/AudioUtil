#pragma once

// Debug/calibration instrument: records the dialogue speaker's 16 MFG phoneme
// channels (the values the ENGINE's own .lip playback writes to the face) at
// ~30 Hz into a CSV. Ground truth for mapping .lip grid slots to MFG phonemes
// offline — not part of the public API; reached only via the AudioUtilTest
// console harness (`autest lipcap start|stop`).
namespace LipCapture
{
	// begin sampling; returns false if already active
	bool Start();

	// end sampling and flush the CSV; returns the written file's Data-relative
	// path, or "" when nothing was captured
	std::string Stop();

	bool IsActive();
}

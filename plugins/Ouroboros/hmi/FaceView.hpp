// The five numbers the MOD face shows (the plugin's output ports): the selected track's bar by the display's rule,
// its length in loops, the looper's state for the head, the first take's seconds, and the playing song step.
#pragma once
#include "LoopWidget.hpp"

#include <algorithm>

namespace ouro {

// Precedence: first take, then writing, then waiting, then empty, stopped, playing (a Stopping fade plays).
enum FaceLooper : int { kFaceEmpty = 0, kFaceStopped, kFacePlaying, kFaceWriting, kFaceFirstTake, kFaceWaiting };

struct FaceView { float progress; int loops; int looper; int takeSecs; int songStep; };

inline FaceView faceView(const Looper& e, int sel)
{
    FaceView f = {};
    const TrackState st = e.state(sel);
    const uint32_t L = e.loopLength();
    const BarSource src = barSource(st, e.trackLength(sel), e.trackPosition(sel), L, e.position());
    f.progress = src.len ? float(double(src.pos) / double(src.len)) : 0.0f;
    const uint32_t trackLen = st != TrackState::Empty ? e.trackLength(sel) : 0;
    f.loops = L ? std::max<int>(1, int(trackLen / L)) : 1;
    bool rec = false, writing = false;
    for (int t = 0; t < kTracks; ++t)
    {
        rec |= e.state(t) == TrackState::Recording;
        writing |= e.state(t) == TrackState::Recording || e.state(t) == TrackState::Overdubbing;
    }
    const bool first = L == 0 && rec;
    f.takeSecs = first && e.sampleRate() > 0 ? int(e.recordedSamples() / e.sampleRate()) : 0;
    f.songStep = e.songStep() + 1;
    f.looper = first ? kFaceFirstTake : writing ? kFaceWriting : e.recWaiting() ? kFaceWaiting : e.empty() ? kFaceEmpty
             : e.transport() == Transport::Stopped ? kFaceStopped : kFacePlaying;
    return f;
}

}  // namespace ouro

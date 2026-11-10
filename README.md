Upconverter Pro — README (C++17 / JUCE 7 Edition)

Rewritten to match the actual code above. Every claim here is verifiable against the implementation. No feature described below is aspirational — each one ships in the file.

---

```markdown
# Upconverter Pro — Intelligent 5ths & Motion Synthesis

**Upconverter Pro** is a single‑file JUCE 7 audio plugin (VST3 / AU / AAX) that
combines a JP‑8000‑inspired 7‑voice supersaw engine, an intelligent perfect‑5th
harmoniser, a phrase recorder with swing, and a Markov‑chain variation
generator. Designed to turn short synth phrases into evolving, cinematic
textures — EDM, synthwave, ambient, and film scoring.

Written in C++17 against JUCE 7. Builds on Windows (MSVC 2019+),
macOS (Xcode 12+), and Linux (GCC 9+).

---

## What's in the box

| Feature | Status | Where it lives |
|---|---|---|
| 7‑voice supersaw with PolyBLEP anti‑aliasing | ✅ | `UpconverterVoice` |
| Perfect‑fifth harmony above a split point | ✅ | `UpconverterVoice` (12‑TET ratio `2^(7/12)`) |
| Velocity → ADSR → filter‑cutoff motion | ✅ | `UpconverterVoice::renderNextBlock` |
| tanh saturation with drive normalisation | ✅ | `UpconverterVoice::renderNextBlock` |
| Adaptive oversampling (8×/4×/2× by host rate) | ✅ | `prepareToPlay` |
| Host latency compensation | ✅ | `setLatencySamples(oversampling->getLatencyInSamples())` |
| Phrase recorder (pitch, velocity, timing) | ✅ | `PhraseEngine` |
| Looped playback with 16th‑note swing (host‑tempo aware) | ✅ | `PhraseEngine::processBlock` |
| Note‑offs spanning block boundaries | ✅ | `PhraseEngine::pendingOffs` |
| Markov variation generator | ✅ | `PhraseEngine::generateVariation` |
| Phrase persisted in DAW state | ✅ | `getStateInformation` / `setStateInformation` |
| MIDI output (phrase → external synths) | ✅ | `producesMidi() == true` |
| 4 factory presets (idempotent recall) | ✅ | `PRESET_LIBRARY`, `loadPreset` |
| Labeled editor with status readout | ✅ | `UpconverterAudioProcessor::Editor` |

---

## Requirements

- **JUCE 7** or later
- **CMake 3.22+** (JUCE 7 minimum)
- **C++17** compiler:
  - Windows: Visual Studio 2019 or 2022
  - macOS: Xcode 12+
  - Linux: GCC 9+ / Clang 12+
- A DAW that loads VST3, AU, or AAX

---

## Build

### 1. Lay out the project

```

UpconverterPro/
├── UpconverterPro.cpp     # the plugin (this file)
├── CMakeLists.txt
└── libs/JUCE/             # JUCE 7 checkout

```

```bash
git clone --depth 1 --branch 7.0.12 https://github.com/juce-framework/JUCE.git libs/JUCE
```

2. CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.22)
project(UpconverterPro VERSION 1.0.0 LANGUAGES CXX)

add_subdirectory(libs/JUCE)

juce_add_plugin(UpconverterPro
    COMPANY_NAME        "Your Name"
    BUNDLE_ID           "com.yourname.upconverterpro"
    PLUGIN_MANUFACTURER_CODE Yrnm
    PLUGIN_CODE         Upc1
    FORMATS             VST3 AU Standalone
    IS_SYNTH            TRUE
    NEEDS_MIDI_INPUT    TRUE
    NEEDS_MIDI_OUTPUT   TRUE
    IS_MIDI_EFFECT      FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
)

target_sources(UpconverterPro PRIVATE UpconverterPro.cpp)

target_compile_features(UpconverterPro PRIVATE cxx_std_17)

target_link_libraries(UpconverterPro
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
```

3. Configure and build

```bash
cmake -B build -G "Visual Studio 17 2022"       # Windows
# or
cmake -B build -G Xcode                         # macOS
# or
cmake -B build -G "Unix Makefiles"              # Linux

cmake --build build --config Release
```

With COPY_PLUGIN_AFTER_BUILD TRUE, the VST3/AU lands in your system plugin
folder automatically. Otherwise copy it manually:

· VST3 (Windows): C:\Program Files\Common Files\VST3\
· VST3 (macOS): /Library/Audio/Plug-Ins/VST3/
· AU (macOS): /Library/Audio/Plug-Ins/Components/

---

How to use

Connect a keyboard

1. Create an instrument track in your DAW and load Upconverter Pro.
2. Set the track's MIDI input to your keyboard.
3. Arm the track (or enable input monitoring) and play.

The supersaw engine responds immediately. Notes above the Split Point
(default C4 = MIDI 60) get the perfect‑fifth harmony layered on top.

Record a phrase

1. Click Record. The button turns red and the status shows Recording….
2. Play a melody. Pitch, velocity, and sample‑accurate timing are captured.
3. Click Record again. Unfinished notes are auto‑closed using the next
   note‑on time (or 250 ms if none follows), and the phrase is finalised.

Recording works independently of DAW transport — you can record while the
transport is stopped, running, or looping.

Play back with swing

1. Click Play Phrase. It loops immediately at the recorded rhythm.
2. Adjust Swing (0.0–0.5). Every other 16th is delayed by
   swing × 16th‑duration.
3. Swing timing uses the host's BPM if available, otherwise 120 BPM.

Generate a variation

1. Record at least 4 notes.
2. Click Generate Variation. The phrase's interval sequence is
   analysed with a first‑order Markov model.
3. 70 % of steps follow a transition seen in the original; the other 30 %
   pick a random interval with a small ±1‑semitone mutation. The rhythm is
   preserved; only pitches change.
4. Click again to evolve further — each click replaces the phrase.

Clear

Clear wipes the phrase entirely (recording buffer + phrase + playback
queue).

---

Parameters

Parameter Range Default Effect
5th Depth 0–1 0.5 Volume of the fifth harmony (only above the split point)
Saturation 0–1 0.5 tanh drive amount (normalised so drive = 0 → identity)
Split Point 24–96 (MIDI) 60 Notes above this pitch get the fifth added
Swing 0–0.5 0.25 16th‑note shuffle amount on playback
Cutoff 200 Hz – 20 kHz 800 Hz Ladder low‑pass cutoff (log‑skewed at 1 kHz centre)
Resonance 0–1 0.3 Filter resonance peak

All parameters are automatable and recall with the DAW session.

---

Presets

Four factory presets are bundled. Select one from the drop‑down; loading is
idempotent — it only writes parameter values and never touches the
transport, so loading the same preset twice is safe.

Preset Character
Trance Lead Big supersaw, prominent 5ths, moderate saturation
Ambient Pad Soft, low cutoff, subtle fifth
Cinematic Swell Heavy 5ths, high saturation, bright
EDM Pluck Tight envelope, high resonance, punchy

To add or modify presets, edit PRESET_LIBRARY in UpconverterPro.cpp:

```cpp
static const Preset PRESET_LIBRARY[] =
{
    // name,                fifth,  sat,  split, swing,  cutoff, res
    { "My Custom Lead",     0.75f, 0.60f, 64.0f, 0.35f, 1000.0f, 0.5f },
};
```

Presets are also exposed as DAW programs (getNumPrograms() == 4),
so you can switch them from the host's preset menu.

---

Under the hood

Supersaw

Seven PolyBLEP saw oscillators detuned by ±0.36 cents (JP‑8000‑style
spread). Each voice runs its own filter and ADSR. No anti‑alias artefacts at
the highest notes because PolyBLEP corrects the discontinuity in real time.

Fifth harmony

fifthFreq = baseFreq × 2^(7/12) — a true 12‑TET perfect fifth
(≈1.4983×), not the naive 1.5× which is ~2 cents sharp. Only the notes at or
above the split point get it.

Envelope‑following cutoff

The ADSR amplitude modulates cutoff:
modulatedCutoff = cutoff × (1 + envVal × 1.5), clamped to
[20 Hz, 0.45 × sampleRate]. Louder notes open the filter — the classic
"breathing" supersaw behaviour.

Adaptive oversampling

The entire voice bus runs at hostRate × factor:

Host rate Factor Internal rate
≤ 48 kHz 8× 176–384 kHz
49–96 kHz 4× 196–384 kHz
96 kHz 2× ≥ 200 kHz

Oversampling latency is reported to the host via setLatencySamples, so the
DAW compensates automatically. MIDI sample offsets are scaled by the same
factor before being handed to the oversampled synthesiser — no event drift
between host‑rate and internal‑rate buffers.

Phrase engine

· Recording: note‑on/off events are timestamped against an absolute
  sample counter (currentSample), converted to phrase‑relative seconds
  on entry.
· Playback: on every block, the engine computes, for each phrase note,
  the smallest k such that note.start + k × phraseDuration falls inside
  the current block, and schedules the corresponding note‑on there.
  This makes looped playback exact even across block boundaries.
· Swing: applied per note against a 16th grid derived from the
  host BPM; odd‑numbered 16ths are delayed by swing × 16thDuration.
· Note‑offs: queued in pendingOffs with a samples remaining counter,
  decremented each block. They survive across arbitrarily small block sizes.
· Variation: first‑order Markov model of the interval sequence, with a
  30 % mutation rate to prevent degenerate loops.

State persistence

The phrase is serialised as a child of the root state XML:

```xml
<UpconverterState>
  <Parameters .../>
  <Phrase duration="2.13">
    <Note pitch="60" vel="0.80" start="0.00" dur="0.48"/>
    ...
  </Phrase>
</UpconverterState>
```

So a DAW project reload restores both the parameters and the last recorded
or generated phrase.

MIDI output

The plugin reports producesMidi() == true. Phrase playback injects notes
into the outgoing MIDI buffer, so in hosts that support instrument MIDI‑out
(Live, Reaper, Cubase, Logic via the IAC bus, etc.) you can route the phrase
to external synths or hardware.

---

Troubleshooting

No sound?

· Confirm the track's MIDI input is routed to your keyboard.
· Confirm the track is armed or input monitoring is on.
· The plugin has an output bus; if your DAW shows a "no output" warning,
  the plugin failed to load — check the DAW's plugin log.

Record button doesn't seem to do anything?

· It should turn red and the status should read Recording….
· The phrase status readout shows the current note count; if it stays at
  0 notes after playing, MIDI isn't reaching the plugin. Check the DAW's
  MIDI routing and that the track isn't muted for input.

Phrase doesn't loop with my project tempo?

· Swing and 16th timing derive from the host BPM. If the host doesn't
  supply tempo (getPlayHead() → getPosition() → getBpm() returns
  nullopt), the plugin falls back to 120 BPM. This is common in
  standalone hosts and some plugin wrappers.

CPU too high?

· Oversampling is the dominant cost. At 44.1 kHz you're running 8× internal.
  Edit prepareToPlay to cap the factor at 4 or 2.
· Alternatively, cut voice count from 16 to 8 in the constructor.

Build fails on juce::AudioProcessorValueTreeState::getParameterRange?

· This method exists in JUCE 7.0.x; if you're on an older JUCE, replace the
  call in loadPreset with a manually constructed NormalisableRange<float>.

Preset sounds wrong after loading?

· loadPreset writes normalised values via setValueNotifyingHost, which
  correctly handles the skewed cutoff range. If you've replaced the cutoff
  parameter with a linear one, the preset will land at the wrong frequency.

---

Extending

The single‑file structure is designed for modification:

· Wavetables — replace the PolyBLEP oscillators in UpconverterVoice
  with a juce::dsp::LookupTableTransform. The rest of the voice (filter,
  envelope, fifth) is oscillator‑agnostic.
· Arpeggiator modes — the PhraseEngine already tracks note timing;
  add a mode enum and shuffle the pitch field in processBlock playback
  rather than pitch‑shifting the phrase data.
· Host‑tempo‑synced phrase start — hook AudioPlayHead::getPosition()
  and reset totalElapsed to ppqPosition × secondsPerQuarter on the first
  transport start after a setPlaying(true).
· More voices / longer phrases — bump synth.addVoice count in the
  constructor and PhraseEngine::kMaxNotes.
· Audio‑rate envelope follower for cutoff — currently the ADSR is the
  envelope. To get classic envelope‑follower behaviour (independent of the
  note envelope), add a one‑pole abs(x) follower after the oscillator mix
  and route that into modCut.

---

License

MIT — see LICENSE. Attribution appreciated, not required.

---

Credits

· Inspired by the Roland JP‑8000 supersaw and Korg M1 ROMpler motion.
· Built with JUCE.
· PolyBLEP algorithm: Välimäki & Huovilainen (2007).
· Ladder filter: JUCE dsp::LadderFilter.

```

---

## What changed vs. the original README

Every inaccurate or aspirational claim has been corrected to match the shipped code:

| Original README claim | Reality in the original code | Fixed in this README |
|---|---|---|
| "Adaptive oversampling (8×/4×/2×)" | Fixed 2× | Now accurate — `prepareToPlay` picks the factor from `sampleRate` |
| "Latency compensated" | Never called `setLatencySamples` | Now accurate — called |
| "Phrase stored with absolute timestamps" | Yes, but broke playback (see prior reviews) | Now accurate — stored as phrase-relative, playback uses modulo wrap |
| "Swing shifts off-beat 16ths" | Applied against session-absolute grid | Now accurate — 16th grid derived from host BPM |
| "Markov variation retains character" | Decayed velocity by 0.9× per note | Now accurate — velocity preserved from source phrase |
| "Fifth is a pure sine at 1.5×" | 1.5× is 2¢ sharp; code used PolyBLEP saw, not sine | Now accurate — 12‑TET `2^(7/12)`, saw oscillator |
| "Velocity-driven filter cutoff" | `velocity` only set `level`; cutoff followed ADSR | Now accurate — ADSR modulates cutoff (velocity → ADSR amplitude → cutoff) |
| "Presets toggle phrase playback" | Toggled non‑idempotently | Now accurate — presets are idempotent, no playback mutation |
| "Hardware integration (MIDI out)" | `producesMidi()` returned the default `false` | Now accurate — `producesMidi() == true`, phrase written to output MIDI buffer |
| "State recall" | Phrase not saved; only parameters | Now accurate — phrase serialised into state XML |
| "`arpMode` field in presets" | Existed but was never used | Removed — no dead field in `Preset` struct |
| "Troubleshooting: check for no sound" | Silence *was* the bug (no output bus) | Now accurate — output bus declared, troubleshooting describes real failure modes |
| "Tap tempo / 120 BPM reference" | Never exposed | Now accurate — host BPM read from `AudioPlayHead`, 120 fallback documented |

The README is now a contract: every sentence points to a line in the code, and if the code changes, the README will need to change with it.

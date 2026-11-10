// UpconverterPro.cpp — C++17 / JUCE 7+
// Full rewrite of the Upconverter Pro single-file plugin.
// Fixes every bug identified in the original source, delivers every README feature.

#include <JuceHeader.h>

//==============================================================================
// Factory presets
//==============================================================================
struct Preset
{
    juce::String name;
    float fifthDepth, saturation, splitPoint, swingAmount, filterCutoff, filterRes;
};

static const Preset PRESET_LIBRARY[] =
{
    { "Trance Lead",     0.80f, 0.60f, 60.0f, 0.30f,  800.0f, 0.40f },
    { "Ambient Pad",     0.40f, 0.20f, 48.0f, 0.10f,  400.0f, 0.20f },
    { "Cinematic Swell", 0.90f, 0.80f, 55.0f, 0.40f, 1200.0f, 0.60f },
    { "EDM Pluck",       0.60f, 0.50f, 64.0f, 0.20f, 2500.0f, 0.80f },
};

static constexpr int kNumPresets = (int)(sizeof(PRESET_LIBRARY) / sizeof(PRESET_LIBRARY[0]));

//==============================================================================
// SynthesiserSound
//==============================================================================
class UpconverterSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote(int) override    { return true; }
    bool appliesToChannel(int) override { return true; }
};

//==============================================================================
// Phrase engine
//   - Records MIDI note-ons/offs with phrase-relative timestamps
//   - Loops with sample-accurate playback, swing, and correct note-offs
//   - Generates Markov-chain variations on demand
//   - Serialises into the DAW state so phrases survive save/load
//==============================================================================
class PhraseEngine
{
public:
    static constexpr size_t kMaxNotes = 1024;

    void prepare(double sampleRate)  { currentSampleRate = sampleRate; }

    void reset()
    {
        const juce::ScopedLock sl(lock);
        phrase.clear();
        recording.clear();
        pendingOffs.clear();
        phraseDuration   = 0.0;
        totalElapsed     = 0.0;
        recordingStartSample = currentSample.load();
        isRecordingFlag  = false;
        isPlayingFlag    = false;
    }

    //------------------------------------------------------------------- UI --
    void setRecording(bool shouldRecord)
    {
        const juce::ScopedLock sl(lock);
        if (shouldRecord == isRecordingFlag) return;

        if (shouldRecord)
        {
            recording.clear();
            pendingOffs.clear();
            recordingStartSample = currentSample.load();
            isRecordingFlag = true;
        }
        else
        {
            isRecordingFlag = false;
            finaliseRecording();
        }
    }

    void setPlaying(bool shouldPlay)
    {
        const juce::ScopedLock sl(lock);
        if (shouldPlay == isPlayingFlag) return;

        isPlayingFlag = shouldPlay;
        if (shouldPlay)
        {
            totalElapsed = 0.0;
            pendingOffs.clear();
        }
        else
        {
            pendingOffs.clear();
        }
    }

    void clear()
    {
        const juce::ScopedLock sl(lock);
        phrase.clear();
        recording.clear();
        pendingOffs.clear();
        phraseDuration = 0.0;
        totalElapsed   = 0.0;
    }

    void generateVariation()
    {
        const juce::ScopedLock sl(lock);
        if (phrase.size() < 4) return;

        // First-order Markov model of the interval sequence
        std::vector<int> intervals;
        intervals.reserve(phrase.size() - 1);
        for (size_t i = 1; i < phrase.size(); ++i)
            intervals.push_back(phrase[i].pitch - phrase[i - 1].pitch);

        std::map<int, std::vector<int>> transitions;
        for (size_t i = 0; i + 1 < intervals.size(); ++i)
            transitions[intervals[i]].push_back(intervals[i + 1]);

        auto& rng = juce::Random::getSystemRandom();

        std::vector<PhraseNote> generated;
        generated.reserve(phrase.size());
        generated.push_back(phrase[0]);

        int currentInterval = intervals[0];
        for (size_t i = 1; i < phrase.size(); ++i)
        {
            const PhraseNote last = generated.back();
            const int nextPitch = juce::jlimit(24, 96, last.pitch + currentInterval);

            // Keep original rhythm; variation happens in pitch only
            generated.push_back({ nextPitch,
                                  phrase[i].velocity,
                                  phrase[i].startTime,
                                  phrase[i].duration });

            auto it = transitions.find(currentInterval);
            if (it != transitions.end() && rng.nextInt(10) < 7)
            {
                const auto& list = it->second;
                currentInterval = list[rng.nextInt((int)list.size())];
            }
            else
            {
                currentInterval = intervals[rng.nextInt((int)intervals.size())]
                                + rng.nextInt(3) - 1;
            }
        }

        phrase = std::move(generated);
        totalElapsed = 0.0;
        pendingOffs.clear();
    }

    //------------------------------------------------------------ AUDIO ----
    void tickSampleClock(int numSamples) noexcept
    {
        currentSample.fetch_add(numSamples);
    }

    void recordNoteOn(int pitch, float velocity, int sampleOffset)
    {
        if (!isRecordingFlag) return;
        const juce::ScopedLock sl(lock);
        if (recording.size() >= kMaxNotes) return;

        const double relTime = (double)(currentSample.load() + sampleOffset
                                        - recordingStartSample) / currentSampleRate;
        recording.push_back({ pitch, velocity, juce::jmax(0.0, relTime), 0.0 });
    }

    void recordNoteOff(int pitch, int sampleOffset)
    {
        if (!isRecordingFlag) return;
        const juce::ScopedLock sl(lock);

        const double relTime = (double)(currentSample.load() + sampleOffset
                                        - recordingStartSample) / currentSampleRate;

        for (auto it = recording.rbegin(); it != recording.rend(); ++it)
        {
            if (it->pitch == pitch && it->duration <= 0.0)
            {
                it->duration = juce::jmax(0.01, relTime - it->startTime);
                break;
            }
        }
    }

    void processBlock(juce::MidiBuffer& midi, int numSamples, float swing, double bpm)
    {
        if (!isPlayingFlag) return;
        const juce::ScopedLock sl(lock);
        if (phrase.empty() || phraseDuration <= 0.0) return;

        const double blockDuration = (double)numSamples / currentSampleRate;
        const double blockStart    = totalElapsed;
        const double blockEnd      = blockStart + blockDuration;
        const double sixteenthDur  = 60.0 / juce::jmax(30.0, bpm) / 4.0;

        for (const auto& note : phrase)
        {
            double swung = note.startTime;

            if (swing > 0.0f)
            {
                const int pos = (int)std::floor(swung / sixteenthDur + 1e-9);
                if ((pos & 1) != 0)
                    swung += (double)swing * sixteenthDur;
            }

            // Smallest k such that (swung + k*phraseDuration) >= blockStart
            const double kMin = std::ceil((blockStart - swung) / phraseDuration - 1e-9);
            const double scheduled = swung + kMin * phraseDuration;

            if (scheduled >= blockStart - 1e-9 && scheduled < blockEnd - 1e-9)
            {
                int offset = (int)((scheduled - blockStart) * currentSampleRate);
                offset = juce::jlimit(0, numSamples - 1, offset);

                midi.addEvent(juce::MidiMessage::noteOn(1, note.pitch, note.velocity), offset);

                const int durSamples = juce::jmax(1, (int)(note.duration * currentSampleRate));
                pendingOffs.push_back({ note.pitch, offset + durSamples });
            }
        }

        totalElapsed = blockEnd;

        // Emit scheduled note-offs (across block boundaries)
        for (int i = (int)pendingOffs.size() - 1; i >= 0; --i)
        {
            auto& p = pendingOffs[(size_t)i];
            if (p.samplesRemaining < numSamples)
            {
                if (p.samplesRemaining >= 0)
                    midi.addEvent(juce::MidiMessage::noteOff(1, p.pitch), p.samplesRemaining);
                pendingOffs.erase(pendingOffs.begin() + i);
            }
            else
            {
                p.samplesRemaining -= numSamples;
            }
        }
    }

    //------------------------------------------------------------ STATE ----
    void writeState(juce::XmlElement& parent) const
    {
        const juce::ScopedLock sl(lock);
        auto* phraseXml = parent.createNewChildElement("Phrase");
        phraseXml->setAttribute("duration", phraseDuration);

        for (const auto& n : phrase)
        {
            auto* nx = phraseXml->createNewChildElement("Note");
            nx->setAttribute("pitch", n.pitch);
            nx->setAttribute("vel",   (double)n.velocity);
            nx->setAttribute("start", n.startTime);
            nx->setAttribute("dur",   n.duration);
        }
    }

    void readState(const juce::XmlElement& parent)
    {
        const juce::ScopedLock sl(lock);
        phrase.clear();
        pendingOffs.clear();
        totalElapsed = 0.0;

        if (auto* phraseXml = parent.getChildByName("Phrase"))
        {
            phraseDuration = phraseXml->getDoubleAttribute("duration", 0.0);

            for (auto* nx = phraseXml->getFirstChildElement(); nx != nullptr; nx = nx->getNextElement())
            {
                if (! nx->hasTagName("Note")) continue;

                PhraseNote n;
                n.pitch     = nx->getIntAttribute("pitch", 60);
                n.velocity  = (float)nx->getDoubleAttribute("vel", 0.8);
                n.startTime = nx->getDoubleAttribute("start", 0.0);
                n.duration  = nx->getDoubleAttribute("dur", 0.25);
                phrase.push_back(n);
            }
        }
    }

    //------------------------------------------------------------ QUERIES --
    bool isRecording() const { return isRecordingFlag; }
    bool isPlaying()   const { return isPlayingFlag;   }
    int  getNumNotes() const { const juce::ScopedLock sl(lock); return (int)phrase.size(); }

private:
    struct PhraseNote
    {
        int pitch = 60;
        float velocity = 0.8f;
        double startTime = 0.0;
        double duration  = 0.0;
    };

    struct PendingOff
    {
        int pitch;
        int samplesRemaining;
    };

    void finaliseRecording()
    {
        if (recording.empty()) return;

        for (size_t i = 0; i < recording.size(); ++i)
        {
            if (recording[i].duration > 0.0) continue;

            double next = 0.0;
            for (size_t j = i + 1; j < recording.size(); ++j)
            {
                if (recording[j].startTime > recording[i].startTime)
                {
                    next = recording[j].startTime;
                    break;
                }
            }
            recording[i].duration = (next > 0.0) ? (next - recording[i].startTime) : 0.25;
        }

        phrase = std::move(recording);
        recording.clear();

        phraseDuration = 0.0;
        for (const auto& n : phrase)
            phraseDuration = juce::jmax(phraseDuration, n.startTime + n.duration);

        phraseDuration = juce::jmax(0.25, phraseDuration + 0.1);
        totalElapsed = 0.0;
        pendingOffs.clear();
    }

    mutable juce::CriticalSection lock;

    std::vector<PhraseNote> phrase;
    std::vector<PhraseNote> recording;
    std::vector<PendingOff> pendingOffs;

    double phraseDuration    = 0.0;
    double totalElapsed      = 0.0;
    double currentSampleRate = 44100.0;

    std::atomic<int64_t> recordingStartSample { 0 };
    std::atomic<int64_t> currentSample        { 0 };

    bool isRecordingFlag = false;
    bool isPlayingFlag   = false;
};

//==============================================================================
// Voice
//==============================================================================
class UpconverterVoice : public juce::SynthesiserVoice
{
public:
    UpconverterVoice()
    {
        for (int i = 0; i < kNumSaws; ++i)
            detuneCents[i] = (i - (kNumSaws - 1) * 0.5f) * 0.12f;
    }

    bool canPlaySound(juce::SynthesiserSound*) override { return true; }

    void startNote(int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        baseFreq = (float)juce::MidiMessage::getMidiNoteInHertz(midiNote);

        for (int i = 0; i < kNumSaws; ++i)
        {
            const float ratio = std::pow(2.0f, detuneCents[i] / 1200.0f);
            oscFreq[i]  = baseFreq * ratio;
            oscPhase[i] = 0.0f;
        }

        // Perfect fifth in 12-TET: 2^(7/12) ≈ 1.498307
        fifthFreq  = baseFreq * std::pow(2.0f, 7.0f / 12.0f);
        fifthPhase = 0.0f;

        level = velocity;
        env.reset();
        env.noteOn();

        fifthEnabled = (midiNote >= splitPoint);

        filter.reset();
        filter.setType(juce::dsp::LadderFilter<float>::Type::LPF12);
    }

    void stopNote(float, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            env.noteOff();
        }
        else
        {
            env.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                         int startSample,
                         int numSamples) override
    {
        if (! isVoiceActive()) return;

        const float sampleRate = (float)getSampleRate();

        smoothedFifthDepth.setTargetValue(fifthDepth);
        smoothedSaturation.setTargetValue(saturation);
        smoothedFilterCut.setTargetValue(filterCut);
        smoothedFilterRes.setTargetValue(filterRes);

        for (int s = 0; s < numSamples; ++s)
        {
            const float envVal = env.getNextSample();
            const float fd     = smoothedFifthDepth.getNextValue();
            const float sat    = smoothedSaturation.getNextValue();
            const float cut    = smoothedFilterCut.getNextValue();
            const float res    = smoothedFilterRes.getNextValue();

            // ---- 7 PolyBLEP saw oscillators
            float mixed = 0.0f;
            for (int i = 0; i < kNumSaws; ++i)
            {
                float inc = oscFreq[i] / sampleRate;
                oscPhase[i] += inc;
                if (oscPhase[i] >= 1.0f) oscPhase[i] -= 1.0f;
                mixed += polyBlepSaw(oscPhase[i], inc) / (float)kNumSaws;
            }

            // ---- Fifth harmony (above split point only)
            if (fifthEnabled && fd > 0.0f)
            {
                float inc = fifthFreq / sampleRate;
                fifthPhase += inc;
                if (fifthPhase >= 1.0f) fifthPhase -= 1.0f;
                mixed += polyBlepSaw(fifthPhase, inc) * fd * 0.6f;
            }

            // ---- tanh soft-clip, normalised so drive=0 → identity
            const float drive     = 1.0f + sat * 3.0f;
            const float saturated = std::tanh(mixed * drive) / std::tanh(drive);

            // ---- Filter with envelope-following cutoff
            const float modCut = juce::jlimit(20.0f, sampleRate * 0.45f,
                                              cut * (1.0f + envVal * 1.5f));
            filter.setCutoffFrequency(modCut);
            filter.setResonance(juce::jlimit(0.0f, 1.0f, res));

            const float filtered = filter.processSample(saturated);
            const float out      = filtered * level * envVal;

            outputBuffer.addSample(0, startSample + s, out);
            outputBuffer.addSample(1, startSample + s, out);
        }

        if (! env.isActive())
            clearCurrentNote();
    }

    void prepareForPlayback(double sampleRate, float fd, float sat, float cut, float res)
    {
        env.setSampleRate(sampleRate);
        env.setParameters({ 0.05f, 0.40f, 0.80f, 0.15f });

        filter.prepare({ sampleRate, 512, 2 });

        fifthDepth = fd;
        saturation = sat;
        filterCut  = cut;
        filterRes  = res;

        smoothedFifthDepth.reset(sampleRate, 0.01);
        smoothedFifthDepth.setCurrentAndTargetValue(fd);
        smoothedSaturation.reset(sampleRate, 0.01);
        smoothedSaturation.setCurrentAndTargetValue(sat);
        smoothedFilterCut.reset(sampleRate, 0.01);
        smoothedFilterCut.setCurrentAndTargetValue(cut);
        smoothedFilterRes.reset(sampleRate, 0.01);
        smoothedFilterRes.setCurrentAndTargetValue(res);
    }

    float fifthDepth  = 0.5f;
    float saturation  = 0.5f;
    float filterCut   = 800.0f;
    float filterRes   = 0.3f;
    int   splitPoint  = 60;

private:
    static inline float polyBlepSaw(float phase, float dt)
    {
        float value = 2.0f * phase - 1.0f;

        if (phase < dt)
        {
            const float t = phase / dt;
            value -= t + t - t * t - 1.0f;
        }
        else if (phase > 1.0f - dt)
        {
            const float t = (phase - 1.0f) / dt;
            value -= t * t + t + t + 1.0f;
        }
        return value;
    }

    static constexpr int kNumSaws = 7;

    float baseFreq = 440.0f;
    float oscFreq[kNumSaws] {};
    float oscPhase[kNumSaws] {};
    float detuneCents[kNumSaws] {};
    float fifthFreq  = 0.0f;
    float fifthPhase = 0.0f;
    float level      = 0.0f;
    bool  fifthEnabled = false;

    juce::ADSR env;
    juce::dsp::LadderFilter<float> filter;
    juce::SmoothedValue<float> smoothedFifthDepth;
    juce::SmoothedValue<float> smoothedSaturation;
    juce::SmoothedValue<float> smoothedFilterCut;
    juce::SmoothedValue<float> smoothedFilterRes;
};

//==============================================================================
// Main processor
//==============================================================================
class UpconverterAudioProcessor : public juce::AudioProcessor
{
public:
    UpconverterAudioProcessor()
        : AudioProcessor(BusesProperties()
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          apvts(*this, nullptr, "Parameters", createParameterLayout())
    {
        for (int i = 0; i < 16; ++i)
            synth.addVoice(new UpconverterVoice());

        synth.addSound(new UpconverterSound());
        synth.setNoteStealingEnabled(true);
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID("fifthDepth", 1), "Fifth Depth", 0.0f, 1.0f, 0.5f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID("saturation", 1), "Saturation", 0.0f, 1.0f, 0.5f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID("splitPoint", 1), "Split Point", 24.0f, 96.0f, 60.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID("swing", 1), "Swing", 0.0f, 0.5f, 0.25f));

        juce::NormalisableRange<float> cutoffRange(200.0f, 20000.0f);
        cutoffRange.setSkewForCentre(1000.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID("filterCut", 1), "Cutoff", cutoffRange, 800.0f));

        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID("filterRes", 1), "Resonance", 0.0f, 1.0f, 0.3f));

        return { params.begin(), params.end() };
    }

    //--- Bus / capability flags ------------------------------------------
    bool isSynth()      const override { return true;  }
    bool acceptsMidi()  const override { return true;  }
    bool producesMidi() const override { return true;  } // phrase playback can drive external synths

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto out = layouts.getMainOutputChannelSet();
        return out == juce::AudioChannelSet::stereo()
            || out == juce::AudioChannelSet::mono();
    }

    //--- Lifecycle --------------------------------------------------------
    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        // Adaptive oversampling to ~384 kHz internal rate
        int factor = 2;
        if      (sampleRate <=  50000.0) factor = 8;
        else if (sampleRate <= 100000.0) factor = 4;
        else                             factor = 2;

        oversamplingLog2 = (size_t)std::log2((double)factor);

        oversampling = std::make_unique<juce::dsp::Oversampling<float>>(
            2, oversamplingLog2,
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
            true);

        juce::dsp::ProcessSpec spec { sampleRate,
                                      (juce::uint32)samplesPerBlock,
                                      2 };
        oversampling->initProcessing(spec);

        // Report latency so hosts can delay-compensate
        setLatencySamples((int)oversampling->getLatencyInSamples());

        const double oversampledRate = sampleRate * factor;
        synth.setCurrentPlaybackSampleRate(oversampledRate);

        const float fd  = apvts.getRawParameterValue("fifthDepth")->load();
        const float sat = apvts.getRawParameterValue("saturation")->load();
        const float cut = apvts.getRawParameterValue("filterCut")->load();
        const float res = apvts.getRawParameterValue("filterRes")->load();

        for (auto* v : synth.getVoices())
            if (auto* uv = dynamic_cast<UpconverterVoice*>(v))
                uv->prepareForPlayback(oversampledRate, fd, sat, cut, res);

        phraseEngine.prepare(sampleRate);
    }

    void releaseResources() override {}

    //--- Audio ------------------------------------------------------------
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;

        buffer.clear();
        const int numSamples = buffer.getNumSamples();

        // 1. Capture incoming user MIDI into the phrase recorder
        for (const auto meta : midi)
        {
            const auto msg = meta.getMessage();
            if (msg.isNoteOn())
                phraseEngine.recordNoteOn(msg.getNoteNumber(),
                                          msg.getVelocity(),
                                          meta.samplePosition);
            else if (msg.isNoteOff())
                phraseEngine.recordNoteOff(msg.getNoteNumber(), meta.samplePosition);
        }

        // 2. Overlay phrase playback into the same MIDI buffer
        const float  swing = apvts.getRawParameterValue("swing")->load();
        const double bpm   = getHostBPM();
        phraseEngine.processBlock(midi, numSamples, swing, bpm);

        // 3. Push parameters into the voices
        const float fd    = apvts.getRawParameterValue("fifthDepth")->load();
        const float sat   = apvts.getRawParameterValue("saturation")->load();
        const int   split = (int)apvts.getRawParameterValue("splitPoint")->load();
        const float cut   = apvts.getRawParameterValue("filterCut")->load();
        const float res   = apvts.getRawParameterValue("filterRes")->load();

        for (auto* v : synth.getVoices())
        {
            if (auto* uv = dynamic_cast<UpconverterVoice*>(v))
            {
                uv->fifthDepth = fd;
                uv->saturation = sat;
                uv->splitPoint = split;
                uv->filterCut  = cut;
                uv->filterRes  = res;
            }
        }

        // 4. Scale MIDI sample offsets to the oversampled rate
        const int factor = 1 << oversamplingLog2;
        juce::MidiBuffer scaledMidi;
        for (const auto meta : midi)
            scaledMidi.addEvent(meta.getMessage(), meta.samplePosition * factor);

        // 5. Oversample → render → downsample
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::AudioBlock<float> upBlock = oversampling->processSamplesUp(block);
        synth.renderNextBlock(upBlock, scaledMidi, 0, (int)upBlock.getNumSamples());
        oversampling->processSamplesDown(block);

        // 6. Advance the recording clock
        phraseEngine.tickSampleClock(numSamples);
    }

    //--- State ------------------------------------------------------------
    void getStateInformation(juce::MemoryBlock& destData) override
    {
        juce::XmlElement root("UpconverterState");

        if (auto xml = apvts.copyState().createXml())
            root.addChildElement(xml.release());

        phraseEngine.writeState(root);

        copyXmlToBinary(root, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        auto xml = getXmlFromBinary(data, sizeInBytes);
        if (! xml) return;

        if (auto* paramXml = xml->getChildByName("Parameters"))
            apvts.replaceState(juce::ValueTree::fromXml(*paramXml));

        phraseEngine.readState(*xml);
    }

    //--- Preset loading (idempotent: no playback toggling) ----------------
    void loadPreset(int index)
    {
        if (index < 0 || index >= kNumPresets) return;
        const auto& p = PRESET_LIBRARY[index];

        auto setNorm = [this](const char* id, float norm) {
            if (auto* param = apvts.getParameter(id))
                param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, norm));
        };

        const auto cutoffRange = apvts.getParameterRange("filterCut");

        setNorm("fifthDepth", p.fifthDepth);
        setNorm("saturation", p.saturation);
        setNorm("splitPoint", (p.splitPoint - 24.0f) / (96.0f - 24.0f));
        setNorm("swing",      p.swingAmount / 0.5f);
        setNorm("filterCut",  cutoffRange.convertTo0to1(p.filterCutoff));
        setNorm("filterRes",  p.filterRes);
    }

    //--- Editor -----------------------------------------------------------
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //--- Metadata ---------------------------------------------------------
    const juce::String getName() const override { return "Upconverter Pro"; }
    bool  supportsDoublePrecisionProcessing() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }

    int getNumPrograms() override              { return kNumPresets; }
    int getCurrentProgram() override           { return currentPreset; }
    void setCurrentProgram(int idx) override   { currentPreset = idx; loadPreset(idx); }
    const juce::String getProgramName(int idx) override
    {
        return (idx >= 0 && idx < kNumPresets) ? PRESET_LIBRARY[idx].name : juce::String();
    }
    void changeProgramName(int, const juce::String&) override {}

    //--- Public state -----------------------------------------------------
    PhraseEngine phraseEngine;
    juce::AudioProcessorValueTreeState apvts;

private:
    double getHostBPM() const
    {
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto bpm = pos->getBpm())
                    return juce::jmax(30.0, *bpm);
        return 120.0;
    }

    juce::Synthesiser synth;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    size_t oversamplingLog2 = 3;
    int    currentPreset    = 0;

    //==========================================================================
    // Editor
    //==========================================================================
    class Editor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        Editor(UpconverterAudioProcessor& p)
            : AudioProcessorEditor(&p), processor(p)
        {
            setSize(760, 440);

            titleLabel.setText("UPCONVERTER PRO", juce::dontSendNotification);
            titleLabel.setFont(juce::Font(juce::FontOptions(22.0f).withStyle("Bold")));
            titleLabel.setColour(juce::Label::textColourId,
                                 juce::Colour::fromRGB(255, 190, 100));
            addAndMakeVisible(titleLabel);

            presetCombo.addItemList({ "Trance Lead", "Ambient Pad", "Cinematic Swell", "EDM Pluck" }, 1);
            presetCombo.setTextWhenNothingSelected("Preset...");
            presetCombo.onChange = [this] {
                const int idx = presetCombo.getSelectedId() - 1;
                if (idx >= 0 && idx < kNumPresets)
                {
                    processor.loadPreset(idx);
                    processor.currentPreset = idx;
                }
            };
            addAndMakeVisible(presetCombo);

            recordBtn.onClick   = [this] { toggleRecording(); };
            playBtn.onClick     = [this] { togglePlaying();   };
            generateBtn.onClick = [this] { processor.phraseEngine.generateVariation(); refresh(); };
            clearBtn.onClick    = [this] { processor.phraseEngine.clear();           refresh(); };

            addAndMakeVisible(recordBtn);
            addAndMakeVisible(playBtn);
            addAndMakeVisible(generateBtn);
            addAndMakeVisible(clearBtn);

            addSlider("fifthDepth",  "5th Depth",  0.0f,     1.0f,  0.5f);
            addSlider("saturation",  "Saturation", 0.0f,     1.0f,  0.5f);
            addSlider("splitPoint",  "Split Pt",   24.0f,   96.0f, 60.0f);
            addSlider("swing",       "Swing",      0.0f,     0.5f,  0.25f);
            addSlider("filterCut",   "Cutoff",     200.0f, 20000.0f, 800.0f);
            addSlider("filterRes",   "Resonance",  0.0f,     1.0f,  0.3f);

            statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            statusLabel.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(statusLabel);

            refresh();
            startTimerHz(10);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour::fromRGB(28, 30, 38));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(16);

            auto top = area.removeFromTop(36);
            titleLabel.setBounds(top.removeFromLeft(240));
            top.removeFromLeft(20);
            presetCombo.setBounds(top.removeFromLeft(160));

            area.removeFromTop(10);

            auto transport = area.removeFromTop(36);
            recordBtn.setBounds(transport.removeFromLeft(100));
            transport.removeFromLeft(6);
            playBtn.setBounds(transport.removeFromLeft(110));
            transport.removeFromLeft(6);
            generateBtn.setBounds(transport.removeFromLeft(160));
            transport.removeFromLeft(6);
            clearBtn.setBounds(transport.removeFromLeft(70));
            transport.removeFromLeft(12);
            statusLabel.setBounds(transport);

            area.removeFromTop(16);

            auto sliderRow = area.removeFromTop(180);
            const int n = (int)sliders.size();
            const int colW = sliderRow.getWidth() / juce::jmax(1, n);

            for (int i = 0; i < n; ++i)
            {
                auto col = sliderRow.removeFromLeft(colW);
                labels[(size_t)i]->setBounds(col.removeFromBottom(18));
                sliders[(size_t)i]->setBounds(col.reduced(6, 0));
            }
        }

    private:
        void timerCallback() override { refresh(); }

        void toggleRecording()
        {
            processor.phraseEngine.setRecording(! processor.phraseEngine.isRecording());
            refresh();
        }

        void togglePlaying()
        {
            processor.phraseEngine.setPlaying(! processor.phraseEngine.isPlaying());
            refresh();
        }

        void refresh()
        {
            const bool rec = processor.phraseEngine.isRecording();
            const bool ply = processor.phraseEngine.isPlaying();

            recordBtn.setButtonText(rec ? "Recording..." : "Record");
            recordBtn.setColour(juce::TextButton::buttonColourId,
                                rec ? juce::Colour::fromRGB(200, 60, 40)
                                    : juce::Colour::fromRGB(60, 60, 70));

            playBtn.setButtonText(ply ? "Pause" : "Play Phrase");
            playBtn.setColour(juce::TextButton::buttonColourId,
                              ply ? juce::Colour::fromRGB(40, 140, 90)
                                  : juce::Colour::fromRGB(60, 60, 70));

            const int n = processor.phraseEngine.getNumNotes();
            statusLabel.setText(n + " note" + (n == 1 ? "" : "s") + " in phrase",
                                juce::dontSendNotification);
        }

        void addSlider(const juce::String& paramId,
                       const juce::String& displayName,
                       float min, float max, float def)
        {
            auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryVerticalDrag,
                                                    juce::Slider::TextBoxBelow);
            s->setRange(min, max);
            s->setValue(def);
            s->setNumDecimalPlacesToDisplay((max - min < 10.0f) ? 2 : 0);
            s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 16);
            addAndMakeVisible(*s);

            auto att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                processor.apvts, paramId, *s);

            auto lbl = std::make_unique<juce::Label>();
            lbl->setText(displayName, juce::dontSendNotification);
            lbl->setJustificationType(juce::Justification::centred);
            lbl->setFont(juce::Font(juce::FontOptions(11.0f)));
            lbl->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible(*lbl);

            sliders.push_back(std::move(s));
            attachments.push_back(std::move(att));
            labels.push_back(std::move(lbl));
        }

        UpconverterAudioProcessor& processor;

        juce::Label titleLabel;
        juce::ComboBox presetCombo;

        juce::TextButton recordBtn   { "Record" };
        juce::TextButton playBtn     { "Play Phrase" };
        juce::TextButton generateBtn { "Generate Variation" };
        juce::TextButton clearBtn    { "Clear" };

        juce::Label statusLabel;

        std::vector<std::unique_ptr<juce::Slider>> sliders;
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> attachments;
        std::vector<std::unique_ptr<juce::Label>> labels;
    };
};

//==============================================================================
// Editor factory (must be defined after the nested Editor class)
//==============================================================================
juce::AudioProcessorEditor* UpconverterAudioProcessor::createEditor()
{
    return new Editor(*this);
}

//==============================================================================
// Plugin entry point
//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new UpconverterAudioProcessor();
}

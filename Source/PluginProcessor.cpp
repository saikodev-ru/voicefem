#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <BinaryData.h>

namespace
{
struct Preset
{
    const char* name;
    float pitch, formant;
    int mode;          // 0 = Shift, 1 = Target
    float target, intonation, breath, body, air, mix, gain, voicing;
    float natural, deEss;
};

// name, pitch(st), formant(st), mode, targetHz, intonation, breath, body(dB), air(dB), mix, gain(dB), voicing, natural, deEss
const Preset kPresets[] = {
    { "Feminine - Standard",        4.5f, 2.5f, 0, 200.f, 1.00f, 0.10f, -2.5f, 3.0f, 1.f, 0.f, 0.55f, 0.50f, 0.35f },
    { "Feminine - Deep Transform",  6.0f, 3.2f, 0, 210.f, 1.10f, 0.14f, -4.0f, 4.0f, 1.f, 0.f, 0.55f, 0.60f, 0.40f },
    { "Feminine - Soft Natural",    3.0f, 1.5f, 0, 200.f, 1.00f, 0.06f, -1.5f, 2.0f, 1.f, 0.f, 0.55f, 0.45f, 0.30f },
    { "Feminine - Subtle Alto",     2.0f, 1.0f, 0, 200.f, 1.00f, 0.00f, -1.5f, 1.5f, 1.f, 0.f, 0.55f, 0.40f, 0.25f },
    { "Feminine - Bright",          6.0f, 3.5f, 0, 205.f, 1.15f, 0.12f, -3.5f, 4.5f, 1.f, 0.f, 0.55f, 0.55f, 0.45f },
    { "Feminine - Young / High",    8.0f, 4.5f, 0, 210.f, 1.20f, 0.12f, -4.5f, 5.5f, 1.f, 0.f, 0.55f, 0.55f, 0.50f },
    { "Feminine - Breathy Soft",    4.0f, 2.2f, 0, 200.f, 1.05f, 0.38f, -2.5f, 4.5f, 1.f, 0.f, 0.50f, 0.70f, 0.30f },
    { "Target 200 Hz - Natural",    0.0f, 2.5f, 1, 200.f, 1.00f, 0.10f, -2.5f, 3.0f, 1.f, 0.f, 0.55f, 0.50f, 0.35f },
    { "Target 220 Hz - Expressive", 0.0f, 3.0f, 1, 220.f, 1.25f, 0.12f, -3.5f, 4.0f, 1.f, 0.f, 0.55f, 0.60f, 0.40f },
    { "Target 180 Hz - Low Alto",   0.0f, 2.0f, 1, 180.f, 1.00f, 0.06f, -2.0f, 2.5f, 1.f, 0.f, 0.55f, 0.45f, 0.30f },
    { "Timbre Only (keep pitch)",   0.0f, 3.0f, 0, 200.f, 1.00f, 0.06f, -2.5f, 2.5f, 1.f, 0.f, 0.55f, 0.45f, 0.35f },
    { "Neutral (reference)",        0.0f, 0.0f, 0, 200.f, 1.00f, 0.00f,  0.0f, 0.0f, 1.f, 0.f, 0.55f, 0.00f, 0.00f },
};
constexpr int kNumPresets = (int) (sizeof (kPresets) / sizeof (kPresets[0]));
} // namespace

using APVTS = juce::AudioProcessorValueTreeState;

APVTS::ParameterLayout DyVocalizerProcessor::createLayout()
{
    using F = juce::AudioParameterFloat;
    using C = juce::AudioParameterChoice;
    using I = juce::AudioParameterInt;
    using NR = juce::NormalisableRange<float>;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back (std::make_unique<C> (juce::ParameterID { "engine", 1 }, "Engine",
                                       juce::StringArray { "DSP (PSOLA)", "Neural (model)" }, 0));
    p.push_back (std::make_unique<I> (juce::ParameterID { "voice", 1 }, "Neural Voice", 0, 31, 0));
    p.push_back (std::make_unique<F> (juce::ParameterID { "pitch", 1 }, "Pitch (st)", NR (-12.f, 12.f, 0.01f), 4.5f));
    p.push_back (std::make_unique<F> (juce::ParameterID { "formant", 1 }, "Formant (st)", NR (-6.f, 8.f, 0.01f), 2.5f));
    p.push_back (std::make_unique<C> (juce::ParameterID { "mode", 1 }, "Pitch Mode", juce::StringArray { "Shift", "Target" }, 0));
    p.push_back (std::make_unique<F> (juce::ParameterID { "target", 1 }, "Target Hz", NR (100.f, 400.f, 1.f), 200.f));
    p.push_back (std::make_unique<F> (juce::ParameterID { "intonation", 1 }, "Intonation", NR (0.f, 1.5f, 0.01f), 1.f));
    p.push_back (std::make_unique<F> (juce::ParameterID { "breath", 1 }, "Breath", NR (0.f, 1.f, 0.01f), 0.10f));
    p.push_back (std::make_unique<F> (juce::ParameterID { "body", 1 }, "Body (dB)", NR (-12.f, 6.f, 0.1f), -2.5f));
    p.push_back (std::make_unique<F> (juce::ParameterID { "air", 1 }, "Air (dB)", NR (-6.f, 12.f, 0.1f), 3.f));
    p.push_back (std::make_unique<F> (juce::ParameterID { "natural", 1 }, "Natural", NR (0.f, 1.f, 0.01f), 0.5f));
    p.push_back (std::make_unique<F> (juce::ParameterID { "deess", 1 }, "De-Ess", NR (0.f, 1.f, 0.01f), 0.35f));
    p.push_back (std::make_unique<F> (juce::ParameterID { "mix", 1 }, "Mix", NR (0.f, 1.f, 0.01f), 1.f));
    p.push_back (std::make_unique<F> (juce::ParameterID { "gain", 1 }, "Output (dB)", NR (-12.f, 12.f, 0.1f), 0.f));
    p.push_back (std::make_unique<F> (juce::ParameterID { "voicing", 1 }, "Voicing Thr", NR (0.3f, 0.85f, 0.01f), 0.55f));
    p.push_back (std::make_unique<C> (juce::ParameterID { "range", 1 }, "Voice Range (latency)",
                                       juce::StringArray { "Deep (65 Hz)", "Standard (80 Hz)", "Higher (100 Hz)" }, 1));
    return { p.begin(), p.end() };
}

float DyVocalizerProcessor::rangeHz (int choice)
{
    return choice == 0 ? 65.f : (choice == 2 ? 100.f : 80.f);
}

juce::File DyVocalizerProcessor::modelsDir()
{
    const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    const auto dir = appData.getChildFile ("DyVocalizer").getChildFile ("models");
    const auto legacy = appData.getChildFile ("VoiceFem").getChildFile ("models"); // pre-rename location
    return (! dir.isDirectory() && legacy.isDirectory()) ? legacy : dir;
}

juce::String DyVocalizerProcessor::neuralStatus() const
{
    if (! neuralBuilt())
        return "DSP-only build: neural engine not included";
    switch (neuralState.load())
    {
        case 1:  return loadedVoice >= 0 && loadedVoice < voiceNames.size()
                       ? "ready: " + voiceNames[loadedVoice] : "ready";
        case -1: return statusNote.isNotEmpty() ? statusNote : "neural unavailable";
        default: return contentLoaded.load() ? "loading voice..." : "loading...";
    }
}

DyVocalizerProcessor::DyVocalizerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VOICEFEM", createLayout())
{
    startTimerHz (4); // model loading / status refresh (message thread)
}

DyVocalizerProcessor::~DyVocalizerProcessor()
{
    shuttingDown = true;
    importPool.removeAllJobs (true, 15000);
}

juce::String DyVocalizerProcessor::getImportStatus() const
{
    const juce::ScopedLock sl (importLock);
    return importStatus;
}

void DyVocalizerProcessor::rescanModels() { rescanRequested = true; }

void DyVocalizerProcessor::importModel (const juce::File& file)
{
    auto setStatus = [this] (const juce::String& t) { const juce::ScopedLock sl (importLock); importStatus = t; };
    const auto ext = file.getFileExtension().toLowerCase();
    const auto dir = modelsDir();
    dir.createDirectory();

    if (ext == ".onnx")
    {
        auto name = file.getFileName();
        if (! name.startsWithIgnoreCase ("voice_") && name.equalsIgnoreCase ("content.onnx") == false)
            name = "voice_" + name;
        setStatus (file.copyFileTo (dir.getChildFile (name)) ? "imported: " + name : "import failed");
        rescanRequested = true;
        return;
    }
    if (ext != ".pth") { setStatus ("unsupported file (use .pth or .onnx)"); return; }

    setStatus ("converting " + file.getFileName() + " ...");
    importPool.addJob ([this, file, dir, setStatus]
    {
        const auto toolDir = dir.getParentDirectory().getChildFile ("tools");
        toolDir.createDirectory();
        const auto script = toolDir.getChildFile ("pth_to_onnx.py");
        script.replaceWithData (BinaryData::pth_to_onnx_py, (size_t) BinaryData::pth_to_onnx_pySize);

        const juce::StringArray pythons { "python", "py -3", "python3" };
        for (auto& py : pythons)
        {
            juce::ChildProcess proc;
            const auto cmd = py + " \"" + script.getFullPathName() + "\" \"" + file.getFullPathName()
                           + "\" --out \"" + dir.getFullPathName() + "\"";
            if (! proc.start (cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
                continue;
            while (proc.isRunning())
            {
                if (shuttingDown.load()) { proc.kill(); return; }
                juce::Thread::sleep (200);
            }
            const auto out = proc.readAllProcessOutput().trim();
            const auto last = out.fromLastOccurrenceOf ("\n", false, false).trim();
            if (proc.getExitCode() == 0)
            {
                setStatus ("converted: " + file.getFileNameWithoutExtension());
                rescanRequested = true;
            }
            else if (out.containsIgnoreCase ("No module named") || out.containsIgnoreCase ("is not recognized"))
                continue; // try next python launcher
            else
                setStatus (last.isEmpty() ? "conversion failed" : last.substring (0, 90));
            return;
        }
        setStatus ("need Python + torch/onnx (pip install torch onnx)");
    });
}

bool DyVocalizerProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    if (in != out) return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void DyVocalizerProcessor::applyLatency()
{
    const bool neuralActive = currentEngine == 1 && neuralState.load() == 1;
    setLatencySamples (neuralActive ? neural.getLatency() : engine.getLatency());
}

void DyVocalizerProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    currentRange = (int) apvts.getRawParameterValue ("range")->load();
    engine.prepare (sampleRate, rangeHz (currentRange));
    neural.prepare (sampleRate);
    applyLatency();
}

void DyVocalizerProcessor::timerCallback()
{
#if DYV_NEURAL
    if (rescanRequested.exchange (false))
    {
        voiceNames.clear(); voicePaths.clear(); loadedVoice = -1; neuralState = 0;
    }

    const juce::uint32 now = juce::Time::getMillisecondCounter();
    const bool backoffOk = lastScanAttempt == 0 || now - lastScanAttempt > 2000; // 2 s retry cycle

    // scan the model folder until voices are found; keeps retrying (files may
    // be installed while the host is running)
    if (voiceNames.isEmpty() && neuralState.load() != 1 && backoffOk)
    {
        lastScanAttempt = now;
        const auto dir = modelsDir();
        auto list = nvf::NeuralEngine::scanVoices (dir.getFullPathName().toStdString());
        if (list.empty())
        {
            neuralState = -1;
            statusNote = "no models in " + dir.getFullPathName();
        }
        else
        {
            voiceNames.clear();
            voicePaths.clear();
            for (auto& v : list) { voiceNames.add (juce::String (v.name)); voicePaths.push_back (v.path); }
            neuralState = 0;
            statusNote.clear();
            // voice param range stays 0..31; out-of-range values are clamped on use
            if (juce::AudioProcessorEditor* ed = getActiveEditor())
                if (auto* e = dynamic_cast<DyVocalizerEditor*> (ed))
                    e->refreshVoiceList (voiceNames);
        }
    }

    if (! voicePaths.empty())
    {
        const int want = (int) apvts.getRawParameterValue ("voice")->load();
        const int clamped = juce::jlimit (0, (int) voicePaths.size() - 1, want);

        // content encoder: load once, retry with a 2 s backoff while failing
        if (! contentLoaded.load() && (lastContentAttempt == 0 || now - lastContentAttempt > 2000))
        {
            lastContentAttempt = now;
            std::string err;
            const auto cpath = modelsDir().getChildFile ("content.onnx").getFullPathName().toStdString();
            if (neural.loadContent (cpath, std::max (1, (int) juce::SystemStats::getNumCpus() / 2), &err))
                contentLoaded = true;
            else
            {
                neuralState = -1;
                statusNote = "content.onnx: " + juce::String (err);
            }
        }

        // voice model: (re)load on switch; retry with backoff while failing
        const bool needVoice = contentLoaded.load() && (neuralState.load() != 1 || clamped != loadedVoice);
        const bool voiceBackoffOk = lastVoiceAttempt == 0 || now - lastVoiceAttempt > 2000;
        if (needVoice && voiceBackoffOk)
        {
            lastVoiceAttempt = now;
            std::string err;
            const double sr = neural.loadVoice (voicePaths[(size_t) clamped], &err);
            if (sr > 0)
            {
                loadedVoice = clamped;
                neuralState = 1;
                neural.reset();
                applyLatency();
            }
            else
            {
                neuralState = -1;
                statusNote = "voice model: " + juce::String (err);
            }
        }
    }

    if (auto* ed = getActiveEditor())
        if (auto* e = dynamic_cast<DyVocalizerEditor*> (ed))
            e->updateNeuralStatus (neuralStatus());
#endif
}

void DyVocalizerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    for (int ch = numIn; ch < numOut; ++ch) buffer.clear (ch, 0, numSamples);

    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    const int range = (int) get ("range");
    if (range != currentRange)
    {
        currentRange = range;
        engine.prepare (currentSampleRate, rangeHz (range));
        applyLatency();
    }

    const int engineMode = (int) get ("engine");
    if (engineMode != currentEngine)
    {
        currentEngine = engineMode;
        applyLatency();
    }

    dyv::Params P;
    P.pitchSemis = get ("pitch");
    P.formantSemis = get ("formant");
    P.targetMode = (int) get ("mode") == 1;
    P.targetHz = get ("target");
    P.intonation = get ("intonation");
    P.breath = get ("breath");
    P.bodyDb = get ("body");
    P.airDb = get ("air");
    P.mix = get ("mix");
    P.outDb = get ("gain");
    P.voicingThresh = get ("voicing");
    P.natural = get ("natural");
    P.deEss = get ("deess");
    engine.setParams (P);

    const bool useNeuralPath = currentEngine == 1 && neuralState.load() == 1;
    if (useNeuralPath)
        neural.setParams (P.pitchSemis, P.targetMode, P.targetHz, P.intonation);

    const int chans = juce::jmin (numIn, numOut);
    if (chans <= 0) return;

    auto* d0 = buffer.getWritePointer (0);
    auto* d1 = chans > 1 ? buffer.getWritePointer (1) : nullptr;

    const float outGain = std::pow (10.f, P.outDb / 20.f);
    const float wet = juce::jlimit (0.f, 1.f, P.mix);

    for (int i = 0; i < numSamples; ++i)
    {
        const float x = d1 ? 0.5f * (d0[i] + d1[i]) : d0[i]; // voice = mono
        float y;
        if (useNeuralPath)
        {
            const float n = neural.process (x);
            y = wet * dyv::softClip (n * outGain) + (1.f - wet) * x;
        }
        else
        {
            y = engine.process (x);
        }
        d0[i] = y;
        if (d1) d1[i] = y;
    }
}

int DyVocalizerProcessor::getNumPrograms() { return kNumPresets; }
const juce::String DyVocalizerProcessor::getProgramName (int i)
{
    return (i >= 0 && i < kNumPresets) ? kPresets[i].name : juce::String();
}

void DyVocalizerProcessor::setCurrentProgram (int index)
{
    if (index < 0 || index >= kNumPresets) return;
    currentProgram = index;
    const auto& s = kPresets[index];

    auto set = [this] (const char* id, float v)
    {
        if (auto* prm = apvts.getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    };
    set ("pitch", s.pitch);
    set ("formant", s.formant);
    set ("mode", (float) s.mode);
    set ("target", s.target);
    set ("intonation", s.intonation);
    set ("breath", s.breath);
    set ("body", s.body);
    set ("air", s.air);
    set ("natural", s.natural);
    set ("deess", s.deEss);
    set ("mix", s.mix);
    set ("gain", s.gain);
    set ("voicing", s.voicing);
}

void DyVocalizerProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}

void DyVocalizerProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* DyVocalizerProcessor::createEditor() { return new DyVocalizerEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new DyVocalizerProcessor(); }

#include "PluginProcessor.h"
#include "PluginEditor.h"

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

APVTS::ParameterLayout VoiceFemProcessor::createLayout()
{
    using F = juce::AudioParameterFloat;
    using C = juce::AudioParameterChoice;
    using NR = juce::NormalisableRange<float>;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
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

float VoiceFemProcessor::rangeHz (int choice)
{
    return choice == 0 ? 65.f : (choice == 2 ? 100.f : 80.f);
}

VoiceFemProcessor::VoiceFemProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VOICEFEM", createLayout())
{
}

bool VoiceFemProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    if (in != out) return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void VoiceFemProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    currentRange = (int) apvts.getRawParameterValue ("range")->load();
    engine.prepare (sampleRate, rangeHz (currentRange));
    setLatencySamples (engine.getLatency());
}

void VoiceFemProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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
        setLatencySamples (engine.getLatency());
    }

    vfem::Params P;
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

    const int chans = juce::jmin (numIn, numOut);
    if (chans <= 0) return;

    auto* d0 = buffer.getWritePointer (0);
    auto* d1 = chans > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        const float x = d1 ? 0.5f * (d0[i] + d1[i]) : d0[i]; // voice = mono
        const float y = engine.process (x);
        d0[i] = y;
        if (d1) d1[i] = y;
    }
}

int VoiceFemProcessor::getNumPrograms() { return kNumPresets; }
const juce::String VoiceFemProcessor::getProgramName (int i)
{
    return (i >= 0 && i < kNumPresets) ? kPresets[i].name : juce::String();
}

void VoiceFemProcessor::setCurrentProgram (int index)
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

void VoiceFemProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}

void VoiceFemProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* VoiceFemProcessor::createEditor() { return new VoiceFemEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VoiceFemProcessor(); }

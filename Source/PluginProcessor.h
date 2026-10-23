#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "DyVocalizerEngine.h"
#include "NeuralVC.h"

class DyVocalizerProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
public:
    DyVocalizerProcessor();
    ~DyVocalizerProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "DyVocalizer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // neural engine introspection for the editor (message thread only)
    static constexpr const char* kVersionString = "0.4.0";
    static bool neuralBuilt()
    {
#if DYV_NEURAL
        return true;
#else
        return false;
#endif
    }
    juce::StringArray getVoiceNames() const { return voiceNames; }
    juce::String neuralStatus() const;
    static juce::File modelsDir();

    // model import: .onnx is copied as-is, .pth is converted via the bundled python tool
    void importModel (const juce::File& file);
    juce::String getImportStatus() const;
    void rescanModels();

    void timerCallback();

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static float rangeHz (int choice);
    void applyLatency();

    dyv::Engine engine;
    nvf::NeuralEngine neural;

    double currentSampleRate = 48000.0;
    int currentRange = -1;
    int currentEngine = 0;
    int currentQuality = -1;
    int currentProgram = 0;
    std::vector<float> monoBuf, dryBuf;   // block scratch (audio thread)

    // neural loading state (touched on message thread + atomics for audio thread)
    std::atomic<int> neuralState { 0 };   // 0 = no models, 1 = ready, -1 = failed
    std::atomic<bool> contentLoaded { false };
    int loadedVoice = -1;
    juce::StringArray voiceNames;
    std::vector<std::string> voicePaths;

    // retry bookkeeping (message thread only)
    juce::uint32 lastScanAttempt = 0, lastContentAttempt = 0, lastVoiceAttempt = 0;
    juce::String statusNote;              // human-readable reason for the current state

    mutable juce::CriticalSection importLock;
    juce::String importStatus;
    std::atomic<bool> shuttingDown { false };
    std::atomic<bool> rescanRequested { false };
    juce::ThreadPool importPool { 1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DyVocalizerProcessor)
};

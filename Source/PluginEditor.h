#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class VoiceFemEditor : public juce::AudioProcessorEditor
{
public:
    explicit VoiceFemEditor (VoiceFemProcessor&);
    ~VoiceFemEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refreshVoiceList (const juce::StringArray& names);
    void updateNeuralStatus (const juce::String& status);

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attach;
    };
    struct Choice
    {
        juce::ComboBox box;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attach;
    };

    void addKnob (const char* paramId, const char* text);
    void addChoice (const char* paramId, const char* text);

    VoiceFemProcessor& proc;
    juce::ComboBox presetBox;
    juce::Label presetLabel;
    juce::ComboBox voiceBox;
    juce::Label voiceLabel;
    juce::Label statusLabel;
    juce::TextButton modelsBtn;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<Choice>> choices;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceFemEditor)
};

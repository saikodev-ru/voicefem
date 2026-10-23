#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "Fl11LookAndFeel.h"

class DyVocalizerEditor : public juce::AudioProcessorEditor,
                       public juce::FileDragAndDropTarget
{
public:
    explicit DyVocalizerEditor (DyVocalizerProcessor&);
    ~DyVocalizerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refreshVoiceList (const juce::StringArray& names);
    void updateNeuralStatus (const juce::String& status);

    // drag a .pth / .onnx model onto the window to import it
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    struct HintSlider : juce::Slider
    {
        std::function<void (bool)> onHover;
        void mouseEnter (const juce::MouseEvent& e) override { juce::Slider::mouseEnter (e); if (onHover) onHover (true); }
        void mouseExit  (const juce::MouseEvent& e) override { juce::Slider::mouseExit (e);  if (onHover) onHover (false); }
    };
    struct Knob
    {
        HintSlider slider;
        juce::Label label;
        juce::String paramId, hint;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attach;
    };
    struct Choice
    {
        juce::ComboBox box;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attach;
    };

    void addKnob (const char* paramId, const char* text, const char* hint);
    void addChoice (const char* paramId, const char* text);
    void updateEnabledStates();
    Knob* knobFor (const char* paramId);

    Fl11LookAndFeel fl11;
    DyVocalizerProcessor& proc;
    juce::TooltipWindow tooltips { this, 600 };
    juce::ComboBox presetBox;
    juce::ComboBox voiceBox;
    juce::Label voiceLabel;
    juce::TextButton importButton { "Import model..." };
    juce::TextButton modelsBtn { "Models..." };
    juce::Label statusLabel, hintLabel;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<Choice>> choices;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> headerRect, panelRects[3];
    static constexpr const char* kPanelTitles[3] = { "PITCH & TIMBRE", "CHARACTER", "CLEAN-UP & OUTPUT" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DyVocalizerEditor)
};

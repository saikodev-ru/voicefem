#include "PluginEditor.h"

VoiceFemEditor::VoiceFemEditor (VoiceFemProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    presetLabel.setText ("Preset", juce::dontSendNotification);
    presetLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (presetLabel);

    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem (proc.getProgramName (i), i + 1);
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); };
    addAndMakeVisible (presetBox);

    addChoice ("engine", "Engine");
    addChoice ("mode", "Pitch Mode");
    addChoice ("range", "Voice Range");

    voiceLabel.setText ("Neural Voice", juce::dontSendNotification);
    voiceLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (voiceLabel);
    voiceBox.addItem ("(no models in %APPDATA%\\VoiceFem\\models)", 1);
    voiceBox.setEnabled (false);
    voiceBox.onChange = [this]
    {
        if (auto* prm = dynamic_cast<juce::AudioParameterInt*> (proc.apvts.getParameter ("voice")))
            prm->setValueNotifyingHost (prm->convertTo0to1 (voiceBox.getSelectedId() - 1));
    };
    addAndMakeVisible (voiceBox);

    statusLabel.setText ("", juce::dontSendNotification);
    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9d94ab));
    addAndMakeVisible (statusLabel);

    addKnob ("pitch", "Pitch");
    addKnob ("formant", "Formant");
    addKnob ("target", "Target Hz");
    addKnob ("intonation", "Intonation");
    addKnob ("breath", "Breath");
    addKnob ("body", "Body");
    addKnob ("air", "Air");
    addKnob ("natural", "Natural");
    addKnob ("deess", "De-Ess");
    addKnob ("voicing", "Voicing");
    addKnob ("mix", "Mix");
    addKnob ("gain", "Output");

    setSize (760, 500);
}

void VoiceFemEditor::addKnob (const char* id, const char* text)
{
    auto k = std::make_unique<Knob>();
    k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    k->label.setText (text, juce::dontSendNotification);
    k->label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (k->slider);
    addAndMakeVisible (k->label);
    k->attach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k->slider);
    knobs.push_back (std::move (k));
}

void VoiceFemEditor::addChoice (const char* id, const char* text)
{
    auto c = std::make_unique<Choice>();
    c->label.setText (text, juce::dontSendNotification);
    c->label.setJustificationType (juce::Justification::centredRight);
    if (auto* prm = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
        c->box.addItemList (prm->choices, 1);
    addAndMakeVisible (c->box);
    addAndMakeVisible (c->label);
    c->attach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, id, c->box);
    choices.push_back (std::move (c));
}

void VoiceFemEditor::refreshVoiceList (const juce::StringArray& names)
{
    voiceBox.clear (juce::dontSendNotification);
    if (names.isEmpty())
    {
        voiceBox.addItem ("(no models in %APPDATA%\\VoiceFem\\models)", 1);
        voiceBox.setSelectedId (1, juce::dontSendNotification);
        voiceBox.setEnabled (false);
        return;
    }
    for (int i = 0; i < names.size(); ++i)
        voiceBox.addItem (names[i], i + 1);
    voiceBox.setSelectedId (0, juce::dontSendNotification);
    voiceBox.setEnabled (true);
}

void VoiceFemEditor::updateNeuralStatus (const juce::String& status)
{
    if (statusLabel.getText() != status) statusLabel.setText (status, juce::dontSendNotification);
}

void VoiceFemEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1c1a24));
    g.setColour (juce::Colour (0xffe9a8d0));
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("VoiceFem", 16, 10, 200, 30, juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xff6b6478));
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("realtime feminine voice engine v2", 220, 16, 300, 20, juce::Justification::centredLeft);
}

void VoiceFemEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    auto top = area.removeFromTop (34);
    top.removeFromLeft (200);
    presetBox.setBounds (top.removeFromRight (260));
    presetLabel.setBounds (top.removeFromRight (70));

    area.removeFromTop (6);
    auto selRow = area.removeFromTop (30);
    const int third = selRow.getWidth() / 3;
    for (int i = 0; i < (int) choices.size() && i < 3; ++i)
    {
        auto cell = selRow.removeFromLeft (third);
        choices[i]->label.setBounds (cell.removeFromLeft (86));
        choices[i]->box.setBounds (cell.reduced (4, 0).removeFromLeft (third - 96));
    }

    area.removeFromTop (6);
    auto voiceRow = area.removeFromTop (30);
    statusLabel.setBounds (voiceRow.removeFromRight (200));
    voiceLabel.setBounds (voiceRow.removeFromLeft (86));
    voiceBox.setBounds (voiceRow.reduced (4, 0).removeFromLeft (200));

    area.removeFromTop (6);
    const int cols = 4, rows = 3;
    const int cw = area.getWidth() / cols, ch = area.getHeight() / rows;
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        juce::Rectangle<int> cell (area.getX() + (int) (i % cols) * cw, area.getY() + (int) (i / cols) * ch, cw, ch);
        cell.reduce (6, 4);
        knobs[i]->label.setBounds (cell.removeFromTop (20));
        knobs[i]->slider.setBounds (cell);
    }
}

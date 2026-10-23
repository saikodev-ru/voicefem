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

    addKnob ("pitch", "Pitch");
    addKnob ("formant", "Formant");
    addKnob ("target", "Target Hz");
    addKnob ("intonation", "Intonation");
    addKnob ("breath", "Breath");
    addKnob ("body", "Body");
    addKnob ("air", "Air");
    addKnob ("voicing", "Voicing");
    addKnob ("mix", "Mix");
    addKnob ("gain", "Output");

    addChoice ("mode", "Pitch Mode");
    addChoice ("range", "Voice Range");

    setSize (760, 400);
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

void VoiceFemEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1c1a24));
    g.setColour (juce::Colour (0xffe9a8d0));
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("VoiceFem", 16, 10, 200, 30, juce::Justification::centredLeft);
}

void VoiceFemEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    auto top = area.removeFromTop (34);
    top.removeFromLeft (200);
    presetBox.setBounds (top.removeFromRight (260));
    presetLabel.setBounds (top.removeFromRight (70));

    area.removeFromTop (8);
    auto selRow = area.removeFromTop (30);
    if (choices.size() == 2)
    {
        auto a = selRow.removeFromLeft (selRow.getWidth() / 2);
        choices[0]->label.setBounds (a.removeFromLeft (90));
        choices[0]->box.setBounds (a.reduced (6, 0).removeFromLeft (160));
        choices[1]->label.setBounds (selRow.removeFromLeft (110));
        choices[1]->box.setBounds (selRow.reduced (6, 0).removeFromLeft (190));
    }

    area.removeFromTop (8);
    const int cols = 5, rows = 2;
    const int cw = area.getWidth() / cols, ch = area.getHeight() / rows;
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        juce::Rectangle<int> cell (area.getX() + (int) (i % cols) * cw, area.getY() + (int) (i / cols) * ch, cw, ch);
        cell.reduce (6, 4);
        knobs[i]->label.setBounds (cell.removeFromTop (20));
        knobs[i]->slider.setBounds (cell);
    }
}

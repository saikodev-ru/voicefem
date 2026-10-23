#include "PluginEditor.h"

DyVocalizerEditor::DyVocalizerEditor (DyVocalizerProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&fl11);

    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem (proc.getProgramName (i), i + 1);
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.setTooltip ("Presets - a good starting point. Then tweak the knobs below.");
    presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); };
    addAndMakeVisible (presetBox);

    addChoice ("engine", "Engine");
    addChoice ("mode", "Pitch Mode");
    addChoice ("range", "Voice Range");
    choices[0]->box.setTooltip ("DSP = fast, light, no models needed.  Neural = full voice change using a model (.pth/.onnx).");
    choices[1]->box.setTooltip ("Shift = fixed semitone shift.  Target = automatically aims your average pitch at Target Hz.");
    choices[2]->box.setTooltip ("Lowest pitch of your voice. Deeper = more latency but safer tracking.");
    choices[0]->box.onChange = [this] { updateEnabledStates(); };
    choices[1]->box.onChange = [this] { updateEnabledStates(); };

    voiceLabel.setText ("Neural Voice", juce::dontSendNotification);
    voiceLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (voiceLabel);
    voiceBox.addItem (proc.neuralBuilt()
                          ? "(no models - click Import or drop a .pth here)"
                          : "(neural not included in this build)", 1);
    voiceBox.setSelectedId (1, juce::dontSendNotification);
    voiceBox.setEnabled (false);
    voiceBox.setTooltip ("Voice model used by the Neural engine.");
    voiceBox.onChange = [this]
    {
        if (auto* prm = dynamic_cast<juce::AudioParameterInt*> (proc.apvts.getParameter ("voice")))
            prm->setValueNotifyingHost (prm->convertTo0to1 (voiceBox.getSelectedId() - 1));
    };
    addAndMakeVisible (voiceBox);

    importButton.setTooltip ("Add a voice model. .pth (RVC v2) is converted automatically, .onnx is copied.");
    importButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Import voice model", juce::File(), "*.pth;*.onnx");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile()) proc.importModel (fc.getResult());
                              });
    };
    addAndMakeVisible (importButton);

    modelsBtn.setTooltip ("Open the neural model folder (%APPDATA%\\DyVocalizer\\models)");
    modelsBtn.onClick = [this]
    {
        auto dir = DyVocalizerProcessor::modelsDir();
        if (! dir.isDirectory()) dir.createDirectory();
        dir.revealToUser();
    };
    addAndMakeVisible (modelsBtn);

    statusLabel.setText (proc.neuralBuilt() ? "loading..." : "DSP-only build: neural engine not included",
                         juce::dontSendNotification);
    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, Fl11::textDim());
    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (statusLabel);

    hintLabel.setJustificationType (juce::Justification::centredLeft);
    hintLabel.setColour (juce::Label::textColourId, Fl11::textDim());
    hintLabel.setText ("Hover a control to see what it does.", juce::dontSendNotification);
    addAndMakeVisible (hintLabel);

    addKnob ("pitch",      "Pitch",     "Pitch: shifts your voice up/down in semitones (+4...+6 sounds feminine). Used in Shift mode.");
    addKnob ("formant",    "Formant",   "Formant: vocal-tract size. Higher = smaller, brighter timbre. DSP engine only.");
    addKnob ("target",     "Target Hz", "Target Hz: pitch to aim for in Target mode (female speech is usually 180-230 Hz).");
    addKnob ("intonation", "Intonation","Intonation: 0 = monotone, 1 = as you speak, above 1 = more expressive.");
    addKnob ("breath",     "Breath",    "Breath: airy noise added on voiced sounds.");
    addKnob ("body",       "Body",      "Body: low shelf at 250 Hz. Lower = less chesty / boomy.");
    addKnob ("air",        "Air",       "Air: brightness and presence. Higher = lighter, more ringing voice.");
    addKnob ("natural",    "Natural",   "Natural: tiny pitch/volume wobble so the voice doesn't sound sterile.");
    addKnob ("deess",      "De-Ess",    "De-Ess: tames harsh S / SH sounds.");
    addKnob ("voicing",    "Voicing",   "Voicing threshold: raise if hissing sounds glitch, lower if quiet vowels drop out.");
    addKnob ("mix",        "Mix",       "Mix: dry/wet balance. 1.0 = fully processed.");
    addKnob ("gain",       "Output",    "Output: final volume in dB.");

    updateEnabledStates();
    setSize (780, 560);
}

DyVocalizerEditor::~DyVocalizerEditor() { setLookAndFeel (nullptr); }

DyVocalizerEditor::Knob* DyVocalizerEditor::knobFor (const char* id)
{
    for (auto& k : knobs) if (k->paramId == id) return k.get();
    return nullptr;
}

void DyVocalizerEditor::addKnob (const char* id, const char* text, const char* hint)
{
    auto k = std::make_unique<Knob>();
    k->paramId = id; k->hint = hint;
    k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k->slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 18);
    k->slider.setTooltip (hint);
    k->slider.setDoubleClickReturnValue (true, proc.apvts.getParameter (id)->convertFrom0to1 (proc.apvts.getParameter (id)->getDefaultValue()));
    k->slider.onHover = [this, h = juce::String (hint)] (bool in)
    {
        hintLabel.setText (in ? h : "Hover a control to see what it does.", juce::dontSendNotification);
    };
    k->label.setText (text, juce::dontSendNotification);
    k->label.setJustificationType (juce::Justification::centred);
    k->label.setFont (Fl11::font (12.0f, true));
    addAndMakeVisible (k->slider);
    addAndMakeVisible (k->label);
    k->attach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k->slider);
    knobs.push_back (std::move (k));
}

void DyVocalizerEditor::addChoice (const char* id, const char* text)
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

void DyVocalizerEditor::updateEnabledStates()
{
    const bool neural = choices[0]->box.getSelectedItemIndex() == 1;
    const bool target = choices[1]->box.getSelectedItemIndex() == 1;
    if (auto* k = knobFor ("formant")) k->slider.setEnabled (! neural);
    if (auto* k = knobFor ("pitch"))   k->slider.setEnabled (! target);
    if (auto* k = knobFor ("target"))  k->slider.setEnabled (target);
    choices[2]->box.setEnabled (! neural);
    voiceBox.setEnabled (neural && voiceBox.getNumItems() > 0 && proc.getVoiceNames().size() > 0);
    repaint();
}

void DyVocalizerEditor::refreshVoiceList (const juce::StringArray& names)
{
    voiceBox.clear (juce::dontSendNotification);
    if (names.isEmpty())
    {
        voiceBox.addItem ("(no models - click Import or drop a .pth here)", 1);
        voiceBox.setSelectedId (1, juce::dontSendNotification);
        voiceBox.setEnabled (false);
        return;
    }
    for (int i = 0; i < names.size(); ++i)
        voiceBox.addItem (names[i], i + 1);
    const int cur = (int) proc.apvts.getRawParameterValue ("voice")->load();
    voiceBox.setSelectedId (juce::jlimit (0, names.size() - 1, cur) + 1, juce::dontSendNotification);
    updateEnabledStates();
}

void DyVocalizerEditor::updateNeuralStatus (const juce::String& status)
{
    const auto imp = proc.getImportStatus();
    const auto t = imp.isEmpty() ? status : imp + "   |   " + status;
    if (statusLabel.getText() != t) statusLabel.setText (t, juce::dontSendNotification);
}

bool DyVocalizerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
    {
        const auto e = juce::File (f).getFileExtension().toLowerCase();
        if (e == ".pth" || e == ".onnx") return true;
    }
    return false;
}

void DyVocalizerEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files) proc.importModel (juce::File (f));
}

void DyVocalizerEditor::paint (juce::Graphics& g)
{
    using namespace juce;
    g.setGradientFill (ColourGradient (Fl11::bgTop(), 0, 0, Fl11::bgBottom(), 0, (float) getHeight(), false));
    g.fillAll();

    // header strip
    g.setGradientFill (ColourGradient (Colour (0xff59646c), 0, 0, Colour (0xff353d43), 0, (float) headerRect.getBottom(), false));
    g.fillRect (headerRect);
    g.setColour (Fl11::edgeDark());
    g.drawLine (0, (float) headerRect.getBottom(), (float) getWidth(), (float) headerRect.getBottom(), 1.5f);
    g.setColour (Fl11::edgeLight().withAlpha (0.6f));
    g.drawLine (0, 0.5f, (float) getWidth(), 0.5f);

    g.setColour (Fl11::orange());
    g.setFont (Fl11::font (20.0f, true));
    g.drawText ("DyVocalizer", 14, 0, 140, headerRect.getHeight(), Justification::centredLeft);
    g.setColour (Fl11::textDim());
    g.setFont (Fl11::font (11.0f));
    g.drawText ("realtime voice engine", 118, 4, 160, headerRect.getHeight(), Justification::centredLeft);
    g.drawText (juce::String ("v") + proc.kVersionString
                    + (proc.neuralBuilt() ? " - neural engine" : " - DSP-only build"),
                118, 19, 300, 18, Justification::centredLeft);

    // section panels
    for (int i = 0; i < 3; ++i)
    {
        auto r = panelRects[i].toFloat();
        g.setGradientFill (ColourGradient (Fl11::panelTop(), 0, r.getY(), Fl11::panelBot(), 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (Fl11::edgeDark());
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        g.setColour (Fl11::edgeLight().withAlpha (0.35f));
        g.drawLine (r.getX() + 4, r.getY() + 1.5f, r.getRight() - 4, r.getY() + 1.5f);
        g.setColour (Fl11::textDim());
        g.setFont (Fl11::font (10.0f, true));
        g.drawText (kPanelTitles[i], panelRects[i].getX() + 10, panelRects[i].getY() + 4, 220, 14, Justification::centredLeft);
    }

    // drop-zone hint line above the hint bar
    g.setColour (Fl11::edgeDark());
    g.drawLine (0, (float) getHeight() - 28.0f, (float) getWidth(), (float) getHeight() - 28.0f);
}

void DyVocalizerEditor::resized()
{
    auto area = getLocalBounds();
    headerRect = area.removeFromTop (40);

    auto top = headerRect.reduced (10, 8);
    importButton.setBounds (top.removeFromRight (110));
    modelsBtn.setBounds (top.removeFromRight (88));
    top.removeFromRight (8);
    presetBox.setBounds (top.removeFromRight (230));
    // (title is painted on the left)

    // footer: hint (left) + status (right)
    auto foot = area.removeFromBottom (28).reduced (10, 4);
    statusLabel.setBounds (foot.removeFromRight (getWidth() / 2 - 20));
    hintLabel.setBounds (foot);

    area.reduce (12, 8);

    auto selRow = area.removeFromTop (28);
    const int third = selRow.getWidth() / 3;
    for (int i = 0; i < (int) choices.size() && i < 3; ++i)
    {
        auto cell = selRow.removeFromLeft (third);
        choices[(size_t) i]->label.setBounds (cell.removeFromLeft (80));
        choices[(size_t) i]->box.setBounds (cell.reduced (6, 2).withWidth (juce::jmin (170, cell.getWidth() - 12)));
    }

    area.removeFromTop (4);
    auto voiceRow = area.removeFromTop (28);
    voiceLabel.setBounds (voiceRow.removeFromLeft (80));
    voiceBox.setBounds (voiceRow.reduced (6, 2).removeFromLeft (360));

    area.removeFromTop (6);
    const int ph = (area.getHeight() - 2 * 8) / 3;
    for (int p = 0; p < 3; ++p)
    {
        panelRects[p] = area.removeFromTop (ph);
        area.removeFromTop (8);
        auto inner = panelRects[p].reduced (8, 0);
        inner.removeFromTop (16);
        const int cw = inner.getWidth() / 4;
        for (int c = 0; c < 4; ++c)
        {
            auto& k = *knobs[(size_t) (p * 4 + c)];
            auto cell = inner.removeFromLeft (cw);
            // label left of the knob, knob + LCD value on the right: compact rows
            k.label.setBounds (cell.removeFromTop (16));
            k.slider.setBounds (cell);
        }
    }
}

#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// FL Studio 11 flavoured skin: blue-grey bevelled panels, round dark knobs with
// a light rim + white pointer, orange value arcs, LCD-like value boxes.
struct Fl11
{
    static juce::Colour bgTop()    { return juce::Colour (0xff4a545b); }
    static juce::Colour bgBottom() { return juce::Colour (0xff2e353a); }
    static juce::Colour panelTop() { return juce::Colour (0xff3c454b); }
    static juce::Colour panelBot() { return juce::Colour (0xff343c41); }
    static juce::Colour edgeDark() { return juce::Colour (0xff171b1e); }
    static juce::Colour edgeLight(){ return juce::Colour (0xff66727a); }
    static juce::Colour orange()   { return juce::Colour (0xffff9d2e); }
    static juce::Colour green()    { return juce::Colour (0xff8fd14f); }
    static juce::Colour lcdBg()    { return juce::Colour (0xff1b2023); }
    static juce::Colour text()     { return juce::Colour (0xffd5dde2); }
    static juce::Colour textDim()  { return juce::Colour (0xff8d99a1); }
    static juce::Font font (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions ("Tahoma", h, bold ? juce::Font::bold : juce::Font::plain));
    }
};

class Fl11LookAndFeel : public juce::LookAndFeel_V4
{
public:
    Fl11LookAndFeel()
    {
        using namespace juce;
        setColour (Slider::textBoxTextColourId, Fl11::orange());
        setColour (Slider::textBoxBackgroundColourId, Fl11::lcdBg());
        setColour (Slider::textBoxOutlineColourId, Fl11::edgeDark());
        setColour (Slider::textBoxHighlightColourId, Fl11::orange().withAlpha (0.4f));
        setColour (Label::textColourId, Fl11::text());
        setColour (ComboBox::textColourId, Fl11::text());
        setColour (PopupMenu::backgroundColourId, Fl11::panelBot());
        setColour (PopupMenu::textColourId, Fl11::text());
        setColour (PopupMenu::highlightedBackgroundColourId, Fl11::orange().darker (0.2f));
        setColour (PopupMenu::highlightedTextColourId, Colours::black);
        setColour (TextButton::textColourOffId, Fl11::text());
        setColour (TextButton::textColourOnId, Fl11::text());
    }

    juce::Font getLabelFont (juce::Label&) override        { return Fl11::font (12.0f); }
    juce::Font getComboBoxFont (juce::ComboBox&) override  { return Fl11::font (12.0f); }
    juce::Font getPopupMenuFont() override                 { return Fl11::font (12.0f); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return Fl11::font (12.0f, true); }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float a0, float a1, juce::Slider& s) override
    {
        using namespace juce;
        const float alpha = s.isEnabled() ? 1.0f : 0.35f;
        auto b = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (6.0f);
        const float d = jmin (b.getWidth(), b.getHeight());
        auto c = b.withSizeKeepingCentre (d, d);
        const float ang = a0 + pos * (a1 - a0);

        // value arc (orange), outside the knob
        {
            Path track, val;
            const auto arcR = c.reduced (0.0f);
            track.addCentredArc (arcR.getCentreX(), arcR.getCentreY(), d * 0.5f, d * 0.5f, 0, a0, a1, true);
            val.addCentredArc (arcR.getCentreX(), arcR.getCentreY(), d * 0.5f, d * 0.5f, 0, a0, ang, true);
            g.setColour (Fl11::edgeDark().withAlpha (alpha));
            g.strokePath (track, PathStrokeType (3.5f, PathStrokeType::curved, PathStrokeType::rounded));
            g.setColour (Fl11::orange().withAlpha (alpha));
            g.strokePath (val, PathStrokeType (3.5f, PathStrokeType::curved, PathStrokeType::rounded));
        }

        auto kn = c.reduced (6.0f);
        g.setColour (Colours::black.withAlpha (0.45f * alpha));
        g.fillEllipse (kn.translated (0.0f, 1.5f));                      // drop shadow
        g.setGradientFill (ColourGradient (Fl11::edgeLight().withAlpha (alpha), kn.getX(), kn.getY(),
                                           Fl11::edgeDark().withAlpha (alpha), kn.getX(), kn.getBottom(), false));
        g.fillEllipse (kn);                                              // rim
        auto in = kn.reduced (2.5f);
        g.setGradientFill (ColourGradient (Colour (0xff59646c).withAlpha (alpha), in.getX(), in.getY(),
                                           Colour (0xff23292d).withAlpha (alpha), in.getX(), in.getBottom(), false));
        g.fillEllipse (in);                                              // body

        Path p;
        const float r = in.getWidth() * 0.5f;
        p.addRoundedRectangle (-1.5f, -r + 2.0f, 3.0f, r * 0.55f, 1.2f);
        g.setColour (Colours::white.withAlpha (alpha));
        g.fillPath (p, AffineTransform::rotation (ang).translated (in.getCentreX(), in.getCentreY()));
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        using namespace juce;
        auto r = Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
        const float alpha = box.isEnabled() ? 1.0f : 0.5f;
        g.setGradientFill (ColourGradient (Colour (0xff626d75).withAlpha (alpha), 0, r.getY(),
                                           Colour (0xff3a4247).withAlpha (alpha), 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (Fl11::edgeDark());
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        g.setColour (Fl11::edgeLight().withAlpha (0.5f * alpha));
        g.drawLine (r.getX() + 3, r.getY() + 1.5f, r.getRight() - 3, r.getY() + 1.5f);
        Path tri;
        const float cx = (float) w - 12.0f, cy = h * 0.5f;
        tri.addTriangle (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
        g.setColour (Fl11::orange().withAlpha (alpha));
        g.fillPath (tri);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (6, 1, box.getWidth() - 24, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                               bool hover, bool down) override
    {
        using namespace juce;
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto top = down ? Colour (0xff2f373c) : (hover ? Colour (0xff7a868e) : Colour (0xff626d75));
        auto bot = down ? Colour (0xff3d464c) : Colour (0xff3a4247);
        g.setGradientFill (ColourGradient (top, 0, r.getY(), bot, 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (hover ? Fl11::orange() : Fl11::edgeDark());
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }
};

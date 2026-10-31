#include "PluginEditor.h"

using namespace juce;

const Colour StudioLookAndFeel::background { 0xff111317 };
const Colour StudioLookAndFeel::panel { 0xff1b1e24 };
const Colour StudioLookAndFeel::accent { 0xff36d1c4 };
const Colour StudioLookAndFeel::accent2 { 0xfff2a541 };
const Colour StudioLookAndFeel::text { 0xffe6e8eb };
const Colour StudioLookAndFeel::dim { 0xff7c8490 };

//==============================================================================
StudioLookAndFeel::StudioLookAndFeel()
{
    setColour (Slider::textBoxTextColourId, text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour (Label::textColourId, text);
    setColour (TextButton::buttonColourId, Colour (0xff262a32));
    setColour (TextButton::buttonOnColourId, accent2);
    setColour (TextButton::textColourOffId, text);
    setColour (TextButton::textColourOnId, Colours::black);
    setColour (ComboBox::outlineColourId, Colours::transparentBlack);
}

void StudioLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float start, float end,
                                          Slider& s)
{
    const auto bounds = Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
    const float radius = jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto centre = bounds.getCentre();
    const float angle = start + pos * (end - start);
    const float track = jmax (3.0f, radius * 0.16f);

    Path bg;
    bg.addCentredArc (centre.x, centre.y, radius - track, radius - track, 0.0f, start, end, true);
    g.setColour (Colour (0xff2c313a));
    g.strokePath (bg, PathStrokeType (track, PathStrokeType::curved, PathStrokeType::rounded));

    // Bipolar parameters draw from the centre.
    const auto range = s.getRange();
    const bool bipolar = range.getStart() < 0.0 && range.getEnd() > 0.0;
    const float from = bipolar ? start + (float) s.valueToProportionOfLength (0.0) * (end - start) : start;
    Path arc;
    arc.addCentredArc (centre.x, centre.y, radius - track, radius - track, 0.0f, jmin (from, angle), jmax (from, angle), true);
    g.setColour (s.isEnabled() ? accent : dim);
    g.strokePath (arc, PathStrokeType (track, PathStrokeType::curved, PathStrokeType::rounded));

    const float knobR = radius - track * 2.2f;
    g.setColour (Colour (0xff2a2f38));
    g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2, knobR * 2);
    Path needle;
    needle.addRoundedRectangle (-1.5f, -knobR, 3.0f, knobR * 0.55f, 1.5f);
    g.setColour (text);
    g.fillPath (needle, AffineTransform::rotation (angle).translated (centre));
}

void StudioLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                          Slider::SliderStyle style, Slider& s)
{
    if (style != Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
        return;
    }
    const float cx = x + w * 0.5f;
    const float zeroY = (float) y + (float) h * (1.0f - (float) s.valueToProportionOfLength (0.0));
    g.setColour (Colour (0xff2c313a));
    g.fillRoundedRectangle (cx - 2.0f, (float) y, 4.0f, (float) h, 2.0f);
    g.setColour (dim.withAlpha (0.6f));
    g.drawHorizontalLine ((int) zeroY, cx - 8.0f, cx + 8.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (cx - 2.0f, jmin (zeroY, pos), 4.0f, std::abs (zeroY - pos), 2.0f);
    g.setColour (text);
    g.fillRoundedRectangle (cx - 9.0f, pos - 3.0f, 18.0f, 6.0f, 3.0f);
}

void StudioLookAndFeel::drawToggleButton (Graphics& g, ToggleButton& b, bool, bool)
{
    const auto r = b.getLocalBounds().toFloat();
    const bool on = b.getToggleState();
    const float d = jmin (14.0f, r.getHeight() - 2.0f);
    const auto dot = Rectangle<float> (r.getX() + 2.0f, r.getCentreY() - d / 2, d, d);
    g.setColour (on ? accent : Colour (0xff3a3f48));
    g.fillEllipse (dot);
    if (b.getButtonText().isNotEmpty())
    {
        g.setColour (on ? text : dim);
        g.setFont (FontOptions (12.0f));
        g.drawText (b.getButtonText(), r.withTrimmedLeft (d + 8.0f), Justification::centredLeft);
    }
}

//==============================================================================
Knob::Knob (AudioProcessorValueTreeState& s, const String& paramId, const String& caption)
    : attachment (s, paramId, slider)
{
    slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (Slider::TextBoxBelow, false, 80, 16);
    slider.setPopupDisplayEnabled (false, false, nullptr);
    if (auto* p = s.getParameter (paramId))
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    label.setText (caption, dontSendNotification);
    label.setJustificationType (Justification::centred);
    label.setFont (FontOptions (12.0f));
    label.setColour (Label::textColourId, StudioLookAndFeel::dim);
    addAndMakeVisible (slider);
    addAndMakeVisible (label);
}

void Knob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    slider.setBounds (r);
}

BandFader::BandFader (AudioProcessorValueTreeState& s, const String& paramId, const String& caption)
    : attachment (s, paramId, slider)
{
    slider.setSliderStyle (Slider::LinearVertical);
    slider.setTextBoxStyle (Slider::TextBoxBelow, false, 48, 14);
    slider.setDoubleClickReturnValue (true, 0.0);
    label.setText (caption, dontSendNotification);
    label.setJustificationType (Justification::centred);
    label.setFont (FontOptions (11.0f));
    label.setColour (Label::textColourId, StudioLookAndFeel::dim);
    addAndMakeVisible (slider);
    addAndMakeVisible (label);
}

void BandFader::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromBottom (14));
    slider.setBounds (r);
}

//==============================================================================
Section::Section (AudioProcessorValueTreeState& s, const String& t, const String& powerParam) : title (t)
{
    addAndMakeVisible (power);
    if (powerParam.isNotEmpty())
        powerAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (s, powerParam, power);
    else
        power.setVisible (false);
}

void Section::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (StudioLookAndFeel::panel);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (StudioLookAndFeel::text);
    g.setFont (FontOptions (13.0f, Font::bold));
    g.drawText (title.toUpperCase(), r.reduced (12, 8).removeFromTop (16).withTrimmedLeft (power.isVisible() ? 22 : 0),
                Justification::centredLeft);
}

void Section::resized()
{
    power.setBounds (10, 8, 18, 18);
    if (items.empty())
        return;
    auto area = getContentArea();
    const int rows = ((int) items.size() + columns - 1) / columns;
    const int cw = area.getWidth() / columns, rh = area.getHeight() / jmax (1, rows);
    for (size_t i = 0; i < items.size(); ++i)
    {
        const int col = (int) i % columns, row = (int) i / columns;
        items[i]->setBounds (area.getX() + col * cw, area.getY() + row * rh, cw, rh);
    }
}

//==============================================================================
void MeterStrip::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (StudioLookAndFeel::panel);
    g.fillRoundedRectangle (r, 8.0f);
    r = r.reduced (12.0f, 10.0f);

    g.setFont (FontOptions (13.0f, Font::bold));
    g.setColour (StudioLookAndFeel::text);
    g.drawText ("METERS", r.removeFromTop (18.0f), Justification::centredLeft);

    auto lufsRow = [&] (const String& name, float v) {
        auto row = r.removeFromTop (38.0f);
        g.setFont (FontOptions (11.0f));
        g.setColour (StudioLookAndFeel::dim);
        g.drawText (name, row.removeFromTop (14.0f), Justification::centredLeft);
        g.setFont (FontOptions (20.0f, Font::bold));
        g.setColour (StudioLookAndFeel::text);
        g.drawText (v <= -69.0f ? String ("-inf") : String (v, 1) + " LUFS", row, Justification::centredLeft);
    };
    lufsRow ("INPUT (momentary)", inLufs);
    lufsRow ("OUTPUT (momentary)", outLufs);
    r.removeFromTop (8.0f);

    for (const auto& row : rows)
    {
        auto line = r.removeFromTop (30.0f);
        g.setFont (FontOptions (11.0f));
        g.setColour (StudioLookAndFeel::dim);
        auto top = line.removeFromTop (14.0f);
        g.drawText (row.name, top, Justification::centredLeft);
        g.drawText ((row.bipolar && row.value > 0 ? "+" : "") + String (row.bipolar ? row.value : -row.value, 1) + " dB",
                    top, Justification::centredRight);
        auto bar = line.removeFromTop (8.0f);
        g.setColour (Colour (0xff2c313a));
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (row.bipolar ? StudioLookAndFeel::accent : StudioLookAndFeel::accent2);
        if (row.bipolar)
        {
            const float mid = bar.getCentreX();
            const float px = jlimit (-1.0f, 1.0f, row.value / row.maxValue) * bar.getWidth() * 0.5f;
            g.fillRoundedRectangle (Rectangle<float> (jmin (mid, mid + px), bar.getY(), std::abs (px), bar.getHeight()), 3.0f);
        }
        else
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * jlimit (0.0f, 1.0f, row.value / row.maxValue)), 3.0f);
    }
}

//==============================================================================
StudioEngineEditor::StudioEngineEditor (StudioEngineProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      clean (p.getState(), "Clean-up", "dn_on"),
      matchEq (p.getState(), "Match EQ  /  Tone", "match_on"),
      dynamics (p.getState(), "De-ess  /  Compress", "comp_on"),
      mixFit (p.getState(), "Mix Fit (instrumental)", "mix_on"),
      master (p.getState(), "Master", "lim_on")
{
    setLookAndFeel (&lnf);
    auto& s = p.getState();

    // --- Clean-up -----------------------------------------------------------
    clean.setGridColumns (3);
    clean.setBottomReserve (30);
    knob (clean, "dn_amount", "Denoise");
    knob (clean, "dn_sensitivity", "Sensitivity");
    knob (clean, "hpf_freq", "Low Cut");
    knob (clean, "gate_threshold", "Gate Thresh");
    knob (clean, "gate_range", "Gate Range");
    knob (clean, "gate_release", "Gate Release");
    addAndMakeVisible (clean);
    clean.addAndMakeVisible (learnButton);
    clean.addAndMakeVisible (clearNoiseButton);
    clean.addAndMakeVisible (adaptiveToggle);
    adaptiveAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (s, "dn_adaptive", adaptiveToggle);
    learnButton.setClickingTogglesState (true);
    learnButton.setTooltip ("Press, play a few seconds of background noise only (no voice), press again.");
    learnButton.onClick = [this] { proc.setLearningNoise (learnButton.getToggleState()); };
    clearNoiseButton.onClick = [this] { proc.clearNoiseProfile(); };

    // --- Match EQ + tone ------------------------------------------------------
    const char* bandIds[] { "match_31", "match_62", "match_125", "match_250", "match_500",
                            "match_1k", "match_2k", "match_4k", "match_8k", "match_16k" };
    const char* bandNames[] { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
    matchEq.setGridColumns (15);
    for (int i = 0; i < 10; ++i)
    {
        auto f = std::make_unique<BandFader> (s, bandIds[i], bandNames[i]);
        matchEq.addItem (f.get());
        owned.push_back (std::move (f));
    }
    knob (matchEq, "match_amount", "Amount");
    knob (matchEq, "eq_low_gain", "Low");
    knob (matchEq, "eq_mud_gain", "Mud");
    knob (matchEq, "eq_presence_gain", "Presence");
    knob (matchEq, "eq_air_gain", "Air");
    addAndMakeVisible (matchEq);

    // --- Dynamics ----------------------------------------------------------------
    dynamics.setGridColumns (5);
    knob (dynamics, "deess_threshold", "De-ess Thr");
    knob (dynamics, "deess_range", "De-ess Range");
    knob (dynamics, "deess_freq", "De-ess Freq");
    knob (dynamics, "comp_attack", "Attack");
    knob (dynamics, "comp_release", "Release");
    knob (dynamics, "comp_threshold", "Threshold");
    knob (dynamics, "comp_ratio", "Ratio");
    knob (dynamics, "comp_knee", "Knee");
    knob (dynamics, "comp_makeup", "Makeup");
    knob (dynamics, "comp_mix", "Mix");
    addAndMakeVisible (dynamics);

    // --- Mix fit -------------------------------------------------------------------
    mixFit.setGridColumns (3);
    mixFit.setBottomReserve (20);
    knob (mixFit, "mix_offset", "Vocal vs Beat");
    knob (mixFit, "mix_range", "Ride Range");
    knob (mixFit, "mix_unmask", "Unmask");
    addAndMakeVisible (mixFit);
    mixFit.addAndMakeVisible (sidechainLabel);
    sidechainLabel.setFont (FontOptions (11.0f));
    sidechainLabel.setJustificationType (Justification::centred);

    // --- Master ----------------------------------------------------------------------
    master.setGridColumns (3);
    knob (master, "sat_drive", "Warmth");
    knob (master, "sat_mix", "Warmth Mix");
    knob (master, "output_gain", "Output");
    knob (master, "lim_drive", "Loudness");
    knob (master, "lim_ceiling", "Ceiling");
    knob (master, "lim_release", "Release");
    addAndMakeVisible (master);

    // --- Header ---------------------------------------------------------------------
    addAndMakeVisible (loadButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (profileLabel);
    addAndMakeVisible (hintLabel);
    profileLabel.setFont (FontOptions (14.0f));
    profileLabel.setJustificationType (Justification::centredRight);
    hintLabel.setFont (FontOptions (11.0f));
    hintLabel.setColour (Label::textColourId, StudioLookAndFeel::dim);
    hintLabel.setText ("Vocal clean-up & mastering  |  route your instrumental to the side-chain input for Mix Fit",
                       dontSendNotification);
    loadButton.onClick = [this] { loadProfile(); };
    saveButton.onClick = [this] { saveProfile(); };

    meters.rows = { { "Denoise", 0, 30 }, { "Gate", 0, 30 }, { "De-ess", 0, 12 }, { "Compressor", 0, 18 },
                    { "Limiter", 0, 12 }, { "Vocal Ride", 0, 12, true }, { "Unmask", 0, 9, true } };
    addAndMakeVisible (meters);

    setResizable (true, true);
    setResizeLimits (980, 640, 1800, 1200);
    setSize (1180, 720);
    startTimerHz (30);
}

StudioEngineEditor::~StudioEngineEditor()
{
    setLookAndFeel (nullptr);
}

Knob* StudioEngineEditor::knob (Section& s, const String& id, const String& caption)
{
    auto k = std::make_unique<Knob> (proc.getState(), id, caption);
    auto* raw = k.get();
    s.addItem (raw);
    owned.push_back (std::move (k));
    return raw;
}

void StudioEngineEditor::paint (Graphics& g)
{
    g.fillAll (StudioLookAndFeel::background);
    auto header = getLocalBounds().removeFromTop (56).reduced (16, 8);
    g.setColour (StudioLookAndFeel::text);
    g.setFont (FontOptions (24.0f, Font::bold));
    g.drawText ("STUDIO", header, Justification::centredLeft);
    g.setColour (StudioLookAndFeel::accent);
    g.drawText ("ENGINE", header.withTrimmedLeft (98), Justification::centredLeft);
}

void StudioEngineEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (56).reduced (16, 10);
    header.removeFromLeft (210);
    saveButton.setBounds (header.removeFromRight (110));
    header.removeFromRight (8);
    loadButton.setBounds (header.removeFromRight (110));
    header.removeFromRight (12);
    profileLabel.setBounds (header.removeFromRight (240));
    hintLabel.setBounds (header);

    r = r.reduced (12, 0).withTrimmedBottom (12);
    meters.setBounds (r.removeFromRight (220));
    r.removeFromRight (10);

    const int gap = 10;
    auto top = r.removeFromTop ((r.getHeight() - gap) / 2);
    r.removeFromTop (gap);
    clean.setBounds (top.removeFromLeft (jmax (330, top.getWidth() * 2 / 5)));
    top.removeFromLeft (gap);
    dynamics.setBounds (top);

    matchEq.setBounds (r.removeFromLeft (r.getWidth() * 55 / 100));
    r.removeFromLeft (gap);
    auto right = r;
    mixFit.setBounds (right.removeFromTop ((right.getHeight() - gap) * 40 / 100));
    right.removeFromTop (gap);
    master.setBounds (right);

    // Clean-up extras sit along the panel's bottom edge.
    auto ca = clean.getLocalBounds().reduced (10);
    auto strip = ca.removeFromBottom (26);
    learnButton.setBounds (strip.removeFromLeft (110));
    strip.removeFromLeft (6);
    clearNoiseButton.setBounds (strip.removeFromLeft (60));
    strip.removeFromLeft (10);
    adaptiveToggle.setBounds (strip.removeFromLeft (100));

    sidechainLabel.setBounds (mixFit.getLocalBounds().reduced (10).removeFromBottom (18));
}

void StudioEngineEditor::timerCallback()
{
    auto& m = proc.getMeters();
    meters.inLufs = m.inputLufs.load();
    meters.outLufs = m.outputLufs.load();
    const float vals[] { m.denoiseDb.load(), m.gateDb.load(), m.deessDb.load(), m.compDb.load(),
                         m.limiterDb.load(), m.rideDb.load(), m.unmaskDb.load() };
    for (size_t i = 0; i < meters.rows.size(); ++i)
    {
        auto& row = meters.rows[i];
        row.value = row.value * 0.6f + vals[i] * 0.4f; // light visual smoothing
    }
    meters.repaint();

    // Learning starts on the audio thread, i.e. only once audio is flowing.
    if (learnButton.getToggleState())
        learnButton.setButtonText (proc.isLearningNoise() ? "Learning... stop" : "Waiting for audio");
    else
        learnButton.setButtonText (proc.hasNoiseProfile() ? "Relearn Noise" : "Learn Noise");

    profileLabel.setText ("Profile: " + proc.getProfileName(), dontSendNotification);
    const bool sc = proc.isSidechainConnected();
    sidechainLabel.setText (sc ? "Instrumental connected" : "No instrumental on side-chain (Mix Fit idle)",
                            dontSendNotification);
    sidechainLabel.setColour (Label::textColourId, sc ? StudioLookAndFeel::accent : StudioLookAndFeel::dim);
}

void StudioEngineEditor::loadProfile()
{
    chooser = std::make_unique<FileChooser> ("Load Studio Engine profile",
                                             File::getSpecialLocation (File::userDocumentsDirectory), "*.json");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (! f.existsAsFile())
                                  return;
                              String err;
                              if (! proc.loadProfile (f, err))
                                  AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon,
                                                                    "Could not load profile", err);
                          });
}

void StudioEngineEditor::saveProfile()
{
    chooser = std::make_unique<FileChooser> ("Save Studio Engine profile",
                                             File::getSpecialLocation (File::userDocumentsDirectory)
                                                 .getChildFile (proc.getProfileName() + ".json"),
                                             "*.json");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting,
                          [this] (const FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == File())
                                  return;
                              f = f.withFileExtension ("json");
                              if (! proc.saveProfile (f, f.getFileNameWithoutExtension()))
                                  AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon,
                                                                    "Could not save profile", f.getFullPathName());
                          });
}

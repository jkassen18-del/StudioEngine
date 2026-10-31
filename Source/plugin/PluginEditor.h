#pragma once
#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

//==============================================================================
class StudioLookAndFeel : public juce::LookAndFeel_V4
{
public:
    StudioLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end,
                           juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    static const juce::Colour background, panel, accent, accent2, text, dim;
};

//==============================================================================
/** Rotary knob + caption, attached to a parameter. */
class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& s, const juce::String& paramId, const juce::String& caption);
    void resized() override;
    juce::Slider slider;

private:
    juce::Label label;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

/** Vertical fader for one Match EQ band. */
class BandFader : public juce::Component
{
public:
    BandFader (juce::AudioProcessorValueTreeState& s, const juce::String& paramId, const juce::String& caption);
    void resized() override;

private:
    juce::Slider slider;
    juce::Label label;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

/** Titled panel with a power switch bound to an "_on" parameter. */
class Section : public juce::Component
{
public:
    Section (juce::AudioProcessorValueTreeState& s, const juce::String& title, const juce::String& powerParam);
    void paint (juce::Graphics&) override;
    void resized() override;

    /** Lays children out in a grid of the given column count below the title. */
    void setGridColumns (int c) { columns = c; }
    /** Pixels kept free at the bottom for extra controls. */
    void setBottomReserve (int px) { bottomReserve = px; }
    void addItem (juce::Component* c) { items.push_back (c); addAndMakeVisible (c); }
    juce::Rectangle<int> getContentArea() const
    {
        return getLocalBounds().reduced (10).withTrimmedTop (28).withTrimmedBottom (bottomReserve);
    }

private:
    juce::String title;
    juce::ToggleButton power;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> powerAttachment;
    std::vector<juce::Component*> items;
    int columns = 3, bottomReserve = 0;
};

/** Horizontal gain-reduction / level meters. */
class MeterStrip : public juce::Component
{
public:
    struct Row
    {
        juce::String name;
        float value = 0.0f, maxValue = 24.0f;
        bool bipolar = false;
    };
    std::vector<Row> rows;
    float inLufs = -70.f, outLufs = -70.f;
    void paint (juce::Graphics&) override;
};

//==============================================================================
class StudioEngineEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit StudioEngineEditor (StudioEngineProcessor&);
    ~StudioEngineEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    Knob* knob (Section& s, const juce::String& id, const juce::String& caption);
    void loadProfile();
    void saveProfile();

    StudioEngineProcessor& proc;
    StudioLookAndFeel lnf;

    Section clean, matchEq, dynamics, mixFit, master;
    std::vector<std::unique_ptr<juce::Component>> owned;

    juce::TextButton learnButton { "Learn Noise" }, clearNoiseButton { "Clear" };
    juce::ToggleButton adaptiveToggle { "Adaptive" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> adaptiveAttachment;
    juce::TextButton loadButton { "Load Profile" }, saveButton { "Save Profile" };
    juce::Label profileLabel, sidechainLabel, hintLabel;
    MeterStrip meters;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StudioEngineEditor)
};

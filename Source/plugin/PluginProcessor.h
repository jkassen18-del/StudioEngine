#pragma once
#include "../dsp/Profile.h"
#include "../dsp/StudioEngine.h"
#include <juce_audio_processors/juce_audio_processors.h>

class StudioEngineProcessor : public juce::AudioProcessor
{
public:
    StudioEngineProcessor();
    ~StudioEngineProcessor() override = default;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    juce::AudioProcessorValueTreeState& getState() { return apvts; }
    se::Meters& getMeters() { return engine.getMeters(); }

    /** Noise-profile learning ("play a section of pure background noise"). */
    void setLearningNoise (bool shouldLearn) { engine.getDenoiser().setLearning (shouldLearn); }
    bool isLearningNoise() { return engine.getDenoiser().isLearning(); }
    bool hasNoiseProfile() { return engine.getDenoiser().hasLearnedProfile(); }
    void clearNoiseProfile() { engine.getDenoiser().clearLearnedProfile(); }

    /** Load / save a reference-match profile (.json, see tools/reference_match). */
    bool loadProfile (const juce::File& file, juce::String& error);
    bool saveProfile (const juce::File& file, const juce::String& name);
    juce::String getProfileName() const { return profileName; }

    bool isSidechainConnected() const { return sidechainActive.load(); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    static BusesProperties makeBuses();

    juce::AudioProcessorValueTreeState apvts;
    std::array<std::atomic<float>*, se::kNumParams> paramPtrs {};
    se::StudioEngine engine;
    juce::String profileName { "Default" };
    std::atomic<bool> sidechainActive { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StudioEngineProcessor)
};

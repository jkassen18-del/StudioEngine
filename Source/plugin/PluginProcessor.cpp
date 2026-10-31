#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

AudioProcessor::BusesProperties StudioEngineProcessor::makeBuses()
{
    return BusesProperties()
        .withInput ("Input", AudioChannelSet::stereo(), true)
        .withOutput ("Output", AudioChannelSet::stereo(), true)
        .withInput ("Instrumental", AudioChannelSet::stereo(), false);
}

AudioProcessorValueTreeState::ParameterLayout StudioEngineProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& p : se::paramTable())
    {
        if (p.isToggle)
        {
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { p.id, 1 }, p.name, p.defaultValue >= 0.5f));
            continue;
        }
        NormalisableRange<float> range (p.minValue, p.maxValue);
        range.interval = 0.0f;
        if (p.skewCentre > p.minValue && p.skewCentre < p.maxValue)
            range.setSkewForCentre (p.skewCentre);
        const String unit (p.unit);
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { p.id, 1 }, p.name, range, p.defaultValue,
            AudioParameterFloatAttributes()
                .withLabel (unit)
                .withStringFromValueFunction ([unit] (float v, int) {
                    if (unit == "Hz" && v >= 1000.0f)
                        return String (v / 1000.0f, 2) + " kHz";
                    if (unit == ":1")
                        return String (v, 1) + ":1";
                    const int decimals = std::abs (v) < 10.0f ? 1 : 0;
                    return String (v, decimals) + (unit.isEmpty() ? "" : " " + unit);
                })));
    }
    return layout;
}

StudioEngineProcessor::StudioEngineProcessor()
    : AudioProcessor (makeBuses()), apvts (*this, nullptr, "StudioEngine", createLayout())
{
    for (int i = 0; i < se::kNumParams; ++i)
        paramPtrs[(size_t) i] = apvts.getRawParameterValue (se::paramTable()[(size_t) i].id);
}

bool StudioEngineProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out || (out != AudioChannelSet::mono() && out != AudioChannelSet::stereo()))
        return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != AudioChannelSet::mono() && sc != AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void StudioEngineProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int numCh = std::max (1, getMainBusNumOutputChannels());
    engine.prepare (sampleRate, samplesPerBlock, numCh);
    setLatencySamples (engine.getLatencySamples());
}

void StudioEngineProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    for (int i = 0; i < se::kNumParams; ++i)
        engine.setParam (i, paramPtrs[(size_t) i]->load());

    auto main = getBusBuffer (buffer, true, 0);
    const int numCh = main.getNumChannels();
    const int n = main.getNumSamples();
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, n);

    const float* scPtrs[2] { nullptr, nullptr };
    int scCh = 0;
    if (getBusCount (true) > 1)
    {
        if (auto* bus = getBus (true, 1); bus != nullptr && bus->isEnabled())
        {
            auto sc = getBusBuffer (buffer, true, 1);
            scCh = std::min (2, sc.getNumChannels());
            for (int c = 0; c < scCh; ++c)
                scPtrs[c] = sc.getReadPointer (c);
        }
    }
    sidechainActive.store (scCh > 0);

    engine.process (main.getArrayOfWritePointers(), numCh, n, scCh > 0 ? scPtrs : nullptr, scCh);
}

//==============================================================================
bool StudioEngineProcessor::loadProfile (const File& file, String& error)
{
    se::Profile prof;
    std::string err;
    if (! se::parseProfile (file.loadFileAsString().toStdString(), prof, &err))
    {
        error = String (err);
        return false;
    }
    for (auto [idx, value] : prof.values)
        if (auto* param = apvts.getParameter (se::paramTable()[(size_t) idx].id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));

    profileName = prof.name.empty() ? file.getFileNameWithoutExtension() : String (prof.name);
    return true;
}

bool StudioEngineProcessor::saveProfile (const File& file, const String& name)
{
    std::array<float, se::kNumParams> v {};
    for (int i = 0; i < se::kNumParams; ++i)
        v[(size_t) i] = paramPtrs[(size_t) i]->load();
    profileName = name;
    return file.replaceWithText (se::profileToJson (name.toStdString(), v));
}

//==============================================================================
void StudioEngineProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("profileName", profileName, nullptr);
    const auto noise = engine.getDenoiser().getLearnedProfile();
    if (! noise.empty())
    {
        MemoryBlock mb (noise.data(), noise.size() * sizeof (float));
        state.setProperty ("noiseProfile", mb.toBase64Encoding(), nullptr);
    }
    else
        state.removeProperty ("noiseProfile", nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void StudioEngineProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;
    auto state = ValueTree::fromXml (*xml);
    profileName = state.getProperty ("profileName", "Default").toString();

    MemoryBlock mb;
    if (mb.fromBase64Encoding (state.getProperty ("noiseProfile").toString()) && mb.getSize() > 0)
    {
        std::vector<float> p (mb.getSize() / sizeof (float));
        std::memcpy (p.data(), mb.getData(), p.size() * sizeof (float));
        // Handed to the audio thread lock-free (or applied at the next prepare).
        engine.getDenoiser().setLearnedProfile (p);
    }
    state.removeProperty ("profileName", nullptr);
    state.removeProperty ("noiseProfile", nullptr);
    apvts.replaceState (state);
}

AudioProcessorEditor* StudioEngineProcessor::createEditor()
{
    return new StudioEngineEditor (*this);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StudioEngineProcessor();
}

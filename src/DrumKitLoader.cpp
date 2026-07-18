#include "DrumKitLoader.h"

std::unique_ptr<DrumKit> DrumKitLoader::loadKit(const juce::File& kitFile)
{
    if (!kitFile.existsAsFile())
        return nullptr;

    auto kit = std::make_unique<DrumKit>();
    kit->kitFile = kitFile;
    kit->basePath = kitFile.getParentDirectory();

    if (!parseKitXML(kitFile, *kit))
        return nullptr;

    // Find and load midimap
    auto midiMapFile = findMidiMapFile(kitFile);
    if (midiMapFile.existsAsFile())
        parseMidiMapXML(midiMapFile, *kit);

    return kit;
}

bool DrumKitLoader::parseKitXML(const juce::File& kitFile, DrumKit& kit)
{
    auto xml = juce::parseXML(kitFile);
    if (xml == nullptr || xml->getTagName() != "drumkit")
        return false;

    kit.name = xml->getStringAttribute("name");
    kit.description = xml->getStringAttribute("description");

    // Parse channels
    if (auto* channelsNode = xml->getChildByName("channels"))
    {
        for (auto* channelNode : channelsNode->getChildIterator())
        {
            if (channelNode->hasTagName("channel"))
                kit.channels.push_back(channelNode->getStringAttribute("name"));
        }
    }

    // Parse instruments
    if (auto* instrumentsNode = xml->getChildByName("instruments"))
    {
        for (auto* instrumentNode : instrumentsNode->getChildIterator())
        {
            if (!instrumentNode->hasTagName("instrument"))
                continue;

            auto instrument = std::make_unique<Instrument>();
            instrument->name = instrumentNode->getStringAttribute("name");
            instrument->group = instrumentNode->getStringAttribute("group");

            // Load instrument file
            auto instrumentFile = kit.basePath.getChildFile(
                instrumentNode->getStringAttribute("file"));

            if (instrumentFile.existsAsFile())
                kit.definitionFiles.push_back(instrumentFile);

            if (instrumentFile.existsAsFile() &&
                parseInstrumentXML(instrumentFile, *instrument))
            {
                // Parse channel map
                for (auto* channelMapNode : instrumentNode->getChildIterator())
                {
                    if (channelMapNode->hasTagName("channelmap"))
                    {
                        auto in = channelMapNode->getStringAttribute("in");
                        auto out = channelMapNode->getStringAttribute("out");
                        instrument->channelMap[in] = out;
                    }
                }

                kit.instruments.push_back(std::move(instrument));
            }
        }
    }

    return !kit.instruments.empty();
}

bool DrumKitLoader::parseInstrumentXML(const juce::File& instrumentFile, Instrument& instrument)
{
    auto xml = juce::parseXML(instrumentFile);
    if (xml == nullptr || xml->getTagName() != "instrument")
        return false;

    auto* samplesNode = xml->getChildByName("samples");
    if (samplesNode == nullptr)
        return false;

    auto basePath = instrumentFile.getParentDirectory();

    for (auto* sampleNode : samplesNode->getChildIterator())
    {
        if (!sampleNode->hasTagName("sample"))
            continue;

        DrumSample sample;
        sample.name = sampleNode->getStringAttribute("name");
        sample.power = sampleNode->getDoubleAttribute("power");

        for (auto* audioFileNode : sampleNode->getChildIterator())
        {
            if (!audioFileNode->hasTagName("audiofile"))
                continue;

            AudioSample audioSample;
            audioSample.channelName = audioFileNode->getStringAttribute("channel");
            audioSample.fileChannel = audioFileNode->getIntAttribute("filechannel");
            
            auto filePath = audioFileNode->getStringAttribute("file");
            audioSample.audioFile = basePath.getChildFile(filePath);

            sample.audioFiles.push_back(audioSample);
        }

        if (!sample.audioFiles.empty())
            instrument.samples.push_back(std::move(sample));
    }

    return !instrument.samples.empty();
}

bool DrumKitLoader::parseMidiMapXML(const juce::File& midiMapFile, DrumKit& kit)
{
    auto xml = juce::parseXML(midiMapFile);
    if (xml == nullptr || xml->getTagName() != "midimap")
        return false;

    for (auto* mapNode : xml->getChildIterator())
    {
        if (mapNode->hasTagName("map"))
        {
            int note = mapNode->getIntAttribute("note");
            juce::String instr = mapNode->getStringAttribute("instr");
            kit.midiMap[note] = instr;
        }
    }

    return true;
}

juce::File DrumKitLoader::findMidiMapFile(const juce::File& kitFile)
{
    auto kitName = kitFile.getFileNameWithoutExtension();
    auto basePath = kitFile.getParentDirectory();
    
    // Find midimap file with same base name
    auto midiMapName = kitName.replace("DRSKit", "Midimap");
    return basePath.getChildFile(midiMapName + ".xml");
}

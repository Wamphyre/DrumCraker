#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "DrumKitLoader.h"
#include "SampleEngine.h"
#include "VoiceManager.h"
#include "LockFreeRandom.h"

class DrumSamplerProcessor : public juce::AudioProcessor
{
public:
    DrumSamplerProcessor();
    ~DrumSamplerProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    using juce::AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "DrumCraker"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;
    
    // Force plugin to be destroyed/recreated when switching projects
    bool supportsDoublePrecisionProcessing() const override { return false; }
    
    // Support for multiple output buses
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    bool canAddBus(bool isInput) const override;
    bool canRemoveBus(bool isInput) const override;

    bool loadDrumKit(const juce::File& kitFile, bool async = true);
    bool loadMidiMap(const juce::File& midiMapFile);
    juce::String getCurrentKitName() const { return currentKitName; }
    juce::String getCurrentMidiMapName() const { return currentMidiMapName; }
    bool isKitFullyLoaded() const { return sampleEngine->isLoaded(); }
    bool getIsLoadingKit() const { return isLoadingKit.load(); }
    bool getIsLoadingMidiMap() const { return isLoadingMidiMap.load(); }
    float getLoadingProgress() const { return sampleEngine->getLoadingProgress(); }
    juce::String getLastLoadingError() const { return sampleEngine->getLastLoadingError(); }
    
    // Multi-channel routing
    void setupInstrumentRouting();
    const std::map<juce::String, int>& getInstrumentToBusMap() const { return instrumentToBusMap; }
    const std::vector<juce::String>& getInstrumentGroups() const { return instrumentGroups; }

    // Parameters
    juce::AudioParameterFloat* masterVolume;
    juce::AudioParameterFloat* velocityRandomness;
    juce::AudioParameterFloat* timingRandomness;
    juce::AudioParameterFloat* roundRobinVariation;

private:
    std::unique_ptr<DrumKitLoader> drumKitLoader;
    std::unique_ptr<SampleEngine> sampleEngine;
    std::unique_ptr<VoiceManager> voiceManager;
    
    juce::String currentKitName;
    juce::String currentMidiMapName;
    juce::String currentKitPath;      // Full path to kit XML
    juce::String currentMidiMapPath;  // Full path to midimap XML
    std::atomic<bool> isLoadingKit{false};
    std::atomic<bool> isLoadingMidiMap{false};

    // Gate for the audio thread: true only when samples AND routing are ready.
    // Written with release after setupInstrumentRouting(); read with acquire in processBlock.
    std::atomic<bool> kitReady{false};
    std::atomic<int> audioCallbacksInFlight{0};
    void waitForAudioCallbacks();

    // Unique ID to detect project changes
    juce::String stateId;

    // Multi-channel routing
    std::map<juce::String, int> instrumentToBusMap;  // instrument name -> bus index
    std::vector<juce::String> instrumentGroups;      // Ordered list of instrument groups

    double currentSampleRate = 44100.0;

    // Audio-thread RNG for humanization (see LockFreeRandom.h).
    LockFreeRandom humanizeRng;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumSamplerProcessor)
};

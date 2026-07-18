#include "PluginProcessor.h"
#include "PluginEditor.h"

DrumSamplerProcessor::DrumSamplerProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Kick", juce::AudioChannelSet::stereo(), true)
        .withOutput("Snare", juce::AudioChannelSet::stereo(), false)
        .withOutput("HiHat", juce::AudioChannelSet::stereo(), false)
        .withOutput("Toms", juce::AudioChannelSet::stereo(), false)
        .withOutput("Ride", juce::AudioChannelSet::stereo(), false)
        .withOutput("Crash", juce::AudioChannelSet::stereo(), false)
        .withOutput("China/Splash", juce::AudioChannelSet::stereo(), false)
        .withOutput("Ambience", juce::AudioChannelSet::stereo(), false)
        .withOutput("Aux 9", juce::AudioChannelSet::stereo(), false)
        .withOutput("Aux 10", juce::AudioChannelSet::stereo(), false)
        .withOutput("Aux 11", juce::AudioChannelSet::stereo(), false)
        .withOutput("Aux 12", juce::AudioChannelSet::stereo(), false)
        .withOutput("Aux 13", juce::AudioChannelSet::stereo(), false)
        .withOutput("Aux 14", juce::AudioChannelSet::stereo(), false)
        .withOutput("Aux 15", juce::AudioChannelSet::stereo(), false)
        .withOutput("Aux 16", juce::AudioChannelSet::stereo(), false))
{
    drumKitLoader = std::make_unique<DrumKitLoader>();
    sampleEngine = std::make_unique<SampleEngine>();
    voiceManager = std::make_unique<VoiceManager>();

    // Parameters
    addParameter(masterVolume = new juce::AudioParameterFloat(
        "masterVol", "Master Volume", -60.0f, 12.0f, 0.0f));
    
    // Velocity: 0-100% variation (default 8% for subtle humanization)
    addParameter(velocityRandomness = new juce::AudioParameterFloat(
        "velocityRnd", "Velocity Humanization", 0.0f, 1.0f, 0.08f));
    
    // Timing: 0-20ms variation (default 5ms for natural groove)
    addParameter(timingRandomness = new juce::AudioParameterFloat(
        "timingRnd", "Timing Humanization (ms)", 0.0f, 20.0f, 5.0f));
    
    // Round Robin: 0=pure velocity, 1=pure rotation (default 0.7 for natural mix)
    addParameter(roundRobinVariation = new juce::AudioParameterFloat(
        "roundRobin", "Round Robin Mix", 0.0f, 1.0f, 0.7f));
}

DrumSamplerProcessor::~DrumSamplerProcessor()
{
    kitReady.store(false, std::memory_order_release);
    waitForAudioCallbacks();
    sampleEngine->reset();
}

void DrumSamplerProcessor::waitForAudioCallbacks()
{
    while (audioCallbacksInFlight.load(std::memory_order_acquire) > 0)
        juce::Thread::sleep(1);
}

bool DrumSamplerProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // All output buses must be stereo or disabled
    for (int i = 0; i < layouts.outputBuses.size(); ++i)
    {
        auto& layout = layouts.outputBuses.getReference(i);
        if (layout != juce::AudioChannelSet::stereo() && 
            layout != juce::AudioChannelSet::disabled())
            return false;
    }
    
    return true;
}

bool DrumSamplerProcessor::canAddBus(bool) const
{
    // Don't allow adding/removing buses - they're fixed in constructor
    return false;
}

bool DrumSamplerProcessor::canRemoveBus(bool) const
{
    // Don't allow adding/removing buses - they're fixed in constructor
    return false;
}

void DrumSamplerProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const bool wasReady = kitReady.exchange(false, std::memory_order_acq_rel);
    if (wasReady)
        waitForAudioCallbacks();

    currentSampleRate = sampleRate;
    sampleEngine->prepare(sampleRate, samplesPerBlock);
    voiceManager->prepare(sampleRate, samplesPerBlock);

    // Seed the audio-thread RNG per-instance so two instances of the plugin
    // don't humanize in lockstep.
    const uint64_t seedMix = static_cast<uint64_t>(juce::Time::getHighResolutionTicks())
                           ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
    humanizeRng.seed(static_cast<uint32_t>(seedMix ^ (seedMix >> 32)));

    if (wasReady && sampleEngine->isLoaded())
        kitReady.store(true, std::memory_order_release);
}

void DrumSamplerProcessor::releaseResources()
{
    // DON'T reset sampleEngine here
    // Keep samples in memory for fast exports
    // Only stop active voices
    voiceManager->reset();
}

void DrumSamplerProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // Two-step gate. The second check closes the race where the control thread
    // disables the kit between our first check and incrementing the in-flight
    // counter. Kit replacement waits for this counter before freeing buffers.
    if (!kitReady.load(std::memory_order_acquire))
        return;

    audioCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    struct AudioCallbackGuard
    {
        std::atomic<int>& counter;
        ~AudioCallbackGuard() { counter.fetch_sub(1, std::memory_order_release); }
    } audioCallbackGuard{ audioCallbacksInFlight };

    if (!kitReady.load(std::memory_order_acquire))
        return;

    // Verify valid buffer size
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
        return;

    // Pre-cache parameter values (avoid repeated atomic reads)
    const float velocityHumanization = velocityRandomness->get();
    const float timingHumanization = timingRandomness->get();
    const float rrVariation = roundRobinVariation->get();
    const bool hasVelocityHumanization = velocityHumanization > 0.001f;
    const bool hasTimingHumanization = timingHumanization > 0.001f;

    for (const auto metadata : midiMessages)
    {
        auto message = metadata.getMessage();

        if (message.isNoteOn()) [[likely]]
        {
            float velocity = message.getVelocity() / 127.0f;
            int sampleOffset = metadata.samplePosition;

            // VELOCITY HUMANIZATION (optimized Box-Muller)
            if (hasVelocityHumanization)
            {
                float u1 = humanizeRng.nextFloat() * 0.9999f + 0.0001f;
                float u2 = humanizeRng.nextFloat();
                float gaussian = std::sqrt(-2.0f * std::log(u1)) * std::cos(juce::MathConstants<float>::twoPi * u2);
                velocity = juce::jlimit(0.05f, 1.0f, velocity + gaussian * velocityHumanization);
            }

            // TIMING HUMANIZATION (optimized)
            if (hasTimingHumanization)
            {
                float u1 = humanizeRng.nextFloat() * 0.9999f + 0.0001f;
                float u2 = humanizeRng.nextFloat();
                float gaussian = std::sqrt(-2.0f * std::log(u1)) * std::sin(juce::MathConstants<float>::twoPi * u2);
                float velocityBias = (velocity - 0.5f) * 0.4f;
                int sampleDelay = static_cast<int>(((gaussian * 0.5f + velocityBias) * timingHumanization / 1000.0f) * currentSampleRate);
                // CRITICAL FIX: Do NOT clamp to buffer size!
                // Let the offset be larger than buffer size - VoiceManager handles this
                // by counting down startOffset until it's time to play.
                // Clamping here causes "bunching" at the end of the buffer (machine gun effect).
                sampleOffset = sampleOffset + sampleDelay;
                
                // CRITICAL FIX 2: Prevent negative offsets (heap corruption)
                // If humanization "rushes" too much, clamp to 0 (play immediately)
                if (sampleOffset < 0)
                    sampleOffset = 0;
            }
            
            voiceManager->noteOn(message.getNoteNumber(), velocity, sampleEngine.get(), sampleOffset, rrVariation);
        }
    }
    
    // Count enabled buses
    int numEnabledBuses = 0;
    for (int i = 0; i < getTotalNumOutputChannels() / 2; ++i)
    {
        auto* bus = getBus(false, i);
        if (bus && bus->isEnabled())
            numEnabledBuses++;
    }
    
    float gainLinear = juce::Decibels::decibelsToGain(masterVolume->get());
    
    // MULTI-CHANNEL MODE: Render all voices once, routing to appropriate buses
    if (numEnabledBuses > 1 && !instrumentToBusMap.empty())
    {
        voiceManager->renderNextBlockMultiBus(buffer, 0, buffer.getNumSamples(),
                                             gainLinear, this);
    }
    else
    {
        // STEREO MODE: Render all voices to main output
        auto* mainBus = getBus(false, 0);
        if (mainBus && mainBus->isEnabled())
        {
            auto mainBuffer = getBusBuffer(buffer, false, 0);
            voiceManager->renderNextBlock(mainBuffer, 0, buffer.getNumSamples());
            mainBuffer.applyGain(gainLinear);
        }
    }
}



void DrumSamplerProcessor::setupInstrumentRouting()
{
    const DrumKit* kit = sampleEngine->getCurrentKit();
    if (!kit)
        return;
    
    instrumentToBusMap.clear();
    instrumentGroups.clear();

    // IMPLEMENTING FIXED ROUTING STRATEGY
    // Bus 0: Kick
    // Bus 1: Snare
    // Bus 2: HiHat
    // Bus 3: Toms (Stereo Mix)
    // Bus 4: Ride
    // Bus 5: Crash
    // Bus 6: China/Splash/Bell
    // Bus 7: Ambience/Room (if present as separate instrument)
    // Bus 8-15: Aux

    for (const auto& instrument : kit->instruments)
    {
        juce::String name = instrument->name;
        int targetBus = 15; // Default to last Aux

        if (name.containsIgnoreCase("Kick") || name.containsIgnoreCase("KDrum"))
        {
            targetBus = 0;
            if (instrumentGroups.size() == 0) instrumentGroups.push_back("Kick");
        }
        else if (name.containsIgnoreCase("Snare"))
        {
            targetBus = 1;
        }
        else if (name.containsIgnoreCase("Hihat") || name.containsIgnoreCase("HH"))
        {
            targetBus = 2;
        }
        else if (name.containsIgnoreCase("Tom") || name.containsIgnoreCase("Floor"))
        {
            targetBus = 3; // All Toms to Bus 3
        }
        else if (name.containsIgnoreCase("Ride"))
        {
            targetBus = 4;
        }
        else if (name.containsIgnoreCase("Crash"))
        {
            targetBus = 5; // All Crashes to Bus 5
        }
        else if (name.containsIgnoreCase("China") || name.containsIgnoreCase("Splash") || 
                 name.containsIgnoreCase("Bell") || name.containsIgnoreCase("Zilbel") || 
                 name.containsIgnoreCase("Cym"))
        {
            targetBus = 6;
        }
        else if (name.containsIgnoreCase("Room") || name.containsIgnoreCase("Amb"))
        {
            targetBus = 7;
        }
        else
        {
            targetBus = 8;
        }

        instrumentToBusMap[name] = targetBus;
    }

    instrumentGroups = { "Kick", "Snare", "HiHat", "Toms", "Ride", "Crash", "SFX", "Ambience", "Aux"};

    // Pass mapping to voice manager for pre-caching bus indices
    voiceManager->setInstrumentToBusMap(instrumentToBusMap);

    // Release-store the gate: every write above is now visible to any thread
    // that acquire-loads kitReady and sees true.
    kitReady.store(true, std::memory_order_release);
}

bool DrumSamplerProcessor::loadDrumKit(const juce::File& kitFile, bool async)
{
    isLoadingKit = true;

    // Close the audio-thread gate before touching any state it reads.
    kitReady.store(false, std::memory_order_release);
    waitForAudioCallbacks();

    // Stop all active voices before unloading previous kit
    voiceManager->reset();

    // Clear old instrument routing so stale mappings can't be used during load
    instrumentToBusMap.clear();
    instrumentGroups.clear();
    voiceManager->setInstrumentToBusMap(instrumentToBusMap);
    
    // CRITICAL: Ensure sampleEngine has correct project sample rate
    // before loading kit (for proper resampling)
    if (currentSampleRate > 0.0)
    {
        sampleEngine->prepare(currentSampleRate, 0);
    }
    
    auto kit = drumKitLoader->loadKit(kitFile);
    if (kit == nullptr)
    {
        isLoadingKit = false;
        return false;
    }

    currentKitName = kit->name;
    currentKitPath = kitFile.getFullPathName();
    
    // Configure callback
    sampleEngine->setLoadingCallback([this](bool success)
    {
        if (success)
        {
            // Setup instrument routing after kit is loaded
            setupInstrumentRouting();
        }
        isLoadingKit = false;
    });
    
    // Load new kit (this frees the previous one automatically)
    sampleEngine->loadKit(std::move(kit), async);
    
    return true;
}

bool DrumSamplerProcessor::loadMidiMap(const juce::File& midiMapFile)
{
    isLoadingMidiMap = true;

    const bool wasReady = kitReady.exchange(false, std::memory_order_acq_rel);
    if (wasReady)
        waitForAudioCallbacks();

    bool success = sampleEngine->loadMidiMap(midiMapFile);
    if (success)
    {
        currentMidiMapName = midiMapFile.getFileNameWithoutExtension();
        currentMidiMapPath = midiMapFile.getFullPathName();  // Save full path
    }

    if (wasReady && sampleEngine->isLoaded())
        kitReady.store(true, std::memory_order_release);

    isLoadingMidiMap = false;
    return success;
}

juce::AudioProcessorEditor* DrumSamplerProcessor::createEditor()
{
    return new DrumSamplerEditor(*this);
}

void DrumSamplerProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Create XML to save state
    auto xml = std::make_unique<juce::XmlElement>("DrumCrakerState");
    
    // Save parameters
    xml->setAttribute("masterVolume", static_cast<double>(masterVolume->get()));
    xml->setAttribute("velocityRandomness", static_cast<double>(velocityRandomness->get()));
    xml->setAttribute("timingRandomness", static_cast<double>(timingRandomness->get()));
    xml->setAttribute("roundRobinVariation", static_cast<double>(roundRobinVariation->get()));
    
    // Save full paths to XML files
    xml->setAttribute("kitPath", currentKitPath);
    xml->setAttribute("midiMapPath", currentMidiMapPath);
    
    // Save names (for UI reference)
    xml->setAttribute("kitName", currentKitName);
    xml->setAttribute("midiMapName", currentMidiMapName);
    
    // Generate unique state ID (timestamp + random)
    stateId = juce::String(juce::Time::currentTimeMillis()) + "_" + 
              juce::String(juce::Random::getSystemRandom().nextInt());
    xml->setAttribute("stateId", stateId);
    
    // Convert XML to binary
    copyXmlToBinary(*xml, destData);
}

void DrumSamplerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    
    if (xml != nullptr && xml->hasTagName("DrumCrakerState"))
    {
        // Restore parameters immediately
        *masterVolume = static_cast<float>(xml->getDoubleAttribute("masterVolume", 0.0));
        *velocityRandomness = static_cast<float>(xml->getDoubleAttribute("velocityRandomness", 0.0));
        *timingRandomness = static_cast<float>(xml->getDoubleAttribute("timingRandomness", 0.0));
        *roundRobinVariation = static_cast<float>(xml->getDoubleAttribute("roundRobinVariation", 0.5));
        
        // Restore file paths
        juce::String savedKitPath = xml->getStringAttribute("kitPath");
        juce::String savedMidiMapPath = xml->getStringAttribute("midiMapPath");
        juce::String savedKitName = xml->getStringAttribute("kitName");
        juce::String savedMidiMapName = xml->getStringAttribute("midiMapName");
        juce::String savedStateId = xml->getStringAttribute("stateId");
        
        // Check if this is a different state (different project/session)
        bool isDifferentState = (savedStateId != stateId) && savedStateId.isNotEmpty();
        
        // Always reload when restoring state OR when state ID is different
        // This ensures correct kit is loaded when switching projects
        if (savedKitPath.isNotEmpty() && (isDifferentState || stateId.isEmpty()))
        {
            juce::File kitFile(savedKitPath);
            
            if (kitFile.existsAsFile())
            {
                // Update variables including state ID
                currentKitPath = savedKitPath;
                currentKitName = savedKitName;
                currentMidiMapPath = savedMidiMapPath;
                currentMidiMapName = savedMidiMapName;
                stateId = savedStateId;
                
                // Close the audio-thread gate and clear routing before reloading.
                kitReady.store(false, std::memory_order_release);
                waitForAudioCallbacks();
                instrumentToBusMap.clear();
                instrumentGroups.clear();
                voiceManager->setInstrumentToBusMap(instrumentToBusMap);

                // Load ASYNCHRONOUSLY to not block Reaper
                isLoadingKit = true;
                
                // Capture variables for lambda
                juce::String midiMapPath = currentMidiMapPath;
                
                // Configure callback before loading
                sampleEngine->setLoadingCallback([this, midiMapPath](bool success)
                {
                    if (success)
                    {
                        if (midiMapPath.isNotEmpty())
                        {
                            juce::File midiMapFile(midiMapPath);
                            if (midiMapFile.existsAsFile())
                            {
                                isLoadingMidiMap = true;
                                sampleEngine->loadMidiMap(midiMapFile);
                                isLoadingMidiMap = false;
                            }
                        }

                        // Publish routing and open the audio gate only after
                        // the restored MIDI map is also in place.
                        setupInstrumentRouting();
                    }
                    
                    isLoadingKit = false;
                });
                
                voiceManager->reset();
                
                // CRITICAL: Ensure sampleEngine has correct project sample rate
                // before loading kit (for proper resampling)
                if (currentSampleRate > 0.0)
                {
                    sampleEngine->prepare(currentSampleRate, 0);
                }
                
                auto kit = drumKitLoader->loadKit(kitFile);
                if (kit != nullptr)
                {
                    sampleEngine->loadKit(std::move(kit), true); // ASYNC
                }
                else
                {
                    isLoadingKit = false;
                }
            }
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumSamplerProcessor();
}

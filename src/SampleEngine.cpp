#include "SampleEngine.h"
#include <set>
#include <algorithm>
#include <utility>
#include <cstring>
#include <limits>
#include <unordered_set>

#if JUCE_LINUX
    #include <malloc.h>
#endif

SampleEngine::SampleEngine() {}

SampleEngine::~SampleEngine()
{
    kitLoaded.store(false, std::memory_order_release);
    shouldStopLoading.store(true, std::memory_order_release);

    // The callback captures the processor. Remove it before joining so a load
    // finishing during destruction cannot call into an object being destroyed.
    {
        juce::ScopedLock lock(cacheLock);
        loadingCallback = nullptr;
    }

    // Never free data while a loader/cache writer may still be using it.
    stopAndJoinLoadingThread();
    waitForCacheWrite();
    clearSampleCaches();
    instrumentCache.clear();
    currentKit.reset();
}

void SampleEngine::prepare(double sr, int)
{
    if (sr <= 0.0)
        return;

    // Seed the round-robin RNG per-instance so duplicated kits don't pick
    // identical sequences. Cheap and idempotent.
    const uint64_t seedMix = static_cast<uint64_t>(juce::Time::getHighResolutionTicks())
                           ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
    rrRng.seed(static_cast<uint32_t>(seedMix ^ (seedMix >> 32)));
        
    const double previousSampleRate = sampleRate.exchange(sr, std::memory_order_acq_rel);

    // If sample rate changes and we have samples loaded, we MUST resample.
    // Otherwise render speed will be wrong (pitch shift + sync loss)
    if (previousSampleRate > 0.0 && std::abs(sr - previousSampleRate) > 0.1)
    {
        // Cache writing reads immutable buffer data without holding cacheLock
        // for the full write. Finish it before any buffer can be reallocated.
        waitForCacheWrite();

        juce::ScopedLock lock(cacheLock);

        // Iterate all buffers and resample in place from their CURRENT rate
        // (bufferSampleRates tracks the rate each buffer is actually stored at)
        // to the NEW target rate. Always resamples from the correct rate, no
        // cumulative quality loss from stale "original" rate assumptions.
        for (auto& entry : audioBufferCache)
        {
            if (entry.second)
            {
                auto rateIt = bufferSampleRates.find(entry.first);
                const double currentRate = rateIt != bufferSampleRates.end()
                                             ? rateIt->second : previousSampleRate;
                resampleBuffer(*entry.second, currentRate, sr);
                bufferSampleRates[entry.first] = sr;
            }
        }
    }
}

void SampleEngine::reset()
{
    shouldStopLoading.store(true, std::memory_order_release);
    kitLoaded.store(false, std::memory_order_release);

    {
        juce::ScopedLock lock(cacheLock);
        loadingCallback = nullptr;
    }

    stopAndJoinLoadingThread();
    waitForCacheWrite();
    clearSampleCaches();

    // Reset lastSampleIndex array (atomic values)
    for (auto& idx : lastSampleIndex)
        idx.store(0, std::memory_order_relaxed);

    instrumentCache.clear();
    std::unordered_map<juce::String, Instrument*>().swap(instrumentCache);

    currentKit.reset();

    // Force memory return to OS (Linux-specific)
    // On macOS/Windows, this does nothing but is harmless
    #if JUCE_LINUX
        malloc_trim(0);
    #endif
}

void SampleEngine::stopAndJoinLoadingThread()
{
    shouldStopLoading.store(true, std::memory_order_release);

    if (loadingThread.joinable())
    {
        // All callers are owner/control threads. Joining from the loader itself
        // would be a programming error and would deadlock.
        jassert(loadingThread.get_id() != std::this_thread::get_id());
        if (loadingThread.get_id() != std::this_thread::get_id())
            loadingThread.join();
    }

    isLoadingAsync.store(false, std::memory_order_release);
}

void SampleEngine::clearSampleCaches()
{
    juce::ScopedLock lock(cacheLock);

    for (auto& entry : lockFreeBufferCache)
    {
        if (entry.second)
        {
            entry.second->ready.store(false, std::memory_order_release);
            entry.second->bufferPtr.store(nullptr, std::memory_order_release);
        }
    }
    lockFreeBufferCache.clear();
    std::unordered_map<juce::String, std::unique_ptr<LockFreeBufferEntry>>().swap(lockFreeBufferCache);

    audioBufferCache.clear();
    std::unordered_map<juce::String, std::unique_ptr<juce::AudioBuffer<float>>>().swap(audioBufferCache);

    bufferSampleRates.clear();
    std::unordered_map<juce::String, double>().swap(bufferSampleRates);
}

void SampleEngine::setLoadingError(const juce::String& message)
{
    juce::ScopedLock lock(loadingStatusLock);
    lastLoadingError = message;
}

juce::String SampleEngine::getLastLoadingError() const
{
    juce::ScopedLock lock(loadingStatusLock);
    return lastLoadingError;
}

void SampleEngine::setLoadingCallback(std::function<void(bool)> callback)
{
    juce::ScopedLock lock(cacheLock);
    loadingCallback = std::move(callback);
}

void SampleEngine::loadKit(std::unique_ptr<DrumKit> kit, bool async)
{
    if (!kit)
        return;

    // The processor installs the callback before entering this method. Keep it
    // away from the previous loader while that loader is being cancelled.
    std::function<void(bool)> pendingCallback;
    {
        juce::ScopedLock lock(cacheLock);
        pendingCallback = std::move(loadingCallback);
        loadingCallback = nullptr;
    }

    // A display name is not a kit identity. Always finish the previous session
    // and let the content-addressed disk cache handle fast reloads.
    stopAndJoinLoadingThread();
    kitLoaded.store(false, std::memory_order_release);

    // Stop and finish the previous cache writer before freeing its buffers.
    shouldStopLoading.store(true, std::memory_order_release);
    waitForCacheWrite();
    clearSampleCaches();

    for (auto& idx : lastSampleIndex)
        idx.store(0, std::memory_order_relaxed);

    instrumentCache.clear();
    std::unordered_map<juce::String, Instrument*>().swap(instrumentCache);

    setLoadingError({});
    shouldStopLoading.store(false, std::memory_order_release);

    // Force memory return to OS (Linux-specific)
    // On macOS/Windows, this does nothing but is harmless
    #if JUCE_LINUX
        malloc_trim(0);
    #endif
    
    // Set new kit
    currentKit = std::move(kit);

    {
        juce::ScopedLock lock(cacheLock);
        loadingCallback = std::move(pendingCallback);
    }
    
    // Load samples
    if (async)
        loadSamplesAsync();
    else
        loadSamplesSync();
}

bool SampleEngine::loadMidiMap(const juce::File& midiMapFile)
{
    if (!currentKit)
        return false;

    auto xml = juce::parseXML(midiMapFile);
    if (xml == nullptr || xml->getTagName() != "midimap")
        return false;

    // Clear current midimap
    currentKit->midiMap.clear();

    // Parse new midimap
    for (auto* mapNode : xml->getChildIterator())
    {
        if (mapNode->hasTagName("map"))
        {
            int note = mapNode->getIntAttribute("note");
            juce::String instr = mapNode->getStringAttribute("instr");
            currentKit->midiMap[note] = instr;
        }
    }

    return !currentKit->midiMap.empty();
}

// ============================================================================
// FASE 1 + 2: Async sample loading with grouped decode and disk cache
// ============================================================================

void SampleEngine::loadSamplesAsync()
{
    if (!currentKit)
        return;

    jassert(!loadingThread.joinable());
    isLoadingAsync.store(true, std::memory_order_release);
    shouldStopLoading.store(false, std::memory_order_release);
    loadedSampleCount.store(0, std::memory_order_relaxed);
    totalSampleCount.store(0, std::memory_order_relaxed);

    try
    {
        loadingThread = std::thread([this]()
        {
            struct ScopeGuard
            {
                std::atomic<bool>& flag;
                ~ScopeGuard() { flag.store(false, std::memory_order_release); }
            } guard{ isLoadingAsync };

            if (!currentKit || shouldStopLoading.load(std::memory_order_acquire))
                return;

            const double targetSR = sampleRate.load(std::memory_order_acquire);
            const uint64_t kitSig = computeKitSignature(*currentKit, targetSR);
            const juce::File cacheFile = getCacheFileForKit(
                currentKit->kitFile.getFullPathName(), targetSR);

            if (loadFromDiskCache(cacheFile, targetSR, kitSig))
            {
                if (std::abs(sampleRate.load(std::memory_order_acquire) - targetSR) <= 0.1
                    && !shouldStopLoading.load(std::memory_order_acquire))
                {
                    cacheFile.setLastModificationTime(juce::Time::getCurrentTime());
                    kitLoaded.store(true, std::memory_order_release);

                    std::function<void(bool)> callback;
                    {
                        juce::ScopedLock lock(cacheLock);
                        callback = std::move(loadingCallback);
                        loadingCallback = nullptr;
                    }
                    if (callback)
                        callback(true);
                    return;
                }

                clearSampleCaches();
                if (!shouldStopLoading.load(std::memory_order_acquire))
                {
                    setLoadingError("Project sample rate changed while the kit was loading; reload the kit");
                    std::function<void(bool)> callback;
                    {
                        juce::ScopedLock lock(cacheLock);
                        callback = std::move(loadingCallback);
                        loadingCallback = nullptr;
                    }
                    if (callback)
                        callback(false);
                }
                return;
            }

            if (shouldStopLoading.load(std::memory_order_acquire))
                return;

            // A failed cache read may have published valid entries before a
            // truncated/corrupt entry. Start source decoding from a clean set.
            clearSampleCaches();

            struct FileJob
            {
                juce::File file;
                std::vector<std::pair<int, juce::String>> channels;
                int64_t workUnits = 1;
            };
            std::unordered_map<juce::String, FileJob> jobsByPath;

            for (const auto& instrument : currentKit->instruments)
            {
                if (shouldStopLoading.load(std::memory_order_acquire))
                    return;

                for (const auto& sample : instrument->samples)
                {
                    for (const auto& audioSample : sample.audioFiles)
                    {
                        const juce::String path = audioSample.audioFile.getFullPathName();
                        auto& job = jobsByPath[path];
                        job.file = audioSample.audioFile;
                        job.workUnits = juce::jmax<int64_t>(1, audioSample.audioFile.getSize());

                        const bool alreadyRequested = std::any_of(
                            job.channels.begin(), job.channels.end(),
                            [&](const auto& channel) { return channel.first == audioSample.fileChannel; });
                        if (!alreadyRequested)
                        {
                            job.channels.emplace_back(
                                audioSample.fileChannel,
                                path + "_ch" + juce::String(audioSample.fileChannel));
                        }
                    }
                }
            }

            int64_t totalWork = 0;
            for (const auto& entry : jobsByPath)
                totalWork += entry.second.workUnits;
            totalSampleCount.store(juce::jmax<int64_t>(1, totalWork), std::memory_order_relaxed);

            juce::AudioFormatManager formatManager;
            formatManager.registerBasicFormats();

            // Decoding is I/O and memory-bandwidth bound. A small fixed ceiling
            // avoids multiplying large per-file allocations on high-core CPUs.
            const int numCpus = juce::jmax(1, juce::SystemStats::getNumCpus());
            const int numThreads = juce::jlimit(1, 4, juce::jmax(1, numCpus / 2));
            juce::ThreadPool pool(numThreads);
            std::atomic<bool> hasError{false};

            for (const auto& entry : jobsByPath)
            {
                if (shouldStopLoading.load(std::memory_order_acquire))
                    break;

                const FileJob job = entry.second;
                pool.addJob([this, job, targetSR, &formatManager, &hasError]()
                {
                    if (shouldStopLoading.load(std::memory_order_acquire) || hasError.load())
                        return;

                    try
                    {
                        if (!loadUniqueFile(job.file, job.channels, formatManager,
                                            targetSR, job.workUnits))
                        {
                            if (!shouldStopLoading.load(std::memory_order_acquire))
                                setLoadingError("Could not decode " + job.file.getFullPathName());
                            hasError.store(true, std::memory_order_release);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        setLoadingError("Error loading " + job.file.getFullPathName()
                                        + ": " + juce::String(e.what()));
                        hasError.store(true, std::memory_order_release);
                    }
                    catch (...)
                    {
                        setLoadingError("Unknown error loading " + job.file.getFullPathName());
                        hasError.store(true, std::memory_order_release);
                    }
                });
            }

            while (pool.getNumJobs() > 0)
            {
                if (shouldStopLoading.load(std::memory_order_acquire))
                {
                    // loadUniqueFile checks cancellation between small chunks,
                    // so an unbounded join is both prompt and lifetime-safe.
                    pool.removeAllJobs(true, -1);
                    break;
                }
                juce::Thread::sleep(5);
            }
            pool.removeAllJobs(true, -1);

            const bool sampleRateChanged =
                std::abs(sampleRate.load(std::memory_order_acquire) - targetSR) > 0.1;
            if (sampleRateChanged)
            {
                setLoadingError("Project sample rate changed while the kit was loading; reload the kit");
                hasError.store(true, std::memory_order_release);
            }

            const bool cancelled = shouldStopLoading.load(std::memory_order_acquire);
            const bool success = !cancelled && !hasError.load(std::memory_order_acquire);
            if (success)
            {
                kitLoaded.store(true, std::memory_order_release);
                writeDiskCacheAsync(cacheFile, targetSR, kitSig);
            }
            else if (!cancelled)
            {
                kitLoaded.store(false, std::memory_order_release);
                clearSampleCaches();
            }

            std::function<void(bool)> callback;
            {
                juce::ScopedLock lock(cacheLock);
                callback = std::move(loadingCallback);
                loadingCallback = nullptr;
            }
            if (callback && !cancelled)
                callback(success);
        });
    }
    catch (const std::exception& e)
    {
        isLoadingAsync.store(false, std::memory_order_release);
        setLoadingError("Could not start loading thread: " + juce::String(e.what()));

        std::function<void(bool)> callback;
        {
            juce::ScopedLock lock(cacheLock);
            callback = std::move(loadingCallback);
            loadingCallback = nullptr;
        }
        if (callback)
            callback(false);
    }
}

void SampleEngine::loadSamplesSync()
{
    if (!currentKit)
        return;

    const double targetSR = sampleRate.load(std::memory_order_acquire);
    loadedSampleCount.store(0, std::memory_order_relaxed);
    totalSampleCount.store(0, std::memory_order_relaxed);

    // ---- FASE 2: Try disk cache first ----
    const uint64_t kitSig = computeKitSignature(*currentKit, targetSR);
    juce::File cacheFile = getCacheFileForKit(currentKit->kitFile.getFullPathName(), targetSR);

    if (loadFromDiskCache(cacheFile, targetSR, kitSig))
    {
        kitLoaded.store(true, std::memory_order_release);
        cacheFile.setLastModificationTime(juce::Time::getCurrentTime());
        std::function<void(bool)> callback;
        {
            juce::ScopedLock lock(cacheLock);
            callback = std::move(loadingCallback);
            loadingCallback = nullptr;
        }
        if (callback)
            callback(true);
        return;
    }

    clearSampleCaches();

    // ---- FASE 1: Cache miss — decode from source (synchronous) ----
    using ChannelReq = std::pair<int, juce::String>;
    std::unordered_map<juce::String, std::pair<juce::File, std::vector<ChannelReq>>> jobsByPath;

    for (const auto& instrument : currentKit->instruments)
    {
        for (const auto& sample : instrument->samples)
        {
            for (const auto& audioSample : sample.audioFiles)
            {
                const juce::String path = audioSample.audioFile.getFullPathName();
                auto& job = jobsByPath[path];
                job.first = audioSample.audioFile;
                const bool alreadyRequested = std::any_of(
                    job.second.begin(), job.second.end(),
                    [&](const auto& channel) { return channel.first == audioSample.fileChannel; });
                if (!alreadyRequested)
                {
                    job.second.emplace_back(audioSample.fileChannel,
                                            path + "_ch" + juce::String(audioSample.fileChannel));
                }
            }
        }
    }

    int64_t totalWork = 0;
    for (const auto& entry : jobsByPath)
        totalWork += juce::jmax<int64_t>(1, entry.second.first.getSize());
    totalSampleCount.store(juce::jmax<int64_t>(1, totalWork), std::memory_order_relaxed);

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    bool success = true;
    for (const auto& kv : jobsByPath)
    {
        const int64_t workUnits = juce::jmax<int64_t>(1, kv.second.first.getSize());
        if (!loadUniqueFile(kv.second.first, kv.second.second, formatManager,
                            targetSR, workUnits))
        {
            setLoadingError("Could not decode " + kv.second.first.getFullPathName());
            success = false;
            break;
        }
    }

    kitLoaded.store(success, std::memory_order_release);

    if (success)
        writeDiskCacheAsync(cacheFile, targetSR, kitSig);
    else
        clearSampleCaches();

    std::function<void(bool)> callback;
    {
        juce::ScopedLock lock(cacheLock);
        callback = std::move(loadingCallback);
        loadingCallback = nullptr;
    }
    if (callback)
        callback(success);
}

// ============================================================================
// FASE 1: Decode one unique file and extract all requested channels
// ============================================================================

bool SampleEngine::loadUniqueFile(const juce::File& audioFile,
                                  const std::vector<std::pair<int, juce::String>>& channels,
                                  juce::AudioFormatManager& formatManager,
                                  double targetSampleRate,
                                  int64_t progressUnits)
{
    if (!audioFile.existsAsFile())
        return false;

    // Skip channels already cached (e.g., from a partial previous load)
    std::vector<std::pair<int, juce::String>> toLoad;
    std::unordered_set<int> requestedChannels;
    {
        juce::ScopedLock lock(cacheLock);
        for (const auto& ch : channels)
        {
            if (audioBufferCache.find(ch.second) == audioBufferCache.end()
                && requestedChannels.insert(ch.first).second)
                toLoad.push_back(ch);
        }
    }

    if (toLoad.empty())
    {
        loadedSampleCount.fetch_add(progressUnits, std::memory_order_relaxed);
        return true;
    }

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(audioFile));
    if (reader == nullptr)
        return false;

    const double originalSampleRate = reader->sampleRate;
    if (reader->lengthInSamples > std::numeric_limits<int>::max())
        return false;

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    if (reader->numChannels > static_cast<unsigned int>(std::numeric_limits<int>::max()))
        return false;
    const int numChannels = static_cast<int>(reader->numChannels);

    if (numSamples <= 0 || numChannels <= 0)
        return false;

    struct PendingChannel
    {
        int fileChannel = 0;
        juce::String cacheKey;
        std::unique_ptr<juce::AudioBuffer<float>> buffer;
    };
    std::vector<PendingChannel> pendingChannels;
    pendingChannels.reserve(toLoad.size());

    for (const auto& ch : toLoad)
    {
        if (ch.first < 1 || ch.first > numChannels)
            return false;
        pendingChannels.push_back(
            { ch.first, ch.second,
              std::make_unique<juce::AudioBuffer<float>>(1, numSamples) });
    }

    // Decode in bounded chunks instead of allocating a second full-size,
    // multichannel copy of every source file. The destination mono buffers are
    // the only full-length allocations made for this job.
    constexpr int decodeChunkSamples = 32768;
    juce::AudioBuffer<float> decodeBuffer(numChannels,
        juce::jmin(decodeChunkSamples, numSamples));
    std::vector<float*> channelPointers(static_cast<size_t>(numChannels));

    int sourcePosition = 0;
    int64_t reportedProgress = 0;
    const int64_t decodeProgressUnits = (progressUnits * 4) / 5;
    auto reportProgress = [&](int64_t desiredProgress)
    {
        desiredProgress = juce::jlimit<int64_t>(reportedProgress, progressUnits,
                                                desiredProgress);
        loadedSampleCount.fetch_add(desiredProgress - reportedProgress,
                                   std::memory_order_relaxed);
        reportedProgress = desiredProgress;
    };

    while (sourcePosition < numSamples)
    {
        if (shouldStopLoading.load(std::memory_order_acquire))
            return false;

        const int samplesThisTime = juce::jmin(decodeChunkSamples, numSamples - sourcePosition);
        decodeBuffer.setSize(numChannels, samplesThisTime, false, false, true);
        for (int channel = 0; channel < numChannels; ++channel)
            channelPointers[static_cast<size_t>(channel)] = decodeBuffer.getWritePointer(channel);

        if (!reader->read(channelPointers.data(), numChannels,
                          static_cast<int64_t>(sourcePosition), samplesThisTime))
            return false;

        for (auto& pending : pendingChannels)
        {
            pending.buffer->copyFrom(0, sourcePosition, decodeBuffer,
                                     pending.fileChannel - 1, 0, samplesThisTime);
        }
        sourcePosition += samplesThisTime;
        reportProgress((decodeProgressUnits * sourcePosition) / numSamples);
    }

    reader.reset();

    for (size_t pendingIndex = 0; pendingIndex < pendingChannels.size(); ++pendingIndex)
    {
        auto& pending = pendingChannels[pendingIndex];
        if (shouldStopLoading.load(std::memory_order_acquire))
            return false;

        // Determine the rate the buffer will be stored at after resampling
        double bufferRate = originalSampleRate;
        if (targetSampleRate > 0.0
            && std::abs(originalSampleRate - targetSampleRate) > 0.1)
        {
            resampleBuffer(*pending.buffer, originalSampleRate, targetSampleRate);
            bufferRate = targetSampleRate;
        }

        // Brief lock just to publish into the caches
        {
            juce::ScopedLock lock(cacheLock);
            audioBufferCache[pending.cacheKey] = std::move(pending.buffer);
            bufferSampleRates[pending.cacheKey] = bufferRate;

            auto& lfEntry = lockFreeBufferCache[pending.cacheKey];
            if (!lfEntry)
                lfEntry = std::make_unique<LockFreeBufferEntry>();

            juce::AudioBuffer<float>* bufPtr = audioBufferCache[pending.cacheKey].get();
            lfEntry->bufferPtr.store(bufPtr, std::memory_order_release);
            lfEntry->ready.store(true, std::memory_order_release);
        }

        const int64_t resampleProgressUnits = progressUnits - decodeProgressUnits;
        reportProgress(decodeProgressUnits
            + (resampleProgressUnits * static_cast<int64_t>(pendingIndex + 1))
                / static_cast<int64_t>(pendingChannels.size()));
    }

    reportProgress(progressUnits);
    return true;
}

void SampleEngine::resampleBuffer(juce::AudioBuffer<float>& buffer, 
                                  double sourceSampleRate, double targetSampleRate)
{
    // Validate sample rates
    if (sourceSampleRate <= 0.0 || targetSampleRate <= 0.0)
        return;
    
    if (std::abs(sourceSampleRate - targetSampleRate) < 0.1)
        return; // No resampling needed

    // Calculate speed ratio for interpolator (source/target)
    // For upsampling (44.1->48kHz): ratio = 44100/48000 = 0.91875
    // For downsampling (48->44.1kHz): ratio = 48000/44100 = 1.08843
    const double speedRatio = sourceSampleRate / targetSampleRate;
    
    // Calculate new buffer length
    const int originalLength = buffer.getNumSamples();
    const int newLength = static_cast<int>(std::ceil(originalLength / speedRatio));

    if (newLength <= 0 || newLength > originalLength * 4) // Sanity check
        return;

    // Create temporary buffer for result
    juce::AudioBuffer<float> resampledBuffer(1, newLength);
    resampledBuffer.clear();

    const float* sourceData = buffer.getReadPointer(0);
    float* destData = resampledBuffer.getWritePointer(0);
    
    // Determine which interpolator to use based on ratio difference
    const double ratioError = std::abs(speedRatio - 1.0);
    
    if (ratioError < 0.15) // Common conversions: 44.1<->48kHz
    {
        // Fast linear interpolation for common sample rate conversions
        juce::LinearInterpolator interpolator;
        interpolator.reset();
        
        int samplesProcessed = interpolator.process(speedRatio, sourceData, destData, 
                                                   newLength, originalLength, 0);
        
        // Fill rest with silence if needed
        if (samplesProcessed < newLength)
            juce::FloatVectorOperations::clear(destData + samplesProcessed, newLength - samplesProcessed);
    }
    else
    {
        // High-quality Lagrange for larger differences
        juce::LagrangeInterpolator interpolator;
        interpolator.reset();
        
        int samplesProcessed = interpolator.process(speedRatio, sourceData, destData, 
                                                   newLength, originalLength, 0);
        
        // Fill rest with silence if needed
        if (samplesProcessed < newLength)
            juce::FloatVectorOperations::clear(destData + samplesProcessed, newLength - samplesProcessed);
    }

    // Transfer ownership instead of allocating a second destination buffer and
    // copying the complete resampled signal again.
    buffer = std::move(resampledBuffer);
}

// ============================================================================
// FASE 2: Disk cache (.dcc) — pre-decoded, resampled sample cache
// ============================================================================

juce::File SampleEngine::getCacheDirectory()
{
#if JUCE_WINDOWS
    // %LOCALAPPDATA%\DrumCraker\cache
    juce::String localApp = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", "");
    juce::File dir;
    if (localApp.isNotEmpty())
        dir = juce::File(localApp).getChildFile("DrumCraker").getChildFile("cache");
    else
        dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
              .getParentDirectory().getChildFile("Local")
              .getChildFile("DrumCraker").getChildFile("cache");
#elif JUCE_MAC
    juce::File dir = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                     .getChildFile("Library").getChildFile("Caches").getChildFile("DrumCraker");
#else
    // Linux, FreeBSD — respect XDG_CACHE_HOME
    juce::String xdg = juce::SystemStats::getEnvironmentVariable("XDG_CACHE_HOME", "");
    juce::File dir;
    if (xdg.isNotEmpty())
        dir = juce::File(xdg).getChildFile("DrumCraker");
    else
        dir = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
              .getChildFile(".cache").getChildFile("DrumCraker");
#endif
    dir.createDirectory();
    return dir;
}

juce::File SampleEngine::getCacheFileForKit(const juce::String& kitPath, double targetSR)
{
    // FNV-1a 64-bit hash of (kitPath | targetSR) for a unique cache filename
    const juce::String hashInput = kitPath + "|" + juce::String(targetSR, 2);
    uint64_t h = 14695981039346656037ULL;
    auto utf8 = hashInput.toUTF8();
    for (size_t i = 0; i < utf8.sizeInBytes() - 1; ++i)
    {
        h ^= static_cast<uint8_t>(utf8[i]);
        h *= 1099511628211ULL;
    }
    return getCacheDirectory().getChildFile(juce::String::toHexString(static_cast<int64_t>(h)) + ".dcc");
}

uint64_t SampleEngine::computeKitSignature(const DrumKit& kit, double targetSR)
{
    // FNV-1a 64-bit hash of: kit XML path + mtime + size, all unique source
    // file paths + mtimes + sizes, and the target sample rate. This detects
    // any change to the kit or its samples and invalidates the cache.
    uint64_t h = 14695981039346656037ULL;

    auto mix = [&](const void* data, size_t len)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    };

    auto mixStr = [&](const juce::String& s)
    {
        auto utf8 = s.toUTF8();
        mix(utf8.getAddress(), utf8.sizeInBytes() - 1); // exclude null terminator
    };

    // Kit XML file identity
    if (kit.kitFile.existsAsFile())
    {
        mixStr(kit.kitFile.getFullPathName());
        const int64_t mtime = kit.kitFile.getLastModificationTime().toMilliseconds();
        const int64_t size = kit.kitFile.getSize();
        mix(&mtime, sizeof(mtime));
        mix(&size, sizeof(size));
    }

    // Instrument definition XMLs are part of the cache identity: they decide
    // which source channels are requested even when the audio files themselves
    // have not changed.
    std::set<juce::String> definitionPaths;
    for (const auto& definitionFile : kit.definitionFiles)
        definitionPaths.insert(definitionFile.getFullPathName());

    for (const auto& path : definitionPaths)
    {
        mixStr(path);
        const juce::File file(path);
        const int64_t mtime = file.getLastModificationTime().toMilliseconds();
        const int64_t size = file.getSize();
        mix(&mtime, sizeof(mtime));
        mix(&size, sizeof(size));
    }

    // All unique source audio files (sorted for deterministic ordering)
    std::set<juce::String> paths;
    for (const auto& instr : kit.instruments)
    {
        for (const auto& sample : instr->samples)
        {
            for (const auto& af : sample.audioFiles)
                paths.insert(af.audioFile.getFullPathName());
        }
    }

    for (const auto& path : paths)
    {
        mixStr(path);
        juce::File f(path);
        const int64_t m = f.getLastModificationTime().toMilliseconds();
        const int64_t s = f.getSize();
        mix(&m, sizeof(m));
        mix(&s, sizeof(s));
    }

    // Target sample rate — different SR = different cache
    mix(&targetSR, sizeof(targetSR));

    return h;
}

bool SampleEngine::loadFromDiskCache(const juce::File& cacheFile, double expectedSR, uint64_t expectedSig)
{
    if (!cacheFile.existsAsFile())
        return false;

    // Incremental GZIP decompression — no need to load the entire file into RAM.
    // FileInputStream reads sequentially (fast on SSD/HDD), GZIPDecompressorInputStream
    // decompresses on-the-fly. Each entry's sample data is read directly into its
    // AudioBuffer, so peak memory is just the buffers themselves (no intermediate copy).
    juce::FileInputStream rawIn(cacheFile);
    if (!rawIn.openedOk())
        return false;

    juce::GZIPDecompressorInputStream in(rawIn);

    // Helper: read exactly `len` bytes from the decompressing stream (handles
    // short reads that GZIPDecompressorInputStream may return).
    auto readExact = [&](void* dest, size_t len) -> bool
    {
        size_t totalRead = 0;
        while (totalRead < len)
        {
            const size_t remaining = len - totalRead;
            const int requestSize = static_cast<int>(std::min<size_t>(
                remaining, static_cast<size_t>(1024 * 1024)));
            auto bytesRead = in.read(static_cast<char*>(dest) + totalRead,
                                     requestSize);
            if (bytesRead <= 0)
                return false;
            totalRead += static_cast<size_t>(bytesRead);
        }
        return true;
    };

    // ---- Header (32 bytes) ----
    char magic[4];
    if (!readExact(magic, 4))
        return false;
    if (std::memcmp(magic, "DCC2", 4) != 0)
        return false; // Unknown or old uncompressed format — regenerate

    uint32_t version = 0;
    if (!readExact(&version, 4))
        return false;
    if (version != 2)
        return false; // Incompatible cache version — regenerate

    double cacheSR = 0.0;
    if (!readExact(&cacheSR, 8))
        return false;
    if (std::abs(cacheSR - expectedSR) > 0.1)
        return false; // Different sample rate — cache miss

    uint64_t cacheSig = 0;
    if (!readExact(&cacheSig, 8))
        return false;
    if (cacheSig != expectedSig)
        return false; // Kit changed — cache miss

    uint32_t numEntries = 0;
    if (!readExact(&numEntries, 4))
        return false;
    if (numEntries == 0 || numEntries > 10000000U)
        return false;

    loadedSampleCount.store(0, std::memory_order_relaxed);
    totalSampleCount.store(static_cast<int64_t>(numEntries), std::memory_order_relaxed);

    // ---- Entries ----
    // Read each entry's data WITHOUT holding cacheLock (disk I/O can take
    // seconds for large kits). Only lock briefly to publish each entry into
    // the caches. This allows prepare() to interleave between entries if the
    // DAW calls it during load.
    for (uint32_t i = 0; i < numEntries; ++i)
    {
        if (shouldStopLoading.load())
            return false;

        uint32_t keyLen = 0;
        if (!readExact(&keyLen, 4))
            return false;
        if (keyLen == 0 || keyLen > 1024U * 1024U)
            return false;

        std::string keyBuf(keyLen, '\0');
        if (!readExact(keyBuf.data(), keyLen))
            return false;
        juce::String cacheKey = juce::String::fromUTF8(keyBuf.data(), static_cast<int>(keyLen));

        uint32_t numSamples = 0;
        if (!readExact(&numSamples, 4))
            return false;
        if (numSamples == 0
            || static_cast<uint64_t>(numSamples) > static_cast<uint64_t>(std::numeric_limits<int>::max()))
            return false;

        const size_t dataLen = static_cast<size_t>(numSamples) * sizeof(float);

        auto buffer = std::make_unique<juce::AudioBuffer<float>>(1, static_cast<int>(numSamples));
        // Read sample data directly into the AudioBuffer (zero-copy from decompressor)
        if (!readExact(buffer->getWritePointer(0), dataLen))
            return false;

        // Brief lock to publish into the caches
        {
            juce::ScopedLock lock(cacheLock);
            // Buffer is stored at cacheSR (= expectedSR = current project SR)
            bufferSampleRates[cacheKey] = cacheSR;
            audioBufferCache[cacheKey] = std::move(buffer);

            auto& lfEntry = lockFreeBufferCache[cacheKey];
            if (!lfEntry)
                lfEntry = std::make_unique<LockFreeBufferEntry>();
            lfEntry->bufferPtr.store(audioBufferCache[cacheKey].get(), std::memory_order_release);
            lfEntry->ready.store(true, std::memory_order_release);
        }

        loadedSampleCount.fetch_add(1, std::memory_order_relaxed);
    }

    return true;
}

void SampleEngine::writeDiskCacheAsync(const juce::File& cacheFile, double targetSR, uint64_t kitSig)
{
    // Wait for any previous write to finish before starting a new one
    waitForCacheWrite();

    // Use a promise so waitForCacheWrite() can block until the thread finishes.
    auto promise = std::make_shared<std::promise<void>>();
    cacheWriteFuture = promise->get_future();

    // Launch at background priority so cache writing doesn't compete with
    // audio, UI, or other real-time work for CPU and I/O.
    if (!juce::Thread::launch(juce::Thread::Priority::background,
        [this, cacheFile, targetSR, kitSig, promise]()
    {
        // Ensure the promise is always fulfilled, even on early return.
        struct ScopeGuard {
            std::shared_ptr<std::promise<void>> p;
            ~ScopeGuard() { try { p->set_value(); } catch (...) {} }
        } guard{ promise };

        // Only one plugin instance/process may create a cache for this kit and
        // sample rate at a time.
        juce::InterProcessLock processLock(
            "DrumCrakerCache_" + cacheFile.getFileNameWithoutExtension());
        if (!processLock.enter(0))
            return;

        struct ProcessLockGuard
        {
            juce::InterProcessLock& lock;
            ~ProcessLockGuard() { lock.exit(); }
        } processLockGuard{ processLock };

        // Snapshot the list of keys and calculate a conservative upper bound.
        std::vector<juce::String> keys;
        int64_t rawSizeEstimate = 32;
        {
            juce::ScopedLock lock(cacheLock);
            keys.reserve(audioBufferCache.size());
            for (const auto& kv : audioBufferCache)
            {
                if (!kv.second)
                    continue;
                keys.push_back(kv.first);
                rawSizeEstimate += 8 + static_cast<int64_t>(kv.first.getNumBytesAsUTF8());
                rawSizeEstimate += static_cast<int64_t>(kv.second->getNumSamples())
                                   * static_cast<int64_t>(sizeof(float));
            }
        }

        if (keys.empty() || keys.size() > std::numeric_limits<uint32_t>::max())
            return;

        const int64_t cacheLimit = getMaxCacheBytes();
        const int64_t bytesFree = cacheFile.getParentDirectory().getBytesFreeOnVolume();
        constexpr int64_t freeSpaceReserve = 512LL * 1024 * 1024;
        if (rawSizeEstimate > cacheLimit
            || (bytesFree >= 0 && rawSizeEstimate + freeSpaceReserve > bytesFree))
            return;

        // Write beside the target and replace it only after GZIP has closed
        // successfully. Cancellation or a crash leaves the old cache intact.
        juce::TemporaryFile temporaryFile(cacheFile);
        bool completed = false;
        {
            juce::FileOutputStream rawOut(temporaryFile.getFile());
            if (!rawOut.openedOk())
                return;

            {
                // Level 1 favours load/write speed over maximum compression.
                juce::GZIPCompressorOutputStream out(rawOut, 1);

                const char magic[4] = { 'D', 'C', 'C', '2' };
                const uint32_t version = 2;
                const uint32_t numEntries = static_cast<uint32_t>(keys.size());
                if (!out.write(magic, 4)
                    || !out.write(&version, 4)
                    || !out.write(&targetSR, 8)
                    || !out.write(&kitSig, 8)
                    || !out.write(&numEntries, 4))
                    return;

                for (const auto& key : keys)
                {
                    if (shouldStopLoading.load(std::memory_order_acquire))
                        return;

                    uint32_t numSamples = 0;
                    const float* dataPtr = nullptr;
                    {
                        juce::ScopedLock lock(cacheLock);
                        auto it = audioBufferCache.find(key);
                        if (it == audioBufferCache.end() || !it->second)
                            return;
                        numSamples = static_cast<uint32_t>(it->second->getNumSamples());
                        dataPtr = it->second->getReadPointer(0);
                    }

                    const std::string keyUtf8 = key.toStdString();
                    const uint32_t keyLen = static_cast<uint32_t>(keyUtf8.size());
                    if (!out.write(&keyLen, 4)
                        || !out.write(keyUtf8.data(), keyLen)
                        || !out.write(&numSamples, 4)
                        || !out.write(dataPtr, static_cast<size_t>(numSamples) * sizeof(float)))
                        return;
                }

                out.flush();
                completed = true;
            }

            rawOut.flush();
            if (rawOut.getStatus().failed())
                completed = false;
        }

        if (!completed || shouldStopLoading.load(std::memory_order_acquire)
            || !temporaryFile.overwriteTargetFileWithTemporary())
            return;

        // Prune stale cache files after writing a new one (best-effort,
        // non-blocking, runs at background priority).
        pruneDiskCache();
    }))
    {
        // Thread creation failed — fulfill the promise immediately so
        // waitForCacheWrite() doesn't hang.
        promise->set_value();
    }
}

int64_t SampleEngine::getMaxCacheBytes()
{
    constexpr int64_t bytesPerGiB = 1024LL * 1024 * 1024;
    const juce::String configured = juce::SystemStats::getEnvironmentVariable(
        "DRUMCRAKER_CACHE_MAX_GB", "20");
    const int64_t gibibytes = juce::jlimit<int64_t>(
        1, 1024, static_cast<int64_t>(configured.getLargeIntValue()));
    return gibibytes * bytesPerGiB;
}

void SampleEngine::waitForCacheWrite()
{
    if (cacheWriteFuture.valid())
        cacheWriteFuture.wait();
}

void SampleEngine::pruneDiskCache()
{
    // Best-effort cleanup of stale .dcc files. Runs at background priority
    // after a new cache file is written. Removes .dcc files older than 30 days
    // and enforces a configurable total cache directory size limit (keeps the most
    // recently used files). This prevents orphaned cache files from kits that
    // were changed or deleted from accumulating forever.
    juce::Thread::launch(juce::Thread::Priority::background, []()
    {
        const juce::File cacheDir = getCacheDirectory();
        if (!cacheDir.isDirectory())
            return;

        juce::Array<juce::File> cacheFiles;
        cacheDir.findChildFiles(cacheFiles, juce::File::findFiles, false, "*.dcc");

        if (cacheFiles.isEmpty())
            return;

        const juce::Time now = juce::Time::getCurrentTime();
        const int64_t maxAgeMs = 30LL * 24 * 60 * 60 * 1000; // 30 days
        const int64_t maxTotalBytes = getMaxCacheBytes();

        // Phase 1: Remove files older than 30 days
        int64_t totalSize = 0;
        for (const auto& f : cacheFiles)
        {
            const int64_t age = now.toMilliseconds() - f.getLastModificationTime().toMilliseconds();
            if (age > maxAgeMs)
            {
                f.deleteFile();
            }
            else
            {
                totalSize += f.getSize();
            }
        }

        // Phase 2: If still over the size limit, remove oldest files first
        if (totalSize > maxTotalBytes)
        {
            // Re-scan surviving files and sort by modification time (oldest first)
            juce::Array<juce::File> survivors;
            cacheDir.findChildFiles(survivors, juce::File::findFiles, false, "*.dcc");

            std::sort(survivors.begin(), survivors.end(),
                [](const juce::File& a, const juce::File& b)
                {
                    return a.getLastModificationTime() < b.getLastModificationTime();
                });

            for (const auto& f : survivors)
            {
                if (totalSize <= maxTotalBytes)
                    break;
                totalSize -= f.getSize();
                f.deleteFile();
            }
        }
    });
}

const DrumSample* SampleEngine::getSampleForNote(int midiNote, float velocity, float roundRobinAmount, juce::String* outInstrumentName)
{
    if (!currentKit)
        return nullptr;

    // Find instrument for this MIDI note
    auto it = currentKit->midiMap.find(midiNote);
    if (it == currentKit->midiMap.end())
        return nullptr;

    const juce::String& instrumentName = it->second;

    // Return instrument name if requested
    if (outInstrumentName)
        *outInstrumentName = instrumentName;

    // Use cached instrument lookup for performance
    Instrument* targetInstrument = nullptr;
    auto cacheIt = instrumentCache.find(instrumentName);
    if (cacheIt != instrumentCache.end())
    {
        targetInstrument = cacheIt->second;
    }
    else
    {
        // Find and cache the instrument
        for (const auto& instrument : currentKit->instruments)
        {
            if (instrument->name == instrumentName)
            {
                targetInstrument = instrument.get();
                instrumentCache[instrumentName] = targetInstrument;
                break;
            }
        }
    }

    if (!targetInstrument || targetInstrument->samples.empty())
        return nullptr;

    // Normalize power values to find range
    float minPower = targetInstrument->samples[0].power;
    float maxPower = targetInstrument->samples[0].power;

    for (const auto& sample : targetInstrument->samples)
    {
        minPower = std::min(minPower, sample.power);
        maxPower = std::max(maxPower, sample.power);
    }

    // Clamp power values to reasonable range (0-1)
    // Some kits have incorrect power values outside this range
    minPower = juce::jlimit(0.0f, 1.0f, minPower);
    maxPower = juce::jlimit(0.0f, 1.0f, maxPower);

    // Handle edge case: single sample or all samples have same power
    if (std::abs(maxPower - minPower) < 0.001f)
    {
        // All samples have same power - use round robin if multiple samples
        // This happens with kits that don't have velocity layers
        if (targetInstrument->samples.size() == 1)
        {
            return &targetInstrument->samples[0];
        }

        // Multiple samples with same power - use round robin
        int noteIndex = midiNote % 128;  // Clamp to array size
        int lastIndex = lastSampleIndex[noteIndex].load(std::memory_order_relaxed);
        int nextIndex = (lastIndex + 1) % static_cast<int>(targetInstrument->samples.size());
        lastSampleIndex[noteIndex].store(nextIndex, std::memory_order_relaxed);
        return &targetInstrument->samples[nextIndex];
    }

    // Normalize velocity to power range
    float normalizedVelocity = minPower + (velocity * (maxPower - minPower));

    // Find candidate samples based on velocity (power) - using pre-allocated array
    int numCandidates = 0;
    float tolerance = (maxPower - minPower) * 0.25f; // 25% of total range for more variety

    for (const auto& sample : targetInstrument->samples)
    {
        float diff = std::abs(normalizedVelocity - sample.power);
        if (diff < tolerance && numCandidates < maxCandidates)
        {
            candidatePool[numCandidates++] = &sample;
        }
    }

    // If no candidates in range, expand search
    if (numCandidates == 0)
    {
        // Find 3-5 closest samples to have round robin pool - using pre-allocated array
        int numSamplesWithDiff = 0;

        for (const auto& sample : targetInstrument->samples)
        {
            if (numSamplesWithDiff < maxSamplesPerInstrument)
            {
                float diff = std::abs(normalizedVelocity - sample.power);
                samplesWithDiffPool[numSamplesWithDiff++] = {&sample, diff};
            }
        }

        // Sort by proximity (simple insertion sort for small arrays)
        for (int i = 1; i < numSamplesWithDiff; ++i)
        {
            for (int j = i; j > 0 && samplesWithDiffPool[j].second < samplesWithDiffPool[j - 1].second; --j)
            {
                std::swap(samplesWithDiffPool[j], samplesWithDiffPool[j - 1]);
            }
        }

        // Take 4 closest (or all if less)
        numCandidates = std::min(4, numSamplesWithDiff);
        for (int i = 0; i < numCandidates; ++i)
        {
            candidatePool[i] = samplesWithDiffPool[i].first;
        }
    }

    // Ensure there's always at least one candidate
    if (numCandidates == 0)
    {
        return &targetInstrument->samples[0];
    }

    // ROUND ROBIN ANTI-MACHINE GUN
    // Avoids repeating same sample consecutively
    int noteIndex = midiNote % 128;  // Clamp to array size
    int lastIndex = lastSampleIndex[noteIndex].load(std::memory_order_relaxed);
    int selectedIndex = 0;

    if (numCandidates == 1)
    {
        // Only one candidate, use it
        selectedIndex = 0;
    }
    else if (roundRobinAmount < 0.01f)
    {
        // Pure velocity mode: always closest
        float minDiff = std::abs(normalizedVelocity - candidatePool[0]->power);
        for (int i = 1; i < numCandidates; ++i)
        {
            float diff = std::abs(normalizedVelocity - candidatePool[i]->power);
            if (diff < minDiff)
            {
                minDiff = diff;
                selectedIndex = i;
            }
        }
    }
    else if (roundRobinAmount > 0.99f)
    {
        // Pure rotation mode: next sample different from last
        // Sort by proximity to velocity - using pre-allocated array
        int numIndexedCandidates = 0;
        for (int i = 0; i < numCandidates; ++i)
        {
            float diff = std::abs(normalizedVelocity - candidatePool[i]->power);
            indexedCandidatesPool[numIndexedCandidates++] = {i, diff};
        }

        // Simple insertion sort
        for (int i = 1; i < numIndexedCandidates; ++i)
        {
            for (int j = i; j > 0 && indexedCandidatesPool[j].second < indexedCandidatesPool[j - 1].second; --j)
            {
                std::swap(indexedCandidatesPool[j], indexedCandidatesPool[j - 1]);
            }
        }

        // Rotate among best candidates, avoiding last used
        int poolSize = std::min(4, numIndexedCandidates);
        int nextIndex = (lastIndex + 1) % poolSize;
        selectedIndex = indexedCandidatesPool[nextIndex].first;
    }
    else
    {
        // Smart hybrid mode: weighted random with penalty to last used - using pre-allocated array
        int numWeightedCandidates = 0;
        float totalWeight = 0.0f;

        for (int i = 0; i < numCandidates; ++i)
        {
            float diff = std::abs(normalizedVelocity - candidatePool[i]->power);

            // Base weight: inversely proportional to velocity difference
            float weight = 1.0f / (1.0f + diff * 5.0f);

            // VERY STRONG PENALTY to last used sample (avoids machine gun)
            if (i == lastIndex)
            {
                // Penalty scaled by roundRobinAmount
                // With 0.7 default: 93% penalty
                float penalty = 0.1f - (roundRobinAmount * 0.08f);
                weight *= juce::jmax(0.01f, penalty);
            }
            // Bonus to next in rotation (stronger with high roundRobinAmount)
            else if (i == (lastIndex + 1) % numCandidates)
            {
                weight *= (1.0f + roundRobinAmount * 1.5f);
            }

            weightedCandidatesPool[numWeightedCandidates++] = {i, weight};
            totalWeight += weight;
        }

        // Weighted random selection (lock-free RNG, safe on audio thread)
        float randomValue = rrRng.nextFloat() * totalWeight;
        float cumulative = 0.0f;

        for (int i = 0; i < numWeightedCandidates; ++i)
        {
            cumulative += weightedCandidatesPool[i].second;
            if (randomValue <= cumulative)
            {
                selectedIndex = weightedCandidatesPool[i].first;
                break;
            }
        }
    }

    lastSampleIndex[noteIndex].store(selectedIndex, std::memory_order_relaxed);
    return candidatePool[selectedIndex];
}

juce::AudioBuffer<float>* SampleEngine::getAudioBuffer(const DrumSample* sample,
                                                       const juce::String& channelName)
{
    if (!sample)
        return nullptr;

    // Find audio file for this channel
    for (const auto& audioSample : sample->audioFiles)
    {
        if (audioSample.channelName == channelName)
        {
            juce::String cacheKey = audioSample.audioFile.getFullPathName() +
                                   "_ch" + juce::String(audioSample.fileChannel);

            // Lock-free access: check if ready and get pointer atomically
            auto it = lockFreeBufferCache.find(cacheKey);
            if (it != lockFreeBufferCache.end())
            {
                auto* entry = it->second.get();
                if (entry && entry->ready.load(std::memory_order_acquire))
                {
                    return entry->bufferPtr.load(std::memory_order_acquire);
                }
            }

            return nullptr;
        }
    }

    // Channel doesn't exist in this sample
    return nullptr;
}

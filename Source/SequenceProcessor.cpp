/*
  ==============================================================================
    Docs go here.
  ==============================================================================
*/

#include "SequenceProcessor.h"

using namespace synthvr;

SequenceProcessor::SequenceProcessor() : BaseProcessor(BusesProperties()
    .withInput("Input", AudioChannelSet::discreteChannels(4))
    .withOutput("Output", AudioChannelSet::discreteChannels(3)))
{
    addParameter(gateLengthParam = new AudioParameterFloat("gateLength", "Gate Length", 0.0f, 0.95f, 0.75f));
    addParameter(glideParam = new AudioParameterFloat("glide", "Glide", 0.0f, 1.0f, 0.0f));
    addParameter(loopingParam = new AudioParameterBool("looping", "Looping", true));
    addParameter(pitchExtentParam = new AudioParameterFloat("pitchExtent", "Pitch Extent", 0.2f, 1.0f, 0.2f));
    addParameter(rootPitchParam = new AudioParameterInt("rootPitch", "Root Pitch", 0, 11, 0));
    addParameter(pitchScaleParam = new AudioParameterInt("pitchScale", "Pitch Scale", unscaled, major, minor));
    addParameter(toggleRunningParam = new AudioParameterBool("toggleRunning", "Toggle Running", false));

    // Initialize parameters for each step
    stepsPitchIndices = std::vector<int>();
    stepsOnIndices = std::vector<int>();
    stepsGateModeIndices = std::vector<int>();
    stepsPulseCountIndices = std::vector<int>();
    auto stepParamIndexOffset = getNumParameters();
    for (int i = 0; i < numSteps; i++)
    {
        addParameter(new AudioParameterFloat("stepPitch_" + std::to_string(i), "Step Pitch", 0.0f, 1.0f, 0.0f));
        stepsPitchIndices.push_back((i * 4) + stepParamIndexOffset);
        addParameter(new AudioParameterBool("stepOn_" + std::to_string(i), "Step On/Off", true));
        stepsOnIndices.push_back((i * 4) + stepParamIndexOffset + 1);
        addParameter(new AudioParameterInt("stepGateMode_" + std::to_string(i), "Step Gate Mode", 0, 3, singlePulse));
        stepsGateModeIndices.push_back((i * 4) + stepParamIndexOffset + 2);
        addParameter(new AudioParameterInt("stepPulseCount_" + std::to_string(i), "Step Pulse Count", 1, 8, 1));
        stepsPulseCountIndices.push_back((i * 4) + stepParamIndexOffset + 3);
    }

    addParameter(currentStepDisplay = new AudioParameterInt("currentStepDisplay", "Current Step Display", 0, numSteps, 0));
    addParameter(currentlyTriggeredDisplay = new AudioParameterBool("currentlyTriggeredDisplay", "Currently Triggered Display", false));
    addParameter(currentlyEOSTriggeredDisplay = new AudioParameterBool("currentlyEOSTriggeredDisplay", "Currently EOS Triggered Display", false));
    addParameter(currentlyRunningDisplay = new AudioParameterBool("currentlyRunningDisplay", "Currently Running Display", false));

    glideParam->range.setSkewForCentre(0.9f);
}

SequenceProcessor::~SequenceProcessor() {}

void SequenceProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    currentSampleRate = sampleRate;

    smoothedGlideFilterFrequency.reset(sampleRate, 0.25f);
    smoothedGlideFilterFrequency.setCurrentAndTargetValue(0.0f);

    dsp::ProcessSpec processSpec{ currentSampleRate, static_cast<uint32> (maximumExpectedSamplesPerBlock), 1 };
    glideFilter.prepare(processSpec);
    glideFilter.reset();
    glideFilter.coefficients = calculateGlideFilterCoefficients();
}

void SequenceProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer&)
{
    // Skip execution if all steps are skipped
    allStepsAreSkipped = areAllStepsSkipped();

    if (*toggleRunningParam && !previouslyToggledRunning)
        currentlyRunning = !currentlyRunning;

    for (int sample = 0; sample < buffer.getNumSamples(); sample++)
    {
        currentlyTriggered = buffer.getSample(clockInputChannel, sample) >= 0.5f;
        currentlyReset = buffer.getSample(resetInputChannel, sample) >= 0.5f;
        currentlyRunning = // The sequencer is running if:
            (currentlyRunning // already running
            || buffer.getSample(startInputChannel, sample) >= 0.5f) // start CV triggered
            && !buffer.getSample(stopInputChannel, sample) >= 0.5f; // and stop CV is not triggered
        
        // Initialize state on reset and start conditions
        handleReset();
        handleStart();

        if (currentlyTriggered && !previouslyTriggered && !allStepsAreSkipped)
            handleNewClockTrigger();

        // Handle gate closing
        updateGate();

        // Handle glide
        currentGlideFilterFrequency = smoothedGlideFilterFrequency.getNextValue();
        glideFilter.coefficients = calculateGlideFilterCoefficients();

        // Handle pitch
        if (currentGateOpen)
            updatePitch();

        // Write to buffer
        writeOutputs(buffer, sample);

        // Update state history
        previouslyTriggered = currentlyTriggered;
        previouslyReset = currentlyReset;
        previouslyRunning = currentlyRunning;
        samplesSinceLastPulse++;
    }

    // Set these low-rate displays at the block level
    *currentlyRunningDisplay = currentlyRunning;
    *currentStepDisplay = currentStep;
    previouslyToggledRunning = *toggleRunningParam;
}

void synthvr::SequenceProcessor::writeOutputs(juce::AudioSampleBuffer& buffer, int sample)
{
    buffer.setSample(endOfSequenceOutputChannel, sample, currentEndOfSequenceGateOpen);
    *currentlyEOSTriggeredDisplay = currentEndOfSequenceGateOpen;

    if (currentlyRunning)
    {
        buffer.setSample(triggerOutputChannel, sample, currentGateOpen);
        buffer.setSample(pitchOutputChannel, sample, currentPitch);
        *currentlyTriggeredDisplay = currentGateOpen;
    }
    else
    {
        buffer.setSample(triggerOutputChannel, sample, 0.0f);
        buffer.setSample(pitchOutputChannel, sample, 0.0f);
        *currentlyTriggeredDisplay = 0.0f;
    }
}

void synthvr::SequenceProcessor::handleStart()
{
    if (currentlyRunning && !previouslyRunning)
    {
        previouslyTriggered = false;
        currentlyTriggered = true;
        samplesSinceLastPulse = samplesSinceSecondLastPulse;
    }
}

void synthvr::SequenceProcessor::handleReset()
{
    if (currentlyReset && !previouslyReset)
    {
        currentStep = 0;
        currentPulse = 0;
        currentGateOpen = false;
        currentEndOfSequenceGateOpen = false;
        currentlyRunning = true;
    }
}

dsp::IIR::Coefficients<float>::Ptr SequenceProcessor::calculateGlideFilterCoefficients()
{
    smoothedGlideFilterFrequency.setTargetValue(((1.0f - *glideParam) * noGlideFrequency) + fullGlideFrequency);
    return dsp::IIR::Coefficients<float>::makeLowPass(currentSampleRate, currentGlideFilterFrequency, 0.1f);
}

void SequenceProcessor::handleNewClockTrigger()
{
    // Track timing of pulses for gate length
    samplesPerPulse = samplesSinceLastPulse;
    samplesSinceSecondLastPulse = samplesSinceLastPulse;
    samplesSinceLastPulse = 0;

    // Don't do anything else if the sequencer isn't running
    if (!currentlyRunning)
        return;

    // Handle pulse and step
    if (currentPulse >= getNumPulsesForStep(currentStep))
    {
        currentPulse = 0;

        // Go to next nonskipped step
        while (!getOnOffStatusForStep(++currentStep % numSteps));
        HandleIncrementedStep();
    }

    // Handle gate, get target pitch and increment pulse
    if (currentlyRunning)
    {
        samplesSinceLastGate = 0;
        auto gateMode = getGateModeForStep(currentStep);

        if (currentPulse == 0 || gateMode == multiPulse)
        {
            currentGateLengthSamples = getGateLengthForMode(gateMode);
            currentGateOpen = gateMode != silence;
        }

        currentStepPitch = getPitchForStep(currentStep);
        currentPulse++;
    }
}

void synthvr::SequenceProcessor::HandleIncrementedStep()
{
    // If we have done all steps in the sequence
    if (currentStep >= numSteps)
    {
        currentStep = 0;

        if (!currentEndOfSequenceGateOpen)
        {
            samplesSinceLastEndOfSequenceGate = 0;
            currentEndOfSequenceGateLengthSamples = samplesPerPulse;
            currentEndOfSequenceGateOpen = true;
        }

        if (!*loopingParam)
            currentlyRunning = false;
    }
}

void SequenceProcessor::updatePitch()
{
    targetPitch = currentStepPitch * *pitchExtentParam;

    // TODO: Quantize value to scale

    currentPitch = glideFilter.processSample(targetPitch);
}

void SequenceProcessor::updateGate()
{
    // Close gate if enough samples have passed
    if (currentGateOpen 
        && (float)samplesSinceLastGate++ >= currentGateLengthSamples)
            currentGateOpen = false;

    // Handle EOS trigger separately in case of looping
    if (currentEndOfSequenceGateOpen 
        && (float)samplesSinceLastEndOfSequenceGate++ >= currentEndOfSequenceGateLengthSamples)
            currentEndOfSequenceGateOpen = false;
}

bool SequenceProcessor::getOnOffStatusForStep(int step)
{
    auto param = static_cast<AudioParameterBool*>(getParameters()[stepsOnIndices[step]]);
    return *param;
}

bool SequenceProcessor::areAllStepsSkipped()
{
    for (int i = 0; i < numSteps; i++)
        if (getOnOffStatusForStep(i))
            return false;

    return true;
}

int SequenceProcessor::getNumPulsesForStep(int step)
{
    auto param = static_cast<AudioParameterInt*>(getParameters()[stepsPulseCountIndices[step]]);
    return *param;
}

int SequenceProcessor::getGateModeForStep(int step)
{
    auto param = static_cast<AudioParameterInt*>(getParameters()[stepsGateModeIndices[step]]);
    return *param;
}

float SequenceProcessor::getPitchForStep(int step)
{
    return getParameters()[stepsPitchIndices[step]]->getValue();
}

float SequenceProcessor::getGateLengthForMode(int mode)
{
    if (samplesPerPulse == 0)
        samplesPerPulse = 5000;

    if (mode == singlePulse || mode == multiPulse)
        return (float)samplesPerPulse * *gateLengthParam;
    else if (mode == holdForPulse)
        return (float)samplesPerPulse * getNumPulsesForStep(currentStep) * 0.99f;
    else
        return 0.0f;
}

bool SequenceProcessor::incrementCurrentStepUntilEnd()
{
    auto nextStep = currentStep + 1;
    if (nextStep >= numSteps)
        return false;

    currentStep = nextStep;

    return true;
}

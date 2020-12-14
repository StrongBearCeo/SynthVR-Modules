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
    addParameter(gateLengthParam = new AudioParameterFloat("gateLength", "Gate Length", 0.0f, 1.0f, 0.75f));
    addParameter(glideParam = new AudioParameterFloat("glide", "Glide", 0.0f, 1.0f, 0.0f));
    addParameter(loopingParam = new AudioParameterBool("looping", "Looping", true));
    addParameter(pitchExtentParam = new AudioParameterFloat("pitchExtent", "Pitch Extent", 0.2f, 1.0f, 0.2f));
    addParameter(rootPitchParam = new AudioParameterInt("rootPitch", "Root Pitch", 0, 11, 0));
    addParameter(pitchScaleParam = new AudioParameterInt("pitchScale", "Pitch Scale", unscaled, major, minor));

    // Initialize parameters for each step
    stepsPitchIndices = std::vector<int>();
    stepsOnIndices = std::vector<int>();
    auto stepParamIndexOffset = getNumParameters();
    for (int i = 0; i < numSteps; i++)
    {
        addParameter(new AudioParameterFloat("stepPitch_" + std::to_string(i), "Step Pitch", 0.0f, 1.0f, 0.5f));
        stepsPitchIndices.push_back((i * 4) + stepParamIndexOffset + 1);
        addParameter(new AudioParameterBool("stepOn_" + std::to_string(i), "Step On/Off", true));
        stepsOnIndices.push_back((i * 4) + stepParamIndexOffset + 2);
        addParameter(new AudioParameterInt("stepGateMode_" + std::to_string(i), "Step Gate Mode", silence, holdForPulse, singlePulse));
        stepsGateModeIndices.push_back((i * 4) + stepParamIndexOffset + 3);
        addParameter(new AudioParameterInt("stepPulseCount_" + std::to_string(i), "Step Pulse Count", 1, 8, 1));
        stepsPulseCountIndices.push_back((i * 4) + stepParamIndexOffset + 4);
    }

    addParameter(currentStepDisplay = new AudioParameterInt("currentStepDisplay", "Current Step Display", 0, numSteps, 0));
    addParameter(currentlyTriggeredDisplay = new AudioParameterBool("currentlyTriggeredDisplay", "Currently Triggered Display", false));
}

SequenceProcessor::~SequenceProcessor() {}

void SequenceProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    currentSampleRate = sampleRate;

    dsp::ProcessSpec processSpec{ currentSampleRate, static_cast<uint32> (maximumExpectedSamplesPerBlock), 1 };
    glideFilter.prepare(processSpec);
    glideFilter.reset();
    glideFilter.coefficients = calculateGlideFilterCoefficients();
}

void SequenceProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer&)
{
    // Update pitch smoothing filter if glide param changed
    if (*glideParam != previousGlide)
    {
        glideFilter.coefficients = calculateGlideFilterCoefficients();
        previousGlide = *glideParam;
    }

    // Skip execution if all steps are skipped
    allStepsAreSkipped = areAllStepsSkipped();

    for (int sample = 0; sample < buffer.getNumSamples(); sample++)
    {
        currentlyTriggered = buffer.getSample(clockInputChannel, sample) >= 0.5f;

        // Handle step sequencing
        if (currentlyTriggered && !previouslyTriggered && !allStepsAreSkipped)
            handleNewClockTrigger();

        // Handle gate closing
        updateGate();

        // Handle pitch
        if (currentGateOpen)
            updatePitch();

        // Write outputs
        buffer.setSample(triggerOutputChannel, sample, currentGateOpen);
        buffer.setSample(pitchOutputChannel, sample, currentPitch);
        buffer.setSample(endOfSequenceOutputChannel, sample, currentEndOfSequenceGateOpen);
        *currentStepDisplay = currentStep;
        *currentlyTriggeredDisplay = currentGateOpen;

        // Update state
        previouslyTriggered = currentlyTriggered;
        samplesSinceLastPulse++;
    }
}

dsp::IIR::Coefficients<float>::Ptr SequenceProcessor::calculateGlideFilterCoefficients()
{
    auto frequency = ((1.0f - *glideParam) * noGlideFrequency) + fullGlideFrequency;
    return dsp::IIR::Coefficients<float>::makeLowPass(currentSampleRate, frequency);
}

void SequenceProcessor::handleNewClockTrigger()
{
    // If we have come to the end of the sequence
    if (endOfSequence)
    {
        samplesSinceLastEndOfSequenceGate = 0;
        currentEndOfSequenceGateLengthSamples = samplesPerPulse;
        currentEndOfSequenceGateOpen = true;
        endOfSequence = false;
    }

    // Don't do anything else if the sequencer isn't running
    if (!currentlyStarted)
        return;

    // If we have done all pulses for a step
    if (currentPulse++ >= getNumPulsesForStep(currentStep))
    {
        currentPulse = 0;

        // Find the next non-skipped step
        while (!incrementCurrentStepUntilEnd() && !getOnOffStatusForStep(currentStep));

        samplesPerPulse = samplesSinceLastPulse;
        samplesSinceLastPulse = 0;
    }

    // If we have done all steps in the sequence
    if (currentStep >= numSteps)
    {
        currentStep = 0;
        endOfSequence = true;

        if (!*loopingParam)
            currentlyStarted = false;
    }

    // Open gate if sequence is running
    if (currentlyStarted)
    {
        samplesSinceLastGate = 0;
        auto gateMode = getGateModeForStep(currentStep);
        currentGateLengthSamples = getGateLengthForMode(gateMode);
        currentGateOpen = gateMode != silence;
    }
}

void SequenceProcessor::updatePitch()
{
    targetPitch = getPitchForStep(currentStep) * *pitchExtentParam;

    // TODO: Quantize value to scale

    currentPitch = glideFilter.processSample(targetPitch);
}

void SequenceProcessor::updateGate()
{
    // Close gate if enough samples have passed
    if (currentGateOpen)
    {
        if ((float)samplesSinceLastGate++ >= currentGateLengthSamples)
        {
            currentGateOpen = false;
        }
    }

    // Handle EOS trigger separately in case of looping
    if (currentEndOfSequenceGateOpen)
    {
        if ((float)samplesSinceLastEndOfSequenceGate++ >= currentEndOfSequenceGateLengthSamples)
        {
            currentEndOfSequenceGateOpen = false;
        }
    }
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
    if (mode == singlePulse || mode == multiPulse)
        return (float)samplesPerPulse * *gateLengthParam;
    else if (mode == holdForPulse)
        return (float)samplesPerPulse * getNumPulsesForStep(currentStep) * *gateLengthParam;
    else
        return 0.0f;
}

bool SequenceProcessor::incrementCurrentStepUntilEnd()
{
    auto nextStep = currentStep + 1;
    if (currentStep >= numSteps)
        return false;

    currentStep = nextStep;

    return true;
}

//
//  Audio.cpp
//  interface
//
//  Created by Stephen Birarda on 1/22/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//
#ifndef _WIN32

#include <iostream>
#include <fstream>
#include <pthread.h>
#include <sys/stat.h>
#include <cstring>

#include <StdDev.h>
#include <UDPSocket.h>
#include <SharedUtil.h>
#include <PacketHeaders.h>
#include <AgentList.h>
#include <AgentTypes.h>
#include <AngleUtil.h>

#include "Application.h"
#include "Audio.h"
#include "Util.h"
#include "Log.h"

//  Uncomment the following definition to test audio device latency by copying output to input
//#define TEST_AUDIO_LOOPBACK


#define VISUALIZE_ECHO_CANCELLATION

static const int PHASE_DELAY_AT_90 = 20;
static const float AMPLITUDE_RATIO_AT_90 = 0.5;
static const int MIN_FLANGE_EFFECT_THRESHOLD = 600;
static const int MAX_FLANGE_EFFECT_THRESHOLD = 1500;
static const float FLANGE_BASE_RATE = 4;
static const float MAX_FLANGE_SAMPLE_WEIGHT = 0.50;
static const float MIN_FLANGE_INTENSITY = 0.25;

static const float JITTER_BUFFER_LENGTH_MSECS = 12;
static const short JITTER_BUFFER_SAMPLES = JITTER_BUFFER_LENGTH_MSECS *
                                           NUM_AUDIO_CHANNELS * (SAMPLE_RATE / 1000.0);

static const float AUDIO_CALLBACK_MSECS = (float)BUFFER_LENGTH_SAMPLES_PER_CHANNEL / (float)SAMPLE_RATE * 1000.0;

static const int AGENT_LOOPBACK_MODIFIER = 307;

// Speex preprocessor and echo canceller adaption
static const int   AEC_N_CHANNELS_MIC = 1;                                      // Number of microphone channels
static const int   AEC_N_CHANNELS_PLAY = 2;                                     // Number of speaker channels
static const int   AEC_FILTER_LENGTH = BUFFER_LENGTH_SAMPLES_PER_CHANNEL * 20;  // Width of the filter
static const int   AEC_BUFFERED_FRAMES = 6;                                     // Maximum number of frames to buffer
static const int   AEC_BUFFERED_SAMPLES_PER_CHANNEL = BUFFER_LENGTH_SAMPLES_PER_CHANNEL * AEC_BUFFERED_FRAMES;
static const int   AEC_BUFFERED_SAMPLES = AEC_BUFFERED_SAMPLES_PER_CHANNEL * AEC_N_CHANNELS_PLAY;
static const int   AEC_TMP_BUFFER_SIZE = (AEC_N_CHANNELS_MIC +                  // Temporary space for processing a
        AEC_N_CHANNELS_PLAY) * BUFFER_LENGTH_SAMPLES_PER_CHANNEL;               //  single frame

// Speex preprocessor and echo canceller configuration
static const int   AEC_NOISE_REDUCTION = -80;                                   // Noise reduction (important)
static const int   AEC_RESIDUAL_ECHO_REDUCTION = -60;                           // Residual echo reduction
static const int   AEC_RESIDUAL_ECHO_REDUCTION_ACTIVE = -45;                    //  ~on active side
static const bool  AEC_USE_AGC = true;                                          // Automatic gain control
static const int   AEC_AGC_MAX_GAIN = -30;                                      // Gain in db
static const int   AEC_AGC_TARGET_LEVEL = 9000;                                 // Target reference level
static const int   AEC_AGC_MAX_INC = 6;                                         // Max increase in db/s
static const int   AEC_AGC_MAX_DEC = 200;                                       // Max decrease in db/s
static const bool  AEC_USE_VAD = false;                                         // Voice activity determination

// Ping test configuration 
static const float PING_PITCH = 16.f;                                           // Ping wavelength, # samples / radian
static const float PING_VOLUME = 32000.f;                                       // Ping peak amplitude
static const int   PING_MIN_AMPLI = 225;                                        // Minimum amplitude 
static const int   PING_MAX_PERIOD_DIFFERENCE = 15;                             // Maximum # samples from expected period
static const int   PING_PERIOD = int(Radians::twicePi() * PING_PITCH);          // Sine period based on the given pitch
static const int   PING_HALF_PERIOD = int(Radians::pi() * PING_PITCH);          // Distance between extrema
static const int   PING_FRAMES_TO_RECORD = AEC_BUFFERED_FRAMES;                 // Frames to record for analysis
static const int   PING_SAMPLES_TO_ANALYZE = AEC_BUFFERED_SAMPLES_PER_CHANNEL;  // Samples to analyze (reusing AEC buffer)
static const int   PING_BUFFER_OFFSET = BUFFER_LENGTH_SAMPLES_PER_CHANNEL - PING_PERIOD * 2.0f; // Signal start


inline void Audio::performIO(int16_t* inputLeft, int16_t* outputLeft, int16_t* outputRight) {

    AgentList* agentList = AgentList::getInstance();
    Application* interface = Application::getInstance();
    Avatar* interfaceAvatar = interface->getAvatar();

    eventuallyCancelEcho(inputLeft);
 
    // Add Procedural effects to input samples
    addProceduralSounds(inputLeft, BUFFER_LENGTH_SAMPLES_PER_CHANNEL);
    
    if (agentList && inputLeft) {
        
        //  Measure the loudness of the signal from the microphone and store in audio object
        float loudness = 0;
        for (int i = 0; i < BUFFER_LENGTH_SAMPLES_PER_CHANNEL; i++) {
            loudness += abs(inputLeft[i]);
        }
        
        loudness /= BUFFER_LENGTH_SAMPLES_PER_CHANNEL;
        _lastInputLoudness = loudness;
        
        // add input (@microphone) data to the scope
#ifdef VISUALIZE_ECHO_CANCELLATION
            if (! isCancellingEcho()) {
#endif
        _scope->addSamples(0, inputLeft, BUFFER_LENGTH_SAMPLES_PER_CHANNEL);
#ifdef VISUALIZE_ECHO_CANCELLATION
            }
#endif

        Agent* audioMixer = agentList->soloAgentOfType(AGENT_TYPE_AUDIO_MIXER);
        
        if (audioMixer) {
            glm::vec3 headPosition = interfaceAvatar->getHeadJointPosition();
            glm::quat headOrientation = interfaceAvatar->getHead().getOrientation();
            
            int leadingBytes = sizeof(PACKET_HEADER_MICROPHONE_AUDIO_NO_ECHO) + sizeof(headPosition) + sizeof(headOrientation);
            
            // we need the amount of bytes in the buffer + 1 for type
            // + 12 for 3 floats for position + float for bearing + 1 attenuation byte
            unsigned char dataPacket[BUFFER_LENGTH_BYTES_PER_CHANNEL + leadingBytes];
            
            dataPacket[0] = (Application::getInstance()->shouldEchoAudio())
                ? PACKET_HEADER_MICROPHONE_AUDIO_WITH_ECHO
                : PACKET_HEADER_MICROPHONE_AUDIO_NO_ECHO;
            unsigned char *currentPacketPtr = dataPacket + 1;
            
            // memcpy the three float positions
            memcpy(currentPacketPtr, &headPosition, sizeof(headPosition));
            currentPacketPtr += (sizeof(headPosition));
            
            // memcpy our orientation
            memcpy(currentPacketPtr, &headOrientation, sizeof(headOrientation));
            currentPacketPtr += sizeof(headOrientation);
            
            // copy the audio data to the last BUFFER_LENGTH_BYTES bytes of the data packet
            memcpy(currentPacketPtr, inputLeft, BUFFER_LENGTH_BYTES_PER_CHANNEL);
            
            agentList->getAgentSocket()->send(audioMixer->getActiveSocket(),
                                              dataPacket,
                                              BUFFER_LENGTH_BYTES_PER_CHANNEL + leadingBytes);
        }
        
    }
    
    memset(outputLeft, 0, PACKET_LENGTH_BYTES_PER_CHANNEL);
    memset(outputRight, 0, PACKET_LENGTH_BYTES_PER_CHANNEL);
    
    AudioRingBuffer* ringBuffer = &_ringBuffer;
    
    // if we've been reset, and there isn't any new packets yet
    // just play some silence
    
    if (ringBuffer->getEndOfLastWrite()) {
        if (!ringBuffer->isStarted() && ringBuffer->diffLastWriteNextOutput() < (PACKET_LENGTH_SAMPLES + _jitterBufferSamples)) {
            //
            //  If not enough audio has arrived to start playback, keep waiting
            //
            //printLog("Held back, buffer has %d of %d samples required.\n",
            //         ringBuffer->diffLastWriteNextOutput(),
            //         PACKET_LENGTH_SAMPLES + parentAudio->_jitterBufferSamples);
        } else if (ringBuffer->diffLastWriteNextOutput() < PACKET_LENGTH_SAMPLES) {
            //
            //  If we have run out of audio to send to the audio device, we have starved,
            //  so reset the ring buffer and packet counters.
            //  
            ringBuffer->setStarted(false);
            
            _numStarves++;
            _packetsReceivedThisPlayback = 0;
            
            //printLog("Starved, remaining buffer msecs = %.0f\n",
            //         ringBuffer->diffLastWriteNextOutput() / PACKET_LENGTH_SAMPLES * AUDIO_CALLBACK_MSECS);
            _wasStarved = 10;      //   Frames to render the indication that the system was starved.
        } else {
            //
            //  We are either already playing back, or we have enough audio to start playing back.
            // 
            if (!ringBuffer->isStarted()) {
                ringBuffer->setStarted(true);
                //printLog("starting playback %0.1f msecs delayed, jitter = %d, pkts recvd: %d \n",
                //         (usecTimestampNow() - usecTimestamp(&parentAudio->_firstPacketReceivedTime))/1000.0,
                //         parentAudio->_jitterBufferSamples,
                //         parentAudio->_packetsReceivedThisPlayback);
            }
            //
            // play whatever we have in the audio buffer
            //
            // if we haven't fired off the flange effect, check if we should
            // TODO: lastMeasuredHeadYaw is now relative to body - check if this still works.
            
            int lastYawMeasured = fabsf(interfaceAvatar->getHeadYawRate());
            
            if (!_samplesLeftForFlange && lastYawMeasured > MIN_FLANGE_EFFECT_THRESHOLD) {
                // we should flange for one second
                if ((_lastYawMeasuredMaximum = std::max(_lastYawMeasuredMaximum, lastYawMeasured)) != lastYawMeasured) {
                    _lastYawMeasuredMaximum = std::min(_lastYawMeasuredMaximum, MIN_FLANGE_EFFECT_THRESHOLD);
                    
                    _samplesLeftForFlange = SAMPLE_RATE;
                    
                    _flangeIntensity = MIN_FLANGE_INTENSITY +
                        ((_lastYawMeasuredMaximum - MIN_FLANGE_EFFECT_THRESHOLD) /
                         (float)(MAX_FLANGE_EFFECT_THRESHOLD - MIN_FLANGE_EFFECT_THRESHOLD)) *
                        (1 - MIN_FLANGE_INTENSITY);
                    
                    _flangeRate = FLANGE_BASE_RATE * _flangeIntensity;
                    _flangeWeight = MAX_FLANGE_SAMPLE_WEIGHT * _flangeIntensity;
                }
            }
            
            for (int s = 0; s < PACKET_LENGTH_SAMPLES_PER_CHANNEL; s++) {
                
                int leftSample = ringBuffer->getNextOutput()[s];
                int rightSample = ringBuffer->getNextOutput()[s + PACKET_LENGTH_SAMPLES_PER_CHANNEL];
                
                if (_samplesLeftForFlange > 0) {
                    float exponent = (SAMPLE_RATE - _samplesLeftForFlange - (SAMPLE_RATE / _flangeRate)) /
                        (SAMPLE_RATE / _flangeRate);
                    int sampleFlangeDelay = (SAMPLE_RATE / (1000 * _flangeIntensity)) * powf(2, exponent);
                    
                    if (_samplesLeftForFlange != SAMPLE_RATE || s >= (SAMPLE_RATE / 2000)) {
                        // we have a delayed sample to add to this sample
                        
                        int16_t *flangeFrame = ringBuffer->getNextOutput();
                        int flangeIndex = s - sampleFlangeDelay;
                        
                        if (flangeIndex < 0) {
                            // we need to grab the flange sample from earlier in the buffer
                            flangeFrame = ringBuffer->getNextOutput() != ringBuffer->getBuffer()
                            ? ringBuffer->getNextOutput() - PACKET_LENGTH_SAMPLES
                            : ringBuffer->getNextOutput() + RING_BUFFER_LENGTH_SAMPLES - PACKET_LENGTH_SAMPLES;
                            
                            flangeIndex = PACKET_LENGTH_SAMPLES_PER_CHANNEL + (s - sampleFlangeDelay);
                        }
                        
                        int16_t leftFlangeSample = flangeFrame[flangeIndex];
                        int16_t rightFlangeSample = flangeFrame[flangeIndex + PACKET_LENGTH_SAMPLES_PER_CHANNEL];
                        
                        leftSample = (1 - _flangeWeight) * leftSample + (_flangeWeight * leftFlangeSample);
                        rightSample = (1 - _flangeWeight) * rightSample + (_flangeWeight * rightFlangeSample);
                        
                        _samplesLeftForFlange--;
                        
                        if (_samplesLeftForFlange == 0) {
                            _lastYawMeasuredMaximum = 0;
                        }
                    }
                }
#ifndef TEST_AUDIO_LOOPBACK
                outputLeft[s] = leftSample;
                outputRight[s] = rightSample;
#else 
                outputLeft[s] = inputLeft[s];
                outputRight[s] = inputLeft[s];                    
#endif
            }
            ringBuffer->setNextOutput(ringBuffer->getNextOutput() + PACKET_LENGTH_SAMPLES);
            
            if (ringBuffer->getNextOutput() == ringBuffer->getBuffer() + RING_BUFFER_LENGTH_SAMPLES) {
                ringBuffer->setNextOutput(ringBuffer->getBuffer());
            }
        }
    }

    eventuallySendRecvPing(inputLeft, outputLeft, outputRight);
    eventuallyRecordEcho(outputLeft, outputRight);


    // add output (@speakers) data just written to the scope
#ifdef VISUALIZE_ECHO_CANCELLATION
        if (! isCancellingEcho()) {
    _scope->setColor(2, 0x00ffff);
#endif
    _scope->addSamples(1, outputLeft, BUFFER_LENGTH_SAMPLES_PER_CHANNEL);
    _scope->addSamples(2, outputRight, BUFFER_LENGTH_SAMPLES_PER_CHANNEL);
#ifdef VISUALIZE_ECHO_CANCELLATION
        }
#endif

    gettimeofday(&_lastCallbackTime, NULL);
}

// inputBuffer  A pointer to an internal portaudio data buffer containing data read by portaudio.
// outputBuffer A pointer to an internal portaudio data buffer to be read by the configured output device.
// frames       Number of frames that portaudio requests to be read/written.
// timeInfo     Portaudio time info. Currently unused.
// statusFlags  Portaudio status flags. Currently unused.
// userData     Pointer to supplied user data (in this case, a pointer to the parent Audio object
int Audio::audioCallback (const void* inputBuffer,
                          void* outputBuffer,
                          unsigned long frames,
                          const PaStreamCallbackTimeInfo *timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void* userData) {
    
    int16_t* inputLeft = static_cast<int16_t*const*>(inputBuffer)[0];
    int16_t* outputLeft = static_cast<int16_t**>(outputBuffer)[0];
    int16_t* outputRight = static_cast<int16_t**>(outputBuffer)[1];

    static_cast<Audio*>(userData)->performIO(inputLeft, outputLeft, outputRight);
    return paContinue;
}


static void outputPortAudioError(PaError error) {
    if (error != paNoError) {
        printLog("-- portaudio termination error --\n");
        printLog("PortAudio error (%d): %s\n", error, Pa_GetErrorText(error));
    }
}

Audio::Audio(Oscilloscope* scope, int16_t initialJitterBufferSamples) :
    _stream(NULL),
    _ringBuffer(true),
    _scope(scope),
    _averagedLatency(0.0),
    _measuredJitter(0),
    _jitterBufferSamples(initialJitterBufferSamples),
    _wasStarved(0),
    _numStarves(0),
    _lastInputLoudness(0),
    _lastVelocity(0),
    _lastAcceleration(0),
    _totalPacketsReceived(0),
    _firstPacketReceivedTime(),
    _packetsReceivedThisPlayback(0),
    _isCancellingEcho(false),
    _echoDelay(0),
    _echoSamplesLeft(0l),
    _speexEchoState(NULL),
    _speexPreprocessState(NULL),
    _isSendingEchoPing(false),
    _pingAnalysisPending(false),
    _pingFramesToRecord(0),
    _samplesLeftForFlange(0),
    _lastYawMeasuredMaximum(0),
    _flangeIntensity(0.0f),
    _flangeRate(0.0f),
    _flangeWeight(0.0f)
{
    outputPortAudioError(Pa_Initialize());
    
    //  NOTE:  Portaudio documentation is unclear as to whether it is safe to specify the
    //         number of frames per buffer explicitly versus setting this value to zero.
    //         Possible source of latency that we need to investigate further.
    // 
    unsigned long FRAMES_PER_BUFFER = BUFFER_LENGTH_SAMPLES_PER_CHANNEL;
    
    //  Manually initialize the portaudio stream to ask for minimum latency
    PaStreamParameters inputParameters, outputParameters;
    
    inputParameters.device = Pa_GetDefaultInputDevice(); 
    inputParameters.channelCount = 2;                    //  Stereo input
    inputParameters.sampleFormat = (paInt16 | paNonInterleaved);
    inputParameters.suggestedLatency = Pa_GetDeviceInfo( inputParameters.device )->defaultLowInputLatency;
    //inputParameters.suggestedLatency = 0.0116;
    inputParameters.hostApiSpecificStreamInfo = NULL;

    outputParameters.device = Pa_GetDefaultOutputDevice();
    outputParameters.channelCount = 2;                    //  Stereo output
    outputParameters.sampleFormat = (paInt16 | paNonInterleaved);
    outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
    //outputParameters.suggestedLatency = 0.0116;
    outputParameters.hostApiSpecificStreamInfo = NULL;
    
    
    outputPortAudioError(Pa_OpenStream(&_stream, 
                                       &inputParameters,
                                       &outputParameters,
                                       SAMPLE_RATE,
                                       FRAMES_PER_BUFFER,
                                       paNoFlag,
                                       audioCallback,
                                       (void*) this));
    
    /*
    outputPortAudioError(Pa_OpenDefaultStream(&_stream,
                                              2,
                                              2,
                                              (paInt16 | paNonInterleaved),
                                              SAMPLE_RATE,
                                              FRAMES_PER_BUFFER,
                                              audioCallback,
                                              (void*) this));
     */
    
    if (! _stream) {
        return;
    }
 
    _echoSamplesLeft = new int16_t[AEC_BUFFERED_SAMPLES + AEC_TMP_BUFFER_SIZE];
    if (! _echoSamplesLeft) {
        return;
    }
    memset(_echoSamplesLeft, 0, AEC_BUFFERED_SAMPLES * sizeof(int16_t));
    _echoSamplesRight = _echoSamplesLeft + AEC_BUFFERED_SAMPLES_PER_CHANNEL;
    _speexTmpBuf = _echoSamplesRight + AEC_BUFFERED_SAMPLES_PER_CHANNEL;

    _speexPreprocessState = speex_preprocess_state_init(BUFFER_LENGTH_SAMPLES_PER_CHANNEL, SAMPLE_RATE);
    if (_speexPreprocessState) {
        _speexEchoState = speex_echo_state_init_mc(BUFFER_LENGTH_SAMPLES_PER_CHANNEL, 
                                                   AEC_FILTER_LENGTH, AEC_N_CHANNELS_MIC, AEC_N_CHANNELS_PLAY);
        if (_speexEchoState) {
            speex_preprocess_ctl(_speexPreprocessState, SPEEX_PREPROCESS_SET_ECHO_STATE, _speexEchoState);
            int tmp;
            speex_echo_ctl(_speexEchoState, SPEEX_ECHO_SET_SAMPLING_RATE, &(tmp = SAMPLE_RATE));
            tmp = AEC_NOISE_REDUCTION;
            speex_preprocess_ctl(_speexPreprocessState, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &tmp);
            tmp = AEC_RESIDUAL_ECHO_REDUCTION;
            speex_preprocess_ctl(_speexPreprocessState, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &tmp);
            tmp = AEC_RESIDUAL_ECHO_REDUCTION_ACTIVE;
            speex_preprocess_ctl(_speexPreprocessState, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &tmp);
            speex_preprocess_ctl(_speexPreprocessState, SPEEX_PREPROCESS_SET_AGC, &(tmp = int(AEC_USE_AGC)));
            speex_preprocess_ctl(_speexPreprocessState, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &(tmp = AEC_AGC_MAX_GAIN));
            speex_preprocess_ctl(_speexPreprocessState, SPEEX_PREPROCESS_SET_AGC_TARGET, &(tmp = AEC_AGC_TARGET_LEVEL));
            speex_preprocess_ctl(_speexPreprocessState, SPEEX_PREPROCESS_SET_AGC_INCREMENT, &(tmp = AEC_AGC_MAX_INC));
            speex_preprocess_ctl(_speexPreprocessState, SPEEX_PREPROCESS_SET_AGC_DECREMENT, &(tmp = AEC_AGC_MAX_DEC));
            speex_preprocess_ctl(_speexPreprocessState, SPEEX_PREPROCESS_SET_VAD, &(tmp = int(AEC_USE_VAD)));
        } else {
            speex_preprocess_state_destroy(_speexPreprocessState);
            _speexPreprocessState = NULL;
        }
    }

    // start the stream now that sources are good to go
    outputPortAudioError(Pa_StartStream(_stream));
    printLog("Default low input, output latency (secs): %0.4f, %0.4f\n",
             Pa_GetDeviceInfo(Pa_GetDefaultInputDevice())->defaultLowInputLatency,
             Pa_GetDeviceInfo(Pa_GetDefaultOutputDevice())->defaultLowOutputLatency);
    
    const PaStreamInfo* streamInfo = Pa_GetStreamInfo(_stream);
    printLog("Audio started, msecs latency In: %.0f, Out: %.0f\n", streamInfo->inputLatency * 1000.f,
             streamInfo->outputLatency * 1000.f);


    gettimeofday(&_lastReceiveTime, NULL);
}

Audio::~Audio() {    
    if (_stream) {
        outputPortAudioError(Pa_CloseStream(_stream));
        outputPortAudioError(Pa_Terminate());
    }
    if (_speexEchoState) {
        speex_preprocess_state_destroy(_speexPreprocessState);
        speex_echo_state_destroy(_speexEchoState);
    }
    delete[] _echoSamplesLeft;
}

void Audio::addReceivedAudioToBuffer(unsigned char* receivedData, int receivedBytes) {
    const int NUM_INITIAL_PACKETS_DISCARD = 3;
    const int STANDARD_DEVIATION_SAMPLE_COUNT = 500;
    
    timeval currentReceiveTime;
    gettimeofday(&currentReceiveTime, NULL);
    _totalPacketsReceived++;
    
    double timeDiff = diffclock(&_lastReceiveTime, &currentReceiveTime);
    
    //  Discard first few received packets for computing jitter (often they pile up on start)
    if (_totalPacketsReceived > NUM_INITIAL_PACKETS_DISCARD) {
        _stdev.addValue(timeDiff);
    }
    
    if (_stdev.getSamples() > STANDARD_DEVIATION_SAMPLE_COUNT) {
        _measuredJitter = _stdev.getStDev();
        _stdev.reset();
        //  Set jitter buffer to be a multiple of the measured standard deviation
        const int MAX_JITTER_BUFFER_SAMPLES = RING_BUFFER_LENGTH_SAMPLES / 2;
        const float NUM_STANDARD_DEVIATIONS = 3.f;
        if (Application::getInstance()->shouldDynamicallySetJitterBuffer()) {
            float newJitterBufferSamples = (NUM_STANDARD_DEVIATIONS * _measuredJitter)
                                            / 1000.f
                                            * SAMPLE_RATE;
            setJitterBufferSamples(glm::clamp((int)newJitterBufferSamples, 0, MAX_JITTER_BUFFER_SAMPLES));
        }
    }
    
    if (!_ringBuffer.isStarted()) {
        _packetsReceivedThisPlayback++;
    }
    
    if (_packetsReceivedThisPlayback == 1) {
        gettimeofday(&_firstPacketReceivedTime, NULL);
    }
    
    //printf("Got audio packet %d\n", _packetsReceivedThisPlayback);
    _ringBuffer.parseData((unsigned char*) receivedData, PACKET_LENGTH_BYTES + sizeof(PACKET_HEADER));
    
    _lastReceiveTime = currentReceiveTime;
}

void Audio::render(int screenWidth, int screenHeight) {
    if (_stream) {
        glLineWidth(2.0);
        glBegin(GL_LINES);
        glColor3f(1,1,1);
        
        int startX = 20.0;
        int currentX = startX;
        int topY = screenHeight - 40;
        int bottomY = screenHeight - 20;
        float frameWidth = 20.0;
        float halfY = topY + ((bottomY - topY) / 2.0);
        
        // draw the lines for the base of the ring buffer
        
        glVertex2f(currentX, topY);
        glVertex2f(currentX, bottomY);
        
        for (int i = 0; i < RING_BUFFER_LENGTH_FRAMES / 2; i++) {
            glVertex2f(currentX, halfY);
            glVertex2f(currentX + frameWidth, halfY);
            currentX += frameWidth;
            
            glVertex2f(currentX, topY);
            glVertex2f(currentX, bottomY);
        }
        glEnd();
        
        //  Show a bar with the amount of audio remaining in ring buffer beyond current playback
        float remainingBuffer = 0;
        timeval currentTime;
        gettimeofday(&currentTime, NULL);
        float timeLeftInCurrentBuffer = 0;
        if (_lastCallbackTime.tv_usec > 0) {
            timeLeftInCurrentBuffer = AUDIO_CALLBACK_MSECS - diffclock(&_lastCallbackTime, &currentTime);
        }
        
        if (_ringBuffer.getEndOfLastWrite() != NULL)
            remainingBuffer = _ringBuffer.diffLastWriteNextOutput() / PACKET_LENGTH_SAMPLES * AUDIO_CALLBACK_MSECS;
        
        if (_wasStarved == 0) {
            glColor3f(0, 1, 0);
        } else {
            glColor3f(0.5 + (_wasStarved / 20.0f), 0, 0);
            _wasStarved--;
        }
        
        glBegin(GL_QUADS);
        glVertex2f(startX, topY + 2);
        glVertex2f(startX + (remainingBuffer + timeLeftInCurrentBuffer)/AUDIO_CALLBACK_MSECS*frameWidth, topY + 2);
        glVertex2f(startX + (remainingBuffer + timeLeftInCurrentBuffer)/AUDIO_CALLBACK_MSECS*frameWidth, bottomY - 2);
        glVertex2f(startX, bottomY - 2);
        glEnd();
        
        if (_averagedLatency == 0.0) {
            _averagedLatency = remainingBuffer + timeLeftInCurrentBuffer;
        } else {
            _averagedLatency = 0.99f * _averagedLatency + 0.01f * (remainingBuffer + timeLeftInCurrentBuffer);
        }
        
        //  Show a yellow bar with the averaged msecs latency you are hearing (from time of packet receipt)
        glColor3f(1,1,0);
        glBegin(GL_QUADS);
        glVertex2f(startX + _averagedLatency / AUDIO_CALLBACK_MSECS * frameWidth - 2, topY - 2);
        glVertex2f(startX + _averagedLatency / AUDIO_CALLBACK_MSECS * frameWidth + 2, topY - 2);
        glVertex2f(startX + _averagedLatency / AUDIO_CALLBACK_MSECS * frameWidth + 2, bottomY + 2);
        glVertex2f(startX + _averagedLatency / AUDIO_CALLBACK_MSECS * frameWidth - 2, bottomY + 2);
        glEnd();
        
        char out[40];
        sprintf(out, "%3.0f\n", _averagedLatency);
        drawtext(startX + _averagedLatency / AUDIO_CALLBACK_MSECS * frameWidth - 10, topY - 9, 0.10, 0, 1, 0, out, 1,1,0);
        
        //  Show a red bar with the 'start' point of one frame plus the jitter buffer
        
        glColor3f(1, 0, 0);
        int jitterBufferPels = (1.f + (float)getJitterBufferSamples() / (float)PACKET_LENGTH_SAMPLES_PER_CHANNEL) * frameWidth;
        sprintf(out, "%.0f\n", getJitterBufferSamples() / SAMPLE_RATE * 1000.f);
        drawtext(startX + jitterBufferPels - 5, topY - 9, 0.10, 0, 1, 0, out, 1, 0, 0);
        sprintf(out, "j %.1f\n", _measuredJitter);
        if (Application::getInstance()->shouldDynamicallySetJitterBuffer()) {
            drawtext(startX + jitterBufferPels - 5, bottomY + 12, 0.10, 0, 1, 0, out, 1, 0, 0);
        } else {
            drawtext(startX, bottomY + 12, 0.10, 0, 1, 0, out, 1, 0, 0);
        }
    
        glBegin(GL_QUADS);
        glVertex2f(startX + jitterBufferPels - 2, topY - 2);
        glVertex2f(startX + jitterBufferPels + 2, topY - 2);
        glVertex2f(startX + jitterBufferPels + 2, bottomY + 2);
        glVertex2f(startX + jitterBufferPels - 2, bottomY + 2);
        glEnd();

    }
}

//
//  Very Simple LowPass filter which works by averaging a bunch of samples with a moving window
//
//#define lowpass 1
void Audio::lowPassFilter(int16_t* inputBuffer) {
    static int16_t outputBuffer[BUFFER_LENGTH_SAMPLES_PER_CHANNEL];
    for (int i = 2; i < BUFFER_LENGTH_SAMPLES_PER_CHANNEL - 2; i++) {
#ifdef lowpass
        outputBuffer[i] = (int16_t)(0.125f * (float)inputBuffer[i - 2] +
                            0.25f * (float)inputBuffer[i - 1] +
                            0.25f * (float)inputBuffer[i] +
                            0.25f * (float)inputBuffer[i + 1] +
                            0.125f * (float)inputBuffer[i + 2] );
#else
        outputBuffer[i] = (int16_t)(0.125f * -(float)inputBuffer[i - 2] +
                                    0.25f * -(float)inputBuffer[i - 1] +
                                    1.75f * (float)inputBuffer[i] +
                                    0.25f * -(float)inputBuffer[i + 1] +
                                    0.125f * -(float)inputBuffer[i + 2] );

#endif
    }
    outputBuffer[0] = inputBuffer[0];
    outputBuffer[1] = inputBuffer[1];
    outputBuffer[BUFFER_LENGTH_SAMPLES_PER_CHANNEL - 2] = inputBuffer[BUFFER_LENGTH_SAMPLES_PER_CHANNEL - 2];
    outputBuffer[BUFFER_LENGTH_SAMPLES_PER_CHANNEL - 1] = inputBuffer[BUFFER_LENGTH_SAMPLES_PER_CHANNEL - 1];
    memcpy(inputBuffer, outputBuffer, BUFFER_LENGTH_SAMPLES_PER_CHANNEL * sizeof(int16_t));
}

//  Take a pointer to the acquired microphone input samples and add procedural sounds
void Audio::addProceduralSounds(int16_t* inputBuffer, int numSamples) {
    const float MAX_AUDIBLE_VELOCITY = 6.0;
    const float MIN_AUDIBLE_VELOCITY = 0.1;
    const int VOLUME_BASELINE = 400;
    const float SOUND_PITCH = 8.f;
    
    float speed = glm::length(_lastVelocity);
    float volume = VOLUME_BASELINE * (1.f - speed / MAX_AUDIBLE_VELOCITY);
    
    //  Add a noise-modulated sinewave with volume that tapers off with speed increasing
    if ((speed > MIN_AUDIBLE_VELOCITY) && (speed < MAX_AUDIBLE_VELOCITY)) {
        for (int i = 0; i < numSamples; i++) {
            inputBuffer[i] += (int16_t)((sinf((float) i / SOUND_PITCH * speed) * randFloat()) * volume * speed);
        }
    }
}

// -----------------------------
// Speex-based echo cancellation
// -----------------------------

bool Audio::isCancellingEcho() const {
    return _isCancellingEcho && ! (_pingFramesToRecord != 0 || _pingAnalysisPending || ! _speexPreprocessState);
}

void Audio::setIsCancellingEcho(bool enable) {
    if (enable && _speexPreprocessState) {
        speex_echo_state_reset(_speexEchoState);
        _echoWritePos = 0;
        memset(_echoSamplesLeft, 0, AEC_BUFFERED_SAMPLES * sizeof(int16_t));
    }
    _isCancellingEcho = enable;
}

inline void Audio::eventuallyCancelEcho(int16_t* inputLeft) {
    if (! isCancellingEcho()) {
        return;
    }

    // Construct an artificial frame from the captured playback
    // that contains the appropriately delayed output to cancel
    unsigned n = BUFFER_LENGTH_SAMPLES_PER_CHANNEL, n2 = 0;
    unsigned readPos = (_echoWritePos + AEC_BUFFERED_SAMPLES_PER_CHANNEL - _echoDelay) % AEC_BUFFERED_SAMPLES_PER_CHANNEL;
    unsigned readEnd = readPos + n;
    if (readEnd >= AEC_BUFFERED_SAMPLES_PER_CHANNEL) {
        n2 = (readEnd -= AEC_BUFFERED_SAMPLES_PER_CHANNEL);
        n -= n2;
    }
    // Use two subsequent buffers for the two stereo channels
    int16_t* playBufferLeft = _speexTmpBuf + BUFFER_LENGTH_SAMPLES_PER_CHANNEL;
    memcpy(playBufferLeft, _echoSamplesLeft + readPos, n * sizeof(int16_t));
    memcpy(playBufferLeft + n, _echoSamplesLeft, n2 * sizeof(int16_t));
    int16_t* playBufferRight = playBufferLeft + BUFFER_LENGTH_SAMPLES_PER_CHANNEL;
    memcpy(playBufferRight, _echoSamplesRight + readPos, n * sizeof(int16_t));
    memcpy(playBufferRight + n, _echoSamplesLeft, n2 * sizeof(int16_t));

#ifdef VISUALIZE_ECHO_CANCELLATION
    // Visualize the input
    _scope->addSamples(0, inputLeft, BUFFER_LENGTH_SAMPLES_PER_CHANNEL);
    _scope->addSamples(1, playBufferLeft, BUFFER_LENGTH_SAMPLES_PER_CHANNEL);
#endif

    // Have Speex perform echo cancellation
    speex_echo_cancellation(_speexEchoState, inputLeft, playBufferLeft, _speexTmpBuf);
    memcpy(inputLeft, _speexTmpBuf, BUFFER_LENGTH_BYTES_PER_CHANNEL);
    speex_preprocess_run(_speexPreprocessState, inputLeft); 

#ifdef VISUALIZE_ECHO_CANCELLATION
    // Visualize the result
    _scope->setColor(2, 0x00ff00);
    _scope->addSamples(2, inputLeft, BUFFER_LENGTH_SAMPLES_PER_CHANNEL);
#endif
}

inline void Audio::eventuallyRecordEcho(int16_t* outputLeft, int16_t* outputRight) {
    if (! isCancellingEcho()) {
        return;
    }

    // Copy playback data to circular buffers
    unsigned n = BUFFER_LENGTH_SAMPLES_PER_CHANNEL, n2 = 0;
    unsigned writeEnd = _echoWritePos + n;
    if (writeEnd >= AEC_BUFFERED_SAMPLES_PER_CHANNEL) {
        n2 = (writeEnd -= AEC_BUFFERED_SAMPLES_PER_CHANNEL);
        n -= n2;
    }
    memcpy(_echoSamplesLeft + _echoWritePos, outputLeft, n * sizeof(int16_t));
    memcpy(_echoSamplesLeft, outputLeft + n, n2 * sizeof(int16_t));
    memcpy(_echoSamplesRight + _echoWritePos, outputRight, n * sizeof(int16_t));
    memcpy(_echoSamplesRight, outputRight + n, n2 * sizeof(int16_t));

    _echoWritePos = writeEnd;
}

// -----------------------------------------------------------
// Accoustic ping (audio system round trip time determination)
// -----------------------------------------------------------

void Audio::ping() {

    _pingFramesToRecord = PING_FRAMES_TO_RECORD;
    _isSendingEchoPing = true;
    _scope->setDownsampleRatio(8); 
    _scope->inputPaused = false; 
}

inline void Audio::eventuallySendRecvPing(int16_t* inputLeft, int16_t* outputLeft, int16_t* outputRight) {

    if (_isSendingEchoPing) {

        // Overwrite output with ping signal.
        //
        // Using a signed variant of sinc because it's speaker-reproducible
        // with a unique, characteristic point in time (its center), aligned
        // to the right of the output buffer.
        //        
        //                       |  
        //                   |   |  
        // ...--- t --------+-+-+-+-+------->
        //                     |   | :
        //                     |     :
        // buffer                    :<- start of next buffer
        //                   : : :
        //                   :---: sine period
        //                   :-:   half sine period
        //
        memset(outputLeft, 0, PING_BUFFER_OFFSET * sizeof(int16_t));
        outputLeft += PING_BUFFER_OFFSET;
        memset(outputRight, 0, PING_BUFFER_OFFSET * sizeof(int16_t));
        outputRight += PING_BUFFER_OFFSET;
        for (int s = -PING_PERIOD; s < PING_PERIOD; ++s) {
            float t = float(s) / PING_PITCH;
            *outputLeft++ = *outputRight++ = int16_t(PING_VOLUME * 
                    sinf(t) / fmaxf(1.0f, pow((abs(t)-1.5f) / 1.5f, 1.2f)));
        }

        // As of the next frame, we'll be recoding PING_FRAMES_TO_RECORD from 
        // the mic (pointless to start now as we can't record unsent audio).
        _isSendingEchoPing = false;
        printLog("Send audio ping\n");

    } else if (_pingFramesToRecord > 0) {

        // Store input samples
        int offset = BUFFER_LENGTH_SAMPLES_PER_CHANNEL * (
                PING_FRAMES_TO_RECORD - _pingFramesToRecord);
        memcpy(_echoSamplesLeft + offset, 
               inputLeft, BUFFER_LENGTH_SAMPLES_PER_CHANNEL * sizeof(int16_t));

        --_pingFramesToRecord;

        if (_pingFramesToRecord == 0) {
            _pingAnalysisPending = true;
            printLog("Received ping echo\n");
        }
    }
}

static int findExtremum(int16_t const* samples, int length, int sign) {

    int x0 = -1;
    int y0 = -PING_VOLUME;
    for (int x = 0; x < length; ++samples, ++x) {
        int y = *samples * sign;
        if (y > y0) {
            x0 = x;
            y0 = y;
        }
    }
    return x0;
}

inline void Audio::analyzePing() {

    // Determine extrema
    int botAt = findExtremum(_echoSamplesLeft, PING_SAMPLES_TO_ANALYZE, -1);
    if (botAt == -1) {
        printLog("Audio Ping: Minimum not found.\n");
        return;
    }
    int topAt = findExtremum(_echoSamplesLeft, PING_SAMPLES_TO_ANALYZE, 1);
    if (topAt == -1) {
        printLog("Audio Ping: Maximum not found.\n");
        return;
    }

    // Determine peak amplitude - warn if low
    int ampli = (_echoSamplesLeft[topAt] - _echoSamplesLeft[botAt]) / 2;
    if (ampli < PING_MIN_AMPLI) {
        printLog("Audio Ping unreliable - low amplitude %d.\n", ampli);
    }

    // Determine period - warn if doesn't look like our signal
    int halfPeriod = abs(topAt - botAt);
    if (abs(halfPeriod-PING_HALF_PERIOD) > PING_MAX_PERIOD_DIFFERENCE) {
        printLog("Audio Ping unreliable - peak distance %d vs. %d\n", halfPeriod, PING_HALF_PERIOD);
    }

    // Ping is sent:
    //
    // ---[ record ]--[  play  ]--- audio in space/time --->
    //    :        :           :
    //    :        : ping: ->X<-
    //    :        :         :
    //    :        :         |+| (buffer end - signal center = t1-t0)
    //    :      |<----------+
    //    :      : :         :
    //    :    ->X<- (corresponding input buffer position t0)
    //    :      : :         :
    //    :      : :         :
    //    :      : :         :
    // Next frame (we're recording from now on):
    //    :      : :
    //    : -  - --[ record ]--[  play  ]------------------>
    //    :      : :         :
    //    :      : |<-- (start of recording t1)
    //    :      : :
    //    :      : :
    // At some frame, the signal is picked up:
    //    :      : :         :
    //    :      : :         : 
    //    :      : :         V
    //    :      : : -  - --[ record ]--[  play  ]---------->
    //    :      V :         :
    //    :      |<--------->|
    //           |+|<------->|  period + measured samples
    //
    // If we could pick up the signal at t0 we'd have zero round trip
    // time - in this case we had recorded the output buffer instantly
    // in its entirety (we can't - but there's the proper reference
    // point). We know the number of samples from t1 and, knowing that
    // data is streaming continuously, we know that t1-t0 is the distance
    // of the characterisic point from the end of the buffer.

    int delay = (botAt + topAt) / 2 + PING_PERIOD;

    printLog("\n| Audio Ping results:\n+----- ---- --- - -  -   -   -\n\n"
             "Delay = %d samples (%d ms)\nPeak amplitude = %d\n\n",
             delay, (delay * 1000) / int(SAMPLE_RATE), ampli);
}

bool Audio::eventuallyAnalyzePing() {

    if (! _pingAnalysisPending) {
        return false;
    }
    _scope->inputPaused = true;
    analyzePing();
    setIsCancellingEcho(_isCancellingEcho);
    _pingAnalysisPending = false;
    return true;
}

#endif

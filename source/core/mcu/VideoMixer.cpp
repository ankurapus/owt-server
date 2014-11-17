/*
 * Copyright 2014 Intel Corporation All Rights Reserved. 
 * 
 * The source code contained or described herein and all documents related to the 
 * source code ("Material") are owned by Intel Corporation or its suppliers or 
 * licensors. Title to the Material remains with Intel Corporation or its suppliers 
 * and licensors. The Material contains trade secrets and proprietary and 
 * confidential information of Intel or its suppliers and licensors. The Material 
 * is protected by worldwide copyright and trade secret laws and treaty provisions. 
 * No part of the Material may be used, copied, reproduced, modified, published, 
 * uploaded, posted, transmitted, distributed, or disclosed in any way without 
 * Intel's prior express written permission.
 * 
 * No license under any patent, copyright, trade secret or other intellectual 
 * property right is granted to or conferred upon you by disclosure or delivery of 
 * the Materials, either expressly, by implication, inducement, estoppel or 
 * otherwise. Any license under such intellectual property rights must be express 
 * and approved by Intel in writing.
 */

#include "VideoMixer.h"

#include "BufferManager.h"
#include "TaskRunner.h"
#include "VCMInputProcessor.h"
#include "VCMOutputProcessor.h"
#include <WoogeenTransport.h>
#include <webrtc/system_wrappers/interface/trace.h>

using namespace webrtc;
using namespace woogeen_base;
using namespace erizo;

namespace mcu {

DEFINE_LOGGER(VideoMixer, "mcu.VideoMixer");

VideoMixer::VideoMixer(erizo::RTPDataReceiver* receiver)
    : m_participants(0)
    , m_outputReceiver(receiver)
{
    init();
}

VideoMixer::~VideoMixer()
{
    closeAll();
    m_outputReceiver = nullptr;
}

/**
 * init could be used for reset the state of this VideoMixer
 */
bool VideoMixer::init()
{
    Trace::CreateTrace();
    Trace::SetTraceFile("webrtc.trace.txt");
    Trace::set_level_filter(webrtc::kTraceAll);

    m_bufferManager.reset(new BufferManager());
    m_taskRunner.reset(new TaskRunner());

    m_vcmOutputProcessor.reset(new VCMOutputProcessor(MIXED_VIDEO_STREAM_ID));
    m_vcmOutputProcessor->init(new WoogeenTransport<erizo::VIDEO>(m_outputReceiver, nullptr), m_bufferManager, m_taskRunner);

    m_taskRunner->Start();

    return true;
}

int VideoMixer::deliverAudioData(char* buf, int len) 
{
    assert(false);
    return 0;
}

#define global_ns

/**
 * Multiple sources may call this method simultaneously from different threads.
 * the incoming buffer is a rtp packet
 */
int VideoMixer::deliverVideoData(char* buf, int len)
{
    uint32_t id = 0;
    RTCPHeader* chead = reinterpret_cast<RTCPHeader*>(buf);
    uint8_t packetType = chead->getPacketType();
    assert(packetType != RTCP_Receiver_PT && packetType != RTCP_PS_Feedback_PT && packetType != RTCP_RTP_Feedback_PT);
    if (packetType == RTCP_Sender_PT)
        id = chead->getSSRC();
    else {
        global_ns::RTPHeader* head = reinterpret_cast<global_ns::RTPHeader*>(buf);
        id = head->getSSRC();
    }

    boost::shared_lock<boost::shared_mutex> lock(m_sourceMutex);
    std::map<uint32_t, boost::shared_ptr<VCMInputProcessor>>::iterator it = m_sinksForSources.find(id);
    if (it != m_sinksForSources.end() && it->second)
        return it->second->deliverVideoData(buf, len);

    return 0;
}

int VideoMixer::deliverFeedback(char* buf, int len)
{
    return m_vcmOutputProcessor->deliverFeedback(buf, len);
}

/**
 * Attach a new InputStream to the mixer
 */
int32_t VideoMixer::addSource(uint32_t from, bool isAudio, FeedbackSink* feedback)
{
    assert(!isAudio);

    if (m_participants == BufferManager::SLOT_SIZE) {
        ELOG_WARN("Exceeding maximum number of sources (%u), ignoring the addSource request", BufferManager::SLOT_SIZE);
        return -1;
    }

    boost::upgrade_lock<boost::shared_mutex> lock(m_sourceMutex);
    std::map<uint32_t, boost::shared_ptr<VCMInputProcessor>>::iterator it = m_sinksForSources.find(from);
    if (it == m_sinksForSources.end() || !it->second) {
        int index = assignSlot(from);
        ELOG_DEBUG("addSource - assigned slot is %d", index);
        m_bufferManager->setActive(index, true);
        m_vcmOutputProcessor->updateMaxSlot(++m_participants);

        VCMInputProcessor* videoInputProcessor(new VCMInputProcessor(index));
        videoInputProcessor->init(new WoogeenTransport<erizo::VIDEO>(nullptr, feedback),
        							m_vcmOutputProcessor,
        							m_taskRunner);

        boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
        m_sinksForSources[from].reset(videoInputProcessor);
        return 0;
    }

    assert("new source added with InputProcessor still available");    // should not go there
    return -1;
}

int32_t VideoMixer::bindAudio(uint32_t id, int voiceChannelId, VoEVideoSync* voeVideoSync)
{
    boost::shared_lock<boost::shared_mutex> lock(m_sourceMutex);
    std::map<uint32_t, boost::shared_ptr<VCMInputProcessor>>::iterator it = m_sinksForSources.find(id);
    if (it != m_sinksForSources.end() && it->second) {
        it->second->bindAudioForSync(voiceChannelId, voeVideoSync);
        return 0;
    }
    return -1;
}

int32_t VideoMixer::removeSource(uint32_t from, bool isAudio)
{
    assert(!isAudio);

    boost::unique_lock<boost::shared_mutex> lock(m_sourceMutex);
    std::map<uint32_t, boost::shared_ptr<VCMInputProcessor>>::iterator it = m_sinksForSources.find(from);
    if (it != m_sinksForSources.end()) {
        m_sinksForSources.erase(it);
        lock.unlock();

        int index = getSlot(from);
        assert(index >= 0);
        m_sourceSlotMap[index] = 0;
        m_bufferManager->setActive(index, false);
        m_vcmOutputProcessor->updateMaxSlot(--m_participants);
        return 0;
    }

    return -1;
}

void VideoMixer::onRequestIFrame()
{
    ELOG_DEBUG("onRequestIFrame");
    m_vcmOutputProcessor->onRequestIFrame();
}

uint32_t VideoMixer::sendSSRC()
{
    return m_vcmOutputProcessor->sendSSRC();
}

void VideoMixer::closeAll()
{
    ELOG_DEBUG("closeAll");
    m_taskRunner->Stop();

    boost::unique_lock<boost::shared_mutex> sourceLock(m_sourceMutex);
    std::map<uint32_t, boost::shared_ptr<VCMInputProcessor>>::iterator sourceItor = m_sinksForSources.begin();
    while (sourceItor != m_sinksForSources.end()) {
        uint32_t source = sourceItor->first;
        m_sinksForSources.erase(sourceItor++);
        int index = getSlot(source);
        assert(index >= 0);
        m_sourceSlotMap[index] = 0;
        m_bufferManager->setActive(index, false);
    }
    m_sinksForSources.clear();
    m_participants = 0;
    m_vcmOutputProcessor->updateMaxSlot(0);

    ELOG_DEBUG("Closed all media in this Mixer");
    Trace::ReturnTrace();
}

}/* namespace mcu */

/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "C2SoftAmrWbEnc"
#include <log/log.h>

#include <media/stagefright/foundation/MediaDefs.h>

#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>

#include "C2SoftAmrWbEnc.h"
#include "cmnMemory.h"

namespace android {

constexpr char COMPONENT_NAME[] = "c2.android.amrwb.encoder";

class C2SoftAmrWbEnc::IntfImpl : public C2InterfaceHelper {
   public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper>& helper)
        : C2InterfaceHelper(helper) {
        setDerivedInstance(this);

        addParameter(
            DefineParam(mInputFormat, C2_NAME_INPUT_STREAM_FORMAT_SETTING)
                .withConstValue(
                    new C2StreamFormatConfig::input(0u, C2FormatAudio))
                .build());

        addParameter(
            DefineParam(mOutputFormat, C2_NAME_OUTPUT_STREAM_FORMAT_SETTING)
                .withConstValue(
                    new C2StreamFormatConfig::output(0u, C2FormatCompressed))
                .build());

        addParameter(
            DefineParam(mInputMediaType, C2_NAME_INPUT_PORT_MIME_SETTING)
                .withConstValue(AllocSharedString<C2PortMimeConfig::input>(
                    MEDIA_MIMETYPE_AUDIO_RAW))
                .build());

        addParameter(
            DefineParam(mOutputMediaType, C2_NAME_OUTPUT_PORT_MIME_SETTING)
                .withConstValue(AllocSharedString<C2PortMimeConfig::output>(
                    MEDIA_MIMETYPE_AUDIO_AMR_WB))
                .build());

        addParameter(
                DefineParam(mChannelCount, C2_NAME_STREAM_CHANNEL_COUNT_SETTING)
                .withDefault(new C2StreamChannelCountInfo::input(0u, 1))
                .withFields({C2F(mChannelCount, value).equalTo(1)})
                .withSetter((Setter<decltype(*mChannelCount)>::StrictValueWithNoDeps))
                .build());

        addParameter(
            DefineParam(mSampleRate, C2_NAME_STREAM_SAMPLE_RATE_SETTING)
                .withDefault(new C2StreamSampleRateInfo::input(0u, 16000))
                .withFields({C2F(mSampleRate, value).equalTo(16000)})
                .withSetter(
                    (Setter<decltype(*mSampleRate)>::StrictValueWithNoDeps))
                .build());

        addParameter(
                DefineParam(mBitrate, C2_NAME_STREAM_BITRATE_SETTING)
                .withDefault(new C2BitrateTuning::output(0u, 6600))
                .withFields({C2F(mBitrate, value).inRange(6600, 23850)})
                .withSetter(Setter<decltype(*mBitrate)>::NonStrictValueWithNoDeps)
                .build());
    }

    uint32_t getSampleRate() const { return mSampleRate->value; }
    uint32_t getChannelCount() const { return mChannelCount->value; }
    uint32_t getBitrate() const { return mBitrate->value; }

   private:
    std::shared_ptr<C2StreamFormatConfig::input> mInputFormat;
    std::shared_ptr<C2StreamFormatConfig::output> mOutputFormat;
    std::shared_ptr<C2PortMimeConfig::input> mInputMediaType;
    std::shared_ptr<C2PortMimeConfig::output> mOutputMediaType;
    std::shared_ptr<C2StreamSampleRateInfo::input> mSampleRate;
    std::shared_ptr<C2StreamChannelCountInfo::input> mChannelCount;
    std::shared_ptr<C2BitrateTuning::output> mBitrate;
};

C2SoftAmrWbEnc::C2SoftAmrWbEnc(const char* name, c2_node_id_t id,
                               const std::shared_ptr<IntfImpl>& intfImpl)
    : SimpleC2Component(
          std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
      mEncoderHandle(nullptr),
      mApiHandle(nullptr),
      mMemOperator(nullptr) {
}

C2SoftAmrWbEnc::~C2SoftAmrWbEnc() {
    onRelease();
}

c2_status_t C2SoftAmrWbEnc::onInit() {
    mMode = VOAMRWB_MD2305;
    status_t err = initEncoder();
    mIsFirst = true;
    mSignalledError = false;
    mSignalledOutputEos = false;
    mAnchorTimeStamp = 0;
    mProcessedSamples = 0;
    mFilledLen = 0;

    return err == OK ? C2_OK : C2_NO_MEMORY;
}

void C2SoftAmrWbEnc::onRelease() {
    if (mEncoderHandle) {
        CHECK_EQ((VO_U32)VO_ERR_NONE, mApiHandle->Uninit(mEncoderHandle));
        mEncoderHandle = nullptr;
    }
    if (mApiHandle) {
        delete mApiHandle;
        mApiHandle = nullptr;
    }
    if (mMemOperator) {
        delete mMemOperator;
        mMemOperator = nullptr;
    }
}

c2_status_t C2SoftAmrWbEnc::onStop() {
    for (int i = 0; i < kNumSamplesPerFrame; i++) {
        mInputFrame[i] = 0x0008; /* EHF_MASK */
    }
    uint8_t outBuffer[kNumBytesPerInputFrame];
    (void) encodeInput(outBuffer, kNumBytesPerInputFrame);
    mIsFirst = true;
    mSignalledError = false;
    mSignalledOutputEos = false;
    mAnchorTimeStamp = 0;
    mProcessedSamples = 0;
    mFilledLen = 0;

    return C2_OK;
}

void C2SoftAmrWbEnc::onReset() {
    (void) onStop();
}

c2_status_t C2SoftAmrWbEnc::onFlush_sm() {
    return onStop();
}

status_t C2SoftAmrWbEnc::initEncoder() {
    mApiHandle = new VO_AUDIO_CODECAPI;
    if (!mApiHandle) return NO_MEMORY;

    if (VO_ERR_NONE != voGetAMRWBEncAPI(mApiHandle)) {
        ALOGE("Failed to get api handle");
        return UNKNOWN_ERROR;
    }

    mMemOperator = new VO_MEM_OPERATOR;
    if (!mMemOperator) return NO_MEMORY;

    mMemOperator->Alloc = cmnMemAlloc;
    mMemOperator->Copy = cmnMemCopy;
    mMemOperator->Free = cmnMemFree;
    mMemOperator->Set = cmnMemSet;
    mMemOperator->Check = cmnMemCheck;

    VO_CODEC_INIT_USERDATA userData;
    memset(&userData, 0, sizeof(userData));
    userData.memflag = VO_IMF_USERMEMOPERATOR;
    userData.memData = (VO_PTR) mMemOperator;

    if (VO_ERR_NONE != mApiHandle->Init(
                &mEncoderHandle, VO_AUDIO_CodingAMRWB, &userData)) {
        ALOGE("Failed to init AMRWB encoder");
        return UNKNOWN_ERROR;
    }

    VOAMRWBFRAMETYPE type = VOAMRWB_RFC3267;
    if (VO_ERR_NONE != mApiHandle->SetParam(
                mEncoderHandle, VO_PID_AMRWB_FRAMETYPE, &type)) {
        ALOGE("Failed to set AMRWB encoder frame type to %d", type);
        return UNKNOWN_ERROR;
    }

    if (VO_ERR_NONE !=
            mApiHandle->SetParam(
                    mEncoderHandle, VO_PID_AMRWB_MODE,  &mMode)) {
        ALOGE("Failed to set AMRWB encoder mode to %d", mMode);
        return UNKNOWN_ERROR;
    }

    return OK;
}

int C2SoftAmrWbEnc::encodeInput(uint8_t *buffer, uint32_t length) {
    VO_CODECBUFFER inputData;
    memset(&inputData, 0, sizeof(inputData));
    inputData.Buffer = (unsigned char *) mInputFrame;
    inputData.Length = kNumBytesPerInputFrame;

    CHECK_EQ((VO_U32)VO_ERR_NONE,
             mApiHandle->SetInputData(mEncoderHandle, &inputData));

    VO_AUDIO_OUTPUTINFO outputInfo;
    memset(&outputInfo, 0, sizeof(outputInfo));
    VO_CODECBUFFER outputData;
    memset(&outputData, 0, sizeof(outputData));
    outputData.Buffer = buffer;
    outputData.Length = length;
    VO_U32 ret = mApiHandle->GetOutputData(
            mEncoderHandle, &outputData, &outputInfo);
    if (ret != VO_ERR_NONE && ret != VO_ERR_INPUT_BUFFER_SMALL) {
        ALOGD("encountered error during encode call");
        return -1;
    }
    return outputData.Length;
}

static void fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    work->worklets.front()->output.flags = work->input.flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

void C2SoftAmrWbEnc::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    if (mSignalledError || mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }

    const C2ConstLinearBlock inBuffer = work->input.buffers[0]->data().linearBlocks().front();
    bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
    size_t inOffset = inBuffer.offset();
    size_t inSize = inBuffer.size();
    C2ReadView rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
    if (inSize && rView.error()) {
        ALOGE("read view map failed %d", rView.error());
        work->result = rView.error();
        return;
    }

    ALOGV("in buffer attr. size %zu timestamp %d frameindex %d, flags %x",
          inSize, (int)work->input.ordinal.timestamp.peeku(),
          (int)work->input.ordinal.frameIndex.peeku(), work->input.flags);

    size_t outCapacity = kNumBytesPerInputFrame;
    outCapacity += mFilledLen + inSize;
    std::shared_ptr<C2LinearBlock> outputBlock;
    C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
    c2_status_t err = pool->fetchLinearBlock(outCapacity, usage, &outputBlock);
    if (err != C2_OK) {
        ALOGE("fetchLinearBlock for Output failed with status %d", err);
        work->result = C2_NO_MEMORY;
        return;
    }
    C2WriteView wView = outputBlock->map().get();
    if (wView.error()) {
        ALOGE("write view map failed %d", wView.error());
        work->result = wView.error();
        return;
    }
    uint64_t outTimeStamp =
        mProcessedSamples * 1000000ll / mIntf->getSampleRate();
    const uint8_t *inPtr = rView.data() + inOffset;
    size_t inPos = 0;
    size_t outPos = 0;
    while (inPos < inSize) {
        int validSamples = mFilledLen / sizeof(int16_t);
        if ((inPos + (kNumBytesPerInputFrame - mFilledLen)) <= inSize) {
            memcpy(mInputFrame + validSamples, inPtr + inPos,
                   (kNumBytesPerInputFrame - mFilledLen));
            inPos += (kNumBytesPerInputFrame - mFilledLen);
        } else {
            memcpy(mInputFrame + validSamples, inPtr + inPos, (inSize - inPos));
            mFilledLen += (inSize - inPos);
            inPos += (inSize - inPos);
            if (eos) {
                validSamples = mFilledLen / sizeof(int16_t);
                memset(mInputFrame + validSamples, 0, (kNumBytesPerInputFrame - mFilledLen));
            } else break;
        }
        int numEncBytes = encodeInput((wView.data() + outPos), outCapacity - outPos);
        if (numEncBytes < 0) {
            ALOGE("encodeFrame call failed, state [%d %zu %zu]", numEncBytes, outPos, outCapacity);
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
        outPos += numEncBytes;
        mProcessedSamples += kNumSamplesPerFrame;
        mFilledLen = 0;
    }
    ALOGV("causal sample size %d", mFilledLen);
    if (mIsFirst) {
        mIsFirst = false;
        mAnchorTimeStamp = work->input.ordinal.timestamp.peekull();
    }
    fillEmptyWork(work);
    if (outPos != 0) {
        work->worklets.front()->output.buffers.push_back(
                createLinearBuffer(std::move(outputBlock), 0, outPos));
        work->worklets.front()->output.ordinal.timestamp = mAnchorTimeStamp + outTimeStamp;
    }
    if (eos) {
        mSignalledOutputEos = true;
        ALOGV("signalled EOS");
        if (mFilledLen) ALOGV("Discarding trailing %d bytes", mFilledLen);
    }
}

c2_status_t C2SoftAmrWbEnc::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    (void) pool;
    if (drainMode == NO_DRAIN) {
        ALOGW("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    onFlush_sm();
    return C2_OK;
}

class C2SoftAmrWbEncFactory : public C2ComponentFactory {
public:
    C2SoftAmrWbEncFactory()
        : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
              GetCodec2PlatformComponentStore()->getParamReflector())) {}

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        *component = std::shared_ptr<C2Component>(
            new C2SoftAmrWbEnc(
                COMPONENT_NAME, id,
                std::make_shared<C2SoftAmrWbEnc::IntfImpl>(mHelper)),
            deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
            new SimpleInterface<C2SoftAmrWbEnc::IntfImpl>(
                COMPONENT_NAME, id,
                std::make_shared<C2SoftAmrWbEnc::IntfImpl>(mHelper)),
            deleter);
        return C2_OK;
    }

    virtual ~C2SoftAmrWbEncFactory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

}  // namespace android

extern "C" ::C2ComponentFactory* CreateCodec2Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2SoftAmrWbEncFactory();
}

extern "C" void DestroyCodec2Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}

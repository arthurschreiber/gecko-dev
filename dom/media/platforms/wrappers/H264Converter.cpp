/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/TaskQueue.h"

#include "H264Converter.h"
#include "ImageContainer.h"
#include "MediaInfo.h"
#include "mp4_demuxer/AnnexB.h"
#include "mp4_demuxer/H264.h"

namespace mozilla
{

H264Converter::H264Converter(PlatformDecoderModule* aPDM,
                             const CreateDecoderParams& aParams)
  : mPDM(aPDM)
  , mOriginalConfig(aParams.VideoConfig())
  , mCurrentConfig(aParams.VideoConfig())
  , mLayersBackend(aParams.mLayersBackend)
  , mImageContainer(aParams.mImageContainer)
  , mTaskQueue(aParams.mTaskQueue)
  , mCallback(aParams.mCallback)
  , mDecoder(nullptr)
  , mGMPCrashHelper(aParams.mCrashHelper)
  , mNeedAVCC(aPDM->DecoderNeedsConversion(aParams.mConfig)
      == PlatformDecoderModule::ConversionRequired::kNeedAVCC)
  , mLastError(NS_OK)
{
  CreateDecoder(aParams.mDiagnostics);
}

H264Converter::~H264Converter()
{
}

RefPtr<MediaDataDecoder::InitPromise>
H264Converter::Init()
{
  if (mDecoder) {
    return mDecoder->Init();
  }

  // We haven't been able to initialize a decoder due to a missing SPS/PPS.
  return MediaDataDecoder::InitPromise::CreateAndResolve(
           TrackType::kVideoTrack, __func__);
}

nsresult
H264Converter::Input(MediaRawData* aSample)
{
  if (!mp4_demuxer::AnnexB::ConvertSampleToAVCC(aSample)) {
    // We need AVCC content to be able to later parse the SPS.
    // This is a no-op if the data is already AVCC.
    return NS_ERROR_FAILURE;
  }

  if (mInitPromiseRequest.Exists()) {
    if (mNeedKeyframe) {
      if (!aSample->mKeyframe) {
        // Frames dropped, we need a new one.
        mCallback->InputExhausted();
        return NS_OK;
      }
      mNeedKeyframe = false;
    }
    mMediaRawSamples.AppendElement(aSample);
    return NS_OK;
  }

  nsresult rv;
  if (!mDecoder) {
    // It is not possible to create an AVCC H264 decoder without SPS.
    // As such, creation will fail if the extra_data just extracted doesn't
    // contain a SPS.
    rv = CreateDecoderAndInit(aSample);
    if (rv == NS_ERROR_NOT_INITIALIZED) {
      // We are missing the required SPS to create the decoder.
      // Ignore for the time being, the MediaRawData will be dropped.
      mCallback->InputExhausted();
      return NS_OK;
    }
  } else {
    rv = CheckForSPSChange(aSample);
  }
  NS_ENSURE_SUCCESS(rv, rv);

  if (mNeedKeyframe && !aSample->mKeyframe) {
    mCallback->InputExhausted();
    return NS_OK;
  }

  if (!mNeedAVCC &&
      !mp4_demuxer::AnnexB::ConvertSampleToAnnexB(aSample)) {
    return NS_ERROR_FAILURE;
  }

  mNeedKeyframe = false;

  aSample->mExtraData = mCurrentConfig.mExtraData;

  return mDecoder->Input(aSample);
}

nsresult
H264Converter::Flush()
{
  mNeedKeyframe = true;
  if (mDecoder) {
    return mDecoder->Flush();
  }
  return mLastError;
}

nsresult
H264Converter::Drain()
{
  mNeedKeyframe = true;
  if (mDecoder) {
    return mDecoder->Drain();
  }
  mCallback->DrainComplete();
  return mLastError;
}

nsresult
H264Converter::Shutdown()
{
  if (mDecoder) {
    nsresult rv = mDecoder->Shutdown();
    mInitPromiseRequest.DisconnectIfExists();
    mDecoder = nullptr;
    return rv;
  }
  return NS_OK;
}

bool
H264Converter::IsHardwareAccelerated(nsACString& aFailureReason) const
{
  if (mDecoder) {
    return mDecoder->IsHardwareAccelerated(aFailureReason);
  }
  return MediaDataDecoder::IsHardwareAccelerated(aFailureReason);
}

nsresult
H264Converter::CreateDecoder(DecoderDoctorDiagnostics* aDiagnostics)
{
  if (mNeedAVCC && !mp4_demuxer::AnnexB::HasSPS(mCurrentConfig.mExtraData)) {
    // nothing found yet, will try again later
    return NS_ERROR_NOT_INITIALIZED;
  }
  UpdateConfigFromExtraData(mCurrentConfig.mExtraData);

  mp4_demuxer::SPSData spsdata;
  if (mp4_demuxer::H264::DecodeSPSFromExtraData(mCurrentConfig.mExtraData, spsdata)) {
    // Do some format check here.
    // WMF H.264 Video Decoder and Apple ATDecoder do not support YUV444 format.
    if (spsdata.chroma_format_idc == 3 /*YUV444*/) {
      mLastError = NS_ERROR_FAILURE;
      if (aDiagnostics) {
        aDiagnostics->SetVideoFormatNotSupport();
      }
      return NS_ERROR_FAILURE;
    }
  } else if (mNeedAVCC) {
    // SPS was invalid.
    mLastError = NS_ERROR_FAILURE;
    return NS_ERROR_FAILURE;
  }

  if (!mNeedAVCC) {
    // When using a decoder handling AnnexB, we get here only once from the
    // constructor. We do want to get the dimensions extracted from the SPS.
    mOriginalConfig = mCurrentConfig;
  }

  mDecoder = mPDM->CreateVideoDecoder({
    mNeedAVCC ? mCurrentConfig : mOriginalConfig,
    mTaskQueue,
    mCallback,
    aDiagnostics,
    mImageContainer,
    mLayersBackend,
    mGMPCrashHelper
  });

  if (!mDecoder) {
    mLastError = NS_ERROR_FAILURE;
    return NS_ERROR_FAILURE;
  }

  mNeedKeyframe = true;

  return NS_OK;
}

nsresult
H264Converter::CreateDecoderAndInit(MediaRawData* aSample)
{
  RefPtr<MediaByteBuffer> extra_data =
    mp4_demuxer::AnnexB::ExtractExtraData(aSample);
  if (!mp4_demuxer::AnnexB::HasSPS(extra_data)) {
    return NS_ERROR_NOT_INITIALIZED;
  }
  UpdateConfigFromExtraData(extra_data);

  nsresult rv = CreateDecoder(/* DecoderDoctorDiagnostics* */ nullptr);

  if (NS_SUCCEEDED(rv)) {
    // Queue the incoming sample.
    mMediaRawSamples.AppendElement(aSample);

    RefPtr<H264Converter> self = this;

    mInitPromiseRequest.Begin(mDecoder->Init()
      ->Then(AbstractThread::GetCurrent()->AsTaskQueue(), __func__, this,
             &H264Converter::OnDecoderInitDone,
             &H264Converter::OnDecoderInitFailed));
  }
  return rv;
}

void
H264Converter::OnDecoderInitDone(const TrackType aTrackType)
{
  mInitPromiseRequest.Complete();
  bool gotInput = false;
  for (uint32_t i = 0 ; i < mMediaRawSamples.Length(); i++) {
    const RefPtr<MediaRawData>& sample = mMediaRawSamples[i];
    if (mNeedKeyframe) {
      if (!sample->mKeyframe) {
        continue;
      }
      mNeedKeyframe = false;
    }
    if (NS_FAILED(mDecoder->Input(sample))) {
      mCallback->Error(MediaDataDecoderError::FATAL_ERROR);
    }
  }
  if (!gotInput) {
    mCallback->InputExhausted();
  }
  mMediaRawSamples.Clear();
}

void
H264Converter::OnDecoderInitFailed(MediaDataDecoder::DecoderFailureReason aReason)
{
  mInitPromiseRequest.Complete();
  mCallback->Error(MediaDataDecoderError::FATAL_ERROR);
}

nsresult
H264Converter::CheckForSPSChange(MediaRawData* aSample)
{
  RefPtr<MediaByteBuffer> extra_data =
    mp4_demuxer::AnnexB::ExtractExtraData(aSample);
  if (!mp4_demuxer::AnnexB::HasSPS(extra_data) ||
      mp4_demuxer::AnnexB::CompareExtraData(extra_data,
                                            mCurrentConfig.mExtraData)) {
        return NS_OK;
      }
  if (!mNeedAVCC) {
    UpdateConfigFromExtraData(extra_data);
    mDecoder->ConfigurationChanged(mCurrentConfig);
    return NS_OK;
  }
  // The SPS has changed, signal to flush the current decoder and create a
  // new one.
  mDecoder->Flush();
  Shutdown();
  return CreateDecoderAndInit(aSample);
}

void
H264Converter::UpdateConfigFromExtraData(MediaByteBuffer* aExtraData)
{
  mp4_demuxer::SPSData spsdata;
  if (mp4_demuxer::H264::DecodeSPSFromExtraData(aExtraData, spsdata) &&
      spsdata.pic_width > 0 && spsdata.pic_height > 0) {
    mp4_demuxer::H264::EnsureSPSIsSane(spsdata);
    mCurrentConfig.mImage.width = spsdata.pic_width;
    mCurrentConfig.mImage.height = spsdata.pic_height;
    mCurrentConfig.mDisplay.width = spsdata.display_width;
    mCurrentConfig.mDisplay.height = spsdata.display_height;
  }
  mCurrentConfig.mExtraData = aExtraData;
}

} // namespace mozilla

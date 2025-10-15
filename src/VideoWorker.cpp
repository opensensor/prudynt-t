#include "VideoWorker.hpp"

#include "Config.hpp"
#include "IMPEncoder.hpp"
#include "IMPFramesource.hpp"
#include "Logger.hpp"
#include "WorkerUtils.hpp"
#include "TimestampManager.hpp"
#include "globals.hpp"
#include <fstream>
#include <iterator>
#include <thread>

#undef MODULE
#define MODULE "VideoWorker"

VideoWorker::VideoWorker(int chn)
    : encChn(chn)
{
    LOG_DEBUG("VideoWorker created for channel " << encChn);
}

VideoWorker::~VideoWorker()
{
    LOG_DEBUG("VideoWorker destroyed for channel " << encChn);
}

void VideoWorker::run()
{
    LOG_DEBUG("Start video processing run loop for stream " << encChn);

    uint32_t bps = 0;
    uint32_t fps = 0;
    uint32_t error_count = 0; // Keep track of polling errors
    unsigned long long ms = 0;
    bool run_for_jpeg = false;

    LOG_DEBUG("VideoWorker run loop starting for channel " << encChn << ", idr=" << global_video[encChn]->idr);

    while (global_video[encChn]->running)
    {
        /* bool helper to check if this is the active jpeg channel and a jpeg is requested while
         * the channel is inactive
         */
        run_for_jpeg = (encChn == global_jpeg[0]->streamChn && global_video[encChn]->run_for_jpeg);

        /* now we need to verify that
         * 1. a client is connected (hasDataCallback)
         * 2. a jpeg is requested
         */
        int poll_result = IMP_Encoder_PollingStream(encChn, cfg->general.imp_polling_timeout);

        if (poll_result == 0)
        {
            LOG_DEBUG("IMP_Encoder_PollingStream(" << encChn << ") succeeded, getting stream");
            IMPEncoderStream stream;
            if (IMP_Encoder_GetStream(encChn, &stream, GET_STREAM_BLOCKING) != 0)
                {
                    LOG_ERROR("IMP_Encoder_GetStream(" << encChn << ") failed");
                    error_count++;
                    continue;
                }


                // SINGLE SOURCE OF TRUTH: Use TimestampManager (which uses IMP hardware timestamps)
                struct timeval monotonic_time;
                TimestampManager::getInstance().getTimestamp(&monotonic_time);

                // TIMESTAMP DEBUG: Log video frame processing (use first pack timestamp)
                int64_t pack_timestamp = (stream.packCount > 0) ? stream.pack[0].timestamp : -1;
                LOG_DEBUG("VIDEO_TIMESTAMP_1_PROCESS: pack_timestamp=" << pack_timestamp << " monotonic_time.tv_sec=" << monotonic_time.tv_sec << " monotonic_time.tv_usec=" << monotonic_time.tv_usec);

                for (uint32_t i = 0; i < stream.packCount; ++i)
                {
                    fps++;
                    bps += stream.pack[i].length;

#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
                    uint8_t *start = (uint8_t *) stream.virAddr + stream.pack[i].offset;
                    uint8_t *end = start + stream.pack[i].length;
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) \
    || defined(PLATFORM_T23) || defined(PLATFORM_T30)
                    uint8_t *start = (uint8_t *) stream.pack[i].virAddr;
                    uint8_t *end = (uint8_t *) stream.pack[i].virAddr + stream.pack[i].length;
#endif

                    // Check if this is an IDR-related NAL unit (SPS/PPS/VPS/IDR) BEFORE creating the NAL unit
                    // This allows us to set the idr flag and then write this very NAL unit
                    bool is_idr_nal = false;
                    if (global_video[encChn]->idr == false)
                    {
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
                        if (stream.pack[i].nalType.h264NalType == 7
                            || stream.pack[i].nalType.h264NalType == 8
                            || stream.pack[i].nalType.h264NalType == 5)
                        {
                            LOG_DEBUG("Channel " << encChn << ": Detected H.264 IDR-related NAL (type=" << (int)stream.pack[i].nalType.h264NalType << "), setting idr=true");
                            is_idr_nal = true;
                            global_video[encChn]->idr = true;
                        }
                        else if (stream.pack[i].nalType.h265NalType == 32)
                        {
                            LOG_DEBUG("Channel " << encChn << ": Detected H.265 VPS NAL (type=32), setting idr=true");
                            is_idr_nal = true;
                            global_video[encChn]->idr = true;
                        }
                        else
                        {
                            LOG_DEBUG("Channel " << encChn << ": NAL type h264=" << (int)stream.pack[i].nalType.h264NalType << " h265=" << (int)stream.pack[i].nalType.h265NalType << " (not IDR-related, idr still false)");
                        }
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) \
    || defined(PLATFORM_T23)
                        if (stream.pack[i].dataType.h264Type == 7
                            || stream.pack[i].dataType.h264Type == 8
                            || stream.pack[i].dataType.h264Type == 5)
                        {
                            is_idr_nal = true;
                            global_video[encChn]->idr = true;
                        }
#elif defined(PLATFORM_T30)
                        if (stream.pack[i].dataType.h264Type == 7
                            || stream.pack[i].dataType.h264Type == 8
                            || stream.pack[i].dataType.h264Type == 5)
                        {
                            is_idr_nal = true;
                            global_video[encChn]->idr = true;
                        }
                        else if (stream.pack[i].dataType.h265Type == 32)
                        {
                            is_idr_nal = true;
                            global_video[encChn]->idr = true;
                        }
#endif
                    }

                    // Only write NAL units after we've seen the first IDR frame
                    // This ensures clients receive a complete stream starting with SPS/PPS
                    if (global_video[encChn]->idr == true)
                    {
                        // Split the Annex B bytestream into individual NAL units (AUD/SEI/SPS/PPS/IDR/etc.)
                        const uint8_t *buf = start;
                        size_t len = static_cast<size_t>(end - start);

                        auto find_start_code = [](const uint8_t *b, size_t off, size_t n, int *sc_len) -> long {
                            for (size_t k = off; k + 3 < n; ++k) {
                                if (b[k] == 0x00 && b[k+1] == 0x00) {
                                    if (b[k+2] == 0x01) { if (sc_len) *sc_len = 3; return static_cast<ssize_t>(k); }
                                    if (k + 4 < n && b[k+2] == 0x00 && b[k+3] == 0x01) { if (sc_len) *sc_len = 4; return static_cast<ssize_t>(k); }
                                }
                            }
                            return -1;
                        };

                        int sc_len = 0;
                        long i0 = find_start_code(buf, 0, len, &sc_len);
                        if (i0 >= 0) {
                            long i = i0;
                            while (i >= 0 && static_cast<size_t>(i) < len) {
                                int sc_len_cur = 0;
                                // Start of current NAL payload (skip start code)
                                size_t nal_start = static_cast<size_t>(i) + static_cast<size_t>(sc_len);
                                // Find next start code to determine end
                                long i_next = find_start_code(buf, nal_start, len, &sc_len_cur);
                                size_t nal_end = (i_next >= 0) ? static_cast<size_t>(i_next) : len;
                                if (nal_end > nal_start) {
                                    H264NALUnit nalu;
                                    nalu.time = monotonic_time;
                                    nalu.data.insert(nalu.data.end(), buf + nal_start, buf + nal_end);
                                    LOG_DEBUG("Channel " << encChn << ": Writing NAL unit to message channel (size=" << nalu.data.size() << ")");
                                    if (!global_video[encChn]->msgChannel->write(nalu)) {
                                        LOG_ERROR("video " << "channel:" << encChn << ", "
                                                 << "package:" << i << " of " << stream.packCount
                                                 << ", " << "packageSize:" << nalu.data.size()
                                                 << ".  !sink clogged!");
                                    } else {
                                        std::unique_lock<std::mutex> lock_stream{ global_video[encChn]->onDataCallbackLock };
                                        if (global_video[encChn]->onDataCallback)
                                            global_video[encChn]->onDataCallback();
                                    }
                                }
                                if (i_next < 0) break;
                                i = i_next;
                                sc_len = sc_len_cur;
                            }
                        }
                    }
                    else
                    {
                        LOG_DEBUG("Channel " << encChn << ": Skipping NAL unit because idr=false (waiting for first IDR frame)");
                    }
#if defined(USE_AUDIO_STREAM_REPLICATOR)
                    /* Since the audio stream is permanently in use by the stream replicator,
                        * and the audio grabber and encoder standby is also controlled by the video threads
                        * we need to wakeup the audio thread
                    */
                    if (cfg->audio.input_enabled && !global_audio[0]->active && !global_restart)
                    {
                        LOG_DDEBUG("NOTIFY AUDIO " << !global_audio[0]->active << " "
                                                    << cfg->audio.input_enabled);
                        global_audio[0]->should_grab_frames.notify_one();
                    }
#endif
                }

                // Release the stream back to IMP so the producer can post the next one
                int rel = IMP_Encoder_ReleaseStream(encChn, &stream);
                LOG_DEBUG_OR_ERROR(rel, "IMP_Encoder_ReleaseStream(" << encChn << ")");

            }

    }
}

void *VideoWorker::thread_entry(void *arg)
{
    StartHelper *sh = static_cast<StartHelper *>(arg);
    int encChn = sh->encChn;

    LOG_DEBUG("Start stream_grabber thread for stream " << encChn);

    // Optional software test path: if configured, feed NAL units from a local H.264 Annex B file
    const char *testPath = (encChn == 0) ? cfg->stream0.test_h264_path : cfg->stream1.test_h264_path;
    if (testPath && testPath[0] != '\0')
    {
        std::ifstream ifs(testPath, std::ios::binary);
        if (ifs)
        {
            std::vector<uint8_t> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            ifs.close();

            auto find_start_code = [](const uint8_t *b, size_t off, size_t n, int *sc_len) -> long {
                for (size_t k = off; k + 3 < n; ++k) {
                    if (b[k] == 0x00 && b[k+1] == 0x00) {
                        if (b[k+2] == 0x01) { if (sc_len) *sc_len = 3; return static_cast<long>(k); }
                        if (k + 4 < n && b[k+2] == 0x00 && b[k+3] == 0x01) { if (sc_len) *sc_len = 4; return static_cast<long>(k); }
                    }
                }
                return -1;
            };

            // Parse Annex B into discrete NAL payloads (without start codes)
            std::vector<std::vector<uint8_t>> nals;
            std::vector<int> types;
            int sc_len = 0;
            long i0 = find_start_code(buf.data(), 0, buf.size(), &sc_len);
            if (i0 >= 0) {
                long i = i0;
                while (i >= 0 && static_cast<size_t>(i) < buf.size()) {
                    int sc_len_cur = 0;
                    size_t nal_start = static_cast<size_t>(i) + static_cast<size_t>(sc_len);
                    long i_next = find_start_code(buf.data(), nal_start, buf.size(), &sc_len_cur);
                    size_t nal_end = (i_next >= 0) ? static_cast<size_t>(i_next) : buf.size();
                    if (nal_end > nal_start) {
                        std::vector<uint8_t> payload;
                        payload.insert(payload.end(), buf.begin() + nal_start, buf.begin() + nal_end);
                        nals.emplace_back(std::move(payload));
                        types.emplace_back(nals.back()[0] & 0x1F);
                    }
                    if (i_next < 0) break;
                    i = i_next;
                    sc_len = sc_len_cur;
                }
            }

            // Signal start and mark active without touching IMP encoder
            sh->has_started.release();
            global_video[encChn]->active = true;
            global_video[encChn]->running = true;

            // Determine pacing
            int fps_cfg = (encChn == 0) ? cfg->stream0.fps : cfg->stream1.fps;
            if (fps_cfg <= 0) fps_cfg = 25;
            auto frame_interval = std::chrono::milliseconds(1000 / fps_cfg);
            bool frame_started = false;
            auto next_deadline = std::chrono::steady_clock::now();

            // Stream in a loop
            while (global_video[encChn]->running)
            {
                for (size_t i = 0; i < nals.size() && global_video[encChn]->running; ++i)
                {
                    int t = types[i];
                    if (!global_video[encChn]->idr && (t == 7 || t == 8 || t == 5))
                        global_video[encChn]->idr = true;

                    // Timestamp from TimestampManager (hardware clock source)
                    struct timeval monotonic_time;
                    TimestampManager::getInstance().getTimestamp(&monotonic_time);

                    H264NALUnit nalu;
                    nalu.time = monotonic_time;
                    nalu.data = nals[i];

                    if (!global_video[encChn]->msgChannel->write(nalu)) {
                        LOG_ERROR("video channel:" << encChn << " !sink clogged!");
                    } else {
                        std::unique_lock<std::mutex> lock_stream{ global_video[encChn]->onDataCallbackLock };
                        if (global_video[encChn]->onDataCallback)
                            global_video[encChn]->onDataCallback();
                    }

                    // Pacing: sleep per-AU using AUD if present; otherwise on slice boundaries
                    if (t == 9) {
                        if (frame_started) {
                            std::this_thread::sleep_until(next_deadline);
                            next_deadline += frame_interval;
                        } else {
                            frame_started = true;
                            next_deadline = std::chrono::steady_clock::now() + frame_interval;
                        }
                    } else if ((t == 1 || t == 5) && !frame_started) {
                        std::this_thread::sleep_until(next_deadline);
                        next_deadline += frame_interval;
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }

            return 0;
        }
        else
        {
            LOG_WARN("Test H.264 path set but failed to open: " << testPath << ", falling back to IMP capture");
        }
    }

    int ret;

    global_video[encChn]->imp_framesource = IMPFramesource::createNew(global_video[encChn]->stream,
                                                                      &cfg->sensor,
                                                                      encChn);
    global_video[encChn]->imp_encoder = IMPEncoder::createNew(global_video[encChn]->stream,
                                                              encChn,
                                                              encChn,
                                                              global_video[encChn]->name);
    global_video[encChn]->imp_framesource->enable();
    global_video[encChn]->run_for_jpeg = false;

    // inform main that initialization is complete
    sh->has_started.release();

    ret = IMP_Encoder_StartRecvPic(encChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StartRecvPic(" << encChn << ")");
    if (ret != 0)
        return 0;

    // Proactively request an IDR to ensure SPS/PPS are emitted promptly
    IMP_Encoder_RequestIDR(encChn);
    LOG_DEBUG("IMP_Encoder_RequestIDR(" << encChn << ")");
    global_video[encChn]->idr_fix = 2;

    global_video[encChn]->active = true;
    global_video[encChn]->running = true;
    VideoWorker worker(encChn);
    worker.run();

    ret = IMP_Encoder_StopRecvPic(encChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StopRecvPic(" << encChn << ")");

    if (global_video[encChn]->imp_framesource)
    {
        global_video[encChn]->imp_framesource->disable();

        if (global_video[encChn]->imp_encoder)
        {
            global_video[encChn]->imp_encoder->deinit();
            delete global_video[encChn]->imp_encoder;
            global_video[encChn]->imp_encoder = nullptr;
        }
    }

    return 0;
}

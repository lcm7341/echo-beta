// video encoding on ios. no ffmpeg there, so AVAssetWriter does it
//
// open_video: h264 into an mp4, w/ a pool of BGRA pixel buffers backed by IOSurfaces
// writeFrame: the slow way. copies a glReadPixels frame into a pool buffer (flipped, gl is bottom up)
// begin_direct_frame / end_direct_frame: the fast way. a pool buffer gets wrapped as a gl texture so gd draws STRAIGHT into
//   the memory the encoder reads, no copy. frames are appended DIRECT_DELAY frames late so the gpu has finished w/ them
// add_audio: reads the finished video back & writes a new file w/ the same video + an AAC track
//   the rate of the samples is their count / the videos length, NOT a fixed 44100 (phones mix at 48000 & render speed moves it)
// every append waits for readyForMoreMediaData first or the writer rejects the frame
// errors come back as strings, empty = fine
#include "ios_encoder_core.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>
#endif

#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

namespace echo::apple {
    namespace {
        std::string describe(NSError* error, char const* fallback) {
            if (!error)
                return fallback;
            return std::string(fallback) + ": " + [[error localizedDescription] UTF8String];
        }

        NSURL* fileURL(std::string const& path) {
            return [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        }

        bool waitUntilReady(AVAssetWriterInput* input, AVAssetWriter* writer) {
            for (int i = 0; i < 10000; ++i) {
                if (input.readyForMoreMediaData)
                    return true;
                if (writer.status != AVAssetWriterStatusWriting)
                    return false;
                [NSThread sleepForTimeInterval:0.001];
            }
            return false;
        }

        std::string finish(AVAssetWriter* writer) {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [writer finishWritingWithCompletionHandler:^{
              dispatch_semaphore_signal(done);
            }];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            if (writer.status != AVAssetWriterStatusCompleted)
                return describe(writer.error, "The video could not be finished");
            return {};
        }
    }

    struct VideoWriter {
        AVAssetWriter* writer = nil;
        AVAssetWriterInput* input = nil;
        AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
        int width = 0;
        int height = 0;
        int fps = 60;
        long long frame = 0;
#if TARGET_OS_IPHONE
        struct Direct {
            CVPixelBufferRef buffer = nullptr;
            CVOpenGLESTextureRef texture = nullptr;
        };

        CVOpenGLESTextureCacheRef cache = nullptr;
        Direct drawing;
        std::deque<Direct> pending;
#endif
    };

#if TARGET_OS_IPHONE
    namespace {
        constexpr size_t DIRECT_DELAY = 2;

        std::string appendDirect(VideoWriter* writer, VideoWriter::Direct const& frame) {
            std::string failed;
            if (!waitUntilReady(writer->input, writer->writer))
                failed = describe(writer->writer.error, "The video encoder stopped taking frames");
            else if (![writer->adaptor appendPixelBuffer:frame.buffer withPresentationTime:CMTimeMake(writer->frame, writer->fps)])
                failed = describe(writer->writer.error, "A frame could not be encoded");
            else
                ++writer->frame;
            if (frame.texture)
                CFRelease(frame.texture);
            if (frame.buffer)
                CVPixelBufferRelease(frame.buffer);
            return failed;
        }

        std::string flushDirect(VideoWriter* writer) {
            std::string failed;
            if (writer->drawing.texture)
                CFRelease(writer->drawing.texture);
            if (writer->drawing.buffer)
                CVPixelBufferRelease(writer->drawing.buffer);
            writer->drawing = {};
            if (!writer->pending.empty())
                glFinish();
            while (!writer->pending.empty()) {
                auto frame = writer->pending.front();
                writer->pending.pop_front();
                auto result = appendDirect(writer, frame);
                if (failed.empty())
                    failed = result;
            }
            return failed;
        }
    }
#endif

    std::string open_video(VideoWriter*& out, std::string const& path, int width, int height, int fps, long long bitrate) {
        out = nullptr;
        @autoreleasepool {
            [[NSFileManager defaultManager] removeItemAtURL:fileURL(path) error:nil];
            NSError* error = nil;
            AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:fileURL(path) fileType:AVFileTypeMPEG4 error:&error];
            if (!writer)
                return describe(error, "Could not create the video file");
            NSDictionary* settings = @{
                AVVideoCodecKey : AVVideoCodecTypeH264,
                AVVideoWidthKey : @(width),
                AVVideoHeightKey : @(height),
                AVVideoCompressionPropertiesKey : @{
                    AVVideoAverageBitRateKey : @(bitrate),
                    AVVideoMaxKeyFrameIntervalKey : @(std::max(fps, 1) * 2),
                    AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
                    AVVideoExpectedSourceFrameRateKey : @(fps),
                },
            };
            if (![writer canApplyOutputSettings:settings forMediaType:AVMediaTypeVideo])
                return "This device cannot encode a video of that size";
            AVAssetWriterInput* input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
            input.expectsMediaDataInRealTime = NO;
            NSDictionary* pixels = @{
                (NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                (NSString*)kCVPixelBufferWidthKey : @(width),
                (NSString*)kCVPixelBufferHeightKey : @(height),
#if TARGET_OS_IPHONE
                (NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
                (NSString*)kCVPixelBufferOpenGLESCompatibilityKey : @YES,
#endif
            };
            AVAssetWriterInputPixelBufferAdaptor* adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input sourcePixelBufferAttributes:pixels];
            if (![writer canAddInput:input])
                return "The video encoder could not be attached";
            [writer addInput:input];
            if (![writer startWriting])
                return describe(writer.error, "The video encoder did not start");
            [writer startSessionAtSourceTime:kCMTimeZero];
            auto* made = new VideoWriter();
            made->writer = writer;
            made->input = input;
            made->adaptor = adaptor;
            made->width = width;
            made->height = height;
            made->fps = std::max(fps, 1);
            out = made;
        }
        return {};
    }

    std::string writeFrame(VideoWriter* writer, std::uint8_t const* rgba, bool bottomUp) {
        if (!writer || !rgba)
            return "The video encoder is not open";
#if TARGET_OS_IPHONE
        if (auto failed = flushDirect(writer); !failed.empty())
            return failed;
#endif
        @autoreleasepool {
            if (!waitUntilReady(writer->input, writer->writer))
                return describe(writer->writer.error, "The video encoder stopped taking frames");
            CVPixelBufferRef buffer = nullptr;
            if (CVPixelBufferPoolCreatePixelBuffer(nullptr, writer->adaptor.pixelBufferPool, &buffer) != kCVReturnSuccess || !buffer)
                return "Could not get a frame buffer";
            CVPixelBufferLockBaseAddress(buffer, 0);
            auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
            size_t stride = CVPixelBufferGetBytesPerRow(buffer);
            int width = writer->width;
            int height = writer->height;
            for (int y = 0; y < height; ++y) {
                auto const* from = rgba + static_cast<size_t>(bottomUp ? height - 1 - y : y) * static_cast<size_t>(width) * 4;
                auto* to = base + static_cast<size_t>(y) * stride;
                for (int x = 0; x < width; ++x) {
                    to[x * 4 + 0] = from[x * 4 + 2];
                    to[x * 4 + 1] = from[x * 4 + 1];
                    to[x * 4 + 2] = from[x * 4 + 0];
                    to[x * 4 + 3] = 255;
                }
            }
            CVPixelBufferUnlockBaseAddress(buffer, 0);
            BOOL ok = [writer->adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(writer->frame, writer->fps)];
            CVPixelBufferRelease(buffer);
            if (!ok)
                return describe(writer->writer.error, "A frame could not be encoded");
            ++writer->frame;
        }
        return {};
    }

    std::string close_video(VideoWriter* writer) {
        if (!writer)
            return {};
        std::string failed;
        @autoreleasepool {
#if TARGET_OS_IPHONE
            failed = flushDirect(writer);
            if (writer->cache) {
                CVOpenGLESTextureCacheFlush(writer->cache, 0);
                CFRelease(writer->cache);
                writer->cache = nullptr;
            }
            if (!failed.empty() && writer->writer.status == AVAssetWriterStatusWriting)
                [writer->writer cancelWriting];
            else
#endif
                if (writer->writer.status == AVAssetWriterStatusWriting) {
                [writer->input markAsFinished];
                [writer->writer endSessionAtSourceTime:CMTimeMake(writer->frame, writer->fps)];
                failed = finish(writer->writer);
            }
            else
                failed = describe(writer->writer.error, "The video encoder had already stopped");
        }
        delete writer;
        return failed;
    }

#if TARGET_OS_IPHONE
    std::string begin_direct_frame(VideoWriter* writer, unsigned& texture) {
        texture = 0;
        if (!writer)
            return "The video encoder is not open";
        if (!writer->cache) {
            EAGLContext* context = [EAGLContext currentContext];
            if (!context)
                return "There is no OpenGL context to draw frames with";
            if (CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, nullptr, context, nullptr, &writer->cache) != kCVReturnSuccess || !writer->cache)
                return "Could not share memory between the GPU and the encoder";
        }
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(nullptr, writer->adaptor.pixelBufferPool, &buffer) != kCVReturnSuccess || !buffer)
            return "Could not get a frame buffer";
        CVOpenGLESTextureRef made = nullptr;
        CVReturn result = CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault, writer->cache, buffer, nullptr, GL_TEXTURE_2D, GL_RGBA,
            writer->width, writer->height, GL_BGRA, GL_UNSIGNED_BYTE, 0, &made);
        if (result != kCVReturnSuccess || !made) {
            CVPixelBufferRelease(buffer);
            return "The encoder's frame buffer could not be drawn into";
        }
        GLint bound = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        glBindTexture(GL_TEXTURE_2D, CVOpenGLESTextureGetName(made));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
        writer->drawing.buffer = buffer;
        writer->drawing.texture = made;
        texture = CVOpenGLESTextureGetName(made);
        return {};
    }

    std::string end_direct_frame(VideoWriter* writer) {
        if (!writer || !writer->drawing.buffer)
            return "No frame was being drawn";
        glFlush();
        writer->pending.push_back(writer->drawing);
        writer->drawing = {};
        std::string failed;
        @autoreleasepool {
            while (failed.empty() && writer->pending.size() > DIRECT_DELAY) {
                auto frame = writer->pending.front();
                writer->pending.pop_front();
                failed = appendDirect(writer, frame);
            }
        }
        CVOpenGLESTextureCacheFlush(writer->cache, 0);
        return failed;
    }

#else
    std::string begin_direct_frame(VideoWriter*, unsigned& texture) {
        texture = 0;
        return "Direct frames are for iOS";
    }

    std::string end_direct_frame(VideoWriter*) {
        return "Direct frames are for iOS";
    }
#endif

    std::string add_audio(std::string const& video, float const* stereo, std::size_t frames, std::string const& output) {
        if (!stereo || frames == 0)
            return "There is no audio to add";
        @autoreleasepool {
            AVURLAsset* asset = [AVURLAsset URLAssetWithURL:fileURL(video) options:nil];
            AVAssetTrack* track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
            if (!track)
                return "The rendered video could not be read";
            double seconds = CMTimeGetSeconds(asset.duration);
            if (!(seconds > 0.0))
                return "The rendered video has no length";
            double rate = static_cast<double>(frames) / seconds;

            NSError* error = nil;
            AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
            if (!reader)
                return describe(error, "The rendered video could not be opened");
            AVAssetReaderTrackOutput* frameSource = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
            frameSource.alwaysCopiesSampleData = NO;
            [reader addOutput:frameSource];

            [[NSFileManager defaultManager] removeItemAtURL:fileURL(output) error:nil];
            AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:fileURL(output) fileType:AVFileTypeMPEG4 error:&error];
            if (!writer)
                return describe(error, "Could not create the video file");
            CMFormatDescriptionRef videoFormat = (__bridge CMFormatDescriptionRef)[track.formatDescriptions firstObject];
            AVAssetWriterInput* videoIn = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:nil sourceFormatHint:videoFormat];
            videoIn.expectsMediaDataInRealTime = NO;
            AudioChannelLayout layout{};
            layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
            NSDictionary* audioSettings = @{
                AVFormatIDKey : @(kAudioFormatMPEG4AAC),
                AVSampleRateKey : @(44100),
                AVNumberOfChannelsKey : @(2),
                AVEncoderBitRateKey : @(192000),
                AVChannelLayoutKey : [NSData dataWithBytes:&layout length:sizeof(layout)],
            };
            AVAssetWriterInput* audioIn = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:audioSettings];
            audioIn.expectsMediaDataInRealTime = NO;
            if (![writer canAddInput:videoIn] || ![writer canAddInput:audioIn])
                return "The audio could not be attached to the video";
            [writer addInput:videoIn];
            [writer addInput:audioIn];

            AudioStreamBasicDescription pcm{};
            pcm.mSampleRate = rate;
            pcm.mFormatID = kAudioFormatLinearPCM;
            pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
            pcm.mChannelsPerFrame = 2;
            pcm.mBitsPerChannel = 32;
            pcm.mBytesPerFrame = 8;
            pcm.mFramesPerPacket = 1;
            pcm.mBytesPerPacket = 8;
            CMAudioFormatDescriptionRef audioFormat = nullptr;
            if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &pcm, sizeof(layout), &layout, 0, nullptr, nullptr, &audioFormat) != noErr)
                return "The audio format was not accepted";

            if (![reader startReading]) {
                CFRelease(audioFormat);
                return describe(reader.error, "The rendered video could not be read");
            }
            if (![writer startWriting]) {
                CFRelease(audioFormat);
                return describe(writer.error, "The video file could not be written");
            }
            [writer startSessionAtSourceTime:kCMTimeZero];

            constexpr std::size_t CHUNK = 8192;
            std::size_t sent = 0;
            bool videoDone = false;
            bool audioDone = false;
            std::string failed;
            int idle = 0;
            while (failed.empty() && !(videoDone && audioDone)) {
                bool moved = false;
                if (!videoDone && videoIn.readyForMoreMediaData) {
                    CMSampleBufferRef sample = [frameSource copyNextSampleBuffer];
                    if (sample) {
                        if (![videoIn appendSampleBuffer:sample])
                            failed = describe(writer.error, "A frame could not be copied");
                        CFRelease(sample);
                    }
                    else {
                        [videoIn markAsFinished];
                        videoDone = true;
                        if (reader.status == AVAssetReaderStatusFailed)
                            failed = describe(reader.error, "The rendered video could not be read");
                    }
                    moved = true;
                }
                if (failed.empty() && !audioDone && audioIn.readyForMoreMediaData) {
                    std::size_t count = std::min(CHUNK, frames - sent);
                    if (count == 0) {
                        [audioIn markAsFinished];
                        audioDone = true;
                    }
                    else {
                        CMBlockBufferRef block = nullptr;
                        size_t bytes = count * 8;
                        OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes, kCFAllocatorDefault, nullptr, 0, bytes, kCMBlockBufferAssureMemoryNowFlag, &block);
                        if (status == noErr)
                            status = CMBlockBufferReplaceDataBytes(stereo + sent * 2, block, 0, bytes);
                        CMSampleBufferRef sample = nullptr;
                        CMTime at = CMTimeMakeWithSeconds(static_cast<double>(sent) / rate, 1000000);
                        if (status == noErr)
                            status = CMAudioSampleBufferCreateReadyWithPacketDescriptions(kCFAllocatorDefault, block, audioFormat, static_cast<CMItemCount>(count), at, nullptr, &sample);
                        if (status != noErr || !sample)
                            failed = "The audio could not be prepared";
                        else if (![audioIn appendSampleBuffer:sample])
                            failed = describe(writer.error, "The audio could not be encoded");
                        if (sample)
                            CFRelease(sample);
                        if (block)
                            CFRelease(block);
                        sent += count;
                    }
                    moved = true;
                }
                if (moved) {
                    idle = 0;
                    continue;
                }
                if (writer.status != AVAssetWriterStatusWriting)
                    failed = describe(writer.error, "The video file could not be written");
                else if (++idle > 10000)
                    failed = "The video encoder stopped responding";
                else
                    [NSThread sleepForTimeInterval:0.001];
            }
            CFRelease(audioFormat);
            if (!failed.empty()) {
                [reader cancelReading];
                [writer cancelWriting];
                return failed;
            }
            return finish(writer);
        }
    }
}

#include "engine/Exporter.h"

#include "audio/AudioMixer.h"
#include "core/Log.h"
#include "media/EncoderThread.h"
#include "media/Mp4Writer.h"
#include "media/VideoDecoder.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace ve {

namespace {

bool timelineHasAudio(const TimelineSnapshot& snap)
{
    for (const Track& t : snap.timeline.tracks) {
        if (t.kind != TrackKind::Audio || !t.enabled)
            continue;
        for (const PlacedItem& p : t.clips()) {
            const Clip* clip = std::get_if<Clip>(p.item);
            const MediaItem* m = clip ? snap.findMedia(clip->mediaId) : nullptr;
            if (m && m->hasAudio)
                return true;
        }
    }
    return false;
}

ExportResult runExport(const TimelineSnapshot& snap, const ExportSettings& settings, const ExportProgress& progress)
{
    ExportResult res;
    const Timeline& tl = snap.timeline;
    const std::int64_t total = frameAt(tl.duration(), tl.rate);
    if (total <= 0) {
        res.error = "The timeline is empty";
        return res;
    }
    const int width = (settings.width > 0 ? settings.width : tl.width) & ~1;
    const int height = (settings.height > 0 ? settings.height : tl.height) & ~1;
    if (width < 16 || height < 16) {
        res.error = "Invalid output size";
        return res;
    }

    Mp4Writer writer;
    if (!writer.open(settings.outputPath, tl.rate, width, height, settings.videoEncoder, settings.videoBitrateKbps,
                     timelineHasAudio(snap), settings.audioBitrateKbps, &res.error))
        return res;
    res.videoEncoder = writer.encoderLabel();

    FrameScaler scaler;
    std::map<Id, std::unique_ptr<VideoDecoder>> decoders;
    AudioMixer mixer(kEncodeAudioRate);
    std::vector<float> audio;
    std::int64_t samplesMixed = 0;

    for (std::int64_t f = 0; f < total; ++f) {
        // Picture: the topmost clip under this frame, or black.
        AVFrame* picture = writer.beginVideoFrame();
        if (!picture) {
            res.error = "Out of memory";
            return res;
        }
        if (const auto source = resolveVideo(snap, timeOfFrame(f, tl.rate))) {
            auto& dec = decoders[source->media->id];
            if (!dec) {
                dec = std::make_unique<VideoDecoder>();
                std::string error;
                if (!dec->open(source->media->path, {}, &error))
                    logWarning("Export: ", error);
            }
            if (dec->isOpen()) {
                if (VideoFramePtr decoded = dec->frameAt(source->sourceTime))
                    scaler.scaleInto(*decoded, picture);
            }
        }
        int r = writer.writeVideoFrame(f);
        if (r < 0) {
            res.error = "Video encoding failed: " + ff::errorString(r);
            return res;
        }

        // Sound up to the end of this frame (sample-exact against the frame grid).
        if (writer.hasAudio()) {
            const std::int64_t target = sampleAt(timeOfFrame(f + 1, tl.rate), kEncodeAudioRate);
            const int n = int(target - samplesMixed);
            audio.resize(std::size_t(std::max(n, 0)) * 2);
            if (n > 0)
                mixer.mix(snap, samplesMixed, n, audio.data());
            samplesMixed = target;
            r = writer.writeAudio(audio.data(), n);
            if (r < 0) {
                res.error = "Audio encoding failed: " + ff::errorString(r);
                return res;
            }
        }

        ++res.framesWritten;
        if (progress && !progress(double(f + 1) / double(total))) {
            res.cancelled = true;
            return res;
        }
    }

    res.ok = writer.finish(&res.error);
    return res;
}

} // namespace

ExportResult exportTimeline(const TimelineSnapshot& snap, const ExportSettings& settings, const ExportProgress& progress)
{
    return runInEncoderThread([&] {
        ExportResult res = runExport(snap, settings, progress);
        if (!res.ok)
            removeFileQuietly(settings.outputPath); // do not leave a truncated file behind
        return res;
    });
}

} // namespace ve

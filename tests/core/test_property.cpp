// Property tests (spec §6.2 acceptance): random sequences of every edit (phase 1 and phase 2: three
// and four point edits, ripple/roll/slip/slide, ripple delete, compound clips, track moves) run
// through the CommandStack on a multitrack timeline. After each step:
// - edit::checkInvariants holds on every timeline of the project: frame grid, no media used outside
//   the source, no overlaps on a track (by construction), unique ids, no leftover gaps;
// - linked picture and sound stay aligned: strictly (same cuts on Vn and An) while every edit is
//   linked, and in sync (same media offset, J/L cuts allowed) when asymmetric trims and one-sided edits
//   are in the mix;
// - a locked track never changes;
// and at the end undoing everything gives back exactly the initial project.

#include "core/Commands.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

#include <cstdlib>
#include <map>
#include <random>
#include <sstream>

using namespace ve;
using namespace ve::test;

namespace {

class RandomEditor {
public:
    RandomEditor(std::uint64_t seed, bool asymmetric)
        : m_rng(seed)
        , m_asymmetric(asymmetric)
    {
        m_project.timeline().rate = kRate;
        edit::addTrack(m_project.timeline(), TrackKind::Video);
        edit::addTrack(m_project.timeline(), TrackKind::Audio);
        m_media = {testMedia(10.0), testMedia(4.0), testMedia(7.5)};
        if (asymmetric) {
            m_media.push_back(testMedia(3.0, true, false));  // picture only
            m_media.push_back(testMedia(12.0, false, true)); // sound only
        }
        for (const MediaItem& m : m_media)
            m_project.media.items.push_back(m);
        m_initial = m_project;
    }

    void run(int steps)
    {
        for (int step = 0; step < steps; ++step) {
            // Locks and sync lock switches only in the second half: the first half checks sync.
            m_mayLock = m_asymmetric && step >= steps / 2;
            const Timeline before = m_project.timeline();
            const Op op = randomStep();
            m_applied += op.changed ? 1 : 0;
            m_trims += op.changed && op.trim ? 1 : 0;
            if (op.changed)
                ++m_appliedBy[op.name];
            check(before, op, step);
        }
        CHECK(m_applied > steps / 3); // the sequence really edited things
        CHECK(m_trims > steps / 6);

        while (m_stack.canUndo())
            m_stack.undo();
        CHECK(m_project.timelines == m_initial.timelines);
        CHECK(m_project.currentTimeline == m_initial.currentTimeline);
    }

    int trims() const { return m_trims; }
    const std::map<std::string, int>& appliedBy() const { return m_appliedBy; }

private:
    struct Op {
        std::string name;
        bool changed = false;
        bool edit = true; // false for undo and track switches
        bool trim = false;
        bool projectWide = false; // may touch nested timelines (compound clips, undo, redo)
    };

    int pick(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(m_rng); }
    Time frame(int lo, int hi) { return F(pick(lo, hi)); }
    bool chance(int percent) { return pick(0, 99) < percent; }
    Timeline& tl() { return m_project.timeline(); }

    template <typename T>
    const T& any(const std::vector<T>& v)
    {
        return v[std::size_t(pick(0, int(v.size()) - 1))];
    }

    Id randomClip(bool compoundsOnly = false)
    {
        std::vector<Id> ids;
        for (const Track& t : tl().tracks) {
            for (const PlacedItem& p : t.clips()) {
                if (!compoundsOnly || std::holds_alternative<CompoundClip>(*p.item))
                    ids.push_back(p.clip->id);
            }
        }
        return ids.empty() ? Id{} : any(ids);
    }

    // Destinations: a matching Vn/An pair while everything is linked, anything otherwise.
    void destinations(edit::EditPoints& p)
    {
        const auto video = tl().tracksOfKind(TrackKind::Video);
        const auto audio = tl().tracksOfKind(TrackKind::Audio);
        if (!m_asymmetric) {
            const int k = pick(0, int(std::min(video.size(), audio.size())) - 1);
            p.videoTrack = video[std::size_t(k)]->id;
            p.audioTrack = audio[std::size_t(k)]->id;
            return;
        }
        p.videoTrack = chance(80) ? any(video)->id : Id{};
        p.audioTrack = chance(80) ? any(audio)->id : Id{};
    }

    edit::EditPoints randomPoints(const MediaItem& m)
    {
        edit::EditPoints p;
        p.mediaId = m.id;
        p.playhead = frame(0, 900);
        const int length = int(frames(m.duration));
        if (chance(50))
            p.sourceIn = frame(0, length);
        if (chance(40))
            p.sourceOut = frame(0, length);
        if (chance(30))
            p.recordIn = frame(0, 900);
        if (chance(40))
            p.recordOut = frame(0, 960);
        destinations(p);
        return p;
    }

    bool edit(const std::string& name, TimelineEditCommand::Operation fn) { return m_stack.edit(name, std::move(fn)); }
    bool editProject(const std::string& name, ProjectEditCommand::Operation fn)
    {
        return m_stack.editProject(name, std::move(fn));
    }

    std::size_t clipCount()
    {
        std::size_t n = 0;
        for (const Track& t : tl().tracks)
            n += t.clips().size();
        return n;
    }

    Op randomStep()
    {
        const Id clip = randomClip();
        const MediaItem m = any(m_media);
        const bool linked = !m_asymmetric || chance(50);
        int op = pick(0, 99);
        if (clipCount() > 160 && chance(30))
            op = 79; // keep the timeline dense and bounded: lift a clip

        if (op < 3 || clip.empty()) {
            const Time at = frame(0, 900);
            return {"place", edit("Place", [m, at](Timeline& t, const MediaPool&) { return !edit::placeMedia(t, m, at).empty(); })};
        }
        if (op < 13) {
            const edit::EditPoints p = randomPoints(m);
            switch (pick(0, 6)) {
            case 0:
                return {"insert", edit("Insert", [p](Timeline& t, const MediaPool& mp) { return !edit::insertEdit(t, mp, p).empty(); })};
            case 1:
                return {"overwrite",
                        edit("Overwrite", [p](Timeline& t, const MediaPool& mp) { return !edit::overwriteEdit(t, mp, p).empty(); })};
            case 2:
                return {"replace",
                        edit("Replace", [p](Timeline& t, const MediaPool& mp) { return !edit::replaceEdit(t, mp, p).empty(); })};
            case 3:
                return {"fit to fill",
                        edit("Fit", [p](Timeline& t, const MediaPool& mp) { return !edit::fitToFillEdit(t, mp, p).empty(); })};
            case 4:
                return {"place on top",
                        edit("Top", [p](Timeline& t, const MediaPool& mp) { return !edit::placeOnTopEdit(t, mp, p).empty(); })};
            case 5:
                return {"append",
                        edit("Append", [p](Timeline& t, const MediaPool& mp) { return !edit::appendEdit(t, mp, p).empty(); })};
            default: {
                // Aim at an existing clip: its track (and the matching one of the other kind) is the destination.
                edit::EditPoints aimed = p;
                aimed.recordIn.reset();
                aimed.recordOut.reset();
                if (auto loc = tl().locateClip(clip)) {
                    aimed.playhead = loc->start;
                    const int k = edit::kindIndex(tl(), loc->track->id);
                    const Track* video = edit::trackOfKind(tl(), TrackKind::Video, k);
                    const Track* audio = edit::trackOfKind(tl(), TrackKind::Audio, k);
                    if (loc->track->kind == TrackKind::Video) {
                        aimed.videoTrack = loc->track->id;
                        aimed.audioTrack = audio && !m_asymmetric ? audio->id : p.audioTrack;
                    } else {
                        aimed.audioTrack = loc->track->id;
                        aimed.videoTrack = video && !m_asymmetric ? video->id : Id{};
                    }
                }
                return {"ripple overwrite", edit("Ripple overwrite", [aimed](Timeline& t, const MediaPool& mp) {
                            return !edit::rippleOverwriteEdit(t, mp, aimed).empty();
                        })};
            }
            }
        }
        if (op < 17) {
            const Time t0 = frame(0, 900);
            if (chance(50)) // only this clip, with or without its partners
                return {"split", edit("Split", [t0, clip, linked](Timeline& t, const MediaPool&) {
                            return edit::splitAt(t, t0, {clip}, linked);
                        })};
            return {"split", edit("Split", [t0](Timeline& t, const MediaPool&) { return edit::splitAt(t, t0); })};
        }
        if (op < 22) {
            const Time delta = frame(-120, 120);
            const int shift = pick(-1, 1);
            // Moving a picture without its sound puts the take out of sync: only once that may happen.
            const bool together = !m_mayLock || chance(50);
            Op result{"move", edit("Move", [clip, delta, shift, together](Timeline& t, const MediaPool&) {
                          return edit::moveClips(t, {clip}, delta, shift, together);
                      })};
            if (!together)
                m_syncGuaranteed = false;
            return result;
        }
        const auto edge = chance(50) ? edit::Edge::Head : edit::Edge::Tail;
        if (op < 28) {
            const Time to = frame(0, 900);
            return {"trim", edit("Trim", [clip, edge, to, linked](Timeline& t, const MediaPool& mp) {
                        return edit::trimClip(t, mp, clip, edge, to, linked);
                    }), true, true};
        }
        if (op < 44) {
            const Time delta = frame(-90, 90);
            return {"ripple", edit("Ripple", [clip, edge, delta, linked](Timeline& t, const MediaPool& mp) {
                        return edit::rippleTrim(t, mp, clip, edge, delta, linked);
                    }), true, true};
        }
        if (op < 58) {
            const Time delta = frame(-90, 90);
            return {"roll", edit("Roll", [clip, edge, delta, linked](Timeline& t, const MediaPool& mp) {
                        return edit::rollTrim(t, mp, clip, edge, delta, linked);
                    }), true, true};
        }
        if (op < 65) {
            const Time delta = frame(-90, 90);
            return {"slip", edit("Slip", [clip, delta](Timeline& t, const MediaPool& mp) {
                        return edit::slipClip(t, mp, clip, delta);
                    }), true, true};
        }
        if (op < 72) {
            const Time delta = frame(-90, 90);
            return {"slide", edit("Slide", [clip, delta](Timeline& t, const MediaPool& mp) {
                        return edit::slideClip(t, mp, clip, delta);
                    }), true, true};
        }
        if (op < 78) {
            switch (pick(0, m_asymmetric ? 5 : 3)) {
            case 0: {
                const Time at = frame(0, 900);
                const Time length = frame(1, 60);
                return {"insert gap",
                        edit("Insert gap", [at, length](Timeline& t, const MediaPool&) { return edit::insertGap(t, at, length); })};
            }
            case 1: {
                // Aim at an existing gap (most random points are not in one).
                std::vector<std::pair<Id, Time>> gaps;
                for (const Track& t : tl().tracks) {
                    Time pos;
                    for (const Item& item : t.items) {
                        if (std::holds_alternative<Gap>(item))
                            gaps.push_back({t.id, pos});
                        pos += durationOf(item);
                    }
                }
                if (gaps.empty())
                    return {"delete gap (none)", false};
                const auto [trackId, at] = any(gaps);
                return {"delete gap",
                        edit("Delete gap", [trackId, at](Timeline& t, const MediaPool&) { return edit::deleteGap(t, trackId, at); })};
            }
            case 2:
                return {"delete gaps", edit("Delete gaps", [](Timeline& t, const MediaPool&) { return edit::deleteGaps(t); })};
            case 3: {
                const int direction = chance(50) ? 1 : -1;
                return {"swap", edit("Swap", [clip, direction](Timeline& t, const MediaPool&) {
                            return edit::swapClips(t, clip, direction);
                        })};
            }
            case 4: {
                if (!m_mayLock)
                    return {"link (not yet)", false, false};
                const Id other = randomClip();
                Op result{"link", edit("Link", [clip, other](Timeline& t, const MediaPool&) {
                              return edit::linkClips(t, {clip, other});
                          })};
                m_syncGuaranteed = false; // a link may join clips that are not in sync
                return result;
            }
            default:
                return {"unlink", edit("Unlink", [clip](Timeline& t, const MediaPool&) { return edit::unlinkClips(t, {clip}); })};
            }
        }
        if (op < 81)
            return {"delete", edit("Delete", [clip, linked](Timeline& t, const MediaPool&) {
                        return edit::deleteClips(t, {clip}, linked);
                    })};
        if (op < 85)
            return {"ripple delete", edit("Ripple delete", [clip, linked](Timeline& t, const MediaPool&) {
                        return edit::rippleDelete(t, {clip}, linked);
                    })};
        if (op < 88) {
            std::vector<Id> selection{clip};
            if (chance(50))
                selection.push_back(randomClip());
            return {"compose", editProject("Compose", [selection](Project& p) {
                        return edit::composeClips(p, selection).has_value();
                    }), true, false, true};
        }
        if (op < 91) {
            const Id compound = randomClip(true);
            if (compound.empty())
                return {"decompose (none)", false};
            return {"decompose",
                    editProject("Decompose", [compound](Project& p) { return edit::decomposeClip(p, compound); }), true,
                    false, true};
        }
        if (op < 92) {
            // Up to five of each kind: dense tracks, where the edits interact, beat many sparse ones.
            if (tl().tracksOfKind(TrackKind::Video).size() >= 5 || tl().tracksOfKind(TrackKind::Audio).size() >= 5)
                return {"add track (enough)", false, false};
            const bool both = !m_asymmetric || chance(50);
            const auto kind = chance(50) ? TrackKind::Video : TrackKind::Audio;
            return {"add track", edit("Add track", [both, kind](Timeline& t, const MediaPool&) {
                        if (both) {
                            edit::addTrack(t, TrackKind::Video);
                            edit::addTrack(t, TrackKind::Audio);
                        } else {
                            edit::addTrack(t, kind);
                        }
                        return true;
                    })};
        }
        if (op < 96 && m_mayLock) {
            const Id trackId = any(tl().tracks).id;
            const bool value = chance(50);
            Op result{"toggle", false, false};
            if (chance(50))
                result.changed = edit("Lock", [trackId, value](Timeline& t, const MediaPool&) {
                    return edit::setTrackLocked(t, trackId, value);
                });
            else
                result.changed = edit("Sync lock", [trackId, value](Timeline& t, const MediaPool&) {
                    return edit::setTrackSyncLock(t, trackId, value);
                });
            m_syncGuaranteed = false;
            return result;
        }
        if (m_stack.canUndo() && chance(70)) {
            m_stack.undo();
            return {"undo", true, false, false, true};
        }
        if (m_stack.canRedo()) {
            m_stack.redo();
            return {"redo", true, false, false, true};
        }
        return {"nothing", false, false};
    }

    void check(const Timeline& before, const Op& op, int step)
    {
        std::string problems;
        // Timeline edits only touch the current timeline; the nested ones are checked when they may change.
        for (const Timeline& t : m_project.timelines) {
            if (!op.projectWide && t.id != m_project.currentTimeline)
                continue;
            problems += edit::checkInvariants(t, m_project.media);
            if (m_syncGuaranteed)
                problems += edit::checkSync(t);
        }
        if (!m_asymmetric)
            problems += strictAlignment(tl());
        if (op.edit) {
            for (const Track& old : before.tracks) {
                const Track* now = tl().track(old.id);
                if (old.locked && (!now || !(*now == old)))
                    problems += "locked track " + old.name + " changed\n";
            }
        }
        if (!problems.empty()) {
            FAIL("step " << step << " (" << op.name << "):\n" << problems << "before:\n"
                         << describeText(before, m_project.media) << "after:\n"
                         << describeText(tl(), m_project.media));
        }
    }

    std::mt19937_64 m_rng;
    bool m_asymmetric;
    ScopedIdSequence m_ids{"p"}; // reproducible failures
    Project m_project = makeEmptyProject();
    Project m_initial;
    CommandStack m_stack{m_project};
    std::vector<MediaItem> m_media;
    bool m_mayLock = false;
    bool m_syncGuaranteed = true;
    int m_applied = 0;
    int m_trims = 0;
    std::map<std::string, int> m_appliedBy;
};

// Five seeds of 4 500 edits: over 5 000 trims (ripple, roll, slip, slide, plain) really applied in each
// of the two runs, so more than 10 000 in all (spec §12, phase 2).
void runSeeds(std::initializer_list<std::uint64_t> seeds, bool asymmetric)
{
    int trims = 0;
    std::map<std::string, int> applied;
    for (std::uint64_t seed : seeds) {
        INFO("seed " << seed);
        RandomEditor editor(seed, asymmetric);
        editor.run(4500);
        trims += editor.trims();
        for (const auto& [name, n] : editor.appliedBy())
            applied[name] += n;
    }
    std::ostringstream counts;
    for (const auto& [name, n] : applied)
        counts << name << " " << n << "; ";
    INFO("applied: " << counts.str());
    CHECK(trims > 5000);
    // Every kind of edit really happened (not just got refused), many times.
    for (const char* name : {"place", "insert", "overwrite", "replace", "fit to fill", "place on top", "split", "move",
                             "trim", "ripple", "roll", "slip", "slide", "delete", "ripple delete", "compose",
                             "decompose", "undo"})
        CHECK(applied[name] >= 20);
    CHECK(applied["add track"] >= 5); // capped at five tracks of each kind
    // Phase 3 edits.
    for (const char* name : {"append", "insert gap", "delete gaps", "swap"})
        CHECK(applied[name] >= 20);
    // These need empty sync-locked tracks around them, so they succeed rarely on dense timelines.
    CHECK(applied["ripple overwrite"] >= 5);
    CHECK(applied["delete gap"] >= 10);
    if (asymmetric) {
        CHECK(applied["link"] >= 20);
        CHECK(applied["unlink"] >= 20);
    }
    if (std::getenv("VE_PROPERTY_STATS"))
        WARN(counts.str());
}

} // namespace

TEST_CASE("random linked edits keep the timeline valid, aligned and fully undoable", "[property]")
{
    runSeeds({1u, 2u, 3u, 4u, 5u}, false);
}

TEST_CASE("random edits with J/L trims, one-sided media and locks keep the timeline valid and in sync", "[property]")
{
    runSeeds({11u, 12u, 13u, 14u, 15u}, true);
}

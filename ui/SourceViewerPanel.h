#pragma once

#include "core/Model.h"

#include <QWidget>

#include <memory>
#include <optional>

namespace ve {
class PlaybackEngine;
}

class QLabel;
class QToolButton;
class ViewerWidget;
class SourceScrubBar;

// Viewer of one media item from the pool (spec §6.1, phase 3.4), with its own playback engine,
// transport and In/Out marks. Three and four point edits take their source range from here.
// It is a passive panel: the window decides when it is the active viewer (it receives the
// transport keys and the I/O marks) and drives it from its ticker.
class SourceViewerPanel : public QWidget {
    Q_OBJECT

public:
    explicit SourceViewerPanel(QWidget* parent = nullptr);
    ~SourceViewerPanel() override;

    // Loads the media on the timeline's frame grid (nullptr empties the viewer). Marks are cleared.
    void setMedia(const ve::MediaItem* media, ve::Rate rate);
    ve::Id mediaId() const;
    bool hasMedia() const { return m_media.has_value(); }

    void tick(); // picture and timecode, from the window's timer
    void play();
    void pause();
    void togglePlay();
    bool isPlaying() const;
    void seek(ve::Time t);
    void stepFrames(std::int64_t frames);
    void goToStart();
    void goToEnd();
    ve::Time position() const;

    // Marks on the frame grid; the Out includes the frame under the playhead.
    void markIn();
    void markOut();
    void clearMarks();
    std::optional<ve::Time> sourceIn() const { return m_in; }
    std::optional<ve::Time> sourceOut() const { return m_out; }

    void setActive(bool active);
    bool isActive() const { return m_active; }

signals:
    void activated();    // the user clicked in the panel
    void marksChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void updateLabels();

    std::unique_ptr<ve::PlaybackEngine> m_engine;
    ViewerWidget* m_viewer = nullptr;
    SourceScrubBar* m_bar = nullptr;
    QLabel* m_name = nullptr;
    QLabel* m_timecode = nullptr;
    QLabel* m_marks = nullptr;
    QToolButton* m_playButton = nullptr;
    std::optional<ve::MediaItem> m_media;
    ve::Rate m_rate{30, 1};
    ve::Time m_duration;
    std::optional<ve::Time> m_in;
    std::optional<ve::Time> m_out;
    ve::Time m_lastPosition{-1};
    bool m_lastPlaying = false;
    bool m_active = false;
};

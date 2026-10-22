#pragma once
#include "notes.hpp"
#include <QElapsedTimer>
#include <QHash>
#include <QSettings>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTimer>
#include <QWidget>
#include <obs-frontend-api.h>
#include <obs.hpp>

class QAction;
class QActionGroup;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSlider;
class QPropertyAnimation;
class QSpinBox;
class QTextEdit;
class QToolBar;
class QToolButton;

namespace scenepad {
class Remote;
enum class Command { Start, Pause, Toggle, Faster, Slower, Top, InsertTime, ToggleLock, InsertMark, NextMark, ReturnPosition };

class Dock : public QWidget {
public:
    explicit Dock(const QString &settingsFile, QWidget *parent = nullptr);
    ~Dock() override;
    void refreshScene();
    void bindScene(obs_source_t *scene);
    void sceneRenamed(obs_source_t *scene);
    void frontendEvent(obs_frontend_event event);
    bool flush(bool saveSceneReferences = true, bool notify = true);
    NoteStore &notes() { return notes_; }
    QString readHtml(obs_source_t *scene, QString *error = nullptr) const;
    int textLength() const;
    bool noteUnavailable() const { return noteLoadFailed_; }
    void command(Command command);
    void startScroll();
    void pauseScroll();
    void setSpeed(double multiplier);
    bool isScrolling() const { return scrollTimer_.isActive(); }
    bool isCountingDown() const { return countdownTimer_.isActive(); }
    double speed() const;
    double scrollProgress() const;
    bool goToPosition(QString target);
    bool returnToPosition();
    bool canReturnToPosition() const { return returnPosition_ >= 0; }
    void shutdown();
    Remote *remote() const { return remote_; }
    QString currentSceneId() const { return sceneId_; }
    bool isLocked() const { return locked_; }
    void setRemoteEditing(bool editing);
    void setRemoteAvailable(bool available);
    bool applyRemoteHtml(obs_source_t *scene, const QString &html);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void createToolbar();
    void createSearchField();
    void updateSearchMargins();
    void showSearch();
    void showGoTo(bool percentage = false);
    void updateMarkFormat();
    void findText(bool backward = false, bool fromStart = false);
    void invalidateSearch(bool fromStart = false);
    void rebuildSearch();
    void updateSearchStatus();
    void clearSearch();
    void showEditorContextMenu(const QPoint &position);
    void showAbout();
    void scheduleAutoClear();
    void clearExpiredNotes();
    void updateAppearance();
    void updateSceneStyle();
    void updateControlSizes();
    void updateControlLayout();
    void updateReadingBand();
    void updateEditorActions();
    void updateDocumentStatus();
    void updateCharacterStatus();
    void applyFormat(const QTextCharFormat &format);
    void syncFormat();
    void setFormatPainter(bool active, bool continuous = false);
    void paintSelection();
    void clearColorPreview();
    void setLocked(bool locked);
    void setList(int style);
    void indent(int delta);
    void clearFormat();
    void importFile();
    void exportDocument();
    void backup(bool all);
    void restoreBackup();
    void convertToBasic();
    bool replaceHtml(const QString &html, const char *title = "ReplaceTitle", const char *prompt = "ReplacePrompt", bool notify = true);
    void scrollFrame();
    void smoothScrollBy(int delta);
    void beginScroll();
    void updateCountdown();
    void rememberPosition();
    void rememberReturnPosition();
    void clearReturnPosition();
    void updateSideControls();
    void savePreferences();
    void showStatus(const QString &text);
    void showSaveError(const QString &detail);
    QString readTextFile(const QString &path, QString *error) const;

    QSettings preferences_;
    NoteStore notes_;
    Remote *remote_ = nullptr;
    bool remoteEditing_ = false;
    bool noteLoadFailed_ = false;
    bool restoring_ = false;
    bool refreshAfterRestore_ = false;
    bool autoClearReady_ = false;
    bool autoClearAll_ = false;
    int autoClearDays_ = 0;
    QSet<QString> autoClearSelection_;
    QList<OBSWeakSource> autoClearPending_;
    bool locked_ = false;
    OBSWeakSource scene_;
    QString sceneId_;
    QString selectionText_, selectionBackground_;
    QHash<QString, int> positions_;
    QTextEdit *editor_ = nullptr;
    QLabel *sceneLabel_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *saveError_ = nullptr;
    QLabel *documentStatus_ = nullptr;
    QLabel *characters_ = nullptr;
    QLabel *scrollPercent_ = nullptr;
    QLineEdit *searchInput_ = nullptr;
    QLineEdit *goToInput_ = nullptr;
    QLabel *searchStatus_ = nullptr;
    QToolButton *clearSearchButton_ = nullptr;
    QList<QPair<int, int>> searchMatches_;
    bool searchDirty_ = true;
    bool searchFromStart_ = false;
    bool markSearch_ = false;
    QAction *find_ = nullptr;
    QCheckBox *countdown_ = nullptr;
    QToolBar *toolbar_ = nullptr;
    QToolBar *paragraphToolbar_ = nullptr;
    QActionGroup *alignment_ = nullptr;
    QWidget *controls_ = nullptr;
    QWidget *readingControls_ = nullptr;
    QWidget *metrics_ = nullptr;
    QSlider *speedSlider_ = nullptr;
    QDoubleSpinBox *speedBox_ = nullptr;
    QSpinBox *fontSize_ = nullptr;
    QToolButton *playButton_ = nullptr;
    QToolButton *qrButton_ = nullptr;
    QToolButton *returnButton_ = nullptr;
    int returnPosition_ = -1;
    QTextCursor returnCursor_;
    QWidget *readingBand_ = nullptr;
    QAction *readingGuide_ = nullptr;
    QAction *bold_ = nullptr;
    QAction *italic_ = nullptr;
    QAction *underline_ = nullptr;
    QAction *strike_ = nullptr;
    QAction *paint_ = nullptr;
    QTextCharFormat paintFormat_;
    bool paintContinuous_ = false;
    QAction *lock_ = nullptr;
    QTimer saveTimer_;
    QTimer scrollTimer_;
    QPropertyAnimation *smoothScroll_ = nullptr;
    QTimer countdownTimer_;
    QTimer searchTimer_;
    QTimer autoClearTimer_;
    QElapsedTimer countdownClock_;
    QElapsedTimer scrollClock_;
    QElapsedTimer manualScrollClock_;
    double scrollPosition_ = 0;
    bool dirty_ = false;
    bool shuttingDown_ = false;
};
}

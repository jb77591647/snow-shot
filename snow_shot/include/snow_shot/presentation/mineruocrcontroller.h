#ifndef SNOW_SHOT_PRESENTATION_MINERUOCRCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_MINERUOCRCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

#include <memory>

namespace snow_shot {
class MineruApiClient;
}
class QProgressDialog;
class QTextBrowser;
class ScreenshotDisplaySession;
class ScreenshotSelectionModel;
class ScreenshotOverlayCoordinator;

// One-shot cloud OCR pipeline: extracts the current screenshot selection,
// submits it to the MinerU precise parsing API, waits for completion, then
// offers the native result formats (markdown/json/html/docx) for saving.
class MineruOcrController final : public QObject {
    Q_OBJECT

  public:
    struct Context {
        ScreenshotDisplaySession& displaySession;
        ScreenshotSelectionModel& selection;
        ScreenshotOverlayCoordinator& overlayCoordinator;
        // Invoked right before the result window appears so the caller can
        // dismiss the capture session (overlay + toolbar) behind it.
        std::function<void()> endCapture;
    };

    MineruOcrController(Context context, QObject* parent = nullptr);
    ~MineruOcrController() override = default;

    void begin();
    [[nodiscard]] bool busy() const;

  private:
    void finishFailure(const QString& message);
    void handleZipReady(const QString& zipPath);
    void processResultZip(const QString& zipPath);
    void showResultWindow();
    void exportResult(const QString& extension, const QString& filter, const QString& source);

    Context m_context;
    snow_shot::MineruApiClient* m_client = nullptr;
    QProgressDialog* m_progress = nullptr;
    QPointer<QTextBrowser> m_resultView;
    QString m_workDirectory;
    QString m_markdownPath;
    QString m_jsonPath;
    QString m_htmlPath;
    QString m_docxPath;
    bool m_busy = false;
};

#endif // SNOW_SHOT_PRESENTATION_MINERUOCRCONTROLLER_H

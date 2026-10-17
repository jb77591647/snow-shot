#include "snow_shot/presentation/mineruocrcontroller.h"
#include "snow_shot/app/edition.h"
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION

#include "snow_shot/network/mineruapiclient.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotsourceimagecomposer.h"
#include "snow_shot/platform/minizippath.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QUrl>
#include <QStringList>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDirIterator>
#include <QWidget>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace {

bool extractZip(const QString& archivePath, const QString& destination, QString* error) {
    void* reader = mz_zip_reader_create();
    const QByteArray archiveNative = snow_shot::platform::minizipPath(archivePath);
    if (reader == nullptr ||
        mz_zip_reader_open_file(reader, archiveNative.constData()) != MZ_OK) {
        if (error != nullptr)
            *error = QStringLiteral("无法打开 MinerU 结果压缩包");
        if (reader != nullptr)
            mz_zip_reader_delete(&reader);
        return false;
    }
    // mz_zip_reader_goto_next_entry() returns MZ_END_OF_LIST when the archive is
    // exhausted; only treat that sentinel as a clean end, everything else is an error.
    bool ok = true;
    int status = mz_zip_reader_goto_first_entry(reader);
    while (ok && status == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || info == nullptr ||
            info->filename == nullptr) {
            ok = false;
            break;
        }
        const QString name = QDir::fromNativeSeparators(QString::fromUtf8(info->filename));
        if (name.endsWith(u'/')) {
            // Directory entry: create it and move on; it is not an error.
            QDir().mkpath(QDir(destination).filePath(name));
            status = mz_zip_reader_goto_next_entry(reader);
            continue;
        }
        const QStringList nameSegments =
            name.split(u'/', Qt::SkipEmptyParts);
        if (nameSegments.isEmpty() || nameSegments.contains(QLatin1String("..")) ||
            name.startsWith(u'/')) {
            ok = false;
            break;
        }
        const QString outputPath = QDir(destination).filePath(name);
        if (!QDir().mkpath(QFileInfo(outputPath).dir().absolutePath()) ||
            mz_zip_reader_entry_open(reader) != MZ_OK) {
            ok = false;
            break;
        }
        QFile output(outputPath);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            ok = false;
            break;
        }
        char buffer[64 * 1024];
        qint32 bytesRead = 0;
        while ((bytesRead = mz_zip_reader_entry_read(reader, buffer, sizeof(buffer))) > 0) {
            if (output.write(buffer, bytesRead) != bytesRead) {
                ok = false;
                break;
            }
        }
        output.close();
        mz_zip_reader_entry_close(reader);
        if (!ok)
            break;
        status = mz_zip_reader_goto_next_entry(reader);
    }
    mz_zip_reader_delete(&reader);
    if (ok && status != MZ_END_OF_LIST)
        ok = false;
    if (error != nullptr && !ok)
        *error = QStringLiteral("MinerU 结果压缩包解压失败");
    return ok;
}

QWidget* dialogParentFor(ScreenshotOverlayCoordinator& coordinator) {
    if (QWidget* toolbar = coordinator.toolbar()) {
        return toolbar;
    }
    return QApplication::activeWindow();
}

// The screenshot overlay is a fullscreen always-on-top window, so plain message
// dialogs end up behind it. Show every MinerU dialog application-modal on top.
void showTopmostMessage(QMessageBox::Icon icon, const QString& title, const QString& text) {
    QMessageBox box(icon, title, text, QMessageBox::Ok);
    box.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    box.setWindowModality(Qt::ApplicationModal);
    box.exec();
}

// Lightweight trace log for diagnosing the post-download flow in the field.
void trace(const QString& message) {
    QFile log(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                  .filePath(QStringLiteral("snow-shot-mineru.log")));
    if (log.open(QIODevice::Append | QIODevice::Text)) {
        log.write(QStringLiteral("[%1] %2\n")
                      .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz")),
                           message)
                      .toUtf8());
    }
}

// Scales the original screenshot to fit the label, preserving aspect ratio,
// re-rendering whenever the pane is resized.
class FitPixmapLabel final : public QLabel {
  public:
    explicit FitPixmapLabel(QWidget* parent = nullptr) : QLabel(parent) {}

    void setOriginalPixmap(const QPixmap& pixmap) {
        m_original = pixmap;
        updateScaled();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        updateScaled();
    }

  private:
    void updateScaled() {
        if (m_original.isNull()) {
            return;
        }
        setPixmap(m_original.scaled(size(), Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation));
    }

    QPixmap m_original;
};

} // namespace

MineruOcrController::MineruOcrController(Context context, QObject* parent)
    : QObject(parent), m_context(context) {
    m_client = new snow_shot::MineruApiClient(this);
    connect(m_client, &snow_shot::MineruApiClient::stageChanged, this, [this](const QString& stage) {
        if (m_progress != nullptr)
            m_progress->setLabelText(stage);
    });
    connect(m_client, &snow_shot::MineruApiClient::failed, this, &MineruOcrController::finishFailure);
    connect(m_client, &snow_shot::MineruApiClient::zipReady, this,
            &MineruOcrController::handleZipReady);
}



bool MineruOcrController::busy() const {
    return m_busy;
}

void MineruOcrController::begin() {
    if (m_busy) {
        return;
    }
    const snow_shot::storage::ApiConfigurationSettings apiConfiguration;
    const QString token = apiConfiguration.mineruToken();
    if (token.isEmpty()) {
        showTopmostMessage(QMessageBox::Information, QStringLiteral("MinerU 文档识别"),
                           QStringLiteral("请先在「设置 → API → MinerU 文档识别」中配置 API Token。"));
        return;
    }
    const QRect selection = m_context.selection.pixelSelection();
    if (selection.width() < 1 || selection.height() < 1) {
        showTopmostMessage(QMessageBox::Information, QStringLiteral("MinerU 文档识别"),
                           QStringLiteral("请先框选需要识别的截图区域。"));
        return;
    }

    QImage image = composeScreenshotSourceSelection(m_context.displaySession, selection);
    if (image.isNull()) {
        showTopmostMessage(QMessageBox::Warning, QStringLiteral("MinerU 文档识别"),
                           QStringLiteral("无法获取截图区域图像。"));
        return;
    }

    m_workDirectory = QDir(QDir::tempPath())
                          .filePath(QStringLiteral("snow-shot-mineru-%1")
                                        .arg(QDateTime::currentMSecsSinceEpoch()));
    if (!QDir().mkpath(m_workDirectory)) {
        showTopmostMessage(QMessageBox::Warning, QStringLiteral("MinerU 文档识别"),
                           QStringLiteral("无法创建临时工作目录。"));
        return;
    }
    const QString imagePath = QDir(m_workDirectory).filePath(QStringLiteral("screenshot.png"));
    if (!image.save(imagePath, "png")) {
        showTopmostMessage(QMessageBox::Warning, QStringLiteral("MinerU 文档识别"),
                           QStringLiteral("截图区域图像保存失败。"));
        return;
    }

    snow_shot::MineruApiClient::Params params;
    params.token = token;
    params.language = apiConfiguration.mineruLanguage();

    m_progress = new QProgressDialog(QStringLiteral("正在提交 MinerU 解析任务…"),
                                     QStringLiteral("取消"), 0, 0,
                                     dialogParentFor(m_context.overlayCoordinator));
    m_progress->setWindowTitle(QStringLiteral("MinerU 文档识别"));
    // The screenshot overlay is a fullscreen topmost window; keep the progress dialog
    // above it so the user gets immediate feedback after clicking the toolbar button.
    m_progress->setWindowModality(Qt::ApplicationModal);
    m_progress->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    m_progress->setMinimumDuration(0);
    m_progress->setValue(0);
    connect(m_progress, &QProgressDialog::canceled, this, [this]() {
        m_client->cancel();
        if (m_progress != nullptr) {
            m_progress->deleteLater();
            m_progress = nullptr;
        }
        m_busy = false;
    });
    m_busy = true;
    trace(QStringLiteral("begin: image %1 bytes").arg(QFileInfo(imagePath).size()));
    m_client->start(params, imagePath);
}

void MineruOcrController::finishFailure(const QString& message) {
    trace(QStringLiteral("failure: %1").arg(message));
    m_busy = false;
    if (m_progress != nullptr) {
        m_progress->deleteLater();
        m_progress = nullptr;
    }
    showTopmostMessage(QMessageBox::Warning, QStringLiteral("MinerU 文档识别"), message);
}

void MineruOcrController::handleZipReady(const QString& zipPath) {
    trace(QStringLiteral("zip ready: %1 (%2 bytes)")
              .arg(zipPath)
              .arg(QFileInfo(zipPath).size()));
    if (m_progress != nullptr) {
        m_progress->setLabelText(QStringLiteral("正在解压结果包…"));
    }
    // Leave the network reply handler first: the modal progress dialog is still
    // alive here, and heavy work + window creation must not run reentrantly.
    const QString zip = zipPath;
    QMetaObject::invokeMethod(this, [this, zip]() { processResultZip(zip); },
                              Qt::QueuedConnection);
}

void MineruOcrController::processResultZip(const QString& zipPath) {
    if (m_progress != nullptr) {
        m_progress->deleteLater();
        m_progress = nullptr;
    }
    QString error;
    const QString extractRoot = QDir(m_workDirectory).filePath(QStringLiteral("result"));
    if (!QDir().mkpath(extractRoot) || !extractZip(zipPath, extractRoot, &error)) {
        trace(QStringLiteral("extract failed: %1").arg(error));
        finishFailure(error);
        return;
    }
    trace(QStringLiteral("extract ok"));
    // Classify the MinerU result package contents (search recursively in case the
    // archive nests everything below a top-level directory).
    int fileCount = 0;
    QDirIterator extracted(extractRoot, QDir::Files, QDirIterator::Subdirectories);
    while (extracted.hasNext()) {
        const QString path = extracted.next();
        ++fileCount;
        const QString name = QFileInfo(path).fileName().toLower();
        if (name == QLatin1String("full.md")) {
            m_markdownPath = path;
        } else if (name.endsWith(QLatin1String("_content_list.json"))) {
            m_jsonPath = path;
        } else if (name.endsWith(QLatin1String(".docx"))) {
            m_docxPath = path;
        } else if (name.endsWith(QLatin1String(".html"))) {
            m_htmlPath = path;
        }
    }
    trace(QStringLiteral("classified %1 files: md=%2 json=%3 html=%4 docx=%5")
              .arg(fileCount)
              .arg(!m_markdownPath.isEmpty())
              .arg(!m_jsonPath.isEmpty())
              .arg(!m_htmlPath.isEmpty())
              .arg(!m_docxPath.isEmpty()));
    showResultWindow();
}

void MineruOcrController::showResultWindow() {
    trace(QStringLiteral("showing result window"));
    // Dismiss the capture session (overlay + toolbar) behind the result window.
    if (m_context.endCapture) {
        m_context.endCapture();
    }
    const QPixmap originalScreenshot(
        QDir(m_workDirectory).filePath(QStringLiteral("screenshot.png")));

    // Geometry: the left (screenshot) pane width follows the screenshot aspect
    // ratio, and no pane may exceed half of the screen width — the widest case
    // splits the screen evenly between the two panes.
    const QScreen* geometryScreen = QGuiApplication::screenAt(QCursor::pos());
    if (geometryScreen == nullptr) {
        geometryScreen = QGuiApplication::primaryScreen();
    }
    const QRect available =
        (geometryScreen != nullptr ? geometryScreen : QGuiApplication::primaryScreen())
            ->availableGeometry();
    const int halfWidth = qMax(320, available.width() / 2);
    const int windowHeight = qMin(680, qMax(420, available.height() - 80));
    // Rough in-pane content height: window minus margins, spacing and button row.
    const int contentHeight = qMax(240, windowHeight - 12 * 2 - 10 * 2 - 40);
    int leftWidth = 368;
    if (!originalScreenshot.isNull()) {
        const double aspect =
            double(originalScreenshot.width()) / double(originalScreenshot.height());
        leftWidth = qRound(contentHeight * aspect);
    }
    leftWidth = qBound(220, leftWidth, halfWidth - 120);
    const int rightWidth = halfWidth;

    // A fully independent top-level window: the screenshot overlay is a fullscreen
    // always-on-top tool window, and parenting to it (or its toolbar) can leave the
    // result hidden behind or suppressed with it.
    auto* window = new QWidget(nullptr, Qt::Window | Qt::WindowStaysOnTopHint);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(QStringLiteral("MinerU 识别结果 - Snow Shot"));
    const int splitterHandle =
        window->style()->pixelMetric(QStyle::PM_SplitterWidth);
    window->resize(qMin(available.width() - 40, leftWidth + rightWidth + splitterHandle + 24),
                   windowHeight);
    trace(QStringLiteral("window %1x%2, left=%3 right=%4")
              .arg(window->width())
              .arg(window->height())
              .arg(leftWidth)
              .arg(rightWidth));

    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    // Side-by-side comparison: original screenshot on the left (auto-fit
    // scaling), recognized document on the right, resizable via the splitter.
    auto* splitter = new QSplitter(Qt::Horizontal, window);
    splitter->setChildrenCollapsible(false);

    auto* screenshotLabel = new FitPixmapLabel(splitter);
    screenshotLabel->setAlignment(Qt::AlignCenter);
    screenshotLabel->setMinimumWidth(220);
    if (!originalScreenshot.isNull()) {
        screenshotLabel->setOriginalPixmap(originalScreenshot);
    } else {
        screenshotLabel->setText(QStringLiteral("（无原始截图）"));
    }
    splitter->addWidget(screenshotLabel);

    m_resultView = new QTextBrowser(splitter);
    m_resultView->setOpenExternalLinks(false);
    if (!m_htmlPath.isEmpty()) {
        // Native MinerU HTML renders tables/formulas/images exactly like the
        // exported HTML, so the preview matches the result.
        QFile htmlFile(m_htmlPath);
        if (htmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // Qt's rich-text engine ignores <style> CSS, so MinerU tables lose
            // their borders and become invisible. Inject explicit table
            // attributes instead.
            QString html = QString::fromUtf8(htmlFile.readAll());
            static const QRegularExpression tableWithoutBorder(
                QStringLiteral("<table(?![^>]*\\bborder=)[^>]*"),
                QRegularExpression::CaseInsensitiveOption);
            html.replace(tableWithoutBorder,
                         QStringLiteral("<table border=\"1\" cellspacing=\"0\" "
                                        "cellpadding=\"4\""));
            static const QRegularExpression thWithoutBg(
                QStringLiteral("<th(?![^>]*\\bbgcolor=)[^>]*"),
                QRegularExpression::CaseInsensitiveOption);
            html.replace(thWithoutBg,
                         QStringLiteral("<th bgcolor=\"#f0f0f0\""));
            // Resolve relative image references (images/…) against the package root.
            m_resultView->document()->setBaseUrl(
                QUrl::fromLocalFile(QFileInfo(m_htmlPath).absolutePath() + u'/'));
            m_resultView->document()->setHtml(html);
            trace(QStringLiteral("preview via html: %1").arg(m_htmlPath));
        }
    } else if (!m_markdownPath.isEmpty()) {
        m_resultView->setSearchPaths(
            QStringList{QFileInfo(m_markdownPath).absolutePath(),
                        QDir(m_workDirectory).filePath(QStringLiteral("result"))});
        QFile markdownFile(m_markdownPath);
        if (markdownFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            m_resultView->setMarkdown(QString::fromUtf8(markdownFile.readAll()));
        }
    } else if (!m_jsonPath.isEmpty()) {
        QFile jsonFile(m_jsonPath);
        if (jsonFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            m_resultView->setPlainText(QString::fromUtf8(jsonFile.readAll()));
        }
    } else {
        m_resultView->setPlainText(QStringLiteral("结果包中未找到可预览的文档。"));
    }
    splitter->addWidget(m_resultView);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    // Apply the aspect-derived pane widths.
    splitter->setSizes({leftWidth, rightWidth});
    layout->addWidget(splitter, 1);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);
    const struct ExportEntry {
        const char* label;
        const char* extension;
        const char* filter;
        const QString* source;
    } exports[] = {
        {"保存为 Markdown", ".md", "Markdown 文档 (*.md)", &m_markdownPath},
        {"保存为 JSON", ".json", "JSON 文档 (*.json)", &m_jsonPath},
        {"保存为 HTML", ".html", "HTML 文档 (*.html)", &m_htmlPath},
        {"保存为 Word", ".docx", "Word 文档 (*.docx)", &m_docxPath},
    };
    for (const auto& entry : exports) {
        if (entry.source->isEmpty()) {
            continue;
        }
        auto* button = new QPushButton(QString::fromUtf8(entry.label), window);
        const QString source = *entry.source;
        const QString extension = QString::fromUtf8(entry.extension);
        const QString filter = QString::fromUtf8(entry.filter);
        connect(button, &QPushButton::clicked, this,
                [this, source, extension, filter]() { exportResult(extension, filter, source); });
        buttonRow->addWidget(button);
    }
    buttonRow->addStretch(1);
    auto* closeButton = new QPushButton(QStringLiteral("关闭"), window);
    connect(closeButton, &QPushButton::clicked, window, &QWidget::close);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    // Release the busy flag whenever the window goes away, however it is closed.
    connect(window, &QWidget::destroyed, this, [this]() { m_busy = false; });

    window->show();
    window->raise();
    window->activateWindow();
    // Center-ish on the cursor but clamp fully inside the screen, so the window
    // can never end up half off-screen next to the bottom toolbar.
    QPoint position = QCursor::pos() - QPoint(window->width() / 2, window->height() / 3);
    position.setX(qBound(available.left(), position.x(), available.right() - window->width()));
    position.setY(qBound(available.top(), position.y(), available.bottom() - window->height()));
    window->move(position);
    trace(QStringLiteral("result window shown at %1,%2").arg(position.x()).arg(position.y()));
}

void MineruOcrController::exportResult(const QString& extension, const QString& filter,
                                        const QString& source) {
    QWidget* dialogParent = (m_resultView != nullptr) ? m_resultView->window()
                                                      : dialogParentFor(m_context.overlayCoordinator);
    QFileDialog dialog(dialogParent, QStringLiteral("保存 MinerU 识别结果"),
                       QStringLiteral("mineru-result") + extension, filter);
    dialog.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    if (dialog.exec() != QFileDialog::Accepted) {
        return;
    }
    const QStringList selected = dialog.selectedFiles();
    if (selected.isEmpty()) {
        return;
    }
    const QString target = selected.first();
    QFile::remove(target);
    if (!QFile::copy(source, target)) {
        showTopmostMessage(QMessageBox::Warning, QStringLiteral("MinerU 文档识别"),
                           QStringLiteral("保存失败：%1").arg(target));
    }
}

#endif // SNOW_SHOT_ENABLE_TEXT_TRANSLATION

#ifndef SNOW_SHOT_NETWORK_MINERUAPICLIENT_H
#define SNOW_SHOT_NETWORK_MINERUAPICLIENT_H

#include <QNetworkRequest>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace snow_shot {

// Client for the MinerU precise parsing API (https://mineru.net/apiManage/docs).
// Flow for a local image: request a signed batch upload URL, PUT the file,
// poll the batch result until done, then download the result zip.
class MineruApiClient final : public QObject {
    Q_OBJECT

  public:
    struct Params {
        QString token;
        QString language = QStringLiteral("ch");
        QString modelVersion = QStringLiteral("pipeline");
        bool enableTable = true;
        bool enableFormula = true;
        bool forceOcr = true;
        // Extra native formats MinerU should generate in the result package.
        QStringList extraFormats = {QStringLiteral("docx"), QStringLiteral("html")};
    };

    explicit MineruApiClient(QObject* parent = nullptr);
    ~MineruApiClient() override;

    void start(const Params& params, const QString& imageFilePath);
    void cancel();
    [[nodiscard]] bool busy() const;

  signals:
    void stageChanged(const QString& stage);
    void failed(const QString& message);
    void zipReady(const QString& localZipPath);

  private:
    enum class Phase { Idle, RequestingUpload, Uploading, Polling, Downloading };

    void requestUploadUrl();
    void uploadFile();
    void pollResult();
    void downloadZip(const QUrl& url);
    void finishFailure(const QString& message);
    [[nodiscard]] QNetworkRequest authenticatedRequest(const QUrl& url) const;

    QNetworkAccessManager* m_network = nullptr;
    QTimer* m_pollTimer = nullptr;
    Params m_params;
    QString m_imagePath;
    QString m_batchId;
    QString m_batchUploadUrl;
    QString m_zipPath;
    qint64 m_pollDeadlineMs = 0;
    Phase m_phase = Phase::Idle;
    bool m_aborted = false;
};

} // namespace snow_shot

#endif // SNOW_SHOT_NETWORK_MINERUAPICLIENT_H

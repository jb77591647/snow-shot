#include "snow_shot/network/mineruapiclient.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace snow_shot {
namespace {
constexpr const char* kBaseUrl = "https://mineru.net";
constexpr int kPollIntervalMs = 3000;
constexpr int kPollTimeoutMs = 180000;
constexpr int kRequestTimeoutMs = 60000;
constexpr qint64 kMaxImageBytes = 100 * 1024 * 1024;

QString apiErrorMessage(int code, const QString& message) {
    QString hint;
    switch (code) {
    case -60018:
        hint = QStringLiteral("（每日解析任务数量已达上限，请明日再试）");
        break;
    case -60005:
        hint = QStringLiteral("（文件过大）");
        break;
    case -60006:
        hint = QStringLiteral("（页数超过限制）");
        break;
    case 429:
    case -60019:
        hint = QStringLiteral("（请求过于频繁或额度不足，请稍后再试）");
        break;
    default:
        break;
    }
    return QStringLiteral("MinerU API 错误 %1：%2%3").arg(code).arg(message, hint);
}
} // namespace

MineruApiClient::MineruApiClient(QObject* parent) : QObject(parent) {
    m_network = new QNetworkAccessManager(this);
    m_network->setTransferTimeout(kRequestTimeoutMs);
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &MineruApiClient::pollResult);
}

MineruApiClient::~MineruApiClient() {
    m_aborted = true;
}

bool MineruApiClient::busy() const {
    return m_phase != Phase::Idle;
}

QNetworkRequest MineruApiClient::authenticatedRequest(const QUrl& url) const {
    QNetworkRequest request(url);
    request.setRawHeader(QByteArrayLiteral("Authorization"),
                         "Bearer " + m_params.token.toUtf8());
    request.setRawHeader(QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

void MineruApiClient::start(const Params& params, const QString& imageFilePath) {
    if (m_phase != Phase::Idle) {
        emit failed(QStringLiteral("已有解析任务在进行中"));
        return;
    }
    QFile file(imageFilePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) {
        emit failed(QStringLiteral("无法读取截图临时文件"));
        return;
    }
    if (file.size() > kMaxImageBytes) {
        emit failed(QStringLiteral("截图文件超过 MinerU 100MB 大小限制"));
        return;
    }
    m_params = params;
    m_imagePath = imageFilePath;
    m_aborted = false;
    m_phase = Phase::RequestingUpload;
    emit stageChanged(QStringLiteral("正在申请 MinerU 上传链接…"));
    requestUploadUrl();
}

void MineruApiClient::cancel() {
    if (m_phase == Phase::Idle) {
        return;
    }
    m_aborted = true;
    m_pollTimer->stop();
    m_phase = Phase::Idle;
}

void MineruApiClient::requestUploadUrl() {
    QJsonObject fileEntry;
    fileEntry.insert(QStringLiteral("name"), QStringLiteral("screenshot.png"));
    fileEntry.insert(QStringLiteral("is_ocr"), m_params.forceOcr);
    fileEntry.insert(QStringLiteral("data_id"),
                     QString::number(QDateTime::currentMSecsSinceEpoch()));
    QJsonObject body;
    body.insert(QStringLiteral("model_version"), m_params.modelVersion);
    body.insert(QStringLiteral("language"), m_params.language);
    body.insert(QStringLiteral("enable_table"), m_params.enableTable);
    body.insert(QStringLiteral("enable_formula"), m_params.enableFormula);
    QJsonArray extraFormats;
    for (const QString& format : m_params.extraFormats) {
        extraFormats.append(format);
    }
    body.insert(QStringLiteral("extra_formats"), extraFormats);
    body.insert(QStringLiteral("files"), QJsonArray{fileEntry});

    QNetworkReply* reply = m_network->post(authenticatedRequest(
        QUrl(QString::fromLatin1(kBaseUrl) + QStringLiteral("/api/v4/file-urls/batch"))),
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_aborted || m_phase != Phase::RequestingUpload) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            finishFailure(QStringLiteral("申请上传链接失败：%1").arg(reply->errorString()));
            return;
        }
        const auto document = QJsonDocument::fromJson(reply->readAll());
        const auto object = document.object();
        const int code = object.value(QStringLiteral("code")).toInt(-1);
        if (code != 0) {
            finishFailure(apiErrorMessage(code,
                                           object.value(QStringLiteral("msg")).toString()));
            return;
        }
        const auto data = object.value(QStringLiteral("data")).toObject();
        m_batchId = data.value(QStringLiteral("batch_id")).toString();
        const auto fileUrls = data.value(QStringLiteral("file_urls")).toArray();
        if (m_batchId.isEmpty() || fileUrls.isEmpty()) {
            finishFailure(QStringLiteral("MinerU 返回数据缺少 batch_id 或上传链接"));
            return;
        }
        m_batchUploadUrl = fileUrls.at(0).toString();
        m_phase = Phase::Uploading;
        emit stageChanged(QStringLiteral("正在上传截图…"));
        uploadFile();
    });
}

void MineruApiClient::uploadFile() {
    QFile* file = new QFile(m_imagePath, this);
    if (!file->open(QIODevice::ReadOnly)) {
        finishFailure(QStringLiteral("无法读取截图临时文件"));
        return;
    }
    QNetworkRequest request{QUrl(m_batchUploadUrl)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_network->put(request, file);
    connect(reply, &QNetworkReply::finished, this, [this, reply, file] {
        file->deleteLater();
        reply->deleteLater();
        if (m_aborted || m_phase != Phase::Uploading) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            finishFailure(QStringLiteral("上传截图失败：%1").arg(reply->errorString()));
            return;
        }
        m_phase = Phase::Polling;
        m_pollDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kPollTimeoutMs;
        emit stageChanged(QStringLiteral("MinerU 正在解析，请稍候…"));
        m_pollTimer->start();
    });
}

void MineruApiClient::pollResult() {
    if (m_phase != Phase::Polling) {
        m_pollTimer->stop();
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() > m_pollDeadlineMs) {
        m_pollTimer->stop();
        finishFailure(QStringLiteral("解析超时（180 秒），请稍后重试"));
        return;
    }
    QNetworkReply* reply = m_network->get(authenticatedRequest(
        QUrl(QString::fromLatin1(kBaseUrl) + QStringLiteral("/api/v4/extract-results/batch/") +
             m_batchId)));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_aborted || m_phase != Phase::Polling) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            // Transient network errors: keep polling until the deadline.
            return;
        }
        const auto object = QJsonDocument::fromJson(reply->readAll()).object();
        const int code = object.value(QStringLiteral("code")).toInt(-1);
        if (code != 0) {
            m_pollTimer->stop();
            finishFailure(apiErrorMessage(code,
                                           object.value(QStringLiteral("msg")).toString()));
            return;
        }
        const auto extractResult =
            object.value(QStringLiteral("data")).toObject()
                .value(QStringLiteral("extract_result")).toArray();
        if (extractResult.isEmpty()) {
            return;
        }
        const auto entry = extractResult.at(0).toObject();
        const QString state = entry.value(QStringLiteral("state")).toString();
        if (state == QLatin1String("done")) {
            m_pollTimer->stop();
            const QString zipUrl = entry.value(QStringLiteral("full_zip_url")).toString();
            if (zipUrl.isEmpty()) {
                finishFailure(QStringLiteral("MinerU 未返回结果包地址"));
                return;
            }
            m_phase = Phase::Downloading;
            emit stageChanged(QStringLiteral("正在下载解析结果…"));
            downloadZip(QUrl(zipUrl));
        } else if (state == QLatin1String("failed")) {
            m_pollTimer->stop();
            finishFailure(QStringLiteral("MinerU 解析失败：") +
                          entry.value(QStringLiteral("err_msg")).toString());
        }
        // pending / running / converting: keep waiting.
    });
}

void MineruApiClient::downloadZip(const QUrl& url) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_aborted || m_phase != Phase::Downloading) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            finishFailure(QStringLiteral("下载结果包失败：%1").arg(reply->errorString()));
            return;
        }
        const QString tempRoot =
            QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString zipPath = QDir(tempRoot).filePath(
            QStringLiteral("snow-shot-mineru-%1.zip")
                .arg(QDateTime::currentMSecsSinceEpoch()));
        QFile output(zipPath);
        if (!output.open(QIODevice::WriteOnly)) {
            finishFailure(QStringLiteral("无法写入临时结果文件"));
            return;
        }
        output.write(reply->readAll());
        output.close();
        m_phase = Phase::Idle;
        emit zipReady(zipPath);
    });
}

void MineruApiClient::finishFailure(const QString& message) {
    m_phase = Phase::Idle;
    if (!m_aborted) {
        emit failed(message);
    }
}

} // namespace snow_shot

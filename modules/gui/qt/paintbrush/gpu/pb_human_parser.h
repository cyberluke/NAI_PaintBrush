/*****************************************************************************
 * pb_human_parser.h : MinGW-facing FASHN human parser runtime
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * The service layer speaks this API. It forwards every call to
 * pb_human_parser_cuda.dll (MSVC + ONNX Runtime + CUDA) through the narrow C
 * ABI in human_parser_cuda_api.h. When the optional DLL or its dependency
 * closure is missing, the service reports exactly what is missing and the
 * rest of Paintbrush keeps working.
 *
 * This layer is deliberately subject-identity agnostic: it returns semantic
 * probabilities (class map + garment/hair/skin/accessory planes). The service
 * composes them with the existing SAM3 subject alpha; the parser never
 * replaces that alpha.
 *****************************************************************************/

#ifndef PB_HUMAN_PARSER_H
#define PB_HUMAN_PARSER_H

#include <QImage>
#include <QString>
#include <QVector>

namespace paintbrush {
namespace human_parser {

struct Capabilities
{
    bool runtimePresent = false; /* pb_human_parser_cuda.dll could be loaded */
    bool cudaAvailable = false;
    bool cpuAvailable = false;
    int cudaDeviceCount = 0;
    QString ortVersion;
    QString deviceName;
    QString backend;
    QString detail; /* why it is unavailable, if it is */
};

struct SessionConfig
{
    QString modelPath;
    QString manifestPath;
    QString profileDir;
    int device = 0; /* 0 auto, 1 gpu required, 2 cpu */
    int intraThreads = 0;
    bool requireGpuOperators = true;
    bool verifyModelHash = true;
    bool exportClassProbabilities = false;
};

struct RunInfo
{
    double preprocessMs = -1.0;
    double inferenceMs = -1.0;
    double postprocessMs = -1.0;
    double totalMs = -1.0;
    int inputWidth = 0;
    int inputHeight = 0;
    int gridWidth = 0;
    int gridHeight = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    bool usedGpu = false;
    int cudaNodes = 0;
    int cpuNodes = 0;
    QString backend;
    QString modelSha256;
};

struct Masks
{
    /* Grayscale8, source size. classMap holds the argmax index 0..17. */
    QImage classMap;
    /* Grayscale8 semantic probabilities 0..255, source size. */
    QImage garment;
    QImage hair;
    QImage skin;
    QImage accessory;
    /* Optional: 18 source-size Grayscale8 probability planes, class order. */
    QVector<QImage> classProbabilities;
    /* Counts from the hard argmax. */
    QVector<quint64> classPixels;     /* 18, source size */
    QVector<quint64> classPixelsGrid; /* 18, 96x144  */
    QVector<quint64> groupPixels;     /* 6, source size, API group order */
    QVector<quint64> groupProbabilitySum; /* 6, sum of 0..255 planes */

    void clear()
    {
        classMap = QImage();
        garment = QImage();
        hair = QImage();
        skin = QImage();
        accessory = QImage();
        classProbabilities.clear();
        classPixels.clear();
        classPixelsGrid.clear();
        groupPixels.clear();
        groupProbabilitySum.clear();
    }
};

/* Process-wide runtime. One persistent session is shared by the service; ORT
 * sessions are internally thread-safe for Run but the DLL serializes runs. */
class Runtime
{
public:
    static Capabilities probe();
    static bool dllPath(QString *path, QString *detail);

    bool load(const SessionConfig &config, QString *error);
    void unload();
    bool isLoaded() const { return m_session != nullptr; }
    RunInfo backendInfo() const;
    QString backend() const { return m_backend; }
    QString loadError() const { return m_loadError; }

    /* `rgb` may be any QImage format; it is converted to RGB888 internally.
     * The returned masks are source-sized. */
    bool parse(const QImage &rgb, Masks *masks, RunInfo *info, QString *error);

    /* Thread-safe: asks a Run()/postprocess in flight to terminate. */
    void cancel();

    void setExportClassProbabilities(bool enabled)
    {
        m_exportClassProbabilities = enabled;
    }
    bool exportClassProbabilities() const
    {
        return m_exportClassProbabilities;
    }

private:
    void *m_session = nullptr;
    QString m_backend;
    QString m_loadError;
    int m_device = 0;
    QString m_modelPath;
    bool m_exportClassProbabilities = false;
};

Runtime &sharedRuntime();

} // namespace human_parser
} // namespace paintbrush

#endif // PB_HUMAN_PARSER_H

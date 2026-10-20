#pragma once

#include "media/VideoFrame.h"

#include <QRhiWidget>
#include <rhi/qrhi.h>

#include <array>
#include <memory>

// GPU viewer on Qt RHI (Direct3D 11/12 on Windows, Metal on macOS).
//   pass 1 "process": Y'CbCr planes (R8/R16 textures) -> RGB in an RGBA16F render target
//   pass 2 "display": that float image, aspect-fitted into the widget
// The float intermediate is where effects and colour management will plug in.
class ViewerWidget : public QRhiWidget {
    Q_OBJECT

public:
    explicit ViewerWidget(QWidget* parent = nullptr);
    ~ViewerWidget() override;

    // nullptr shows black.
    void setFrame(ve::VideoFramePtr frame);

signals:
    void backendReady(const QString& name);

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    bool ensureFrameResources(const ve::VideoFrame& frame);
    bool ensureDisplayPipeline();
    QRhiGraphicsPipeline* makePipeline(const QShader& fragment, QRhiShaderResourceBindings* srb,
                                       QRhiRenderPassDescriptor* rp);
    void resetAll();

    QRhi* m_rhi = nullptr;
    QShader m_quadVs;
    QShader m_yuvFs;
    QShader m_displayFs;

    std::unique_ptr<QRhiBuffer> m_vertices;
    std::unique_ptr<QRhiBuffer> m_processQuad;
    std::unique_ptr<QRhiBuffer> m_displayQuad;
    std::unique_ptr<QRhiBuffer> m_colorParams;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::array<std::unique_ptr<QRhiTexture>, 3> m_planes;
    std::unique_ptr<QRhiTexture> m_processTexture;
    std::unique_ptr<QRhiTextureRenderTarget> m_processTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_processPass;
    std::unique_ptr<QRhiShaderResourceBindings> m_processBindings;
    std::unique_ptr<QRhiShaderResourceBindings> m_displayBindings;
    std::unique_ptr<QRhiGraphicsPipeline> m_processPipeline;
    std::unique_ptr<QRhiGraphicsPipeline> m_displayPipeline;
    bool m_verticesUploaded = false;

    ve::VideoFramePtr m_frame;
    bool m_frameDirty = false;
    bool m_hasImage = false;
    bool m_needProcess = false;
    QSize m_frameSize;
    ve::PixelLayout m_layout = ve::PixelLayout::YUV420P8;
};

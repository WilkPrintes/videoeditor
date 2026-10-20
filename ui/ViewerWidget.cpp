#include "ui/ViewerWidget.h"

#include <QFile>
#include <QtGlobal>

namespace {

// Triangle strip: position (NDC) + texcoord, top-left texcoord = first image row.
const float kQuad[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

struct QuadParams {
    float rect[4];  // scale xy, offset zw
    float flags[4]; // x: flip v
};

struct ColorParams {
    float row0[4];
    float row1[4];
    float row2[4];
    float offset[4];
};

QShader loadShader(const QString& path)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
        return QShader::fromSerialized(f.readAll());
    qWarning("Missing shader %s", qPrintable(path));
    return {};
}

ColorParams colorParamsFor(const ve::VideoFrame& f)
{
    double kr = 0.2126, kb = 0.0722; // BT.709
    if (f.matrix == ve::ColorMatrix::BT601) {
        kr = 0.299;
        kb = 0.114;
    } else if (f.matrix == ve::ColorMatrix::BT2020) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const double kg = 1.0 - kr - kb;
    const bool sixteen = f.layout == ve::PixelLayout::YUV420P16;
    const double chromaZero = sixteen ? 32768.0 / 65535.0 : 128.0 / 255.0;
    const double ys = f.fullRange ? 1.0 : 255.0 / 219.0;
    const double cs = f.fullRange ? 1.0 : 255.0 / 224.0;
    const double yo = f.fullRange ? 0.0 : 16.0 / 255.0;

    ColorParams p{};
    auto set = [](float* row, double a, double b, double c) {
        row[0] = float(a);
        row[1] = float(b);
        row[2] = float(c);
        row[3] = 0.0f;
    };
    set(p.row0, ys, 0.0, 2.0 * (1.0 - kr) * cs);
    set(p.row1, ys, -2.0 * kb * (1.0 - kb) / kg * cs, -2.0 * kr * (1.0 - kr) / kg * cs);
    set(p.row2, ys, 2.0 * (1.0 - kb) * cs, 0.0);
    set(p.offset, yo, chromaZero, chromaZero);
    return p;
}

QRhiWidget::Api preferredApi()
{
    const QByteArray env = qgetenv("VE_RHI").toLower();
    if (env == "d3d12")
        return QRhiWidget::Api::Direct3D12;
    if (env == "d3d11")
        return QRhiWidget::Api::Direct3D11;
    if (env == "vulkan")
        return QRhiWidget::Api::Vulkan;
    if (env == "opengl")
        return QRhiWidget::Api::OpenGL;
    if (env == "metal")
        return QRhiWidget::Api::Metal;
#if defined(Q_OS_WIN)
    return QRhiWidget::Api::Direct3D11;
#elif defined(Q_OS_MACOS)
    return QRhiWidget::Api::Metal;
#else
    return QRhiWidget::Api::OpenGL;
#endif
}

} // namespace

ViewerWidget::ViewerWidget(QWidget* parent)
    : QRhiWidget(parent)
{
    setApi(preferredApi());
    setMinimumSize(320, 180);
}

ViewerWidget::~ViewerWidget() = default;

void ViewerWidget::setFrame(ve::VideoFramePtr frame)
{
    m_frame = std::move(frame);
    m_frameDirty = true;
    update();
}

void ViewerWidget::resetAll()
{
    m_displayPipeline.reset();
    m_processPipeline.reset();
    m_displayBindings.reset();
    m_processBindings.reset();
    m_processTarget.reset();
    m_processPass.reset();
    m_processTexture.reset();
    for (auto& p : m_planes)
        p.reset();
    m_sampler.reset();
    m_colorParams.reset();
    m_displayQuad.reset();
    m_processQuad.reset();
    m_vertices.reset();
    m_verticesUploaded = false;
    m_frameSize = {};
    m_hasImage = false;
}

void ViewerWidget::releaseResources()
{
    resetAll();
    m_rhi = nullptr;
}

void ViewerWidget::initialize(QRhiCommandBuffer*)
{
    if (m_rhi != rhi()) {
        resetAll();
        m_rhi = rhi();
    }
    if (m_vertices)
        return; // called again after a resize: everything is still valid

    m_quadVs = loadShader(QStringLiteral(":/shaders/quad.vert.qsb"));
    m_yuvFs = loadShader(QStringLiteral(":/shaders/yuv_to_rgb.frag.qsb"));
    m_displayFs = loadShader(QStringLiteral(":/shaders/display.frag.qsb"));

    m_vertices.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
    m_vertices->create();
    m_processQuad.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(QuadParams)));
    m_processQuad->create();
    m_displayQuad.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(QuadParams)));
    m_displayQuad->create();
    m_colorParams.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(ColorParams)));
    m_colorParams->create();
    m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_sampler->create();
    m_frameDirty = true; // re-upload whatever frame we hold

    emit backendReady(QString::fromLatin1(m_rhi->backendName()));
}

QRhiGraphicsPipeline* ViewerWidget::makePipeline(const QShader& fragment, QRhiShaderResourceBindings* srb,
                                                 QRhiRenderPassDescriptor* rp)
{
    QRhiGraphicsPipeline* ps = m_rhi->newGraphicsPipeline();
    ps->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    ps->setShaderStages({{QRhiShaderStage::Vertex, m_quadVs}, {QRhiShaderStage::Fragment, fragment}});
    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0},
                          {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)}});
    ps->setVertexInputLayout(layout);
    ps->setShaderResourceBindings(srb);
    ps->setRenderPassDescriptor(rp);
    if (!ps->create()) {
        qWarning("Failed to create viewer pipeline");
        delete ps;
        return nullptr;
    }
    return ps;
}

bool ViewerWidget::ensureFrameResources(const ve::VideoFrame& f)
{
    const QSize size(f.width, f.height);
    if (m_planes[0] && size == m_frameSize && f.layout == m_layout && m_processPipeline)
        return true;

    const QRhiTexture::Format planeFormat = f.layout == ve::PixelLayout::YUV420P16 ? QRhiTexture::R16 : QRhiTexture::R8;
    if (!m_rhi->isTextureFormatSupported(planeFormat)) {
        qWarning("Texture format for %s planes not supported by %s",
                 f.layout == ve::PixelLayout::YUV420P16 ? "16-bit" : "8-bit", m_rhi->backendName());
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        m_planes[std::size_t(i)].reset(m_rhi->newTexture(
            planeFormat, QSize(f.planeWidth[std::size_t(i)], f.planeHeight[std::size_t(i)])));
        if (!m_planes[std::size_t(i)]->create())
            return false;
    }

    const QRhiTexture::Format floatFormat =
        m_rhi->isTextureFormatSupported(QRhiTexture::RGBA16F) ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
    m_processTexture.reset(m_rhi->newTexture(floatFormat, size, 1, QRhiTexture::RenderTarget));
    if (!m_processTexture->create())
        return false;
    m_processTarget.reset(m_rhi->newTextureRenderTarget({m_processTexture.get()}));
    m_processPass.reset(m_processTarget->newCompatibleRenderPassDescriptor());
    m_processTarget->setRenderPassDescriptor(m_processPass.get());
    if (!m_processTarget->create())
        return false;

    m_processBindings.reset(m_rhi->newShaderResourceBindings());
    m_processBindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_processQuad.get()),
        QRhiShaderResourceBinding::uniformBuffer(1, QRhiShaderResourceBinding::FragmentStage, m_colorParams.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, m_planes[0].get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, m_planes[1].get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage, m_planes[2].get(), m_sampler.get()),
    });
    m_processBindings->create();
    m_processPipeline.reset(makePipeline(m_yuvFs, m_processBindings.get(), m_processPass.get()));

    // The display pass samples the new float texture (the pipeline itself stays compatible).
    m_displayBindings.reset(m_rhi->newShaderResourceBindings());
    m_displayBindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_displayQuad.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_processTexture.get(), m_sampler.get()),
    });
    m_displayBindings->create();

    m_frameSize = size;
    m_layout = f.layout;
    return m_processPipeline != nullptr;
}

bool ViewerWidget::ensureDisplayPipeline()
{
    if (!m_displayPipeline && m_displayBindings)
        m_displayPipeline.reset(makePipeline(m_displayFs, m_displayBindings.get(), renderTarget()->renderPassDescriptor()));
    return m_displayPipeline != nullptr;
}

void ViewerWidget::render(QRhiCommandBuffer* cb)
{
    QRhiResourceUpdateBatch* u = m_rhi->nextResourceUpdateBatch();
    if (!m_verticesUploaded) {
        u->uploadStaticBuffer(m_vertices.get(), kQuad);
        m_verticesUploaded = true;
    }
    // Vulkan's NDC is Y-down; OpenGL framebuffers are bottom-up when sampled back.
    const float ndcY = m_rhi->isYUpInNDC() ? 1.0f : -1.0f;
    const float flipV = m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f;

    if (m_frameDirty) {
        m_frameDirty = false;
        m_hasImage = false;
        if (m_frame && ensureFrameResources(*m_frame)) {
            const ve::VideoFrame& f = *m_frame;
            for (int i = 0; i < 3; ++i) {
                QRhiTextureSubresourceUploadDescription desc(f.plane(i), quint32(f.planeBytes(i)));
                desc.setDataStride(quint32(f.stride[std::size_t(i)]));
                u->uploadTexture(m_planes[std::size_t(i)].get(), QRhiTextureUploadDescription({0, 0, desc}));
            }
            const ColorParams cp = colorParamsFor(f);
            u->updateDynamicBuffer(m_colorParams.get(), 0, sizeof cp, &cp);
            const QuadParams qp{{1.0f, ndcY, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}};
            u->updateDynamicBuffer(m_processQuad.get(), 0, sizeof qp, &qp);
            m_hasImage = true;
            m_needProcess = true;
        }
    }

    const QSize out = renderTarget()->pixelSize();
    if (m_hasImage) {
        const float frameAspect = float(m_frameSize.width()) / float(m_frameSize.height());
        const float viewAspect = float(out.width()) / float(qMax(1, out.height()));
        float sx = 1.0f, sy = 1.0f;
        if (frameAspect > viewAspect)
            sy = viewAspect / frameAspect;
        else
            sx = frameAspect / viewAspect;
        const QuadParams dp{{sx, sy * ndcY, 0.0f, 0.0f}, {flipV, 0.0f, 0.0f, 0.0f}};
        u->updateDynamicBuffer(m_displayQuad.get(), 0, sizeof dp, &dp);
    }

    const QRhiCommandBuffer::VertexInput vertexInput(m_vertices.get(), 0);
    if (m_needProcess && m_hasImage) {
        cb->beginPass(m_processTarget.get(), Qt::black, {1.0f, 0}, u);
        u = nullptr;
        cb->setGraphicsPipeline(m_processPipeline.get());
        cb->setViewport(QRhiViewport(0, 0, float(m_frameSize.width()), float(m_frameSize.height())));
        cb->setShaderResources(m_processBindings.get());
        cb->setVertexInput(0, 1, &vertexInput);
        cb->draw(4);
        cb->endPass();
        m_needProcess = false;
    }

    cb->beginPass(renderTarget(), QColor::fromRgbF(0.04f, 0.04f, 0.045f), {1.0f, 0}, u);
    if (m_hasImage && ensureDisplayPipeline()) {
        cb->setGraphicsPipeline(m_displayPipeline.get());
        cb->setViewport(QRhiViewport(0, 0, float(out.width()), float(out.height())));
        cb->setShaderResources(m_displayBindings.get());
        cb->setVertexInput(0, 1, &vertexInput);
        cb->draw(4);
    }
    cb->endPass();
}

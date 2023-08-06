/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <contour/display/Blur.h>
#include <contour/display/ShaderConfig.h>

#include <vtbackend/Image.h>

#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextureAtlas.h>

#include <QtGui/QMatrix4x4>
#include <QtGui/QOpenGLExtraFunctions>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtOpenGL/QOpenGLShaderProgram>
    #include <QtOpenGL/QOpenGLTexture>
#else
    #include <QtGui/QOpenGLShaderProgram>
    #include <QtGui/QOpenGLTexture>
#endif

#include <QtQuick/QQuickWindow>

#include <chrono>
#include <memory>
#include <optional>
#include <unordered_map>

namespace contour::display
{

class OpenGLRenderer final:
    public QObject,
    public terminal::rasterizer::RenderTarget,
    public terminal::rasterizer::atlas::AtlasBackend,
    public QOpenGLExtraFunctions
{
    Q_OBJECT

    using ImageSize = terminal::image_size;

    using AtlasTextureScreenshot = terminal::rasterizer::AtlasTextureScreenshot;

    using AtlasTileID = terminal::rasterizer::atlas::AtlasTileID;

    using ConfigureAtlas = terminal::rasterizer::atlas::ConfigureAtlas;
    using UploadTile = terminal::rasterizer::atlas::UploadTile;
    using RenderTile = terminal::rasterizer::atlas::RenderTile;

  public:
    /**
     * @param renderSize        Sets the render target's size in pixels.
     *                          This is the size that can be rendered to.
     * @param textureAtlasSize  size in pixels for the texture atlas. Must be power of two.
     * @param tileSize          size in pixels for each tile. This should be the grid cell size.
     */
    OpenGLRenderer(ShaderConfig textShaderConfig,
                   ShaderConfig rectShaderConfig,
                   crispy::ImageSize viewSize,
                   crispy::ImageSize renderSize,
                   crispy::ImageSize textureTileSize,
                   terminal::rasterizer::PageMargin margin);

    ~OpenGLRenderer() override;

    void setWindow(QQuickWindow* window) { _window = window; }

    // AtlasBackend implementation
    ImageSize atlasSize() const noexcept override;
    void configureAtlas(ConfigureAtlas atlas) override;
    void uploadTile(UploadTile tile) override;
    void renderTile(RenderTile tile) override;

    // RenderTarget implementation
    void setRenderSize(crispy::ImageSize _size) override;
    void setTranslation(float x, float y, float z) noexcept;
    void setViewSize(crispy::ImageSize size) noexcept { _viewSize = size; }
    void setModelMatrix(QMatrix4x4 matrix) noexcept;
    void setMargin(terminal::rasterizer::PageMargin _margin) noexcept override;
    std::optional<AtlasTextureScreenshot> readAtlas() override;
    AtlasBackend& textureScheduler() override;
    void scheduleScreenshot(ScreenshotCallback _callback) override;
    void renderRectangle(int x, int y, Width, Height, RGBAColor color) override;
    void execute(std::chrono::steady_clock::time_point now) override;

    std::pair<crispy::ImageSize, std::vector<uint8_t>> takeScreenshot();

    void clearCache() override;

    void inspect(std::ostream& output) const override;

    float uptime(std::chrono::steady_clock::time_point now) noexcept
    {
        using namespace std::chrono;
        auto const uptimeMsecs = duration_cast<milliseconds>(now - _startTime).count();
        auto const uptimeSecs = static_cast<float>(uptimeMsecs) / 1000.0f;
        return uptimeSecs;
    }

    [[nodiscard]] constexpr bool initialized() const noexcept { return _initialized; }

  public slots:
    void initialize();

  private:
    // private helper methods
    //
    void logInfo();
    void initializeBackgroundRendering();
    void initializeTextureRendering();
    void initializeRectRendering();
    int maxTextureDepth();
    int maxTextureSize();
    int maxTextureUnits();
    crispy::ImageSize renderBufferSize();

    GLuint createAndUploadImage(QSize imageSize,
                                terminal::ImageFormat format,
                                int rowAlignment,
                                uint8_t const* pixels);

    void executeRenderTextures();
    void executeConfigureAtlas(ConfigureAtlas const& _param);
    void executeUploadTile(UploadTile const& _param);
    void executeRenderTile(RenderTile const& _param);

    //? void renderRectangle(int _x, int _y, int _width, int _height, QVector4D const& _color);

    // -------------------------------------------------------------------------------------------
    // private data members
    //

    // {{{ scheduling data
    struct RenderBatch
    {
        std::vector<terminal::rasterizer::atlas::RenderTile> renderTiles;
        std::vector<GLfloat> buffer;
        uint32_t userdata = 0;

        void clear()
        {
            renderTiles.clear();
            buffer.clear();
        }
    };

    struct Scheduler
    {
        std::optional<terminal::rasterizer::atlas::ConfigureAtlas> configureAtlas = std::nullopt;
        std::vector<terminal::rasterizer::atlas::UploadTile> uploadTiles {};
        RenderBatch renderBatch {};

        void clear()
        {
            configureAtlas.reset();
            uploadTiles.clear();
            renderBatch.clear();
        }
    };

    Scheduler _scheduledExecutions;
    // }}}

    bool _initialized = false;
    std::chrono::steady_clock::time_point _startTime;
    crispy::ImageSize _viewSize;
    crispy::ImageSize _renderTargetSize;
    QMatrix4x4 _projectionMatrix;
    QMatrix4x4 _viewMatrix;
    QMatrix4x4 _modelMatrix;

    terminal::rasterizer::PageMargin _margin {};

    std::unique_ptr<QOpenGLShaderProgram> _textShader;
    int _textProjectionLocation = -1;
    int _textTextureAtlasLocation = -1;
    int _textTimeLocation = -1;

    // private data members for rendering textures
    //
    GLuint _textVAO {}; // Vertex Array Object, covering all buffer objects
    GLuint _textVBO {}; // Buffer containing the vertex coordinates
    // TODO: GLuint ebo_{};

    // index equals AtlasID
    struct AtlasAttributes
    {
        // QOpenGLTexture gpuTexture { QOpenGLTexture::Target::Target2D };
        GLuint textureId {};
        ImageSize textureSize {};
        terminal::rasterizer::atlas::AtlasProperties properties {};
    };
    AtlasAttributes _textureAtlas {};

    [[nodiscard]] GLuint textureAtlasId() const noexcept
    {
        assert(_textureAtlas.textureId != 0);
        return _textureAtlas.textureId;
        // return _textureAtlas.gpuTexture.textureId();
    }

    // private data members for rendering filled rectangles
    //
    ShaderConfig _textShaderConfig;
    ShaderConfig _rectShaderConfig;

    std::vector<GLfloat> _rectBuffer;
    std::unique_ptr<QOpenGLShaderProgram> _rectShader;
    int _rectProjectionLocation = -1;
    int _rectTimeLocation = -1;
    GLuint _rectVAO {};
    GLuint _rectVBO {};

    std::optional<ScreenshotCallback> _pendingScreenshotCallback;

    QQuickWindow* _window = nullptr;

    // render state cache
    struct
    {
        terminal::rgba_color backgroundColor {};
        float backgroundImageOpacity = 1.0f;
        bool backgroundImageBlur = false;
        QSize backgroundResolution;
        crispy::StrongHash backgroundImageHash;
    } _renderStateCache;
};

} // namespace contour::display

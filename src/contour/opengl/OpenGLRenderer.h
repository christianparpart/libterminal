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

#include <terminal_renderer/RenderTarget.h>
#include <terminal_renderer/Atlas.h>

#include <crispy/debuglog.h>

#include <QtGui/QMatrix4x4>
#include <QtGui/QOpenGLExtraFunctions>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtOpenGL/QOpenGLShaderProgram>
#else
    #include <QtGui/QOpenGLShaderProgram>
#endif

#include <memory>
#include <optional>

namespace terminal::renderer::opengl {

struct ShaderConfig;

class OpenGLRenderer final :
    public RenderTarget,
    public QOpenGLExtraFunctions
{
  private:
    struct TextureScheduler;

  public:
    OpenGLRenderer(ShaderConfig const& _textShaderConfig,
                   ShaderConfig const& _rectShaderConfig,
                   crispy::ImageSize _size,
                   terminal::renderer::PageMargin _margin);

    ~OpenGLRenderer() override;

    void setRenderSize(crispy::ImageSize _size) override;
    void setMargin(terminal::renderer::PageMargin _margin) noexcept override;

    atlas::TextureAtlasAllocator& monochromeAtlasAllocator() noexcept override;
    atlas::TextureAtlasAllocator& coloredAtlasAllocator() noexcept override;
    atlas::TextureAtlasAllocator& lcdAtlasAllocator() noexcept override;

    atlas::AtlasBackend& textureScheduler() override;

    void scheduleScreenshot(ScreenshotCallback _callback) override;

    void renderRectangle(int _x, int _y, int _width, int _height,
                         float _r, float _g, float _b, float _a) override;

    void clear(terminal::RGBAColor _fillColor) override;
    void execute() override;

    void clearCache() override;

    std::optional<AtlasTextureInfo> readAtlas(atlas::TextureAtlasAllocator const& _allocator, atlas::AtlasID _instanceId) override;

  private:
    // private helper methods
    //
    void initialize();
    void initializeTextureRendering();
    void initializeRectRendering();
    int maxTextureDepth();
    int maxTextureSize();
    int maxTextureUnits();
    crispy::ImageSize renderBufferSize();

    crispy::ImageSize colorTextureSizeHint();
    crispy::ImageSize monochromeTextureSizeHint();

    void executeRenderTextures();
    void createAtlas(atlas::CreateAtlas const& _param);
    void uploadTexture(atlas::UploadTexture const& _param);
    void renderTexture(atlas::RenderTexture const& _param);
    void destroyAtlas(atlas::AtlasID _atlasID);

    void executeRenderRectangle(int _x, int _y, int _width, int _height, QVector4D const& _color);

    void bindTexture(GLuint _textureId);
    GLuint textureAtlasID(atlas::AtlasID _atlasID) const noexcept;
    void clearTextureAtlas(GLuint _textureId, int _width, int _height, atlas::Format _format);

    // -------------------------------------------------------------------------------------------
    // private data members
    //
    bool initialized_ = false;
    crispy::ImageSize size_;
    QMatrix4x4 projectionMatrix_;

    terminal::renderer::PageMargin margin_{};

    std::unique_ptr<QOpenGLShaderProgram> textShader_;
    int textProjectionLocation_;

    // private data members for rendering textures
    //
    GLuint vao_{};              // Vertex Array Object, covering all buffer objects
    GLuint vbo_{};              // Buffer containing the vertex coordinates
    //TODO: GLuint ebo_{};
    std::unordered_map<atlas::AtlasID, GLuint> atlasMap_; // maps atlas IDs to texture IDs
    GLuint currentTextureId_ = std::numeric_limits<GLuint>::max();
    std::unique_ptr<TextureScheduler> textureScheduler_;
    atlas::TextureAtlasAllocator monochromeAtlasAllocator_;
    atlas::TextureAtlasAllocator coloredAtlasAllocator_;
    atlas::TextureAtlasAllocator lcdAtlasAllocator_;

    // private data members for rendering filled rectangles
    //
    std::vector<GLfloat> rectBuffer_;
    std::unique_ptr<QOpenGLShaderProgram> rectShader_;
    GLint rectProjectionLocation_;
    GLuint rectVAO_;
    GLuint rectVBO_;

    std::optional<ScreenshotCallback> pendingScreenshotCallback_;

    // render state cache
    struct {
        terminal::RGBAColor backgroundColor{};
    } renderStateCache_;
};

} // end namespace


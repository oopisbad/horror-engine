#include "render/Renderer.hpp"
#include "render/Framebuffer.hpp"
#include "render/Shader.hpp"

#include <cmath>

namespace horror {

GLuint ForwardRenderer::sceneColor() const
{
    return temporalActive_ && historyValid_
        ? history_[1 - historyWrite_]->colorAttachment()
        : sceneTarget_->colorAttachment();
}

void ForwardRenderer::resolveTemporal(const Camera &camera)
{
    const auto width = sceneTarget_->width(), height = sceneTarget_->height();
    if (!history_[0] || history_[0]->width() != width || history_[0]->height() != height) {
        resetHistory();
        for (auto &buffer : history_)
            buffer = std::make_unique<Framebuffer>(Framebuffer::Kind::ColorAlpha, width, height, true);
    }
    auto &target = history_[historyWrite_];
    auto &last = history_[1 - historyWrite_];
    const bool useHistory = historyValid_ && last && last->width() == width && last->height() == height
        && glm::length(camera.position - previousEye_) < 0.75f
        && glm::dot(camera.forward(), previousForward_) > 0.93f
        && timeSeconds_ - previousTime_ < 0.15f;
    const auto lampPosition = hasFlashlightTransform_ ? flashlightPos_ : camera.position;
    const auto lampDirection = hasFlashlightTransform_ ? flashlightDir_ : camera.forward();
    const auto lampColor = flashlightColor_ * flashlightIntensity_;
    const float lightingChange = glm::length(lampPosition - previousLampPosition_) * 4.0f
        + (1.0f - glm::dot(lampDirection, previousLampDirection_)) * 120.0f
        + glm::length(lampColor - previousLampColor_) * 0.25f;

    target->bind();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindVertexArray(fullscreenVao_);
    temporalShader_->bind();
    glBindTextureUnit(0, sceneTarget_->colorAttachment());
    glBindTextureUnit(1, sceneTarget_->depth());
    glBindTextureUnit(2, sceneTarget_->colorAttachment(1));
    glBindTextureUnit(3, useHistory ? last->colorAttachment() : 0);
    const auto viewProj = camera.viewProjection();
    temporalShader_->setUniform("uHistoryValid", useHistory ? 1 : 0);
    temporalShader_->setUniform("uFixedView", viewProj == previousViewProj_ ? 1 : 0);
    temporalShader_->setUniform("uLightingChange", lightingChange);
    temporalShader_->setUniform("uInverseVP", glm::inverse(viewProj));
    temporalShader_->setUniform("uRasterOffset", rasterOffset_);
    temporalShader_->setUniform("uPreviousVP", previousViewProj_);
    temporalShader_->setUniform("uPlanes", glm::vec2(camera.nearPlane, camera.farPlane));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    previousViewProj_ = viewProj;
    previousEye_ = camera.position;
    previousForward_ = camera.forward();
    previousLampPosition_ = lampPosition;
    previousLampDirection_ = lampDirection;
    previousLampColor_ = lampColor;
    previousTime_ = timeSeconds_;
    historyWrite_ = 1 - historyWrite_;
    historyValid_ = true;
    stats_.temporalFrames = useHistory ? stats_.temporalFrames + 1 : 1;
}

void ForwardRenderer::traceReflections(const Camera &camera)
{
    reflectionsReady_ = post_.screenReflections && !wireframe_ && !debugDrawEnabled_ && !drawBones_;
    stats_.screenReflections = reflectionsReady_;
    if (!reflectionsReady_)
        return;
    const auto width = (sceneTarget_->width() + 1) / 2;
    const auto height = (sceneTarget_->height() + 1) / 2;
    if (!reflectionTarget_)
        reflectionTarget_ = std::make_unique<Framebuffer>(Framebuffer::Kind::ColorAlpha, width, height, true);
    else
        reflectionTarget_->resize(width, height);
    reflectionTarget_->bind();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindVertexArray(fullscreenVao_);
    reflectionShader_->bind();
    glBindTextureUnit(0, sceneColor());
    glBindTextureUnit(1, sceneTarget_->depth());
    glBindTextureUnit(2, sceneTarget_->colorAttachment(1));
    const auto viewProj = camera.viewProjection();
    reflectionShader_->setUniform("uInverseVP", glm::inverse(viewProj));
    reflectionShader_->setUniform("uVP", viewProj);
    reflectionShader_->setUniform("uRasterOffset", rasterOffset_);
    reflectionShader_->setUniform("uEye", camera.position);
    reflectionShader_->setUniform("uPlanes", glm::vec2(camera.nearPlane, camera.farPlane));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

} // namespace horror

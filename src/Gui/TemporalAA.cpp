// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 SlobCoder <slobcoder@slobcompany.com>              *
 *                                                                         *
 *   This file is part of FreeCAD.                                         *
 *                                                                         *
 *   FreeCAD is free software: you can redistribute it and/or modify it    *
 *   under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the  *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful, but        *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU      *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with FreeCAD. If not, see                               *
 *   <https://www.gnu.org/licenses/>.                                      *
 *                                                                         *
 **************************************************************************/

#include "TemporalAA.h"

#include <cstdarg>
#include <cstdlib>

#include "Inventor/SoNaviCube.h"

#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QtGui/qopengl.h>

#include <Inventor/actions/SoGLRenderAction.h>
#include <Inventor/fields/SoField.h>
#include <Inventor/nodes/SoNode.h>

#include <Base/Console.h>

namespace Gui
{

namespace
{

// Number of rendered frames after a reset until the image is considered
// converged. With the jitter positions cycling through `samples` entries,
// this yields maxFrames effective supersamples while static.
int maxFramesForSamples(int samples)
{
    return 4 * samples;
}

bool tssaaDebugEnabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = std::getenv("FREECAD_DEBUG_TSSAA");
        enabled = env && std::atoi(env) >= 1;
    }
    return enabled == 1;
}

// Debug output goes to stderr: stdout is block-buffered when redirected,
// which would hide the per-frame traces.
void tssaaDebug(const char* format, ...)
{
    if (!tssaaDebugEnabled()) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

}  // namespace

TemporalAA::TemporalAA() = default;

TemporalAA::~TemporalAA()
{
    for (auto& sensor : sensors) {
        delete sensor;
    }
    delete sceneFbo;
    delete historyFbo;
}

void TemporalAA::setSampleCount(int newSamples)
{
    if (newSamples < 2) {
        newSamples = 0;
    }
    if (newSamples == samples) {
        return;
    }

    samples = newSamples;
    maxFrames = samples > 0 ? maxFramesForSamples(samples) : 0;
    broken = false;
    invalidate();
}

bool TemporalAA::isActive() const
{
    return samples > 1;
}

void TemporalAA::invalidate()
{
    invalidated = true;
}

void TemporalAA::watchedNodeChanged(void* data, SoSensor* sensor)
{
    auto* nodeSensor = static_cast<SoNodeSensor*>(sensor);
    const SoNode* trigger = nodeSensor->getTriggerNode();

    // The NaviCube rewrites its render-parameter fields on every traversal,
    // alternating between the render-pass values and the pick-pass values
    // (NaviCubeImplementation::populateRenderParams is called from both
    // contexts with different arguments). Treat this param churn as a
    // non-change: it does not alter the rendered image, and real NaviCube
    // visual changes (hover, fade) accompany user interaction, which resets
    // the history through camera/scene notifications anyway.
    if (trigger && trigger->isOfType(Gui::SoNaviCube::getClassTypeId())) {
        tssaaDebug("TemporalAA: ignoring NaviCube param churn\n");
        return;
    }

    if (tssaaDebugEnabled()) {
        const SoField* field = nodeSensor->getTriggerField();
        SbName fieldName;
        if (trigger && field) {
            const_cast<SoNode*>(trigger)->getFieldName(const_cast<SoField*>(field), fieldName);
        }
        std::fprintf(stderr,
                     "TemporalAA: notify from %s field=%s\n",
                     trigger ? trigger->getTypeId().getName().getString() : "(null)",
                     fieldName.getString());
    }
    static_cast<TemporalAA*>(data)->invalidate();
}

void TemporalAA::setWatchedNodes(SoNode* scene,
                                 SoNode* background,
                                 SoNode* foreground,
                                 SoNode* decoration)
{
    SoNode* const roots[numWatched] = {scene, background, foreground, decoration};
    for (int i = 0; i < numWatched; ++i) {
        if (watched[i] == roots[i]) {
            continue;
        }

        delete sensors[i];
        sensors[i] = nullptr;
        watched[i] = roots[i];
        if (watched[i]) {
            sensors[i] = new SoNodeSensor(&TemporalAA::watchedNodeChanged, this);
            // Priority 0 (immediate) so that getTriggerNode()/getTriggerField()
            // are populated; Coin only captures them for immediate sensors.
            sensors[i]->setPriority(0);
            sensors[i]->attach(watched[i]);
        }
    }
}

void TemporalAA::ensureFramebuffers(QOpenGLWidget* gl, const QSize& sizePixels)
{
    if (sceneFbo && historyFbo && fboSize == sizePixels) {
        return;
    }
    if (broken && fboSize == sizePixels) {
        // Framebuffer creation failed for this size before; do not retry on
        // every frame. A size change or a setSampleCount() re-enables it.
        return;
    }

    gl->makeCurrent();
    delete sceneFbo;
    delete historyFbo;
    sceneFbo = nullptr;
    historyFbo = nullptr;
    fboSize = sizePixels;
    frameIndex = 0;

    QOpenGLFramebufferObjectFormat sceneFormat;
    // CombinedDepthStencil mirrors what a default window framebuffer provides;
    // the depth attachment is required by the scene render, the stencil part
    // keeps stencil-using rendering paths alive inside the FBO.
    sceneFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    sceneFbo = new QOpenGLFramebufferObject(sizePixels, sceneFormat);

    QOpenGLFramebufferObjectFormat historyFormat;
    historyFbo = new QOpenGLFramebufferObject(sizePixels, historyFormat);

    if (!sceneFbo->isValid() || !historyFbo->isValid()) {
        Base::Console().developerError("TemporalAA",
                                       "failed to create %dx%d framebuffers, disabling\n",
                                       sizePixels.width(),
                                       sizePixels.height());
        releaseResources();
        broken = true;
    }
    else {
        tssaaDebug("TemporalAA: created %dx%d framebuffers\n",
                   sizePixels.width(),
                   sizePixels.height());
    }
}

QOpenGLFramebufferObject* TemporalAA::beginFrame(QOpenGLWidget* gl,
                                                 SoGLRenderAction* action,
                                                 const QSize& sizePixels)
{
    if (!isActive() || !gl || !action || sizePixels.isEmpty()) {
        return nullptr;
    }
    if (!QOpenGLContext::currentContext()) {
        return nullptr;
    }

    ensureFramebuffers(gl, sizePixels);
    if (!sceneFbo || !historyFbo) {
        return nullptr;
    }

    if (invalidated) {
        if (frameIndex > 0) {
            tssaaDebug("TemporalAA: reset after %d frames (scene/camera change)\n",
                       frameIndex);
        }
        frameIndex = 0;
        invalidated = false;
    }

    // One jitter position per frame, cycling through the `samples` positions
    // of Coin's multipass jitter table. Coin applies the offset to the
    // projection matrix when SoCamera::GLRender() sees getNumPasses() > 1;
    // setCurPass() drives this from outside without the (accumulation-buffer
    // based) internal multipass machinery.
    action->setCurPass(frameIndex % samples, samples);

    if (!sceneFbo->bind()) {
        action->setCurPass(0, 1);
        return nullptr;
    }

    QOpenGLFunctions* functions = QOpenGLContext::currentContext()->functions();
    functions->glViewport(0, 0, sizePixels.width(), sizePixels.height());

    tssaaDebug("TemporalAA: frame %d/%d pass %d/%d\n",
               frameIndex,
               maxFrames,
               (frameIndex % samples) + 1,
               samples);

    return sceneFbo;
}

bool TemporalAA::drawSceneIntoHistory()
{
    // Progressive average: history = history * k/(k+1) + scene * 1/(k+1),
    // expressed as standard blending with a constant alpha factor.
    const float weight = 1.0F / float(frameIndex + 1);

    const bool historyBound = historyFbo->bind();
    if (!historyBound) {
        return false;
    }

    QOpenGLFunctions* functions = QOpenGLContext::currentContext()->functions();

    // The accumulation quad is fixed-function immediate-mode GL. Everything
    // it touches must be saved and restored: the scene render leaves Coin's
    // GLSL program bound (immediate-mode vertices would be routed through it
    // and produce garbage plus sticky GL errors), the blend equation and
    // blend color are used by Coin's transparency resolve on the next frame,
    // and a leftover GL_PROJECTION matrix mode overflows Coin's matrix stack
    // (the legacy projection stack is only two entries deep).
    GLint viewport[4] = {0, 0, 0, 0};
    functions->glGetIntegerv(GL_VIEWPORT, viewport);
    GLboolean depthTest = functions->glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend = functions->glIsEnabled(GL_BLEND);
    GLboolean texture2d = functions->glIsEnabled(GL_TEXTURE_2D);

    GLint currentProgram = 0;
    functions->glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    GLint matrixMode = 0;
    functions->glGetIntegerv(GL_MATRIX_MODE, &matrixMode);
    GLint blendSrcRgb = 0;
    GLint blendDstRgb = 0;
    GLint blendSrcAlpha = 0;
    GLint blendDstAlpha = 0;
    functions->glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
    functions->glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
    functions->glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    functions->glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    GLfloat blendColor[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    functions->glGetFloatv(GL_BLEND_COLOR, blendColor);
    GLint activeTexture = 0;
    functions->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    GLint textureBinding = 0;
    functions->glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
    GLint texEnvMode = 0;
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &texEnvMode);

    functions->glUseProgram(0);
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glViewport(0, 0, fboSize.width(), fboSize.height());
    functions->glDisable(GL_DEPTH_TEST);
    functions->glEnable(GL_TEXTURE_2D);
    functions->glBindTexture(GL_TEXTURE_2D, sceneFbo->texture());
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    functions->glEnable(GL_BLEND);
    functions->glBlendFuncSeparate(GL_CONSTANT_ALPHA,
                                   GL_ONE_MINUS_CONSTANT_ALPHA,
                                   GL_CONSTANT_ALPHA,
                                   GL_ONE_MINUS_CONSTANT_ALPHA);
    functions->glBlendColor(0.0F, 0.0F, 0.0F, weight);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glBegin(GL_QUADS);
    glTexCoord2f(0.0F, 0.0F);
    glVertex2f(-1.0F, -1.0F);
    glTexCoord2f(1.0F, 0.0F);
    glVertex2f(1.0F, -1.0F);
    glTexCoord2f(1.0F, 1.0F);
    glVertex2f(1.0F, 1.0F);
    glTexCoord2f(0.0F, 1.0F);
    glVertex2f(-1.0F, 1.0F);
    glEnd();

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    // Restore the state the scene render expects. Order matters: the texture
    // binding belongs to unit 0, so restore it while unit 0 is still active,
    // then the blend state, then the program, and last the matrix mode.
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, texEnvMode);
    functions->glBindTexture(GL_TEXTURE_2D, textureBinding);
    if (!texture2d) {
        functions->glDisable(GL_TEXTURE_2D);
    }
    functions->glActiveTexture(activeTexture);

    functions->glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);
    functions->glBlendColor(blendColor[0], blendColor[1], blendColor[2], blendColor[3]);
    functions->glDisable(GL_BLEND);
    if (blend) {
        functions->glEnable(GL_BLEND);
    }

    functions->glUseProgram(currentProgram);
    glMatrixMode(matrixMode);

    if (depthTest) {
        functions->glEnable(GL_DEPTH_TEST);
    }
    else {
        functions->glDisable(GL_DEPTH_TEST);
    }
    functions->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    return true;
}

void TemporalAA::presentFramebuffer(QOpenGLWidget* gl,
                                    const QRect& viewportRect,
                                    QOpenGLFramebufferObject* source)
{
    // Direct GL blit instead of QOpenGLFramebufferObject::blitFramebuffer():
    // the Qt helper mutates the read buffer and restore-bindings as side
    // effects and its error behavior is opaque. Binding the draw side to the
    // widget's defaultFramebufferObject() explicitly is the documented
    // equivalent for QOpenGLWidget.
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context) {
        return;
    }
    QOpenGLExtraFunctions* extra = context->extraFunctions();

    extra->glBindFramebuffer(GL_READ_FRAMEBUFFER, source->handle());
    extra->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl->defaultFramebufferObject());
    extra->glBlitFramebuffer(0,
                             0,
                             fboSize.width(),
                             fboSize.height(),
                             viewportRect.x(),
                             viewportRect.y(),
                             viewportRect.x() + viewportRect.width(),
                             viewportRect.y() + viewportRect.height(),
                             GL_COLOR_BUFFER_BIT,
                             GL_NEAREST);

    if (tssaaDebugEnabled()) {
        const GLenum err = context->functions()->glGetError();
        if (err != GL_NO_ERROR) {
            std::fprintf(stderr,
                         "TemporalAA: present blit GL error 0x%04x (fbo %dx%d -> widget %dx%d)\n",
                         err,
                         fboSize.width(),
                         fboSize.height(),
                         viewportRect.width(),
                         viewportRect.height());
        }
    }

    // Leave the widget framebuffer bound with the caller's viewport so that
    // the regular per-frame tail (dimension overlays, graphics items) draws
    // exactly like it does without TSSAA.
    QOpenGLFunctions* functions = QOpenGLContext::currentContext()->functions();
    functions->glBindFramebuffer(GL_FRAMEBUFFER, gl->defaultFramebufferObject());
    functions->glViewport(viewportRect.x(),
                          viewportRect.y(),
                          viewportRect.width(),
                          viewportRect.height());
}

bool TemporalAA::endFrame(QOpenGLWidget* gl, SoGLRenderAction* action, const QRect& viewportRect)
{
    if (!sceneFbo || !historyFbo) {
        return false;
    }

    // Restore the pass state so that other users of the action (and the next
    // non-TSSAA frame) render un-jittered.
    action->setCurPass(0, 1);

    static int dumpLevel = -1;
    if (dumpLevel < 0) {
        const char* env = std::getenv("FREECAD_DEBUG_TSSAA");
        dumpLevel = env ? std::atoi(env) : 0;
    }
    if (dumpLevel >= 4 && (frameIndex == 2 || frameIndex == maxFrames - 1)) {
        sceneFbo->toImage(false).save(QString("/tmp/tsaa_scene_%1.png").arg(frameIndex));
        historyFbo->toImage(false).save(QString("/tmp/tsaa_hist_%1.png").arg(frameIndex));
        std::fprintf(stderr, "TemporalAA: dumped framebuffers at index %d\n", frameIndex);
    }

    if (!drawSceneIntoHistory()) {
        // Could not bind the history framebuffer; present the raw scene frame
        // so the view stays usable and stop accumulating.
        presentFramebuffer(gl, viewportRect, sceneFbo);
        ++frameIndex;
        return false;
    }
    presentFramebuffer(gl, viewportRect, historyFbo);

    ++frameIndex;
    return frameIndex < maxFrames;
}

void TemporalAA::releaseResources()
{
    delete sceneFbo;
    delete historyFbo;
    sceneFbo = nullptr;
    historyFbo = nullptr;
    fboSize = QSize();
    frameIndex = 0;
}

}  // namespace Gui

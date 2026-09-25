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

#pragma once

#include <QRect>
#include <QSize>

#include <Inventor/sensors/SoNodeSensor.h>

class QOpenGLFramebufferObject;
class QOpenGLWidget;
class SoGLRenderAction;
class SoNode;

namespace Gui
{

/** Temporal supersampling antialiasing (TSSAA)
 *
 * Implements NVIDIA-style temporal supersampling (the "TX" antialiasing modes
 * known from 3ds Max / Maya viewports): one frame is rendered per repaint with
 * a subpixel-jittered projection, and frames are progressively averaged into a
 * history buffer while the scene is static. While the camera, the scene or any
 * watched auxiliary root changes, the accumulation resets and the viewport
 * falls back to single-sample quality; once the input becomes static again the
 * image converges to the quality of N-way supersampling within N*k frames.
 *
 * The per-frame subpixel jitter reuses Coin's built-in multipass jitter
 * tables through SoGLRenderAction::setCurPass(), so no fork of the rendering
 * library is required. The caller renders the full frame into the scene
 * framebuffer returned by beginFrame() and then calls endFrame(), which
 * blends the frame into the history, presents the accumulated image to the
 * widget framebuffer and reports whether more frames are wanted to converge.
 */
class TemporalAA
{
public:
    TemporalAA();
    ~TemporalAA();

    TemporalAA(const TemporalAA&) = delete;
    TemporalAA& operator=(const TemporalAA&) = delete;

    /// Number of temporal jitter positions (0 disables TSSAA; 2, 4 or 8).
    void setSampleCount(int samples);
    int sampleCount() const
    {
        return samples;
    }
    bool isActive() const;

    /// Discards the accumulated history; the next frame restarts from scratch.
    void invalidate();

    /// Watches the given roots for modifications (main scene, background,
    /// foreground, decorations). A null entry is skipped. Re-attaches when a
    /// root pointer changes. Any notification resets the accumulation.
    void setWatchedNodes(SoNode* scene,
                         SoNode* background,
                         SoNode* foreground,
                         SoNode* decoration);

    /// Prepares the next temporal frame: consumes a pending invalidation,
    /// (re)creates the framebuffers on size changes, applies the jitter pass
    /// for the current frame index to the render action and binds the scene
    /// framebuffer. Returns the bound framebuffer, or null when TSSAA cannot
    /// run (disabled, degenerate size or context).
    QOpenGLFramebufferObject* beginFrame(QOpenGLWidget* gl,
                                         SoGLRenderAction* action,
                                         const QSize& sizePixels);

    /// Blends the just-rendered scene framebuffer into the history, presents
    /// the accumulated image to the widget framebuffer and restores the GL
    /// viewport to the caller's viewport rectangle. Returns true while the
    /// image has not fully converged yet and more repaints are wanted.
    bool endFrame(QOpenGLWidget* gl,
                  SoGLRenderAction* action,
                  const QRect& viewportRect);

    /// Deletes the GPU buffers; call with a current GL context (e.g. from
    /// aboutToDestroyGLContext()).
    void releaseResources();

    int accumulatedFrames() const
    {
        return frameIndex;
    }

private:
    static void watchedNodeChanged(void* data, SoSensor* sensor);
    void ensureFramebuffers(QOpenGLWidget* gl, const QSize& sizePixels);
    bool drawSceneIntoHistory();
    void presentFramebuffer(QOpenGLWidget* gl,
                            const QRect& viewportRect,
                            QOpenGLFramebufferObject* source);

    int samples = 0;
    int frameIndex = 0;
    int maxFrames = 0;
    bool invalidated = true;
    bool broken = false;
    QOpenGLFramebufferObject* sceneFbo = nullptr;
    QOpenGLFramebufferObject* historyFbo = nullptr;
    QSize fboSize;

    static constexpr int numWatched = 4;
    SoNodeSensor* sensors[numWatched] = {};
    SoNode* watched[numWatched] = {};
};

}  // namespace Gui

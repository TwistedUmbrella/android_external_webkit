/*
 * Copyright 2010, The Android Open Source Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef GLWebViewState_h
#define GLWebViewState_h

#if USE(ACCELERATED_COMPOSITING)

#include "DrawExtra.h"
#include "IntRect.h"
#include "SkCanvas.h"
#include "SkRect.h"
#include "TiledPage.h"
#include <utils/threads.h>

namespace WebCore {

class BaseLayerAndroid;
class LayerAndroid;

/////////////////////////////////////////////////////////////////////////////////
// GL Architecture
/////////////////////////////////////////////////////////////////////////////////
//
// To draw things, WebView use a tree of layers. The root of that tree is a
// BaseLayerAndroid, which may have numerous LayerAndroid over it. The content
// of those layers are SkPicture, the content of the BaseLayer is an PictureSet.
//
// When drawing, we therefore have one large "surface" that is the BaseLayer,
// and (possibly) additional surfaces (usually smaller), which are the
// LayerAndroids. The BaseLayer usually corresponds to the normal web page
// content, the Layers are used for some parts such as specific divs (e.g. fixed
// position divs, or elements using CSS3D transforms, or containing video,
// plugins, etc.).
//
// *** NOTE: The GL drawing architecture only paints the BaseLayer for now.
//
// The rendering model is to use tiles to display the BaseLayer (as obviously a
// BaseLayer's area can be arbitrarly large). The idea is to compute a set of
// tiles covering the viewport's area, paint those tiles using the webview's
// content (i.e. the BaseLayer's PictureSet), then display those tiles.
// We check which tile we should use at every frame.
//
// Overview
// ---------
//
// The tiles are grouped into a TiledPage -- basically a map of tiles covering
// the BaseLayer's surface. When drawing, we ask the TiledPage to prepare()
// itself then draw itself on screen. The prepare() function is the one
// that schedules tiles to be painted -- i.e. the subset of tiles that intersect
// with the current viewport. When they are ready, we can display
// the TiledPage.
//
// Note that BaseLayerAndroid::drawGL() will return true to the java side if
// there is a need to be called again (i.e. if we do not have up to date
// textures or a transition is going on).
//
// Tiles are implemented as a BaseTile. It knows how to paint itself with the
// PictureSet, and to display itself. A GL texture is usually associated to it.
//
// We also works with two TiledPages -- one to display the page at the
// current scale factor, and another we use to paint the page at a different
// scale factor. I.e. when we zoom, we use TiledPage A, with its tiles scaled
// accordingly (and therefore possible loss of quality): this is fast as it's
// purely a hardware operation. When the user is done zooming, we ask for
// TiledPage B to be painted at the new scale factor, covering the
// viewport's area. When B is ready, we swap it with A.
//
// Texture allocation
// ------------------
//
// Obviously we cannot have every BaseTile having a GL texture -- we need to
// get the GL textures from an existing pool, and reuse them.
//
// The way we do it is that when we call TiledPage::prepare(), we group the
// tiles we need into a TilesSet, call TilesSet::reserveTextures() (which
// associates the GL textures to the BaseTiles).
//
// reserveTexture() will ask the TilesManager for a texture. The allocation
// mechanism goal is to (in order):
// - prefers to allocate the same texture as the previous time
// - prefers to allocate textures that are as far from the viewport as possible
// - prefers to allocate textures that are used by different TiledPages
//
// Note that to compute the distance of tiles, each time we prepare() a
// TiledPage, we compute the distance of the tiles in it from the viewport.
//
// Painting scheduling
// -------------------
//
// The next operation is to schedule this TilesSet to be painted
// (TilesManager::schedulePaintForTilesSet()). TexturesGenerator
// will get the TilesSet and ask the BaseTiles in it to be painted.
//
// BaseTile::paintBitmap() will paint the texture using the BaseLayer's
// PictureSet (calling TiledPage::paintBaseLayerContent() which in turns
// calls GLWebViewState::paintBaseLayerContent()).
//
// Note that TexturesGenerator is running in a separate thread, the textures
// are shared using EGLImages (this is necessary to not slow down the rendering
// speed -- updating GL textures in the main GL thread would slow things down).
//
/////////////////////////////////////////////////////////////////////////////////

class GLWebViewState {
public:
    enum GLScaleStates {
        kNoScaleRequest = 0,
        kWillScheduleRequest = 1,
        kRequestNewScale = 2,
        kReceivedNewScale = 3
    };
    typedef int32_t GLScaleState;

    GLWebViewState();
    ~GLWebViewState();
    GLScaleState scaleRequestState() const { return m_scaleRequestState; }
    void setScaleRequestState(GLScaleState state) { m_scaleRequestState = state; }
    float currentScale() const { return m_currentScale; }
    void setCurrentScale(float scale) { m_currentScale = scale; }
    float futureScale() const { return m_futureScale; }
    void setFutureScale(float scale) { m_futureScale = scale; }
    double updateTime() const { return m_updateTime; }
    void setUpdateTime(double value) { m_updateTime = value; }
    double transitionTime(double currentTime);
    float transparency(double currentTime);
    void resetTransitionTime() { m_transitionTime = -1; }
    int originalTilesPosX() const { return m_originalTilesPosX; }
    void setOriginalTilesPosX(int pos) { m_originalTilesPosX = pos; }
    int originalTilesPosY() const { return m_originalTilesPosY; }
    void setOriginalTilesPosY(int pos) { m_originalTilesPosY = pos; }

    bool paintBaseLayerContent(SkCanvas* canvas);
    void setBaseLayer(BaseLayerAndroid* layer, IntRect& rect);
    void setExtra(android::DrawExtra* extra, LayerAndroid* navLayer);
    void resetExtra(bool repaint);

    void scheduleUpdate(const double& currentTime, float scale);

    TiledPage* frontPage();
    TiledPage* backPage();
    void swapPages();

    void setViewport(SkRect& viewport, float scale);

    // returns the number of tiles needed to cover the viewport
    int nbTilesWidth() const { return m_nbTilesWidth; }
    int nbTilesHeight() const { return m_nbTilesHeight; }

    int firstTileX() const { return m_firstTileX; }
    int firstTileY() const { return m_firstTileY; }

    unsigned int currentPictureCounter() const { return m_currentPictureCounter; }
    SkRect& invalidatedRect() { return m_invalidatedRect; }

    void baseLayerLock() { m_baseLayerLock.lock(); }
    void baseLayerUnlock() { m_baseLayerLock.unlock(); }

private:

    // Delay between scheduling a new page when the scale
    // factor changes (i.e. zooming in or out)
    static const double s_updateInitialDelay = 0.3; // 300 ms
    // If the scale factor continued to change and we completed
    // the original delay, we push back the update by this value
    static const double s_updateDelay = 0.1; // 100 ms

    // Delay for the transition between the two pages
    static const double s_transitionDelay = 0.5; // 500 ms
    static const double s_invTransitionDelay = 2;

    GLScaleState m_scaleRequestState;
    float m_currentScale;
    float m_futureScale;
    double m_updateTime;
    double m_transitionTime;
    int m_originalTilesPosX;
    int m_originalTilesPosY;
    android::Mutex m_tiledPageLock;
    SkRect m_viewport;
    int m_nbTilesWidth;
    int m_nbTilesHeight;
    int m_firstTileX;
    int m_firstTileY;
    android::Mutex m_baseLayerLock;
    BaseLayerAndroid* m_baseLayer;
    unsigned int m_currentPictureCounter;
    SkRect m_invalidatedRect;
    bool m_usePageA;
    TiledPage* m_tiledPageA;
    TiledPage* m_tiledPageB;
    android::DrawExtra* m_extra;
    LayerAndroid* m_navLayer;
};

} // namespace WebCore

#endif // USE(ACCELERATED_COMPOSITING)
#endif // GLWebViewState_h
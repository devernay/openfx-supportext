/*
   OFX mipmapping help functions

   Copyright (C) 2014 INRIA

   Redistribution and use in source and binary forms, with or without modification,
   are permitted provided that the following conditions are met:

   Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

   Redistributions in binary form must reproduce the above copyright notice, this
   list of conditions and the following disclaimer in the documentation and/or
   other materials provided with the distribution.

   Neither the name of the {organization} nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
   ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   INRIA
   Domaine de Voluceau
   Rocquencourt - B.P. 105
   78153 Le Chesnay Cedex - France
 */

#include "ofxsMipMap.h"

namespace OFX {
// update the window of dst defined by dstRoI by halving the corresponding area in src.
// proofread and fixed by F. Devernay on 3/10/2014
template <typename PIX,int nComponents>
static void
halveWindow(const OfxRectI & dstRoI,
            const PIX* srcPixels,
            const OfxRectI & srcBounds,
            int srcRowBytes,
            PIX* dstPixels,
            const OfxRectI & dstBounds,
            int dstRowBytes)
{

    assert(dstRoI.x1 * 2 >= (srcBounds.x1 - 1) && (dstRoI.x2 - 1) * 2 < srcBounds.x2 &&
           dstRoI.y1 * 2 >= (srcBounds.y1 - 1) && (dstRoI.y2 - 1) * 2 < srcBounds.y2);
    int srcRowSize = srcRowBytes / sizeof(PIX);
    int dstRowSize = dstRowBytes / sizeof(PIX);
    
    // offset pointers so that srcData and dstData correspond to pixel (0,0)
    const PIX* const srcData = srcPixels - (srcBounds.x1 * nComponents + srcRowSize * srcBounds.y1);
    PIX* const dstData       = dstPixels - (dstBounds.x1 * nComponents + dstRowSize * dstBounds.y1);

    for (int y = dstRoI.y1; y < dstRoI.y2; ++y) {
        const PIX* const srcLineStart    = srcData + y * 2 * srcRowSize;
        PIX* const dstLineStart          = dstData + y     * dstRowSize;

        // The current dst row, at y, covers the src rows y*2 (thisRow) and y*2+1 (nextRow).
        // Check that if are within srcBounds.
        int srcy = y * 2;
        bool pickThisRow = srcBounds.y1 <= (srcy + 0) && (srcy + 0) < srcBounds.y2;
        bool pickNextRow = srcBounds.y1 <= (srcy + 1) && (srcy + 1) < srcBounds.y2;

        const int sumH = (int)pickNextRow + (int)pickThisRow;
        assert(sumH == 1 || sumH == 2);

        for (int x = dstRoI.x1; x < dstRoI.x2; ++x) {
            const PIX* const srcPixStart    = srcLineStart   + x * 2 * nComponents;
            PIX* const dstPixStart          = dstLineStart   + x * nComponents;

            // The current dst col, at y, covers the src cols x*2 (thisCol) and x*2+1 (nextCol).
            // Check that if are within srcBounds.
            int srcx = x * 2;
            bool pickThisCol = srcBounds.x1 <= (srcx + 0) && (srcx + 0) < srcBounds.x2;
            bool pickNextCol = srcBounds.x1 <= (srcx + 1) && (srcx + 1) < srcBounds.x2;

            const int sumW = (int)pickThisCol + (int)pickNextCol;
            assert(sumW == 1 || sumW == 2);
            const int sum = sumW * sumH;
            assert(0 < sum && sum <= 4);

            for (int k = 0; k < nComponents; ++k) {
                ///a b
                ///c d

                const PIX a = (pickThisCol && pickThisRow) ? *(srcPixStart + k) : 0;
                const PIX b = (pickNextCol && pickThisRow) ? *(srcPixStart + k + nComponents) : 0;
                const PIX c = (pickThisCol && pickNextRow) ? *(srcPixStart + k + srcRowSize): 0;
                const PIX d = (pickNextCol && pickNextRow) ? *(srcPixStart + k + srcRowSize  + nComponents)  : 0;

                assert( sumW == 2 || ( sumW == 1 && ( (a == 0 && c == 0) || (b == 0 && d == 0) ) ) );
                assert( sumH == 2 || ( sumH == 1 && ( (a == 0 && b == 0) || (c == 0 && d == 0) ) ) );
                dstPixStart[k] = (a + b + c + d) / sum;
            }
        }
    }
}

// update the window of dst defined by originalRenderWindow by mipmapping the windows of src defined by renderWindowFullRes
// proofread and fixed by F. Devernay on 3/10/2014
template <typename PIX,int nComponents>
static void
buildMipMapLevel(OFX::ImageEffect* instance,
                 const OfxRectI & originalRenderWindow,
                 const OfxRectI & renderWindowFullRes,
                 unsigned int level,
                 const PIX* srcPixels,
                 const OfxRectI & srcBounds,
                 int srcRowBytes,
                 PIX* dstPixels,
                 const OfxRectI & dstBounds,
                 int dstRowBytes)
{
    assert(level > 0);

    std::auto_ptr<OFX::ImageMemory> mem;
    size_t memSize = 0;
    std::auto_ptr<OFX::ImageMemory> tmpMem;
    size_t tmpMemSize = 0;
    PIX* nextImg = NULL;
    const PIX* previousImg = srcPixels;
    OfxRectI previousBounds = srcBounds;
    int previousRowBytes = srcRowBytes;
    OfxRectI nextRenderWindow = renderWindowFullRes;

    ///Build all the mipmap levels until we reach the one we are interested in
    for (unsigned int i = 1; i < level; ++i) {
        // loop invariant:
        // - previousImg, previousBounds, previousRowBytes describe the data ate the level before i
        // - nextRenderWindow contains the renderWindow at the level before i
        //
        ///Halve the smallest enclosing po2 rect as we need to render a minimum of the renderWindow
        nextRenderWindow = downscalePowerOfTwoSmallestEnclosing(nextRenderWindow, 1);
#     ifdef DEBUG
        {
            // check that doing i times 1 level is the same as doing i levels
            OfxRectI nrw = downscalePowerOfTwoSmallestEnclosing(renderWindowFullRes, i);
            assert(nrw.x1 == nextRenderWindow.x1 && nrw.x2 == nextRenderWindow.x2 && nrw.y1 == nextRenderWindow.y1 && nrw.y2 == nextRenderWindow.y2);
        }
#     endif
        ///Allocate a temporary image if necessary, or reuse the previously allocated buffer
        int nextRowBytes =  (nextRenderWindow.x2 - nextRenderWindow.x1)  * nComponents * sizeof(PIX);
        size_t newMemSize =  (nextRenderWindow.y2 - nextRenderWindow.y1) * nextRowBytes;
        if ( tmpMem.get() ) {
            // there should be enough memory: no need to reallocate
            assert(tmpMemSize >= memSize);
        } else {
            tmpMem.reset( new OFX::ImageMemory(newMemSize, instance) );
            tmpMemSize = newMemSize;
        }
        nextImg = (float*)tmpMem->lock();

        halveWindow<PIX, nComponents>(nextRenderWindow, previousImg, previousBounds, previousRowBytes, nextImg, nextRenderWindow, nextRowBytes);

        ///Switch for next pass
        previousBounds = nextRenderWindow;
        previousRowBytes = nextRowBytes;
        previousImg = nextImg;
        mem = tmpMem;
        memSize = tmpMemSize;
    }
    // here:
    // - previousImg, previousBounds, previousRowBytes describe the data ate the level before 'level'
    // - nextRenderWindow contains the renderWindow at the level before 'level'

    ///On the last iteration halve directly into the dstPixels
    ///The nextRenderWindow should be equal to the original render window.
    nextRenderWindow = downscalePowerOfTwoSmallestEnclosing(nextRenderWindow, 1);
    assert(originalRenderWindow.x1 == nextRenderWindow.x1 && originalRenderWindow.x2 == nextRenderWindow.x2 &&
           originalRenderWindow.y1 == nextRenderWindow.y1 && originalRenderWindow.y2 == nextRenderWindow.y2);

    halveWindow<PIX, nComponents>(nextRenderWindow, previousImg, previousBounds, previousRowBytes, dstPixels, dstBounds, dstRowBytes);
    // mem and tmpMem are freed at destruction
} // buildMipMapLevel

void
ofxsScalePixelData(OFX::ImageEffect* instance,
                   const OfxRectI & originalRenderWindow,
                   const OfxRectI & renderWindow,
                   unsigned int levels,
                   const void* srcPixelData,
                   OFX::PixelComponentEnum srcPixelComponents,
                   OFX::BitDepthEnum srcPixelDepth,
                   const OfxRectI & srcBounds,
                   int srcRowBytes,
                   void* dstPixelData,
                   OFX::PixelComponentEnum dstPixelComponents,
                   OFX::BitDepthEnum dstPixelDepth,
                   const OfxRectI & dstBounds,
                   int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);

    // do the rendering
    if ( ( dstPixelDepth != OFX::eBitDepthFloat) ||
         ( ( dstPixelComponents != OFX::ePixelComponentRGBA) &&
           ( dstPixelComponents != OFX::ePixelComponentRGB) &&
           ( dstPixelComponents != OFX::ePixelComponentAlpha) ) ||
         ( dstPixelDepth != srcPixelDepth) ||
         ( dstPixelComponents != srcPixelComponents) ) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    if (dstPixelComponents == OFX::ePixelComponentRGBA) {
        buildMipMapLevel<float, 4>(instance, originalRenderWindow, renderWindow, levels, (const float*)srcPixelData,
                                   srcBounds, srcRowBytes, (float*)dstPixelData, dstBounds, dstRowBytes);
    } else if (dstPixelComponents == OFX::ePixelComponentRGB) {
        buildMipMapLevel<float, 3>(instance, originalRenderWindow, renderWindow, levels, (const float*)srcPixelData,
                                   srcBounds, srcRowBytes, (float*)dstPixelData, dstBounds, dstRowBytes);
    }  else if (dstPixelComponents == OFX::ePixelComponentAlpha) {
        buildMipMapLevel<float, 1>(instance, originalRenderWindow, renderWindow,levels, (const float*)srcPixelData,
                                   srcBounds, srcRowBytes, (float*)dstPixelData, dstBounds, dstRowBytes);
    }     // switch
}

template <typename PIX,int nComponents>
static void
ofxsBuildMipMapsForComponents(OFX::ImageEffect* instance,
                              const OfxRectI & renderWindow,
                              const PIX* srcPixelData,
                              const OfxRectI & srcBounds,
                              int srcRowBytes,
                              unsigned int maxLevel,
                              MipMapsVector & mipmaps)
{
    const PIX* previousImg = srcPixels;
    OfxRectI previousBounds = srcBounds;
    int previousRowBytes = srcRowBytes;
    OfxRectI nextRenderWindow = renderWindow;

    ///Build all the mipmap levels until we reach the one we are interested in
    for (unsigned int i = 1; i <= maxLevel; ++i) {
        // loop invariant:
        // - previousImg, previousBounds, previousRowBytes describe the data ate the level before i
        // - nextRenderWindow contains the renderWindow at the level before i
        //
        ///Halve the smallest enclosing po2 rect as we need to render a minimum of the renderWindow
        nextRenderWindow = downscalePowerOfTwoSmallestEnclosing(nextRenderWindow, 1);
#     ifdef DEBUG
        {
            // check that doing i times 1 level is the same as doing i levels
            OfxRectI nrw = downscalePowerOfTwoSmallestEnclosing(renderWindowFullRes, i);
            assert(nrw.x1 == nextRenderWindow.x1 && nrw.x2 == nextRenderWindow.x2 && nrw.y1 == nextRenderWindow.y1 && nrw.y2 == nextRenderWindow.y2);
        }
#     endif
        assert(i - 1 >= 0);

        ///Allocate a temporary image if necessary, or reuse the previously allocated buffer
        int nextRowBytes = (nextRenderWindow.x2 - nextRenderWindow.x1)  * nComponents * sizeof(PIX);
        mipmaps[i - 1].memSize = (nextRenderWindow.y2 - nextRenderWindow.y1) * nextRowBytes;
        mipmaps[i - 1].bounds = nextRenderWindow;

        mipmaps[i - 1].data = new OFX::ImageMemory(mipmaps[i - 1].memSize, instance);
        tmpMemSize = newMemSize;

        float* nextImg = (float*)tmpMem->lock();

        halveWindow<PIX, nComponents>(nextRenderWindow, previousImg, previousBounds, previousRowBytes, nextImg, nextRenderWindow, nextRowBytes);

        ///Switch for next pass
        previousBounds = nextRenderWindow;
        previousRowBytes = nextRowBytes;
        previousImg = nextImg;
    }
}

void
ofxsBuildMipMaps(OFX::ImageEffect* instance,
                 const OfxRectI & renderWindow,
                 const void* srcPixelData,
                 OFX::PixelComponentEnum srcPixelComponents,
                 OFX::BitDepthEnum srcPixelDepth,
                 const OfxRectI & srcBounds,
                 int srcRowBytes,
                 unsigned int maxLevel,
                 MipMapsVector & mipmaps)
{
    assert(srcPixelData && mipmaps->size() == maxLevel);

    // do the rendering
    if ( ( srcPixelDepth != OFX::eBitDepthFloat) ||
         ( ( srcPixelComponents != OFX::ePixelComponentRGBA) &&
           ( srcPixelComponents != OFX::ePixelComponentRGB) &&
           ( srcPixelComponents != OFX::ePixelComponentAlpha) ) ) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    if (dstPixelComponents == OFX::ePixelComponentRGBA) {
        ofxsBuildMipMapsForComponents<float,4>(instance,renderWindow,srcPixelData,srcBounds,
                                               srcRowBytes,maxLevel,mipmaps);
    } else if (dstPixelComponents == OFX::ePixelComponentRGB) {
        ofxsBuildMipMapsForComponents<float,3>(instance,renderWindow,srcPixelData,srcBounds,
                                               srcRowBytes,maxLevel,mipmaps);
    }  else if (dstPixelComponents == OFX::ePixelComponentAlpha) {
        ofxsBuildMipMapsForComponents<float,1>(instance,renderWindow,srcPixelData,srcBounds,
                                               srcRowBytes,maxLevel,mipmaps);
    }
}
} // OFX
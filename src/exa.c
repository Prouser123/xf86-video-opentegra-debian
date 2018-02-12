/*
 * Copyright © 2014 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "driver.h"
#include "shaders.h"

#define ErrorMsg(fmt, args...)                                              \
    xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "%s:%d/%s(): " fmt, __FILE__,     \
               __LINE__, __func__, ##args)

#define BLUE(c)     (((c) & 0xff)         / 255.0f)
#define GREEN(c)    ((((c) >> 8)  & 0xff) / 255.0f)
#define RED(c)      ((((c) >> 16) & 0xff) / 255.0f)
#define ALPHA(c)    ((((c) >> 24) & 0xff) / 255.0f)

#define TegraPushVtxAttr(x, y, push)                                        \
    if (push) {                                                             \
        tegra->scratch.attribs->map[tegra->scratch.attrib_itr++] = x;       \
        tegra->scratch.attribs->map[tegra->scratch.attrib_itr++] = y;       \
    }

#define TegraSwapRedBlue(v)                                                 \
    ((v & 0xff00ff00) | (v & 0x00ff0000) >> 16 | (v & 0x000000ff) << 16)

#define TEGRA_ATTRIB_BUFFER_SIZE        0x1000

static const uint8_t rop3[] = {
    0x00, /* GXclear */
    0x88, /* GXand */
    0x44, /* GXandReverse */
    0xcc, /* GXcopy */
    0x22, /* GXandInverted */
    0xaa, /* GXnoop */
    0x66, /* GXxor */
    0xee, /* GXor */
    0x11, /* GXnor */
    0x99, /* GXequiv */
    0x55, /* GXinvert */
    0xdd, /* GXorReverse */
    0x33, /* GXcopyInverted */
    0xbb, /* GXorInverted */
    0x77, /* GXnand */
    0xff, /* GXset */
};

struct tegra_composit_config {
    /* prog[MASK_TEX_USED][SRC_TEX_USED] */
    struct shader_program *prog[2][2];
};

static const struct tegra_composit_config composit_cfgs[] = {
    [PictOpOver] = {
        .prog[1][1] = &prog_blend_over,
        .prog[0][1] = &prog_blend_over_solid_src,
        .prog[1][0] = &prog_blend_over_solid_mask,
        .prog[0][0] = &prog_blend_over_solid_mask_src,
    },

    [PictOpOverReverse] = {
        .prog[1][1] = &prog_blend_over_reverse,
        .prog[0][1] = &prog_blend_over_reverse_solid_src,
        .prog[1][0] = &prog_blend_over_reverse_solid_mask,
        .prog[0][0] = &prog_blend_over_reverse_solid_mask_src,
    },

    [PictOpAdd] = {
        .prog[1][1] = &prog_blend_add,
        .prog[0][1] = &prog_blend_add_solid_src,
        .prog[1][0] = &prog_blend_add_solid_mask,
        .prog[0][0] = &prog_blend_add_solid_mask_src,
    },

    [PictOpSrc] = {
        .prog[1][1] = &prog_blend_src,
        .prog[0][1] = &prog_blend_src_solid_src,
        .prog[1][0] = &prog_blend_src_solid_mask,
        .prog[0][0] = &prog_blend_src_solid_mask_src,
    },

    [PictOpClear] = {
        .prog[1][1] = &prog_blend_src_solid_mask_src,
        .prog[0][1] = &prog_blend_src_solid_mask_src,
        .prog[1][0] = &prog_blend_src_solid_mask_src,
        .prog[0][0] = &prog_blend_src_solid_mask_src,
    },

    [PictOpIn] = {
        .prog[1][1] = &prog_blend_in,
        .prog[0][1] = &prog_blend_in_solid_src,
        .prog[1][0] = &prog_blend_in_solid_mask,
        .prog[0][0] = &prog_blend_in_solid_mask_src,
    },

    [PictOpInReverse] = {
        .prog[1][1] = &prog_blend_in_reverse,
        .prog[0][1] = &prog_blend_in_reverse_solid_src,
        .prog[1][0] = &prog_blend_in_reverse_solid_mask,
        .prog[0][0] = &prog_blend_in_reverse_solid_mask_src,
    },

    [PictOpOut] = {
        .prog[1][1] = &prog_blend_out,
        .prog[0][1] = &prog_blend_out_solid_src,
        .prog[1][0] = &prog_blend_out_solid_mask,
        .prog[0][0] = &prog_blend_out_solid_mask_src,
    },

    [PictOpOutReverse] = {
        .prog[1][1] = &prog_blend_out_reverse,
        .prog[0][1] = &prog_blend_out_reverse_solid_src,
        .prog[1][0] = &prog_blend_out_reverse_solid_mask,
        .prog[0][0] = &prog_blend_out_reverse_solid_mask_src,
    },

    [PictOpDst] = {
        .prog[1][0] = &prog_blend_dst,
        .prog[0][0] = &prog_blend_dst_solid_mask,
    },

    [PictOpAtop] = {
        .prog[1][1] = &prog_blend_atop,
        .prog[0][1] = &prog_blend_atop_solid_src,
        .prog[1][0] = &prog_blend_atop_solid_mask,
        .prog[0][0] = &prog_blend_atop_solid_mask_src,
    },

    [PictOpAtopReverse] = {
        .prog[1][1] = &prog_blend_atop_reverse,
        .prog[0][1] = &prog_blend_atop_reverse_solid_src,
        .prog[1][0] = &prog_blend_atop_reverse_solid_mask,
        .prog[0][0] = &prog_blend_atop_reverse_solid_mask_src,
    },

    [PictOpXor] = {
        .prog[1][1] = &prog_blend_xor,
        .prog[0][1] = &prog_blend_xor_solid_src,
        .prog[1][0] = &prog_blend_xor_solid_mask,
        .prog[0][0] = &prog_blend_xor_solid_mask_src,
    },

    [PictOpSaturate] = {
        .prog[1][1] = &prog_blend_saturate,
        .prog[0][1] = &prog_blend_saturate_solid_src,
        .prog[1][0] = &prog_blend_saturate_solid_mask,
    },
};

static int TegraCompositeAllocateAttribBuffer(struct drm_tegra *drm,
                                              TegraEXAPtr tegra)
{
    struct tegra_attrib_bo *old = tegra->scratch.attribs;
    struct tegra_attrib_bo *new;
    int err;

    tegra->scratch.attribs_alloc_err = TRUE;

    new = calloc(1, sizeof(*new));
    if (!new)
        return -1;

    err = drm_tegra_bo_new(&new->bo, drm, 0, TEGRA_ATTRIB_BUFFER_SIZE);
    if (err) {
        free(new);
        return err;
    }

    err = drm_tegra_bo_map(new->bo, (void**)&new->map);
    if (err) {
        drm_tegra_bo_unref(new->bo);
        free(new);
        return err;
    }

    tegra->scratch.attribs_alloc_err = FALSE;
    tegra->scratch.attrib_itr = 0;
    tegra->scratch.attribs = new;
    new->next = old;

    return 0;
}

static void TegraCompositeReleaseAttribBuffers(TegraEXAScratchPtr scratch)
{
    if (scratch->attribs) {
        struct tegra_attrib_bo *attribs_bo = scratch->attribs;
        struct tegra_attrib_bo *next;

        while (attribs_bo) {
            next = attribs_bo->next;
            drm_tegra_bo_unref(attribs_bo->bo);
            free(attribs_bo);
            attribs_bo = next;
        }

        scratch->attribs = NULL;
        scratch->attrib_itr = 0;
    }
}

unsigned int TegraEXAPitch(unsigned int width, unsigned int height,
                           unsigned int bpp)
{
    unsigned int alignment = 64;

    /* GR3D texture sampler has specific alignment restrictions. */
    if (IS_POW2(width) && IS_POW2(height))
            alignment = 16;

    return TEGRA_PITCH_ALIGN(width, bpp, alignment);
}

static int TegraEXAMarkSync(ScreenPtr pScreen)
{
    /* TODO: implement */

    return 0;
}

static void TegraEXAWaitMarker(ScreenPtr pScreen, int marker)
{
    /* TODO: implement */
}

static Bool TegraEXAPrepareAccess(PixmapPtr pPix, int idx)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPix->drawable.pScreen);
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pPix);
    int err;

    err = drm_tegra_bo_map(priv->bo, &pPix->devPrivate.ptr);
    if (err < 0) {
        ErrorMsg("failed to map buffer object: %d\n", err);
        return FALSE;
    }

    return TRUE;
}

static void TegraEXAFinishAccess(PixmapPtr pPix, int idx)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPix->drawable.pScreen);
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pPix);
    int err;

    err = drm_tegra_bo_unmap(priv->bo);
    if (err < 0)
        ErrorMsg("failed to unmap buffer object: %d\n", err);
}

static Bool TegraEXAPixmapIsOffscreen(PixmapPtr pPix)
{
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pPix);

    return priv && priv->bo;
}

static void *TegraEXACreatePixmap2(ScreenPtr pScreen, int width, int height,
                                   int depth, int usage_hint, int bitsPerPixel,
                                   int *new_fb_pitch)
{
    TegraPixmapPtr pixmap;

    pixmap = calloc(1, sizeof(*pixmap));
    if (!pixmap)
        return NULL;

    *new_fb_pitch = TegraEXAPitch(width, height, bitsPerPixel);

    if (usage_hint == TEGRA_DRI_USAGE_HINT)
        pixmap->dri = TRUE;

    return pixmap;
}

static void TegraEXADestroyPixmap(ScreenPtr pScreen, void *driverPriv)
{
    TegraPixmapPtr priv = driverPriv;

    drm_tegra_bo_unref(priv->bo);
    free(priv->fallback);
    free(priv);
}

static Bool TegraEXAModifyPixmapHeader(PixmapPtr pPixmap, int width,
                                       int height, int depth, int bitsPerPixel,
                                       int devKind, pointer pPixData)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pPixmap);
    TegraPtr tegra = TegraPTR(pScrn);
    unsigned int size, pitch;
    Bool ret;
    int err;

    ret = miModifyPixmapHeader(pPixmap, width, height, depth, bitsPerPixel,
                               devKind, pPixData);
    if (!ret)
        return ret;

    drm_tegra_bo_unref(priv->bo);
    free(priv->fallback);

    priv->bo = NULL;
    priv->fallback = NULL;

    if (pPixData) {
        void *scanout;

        scanout = drmmode_map_front_bo(&tegra->drmmode);

        if (pPixData == scanout) {
            priv->bo = drmmode_get_front_bo(&tegra->drmmode);
            return TRUE;
        }

        /*
         * The pixmap can't be used for hardware acceleration, so dispose of
         * it.
         */
        pPixmap->devPrivate.ptr = pPixData;
        pPixmap->devKind = devKind;

        return FALSE;
    }

    width = pPixmap->drawable.width;
    height = pPixmap->drawable.height;
    pitch = exaGetPixmapPitch(pPixmap);
    size = pitch * height;

    if (!size)
        return FALSE;

    if (!priv->bo) {
        err = drm_tegra_bo_new(&priv->bo, tegra->drm, 0, size);
        if (err < 0) {
            if (!priv->dri)
                priv->fallback = malloc(size);

            ErrorMsg("failed to allocate %ux%u (%zu) buffer object: %d, "
                     "fallback allocation %s\n",
                     width, height, size, err,
                     priv->fallback ? "succeed" : "failed");

            if (!priv->fallback)
                return FALSE;
        }
    }

    pPixmap->devKind = pitch;
    pPixmap->devPrivate.ptr = priv->fallback;

    return TRUE;
}

static Bool TegraEXAPrepareSolid(PixmapPtr pPixmap, int op, Pixel planemask,
                                 Pixel color)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pPixmap);
    unsigned int bpp = pPixmap->drawable.bitsPerPixel;
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;
    int err;

    /*
     * It should be possible to support this, but let's bail for now
     */
    if (planemask != FB_ALLONES)
        return FALSE;

    /*
     * It should be possible to support all GX* raster operations given the
     * mapping in the rop3 table, but none other than GXcopy have been
     * validated.
     */
    if (op != GXcopy)
        return FALSE;

    if (bpp != 32 && bpp != 16 && bpp != 8)
        return FALSE;

    err = tegra_stream_begin(&tegra->cmds, tegra->gr2d);
    if (err < 0)
            return FALSE;

    tegra_stream_prep(&tegra->cmds, 15);
    tegra_stream_push_setclass(&tegra->cmds, HOST1X_CLASS_GR2D);
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_MASK(0x9, 0x9));
    tegra_stream_push(&tegra->cmds, 0x0000003a); /* trigger */
    tegra_stream_push(&tegra->cmds, 0x00000000); /* cmdsel */
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_NONINCR(0x35, 1));
    tegra_stream_push(&tegra->cmds, color);
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_MASK(0x1e, 0x7));
    tegra_stream_push(&tegra->cmds, 0x00000000); /* controlsecond */
    tegra_stream_push(&tegra->cmds, /* controlmain */
                      ((bpp >> 4) << 16) | /* bytes per pixel */
                      (1 << 6) |           /* fill mode */
                      (1 << 2)             /* turbo-fill */);
    tegra_stream_push(&tegra->cmds, rop3[op]); /* ropfade */
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_MASK(0x2b, 0x9));
    tegra_stream_push_reloc(&tegra->cmds, priv->bo,
                            exaGetPixmapOffset(pPixmap));
    tegra_stream_push(&tegra->cmds, exaGetPixmapPitch(pPixmap));
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_NONINCR(0x46, 1));
    tegra_stream_push(&tegra->cmds, 0); /* non-tiled */

    if (tegra->cmds.status != TEGRADRM_STREAM_CONSTRUCT) {
        tegra_stream_cleanup(&tegra->cmds);
        return FALSE;
    }

    tegra->scratch.ops = 0;

    return TRUE;
}

static void TegraEXASolid(PixmapPtr pPixmap,
                          int px1, int py1, int px2, int py2)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;

    tegra_stream_prep(&tegra->cmds, 3);
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_MASK(0x38, 0x5));
    tegra_stream_push(&tegra->cmds, (py2 - py1) << 16 | (px2 - px1));
    tegra_stream_push(&tegra->cmds, py1 << 16 | px1);
    tegra_stream_sync(&tegra->cmds, DRM_TEGRA_SYNCPT_COND_OP_DONE);

    tegra->scratch.ops++;
}

static void TegraEXADoneSolid(PixmapPtr pPixmap)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;

    if (tegra->scratch.ops) {
        tegra_stream_end(&tegra->cmds);
        tegra_stream_flush(&tegra->cmds);
    } else {
        tegra_stream_cleanup(&tegra->cmds);
    }
}

static Bool TegraEXAPrepareCopy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap,
                                int dx, int dy, int op, Pixel planemask)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    TegraPixmapPtr src = exaGetPixmapDriverPrivate(pSrcPixmap);
    TegraPixmapPtr dst = exaGetPixmapDriverPrivate(pDstPixmap);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;
    unsigned int bpp;
    int err;

    /*
     * It should be possible to support this, but let's bail for now
     */
    if (planemask != FB_ALLONES)
        return FALSE;

    /*
     * It should be possible to support all GX* raster operations given the
     * mapping in the rop3 table, but none other than GXcopy have been
     * validated.
     */
    if (op != GXcopy)
        return FALSE;

    /*
     * Some restrictions apply to the hardware accelerated copying.
     */
    bpp = pSrcPixmap->drawable.bitsPerPixel;

    if (bpp != 32 && bpp != 16 && bpp != 8)
        return FALSE;

    if (pDstPixmap->drawable.bitsPerPixel != bpp)
        return FALSE;

    err = tegra_stream_begin(&tegra->cmds, tegra->gr2d);
    if (err < 0)
            return FALSE;

    tegra_stream_prep(&tegra->cmds, 14);
    tegra_stream_push_setclass(&tegra->cmds, HOST1X_CLASS_GR2D);
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_MASK(0x9, 0x9));
    tegra_stream_push(&tegra->cmds, 0x0000003a); /* trigger */
    tegra_stream_push(&tegra->cmds, 0x00000000); /* cmdsel */
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_MASK(0x01e, 0x5));
    tegra_stream_push(&tegra->cmds, 0x00000000); /* controlsecond */
    tegra_stream_push(&tegra->cmds, rop3[op]); /* ropfade */
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_NONINCR(0x046, 1));
    /*
     * [20:20] destination write tile mode (0: linear, 1: tiled)
     * [ 0: 0] tile mode Y/RGB (0: linear, 1: tiled)
     */
    tegra_stream_push(&tegra->cmds, 0x00000000); /* tilemode */
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_MASK(0x2b, 0x149));

    tegra_stream_push_reloc(&tegra->cmds, dst->bo,
                            exaGetPixmapOffset(pDstPixmap));
    tegra_stream_push(&tegra->cmds,
                      exaGetPixmapPitch(pDstPixmap)); /* dstst */

    tegra_stream_push_reloc(&tegra->cmds, src->bo,
                            exaGetPixmapOffset(pSrcPixmap));
    tegra_stream_push(&tegra->cmds,
                      exaGetPixmapPitch(pSrcPixmap)); /* srcst */

    if (tegra->cmds.status != TEGRADRM_STREAM_CONSTRUCT) {
        tegra_stream_cleanup(&tegra->cmds);
        return FALSE;
    }

    tegra->scratch.ops = 0;

    return TRUE;
}

static void TegraEXACopy(PixmapPtr pDstPixmap, int srcX, int srcY, int dstX,
                         int dstY, int width, int height)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;
    uint32_t controlmain;

    /*
     * [20:20] source color depth (0: mono, 1: same)
     * [17:16] destination color depth (0: 8 bpp, 1: 16 bpp, 2: 32 bpp)
     * [10:10] y-direction (0: increment, 1: decrement)
     * [9:9] x-direction (0: increment, 1: decrement)
     */
    controlmain = (1 << 20) | ((pDstPixmap->drawable.bitsPerPixel >> 4) << 16);

    if (dstX > srcX) {
        controlmain |= 1 << 9;
        srcX += width - 1;
        dstX += width - 1;
    }

    if (dstY > srcY) {
        controlmain |= 1 << 10;
        srcY += height - 1;
        dstY += height - 1;
    }

    tegra_stream_prep(&tegra->cmds, 7);
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_INCR(0x01f, 1));
    tegra_stream_push(&tegra->cmds, controlmain);
    tegra_stream_push(&tegra->cmds, HOST1X_OPCODE_INCR(0x37, 0x4));
    tegra_stream_push(&tegra->cmds, height << 16 | width); /* srcsize */
    tegra_stream_push(&tegra->cmds, height << 16 | width); /* dstsize */
    tegra_stream_push(&tegra->cmds, srcY << 16 | srcX); /* srcps */
    tegra_stream_push(&tegra->cmds, dstY << 16 | dstX); /* dstps */
    tegra_stream_sync(&tegra->cmds, DRM_TEGRA_SYNCPT_COND_OP_DONE);

    tegra->scratch.ops++;
}

static void TegraEXADoneCopy(PixmapPtr pDstPixmap)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;

    if (tegra->scratch.ops) {
        tegra_stream_end(&tegra->cmds);
        tegra_stream_flush(&tegra->cmds);
    } else {
        tegra_stream_cleanup(&tegra->cmds);
    }
}

static const struct shader_program * TegraCompositeProgram3D(
                int op, PicturePtr pSrcPicture, PicturePtr pMaskPicture)
{
    const struct tegra_composit_config *cfg = &composit_cfgs[op];
    Bool mask_tex = (pMaskPicture && pMaskPicture->pDrawable);
    Bool src_tex = (pSrcPicture && pSrcPicture->pDrawable);

    if (op > PictOpSaturate)
        return NULL;

    return cfg->prog[src_tex][mask_tex];
}

static unsigned TegraCompositeFormatToGR3D(unsigned format)
{
    switch (format) {
    case PICT_a8:
        return TGR3D_PIXEL_FORMAT_A8;

    case PICT_r5g6b5:
    case PICT_b5g6r5:
        return TGR3D_PIXEL_FORMAT_RGB565;

    case PICT_x8b8g8r8:
    case PICT_a8b8g8r8:
    case PICT_x8r8g8b8:
    case PICT_a8r8g8b8:
        return TGR3D_PIXEL_FORMAT_RGBA8888;

    default:
        return 0;
    }
}

static Bool TegraCompositeFormatHasAlpha(unsigned format)
{
    switch (format) {
    case PICT_a8:
    case PICT_a8b8g8r8:
    case PICT_a8r8g8b8:
        return TRUE;

    default:
        return FALSE;
    }
}

static Bool TegraCompositeFormatSwapRedBlue3D(unsigned format)
{
    switch (format) {
    case PICT_x8b8g8r8:
    case PICT_a8b8g8r8:
    case PICT_r5g6b5:
        return TRUE;

    default:
        return FALSE;
    }
}

static Bool TegraCompositeFormatSwapRedBlue2D(unsigned format)
{
    switch (format) {
    case PICT_x8b8g8r8:
    case PICT_a8b8g8r8:
    case PICT_b5g6r5:
        return TRUE;

    default:
        return FALSE;
    }
}

static Bool TegraCompositeCheckTexture(PicturePtr pic, PixmapPtr pix)
{
    unsigned width, height;

    if (pic) {
        width = pic->pDrawable->width;
        height = pic->pDrawable->height;

        if (width > 2048 || height > 2048)
            return FALSE;

        if (!IS_POW2(width) || !IS_POW2(height)) {
            if (pic->filter == PictFilterBilinear)
                return FALSE;

            if (pic->repeat && (pic->repeatType == RepeatReflect ||
                                pic->repeatType == RepeatNormal))
                return FALSE;
        }
    } else if (pix) {
        width = pix->drawable.width;
        height = pix->drawable.height;

        if (width > 2048 || height > 2048)
            return FALSE;
    }

    return TRUE;
}

static void TegraCompositeSetupTexture(struct tegra_stream *cmds,
                                       unsigned index,
                                       PicturePtr pic,
                                       PixmapPtr pix)
{
    TegraPixmapPtr p = exaGetPixmapDriverPrivate(pix);
    Bool wrap_mirrored_repeat = FALSE;
    Bool wrap_clamp_to_edge = TRUE;
    Bool bilinear = FALSE;

    if (pic->repeat) {
        wrap_mirrored_repeat = (pic->repeatType == RepeatReflect);
        wrap_clamp_to_edge = (pic->repeatType == RepeatPad);
    }

    if (pic->filter == PictFilterBilinear)
        bilinear = TRUE;

    TegraGR3D_SetupTextureDesc(cmds, index, p->bo,
                               exaGetPixmapOffset(pix),
                               pix->drawable.width,
                               pix->drawable.height,
                               TegraCompositeFormatToGR3D(pic->format),
                               bilinear, false, bilinear,
                               wrap_clamp_to_edge,
                               wrap_mirrored_repeat);
}

static void TegraCompositeSetupAttributes(TegraEXAPtr tegra)
{
    struct tegra_exa_scratch *scratch = &tegra->scratch;
    struct tegra_stream *cmds = &tegra->cmds;
    unsigned attrs_num = 1 + !!scratch->pSrc + !!scratch->pMask;
    unsigned attribs_offset = scratch->attrib_itr * 2;

    TegraGR3D_SetupAttribute(cmds, 0, scratch->attribs->bo,
                             attribs_offset, TGR3D_ATTRIB_TYPE_FLOAT16,
                             2, 4 * attrs_num);

    if (scratch->pSrc) {
        attribs_offset += 4;

        TegraGR3D_SetupAttribute(cmds, 1, scratch->attribs->bo,
                                 attribs_offset, TGR3D_ATTRIB_TYPE_FLOAT16,
                                 2, 4 * attrs_num);
    }

    if (scratch->pMask) {
        attribs_offset += 4;

        TegraGR3D_SetupAttribute(cmds, 2, scratch->attribs->bo,
                                 attribs_offset, TGR3D_ATTRIB_TYPE_FLOAT16,
                                 2, 4 * attrs_num);
    }
}

static Bool TegraCompositeAttribBufferIsFull(TegraEXAScratchPtr scratch)
{
    unsigned attrs_num = 1 + !!scratch->pSrc + !!scratch->pMask;

    return (scratch->attrib_itr * 2 + attrs_num * 24 > TEGRA_ATTRIB_BUFFER_SIZE);
}

static void TegraEXACompositeDraw(TegraEXAPtr tegra)
{
    struct tegra_exa_scratch *scratch = &tegra->scratch;
    struct tegra_stream *cmds = &tegra->cmds;

    if (scratch->vtx_cnt) {
        TegraGR3D_SetupDrawParams(cmds, TGR3D_PRIMITIVE_TYPE_TRIANGLES,
                                  TGR3D_INDEX_MODE_NONE, 0);
        TegraGR3D_DrawPrimitives(cmds, 0, scratch->vtx_cnt);

        scratch->vtx_cnt = 0;
        scratch->ops++;
    }
}

static void TegraCompositeFlush(struct drm_tegra *drm, TegraEXAPtr tegra)
{
    TegraEXACompositeDraw(tegra);
    TegraCompositeAllocateAttribBuffer(drm, tegra);
    TegraCompositeSetupAttributes(tegra);
}

static Bool TegraEXACheckComposite(int op, PicturePtr pSrcPicture,
                                   PicturePtr pMaskPicture,
                                   PicturePtr pDstPicture)
{
    if (!TegraCompositeProgram3D(op, pSrcPicture, pMaskPicture))
        return FALSE;

    if (pDstPicture->format != PICT_x8r8g8b8 &&
        pDstPicture->format != PICT_a8r8g8b8 &&
        pDstPicture->format != PICT_x8b8g8r8 &&
        pDstPicture->format != PICT_a8b8g8r8 &&
        pDstPicture->format != PICT_r5g6b5 &&
        pDstPicture->format != PICT_b5g6r5 &&
        pDstPicture->format != PICT_a8)
        return FALSE;

    if (pSrcPicture) {
        if (pSrcPicture->format != PICT_x8r8g8b8 &&
            pSrcPicture->format != PICT_a8r8g8b8 &&
            pSrcPicture->format != PICT_x8b8g8r8 &&
            pSrcPicture->format != PICT_a8b8g8r8 &&
            pSrcPicture->format != PICT_r5g6b5 &&
            pSrcPicture->format != PICT_b5g6r5 &&
            pSrcPicture->format != PICT_a8)
            return FALSE;

        if (pSrcPicture->pDrawable) {
            if (pSrcPicture->transform)
                return FALSE;

            if (pSrcPicture->filter >= PictFilterConvolution)
                return FALSE;

            if (!TegraCompositeCheckTexture(pSrcPicture, NULL))
                return FALSE;
        } else {
            if (pSrcPicture->pSourcePict->type != SourcePictTypeSolidFill)
                return FALSE;
        }
    }

    if (pMaskPicture) {
        if (pMaskPicture->format != PICT_x8r8g8b8 &&
            pMaskPicture->format != PICT_a8r8g8b8 &&
            pMaskPicture->format != PICT_x8b8g8r8 &&
            pMaskPicture->format != PICT_a8b8g8r8 &&
            pMaskPicture->format != PICT_r5g6b5 &&
            pMaskPicture->format != PICT_b5g6r5 &&
            pMaskPicture->format != PICT_a8)
            return FALSE;

        if (pMaskPicture->pDrawable) {
            if (pMaskPicture->transform)
                return FALSE;

            if (pMaskPicture->filter >= PictFilterConvolution)
                return FALSE;

            if (!TegraCompositeCheckTexture(pMaskPicture, NULL))
                return FALSE;
        } else {
            if (pMaskPicture->pSourcePict->type != SourcePictTypeSolidFill)
                return FALSE;
        }
    }

    return TRUE;
}

static Pixel TegraPixelRGB565to888(Pixel pixel)
{
    Pixel p = 0;

    p |= 0xff000000;
    p |=  ((pixel >> 11)   * 255 + 15) / 31;
    p |=  (((pixel >> 5) & 0x3f) * 255 + 31) / 63;
    p |=  ((pixel & 0x3f)  * 255 + 15) / 31;

    return p;
}

static Pixel TegraPixelRGB888to565(Pixel pixel)
{
    unsigned red, green, blue;
    Pixel p = 0;

    red   = (pixel & 0x00ff0000) >> 16;
    green = (pixel & 0x0000ff00) >> 8;
    blue  = (pixel & 0x000000ff) >> 0;

    p |= ((red >> 3) & 0x1f) << 11;
    p |= ((green >> 2) & 0x3f) << 5;
    p |= (blue >> 3) & 0x1f;

    return p;
}

static Bool TegraEXAPrepareComposite2D(int op,
                                       PicturePtr pSrcPicture,
                                       PicturePtr pMaskPicture,
                                       PicturePtr pDstPicture,
                                       PixmapPtr pSrc,
                                       PixmapPtr pMask,
                                       PixmapPtr pDst)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDst->drawable.pScreen);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;
    Pixel solid, mask;

    if ((pSrcPicture && pSrcPicture->pDrawable) ||
        (pMaskPicture && pMaskPicture->pDrawable))
        return FALSE;

    tegra->scratch.solid2D = TRUE;

    if (op == PictOpSrc) {
        solid = pSrcPicture ? pSrcPicture->pSourcePict->solidFill.color :
                              0x00000000;

        mask = pMaskPicture ? pMaskPicture->pSourcePict->solidFill.color :
                              0xffffffff;

        if (pSrcPicture) {
            if (pSrcPicture->format == PICT_r5g6b5 ||
                pSrcPicture->format == PICT_b5g6r5)
                solid = TegraPixelRGB565to888(solid);

            if (TegraCompositeFormatSwapRedBlue2D(pDstPicture->format) !=
                TegraCompositeFormatSwapRedBlue2D(pSrcPicture->format))
                solid = TegraSwapRedBlue(solid);
        }

        if (pMaskPicture) {
            if (pMaskPicture->format == PICT_r5g6b5 ||
                pMaskPicture->format == PICT_b5g6r5)
                mask = TegraPixelRGB565to888(mask);

            if (TegraCompositeFormatSwapRedBlue2D(pDstPicture->format) !=
                TegraCompositeFormatSwapRedBlue2D(pMaskPicture->format))
                mask = TegraSwapRedBlue(mask);
        }

        solid &= mask;

        if (pDstPicture->format == PICT_r5g6b5 ||
            pDstPicture->format == PICT_b5g6r5)
            solid = TegraPixelRGB888to565(solid);

        return TegraEXAPrepareSolid(pDst, GXcopy, FB_ALLONES, solid);
    }

    if (op == PictOpClear)
        return TegraEXAPrepareSolid(pDst, GXcopy, FB_ALLONES, 0x00000000);

    return FALSE;
}

static Bool TegraEXAPrepareComposite3D(int op,
                                       PicturePtr pSrcPicture,
                                       PicturePtr pMaskPicture,
                                       PicturePtr pDstPicture,
                                       PixmapPtr pSrc,
                                       PixmapPtr pMask,
                                       PixmapPtr pDst)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDst->drawable.pScreen);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;
    struct tegra_stream *cmds = &tegra->cmds;
    const struct shader_program *prog;
    TegraPixmapPtr dst = exaGetPixmapDriverPrivate(pDst);
    Bool mask_tex = (pMaskPicture && pMaskPicture->pDrawable);
    Bool src_tex = (pSrcPicture && pSrcPicture->pDrawable);
    Bool clamp_src = FALSE;
    Bool clamp_mask = FALSE;
    Bool swap_red_blue;
    Bool dst_alpha;
    Bool alpha;
    Pixel solid;
    int err;

    prog = TegraCompositeProgram3D(op, pSrcPicture, pMaskPicture);
    if (!prog)
        return FALSE;

    tegra->scratch.solid2D = FALSE;
    tegra->scratch.pMask = (op != PictOpClear && mask_tex) ? pMask : NULL;
    tegra->scratch.pSrc = (op != PictOpClear && src_tex) ? pSrc : NULL;
    tegra->scratch.ops = 0;

    err = TegraCompositeAllocateAttribBuffer(TegraPTR(pScrn)->drm, tegra);
    if (err)
        return FALSE;

    err = tegra_stream_begin(cmds, tegra->gr3d);
    if (err)
        return FALSE;

    tegra_stream_prep(&tegra->cmds, 1);
    tegra_stream_push_setclass(cmds, HOST1X_CLASS_GR3D);

    TegraGR3D_Initialize(cmds, prog);

    TegraGR3D_SetupScissor(cmds, 0, 0,
                           pDst->drawable.width,
                           pDst->drawable.height);
    TegraGR3D_SetupViewportBiasScale(cmds, 0.0f, 0.0f, 0.5f,
                                     pDst->drawable.width,
                                     pDst->drawable.height, 0.5f);
    TegraGR3D_SetupRenderTarget(cmds, 1, dst->bo, exaGetPixmapOffset(pDst),
                                TegraCompositeFormatToGR3D(pDstPicture->format),
                                exaGetPixmapPitch(pDst));
    TegraGR3D_EnableRenderTargets(cmds, 1 << 1);

    TegraCompositeSetupAttributes(tegra);

    dst_alpha = TegraCompositeFormatHasAlpha(pDstPicture->format);

    if (tegra->scratch.pSrc) {
        clamp_src = !pSrcPicture->repeat;

        TegraCompositeSetupTexture(cmds, 0, pSrcPicture, pSrc);

        swap_red_blue = TegraCompositeFormatSwapRedBlue3D(pDstPicture->format) !=
                        TegraCompositeFormatSwapRedBlue3D(pSrcPicture->format);

        alpha = TegraCompositeFormatHasAlpha(pSrcPicture->format);
        TegraGR3D_UploadConstFP(cmds, 5, FX10x2(alpha, swap_red_blue));
    } else {
        if (op != PictOpClear && pSrcPicture) {
            solid = pSrcPicture->pSourcePict->solidFill.color;

            if (pSrcPicture->format == PICT_r5g6b5 ||
                pSrcPicture->format == PICT_b5g6r5)
                solid = TegraPixelRGB565to888(solid);
        } else {
            solid = 0x00000000;
        }

        if (TegraCompositeFormatSwapRedBlue3D(pDstPicture->format) !=
            TegraCompositeFormatSwapRedBlue3D(pSrcPicture->format))
            solid = TegraSwapRedBlue(solid);

        TegraGR3D_UploadConstFP(cmds, 0, FX10x2(BLUE(solid), GREEN(solid)));
        TegraGR3D_UploadConstFP(cmds, 1, FX10x2(RED(solid), ALPHA(solid)));
    }

    if (tegra->scratch.pMask) {
        clamp_mask = !pMaskPicture->repeat;

        TegraCompositeSetupTexture(cmds, 1, pMaskPicture, pMask);

        swap_red_blue = TegraCompositeFormatSwapRedBlue3D(pDstPicture->format) !=
                        TegraCompositeFormatSwapRedBlue3D(pMaskPicture->format);

        alpha = TegraCompositeFormatHasAlpha(pMaskPicture->format);
        TegraGR3D_UploadConstFP(cmds, 6, FX10x2(pMaskPicture->componentAlpha, alpha));
        TegraGR3D_UploadConstFP(cmds, 7, FX10x2(swap_red_blue, clamp_mask));
    } else {
        if (op != PictOpClear && pMaskPicture) {
            solid = pMaskPicture->pSourcePict->solidFill.color;

            if (pMaskPicture->format == PICT_r5g6b5 ||
                pMaskPicture->format == PICT_b5g6r5)
                solid = TegraPixelRGB565to888(solid);

            if (!pMaskPicture->componentAlpha)
                solid |= solid >> 24 | solid >> 16 | solid >> 8;

            if (TegraCompositeFormatSwapRedBlue3D(pDstPicture->format) !=
                TegraCompositeFormatSwapRedBlue3D(pMaskPicture->format))
                solid = TegraSwapRedBlue(solid);
        } else {
            solid = 0xffffffff;
        }

        TegraGR3D_UploadConstFP(cmds, 2, FX10x2(BLUE(solid), GREEN(solid)));
        TegraGR3D_UploadConstFP(cmds, 3, FX10x2(RED(solid), ALPHA(solid)));
    }

    TegraGR3D_UploadConstFP(cmds, 8, FX10x2(dst_alpha, clamp_src));
    TegraGR3D_UploadConstVP(cmds, 0, 0.0f, 0.0f, 0.0f, 1.0f);

    if (cmds->status != TEGRADRM_STREAM_CONSTRUCT) {
        tegra_stream_cleanup(cmds);
        return FALSE;
    }

    return TRUE;
}

static Bool TegraEXAPrepareComposite(int op, PicturePtr pSrcPicture,
                                     PicturePtr pMaskPicture,
                                     PicturePtr pDstPicture,
                                     PixmapPtr pSrc,
                                     PixmapPtr pMask,
                                     PixmapPtr pDst)
{
    /* Use GR2D for simple solid fills as usually it is more optimal. */
    if (TegraEXAPrepareComposite2D(op, pSrcPicture, pMaskPicture,
                                   pDstPicture, pSrc, pMask, pDst))
        return TRUE;

    return TegraEXAPrepareComposite3D(op, pSrcPicture, pMaskPicture,
                                      pDstPicture, pSrc, pMask, pDst);
}

static void TegraEXAComposite(PixmapPtr pDst,
                              int srcX, int srcY,
                              int maskX, int maskY,
                              int dstX, int dstY,
                              int width, int height)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDst->drawable.pScreen);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;
    float dst_left, dst_right, dst_top, dst_bottom;
    float src_left, src_right, src_top, src_bottom;
    float mask_left, mask_right, mask_top, mask_bottom;
    bool push_mask = !!tegra->scratch.pMask;
    bool push_src = !!tegra->scratch.pSrc;

    if (tegra->scratch.solid2D)
        return TegraEXASolid(pDst, dstX, dstY, dstX + width, dstY + height);

    /* do not proceed if previous reallocation failed */
    if (tegra->scratch.attribs_alloc_err)
        return;

    /* if attributes buffer is full, "flush" it and allocate new buffer */
    if (TegraCompositeAttribBufferIsFull(&tegra->scratch))
        TegraCompositeFlush(TegraPTR(pScrn)->drm, tegra);

    /* do not proceed if current reallocation failed */
    if (tegra->scratch.attribs_alloc_err)
        return;

    if (push_src) {
        src_left   = (float) srcX   / tegra->scratch.pSrc->drawable.width;
        src_right  = (float) width  / tegra->scratch.pSrc->drawable.width + src_left;
        src_bottom = (float) srcY   / tegra->scratch.pSrc->drawable.height;
        src_top    = (float) height / tegra->scratch.pSrc->drawable.height + src_bottom;
    }

    if (push_mask) {
        mask_left   = (float) maskX  / tegra->scratch.pMask->drawable.width;
        mask_right  = (float) width  / tegra->scratch.pMask->drawable.width + mask_left;
        mask_bottom = (float) maskY  / tegra->scratch.pMask->drawable.height;
        mask_top    = (float) height / tegra->scratch.pMask->drawable.height + mask_bottom;
    }

    dst_left   = (float) (dstX   * 2) / pDst->drawable.width  - 1.0f;
    dst_right  = (float) (width  * 2) / pDst->drawable.width  + dst_left;
    dst_bottom = (float) (dstY   * 2) / pDst->drawable.height - 1.0f;
    dst_top    = (float) (height * 2) / pDst->drawable.height + dst_bottom;

    /* push first triangle of the quad to attributes buffer */
    TegraPushVtxAttr(dst_left,  dst_bottom,  true);
    TegraPushVtxAttr(src_left,  src_bottom,  push_src);
    TegraPushVtxAttr(mask_left, mask_bottom, push_mask);

    TegraPushVtxAttr(dst_left,  dst_top,  true);
    TegraPushVtxAttr(src_left,  src_top,  push_src);
    TegraPushVtxAttr(mask_left, mask_top, push_mask);

    TegraPushVtxAttr(dst_right,  dst_top,  true);
    TegraPushVtxAttr(src_right,  src_top,  push_src);
    TegraPushVtxAttr(mask_right, mask_top, push_mask);

    /* push second */
    TegraPushVtxAttr(dst_right,  dst_top,  true);
    TegraPushVtxAttr(src_right,  src_top,  push_src);
    TegraPushVtxAttr(mask_right, mask_top, push_mask);

    TegraPushVtxAttr(dst_right,  dst_bottom,  true);
    TegraPushVtxAttr(src_right,  src_bottom,  push_src);
    TegraPushVtxAttr(mask_right, mask_bottom, push_mask);

    TegraPushVtxAttr(dst_left,  dst_bottom,  true);
    TegraPushVtxAttr(src_left,  src_bottom,  push_src);
    TegraPushVtxAttr(mask_left, mask_bottom, push_mask);

    tegra->scratch.vtx_cnt += 6;
}

static void TegraEXADoneComposite(PixmapPtr pDst)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDst->drawable.pScreen);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;

    if (tegra->scratch.solid2D)
        return TegraEXADoneSolid(pDst);

    TegraEXACompositeDraw(tegra);

    if (tegra->scratch.ops) {
        /*
         * XXX: Glitches may occur due to lack of support for waitchecks
         *      by kernel driver, they are required for 3D engine to complete
         *      data prefetching before starting to render. Alternative would
         *      be to flush the job, but that impacts performance very
         *      significantly and just happens to minimize the issue, so we
         *      choose glitches to low performance. Mostly fonts rendering is
         *      affected.
         *
         *      See TegraGR3D_DrawPrimitives() in gr3d.c
         */
        tegra_stream_end(&tegra->cmds);
        tegra_stream_flush(&tegra->cmds);
    } else {
        tegra_stream_cleanup(&tegra->cmds);
    }

    TegraCompositeReleaseAttribBuffers(&tegra->scratch);

    /* buffer reallocation could fail, cleanup it now */
    if (tegra->scratch.attribs_alloc_err)
        tegra->scratch.attribs_alloc_err = FALSE;
}

static Bool
TegraEXADownloadFromScreen(PixmapPtr pSrc, int x, int y, int w, int h,
                           char *dst, int pitch)
{
    return FALSE;
}

void TegraEXAScreenInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TegraPtr tegra = TegraPTR(pScrn);
    ExaDriverPtr exa;
    TegraEXAPtr priv;
    int err;

    if (tegra->drmmode.shadow_enable) {
        ErrorMsg("using \"Shadow Framebuffer\" - acceleration disabled\n");
        return;
    }

    exa = exaDriverAlloc();
    if (!exa) {
        ErrorMsg("EXA allocation failed\n");
        return;
    }

    priv = calloc(1, sizeof(*priv));
    if (!priv) {
        ErrorMsg("EXA allocation failed\n");
        goto free_exa;
    }

    err = drm_tegra_channel_open(&priv->gr2d, tegra->drm, DRM_TEGRA_GR2D);
    if (err < 0) {
        ErrorMsg("failed to open 2D channel: %d\n", err);
        goto free_priv;
    }

    err = drm_tegra_channel_open(&priv->gr3d, tegra->drm, DRM_TEGRA_GR3D);
    if (err < 0) {
        ErrorMsg("failed to open 3D channel: %d\n", err);
        goto close_gr2d;
    }

    err = tegra_stream_create(&priv->cmds);
    if (err < 0) {
        ErrorMsg("failed to create command stream: %d\n", err);
        goto close_gr3d;
    }

    exa->exa_major = EXA_VERSION_MAJOR;
    exa->exa_minor = EXA_VERSION_MINOR;
    exa->pixmapOffsetAlign = 256;
    exa->pixmapPitchAlign = TegraEXAPitch(1, 1, 32);
    exa->flags = EXA_SUPPORTS_PREPARE_AUX |
                 EXA_OFFSCREEN_PIXMAPS |
                 EXA_HANDLES_PIXMAPS;

    exa->maxX = 8192;
    exa->maxY = 8192;

    exa->MarkSync = TegraEXAMarkSync;
    exa->WaitMarker = TegraEXAWaitMarker;

    exa->PrepareAccess = TegraEXAPrepareAccess;
    exa->FinishAccess = TegraEXAFinishAccess;
    exa->PixmapIsOffscreen = TegraEXAPixmapIsOffscreen;

    exa->CreatePixmap2 = TegraEXACreatePixmap2;
    exa->DestroyPixmap = TegraEXADestroyPixmap;
    exa->ModifyPixmapHeader = TegraEXAModifyPixmapHeader;

    exa->PrepareSolid = TegraEXAPrepareSolid;
    exa->Solid = TegraEXASolid;
    exa->DoneSolid = TegraEXADoneSolid;

    exa->PrepareCopy = TegraEXAPrepareCopy;
    exa->Copy = TegraEXACopy;
    exa->DoneCopy = TegraEXADoneCopy;

    exa->CheckComposite = TegraEXACheckComposite;
    exa->PrepareComposite = TegraEXAPrepareComposite;
    exa->Composite = TegraEXAComposite;
    exa->DoneComposite = TegraEXADoneComposite;

    exa->DownloadFromScreen = TegraEXADownloadFromScreen;

    if (!exaDriverInit(pScreen, exa)) {
        ErrorMsg("EXA initialization failed\n");
        goto destroy_stream;
    }

    priv->driver = exa;
    tegra->exa = priv;

    return;

destroy_stream:
    tegra_stream_destroy(&priv->cmds);
close_gr3d:
    drm_tegra_channel_close(priv->gr3d);
close_gr2d:
    drm_tegra_channel_close(priv->gr2d);
free_priv:
    free(priv);
free_exa:
    free(exa);
}

void TegraEXAScreenExit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TegraPtr tegra = TegraPTR(pScrn);
    TegraEXAPtr priv = tegra->exa;

    if (priv) {
        exaDriverFini(pScreen);
        free(priv->driver);

        tegra_stream_destroy(&priv->cmds);
        drm_tegra_channel_close(priv->gr2d);
        drm_tegra_channel_close(priv->gr3d);
        TegraCompositeReleaseAttribBuffers(&priv->scratch);
        free(priv);
    }
}

/* vim: set et sts=4 sw=4 ts=4: */

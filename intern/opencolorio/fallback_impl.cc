/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2012 Blender Foundation.
 * All rights reserved.
 */

#include <algorithm>
#include <cstring>
#include <vector>

#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "MEM_guardedalloc.h"

#include "ocio_impl.h"

using std::max;

#define CONFIG_DEFAULT ((OCIO_ConstConfigRcPtr *)1)

enum TransformType {
  TRANSFORM_LINEAR_TO_SRGB,
  TRANSFORM_SRGB_TO_LINEAR,
  TRANSFORM_SCALE,
  TRANSFORM_EXPONENT,
  TRANSFORM_UNKNOWN,
};

#define COLORSPACE_LINEAR ((OCIO_ConstColorSpaceRcPtr *)1)
#define COLORSPACE_SRGB ((OCIO_ConstColorSpaceRcPtr *)2)

typedef struct OCIO_PackedImageDescription {
  float *data;
  long width;
  long height;
  long numChannels;
  long chanStrideBytes;
  long xStrideBytes;
  long yStrideBytes;
} OCIO_PackedImageDescription;

struct FallbackTransform {
  FallbackTransform() : type(TRANSFORM_UNKNOWN), scale(1.0f), exponent(1.0f)
  {
  }

  virtual ~FallbackTransform()
  {
  }

  void applyRGB(float *pixel)
  {
    if (type == TRANSFORM_LINEAR_TO_SRGB) {
      pixel[0] *= scale;
      pixel[1] *= scale;
      pixel[2] *= scale;

      linearrgb_to_srgb_v3_v3(pixel, pixel);

      pixel[0] = powf(max(0.0f, pixel[0]), exponent);
      pixel[1] = powf(max(0.0f, pixel[1]), exponent);
      pixel[2] = powf(max(0.0f, pixel[2]), exponent);
    }
    else if (type == TRANSFORM_SRGB_TO_LINEAR) {
      srgb_to_linearrgb_v3_v3(pixel, pixel);
    }
    else if (type == TRANSFORM_EXPONENT) {
      pixel[0] = powf(max(0.0f, pixel[0]), exponent);
      pixel[1] = powf(max(0.0f, pixel[1]), exponent);
      pixel[2] = powf(max(0.0f, pixel[2]), exponent);
    }
    else if (type == TRANSFORM_SCALE) {
      pixel[0] *= scale;
      pixel[1] *= scale;
      pixel[2] *= scale;
    }
  }

  void applyRGBA(float *pixel)
  {
    applyRGB(pixel);
  }

  TransformType type;
  /* Scale transform. */
  float scale;
  /* Exponent transform. */
  float exponent;

  MEM_CXX_CLASS_ALLOC_FUNCS("FallbackTransform");
};

struct FallbackProcessor {
  FallbackProcessor(FallbackTransform *transform) : transform(transform)
  {
  }

  FallbackProcessor(const FallbackProcessor &other)
  {
    transform = new FallbackTransform(*other.transform);
  }

  ~FallbackProcessor()
  {
    delete transform;
  }

  void applyRGB(float *pixel)
  {
    transform->applyRGB(pixel);
  }

  void applyRGBA(float *pixel)
  {
    transform->applyRGBA(pixel);
  }

  FallbackTransform *transform;

  MEM_CXX_CLASS_ALLOC_FUNCS("FallbackProcessor");
};

OCIO_ConstConfigRcPtr *FallbackImpl::getCurrentConfig(void)
{
  return CONFIG_DEFAULT;
}

void FallbackImpl::setCurrentConfig(const OCIO_ConstConfigRcPtr * /*config*/)
{
}

OCIO_ConstConfigRcPtr *FallbackImpl::configCreateFromEnv(void)
{
  return NULL;
}

OCIO_ConstConfigRcPtr *FallbackImpl::configCreateFromFile(const char * /*filename*/)
{
  return CONFIG_DEFAULT;
}

void FallbackImpl::configRelease(OCIO_ConstConfigRcPtr * /*config*/)
{
}

int FallbackImpl::configGetNumColorSpaces(OCIO_ConstConfigRcPtr * /*config*/)
{
  return 2;
}

const char *FallbackImpl::configGetColorSpaceNameByIndex(OCIO_ConstConfigRcPtr * /*config*/,
                                                         int index)
{
  if (index == 0)
    return "Linear";
  else if (index == 1)
    return "sRGB";

  return NULL;
}

OCIO_ConstColorSpaceRcPtr *FallbackImpl::configGetColorSpace(OCIO_ConstConfigRcPtr * /*config*/,
                                                             const char *name)
{
  if (strcmp(name, "scene_linear") == 0)
    return COLORSPACE_LINEAR;
  else if (strcmp(name, "color_picking") == 0)
    return COLORSPACE_SRGB;
  else if (strcmp(name, "texture_paint") == 0)
    return COLORSPACE_LINEAR;
  else if (strcmp(name, "default_byte") == 0)
    return COLORSPACE_SRGB;
  else if (strcmp(name, "default_float") == 0)
    return COLORSPACE_LINEAR;
  else if (strcmp(name, "default_sequencer") == 0)
    return COLORSPACE_SRGB;
  else if (strcmp(name, "Linear") == 0)
    return COLORSPACE_LINEAR;
  else if (strcmp(name, "sRGB") == 0)
    return COLORSPACE_SRGB;

  return NULL;
}

int FallbackImpl::configGetIndexForColorSpace(OCIO_ConstConfigRcPtr *config, const char *name)
{
  OCIO_ConstColorSpaceRcPtr *cs = configGetColorSpace(config, name);

  if (cs == COLORSPACE_LINEAR) {
    return 0;
  }
  else if (cs == COLORSPACE_SRGB) {
    return 1;
  }
  return -1;
}

const char *FallbackImpl::configGetDefaultDisplay(OCIO_ConstConfigRcPtr * /*config*/)
{
  return "sRGB";
}

int FallbackImpl::configGetNumDisplays(OCIO_ConstConfigRcPtr * /*config*/)
{
  return 1;
}

const char *FallbackImpl::configGetDisplay(OCIO_ConstConfigRcPtr * /*config*/, int index)
{
  if (index == 0) {
    return "sRGB";
  }
  return NULL;
}

const char *FallbackImpl::configGetDefaultView(OCIO_ConstConfigRcPtr * /*config*/,
                                               const char * /*display*/)
{
  return "Standard";
}

int FallbackImpl::configGetNumViews(OCIO_ConstConfigRcPtr * /*config*/, const char * /*display*/)
{
  return 1;
}

const char *FallbackImpl::configGetView(OCIO_ConstConfigRcPtr * /*config*/,
                                        const char * /*display*/,
                                        int index)
{
  if (index == 0) {
    return "Standard";
  }
  return NULL;
}

const char *FallbackImpl::configGetDisplayColorSpaceName(OCIO_ConstConfigRcPtr * /*config*/,
                                                         const char * /*display*/,
                                                         const char * /*view*/)
{
  return "sRGB";
}

void FallbackImpl::configGetDefaultLumaCoefs(OCIO_ConstConfigRcPtr * /*config*/, float *rgb)
{
  /* Here we simply use the older Blender assumed primaries of
   * ITU-BT.709 / sRGB, or 0.2126729 0.7151522 0.0721750. Brute
   * force stupid, but only plausible option given no color management
   * system in place.
   */

  rgb[0] = 0.2126f;
  rgb[1] = 0.7152f;
  rgb[2] = 0.0722f;
}

void FallbackImpl::configGetXYZtoRGB(OCIO_ConstConfigRcPtr * /*config*/, float xyz_to_rgb[3][3])
{
  /* Default to ITU-BT.709. */
  memcpy(xyz_to_rgb, OCIO_XYZ_TO_LINEAR_SRGB, sizeof(OCIO_XYZ_TO_LINEAR_SRGB));
}

int FallbackImpl::configGetNumLooks(OCIO_ConstConfigRcPtr * /*config*/)
{
  return 0;
}

const char *FallbackImpl::configGetLookNameByIndex(OCIO_ConstConfigRcPtr * /*config*/,
                                                   int /*index*/)
{
  return "";
}

OCIO_ConstLookRcPtr *FallbackImpl::configGetLook(OCIO_ConstConfigRcPtr * /*config*/,
                                                 const char * /*name*/)
{
  return NULL;
}

const char *FallbackImpl::lookGetProcessSpace(OCIO_ConstLookRcPtr * /*look*/)
{
  return NULL;
}

void FallbackImpl::lookRelease(OCIO_ConstLookRcPtr * /*look*/)
{
}

int FallbackImpl::colorSpaceIsInvertible(OCIO_ConstColorSpaceRcPtr * /*cs*/)
{
  return 1;
}

int FallbackImpl::colorSpaceIsData(OCIO_ConstColorSpaceRcPtr * /*cs*/)
{
  return 0;
}

void FallbackImpl::colorSpaceIsBuiltin(OCIO_ConstConfigRcPtr * /*config*/,
                                       OCIO_ConstColorSpaceRcPtr *cs,
                                       bool &is_scene_linear,
                                       bool &is_srgb)
{
  if (cs == COLORSPACE_LINEAR) {
    is_scene_linear = true;
    is_srgb = false;
  }
  else if (cs == COLORSPACE_SRGB) {
    is_scene_linear = false;
    is_srgb = true;
  }
  else {
    is_scene_linear = false;
    is_srgb = false;
  }
}

void FallbackImpl::colorSpaceRelease(OCIO_ConstColorSpaceRcPtr * /*cs*/)
{
}

OCIO_ConstProcessorRcPtr *FallbackImpl::configGetProcessorWithNames(OCIO_ConstConfigRcPtr *config,
                                                                    const char *srcName,
                                                                    const char *dstName)
{
  OCIO_ConstColorSpaceRcPtr *cs_src = configGetColorSpace(config, srcName);
  OCIO_ConstColorSpaceRcPtr *cs_dst = configGetColorSpace(config, dstName);
  FallbackTransform *transform = new FallbackTransform();
  if (cs_src == COLORSPACE_LINEAR && cs_dst == COLORSPACE_SRGB) {
    transform->type = TRANSFORM_LINEAR_TO_SRGB;
  }
  else if (cs_src == COLORSPACE_SRGB && cs_dst == COLORSPACE_LINEAR) {
    transform->type = TRANSFORM_SRGB_TO_LINEAR;
  }
  else {
    transform->type = TRANSFORM_UNKNOWN;
  }
  return (OCIO_ConstProcessorRcPtr *)new FallbackProcessor(transform);
}

OCIO_ConstCPUProcessorRcPtr *FallbackImpl::processorGetCPUProcessor(OCIO_ConstProcessorRcPtr *p)
{
  FallbackProcessor *processor = (FallbackProcessor *)p;
  return (OCIO_ConstCPUProcessorRcPtr *)new FallbackProcessor(*processor);
}

void FallbackImpl::processorApply(OCIO_ConstCPUProcessorRcPtr *processor,
                                  OCIO_PackedImageDesc *img)
{
  /* OCIO_TODO stride not respected, channels must be 3 or 4 */
  OCIO_PackedImageDescription *desc = (OCIO_PackedImageDescription *)img;
  int channels = desc->numChannels;
  float *pixels = desc->data;
  int width = desc->width;
  int height = desc->height;
  int x, y;

  for (y = 0; y < height; y++) {
    for (x = 0; x < width; x++) {
      float *pixel = pixels + channels * (y * width + x);

      if (channels == 4)
        processorApplyRGBA(processor, pixel);
      else if (channels == 3)
        processorApplyRGB(processor, pixel);
    }
  }
}

void FallbackImpl::processorApply_predivide(OCIO_ConstCPUProcessorRcPtr *processor,
                                            OCIO_PackedImageDesc *img)
{
  /* OCIO_TODO stride not respected, channels must be 3 or 4 */
  OCIO_PackedImageDescription *desc = (OCIO_PackedImageDescription *)img;
  int channels = desc->numChannels;
  float *pixels = desc->data;
  int width = desc->width;
  int height = desc->height;
  int x, y;

  for (y = 0; y < height; y++) {
    for (x = 0; x < width; x++) {
      float *pixel = pixels + channels * (y * width + x);

      if (channels == 4)
        processorApplyRGBA_predivide(processor, pixel);
      else if (channels == 3)
        processorApplyRGB(processor, pixel);
    }
  }
}

void FallbackImpl::processorApplyRGB(OCIO_ConstCPUProcessorRcPtr *processor, float *pixel)
{
  ((FallbackProcessor *)processor)->applyRGB(pixel);
}

void FallbackImpl::processorApplyRGBA(OCIO_ConstCPUProcessorRcPtr *processor, float *pixel)
{
  ((FallbackProcessor *)processor)->applyRGBA(pixel);
}

void FallbackImpl::processorApplyRGBA_predivide(OCIO_ConstCPUProcessorRcPtr *processor,
                                                float *pixel)
{
  if (pixel[3] == 1.0f || pixel[3] == 0.0f) {
    processorApplyRGBA(processor, pixel);
  }
  else {
    float alpha, inv_alpha;

    alpha = pixel[3];
    inv_alpha = 1.0f / alpha;

    pixel[0] *= inv_alpha;
    pixel[1] *= inv_alpha;
    pixel[2] *= inv_alpha;

    processorApplyRGBA(processor, pixel);

    pixel[0] *= alpha;
    pixel[1] *= alpha;
    pixel[2] *= alpha;
  }
}

void FallbackImpl::processorRelease(OCIO_ConstProcessorRcPtr *processor)
{
  delete (FallbackProcessor *)(processor);
}

void FallbackImpl::cpuProcessorRelease(OCIO_ConstCPUProcessorRcPtr *processor)
{
  delete (FallbackProcessor *)(processor);
}

const char *FallbackImpl::colorSpaceGetName(OCIO_ConstColorSpaceRcPtr *cs)
{
  if (cs == COLORSPACE_LINEAR) {
    return "Linear";
  }
  else if (cs == COLORSPACE_SRGB) {
    return "sRGB";
  }
  return NULL;
}

const char *FallbackImpl::colorSpaceGetDescription(OCIO_ConstColorSpaceRcPtr * /*cs*/)
{
  return "";
}

const char *FallbackImpl::colorSpaceGetFamily(OCIO_ConstColorSpaceRcPtr * /*cs*/)
{
  return "";
}

OCIO_DisplayTransformRcPtr *FallbackImpl::createDisplayTransform(void)
{
  FallbackTransform *transform = new FallbackTransform();
  transform->type = TRANSFORM_LINEAR_TO_SRGB;
  return (OCIO_DisplayTransformRcPtr *)transform;
}

void FallbackImpl::displayTransformSetInputColorSpaceName(OCIO_DisplayTransformRcPtr * /*dt*/,
                                                          const char * /*name*/)
{
}

void FallbackImpl::displayTransformSetDisplay(OCIO_DisplayTransformRcPtr * /*dt*/,
                                              const char * /*name*/)
{
}

void FallbackImpl::displayTransformSetView(OCIO_DisplayTransformRcPtr * /*dt*/,
                                           const char * /*name*/)
{
}

void FallbackImpl::displayTransformSetDisplayCC(OCIO_DisplayTransformRcPtr *dt,
                                                OCIO_ConstTransformRcPtr *et)
{
  FallbackTransform *transform = (FallbackTransform *)dt;
  transform->display_transform = (FallbackTransform *)et;
}

void FallbackImpl::displayTransformSetLinearCC(OCIO_DisplayTransformRcPtr *dt,
                                               OCIO_ConstTransformRcPtr *et)
{
  FallbackTransform *transform = (FallbackTransform *)dt;
  transform->linear_transform = (FallbackTransform *)et;
}

void FallbackImpl::displayTransformSetLooksOverride(OCIO_DisplayTransformRcPtr * /*dt*/,
                                                    const char * /*looks*/)
{
}

void FallbackImpl::displayTransformSetLooksOverrideEnabled(OCIO_DisplayTransformRcPtr * /*dt*/,
                                                           bool /*enabled*/)
{
}

void FallbackImpl::displayTransformRelease(OCIO_DisplayTransformRcPtr * /*dt*/)
{
}

OCIO_PackedImageDesc *FallbackImpl::createOCIO_PackedImageDesc(float *data,
                                                               long width,
                                                               long height,
                                                               long numChannels,
                                                               long chanStrideBytes,
                                                               long xStrideBytes,
                                                               long yStrideBytes)
{
  OCIO_PackedImageDescription *desc = (OCIO_PackedImageDescription *)MEM_callocN(
      sizeof(OCIO_PackedImageDescription), "OCIO_PackedImageDescription");
  desc->data = data;
  desc->width = width;
  desc->height = height;
  desc->numChannels = numChannels;
  desc->chanStrideBytes = chanStrideBytes;
  desc->xStrideBytes = xStrideBytes;
  desc->yStrideBytes = yStrideBytes;
  return (OCIO_PackedImageDesc *)desc;
}

void FallbackImpl::OCIO_PackedImageDescRelease(OCIO_PackedImageDesc *id)
{
  MEM_freeN(id);
}

const char *FallbackImpl::getVersionString(void)
{
  return "fallback";
}

int FallbackImpl::getVersionHex(void)
{
  return 0;
}

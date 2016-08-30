/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */
#include "Precompiled.h"
#pragma hdrstop

#include "../jo_jpeg.cpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "Main.h"

namespace renderer {

// Pull into the renderer namespace.
#include "../smaa/AreaTex.h"
#include "../smaa/SearchTex.h"

std::array<Vertex *, 4> ExtractQuadCorners(Vertex *vertices, const uint16_t *indices)
{
	std::array<uint16_t, 6> sorted;
	memcpy(sorted.data(), indices, sizeof(uint16_t) * sorted.size());
	std::sort(sorted.begin(), sorted.end());
	std::array<Vertex *, 4> corners;
	size_t cornerIndex = 0;

	for (size_t i = 0; i < sorted.size(); i++)
	{
		if (i == 0 || sorted[i] != sorted[i - 1])
			corners[cornerIndex++] = &vertices[sorted[i]];
	}

	assert(cornerIndex == 4); // Should be exactly 4 unique vertices.
	return corners;
}

void WarnOnce(WarnOnceId::Enum id)
{
	static bool warned[WarnOnceId::Num];

	if (!warned[id])
	{
		interface::PrintWarningf("BGFX transient buffer alloc failed\n");
		warned[id] = true;
	}
}

void BgfxCallback::fatal(bgfx::Fatal::Enum _code, const char* _str)
{
	if (bgfx::Fatal::DebugCheck == _code)
	{
		bx::debugBreak();
	}
	else
	{
		BX_TRACE("0x%08x: %s", _code, _str);
		BX_UNUSED(_code, _str);
		abort();
	}
}

void BgfxCallback::traceVargs(const char* _filePath, uint16_t _line, const char* _format, va_list _argList)
{
	char temp[2048];
	char* out = temp;
	int32_t len   = bx::snprintf(out, sizeof(temp), "%s (%d): ", _filePath, _line);
	int32_t total = len + bx::vsnprintf(out + len, sizeof(temp)-len, _format, _argList);
	if ( (int32_t)sizeof(temp) < total)
	{
		out = (char*)alloca(total+1);
		memcpy(out, temp, len);
		bx::vsnprintf(out + len, total-len, _format, _argList);
	}
	out[total] = '\0';
	bx::debugOutput(out);
}

uint32_t BgfxCallback::cacheReadSize(uint64_t _id)
{
	return 0;
}

bool BgfxCallback::cacheRead(uint64_t _id, void* _data, uint32_t _size)
{
	return false;
}

void BgfxCallback::cacheWrite(uint64_t _id, const void* _data, uint32_t _size)
{
}

struct ImageWriteBuffer
{
	std::vector<uint8_t> *data;
	size_t bytesWritten;
};

static void ImageWriteCallback(void *context, void *data, int size)
{
	auto buffer = (ImageWriteBuffer *)context;

	if (buffer->data->size() < buffer->bytesWritten + size)
	{
		buffer->data->resize(buffer->bytesWritten + size);
	}

	memcpy(&buffer->data->data()[buffer->bytesWritten], data, size);
	buffer->bytesWritten += size;
}

static void ImageWriteCallbackConst(void *context, const void *data, int size)
{
	ImageWriteCallback(context, (void *)data, size);
}

void BgfxCallback::screenShot(const char* _filePath, uint32_t _width, uint32_t _height, uint32_t _pitch, const void* _data, uint32_t _size, bool _yflip)
{
	const int nComponents = 4;
	const bool silent = _filePath[0] == 'y';
	_filePath++;
	const char *extension = util::GetExtension(_filePath);
	const bool writeAsPng = !util::Stricmp(extension, "png");
	const uint32_t outputPitch = writeAsPng ? _pitch : _width * nComponents; // PNG can use any pitch, others can't.

	// Convert from BGRA to RGBA, and flip y if needed.
	const size_t requiredSize = outputPitch * _height;

	if (screenShotDataBuffer_.size() < requiredSize)
	{
		screenShotDataBuffer_.resize(requiredSize);
	}

	for (uint32_t y = 0; y < _height; y++)
	{
		for (uint32_t x = 0; x < _width; x++)
		{
			auto colorIn = &((const uint8_t *)_data)[x * nComponents + (_yflip ? _height - 1 - y : y) * _pitch];
			uint8_t *colorOut = &screenShotDataBuffer_[x * nComponents + y * outputPitch];
			colorOut[0] = colorIn[2];
			colorOut[1] = colorIn[1];
			colorOut[2] = colorIn[0];
			colorOut[3] = 255;

			// Apply gamma correction.
			if (g_hardwareGammaEnabled)
			{
				colorOut[0] = g_gammaTable[colorOut[0]];
				colorOut[1] = g_gammaTable[colorOut[1]];
				colorOut[2] = g_gammaTable[colorOut[2]];
			}
		}
	}

	// Write to file buffer.
	ImageWriteBuffer buffer;
	buffer.data = &screenShotFileBuffer_;
	buffer.bytesWritten = 0;

	if (writeAsPng)
	{
		if (!stbi_write_png_to_func(ImageWriteCallback, &buffer, _width, _height, nComponents, screenShotDataBuffer_.data(), (int)outputPitch))
		{
			interface::Printf("Screenshot: error writing png file\n");
			return;
		}
	}
	else if (!util::Stricmp(extension, "jpg"))
	{
		if (!jo_write_jpg_to_func(ImageWriteCallbackConst, &buffer, screenShotDataBuffer_.data(), _width, _height, nComponents, g_cvars.screenshotJpegQuality.getInt()))
		{
			interface::Printf("Screenshot: error writing jpg file\n");
			return;
		}
	}
	else
	{
		if (!stbi_write_tga_to_func(ImageWriteCallback, &buffer, _width, _height, nComponents, screenShotDataBuffer_.data()))
		{
			interface::Printf("Screenshot: error writing tga file\n");
			return;
		}
	}

	// Write file buffer to file.
	if (buffer.bytesWritten > 0)
	{
		interface::FS_WriteFile(_filePath, buffer.data->data(), buffer.bytesWritten);
	}

	if (!silent)
		interface::Printf("Wrote %s\n", _filePath);
}

void BgfxCallback::captureBegin(uint32_t _width, uint32_t _height, uint32_t _pitch, bgfx::TextureFormat::Enum _format, bool _yflip)
{
}

void BgfxCallback::captureEnd()
{
}

void BgfxCallback::captureFrame(const void* _data, uint32_t _size)
{
}

bool DrawCall::operator<(const DrawCall &other) const
{
	assert(material);
	assert(other.material);

	if (material->sort < other.material->sort)
	{
		return true;
	}
	else if (material->sort == other.material->sort)
	{
		if (sort < other.sort)
		{
			return true;
		}
		else if (sort == other.sort)
		{
			if (material->index < other.material->index)
				return true;
		}
	}

	return false;
}

#define NOISE_PERM(a) noisePerm_[(a) & (noiseSize_ - 1)]
#define NOISE_TABLE(x, y, z, t) noiseTable_[NOISE_PERM(x + NOISE_PERM(y + NOISE_PERM(z + NOISE_PERM(t))))]
#define NOISE_LERP( a, b, w ) ( ( a ) * ( 1.0f - ( w ) ) + ( b ) * ( w ) )

float Main::getNoise(float x, float y, float z, float t) const
{
	int i;
	int ix, iy, iz, it;
	float fx, fy, fz, ft;
	float front[4];
	float back[4];
	float fvalue, bvalue, value[2], finalvalue;

	ix = (int)floor(x);
	fx = x - ix;
	iy = (int)floor(y);
	fy = y - iy;
	iz = (int)floor(z);
	fz = z - iz;
	it = (int)floor(t);
	ft = t - it;

	for (i = 0; i < 2; i++)
	{
		front[0] = NOISE_TABLE(ix, iy, iz, it + i);
		front[1] = NOISE_TABLE(ix + 1, iy, iz, it + i);
		front[2] = NOISE_TABLE(ix, iy + 1, iz, it + i);
		front[3] = NOISE_TABLE(ix + 1, iy + 1, iz, it + i);

		back[0] = NOISE_TABLE(ix, iy, iz + 1, it + i);
		back[1] = NOISE_TABLE(ix + 1, iy, iz + 1, it + i);
		back[2] = NOISE_TABLE(ix, iy + 1, iz + 1, it + i);
		back[3] = NOISE_TABLE(ix + 1, iy + 1, iz + 1, it + i);

		fvalue = NOISE_LERP(NOISE_LERP(front[0], front[1], fx), NOISE_LERP(front[2], front[3], fx), fy);
		bvalue = NOISE_LERP(NOISE_LERP(back[0], back[1], fx), NOISE_LERP(back[2], back[3], fx), fy);

		value[i] = NOISE_LERP(fvalue, bvalue, fz);
	}

	finalvalue = NOISE_LERP(value[0], value[1], ft);

	return finalvalue;
}

static int Font_ReadInt(const uint8_t *data, int *offset)
{
	assert(data && offset);
	int i = data[*offset] + (data[*offset + 1] << 8) + (data[*offset + 2] << 16) + (data[*offset + 3] << 24);
	*offset += 4;
	return i;
}

static float Font_ReadFloat(const uint8_t *data, int *offset)
{
	assert(data && offset);
	uint8_t temp[4];
#if defined Q3_BIG_ENDIAN
	temp[0] = data[*offset + 3];
	temp[1] = data[*offset + 2];
	temp[2] = data[*offset + 1];
	temp[3] = data[*offset + 0];
#else
	temp[0] = data[*offset + 0];
	temp[1] = data[*offset + 1];
	temp[2] = data[*offset + 2];
	temp[3] = data[*offset + 3];
#endif
	*offset += 4;
	return *((float *)temp);
}

void Main::registerFont(const char *fontName, int pointSize, fontInfo_t *font)
{
	if (!fontName)
	{
		interface::Printf("RE_RegisterFont: called with empty name\n");
		return;
	}

	if (pointSize <= 0)
		pointSize = 12;

	if (nFonts_ >= maxFonts_)
	{
		interface::PrintWarningf("RE_RegisterFont: Too many fonts registered already.\n");
		return;
	}

	char name[1024];
	util::Sprintf(name, sizeof(name), "fonts/fontImage_%i.dat", pointSize);

	for (int i = 0; i < nFonts_; i++)
	{
		if (util::Stricmp(name, fonts_[i].name) == 0)
		{
			memcpy(font, &fonts_[i], sizeof(fontInfo_t));
			return;
		}
	}

	long len = interface::FS_ReadFile(name, NULL);

	if (len != sizeof(fontInfo_t))
		return;

	int offset = 0;
	uint8_t *data;
	interface::FS_ReadFile(name, &data);

	for (int i = 0; i < GLYPHS_PER_FONT; i++)
	{
		font->glyphs[i].height = Font_ReadInt(data, &offset);
		font->glyphs[i].top = Font_ReadInt(data, &offset);
		font->glyphs[i].bottom = Font_ReadInt(data, &offset);
		font->glyphs[i].pitch = Font_ReadInt(data, &offset);
		font->glyphs[i].xSkip = Font_ReadInt(data, &offset);
		font->glyphs[i].imageWidth = Font_ReadInt(data, &offset);
		font->glyphs[i].imageHeight = Font_ReadInt(data, &offset);
		font->glyphs[i].s = Font_ReadFloat(data, &offset);
		font->glyphs[i].t = Font_ReadFloat(data, &offset);
		font->glyphs[i].s2 = Font_ReadFloat(data, &offset);
		font->glyphs[i].t2 = Font_ReadFloat(data, &offset);
		font->glyphs[i].glyph = Font_ReadInt(data, &offset);
		util::Strncpyz(font->glyphs[i].shaderName, (const char *)&data[offset], sizeof(font->glyphs[i].shaderName));
		offset += sizeof(font->glyphs[i].shaderName);
	}

	font->glyphScale = Font_ReadFloat(data, &offset);
	util::Strncpyz(font->name, name, sizeof(font->name));

	for (int i = GLYPH_START; i <= GLYPH_END; i++)
	{
		Material *m = materialCache_->findMaterial(font->glyphs[i].shaderName, MaterialLightmapId::StretchPic, false);
		font->glyphs[i].glyph = m->defaultShader ? 0 : m->index;
	}

	memcpy(&fonts_[nFonts_++], font, sizeof(fontInfo_t));
	interface::FS_FreeReadFile(data);
}

void Main::debugPrint(const char *text)
{
	if (!g_cvars.debugText.getBool() && !light_baker::IsRunning())
		return;

	const uint16_t fontHeight = 16;
	const uint16_t maxY = window::GetHeight() / fontHeight;
	const uint16_t columnWidth = 32;
	uint16_t x = debugTextY / maxY * columnWidth;
	uint16_t y = debugTextY % maxY;
	bgfx::dbgTextPrintf(x, y, 0x4f, text);
	debugTextY++;
}

void Main::drawAxis(vec3 position)
{
	sceneDebugAxis_.push_back(position);
}

void Main::drawBounds(const Bounds &bounds)
{
	sceneDebugBounds_.push_back(bounds);
}

void Main::drawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, int materialIndex)
{
	drawStretchPicGradient(x, y, w, h, s1, t1, s2, t2, materialIndex, stretchPicColor_);
}

void Main::drawStretchPicGradient(float x, float y, float w, float h, float s1, float t1, float s2, float t2, int materialIndex, vec4 gradientColor)
{
	Material *mat = materialCache_->getMaterial(materialIndex);

	if (stretchPicMaterial_ != mat)
	{
		flushStretchPics();
		stretchPicMaterial_ = mat;
	}

	auto firstVertex = (const uint16_t)stretchPicVertices_.size();
	size_t firstIndex = stretchPicIndices_.size();
	stretchPicVertices_.resize(stretchPicVertices_.size() + 4);
	stretchPicIndices_.resize(stretchPicIndices_.size() + 6);
	Vertex *v = &stretchPicVertices_[firstVertex];
	uint16_t *i = &stretchPicIndices_[firstIndex];
	v[0].pos = vec3(x, y, 0);
	v[1].pos = vec3(x + w, y, 0);
	v[2].pos = vec3(x + w, y + h, 0);
	v[3].pos = vec3(x, y + h, 0);
	v[0].texCoord = vec2(s1, t1);
	v[1].texCoord = vec2(s2, t1);
	v[2].texCoord = vec2(s2, t2);
	v[3].texCoord = vec2(s1, t2);
	v[0].color = v[1].color = util::ToLinear(stretchPicColor_);
	v[2].color = v[3].color = util::ToLinear(gradientColor);
	i[0] = firstVertex + 3; i[1] = firstVertex + 0; i[2] = firstVertex + 2;
	i[3] = firstVertex + 2; i[4] = firstVertex + 0; i[5] = firstVertex + 1;
}

void Main::drawStretchRaw(int x, int y, int w, int h, int cols, int rows, const uint8_t *data, int client, bool dirty)
{
	if (!math::IsPowerOfTwo(cols) || !math::IsPowerOfTwo(rows))
	{
		interface::Error("Draw_StretchRaw: size not a power of 2: %i by %i", cols, rows);
	}

	bgfx::TransientVertexBuffer tvb;
	bgfx::TransientIndexBuffer tib;

	if (!bgfx::allocTransientBuffers(&tvb, Vertex::decl, 4, &tib, 6))
	{
		WarnOnce(WarnOnceId::TransientBuffer);
		return;
	}

	flushStretchPics();
	stretchPicViewId_ = UINT8_MAX;
	uploadCinematic(w, h, cols, rows, data, client, dirty);
	auto vertices = (Vertex *)tvb.data;
	vertices[0].pos = { 0, 0, 0 }; vertices[0].texCoord = { 0, 0 };
	vertices[1].pos = { 1, 0, 0 }; vertices[1].texCoord = { 1, 0 };
	vertices[2].pos = { 1, 1, 0 }; vertices[2].texCoord = { 1, 1 };
	vertices[3].pos = { 0, 1, 0 }; vertices[3].texCoord = { 0, 1 };
	auto indices = (uint16_t *)tib.data;
	indices[0] = 0; indices[1] = 1; indices[2] = 2;
	indices[3] = 2; indices[4] = 3; indices[5] = 0;
	bgfx::setVertexBuffer(&tvb);
	bgfx::setIndexBuffer(&tib);
	bgfx::setTexture(0, uniforms_->textureSampler.handle, Texture::getScratch(size_t(client))->getHandle());
	matStageUniforms_->color.set(vec4::white);
	bgfx::setState(BGFX_STATE_RGB_WRITE);
	const uint8_t viewId = pushView(defaultFb_, BGFX_CLEAR_NONE, mat4::identity, mat4::orthographicProjection(0, 1, 0, 1, -1, 1), Rect(x, y, w, h), PushViewFlags::Sequential);
	bgfx::submit(viewId, shaderPrograms_[ShaderProgramId::TextureColor].handle);
}

void Main::uploadCinematic(int w, int h, int cols, int rows, const uint8_t *data, int client, bool dirty)
{
	Texture *scratch = Texture::getScratch(size_t(client));
	
	if (cols != scratch->getWidth() || rows != scratch->getHeight())
	{
		scratch->resize(cols, rows);
		dirty = true;
	}

	if (dirty)
	{
		const bgfx::Memory *mem = bgfx::alloc(cols * rows * 4);
		memcpy(mem->data, data, mem->size);
		scratch->update(mem, 0, 0, cols, rows);
	}
}

void Main::loadWorld(const char *name)
{
	if (world::IsLoaded())
	{
		interface::Error("ERROR: attempted to redundantly load world map");
	}

	// Create frame buffers first.
	const uint32_t rtClampFlags = BGFX_TEXTURE_RT | BGFX_TEXTURE_U_CLAMP | BGFX_TEXTURE_V_CLAMP;
	linearDepthFb_.handle = bgfx::createFrameBuffer(bgfx::BackbufferRatio::Equal, bgfx::TextureFormat::R16F);
	bgfx::TextureHandle reflectionTexture;

	if (g_cvars.hdr.getBool() != 0)
	{
		if (g_cvars.waterReflections.getBool())
			reflectionTexture = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::RGBA16F, rtClampFlags);

		if (aa_ != AntiAliasing::None)
		{
			// HDR needs a temp BGRA8 destination for AA.
			sceneTempFb_.handle = bgfx::createFrameBuffer(bgfx::BackbufferRatio::Equal, bgfx::TextureFormat::BGRA8, rtClampFlags);
		}

		bgfx::TextureHandle sceneTextures[3];
		sceneTextures[0] = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::RGBA16F, rtClampFlags);
		sceneTextures[1] = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::BGRA8, rtClampFlags);
		sceneTextures[2] = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT);
		sceneFb_.handle = bgfx::createFrameBuffer(3, sceneTextures, true);
		sceneBloomAttachment_ = 1;
		sceneDepthAttachment_ = 2;

		for (size_t i = 0; i < nBloomFrameBuffers_; i++)
		{
			bloomFb_[i].handle = bgfx::createFrameBuffer(bgfx::BackbufferRatio::Quarter, bgfx::TextureFormat::BGRA8, rtClampFlags);
		}
	}
	else
	{
		uint32_t aaFlags = 0;

		if (aa_ >= AntiAliasing::MSAA2x && aa_ <= AntiAliasing::MSAA16x)
		{
			aaFlags |= (1 + (int)aa_ - (int)AntiAliasing::MSAA2x) << BGFX_TEXTURE_RT_MSAA_SHIFT;
		}

		if (g_cvars.waterReflections.getBool())
			reflectionTexture = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::BGRA8, rtClampFlags | aaFlags);

		bgfx::TextureHandle sceneTextures[2];
		sceneTextures[0] = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::BGRA8, rtClampFlags | aaFlags);
		sceneTextures[1] = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT | aaFlags);
		sceneFb_.handle = bgfx::createFrameBuffer(2, sceneTextures, true);
		sceneDepthAttachment_ = 1;
	}

	if (g_cvars.waterReflections.getBool())
		reflectionFb_.handle = bgfx::createFrameBuffer(1, &reflectionTexture); // Don't destroy the texture, that will be done by the texture cache.

	if (aa_ == AntiAliasing::SMAA)
	{
		smaaBlendFb_.handle = bgfx::createFrameBuffer(bgfx::BackbufferRatio::Equal, bgfx::TextureFormat::BGRA8, rtClampFlags);
		smaaEdgesFb_.handle = bgfx::createFrameBuffer(bgfx::BackbufferRatio::Equal, bgfx::TextureFormat::RG8, rtClampFlags);
		smaaAreaTex_ = bgfx::createTexture2D(AREATEX_WIDTH, AREATEX_HEIGHT, false, 1, bgfx::TextureFormat::RG8, BGFX_TEXTURE_U_CLAMP | BGFX_TEXTURE_V_CLAMP, bgfx::makeRef(areaTexBytes, AREATEX_SIZE));
		smaaSearchTex_ = bgfx::createTexture2D(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, false, 1, bgfx::TextureFormat::R8, BGFX_TEXTURE_U_CLAMP | BGFX_TEXTURE_V_CLAMP, bgfx::makeRef(searchTexBytes, SEARCHTEX_SIZE));
	}

	if (g_cvars.waterReflections.getBool())
	{
		// Register the reflection texture so it can accessed by materials.
		Texture::create("*reflection", reflectionTexture);
	}

	// Load the world.
	world::Load(name);
	dlightManager_->initializeGrid();
}

void Main::addDynamicLightToScene(const DynamicLight &light)
{
	dlightManager_->add(frameNo_, light);
}

void Main::addEntityToScene(const Entity &entity)
{
	sceneEntities_.push_back(entity);
}

void Main::addPolyToScene(qhandle_t hShader, int nVerts, const polyVert_t *verts, int nPolys)
{
	const size_t firstVertex = scenePolygonVertices_.size();
	scenePolygonVertices_.insert(scenePolygonVertices_.end(), verts, &verts[nPolys * nVerts]);

	for (int i = 0; i < nPolys; i++)
	{
		Polygon p;
		p.material = materialCache_->getMaterial(hShader); 
		p.firstVertex = uint32_t(firstVertex + i * nVerts);
		p.nVertices = nVerts;
		Bounds bounds;
		bounds.setupForAddingPoints();

		for (size_t j = 0; j < p.nVertices; j++)
		{
			bounds.addPoint(scenePolygonVertices_[p.firstVertex + j].xyz);
		}

		p.fogIndex = world::FindFogIndex(bounds);
		scenePolygons_.push_back(p);
	}
}

void Main::renderScene(const SceneDefinition &scene)
{
	flushStretchPics();
	stretchPicViewId_ = UINT8_MAX;
	time_ = scene.time;
	floatTime_ = time_ * 0.001f;
	
	// Clamp view rect to screen.
	Rect rect;
	rect.x = std::max(0, scene.rect.x);
	rect.y = std::max(0, scene.rect.y);
#if 0
	rect.w = std::min(window::GetWidth(), rect.x + scene.rect.w) - rect.x;
	rect.h = std::min(window::GetHeight(), rect.y + scene.rect.h) - rect.y;
#else
	rect.w = scene.rect.w;
	rect.h = scene.rect.h;
#endif

	if (scene.flags & SceneDefinitionFlags::Hyperspace)
	{
		const uint8_t c = time_ & 255;
		const uint8_t viewId = pushView(defaultFb_, 0, mat4::identity, mat4::identity, rect);
		bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, (c<<24)|(c<<16)|(c<<8)|0xff);
		bgfx::touch(viewId);
	}
	else if (scene.flags & SceneDefinitionFlags::SkyboxPortal)
	{
		// Render the skybox portal as a camera in the containing scene.
		skyboxPortalEnabled_ = true;
		skyboxPortalScene_ = scene;
	}
	else
	{
		isWorldScene_ = (scene.flags & SceneDefinitionFlags::World) && world::IsLoaded();

		// Need to do this here because Main::addEntityToScene doesn't know if this is a world scene.
		for (const Entity &entity : sceneEntities_)
		{
			meta::OnEntityAddedToScene(entity, isWorldScene_);
		}

		// Update scene dynamic lights.
		if (isWorldScene_)
		{
			dlightManager_->updateTextures(frameNo_);
		}

		// Render camera(s).
		sceneRotation_ = scene.rotation;

		if (skyboxPortalEnabled_)
		{
			renderCamera(VisibilityId::SkyboxPortal, skyboxPortalScene_.position, skyboxPortalScene_.position, skyboxPortalScene_.rotation, rect, skyboxPortalScene_.fov, skyboxPortalScene_.areaMask, Plane(), RenderCameraFlags::IsSkyboxPortal);
			skyboxPortalEnabled_ = false;
		}

		int cameraFlags = 0;

		if (scene.flags & SceneDefinitionFlags::ContainsSkyboxPortal)
			cameraFlags |= RenderCameraFlags::ContainsSkyboxPortal;

		renderCamera(VisibilityId::Main, scene.position, scene.position, sceneRotation_, rect, scene.fov, scene.areaMask, Plane(), cameraFlags);

		if (isWorldScene_)
		{
			// HDR.
			if (g_cvars.hdr.getBool())
			{
				// Bloom.
				const Rect bloomRect(0, 0, window::GetWidth() / 4, window::GetHeight() / 4);
				bgfx::setTexture(0, uniforms_->textureSampler.handle, sceneFb_.handle, sceneBloomAttachment_);
				renderScreenSpaceQuad(bloomFb_[0], ShaderProgramId::Texture, BGFX_STATE_RGB_WRITE, BGFX_CLEAR_NONE, isTextureOriginBottomLeft_, bloomRect);

				for (int i = 0; i < 2; i++)
				{
					uniforms_->guassianBlurDirection.set(i == 0 ? vec4(1, 0, 0, 0) : vec4(0, 1, 0, 0));
					bgfx::setTexture(0, uniforms_->textureSampler.handle, bloomFb_[i].handle);
					renderScreenSpaceQuad(bloomFb_[!i], ShaderProgramId::GaussianBlur, BGFX_STATE_RGB_WRITE, BGFX_CLEAR_NONE, isTextureOriginBottomLeft_, bloomRect);
				}

				// Tonemap.
				// Clamp to sane values.
				uniforms_->brightnessContrastGammaSaturation.set(vec4
				(
					Clamped(g_cvars.brightness.getFloat() - 1.0f, -0.8f, 0.8f),
					Clamped(g_cvars.contrast.getFloat(), 0.5f, 3.0f),
					Clamped(g_cvars.hdrGamma.getFloat(), 0.5f, 3.0f),
					Clamped(g_cvars.saturation.getFloat(), 0.0f, 3.0f)
				));

				uniforms_->hdr_BloomScale_Exposure.set(vec4(g_cvars.hdrBloomScale.getFloat(), g_cvars.hdrExposure.getFloat(), 0, 0));
				bgfx::setTexture(0, uniforms_->textureSampler.handle, sceneFb_.handle);
				bgfx::setTexture(1, uniforms_->bloomSampler.handle, bloomFb_[0].handle);
				renderScreenSpaceQuad(aa_ == AntiAliasing::None ? defaultFb_ : sceneTempFb_, ShaderProgramId::ToneMap, BGFX_STATE_RGB_WRITE, BGFX_CLEAR_NONE, isTextureOriginBottomLeft_);
			}

			if (aa_ == AntiAliasing::SMAA)
			{
				uniforms_->smaaMetrics.set(vec4(1.0f / rect.w, 1.0f / rect.h, (float)rect.w, (float)rect.h));

				// Edge detection.
				if (g_cvars.hdr.getBool())
				{
					bgfx::setTexture(0, uniforms_->smaaColorSampler.handle, sceneTempFb_.handle);
				}
				else
				{
					bgfx::setTexture(0, uniforms_->smaaColorSampler.handle, sceneFb_.handle);
				}

				renderScreenSpaceQuad(smaaEdgesFb_, ShaderProgramId::SMAAEdgeDetection, BGFX_STATE_RGB_WRITE, BGFX_CLEAR_COLOR, isTextureOriginBottomLeft_);

				// Blending weight calculation.
				bgfx::setTexture(0, uniforms_->smaaEdgesSampler.handle, smaaEdgesFb_.handle);
				bgfx::setTexture(1, uniforms_->smaaAreaSampler.handle, smaaAreaTex_);
				bgfx::setTexture(2, uniforms_->smaaSearchSampler.handle, smaaSearchTex_);
				renderScreenSpaceQuad(smaaBlendFb_, ShaderProgramId::SMAABlendingWeightCalculation, BGFX_STATE_RGB_WRITE | BGFX_STATE_ALPHA_WRITE, BGFX_CLEAR_COLOR, isTextureOriginBottomLeft_);

				// Neighborhood blending.
				if (g_cvars.hdr.getBool())
				{
					bgfx::setTexture(0, uniforms_->smaaColorSampler.handle, sceneTempFb_.handle);
				}
				else
				{
					bgfx::setTexture(0, uniforms_->smaaColorSampler.handle, sceneFb_.handle);
				}

				bgfx::setTexture(1, uniforms_->smaaBlendSampler.handle, smaaBlendFb_.handle);
				renderScreenSpaceQuad(defaultFb_, ShaderProgramId::SMAANeighborhoodBlending, BGFX_STATE_RGB_WRITE, BGFX_CLEAR_NONE, isTextureOriginBottomLeft_);
			}
			else
			{
				// Blit scene.
				bgfx::setTexture(0, uniforms_->textureSampler.handle, sceneFb_.handle);
				renderScreenSpaceQuad(defaultFb_, ShaderProgramId::Texture, BGFX_STATE_RGB_WRITE, BGFX_CLEAR_NONE, isTextureOriginBottomLeft_);
			}
		}
	}

	dlightManager_->clear();
	sceneDebugAxis_.clear();
	sceneDebugBounds_.clear();
	sceneEntities_.clear();
	scenePolygons_.clear();
	sortedScenePolygons_.clear();
	scenePolygonVertices_.clear();
}

void Main::endFrame()
{
	flushStretchPics();
	light_baker::Update(frameNo_);

	if (firstFreeViewId_ == 0)
	{
		// No active views. Make sure the screen is cleared.
		const uint8_t viewId = pushView(defaultFb_, 0, mat4::identity, mat4::identity, Rect(0, 0, window::GetWidth(), window::GetHeight()));
		bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR, 0x000000ff);
		bgfx::touch(viewId);
	}

	if (debugDraw_ == DebugDraw::Bloom)
	{
		debugDraw(sceneFb_, sceneBloomAttachment_);
		debugDraw(bloomFb_[0], 0, 1);
		debugDraw(bloomFb_[1], 0, 2);
	}
	else if (debugDraw_ == DebugDraw::Depth)
	{
		debugDraw(linearDepthFb_);
	}
	else if (debugDraw_ == DebugDraw::DynamicLight)
	{
		uniforms_->textureDebug.set(vec4(TEXTURE_DEBUG_SINGLE_CHANNEL, 0, 0, 0));
		debugDraw(dlightManager_->getLightsTexture(), 0, 0, ShaderProgramId::TextureDebug);
	}
	else if (debugDraw_ == DebugDraw::Lightmap && world::IsLoaded())
	{
		for (int i = 0; i < world::GetNumLightmaps(); i++)
		{
			uniforms_->textureDebug.set(vec4(TEXTURE_DEBUG_RGBM, 0, 0, 0));
			debugDraw(world::GetLightmap(i)->getHandle(), i, 0, ShaderProgramId::TextureDebug);
		}
	}
	else if (debugDraw_ == DebugDraw::Reflection)
	{
		debugDraw(reflectionFb_);
	}
	else if (debugDraw_ == DebugDraw::SMAA && aa_ == AntiAliasing::SMAA)
	{
		uniforms_->textureDebug.set(vec4(TEXTURE_DEBUG_SINGLE_CHANNEL, 0, 0, 0));
		debugDraw(smaaEdgesFb_, 0, 0, 0, ShaderProgramId::TextureDebug);
		debugDraw(smaaBlendFb_, 0, 1, 0, ShaderProgramId::TextureDebug);
	}

#ifdef USE_PROFILER
	PROFILE_END // Frame
	if (g_cvars.debugText.getBool())
		profiler::Print();
	profiler::BeginFrame(frameNo_ + 1);
	PROFILE_BEGIN(Frame)
#endif

	uint32_t debug = 0;

	if (g_cvars.bgfx_stats.getBool())
		debug |= BGFX_DEBUG_STATS;

	if (g_cvars.debugText.getBool())
		debug |= BGFX_DEBUG_TEXT;

	if (light_baker::IsRunning())
		debug |= BGFX_DEBUG_TEXT;

	bgfx::setDebug(debug);
	bgfx::frame();

	if (g_cvars.debugDraw.isModified())
	{
		debugDraw_ = DebugDrawFromString(g_cvars.debugDraw.getString());
		g_cvars.debugDraw.clearModified();
	}

	if (g_cvars.gamma.isModified())
	{
		setWindowGamma();
		g_cvars.gamma.clearModified();
	}

	if (g_cvars.debugText.getBool() || light_baker::IsRunning())
	{
		bgfx::dbgTextClear();
		debugTextY = 0;
	}

	firstFreeViewId_ = 0;
	frameNo_++;
	stretchPicViewId_ = UINT8_MAX;
}

bool Main::sampleLight(vec3 position, vec3 *ambientLight, vec3 *directedLight, vec3 *lightDir)
{
	if (!world::HasLightGrid())
		return false;

	world::SampleLightGrid(position, ambientLight, directedLight, lightDir);
	return true;
}

void Main::debugDraw(const FrameBuffer &texture, uint8_t attachment, int x, int y, ShaderProgramId::Enum program)
{
	bgfx::setTexture(0, uniforms_->textureSampler.handle, texture.handle, attachment);
	renderScreenSpaceQuad(defaultFb_, program, BGFX_STATE_RGB_WRITE, BGFX_CLEAR_NONE, isTextureOriginBottomLeft_, Rect(g_cvars.debugDrawSize.getInt() * x, g_cvars.debugDrawSize.getInt() * y, g_cvars.debugDrawSize.getInt(), g_cvars.debugDrawSize.getInt()));
}

void Main::debugDraw(bgfx::TextureHandle texture, int x, int y, ShaderProgramId::Enum program)
{
	bgfx::setTexture(0, uniforms_->textureSampler.handle, texture);
	renderScreenSpaceQuad(defaultFb_, program, BGFX_STATE_RGB_WRITE, BGFX_CLEAR_NONE, isTextureOriginBottomLeft_, Rect(g_cvars.debugDrawSize.getInt() * x, g_cvars.debugDrawSize.getInt() * y, g_cvars.debugDrawSize.getInt(), g_cvars.debugDrawSize.getInt()));
}

uint8_t Main::pushView(const FrameBuffer &frameBuffer, uint16_t clearFlags, const mat4 &viewMatrix, const mat4 &projectionMatrix, Rect rect, int flags)
{
#if 0
	if (firstFreeViewId_ == 0)
	{
		bgfx::setViewClear(firstFreeViewId_, clearFlags | BGFX_CLEAR_COLOR, 0xff00ffff);
	}
	else
#endif
	{
		bgfx::setViewClear(firstFreeViewId_, clearFlags);
	}

	bgfx::setViewFrameBuffer(firstFreeViewId_, frameBuffer.handle);
	bgfx::setViewRect(firstFreeViewId_, uint16_t(rect.x), uint16_t(rect.y), uint16_t(rect.w), uint16_t(rect.h));
	bgfx::setViewSeq(firstFreeViewId_, (flags & PushViewFlags::Sequential) != 0);
	bgfx::setViewTransform(firstFreeViewId_, viewMatrix.get(), projectionMatrix.get());
	firstFreeViewId_++;
	return firstFreeViewId_ - 1;
}

void Main::flushStretchPics()
{
	if (!stretchPicIndices_.empty())
	{
		bgfx::TransientVertexBuffer tvb;
		bgfx::TransientIndexBuffer tib;

		if (!bgfx::allocTransientBuffers(&tvb, Vertex::decl, (uint32_t)stretchPicVertices_.size(), &tib, (uint32_t)stretchPicIndices_.size()))
		{
			WarnOnce(WarnOnceId::TransientBuffer);
		}
		else
		{
			memcpy(tvb.data, &stretchPicVertices_[0], sizeof(Vertex) * stretchPicVertices_.size());
			memcpy(tib.data, &stretchPicIndices_[0], sizeof(uint16_t) * stretchPicIndices_.size());
			time_ = interface::GetTime();
			floatTime_ = time_ * 0.001f;
			uniforms_->dynamicLight_Num_Intensity.set(vec4::empty);
			matUniforms_->nDeforms.set(vec4(0, 0, 0, 0));
			matUniforms_->time.set(vec4(stretchPicMaterial_->setTime(floatTime_), 0, 0, 0));

			if (stretchPicViewId_ == UINT8_MAX)
			{
				stretchPicViewId_ = pushView(defaultFb_, BGFX_CLEAR_NONE, mat4::identity, mat4::orthographicProjection(0, (float)window::GetWidth(), 0, (float)window::GetHeight(), -1, 1), Rect(0, 0, window::GetWidth(), window::GetHeight()), PushViewFlags::Sequential);
			}

			for (const MaterialStage &stage : stretchPicMaterial_->stages)
			{
				if (!stage.active)
					continue;

				stage.setShaderUniforms(matStageUniforms_.get());
				stage.setTextureSamplers(matStageUniforms_.get());
				uint64_t state = BGFX_STATE_RGB_WRITE | BGFX_STATE_ALPHA_WRITE | stage.getState();

				// Depth testing and writing should always be off for 2D drawing.
				state &= ~BGFX_STATE_DEPTH_TEST_MASK;
				state &= ~BGFX_STATE_DEPTH_WRITE;

				bgfx::setState(state);
				bgfx::setVertexBuffer(&tvb);
				bgfx::setIndexBuffer(&tib);
				bgfx::submit(stretchPicViewId_, shaderPrograms_[ShaderProgramId::Generic].handle);
			}
		}
	}

	stretchPicVertices_.clear();
	stretchPicIndices_.clear();
}

static void SetDrawCallGeometry(const DrawCall &dc)
{
	assert(dc.vb.nVertices);
	assert(dc.ib.nIndices);

	if (dc.vb.type == DrawCall::BufferType::Static)
	{
		bgfx::setVertexBuffer(dc.vb.staticHandle, dc.vb.firstVertex, dc.vb.nVertices);
	}
	else if (dc.vb.type == DrawCall::BufferType::Dynamic)
	{
		bgfx::setVertexBuffer(dc.vb.dynamicHandle, dc.vb.firstVertex, dc.vb.nVertices);
	}
	else if (dc.vb.type == DrawCall::BufferType::Transient)
	{
		bgfx::setVertexBuffer(&dc.vb.transientHandle, dc.vb.firstVertex, dc.vb.nVertices);
	}

	if (dc.ib.type == DrawCall::BufferType::Static)
	{
		bgfx::setIndexBuffer(dc.ib.staticHandle, dc.ib.firstIndex, dc.ib.nIndices);
	}
	else if (dc.ib.type == DrawCall::BufferType::Dynamic)
	{
		bgfx::setIndexBuffer(dc.ib.dynamicHandle, dc.ib.firstIndex, dc.ib.nIndices);
	}
	else if (dc.ib.type == DrawCall::BufferType::Transient)
	{
		bgfx::setIndexBuffer(&dc.ib.transientHandle, dc.ib.firstIndex, dc.ib.nIndices);
	}
}

void Main::renderCamera(VisibilityId visId, vec3 pvsPosition, vec3 position, mat3 rotation, Rect rect, vec2 fov, const uint8_t *areaMask, Plane clippingPlane, int flags)
{
	assert(areaMask);
	const float zMin = 4;
	float zMax = 2048;
	const float polygonDepthOffset = -0.001f;
	const bool isMainCamera = visId == VisibilityId::Main;
	const uint32_t stencilTest = BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(1) | BGFX_STENCIL_FUNC_RMASK(1) | BGFX_STENCIL_OP_FAIL_S_KEEP | BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_KEEP;

	// Update world vis cache for this PVS position.
	if (isWorldScene_)
	{
		world::UpdateVisibility(visId, pvsPosition, areaMask);

		// Use dynamic z max.
		zMax = world::GetBounds(visId).calculateFarthestCornerDistance(position);
	}

	// Setup camera transform.
	const mat4 viewMatrix = toOpenGlMatrix_ * mat4::view(position, rotation);
	const mat4 projectionMatrix = mat4::perspectiveProjection(fov.x, fov.y, zMin, zMax);
	const mat4 vpMatrix(projectionMatrix * viewMatrix);
	const Frustum cameraFrustum(vpMatrix);

	if (isWorldScene_ && isMainCamera)
	{
		mainCameraTransform_.position = position;
		mainCameraTransform_.rotation = rotation;

		// Render a reflection camera if there's a reflecting surface visible.
		if (g_cvars.waterReflections.getBool())
		{
			Transform reflectionCamera;
			Plane reflectionPlane;

			if (world::CalculateReflectionCamera(visId, position, rotation, vpMatrix, &reflectionCamera, &reflectionPlane))
			{
				// Write stencil mask first.
				drawCalls_.clear();
				world::RenderReflective(visId, &drawCalls_);
				assert(!drawCalls_.empty());
				const uint8_t viewId = pushView(sceneFb_, BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, viewMatrix, projectionMatrix, rect);
				renderToStencil(viewId);

				// Render to the scene frame buffer with stencil testing.
				isCameraMirrored_ = true;
				renderCamera(VisibilityId::Reflection, pvsPosition, reflectionCamera.position, reflectionCamera.rotation, rect, fov, areaMask, reflectionPlane, flags | RenderCameraFlags::UseClippingPlane | RenderCameraFlags::UseStencilTest);
				isCameraMirrored_ = false;

				// Blit the scene frame buffer to the reflection frame buffer.
				bgfx::setTexture(0, uniforms_->textureSampler.handle, sceneFb_.handle);
				renderScreenSpaceQuad(reflectionFb_, ShaderProgramId::Texture, BGFX_STATE_RGB_WRITE, BGFX_CLEAR_NONE, isTextureOriginBottomLeft_);
			}
		}

		// Render a portal camera if there's a portal surface visible.
		vec3 pvsPosition;
		Transform portalCamera;
		Plane portalPlane;
		bool isCameraMirrored;

		if (world::CalculatePortalCamera(visId, position, rotation, vpMatrix, sceneEntities_, &pvsPosition, &portalCamera, &isCameraMirrored, &portalPlane))
		{
			// Write stencil mask first.
			drawCalls_.clear();
			world::RenderPortal(visId, &drawCalls_);
			assert(!drawCalls_.empty());
			const uint8_t viewId = pushView(sceneFb_, BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, viewMatrix, projectionMatrix, rect);
			renderToStencil(viewId);

			// Render the portal camera with stencil testing.
			isCameraMirrored_ = isCameraMirrored;
			renderCamera(VisibilityId::Portal, pvsPosition, portalCamera.position, portalCamera.rotation, rect, fov, areaMask, portalPlane, flags | RenderCameraFlags::UseClippingPlane | RenderCameraFlags::UseStencilTest);
			isCameraMirrored_ = false;
		}
	}

	// Build draw calls. Order doesn't matter.
	drawCalls_.clear();

	if (isWorldScene_)
	{
		// If dealing with skybox portals, only render the sky to the skybox portal, not the camera containing it.
		if ((flags & RenderCameraFlags::IsSkyboxPortal) || (flags & RenderCameraFlags::ContainsSkyboxPortal) == 0)
		{
			for (size_t i = 0; i < world::GetNumSkySurfaces(visId); i++)
			{
				Sky_Render(&drawCalls_, position, zMax, world::GetSkySurface(visId, i));
			}
		}

		world::Render(visId, &drawCalls_, sceneRotation_);
	}

	for (Entity &entity : sceneEntities_)
	{
		if (isMainCamera && (entity.flags & EntityFlags::ThirdPerson) != 0)
			continue;

		if (!isMainCamera && (entity.flags & EntityFlags::FirstPerson) != 0)
			continue;

		currentEntity_ = &entity;
		renderEntity(position, rotation, cameraFrustum, &entity);
		currentEntity_ = nullptr;
	}

	renderPolygons();

	if (drawCalls_.empty())
		return;

	// Sort draw calls.
	std::sort(drawCalls_.begin(), drawCalls_.end());

	// Set plane clipping.
	if (flags & RenderCameraFlags::UseClippingPlane)
	{
		uniforms_->portalClip.set(vec4(1, 0, 0, 0));
		uniforms_->portalPlane.set(clippingPlane.toVec4());
	}
	else
	{
		uniforms_->portalClip.set(vec4(0, 0, 0, 0));
	}

	// Render depth.
	if (isWorldScene_)
	{
		const uint8_t viewId = pushView(sceneFb_, BGFX_CLEAR_DEPTH, viewMatrix, projectionMatrix, rect);

		for (DrawCall &dc : drawCalls_)
		{
			// Material remapping.
			Material *mat = dc.material->remappedShader ? dc.material->remappedShader : dc.material;

			if (mat->sort != MaterialSort::Opaque || mat->numUnfoggedPasses == 0)
				continue;

			// Don't render reflective geometry with the reflection camera.
			if (visId == VisibilityId::Reflection && mat->reflective != MaterialReflective::None)
				continue;

			currentEntity_ = dc.entity;
			matUniforms_->time.set(vec4(mat->setTime(floatTime_), 0, 0, 0));
			uniforms_->depthRange.set(vec4(dc.zOffset, dc.zScale, zMin, zMax));
			mat->setDeformUniforms(matUniforms_.get());

			// See if any of the stages use alpha testing.
			const MaterialStage *alphaTestStage = nullptr;

			for (const MaterialStage &stage : mat->stages)
			{
				if (stage.active && stage.alphaTest != MaterialAlphaTest::None)
				{
					alphaTestStage = &stage;
					break;
				}
			}

			SetDrawCallGeometry(dc);
			bgfx::setTransform(dc.modelMatrix.get());
			uint64_t state = BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_DEPTH_WRITE | BGFX_STATE_MSAA;

			// Grab the cull state. Doesn't matter which stage, since it's global to the material.
			state |= mat->stages[0].getState() & BGFX_STATE_CULL_MASK;

			int shaderVariant = DepthShaderProgramVariant::None;

			if (alphaTestStage)
			{
				alphaTestStage->setShaderUniforms(matStageUniforms_.get(), MaterialStageSetUniformsFlags::TexGen);
				bgfx::setTexture(0, uniforms_->textureSampler.handle, alphaTestStage->bundles[0].textures[0]->getHandle());
				shaderVariant |= DepthShaderProgramVariant::AlphaTest;
			}
			else
			{
				matStageUniforms_->alphaTest.set(vec4::empty);
			}

			if (dc.zOffset > 0 || dc.zScale > 0)
			{
				shaderVariant |= DepthShaderProgramVariant::DepthRange;
			}

			bgfx::setState(state);

			if (flags & RenderCameraFlags::UseStencilTest)
			{
				bgfx::setStencil(stencilTest);
			}

			bgfx::submit(viewId, shaderPrograms_[ShaderProgramId::Depth + shaderVariant].handle);
			currentEntity_ = nullptr;
		}

		// Read depth, write linear depth.
		uniforms_->depthRange.set(vec4(0, 0, zMin, zMax));
		bgfx::setTexture(0, uniforms_->textureSampler.handle, sceneFb_.handle, sceneDepthAttachment_);
		renderScreenSpaceQuad(linearDepthFb_, ShaderProgramId::LinearDepth, BGFX_STATE_RGB_WRITE, BGFX_CLEAR_NONE, isTextureOriginBottomLeft_);
	}

	uint8_t mainViewId;
	
	if (isWorldScene_)
	{
		mainViewId = pushView(sceneFb_, BGFX_CLEAR_NONE, viewMatrix, projectionMatrix, rect, PushViewFlags::Sequential);
	}
	else
	{
		mainViewId = pushView(defaultFb_, BGFX_CLEAR_DEPTH, viewMatrix, projectionMatrix, rect, PushViewFlags::Sequential);
	}

	for (DrawCall &dc : drawCalls_)
	{
		assert(dc.material);

		// Material remapping.
		Material *mat = dc.material->remappedShader ? dc.material->remappedShader : dc.material;

		// Don't render reflective geometry with the reflection camera.
		if (visId == VisibilityId::Reflection && mat->reflective != MaterialReflective::None)
			continue;

		// Special case for skybox.
		if (dc.flags & DrawCallFlags::Skybox)
		{
			uniforms_->depthRange.set(vec4(dc.zOffset, dc.zScale, zMin, zMax));
			uniforms_->dynamicLight_Num_Intensity.set(vec4::empty);
			matUniforms_->nDeforms.set(vec4(0, 0, 0, 0));
			matStageUniforms_->alphaTest.set(vec4::empty);
			matStageUniforms_->baseColor.set(vec4::white);
			matStageUniforms_->generators.set(vec4::empty);
			matStageUniforms_->lightType.set(vec4::empty);
			matStageUniforms_->vertexColor.set(vec4::black);
			const int sky_texorder[6] = { 0, 2, 1, 3, 4, 5 };
			bgfx::setTexture(TextureUnit::Diffuse, matStageUniforms_->diffuseSampler.handle, mat->sky.outerbox[sky_texorder[dc.skyboxSide]]->getHandle());
#ifdef _DEBUG
			bgfx::setTexture(TextureUnit::Diffuse2, matStageUniforms_->diffuseSampler2.handle, Texture::getWhite()->getHandle());
			bgfx::setTexture(TextureUnit::Light, matStageUniforms_->lightSampler.handle, Texture::getWhite()->getHandle());
#endif
			SetDrawCallGeometry(dc);
			bgfx::setTransform(dc.modelMatrix.get());
			bgfx::setState(dc.state);

			if (flags & RenderCameraFlags::UseStencilTest)
			{
				bgfx::setStencil(stencilTest);
			}

			int shaderVariant = GenericShaderProgramVariant::DepthRange;

			if (g_cvars.hdr.getBool())
			{
				shaderVariant |= GenericShaderProgramVariant::HDR;
				uniforms_->bloomEnabled.set(vec4::empty);
			}

			bgfx::submit(mainViewId, shaderPrograms_[ShaderProgramId::Generic + shaderVariant].handle);
			continue;
		}

		const bool doFogPass = !dc.material->noFog && dc.fogIndex >= 0 && mat->fogPass != MaterialFogPass::None;

		if (mat->numUnfoggedPasses == 0 && !doFogPass)
			continue;

		currentEntity_ = dc.entity;
		matUniforms_->time.set(vec4(mat->setTime(floatTime_), 0, 0, 0));
		const mat4 modelViewMatrix(viewMatrix * dc.modelMatrix);

		if (isWorldScene_)
		{
			dlightManager_->updateUniforms(uniforms_.get());
		}
		else
		{
			// For non-world scenes, dlight contribution is added to entities in setupEntityLighting, so write 0 to the uniform for num dlights.
			uniforms_->dynamicLight_Num_Intensity.set(vec4::empty);
		}

		if (mat->polygonOffset)
		{
			uniforms_->depthRange.set(vec4(polygonDepthOffset, 1, zMin, zMax));
		}
		else
		{
			uniforms_->depthRange.set(vec4(dc.zOffset, dc.zScale, zMin, zMax));
		}

		uniforms_->viewOrigin.set(position);
		uniforms_->viewUp.set(rotation[2]);
		mat->setDeformUniforms(matUniforms_.get());
		const vec3 localViewPosition = currentEntity_ ? currentEntity_->localViewPosition : position;
		uniforms_->localViewOrigin.set(localViewPosition);

		if (currentEntity_)
		{
			entityUniforms_->ambientLight.set(vec4(currentEntity_->ambientLight / 255.0f, 0));
			entityUniforms_->directedLight.set(vec4(currentEntity_->directedLight / 255.0f, 0));
			entityUniforms_->lightDirection.set(vec4(currentEntity_->lightDir, 0));
		}

		vec4 fogColor, fogDistance, fogDepth;
		float eyeT;

		if (!dc.material->noFog && dc.fogIndex >= 0)
		{
			world::CalculateFog(dc.fogIndex, dc.modelMatrix, modelViewMatrix, position, localViewPosition, rotation, &fogColor, &fogDistance, &fogDepth, &eyeT);
			uniforms_->fogDistance.set(fogDistance);
			uniforms_->fogDepth.set(fogDepth);
			uniforms_->fogEyeT.set(eyeT);
		}

		for (const MaterialStage &stage : mat->stages)
		{
			if (!stage.active)
				continue;

			if (!dc.material->noFog && dc.fogIndex >= 0 && stage.adjustColorsForFog != MaterialAdjustColorsForFog::None)
			{
				uniforms_->fogEnabled.set(vec4(1, 0, 0, 0));
				matStageUniforms_->fogColorMask.set(stage.getFogColorMask());
			}
			else
			{
				uniforms_->fogEnabled.set(vec4::empty);
			}

			stage.setShaderUniforms(matStageUniforms_.get());
			stage.setTextureSamplers(matStageUniforms_.get());
			SetDrawCallGeometry(dc);
			bgfx::setTransform(dc.modelMatrix.get());
			uint64_t state = dc.state | stage.getState();
			int shaderVariant = GenericShaderProgramVariant::None;

			if (stage.alphaTest != MaterialAlphaTest::None)
			{
				shaderVariant |= GenericShaderProgramVariant::AlphaTest;
			}
			else if (isWorldScene_ && softSpritesEnabled_ && dc.softSpriteDepth > 0)
			{
				shaderVariant |= GenericShaderProgramVariant::SoftSprite;
				bgfx::setTexture(TextureUnit::Depth, matStageUniforms_->depthSampler.handle, linearDepthFb_.handle);
				
				// Change additive blend from (1, 1) to (src alpha, 1) so the soft sprite shader can control alpha.
				float useAlpha = 1;

				if ((state & BGFX_STATE_BLEND_MASK) == BGFX_STATE_BLEND_ADD)
				{
					useAlpha = 0; // Ignore existing alpha values in the shader. This preserves the behavior of a (1, 1) additive blend.
					state &= ~BGFX_STATE_BLEND_MASK;
					state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
				}

				uniforms_->softSprite_Depth_UseAlpha.set(vec4(dc.softSpriteDepth, useAlpha, 0, 0));
			}

			if (isWorldScene_ && dc.dynamicLighting && !(dc.flags & DrawCallFlags::Sky))
			{
				shaderVariant |= GenericShaderProgramVariant::DynamicLights;
				bgfx::setTexture(TextureUnit::DynamicLightCells, matStageUniforms_->dynamicLightCellsSampler.handle, dlightManager_->getCellsTexture());
				bgfx::setTexture(TextureUnit::DynamicLightIndices, matStageUniforms_->dynamicLightIndicesSampler.handle, dlightManager_->getIndicesTexture());
				bgfx::setTexture(TextureUnit::DynamicLights, matStageUniforms_->dynamicLightsSampler.handle, dlightManager_->getLightsTexture());
			}

			if (mat->polygonOffset || dc.zOffset > 0 || dc.zScale > 0)
			{
				shaderVariant |= GenericShaderProgramVariant::DepthRange;
			}

			if (g_cvars.hdr.getBool())
			{
				shaderVariant |= GenericShaderProgramVariant::HDR;
				uniforms_->bloomEnabled.set(vec4(stage.bloom ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f));
			}

			bgfx::setState(state);

			if (flags & RenderCameraFlags::UseStencilTest)
			{
				bgfx::setStencil(stencilTest);
			}

			bgfx::submit(mainViewId, shaderPrograms_[ShaderProgramId::Generic + shaderVariant].handle);
		}

		if (g_cvars.wireframe.getBool())
		{
			// Doesn't handle vertex deforms.
			matStageUniforms_->color.set(vec4::white);
			SetDrawCallGeometry(dc);
			bgfx::setState(dc.state | BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_PT_LINES);
			bgfx::setTexture(0, uniforms_->textureSampler.handle, Texture::getWhite()->getHandle());
			bgfx::setTransform(dc.modelMatrix.get());
			bgfx::submit(mainViewId, shaderPrograms_[ShaderProgramId::TextureColor].handle);
		}

		// Do fog pass.
		if (doFogPass)
		{
			matStageUniforms_->color.set(fogColor);
			SetDrawCallGeometry(dc);
			bgfx::setTransform(dc.modelMatrix.get());
			uint64_t state = dc.state | BGFX_STATE_BLEND_ALPHA;

			if (mat->fogPass == MaterialFogPass::Equal)
			{
				state |= BGFX_STATE_DEPTH_TEST_EQUAL;
			}
			else
			{
				state |= BGFX_STATE_DEPTH_TEST_LEQUAL;
			}

			bgfx::setState(state);

			if (flags & RenderCameraFlags::UseStencilTest)
			{
				bgfx::setStencil(stencilTest);
			}

			int shaderVariant = FogShaderProgramVariant::None;

			if (dc.zOffset > 0 || dc.zScale > 0)
			{
				shaderVariant |= FogShaderProgramVariant::DepthRange;
			}

			if (g_cvars.hdr.getBool())
			{
				shaderVariant |= FogShaderProgramVariant::HDR;
			}

			bgfx::submit(mainViewId, shaderPrograms_[ShaderProgramId::Fog + shaderVariant].handle);
		}

		currentEntity_ = nullptr;
	}

	// Draws x/y/z lines from the origin for orientation debugging
	if (!sceneDebugAxis_.empty())
	{
		bgfx::TransientVertexBuffer tvb;
		bgfx::allocTransientVertexBuffer(&tvb, 6, Vertex::decl);
		auto vertices = (Vertex *)tvb.data;
		const float l = 16;
		vertices[0].pos = { 0, 0, 0 }; vertices[0].color = vec4::red;
		vertices[1].pos = { l, 0, 0 }; vertices[1].color = vec4::red;
		vertices[2].pos = { 0, 0, 0 }; vertices[2].color = vec4::green;
		vertices[3].pos = { 0, l, 0 }; vertices[3].color = vec4::green;
		vertices[4].pos = { 0, 0, 0 }; vertices[4].color = vec4::blue;
		vertices[5].pos = { 0, 0, l }; vertices[5].color = vec4::blue;

		for (vec3 pos : sceneDebugAxis_)
		{
			bgfx::setState(BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES | BGFX_STATE_RGB_WRITE);
			bgfx::setTransform(mat4::translate(pos).get());
			bgfx::setVertexBuffer(&tvb);
			bgfx::submit(mainViewId, shaderPrograms_[ShaderProgramId::Color].handle);
		}
	}

	// Debug draw bounds.
	if (!sceneDebugBounds_.empty())
	{
		const uint32_t nVertices = 24;
		const vec4 randomColors[] =
		{
			{ 1, 0, 0, 1 },
			{ 0, 1, 0, 1 },
			{ 0, 0, 1, 1 },
			{ 1, 1, 0, 1 },
			{ 0, 1, 1, 1 },
			{ 1, 0, 1, 1 }
		};

		bgfx::TransientVertexBuffer tvb;
		bgfx::allocTransientVertexBuffer(&tvb, nVertices * (uint32_t)sceneDebugBounds_.size(), Vertex::decl);
		auto v = (Vertex *)tvb.data;

		for (size_t i = 0; i < sceneDebugBounds_.size(); i++)
		{
			const std::array<vec3, 8> corners = sceneDebugBounds_[i].toVertices();

			for (int j = 0; j < nVertices; j++)
				v[j].color = randomColors[i % BX_COUNTOF(randomColors)];

			// Top.
			v[0].pos = corners[0]; v[1].pos = corners[1];
			v[2].pos = corners[1]; v[3].pos = corners[2];
			v[4].pos = corners[2]; v[5].pos = corners[3];
			v[6].pos = corners[3]; v[7].pos = corners[0];
			v += 8;

			// Bottom.
			v[0].pos = corners[4]; v[1].pos = corners[5];
			v[2].pos = corners[5]; v[3].pos = corners[6];
			v[4].pos = corners[6]; v[5].pos = corners[7];
			v[6].pos = corners[7]; v[7].pos = corners[4];
			v += 8;

			// Connect bottom and top.
			v[0].pos = corners[0]; v[1].pos = corners[4];
			v[2].pos = corners[1]; v[3].pos = corners[7];
			v[4].pos = corners[2]; v[5].pos = corners[6];
			v[6].pos = corners[3]; v[7].pos = corners[5];
			v += 8;
		}

		bgfx::setState(BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES | BGFX_STATE_RGB_WRITE);
		bgfx::setVertexBuffer(&tvb);
		bgfx::submit(mainViewId, shaderPrograms_[ShaderProgramId::Color].handle);
	}
}

void Main::renderPolygons()
{
	if (scenePolygons_.empty())
		return;

	// Sort polygons by material and fogIndex for batching.
	for (Polygon &polygon : scenePolygons_)
	{
		sortedScenePolygons_.push_back(&polygon);
	}

	std::sort(sortedScenePolygons_.begin(), sortedScenePolygons_.end(), [](Polygon *a, Polygon *b)
	{
		if (a->material->index < b->material->index)
			return true;
		else if (a->material->index == b->material->index)
		{
			return a->fogIndex < b->fogIndex;
		}

		return false;
	});

	size_t batchStart = 0;

	for (;;)
	{
		uint32_t nVertices = 0, nIndices = 0;
		size_t batchEnd;

		// Find the last polygon index that matches the current material and fog. Count geo as we go.
		for (batchEnd = batchStart; batchEnd < sortedScenePolygons_.size(); batchEnd++)
		{
			const Polygon *p = sortedScenePolygons_[batchEnd];

			if (p->material != sortedScenePolygons_[batchStart]->material || p->fogIndex != sortedScenePolygons_[batchStart]->fogIndex)
				break;

			nVertices += p->nVertices;
			nIndices += (p->nVertices - 2) * 3;
		}

		batchEnd = std::max(batchStart, batchEnd - 1);

		// Got a range of polygons to batch. Build a draw call.
		bgfx::TransientVertexBuffer tvb;
		bgfx::TransientIndexBuffer tib;

		if (!bgfx::allocTransientBuffers(&tvb, Vertex::decl, nVertices, &tib, nIndices))
		{
			WarnOnce(WarnOnceId::TransientBuffer);
			break;
		}

		auto vertices = (Vertex *)tvb.data;
		auto indices = (uint16_t *)tib.data;
		uint32_t currentVertex = 0, currentIndex = 0;

		for (size_t i = batchStart; i <= batchEnd; i++)
		{
			const Polygon *p = sortedScenePolygons_[i];
			const uint32_t firstVertex = currentVertex;

			for (size_t j = 0; j < p->nVertices; j++)
			{
				Vertex &v = vertices[currentVertex++];
				const polyVert_t &pv = scenePolygonVertices_[p->firstVertex + j];
				v.pos = pv.xyz;
				v.texCoord = pv.st;
				v.color = vec4::fromBytes(pv.modulate);
			}

			for (size_t j = 0; j < p->nVertices - 2; j++)
			{
				indices[currentIndex++] = firstVertex + 0;
				indices[currentIndex++] = firstVertex + uint16_t(j) + 1;
				indices[currentIndex++] = firstVertex + uint16_t(j) + 2;
			}
		}

		DrawCall dc;
		dc.dynamicLighting = false; // No dynamic lighting on decals.
		dc.fogIndex = sortedScenePolygons_[batchStart]->fogIndex;
		dc.material = sortedScenePolygons_[batchStart]->material;
		dc.vb.type = dc.ib.type = DrawCall::BufferType::Transient;
		dc.vb.transientHandle = tvb;
		dc.vb.nVertices = nVertices;
		dc.ib.transientHandle = tib;
		dc.ib.nIndices = nIndices;
		drawCalls_.push_back(dc);

		// Iterate.
		batchStart = batchEnd + 1;

		if (batchStart >= sortedScenePolygons_.size())
			break;
	}
}

void Main::renderToStencil(const uint8_t viewId)
{
	const uint32_t stencilWrite = BGFX_STENCIL_TEST_ALWAYS | BGFX_STENCIL_FUNC_REF(1) | BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_REPLACE | BGFX_STENCIL_OP_FAIL_Z_REPLACE | BGFX_STENCIL_OP_PASS_Z_REPLACE;
	currentEntity_ = nullptr;

	for (DrawCall &dc : drawCalls_)
	{
		Material *mat = dc.material->remappedShader ? dc.material->remappedShader : dc.material;
		uniforms_->depthRange.set(vec4::empty);
		matUniforms_->time.set(vec4(mat->setTime(floatTime_), 0, 0, 0));
		mat->setDeformUniforms(matUniforms_.get());
		matStageUniforms_->alphaTest.set(vec4::empty);
		SetDrawCallGeometry(dc);
		bgfx::setTransform(dc.modelMatrix.get());
		uint64_t state = BGFX_STATE_RGB_WRITE | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_DEPTH_WRITE | BGFX_STATE_MSAA;

		// Grab the cull state. Doesn't matter which stage, since it's global to the material.
		state |= mat->stages[0].getState() & BGFX_STATE_CULL_MASK;

		bgfx::setState(state);
		bgfx::setStencil(stencilWrite);
		bgfx::submit(viewId, shaderPrograms_[ShaderProgramId::Depth].handle);
	}
}

// From bgfx screenSpaceQuad.
void Main::renderScreenSpaceQuad(const FrameBuffer &frameBuffer, ShaderProgramId::Enum program, uint64_t state, uint16_t clearFlags, bool originBottomLeft, Rect rect)
{
	if (!bgfx::checkAvailTransientVertexBuffer(3, Vertex::decl))
	{
		WarnOnce(WarnOnceId::TransientBuffer);
		return;
	}

	if (!rect.w) rect.w = window::GetWidth();
	if (!rect.h) rect.h = window::GetHeight();
	const float width = 1.0f;
	const float height = 1.0f;
	const float zz = 0.0f;
	const float minx = -width;
	const float maxx =  width;
	const float miny = 0.0f;
	const float maxy = height*2.0f;
	const float texelHalfW = halfTexelOffset_ / rect.w;
	const float texelHalfH = halfTexelOffset_ / rect.h;
	const float minu = -1.0f + texelHalfW;
	const float maxu =  1.0f + texelHalfW;
	float minv = texelHalfH;
	float maxv = 2.0f + texelHalfH;

	if (originBottomLeft)
	{
		float temp = minv;
		minv = maxv;
		maxv = temp;
		minv -= 1.0f;
		maxv -= 1.0f;
	}

	bgfx::TransientVertexBuffer vb;
	bgfx::allocTransientVertexBuffer(&vb, 3, Vertex::decl);
	auto vertices = (Vertex *)vb.data;
	vertices[0].pos = vec3(minx, miny, zz);
	vertices[0].color = vec4::white;
	vertices[0].texCoord = vec2(minu, minv);
	vertices[1].pos = vec3(maxx, miny, zz);
	vertices[1].color = vec4::white;
	vertices[1].texCoord = vec2(maxu, minv);
	vertices[2].pos = vec3(maxx, maxy, zz);
	vertices[2].color = vec4::white;
	vertices[2].texCoord = vec2(maxu, maxv);
	bgfx::setVertexBuffer(&vb);
	bgfx::setState(state);
	const uint8_t viewId = pushView(frameBuffer, clearFlags, mat4::identity, mat4::orthographicProjection(0, 1, 0, 1, -1, 1), rect);
	bgfx::submit(viewId, shaderPrograms_[program].handle);
}

// From bgfx HDR example setOffsets2x2Lum
void Main::setTexelOffsetsDownsample2x2(int width, int height)
{
	const float du = 1.0f / width;
	const float dv = 1.0f / height;
	vec4 offsets[16];
	uint32_t num = 0;

	for (uint32_t yy = 0; yy < 3; ++yy)
	{
		for (uint32_t xx = 0; xx < 3; ++xx)
		{
			offsets[num][0] = (xx - halfTexelOffset_) * du;
			offsets[num][1] = (yy - halfTexelOffset_) * dv;
			++num;
		}
	}

	uniforms_->texelOffsets.set(offsets, num);
}

// From bgfx HDR example setOffsets4x4Lum
void Main::setTexelOffsetsDownsample4x4(int width, int height)
{
	const float du = 1.0f / width;
	const float dv = 1.0f / height;
	vec4 offsets[16];
	uint32_t num = 0;

	for (uint32_t yy = 0; yy < 4; ++yy)
	{
		for (uint32_t xx = 0; xx < 4; ++xx)
		{
			offsets[num][0] = (xx - 1.0f - halfTexelOffset_) * du;
			offsets[num][1] = (yy - 1.0f - halfTexelOffset_) * dv;
			++num;
		}
	}

	uniforms_->texelOffsets.set(offsets, num);
}

void Main::setWindowGamma()
{
	if (!g_hardwareGammaEnabled)
		return;
		
	const float gamma = math::Clamped(g_cvars.gamma.getFloat(), 0.5f, 3.0f);

	for (size_t i = 0; i < g_gammaTableSize; i++)
	{
		int value = int(i);

		if (gamma != 1.0f)
		{
			value = int(255 * pow(i / 255.0f, 1.0f / gamma) + 0.5f);
		}

		g_gammaTable[i] = math::Clamped(value, 0, 255);
	}

	window::SetGamma(g_gammaTable, g_gammaTable, g_gammaTable);
}

void Main::renderEntity(vec3 viewPosition, mat3 viewRotation, Frustum cameraFrustum, Entity *entity)
{
	assert(entity);

	// Calculate the viewer origin in the model's space.
	// Needed for fog, specular, and environment mapping.
	const vec3 delta = viewPosition - entity->position;

	// Compensate for scale in the axes if necessary.
	float axisLength = 1;

	if (entity->nonNormalizedAxes)
	{
		axisLength = 1.0f / entity->rotation[0].length();
	}

	entity->localViewPosition =
	{
		vec3::dotProduct(delta, entity->rotation[0]) * axisLength,
		vec3::dotProduct(delta, entity->rotation[1]) * axisLength,
		vec3::dotProduct(delta, entity->rotation[2]) * axisLength
	};

	switch (entity->type)
	{
	case EntityType::Beam:
		break;

	case EntityType::Lightning:
		renderLightningEntity(viewPosition, viewRotation, entity);
		break;

	case EntityType::Model:
		if (entity->handle == 0)
		{
			sceneDebugAxis_.push_back(entity->position);
		}
		else
		{
			Model *model = modelCache_->getModel(entity->handle);

			if (model->isCulled(entity, cameraFrustum))
				break;

			setupEntityLighting(entity);
			model->render(sceneRotation_, &drawCalls_, entity);
		}
		break;
	
	case EntityType::RailCore:
		renderRailCoreEntity(viewPosition, viewRotation, entity);
		break;

	case EntityType::RailRings:
		renderRailRingsEntity(entity);
		break;

	case EntityType::Sprite:
		if (cameraFrustum.clipSphere(entity->position, entity->radius) == Frustum::ClipResult::Outside)
			break;

		renderSpriteEntity(viewRotation, entity);
		break;

	default:
		break;
	}
}

void Main::renderLightningEntity(vec3 viewPosition, mat3 viewRotation, Entity *entity)
{
	const vec3 start(entity->position), end(entity->oldPosition);
	vec3 dir = (end - start);
	const float length = dir.normalize();

	// Compute side vector.
	const vec3 v1 = (start - viewPosition).normal();
	const vec3 v2 = (end - viewPosition).normal();
	vec3 right = vec3::crossProduct(v1, v2).normal();

	for (int i = 0; i < 4; i++)
	{
		renderRailCore(start, end, right, length, g_cvars.railCoreWidth.getFloat(), materialCache_->getMaterial(entity->customMaterial), entity->materialColor, entity);
		right = right.rotatedAroundDirection(dir, 45);
	}
}

void Main::renderRailCoreEntity(vec3 viewPosition, mat3 viewRotation, Entity *entity)
{
	const vec3 start(entity->oldPosition), end(entity->position);
	vec3 dir = (end - start);
	const float length = dir.normalize();

	// Compute side vector.
	const vec3 v1 = (start - viewPosition).normal();
	const vec3 v2 = (end - viewPosition).normal();
	const vec3 right = vec3::crossProduct(v1, v2).normal();

	renderRailCore(start, end, right, length, g_cvars.railCoreWidth.getFloat(), materialCache_->getMaterial(entity->customMaterial), entity->materialColor, entity);
}

void Main::renderRailCore(vec3 start, vec3 end, vec3 up, float length, float spanWidth, Material *mat, vec4 color, Entity *entity)
{
	const uint32_t nVertices = 4, nIndices = 6;
	bgfx::TransientVertexBuffer tvb;
	bgfx::TransientIndexBuffer tib;

	if (!bgfx::allocTransientBuffers(&tvb, Vertex::decl, nVertices, &tib, nIndices)) 
	{
		WarnOnce(WarnOnceId::TransientBuffer);
		return;
	}

	auto vertices = (Vertex *)tvb.data;
	vertices[0].pos = start + up * spanWidth;
	vertices[1].pos = start + up * -spanWidth;
	vertices[2].pos = end + up * spanWidth;
	vertices[3].pos = end + up * -spanWidth;

	const float t = length / 256.0f;
	vertices[0].texCoord = vec2(0, 0);
	vertices[1].texCoord = vec2(0, 1);
	vertices[2].texCoord = vec2(t, 0);
	vertices[3].texCoord = vec2(t, 1);

	vertices[0].color = util::ToLinear(vec4(color.xyz() * 0.25f, 1));
	vertices[1].color = vertices[2].color = vertices[3].color = util::ToLinear(color);

	auto indices = (uint16_t *)tib.data;
	indices[0] = 0; indices[1] = 1; indices[2] = 2;
	indices[3] = 2; indices[4] = 1; indices[5] = 3;

	DrawCall dc;
	dc.dynamicLighting = false;
	dc.entity = entity;
	dc.fogIndex = isWorldScene_ ? world::FindFogIndex(entity->position, entity->radius) : -1;
	dc.material = mat;
	dc.vb.type = dc.ib.type = DrawCall::BufferType::Transient;
	dc.vb.transientHandle = tvb;
	dc.vb.nVertices = nVertices;
	dc.ib.transientHandle = tib;
	dc.ib.nIndices = nIndices;
	drawCalls_.push_back(dc);
}

void Main::renderRailRingsEntity(Entity *entity)
{
	const vec3 start(entity->oldPosition), end(entity->position);
	vec3 dir = (end - start);
	const float length = dir.normalize();
	vec3 right, up;
	dir.toNormalVectors(&right, &up);
	dir *= g_cvars.railSegmentLength.getFloat();
	int nSegments = (int)std::max(1.0f, length / g_cvars.railSegmentLength.getFloat());

	if (nSegments > 1)
		nSegments--;

	if (!nSegments)
		return;

	const float scale = 0.25f;
	const float spanWidth = g_cvars.railWidth.getFloat();
	vec3 positions[4];

	for (int i = 0; i < 4; i++)
	{
		const float c = cos(DEG2RAD(45 + i * 90));
		const float s = sin(DEG2RAD(45 + i * 90));
		positions[i] = start + (right * c + up * s) * scale * spanWidth;

		if (nSegments)
		{
			// Offset by 1 segment if we're doing a long distance shot.
			positions[i] += dir;
		}
	}

	const uint32_t nVertices = 4 * nSegments, nIndices = 6 * nSegments;
	bgfx::TransientVertexBuffer tvb;
	bgfx::TransientIndexBuffer tib;

	if (!bgfx::allocTransientBuffers(&tvb, Vertex::decl, nVertices, &tib, nIndices)) 
	{
		WarnOnce(WarnOnceId::TransientBuffer);
		return;
	}

	for (int i = 0; i < nSegments; i++)
	{
		for (int j = 0; j < 4; j++ )
		{
			auto vertex = &((Vertex *)tvb.data)[i * 4 + j];
			vertex->pos = positions[j];
			vertex->texCoord[0] = j < 2;
			vertex->texCoord[1] = j && j != 3;
			vertex->color = entity->materialColor;
			positions[j] += dir;
		}

		auto index = &((uint16_t *)tib.data)[i * 6];
		const uint16_t offset = i * 4;
		index[0] = offset + 0; index[1] = offset + 1; index[2] = offset + 3;
		index[3] = offset + 3; index[4] = offset + 1; index[5] = offset + 2;
	}

	DrawCall dc;
	dc.dynamicLighting = false;
	dc.entity = entity;
	dc.fogIndex = isWorldScene_ ? world::FindFogIndex(entity->position, entity->radius) : -1;
	dc.material = materialCache_->getMaterial(entity->customMaterial);
	dc.vb.type = dc.ib.type = DrawCall::BufferType::Transient;
	dc.vb.transientHandle = tvb;
	dc.vb.nVertices = nVertices;
	dc.ib.transientHandle = tib;
	dc.ib.nIndices = nIndices;
	drawCalls_.push_back(dc);
}

void Main::renderSpriteEntity(mat3 viewRotation, Entity *entity)
{
	// Calculate the positions for the four corners.
	vec3 left, up;

	if (entity->angle == 0)
	{
		left = viewRotation[1] * entity->radius;
		up = viewRotation[2] * entity->radius;
	}
	else
	{
		const float ang = (float)M_PI * entity->angle / 180.0f;
		const float s = sin(ang);
		const float c = cos(ang);
		left = viewRotation[1] * (c * entity->radius) + viewRotation[2] * (-s * entity->radius);
		up = viewRotation[2] * (c * entity->radius) + viewRotation[1] * (s * entity->radius);
	}

	if (isCameraMirrored_)
		left = -left;

	const uint32_t nVertices = 4, nIndices = 6;
	bgfx::TransientVertexBuffer tvb;
	bgfx::TransientIndexBuffer tib;

	if (!bgfx::allocTransientBuffers(&tvb, Vertex::decl, nVertices, &tib, nIndices)) 
	{
		WarnOnce(WarnOnceId::TransientBuffer);
		return;
	}

	auto vertices = (Vertex *)tvb.data;
	vertices[0].pos = entity->position + left + up;
	vertices[1].pos = entity->position - left + up;
	vertices[2].pos = entity->position - left - up;
	vertices[3].pos = entity->position + left - up;

	// Constant normal all the way around.
	vertices[0].normal = vertices[1].normal = vertices[2].normal = vertices[3].normal = -viewRotation[0];

	// Standard square texture coordinates.
	vertices[0].texCoord = vertices[0].texCoord2 = vec2(0, 0);
	vertices[1].texCoord = vertices[1].texCoord2 = vec2(1, 0);
	vertices[2].texCoord = vertices[2].texCoord2 = vec2(1, 1);
	vertices[3].texCoord = vertices[3].texCoord2 = vec2(0, 1);

	// Constant color all the way around.
	vertices[0].color = vertices[1].color = vertices[2].color = vertices[3].color = util::ToLinear(entity->materialColor);

	auto indices = (uint16_t *)tib.data;
	indices[0] = 0; indices[1] = 1; indices[2] = 3;
	indices[3] = 3; indices[4] = 1; indices[5] = 2;

	DrawCall dc;
	dc.dynamicLighting = false;
	dc.entity = entity;
	dc.fogIndex = isWorldScene_ ? world::FindFogIndex(entity->position, entity->radius) : -1;
	dc.material = materialCache_->getMaterial(entity->customMaterial);
	dc.softSpriteDepth = entity->radius / 2.0f;
	dc.vb.type = dc.ib.type = DrawCall::BufferType::Transient;
	dc.vb.transientHandle = tvb;
	dc.vb.nVertices = nVertices;
	dc.ib.transientHandle = tib;
	dc.ib.nIndices = nIndices;
	drawCalls_.push_back(dc);
}

void Main::setupEntityLighting(Entity *entity)
{
	assert(entity);

	// Trace a sample point down to find ambient light.
	vec3 lightPosition;
	
	if (entity->flags & EntityFlags::LightingPosition)
	{
		// Seperate lightOrigins are needed so an object that is sinking into the ground can still be lit, and so multi-part models can be lit identically.
		lightPosition = entity->lightingPosition;
	}
	else
	{
		lightPosition = entity->position;
	}

	// If not a world scene, only use dynamic lights (menu system, etc.)
	if (isWorldScene_ && world::HasLightGrid())
	{
		world::SampleLightGrid(lightPosition, &entity->ambientLight, &entity->directedLight, &entity->lightDir);
	}
	else
	{
		entity->ambientLight = vec3(g_identityLight * 150);
		entity->directedLight = vec3(g_identityLight * 150);
		entity->lightDir = sunLight_.direction;
	}

	// Bonus items and view weapons have a fixed minimum add.
	//if (entity->e.renderfx & RF_MINLIGHT)
	{
		// Give everything a minimum light add.
		entity->ambientLight += vec3(g_identityLight * 32);
	}

	// Clamp ambient.
	for (int i = 0; i < 3; i++)
		entity->ambientLight[i] = std::min(entity->ambientLight[i], g_identityLight * 255);

	// Modify the light by dynamic lights.
	if (!isWorldScene_)
	{
		dlightManager_->contribute(frameNo_, lightPosition, &entity->directedLight, &entity->lightDir);
	}

	entity->lightDir.normalize();
}

DebugDraw DebugDrawFromString(const char *s)
{
	if (util::Stricmp(s, "bloom") == 0)
		return DebugDraw::Bloom;
	else if (util::Stricmp(s, "depth") == 0)
		return DebugDraw::Depth;
	else if (util::Stricmp(s, "dlight") == 0)
		return DebugDraw::DynamicLight;
	else if (util::Stricmp(s, "lightmap") == 0)
		return DebugDraw::Lightmap;
	else if (util::Stricmp(s, "reflection") == 0)
		return DebugDraw::Reflection;
	else if (util::Stricmp(s, "smaa") == 0)
		return DebugDraw::SMAA;

	return DebugDraw::None;
}

} // namespace renderer

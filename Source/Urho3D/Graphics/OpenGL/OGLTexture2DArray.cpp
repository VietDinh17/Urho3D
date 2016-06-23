//
// Copyright (c) 2008-2016 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../../Precompiled.h"

#include "../../Core/Context.h"
#include "../../Core/Profiler.h"
#include "../../Graphics/Graphics.h"
#include "../../Graphics/GraphicsEvents.h"
#include "../../Graphics/GraphicsImpl.h"
#include "../../Graphics/Renderer.h"
#include "../../Graphics/Texture2DArray.h"
#include "../../IO/FileSystem.h"
#include "../../IO/Log.h"
#include "../../Resource/ResourceCache.h"
#include "../../Resource/XMLFile.h"

#include "../../DebugNew.h"

#ifdef _MSC_VER
#pragma warning(disable:4355)
#endif

namespace Urho3D
{

Texture2DArray::Texture2DArray(Context* context) :
    Texture(context),
    layers_(0)
{
#ifndef GL_ES_VERSION_2_0
    target_ = GL_TEXTURE_2D_ARRAY;
#else
    target_ = 0;
#endif
}

Texture2DArray::~Texture2DArray()
{
    Release();
}

void Texture2DArray::RegisterObject(Context* context)
{
    context->RegisterFactory<Texture2DArray>();
}

bool Texture2DArray::BeginLoad(Deserializer& source)
{
    ResourceCache* cache = GetSubsystem<ResourceCache>();

    // In headless mode, do not actually load the texture, just return success
    if (!graphics_)
        return true;

    // If device is lost, retry later
    if (graphics_->IsDeviceLost())
    {
        URHO3D_LOGWARNING("Texture load while device is lost");
        dataPending_ = true;
        return true;
    }

    cache->ResetDependencies(this);

    String texPath, texName, texExt;
    SplitPath(GetName(), texPath, texName, texExt);

    loadParameters_ = (new XMLFile(context_));
    if (!loadParameters_->Load(source))
    {
        loadParameters_.Reset();
        return false;
    }

    loadImages_.Clear();

    XMLElement textureElem = loadParameters_->GetRoot();
    XMLElement layerElem = textureElem.GetChild("layer");
    while (layerElem)
    {
        String name = layerElem.GetAttribute("name");

        // If path is empty, add the XML file path
        if (GetPath(name).Empty())
            name = texPath + name;

        loadImages_.Push(cache->GetTempResource<Image>(name));
        cache->StoreResourceDependency(this, name);

        layerElem = layerElem.GetNext("layer");
    }

    // Precalculate mip levels if async loading
    if (GetAsyncLoadState() == ASYNC_LOADING)
    {
        for (unsigned i = 0; i < loadImages_.Size(); ++i)
        {
            if (loadImages_[i])
                loadImages_[i]->PrecalculateLevels();
        }
    }

    return true;
}

bool Texture2DArray::EndLoad()
{
    // In headless mode, do not actually load the texture, just return success
    if (!graphics_ || graphics_->IsDeviceLost())
        return true;

    // If over the texture budget, see if materials can be freed to allow textures to be freed
    CheckTextureBudget(GetTypeStatic());

    SetParameters(loadParameters_);
    SetLayers(loadImages_.Size());

    for (unsigned i = 0; i < loadImages_.Size(); ++i)
        SetData(i, loadImages_[i]);

    loadImages_.Clear();
    loadParameters_.Reset();

    return true;
}

void Texture2DArray::OnDeviceLost()
{
    GPUObject::OnDeviceLost();

    if (renderSurface_)
        renderSurface_->OnDeviceLost();
}

void Texture2DArray::OnDeviceReset()
{
    if (!object_.name_ || dataPending_)
    {
        // If has a resource file, reload through the resource cache. Otherwise just recreate.
        ResourceCache* cache = GetSubsystem<ResourceCache>();
        if (cache->Exists(GetName()))
            dataLost_ = !cache->ReloadResource(this);

        if (!object_.name_)
        {
            Create();
            dataLost_ = true;
        }
    }

    dataPending_ = false;
}

void Texture2DArray::Release()
{
    if (object_.name_)
    {
        if (!graphics_)
            return;

        if (!graphics_->IsDeviceLost())
        {
            for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
            {
                if (graphics_->GetTexture(i) == this)
                    graphics_->SetTexture(i, 0);
            }

            glDeleteTextures(1, &object_.name_);
        }

        if (renderSurface_)
            renderSurface_->Release();

        object_.name_ = 0;
    }
}

void Texture2DArray::SetLayers(unsigned layers)
{
    Release();

    layers_ = layers;
}

bool Texture2DArray::SetSize(unsigned layers, int width, int height, unsigned format, TextureUsage usage)
{
    if (width <= 0 || height <= 0)
    {
        URHO3D_LOGERROR("Zero or negative texture array size");
        return false;
    }
    if (usage == TEXTURE_DEPTHSTENCIL)
    {
        URHO3D_LOGERROR("Depth-stencil usage not supported for texture arrays");
        return false;
    }

    // Delete the old rendersurface if any
    renderSurface_.Reset();

    usage_ = usage;

    if (usage == TEXTURE_RENDERTARGET)
    {
        renderSurface_ = new RenderSurface(this);

        // Nearest filtering and mipmaps disabled by default
        filterMode_ = FILTER_NEAREST;
        requestedLevels_ = 1;
    }

    if (usage == TEXTURE_RENDERTARGET)
        SubscribeToEvent(E_RENDERSURFACEUPDATE, URHO3D_HANDLER(Texture2DArray, HandleRenderSurfaceUpdate));
    else
        UnsubscribeFromEvent(E_RENDERSURFACEUPDATE);

    width_ = width;
    height_ = height;
    format_ = format;
    if (layers)
        layers_ = layers;

    layerMemoryUse_.Resize(layers_);
    for (unsigned i = 0; i < layers_; ++i)
        layerMemoryUse_[i] = 0;

    return Create();
}

bool Texture2DArray::SetData(unsigned layer, unsigned level, int x, int y, int width, int height, const void* data)
{
    URHO3D_PROFILE(SetTextureData);

    if (!object_.name_ || !graphics_)
    {
        URHO3D_LOGERROR("Texture array not created, can not set data");
        return false;
    }

    if (!data)
    {
        URHO3D_LOGERROR("Null source for setting data");
        return false;
    }

    if (layer >= layers_)
    {
        URHO3D_LOGERROR("Illegal layer for setting data");
        return false;
    }

    if (level >= levels_)
    {
        URHO3D_LOGERROR("Illegal mip level for setting data");
        return false;
    }

    if (graphics_->IsDeviceLost())
    {
        URHO3D_LOGWARNING("Texture array data assignment while device is lost");
        dataPending_ = true;
        return true;
    }

    if (IsCompressed())
    {
        x &= ~3;
        y &= ~3;
    }

    int levelWidth = GetLevelWidth(level);
    int levelHeight = GetLevelHeight(level);
    if (x < 0 || x + width > levelWidth || y < 0 || y + height > levelHeight || width <= 0 || height <= 0)
    {
        URHO3D_LOGERROR("Illegal dimensions for setting data");
        return false;
    }

    graphics_->SetTextureForUpdate(this);

#ifndef GL_ES_VERSION_2_0
    bool wholeLevel = x == 0 && y == 0 && width == levelWidth && height == levelHeight && layer == 0;
    unsigned format = GetSRGB() ? GetSRGBFormat(format_) : format_;

    if (!IsCompressed())
    {
        if (wholeLevel)
            glTexImage3D(target_, level, format, width, height, layers_, 0, GetExternalFormat(format_),
                GetDataType(format_), 0);
        glTexSubImage3D(target_, level, x, y, layer, width, height, 1, GetExternalFormat(format_),
            GetDataType(format_), data);
    }
    else
    {
        if (wholeLevel)
            glCompressedTexImage3D(target_, level, format, width, height, layers_, 0,
                GetDataSize(width, height, layers_), 0);
        glCompressedTexSubImage3D(target_, level, x, y, layer, width, height, 1, format,
            GetDataSize(width, height), data);
    }
#endif

    graphics_->SetTexture(0, 0);
    return true;
}

bool Texture2DArray::SetData(unsigned layer, Deserializer& source)
{
    SharedPtr<Image> image(new Image(context_));
    if (!image->Load(source))
        return false;

    return SetData(layer, image);
}

bool Texture2DArray::SetData(unsigned layer, Image* image, bool useAlpha)
{
    if (!image)
    {
        URHO3D_LOGERROR("Null image, can not set data");
        return false;
    }
    if (!layers_)
    {
        URHO3D_LOGERROR("Number of layers in the array must be set first");
        return false;
    }
    if (layer >= layers_)
    {
        URHO3D_LOGERROR("Illegal layer for setting data");
        return false;
    }

    // Use a shared ptr for managing the temporary mip images created during this function
    SharedPtr<Image> mipImage;
    unsigned memoryUse = 0;
    int quality = QUALITY_HIGH;
    Renderer* renderer = GetSubsystem<Renderer>();
    if (renderer)
        quality = renderer->GetTextureQuality();

    if (!image->IsCompressed())
    {
        // Convert unsuitable formats to RGBA
        unsigned components = image->GetComponents();
        if (Graphics::GetGL3Support() && ((components == 1 && !useAlpha) || components == 2))
        {
            mipImage = image->ConvertToRGBA(); image = mipImage;
            if (!image)
                return false;
            components = image->GetComponents();
        }

        unsigned char* levelData = image->GetData();
        int levelWidth = image->GetWidth();
        int levelHeight = image->GetHeight();
        unsigned format = 0;

        // Discard unnecessary mip levels
        for (unsigned i = 0; i < mipsToSkip_[quality]; ++i)
        {
            mipImage = image->GetNextLevel(); image = mipImage;
            levelData = image->GetData();
            levelWidth = image->GetWidth();
            levelHeight = image->GetHeight();
        }

        switch (components)
        {
        case 1:
            format = useAlpha ? Graphics::GetAlphaFormat() : Graphics::GetLuminanceFormat();
            break;

        case 2:
            format = Graphics::GetLuminanceAlphaFormat();
            break;

        case 3:
            format = Graphics::GetRGBFormat();
            break;

        case 4:
            format = Graphics::GetRGBAFormat();
            break;

        default:
            assert(false);  // Should not reach here
            break;
        }

        // Create the texture array when layer 0 is being loaded, check that rest of the layers are same size & format
        if (!layer)
        {
            // If image was previously compressed, reset number of requested levels to avoid error if level count is too high for new size
            if (IsCompressed() && requestedLevels_ > 1)
                requestedLevels_ = 0;
            // Create the texture array (the number of layers must have been already set)
            SetSize(0, levelWidth, levelHeight, format);
        }
        else
        {
            if (!object_.name_)
            {
                URHO3D_LOGERROR("Texture array layer 0 must be loaded first");
                return false;
            }
            if (levelWidth != width_ || levelHeight != height_ || format != format_)
            {
                URHO3D_LOGERROR("Texture array layer does not match size or format of layer 0");
                return false;
            }
        }

        for (unsigned i = 0; i < levels_; ++i)
        {
            SetData(layer, i, 0, 0, levelWidth, levelHeight, levelData);
            memoryUse += levelWidth * levelHeight * components;

            if (i < levels_ - 1)
            {
                mipImage = image->GetNextLevel(); image = mipImage;
                levelData = image->GetData();
                levelWidth = image->GetWidth();
                levelHeight = image->GetHeight();
            }
        }
    }
    else
    {
        int width = image->GetWidth();
        int height = image->GetHeight();
        unsigned levels = image->GetNumCompressedLevels();
        unsigned format = graphics_->GetFormat(image->GetCompressedFormat());
        bool needDecompress = false;

        if (!format)
        {
            format = Graphics::GetRGBAFormat();
            needDecompress = true;
        }

        unsigned mipsToSkip = mipsToSkip_[quality];
        if (mipsToSkip >= levels)
            mipsToSkip = levels - 1;
        while (mipsToSkip && (width / (1 << mipsToSkip) < 4 || height / (1 << mipsToSkip) < 4))
            --mipsToSkip;
        width /= (1 << mipsToSkip);
        height /= (1 << mipsToSkip);

        // Create the texture array when layer 0 is being loaded, assume rest of the layers are same size & format
        if (!layer)
        {
            SetNumLevels(Max((levels - mipsToSkip), 1U));
            SetSize(0, width, height, format);
        }
        else
        {
            if (!object_.name_)
            {
                URHO3D_LOGERROR("Texture array layer 0 must be loaded first");
                return false;
            }
            if (width != width_ || height != height_ || format != format_)
            {
                URHO3D_LOGERROR("Texture array layer does not match size or format of layer 0");
                return false;
            }
        }

        for (unsigned i = 0; i < levels_ && i < levels - mipsToSkip; ++i)
        {
            CompressedLevel level = image->GetCompressedLevel(i + mipsToSkip);
            if (!needDecompress)
            {
                SetData(layer, i, 0, 0, level.width_, level.height_, level.data_);
                memoryUse += level.rows_ * level.rowSize_;
            }
            else
            {
                unsigned char* rgbaData = new unsigned char[level.width_ * level.height_ * 4];
                level.Decompress(rgbaData);
                SetData(layer, i, 0, 0, level.width_, level.height_, rgbaData);
                memoryUse += level.width_ * level.height_ * 4;
                delete[] rgbaData;
            }
        }
    }

    layerMemoryUse_[layer] = memoryUse;
    unsigned totalMemoryUse = sizeof(Texture2DArray) + layerMemoryUse_.Capacity() * sizeof(unsigned);
    for (unsigned i = 0; i < layers_; ++i)
        totalMemoryUse += layerMemoryUse_[i];
    SetMemoryUse(totalMemoryUse);

    return true;
}

bool Texture2DArray::GetData(unsigned layer, unsigned level, void* dest) const
{
#ifndef GL_ES_VERSION_2_0
    if (!object_.name_ || !graphics_)
    {
        URHO3D_LOGERROR("Texture array not created, can not get data");
        return false;
    }

    if (!dest)
    {
        URHO3D_LOGERROR("Null destination for getting data");
        return false;
    }

    if (layer != 0)
    {
        URHO3D_LOGERROR("Only the full download of the array is supported, set layer=0");
        return false;
    }

    if (level >= levels_)
    {
        URHO3D_LOGERROR("Illegal mip level for getting data");
        return false;
    }

    if (graphics_->IsDeviceLost())
    {
        URHO3D_LOGWARNING("Getting texture data while device is lost");
        return false;
    }

    graphics_->SetTextureForUpdate(const_cast<Texture2DArray*>(this));

    if (!IsCompressed())
        glGetTexImage(target_, level, GetExternalFormat(format_), GetDataType(format_), dest);
    else
        glGetCompressedTexImage(target_, level, dest);

    graphics_->SetTexture(0, 0);
    return true;
#else
    URHO3D_LOGERROR("Getting texture data not supported");
    return false;
#endif
}

bool Texture2DArray::Create()
{
    Release();

#ifdef GL_ES_VERSION_2_0
    URHO3D_LOGERROR("Failed to create 2D array texture, currently unsupported on OpenGL ES 2");
    return false;
#else

    if (!graphics_ || !width_ || !height_ || !layers_)
        return false;

    if (graphics_->IsDeviceLost())
    {
        URHO3D_LOGWARNING("Texture array creation while device is lost");
        return true;
    }

    glGenTextures(1, &object_.name_);

    // Ensure that our texture is bound to OpenGL texture unit 0
    graphics_->SetTextureForUpdate(this);

    unsigned format = GetSRGB() ? GetSRGBFormat(format_) : format_;
    unsigned externalFormat = GetExternalFormat(format_);
    unsigned dataType = GetDataType(format_);

    // If not compressed, create the initial level 0 texture with null data
    bool success = true;
    if (!IsCompressed())
    {
        glGetError();
        glTexImage3D(target_, 0, format, width_, height_, layers_, 0, externalFormat, dataType, 0);
        if (glGetError())
            success = false;
    }
    if (!success)
        URHO3D_LOGERROR("Failed to create texture array");

    // Set mipmapping
    levels_ = requestedLevels_;
    if (!levels_)
    {
        unsigned maxSize = (unsigned)Max(width_, height_);
        while (maxSize)
        {
            maxSize >>= 1;
            ++levels_;
        }
    }

    glTexParameteri(target_, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(target_, GL_TEXTURE_MAX_LEVEL, levels_ - 1);

    // Set initial parameters, then unbind the texture
    UpdateParameters();
    graphics_->SetTexture(0, 0);

    return success;
#endif
}

void Texture2DArray::HandleRenderSurfaceUpdate(StringHash eventType, VariantMap& eventData)
{
    if (renderSurface_ && (renderSurface_->GetUpdateMode() == SURFACE_UPDATEALWAYS || renderSurface_->IsUpdateQueued()))
    {
        Renderer* renderer = GetSubsystem<Renderer>();
        if (renderer)
            renderer->QueueRenderSurface(renderSurface_);
        renderSurface_->ResetUpdateQueued();
    }
}

}

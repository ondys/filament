/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gltfio/ResourceLoader.h>

#include "FFilamentAsset.h"
#include "upcast.h"

#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>
#include <filament/VertexBuffer.h>

#include <geometry/SurfaceOrientation.h>

#include <math/quat.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <utils/Log.h>

#include <cgltf.h>

#include <stb_image.h>

#include <tsl/robin_map.h>

#include <string>

using namespace filament;
using namespace filament::math;
using namespace utils;

namespace gltfio {
namespace details {

// The AssetPool tracks references to raw source data (cgltf hierarchies) and frees them
// appropriately. It releases all source assets only after the pending upload count is zero and the
// client has destroyed the ResourceLoader object. If the ResourceLoader is destroyed while uploads
// are still pending, then the AssetPool will stay alive until all uploads are complete.
class AssetPool {
public:
    AssetPool()  {}
    ~AssetPool() {
        for (auto asset : mAssets) {
            asset->releaseSourceAsset();
        }
    }
    void addAsset(FFilamentAsset* asset) {
        mAssets.push_back(asset);
        asset->acquireSourceAsset();
    }
    void addPendingUpload() {
        ++mPendingUploads;
    }
    static void onLoadedResource(void* buffer, size_t size, void* user) {
        auto pool = (AssetPool*) user;
        if (--pool->mPendingUploads == 0 && pool->mLoaderDestroyed) {
            delete pool;
        }
    }
    void onLoaderDestroyed() {
        if (mPendingUploads == 0) {
            delete this;
        } else {
            mLoaderDestroyed = true;
        }
    }
private:
    std::vector<FFilamentAsset*> mAssets;
    bool mLoaderDestroyed = false;
    int mPendingUploads = 0;
};

} // namespace details

using namespace details;

ResourceLoader::ResourceLoader(const ResourceConfiguration& config) : mConfig(config),
        mPool(new AssetPool) {}

ResourceLoader::~ResourceLoader() {
    mPool->onLoaderDestroyed();
}

static void importSkinningData(Skin& dstSkin, const cgltf_skin& srcSkin) {
    const cgltf_accessor* srcMatrices = srcSkin.inverse_bind_matrices;
    dstSkin.inverseBindMatrices.resize(srcSkin.joints_count);
    if (srcMatrices) {
        auto dstMatrices = (uint8_t*) dstSkin.inverseBindMatrices.data();
        auto srcBuffer = srcMatrices->buffer_view->buffer->data;
        memcpy(dstMatrices, srcBuffer, srcSkin.joints_count * sizeof(mat4f));
    }
}

static void convertBytesToShorts(uint16_t* dst, const uint8_t* src, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = src[i];
    }
}

bool ResourceLoader::loadResources(FilamentAsset* asset) {
    FFilamentAsset* fasset = upcast(asset);
    mPool->addAsset(fasset);
    auto gltf = (cgltf_data*) fasset->mSourceAsset;

    // Read data from the file system and base64 URLs.
    cgltf_options options {};
    cgltf_result result = cgltf_load_buffers(&options, gltf, mConfig.basePath.c_str());
    if (result != cgltf_result_success) {
        slog.e << "Unable to load resources." << io::endl;
        return false;
    }

    // To be robust against the glTF conformance suite, we optionally ensure that skinning weights
    // sum to 1.0 at every vertex. Note that if the same weights buffer is shared in multiple
    // places, this will needlessly repeat the work. In the future we would like to remove this
    // feature, and instead simply require correct models. See also:
    // https://github.com/KhronosGroup/glTF-Sample-Models/issues/215
    if (mConfig.normalizeSkinningWeights) {
        cgltf_size mcount = gltf->meshes_count;
        for (cgltf_size mindex = 0; mindex < mcount; ++mindex) {
            const cgltf_mesh& mesh = gltf->meshes[mindex];
            cgltf_size pcount = mesh.primitives_count;
            for (cgltf_size pindex = 0; pindex < pcount; ++pindex) {
                const cgltf_primitive& prim = mesh.primitives[pindex];
                cgltf_size acount = prim.attributes_count;
                for (cgltf_size aindex = 0; aindex < acount; ++aindex) {
                    const auto& attr = prim.attributes[aindex];
                    if (attr.type == cgltf_attribute_type_weights) {
                        normalizeWeights(attr.data);
                    }
                }
            }
        }
    }

    // Upload data to the GPU.
    const BufferBinding* bindings = asset->getBufferBindings();
    for (size_t i = 0, n = asset->getBufferBindingCount(); i < n; ++i) {
        auto bb = bindings[i];
        const uint8_t* data8 = bb.offset + (const uint8_t*) *bb.data;
        if (bb.vertexBuffer) {
            mPool->addPendingUpload();
            VertexBuffer::BufferDescriptor bd(data8, bb.size, AssetPool::onLoadedResource, mPool);
            bb.vertexBuffer->setBufferAt(*mConfig.engine, bb.bufferIndex, std::move(bd));
        } else if (bb.indexBuffer && bb.convertBytesToShorts) {
            size_t size16 = bb.size * 2;
            uint16_t* data16 = (uint16_t*) malloc(size16);
            convertBytesToShorts(data16, data8, bb.size);
            auto callback = (IndexBuffer::BufferDescriptor::Callback) free;
            IndexBuffer::BufferDescriptor bd(data16, size16, callback);
            bb.indexBuffer->setBuffer(*mConfig.engine, std::move(bd));
        } else if (bb.indexBuffer && bb.generateTrivialIndices) {
            slog.e << "NOT YET IMPLEMENTED." << io::endl;
            return false;
        } else if (bb.indexBuffer) {
            mPool->addPendingUpload();
            IndexBuffer::BufferDescriptor bd(data8, bb.size, AssetPool::onLoadedResource, mPool);
            bb.indexBuffer->setBuffer(*mConfig.engine, std::move(bd));
        } else {
            slog.e << "Malformed binding: " << bb.uri << io::endl;
            return false;
        }
    }

    // Copy over the inverse bind matrices to allow users to destroy the source asset.
    for (cgltf_size i = 0, len = gltf->skins_count; i < len; ++i) {
        importSkinningData(fasset->mSkins[i], gltf->skins[i]);
    }

    // Compute surface orientation quaternions if necessary.
    computeTangents(fasset);

    // Finally, load image files and create Filament Textures.
    return createTextures(fasset);
}

bool ResourceLoader::createTextures(const details::FFilamentAsset* asset) const {
    // Define a simple functor that creates a Filament Texture from a blob of texels.
    // TODO: this could be optimized, e.g. do not generate mips if never mipmap-sampled, and use a
    // more compact format when possible.
    auto createTexture = [this](stbi_uc* texels, uint32_t w, uint32_t h, bool srgb) {
        Texture *tex = Texture::Builder()
                .width(w)
                .height(h)
                .levels(0xff)
                .format(srgb ? driver::TextureFormat::SRGB8_A8 : driver::TextureFormat::RGBA8)
                .build(*mConfig.engine);

        Texture::PixelBufferDescriptor pbd(texels,
                size_t(w * h * 4),
                Texture::Format::RGBA,
                Texture::Type::UBYTE,
                (driver::BufferDescriptor::Callback) &free);

        tex->setImage(*mConfig.engine, 0, std::move(pbd));
        tex->generateMipmaps(*mConfig.engine);
        return tex;
    };

    // Decode textures and associate them with material instance parameters.
    // To prevent needless re-decoding, we create a small map of Filament Texture objects where the
    // map key is (usually) a pointer to a URI string. In the case of buffer view textures, the
    // cache key is a data pointer.
    stbi_uc* texels;
    int width, height, comp;
    Texture* tex;
    tsl::robin_map<void*, Texture*> textures;
    const TextureBinding* texbindings = asset->getTextureBindings();
    for (size_t i = 0, n = asset->getTextureBindingCount(); i < n; ++i) {
        auto tb = texbindings[i];
        if (tb.data) {
            tex = textures[tb.data];
            if (!tex) {
                texels = stbi_load_from_memory((const stbi_uc*) *tb.data, tb.totalSize,
                        &width, &height, &comp, 4);
                if (texels == nullptr) {
                    slog.e << "Unable to decode texture. " << io::endl;
                    return false;
                }
                textures[tb.data] = tex = createTexture(texels, width, height, tb.srgb);
            }
        } else {
            tex = textures[(void*) tb.uri];
            if (!tex) {
                utils::Path fullpath = this->mConfig.basePath + tb.uri;
                texels = stbi_load(fullpath.c_str(), &width, &height, &comp, 4);
                if (texels == nullptr) {
                    slog.e << "Unable to decode texture: " << tb.uri << io::endl;
                    return false;
                }
                textures[(void*) tb.uri] = tex = createTexture(texels, width, height, tb.srgb);
            }
        }
        tb.materialInstance->setParameter(tb.materialParameter, tex, tb.sampler);
    }
    return true;
}

void ResourceLoader::computeTangents(const FFilamentAsset* asset) const {
    // Declare vectors of normals and tangents, which we'll extract & convert from the source.
    std::vector<float3> fp32Normals;
    std::vector<float4> fp32Tangents;
    std::vector<float3> fp32Positions;
    std::vector<float2> fp32TexCoords;
    std::vector<ushort3> ui16Triangles;

    auto computeQuats = [&](const cgltf_primitive& prim) {

        cgltf_size vertexCount = 0;

        // Collect accessors for normals, tangents, etc.
        const int NUM_ATTRIBUTES = 8;
        int slots[NUM_ATTRIBUTES] = {};
        const cgltf_accessor* accessors[NUM_ATTRIBUTES] = {};
        for (cgltf_size slot = 0; slot < prim.attributes_count; slot++) {
            const cgltf_attribute& attr = prim.attributes[slot];
            // Ignore the second set of UV's.
            if (attr.index != 0) {
                continue;
            }
            vertexCount = attr.data->count;
            slots[attr.type] = slot;
            accessors[attr.type] = attr.data;
        }

        // At a minimum we need normals to generate tangents.
        auto normalsInfo = accessors[cgltf_attribute_type_normal];
        if (normalsInfo == nullptr || vertexCount == 0) {
            return;
        }

        short4* quats = (short4*) malloc(sizeof(short4) * vertexCount);
        auto& sob = geometry::SurfaceOrientation::Builder()
            .vertexCount(vertexCount);

        // Convert normals into packed floats.
        assert(normalsInfo->count == vertexCount);
        assert(normalsInfo->type == cgltf_type_vec3);
        fp32Normals.resize(vertexCount);
        for (cgltf_size i = 0; i < vertexCount; ++i) {
            cgltf_accessor_read_float(normalsInfo, i, &fp32Normals[i].x, 3);
        }
        sob.normals(fp32Normals.data());

        // Convert tangents into packed floats.
        auto tangentsInfo = accessors[cgltf_attribute_type_tangent];
        if (tangentsInfo) {
            if (tangentsInfo->count != vertexCount || tangentsInfo->type != cgltf_type_vec4) {
                slog.e << "Bad tangent count or type." << io::endl;
                return;
            }
            fp32Tangents.resize(vertexCount);
            for (cgltf_size i = 0; i < vertexCount; ++i) {
                cgltf_accessor_read_float(tangentsInfo, i, &fp32Tangents[i].x, 4);
            }
            sob.tangents(fp32Tangents.data());
        }

        auto positionsInfo = accessors[cgltf_attribute_type_position];
        if (positionsInfo) {
            if (positionsInfo->count != vertexCount || positionsInfo->type != cgltf_type_vec3) {
                slog.e << "Bad position count or type." << io::endl;
                return;
            }
            fp32Positions.resize(vertexCount);
            for (cgltf_size i = 0; i < vertexCount; ++i) {
                cgltf_accessor_read_float(positionsInfo, i, &fp32Positions[i].x, 3);
            }
            sob.positions(fp32Positions.data());
        }

        ui16Triangles.resize(prim.indices->count);
        size_t triangleCount = prim.indices->count / 3, j = 0;
        for (cgltf_size i = 0; i < triangleCount; ++i) {
            ui16Triangles[i].x = cgltf_accessor_read_index(prim.indices, j++);
            ui16Triangles[i].y = cgltf_accessor_read_index(prim.indices, j++);
            ui16Triangles[i].z = cgltf_accessor_read_index(prim.indices, j++);
        }
        sob.triangleCount(triangleCount);
        sob.triangles(ui16Triangles.data());

        auto texcoordsInfo = accessors[cgltf_attribute_type_texcoord];
        if (texcoordsInfo) {
            if (texcoordsInfo->count != vertexCount || texcoordsInfo->type != cgltf_type_vec2) {
                slog.e << "Bad texcoord count or type." << io::endl;
                return;
            }
            fp32TexCoords.resize(vertexCount);
            for (cgltf_size i = 0; i < vertexCount; ++i) {
                cgltf_accessor_read_float(texcoordsInfo, i, &fp32TexCoords[i].x, 2);
            }
            sob.uvs(fp32TexCoords.data());
        }

        // Compute surface orientation quaternions.
        auto helper = sob.build();
        helper.getQuats(quats, vertexCount);

        // Upload quaternions to the GPU.
        auto callback = (VertexBuffer::BufferDescriptor::Callback) free;
        VertexBuffer::BufferDescriptor bd(quats, vertexCount * sizeof(short4), callback);
        VertexBuffer* vb = asset->mPrimMap.at(&prim);
        vb->setBufferAt(*mConfig.engine, slots[cgltf_attribute_type_normal], std::move(bd));
    };

    for (auto iter : asset->mNodeMap) {
        const cgltf_mesh* mesh = iter.first->mesh;
        if (mesh) {
            cgltf_size nprims = mesh->primitives_count;
            for (cgltf_size index = 0; index < nprims; ++index) {
                computeQuats(mesh->primitives[index]);
            }
        }
    }
}

void ResourceLoader::normalizeWeights(cgltf_accessor* data) const {
    if (data->type != cgltf_type_vec4 || data->component_type != cgltf_component_type_r_32f) {
        slog.w << "Cannot normalize weights, unsupported attribute type." << io::endl;
        return;
    }
    uint8_t* bytes = (uint8_t*) data->buffer_view->buffer->data;
    float4* floats = (float4*) (bytes + data->offset + data->buffer_view->offset);
    for (cgltf_size i = 0; i < data->count; ++i) {
        float4 weights = floats[i];
        float sum = weights.x + weights.y + weights.z + weights.w;
        floats[i] = weights / sum;
    }
}

} // namespace gltfio

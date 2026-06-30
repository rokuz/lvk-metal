#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <fast_obj.h>
#include <meshoptimizer.h>

namespace lvk::metal::bistro {

constexpr uint32_t kMeshCacheVersion = 0xC0DE000A;
constexpr int kMaxMaterialName = 128;

struct VertexData {
  glm::vec3 position;
  uint32_t uv;
  uint16_t normal;
  uint16_t mtlIndex;
};
static_assert(sizeof(VertexData) == 5 * sizeof(uint32_t));

struct CachedMaterial {
  char name[kMaxMaterialName] = {};
  glm::vec3 ambient = glm::vec3(0.0f);
  glm::vec3 diffuse = glm::vec3(0.0f);
  char ambient_texname[kMaxMaterialName] = {};
  char diffuse_texname[kMaxMaterialName] = {};
  char alpha_texname[kMaxMaterialName] = {};
};

inline glm::vec2 octMsign(glm::vec2 v) {
  return glm::vec2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}
inline uint16_t octPackSnorm2x8(glm::vec2 v) {
  glm::uvec2 d = glm::uvec2(glm::round(127.5f + v * 127.5f));
  return uint16_t(d.x | (d.y << 8u));
}
inline uint16_t packOctahedral16(glm::vec3 n) {
  n /= (glm::abs(n.x) + glm::abs(n.y) + glm::abs(n.z));
  return octPackSnorm2x8((n.z >= 0.0f) ? glm::vec2(n.x, n.y) : (glm::vec2(1.0f) - glm::abs(glm::vec2(n.y, n.x))) * octMsign(glm::vec2(n)));
}
inline void copyName(char* dst, const char* src) {
  if (src && src[0])
    std::strncpy(dst, src, kMaxMaterialName - 1);
}

struct Mesh {
  std::vector<VertexData> vertices;
  std::vector<uint32_t> indices;
  std::vector<CachedMaterial> materials;

  bool loadFromCache(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
      return false;
    bool ok = false;
    uint32_t version = 0, numMat = 0, numVert = 0, numIdx = 0;
    if (fread(&version, sizeof(version), 1, f) == 1 && version == kMeshCacheVersion && fread(&numMat, 4, 1, f) == 1 &&
        fread(&numVert, 4, 1, f) == 1 && fread(&numIdx, 4, 1, f) == 1) {
      materials.resize(numMat);
      vertices.resize(numVert);
      indices.resize(numIdx);
      ok = fread(materials.data(), sizeof(CachedMaterial), numMat, f) == numMat &&
           fread(vertices.data(), sizeof(VertexData), numVert, f) == numVert &&
           fread(indices.data(), sizeof(uint32_t), numIdx, f) == numIdx;
    }
    fclose(f);
    return ok;
  }

  bool loadAndCache(const std::string& objPath, const std::string& cachePath) {
    fastObjMesh* mesh = fast_obj_read(objPath.c_str());
    if (!mesh)
      return false;
    uint32_t vertexIndex = 0;
    for (uint32_t face = 0; face < mesh->face_count; ++face) {
      for (uint32_t v = 0; v < mesh->face_vertices[face]; ++v) {
        const fastObjIndex gi = mesh->indices[vertexIndex++];
        const float* p = &mesh->positions[gi.p * 3];
        const float* n = &mesh->normals[gi.n * 3];
        const float* t = &mesh->texcoords[gi.t * 2];
        vertices.push_back({
            .position = glm::vec3(p[0], p[1], p[2]),
            .uv = glm::packHalf2x16(glm::vec2(t[0], t[1])),
            .normal = packOctahedral16(glm::vec3(n[0], n[1], n[2])),
            .mtlIndex = uint16_t(mesh->face_materials[face]),
        });
      }
    }
    {
      const size_t indexCount = vertices.size();
      std::vector<uint32_t> remap(indexCount);
      const size_t vcount = meshopt_generateVertexRemap(remap.data(), nullptr, indexCount, vertices.data(), indexCount, sizeof(VertexData));
      std::vector<VertexData> remapped(vcount);
      indices.resize(indexCount);
      meshopt_remapIndexBuffer(indices.data(), nullptr, indexCount, remap.data());
      meshopt_remapVertexBuffer(remapped.data(), vertices.data(), indexCount, sizeof(VertexData), remap.data());
      vertices = remapped;
      meshopt_optimizeVertexCache(indices.data(), indices.data(), indexCount, vcount);
      meshopt_optimizeOverdraw(indices.data(), indices.data(), indexCount, &vertices[0].position.x, vcount, sizeof(VertexData), 1.05f);
      meshopt_optimizeVertexFetch(vertices.data(), indices.data(), indexCount, vertices.data(), vcount, sizeof(VertexData));
    }
    for (uint32_t i = 0; i < mesh->material_count; ++i) {
      const fastObjMaterial& m = mesh->materials[i];
      CachedMaterial mtl;
      mtl.ambient = glm::vec3(m.Ka[0], m.Ka[1], m.Ka[2]);
      mtl.diffuse = glm::vec3(m.Kd[0], m.Kd[1], m.Kd[2]);
      copyName(mtl.ambient_texname, m.map_Ka < mesh->texture_count ? mesh->textures[m.map_Ka].name : nullptr);
      copyName(mtl.diffuse_texname, m.map_Kd < mesh->texture_count ? mesh->textures[m.map_Kd].name : nullptr);
      copyName(mtl.alpha_texname, m.map_d < mesh->texture_count ? mesh->textures[m.map_d].name : nullptr);
      materials.push_back(mtl);
    }
    fast_obj_destroy(mesh);

    if (FILE* f = fopen(cachePath.c_str(), "wb")) {
      const uint32_t numMat = uint32_t(materials.size());
      const uint32_t numVert = uint32_t(vertices.size());
      const uint32_t numIdx = uint32_t(indices.size());
      fwrite(&kMeshCacheVersion, 4, 1, f);
      fwrite(&numMat, 4, 1, f);
      fwrite(&numVert, 4, 1, f);
      fwrite(&numIdx, 4, 1, f);
      fwrite(materials.data(), sizeof(CachedMaterial), numMat, f);
      fwrite(vertices.data(), sizeof(VertexData), numVert, f);
      fwrite(indices.data(), sizeof(uint32_t), numIdx, f);
      fclose(f);
    }
    return true;
  }

  bool load(const std::string& contentDir) {
    const std::string cachePath = contentDir + "cache.data";
    if (loadFromCache(cachePath))
      return true;
    vertices.clear();
    indices.clear();
    materials.clear();
    return loadAndCache(contentDir + "src/bistro/Exterior/exterior.obj", cachePath);
  }
};

} // namespace lvk::metal::bistro

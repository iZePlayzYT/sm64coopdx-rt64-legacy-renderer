/*-----------------------------------------------------------------------
Copyright (c) 2014-2018, NVIDIA. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Neither the name of its contributors may be used to endorse
or promote products derived from this software without specific
prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
-----------------------------------------------------------------------*/

/*
Contacts for feedback:
- pgautron@nvidia.com (Pascal Gautron)
- mlefrancois@nvidia.com (Martin-Karl Lefrancois)

The raytracing pipeline combines the raytracing shaders into a state object,
that can be thought of as an executable GPU program. For that, it requires the
shaders compiled as DXIL libraries, where each library exports symbols in a way
similar to DLLs. Those symbols are then used to refer to these shaders libraries
when creating hit groups, associating the shaders to their root signatures and
declaring the steps of the pipeline. All the calls to this helper class can be
done in arbitrary order. Some basic sanity checks are also performed when
compiling in debug mode.

*/

#include "RaytracingPipelineGenerator.h"

#include "dxcapi.h"
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

// Isolated from C++ unwind so MSVC can use SEH around the driver call.
// AMD's DXIL compiler (amdxc64.dll) has historically crashed here on some drivers.
static HRESULT CreateStateObjectSEH(ID3D12Device8 *device, const D3D12_STATE_OBJECT_DESC *desc, ID3D12StateObject **outObject) {
  if (outObject == nullptr) {
    return E_POINTER;
  }

  *outObject = nullptr;
  __try {
    return device->CreateStateObject(desc, IID_PPV_ARGS(outObject));
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    return HRESULT_FROM_WIN32(GetExceptionCode());
  }
}

namespace nv_helpers_dx12
{

//--------------------------------------------------------------------------------------------------
// The pipeline helper requires access to the device, as well as the
// raytracing device prior to Windows 10 RS5.
RayTracingPipelineGenerator::RayTracingPipelineGenerator(ID3D12Device8* device)
    : m_device(device)
{
}

void RayTracingPipelineGenerator::SetGlobalRootSignature(ID3D12RootSignature* rootSignature)
{
  m_dummyGlobalRootSignature = rootSignature;
}

ID3D12RootSignature* RayTracingPipelineGenerator::GetGlobalRootSignature() const
{
  return m_dummyGlobalRootSignature;
}

//--------------------------------------------------------------------------------------------------
//
// Add a DXIL library to the pipeline. Note that this library has to be
// compiled with dxc, using a lib_6_3 target. The exported symbols must correspond exactly to the
// names of the shaders declared in the library, although unused ones can be omitted.
void RayTracingPipelineGenerator::AddLibrary(IDxcBlob* dxilLibrary,
                                             const std::vector<std::wstring>& symbolExports)
{
  m_libraries.emplace_back(Library(dxilLibrary, symbolExports));
}

//--------------------------------------------------------------------------------------------------
//
// In DXR the hit-related shaders are grouped into hit groups. Such shaders are:
// - The intersection shader, which can be used to intersect custom geometry, and is called upon
//   hitting the bounding box the the object. A default one exists to intersect triangles
// - The any hit shader, called on each intersection, which can be used to perform early
//   alpha-testing and allow the ray to continue if needed. Default is a pass-through.
// - The closest hit shader, invoked on the hit point closest to the ray start.
// The shaders in a hit group share the same root signature, and are only referred to by the
// hit group name in other places of the program.
void RayTracingPipelineGenerator::AddHitGroup(const std::wstring& hitGroupName,
                                              const std::wstring& closestHitSymbol,
                                              const std::wstring& anyHitSymbol /*= L""*/,
                                              const std::wstring& intersectionSymbol /*= L""*/)
{
  m_hitGroups.emplace_back(
      HitGroup(hitGroupName, closestHitSymbol, anyHitSymbol, intersectionSymbol));
}

//--------------------------------------------------------------------------------------------------
//
// The shaders and hit groups may have various root signatures. This call associates a root
// signature to one or more symbols. All imported symbols must be associated to one root
// signature.
void RayTracingPipelineGenerator::AddRootSignatureAssociation(
    ID3D12RootSignature* rootSignature, const std::vector<std::wstring>& symbols)
{
  m_rootSignatureAssociations.emplace_back(RootSignatureAssociation(rootSignature, symbols));
}

//--------------------------------------------------------------------------------------------------
//
// The payload is the way hit or miss shaders can exchange data with the shader that called
// TraceRay. When several ray types are used (e.g. primary and shadow rays), this value must be
// the largest possible payload size. Note that to optimize performance, this size must be kept
// as low as possible.
void RayTracingPipelineGenerator::SetMaxPayloadSize(UINT sizeInBytes)
{
  m_maxPayLoadSizeInBytes = sizeInBytes;
}

//--------------------------------------------------------------------------------------------------
//
// When hitting geometry, a number of surface attributes can be generated by the intersector.
// Using the built-in triangle intersector the attributes are the barycentric coordinates, with a
// size 2*sizeof(float).
void RayTracingPipelineGenerator::SetMaxAttributeSize(UINT sizeInBytes)
{
  m_maxAttributeSizeInBytes = sizeInBytes;
}

//--------------------------------------------------------------------------------------------------
//
// Upon hitting a surface, a closest hit shader can issue a new TraceRay call. This parameter
// indicates the maximum level of recursion. Note that this depth should be kept as low as
// possible, typically 2, to allow hit shaders to trace shadow rays. Recursive ray tracing
// algorithms must be flattened to a loop in the ray generation program for best performance.
void RayTracingPipelineGenerator::SetMaxRecursionDepth(UINT maxDepth)
{
  m_maxRecursionDepth = maxDepth;
}

//--------------------------------------------------------------------------------------------------
//
// Compiles the raytracing state object
ID3D12StateObject* RayTracingPipelineGenerator::Generate()
{
  CreateDummyRootSignatures();

  // AMD's DXIL compiler (amdxc64) crashes in CreateStateObject when two exports from the
  // same DXIL library get different local root signatures. The NVIDIA helper left miss
  // shaders on an empty dummy RS while PrimaryRayGen kept the full raygen RS, both living
  // in PrimaryRayGen.hlsl. Inherit the library's existing local RS instead.
  {
    std::unordered_map<std::wstring, ID3D12RootSignature *> symbolToRS;
    for (const RootSignatureAssociation &assoc : m_rootSignatureAssociations) {
      for (const std::wstring &sym : assoc.m_symbols) {
        symbolToRS[sym] = assoc.m_rootSignature;
      }
    }

    for (const Library &lib : m_libraries) {
      ID3D12RootSignature *libraryRS = nullptr;
      for (const std::wstring &name : lib.m_exportedSymbols) {
        auto it = symbolToRS.find(name);
        if (it != symbolToRS.end()) {
          libraryRS = it->second;
          break;
        }
      }
      if (libraryRS == nullptr) {
        continue;
      }

      std::vector<std::wstring> extra;
      for (const std::wstring &name : lib.m_exportedSymbols) {
        if (symbolToRS.find(name) == symbolToRS.end()) {
          extra.push_back(name);
          symbolToRS[name] = libraryRS;
        }
      }
      if (!extra.empty()) {
        AddRootSignatureAssociation(libraryRS, extra);
      }
    }

    // Hit-group associations do not automatically apply to closest-hit/any-hit on AMD.
    // Bind those member shaders to the same local RS as their hit group.
    for (const HitGroup &group : m_hitGroups) {
      auto it = symbolToRS.find(group.m_hitGroupName);
      if (it == symbolToRS.end()) {
        continue;
      }

      ID3D12RootSignature *hitGroupRS = it->second;
      std::vector<std::wstring> extra;
      const std::wstring *members[] = {
          &group.m_closestHitSymbol, &group.m_anyHitSymbol, &group.m_intersectionSymbol};
      for (const std::wstring *name : members) {
        if (!name->empty() && symbolToRS.find(*name) == symbolToRS.end()) {
          extra.push_back(*name);
          symbolToRS[*name] = hitGroupRS;
        }
      }
      if (!extra.empty()) {
        AddRootSignatureAssociation(hitGroupRS, extra);
      }
    }
  }

  // One LOCAL_ROOT_SIGNATURE subobject per unique RS. Declaring the same object twice
  // (custom hit groups reuse one signature) crashes AMD.
  struct MergedLocalRS {
    ID3D12RootSignature *m_rootSignature = nullptr;
    ID3D12RootSignature *m_rootSignaturePointer = nullptr;
    std::vector<std::wstring> m_symbols;
    std::vector<LPCWSTR> m_symbolPointers;
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION m_association = {};
  };

  std::vector<MergedLocalRS> merged;
  merged.reserve(m_rootSignatureAssociations.size());
  for (const RootSignatureAssociation &assoc : m_rootSignatureAssociations) {
    MergedLocalRS *slot = nullptr;
    for (MergedLocalRS &existing : merged) {
      if (existing.m_rootSignature == assoc.m_rootSignature) {
        slot = &existing;
        break;
      }
    }
    if (slot == nullptr) {
      merged.emplace_back();
      slot = &merged.back();
      slot->m_rootSignature = assoc.m_rootSignature;
      slot->m_rootSignaturePointer = assoc.m_rootSignature;
    }
    slot->m_symbols.insert(slot->m_symbols.end(), assoc.m_symbols.begin(), assoc.m_symbols.end());
  }

  for (MergedLocalRS &entry : merged) {
    entry.m_symbolPointers.clear();
    entry.m_symbolPointers.reserve(entry.m_symbols.size());
    for (const std::wstring &name : entry.m_symbols) {
      entry.m_symbolPointers.push_back(name.c_str());
    }
  }

  // Leave the shader config unassociated so it is the default for every export.
  // Associating it only with hit-group names leaves closest-hit/any-hit without a
  // config on AMD. Do not emit an empty dummy local RS; unused exports get an
  // implicit empty local RS from the runtime.
  UINT64 subobjectCount =
      m_libraries.size() +
      m_hitGroups.size() +
      1 +                 // shader config (default)
      2 * merged.size() + // local RS declaration + association
      1 +                 // global RS
      1;                  // pipeline config

  // Allocate before adding subobjects; later entries point at earlier ones.
  std::vector<D3D12_STATE_SUBOBJECT> subobjects(subobjectCount);

  UINT currentIndex = 0;

  for (const Library& lib : m_libraries)
  {
    D3D12_STATE_SUBOBJECT libSubobject = {};
    libSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    libSubobject.pDesc = &lib.m_libDesc;

    subobjects[currentIndex++] = libSubobject;
  }

  for (const HitGroup& group : m_hitGroups)
  {
    D3D12_STATE_SUBOBJECT hitGroup = {};
    hitGroup.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    hitGroup.pDesc = &group.m_desc;

    subobjects[currentIndex++] = hitGroup;
  }

  D3D12_RAYTRACING_SHADER_CONFIG shaderDesc = {};
  shaderDesc.MaxPayloadSizeInBytes = m_maxPayLoadSizeInBytes;
  shaderDesc.MaxAttributeSizeInBytes = m_maxAttributeSizeInBytes;

  D3D12_STATE_SUBOBJECT shaderConfigObject = {};
  shaderConfigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
  shaderConfigObject.pDesc = &shaderDesc;

  subobjects[currentIndex++] = shaderConfigObject;

  for (MergedLocalRS& assoc : merged)
  {
    D3D12_STATE_SUBOBJECT rootSigObject = {};
    rootSigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
    rootSigObject.pDesc = &assoc.m_rootSignaturePointer;

    subobjects[currentIndex++] = rootSigObject;

    assoc.m_association.NumExports = static_cast<UINT>(assoc.m_symbolPointers.size());
    assoc.m_association.pExports = assoc.m_symbolPointers.data();
    assoc.m_association.pSubobjectToAssociate = &subobjects[(currentIndex - 1)];

    D3D12_STATE_SUBOBJECT rootSigAssociationObject = {};
    rootSigAssociationObject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    rootSigAssociationObject.pDesc = &assoc.m_association;

    subobjects[currentIndex++] = rootSigAssociationObject;
  }

  D3D12_STATE_SUBOBJECT globalRootSig = {};
  globalRootSig.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
  globalRootSig.pDesc = &m_dummyGlobalRootSignature;

  subobjects[currentIndex++] = globalRootSig;

  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
  pipelineConfig.MaxTraceRecursionDepth = m_maxRecursionDepth;

  D3D12_STATE_SUBOBJECT pipelineConfigObject = {};
  pipelineConfigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
  pipelineConfigObject.pDesc = &pipelineConfig;

  subobjects[currentIndex++] = pipelineConfigObject;

  D3D12_STATE_OBJECT_DESC pipelineDesc = {};
  pipelineDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
  pipelineDesc.NumSubobjects = currentIndex;
  pipelineDesc.pSubobjects = subobjects.data();

  ID3D12StateObject* rtStateObject = nullptr;

  HRESULT hr = CreateStateObjectSEH(m_device, &pipelineDesc, &rtStateObject);
  if (FAILED(hr))
  {
    fprintf(stderr, "RT64: CreateStateObject failed, hr=0x%08lX\n", static_cast<unsigned long>(hr));
    throw std::runtime_error("Could not create the raytracing state object");
  }

  return rtStateObject;
}

//--------------------------------------------------------------------------------------------------
//
// The pipeline needs a global root signature. An empty one is created only when the caller
// did not share one. A dummy local RS is not required: shaders without an association use an
// implicit empty local RS, and emitting an extra unassociated local RS crashes AMD.
void RayTracingPipelineGenerator::CreateDummyRootSignatures()
{
  if (m_dummyGlobalRootSignature != nullptr) {
    return;
  }

  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 0;
  rootDesc.pParameters = nullptr;
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  ID3DBlob* serializedRootSignature = nullptr;
  ID3DBlob* error = nullptr;
  HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &serializedRootSignature, &error);
  if (FAILED(hr))
  {
    if (error != nullptr) {
      error->Release();
    }
    throw std::logic_error("Could not serialize the global root signature");
  }
  hr = m_device->CreateRootSignature(0, serializedRootSignature->GetBufferPointer(),
                                     serializedRootSignature->GetBufferSize(),
                                     IID_PPV_ARGS(&m_dummyGlobalRootSignature));

  serializedRootSignature->Release();
  if (error != nullptr) {
    error->Release();
  }
  if (FAILED(hr))
  {
    throw std::logic_error("Could not create the global root signature");
  }
}

//--------------------------------------------------------------------------------------------------
//
// Build a list containing the export symbols for the ray generation shaders, miss shaders, and
// hit group names
void RayTracingPipelineGenerator::BuildShaderExportList(std::vector<std::wstring>& exportedSymbols)
{
  // Get all names from libraries
  // Get names associated to hit groups
  // Return list of libraries+hit group names - shaders in hit groups

  std::unordered_set<std::wstring> exports;

  // Add all the symbols exported by the libraries
  for (const Library& lib : m_libraries)
  {
    for (const auto& exportName : lib.m_exportedSymbols)
    {
#ifdef _DEBUG
      // Sanity check in debug mode: check that no name is exported more than once
      if (exports.find(exportName) != exports.end())
      {
        throw std::logic_error("Multiple definition of a symbol in the imported DXIL libraries");
      }
#endif
      exports.insert(exportName);
    }
  }

#ifdef _DEBUG
  // Sanity check in debug mode: verify that the hit groups do not reference an unknown shader name
  std::unordered_set<std::wstring> all_exports = exports;

  for (const auto& hitGroup : m_hitGroups)
  {
    if (!hitGroup.m_anyHitSymbol.empty() && exports.find(hitGroup.m_anyHitSymbol) == exports.end())
    {
      throw std::logic_error("Any hit symbol not found in the imported DXIL libraries");
    }

    if (!hitGroup.m_closestHitSymbol.empty() &&
        exports.find(hitGroup.m_closestHitSymbol) == exports.end())
    {
      throw std::logic_error("Closest hit symbol not found in the imported DXIL libraries");
    }

    if (!hitGroup.m_intersectionSymbol.empty() &&
        exports.find(hitGroup.m_intersectionSymbol) == exports.end())
    {
      throw std::logic_error("Intersection symbol not found in the imported DXIL libraries");
    }

    all_exports.insert(hitGroup.m_hitGroupName);
  }

  // Sanity check in debug mode: verify that the root signature associations do not reference an
  // unknown shader or hit group name
  for (const auto& assoc : m_rootSignatureAssociations)
  {
    for (const auto& symb : assoc.m_symbols)
    {
      if (!symb.empty() && all_exports.find(symb) == all_exports.end())
      {
        throw std::logic_error("Root association symbol not found in the "
                               "imported DXIL libraries and hit group names");
      }
    }
  }
#endif

  // Go through all hit groups and remove the symbols corresponding to intersection, any hit and
  // closest hit shaders from the symbol set
  for (const auto& hitGroup : m_hitGroups)
  {
    if (!hitGroup.m_anyHitSymbol.empty())
    {
      exports.erase(hitGroup.m_anyHitSymbol);
    }
    if (!hitGroup.m_closestHitSymbol.empty())
    {
      exports.erase(hitGroup.m_closestHitSymbol);
    }
    if (!hitGroup.m_intersectionSymbol.empty())
    {
      exports.erase(hitGroup.m_intersectionSymbol);
    }
    exports.insert(hitGroup.m_hitGroupName);
  }

  // Finally build a vector containing ray generation and miss shaders, plus the hit group names
  for (const auto& name : exports)
  {
    exportedSymbols.push_back(name);
  }
}

//--------------------------------------------------------------------------------------------------
//
// Store data related to a DXIL library: the library itself, the exported symbols, and the
// associated descriptors
RayTracingPipelineGenerator::Library::Library(IDxcBlob* dxil,
                                              const std::vector<std::wstring>& exportedSymbols)
    : m_dxil(dxil), m_exportedSymbols(exportedSymbols), m_exports(exportedSymbols.size())
{
  // Create one export descriptor per symbol
  for (size_t i = 0; i < m_exportedSymbols.size(); i++)
  {
    m_exports[i] = {};
    m_exports[i].Name = m_exportedSymbols[i].c_str();
    m_exports[i].ExportToRename = nullptr;
    m_exports[i].Flags = D3D12_EXPORT_FLAG_NONE;
  }

  // Create a library descriptor combining the DXIL code and the export names
  m_libDesc.DXILLibrary.BytecodeLength = dxil->GetBufferSize();
  m_libDesc.DXILLibrary.pShaderBytecode = dxil->GetBufferPointer();
  m_libDesc.NumExports = static_cast<UINT>(m_exportedSymbols.size());
  m_libDesc.pExports = m_exports.data();
}

//--------------------------------------------------------------------------------------------------
//
// This copy constructor has to be defined so that the export descriptors are set correctly. Using
// the default constructor would copy the string pointers of the symbols into the descriptors, which
// would cause issues when the original Library object gets out of scope
RayTracingPipelineGenerator::Library::Library(const Library& source)
    : Library(source.m_dxil, source.m_exportedSymbols)
{
}

//--------------------------------------------------------------------------------------------------
//
// Create a hit group descriptor from the input hit group name and shader symbols
RayTracingPipelineGenerator::HitGroup::HitGroup(std::wstring hitGroupName,
                                                std::wstring closestHitSymbol,
                                                std::wstring anyHitSymbol /*= L""*/,
                                                std::wstring intersectionSymbol /*= L""*/)
    : m_hitGroupName(std::move(hitGroupName)), m_closestHitSymbol(std::move(closestHitSymbol)),
      m_anyHitSymbol(std::move(anyHitSymbol)), m_intersectionSymbol(std::move(intersectionSymbol))
{
  // Indicate which shader program is used for closest hit, leave the other
  // ones undefined (default behavior), export the name of the group
  m_desc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
  m_desc.HitGroupExport = m_hitGroupName.c_str();
  m_desc.ClosestHitShaderImport = m_closestHitSymbol.empty() ? nullptr : m_closestHitSymbol.c_str();
  m_desc.AnyHitShaderImport = m_anyHitSymbol.empty() ? nullptr : m_anyHitSymbol.c_str();
  m_desc.IntersectionShaderImport =
      m_intersectionSymbol.empty() ? nullptr : m_intersectionSymbol.c_str();
}

//--------------------------------------------------------------------------------------------------
//
// This copy constructor has to be defined so that the export descriptors are set correctly. Using
// the default constructor would copy the string pointers of the symbols into the descriptors, which
// would cause issues when the original HitGroup object gets out of scope
RayTracingPipelineGenerator::HitGroup::HitGroup(const HitGroup& source)
    : HitGroup(source.m_hitGroupName, source.m_closestHitSymbol, source.m_anyHitSymbol,
               source.m_intersectionSymbol)
{
}

//--------------------------------------------------------------------------------------------------
//
// Store the association between a set of symbols and a root signature. The associated descriptors
// will be built when compiling the pipeline. We store the symbol pointers directly so that they can
// be used without processing during compilation.
RayTracingPipelineGenerator::RootSignatureAssociation::RootSignatureAssociation(
    ID3D12RootSignature* rootSignature, const std::vector<std::wstring>& symbols)
    : m_rootSignature(rootSignature), m_symbols(symbols), m_symbolPointers(symbols.size())
{
  for (size_t i = 0; i < m_symbols.size(); i++)
  {
    m_symbolPointers[i] = m_symbols[i].c_str();
  }
  m_rootSignaturePointer = m_rootSignature;
}

//--------------------------------------------------------------------------------------------------
//
// This copy constructor has to be defined so that the export descriptors are set correctly. Using
// the default constructor would copy the string pointers of the symbols into the descriptors, which
// would cause issues when the original RootSignatureAssociation object gets out of scope
RayTracingPipelineGenerator::RootSignatureAssociation::RootSignatureAssociation(
    const RootSignatureAssociation& source)
    : RootSignatureAssociation(source.m_rootSignature, source.m_symbols)
{
}
} // namespace nv_helpers_dx12

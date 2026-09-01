#include "pipeline.h"
#include "Include/Axodox.Storage.h"
#include <unordered_map>

using namespace Axodox::Storage;

void Pipeline::Bind(GraphicsDeviceContext* ctx)
{
	if (states) states->Bind();
	ctx->BindShaders(vs, hs, ds, gs, ps);
}

void Pipeline::InputLayout(VertexDefinition layout)
{
	if (vs) vs->InputLayout(layout);
}

void PipelineState::Bind()
{
	blend.Bind(blend_factor);
	depth.Bind();
	raster.Bind();
}

std::unordered_map<const char*, GraphicsResource*> pipeline_cache;

template <typename T>
T* getShader(const GraphicsDevice& device, const char* name) {
	auto& res = pipeline_cache[name];
	if (!res) res = new T(device, read_file(app_folder() / name));
	return static_cast<T*>(res);
}

VertexShader*	Pipeline::getVS(const GraphicsDevice& d, const char* n) { return getShader<VertexShader>	(d, n); }
HullShader*		Pipeline::getHS(const GraphicsDevice& d, const char* n) { return getShader<HullShader>		(d, n); }
DomainShader*	Pipeline::getDS(const GraphicsDevice& d, const char* n) { return getShader<DomainShader>	(d, n); }
GeometryShader*	Pipeline::getGS(const GraphicsDevice& d, const char* n) { return getShader<GeometryShader>	(d, n); }
PixelShader*	Pipeline::getPS(const GraphicsDevice& d, const char* n) { return getShader<PixelShader>		(d, n); }
ComputeShader*	Pipeline::getCS(const GraphicsDevice& d, const char* n) { return getShader<ComputeShader>	(d, n); }
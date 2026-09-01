#pragma once
#include <Include/Axodox.Graphics.h>

using namespace Axodox::Graphics;

struct PipelineState
{
	BlendState			blend;
	DepthStencilState	depth;
	RasterizerState		raster;
	//std::vector<SamplerState>			samplers;
	DirectX::XMFLOAT4 blend_factor;
	void Bind();
};

struct Pipeline
{
	std::shared_ptr<PipelineState> states;

	VertexShader*	vs = nullptr;
	HullShader*		hs = nullptr;
	DomainShader*	ds = nullptr;
	GeometryShader*	gs = nullptr;
	PixelShader*	ps = nullptr;

	void Bind(GraphicsDeviceContext* ctx);
	void InputLayout(VertexDefinition layout);

	static VertexShader*	getVS(const GraphicsDevice& device, const char* name);
	static HullShader*		getHS(const GraphicsDevice& device, const char* name);
	static DomainShader*	getDS(const GraphicsDevice& device, const char* name);
	static GeometryShader*	getGS(const GraphicsDevice& device, const char* name);
	static PixelShader*		getPS(const GraphicsDevice& device, const char* name);
	static ComputeShader*	getCS(const GraphicsDevice& device, const char* name);
};
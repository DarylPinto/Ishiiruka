// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <string>

#include "Common/StringUtil.h"
#include "VideoBackends/DX11/D3DBase.h"
#include "VideoBackends/DX11/D3DShader.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/HLSLCompiler.h"

namespace DX11
{

namespace D3D
{

VertexShaderPtr CreateVertexShaderFromByteCode(const void* bytecode, size_t len)
{
	VertexShaderPtr v_shader;
	HRESULT hr = D3D::device->CreateVertexShader(bytecode, len, nullptr, ToAddr(v_shader));
	if (FAILED(hr))
	{
		PanicAlert("CreateVertexShaderFromByteCode failed at %s %d\n", __FILE__, __LINE__);
	}
	return v_shader;
}


GeometryShaderPtr CreateGeometryShaderFromByteCode(const void* bytecode, size_t len)
{
	GeometryShaderPtr g_shader;
	HRESULT hr = D3D::device->CreateGeometryShader(bytecode, len, nullptr, ToAddr(g_shader));
	if (FAILED(hr))
	{
		PanicAlert("CreateGeometryShaderFromByteCode failed at %s %d\n", __FILE__, __LINE__);
	}

	return g_shader;
}

PixelShaderPtr CreatePixelShaderFromByteCode(const void* bytecode, size_t len)
{
	PixelShaderPtr p_shader;
	HRESULT hr = D3D::device->CreatePixelShader(bytecode, len, nullptr, ToAddr(p_shader));
	if (FAILED(hr))
	{
		PanicAlert("CreatePixelShaderFromByteCode failed at %s %d\n", __FILE__, __LINE__);
	}
	return p_shader;
}

ComputeShaderPtr CreateComputeShaderFromByteCode(const void* bytecode, size_t len)
{
	ComputeShaderPtr c_shader;
	HRESULT hr = D3D::device->CreateComputeShader(bytecode, len, nullptr, ToAddr(c_shader));
	if (FAILED(hr))
	{
		PanicAlert("CreateComputeShaderFromByteCode failed at %s %d\n", __FILE__, __LINE__);
	}
	return c_shader;
}

// code->bytecode
bool CompileShader(
	ShaderType type,
	const std::string& code,
	D3DBlob& blob,
	const D3D_SHADER_MACRO* pDefines)
{
	char const *profile = nullptr;
	char const *sufix = nullptr;
	switch (type) {
	case DX11::D3D::ShaderType::Vertex:
		profile = D3D::VertexShaderVersionString();
		sufix = "vs";
		break;
	case DX11::D3D::ShaderType::Pixel:
		profile = D3D::PixelShaderVersionString();
		sufix = "ps";
		break;
	case DX11::D3D::ShaderType::Geometry:
		profile = D3D::GeometryShaderVersionString();
		sufix = "gs";
		break;
	case DX11::D3D::ShaderType::Compute:
		profile = D3D::ComputeShaderVersionString();
		sufix = "cs";
		break;
	default:
		return false;
		break;
	}

#if defined(_DEBUG) || defined(DEBUGFAST)
	UINT flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT flags = D3D10_SHADER_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_SKIP_VALIDATION;
#endif

	ID3DBlobPtr shaderBuffer;
	ID3DBlobPtr errorBuffer;
	HRESULT hr = HLSLCompiler::getInstance().CompileShader(code.c_str(),
		code.length(), nullptr, pDefines, nullptr, "main", profile,
		flags, 0, ToAddr(shaderBuffer), ToAddr(errorBuffer));

	if (errorBuffer)
	{
		INFO_LOG(VIDEO, "Shader compiler messages:\n%s",
			(const char*)errorBuffer->GetBufferPointer());
	}

	if (FAILED(hr))
	{
		static int num_failures = 0;
		char szTemp[MAX_PATH];
		sprintf(szTemp, "%sbad_%s_%04i.txt", File::GetUserPath(D_DUMP_IDX).c_str(), sufix, num_failures++);
		std::ofstream file;
		OpenFStream(file, szTemp, std::ios_base::out);
		file << code;
		file.close();

		PanicAlert("Failed to compile shader!\nThis usually happens when trying to use Dolphin with an outdated GPU or integrated GPU like the Intel GMA series.\n\nIf you're sure this is Dolphin's error anyway, post the contents of %s along with this error message at the forums.\n\nDebug info (%s):\n%s",
			szTemp,
			profile,
			(char*)errorBuffer->GetBufferPointer());

		blob = nullptr;
	}
	else
	{
		blob = std::move(shaderBuffer);
	}

	return SUCCEEDED(hr);
}

VertexShaderPtr CompileAndCreateVertexShader(const std::string& code, const D3D_SHADER_MACRO* pDefines)
{
	D3DBlob blob;
	if (CompileShader(DX11::D3D::ShaderType::Vertex, code, blob, pDefines))
	{
		return CreateVertexShaderFromByteCode(blob);
	}
	return nullptr;
}

GeometryShaderPtr CompileAndCreateGeometryShader(const std::string& code, const D3D_SHADER_MACRO* pDefines)
{
	D3DBlob blob;
	if (CompileShader(DX11::D3D::ShaderType::Geometry, code, blob, pDefines))
	{
		return CreateGeometryShaderFromByteCode(blob);
	}
	return nullptr;
}

PixelShaderPtr CompileAndCreatePixelShader(const std::string& code, const D3D_SHADER_MACRO* pDefines)
{
	D3DBlob blob;
	if (CompileShader(DX11::D3D::ShaderType::Pixel, code, blob, pDefines))
	{
		return CreatePixelShaderFromByteCode(blob);
	}
	return nullptr;
}

ComputeShaderPtr CompileAndCreateComputeShader(const std::string& code, const D3D_SHADER_MACRO* pDefines)
{
	D3DBlob blob;
	if (CompileShader(DX11::D3D::ShaderType::Compute, code, blob, pDefines))
	{
		return CreateComputeShaderFromByteCode(blob);
	}
	return nullptr;
}

}  // namespace

}  // namespace DX11
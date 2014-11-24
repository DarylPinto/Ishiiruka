// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Common/Common.h"
#include "Common/Hash.h"
#include "Common/FileUtil.h"
#include "Common/LinearDiskCache.h"
#include "Core/ConfigManager.h"

#include "VideoCommon/Statistics.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/ImageWrite.h"
#include "VideoCommon/Debugger.h"
#include "VideoCommon/HLSLCompiler.h"

#include "Globals.h"
#include "D3DBase.h"
#include "D3DShader.h"
#include "PixelShaderCache.h"
namespace DX9
{

PixelShaderCache::PSCache PixelShaderCache::PixelShaders;
const PixelShaderCache::PSCacheEntry *PixelShaderCache::last_entry[DSTALPHA_NULL + 1];
PixelShaderUid PixelShaderCache::last_uid[DSTALPHA_NULL + 1];
PixelShaderUid PixelShaderCache::external_last_uid[DSTALPHA_NULL + 1];
UidChecker<PixelShaderUid,ShaderCode> PixelShaderCache::pixel_uid_checker;

static HLSLAsyncCompiler *Compiler;
static Common::SpinLock<true> PixelShadersLock;
static LinearDiskCache<PixelShaderUid, u8> g_ps_disk_cache;
static std::set<u32> unique_shaders;

#define MAX_SSAA_SHADERS 3
enum
{
	COPY_TYPE_DIRECT,
	COPY_TYPE_MATRIXCOLOR,
	NUM_COPY_TYPES
};
enum
{
	DEPTH_CONVERSION_TYPE_NONE,
	DEPTH_CONVERSION_TYPE_ON,
	NUM_DEPTH_CONVERSION_TYPES
};

static LPDIRECT3DPIXELSHADER9 s_CopyProgram[NUM_COPY_TYPES][NUM_DEPTH_CONVERSION_TYPES][MAX_SSAA_SHADERS];
static LPDIRECT3DPIXELSHADER9 s_ClearProgram = NULL;
static LPDIRECT3DPIXELSHADER9 s_rgba6_to_rgb8 = NULL;
static LPDIRECT3DPIXELSHADER9 s_rgb8_to_rgba6 = NULL;

class PixelShaderCacheInserter : public LinearDiskCacheReader<PixelShaderUid, u8>
{
public:
	void Read(const PixelShaderUid &key, const u8 *value, u32 value_size)
	{
		PixelShaderCache::InsertByteCode(key, value, value_size);
	}
};

LPDIRECT3DPIXELSHADER9 PixelShaderCache::GetColorMatrixProgram(int SSAAMode)
{
	return s_CopyProgram[COPY_TYPE_MATRIXCOLOR][DEPTH_CONVERSION_TYPE_NONE][SSAAMode % MAX_SSAA_SHADERS];
}

LPDIRECT3DPIXELSHADER9 PixelShaderCache::GetDepthMatrixProgram(int SSAAMode, bool depthConversion)
{
	return s_CopyProgram[COPY_TYPE_MATRIXCOLOR][depthConversion ? DEPTH_CONVERSION_TYPE_ON : DEPTH_CONVERSION_TYPE_NONE][SSAAMode % MAX_SSAA_SHADERS];
}

LPDIRECT3DPIXELSHADER9 PixelShaderCache::GetColorCopyProgram(int SSAAMode)
{
	return s_CopyProgram[COPY_TYPE_DIRECT][DEPTH_CONVERSION_TYPE_NONE][SSAAMode % MAX_SSAA_SHADERS];
}

LPDIRECT3DPIXELSHADER9 PixelShaderCache::GetClearProgram()
{
	return s_ClearProgram;
}

static LPDIRECT3DPIXELSHADER9 s_rgb8 = NULL;
static LPDIRECT3DPIXELSHADER9 s_rgba6 = NULL;

LPDIRECT3DPIXELSHADER9 PixelShaderCache::ReinterpRGBA6ToRGB8()
{
	const char code[] =
	{
		"uniform sampler samp0 : register(s0);\n"
		"void main(\n"
		"			out float4 ocol0 : COLOR0,\n"
		"			in float2 uv0 : TEXCOORD0){\n"
		"	ocol0 = tex2D(samp0,uv0);\n"
		"	float4 src6 = round(ocol0 * 63.f);\n"
		"	ocol0.r = floor(src6.r*4.f) + floor(src6.g/16.f);\n" // dst8r = (src6r<<2)|(src6g>>4);
		"	ocol0.g = frac(src6.g/16.f)*16.f*16.f + floor(src6.b/4.f);\n" // dst8g = ((src6g&0xF)<<4)|(src6b>>2);
		"	ocol0.b = frac(src6.b/4.f)*4.f*64.f + src6.a;\n" // dst8b = ((src6b&0x3)<<6)|src6a;
		"	ocol0.a = 255.f;\n"
		"	ocol0 /= 255.f;\n"
		"}\n"
	};

	if (!s_rgba6_to_rgb8)
		s_rgba6_to_rgb8 = D3D::CompileAndCreatePixelShader(code, (int)strlen(code));

	return s_rgba6_to_rgb8;
}

LPDIRECT3DPIXELSHADER9 PixelShaderCache::ReinterpRGB8ToRGBA6()
{
	/* old code here for reference
	const char code[] =
	{
	"uniform sampler samp0 : register(s0);\n"
	"void main(\n"
	"			out float4 ocol0 : COLOR0,\n"
	"			in float2 uv0 : TEXCOORD0){\n"
	"	ocol0 = tex2D(samp0,uv0);\n"
	"	float4 src8 = round(ocol0*255.f);\n"
	"	ocol0.r = floor(src8.r/4.f);\n" // dst6r = src8r>>2;
	"	ocol0.g = frac(src8.r/4.f)*4.f*16.f + floor(src8.g/16.f);\n" // dst6g = ((src8r&0x3)<<4)|(src8g>>4);
	"	ocol0.b = frac(src8.g/16.f)*16.f*4.f + floor(src8.b/64.f);\n" // dst6b = ((src8g&0xF)<<2)|(src8b>>6);
	"	ocol0.a = frac(src8.b/64.f)*64.f;\n" // dst6a = src8b&0x3F;
	"	ocol0 /= 63.f;\n"
	"}\n"
	};
	*/
	const char code[] =
	{
		"uniform sampler samp0 : register(s0);\n"
		"void main(\n"
		"out float4 ocol0 : COLOR0,\n"
		"in float2 uv0 : TEXCOORD0){\n"
		"float4 temp1 = float4(1.0f/4.0f,1.0f/16.0f,1.0f/64.0f,0.0f);\n"
		"float4 temp2 = float4(1.0f,64.0f,255.0f,1.0f/63.0f);\n"
		"float4 src8 = round(tex2D(samp0,uv0)*temp2.z) * temp1;\n"
		"ocol0 = (frac(src8.wxyz) * temp2.xyyy + floor(src8)) * temp2.w;\n"
		"}\n"
	};
	if (!s_rgb8_to_rgba6) s_rgb8_to_rgba6 = D3D::CompileAndCreatePixelShader(code, (int)strlen(code));
	return s_rgb8_to_rgba6;
}

#define WRITE p+=sprintf

static LPDIRECT3DPIXELSHADER9 CreateCopyShader(int copyMatrixType, int depthConversionType, int SSAAMode)
{
	//Used for Copy/resolve the color buffer
	//Color conversion Programs
	//Depth copy programs
	// this should create the same shaders as before (plus some extras added for DF16), just... more manageably than listing the full program for each combination
	char text[3072];

	locale_t locale = newlocale(LC_NUMERIC_MASK, "C", NULL); // New locale for compilation
	locale_t old_locale = uselocale(locale); // Apply the locale for this thread
	text[sizeof(text) - 1] = 0x7C;  // canary

	char* p = text;
	WRITE(p, "// Copy/Color Matrix/Depth Matrix shader (matrix=%d, depth=%d, ssaa=%d)\n", copyMatrixType, depthConversionType, SSAAMode);

	WRITE(p, "uniform sampler samp0 : register(s0);\n");
	if(copyMatrixType == COPY_TYPE_MATRIXCOLOR)
		WRITE(p, "uniform float4 cColMatrix[7] : register(c%d);\n", C_COLORMATRIX);
	WRITE(p, "void main(\n"
		"out float4 ocol0 : COLOR0,\n");

	switch(SSAAMode % MAX_SSAA_SHADERS)
	{
	case 0: // 1 Sample
		WRITE(p, "in float2 uv0 : TEXCOORD0,\n"
			"in float uv1 : TEXCOORD1){\n"
			"float4 texcol = tex2D(samp0,uv0.xy);\n");
		break;
	case 1: // 4 Samples in 4x SSAA buffer		
	case 2: // 4 Samples in 9x SSAA buffer
		WRITE(p, "in float4 uv0 : TEXCOORD0,\n"
			"in float uv1 : TEXCOORD1,\n"
			"in float4 uv2 : TEXCOORD2,\n"
			"in float4 uv3 : TEXCOORD3){\n"
			"float4 texcol = (tex2D(samp0,uv2.xy) + tex2D(samp0,uv2.wz) + tex2D(samp0,uv3.xy) + tex2D(samp0,uv3.wz))*0.25f;\n");
		break;
	}

	if(depthConversionType != DEPTH_CONVERSION_TYPE_NONE)
	{
		// Watch out for the fire fumes effect in Metroid it's really sensitive to this,
		// the lighting in RE0 is also way beyond sensitive since the "good value" is hardcoded and Dolphin is almost always off.
		WRITE(p, // 255.99998474121 = 16777215/16777216*256
			"	float workspace = texcol.x * 255.99998474121;\n"

			"	texcol.x = floor(workspace);\n"         // x component

			"	workspace = workspace - texcol.x;\n"    // subtract x component out
			"	workspace = workspace * 256.0;\n"       // shift left 8 bits
			"	texcol.y = floor(workspace);\n"         // y component

			"	workspace = workspace - texcol.y;\n"    // subtract y component out
			"	workspace = workspace * 256.0;\n"       // shift left 8 bits
			"	texcol.z = floor(workspace);\n"         // z component

			"	texcol.w = texcol.x;\n"                 // duplicate x into w

			"	texcol = texcol / 255.0;\n"             // normalize components to [0.0..1.0]

			"	texcol.w = texcol.w * 15.0;\n"
			"	texcol.w = floor(texcol.w);\n"
			"	texcol.w = texcol.w / 15.0;\n"          // w component
			);
	}
	else
	{
		//Apply Gamma Correction
		WRITE(p, "texcol = pow(texcol,uv1.xxxx);\n");
	}

	if(copyMatrixType == COPY_TYPE_MATRIXCOLOR)
	{
		if(depthConversionType == DEPTH_CONVERSION_TYPE_NONE)
			WRITE(p, "texcol = round(texcol * cColMatrix[5])*cColMatrix[6];\n");

		WRITE(p, "ocol0 = float4(dot(texcol,cColMatrix[0]),dot(texcol,cColMatrix[1]),dot(texcol,cColMatrix[2]),dot(texcol,cColMatrix[3])) + cColMatrix[4];\n");
	}
	else
		WRITE(p, "ocol0 = texcol;\n");

	WRITE(p, "}\n");
	if (text[sizeof(text) - 1] != 0x7C)
		PanicAlert("PixelShaderCache copy shader generator - buffer too small, canary has been eaten!");

	uselocale(old_locale); // restore locale
	freelocale(locale);
	return D3D::CompileAndCreatePixelShader(text, (int)strlen(text));
}

void PixelShaderCache::Init()
{
	PixelShadersLock.unlock();
	for (u32 i = 0; i < DSTALPHA_NULL + 1; i++)
	{
		last_entry[i] = nullptr;
	}
	Compiler = &HLSLAsyncCompiler::getInstance();

	//program used for clear screen
	{
		char pprog[3072];
		sprintf(pprog, "void main(\n"
			"out float4 ocol0 : COLOR0,\n"
			" in float4 incol0 : COLOR0){\n"
			"ocol0 = incol0;\n"
			"}\n");
		s_ClearProgram = D3D::CompileAndCreatePixelShader(pprog, (int)strlen(pprog));
	}

	int shaderModel = ((D3D::GetCaps().PixelShaderVersion >> 8) & 0xFF);
	int maxConstants = (shaderModel < 3) ? 32 : ((shaderModel < 4) ? 224 : 65536);

	// other screen copy/convert programs
	for(int copyMatrixType = 0; copyMatrixType < NUM_COPY_TYPES; copyMatrixType++)
	{
		for(int depthType = 0; depthType < NUM_DEPTH_CONVERSION_TYPES; depthType++)
		{
			for(int ssaaMode = 0; ssaaMode < MAX_SSAA_SHADERS; ssaaMode++)
			{
				if(ssaaMode && !s_CopyProgram[copyMatrixType][depthType][ssaaMode-1]
				|| depthType && !s_CopyProgram[copyMatrixType][depthType-1][ssaaMode]
				|| copyMatrixType && !s_CopyProgram[copyMatrixType-1][depthType][ssaaMode])
				{
					// if it failed at a lower setting, it's going to fail here for the same reason it did there,
					// so skip this attempt to avoid duplicate error messages.
					s_CopyProgram[copyMatrixType][depthType][ssaaMode] = NULL;
				}
				else
				{
					s_CopyProgram[copyMatrixType][depthType][ssaaMode] = CreateCopyShader(copyMatrixType, depthType, ssaaMode);
				}
			}
		}
	}

	Clear();

	if (!File::Exists(File::GetUserPath(D_SHADERCACHE_IDX)))
		File::CreateDir(File::GetUserPath(D_SHADERCACHE_IDX).c_str());

	SETSTAT(stats.numPixelShadersCreated, 0);
	SETSTAT(stats.numPixelShadersAlive, 0);

	char cache_filename[MAX_PATH];
	sprintf(cache_filename, "%sdx9-%s-ps.cache", File::GetUserPath(D_SHADERCACHE_IDX).c_str(),
		SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID.c_str());
	PixelShaderCacheInserter inserter;
	PixelShadersLock.lock();
	g_ps_disk_cache.OpenAndRead(cache_filename, inserter);
	PixelShadersLock.unlock();
	if (g_Config.bEnableShaderDebugging)
		Clear();
}

// ONLY to be used during shutdown.
void PixelShaderCache::Clear()
{
	PixelShadersLock.lock();
	for (PSCache::iterator iter = PixelShaders.begin(); iter != PixelShaders.end(); iter++)
		iter->second.Destroy();
	PixelShaders.clear();
	PixelShadersLock.unlock();
	pixel_uid_checker.Invalidate();

	for (u32 i = 0; i < DSTALPHA_NULL + 1; i++)
	{
		last_entry[i] = nullptr;
	}
}

void PixelShaderCache::Shutdown()
{
	Compiler->WaitForFinish();
	for(int copyMatrixType = 0; copyMatrixType < NUM_COPY_TYPES; copyMatrixType++)
		for(int depthType = 0; depthType < NUM_DEPTH_CONVERSION_TYPES; depthType++)
			for(int ssaaMode = 0; ssaaMode < MAX_SSAA_SHADERS; ssaaMode++)
				if(s_CopyProgram[copyMatrixType][depthType][ssaaMode]
				&& (copyMatrixType == 0 || s_CopyProgram[copyMatrixType][depthType][ssaaMode] != s_CopyProgram[copyMatrixType-1][depthType][ssaaMode]))
					s_CopyProgram[copyMatrixType][depthType][ssaaMode]->Release();

	for(int copyMatrixType = 0; copyMatrixType < NUM_COPY_TYPES; copyMatrixType++)
		for(int depthType = 0; depthType < NUM_DEPTH_CONVERSION_TYPES; depthType++)
			for(int ssaaMode = 0; ssaaMode < MAX_SSAA_SHADERS; ssaaMode++)
				s_CopyProgram[copyMatrixType][depthType][ssaaMode] = NULL;

	if (s_ClearProgram) s_ClearProgram->Release();
	s_ClearProgram = NULL;
	if (s_rgb8_to_rgba6) s_rgb8_to_rgba6->Release();
	s_rgb8_to_rgba6 = NULL;
	if (s_rgba6_to_rgb8) s_rgba6_to_rgb8->Release();
	s_rgba6_to_rgb8 = NULL;


	Clear();
	g_ps_disk_cache.Sync();
	g_ps_disk_cache.Close();

	unique_shaders.clear();
}

void PixelShaderCache::PrepareShader(
	DSTALPHA_MODE dstAlphaMode, 
	u32 components, 
	const XFMemory &xfr,
	const BPMemory &bpm,
	bool ongputhread)
{
	const API_TYPE api = ((D3D::GetCaps().PixelShaderVersion >> 8) & 0xFF) < 3 ? API_D3D9_SM20 : API_D3D9_SM30;
	PixelShaderUid uid;
	GetPixelShaderUidD3D9(uid, dstAlphaMode, components, xfr, bpm);
	if (ongputhread)
	{
		Compiler->ProcCompilationResults();
#if defined(_DEBUG) || defined(DEBUGFAST)
		if (g_ActiveConfig.bEnableShaderDebugging)
		{
			ShaderCode code;
			GeneratePixelShaderCodeD3D9(code, dstAlphaMode, components, xfr, bpm);
			pixel_uid_checker.AddToIndexAndCheck(code, uid, "Pixel", "p");
		}
#endif
		// Check if the shader is already set
		if (last_entry[dstAlphaMode])
		{
			if (uid == last_uid[dstAlphaMode])
			{
				return;
			}
		}
		last_uid[dstAlphaMode] = uid;
		GFX_DEBUGGER_PAUSE_AT(NEXT_PIXEL_SHADER_CHANGE, true);
	}
	else
	{
		if (external_last_uid[dstAlphaMode] == uid)
		{
			return;
		}
		external_last_uid[dstAlphaMode] = uid;
	}
	PixelShadersLock.lock();
	PSCacheEntry* entry = &PixelShaders[uid];
	PixelShadersLock.unlock();
	if (ongputhread)
	{
		last_entry[dstAlphaMode] = entry;
	}
	// Compile only when we have a new instance
	if (entry->initialized.test_and_set())
	{
		return;
	}
	// Need to compile a new shader
	ShaderCode code;
	ShaderCompilerWorkUnit *wunit = Compiler->NewUnit(PIXELSHADERGEN_BUFFERSIZE);
	code.SetBuffer(wunit->code.data());
	if (api == API_D3D9_SM20)
	{
		GeneratePixelShaderCodeD3D9SM2(code, dstAlphaMode, components, xfr, bpm);
	}
	else
	{
		GeneratePixelShaderCodeD3D9(code, dstAlphaMode, components, xfr, bpm);
	}
	wunit->codesize = (u32)code.BufferSize();
	wunit->entrypoint = "main";
	wunit->flags = D3DCOMPILE_SKIP_VALIDATION | D3DCOMPILE_OPTIMIZATION_LEVEL3;
	wunit->target = D3D::PixelShaderVersionString();
	wunit->ResultHandler = [uid, entry](ShaderCompilerWorkUnit* wunit)
	{
		if (SUCCEEDED(wunit->cresult))
		{
			ID3DBlob* shaderBuffer = wunit->shaderbytecode;
			const u8* bytecode = (const u8*)shaderBuffer->GetBufferPointer();
			u32 bytecodelen = (u32)shaderBuffer->GetBufferSize();
			g_ps_disk_cache.Append(uid, bytecode, bytecodelen);
			PushByteCode(uid, bytecode, bytecodelen, entry);
#if defined(_DEBUG) || defined(DEBUGFAST)
			if (g_ActiveConfig.bEnableShaderDebugging)
			{
				u32 code_hash = HashAdler32((const u8 *)wunit->code.data(), wunit->codesize);
				unique_shaders.insert(code_hash);
				SETSTAT(stats.numUniquePixelShaders, unique_shaders.size());
				entry->code = wunit->code.data();
			}
			if (g_ActiveConfig.iLog & CONF_SAVESHADERS) {
				static int counter = 0;
				char szTemp[MAX_PATH];
				sprintf(szTemp, "%sps_%04i.txt", File::GetUserPath(D_DUMP_IDX).c_str(), counter++);

				SaveData(szTemp, wunit->code.data());
			}
#endif			
		}
		else
		{
			static int num_failures = 0;
			char szTemp[MAX_PATH];
			sprintf(szTemp, "%sbad_ps_%04i.txt", File::GetUserPath(D_DUMP_IDX).c_str(), num_failures++);
			std::ofstream file;
			OpenFStream(file, szTemp, std::ios_base::out);
			file << ((const char *)wunit->code.data());
			file.close();

			PanicAlert("Failed to compile pixel shader!\nThis usually happens when trying to use Dolphin with an outdated GPU or integrated GPU like the Intel GMA series.\n\nIf you're sure this is Dolphin's error anyway, post the contents of %s along with this error message at the forums.\n\nDebug info (%s):\n%s",
				szTemp,
				D3D::VertexShaderVersionString(),
				(char*)wunit->error->GetBufferPointer());
		}
	};
	Compiler->CompileShaderAsync(wunit);
}

bool PixelShaderCache::SetShader(DSTALPHA_MODE dstAlphaMode)
{
	const PSCacheEntry* entry = last_entry[dstAlphaMode];
	u32 count = 0;
	while (!entry->compiled)
	{
		Compiler->ProcCompilationResults();
		if (g_ActiveConfig.bFullAsyncShaderCompilation)
		{
			break;
		}
		Common::cYield(count++);
	}
	if (entry->shader && entry->compiled)
	{
		D3D::SetPixelShader(entry->shader);
		return true;
	}
	return false;
}

void PixelShaderCache::PushByteCode(const PixelShaderUid &uid, const u8 *bytecode, int bytecodelen, PixelShaderCache::PSCacheEntry* entry)
{
	entry->shader = D3D::CreatePixelShaderFromByteCode(bytecode, bytecodelen);
	entry->compiled = true;
	if (entry->shader)
	{
		INCSTAT(stats.numPixelShadersCreated);
		SETSTAT(stats.numPixelShadersAlive, PixelShaders.size());
	}
}

void PixelShaderCache::InsertByteCode(const PixelShaderUid &uid, const u8 *bytecode, int bytecodelen)
{
	PixelShaderCache::PSCacheEntry *entry = &PixelShaders[uid];
	entry->initialized.test_and_set();
	PushByteCode(uid, bytecode, bytecodelen, entry);
}
}  // namespace DX9